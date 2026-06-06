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

// Does `a` dominate `b`? Tri-state — `GaveUp` on iteration-cap overflow.
[[nodiscard]] DSS_EXPORT MirDomResult
mirDominatesBlock(MirBlockId a, MirBlockId b, MirDomTree const& dom);

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

// Natural-loop forest computation. See `MirNaturalLoop` for shape.
[[nodiscard]] DSS_EXPORT std::vector<MirNaturalLoop>
mirNaturalLoops(Mir const& mir,
                MirDomTree const& dom,
                std::vector<std::vector<MirBlockId>> const& preds);

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
