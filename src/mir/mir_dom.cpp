// MIR dominator-tree helpers (D-OPT-MIR-DOM-HPP-CPP-SPLIT closure,
// cycle 10e). Bodies extracted from `mir_dom.hpp` so the header carries
// only declarations + struct/enum layouts. See `mir_dom.hpp` for the
// per-function docblocks and algorithmic rationale; this TU is the
// implementation surface only.

#include "mir/mir_dom.hpp"

#include "mir/mir_cfg.hpp"   // mirReversePostOrder (forward-reachability for the exit set)
#include "mir/mir_opcode.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dss {

namespace {

// ── the shared Cooper-Harvey-Kennedy core ────────────────────────────────────
//
// ONE graph-pure implementation parameterized by (nodeCount, entrySlot,
// order-of-slots, preds-of-slots) — both the forward tree
// (`computeMirDomTree`) and the reverse tree (`computeMirPostDomTree`)
// call it; the core never touches `Mir`. Slots are raw u32 node indices
// (`kUnsetSlot` = unresolved); the callers adapt to/from `MirBlockId`.
//
// Contract: `order[0]` must be `entrySlot` (the iteration skips index 0
// — the root's idom is its own self-loop, seeded here). Nodes not in
// `order` are unreachable from the root and keep `kUnsetSlot`.
constexpr std::uint32_t kUnsetSlot = static_cast<std::uint32_t>(-1);

struct ChkResult {
    std::vector<std::uint32_t> idom;    // slot → idom slot; kUnsetSlot = unresolved
    std::vector<std::uint8_t>  gaveUp;
};

ChkResult runChkCore(std::size_t nodeCount, std::uint32_t entrySlot,
                     std::vector<std::uint32_t> const& order,
                     std::vector<std::vector<std::uint32_t>> const& preds) {
    ChkResult st;
    st.idom.assign(nodeCount, kUnsetSlot);
    st.gaveUp.assign(nodeCount, 0);
    auto& idom = st.idom;
    std::vector<std::uint32_t> rpoIndex(nodeCount, kUnsetSlot);
    for (std::uint32_t i = 0; i < order.size(); ++i) {
        rpoIndex[order[i]] = i;
    }
    if (entrySlot >= nodeCount) return st;
    idom[entrySlot] = entrySlot;
    std::uint32_t const stepCap = static_cast<std::uint32_t>(idom.size() * 2 + 4);
    auto intersect = [&](std::uint32_t b1, std::uint32_t b2) {
        std::uint32_t finger1 = b1;
        std::uint32_t finger2 = b2;
        std::uint32_t steps = 0;
        while (finger1 != finger2) {
            if (++steps > stepCap) return kUnsetSlot;
            if (rpoIndex[finger1] == kUnsetSlot
             || rpoIndex[finger2] == kUnsetSlot) {
                return kUnsetSlot;
            }
            while (rpoIndex[finger1] > rpoIndex[finger2]) {
                std::uint32_t const next = idom[finger1];
                if (next == kUnsetSlot || rpoIndex[next] == kUnsetSlot) {
                    return kUnsetSlot;
                }
                finger1 = next;
                if (++steps > stepCap) return kUnsetSlot;
            }
            while (rpoIndex[finger2] > rpoIndex[finger1]) {
                std::uint32_t const next = idom[finger2];
                if (next == kUnsetSlot || rpoIndex[next] == kUnsetSlot) {
                    return kUnsetSlot;
                }
                finger2 = next;
                if (++steps > stepCap) return kUnsetSlot;
            }
        }
        return finger1;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 1; i < order.size(); ++i) {
            std::uint32_t const b = order[i];
            std::uint32_t newIdom = kUnsetSlot;
            for (std::uint32_t const p : preds[b]) {
                if (rpoIndex[p] == kUnsetSlot) continue;
                if (idom[p] == kUnsetSlot) continue;
                if (newIdom == kUnsetSlot) {
                    newIdom = p;
                } else {
                    std::uint32_t const interSlot = intersect(newIdom, p);
                    if (interSlot == kUnsetSlot) {
                        st.gaveUp[b] = true;
                        continue;
                    }
                    newIdom = interSlot;
                }
            }
            if (newIdom != kUnsetSlot && idom[b] != newIdom) {
                idom[b] = newIdom;
                changed = true;
            }
        }
    }
    return st;
}

} // namespace

