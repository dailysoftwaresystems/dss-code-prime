#pragma once

#include "asm/asm.hpp"                  // AssembledModule, ModuleSymbol, ExternImport
#include "core/types/strong_ids.hpp"    // SymbolId
#include "core/types/symbol_attrs.hpp"  // SymbolBinding, SymbolVisibility, isExternallyVisible

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Shared symbol-NAMING substrate for the object/image writers — the ONE owner of
// the `AssembledModule::symbols` lookup, serving BOTH artifact tiers:
//   * RELOCATABLE objects (ELF ET_REL, PE/COFF Obj, Mach-O MH_OBJECT) via
//     `definedName` / `definedBinding` / `externName` —
//     D-LK-OBJECT-EXTERN-SYMBOL-NAMES;
//   * FINAL IMAGES (ELF ET_EXEC + ET_DYN, Mach-O exec/dylib, PE image) via
//     `imageName` — D-LINK-ELF-EXEC-SYMBOL-NAMES-REPLACED-BY-SYNTHETIC-IDS.
// `definedAliases` serves BOTH tiers unchanged — see its docblock for why the
// extra names do NOT take `imageName`'s wider predicate. An anchor name is
// never wrapped, because a split name is not greppable and the registry guard
// then reads a DIFFERENT, shorter name and passes:
// D-LK-ALIAS-NAME-ABSENT-FROM-REEMITTED-OBJECT-SYMTAB
// The two tiers differ in ONE predicate clause and that difference is documented
// on `imageName`; everything else (the carrier, the lookup, the `<prefix><id>`
// fallback shape) is shared, so a writer can never grow a private name table.
//
// A relocatable object's `.symtab` must carry the SOURCE-LEVEL C identifier
// name for every externally-visible DEFINED function — otherwise a foreign
// linker fails `gcc main.c dss.o` with "undefined reference to foo". An
// externally-visible symbol (`isExternallyVisible` binding/visibility) gets
// its `ModuleSymbol.name` (already format-mangled by the compile pipeline's
// `nameOf` — identity on ELF/PE, a leading `_` on Mach-O, so it is emitted
// VERBATIM and a re-mangle would DOUBLE it). Everything else — a STATIC/Local
// function, a compiler-SYNTHESIZED symbol (string-literal rodata, an init
// thunk), an interior `&&label` block symbol, or a hand-built substrate
// module with an empty `symbols` table — keeps the caller's internal
// `<prefix><id>` fallback: those are resolved INTRA-object (by the DSS linker,
// or by a reloc within the same `.o`), never by a foreign linker by name, so
// the anchor explicitly permits them to "stay internal". A static
// (Local-binding) function therefore falls through to the fallback — this is
// the `may stay internal` name carve-out.
//
// NAME<->BINDING LOCKSTEP (D-LK-INTERNAL-LINKAGE-FN-EMITTED-GLOBAL-FOREIGN-COLLISION,
// TF-C54): `definedBinding` is the exact binding companion of
// `definedName`. It runs the IDENTICAL `definedBySym_` lookup + the IDENTICAL
// `!name.empty() && isExternallyVisible(...)` predicate, so a symbol that gets
// the `<prefix><id>` fallback NAME is ALWAYS emitted with `SymbolBinding::Local`
// binding, and a symbol that keeps its real NAME keeps its real (Global/Weak)
// binding -- the two can never drift. Before TF-C54 the writers consulted
// `definedName` for the name but HARDCODED the GLOBAL storage class, so a
// `static` function shipped as a GLOBAL `sym_<id>`: invisible to DSS's own
// (cuId,SymbolId)-keyed linker, but a FOREIGN linker keys by name -> two TUs
// both defining GLOBAL `sym_3774` -> `ld: multiple definition` x thousands
// (the arm64/x86_64 multi-TU sqlite foreign link). Each format writer maps this
// ONE `SymbolBinding` decision to its own vocabulary (ELF STB_*, COFF
// IMAGE_SYM_CLASS_*, Mach-O N_EXT) + ELF's local-before-global `.symtab`
// ordering -- no `if(format)` in this shared substrate.
//
// The IMPORT side (`externName`) names an UNDEFINED extern reference so a
// foreign linker resolves it — its `ExternImport.mangledName` (already the
// on-binary form) replaces the internal `<prefix><id>`. Consumed by the ET_REL
// writer's undefined-symbol loop once the format declares an `externCallDispatch`
// (D-LK-OBJECT-EXTERN-CALL-RELOCATABLE): an extern call lowers to a `call rel32`
// + a reloc against the extern, and the writer emits the extern as an SHN_UNDEF
// symtab entry the final (foreign) linker resolves.
//
// FORMAT-NEUTRAL: the one per-format value, the internal-fallback PREFIX
// ("sym_" on ELF/PE, "_sym_" on Mach-O), is a caller PARAMETER — the same
// per-format vocabulary those writers already carry, NOT an `if(format)`
// branch in shared substrate. Data comes from `AssembledModule.symbols`, which
// flows intact into every `encode()` — no signature change, no plumbing.

