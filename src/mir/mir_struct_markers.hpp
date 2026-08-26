#pragma once

// Canonical StructCfMarker derivation (D-OPT4-1 closure) — markers are
// a pure FUNCTION OF THE CFG, derived from dominators / post-dominators
// / natural loops, never hand-maintained through transforms.
//
// THE SPEC — priority order, FIRST CLAIM WINS, deterministic (rules run
// in order; within a rule, candidate blocks iterate in FUNCTION BLOCK
// order). `deriveStructCfMarkers` returns a module-blockCount()-sized
// vector; the INTENT is that only the function's REACHABLE blocks get
// derived values and everything else (unreachable blocks, other
// functions' blocks, the slot-0 sentinel) stays `Linear` — but the
// implementation is WIDER than that intent; see ⚠ below.
//
//   1. `funcBlockAt(f, 0)`                                → EntryBlock
//   2. back-edge target (∃ REACHABLE pred P that the
//      block dominates)                                   → LoopHeader
//   3. target of a loop-EXITING edge (an edge from a
//      natural-loop body block to a non-body block,
//      per `mirNaturalLoops` — see ⚠ for which loops
//      that actually reports)                             → LoopExit
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
// ⚠ THE DERIVATION REACHES PAST THE FUNCTION BEING DERIVED, and the
// difference is OBSERVABLE. `mirDominatesBlock(s, u, dom)` answers
// `Dominates` whenever `s.v == u.v`, BEFORE it consults the tree — so a
// SELF-LOOPING block is its own back-edge source even in a function
// whose dominator tree never saw it. Rule 3 therefore sees a
// single-block pseudo-loop for EVERY self-looping block in the MODULE,
// in EVERY function's derivation, and claims `LoopExit` on that block's
// non-self successors. Two consequences:
//   - an UNREACHABLE block CAN come back non-`Linear` (its function's
//     applier stamps it, and mir_text round-trips it), which is exactly
//     what the intent above says cannot happen;
//   - deriving function F can write non-`Linear` markers into slots
//     owned by function G. Nothing in-tree consumes those: `applyDerived`
//     reads only F's own slots and the verifier reads only F's REACHABLE
//     blocks — but the returned vector carries them.
// This is BEHAVIOUR, not aspiration: it predates the O(function)
// rework, it is pinned by `ForeignSelfLoopPseudoLoopClaimIsPreserved` in
// tests/mir/test_mir_struct_markers.cpp, and the scoped back-edge sweep
// reproduces it bit-for-bit ON PURPOSE. Narrowing it is a derivation-rule
// change — a decision to take deliberately, with the verifier and every
// producer moving together, never a silent tidy-up
// (D-MIR-STRUCTCF-DERIVATION-REACHES-PAST-THE-FUNCTION).
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
//
// COST: O(function) for the derivation itself, plus ONE O(module) scan
// for the self-loop index the cross-function rule above needs, plus the
// returned module-SIZED `out` vector this call fills (a fresh allocation
// per call — measured 0.15% of a whole-module rederive and deliberately
// NOT reused; see mir_struct_markers.cpp before changing that). A caller
// that derives EVERY function should use the module-wide applier below,
// which hoists that scan (and the post-dominator scratch) out of its
// per-function loop instead of paying them per call.
[[nodiscard]] DSS_EXPORT std::vector<StructCfMarker>
deriveStructCfMarkers(Mir const& mir, MirFuncId f,
                      std::vector<std::vector<MirBlockId>> const& preds,
                      std::vector<MirBlockId> const& rpo,
                      MirDomTree const& dom);

// ── THE SUBSTRATE BUNDLE FOR A CALLER THAT DERIVES EVERY FUNCTION ───────────
//
// The two O(module) establishments the overload above pays PER CALL — the
// self-loop index and the post-dominator buffers — hoisted into one object the
// caller owns for the whole sweep. The module-wide applier below has always
// hoisted them; this bundle is what lets a caller that is NOT the applier do
// the same.
//
// ✔MEASURED 2026-08-25 (cycle P36) — why it exists: `MirVerifier::checkDomination`
// derives markers for EVERY function through the per-call overload, so one
// whole-program verify paid `moduleFuncCount` self-loop scans and
// `moduleFuncCount` fresh eight-buffer post-dominator allocations. The optimizer
// runs that verify once per module and the cross-CU merge runs it again, so a
// 103-TU sqlite build paid it TWICE over an 86,411-block module
// ([[D-PERF-VERIFIER-REESTABLISHES-MODULE-SUBSTRATES-PER-FUNCTION]]). Same
// defect the module-wide applier already fixed; the verifier just could not
// reach the fix.
//
// Contract, the `MirDomScratch` / `MirPostDomScratch` pattern: one bundle per
// (sweep × module). It binds to the first module it sees ({module id,
// blockCount}); a later call with a DIFFERENT module fails loud rather than
// serving an index built from other blocks.
struct MirStructCfScratch {
    std::uint32_t moduleIdV  = 0;
    std::uint32_t blockCount = 0;   // 0 = not yet bound to a module
    std::vector<std::uint32_t> moduleSelfLoops;  // filled once, at bind time
    std::vector<std::uint32_t> candidates;       // per-function, storage reused
    MirPostDomScratch          postDom;
};

// Scratch-backed derivation — BYTE-IDENTICAL to the per-call overload above
// (same `deriveInto` body, same inputs; the bundle only changes where the two
// module substrates come from), O(function) per call after the first.
[[nodiscard]] DSS_EXPORT std::vector<StructCfMarker>
deriveStructCfMarkers(Mir const& mir, MirFuncId f,
                      std::vector<std::vector<MirBlockId>> const& preds,
                      std::vector<MirBlockId> const& rpo,
                      MirDomTree const& dom,
                      MirStructCfScratch& scratch);

// The applier: stamp every block of `f` with its derived marker
// (unreachable blocks of `f` stamp `Linear` — the derivation's value
// for them). Producers call this after `MirBuilder::finish()`.
DSS_EXPORT void rederiveStructCfMarkers(Mir& mir, MirFuncId f);

// Module-wide applier — every function, and every whole-module substrate
// established exactly ONCE: the predecessor map, the dominator scratch,
// the post-dominator scratch, and the self-loop index. Total cost is
// O(module) + Σ O(function), NOT functions × O(module).
//
// That distinction is the whole point: this is the marker path every
// producer runs (HIR→MIR lowering, SimplifyCfg, the inliner, the cross-CU
// merge, prune-unreachable), and on a merged whole-program module it used
// to be quadratic in module size. Measured on the SQLite CLI (103 TUs,
// --config=release, 86,411 blocks / 4,030 functions in the merged module):
// 5.3-6.6s per whole-module call and 29.0s across the 320 calls of one
// compile, against 0.19-0.22s per call and 1.2s across the same 320 after.
// Anything added here that is O(module) per FUNCTION puts that back.
DSS_EXPORT void rederiveStructCfMarkers(Mir& mir);

} // namespace dss
