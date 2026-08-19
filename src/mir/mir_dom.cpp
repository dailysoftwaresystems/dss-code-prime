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

// Slot projection for the templated core: the forward-dominator path passes
// the caller's `preds` (MirBlockId elements) DIRECTLY — the former per-call
// whole-module `predSlots` copy was ~95% of CSE/LICM's cost on SQLite
// (D-OPT-DOMTREE-SCRATCH-REUSE) — while the post-dominator path passes its
// per-function uint32 reverse graph. ONE core, two element shapes.
[[nodiscard]] inline std::uint32_t chkSlotOf(std::uint32_t s) noexcept { return s; }
[[nodiscard]] inline std::uint32_t chkSlotOf(MirBlockId b) noexcept { return b.v; }

// The CHK fixpoint over CALLER-PROVIDED arrays (sized `nodeCount`, pre-set to
// kUnsetSlot/kUnsetSlot/0 for every slot this call may touch — fresh
// allocation and the scratch's touched-slot reset both satisfy that). The
// arrays MUST stay module-sized: the intersect step-cap derives from
// `idom.size()`, so compressing to function-local sizing would change when
// pathological chains give up (a behavior change). Writes touch ONLY
// `order ∪ {entry}` — the invariant the scratch's partial reset relies on.
template <typename OrderVec, typename PredsVec>
void runChkCoreInto(std::size_t nodeCount, std::uint32_t entrySlot,
                    OrderVec const& order, PredsVec const& preds,
                    std::vector<std::uint32_t>& idom,
                    std::vector<std::uint8_t>&  gaveUp,
                    std::vector<std::uint32_t>& rpoIndex) {
    for (std::uint32_t i = 0; i < order.size(); ++i) {
        rpoIndex[chkSlotOf(order[i])] = i;
    }
    if (entrySlot >= nodeCount) return;
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
            std::uint32_t const b = chkSlotOf(order[i]);
            std::uint32_t newIdom = kUnsetSlot;
            // Legacy size-tolerance preserved: the former predSlots copy was
            // sized nodeCount with a `i < preds.size()` copy guard, so a
            // too-small preds map read as "no predecessors" — keep that
            // byte-identical (the scratch overload separately fail-louds a
            // size mismatch at its entry, where it is a caller bug).
            if (b < preds.size()) {
                for (auto const pe : preds[b]) {
                    std::uint32_t const p = chkSlotOf(pe);
                    if (rpoIndex[p] == kUnsetSlot) continue;
                    if (idom[p] == kUnsetSlot) continue;
                    if (newIdom == kUnsetSlot) {
                        newIdom = p;
                    } else {
                        std::uint32_t const interSlot = intersect(newIdom, p);
                        if (interSlot == kUnsetSlot) {
                            gaveUp[b] = true;
                            continue;
                        }
                        newIdom = interSlot;
                    }
                }
            }
            if (newIdom != kUnsetSlot && idom[b] != newIdom) {
                idom[b] = newIdom;
                changed = true;
            }
        }
    }
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
    // Fresh whole-module core arrays; the templated core reads the caller's
    // `preds`/`order` directly (the former predSlots/orderSlots re-copies are
    // gone — same values, same iteration order).
    std::vector<std::uint32_t> coreIdom(mir.blockCount(), kUnsetSlot);
    std::vector<std::uint8_t>  coreGaveUp(mir.blockCount(), 0);
    std::vector<std::uint32_t> rpoIndex(mir.blockCount(), kUnsetSlot);
    runChkCoreInto(mir.blockCount(), entry.v, order, preds,
                   coreIdom, coreGaveUp, rpoIndex);
    for (std::size_t i = 0; i < coreGaveUp.size(); ++i) {
        st.gaveUp[i] = coreGaveUp[i];
    }
    for (std::size_t i = 0; i < coreIdom.size(); ++i) {
        st.idom[i] = (coreIdom[i] == kUnsetSlot)
            ? MirBlockId{}
            : MirBlockId{coreIdom[i], mir.id().v};
    }
    return st;
}

