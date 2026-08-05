#include "link/linker.hpp"

#include "core/types/object_format_kind.hpp"  // externCallUsesIndirectShape
#include "core/types/parse_diagnostic.hpp"
#include "core/types/section_kind.hpp"        // relocBearingGlobalSection (c145 chokepoint)
#include "link/cross_cu_resolve.hpp"
#include "link/entry_trampoline.hpp"
#include "link/format/elf.hpp"
#include "link/format/macho.hpp"
#include "link/format/pe.hpp"
#include "link/format/spirv.hpp"
#include "link/format/wasm.hpp"
#include "link/symbol_kind.hpp"
#include "lir/lir_pass_util.hpp"

#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dss::linker {

namespace {

using dss::report;

// D-LK4-3 — build the collision-proof compound-key symbol index for one module.
// Every function / data item / extern import is keyed by `(module.cuId, SymbolId)`
// so two CUs minting the same bare SymbolId stay distinct. A duplicate COMPOUND
// key — the same SymbolId declared twice in THIS CU (across functions, data, or
// externs) — is an ambiguous-resolution error: the `emplace`-failure detects it,
// unifying the former cross-table + within-table duplicate checks into one gate.
void buildCompoundIndex(std::unordered_map<LinkedSymbolKey, SymbolKind>& index,
                        AssembledModule const& m,
                        DiagnosticReporter& reporter) {
    auto declare = [&](SymbolId sym, SymbolKind kind, char const* what) {
        if (!index.emplace(LinkedSymbolKey{m.cuId, sym}, kind).second) {
            report(reporter, DiagnosticCode::K_SymbolUndefined,
                   DiagnosticSeverity::Error,
                   "symbol #" + std::to_string(sym.v) + " (CU #" +
                   std::to_string(m.cuId.v) + ") is declared more than once — this " +
                   what + " collides with a prior function / data item / extern import "
                   "of the same SymbolId in this CompilationUnit (ambiguous resolution).");
        }
    };
    for (auto const& fn : m.functions) declare(fn.symbol, SymbolKind::Defined, "function");
    // D-CSUBSET-COMPUTED-GOTO: each function's synthetic per-block symbols (the
    // `&&label` block-address `lea` relocation sources) are intra-module DEFINED
    // symbols — declare them so the per-CU relocation-resolvability check below
    // recognizes the block-address `lea`'s reloc target. The per-format walker
    // assigns each its interior-block VA before applying relocations.
    for (auto const& fn : m.functions)
        for (auto const& bs : fn.blockSymbols)
            declare(bs.symbol, SymbolKind::BlockLocal, "synthetic block symbol");
    for (auto const& di : m.dataItems) declare(di.symbol, SymbolKind::Data, "data item");
    for (std::size_t i = 0; i < m.externImports.size(); ++i) {
        auto const& ext = m.externImports[i];
        // Empty mangledName silently produces broken imports
        // (GetProcAddress("") → null IAT slot). Fail loud so the toolchain
        // owns the diagnostic. An empty libraryPath is NOT rejected here
        // (c86, D-CSUBSET-BARE-PROTO-EXTERN-SYNTHESIS): a bare-prototype
        // cross-TU reference legitimately carries no library — whether it is
        // UNDEFINED depends on cross-CU resolution AND on being referenced,
        // so that surface lives at the emission head
        // (`rejectOrDropUnreferencedExterns`), AFTER the multi-module reference
        // resolution has had its chance.
        if (ext.mangledName.empty()) {
            report(reporter, DiagnosticCode::K_SymbolUndefined,
                   DiagnosticSeverity::Error,
                   "ExternImport[" + std::to_string(i) + "] (symbol #" +
                   std::to_string(ext.symbol.v) + ") has empty mangledName — "
                   "import-table entries require a non-empty symbol name.");
        }
        declare(ext.symbol, SymbolKind::Extern, "extern import");
    }
}

// D-LINK-EXTERN-IMPORT-REFERENCE-GATE (generalizes c86's
// D-CSUBSET-BARE-PROTO-EXTERN-SYNTHESIS): the extern-import REFERENCE gate — ld's
// rule for what an emitted image / relocatable object actually imports. An extern
// import SURVIVES iff it is EAGER **or** REFERENCED; otherwise it is DROPPED.
// Runs on the FINAL emission module (post-LK11: a sibling-CU-resolved extern was
// already STRIPPED by the merge). "Referenced" = a relocation in ANY function or
// data item targets the extern's symbol (an aggregate global can hold a function
// pointer to an extern — the sqlite aSyscall[] shape — so data-item relocs count).
// The three outcomes:
//   * EAGER (isEagerImport) ⇒ KEEP unconditionally. A shipped-library descriptor
//     symbol (a `#include`d library export) is bound whether or not this TU
//     references it — the D-FFI-DESCRIPTOR-EAGER-IMPORT invariant (the descriptor
//     lists a real library export the loader must resolve; dropping it would
//     under-bind a genuine dependency). Eager ⟹ library-bound by construction.
//   * REFERENCED (non-eager) ⇒ split on library binding:
//       - library-bound (printf from libc): KEEP — the per-format walker emits
//         its import-table entry.
//       - UNBOUND (empty libraryPath — a bare-prototype cross-TU reference,
//         C 6.2.2p5): policy-keyed by the schema-driven `allowsUndefinedImports()`
//         (c150's three flavors): a RELOCATABLE `.o` / ELF ET_DYN `.so` a LATER
//         binder resolves KEEPS the row as a legal undefined symbol; an EXEC image
//         (nothing later binds it) rejects LOUD, naming the symbol (never an
//         IAT/DT_NEEDED entry with no owning image, which would defer the failure
//         to the loader or read a null slot).
//   * UNREFERENCED + NON-EAGER ⇒ DROPPED (bound OR unbound; returns false ⇒ the
//     caller swaps in `filtered`): gcc's rule that an unused extern declaration
//     emits no import. A source `extern int f(int);` never called, or a bare
//     prototype nobody uses (sqlite3.h declares the whole API; a config-gated
//     symbol may be defined nowhere), is dead declaration surface. ★ The
//     LIBRARY-BOUND half of this drop is the c86→reference-gate generalization:
//     BEFORE it, a non-eager row auto-bound to the format-default library (e.g. a
//     3-field `HirExternRecord` defaulting `noLibraryBinding=false`) flowed to the
//     walker and its DT_NEEDED / import entry broke the LOADER (ld.so "undefined
//     symbol" at load, exit 127) — the sole load-blocker for the SQLite
//     testfixture's declared-but-never-called `Sqlitetestsse_Init`. Dropping the
//     unbound half also avoids a broken import group (an empty DT_NEEDED /
//     IMAGE_IMPORT_DESCRIPTOR name).
// AGNOSTIC: the drop is UNIFORM across elf/pe/macho — no format-name branch. The
// only format-keyed decision is the EXEC-vs-later-binder reject, which reads the
// schema's `allowsUndefinedImports()` (never a format name). Returns true when `m`
// flowed through untouched (no drops), false when `filtered` holds the dropped-row
// copy. Rejects (if any) are reported regardless of the return value.
[[nodiscard]] bool rejectOrDropUnreferencedExterns(
    AssembledModule const& m,
    AssembledModule&       filtered,
    bool                   allowUndefinedExterns,
    DiagnosticReporter&    reporter) {
    // Candidate test: does ANY named import need the gate — i.e. is there a
    // NON-EAGER row? An eager row is always kept, so a module whose every named
    // import is eager has nothing to gate (the common `#include`-only fast path).
    // This MUST fire for a library-bound non-eager row too (not just unbound
    // ones): otherwise an unreferenced `Sqlitetestsse_Init` in a module with no
    // unbound rows would skip the gate and survive to the loader. Every unbound
    // row is non-eager (eager ⟹ bound), so `!isEagerImport` also covers them.
    bool anyCandidate = false;
    for (auto const& ext : m.externImports) {
        if (!ext.mangledName.empty() && !ext.isEagerImport) {
            anyCandidate = true;
            break;
        }
    }
    if (!anyCandidate) return true;   // all named imports eager → nothing to gate
    // Reference scan: every relocation target across functions AND data items.
    std::unordered_set<std::uint32_t> referenced;
    for (auto const& fn : m.functions) {
        for (auto const& rel : fn.relocations) referenced.insert(rel.target.v);
    }
    for (auto const& di : m.dataItems) {
        for (auto const& rel : di.relocations) referenced.insert(rel.target.v);
    }
    bool anyDrop = false;
    for (auto const& ext : m.externImports) {
        if (ext.mangledName.empty()) continue;   // already rejected (compound index)
        if (ext.isEagerImport) continue;         // eager law: keep even if unreferenced
        if (referenced.contains(ext.symbol.v)) {
            if (ext.libraryPath.empty()) {
                // REFERENCED + UNBOUND — the c143/c150 policy, UNCHANGED. The
                // schema-driven THREE-flavor `allowsUndefinedImports()`:
                //   * RELOCATABLE (.o/.obj): a LEGAL SHN_UNDEF symbol the FINAL
                //     (foreign) linker resolves (the `SQLITE_API` shape,
                //     D-LK-OBJECT-NOLIB-EXTERN-RELOCATABLE). KEEP.
                //   * ELF ET_DYN (.so): `ld -shared` semantics — ld.so resolves
                //     it from the global scope at load. KEEP.
                //   * EXEC image: nothing later binds it — reject LOUD, naming
                //     the symbol (c151 D-LK1-4: the PIE ET_DYN executable is THIS
                //     flavor, discriminated by its schema's entry cluster, never a
                //     format-name check).
                // ★★ This reject MUST stay gated on `libraryPath.empty()`: a
                //    referenced LIBRARY-BOUND row (printf) must NEVER reach it, or
                //    every referenced libc import on an exec would reject loud —
                //    catastrophic. The `else` below (referenced + library-bound)
                //    keeps the row silently.
                if (!allowUndefinedExterns) {
                    report(reporter, DiagnosticCode::K_SymbolUndefined,
                           DiagnosticSeverity::Error,
                           "undefined symbol '" + ext.mangledName + "' — the symbol "
                           "is referenced (a prototype/extern declaration with no "
                           "import library) but no linked compilation unit defines "
                           "it and no library import binds it. Provide a definition "
                           "in a linked translation unit, or declare the owning "
                           "library for the symbol.");
                }
                // else: kept — resolved by the final linker (relocatable) or
                // by ld.so's global scope at load (ELF ET_DYN).
            }
            // else: referenced + library-bound (printf) → KEEP. Do nothing.
        } else {
            anyDrop = true;   // unreferenced + non-eager ⇒ drop below (bound OR unbound)
        }
    }
    if (!anyDrop) return true;   // rejects (if any) reported; module unchanged
    filtered = m;   // copy, then erase the unreferenced non-eager rows
    std::erase_if(filtered.externImports, [&](ExternImport const& ext) {
        return !ext.mangledName.empty() && !ext.isEagerImport
            && !referenced.contains(ext.symbol.v);
    });
    return false;
}

// LK11a — the cross-CU symbol merge + resolution, in three steps:
//  (1) DEFINITION merge + weak-vs-strong: only Global/Weak definitions participate
//      (Local stays module-private); the surviving definition of each name lands in
//      `image.resolvedGlobalDefs` (name -> winning (cuId, SymbolId)). Order-INDEPENDENT:
//      a strong (Global) def always shadows weak; two strong defs of one name are an
//      ambiguous redefinition (K_SymbolRedefinedAcrossUnits); among all-weak defs the
//      lowest (cuId, SymbolId) wins (same result regardless of module order).
//  (2) REFERENCE resolution: an extern import whose name is DEFINED in a sibling CU is a
//      cross-CU reference — a local/sibling definition shadows the extern declaration, so
//      the reference binds to that definition (recorded in `image.resolvedCrossCuRefs`),
//      NOT a DLL import. An extern with no cross-CU definition stays a real FFI import.
//  (3) per-CU relocation resolution: every relocation target must be declared in its own
//      CU (a definition or an extern import) per the compound index, else K_SymbolUndefined.
// Byte patching against the resolved addresses is LK11b (needs the merged-image layout).
void resolveCrossCuSymbols(std::span<AssembledModule const> modules,
                           std::unordered_map<LinkedSymbolKey, SymbolKind> const& compoundIndex,
                           LinkedImage&        image,
                           DiagnosticReporter& reporter) {
    // (1) DEFINITION merge + weak-vs-strong — delegated to the PURE, tier-neutral
    // `resolveCrossCuDefs` kernel (Cycle 24). Flatten every module's symbol table into
    // `(name, binding, key)` triples (Local stays in — the kernel excludes it), resolve,
    // then emit ONE K_SymbolRedefinedAcrossUnits per recorded conflict event (K strong
    // defs of one name → K-1 events, so the diagnostic count is unchanged). The kernel
    // owns the strong-shadows-weak / all-weak-lowest-key policy AND hands back the
    // colliding key PAIR per conflict; this caller owns the diagnostic. Each conflict
    // names BOTH defining CompilationUnits (existing + incoming) — byte-for-byte the
    // original wording, before the conflict data was reduced to a name. The winner table
    // flows into reference-resolution + the resolvedGlobalDefs copy below.
    std::vector<CrossCuDef> defs;
    for (auto const& m : modules) {
        for (auto const& s : m.symbols) {
            defs.push_back(CrossCuDef{s.name, s.binding, LinkedSymbolKey{m.cuId, s.symbol}});
        }
    }
    CrossCuResolution const resolution = resolveCrossCuDefs(defs);
    for (auto const& c : resolution.conflicts) {
        report(reporter, DiagnosticCode::K_SymbolRedefinedAcrossUnits,
               DiagnosticSeverity::Error,
               "symbol \"" + c.name + "\" has multiple strong (Global) "
               "definitions across CompilationUnits (CU #" +
               std::to_string(c.existing.cuId.v) + " and CU #" +
               std::to_string(c.incoming.cuId.v) + ") — a strong symbol may be "
               "defined only once across the linked image.");
    }
    // (2) Reference resolution — an extern import whose name is DEFINED in a sibling CU
    // binds to that definition (the definition shadows the extern declaration). Record
    // the symbolic edge; LK11b patches the referencing relocations to the def's address.
    // An extern with no cross-CU definition stays a real FFI import (resolved via the
    // import table — unchanged). Local defs are not in `winners`, so a Local of the same
    // name never satisfies an extern (correct — Local is module-private).
    image.resolvedCrossCuRefs.clear();
    for (auto const& m : modules) {
        for (auto const& ext : m.externImports) {
            auto it = resolution.winners.find(ext.mangledName);
            if (it != resolution.winners.end()) {
                image.resolvedCrossCuRefs.push_back(LinkedImage::CrossCuRef{
                    LinkedSymbolKey{m.cuId, ext.symbol}, it->second});
            }
        }
    }
    // (3) Per-CU relocation resolution — every relocation target must resolve to a symbol
    // declared in its own CU (a definition or an extern import). An unresolved target is
    // undefined. (The compound index is keyed by (cuId, SymbolId), so the lookup is
    // per-CU.) Byte patching against the resolved address is LK11b.
    for (auto const& m : modules) {
        for (auto const& fn : m.functions) {
            for (auto const& rel : fn.relocations) {
                if (!compoundIndex.contains(LinkedSymbolKey{m.cuId, rel.target})) {
                    report(reporter, DiagnosticCode::K_SymbolUndefined,
                           DiagnosticSeverity::Error,
                           "relocation in CU #" + std::to_string(m.cuId.v) +
                           " targets symbol #" + std::to_string(rel.target.v) +
                           " which is not defined or imported in that CompilationUnit "
                           "(a cross-CU reference must be declared as an extern import).");
                }
            }
        }
        // D-LINK-DATA-RELOC-TARGET-VALIDATE: DATA-item relocations get the SAME
        // resolvability check as function relocations above — an abs64 pointer slot
        // in a symbol-address global / function-pointer table / jump table (the c67
        // encodeAggregateValue arm, the c70 switch table) targets a SymbolId that
        // must be declared in this CU (the compound index covers functions, data
        // items, block symbols, and extern imports). Closing this asymmetry makes a
        // mis-remapped or dangling DATA reloc target fail LOUD at link
        // (K_SymbolUndefined) instead of surfacing as a SILENT wrong/NULL pointer at
        // runtime, exactly the class the function-reloc loop already guards.
        for (auto const& di : m.dataItems) {
            for (auto const& rel : di.relocations) {
                if (!compoundIndex.contains(LinkedSymbolKey{m.cuId, rel.target})) {
                    report(reporter, DiagnosticCode::K_SymbolUndefined,
                           DiagnosticSeverity::Error,
                           "data-item relocation in CU #" + std::to_string(m.cuId.v) +
                           " (data SymbolId #" + std::to_string(di.symbol.v) +
                           ") targets symbol #" + std::to_string(rel.target.v) +
                           " which is not defined or imported in that CompilationUnit "
                           "(a cross-CU reference must be declared as an extern import).");
                }
            }
        }
    }
    image.resolvedGlobalDefs.clear();
    for (auto const& [name, key] : resolution.winners) image.resolvedGlobalDefs.emplace(name, key);
    image.symbolCount = image.resolvedGlobalDefs.size();
}

// LK11b — pre-merge N resolved CUs into ONE combined AssembledModule, so the existing
// single-CU format walker emits the merged image (no per-format N-module rewrite).
// Symbols are remapped into a fresh unified id space: each winning Global/Weak
// DEFINITION (image.resolvedGlobalDefs) gets one fresh id that all same-name versions
// fold onto (dedup); a Local symbol gets a distinct fresh id (module-private — two CUs'
// SymbolId{3} locals are different functions). Every function's relocations are
// retargeted from (its cuId, old SymbolId) to the merged id.
//
// Cross-CU REFERENCE resolution is keyed on the FORMAT's declared extern-call shape
// (`externCallDispatch` — config, never format identity; c154):
//
//   * `direct-plt` (every shipped format) or UNDECLARED: the call site is a plain
//     direct call (E8 rel32 / BL imm26) — the reference retargets DIRECTLY to the
//     sibling definition's merged id, exactly like an intra-CU call after the merge
//     (the definition is IN the merged image). Correct for every reference shape:
//     a direct call branches to the def; an address-taken abs64 data slot gets the
//     def's real address (pointer identity holds). Pre-c154 this arm did not exist —
//     the merge unconditionally minted the indirect slot below and retargeted the
//     DIRECT call into the slot's DATA bytes (a silent branch-to-data SIGSEGV,
//     witnessed on elf-exec + pe-exec before the fix). An UNDECLARED dispatch means
//     the format cannot lower extern CALLS at all (MIR->LIR fails loud), so any
//     surviving reference is data-shaped — direct retarget is correct there too.
//
//   * `indirect-slot`: the call site DEREFERENCES a pointer slot (`call qword ptr
//     [slot]`, x86 `FF 15 disp32` — see `mir_to_lir.cpp` CallIndirectViaExtern).
//     For each extern bound to a sibling-CU definition (image.resolvedCrossCuRefs)
//     the merge mints a fresh 8-byte data item — the GOT-like thunk slot — carrying
//     ONE absolute-64-bit-pointer relocation to the definition, and retargets the
//     reference's merged id to THAT SLOT (not the def). The slot is a CONST pointer
//     table written only by the loader, so it mints as `RelRoConst` via the shared
//     c145 `relocBearingGlobalSection` chokepoint (const + reloc-bearing -> relro;
//     pre-c154 it minted as `Rodata`, the D-LK-DYN-RODATA-ITEM-RELOC loud wall on
//     the ET_DYN arm and the Mach-O exec __TEXT,__const rebase reject).
//
// Either way the extern import is STRIPPED (the sibling def shadows the library
// fallback). An extern with NO cross-CU definition is a real FFI import, kept
// (remapped) for the library tier (FF11).
//
// The absolute-pointer relocation kind is found AGNOSTICALLY from the `TargetSchema` —
// the row whose `relocationInfo` reports `widthBytes == 8 && !pcRelative` — never by a
// hardcoded "abs64" name / kind constant (the standing source/target/format agnosticism
// veto). If no such row exists, fail loud (K_AbsolutePointerRelocMissing); a thunk slot
// without its abs64 fixup would be a broken null pointer.
AssembledModule mergeModules(std::span<AssembledModule const> modules,
                             LinkedImage const&        image,
                             TargetSchema const&       targetSchema,
                             ObjectFormatSchema const& objectFormatSchema,
                             DiagnosticReporter&       reporter) {
    AssembledModule combined;

    // The format's extern-call shape decides the reference-resolution mechanism
    // below: an indirect-slot call site needs the deref-able thunk slot; a
    // direct-plt (or undeclared-dispatch) reference binds straight to the def.
    bool const useIndirectSlot =
        objectFormatSchema.externCallDispatch().has_value()
        && externCallUsesIndirectShape(*objectFormatSchema.externCallDispatch());

    // Find the target's ABSOLUTE 64-bit pointer relocation kind by FORMULA (never by
    // name/constant — agnosticism). This is the relocation the thunk slot carries so the
    // format walker writes the sibling def's runtime address into the slot bytes.
    // Resolved ONCE; consumed only if there is at least one cross-CU reference to thunk.
    std::optional<RelocationKind> absPtrKind;
    for (auto const& r : targetSchema.relocations()) {
        if (r.widthBytes == 8 && !r.pcRelative) { absPtrKind = r.kind; break; }
    }

    // Per-module SymbolId.v -> ModuleSymbol for O(1) name/binding lookup during remap.
    std::vector<std::unordered_map<std::uint32_t, ModuleSymbol const*>> byId(modules.size());
    for (std::size_t i = 0; i < modules.size(); ++i) {
        for (auto const& ms : modules[i].symbols) byId[i].emplace(ms.symbol.v, &ms);
    }

    // (cuId, old SymbolId) -> fresh merged SymbolId.v. Pre-assign one fresh id per
    // resolved global name (the winning definition); same-name versions fold onto it.
    std::unordered_map<LinkedSymbolKey, std::uint32_t> remap;
    std::unordered_map<std::string, std::uint32_t>     nameToId;
    std::uint32_t nextId = 1;
    for (auto const& [name, key] : image.resolvedGlobalDefs) {
        std::uint32_t const id = nextId++;
        remap.emplace(key, id);
        nameToId.emplace(name, id);
    }
    auto mergedIdFor = [&](std::size_t modIdx, SymbolId old) -> std::uint32_t {
        LinkedSymbolKey const key{modules[modIdx].cuId, old};
        if (auto it = remap.find(key); it != remap.end()) return it->second;
        // Not a pre-assigned winner: fold an externally-visible same-name def onto its
        // winner; otherwise mint a fresh id (Local / extern import / unnamed).
        if (auto sit = byId[modIdx].find(old.v); sit != byId[modIdx].end()) {
            ModuleSymbol const& ms = *sit->second;
            if (ms.binding != SymbolBinding::Local) {
                if (auto nit = nameToId.find(ms.name); nit != nameToId.end()) {
                    remap.emplace(key, nit->second);
                    return nit->second;
                }
            }
        }
        std::uint32_t const id = nextId++;
        remap.emplace(key, id);
        return id;
    };
    // Single chokepoint for relocation retargeting — functions AND data items route
    // through it, so a future change can't retarget one kind and miss the other.
    auto retargetRelocs = [&](std::size_t modIdx, std::vector<Relocation>& relocs) {
        for (auto& rel : relocs) rel.target = SymbolId{mergedIdFor(modIdx, rel.target)};
    };

    // Cross-CU REFERENCE resolution. An extern import bound to a sibling-CU definition
    // (image.resolvedCrossCuRefs) resolves to that def; the MECHANISM is dispatch-keyed
    // (see the function docblock):
    //   * direct-plt / undeclared → retarget the reference's merged id DIRECTLY to the
    //     definition's merged id (via `remap` — every referencing reloc then routes
    //     through `retargetRelocs` to the def, like an intra-CU reference).
    //   * indirect-slot → mint a fresh RelRoConst thunk slot (8 zero bytes + ONE
    //     absolute-64-bit relocation to the definition's merged id; the walker writes
    //     the def's runtime VA into the slot) and retarget the reference to THE SLOT —
    //     the indirect call site dereferences it. CONSTRAINT (c154 review): this arm
    //     retargets EVERY reference shape to the slot, which is correct ONLY for
    //     slot-deref-shaped sites (`FF 15` calls / GotIndirect data derefs). An
    //     ADDRESS-TAKEN cross-CU reference (`lea`-of-extern-fn, an abs64 `&extern_fn`
    //     data initializer) would receive the SLOT's address — one indirection off.
    //     Unreachable today (no shipped format declares indirect-slot and no shipped
    //     route reaches N>1 modules); when the separate-compilation trigger fires
    //     (D-OPT7-CROSSCU-THUNK-RESERVED-FOR-SEPARATE-COMPILATION), retargeting must
    //     key per-reference-shape or fail loud on a non-call-shaped reference here.
    // Either way the extern import is STRIPPED (the sibling def shadows the library
    // fallback). A real FFI extern (no sibling def, absent from resolvedCrossCuRefs) is
    // untouched — that is FF11's library tier. The definition's merged id is resolvable
    // from `remap` here (the def is a winning global, pre-assigned above).
    std::unordered_map<std::uint32_t, std::size_t> cuIdToIdx;
    for (std::size_t i = 0; i < modules.size(); ++i) cuIdToIdx.emplace(modules[i].cuId.v, i);
    std::unordered_set<LinkedSymbolKey> strippedExterns;
    if (useIndirectSlot && !image.resolvedCrossCuRefs.empty() && !absPtrKind.has_value()) {
        // The target cannot express a 64-bit absolute pointer fixup — a thunk slot would be
        // an un-relocated null. Fail loud rather than emit an image whose cross-CU indirect
        // calls dereference a null slot. (The direct-plt arm needs no pointer slot, so a
        // target without an abs64 row still cross-CU-links there.)
        report(reporter, DiagnosticCode::K_AbsolutePointerRelocMissing,
               DiagnosticSeverity::Error,
               "cross-CU reference resolution needs an absolute 64-bit pointer relocation "
               "(widthBytes == 8 && !pcRelative) to mint a thunk slot, but target schema '" +
               std::string{targetSchema.name()} + "' declares no such relocation kind — "
               "an indirect cross-CU call would dereference an un-relocated null slot. Add "
               "the absolute-64-bit relocation row to the target's *.target.json.");
        return combined;  // half-merge aborted; caller's errorCount delta short-circuits emit
    }
    for (auto const& ref : image.resolvedCrossCuRefs) {
        auto const dit = cuIdToIdx.find(ref.definition.cuId.v);
        if (dit == cuIdToIdx.end()) {
            // INVARIANT: resolveCrossCuSymbols populated `ref.definition` from THIS same
            // `modules` span, so its CU is always present. A miss means a future refactor
            // breached that contract — fail LOUD instead of silently skipping: a silent
            // `continue` would leave the reference unresolved (no retarget) yet the
            // extern un-stripped, surfacing as a confusing downstream undefined rather than
            // pointing at the breached merge contract here.
            report(reporter, DiagnosticCode::K_SymbolUndefined, DiagnosticSeverity::Error,
                   "cross-CU reference resolution: the definition's CompilationUnit (cuId " +
                   std::to_string(ref.definition.cuId.v) + ") is not in the merge span — the "
                   "resolvedCrossCuRefs invariant (definition comes from a merged CU) was "
                   "breached.");
            continue;
        }
        std::uint32_t const defId = mergedIdFor(dit->second, ref.definition.symbol);
        if (useIndirectSlot) {
            std::uint32_t const thunkSlotId = nextId++;
            AssembledData slot;
            slot.symbol  = SymbolId{thunkSlotId};
            // A CONST pointer table the loader fills — the shared c145 chokepoint
            // routes it to RelRoConst (relocated-then-read-only), NEVER read-only
            // Rodata (the pre-c154 D-LK-DYN-RODATA-ITEM-RELOC wall) and NEVER a
            // format-identity branch.
            slot.section = relocBearingGlobalSection(/*isThreadLocal=*/false,
                                                     /*isConst=*/true);
            slot.bytes.assign(8, std::uint8_t{0});  // 8 zero bytes — the abs64 fixup site
            Relocation slotRel;
            slotRel.offset = 0;
            slotRel.target = SymbolId{defId};       // the sibling def's merged id
            slotRel.kind   = *absPtrKind;           // abs64 (found by formula)
            slotRel.addend = 0;
            slot.relocations.push_back(slotRel);
            combined.dataItems.push_back(std::move(slot));
            remap[ref.reference] = thunkSlotId;     // the indirect call's reloc → the slot
        } else {
            remap[ref.reference] = defId;           // direct bind — reloc → the def itself
        }
        strippedExterns.insert(ref.reference);
    }

    // ── D-LK11-EXTERN-IMPORT-DEDUP — coalesce the merged CUs' duplicate extern
    // imports into ONE row per imported DYNAMIC SYMBOL, and FOLD their payloads.
    //
    // Before this pass the extern path CONCATENATED. Two CUs both importing
    // `puts` from `msvcrt.dll` produced TWO rows: `mergedIdFor` mints a FRESH id
    // per extern (its name-fold consults `nameToId`, which is populated only from
    // `image.resolvedGlobalDefs` — DEFINITIONS, never imports), so nothing ever
    // folded them and the walkers faithfully emitted both. MEASURED at 2 CUs: PE
    // → 2 IAT/ILT thunk rows + 2 `IMAGE_IMPORT_BY_NAME` in one descriptor; ELF →
    // 2 `.dynsym` entries carrying the IDENTICAL name, 2 `.rela.dyn` GLOB_DAT
    // rows, and a 24-byte `.got` where 16 is correct. 4 CUs → 4 of each.
    //
    // DEDUP KEY = (mangledName, libraryPath, version) — the triple that IDENTIFIES
    // a dynamic symbol, and nothing weaker:
    //   * `libraryPath` is IN the key: `foo` from `a.dll` and `foo` from `b.dll`
    //     are different imports (it is the very field the walkers group DT_NEEDED
    //     / IMAGE_IMPORT_DESCRIPTOR / LC_LOAD_DYLIB by).
    //   * `version` is IN the key: `puts@GLIBC_2.2.5` and `puts@GLIBC_2.17` are
    //     genuinely different dynamic symbols (c156 D-LK-ELF-SYMBOL-VERSIONING).
    //     Folding them would bind one of the two call sites to the wrong glibc
    //     compat form — precisely the misbind c156 exists to prevent, so a
    //     name-only key would REINTRODUCE it here.
    // The key is LENGTH-PREFIXED, not separator-joined: a mangledName is arbitrary
    // bytes from a descriptor, so any separator-joined encoding is non-injective
    // (two different triples could collide into one key and fold two UNRELATED
    // imports). AGNOSTIC: the fold is structural equality on declared data — a
    // `.dynsym` row and an IAT slot dedup by the same rule, no format/arch/
    // language branch.
    //
    // RETARGETING IS FREE — pre-seeding `remap` is the WHOLE retarget. `mergedIdFor`
    // consults `remap` FIRST, and every consumer of a symbol id routes through it:
    // the single `retargetRelocs` chokepoint (functions AND data items),
    // `combined.symbols`, and `userEntrySymbol`. Same mechanism the cross-CU
    // reference bind above already uses.
    //
    // PLACEMENT is forced: AFTER the `resolvedGlobalDefs` pre-assignment and AFTER
    // the thunk-slot mint (both seed `remap` / consume `nextId`), and BEFORE the
    // emission loop (module 0's relocations are retargeted in that loop's first
    // iteration, before module 1's externs would otherwise have been seen).
    // A cross-CU-resolved extern is SKIPPED: its `remap` entry already points at
    // the sibling def / thunk slot, and overwriting that would un-bind the call.
    //
    // ★ THE PAYLOAD IS FOLDED, NEVER DROPPED. Keeping the first row and discarding
    // the rest is a SILENT MISCOMPILE, not a size win — see the per-field rules at
    // each site below.
    //
    // ★★ THE TWO TIERS NOW AGREE, AND THAT IS LOAD-BEARING — keep them in step.
    // The MIR-tier merge (mir_merge.cpp `ffiCanonicalForName` + `survivingExterns`)
    // performs the same fold on the LIVE route (`--compile a.c b.c`, every
    // `--project` build, both sqlite legs); this assembled-tier merge is reached
    // only via `--resolve-library`. Both key on the SAME triple
    // (mangledName, libraryPath, version) and both FAIL LOUD on the same four
    // fields. ⚠ HISTORY, so the weaker shape is not re-introduced as a
    // "simplification": until TF-C119 the MIR tier keyed on mangledName ALONE —
    // folding across libraryPath AND across `version`, which is the c156
    // D-LK-ELF-SYMBOL-VERSIONING misbind shape — and was first-wins on every field
    // but `isEagerImport`. The safer tier had been hardened first and the reachable
    // one left weaker; they were harmonized in the same cycle. A divergence here is
    // a defect, not a design choice.
    {
        // Length-prefixed ⇒ injective over arbitrary field bytes.
        auto const dedupKey = [](ExternImport const& e) {
            return std::format("{}:{}|{}:{}|{}:{}",
                               e.mangledName.size(), e.mangledName,
                               e.libraryPath.size(), e.libraryPath,
                               e.version.size(), e.version);
        };
        // A disagreement between two CUs about ONE dynamic symbol is a REAL
        // conflict, never a pick-one. It gets its OWN code rather than borrowing
        // `K_SymbolRedefinedAcrossUnits`: an import ATTRIBUTE conflict is not a
        // redefinition, and a misfiled code sends the reader hunting the wrong
        // fault — the defect class TF-C118 closed when every optimizer diagnostic
        // was printing under the parser's letter.
        auto const conflict = [&](ExternImport const& e, char const* field,
                                  std::string const& kept,
                                  std::string const& incoming) {
            report(reporter, DiagnosticCode::K_ExternImportAttributeConflict,
                   DiagnosticSeverity::Error,
                   "extern import \"" + e.mangledName + "\"" +
                   (e.libraryPath.empty() ? std::string{}
                                          : " (library \"" + e.libraryPath + "\")") +
                   (e.version.empty() ? std::string{}
                                      : " (version \"" + e.version + "\")") +
                   " is declared with conflicting " + field +
                   " across CompilationUnits (" + kept + " vs " + incoming +
                   ") — one dynamic symbol cannot be imported two ways "
                   "(D-LK11-EXTERN-IMPORT-DEDUP).");
        };
        // `dataSizeBytes` / `dataAlignBytes` SIZE the ELF copy-relocation `.bss`
        // slot (c84 D-LK-EXTERN-DATA-IMPORT). One CU legitimately holds an
        // INCOMPLETE type (`extern const char v[];` ⇒ 0/0 — extern_import.hpp
        // :76-81), so a zero is "unknown here", not a disagreement: take the
        // non-zero. Two DIFFERING non-zero values would reserve the SAME slot two
        // ways — the loader memcpy's `st_size` bytes, so picking either silently
        // truncates or over-copies. Fail loud.
        auto const foldNonZero = [&](std::uint64_t& kept, std::uint64_t incoming,
                                     char const* field, ExternImport const& e) {
            if (incoming == 0 || kept == incoming) return;  // incomplete / agrees
            if (kept == 0) { kept = incoming; return; }     // the complete type wins
            conflict(e, field, std::to_string(kept), std::to_string(incoming));
        };
        auto const boolStr = [](bool b) { return std::string{b ? "true" : "false"}; };

        std::unordered_map<std::string, std::size_t> externIdxForKey;
        for (std::size_t i = 0; i < modules.size(); ++i) {
            for (auto const& ext : modules[i].externImports) {
                LinkedSymbolKey const key{modules[i].cuId, ext.symbol};
                if (strippedExterns.contains(key)) continue;  // bound to a sibling def
                auto const [kit, firstOfItsKind] =
                    externIdxForKey.try_emplace(dedupKey(ext),
                                                combined.externImports.size());
                if (firstOfItsKind) {
                    ExternImport out = ext;
                    out.symbol = SymbolId{mergedIdFor(i, ext.symbol)};  // the CANONICAL id
                    combined.externImports.push_back(std::move(out));
                    continue;
                }
                ExternImport& kept = combined.externImports[kit->second];
                // THE retarget: every relocation / symbol row that named this CU's
                // copy now resolves to the canonical id through `mergedIdFor`.
                // A pre-existing DIFFERENT mapping is impossible (`buildCompoundIndex`
                // guarantees an extern's SymbolId is unique within its CU, so an
                // extern key can never collide with a pre-assigned definition key) —
                // if it ever happens the compound-index contract was breached, and a
                // silent overwrite would mis-bind a call, so say so LOUD.
                if (auto const [rit, fresh] = remap.emplace(key, kept.symbol.v);
                    !fresh && rit->second != kept.symbol.v) {
                    report(reporter, DiagnosticCode::K_SymbolUndefined,
                           DiagnosticSeverity::Error,
                           "extern import \"" + ext.mangledName + "\" (CU #" +
                           std::to_string(modules[i].cuId.v) + ", symbol #" +
                           std::to_string(ext.symbol.v) + ") already maps to merged "
                           "id " + std::to_string(rit->second) + " but its dedup group "
                           "is canonicalized to " + std::to_string(kept.symbol.v) +
                           " — the (cuId, SymbolId) uniqueness contract the compound "
                           "index enforces was breached (D-LK11-EXTERN-IMPORT-DEDUP).");
                }
                // `isEagerImport` — OR-COMBINE, as extern_import.hpp:112-114 mandates
                // and the MIR-tier merge implements. Keeping a NON-eager row when a
                // sibling CU declared the same import EAGER would let
                // `rejectOrDropUnreferencedExterns` DROP a shipped-descriptor symbol
                // the loader must bind (D-FFI-DESCRIPTOR-EAGER-IMPORT) — that is a
                // LOAD failure (pe 0xC0000139 / elf exit 127), not a size regression.
                // Order-INDEPENDENT: whichever CU lands first, the bit is ORed in.
                kept.isEagerImport = kept.isEagerImport || ext.isEagerImport;
                // `isData` / `isThreadLocal` — a disagreement is a REAL conflict, and
                // silently picking either row is the D-LK-EXTERN-DATA-IMPORT
                // silent-miscompile shape: `isData` decides whether the walker binds
                // the name through the DATA-slot model (the ELF copy-relocation) or
                // the function-import path, so the loser's CU would have every
                // reference bound through the WRONG model — a PLT stub standing in
                // for a data object, or a copy-reloc `.bss` slot standing in for a
                // function. `isThreadLocal` likewise selects the (unimplemented,
                // walker-rejected) initial-exec TLS model — D-CSUBSET-THREAD-LOCAL.
                if (kept.isData != ext.isData) {
                    conflict(ext, "`isData` (data object vs function import)",
                             boolStr(kept.isData), boolStr(ext.isData));
                }
                if (kept.isThreadLocal != ext.isThreadLocal) {
                    conflict(ext, "`isThreadLocal` (thread storage duration)",
                             boolStr(kept.isThreadLocal), boolStr(ext.isThreadLocal));
                }
                foldNonZero(kept.dataSizeBytes,  ext.dataSizeBytes,
                            "`dataSizeBytes` (copy-relocation slot size)", ext);
                foldNonZero(kept.dataAlignBytes, ext.dataAlignBytes,
                            "`dataAlignBytes` (copy-relocation slot alignment)", ext);
                // Every remaining ExternImport field is accounted for: `symbol` IS the
                // dedup output (replaced by the canonical merged id above), and
                // `mangledName` / `libraryPath` / `version` are the KEY, hence equal
                // by construction. No field is carried over silently.
            }
        }
    }

    // A defined symbol (function or data) is SHADOWED when it is an externally-visible
    // (Global/Weak) definition whose name's WINNING definition (image.resolvedGlobalDefs)
    // lives at a DIFFERENT (cuId, SymbolId) — i.e. this body lost the weak-vs-strong /
    // all-weak resolution. Its body MUST be DROPPED (not emitted): every reference to the
    // name already folds onto the winner's merged id via `mergedIdFor`, so emitting the
    // loser's body would mint a SECOND function/data item carrying the SAME merged id —
    // the within-image duplicate-SymbolId collision the compound index rejects. This is
    // the EMISSION-tier completion of strong-over-weak: the resolution layer picks the
    // winner; here we drop the loser's bytes (exactly what a real linker does — the
    // shadowed weak body never lands in the image). Local / unnamed bodies are never
    // shadowed (Local is module-private; absent from resolvedGlobalDefs).
    auto isShadowedDuplicate = [&](std::size_t modIdx, SymbolId sym) -> bool {
        auto sit = byId[modIdx].find(sym.v);
        if (sit == byId[modIdx].end()) return false;          // unnamed / synthesized — keep
        ModuleSymbol const& ms = *sit->second;
        if (ms.binding == SymbolBinding::Local) return false; // module-private — keep
        auto wit = image.resolvedGlobalDefs.find(ms.name);
        if (wit == image.resolvedGlobalDefs.end()) return false;  // not a resolved global — keep
        LinkedSymbolKey const winner = wit->second;
        LinkedSymbolKey const self{modules[modIdx].cuId, sym};
        return !(winner == self);  // a different key won → this body is shadowed
    };

    std::unordered_set<std::uint32_t> seenMergedSymIds;
    for (std::size_t i = 0; i < modules.size(); ++i) {
        auto const& m = modules[i];
        for (auto const& fn : m.functions) {
            if (isShadowedDuplicate(i, fn.symbol)) continue;  // shadowed weak body — drop
            AssembledFunction out = fn;  // bytes + relocations + sourceMap copied
            out.symbol = SymbolId{mergedIdFor(i, fn.symbol)};
            retargetRelocs(i, out.relocations);
            combined.functions.push_back(std::move(out));
        }
        for (auto const& di : m.dataItems) {
            if (isShadowedDuplicate(i, di.symbol)) continue;  // shadowed global data — drop
            AssembledData out = di;
            out.symbol = SymbolId{mergedIdFor(i, di.symbol)};
            retargetRelocs(i, out.relocations);  // same chokepoint as the function path
            combined.dataItems.push_back(std::move(out));
        }
        // `combined.externImports` is NOT built here: the extern path is owned
        // ENTIRELY by the D-LK11-EXTERN-IMPORT-DEDUP pass above (it strips the
        // cross-CU-resolved rows, canonicalizes each remaining import to one row,
        // and folds the duplicates' payloads into it). Emitting here too would
        // re-introduce the duplicates the pass just coalesced. Row ORDER is
        // unchanged — that pass walks the same module-major traversal, so imports
        // still appear in first-occurrence order.
        //
        // Carry each SURVIVING definition's ModuleSymbol row (name / binding /
        // visibility), re-keyed to the merged id — the walkers' real-name surfaces
        // (the c150 ET_DYN `.dynsym` exports, the c139 ET_REL `.symtab` names, the
        // dylib/DLL export tables) all read `module.symbols`, so a merge that drops
        // the table would silently emit an EXPORT-LESS library image (witnessed on
        // the c154 dyn probe before this rebuild: an empty `.dynsym`). A shadowed
        // duplicate's row is dropped with its body; same-name versions folded onto
        // one winner id keep exactly the winner's row (`seenMergedSymIds` dedups —
        // defensive; the shadow drop already excludes losers).
        for (auto const& ms : m.symbols) {
            if (ms.name.empty()) continue;
            if (isShadowedDuplicate(i, ms.symbol)) continue;
            std::uint32_t const mergedId = mergedIdFor(i, ms.symbol);
            if (!seenMergedSymIds.insert(mergedId).second) continue;
            combined.symbols.push_back(ModuleSymbol{
                SymbolId{mergedId}, ms.name, ms.binding, ms.visibility});
        }
    }
    combined.expectedFuncCount = combined.functions.size();

    // Exactly one CU may name the user entry; more than one is an ambiguous cross-CU
    // entry (the merged image has exactly one entry point) — fail loud.
    bool entrySet = false;
    for (std::size_t i = 0; i < modules.size(); ++i) {
        if (!modules[i].userEntrySymbol.has_value()) continue;
        if (entrySet) {
            report(reporter, DiagnosticCode::K_SymbolRedefinedAcrossUnits,
                   DiagnosticSeverity::Error,
                   "more than one CompilationUnit names a user entry symbol — the merged "
                   "image has exactly one entry point.");
            continue;
        }
        combined.userEntrySymbol = SymbolId{mergedIdFor(i, *modules[i].userEntrySymbol)};
        entrySet = true;
    }
    // combined.cuId stays default — the merged image is not a single CU.
    return combined;
}

} // namespace

