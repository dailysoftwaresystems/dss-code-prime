#pragma once

// MANDATORY post-lowering CFG-reachability prune (D-MIR-UNREACHABLE-PRUNE-NORMALIZE).
//
// **Why this exists**: hir_to_mir creates continuation blocks EAGERLY —
// if-join, loop exit/latch, &&/|| join, switch exit, dead-code-after-return.
// When every arm of a construct seals (return / break), that continuation
// ends up with ZERO predecessors: it is unreachable from the function
// entry. The production MirVerifier rejects ANY block not reachable from
// entry (I_UnreachableBlock). This pass drops every such block, funneling
// ALL constructs (if / if-else / && / || / while / do-while / for / switch
// / dead-code-after-return / future goto) through ONE chokepoint.
//
// **What it is NOT**: it is NOT a PassId in the optimizer pipeline. A
// pipeline config must not be able to omit a verifier-correctness
// normalize. It is wired UNCONDITIONALLY in `optimizeModule`
// (program/compile_pipeline.cpp) — the universal chokepoint that runs on
// every CU and on the merged module, BEFORE opt::optimize's
// verify-after-every-pass ever sees the module. It is idempotent.
//
// **Mechanism**: ONE CFG-reachability prune that REUSES the shared
// MirFunctionRebuilder / MirRebuildPolicy substrate (mir_rebuild_helper).
// No new compaction / remap code — the exact behavior already exists as
// DCE's policy shape (selectBlocks => RPO-reachable, acceptPhiIncoming =>
// pred-in-blockMap). Unlike DCE this prune drops BLOCKS ONLY — no
// liveness / instruction elimination, no whole-function elision, no
// global elision (so func-ids stay stable; see `cloneGlobalsVerbatim`).
//
// **Agnostic**: pure CFG reachability — zero language / CPU / object-format
// identity. Source/target/linker agnostic by construction.

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"

#include <cstddef>

namespace dss::opt::passes {

struct PruneUnreachableResult {
    bool        ok           = false;
    std::size_t blocksPruned = 0;
};

// Drop every block unreachable-from-entry in every function of `mir`,
// in place. `interner` is accepted for signature parity with the other
// MIR-tier passes (this prune needs no type decoding). Always returns
// ok=true on success; fails loud (via the rebuilder substrate) on a
// structural-contract violation. Re-derives StructCfMarkers after the
// rebuild — the CFG changed.
[[nodiscard]] DSS_EXPORT PruneUnreachableResult
runPruneUnreachableBlocks(Mir& mir, TypeInterner const& interner,
                          DiagnosticReporter& reporter);

} // namespace dss::opt::passes
