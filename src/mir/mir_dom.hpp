#pragma once

// Shared MIR dominator-tree helpers — Cooper-Harvey-Kennedy iterative
// idom + tri-state dominates + dominance-frontier — consumed by the
// verifier (ML3 use-dom-def check) and by optimizer passes that need
// dominance information (D-OPT-DOMTREE-EXTRACTION).

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

// Build the predecessor adjacency: preds[blockSlot] = MirBlockIds
// naming this block as a successor. O(V + E). Silently skips
// out-of-range successor edges — `MirVerifier::checkStructuralInvariants`
// emits the I_VerifierFailure diagnostic for them; the dominator
// computation only needs the well-formed subset.
[[nodiscard]] inline std::vector<std::vector<MirBlockId>>
mirBuildPredecessors(Mir const& mir) {
    std::vector<std::vector<MirBlockId>> preds(mir.blockCount());
    for (std::uint32_t i = 1; i < mir.blockCount(); ++i) {
        MirBlockId const from{i, mir.id().v};
        for (MirBlockId const to : mir.blockSuccessors(from)) {
            if (to.v < preds.size()) {
                preds[to.v].push_back(from);
            }
        }
    }
    return preds;
}

// Cooper-Harvey-Kennedy iterative dominators ("A Simple, Fast
// Dominance Algorithm"). Returns `idom[blockSlot] = MirBlockId` mapping
// each reachable block (in `order`) to its immediate dominator (entry's
// idom is itself). Unreachable blocks have InvalidMirBlock.
//
// Termination safety: the inner `intersect` walks idom-chains; a
// malformed idom (cycle / missing entry self-loop) is bounded by a
// step cap derived from idom array size. Overflow → block tagged
// `gaveUp`, never an infinite loop.
[[nodiscard]] inline MirDomTree
computeMirDomTree(Mir const&                                  mir,
                  MirBlockId                                  entry,
                  std::vector<MirBlockId> const&              order,
                  std::vector<std::vector<MirBlockId>> const& preds) {
    MirDomTree st;
    st.idom.resize(mir.blockCount());
    st.gaveUp.resize(mir.blockCount(), false);
    auto& idom = st.idom;
    std::vector<std::uint32_t> rpoIndex(mir.blockCount(),
        static_cast<std::uint32_t>(-1));
    for (std::uint32_t i = 0; i < order.size(); ++i) {
        rpoIndex[order[i].v] = i;
    }
    if (!entry.valid()) return st;
    idom[entry.v] = entry;
    std::uint32_t const stepCap = static_cast<std::uint32_t>(idom.size() * 2 + 4);
    auto intersect = [&](MirBlockId b1, MirBlockId b2) {
        MirBlockId finger1 = b1;
        MirBlockId finger2 = b2;
        std::uint32_t steps = 0;
        while (finger1.v != finger2.v) {
            if (++steps > stepCap) return MirBlockId{};
            if (rpoIndex[finger1.v] == static_cast<std::uint32_t>(-1)
             || rpoIndex[finger2.v] == static_cast<std::uint32_t>(-1)) {
                return MirBlockId{};
            }
            while (rpoIndex[finger1.v] > rpoIndex[finger2.v]) {
                MirBlockId const next = idom[finger1.v];
                if (!next.valid()
                 || rpoIndex[next.v] == static_cast<std::uint32_t>(-1)) {
                    return MirBlockId{};
                }
                finger1 = next;
                if (++steps > stepCap) return MirBlockId{};
            }
            while (rpoIndex[finger2.v] > rpoIndex[finger1.v]) {
                MirBlockId const next = idom[finger2.v];
                if (!next.valid()
                 || rpoIndex[next.v] == static_cast<std::uint32_t>(-1)) {
                    return MirBlockId{};
                }
                finger2 = next;
                if (++steps > stepCap) return MirBlockId{};
            }
        }
        return finger1;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 1; i < order.size(); ++i) {
            MirBlockId const b = order[i];
            MirBlockId newIdom{};
            for (MirBlockId const p : preds[b.v]) {
                if (rpoIndex[p.v] == static_cast<std::uint32_t>(-1)) continue;
                if (!idom[p.v].valid()) continue;
                if (!newIdom.valid()) {
                    newIdom = p;
                } else {
                    MirBlockId const interBlock = intersect(newIdom, p);
                    if (!interBlock.valid()) {
                        st.gaveUp[b.v] = true;
                        continue;
                    }
                    newIdom = interBlock;
                }
            }
            if (newIdom.valid() && idom[b.v].v != newIdom.v) {
                idom[b.v] = newIdom;
                changed = true;
            }
        }
    }
    return st;
}

