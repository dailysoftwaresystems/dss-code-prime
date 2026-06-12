#pragma once

// Canonical StructCfMarker derivation (D-OPT4-1 closure) — markers are
// a pure FUNCTION OF THE CFG, derived from dominators / post-dominators
// / natural loops, never hand-maintained through transforms.
//
// THE SPEC — priority order, FIRST CLAIM WINS, deterministic (rules run
// in order; within a rule, candidate blocks iterate in FUNCTION BLOCK
// order). `deriveStructCfMarkers` returns a module-blockCount()-sized
// vector; only the function's REACHABLE blocks get derived values —
// everything else (unreachable blocks, other functions' blocks, the
// slot-0 sentinel) stays `Linear`.
//
//   1. `funcBlockAt(f, 0)`                                → EntryBlock
//   2. back-edge target (∃ REACHABLE pred P that the
//      block dominates)                                   → LoopHeader
//   3. target of a loop-EXITING edge (an edge from a
//      natural-loop body block to a non-body block,
//      per `mirNaturalLoops`)                             → LoopExit
//   4. for each reachable CondBr-terminated block H that
//      is NOT a derived LoopHeader (a loop-condition
//      CondBr is loop vocabulary, not if vocabulary):
//      J = ipdom(H) (invalid/gaveUp → treat as virtual);
//        succs[0] != J                                    → IfThen
//        succs[1] != J                                    → IfElse
//        J REAL + unclaimed                               → IfJoin
//      Covers if / ternary / && / || diamonds. A both-
//      arms-return if has J = virtual → arms marked, NO
//      IfJoin. QUIRK ACCEPTED AS CANON: a compound loop
//      condition (`while (a && b)`) derives the BODY-HEAD
//      as IfThen via the join-block's CondBr — the header
//      is the LoopHeader (rule 2), so rule 4 skips it, and
//      the &&-join's CondBr (body, exit) is an if-shape
//      whose then-arm is the loop body. Canonical is
//      canonical: every producer and the verifier agree.
//   5. for each reachable Switch-terminated block:
//        each case/default target != ipdom               → SwitchCase
//        ipdom REAL + unclaimed                          → SwitchJoin
//      (the discriminant block itself falls to lower
//      rules — SwitchHead is NOT derived; see below)
//   6. otherwise                                          → Linear
//
// DELIBERATELY NOT DERIVED (dormant enum values — round-trip vocabulary
// for `mir_text` only after this cycle):
//   - ExitBlock:  no producer ever emitted it; the old verifier rule
//     ("ExitBlock terminates in Return/Unreachable") died with the
//     count-parity model.
//   - LoopLatch:  hir_to_mir still stamps it creation-time (do-while
//     continue target, for-update block) as an intent default that the
//     final rederive OVERWRITES — it is NOT CFG-derivable: a while
//     body-tail and a for-update block can present IDENTICAL CFG
//     shapes. Back-edge SOURCES are derivable (mirNaturalLoops::
//     backEdgeSources) when a future WASM consumer needs them.
//   - SwitchHead: never emitted by any producer.
//
// PLACEMENT PRINCIPLE: producers re-derive AT THEIR OWN SITES (HIR→MIR
// lowering, SimplifyCfg, the inliner, the cross-CU merge — each calls
// `rederiveStructCfMarkers` after `finish()`); the verifier RECOMPUTES
// the derivation independently and checks stored == derived per
// reachable block. A central rederive-before-verify would make the
// equality tautological — the verifier must never trust a producer-
// supplied vector.
//
// CFG-preserving passes (ConstFold / Mem2Reg / CopyProp / CSE / LICM /
// DCE) copy markers verbatim through the rebuild substrate; equality
// keeps holding because their rebuilds don't change dominance /
// post-dominance / loop structure among surviving blocks.

#include "core/export.hpp"
#include "mir/mir.hpp"
#include "mir/mir_dom.hpp"

#include <vector>

namespace dss {

// Enum-spelling name for diagnostics ("LoopHeader", "IfJoin", …).
// Distinct from mir_text's lowercase serialization vocabulary — this
// is the human-facing diagnostic name, not the round-trip token.
[[nodiscard]] DSS_EXPORT char const* structCfMarkerName(StructCfMarker m) noexcept;

// The canonical derivation (see the spec above). Convenience overload —
// computes predecessors / RPO / dominators itself.
[[nodiscard]] DSS_EXPORT std::vector<StructCfMarker>
deriveStructCfMarkers(Mir const& mir, MirFuncId f);

// Precomputed-substrate overload: `preds` is module-wide
// (`mirBuildPredecessors`), `rpo` is the function's reverse post-order
// from its entry, `dom` the function's dominator tree over that order.
// The verifier reuses its per-function dominance computation here so
// one verify costs ONE preds/RPO/dom per function (the post-dominator
// tree is the only addition, computed internally).
[[nodiscard]] DSS_EXPORT std::vector<StructCfMarker>
deriveStructCfMarkers(Mir const& mir, MirFuncId f,
                      std::vector<std::vector<MirBlockId>> const& preds,
                      std::vector<MirBlockId> const& rpo,
                      MirDomTree const& dom);

// The applier: stamp every block of `f` with its derived marker
// (unreachable blocks of `f` stamp `Linear` — the derivation's value
// for them). Producers call this after `MirBuilder::finish()`.
DSS_EXPORT void rederiveStructCfMarkers(Mir& mir, MirFuncId f);

// Module-wide applier — every function, one predecessor build.
DSS_EXPORT void rederiveStructCfMarkers(Mir& mir);

} // namespace dss
