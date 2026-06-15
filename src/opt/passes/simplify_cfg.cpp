#include "opt/passes/simplify_cfg.hpp"

#include "mir/mir_cfg.hpp"
#include "mir/mir_dom.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"
#include "opt/passes/mir_rebuild_helper.hpp"
#include "opt/passes/path_compress.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dss::opt::passes {

namespace {

class SimplifyCfgPolicy final : public MirRebuildPolicy {
public:
    explicit SimplifyCfgPolicy(Mir const& src) : src_(src) {}

    [[nodiscard]] std::size_t branchesFolded() const noexcept {
        return branchesFolded_;
    }
    [[nodiscard]] std::size_t blocksJumpThreaded() const noexcept {
        return blocksJumpThreaded_;
    }
    [[nodiscard]] std::size_t blocksMerged() const noexcept {
        return blocksMerged_;
    }

    void analyze(MirFuncId fn);

    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        // Walk reachable-from-entry blocks in RPO order, excluding
        // those flagged for elision by `analyze()`. THREE elision
        // categories share this filter:
        //   * trampoline blocks (keys of `jumpThreadMap_`);
        //   * absorbed-into-predecessor blocks (keys of
        //     `absorbedToHead_` — the merge "tail" of each P→B pair);
        //   * POST-FOLD-UNREACHABLE blocks (not in `postFoldReachable_`)
        //     — the dead arms a branch-fold disconnected, plus their
        //     exclusive subtrees. Branch-folding `CondBr(b,T,F)` → `Br(T)`
        //     severs the b→F edge in the OUTPUT; F (and any block reached
        //     ONLY through it) is no longer reachable from entry. Emitting
        //     it would leave an orphan island the per-pass MirVerifier
        //     rejects (I_UnreachableBlock) BEFORE a later Dce could prune
        //     it — so SimplifyCfg prunes the arms its OWN folds disconnect,
        //     in the SAME rebuild, by construction.
        // `mirReversePostOrder` walks the SOURCE CFG (it does not know the
        // fold), so the `postFoldReachable_` filter is what removes the
        // dead arm; the source-RPO is only the iteration order.
        auto const rpo = mirReversePostOrder(src, src.funcEntry(fn));
        std::vector<MirBlockId> out;
        out.reserve(rpo.size());
        for (MirBlockId const b : rpo) {
            if (jumpThreadMap_.count(b) != 0) continue;
            // Absorbed blocks are keys of `absorbedToHead_` — single
            // source of truth for the merge elision set.
            if (absorbedToHead_.count(b) != 0) continue;
            // Post-fold-unreachable blocks (dead arms + their exclusive
            // subtrees) vanish in this rebuild.
            if (postFoldReachable_.count(b) == 0) continue;
            out.push_back(b);
        }
        return out;
    }

    [[nodiscard]] MirBlockId redirectBlockTarget(MirBlockId oldTarget) override {
        // Chase to a surviving block. Two redirect categories:
        //   (a) trampoline → its successor (jumpThreadMap_).
        //   (b) absorbed → its absorb head (absorbedToHead_).
        // Both maps are path-compressed so a single lookup suffices.
        // An absorbed block's id is no longer valid in the rebuilt
        // module; predecessors-as-phi-incomings flow through this
        // hook (mir_rebuild_helper phase 3 calls it on `inc.pred`).
        if (auto it = jumpThreadMap_.find(oldTarget);
            it != jumpThreadMap_.end()) {
            return it->second;
        }
        if (auto it = absorbedToHead_.find(oldTarget);
            it != absorbedToHead_.end()) {
            return it->second;
        }
        return oldTarget;
    }

    [[nodiscard]] std::optional<MirBlockId>
    absorbSuccessor(MirBlockId oldB) override {
        auto it = absorbMap_.find(oldB);
        if (it == absorbMap_.end()) return std::nullopt;
        return it->second;
    }

    // No `onBlockBegin` marker repair: markers are NOT maintained
    // through the rebuild — `runSimplifyCfg` re-stamps every block
    // from the canonical CFG derivation (`rederiveStructCfMarkers`)
    // after `finish()`, which subsumed the cycle-9 "non-Linear wins"
    // absorb-chain walk and its ≤1-non-Linear-per-chain admission
    // gate (D-OPT4-1-NON-LINEAR-MARKER-MERGE closure).