std::vector<std::vector<MirBlockId>>
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

MirDomTree
computeMirDomTree(Mir const&                                  mir,
                  MirBlockId                                  entry,
                  std::vector<MirBlockId> const&              order,
                  std::vector<std::vector<MirBlockId>> const& preds) {
    MirDomTree st;
    st.idom.resize(mir.blockCount());
    st.gaveUp.resize(mir.blockCount(), false);
    if (!entry.valid()) return st;
    // Adapt the strong-id graph to the slot-parameterized shared core.
    std::vector<std::uint32_t> orderSlots;
    orderSlots.reserve(order.size());
    for (MirBlockId const b : order) orderSlots.push_back(b.v);
    std::vector<std::vector<std::uint32_t>> predSlots(mir.blockCount());
    for (std::size_t i = 0; i < preds.size() && i < predSlots.size(); ++i) {
        predSlots[i].reserve(preds[i].size());
        for (MirBlockId const p : preds[i]) predSlots[i].push_back(p.v);
    }
    ChkResult core = runChkCore(mir.blockCount(), entry.v, orderSlots, predSlots);
    st.gaveUp = std::move(core.gaveUp);
    for (std::size_t i = 0; i < core.idom.size(); ++i) {
        st.idom[i] = (core.idom[i] == kUnsetSlot)
            ? MirBlockId{}
            : MirBlockId{core.idom[i], mir.id().v};
    }
    return st;
}

MirPostDomTree
computeMirPostDomTree(Mir const& mir, MirFuncId f) {
    std::size_t const n = mir.blockCount();
    std::uint32_t const virtualSlot = static_cast<std::uint32_t>(n);
    MirPostDomTree st;
    st.virtualExit = virtualSlot;
    st.ipdom.assign(n + 1, MirBlockId{});
    st.gaveUp.assign(n + 1, 0);
    if (mir.funcBlockCount(f) == 0) return st;

    // Forward-reachability gates the exit set: an UNREACHABLE
    // Return-terminated block (dead code) must not define the join
    // structure of live code.
    MirBlockId const entry = mir.funcEntry(f);
    auto const rpo = mirReversePostOrder(mir, entry);
    std::unordered_set<std::uint32_t> reachable;
    reachable.reserve(rpo.size());
    for (MirBlockId const b : rpo) reachable.insert(b.v);

    // Reverse graph over slots [0, n]; slot n is the virtual exit.
    //   reverse-preds(b)  = forward successors of b (+ virtual for exits)
    //   reverse-succs(b)  = forward predecessors of b
    //   reverse-succs(virtual) = the exit blocks (reverse-graph root edges)
    std::vector<std::vector<std::uint32_t>> revPreds(n + 1);
    std::vector<std::vector<std::uint32_t>> revSuccs(n + 1);
    std::uint32_t const nb = mir.funcBlockCount(f);
    for (std::uint32_t bi = 0; bi < nb; ++bi) {
        MirBlockId const b = mir.funcBlockAt(f, bi);
        for (MirBlockId const s : mir.blockSuccessors(b)) {
            if (!s.valid() || s.v >= n) continue;  // verifier owns the diagnostic
            revPreds[b.v].push_back(s.v);
            revSuccs[s.v].push_back(b.v);
        }
        if (!reachable.contains(b.v)) continue;
        std::uint32_t const ni = mir.blockInstCount(b);
        if (ni == 0) continue;  // unterminated — verifier owns the diagnostic
        MirOpcode const term = mir.instOpcode(mir.blockInstAt(b, ni - 1));
        if (term == MirOpcode::Return || term == MirOpcode::Unreachable) {
            revPreds[b.v].push_back(virtualSlot);
            revSuccs[virtualSlot].push_back(b.v);
        }
    }

    // Reverse-RPO from the virtual exit (iterative post-order, reversed
    // — the same traversal shape as mirReversePostOrder, over revSuccs).
    std::vector<std::uint32_t> order;
    {
        std::vector<std::uint8_t> visited(n + 1, 0);
        struct Frame { std::uint32_t slot; std::size_t nextSucc; };
        std::vector<Frame> stack;
        visited[virtualSlot] = 1;
        stack.push_back({virtualSlot, 0});
        while (!stack.empty()) {
            Frame& top = stack.back();
            auto const& succs = revSuccs[top.slot];
            if (top.nextSucc < succs.size()) {
                std::uint32_t const s = succs[top.nextSucc++];
                if (!visited[s]) {
                    visited[s] = 1;
                    stack.push_back({s, 0});
                }
            } else {
                order.push_back(top.slot);
                stack.pop_back();
            }
        }
        std::reverse(order.begin(), order.end());
    }

    ChkResult core = runChkCore(n + 1, virtualSlot, order, revPreds);
    st.gaveUp = std::move(core.gaveUp);
    for (std::size_t i = 0; i <= n; ++i) {
        // Three-valued mapping (see MirPostDomTree): kUnsetSlot →
        // INVALID (reverse-unreachable: no path to any exit); the
        // virtual slot maps to a SYNTHETIC id callers must compare,
        // never dereference.
        st.ipdom[i] = (core.idom[i] == kUnsetSlot)
            ? MirBlockId{}
            : MirBlockId{core.idom[i], mir.id().v};
    }
    return st;
}

