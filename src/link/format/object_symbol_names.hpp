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
// name for every EXTERNAL-LINKAGE DEFINED function — otherwise a foreign
// linker fails `gcc main.c dss.o` with "undefined reference to foo". An
// external-linkage symbol (`hasExternalLinkage`, i.e. a named row whose
// binding is not Local — see that predicate for why it is deliberately NOT
// `isExternallyVisible`) gets
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
// NAME<->BINDING<->VISIBILITY LOCKSTEP (D-LK-INTERNAL-LINKAGE-FN-EMITTED-GLOBAL-FOREIGN-COLLISION,
// TF-C54; the visibility leg added by
// D-LK-OBJECT-GLOBAL-HIDDEN-VISIBILITY-EMITTED-LOCAL): `definedBinding` and
// `definedVisibility` are the exact companions of
// `definedName`. All three run the IDENTICAL `definedBySym_` lookup + the
// IDENTICAL `hasExternalLinkage` predicate, so a symbol that gets
// the `<prefix><id>` fallback NAME is ALWAYS emitted with `SymbolBinding::Local`
// binding and `SymbolVisibility::Default`, and a symbol that keeps its real
// NAME keeps its real (Global/Weak) binding AND its real visibility -- the
// three can never drift. Before TF-C54 the writers consulted
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
// ★ AND THE IMPORT SIDE HAS THE SAME LOCKSTEP PAIR (`externBinding`,
// D-CSUBSET-WEAK-EXTERN-IMPORT-NOT-IN-SYMBOL-TABLE). It is the identical defect
// one rail over: the writers consulted `externName` for the name and HARDCODED
// the strong binding, so `extern int ea __attribute__((weak));` — a reference
// the language understood, the HIR recorded and the MIR carried — reached the
// wire as an ORDINARY undefined symbol on all three formats, losing the one
// property that makes it weak (that it may legally resolve to nothing).
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
            if (!hasExternalLinkage(ms)) continue;
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

    // ── THE ONE PREDICATE the name / binding / visibility trio share ───────
    //    D-LK-OBJECT-GLOBAL-HIDDEN-VISIBILITY-EMITTED-LOCAL
    //
    // "Does this row carry a LINKAGE IDENTITY a linker resolves by name?" —
    // which is a question about BINDING alone. `Local` is C's internal
    // linkage: the name is a TU-private label no other TU may name. Everything
    // else (`Global`, `Weak`) has EXTERNAL LINKAGE, and a linker resolving a
    // static link needs its real name and its real binding.
    //
    // ★★ IT IS DELIBERATELY *NOT* `isExternallyVisible`, AND THAT SUBSTITUTION
    // WAS THE DEFECT. `isExternallyVisible` answers a DIFFERENT question —
    // "may another IMAGE reference this at run time" — and it therefore folds
    // VISIBILITY into its answer, returning false for Global+Hidden. That is
    // the RIGHT question for DCE (which must not delete a symbol a later image
    // may call) and for a dynamic export table (`.dynsym`, the Mach-O export
    // trie, the PE export directory), and both keep using it. It is the WRONG
    // question for a `.symtab` name and binding: a hidden symbol is invisible
    // to other IMAGES while still being an ordinary external-linkage symbol
    // that a STATIC link resolves by name. Folding the two collapsed a
    // 2-axis fact onto one axis and emitted `STB_LOCAL` + `<prefix><id>` where
    // every reference emits a real name with a real binding.
    //
    // ✔MEASURED 2026-09-05 (Ubuntu 24.04, binutils 2.42 / llvm 18), one source
    // carrying `visibility("hidden")`, `("internal")`, `("protected")`, a
    // `static` CONTROL and a plain extern-linkage CONTROL, compiled to a
    // relocatable object AND to a final image, each reference probed SEPARATELY:
    //
    //   reference / tier          hidden        internal      protected     CONTROL static  CONTROL plain
    //   gcc 13.3.0 ELF .o         GLOBAL HIDDEN GLOBAL INTERNAL GLOBAL PROTECTED LOCAL DEFAULT GLOBAL DEFAULT
    //   gcc 13.3.0 ELF exec       GLOBAL HIDDEN  —             —             LOCAL DEFAULT GLOBAL DEFAULT
    //   clang 18.1.3 ELF exec     GLOBAL HIDDEN  —             —             LOCAL DEFAULT GLOBAL DEFAULT
    //   clang 18.1.3 Mach-O .o    private extern private extern external      non-external  external
    //   mingw gcc 13.2.0 COFF .o  EXTERNAL(scl 2) —            —             STATIC (scl 3) EXTERNAL(scl 2)
    //
    // NOT ONE of them makes a hidden symbol internal-linkage, and every one of
    // them keeps its real NAME. (mingw-w64 gcc additionally WARNS "visibility
    // attribute not supported in this configuration; ignored" — COFF has no
    // visibility axis at all, so EXTERNAL with the real name is the whole of
    // the correct answer there, and it is what this predicate produces for the
    // PE writer with no per-format code.)
    [[nodiscard]] static constexpr bool
    hasExternalLinkage(ModuleSymbol const& ms) noexcept {
        return !ms.name.empty() && ms.binding != SymbolBinding::Local;
    }

    // The `.symtab` name for a DEFINED function symbol. Returns the real
    // (already-mangled) source name for an external-linkage definition;
    // otherwise the internal `<internalPrefix><id>` (static/local, synthesized,
    // or a substrate module with no `ModuleSymbol` row for `id`).
    [[nodiscard]] std::string
    definedName(SymbolId id, std::string_view internalPrefix) const {
        if (auto const it = definedBySym_.find(id.v); it != definedBySym_.end()) {
            if (hasExternalLinkage(*it->second)) return it->second->name;
        }
        return std::string{internalPrefix} + std::to_string(id.v);
    }

    // The `.symtab` name for a DEFINED symbol in a FINAL IMAGE (an exec, a PIE,
    // a `.so`/dylib/DLL) -- the final-image companion of `definedName`, reading
    // the SAME `module.symbols` carrier through the SAME lookup, and differing
    // in EXACTLY ONE clause: it does NOT require `hasExternalLinkage`.
    // D-LINK-ELF-EXEC-SYMBOL-NAMES-REPLACED-BY-SYNTHETIC-IDS.
    //
    // ★ WHY THE PREDICATE LEGITIMATELY DIFFERS, rather than one method serving
    // both tiers. `definedName`'s linkage gate is not a naming preference --
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
    // the SAME `hasExternalLinkage` predicate:
    //   * present, named, external linkage -> the real `ms.binding`
    //     (Global or Weak) -- exactly the case `definedName` returns the real
    //     name for;
    //   * absent / nameless / Local binding (a `static` def, a synthesized
    //     rodata/thunk, an interior block symbol, or a substrate module with no
    //     `ModuleSymbol` row) -> `SymbolBinding::Local` -- exactly the case
    //     `definedName` returns the `<prefix><id>` fallback for.
    // Guarantee: a `<prefix><id>`-named symbol is ALWAYS Local; a real-named
    // symbol keeps its real binding. The `!ms.name.empty()` clause is load-
    // bearing (a nameless row can never be foreign-visible, so it must stay
    // Local, matching the name side). See the LOCKSTEP note above +
    // D-LK-INTERNAL-LINKAGE-FN-EMITTED-GLOBAL-FOREIGN-COLLISION.
    [[nodiscard]] SymbolBinding
    definedBinding(SymbolId id) const {
        if (auto const it = definedBySym_.find(id.v); it != definedBySym_.end()) {
            if (hasExternalLinkage(*it->second)) {
                return it->second->binding;  // Global or Weak
            }
        }
        return SymbolBinding::Local;
    }

    // ── The `.symtab` VISIBILITY for a DEFINED symbol ─────────────────────
    //    D-LK-OBJECT-GLOBAL-HIDDEN-VISIBILITY-EMITTED-LOCAL
    //
    // The THIRD member of the lockstep trio, carried as its OWN axis instead
    // of being folded into the binding. Same lookup, same `hasExternalLinkage`
    // predicate, so it cannot drift from the name or the binding: a symbol
    // that took the `<prefix><id>` fallback name and the `Local` binding takes
    // `Default` here too, because a TU-private label has no visibility to
    // declare -- ✔MEASURED, that is exactly what gcc emits for a `static`
    // (`FUNC LOCAL DEFAULT`) and what clang emits on Mach-O (`non-external`,
    // no private-extern bit).
    //
    // ★ WHY AN ACCESSOR RATHER THAN LETTING EACH WRITER READ `ms.visibility`.
    // A writer that reached into `module.symbols` for the visibility would be
    // reading a DIFFERENT row from the one `definedName`/`definedBinding`
    // answered for whenever an id carries aliases (this class is
    // first-row-wins, and `definedAliases` holds the rest), which is the exact
    // drift the trio exists to make impossible. One lookup, one predicate,
    // three answers.
    //
    // ★★ WHAT EACH FORMAT DOES WITH IT — the per-format vocabulary lives in
    // the writers, as `stbForBinding` / `definedNType` already do for binding:
    //   * ELF spells it natively in `st_other` (STV_DEFAULT / STV_INTERNAL /
    //     STV_HIDDEN / STV_PROTECTED), so the mapping is total and lossless.
    //   * Mach-O has ONE bit, N_PEXT ("private extern"), and clang sets it for
    //     exactly `hidden` and `internal` while leaving `protected` a plain
    //     external -- i.e. the Mach-O bit is `!isExternallyVisible(...)` over
    //     the pair, which is why that predicate stays and is consulted THERE.
    //   * COFF has no visibility axis; mingw-w64 gcc warns and ignores the
    //     attribute, so the PE writer needs no visibility code at all and gets
    //     the reference's answer (EXTERNAL, real name) for free.
    [[nodiscard]] SymbolVisibility
    definedVisibility(SymbolId id) const {
        if (auto const it = definedBySym_.find(id.v); it != definedBySym_.end()) {
            if (hasExternalLinkage(*it->second)) {
                return it->second->visibility;
            }
        }
        return SymbolVisibility::Default;
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
    //   * that has EXTERNAL LINKAGE (`hasExternalLinkage`, the same predicate
    //     `definedName` runs). A Local extra name stays internal for exactly
    //     the reason the canonical's would -- `definedName` carves such names
    //     to `<prefix><id>`, and an alias is not a licence to bypass that.
    //     ⚠ BOUNDARY: a Local-only alias of a defined symbol is therefore
    //     still absent from the re-emitted symtab. No shipped shape produces
    //     one (an alias exists to be referred to by name from elsewhere, which
    //     is what external linkage means), but it is a real gap and is named
    //     here rather than left to be rediscovered.
    //   ★ A HIDDEN alias DOES qualify, since D-LK-OBJECT-GLOBAL-HIDDEN-VISIBILITY-EMITTED-LOCAL
    //     made `hasExternalLinkage` the shared predicate: `__attribute__((alias,
    //     visibility("hidden")))` names a real symbol a static link resolves,
    //     and the writers carry its visibility through `alias->visibility`
    //     exactly as they carry the canonical's through `definedVisibility`.
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
    // Local extra name (`macho_object_reader.cpp` gives every NON-EXTERNAL
    // nlist `SymbolBinding::Local`, so `hasExternalLinkage` keeps excluding it
    // after that predicate replaced `isExternallyVisible` — the argument below
    // survived the change because it never depended on VISIBILITY)
    // (D-LINK-EQUAL-OFFSET-DEFINED-SYMBOLS-BECOME-TWIN-ATOMS names that case).
    // A module-private label denotes an address the real name already denotes,
    // so emitting it into an image adds a second nlist/`.symtab` row that
    // answers no question a debugger asks — and ld64/ld drop exactly these when
    // they build an image. So the image tier legitimately wants the SAME
    // external-linkage set: two names that are both real ABI, never a name
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

    // ── The REFERENCE binding for an UNDEFINED extern symbol ──────────────
    //    D-CSUBSET-WEAK-EXTERN-IMPORT-NOT-IN-SYMBOL-TABLE
    //
    // The exact binding companion of `externName`, built the way
    // `definedBinding` is built for `definedName` and for the same stated
    // reason (the NAME<->BINDING LOCKSTEP note in this file's header): it runs
    // the IDENTICAL `externBySym_` lookup behind the IDENTICAL
    // `!mangledName.empty()` predicate, so a target that takes the real import
    // NAME takes its real binding, and a target that falls back to
    // `<prefix><id>` takes the fallback binding. The two cannot drift because
    // there is one lookup and one predicate, written once.
    //
    // ★★ THE FALLBACK IS `Global`, AND THAT IS THE LOAD-BEARING HALF. A reloc
    // target that is neither defined here nor a known import — the
    // `<prefix><id>` arm — is a reference DSS cannot describe. Emitting it WEAK
    // would tell the final linker it may resolve to nothing, and the program
    // would then read through address 0 instead of failing to link: a silent
    // wrong answer produced by the arm that exists precisely because something
    // is unknown. Strong is the only honest answer where the binding is not
    // known, and it is also the pre-existing behaviour, so this method is
    // byte-identical to a hardcoded strong binding for every module that
    // carries no weak import at all.
    //
    // FORMAT-NEUTRAL like everything else here: the caller maps this ONE
    // `SymbolBinding` to its own wire vocabulary (ELF STB_WEAK, Mach-O
    // N_WEAK_REF, COFF IMAGE_SYM_CLASS_WEAK_EXTERNAL), which is the same
    // division of labour `definedBinding` already has.
    [[nodiscard]] SymbolBinding externBinding(SymbolId id) const {
        if (auto const it = externBySym_.find(id.v); it != externBySym_.end()) {
            if (!it->second->mangledName.empty()) {
                return it->second->binding;
            }
        }
        return SymbolBinding::Global;
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