    // Branch-fold phi hygiene — TWO composed drops:
    //
    // (1) The dead-EDGE case (cycle B): folding `CondBr(b, T, F)` →
    //     `Br(taken)` removes the b → abandoned CFG edge while b itself
    //     STAYS LIVE, so a phi in the abandoned target naming b as a pred
    //     would go stale (I_PhiPredNotInCfg — the unreachable-pred
    //     cleanup never fires for a live pred). Drop exactly those
    //     (pred == b, owner == abandoned) incomings, keyed by EDGE.
    //
    // (2) The dead-PRED case (C2): a phi-bearing block can survive the
    //     fold (it stays post-fold-reachable) while ONE of its preds
    //     becomes post-fold-UNREACHABLE (the pred lived only on a folded-
    //     away arm). That pred is elided from `selectBlocks`, so its phi
    //     incoming must be dropped too — accept only incomings whose pred
    //     is post-fold-reachable. COMPLETENESS (the rebuilder phase-3
    //     abort, mir_rebuild_helper.cpp ~185): an accepted incoming's
    //     redirected pred MUST be in blockMap. A reachable pred is either
    //     kept verbatim (in blockMap), or absorbed → `redirectBlockTarget`
    //     sends it to its (reachable, non-absorbed) chain head (in
    //     blockMap), or a trampoline → its phi-free target (and a
    //     trampoline is never a phi's pred by the jump-thread gate). So
    //     "reachable pred" is exactly the accept set the redirect can
    //     complete. A reachable phi-block always keeps >= 1 incoming
    //     (a phi-block with ALL preds unreachable would itself be
    //     unreachable, hence excluded from selectBlocks) — so this never
    //     starves a surviving phi to zero incomings.
    [[nodiscard]] bool acceptPhiIncoming(
        MirPhiIncoming const& inc, MirBlockId oldPhiBlock,
        std::unordered_map<std::uint32_t, MirBlockId> const& /*blockMap*/) override {
        if (abandonedPhiEdges_.find(edgeKey_(inc.pred.v, oldPhiBlock.v))
            != abandonedPhiEdges_.end()) {
            return false;  // (1) dead EDGE
        }
        if (postFoldReachable_.count(inc.pred) == 0) {
            return false;  // (2) dead PRED
        }
        return true;
    }

    [[nodiscard]] std::optional<MirInstId>
    tryRewriteTerminator(MirOpcode op, MirInstId oldId, MirBuilder& dst,
                         std::unordered_map<std::uint32_t, MirInstId> const& /*rewrite*/,
                         std::unordered_map<std::uint32_t, MirBlockId> const& blockMap) override {
        if (op != MirOpcode::CondBr) return std::nullopt;
        auto it = foldedBranches_.find(oldId);
        if (it == foldedBranches_.end()) return std::nullopt;
        // `it->second` is the OLD-id of the target block (already
        // path-compressed through `jumpThreadMap_` at analysis time).
        // Resolve to NEW via blockMap.
        auto const blkIt = blockMap.find(it->second.v);
        if (blkIt == blockMap.end()) {
            std::fprintf(stderr,
                "dss::opt::passes::SimplifyCfg fatal: branch-fold target "
                "block v=%u for terminator v=%u not in blockMap — "
                "analysis-tier invariant violation (target must be a "
                "surviving reachable block).\n",
                it->second.v, oldId.v);
            std::abort();
        }
        ++branchesFolded_;
        return dst.addBr(blkIt->second);
    }

    void resetPerFunction() {
        foldedBranches_.clear();
        jumpThreadMap_.clear();
        absorbMap_.clear();
        absorbedToHead_.clear();
        abandonedPhiEdges_.clear();
        postFoldReachable_.clear();
    }

    // Branch-folding emits a Br whose new id is structurally
    // unrelated to the source CondBr's id. Recording the mapping
    // in `rewrite_` would let a future consumer that looks up the
    // CondBr's id (terminators have no SSA result today — but the
    // map has no type discipline against it) silently retrieve a
    // Br id with wrong shape. Following DCE's precedent.
    [[nodiscard]] bool recordTerminatorInRewrite() const noexcept override {
        return false;
    }

private:
    Mir const& src_;