MirDomTree const&
computeMirDomTree(Mir const&                                  mir,
                  MirBlockId                                  entry,
                  std::vector<MirBlockId> const&              order,
                  std::vector<std::vector<MirBlockId>> const& preds,
                  MirDomScratch&                              scratch) {
    // Bind-or-verify (the MirMemoryClobbers stale-guard pattern): the
    // optimizer mints a fresh MirModuleId per rebuild, so a scratch reused
    // across a rebuild fails loud here instead of silently mixing modules.
    std::uint32_t const bc = static_cast<std::uint32_t>(mir.blockCount());
    if (scratch.blockCount == 0 && scratch.moduleIdV == 0) {
        scratch.moduleIdV  = mir.id().v;
        scratch.blockCount = bc;
        scratch.coreIdom.assign(bc, kUnsetSlot);
        scratch.coreGaveUp.assign(bc, 0);
        scratch.rpoIndex.assign(bc, kUnsetSlot);
        scratch.tree.idom.assign(bc, MirBlockId{});
        scratch.tree.gaveUp.assign(bc, false);
        scratch.children.assign(bc, {});
    } else if (scratch.moduleIdV != mir.id().v || scratch.blockCount != bc) {
        std::fprintf(stderr,
            "dss::computeMirDomTree fatal: MirDomScratch bound to module "
            "id=%u blocks=%u used with module id=%u blocks=%u — stale scratch "
            "across a rebuild (D-OPT-DOMTREE-SCRATCH-REUSE contract).\n",
            scratch.moduleIdV, scratch.blockCount, mir.id().v, bc);
        std::abort();
    }
    if (preds.size() != mir.blockCount()) {
        std::fprintf(stderr,
            "dss::computeMirDomTree fatal: preds.size()=%zu != "
            "mir.blockCount()=%zu — the scratch overload requires the pass's "
            "hoisted whole-module predecessor map.\n",
            preds.size(), mir.blockCount());
        std::abort();
    }

    // Reset-at-entry from the PREVIOUS call's self-recorded write set — the
    // caller's `order` from that call may be long gone (dangling), which is
    // why the touched list is copied into the scratch, never re-derived.
    for (std::uint32_t const slot : scratch.touched) {
        scratch.coreIdom[slot]   = kUnsetSlot;
        scratch.coreGaveUp[slot] = 0;
        scratch.rpoIndex[slot]   = kUnsetSlot;
        scratch.tree.idom[slot]  = MirBlockId{};
        scratch.tree.gaveUp[slot] = false;
        scratch.children[slot].clear();   // parents ⊆ touched (idom values ∈ order)
    }
    scratch.touched.clear();

    // Record THIS call's write set: order ∪ {entry} (the proven-complete
    // write set of the core + the conversion below). The entry slot is
    // bounds-guarded — an out-of-range entry is a reachable silent input the
    // core's own early-return tolerates (never written, never recorded).
    scratch.touched.reserve(order.size() + 1);
    for (MirBlockId const b : order) scratch.touched.push_back(b.v);
    if (entry.valid() && entry.v < bc) scratch.touched.push_back(entry.v);

    if (entry.valid()) {
        runChkCoreInto(mir.blockCount(), entry.v, order, preds,
                       scratch.coreIdom, scratch.coreGaveUp, scratch.rpoIndex);
        // Conversion restricted to the write set — untouched slots already
        // hold the fresh-allocation defaults (MirBlockId{} / false) by reset.
        for (std::uint32_t const slot : scratch.touched) {
            scratch.tree.gaveUp[slot] = scratch.coreGaveUp[slot];
            scratch.tree.idom[slot] =
                (scratch.coreIdom[slot] == kUnsetSlot)
                    ? MirBlockId{}
                    : MirBlockId{scratch.coreIdom[slot], mir.id().v};
        }
    }

    // Ascending-sorted UNIQUE copy for the children fill: the fresh
    // mirDomTreeChildren iterates slots ascending and visits each ONCE — the
    // touched list may hold the entry slot twice (order[0] == entry), so
    // dedup here or the fill would emit duplicate children. The CSE dom-DFS
    // traversal order depends on this order — keep it identical.
    scratch.touchedSorted.assign(scratch.touched.begin(), scratch.touched.end());
    std::sort(scratch.touchedSorted.begin(), scratch.touchedSorted.end());
    scratch.touchedSorted.erase(
        std::unique(scratch.touchedSorted.begin(), scratch.touchedSorted.end()),
        scratch.touchedSorted.end());
    scratch.childrenFilled = false;
    return scratch.tree;
}

