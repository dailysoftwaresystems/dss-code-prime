#pragma once

#include "asm/asm.hpp"                  // AssembledModule, ModuleSymbol, ExternImport
#include "core/types/strong_ids.hpp"    // SymbolId
#include "core/types/symbol_attrs.hpp"  // SymbolBinding, SymbolVisibility, isExternallyVisible

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

// Shared symbol-NAMING substrate for the object/image writers — the ONE owner of
// the `AssembledModule::symbols` lookup, serving BOTH artifact tiers:
//   * RELOCATABLE objects (ELF ET_REL, PE/COFF Obj, Mach-O MH_OBJECT) via
//     `definedName` / `definedBinding` / `externName` —
//     D-LK-OBJECT-EXTERN-SYMBOL-NAMES;
//   * FINAL IMAGES (ELF ET_EXEC + ET_DYN, Mach-O exec/dylib, PE image) via
//     `imageName` — D-LINK-ELF-EXEC-SYMBOL-NAMES-REPLACED-BY-SYNTHETIC-IDS.
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
// NAME<->BINDING LOCKSTEP (D-LK-INTERNAL-LINKAGE-FN-EMITTED-GLOBAL-FOREIGN-
// COLLISION, TF-C54): `definedBinding` is the exact binding companion of
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
            definedBySym_.emplace(ms.symbol.v, &ms);
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
    // definition`), which is the exact defect D-LK-INTERNAL-LINKAGE-FN-EMITTED-
    // GLOBAL-FOREIGN-COLLISION closed. A FINAL IMAGE is never re-linked, so its
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
    // the dynamic export). The c-subset CAN form a Global+Hidden symbol
    // (`__attribute__((visibility("hidden")))` on a NON-static fn -- e.g.
    // examples/c-subset/attributes_syntax's `dead_helper`), but none is routed
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
    std::unordered_map<std::uint32_t, ExternImport const*> externBySym_;
};

} // namespace dss::link::format