    // Per-function analysis state. Reset per fn; counters live.
    // CondBr inst id → folded target block (old id, already
    // jump-threaded through the trampoline chain).
    std::unordered_map<MirInstId, MirBlockId>  foldedBranches_;
    // Trampoline-block id → its ultimate surviving target. Path-
    // compressed so the rebuilder's redirect chase is O(1). This
    // map's KEY-SET is also the "elided block" set (single source
    // of truth — `selectBlocks` queries it directly).
    std::unordered_map<MirBlockId, MirBlockId> jumpThreadMap_;
    // Block-merge state (D-OPT5-BLOCK-MERGE):
    //   - `absorbMap_[P] = B`: P's terminator (Br(B)) is dropped;
    //     B's insts get inlined after P's; B's terminator becomes
    //     the merged block's. Walked by `absorbSuccessor`.
    //   - `absorbedToHead_[B] = H`: reverse map for phi-incoming
    //     redirection. An incoming-from-absorbed-B redirects to H
    //     (the ultimate chain head). Path-compressed so chained
    //     merges resolve directly to the head. The KEY-SET of this
    //     map IS the "absorbed block" elision set (single source of
    //     truth — `selectBlocks` queries it directly).
    // Markers play NO role in merge admission: the post-rebuild
    // `rederiveStructCfMarkers` re-stamps every block from the CFG
    // (D-OPT4-1-NON-LINEAR-MARKER-MERGE closed — the c3-era ≤1-non-
    // Linear-per-chain bookkeeping and its admission gate are gone).
    std::unordered_map<MirBlockId, MirBlockId> absorbMap_;
    std::unordered_map<MirBlockId, MirBlockId> absorbedToHead_;
    // CFG edges (foldingBlock → abandonedTarget) removed by branch-
    // folding, keyed (pred << 32 | owner). `acceptPhiIncoming` drops a
    // phi incoming that travels a dead edge (see the override's doc).
    std::unordered_set<std::uint64_t> abandonedPhiEdges_;
    // Blocks reachable from entry over POST-FOLD effective successors
    // (C2): a folded block's only effective successor is its taken
    // target; every other block keeps its source successors. Computed
    // after `foldedBranches_` is populated. `selectBlocks` filters to
    // this set (dead arms + exclusive subtrees vanish) and
    // `acceptPhiIncoming` drops incomings from preds outside it. Absorbed
    // and jump-threaded blocks that are still reachable REMAIN in this set
    // (their own elision is owned by `absorbedToHead_` / `jumpThreadMap_`;
    // this set is purely the post-fold reachability fact).
    std::unordered_set<MirBlockId> postFoldReachable_;

    [[nodiscard]] static std::uint64_t edgeKey_(std::uint32_t pred,
                                                std::uint32_t owner) noexcept {
        return (static_cast<std::uint64_t>(pred) << 32) | owner;
    }

    std::size_t branchesFolded_     = 0;
    std::size_t blocksJumpThreaded_ = 0;
    std::size_t blocksMerged_       = 0;
};

