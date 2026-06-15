// MANDATORY post-lowering CFG-reachability prune.
// Anchor: D-MIR-UNREACHABLE-PRUNE-NORMALIZE.
//
// Drops every block unreachable-from-entry, funneling all eager-
// continuation constructs (if / if-else / && / || / while / do-while /
// for / switch / dead-code-after-return / future goto) through ONE
// chokepoint that REUSES the shared MirFunctionRebuilder substrate.
// See prune_unreachable.hpp for the design rationale.

#include "opt/passes/prune_unreachable.hpp"

#include "mir/mir_cfg.hpp"           // mirReversePostOrder
#include "mir/mir_node.hpp"
#include "mir/mir_struct_markers.hpp" // rederiveStructCfMarkers
#include "opt/passes/mir_rebuild_helper.hpp"

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dss::opt::passes {

namespace {

// Policy: keep ONLY the RPO-reachable-from-entry blocks (drop the rest)
// and drop phi incomings whose pred didn't survive. Mirrors DCE's two
// hooks EXACTLY — but does NO liveness / instruction elimination. Every
// surviving block keeps every instruction verbatim; the prune's sole
// transform is dropping unreachable BLOCKS.
class PruneUnreachablePolicy final : public MirRebuildPolicy {
public:
    // Phase 1: the surviving block set = blocks forward-reachable from
    // the function entry, in RPO. IDENTICAL call to the one DCE uses
    // (mir/mir_cfg.hpp). The entry block is always element 0 of this
    // list (it is the reachability root), so it is never pruned.
    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        return mirReversePostOrder(src, src.funcEntry(fn));
    }

    // Phase 3: drop a phi incoming whose predecessor block did not
    // survive the prune. Mirrors DCE — a dead pred (e.g. a future
    // goto's dead block branching to a reachable label) would otherwise
    // hit the rebuilder phase-3 onZeroPhiIncomings / "pred not in
    // blockMap" abort. A reachable join keeps every incoming from a
    // surviving pred (>= 1, by construction of reachability).
    [[nodiscard]] bool acceptPhiIncoming(
        MirPhiIncoming const& inc, MirBlockId /*oldPhiBlock*/,
        std::unordered_map<std::uint32_t, MirBlockId> const& blockMap) override {
        return blockMap.count(inc.pred.v) != 0;
    }
};

} // namespace

PruneUnreachableResult
runPruneUnreachableBlocks(Mir& mir, TypeInterner const& /*interner*/,
                          DiagnosticReporter& /*reporter*/) {
    PruneUnreachableResult result{};
    MirBuilder builder;

    // Tally pre-vs-post block counts honestly for the result/log. The
    // prune drops only blocks, never functions, so a simple per-function
    // sum is exact.
    std::size_t blocksBefore = 0;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        blocksBefore += mir.funcBlockCount(mir.funcAt(i));
    }

    // Rebuild every function FIRST — added in source order, so each new
    // func ordinal equals the old one (the prune drops only blocks within
    // a function; it never drops or reorders functions). This MUST precede
    // the globals clone: cloneGlobalsVerbatim re-stamps each runtime-init
    // global's initFunc to the new module's tag at the SAME ordinal, which
    // requires the function slots to already exist in `builder`.
    PruneUnreachablePolicy policy{};
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        MirFunctionRebuilder rb{mir, builder, policy};
        rb.rebuildFunction(f);
    }

    // Verbatim globals clone — NEVER carves out (the prune is mandatory
    // for verifier-correctness on EVERY module, including runtime-init
    // ones) and keeps each global's initFunc (same ordinal, re-tagged to
    // the new module). See cloneGlobalsVerbatim's contract.
    cloneGlobalsVerbatim(mir, builder);

    mir = std::move(builder).finish();

    std::size_t blocksAfter = 0;
    std::size_t const nfAfter = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nfAfter; ++i) {
        blocksAfter += mir.funcBlockCount(mir.funcAt(i));
    }
    result.blocksPruned =
        blocksBefore > blocksAfter ? blocksBefore - blocksAfter : 0;

    // The CFG changed (blocks dropped) → re-derive every surviving
    // block's canonical StructCfMarker from the NEW shape, exactly like
    // SimplifyCfg / the inliner / merge. The verifier RECOMPUTES the
    // derivation and requires stored == derived, so a stale marker on a
    // block whose role changed (e.g. an if-join that lost an arm) would
    // otherwise trip I_StructCfMismatch.
    rederiveStructCfMarkers(mir);
    result.ok = true;
    return result;
}

} // namespace dss::opt::passes