namespace {

// Every buffer the reverse-graph CHK walk needs, held BY REFERENCE so the
// fresh-allocation path can point at locals and the scratch path at its own
// retained storage. ONE implementation writes through this — that is what
// makes the two public overloads byte-identical by construction rather than
// by two bodies kept in sync by hand.
//
// Entry contract (both paths satisfy it — fresh by allocation, scratch by the
// touched-slot reset): every array is sized `blockCount + 1` and holds the
// fresh-allocation default in EVERY slot this call may touch — revPreds /
// revSuccs empty, visited 0, coreIdom / rpoIndex kUnsetSlot, coreGaveUp 0,
// tree.ipdom MirBlockId{}, tree.gaveUp 0. `order`, `touchedRev` and
// `touchedNodes` are cleared here.
struct PostDomBuffers {
    std::vector<std::vector<std::uint32_t>>& revPreds;
    std::vector<std::vector<std::uint32_t>>& revSuccs;
    std::vector<std::uint8_t>&               visited;
    std::vector<std::uint32_t>&              order;
    std::vector<std::uint32_t>&              coreIdom;
    std::vector<std::uint8_t>&               coreGaveUp;
    std::vector<std::uint32_t>&              rpoIndex;
    MirPostDomTree&                          tree;
    // Self-recorded write sets, so the NEXT call on a scratch can restore the
    // defaults by touching only what this call dirtied.
    std::vector<std::uint32_t>&              touchedRev;
    std::vector<std::uint32_t>&              touchedNodes;
};

void computePostDomInto(Mir const& mir, MirFuncId f, PostDomBuffers b) {
    std::size_t const n = mir.blockCount();
    std::uint32_t const virtualSlot = static_cast<std::uint32_t>(n);
    b.order.clear();
    b.touchedRev.clear();
    b.touchedNodes.clear();
    b.tree.virtualExit = virtualSlot;
    if (mir.funcBlockCount(f) == 0) return;

    // Forward-reachability gates the exit set: an UNREACHABLE
    // Return-terminated block (dead code) must not define the join
    // structure of live code.
    MirBlockId const entry = mir.funcEntry(f);
    auto const rpo = mirReversePostOrder(mir, entry);
    std::unordered_set<std::uint32_t> reachable;
    reachable.reserve(rpo.size());
    for (MirBlockId const bb : rpo) reachable.insert(bb.v);

    // Reverse graph over slots [0, n]; slot n is the virtual exit.
    //   reverse-preds(b)  = forward successors of b (+ virtual for exits)
    //   reverse-succs(b)  = forward predecessors of b
    //   reverse-succs(virtual) = the exit blocks (reverse-graph root edges)
    auto pushRevPred = [&](std::uint32_t slot, std::uint32_t v) {
        if (b.revPreds[slot].empty()) b.touchedRev.push_back(slot);
        b.revPreds[slot].push_back(v);
    };
    auto pushRevSucc = [&](std::uint32_t slot, std::uint32_t v) {
        if (b.revSuccs[slot].empty()) b.touchedRev.push_back(slot);
        b.revSuccs[slot].push_back(v);
    };
    std::uint32_t const nb = mir.funcBlockCount(f);
    for (std::uint32_t bi = 0; bi < nb; ++bi) {
        MirBlockId const blk = mir.funcBlockAt(f, bi);
        for (MirBlockId const s : mir.blockSuccessors(blk)) {
            if (!s.valid() || s.v >= n) continue;  // verifier owns the diagnostic
            pushRevPred(blk.v, s.v);
            pushRevSucc(s.v, blk.v);
        }
        if (!reachable.contains(blk.v)) continue;
        std::uint32_t const ni = mir.blockInstCount(blk);
        if (ni == 0) continue;  // unterminated — verifier owns the diagnostic
        MirOpcode const term = mir.instOpcode(mir.blockInstAt(blk, ni - 1));
        if (term == MirOpcode::Return || term == MirOpcode::Unreachable) {
            pushRevPred(blk.v, virtualSlot);
            pushRevSucc(virtualSlot, blk.v);
        }
    }

    // Reverse-RPO from the virtual exit (iterative post-order, reversed
    // — the same traversal shape as mirReversePostOrder, over revSuccs).
    {
        struct Frame { std::uint32_t slot; std::size_t nextSucc; };
        std::vector<Frame> stack;
        b.visited[virtualSlot] = 1;
        stack.push_back({virtualSlot, 0});
        while (!stack.empty()) {
            Frame& top = stack.back();
            auto const& succs = b.revSuccs[top.slot];
            if (top.nextSucc < succs.size()) {
                std::uint32_t const s = succs[top.nextSucc++];
                if (!b.visited[s]) {
                    b.visited[s] = 1;
                    stack.push_back({s, 0});
                }
            } else {
                b.order.push_back(top.slot);
                stack.pop_back();
            }
        }
        std::reverse(b.order.begin(), b.order.end());
    }
    // `order` IS the visited set, the CHK core's write set (the core writes
    // only `order ∪ {entry}`, and entry == virtualSlot == order[0]) and the
    // result-conversion write set below — one list covers all four.
    b.touchedNodes.assign(b.order.begin(), b.order.end());

    // Same templated core as the forward tree (uint32 slot elements here).
    runChkCoreInto(n + 1, virtualSlot, b.order, b.revPreds,
                   b.coreIdom, b.coreGaveUp, b.rpoIndex);
    for (std::uint32_t const slot : b.touchedNodes) {
        b.tree.gaveUp[slot] = b.coreGaveUp[slot];
        // Three-valued mapping (see MirPostDomTree): kUnsetSlot →
        // INVALID (reverse-unreachable: no path to any exit); the
        // virtual slot maps to a SYNTHETIC id callers must compare,
        // never dereference. Slots outside the write set keep the
        // MirBlockId{} / 0 defaults, which is exactly what the
        // kUnsetSlot arm would have produced for them.
        b.tree.ipdom[slot] = (b.coreIdom[slot] == kUnsetSlot)
            ? MirBlockId{}
            : MirBlockId{b.coreIdom[slot], mir.id().v};
    }
}

} // namespace

