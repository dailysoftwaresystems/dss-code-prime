#pragma once

// Shared MIR dominator-tree helpers — Cooper-Harvey-Kennedy iterative
// idom + tri-state dominates + dominance-frontier — consumed by the
// verifier (ML3 use-dom-def check) and by optimizer passes that need
// dominance information (D-OPT-DOMTREE-EXTRACTION).

#include "mir/mir.hpp"

#include <algorithm>
#include <cstdint>
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

} // namespace dss
