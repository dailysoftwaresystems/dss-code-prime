#pragma once

#include "asm/asm.hpp"                // AssembledModule
#include "core/types/strong_ids.hpp"  // SymbolId

#include <cstdint>

// The ONE answer to "which SymbolIds are already taken in this module", for
// every link-tier pass that has to MINT one.
//
// Two such passes exist today and they are in different files:
//   * `injectEntryTrampoline` (entry_trampoline.cpp) mints the synthetic
//     `_start` body and, on the ByNameImport exit path, a synthetic extern;
//   * `materializeObjectImportSlots` (linker.cpp) mints one carried
//     import slot per referenced extern import — DATA
//     (D-LK-PE-OBJECT-WEAK-DATA-EXTERN-REL32-TO-AN-ABSOLUTE-TARGET) and,
//     under an `indirect-slot` dispatch, FUNCTION too
//     (D-LK-PE-OBJECT-WEAK-FUNCTION-ADDR-REL32-TO-AN-ABSOLUTE-TARGET).
//
// ★ IT IS HOISTED RATHER THAN COPIED, AND THE COST OF THE COPY IS ALREADY
// RECORDED IN THIS FUNCTION'S OWN HISTORY. The scan has been WIDENED twice
// after a collision reached a walker: once for `dataItems` (a string-literal
// promoted MirGlobal shared an id with the trampoline's extern, surfacing as
// `K_DuplicateDataSymbol` in the PE walker), and once for `blockSymbols` (a
// computed-goto `&&label` target, surfacing as a compound-index redeclaration).
// Both times the fix was one line HERE. A second copy would have kept the old,
// narrower rule and failed the same way in a different pass, with the
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
    }
    for (auto const& d : mod.dataItems) {
        if (d.symbol.v > maxV) maxV = d.symbol.v;
    }
    for (auto const& fn : mod.functions) {
        for (auto const& bs : fn.blockSymbols) {
            if (bs.symbol.v > maxV) maxV = bs.symbol.v;
        }
    }
    return maxV;
}

}  // namespace dss::linker