MirPostDomTree
computeMirPostDomTree(Mir const& mir, MirFuncId f) {
    std::size_t const n = mir.blockCount();
    MirPostDomTree st;
    st.virtualExit = static_cast<std::uint32_t>(n);
    st.ipdom.assign(n + 1, MirBlockId{});
    st.gaveUp.assign(n + 1, 0);
    // Fresh whole-module buffers for ONE function's walk — the cost the
    // scratch overload exists to remove (D-OPT-POSTDOM-SCRATCH-REUSE).
    std::vector<std::vector<std::uint32_t>> revPreds(n + 1);
    std::vector<std::vector<std::uint32_t>> revSuccs(n + 1);
    std::vector<std::uint8_t>  visited(n + 1, 0);
    std::vector<std::uint32_t> order;
    std::vector<std::uint32_t> coreIdom(n + 1, kUnsetSlot);
    std::vector<std::uint8_t>  coreGaveUp(n + 1, 0);
    std::vector<std::uint32_t> rpoIndex(n + 1, kUnsetSlot);
    std::vector<std::uint32_t> touchedRev;
    std::vector<std::uint32_t> touchedNodes;
    computePostDomInto(mir, f, PostDomBuffers{revPreds, revSuccs, visited,
                                              order, coreIdom, coreGaveUp,
                                              rpoIndex, st, touchedRev,
                                              touchedNodes});
    return st;
}

