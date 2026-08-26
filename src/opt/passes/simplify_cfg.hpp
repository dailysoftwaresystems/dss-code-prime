#pragma once

// MIR-tier SimplifyCFG: minimal-scope CFG simplification.
//
// **Scope**:
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
//   3. **Block-merge** (D-OPT5-BLOCK-MERGE): a straight-line (P, B)
//      pair — P ends Br(B), B's only pred is P, B non-entry, no Phi,
//      instCount > 1 — merges into one block. The gate is pure
//      CFG-legality; markers play no role in admission.
//
// **StructCfMarker discipline (D-OPT4-1, closed)**: markers are NOT
// maintained through the rebuild — after `finish()` the pass re-stamps
// every block from the canonical CFG derivation
// (`rederiveStructCfMarkers`, mir_struct_markers.hpp). This subsumed
// the cycle-9 "non-Linear wins" repair and the c3-era both-non-Linear
// merge refusal (D-OPT4-1-NON-LINEAR-MARKER-MERGE).
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
#include <cstdint>

namespace dss::opt::passes {

struct SimplifyCfgResult {
    bool        ok                 = false;
    std::size_t branchesFolded     = 0;
    std::size_t blocksJumpThreaded = 0;
    std::size_t blocksMerged       = 0;
    // ── D-PERF-SIMPLIFYCFG-ADDRESS-TAKEN-QUERY-RESCANS-THE-WHOLE-FUNCTION ──
    //
    // THE ALGORITHMIC-SHAPE INSTRUMENT, and the reason it is on the RESULT
    // rather than in a comment. This pass answers two purely STRUCTURAL
    // questions about the source function — "is this block address-taken?"
    // and "does this block contain a Phi?" — and it used to answer the first
    // by calling `Mir::isBlockAddressTaken`, which RE-SCANS EVERY INSTRUCTION
    // OF THE OWNING FUNCTION on every call, once per block. That is
    // Θ(blocks × instructions): ✔MEASURED 66 ms of a 70 ms pass on a 4201-arm
    // switch, with `mutated == 0` — the pass spent 94% of its life proving it
    // had nothing to do. Both questions are now answered from a memo built by
    // ONE sweep of the function.
    //
    // `analysisInstScans` counts the instructions THAT SWEEP reads, summed
    // over every analyzed function. Its value is therefore exactly the
    // instruction count of the functions analyzed — one visit each, never one
    // visit per block per block. A test asserts that EQUALITY, because a wall-
    // clock assertion is fragile (and `wall_clock_in_tests_guard` refuses one)
    // while this number is deterministic and states the complexity directly.
    //
    // ⚠ THE BLINDNESS OF THIS INSTRUMENT, stated so no reader over-trusts it:
    // it counts what THIS pass reads. Work done inside a `Mir` accessor the
    // pass calls is invisible to it. That is precisely why the test asserts a
    // LOWER bound too — a `0` here means the sweep is gone, which means the
    // per-block rescan is back.
    std::uint64_t analysisInstScans = 0;
};

// `maintainMarkers` (default true = the developer/test posture): re-stamp every
// block's StructCfMarker from the canonical CFG derivation after the rebuild.
// Markers feed ONLY the verifier (no optimization decision or codegen reads
// them; pass rebuilds copy them through verbatim), so a pipeline that does NOT
// verify after every pass (`OptPipeline::verifyEveryPass == false`, the release
// posture) passes false here and the optimizer re-derives ONCE after the whole
// pipeline instead — the whole-module derivation (preds + per-fn RPO + dom) was
// ~78% of this pass's cost on SQLite (the D-OPT1-VERIFY-FREQUENCY-CONFIG
// posture split, applied to marker maintenance).
[[nodiscard]] DSS_EXPORT SimplifyCfgResult
runSimplifyCfg(Mir& mir, TypeInterner const& interner,
               DiagnosticReporter& reporter, bool maintainMarkers = true);

} // namespace dss::opt::passes