// Does `a` dominate `b`? Tri-state — `GaveUp` on iteration-cap overflow.
[[nodiscard]] inline MirDomResult
mirDominatesBlock(MirBlockId a, MirBlockId b, MirDomTree const& dom) {
    if (!a.valid() || !b.valid()) return MirDomResult::DoesNot;
    if (a.v == b.v) return MirDomResult::Dominates;
    auto const& idom = dom.idom;
    if (b.v >= idom.size()) return MirDomResult::DoesNot;
    MirBlockId cur = b;
    std::uint32_t steps = 0;
    std::uint32_t const stepCap = static_cast<std::uint32_t>(idom.size() + 2);
    while (idom[cur.v].valid() && idom[cur.v].v != cur.v) {
        if (++steps > stepCap) return MirDomResult::GaveUp;
        cur = idom[cur.v];
        if (cur.v == a.v) return MirDomResult::Dominates;
    }
    return MirDomResult::DoesNot;
}

// Dominance frontier: for each block `b`, the set of blocks `n` such
// that `b` dominates a predecessor of `n` but does NOT strictly
// dominate `n` itself. Cooper-Harvey-Kennedy formulation — same paper
// as the idom algorithm. Used by Mem2Reg to determine where Phi
// nodes need to be inserted (per the Cytron-Ferrante SSA construction).
//
// Returns `df[b.v]` = vector of block ids in `b`'s frontier. Blocks
// not reachable in `dom` get an empty entry. Idempotent + O(E) total.
[[nodiscard]] inline std::vector<std::vector<MirBlockId>>
mirDominanceFrontier(Mir const& mir,
                     MirDomTree const& dom,
                     std::vector<std::vector<MirBlockId>> const& preds) {
    std::vector<std::vector<MirBlockId>> df(mir.blockCount());
    auto const& idom = dom.idom;
    for (std::uint32_t i = 1; i < mir.blockCount(); ++i) {
        // Skip blocks whose idom couldn't be resolved — the verifier
        // already maps these to I_VerifierFailure; computing a frontier
        // off an under-conservative idom would produce silently wrong
        // results (Mem2Reg would under-insert phis).
        if (i < dom.gaveUp.size() && dom.gaveUp[i]) continue;
        MirBlockId const b{i, mir.id().v};
        auto const& pb = preds[i];
        if (pb.size() < 2) continue;  // only join points contribute
        MirBlockId const bIdom = (i < idom.size()) ? idom[i] : MirBlockId{};
        if (!bIdom.valid()) continue;
        for (MirBlockId const p : pb) {
            MirBlockId runner = p;
            std::uint32_t steps = 0;
            std::uint32_t const stepCap =
                static_cast<std::uint32_t>(idom.size() * 2 + 4);
            while (runner.valid() && runner.v != bIdom.v) {
                if (++steps > stepCap) {
                    // Malformed idom chain — Mem2Reg / LICM consumers
                    // would silently under-report the frontier. The
                    // verifier should have caught this via the gaveUp
                    // flag; reaching here means a substrate-contract
                    // violation. Fail loud.
                    std::fprintf(stderr,
                        "dss::mirDominanceFrontier fatal: step-cap "
                        "exceeded walking from block #%u (predecessor "
                        "of #%u) up to idom #%u — malformed idom chain "
                        "(should have been gaveUp-flagged by "
                        "computeMirDomTree).\n",
                        p.v, b.v, bIdom.v);
                    std::abort();
                }
                df[runner.v].push_back(b);
                MirBlockId const next = idom[runner.v];
                if (!next.valid() || next.v == runner.v) break;
                runner = next;
            }
        }
    }
    return df;
}

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
[[nodiscard]] inline std::vector<std::vector<MirBlockId>>
mirDomTreeChildren(Mir const& mir, MirDomTree const& dom) {
    std::vector<std::vector<MirBlockId>> children(mir.blockCount());
    for (std::uint32_t i = 1; i < mir.blockCount(); ++i) {
        if (i < dom.gaveUp.size() && dom.gaveUp[i]) continue;
        if (i >= dom.idom.size()) continue;
        MirBlockId const parent = dom.idom[i];
        if (!parent.valid()) continue;
        if (parent.v == i) continue;  // skip the entry's self-loop
        MirBlockId const b{i, mir.id().v};
        children[parent.v].push_back(b);
    }
    return children;
}

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

