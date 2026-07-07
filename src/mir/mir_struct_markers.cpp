// Canonical StructCfMarker derivation — see mir_struct_markers.hpp for
// THE SPEC (priority order, first-claim-wins, dormant enum values, the
// placement principle). This TU is the single implementation every
// producer AND the verifier share; a derivation-rule change here moves
// producers and checker together, never one without the other.

#include "mir/mir_struct_markers.hpp"

#include "mir/mir_cfg.hpp"
#include "mir/mir_opcode.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include <vector>

namespace dss {

char const* structCfMarkerName(StructCfMarker m) noexcept {
    switch (m) {
        case StructCfMarker::Linear:     return "Linear";
        case StructCfMarker::EntryBlock: return "EntryBlock";
        case StructCfMarker::ExitBlock:  return "ExitBlock";
        case StructCfMarker::LoopHeader: return "LoopHeader";
        case StructCfMarker::LoopLatch:  return "LoopLatch";
        case StructCfMarker::LoopExit:   return "LoopExit";
        case StructCfMarker::IfThen:     return "IfThen";
        case StructCfMarker::IfElse:     return "IfElse";
        case StructCfMarker::IfJoin:     return "IfJoin";
        case StructCfMarker::SwitchHead: return "SwitchHead";
        case StructCfMarker::SwitchCase: return "SwitchCase";
        case StructCfMarker::SwitchJoin: return "SwitchJoin";
    }
    return "<unknown-marker>";
}

std::vector<StructCfMarker>
deriveStructCfMarkers(Mir const& mir, MirFuncId f,
                      std::vector<std::vector<MirBlockId>> const& preds,
                      std::vector<MirBlockId> const& rpo,
                      MirDomTree const& dom) {
    // Rule 6 is the initialization: everything not claimed below is
    // Linear (including unreachable blocks, other functions' blocks,
    // and the slot-0 sentinel).
    std::vector<StructCfMarker> out(mir.blockCount(), StructCfMarker::Linear);
    std::uint32_t const nb = mir.funcBlockCount(f);
    if (nb == 0) return out;

    std::unordered_set<std::uint32_t> reachable;
    reachable.reserve(rpo.size());
    for (MirBlockId const b : rpo) reachable.insert(b.v);

    // FIRST CLAIM WINS. No rule claims Linear, so "claimed" ≡ non-Linear
    // — the priority order doubles as the multi-role collision policy
    // (e.g. a LoopHeader that is also an if-join stays LoopHeader).
    auto claim = [&](std::uint32_t slot, StructCfMarker m) {
        if (slot >= out.size()) return;  // malformed edge — verifier owns it
        if (out[slot] == StructCfMarker::Linear) out[slot] = m;
    };

    // ── rule 1: function entry ──
    claim(mir.funcBlockAt(f, 0).v, StructCfMarker::EntryBlock);

    // ── rule 2: back-edge targets → LoopHeader ──
    // A block claims ITSELF when some REACHABLE predecessor is
    // dominated by it (the back-edge source). Unreachable preds are
    // excluded: a dead Br-to-header (e.g. a for-update block whose
    // body always returns) must not resurrect a loop that live code
    // never closes.
    for (MirBlockId const b : rpo) {
        if (b.v >= preds.size()) continue;
        for (MirBlockId const p : preds[b.v]) {
            if (!reachable.contains(p.v)) continue;
            if (mirDominatesBlock(b, p, dom) == MirDomResult::Dominates) {
                claim(b.v, StructCfMarker::LoopHeader);
                break;
            }
        }
    }

    // ── rule 3: loop-exiting-edge targets → LoopExit ──
    auto const loops = mirNaturalLoops(mir, dom, preds);
    for (MirNaturalLoop const& loop : loops) {
        std::unordered_set<std::uint32_t> inBody;
        inBody.reserve(loop.body.size());
        for (MirBlockId const b : loop.body) inBody.insert(b.v);
        for (MirBlockId const u : loop.body) {
            for (MirBlockId const s : mir.blockSuccessors(u)) {
                if (!s.valid() || s.v >= mir.blockCount()) continue;
                if (!inBody.contains(s.v)) claim(s.v, StructCfMarker::LoopExit);
            }
        }
    }

    // ── rules 4 + 5 need the post-dominator tree ──
    MirPostDomTree const postdom = computeMirPostDomTree(mir, f);
    auto ipdomSlotOf = [&](std::uint32_t hSlot) -> std::uint32_t {
        // Tri-state collapse for derivation purposes: INVALID (reverse-
        // unreachable) and gaveUp (malformed input — other verifier
        // rules own the diagnostic) both act as "no real join" = the
        // virtual slot.
        if (hSlot < postdom.gaveUp.size() && postdom.gaveUp[hSlot]) {
            return postdom.virtualExitSlot();
        }
        MirBlockId const j = postdom.ipdom[hSlot];
        if (!j.valid()) return postdom.virtualExitSlot();
        return j.v;
    };

    // ── rule 4: if-family (CondBr diamonds) ──
    for (std::uint32_t bi = 0; bi < nb; ++bi) {
        MirBlockId const h = mir.funcBlockAt(f, bi);
        if (!reachable.contains(h.v)) continue;
        if (mir.blockInstCount(h) == 0) continue;  // unterminated — verifier owns it
        if (mir.instOpcode(mir.blockTerminator(h)) != MirOpcode::CondBr) continue;
        // A loop-condition CondBr is loop vocabulary (rule 2 claimed the
        // block); its successors are the loop body + the rule-3 LoopExit,
        // not if-arms.
        if (out[h.v] == StructCfMarker::LoopHeader) continue;
        auto const succs = mir.blockSuccessors(h);
        if (succs.size() != 2) continue;  // malformed CondBr — verifier owns it
        std::uint32_t const j = ipdomSlotOf(h.v);
        if (succs[0].v != j) claim(succs[0].v, StructCfMarker::IfThen);
        if (succs[1].v != j) claim(succs[1].v, StructCfMarker::IfElse);
        if (j != postdom.virtualExitSlot()) claim(j, StructCfMarker::IfJoin);
    }

    // ── rule 5: switch-family ──
    for (std::uint32_t bi = 0; bi < nb; ++bi) {
        MirBlockId const h = mir.funcBlockAt(f, bi);
        if (!reachable.contains(h.v)) continue;
        if (mir.blockInstCount(h) == 0) continue;
        if (mir.instOpcode(mir.blockTerminator(h)) != MirOpcode::Switch) continue;
        std::uint32_t const j = ipdomSlotOf(h.v);
        for (MirBlockId const s : mir.blockSuccessors(h)) {
            if (!s.valid() || s.v >= mir.blockCount()) continue;
            if (s.v != j) claim(s.v, StructCfMarker::SwitchCase);
        }
        if (j != postdom.virtualExitSlot()) claim(j, StructCfMarker::SwitchJoin);
    }

    return out;
}

std::vector<StructCfMarker>
deriveStructCfMarkers(Mir const& mir, MirFuncId f) {
    if (mir.funcBlockCount(f) == 0) {
        return std::vector<StructCfMarker>(mir.blockCount(),
                                           StructCfMarker::Linear);
    }
    auto const preds = mirBuildPredecessors(mir);
    MirBlockId const entry = mir.funcEntry(f);
    auto const rpo = mirReversePostOrder(mir, entry);
    MirDomTree const dom = computeMirDomTree(mir, entry, rpo, preds);
    return deriveStructCfMarkers(mir, f, preds, rpo, dom);
}

namespace {

// Shared applier tail: stamp every block of `f` from `derived`.
void applyDerived(Mir& mir, MirFuncId f,
                  std::vector<StructCfMarker> const& derived) {
    std::uint32_t const nb = mir.funcBlockCount(f);
    for (std::uint32_t bi = 0; bi < nb; ++bi) {
        MirBlockId const b = mir.funcBlockAt(f, bi);
        mir.setBlockMarker(b, derived[b.v]);
    }
}

} // namespace

void rederiveStructCfMarkers(Mir& mir, MirFuncId f) {
    applyDerived(mir, f, deriveStructCfMarkers(mir, f));
}

void rederiveStructCfMarkers(Mir& mir) {
    static bool const trace = std::getenv("DSS_OPT_TRACE") != nullptr;
    auto const t0 = std::chrono::steady_clock::now();
    // One predecessor build for the whole module; per-function RPO/dom.
    auto const preds = mirBuildPredecessors(mir);
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        if (mir.funcBlockCount(f) == 0) continue;
        MirBlockId const entry = mir.funcEntry(f);
        auto const rpo = mirReversePostOrder(mir, entry);
        MirDomTree const dom = computeMirDomTree(mir, entry, rpo, preds);
        applyDerived(mir, f, deriveStructCfMarkers(mir, f, preds, rpo, dom));
    }
    if (trace) {
        auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "opt:   rederiveStructCfMarkers whole-module %lldms\n",
                     static_cast<long long>(ms));
    }
}

} // namespace dss