namespace dss::link::format {

class ObjectSymbolNames {
public:
    explicit ObjectSymbolNames(AssembledModule const& module) {
        definedBySym_.reserve(module.symbols.size());
        for (ModuleSymbol const& ms : module.symbols) {
            auto const [it, fresh] = definedBySym_.emplace(ms.symbol.v, &ms);
            if (fresh) continue;
            // A SECOND (third, …) row for one SymbolId is an ALIAS: one atom,
            // several names. See `definedAliases` for what qualifies and why
            // the canonical row is the first one.
            if (ms.name.empty() || ms.name == it->second->name) continue;
            if (!isExternallyVisible(ms.binding, ms.visibility)) continue;
            // ...and against every alias ALREADY accepted for this id, not the
            // canonical alone. `[A, B, B]` otherwise emits `B` twice, which is
            // the duplicate-symbol error at the foreign linker that the
            // "name DIFFERS" clause below exists to prevent -- the clause held
            // the property against the canonical and not against itself.
            // No shipped reader produces a repeated alias today (each folds one
            // boundary symbol at a time), so this is a hole in a guard rather
            // than a live defect; a guard that does not hold the property it
            // claims is the defect. The scan is linear over a vector that is
            // empty for almost every id and one or two entries otherwise.
            auto& rows = aliasesBySym_[ms.symbol.v];
            bool repeated = false;
            for (ModuleSymbol const* seen : rows) {
                if (seen->name == ms.name) { repeated = true; break; }
            }
            if (repeated) continue;
            rows.push_back(&ms);
        }
        externBySym_.reserve(module.externImports.size());
        for (ExternImport const& e : module.externImports) {
            externBySym_.emplace(e.symbol.v, &e);
        }
    }

    // The `.symtab` name for a DEFINED function symbol. Returns the real
    // (already-mangled) source name for an externally-visible definition;
    // otherwise the internal `<internalPrefix><id>` (static/local, synthesized,
    // or a substrate module with no `ModuleSymbol` row for `id`).
    [[nodiscard]] std::string
    definedName(SymbolId id, std::string_view internalPrefix) const {
        if (auto const it = definedBySym_.find(id.v); it != definedBySym_.end()) {
            ModuleSymbol const& ms = *it->second;
            if (!ms.name.empty()
                && isExternallyVisible(ms.binding, ms.visibility)) {
                return ms.name;
            }
        }
        return std::string{internalPrefix} + std::to_string(id.v);
    }

    // The `.symtab` name for a DEFINED symbol in a FINAL IMAGE (an exec, a PIE,
    // a `.so`/dylib/DLL) -- the final-image companion of `definedName`, reading
    // the SAME `module.symbols` carrier through the SAME lookup, and differing
    // in EXACTLY ONE clause: it does NOT require `isExternallyVisible`.
    // D-LINK-ELF-EXEC-SYMBOL-NAMES-REPLACED-BY-SYNTHETIC-IDS.
    //
    // ★ WHY THE PREDICATE LEGITIMATELY DIFFERS, rather than one method serving
    // both tiers. `definedName`'s visibility gate is not a naming preference --
    // it exists because a RELOCATABLE object's `.symtab` names are a FOREIGN
    // LINKER'S RESOLUTION KEYS: exposing a `static` under its real name there
    // makes two TUs that both define `helper` collide at `ld` (`multiple
    // definition`), which is the exact defect
    // D-LK-INTERNAL-LINKAGE-FN-EMITTED-GLOBAL-FOREIGN-COLLISION
    // closed. A FINAL IMAGE is never re-linked, so its
    // `.symtab` resolves NOTHING -- it is read by debuggers, profilers and crash
    // reporters, for which a `static`'s real name is precisely the wanted
    // answer, and duplicate local names across CUs are normal and harmless
    // (`readelf -sW` over any gcc-linked binary shows exactly this shape: every
    // static function keeps its source name in the final image). Suppressing the
    // name there bought nothing and cost every backtrace frame its identity:
    // ✔MEASURED before this method existed, gdb over a DSS ELF exec printed
    // `#0 sym_84 / #1 sym_89 / #2 sym_93` for `static_helper`/`global_helper`/
    // `main`, on BOTH x86_64 and aarch64.
    //
    // THE FALLBACK IS DELIBERATE, NOT INCIDENTAL, and covers exactly the symbols
    // that genuinely HAVE no declared name -- there is nothing truer to emit for
    // them, and `<prefix><id>` is at least unique and greppable:
    //   * the LINKER-INJECTED entry trampoline (`entry_trampoline.cpp` prepends
    //     it as functions[0] with a minted SymbolId and no `ModuleSymbol` row);
    //   * a compiler-SYNTHESIZED symbol (string-literal rodata, an init thunk);
    //   * an interior `&&label` block symbol (D-CSUBSET-COMPUTED-GOTO);
    //   * a hand-built substrate module with an empty `symbols` table.
    // A row that EXISTS but carries an empty name also lands here rather than
    // emitting a zero-length name: an empty `.symtab` name is indistinguishable
    // from "no name recorded" to every reader, so it would be a silent loss.
    [[nodiscard]] std::string
    imageName(SymbolId id, std::string_view internalPrefix) const {
        if (auto const it = definedBySym_.find(id.v); it != definedBySym_.end()) {
            ModuleSymbol const& ms = *it->second;
            if (!ms.name.empty()) {
                return ms.name;
            }
        }
        return std::string{internalPrefix} + std::to_string(id.v);
    }