LinkedImage link(std::span<AssembledModule const> modules,
                 TargetSchema const&       targetSchema,
                 ObjectFormatSchema const& objectFormatSchema,
                 DiagnosticReporter&       reporter,
                 ImageRequest const&       request) {
    LinkedImage image;
    image.format = objectFormatSchema.kind();

    // ── Per-PROGRAM image-request gate (D-SQLITE-PE64-FULL-TIER-STACK-DEPTH) ──
    //
    // Runs FIRST, before any module work: a request the chosen format cannot
    // honour is a build error regardless of what the modules contain, and
    // reporting it up-front costs the user nothing.
    //
    // The decision itself lives in `enforceImageRequest` (link/image_request)
    // so the gate and every walker arm share ONE implementation -- the first
    // cut split them and the PE walker ended up checking the CAPABILITY but
    // not the declared BOUNDS, which let a direct `pe::encode` call write an
    // out-of-range reserve and report success.
    //
    // What it asks is a declared CAPABILITY, never a format identity: the
    // capability is not even uniform within one kind -- `pe64-...-exec`
    // declares it (the Windows loader reads SizeOfStackReserve) while
    // `pe64-...-dll` does not (the loader IGNORES a DLL's copy of the same
    // field), so an `if (kind == Pe)` branch here would write a field nothing
    // reads. ELF declares nothing because no ELF image field carries a stack
    // SIZE at all.
    //
    // This is the chokepoint every compile-driven emission passes through, so
    // a request can never slip past into a walker that would drop it silently
    // -- the whole failure mode being a build that reports success while
    // emitting an image whose stack is not what was asked for.
    if (!enforceImageRequest(request, objectFormatSchema, "linker", reporter)) {
        image.resolvedFuncCount = 0;
        return image;
    }

    // D-LK4-3 — N==0 is a caller error; N>1 (cross-CU) builds the collision-proof
    // compound-key index + validates each CU, then fail-louds: the multi-CU image
    // MERGE (cross-CU name resolution + weak-vs-strong) is LK11. N==1 is the
    // single-CU path below (full image emission, behavior unchanged).
    if (modules.empty()) {
        report(reporter, DiagnosticCode::K_CrossCuMergeUnsupported,
               DiagnosticSeverity::Error,
               "linker::link received no modules to link.");
        return image;
    }
    // N>1: cross-CU merge (LK11a resolution + LK11b pre-merge emission). Validate each
    // CU, resolve symbols, then pre-merge the resolved CUs into ONE combined module that
    // flows through the SAME single-CU emission path below (kind validation + walker).
    // N==1 uses the sole module directly (path unchanged).
    AssembledModule mergedStorage;                 // populated only for N>1
    AssembledModule const* selectedInput = &modules[0];
    if (modules.size() > 1) {
        std::size_t const errsBeforeMerge = reporter.errorCount();
        // Per-CU validation (within-CU duplicate SymbolId + empty extern name) via
        // the same compound-key gate the single-CU path uses; cross-CU entries never
        // collide (distinct cuId), so one shared index validates every CU. An
        // empty extern libraryPath is NOT rejected here (c86): whether it is an
        // undefined symbol depends on the cross-CU resolution below — the
        // shared post-merge `rejectOrDropUnreferencedExterns` owns that surface.
        std::unordered_map<LinkedSymbolKey, SymbolKind> compoundIndex;
        for (auto const& m : modules) buildCompoundIndex(compoundIndex, m, reporter);
        // DEFINITION merge + weak-vs-strong (-> resolvedGlobalDefs) + REFERENCE
        // resolution (-> resolvedCrossCuRefs) + per-CU undefined-reloc check.
        resolveCrossCuSymbols(modules, compoundIndex, image, reporter);
        if (reporter.errorCount() != errsBeforeMerge) {
            // A within-CU duplicate, a cross-CU redefinition, or an undefined reference —
            // fail-loud already reported; do not emit a half-merged image.
            return image;
        }
        // Pre-merge into one combined module + flow it through the emission path below.
        // `targetSchema` is threaded in so the merge can find the absolute-64-bit pointer
        // relocation kind by formula (thunk-slot minting) — never by a hardcoded name;
        // `objectFormatSchema` so the reference-resolution mechanism keys on the format's
        // DECLARED `externCallDispatch` (direct bind vs indirect thunk slot — c154).
        mergedStorage = mergeModules(modules, image, targetSchema,
                                     objectFormatSchema, reporter);
        if (reporter.errorCount() != errsBeforeMerge) {
            return image;  // merge fail-loud (ambiguous entry / cross-CU ref pending).
        }
        selectedInput = &mergedStorage;
    }
    // D-LINK-EXTERN-IMPORT-REFERENCE-GATE (generalizes c86): the extern-import
    // reference gate, on the FINAL emission module (post-merge — a sibling-
    // resolved extern was already stripped). An import survives iff EAGER or
    // REFERENCED: a REFERENCED unbound extern on an exec rejects LOUD (naming the
    // symbol) and aborts before any emission state; an UNREFERENCED NON-EAGER
    // import (library-bound OR unbound) is DROPPED (gcc's unused-decl-emits-
    // nothing rule — see rejectOrDropUnreferencedExterns) so no walker ever
    // emits a spurious import (the load-blocker for a defaulted `extern` the TU
    // never calls). An EAGER `#include`d descriptor import is kept regardless.
    // Placed BEFORE externImportNames / the data-import gate / the trampoline so
    // every downstream consumer sees the filtered module.
    AssembledModule unboundFilteredStorage;   // populated only when rows drop
    {
        std::size_t const errsBeforeUnbound = reporter.errorCount();
        if (!rejectOrDropUnreferencedExterns(*selectedInput, unboundFilteredStorage,
                                        objectFormatSchema.allowsUndefinedImports(),
                                        reporter)) {
            selectedInput = &unboundFilteredStorage;
        }
        if (reporter.errorCount() != errsBeforeUnbound) {
            // Undefined symbol(s) reported — not valid emission input.
            image.resolvedFuncCount = 0;
            return image;
        }
    }
    AssembledModule const& inputModule = *selectedInput;
    image.expectedFuncCount = inputModule.expectedFuncCount;
    // The import-table symbol names the emitted image carries (cross-CU-resolved externs
    // were stripped from the merged module, so they do not appear — see externImportNames).
    image.externImportNames.clear();
    for (auto const& ext : inputModule.externImports) {
        image.externImportNames.push_back(ext.mangledName);
    }

    // c82+c84 (D-LK-EXTERN-DATA-IMPORT): a DATA import that SURVIVED — no
    // sibling-CU definition resolved it away at the merge — binds per the
    // format's SCHEMA-DECLARED `dataImportBinding` model (c84: the ELF
    // exec formats declare "copy-relocation" and their walker reserves an
    // exec-local `.bss` copy slot + emits the R_*_COPY dynamic reloc). A
    // format that declares NO model (PE / Mach-O until their `__imp_`
    // data-thunk / non-lazy-pointer models land; every relocatable
    // flavor) REJECTS LOUD here, the one chokepoint all format walkers
    // share: its import walker binds imports as CODE (PLT-style stubs /
    // IAT thunks) — a data symbol bound that way "links" and then reads
    // jump-stub BYTES as the object's value at run time, the exact
    // silent-miscompile class the fail-loud rule exists for. Schema-
    // declared, not format-name-enumerated (the supportedDataSections
    // discipline): a format gains data imports by declaring the field +
    // landing its walker arm — zero gate changes.
    //
    // D-LK-OBJECT-DATA-EXTERN-RELOCATABLE (c144): the "no model → reject"
    // rule is IMAGE-scoped. A relocatable object does NOT bind imports — it
    // has no import walker; a surviving data extern is emitted as an
    // SHN_UNDEF symbol (the reloc-driven symtab loop in elf.cpp) and the
    // FINAL (foreign) linker resolves it — for an executable link via a
    // copy-relocation, exactly what gcc does for `extern FILE *stdout` in a
    // `.o` (empirically R_X86_64_PC32 + a NOTYPE UND symbol, even under
    // default-PIE). The data-bound-as-code miscompile this reject guards
    // against lives in the IMAGE import walker; the relocatable writer's own
    // guard is that a data-extern reference emits PC32, never the PLT32 call
    // variant (elf.cpp restricts pltNativeId to FUNCTION externs). So only an
    // IMAGE (load-time-bound, no later linker to resolve the object) with no
    // declared binding rejects. Mirrors D-LK-OBJECT-NOLIB-EXTERN-RELOCATABLE
    // (c143), which kept referenced no-library FUNCTION externs as SHN_UNDEF.
    //
    // c150 (D-LK1-4): ELF ET_DYN is now image-flavored, so a `.so` with a
    // surviving data extern takes THIS gate — its schema declares
    // `dataImportBinding: "got-indirect"` (the c117/c149 GotIndirect model:
    // ld.so fills a GOT slot with the object's address; the lowering derefs
    // it), so it passes by config. Copy-relocation stays exec-only
    // (validate() rejects it on a dyn schema).
    //
    // This gate is format-AGNOSTIC (isImageFlavor() is false for every
    // relocatable flavor), so it lifts the reject for the Mach-O `object` and
    // PE `Obj` relocatable formats too, not only ELF ET_REL. That is correct:
    // those relocatable writers also emit a surviving extern as a faithful
    // undefined symbol (macho.cpp `N_UNDF|N_EXT`, SectionNumber 0; pe.cpp
    // `IMAGE_SYM_UNDEFINED`) with the producer-chosen relocation — none does
    // isData-dependent code-binding (the import-thunk/stub machinery is
    // exec-only). A relocatable writer that DID bind data as code would have
    // to reject or fix it in ITS OWN walker (as elf.cpp does via PC32); this
    // central gate only protects the IMAGE walkers.
    if (objectFormatSchema.isImageFlavor()
        && !objectFormatSchema.dataImportBinding().has_value()) {
        for (auto const& ext : inputModule.externImports) {
            if (!ext.isData) continue;
            report(reporter, DiagnosticCode::K_FormatLacksImportSupport,
                   DiagnosticSeverity::Error,
                   std::string{"linker: extern DATA import '"} + ext.mangledName
                       + "' (from '" + ext.libraryPath
                       + "') cannot be bound — format '"
                       + std::string{objectFormatSchema.name()}
                       + "' declares no 'dataImportBinding' model "
                         "(D-LK-EXTERN-DATA-IMPORT). Library data objects "
                         "bind via the format's declared mechanism (ELF exec: "
                         "\"copy-relocation\"); binding one through the "
                         "function-import machinery would read jump-stub "
                         "bytes as the object's value. A cross-TU extern "
                         "object resolves by compiling it WITH its defining "
                         "translation unit; a true library data object "
                         "(e.g. libc stdout) needs the format's binding "
                         "model to land.");
            image.resolvedFuncCount = 0;
            return image;
        }
    }

    // D-CSUBSET-THREAD-LOCAL-INITIAL-EXEC (TLS C1): a SURVIVING thread-
    // local extern import — an `extern thread_local` whose definition no
    // sibling CU supplied, i.e. a TRUE LIBRARY thread-local (glibc
    // `errno`-class objects). NO shipped binding model can carry it:
    //   * copy-relocation copies into ONE process-shared exec slot — the
    //     antithesis of thread storage (every thread would alias one copy);
    //   * the local-exec model this arc ships computes LINK-TIME tpoffs
    //     against the exec's OWN PT_TLS block — a library's TLS block has
    //     a loader-assigned module offset, unknowable at link time.
    // A library thread-local needs the INITIAL-EXEC model (a GOT slot the
    // loader fills with the tpoff — R_*_TPOFF64-class dynamic relocs),
    // deferred until a consumer exists. An INTRA-program `extern
    // thread_local` never reaches this gate: the LK11 cross-CU merge
    // strips the import row when a sibling CU defines it. Unconditional
    // (before any per-format walker) — this is storage-model capability,
    // not format capability, so it lives at the agnostic tier.
    for (auto const& ext : inputModule.externImports) {
        if (!ext.isThreadLocal) continue;
        report(reporter, DiagnosticCode::K_FormatLacksThreadLocalSupport,
               DiagnosticSeverity::Error,
               std::string{"linker: extern THREAD-LOCAL import '"}
                   + ext.mangledName + "' (from '" + ext.libraryPath
                   + "') cannot be bound — a library thread-local needs "
                     "the initial-exec TLS model (a loader-filled tpoff "
                     "GOT slot), which is not implemented "
                     "(D-CSUBSET-THREAD-LOCAL-INITIAL-EXEC). The shipped "
                     "local-exec model covers only THIS executable's own "
                     "thread-locals; copy-relocation cannot apply (one "
                     "process-shared copy slot would defeat the declared "
                     "thread storage duration). An intra-program `extern "
                     "thread_local` resolves by compiling it with its "
                     "defining translation unit.");
        image.resolvedFuncCount = 0;
        return image;
    }

    // D-LK10-ENTRY Slice C (plan 14 §2.13): when the format declares
    // a `processExit` block + `entryCallingConvention` (validated by
    // Slice B's cross-field rule), synthesize a `_start` trampoline
    // and prepend it as `functions[0]`. The walker's
    // `imageEntryOverride` path then uses the trampoline as the
    // image entry, while the schema's `entryPoint` continues to name
    // the user fn (the trampoline's call target).
    //
    // Without this hook, the emitted executables point e_entry /
    // AddressOfEntryPoint / LC_MAIN.entryoff directly at the user
    // fn, which SEGVs on its `ret` epilogue (process entry has no
    // return address). See §2.13 for the full design.
    //
    // Bypass conditions: caller-provided `imageEntryOverride` (a
    // pre-injected trampoline; do not re-inject) OR empty functions
    // (no module to wrap).
    //
    // c150 (D-LK1-4): the condition is SCHEMA-driven by design — an
    // ELF ET_DYN `.so` declares NO `processExit` (validate() rejects
    // it there: entry machinery is exec-flavor-only), so no
    // trampoline is synthesized for a shared library (a `.so` has no
    // entry; e_entry = 0). c151 (the D-LK1-4 PIE half, landed):
    // the ELF ET_DYN PIE schema declares `processExit` (one of its
    // entry-cluster members) and gets the trampoline through this
    // same condition — zero gate changes, exactly as designed; its
    // e_entry is the trampoline's BASE-RELATIVE VA (ld.so adds the
    // load base).
    AssembledModule moduleCopy;
    AssembledModule const* moduleP = &inputModule;
    bool const wantTrampoline =
        objectFormatSchema.processExit().has_value()
     && !inputModule.functions.empty()
     && !inputModule.imageEntryOverride.has_value();
    if (wantTrampoline) {
        moduleCopy = inputModule;
        if (!injectEntryTrampoline(moduleCopy, targetSchema,
                                    objectFormatSchema, reporter)) {
            // Diagnostic(s) emitted by the trampoline emitter.
            image.resolvedFuncCount = 0;
            return image;
        }
        moduleP = &moduleCopy;
        image.expectedFuncCount = moduleCopy.expectedFuncCount;
    }
    AssembledModule const& module = *moduleP;

    // Snapshot error count so we can detect whether the cross-
    // reference unifier pass (below) added any new K_* diagnostics
    // — if so, the module is not valid input for the format walker
    // and we must skip dispatch. Separation of concerns mirrors
    // ML6 cycle 3a's `LirAllocation::ok()` discipline: linkage
    // VALIDITY (kind + symbol cross-checks) is one stage; format
    // EMISSION (byte serialization) is another; the second runs
    // only when the first passed.
    std::size_t const errorsAtEntry = reporter.errorCount();

    // D-LK4-3: index every symbol this CU declares — functions, rodata data items,
    // and FFI extern imports — into the collision-proof compound-key map (keyed by
    // (cuId, SymbolId)). `buildCompoundIndex` fails loud on a duplicate compound key
    // (the same SymbolId declared twice in this CU, across any kind) and on empty
    // extern mangledName; an empty libraryPath is the c86 undefined-symbol
    // surface, handled by `rejectOrDropUnreferencedExterns` at the head of this
    // emission section (referenced ⇒ rejected loud; unreferenced ⇒ dropped).
    // Single-CU resolution here; cross-CU symbol-table merge is LK11. Externs
    // resolve against the per-format walker's import-table emission (LK6
    // cycle 2 — PE IAT / ELF GOT+PLT / Mach-O chained-fixups).
    std::unordered_map<LinkedSymbolKey, SymbolKind> symbolIndex;
    buildCompoundIndex(symbolIndex, module, reporter);
    image.symbolCount = symbolIndex.size();

    for (auto const& fn : module.functions) {
        bool funcResolved = true;
        for (auto const& reloc : fn.relocations) {
            // Cross-reference unifier (plan 13 §2.6): the SAME opaque
            // `kind` tag must resolve on BOTH the target side (so the
            // formula is known) AND the format side (so the platform-
            // native name is known). The diagnostic names BOTH sides
            // when both miss — masking either half hides half the
            // misconfiguration from the user and forces a second
            // round-trip after they fix the first error.
            auto const* targetReloc =
                targetSchema.relocationInfo(reloc.kind);
            auto const* formatReloc =
                objectFormatSchema.relocationByKind(reloc.kind);
            bool const kindResolved =
                targetReloc != nullptr && formatReloc != nullptr;
            if (!kindResolved) {
                std::string msg = "relocation kind ";
                msg += std::to_string(reloc.kind.v);
                msg += " in symbol #";
                msg += std::to_string(fn.symbol.v);
                msg += " is not declared by ";
                bool needAnd = false;
                if (targetReloc == nullptr) {
                    msg += "target schema '";
                    msg += std::string{targetSchema.name()};
                    msg += "'";
                    needAnd = true;
                }
                if (formatReloc == nullptr) {
                    if (needAnd) msg += " and ";
                    msg += "object format '";
                    msg += std::string{objectFormatSchema.name()};
                    msg += "'";
                }
                report(reporter,
                       DiagnosticCode::K_RelocationKindMismatch,
                       DiagnosticSeverity::Error,
                       std::move(msg));
                funcResolved = false;
            }
            // Symbol-undefined runs INDEPENDENTLY of kind resolution
            // — a misconfigured reloc whose kind is wrong AND whose
            // target is undefined should surface BOTH diagnostics
            // in one pass, not require two link attempts.
            //
            // A reloc target resolves if ANY of:
            //   (a) it names a defined function in this module, OR
            //   (b) it names an extern import (LK6 cycle 2 —
            //       resolved at link time via the per-format walker's
            //       import-table emission), OR
            //   (c) it names a module-level data item — rodata global,
            //       string literal global, etc. (D-LK4-RODATA-PRODUCER
            //       2026-06-02 — the per-format walker merges these
            //       into its symbolVa map at section-relative offsets), OR
            //   (d) it is a WRITER-RESERVED singleton id (D-CSUBSET-THREAD-
            //       LOCAL TLS C3 — the PE `_tls_index` slot the format writer
            //       mints + binds; the writer defines it into symbolVa or
            //       fails loud, exactly like an extern import in (b)).
            // Anything else is a hard undefined.
            if (!symbolIndex.contains(LinkedSymbolKey{module.cuId, reloc.target})
                && !isWriterReservedSymbolIdValue(reloc.target.v)) {
                std::string msg = "relocation in symbol #";
                msg += std::to_string(fn.symbol.v);
                msg += " references undefined symbol #";
                msg += std::to_string(reloc.target.v);
                msg += " (not declared by any AssembledFunction, "
                       "ExternImport, nor AssembledData item)";
                report(reporter,
                       DiagnosticCode::K_SymbolUndefined,
                       DiagnosticSeverity::Error,
                       std::move(msg));
                funcResolved = false;
            }
        }
        if (funcResolved) ++image.resolvedFuncCount;
    }

    // D-LINK-DATA-RELOC-TARGET-VALIDATE: DATA-item relocations get the SAME
    // undefined-target check as the function relocations above. A symbol-address
    // global / function-pointer table / c70 switch jump table carries abs64
    // relocations in its DATA bytes whose target must be a declared function / data
    // item / synthetic block symbol / extern import (all indexed in `symbolIndex` by
    // buildCompoundIndex) or a writer-reserved singleton. Without this, a
    // mis-remapped or dangling data-reloc target surfaced ONLY as a silent wrong /
    // NULL pointer at runtime — the class the aSyscall Local/Global merge miscompile
    // (D-LINK-LOCAL-FN-ADDR-STATIC-DATA-VA0) belonged to — instead of a LOUD
    // K_SymbolUndefined at link. This is the N==1 twin of the per-CU data check the
    // multi-module `resolveCrossCuSymbols` now runs (and the merged whole-program
    // image flows through THIS single-CU path).
    for (auto const& di : module.dataItems) {
        for (auto const& reloc : di.relocations) {
            if (!symbolIndex.contains(LinkedSymbolKey{module.cuId, reloc.target})
                && !isWriterReservedSymbolIdValue(reloc.target.v)) {
                std::string msg = "data-item relocation in symbol #";
                msg += std::to_string(di.symbol.v);
                msg += " references undefined symbol #";
                msg += std::to_string(reloc.target.v);
                msg += " (not declared by any AssembledFunction, "
                       "ExternImport, nor AssembledData item)";
                report(reporter,
                       DiagnosticCode::K_SymbolUndefined,
                       DiagnosticSeverity::Error,
                       std::move(msg));
            }
        }
    }

    // Skip walker dispatch if the cross-reference unifier failed.
    // A module that produces K_SymbolUndefined / K_RelocationKindMismatch
    // is not valid input for the format walker — emitting partial
    // bytes from a known-invalid module is exactly the silent-failure
    // class the substrate discipline rejects.
    //
    // Resetting `resolvedFuncCount` is load-bearing (architect
    // convergence): without it, a 3-function module where ONLY fn#1
    // had a reloc failure would report `resolvedFuncCount = 2`,
    // `expectedFuncCount = 3` (ok() = false — correct) — but a
    // 1-function module whose ONLY function had a reloc failure
    // would still report `resolvedFuncCount = 0` (ok() = false, also
    // correct). The subtle bug: a 2-function module where fn#0 had
    // no relocs and fn#1's relocs all failed: pre-fix produced
    // resolvedFuncCount=1, expectedFuncCount=2 (ok()=false, correct
    // by accident). But if EVERY function's relocs failed,
    // resolvedFuncCount remained partial. Reset on linkage failure
    // makes "linkage failed → no functions are resolved" structural.
    if (reporter.errorCount() != errorsAtEntry) {
        image.resolvedFuncCount = 0;
        return image;
    }

    // D-LK4-RODATA-SUBSTRATE precondition guard: until the per-
    // format walker arms (D-LK2-RODATA / D-LK1-RODATA / D-LK3-
    // RODATA) close, no walker consumes `module.dataItems`. A
    // future producer (HIR string-literal promotion → MIR global →
    // assembler) that lands `dataItems` BEFORE the matching walker
    // arm would silently emit a binary with NO `.rdata`/`.rodata`/
    // `__cstring` section — every string literal silently drops on
    // the floor; downstream `printf("hello")` reads garbage from
    // an unrelated VA. Fail-loud HERE so the producer cycle is
    // sequenced after at least one walker arm.
    //
    // **D-LK4-RODATA-BSS-INVARIANT + duplicate-SymbolId + zero-
    // alignment guards**: validate the substrate invariants on
    // `dataItems` BEFORE the per-format capability gate. The
    // validate() function fail-louds on each violation so a
    // future producer landing malformed items hits a precise
    // diagnostic naming the violation, not a generic "no walker
    // yet" rejection. Order matters: invariant failures point
    // at the producer; the capability gate points at the missing
    // walker. (INVARIANT — multi-agent audit fold pin: this
    // ordering is load-bearing; swapping the two blocks would
    // surface a missing-walker rejection on an already-malformed
    // dataItems payload and obscure the real producer defect.)
    if (!module.dataItems.empty()) {
        bool const dataItemsValid =
            validateAssembledData(module.dataItems, reporter);
        if (!dataItemsValid) {
            image.resolvedFuncCount = 0;
            return image;
        }
    }

    // ── Per-format walker capability gate for `dataItems` ──────
    //
    // **Schema-declared, not enumerated in C++** (D-LK2-RODATA
    // audit fold — agnosticism rule). Each format's JSON ships a
    // `supportedDataSections: ["rodata", ...]` array advertising
    // which `DataSectionKind` values its walker accepts. The gate
    // below consults that set per-item — formats whose walker arm
    // has not landed (ELF + Mach-O until D-LK1-RODATA / D-LK3-
    // RODATA close), AND format flavors that cannot carry rodata
    // (PE Obj — relocatable .obj emits rodata via the symbol
    // table, not via the dataItems pipeline), reject loudly.
    //
    // Adding a fourth executable format that supports rodata =
    // drop `"supportedDataSections": ["rodata"]` into its JSON;
    // zero source changes in this gate. **NO `kind == Pe` style
    // format-name branches here** — the standing veto on source/
    // target/format-name branching in shared substrate is held.
    for (auto const& d : module.dataItems) {
        if (!objectFormatSchema.acceptsDataSection(d.section)) {
            report(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                   DiagnosticSeverity::Error,
                   std::format(
                       "linker: module carries an AssembledData "
                       "item with section={} but format '{}' "
                       "(kind={}) does not advertise that section "
                       "in its 'supportedDataSections' set. Either "
                       "the format's walker arm has not yet closed "
                       "(see plan §3.1: D-LK1-RODATA / D-LK2-RODATA "
                       "/ D-LK3-RODATA / D-LK4-RODATA-PRODUCER) or "
                       "the format flavor cannot carry that "
                       "section (e.g. relocatable .obj).",
                       dataSectionKindName(d.section),
                       objectFormatSchema.name(),
                       objectFormatKindName(
                           objectFormatSchema.kind())));
            image.resolvedFuncCount = 0;
            return image;
        }
    }

    // Format-keyed dispatch — closed-enum switch, fail-loud on
    // any format whose walker hasn't landed yet so the substrate
    // discipline reports the missing walker instead of silently
    // returning empty bytes.
    switch (objectFormatSchema.kind()) {
    case ObjectFormatKind::Unknown:
        report(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
               DiagnosticSeverity::Error,
               "linker: format kind 'unknown' is the invalid sentinel; "
               "the object format schema was constructed without a valid "
               "format declaration");
        break;
    case ObjectFormatKind::Elf:
        image.bytes = elf::encode(module, targetSchema,
                                  objectFormatSchema, reporter);
        break;
    case ObjectFormatKind::Pe:
        // `request` is threaded ONLY to the walkers that implement a
        // declared vehicle for one of its fields (today: PE, for
        // `stackReserveControl.vehicle = pe-optional-header`). The other
        // walkers take no request BY CONSTRUCTION: their formats declare no
        // capability, so the gate above has already refused any request that
        // would reach them. A new vehicle lands WITH its walker's parameter,
        // never before — so "declared but silently dropped" is unreachable.
        image.bytes = pe::encode(module, targetSchema,
                                 objectFormatSchema, reporter, request);
        break;
    case ObjectFormatKind::MachO:
        image.bytes = macho::encode(module, targetSchema,
                                     objectFormatSchema, reporter);
        break;
    case ObjectFormatKind::Wasm:
        // LK8 skeleton: emits the 8-byte module preamble (magic +
        // version). Plan 18 fills the section emitters; this
        // walker is the substrate plumb-through that proves the
        // format-blind dispatch routes WASM correctly.
        image.bytes = wasm::encode(module, targetSchema,
                                   objectFormatSchema, reporter);
        break;
    case ObjectFormatKind::Spirv:
        // LK9 skeleton: emits the 5-word SPIR-V module header
        // (magic + version + generator + bound + reserved) per
        // SPIR-V Spec §2.3. Plan 17 fills the instruction stream;
        // this walker is the substrate plumb-through that proves
        // the format-blind dispatch routes SPIR-V correctly.
        image.bytes = spirv::encode(module, targetSchema,
                                    objectFormatSchema, reporter);
        break;
    }

    // Post-walker error gate (architect O1 fold from LK6 cycle 2a):
    // a walker-side fail-loud (e.g. K_FormatLacksImportSupport on
    // ELF/MachO when externImports is non-empty) registers errors
    // on the reporter but leaves `resolvedFuncCount` at the value
    // the pre-walker unifier set. Without this gate, `image.ok()`
    // returns true while `image.bytes` is empty — silent contract
    // violation. Same shape as the pre-walker gate above.
    if (reporter.errorCount() != errorsAtEntry) {
        image.resolvedFuncCount = 0;
    } else {
        // Clean conclusion: the walker produced bytes with no new diagnostic.
        // This is the ONLY point that marks the link successful — every failure
        // early-return above returns before here, leaving `linkedCleanly`
        // false. It distinguishes a genuinely EMPTY module (a valid 0-function
        // success — D-CSUBSET-TESTTU-SILENT-EXIT1) from a failure that also
        // ended with `resolvedFuncCount == expectedFuncCount == 0` (e.g. the
        // undefined-extern early-return at :610-611).
        image.linkedCleanly = true;
    }

    return image;
}

// D-LK4-3 single-module convenience overload — delegates to the span entry with a
// 1-element span. Keeps every existing single-CU caller source-unchanged.
LinkedImage link(AssembledModule const&    module,
                 TargetSchema const&       targetSchema,
                 ObjectFormatSchema const& objectFormatSchema,
                 DiagnosticReporter&       reporter,
                 ImageRequest const&       request) {
    return link(std::span<AssembledModule const>{&module, 1},
                targetSchema, objectFormatSchema, reporter, request);
}

} // namespace dss::linker
