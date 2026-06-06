#pragma once

// MIR-tier Loop-Invariant Code Motion (LICM).
//
// **Scope (OPT6 c1)**: hoist provably-invariant instructions from a
// loop body to the loop's preheader. A loop is a natural loop —
// header H + back-edge sources B_i + body = transitive predecessors
// of B_i reachable without going through H (Aho-Sethi-Ullman /
// Cooper-Harvey-Kennedy natural-loop computation).
//
// **Hoist eligibility (c1)**:
//   - Pure: NOT `opcodeInfo.hasSideEffects` (excludes Alloca / Store /
//     Call / IntrinsicCall / terminators).
//   - NOT a Phi (loop-header phis are inherently loop-bound; their
//     incomings are by definition not invariant).
//   - NOT Volatile (observable memory semantics).
//   - NOT a Load (alias-unsafe defer — no alias-analysis substrate;
//     anchor `D-OPT-LOAD-ALIAS-ANALYSIS` follows CSE's parallel exclusion).
//   - NOT trap-eligible (SDiv / UDiv / SMod / UMod — division by zero
//     would happen unconditionally in the preheader; in a loop that
//     might never execute, this changes observable behavior). Anchor
//     `D-OPT6-LICM-TRAP-SAFE-HOIST` for a future cycle that proves
//     non-zero divisor via interval / value-range analysis.
//   - ALL operands defined OUTSIDE the loop body OR already
//     hoisted earlier this loop (chained-invariant resolution,
//     D-OPT6-LICM-CHAINED-INVARIANTS, cycle 10j closure
//     2026-06-04). Termination: a per-loop monotone-growing
//     `hoistedInThisLoop` set; the iterator breaks on a no-
//     progress round. Defensive step cap at 64 (bounded above by
//     `|loop.body inst count|`).
//
// **Preheader requirement (c1)**: the pass hoists ONLY when the
// loop header has EXACTLY ONE non-back-edge predecessor — that
// predecessor IS the preheader. Loops with zero or multiple
// external predecessors are skipped; preheader insertion (block
// creation) is anchored `D-OPT6-LICM-PREHEADER-INSERTION` for a
// future cycle (it's a block-INSERT transform, distinct from
// SimplifyCFG's block-ELISION; sharing the rebuild substrate
// requires a hook the c1 pass doesn't yet need).
//
// **Mechanism**: the pass uses the new `MirRebuildPolicy::
// onBlockBeforeTerminator` hook. For each hoist candidate I in loop
// body block L (target preheader P):
//   1. `shouldEmit(I)` returns false → I is omitted from L's
//      original-block rebuild.
//   2. `onBlockBeforeTerminator(P, ...)` emits a clone of I into P
//      AFTER P's source instructions and BEFORE P's terminator. The
//      clone's operands are resolved through the rewrite map (which
//      is populated for every loop-external def by the time P's
//      rebuild reaches its terminator). The new id is recorded in
//      the rewrite map under I's old id so users in L (and any
//      block dominated by L) resolve to the new hoisted location.
//
// **Why this shape**: hoisting is move-not-delete. The substrate's
// `tryRewrite` returns a NEW id; `substituteOldOperand` redirects
// uses; but neither expresses "emit this old inst INTO A DIFFERENT
// block." `onBlockBeforeTerminator` is the natural insertion-site
// hook — same pattern as Mem2Reg's `onBlockBegin` (insert Phis at
// block head) but at the OTHER end of the block.
//
// **Runtime-init globals carve-out**: same shape as the other
// MIR-tier passes via `cloneGlobalsOrCarveOut`.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"

#include <cstddef>

namespace dss::opt::passes {

struct LicmResult {
    bool        ok                = false;
    std::size_t instructionsHoisted = 0;
};

[[nodiscard]] DSS_EXPORT LicmResult
runLicm(Mir& mir, TypeInterner const& interner,
        DiagnosticReporter& reporter);

} // namespace dss::opt::passes
