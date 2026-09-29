#pragma once

// Shared MIR dominator-tree helpers — Cooper-Harvey-Kennedy iterative
// idom + tri-state dominates + dominance-frontier — consumed by the
// verifier (ML3 use-dom-def check) and by optimizer passes that need
// dominance information (D-OPT-DOMTREE-EXTRACTION).
//
// Header / TU split (D-OPT-MIR-DOM-HPP-CPP-SPLIT): function bodies live
// in `mir_dom.cpp`. Struct + enum definitions stay in the header
// (consumers need their layout); function declarations are non-inline.
// Triggered by the 5th non-verifier consumer (`mir_alias.hpp`) — the
// inline header was forcing every consumer + transitively-including
// test TU to recompile + re-instantiate the heavyweight Cooper-Harvey-
// Kennedy bodies.
//
// Transitive includes (`<algorithm>` + `<unordered_set>`) are retained
// so consumers reasoning about `MirNaturalLoop::body`'s sorted-by-slot
// invariant or constructing their own dominator-frontier-derived sets
// don't depend on a non-portable transitive chain.

#include "core/export.hpp"
#include "mir/mir.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <unordered_set>
#include <vector>

namespace dss {

// Per-block dominator state. `idom[b.v]` is the immediate dominator;
// `gaveUp[b.v]` is non-zero iff the iterative intersect bailed for at
// least one predecessor of `b` (malformed input — idom cycle, missing
// self-loop at entry, etc.). The verifier maps this to
// `I_VerifierFailure`; the optimizer (Mem2Reg / future LICM) treats
// gaveUp blocks conservatively (do not rewrite).
//
// `gaveUp` is `vector<uint8_t>` rather than `vector<bool>` deliberately —
// the proxy-iterator trap of `vector<bool>` (no stable references,
// `auto&` doesn't behave as expected) buys ~7x memory savings on a
// field whose consumer count is small; the storage cost at realistic
// block counts (< 10k per fn) is negligible.
struct MirDomTree {
    std::vector<MirBlockId>   idom;
    std::vector<std::uint8_t> gaveUp;
};

// Tri-state dominance result. `GaveUp` = the iteration-count guard
// fired during the dominates walk — the caller MUST map to a "could
// not resolve" diagnostic, not a wrong-blame "not dominated."
enum class MirDomResult : std::uint8_t { Dominates, DoesNot, GaveUp };

// Per-block POST-dominator state (the reverse-graph sibling of
// `MirDomTree`), computed per function over a VIRTUAL exit node so
// multi-exit functions have a single reverse-graph root. Arrays are
// sized `mir.blockCount() + 1`; the extra slot IS the virtual exit.
//
// `ipdom[b.v]` is THREE-valued:
//   - a REAL block slot: the immediate post-dominator (the join every
//     path from `b` to function exit passes through);
//   - the VIRTUAL slot (`virtualExitSlot()`): paths from `b` diverge
//     to DISTINCT exits — there is no real join (e.g. the head of a
//     both-arms-return if);
//   - INVALID (`!valid()`): `b` is reverse-unreachable — no path from
//     `b` reaches any exit (an infinite-loop region, e.g. `for(;;)`
//     with no break).
//
// WARNING: the virtual-exit id `MirBlockId{blockCount, tag}` is a
// synthetic slot, NOT an arena element — passing it to any
// `Mir::block*` accessor ABORTS (arena bounds guard). Compare against
// `virtualExitSlot()` / use `isVirtualExit()`; never dereference.
struct MirPostDomTree {
    std::vector<MirBlockId>   ipdom;
    std::vector<std::uint8_t> gaveUp;
    // The synthetic exit's slot value (== blockCount() of the module
    // the tree was computed over). Stored, not recomputed, so a tree
    // outlives intermediate module rebuilds without ambiguity.
    std::uint32_t virtualExit = 0;