void SimplifyCfgPolicy::analyze(MirFuncId fn) {
    resetPerFunction();
    MirBlockId const entry = src_.funcEntry(fn);
    auto const rpo = mirReversePostOrder(src_, entry);
    if (rpo.empty()) return;

    // Step 1: identify trampoline blocks. A block B is a trampoline iff:
    //   - It is NOT the function entry (we cannot elide the entry).
    //   - Its instruction count is exactly 1 (the terminator).
    //   - The terminator is a Br.
    //   - The Br's target S has no Phi nodes (conservative — avoids
    //     phi-incoming-from-B fan-out which requires reflowing every
    //     incoming through B's full predecessor set).
    auto hasAnyPhi = [&](MirBlockId b) {
        std::uint32_t const n = src_.blockInstCount(b);
        for (std::uint32_t i = 0; i < n; ++i) {
            if (src_.instOpcode(src_.blockInstAt(b, i)) == MirOpcode::Phi) {
                return true;
            }
        }
        return false;
    };
    for (MirBlockId const b : rpo) {
        if (b.v == entry.v) continue;
        if (src_.blockInstCount(b) != 1) continue;
        MirInstId const term = src_.blockInstAt(b, 0);
        if (src_.instOpcode(term) != MirOpcode::Br) continue;
        auto const succs = src_.blockSuccessors(b);
        if (succs.size() != 1) continue;  // defense — Br has exactly 1
        MirBlockId const tgt = succs[0];
        if (hasAnyPhi(tgt)) continue;
        // Also skip if the target is the trampoline itself (would
        // mean an infinite Br-to-self loop; verifier should reject
        // but defense in depth).
        if (tgt.v == b.v) continue;
        jumpThreadMap_[b] = tgt;
    }
    // Path-compress the trampoline chain B1 → B2 → ... → S so every
    // entry maps directly to a non-trampoline target. Fail loud if
    // the post-compression invariant breaks (target still a key) —
    // cycle in jump-thread chain is a substrate violation.
    pathCompressAndVerify(jumpThreadMap_, "SimplifyCfg");
    blocksJumpThreaded_ += jumpThreadMap_.size();

    // Step 1b: identify block-merge candidates (D-OPT5-BLOCK-MERGE).
    // A (P, B) pair is mergeable iff:
    //   - P's terminator is Br (1 successor, that successor = B).
    //   - B has exactly 1 predecessor (P).
    //   - B is not the entry block.
    //   - B has no Phi nodes (a Phi with 1 incoming is degenerate;
    //     CopyProp's Phi-collapse should have eliminated it
    //     already, but defending here keeps the merge sound).
    //   - B's instCount > 1 (a 1-inst Br-only B is the trampoline
    //     case handled by `jumpThreadMap_` above — block-merge
    //     defers to that).
    //   - Neither P nor B is in `jumpThreadMap_` (they'd be elided
    //     anyway; block-merge is exclusive with jump-thread elision).
    // Markers are NOT a gate: both-non-Linear (P, B) pairs merge —
    // the post-rebuild canonical re-derivation stamps the merged
    // block's actual structural role (D-OPT4-1-NON-LINEAR-MARKER-
    // MERGE closed). The remaining conditions are pure CFG-legality.
    auto const preds = mirBuildPredecessors(src_);
    auto isCandidateForMerge = [&](MirBlockId P, MirBlockId B) -> bool {
        if (B.v == entry.v) return false;
        if (jumpThreadMap_.count(P) || jumpThreadMap_.count(B)) return false;
        if (B.v >= preds.size() || preds[B.v].size() != 1) return false;
        if (preds[B.v][0].v != P.v) return false;
        std::uint32_t const bCount = src_.blockInstCount(B);
        if (bCount <= 1) return false;  // empty/trampoline — handled elsewhere
        // Phi gate.
        for (std::uint32_t i = 0; i < bCount; ++i) {
            if (src_.instOpcode(src_.blockInstAt(B, i)) == MirOpcode::Phi) {
                return false;
            }
        }
        return true;
    };
    // Process pairs in RPO order, incrementally admitting them into
    // absorbMap_ + absorbedToHead_. RPO order admits the outer-most
    // pair first; each subsequent admission extends an existing chain
    // or starts a new one. (The c3-era per-chain non-Linear marker
    // bookkeeping is gone — the post-rebuild canonical re-derivation
    // owns markers; admission is pure CFG-legality.)
    for (MirBlockId const P : rpo) {
        std::uint32_t const n = src_.blockInstCount(P);
        if (n == 0) continue;
        MirInstId const term = src_.blockInstAt(P, n - 1);
        if (src_.instOpcode(term) != MirOpcode::Br) continue;
        auto const succs = src_.blockSuccessors(P);
        if (succs.size() != 1) continue;
        MirBlockId const B = succs[0];
        if (!isCandidateForMerge(P, B)) continue;
        // P already absorbed → would re-bind; B already absorbed →
        // would create a two-predecessor situation (impossible by
        // 1-pred gate, but defense in depth).
        if (absorbMap_.count(P) || absorbedToHead_.count(B)) continue;
        // Find the chain head H: if P is itself absorbed (an earlier
        // RPO iteration admitted some (X, P) pair), the head is the
        // root of P's chain. Otherwise P is its own head.
        MirBlockId head = P;
        if (auto it = absorbedToHead_.find(P); it != absorbedToHead_.end()) {
            head = it->second;
        }
        // Admit.
        absorbMap_[P] = B;
        absorbedToHead_[B] = head;
    }
    // Disjointness invariant: a block elided by trampoline jump-thread
    // must NOT also appear as an absorbed block — the two redirect
    // categories must be exclusive so `redirectBlockTarget`'s lookup
    // order doesn't silently miscompile a collision.
    for (auto const& [B, _] : absorbedToHead_) {
        if (jumpThreadMap_.count(B) != 0) {
            std::fprintf(stderr,
                "dss::opt::passes::SimplifyCfg fatal: block v=%u is "
                "BOTH a trampoline (in jumpThreadMap_) AND an absorbed "
                "block (in absorbedToHead_) — exclusion-invariant "
                "violation. The analyze() gates must keep these "
                "categories disjoint.\n", B.v);
            std::abort();
        }
    }
    // Counter records the BLOCK COUNT eliminated by merge (the
    // count-by-blocks framing). Each absorbed block appears in
    // exactly one pair as the tail.
    blocksMerged_ += absorbedToHead_.size();
    // Inversion invariant (type-design FOLD-NOW from 2nd-pass
    // review): absorbMap_.values() must equal absorbedToHead_.keys()
    // and the two maps must have the same size. A future maintainer
    // who adds to one map but forgets the other would silently
    // miscompile (selectBlocks would emit an "absorbed" block, or
    // the rebuilder would chase an absorb chain into a dead key).
    if (absorbMap_.size() != absorbedToHead_.size()) {
        std::fprintf(stderr,
            "dss::opt::passes::SimplifyCfg fatal: absorbMap_ size %zu "
            "≠ absorbedToHead_ size %zu — inversion-invariant "
            "violation (every absorbed block in one map must appear "
            "in the other).\n",
            absorbMap_.size(), absorbedToHead_.size());
        std::abort();
    }
    for (auto const& [P, B] : absorbMap_) {
        if (absorbedToHead_.count(B) != 1) {
            std::fprintf(stderr,
                "dss::opt::passes::SimplifyCfg fatal: absorbMap_ "
                "entry P v=%u → B v=%u, but B is not in "
                "absorbedToHead_ — inversion-invariant violation.\n",
                P.v, B.v);
            std::abort();
        }
    }

    // Step 2: identify branch-foldable terminators. A CondBr is
    // foldable iff its condition resolves (via the source MIR) to a
    // Bool Const. After ConstFold + CopyProp + CSE upstream, a
    // condition that simplifies to a literal is the dominant case.
    // Branch-fold targets reuse the same redirect logic as the
    // rebuilder's emit path: chase through `jumpThreadMap_` to the
    // chain tail (which is in the surviving block-map by construction).
    for (MirBlockId const b : rpo) {
        if (jumpThreadMap_.count(b)) continue;
        std::uint32_t const n = src_.blockInstCount(b);
        if (n == 0) continue;
        MirInstId const term = src_.blockInstAt(b, n - 1);
        if (src_.instOpcode(term) != MirOpcode::CondBr) continue;
        auto const ops = src_.instOperands(term);
        if (ops.empty()) continue;
        MirInstId const cond = ops[0];
        if (src_.instOpcode(cond) != MirOpcode::Const) continue;
        auto const lit = src_.literalValue(src_.constLiteralIndex(cond));
        if (lit.core != TypeKind::Bool) continue;
        // Bool literal is stored as int64 0 / 1 in the I64 arm.
        auto const* asInt = std::get_if<std::int64_t>(&lit.value);
        if (asInt == nullptr) continue;
        bool const taken = (*asInt != 0);
        // `blockSuccessors` returns CondBr arms in (ifTrue, ifFalse)
        // order — the Mir::blockSuccessors contract ("CondBr →
        // [ifTrue, ifFalse]", mir.hpp). Reversing this contract
        // would silently flip every branch-fold; the
        // `CondBrTrueFoldsToThenArm` + `CondBrFalseFoldsToElseArm`
        // pair pins both polarities at the test layer.
        auto const succs = src_.blockSuccessors(b);
        if (succs.size() != 2) continue;
        MirBlockId const rawTgt = taken ? succs[0] : succs[1];
        // Phi hygiene: the NOT-taken raw arm's CFG edge (b → abandoned)
        // dies with the fold; record it so `acceptPhiIncoming` drops
        // the abandoned target's incoming-from-b (b stays live — the
        // unreachable-pred cleanup can never catch this). A CondBr
        // whose both arms name the SAME block keeps its single edge
        // alive → nothing to record. The RAW target is the right key:
        // a jump-threaded abandoned arm's FINAL target has no phis
        // (the trampoline gate requires a phi-free target).
        MirBlockId const rawAbandoned = taken ? succs[1] : succs[0];
        if (rawAbandoned.v != rawTgt.v) {
            abandonedPhiEdges_.insert(edgeKey_(b.v, rawAbandoned.v));
        }
        // Chase jump-thread but NOT absorb: an absorbed block has
        // exactly 1 pred (its absorb head) by the merge gate, and
        // that pred is the head itself — no third block can have a
        // CondBr arm targeting the absorbed block (CondBr branches
        // are distinct from the absorb-head's straight-line Br
        // chain). So `rawTgt` is never in `absorbedToHead_` when
        // reached via a CondBr arm; only `jumpThreadMap_` resolution
        // is needed here.
        auto const it = jumpThreadMap_.find(rawTgt);
        MirBlockId const tgt = (it == jumpThreadMap_.end()) ? rawTgt : it->second;
        foldedBranches_[term] = tgt;
    }

    // Step 3: post-fold reachability (C2). DFS from entry over EFFECTIVE
    // successors: a block whose terminator was folded has its taken
    // target as its ONLY successor (the abandoned arm is severed); every
    // other block keeps its source `blockSuccessors`. The reachable set
    // is the blocks `selectBlocks` keeps (minus the trampoline / absorb
    // elisions) — dead arms + their exclusive subtrees are excluded by
    // construction, so the rebuilt function has no orphan island for the
    // per-pass MirVerifier to reject (I_UnreachableBlock). The fold
    // target stored in `foldedBranches_` is already jump-thread-
    // compressed, so the DFS reaches the surviving chain tail directly.
    {
        std::vector<MirBlockId> stack;
        stack.push_back(entry);
        postFoldReachable_.insert(entry);
        while (!stack.empty()) {
            MirBlockId const b = stack.back();
            stack.pop_back();
            std::uint32_t const n = src_.blockInstCount(b);
            // Effective successors. A folded terminator collapses to its
            // single taken target; otherwise the source successors stand.
            auto pushSucc = [&](MirBlockId s) {
                if (postFoldReachable_.insert(s).second) stack.push_back(s);
            };
            bool folded = false;
            if (n != 0) {
                MirInstId const term = src_.blockInstAt(b, n - 1);
                if (auto const fit = foldedBranches_.find(term);
                    fit != foldedBranches_.end()) {
                    pushSucc(fit->second);
                    folded = true;
                }
            }
            if (!folded) {
                for (MirBlockId const s : src_.blockSuccessors(b)) pushSucc(s);
            }
        }
    }
}

} // namespace

SimplifyCfgResult runSimplifyCfg(Mir& mir, TypeInterner const& /*interner*/,
                                 DiagnosticReporter& reporter) {
    SimplifyCfgResult result{};
    MirBuilder builder;

    if (cloneGlobalsOrCarveOut(mir, builder, reporter, "SimplifyCfg")
        == GlobalClonePrelude::CarvedOut) {
        result.ok = true;
        return result;
    }

    SimplifyCfgPolicy policy{mir};
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        policy.analyze(f);
        MirFunctionRebuilder rb{mir, builder, policy};
        rb.rebuildFunction(f);
    }

    result.branchesFolded     = policy.branchesFolded();
    result.blocksJumpThreaded = policy.blocksJumpThreaded();
    result.blocksMerged       = policy.blocksMerged();
    mir = std::move(builder).finish();
    // Canonical-marker stamping (D-OPT4-1): SimplifyCfg mutates the CFG
    // (fold/thread/merge), so every surviving block's structural role is
    // re-derived from the NEW shape — no incremental marker repair.
    rederiveStructCfMarkers(mir);
    result.ok = true;
    return result;
}

} // namespace dss::opt::passes