    // The `.symtab` BINDING for a DEFINED symbol -- the binding companion of
    // `definedName`, coupled to it BYTE-FOR-BYTE by using the SAME lookup and
    // the SAME `!name.empty() && isExternallyVisible(...)` predicate:
    //   * present, named, externally-visible -> the real `ms.binding`
    //     (Global or Weak) -- exactly the case `definedName` returns the real
    //     name for;
    //   * absent / nameless / not-externally-visible (a `static`/Local def, a
    //     synthesized rodata/thunk, an interior block symbol, or a substrate
    //     module with no `ModuleSymbol` row) -> `SymbolBinding::Local` --
    //     exactly the case `definedName` returns the `<prefix><id>` fallback for.
    // Guarantee: a `<prefix><id>`-named symbol is ALWAYS Local; a real-named
    // symbol keeps its real binding. The `!ms.name.empty()` clause is load-
    // bearing (a nameless row can never be foreign-visible, so it must stay
    // Local, matching the name side). See the LOCKSTEP note above +
    // D-LK-INTERNAL-LINKAGE-FN-EMITTED-GLOBAL-FOREIGN-COLLISION.
    //
    // BOUNDARY (D-LK-OBJECT-GLOBAL-HIDDEN-VISIBILITY-EMITTED-LOCAL, OPEN/unfired):
    // reusing `isExternallyVisible` means a Global-binding + Hidden-visibility
    // def resolves here to Local, so it emits STB_LOCAL (name already carved to
    // `<prefix><id>` by `definedName` — this only aligns the binding). gcc keeps
    // it STB_GLOBAL + STV_HIDDEN (resolvable during a static link, hidden from
    // the dynamic export). The c CAN form a Global+Hidden symbol
    // (`__attribute__((visibility("hidden")))` on a NON-static fn -- e.g.
    // examples/c/attributes_syntax's `dead_helper`), but none is routed
    // to a relocatable writer LIVE: it is DCE-eliminated (an unused hidden
    // helper), or reaches an EXEC writer where `isExec` forces Global -- so the
    // Local-emission trap never fires at the ET_REL/COFF/Mach-O symtab. Closing
    // it means a coupled name+binding STV_HIDDEN emission (real name +
    // STB_GLOBAL + st_other=STV_HIDDEN).
    [[nodiscard]] SymbolBinding
    definedBinding(SymbolId id) const {
        if (auto const it = definedBySym_.find(id.v); it != definedBySym_.end()) {
            ModuleSymbol const& ms = *it->second;
            if (!ms.name.empty()
                && isExternallyVisible(ms.binding, ms.visibility)) {
                return ms.binding;  // Global or Weak
            }
        }
        return SymbolBinding::Local;
    }