    [[nodiscard]] std::uint32_t virtualExitSlot() const noexcept { return virtualExit; }
    [[nodiscard]] bool isVirtualExit(MirBlockId b) const noexcept {
        return b.valid() && b.v == virtualExit;
    }
};

// Natural-loop forest: every back-edge (u → v) where v dominates u
// induces a natural loop with header `v`. The loop's body is `v`
// itself plus every block from which `u` is reachable in the
// predecessor graph (the standard Aho-Sethi-Ullman / Cooper-Harvey-
// Kennedy natural-loop computation). Multiple back-edges to the
// same header merge into one loop with multiple `backEdgeSources`.
//
// LICM consumes this to: (a) identify hoist candidates whose
// operands are all defined OUTSIDE the loop body, (b) locate the
// preheader (the unique non-back-edge predecessor of the header,
// when one exists).
struct MirNaturalLoop {
    MirBlockId              header;
    std::vector<MirBlockId> body;             // blocks IN the loop (header + all reachable-to-back-edge)
    std::vector<MirBlockId> backEdgeSources;  // blocks with edges back to header
};

// Build the predecessor adjacency: preds[blockSlot] = MirBlockIds
// naming this block as a successor. O(V + E). Silently skips
// out-of-range successor edges — `MirVerifier::checkStructuralInvariants`
// emits the I_VerifierFailure diagnostic for them; the dominator
// computation only needs the well-formed subset.
[[nodiscard]] DSS_EXPORT std::vector<std::vector<MirBlockId>>
mirBuildPredecessors(Mir const& mir);

// Cooper-Harvey-Kennedy iterative dominators ("A Simple, Fast
// Dominance Algorithm"). Returns `idom[blockSlot] = MirBlockId` mapping
// each reachable block (in `order`) to its immediate dominator (entry's
// idom is itself). Unreachable blocks have InvalidMirBlock.
//
// Termination safety: the inner `intersect` walks idom-chains; a
// malformed idom (cycle / missing entry self-loop) is bounded by a
// step cap derived from idom array size. Overflow → block tagged
// `gaveUp`, never an infinite loop.
[[nodiscard]] DSS_EXPORT MirDomTree
computeMirDomTree(Mir const&                                  mir,
                  MirBlockId                                  entry,
                  std::vector<MirBlockId> const&              order,
                  std::vector<std::vector<MirBlockId>> const& preds);

// Reusable dominator-computation scratch (D-OPT-DOMTREE-SCRATCH-REUSE): the
// fresh-allocation `computeMirDomTree` above allocates SIX whole-module-sized
// buffers per call — including a full copy of the caller's predecessor map —
// for ONE function's CHK walk; on a large module that allocation storm is
// ~95% of CSE's and LICM's per-pass cost (measured: ~94s of the sqlite
// release optimize). The scratch owns those buffers ONCE per pass call and
// resets, at the ENTRY of each call, ONLY the slots the PREVIOUS call wrote
// (its self-recorded touched list = that call's `order` ∪ {entry} — the
// proven-complete write set), restoring exactly the fresh-allocation
// defaults. Same inputs → byte-identical dom trees (the differential pins in
// test_mir_dom.cpp compare v AND arenaTag over the FULL module-sized arrays
// after every call in adversarial sequences).
//
// Contract: one scratch per (pass call × module). The scratch binds to the
// first module it sees ({module id, blockCount} — a later call with a
// DIFFERENT module fails loud, the MirMemoryClobbers stale-guard pattern).
// The returned tree/children references are valid ONLY until the next
// compute call with the same scratch. Bind results as `auto const&`.
struct MirDomScratch {
    std::uint32_t moduleIdV  = 0;
    std::uint32_t blockCount = 0;   // 0 = not yet bound to a module
    // CHK core buffers (module-sized; kUnsetSlot / 0 outside touched slots).
    // Module-sized ON PURPOSE: the core's intersect step-cap derives from the
    // idom array SIZE — compressing to function-local sizing would change
    // when pathological chains give up (a behavior change). Do not compress.
    std::vector<std::uint32_t> coreIdom;
    std::vector<std::uint8_t>  coreGaveUp;
    std::vector<std::uint32_t> rpoIndex;
    // Result buffers (module-sized; MirBlockId{} / 0 outside touched slots).
    MirDomTree tree;
    std::vector<std::vector<MirBlockId>> children;
    // The previous call's write set (reset at the next call's entry) + the
    // current call's ascending-sorted copy (children fill iterates it SORTED
    // so the children lists keep the fresh path's ascending-slot order — the
    // CSE dom-DFS traversal order depends on it).
    std::vector<std::uint32_t> touched;
    std::vector<std::uint32_t> touchedSorted;
    bool childrenFilled = false;   // children fill is once-per-compute-call
    // Dominance-frontier result (module-sized; empty outside the fill's write
    // set) for the scratch-backed `mirDominanceFrontier` overload. Its write
    // set is NOT a subset of `touched`: a frontier walk starts at EVERY
    // predecessor of a join, and a predecessor unreachable from this function's
    // entry is outside `order` yet still receives the join (exactly as the
    // fresh path writes it). So the fill records its own write set, and the
    // next compute call resets those slots — never re-derived from `touched`.
    std::vector<std::vector<MirBlockId>> frontier;
    std::vector<std::uint32_t> frontierTouched;
    bool frontierFilled = false;   // frontier fill is once-per-compute-call
};

// Scratch-backed dominator computation — byte-identical results to the
// fresh-allocation overload above (same core, same values), O(|order|) per
// call instead of O(module). `preds` MUST be `mirBuildPredecessors(mir)` for
// the SAME module (fail-loud size check). The returned reference lives in
// `scratch` and is invalidated by the next call with that scratch.
[[nodiscard]] DSS_EXPORT MirDomTree const&
computeMirDomTree(Mir const&                                  mir,
                  MirBlockId                                  entry,
                  std::vector<MirBlockId> const&              order,
                  std::vector<std::vector<MirBlockId>> const& preds,
                  MirDomScratch&                              scratch);

// Does `a` dominate `b`? Tri-state — `GaveUp` on iteration-cap overflow.
[[nodiscard]] DSS_EXPORT MirDomResult
mirDominatesBlock(MirBlockId a, MirBlockId b, MirDomTree const& dom);

// Post-dominator tree for ONE function — the same Cooper-Harvey-
// Kennedy core as `computeMirDomTree` run over the REVERSE graph with
// a virtual exit (see `MirPostDomTree`):
//   - reverse-predecessors of a block = its forward successors;
//   - the virtual node's reverse-successors = every Return/Unreachable-
//     terminated block of the function that is forward-REACHABLE from
//     the function entry (unreachable exits don't define the join
//     structure of live code);
//   - the traversal order is reverse-RPO from the virtual node.
// Consumed by `deriveStructCfMarkers` (mir_struct_markers.hpp) for
// if/switch join derivation.
[[nodiscard]] DSS_EXPORT MirPostDomTree
computeMirPostDomTree(Mir const& mir, MirFuncId f);

// Reusable post-dominator scratch (D-OPT-POSTDOM-SCRATCH-REUSE) — the
// reverse-graph sibling of `MirDomScratch`, and the same bargain. The
// fresh-allocation `computeMirPostDomTree` above allocates EIGHT whole-module-
// sized buffers per call for ONE function's reverse CHK walk — including two
// `vector<vector<uint32_t>>` adjacency maps, which alone are ~48 bytes PER
// MODULE BLOCK per call. A whole-module marker re-derivation calls it once per
// function, so that allocation storm is ~59% of `rederiveStructCfMarkers`'s
// cost on a merged SQLite module (measured: 3.04s of a 5.16s call, 86,411
// blocks / 4,030 functions).
//
// The scratch owns those buffers ONCE and resets, at the ENTRY of each call,
// ONLY the slots the PREVIOUS call wrote (two self-recorded touched lists —
// the adjacency write set and the reverse-RPO order — which are the
// proven-complete write sets), restoring exactly the fresh-allocation
// defaults. Same inputs → byte-identical trees (the differential pins in
// test_mir_struct_markers.cpp — `MirPostDomScratchReuse.*`, beside the
// derivation rules that consume this tree — compare ipdom v AND arenaTag plus
// gaveUp over the FULL module-sized arrays after every call in adversarial
// sequences).
//
// Contract: identical to `MirDomScratch` — one scratch per (pass call ×
// module), bound on first use to {module id, blockCount} with a fail-loud
// stale guard, and the returned reference is valid ONLY until the next call
// with the same scratch. Bind results as `auto const&`.
//
// Arrays are sized `blockCount + 1` (the extra slot IS the virtual exit), and
// are module-sized ON PURPOSE for the same reason `MirDomScratch`'s are: the
// CHK intersect step-cap derives from the idom array SIZE, so compressing to
// function-local sizing would change when pathological chains give up. Do not
// compress.
struct MirPostDomScratch {
    std::uint32_t moduleIdV  = 0;
    std::uint32_t blockCount = 0;   // 0 = not yet bound to a module
    // Reverse-graph adjacency (revPreds[b] = forward successors of b, plus the
    // virtual exit for exit blocks; revSuccs is its transpose).
    std::vector<std::vector<std::uint32_t>> revPreds;
    std::vector<std::vector<std::uint32_t>> revSuccs;
    // Reverse-RPO traversal state.
    std::vector<std::uint8_t>  visited;
    std::vector<std::uint32_t> order;
    // CHK core buffers (kUnsetSlot / 0 outside touched slots).
    std::vector<std::uint32_t> coreIdom;
    std::vector<std::uint8_t>  coreGaveUp;
    std::vector<std::uint32_t> rpoIndex;
    // Result buffer (MirBlockId{} / 0 outside touched slots).
    MirPostDomTree tree;
    // The previous call's write sets, reset at the next call's entry:
    // `touchedRev` = every slot whose revPreds/revSuccs list was appended to;
    // `touchedNodes` = the reverse-RPO order (== the visited set == the CHK
    // core's and the result conversion's write set).
    std::vector<std::uint32_t> touchedRev;
    std::vector<std::uint32_t> touchedNodes;
};

// Scratch-backed post-dominator computation — byte-identical results to the
// fresh-allocation overload above (same core, same values, same virtualExit),
// O(function) per call instead of O(module). The returned reference lives in
// `scratch` and is invalidated by the next call with that scratch.
[[nodiscard]] DSS_EXPORT MirPostDomTree const&
computeMirPostDomTree(Mir const& mir, MirFuncId f, MirPostDomScratch& scratch);

// Does `a` post-dominate `b`? Tri-state sibling of `mirDominatesBlock`.
// `a` may be the virtual-exit id (`MirBlockId{postdom.virtualExitSlot(),
// tag}`) — it post-dominates every reverse-reachable block.
[[nodiscard]] DSS_EXPORT MirDomResult
mirPostDominatesBlock(MirBlockId a, MirBlockId b, MirPostDomTree const& postdom);

// Dominance frontier: for each block `b`, the set of blocks `n` such
// that `b` dominates a predecessor of `n` but does NOT strictly
// dominate `n` itself. Cooper-Harvey-Kennedy formulation — same paper
// as the idom algorithm. Used by Mem2Reg to determine where Phi
// nodes need to be inserted (per the Cytron-Ferrante SSA construction).
//
// Returns `df[b.v]` = vector of block ids in `b`'s frontier. Blocks
// not reachable in `dom` get an empty entry. Idempotent + O(E) total.
[[nodiscard]] DSS_EXPORT std::vector<std::vector<MirBlockId>>
mirDominanceFrontier(Mir const& mir,
                     MirDomTree const& dom,
                     std::vector<std::vector<MirBlockId>> const& preds);

// Scratch-backed dominance frontier — byte-identical to the fresh overload
// above, O(function) per call instead of O(module).
// D-OPT-MEM2REG-WHOLE-MODULE-DOMINANCE-PER-FUNCTION: Mem2Reg asked for a
// ONE-function frontier by sweeping and allocating the WHOLE module, once per
// function, which made the pass quadratic in module size.
//
// WHY IT IS IDENTICAL. The fresh sweep visits slots `1..blockCount` ascending
// and contributes only for a block whose idom is VALID — and a one-function
// tree gives a valid idom to `order ∪ {entry}` and to nothing else. That set,
// ascending, is exactly the scratch's `touchedSorted`, so iterating it visits
// the same blocks in the same order, runs the same walk with the same step
// cap (`idom.size()` is module-sized in both), and appends to every list in
// the same order. Lists never written stay empty, as fresh ones are.
//
// `dom` MUST be the tree the SAME scratch's compute call just produced
// (fail-loud identity check, as for the children overload), and `preds` the
// pass's hoisted whole-module map (fail-loud size check). The returned
// reference is invalidated by the next compute call with that scratch.
[[nodiscard]] DSS_EXPORT std::vector<std::vector<MirBlockId>> const&
mirDominanceFrontier(Mir const& mir,
                     MirDomTree const& dom,
                     std::vector<std::vector<MirBlockId>> const& preds,
                     MirDomScratch& scratch);

// ── THE WORK THESE HELPERS DO, AS A COUNT ─────────────────────────────────────
//
// D-OPT-MEM2REG-WHOLE-MODULE-DOMINANCE-PER-FUNCTION. Every predecessor-map,
// dominator-tree, dominance-frontier and dominator-children helper in this
// header adds the number of block SLOTS it swept (allocated, reset, or
// iterated) to a per-thread counter; this returns it and zeroes it. The fresh
// overloads each sweep the whole module (`blockCount`), the scratch overloads
// sweep their write sets — so a caller that asks for one-function answers
// through the fresh overloads, once per function, shows up here as
// functions × module blocks, and one that uses the scratch shows up as linear.
//
// It exists so a complexity regression can be pinned by COUNTING, never by a
// stopwatch: the count is deterministic, host- and load-independent, and the
// same in Debug and Release. Per-THREAD because the driver's per-CU pool runs
// several modules at once; a pass, and a test, reads the thread it ran on.
// ⓘ Scope, stated: the forward-dominator family only. The post-dominator
// helpers are not counted; the natural-loop sweep has its OWN counter,
// `mirNaturalLoopSourcesSweptTake`, so neither family's pins move the other's.
[[nodiscard]] DSS_EXPORT std::uint64_t mirDomSlotsSweptTake() noexcept;

// Dominator-tree children: invert `idom` so consumers can walk the
// tree top-down. Returns `children[b.v]` = list of blocks whose
// immediate dominator is `b` (excluding the entry's self-loop).
// O(V), one linear scan over `idom`. Mem2Reg's rename DFS walks the
// dom tree in pre-order; LICM's hoist scan walks it bottom-up.
//
// Blocks with `gaveUp[i] != 0` or invalid `idom[i]` get no parent
// entry — the verifier maps those to `I_VerifierFailure`; this helper
// silently drops them from the tree (a conservative caller treats
// "no parent" as "do not promote / hoist through this block").
[[nodiscard]] DSS_EXPORT std::vector<std::vector<MirBlockId>>
mirDomTreeChildren(Mir const& mir, MirDomTree const& dom);

// Scratch-backed children inversion (D-OPT-DOMTREE-SCRATCH-REUSE) — byte-
// identical to the fresh overload above (ascending-slot iteration over the
// scratch's sorted touched set; contributing parents are always inside the
// touched set because every stored idom value is in `order`). `dom` MUST be
// the tree the SAME scratch's compute call just produced (fail-loud identity
// check) — the touched bookkeeping is what makes the partial reset complete.
[[nodiscard]] DSS_EXPORT std::vector<std::vector<MirBlockId>> const&
mirDomTreeChildren(Mir const& mir, MirDomTree const& dom,
                   MirDomScratch& scratch);

// ── THE NATURAL-LOOP FOREST OF ONE FUNCTION ──────────────────────────────────
//
// THE RULE, stated once: the natural loops of function `f` are the loops whose
// back-edge SOURCE is one of `f`'s own blocks. `mirBackEdgeCandidates` below
// builds that source set and is the ONLY production builder of it; every
// consumer reaches the forest through it and this sweep — the struct-CF marker
// derivation (so every producer that re-stamps markers AND the verifier that
// re-derives them), LICM, and the thin-LTO module summary.
//
// WHY `f`'s OWN RANGE, AND NOT ONLY ITS DOMINATOR ORDER: a block `u` outside
// `order` has an INVALID idom, so `mirDominatesBlock(s, u, dom)` can answer
// `Dominates` only when `s.v == u.v` — the only back edge such a block can
// source is a self-loop. An UNREACHABLE self-looping block of `f`'s OWN is
// still a loop of `f` (canon, D-MIR-STRUCTCF-UNREACHABLE-BLOCK-CLAIMED): its
// function claims it, and no other function does.
//
// ⓘ HISTORY (D-MIR-STRUCTCF-DERIVATION-REACHES-PAST-THE-FUNCTION, now closed).
// Until 2026-09-18 every function's source set also carried every SELF-LOOPING
// block of the MODULE, so that the scoped sweep reproduced a whole-module one —
// and a foreign self-loop became a one-block pseudo-loop in EVERY function's
// forest. ✔MEASURED on the sqlite amalgamation, release: 256,878 pseudo-loops
// per LICM call on the full 9.57 MB (98.8% of each forest; ×88 for ×7.9
// functions — quadratic) and 11.0% of the compile's retired instructions, while
// scoping the rule changed no artifact byte over the corpus. The whole-module
// overload and the module self-loop index are deleted, so the reach has no
// second door back in.
//
// `candidateSources` MUST be ascending, unique, and every element in
// [1, blockCount) — a violation is a caller bug and fails loud (ascending is
// what fixes `MirNaturalLoop::backEdgeSources` order). Loop BODIES are the
// backward closure over `preds` from the back-edge sources, so the sweep still
// discovers every body block.
[[nodiscard]] DSS_EXPORT std::vector<MirNaturalLoop>
mirNaturalLoops(Mir const& mir,
                MirDomTree const& dom,
                std::vector<std::vector<MirBlockId>> const& preds,
                std::span<std::uint32_t const> candidateSources);

// The back-edge SOURCE set of `f` — THE rule above — in the shape the sweep
// demands (ascending, unique, in [1, blockCount)): `f`'s own contiguous block
// range, plus anything in `rpo` outside that range (a malformed cross-function
// edge — the verifier owns the diagnostic, but an analysis must not silently
// answer differently while one exists). Nothing else: no block of another
// function enters `f`'s sweep.
//
// ⓘ It lives HERE, beside the sweep it feeds, and in no one caller.
// ✔MEASURED 2026-08-25 (cycle P36): while it was private to
// `mir_struct_markers.cpp`, the SECOND caller that needed it — `runLicm` —
// could not reach it and kept an O(functions × module blocks) sweep
// ([[D-OPT-LICM-NATURAL-LOOPS-MODULE-WIDE-SCAN]]). A contract whose only
// satisfier is private to one TU is a contract the next caller re-derives.
//
// `out` is cleared and refilled; pass the SAME vector across a function loop so
// the storage is reused rather than reallocated per function. `f` must have at
// least one block. Fails loud if `f`'s blocks are not contiguous in the arena —
// the range enumeration rests on that, and a future non-contiguous layout would
// otherwise silently NARROW the sweep instead of failing.
DSS_EXPORT void mirBackEdgeCandidates(Mir const& mir, MirFuncId f,
                                      std::vector<MirBlockId> const& rpo,
                                      std::vector<std::uint32_t>& out);

// The natural-loop sweep's work, as a COUNT: every back-edge candidate source
// `mirNaturalLoops` visits is added to a per-thread counter; this returns it and
// zeroes it. A consumer that asks for each function's forest over that
// function's own blocks shows up as Σ function blocks — linear in the module;
// one that lets blocks of OTHER functions into each function's sweep shows up
// as functions × those blocks. Deterministic, host- and load-independent, so a
// pin COUNTS and never times. Per-THREAD for the same reason as
// `mirDomSlotsSweptTake`: the driver's per-CU pool runs several modules at once.
[[nodiscard]] DSS_EXPORT std::uint64_t mirNaturalLoopSourcesSweptTake() noexcept;

// Iterated dominance frontier (IDF) of a set of "def blocks". For
// Cytron-Ferrante SSA construction (Mem2Reg): a Phi for variable V
// must be inserted at every block in IDF(def-blocks-of(V)). The
// classic worklist formulation: start with the def set, expand by
// DF, repeat until no new blocks are added. Each block enters the
// IDF at most once → terminates in O(|IDF| · |DF|).
//
// `df` is the output of `mirDominanceFrontier`. `defBlocks` is the
// blocks containing a definition of the variable (e.g. a Store to
// a promotable alloca for Mem2Reg). Returns the IDF in
// insertion-order; the caller iterates linearly.
[[nodiscard]] DSS_EXPORT std::vector<MirBlockId>
mirIteratedDominanceFrontier(
    std::vector<MirBlockId> const& defBlocks,
    std::vector<std::vector<MirBlockId>> const& df);

} // namespace dss
