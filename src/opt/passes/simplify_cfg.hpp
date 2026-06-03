#pragma once

// MIR-tier SimplifyCFG: minimal-scope CFG simplification.
//
// **Scope (OPT5+ c2)**:
//   1. **Branch-folding**: `CondBr(Const(true|false), T, F)` collapses
//      to `Br(T|F)`. The folded-out arm becomes unreachable → the
//      subsequent DCE pass elides its blocks. This is the high-value
//      transform unlocked by the upstream ConstFold + CopyProp + CSE
//      cluster (a Phi-collapsed condition that resolves to a literal
//      enables this fold).
//   2. **Empty-block jump-threading**: a trampoline block B whose
//      ONLY instruction is a `Br(S)` terminator + S has no Phi nodes
//      (conservative — avoids Phi-incoming fan-out) is elided.
//      Predecessor terminators that branched to B are redirected to
//      S directly via the new `redirectBlockTarget` hook.
//
// **OUT-OF-SCOPE for c2** (anchored):
//   - General block-merge (B has 1 pred + P has B as only succ).
//     Phi-incoming fix-up requires the predecessor-fan-out algorithm
//     this cycle's empty-block-only scope deliberately avoids.
//   - StructCfMarker re-derivation. Branch-folding + empty-block
//     elision PRESERVE the surviving blocks' markers (we never
//     change a block's structural role; we only drop blocks). Marker
//     re-derivation activates when block-merge lands (D-OPT4-1).
//
// **Mechanism**: SimplifyCFG uses two new `MirRebuildPolicy` hooks:
//   - `tryRewriteTerminator(op, oldId, dst, rewrite, blockMap)`:
//     consulted before the standard terminator emit. SimplifyCFG
//     overrides this for `CondBr` with a constant condition.
//   - `redirectBlockTarget(oldTarget)`: consulted by the standard
//     emit when resolving every successor block id. SimplifyCFG
//     overrides this to redirect predecessors of an elided
//     trampoline block to its successor.
//
// **Internal fixed-point**: a chain of trampoline blocks `B1 → B2 →
// B3 → ... → S` (each just a `Br` to the next) collapses to direct
// `pred → S` redirects via transitive resolution + path compression
// (same `pathCompressAndVerify` substrate CopyProp + CSE consume).
//
// **Pipeline-level fixed-point**: ConstFold + SimplifyCFG + DCE are
// mutually enabling — ConstFold turns `if (5 < 3)` into `if (false)`;
// SimplifyCFG folds the CondBr; DCE drops the now-unreachable arm.
// Repeats may surface new opportunities. The pipeline's
// `maxIterations: N` field (D-OPT-FIXED-POINT-LOOP +
// D-OPT1-PASS-RUN-MAX-ITER) drives the whole pipeline up to N
// iterations or until no pass mutates.
//
// **Runtime-init globals carve-out**: same shape as ConstFold/Dce/
// Mem2Reg/CopyProp/CSE via shared `cloneGlobalsOrCarveOut`.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"

#include <cstddef>

namespace dss::opt::passes {

struct SimplifyCfgResult {
    bool        ok                 = false;
    std::size_t branchesFolded     = 0;
    std::size_t blocksJumpThreaded = 0;
};

[[nodiscard]] DSS_EXPORT SimplifyCfgResult
runSimplifyCfg(Mir& mir, TypeInterner const& interner,
               DiagnosticReporter& reporter);

} // namespace dss::opt::passes