MirDomResult
mirPostDominatesBlock(MirBlockId a, MirBlockId b, MirPostDomTree const& postdom) {
    if (!a.valid() || !b.valid()) return MirDomResult::DoesNot;
    if (a.v == b.v) return MirDomResult::Dominates;
    auto const& ipdom = postdom.ipdom;
    if (b.v >= ipdom.size()) return MirDomResult::DoesNot;
    MirBlockId cur = b;
    std::uint32_t steps = 0;
    std::uint32_t const stepCap = static_cast<std::uint32_t>(ipdom.size() + 2);
    while (ipdom[cur.v].valid() && ipdom[cur.v].v != cur.v) {
        if (++steps > stepCap) return MirDomResult::GaveUp;
        cur = ipdom[cur.v];
        if (cur.v == a.v) return MirDomResult::Dominates;
    }
    return MirDomResult::DoesNot;
}

MirDomResult
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

std::vector<std::vector<MirBlockId>>
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

std::vector<std::vector<MirBlockId>>
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

std::vector<MirNaturalLoop>
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
                // SSA-soundness gate: a `gaveUp` block has
                // unresolved dominance — admitting it into the loop
                // body could let LICM hoist through it, producing a
                // def in the preheader that no longer dominates its
                // use in the gaveUp block. Filter at the worklist
                // step so the body computation stays sound; the
                // verifier should have flagged the gaveUp block
                // upstream, but this is a defense-in-depth measure
                // against a verifier-gating regression
                // (D-OPT6-LICM-GAVEUP-BODY-FILTER).
                if (p.v < dom.gaveUp.size() && dom.gaveUp[p.v]) continue;
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
        // INVARIANT: no `gaveUp` block is in the body set. The
        // worklist filter above gates predecessor admission; this
        // post-condition catches the case where a back-edge SOURCE
        // is itself in the seed set despite being gaveUp (e.g. a
        // future seed-expansion that bypasses the back-edge
        // dominance check). LICM's SSA-soundness depends on this.
        for (std::uint32_t const slot : inBody) {
            if (slot < dom.gaveUp.size() && dom.gaveUp[slot]) {
                std::fprintf(stderr,
                    "dss::mirNaturalLoops fatal: body slot v=%u is "
                    "gaveUp — dominance is unsound; LICM hoisting "
                    "through this block would violate def-dominates-"
                    "use (D-OPT6-LICM-GAVEUP-BODY-FILTER).\n", slot);
                std::abort();
            }
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

std::vector<MirBlockId>
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
