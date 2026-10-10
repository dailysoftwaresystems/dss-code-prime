#pragma once

#include "asm/asm.hpp"                // AssembledModule
#include "core/types/strong_ids.hpp"  // SymbolId

#include <cstdint>

// The ONE answer to "which SymbolIds are already taken in this module", for
// every link-tier pass that has to MINT one.
//
// Three such passes exist today and they are in different files:
//   * `injectEntryTrampoline` (entry_trampoline.cpp) mints the synthetic
//     `_start` body and, on the ByNameImport exit path, a synthetic extern;
//   * `materializeObjectImportSlots` (linker.cpp) mints one carried
//     import slot per referenced extern import — DATA
//     (D-LK-PE-OBJECT-WEAK-DATA-EXTERN-REL32-TO-AN-ABSOLUTE-TARGET) and,
//     under an `indirect-slot` dispatch, FUNCTION too
//     (D-LK-PE-OBJECT-WEAK-FUNCTION-ADDR-REL32-TO-AN-ABSOLUTE-TARGET);
//   * `lowerGotSlotReferences` (linker.cpp, P68 round 11) mints one GOT slot
//     per (symbol, addend) an image's GOT-slot-relative references name.
//
// ★ IT IS HOISTED RATHER THAN COPIED, AND THE COST OF THE COPY IS ALREADY
// RECORDED IN THIS FUNCTION'S OWN HISTORY. The scan has been WIDENED six
// times, each after a collision reached a walker or a link that should have
// been refused, or was found by reading: first for `dataItems` (a
// string-literal promoted MirGlobal shared an id with the trampoline's extern,
// surfacing as `K_DuplicateDataSymbol` in the PE walker), then for
// `blockSymbols` (a computed-goto `&&label` target, surfacing as a
// compound-index redeclaration); the four after those are told where they
// stand below.
// Every time the fix was a few lines HERE. A second copy would have kept the
// old, narrower rule and failed the same way in a different pass, with the
// diagnostic again pointing at a walker rather than at the mint.
//
// CALLER CONTRACT: mint SEQUENTIALLY from the returned value — `maxV+1`,
// `maxV+2`, ... Re-calling this between mints WITHOUT having mutated the module
// returns the SAME number and silently hands out one id twice; that is the bug
// caught at the trampoline's Slice C build, and it is a property of the caller,
// not of this scan.
namespace dss::linker {

[[nodiscard]] inline std::uint32_t
maxExistingSymbolIdV(AssembledModule const& mod) noexcept {
    std::uint32_t maxV = 0;
    for (auto const& fn : mod.functions) {
        if (fn.symbol.v > maxV) maxV = fn.symbol.v;
    }
    for (auto const& ext : mod.externImports) {
        if (ext.symbol.v > maxV) maxV = ext.symbol.v;
        // ★ AND ITS ADDRESS-SLOT SYMBOL — the THIRD widening (P68 round 11).
        // A preemption reference in a `.so` names the import's address through
        // a second id of its own (D-MIR-DYLIB-SELF-CALL-BYPASSES-WEAK-COALESCING),
        // and it lives only here. ✔MEASURED 2026-09-24: the GOT-slot lowering —
        // the first minting pass that runs on a `.so` — minted id #11 for an
        // aarch64 shared object whose address slot was already #11, and the
        // link refused it as "declared more than once".
        if (ext.addressSlotSymbol.v > maxV) maxV = ext.addressSlotSymbol.v;
        // And its call entry taken as an address (P69 review M1 (c)), for the
        // identical reason: an id that lives only on the row.
        if (ext.callEntrySymbol.v > maxV) maxV = ext.callEntrySymbol.v;
    }
    for (auto const& d : mod.dataItems) {
        if (d.symbol.v > maxV) maxV = d.symbol.v;
    }
    for (auto const& fn : mod.functions) {
        for (auto const& bs : fn.blockSymbols) {
            if (bs.symbol.v > maxV) maxV = bs.symbol.v;
        }
    }
    // ★ AND EVERY SYMBOL WHOSE ADDRESS IS 0 — the FOURTH widening (P69,
    // D-LK-WEAK-UNDEFINED-SYMBOL-NAMED-DIRECTLY-IS-NOT-ADDRESS-ZERO): the
    // reference gate mints one per weak symbol it resolves to nothing, before
    // the GOT-slot lowering and the entry trampoline mint theirs, and it lives
    // only in this list.
    for (SymbolId const s : mod.nullAddressSymbols) {
        if (s.v > maxV) maxV = s.v;
    }
    // ★ AND THE TWO IDS AN EXCEPTION SCOPE NAMES — the FIFTH widening (P69,
    // D-LK-MERGE-LEFT-SEH-SCOPE-IDS-AND-UNIT-ENTRY-UNRENUMBERED). A scope's
    // filter funclet and personality are references that no relocation carries,
    // so the gate that refuses an undefined relocation target never reads them:
    // the format's writer is the first to ask what they name. In a well-formed
    // module they name a function and an import, which the loops above already
    // counted. In one where a scope names NOTHING, the id lives only in the
    // scope — and a pass that minted "the next free id" before the writer ran
    // handed that very number to its own new symbol. ✔MEASURED 2026-10-08: a
    // guarded unit whose personality row is gone LINKED, its handler field
    // naming the import the entry trampoline had just minted for the process
    // exit; with the scope's ids counted here the mint goes past them, the id
    // still names nothing, and the writer refuses it by name.
    for (auto const& fn : mod.functions) {
        for (auto const& scope : fn.sehScopes) {
            if (scope.filterFuncletSymbol.v > maxV) maxV = scope.filterFuncletSymbol.v;
            if (scope.personalitySymbol.v > maxV) maxV = scope.personalitySymbol.v;
        }
    }
    // ★ AND EVERY SYMBOL ROW — the SIXTH widening (P69 fold 2). A unit an object
    // reader produced numbers its ids by RECORD
    // (`link/format/record_symbol_ids.hpp`), and a record that is neither a body
    // nor an import — a section's own symbol, a label inside a body — is still a
    // ROW of `symbols` under its record's id. No loop above reads a row, so "the
    // next id past the bodies and the imports" could be the id such a row holds.
    // ✔MEASURED 2026-10-08 (the review of fold 1, on the fixture
    // `tests/link/data/pe_directive_entry_reference_x86_64_pe.obj`): the id
    // `requireEntryReference` minted for the entry's reference EQUALLED the Local
    // row of the object's `.chks64` section symbol — one id, two symbols. A PE
    // image writes no symbol table, so nothing read the pair that day; a
    // relocatable artifact writes a record per row. The passes that mint on a
    // reader-produced unit AFTER the read (`requireEntryReference`,
    // `handTheNameToTheCommon`) ask this scan, so the reader's one counter is
    // continued here and never restarted beside it.
    for (auto const& row : mod.symbols) {
        if (row.symbol.v > maxV) maxV = row.symbol.v;
    }
    return maxV;
}

}  // namespace dss::linker
