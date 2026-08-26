// Canonical StructCfMarker derivation — see mir_struct_markers.hpp for
// THE SPEC (priority order, first-claim-wins, dormant enum values, the
// placement principle). This TU is the single implementation every
// producer AND the verifier share; a derivation-rule change here moves
// producers and checker together, never one without the other.

#include "mir/mir_struct_markers.hpp"

#include "mir/mir_cfg.hpp"
#include "mir/mir_opcode.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
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

namespace {

// ⓘ `mirModuleSelfLoopBlocks` and `mirBackEdgeCandidates` USED TO LIVE HERE, in
// this anonymous namespace. They are now exported from `mir/mir_dom.hpp`, beside
// the scoped `mirNaturalLoops` completeness clause they exist to discharge —
// because the second caller that needed them (`opt::passes::runLicm`) could not
// reach them and silently kept the O(module)-per-function sweep
// ([[D-OPT-LICM-NATURAL-LOOPS-MODULE-WIDE-SCAN]]). Same bodies, moved verbatim.

// THE derivation. The public overloads below differ only in where the two
// reusable substrates come from: `moduleSelfLoops` (a module property) and
// `pdScratch` (the post-dominator scratch). A whole-module re-derivation
// hoists both out of its per-function loop; a one-off call owns them locally.
std::vector<StructCfMarker>
deriveInto(Mir const& mir, MirFuncId f,
           std::vector<std::vector<MirBlockId>> const& preds,
           std::vector<MirBlockId> const& rpo,
           MirDomTree const& dom,
           std::span<std::uint32_t const> moduleSelfLoops,
           MirPostDomScratch& pdScratch,
           std::vector<std::uint32_t>& candidateBuf) {
    // Rule 6 is the initialization: everything not claimed below is
    // Linear (including unreachable blocks, other functions' blocks,
    // and the slot-0 sentinel).
    //
    // Deliberately a FRESH module-sized vector per function, not a reused
    // buffer: `applyDerived` needs the whole thing, and at 86,411 blocks ×
    // 4,030 functions this fill measures 8ms of a 5,160ms whole-module
    // re-derivation (0.15%) because the buffer is L2-resident and the
    // allocator hands back the same block. A reuse mechanism here would buy
    // 8ms and cost a staleness invariant to get wrong.
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
    // The back-edge sweep is SCOPED to `f`'s own candidates (plus the module
    // self-loop index — see `mirBackEdgeCandidates`): `dom` is a
    // one-function tree, so the whole-module sweep was O(module) work per
    // function, i.e. quadratic in module size, and measured 2,101ms of a
    // 5,160ms whole-module re-derivation on merged SQLite.
    mirBackEdgeCandidates(mir, f, rpo, moduleSelfLoops, candidateBuf);
    auto const loops = mirNaturalLoops(
        mir, dom, preds, std::span<std::uint32_t const>{candidateBuf});
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
    // Scratch-backed (D-OPT-POSTDOM-SCRATCH-REUSE): the fresh-allocation
    // overload builds EIGHT module-sized buffers for one function's reverse
    // walk — 3,044ms of that same 5,160ms call.
    MirPostDomTree const& postdom = computeMirPostDomTree(mir, f, pdScratch);
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

std::vector<StructCfMarker>
deriveStructCfMarkers(Mir const& mir, MirFuncId f,
                      std::vector<std::vector<MirBlockId>> const& preds,
                      std::vector<MirBlockId> const& rpo,
                      MirDomTree const& dom) {
    // One-off call: it owns the two reusable substrates itself. Both are
    // O(module) to establish, which is what this overload already cost —
    // callers that derive EVERY function (the module-wide applier below)
    // hoist them instead.
    std::vector<std::uint32_t> selfLoops;
    if (mir.funcBlockCount(f) != 0) mirModuleSelfLoopBlocks(mir, selfLoops);
    MirPostDomScratch pdScratch;
    std::vector<std::uint32_t> candidateBuf;
    return deriveInto(mir, f, preds, rpo, dom, selfLoops, pdScratch,
                      candidateBuf);
}

std::vector<StructCfMarker>
deriveStructCfMarkers(Mir const& mir, MirFuncId f,
                      std::vector<std::vector<MirBlockId>> const& preds,
                      std::vector<MirBlockId> const& rpo,
                      MirDomTree const& dom,
                      MirStructCfScratch& scratch) {
    std::uint32_t const bc = static_cast<std::uint32_t>(mir.blockCount());
    if (scratch.blockCount == 0) {          // first use — bind + fill the index
        scratch.moduleIdV  = mir.id().v;
        scratch.blockCount = bc;
        mirModuleSelfLoopBlocks(mir, scratch.moduleSelfLoops);
    } else if (scratch.moduleIdV != mir.id().v || scratch.blockCount != bc) {
        // The self-loop index is a property of the module it was swept over.
        // Serving it for a different module would be a WRONG loop forest, not a
        // slow one — so this is a fail-loud, exactly as MirDomScratch's and
        // MirPostDomScratch's stale-module guards are.
        std::fprintf(stderr,
            "dss::deriveStructCfMarkers fatal: MirStructCfScratch bound to "
            "module id=%u blockCount=%u was reused for module id=%u "
            "blockCount=%u — one bundle per (sweep x module).\n",
            scratch.moduleIdV, scratch.blockCount, mir.id().v, bc);
        std::abort();
    }
    return deriveInto(mir, f, preds, rpo, dom,
                      std::span<std::uint32_t const>{scratch.moduleSelfLoops},
                      scratch.postDom, scratch.candidates);
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

void rederiveStructCfMarkers(Mir& mir, MirFuncId f) {
    applyDerived(mir, f, deriveStructCfMarkers(mir, f));
}

void rederiveStructCfMarkers(Mir& mir) {
    static bool const trace = std::getenv("DSS_OPT_TRACE") != nullptr;
    auto const t0 = std::chrono::steady_clock::now();
    // EVERY whole-module substrate is established ONCE, outside the loop, and
    // every per-function step is O(function). Before this, three separate
    // O(module) computations ran once PER FUNCTION — quadratic in module size:
    // the predecessor map (already hoisted, D-OPT-DOMTREE-SCRATCH-REUSE), the
    // natural-loop back-edge sweep, and the post-dominator tree.
    auto const preds = mirBuildPredecessors(mir);
    MirDomScratch domScratch;
    // ONE bundle for the whole sweep — the same object the verifier now uses, so
    // "which substrates are hoisted" has a single definition rather than one
    // copy per caller ([[D-PERF-VERIFIER-REESTABLISHES-MODULE-SUBSTRATES-PER-FUNCTION]]).
    MirStructCfScratch cfScratch;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        if (mir.funcBlockCount(f) == 0) continue;
        MirBlockId const entry = mir.funcEntry(f);
        auto const rpo = mirReversePostOrder(mir, entry);
        MirDomTree const& dom =
            computeMirDomTree(mir, entry, rpo, preds, domScratch);
        applyDerived(mir, f,
                     deriveStructCfMarkers(mir, f, preds, rpo, dom, cfScratch));
    }
    if (trace) {
        auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "opt:   rederiveStructCfMarkers whole-module %lldms\n",
                     static_cast<long long>(ms));
    }
}

} // namespace dss