[[nodiscard]] inline std::vector<MirNaturalLoop>
mirNaturalLoops(Mir const& mir,
                MirDomTree const& dom,
                std::vector<std::vector<MirBlockId>> const& preds) {
    // Group back-edges by header.
    std::unordered_map<std::uint32_t, std::vector<MirBlockId>> byHeader;
    for (std::uint32_t i = 1; i < mir.blockCount(); ++i) {
        if (i < dom.gaveUp.size() && dom.gaveUp[i]) continue;
        MirBlockId const u{i, mir.id().v};
        for (MirBlockId const s : mir.blockSuccessors(u)) {
            if (!s.valid() || s.v >= mir.blockCount()) continue;
            // Back-edge: successor dominates the source.
            if (mirDominatesBlock(s, u, dom) == MirDomResult::Dominates) {
                byHeader[s.v].push_back(u);
            }
        }
    }
    std::vector<MirNaturalLoop> loops;
    loops.reserve(byHeader.size());
    for (auto const& [headerSlot, backSources] : byHeader) {
        MirNaturalLoop loop;
        loop.header = MirBlockId{headerSlot, mir.id().v};
        loop.backEdgeSources = backSources;
        // Body = header + worklist over predecessors from each back-edge source.
        std::unordered_set<std::uint32_t> inBody;
        inBody.insert(headerSlot);
        std::vector<MirBlockId> worklist = backSources;
        for (MirBlockId const b : backSources) inBody.insert(b.v);
        while (!worklist.empty()) {
            MirBlockId const n = worklist.back();
            worklist.pop_back();
            if (n.v == headerSlot) continue;
            if (n.v >= preds.size()) continue;
            for (MirBlockId const p : preds[n.v]) {
                if (inBody.insert(p.v).second) {
                    worklist.push_back(p);
                }
            }
        }
        // INVARIANT: header is in the body set (seeded at line 1 of
        // the body-construction worklist). Verifying here catches a
        // future regression that forgets the seed (silent: LICM
        // would then hoist out of the header itself).
        if (inBody.count(headerSlot) == 0) {
            std::fprintf(stderr,
                "dss::mirNaturalLoops fatal: header block v=%u "
                "missing from body set — natural-loop construction "
                "invariant violation.\n", headerSlot);
            std::abort();
        }
        loop.body.reserve(inBody.size());
        for (std::uint32_t const slot : inBody) {
            loop.body.push_back(MirBlockId{slot, mir.id().v});
        }
        // Determinism: `unordered_set` iteration order is
        // implementation-defined. Sort by slot id so downstream
        // consumers (LICM hoist plan, future loop-rotation passes)
        // see a stable order regardless of stdlib version.
        std::sort(loop.body.begin(), loop.body.end(),
                  [](MirBlockId a, MirBlockId b) { return a.v < b.v; });
        loops.push_back(std::move(loop));
    }
    // Determinism: the byHeader map is unordered too. Sort the loop
    // list by header slot so iteration order is stable.
    std::sort(loops.begin(), loops.end(),
              [](MirNaturalLoop const& a, MirNaturalLoop const& b) {
                  return a.header.v < b.header.v;
              });
    return loops;
}

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
[[nodiscard]] inline std::vector<MirBlockId>
mirIteratedDominanceFrontier(
    std::vector<MirBlockId> const& defBlocks,
    std::vector<std::vector<MirBlockId>> const& df) {
    std::vector<MirBlockId> worklist(defBlocks.begin(), defBlocks.end());
    std::unordered_set<std::uint32_t> inIDF;
    std::unordered_set<std::uint32_t> onWorklist;
    for (MirBlockId const b : defBlocks) onWorklist.insert(b.v);
    std::vector<MirBlockId> idf;
    while (!worklist.empty()) {
        MirBlockId const b = worklist.back();
        worklist.pop_back();
        if (b.v >= df.size()) continue;
        for (MirBlockId const f : df[b.v]) {
            if (!inIDF.insert(f.v).second) continue;
            idf.push_back(f);
            if (onWorklist.insert(f.v).second) {
                worklist.push_back(f);
            }
        }
    }
    return idf;
}

} // namespace dss