    // Every ADDITIONAL name bound to `id` beyond the canonical one, in
    // `module.symbols` order, each carrying its OWN binding.
    // D-LK-ALIAS-NAME-ABSENT-FROM-REEMITTED-OBJECT-SYMTAB.
    //
    // ★ WHY THIS EXISTS. Since equal-offset defined symbols collapse to ONE
    // atom with several names (D-LINK-EQUAL-OFFSET-DEFINED-SYMBOLS-BECOME-TWIN-ATOMS),
    // `module.symbols` can carry several rows for one `SymbolId` --
    // `strong_fn` GLOBAL and `weak_alias` WEAK at the same address is the shape
    // gcc emits for `__attribute__((weak, alias("strong_fn")))`. `definedName`
    // and `definedBinding` are FIRST-ROW-WINS by design (a symtab entry, an
    // index map key and a relocation target all need exactly one answer), so
    // every later row was silently dropped from a re-emitted object: DSS read
    // an object carrying two names and wrote one back.
    //
    // WHAT QUALIFIES, and each clause is the same predicate `definedName` uses:
    //   * a NAMED row (an empty name is "no name recorded", not a name);
    //   * whose name DIFFERS from the canonical's AND from every alias already
    //     accepted for this id (a byte-identical repeat is the same name, and
    //     emitting it twice is a duplicate-symbol error at the foreign linker);
    //   * that is EXTERNALLY VISIBLE. A non-externally-visible extra name stays
    //     internal for exactly the reason the canonical's would -- `definedName`
    //     carves such names to `<prefix><id>`, and an alias is not a licence to
    //     bypass that. ⚠ BOUNDARY: a Local-only alias of a defined symbol is
    //     therefore still absent from the re-emitted symtab. No shipped shape
    //     produces one (an alias exists to be referred to by name from
    //     elsewhere, which is what external visibility means), but it is a real
    //     gap and is named here rather than left to be rediscovered.
    //
    // The canonical is the FIRST row, which keeps every existing id-to-row
    // lookup byte-identical: this method only ADDS names a writer may emit
    // after the canonical one, and never changes which row is canonical.
    //
    // ★★ ONE ACCESSOR SERVES BOTH TIERS, and unlike `definedName`/`imageName`
    // that is a POSITIVE finding rather than an omission. `imageName` drops
    // `definedName`'s visibility gate because a `static`'s real name is the
    // frame a debugger prints and nothing re-links an image. Applying the same
    // reasoning to the EXTRA names would admit compiler-private section labels:
    // ✔MEASURED — INSTRUMENT: `tests/link/clang_macho_subsections_object.inc`,
    // a real clang-produced arm64 MH_OBJECT checked into this tree byte-for-byte
    // (Ubuntu clang 19.1.1, `-target arm64-apple-macos11 -c -O1`), whose header
    // carries the nlist dump read back OUT of those bytes: `nlist_64[0]` is
    // `ltmp0`, non-external, n_sect=1, n_value=0x00, and `nlist_64[4]` is the
    // EXTERNAL `_outer` at the same n_value=0x00. One address, two names, one of
    // them compiler-private. ⚠ Stated with its instrument because a bare
    // `✔MEASURED` is a claim about an experiment nobody can re-run. So clang
    // emits `ltmp0` at the exact `n_value` of the first function of an ordinary
    // translation unit, and the readers correctly fold it in here as a
    // non-externally-visible extra name
    // (D-LINK-EQUAL-OFFSET-DEFINED-SYMBOLS-BECOME-TWIN-ATOMS names that case).
    // A module-private label denotes an address the real name already denotes,
    // so emitting it into an image adds a second nlist/`.symtab` row that
    // answers no question a debugger asks — and ld64/ld drop exactly these when
    // they build an image. So the image tier legitimately wants the SAME
    // externally-visible set: two names that are both real ABI, never a name
    // plus its compiler-private shadow. The Local-only-alias boundary noted
    // above therefore applies to both tiers, for two different reasons.
    [[nodiscard]] std::span<ModuleSymbol const* const>
    definedAliases(SymbolId id) const {
        if (auto const it = aliasesBySym_.find(id.v);
            it != aliasesBySym_.end()) {
            return it->second;
        }
        return {};
    }

    // The `.symtab` name for an UNDEFINED extern reference. Returns the import's
    // (already-mangled) on-binary name so a foreign linker resolves it;
    // otherwise the internal `<internalPrefix><id>` (a reloc target that is
    // neither a defined symbol nor a known import).
    [[nodiscard]] std::string
    externName(SymbolId id, std::string_view internalPrefix) const {
        if (auto const it = externBySym_.find(id.v); it != externBySym_.end()) {
            if (!it->second->mangledName.empty()) {
                return it->second->mangledName;
            }
        }
        return std::string{internalPrefix} + std::to_string(id.v);
    }

private:
    // Keyed by SymbolId.v — pointers alias the caller's `AssembledModule`,
    // which outlives this helper (the writer builds it on the stack inside
    // `encode()`, before the symtab loop, and discards it after).
    std::unordered_map<std::uint32_t, ModuleSymbol const*> definedBySym_;
    // Only the ids that HAVE aliases appear here, so the common case costs a
    // failed hash lookup and no allocation.
    std::unordered_map<std::uint32_t, std::vector<ModuleSymbol const*>>
        aliasesBySym_;
    std::unordered_map<std::uint32_t, ExternImport const*> externBySym_;
};

} // namespace dss::link::format