MirPostDomTree const&
computeMirPostDomTree(Mir const& mir, MirFuncId f, MirPostDomScratch& scratch) {
    // Bind-or-verify (the MirDomScratch / MirMemoryClobbers stale-guard
    // pattern): the optimizer mints a fresh MirModuleId per rebuild, so a
    // scratch reused across a rebuild fails loud here instead of silently
    // mixing modules.
    std::uint32_t const bc = static_cast<std::uint32_t>(mir.blockCount());
    std::size_t const n1 = static_cast<std::size_t>(bc) + 1;
    if (scratch.blockCount == 0 && scratch.moduleIdV == 0) {
        scratch.moduleIdV  = mir.id().v;
        scratch.blockCount = bc;
        scratch.revPreds.assign(n1, {});
        scratch.revSuccs.assign(n1, {});
        scratch.visited.assign(n1, 0);
        scratch.coreIdom.assign(n1, kUnsetSlot);
        scratch.coreGaveUp.assign(n1, 0);
        scratch.rpoIndex.assign(n1, kUnsetSlot);
        scratch.tree.ipdom.assign(n1, MirBlockId{});
        scratch.tree.gaveUp.assign(n1, 0);
    } else if (scratch.moduleIdV != mir.id().v || scratch.blockCount != bc) {
        std::fprintf(stderr,
            "dss::computeMirPostDomTree fatal: MirPostDomScratch bound to "
            "module id=%u blocks=%u used with module id=%u blocks=%u — stale "
            "scratch across a rebuild (D-OPT-POSTDOM-SCRATCH-REUSE "
            "contract).\n",
            scratch.moduleIdV, scratch.blockCount, mir.id().v, bc);
        std::abort();
    }

    // Reset-at-entry from the PREVIOUS call's self-recorded write sets,
    // restoring exactly the fresh-allocation defaults. Two lists because the
    // adjacency write set (successor slots of the previous function's blocks)
    // and the reverse-RPO order are different sets.
    for (std::uint32_t const slot : scratch.touchedRev) {
        scratch.revPreds[slot].clear();
        scratch.revSuccs[slot].clear();
    }
    for (std::uint32_t const slot : scratch.touchedNodes) {
        scratch.visited[slot]         = 0;
        scratch.coreIdom[slot]        = kUnsetSlot;
        scratch.coreGaveUp[slot]      = 0;
        scratch.rpoIndex[slot]        = kUnsetSlot;
        scratch.tree.ipdom[slot]      = MirBlockId{};
        scratch.tree.gaveUp[slot]     = 0;
    }
    scratch.touchedRev.clear();
    scratch.touchedNodes.clear();

    computePostDomInto(mir, f, PostDomBuffers{
        scratch.revPreds, scratch.revSuccs, scratch.visited, scratch.order,
        scratch.coreIdom, scratch.coreGaveUp, scratch.rpoIndex, scratch.tree,
        scratch.touchedRev, scratch.touchedNodes});
    return scratch.tree;
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

std::vector<std::vector<MirBlockId>> const&
mirDomTreeChildren(Mir const& mir, MirDomTree const& dom,
                   MirDomScratch& scratch) {
    // The touched bookkeeping is what makes the partial children reset
    // complete — this overload is only sound for the tree the SAME scratch's
    // compute call just produced. Fail loud on any other tree.
    if (&dom != &scratch.tree) {
        std::fprintf(stderr,
            "dss::mirDomTreeChildren fatal: scratch-children called with a "
            "tree that is NOT this scratch's own (the touched-slot reset "
            "contract only covers scratch.tree — "
            "D-OPT-DOMTREE-SCRATCH-REUSE).\n");
        std::abort();
    }
    // Idempotent per compute call (the fresh overload returns identical
    // content on repeat calls; refilling here would duplicate entries).
    if (scratch.childrenFilled) return scratch.children;
    scratch.childrenFilled = true;
    // Ascending-slot iteration over the touched set — the same order and the
    // same per-slot body as the fresh overload's `i = 1..blockCount` scan
    // (untouched slots contribute nothing there: their idom is invalid).
    // Contributing parents are inside the touched set (every stored idom
    // value is in `order`), so the reset cleared exactly the lists this
    // fill repopulates.
    for (std::uint32_t const i : scratch.touchedSorted) {
        if (i < 1u) continue;   // parity: the fresh loop starts at slot 1
        if (i < dom.gaveUp.size() && dom.gaveUp[i]) continue;
        if (i >= dom.idom.size()) continue;
        MirBlockId const parent = dom.idom[i];
        if (!parent.valid()) continue;
        if (parent.v == i) continue;  // skip the entry's self-loop
        MirBlockId const b{i, mir.id().v};
        scratch.children[parent.v].push_back(b);
    }
    return scratch.children;
}

namespace {

// The natural-loop forest over a caller-chosen back-edge SOURCE sweep. ONE
// implementation; the two public overloads differ only in `forEachCandidate`
// (every module slot vs. a caller-supplied ascending list) — see the scoped
// overload's docblock in mir_dom.hpp for why a scoped sweep can be complete.
template <typename ForEachCandidate>
std::vector<MirNaturalLoop>
naturalLoopsCore(Mir const& mir,
                 MirDomTree const& dom,
                 std::vector<std::vector<MirBlockId>> const& preds,
                 ForEachCandidate&& forEachCandidate) {
    // Group back-edges by header.
    std::unordered_map<std::uint32_t, std::vector<MirBlockId>> byHeader;
    forEachCandidate([&](std::uint32_t i) {
        if (i < dom.gaveUp.size() && dom.gaveUp[i]) return;
        MirBlockId const u{i, mir.id().v};
        for (MirBlockId const s : mir.blockSuccessors(u)) {
            if (!s.valid() || s.v >= mir.blockCount()) continue;
            // Back-edge: successor dominates the source.
            if (mirDominatesBlock(s, u, dom) == MirDomResult::Dominates) {
                byHeader[s.v].push_back(u);
            }
        }
    });
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

} // namespace

std::vector<MirNaturalLoop>
mirNaturalLoops(Mir const& mir,
                MirDomTree const& dom,
                std::vector<std::vector<MirBlockId>> const& preds) {
    return naturalLoopsCore(mir, dom, preds, [&](auto&& visit) {
        for (std::uint32_t i = 1; i < mir.blockCount(); ++i) visit(i);
    });
}

std::vector<MirNaturalLoop>
mirNaturalLoops(Mir const& mir,
                MirDomTree const& dom,
                std::vector<std::vector<MirBlockId>> const& preds,
                std::span<std::uint32_t const> candidateSources) {
    // Ascending + unique + in-range is the contract that makes this overload
    // byte-identical to the whole-module sweep (it fixes `backEdgeSources`
    // order). A violation is a caller bug, never a tolerable input.
    std::uint32_t const bc = static_cast<std::uint32_t>(mir.blockCount());
    for (std::size_t k = 0; k < candidateSources.size(); ++k) {
        std::uint32_t const s = candidateSources[k];
        if (s < 1u || s >= bc) {
            std::fprintf(stderr,
                "dss::mirNaturalLoops fatal: candidateSources[%zu] = %u is "
                "outside [1, blockCount=%u) — the scoped overload's contract "
                "(D-OPT-NATURAL-LOOPS-MODULE-WIDE-SCAN).\n", k, s, bc);
            std::abort();
        }
        if (k > 0 && s <= candidateSources[k - 1]) {
            std::fprintf(stderr,
                "dss::mirNaturalLoops fatal: candidateSources is not strictly "
                "ascending at index %zu (%u after %u) — back-edge source order "
                "would diverge from the whole-module sweep.\n",
                k, s, candidateSources[k - 1]);
            std::abort();
        }
    }
    return naturalLoopsCore(mir, dom, preds, [&](auto&& visit) {
        for (std::uint32_t const s : candidateSources) visit(s);
    });
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
