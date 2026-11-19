#pragma once

// ★★★ THE GUARDED RUNS LIVENESS READS, AND WHERE TWO RULES ABOUT A BLOCK'S IDENTITY MEET
// (D-LIR-NO-EXCEPTIONAL-EDGE-INTO-A-TRY-HANDLER).
//
// MIR→LIR names a `__try` scope's blocks by ITS OWN ids (`SehScopeDescriptor`). Liveness and the allocator
// run on a LATER module — the one `lowerWideCallArgs` rebuilt — and a block id is a position in ONE
// module's arena. So the ids are FOLLOWED to the module liveness runs on, through the block image every
// rebuild in between published, by the one owner of that question (`translateDescriptorBlockIds`,
// D-LIR-DESCRIPTOR-BLOCK-IDS-SHIFTED-BY-A-BLOCK-INSERTING-PASS): a scope's first block and its handler go
// to the FIRST piece of what they became, its last block to the LAST piece.
//
// That is the reading the scope TABLE gets at the binding, over a PREFIX of the same chain of rebuilds.
// So the run liveness makes the handler block an exceptional successor of is the run the table's two
// addresses will enclose, cut at the module liveness runs on — and a rebuild placed between MIR→LIR and
// liveness that moves, splits or inserts a block moves the run with it instead of leaving liveness with
// the ids of another module. Nothing here assumes a rebuild is the identity; today's one (the wide-call
// materialization) publishes the identity it performed, and a rebuild that publishes no image is refused
// by the owner, by name.
//
// One record is one run. A region whose blocks are laid out apart is SEVERAL descriptors with one handler
// block, and each becomes a region of its own here: liveness takes any set of runs.

#include "core/types/diagnostic_reporter.hpp"
#include "lir/lir_descriptor_blocks.hpp"   // LirBlockRebuild, translateDescriptorBlockIds
#include "lir/lir_liveness.hpp"            // LirGuardedRegion
#include "lir/lowering/mir_to_lir.hpp"     // SehScopeDescriptor

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace dss {

// The guarded runs of the module `stepsToLivenessModule.back().out` (MIR→LIR's own module when the chain
// is empty), one per descriptor and in the descriptors' order. `scopes` are MIR→LIR's and are not
// modified. nullopt = a block could not be followed (reported by the owner, naming the scope and the id).
[[nodiscard]] inline std::optional<std::vector<LirGuardedRegion>>
guardedRegionsAt(std::span<LirBlockRebuild const>    stepsToLivenessModule,
                 std::span<SehScopeDescriptor const> scopes,
                 DiagnosticReporter&                 reporter) {
    std::vector<SehScopeDescriptor> followed(scopes.begin(), scopes.end());
    if (!followed.empty()) {
        std::vector<JumpTableDescriptor>   noJumpTables;
        std::vector<LirBlockSymbolBinding> noLabelBindings;
        if (!translateDescriptorBlockIds(stepsToLivenessModule, noJumpTables, noLabelBindings,
                                         followed, reporter)) {
            return std::nullopt;
        }
    }
    std::vector<LirGuardedRegion> regions;
    regions.reserve(followed.size());
    for (SehScopeDescriptor const& s : followed) {
        LirGuardedRegion r;
        r.funcIndex     = static_cast<std::uint32_t>(s.funcIndex);
        r.firstBlockV   = s.beginLirBlockV;
        r.lastBlockV    = s.endLirBlockV;
        r.landingBlockV = s.handlerLirBlockV;
        regions.push_back(r);
    }
    return regions;
}

}  // namespace dss
