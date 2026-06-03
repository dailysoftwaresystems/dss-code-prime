#include "opt/passes/simplify_cfg.hpp"

#include "mir/mir_cfg.hpp"
#include "mir/mir_dom.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/passes/mir_rebuild_helper.hpp"
#include "opt/passes/path_compress.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <unordered_map>
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
        // those flagged for elision by `analyze()`. Two elision
        // categories share this filter: trampoline blocks (keys of
        // `jumpThreadMap_`) and absorbed-into-predecessor blocks
        // (values of `absorbMap_` — the merge "tail" of each
        // P→B pair; P stays, B vanishes). Both maps are the single
        // sources of truth for their respective elision sets.
        auto const rpo = mirReversePostOrder(src, src.funcEntry(fn));
        std::vector<MirBlockId> out;
        out.reserve(rpo.size());
        for (MirBlockId const b : rpo) {
            if (jumpThreadMap_.count(b) != 0) continue;
            // Absorbed blocks are keys of `absorbedToHead_` — single
            // source of truth for the merge elision set.
            if (absorbedToHead_.count(b) != 0) continue;
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

    // Block-merge marker re-derivation: when a merge head's source
    // marker is Linear but the absorb chain includes a non-Linear
    // marker, override the rebuilt block's marker so the structural
    // role is preserved. The c3 gate restricts merges to "at least
    // one side is Linear" so at most ONE non-Linear marker exists in
    // any absorb chain — pick it. (Both-non-Linear merges are
    // anchored as D-OPT4-1-NON-LINEAR-MARKER-MERGE for a future
    // cycle where the marker semantics require a CFG-shape-driven
    // re-derivation rather than the simple "non-Linear wins" rule.)
    void onBlockBegin(MirBlockId oldB, MirBlockId newB,
                      MirBuilder& dst,
                      std::unordered_map<std::uint32_t, MirInstId>& /*rewrite*/,
                      std::unordered_map<std::uint32_t, MirBlockId> const& /*blockMap*/) override {
        StructCfMarker const headMarker = src_.blockMarker(oldB);
        if (headMarker != StructCfMarker::Linear) return;  // already non-Linear
        // Walk the absorb chain to find a non-Linear marker.
        MirBlockId cur = oldB;
        std::uint32_t cap = static_cast<std::uint32_t>(absorbMap_.size()) + 1;
        while (cap-- > 0) {
            auto it = absorbMap_.find(cur);
            if (it == absorbMap_.end()) return;
            cur = it->second;
            StructCfMarker const cm = src_.blockMarker(cur);
            if (cm != StructCfMarker::Linear) {
                dst.setBlockMarker(newB, cm);
                return;
            }
        }
        // Cap exhausted = cycle in absorb chain. Should be structurally
        // impossible (the 1-pred-of-tail + non-entry-of-tail gates
        // prevent P→B→P shapes; unreachable cycles are excluded from
        // RPO before analyze). Fail loud so a future regression that
        // admits a cycle surfaces here rather than silently returning.
        std::fprintf(stderr,
            "dss::opt::passes::SimplifyCfg fatal: onBlockBegin absorb-"
            "chain walk exceeded cap from oldB v=%u — cycle in "
            "absorbMap_ (substrate-contract violation).\n", oldB.v);
        std::abort();
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
        chainNonLinearMarker_.clear();
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
    //   - `chainNonLinearMarker_[H]`: per-chain-head record of "this
    //     chain already contains a non-Linear marker." Used to
    //     reject admission of a pair whose addition would push the
    //     chain past the conservative "≤1 non-Linear per chain"
    //     invariant (the marker re-derivation rule that c3 ships).
    //     Multiple non-Linear markers in one chain anchor as
    //     D-OPT4-1-NON-LINEAR-MARKER-MERGE for full CFG-shape-driven
    //     marker re-derivation.
    std::unordered_map<MirBlockId, MirBlockId> absorbMap_;
    std::unordered_map<MirBlockId, MirBlockId> absorbedToHead_;
    std::unordered_map<MirBlockId, StructCfMarker> chainNonLinearMarker_;

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
    //   - At least one of {P.marker, B.marker} is Linear (conservative
    //     marker re-derivation; non-Linear-on-both is anchored as
    //     D-OPT4-1-NON-LINEAR-MARKER-MERGE pending a CFG-shape-driven
    //     marker repair).
    //   - Neither P nor B is in `jumpThreadMap_` (they'd be elided
    //     anyway; block-merge is exclusive with jump-thread elision).
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
        // Marker gate (c3 conservative).
        StructCfMarker const pm = src_.blockMarker(P);
        StructCfMarker const bm = src_.blockMarker(B);
        if (pm != StructCfMarker::Linear && bm != StructCfMarker::Linear) {
            return false;
        }
        return true;
    };
    // Process pairs in RPO order, incrementally admitting them into
    // absorbMap_ + absorbedToHead_. This lets us track the
    // per-chain non-Linear marker count + reject any pair whose
    // admission would push a chain past the "≤1 non-Linear per
    // chain" invariant the c3 marker re-derivation depends on.
    // RPO order admits the outer-most pair first; each subsequent
    // admission extends an existing chain or starts a new one.
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
        // Chain non-Linear invariant check. If the chain already has
        // a non-Linear marker AND B's marker is also non-Linear, this
        // admission would silently lose marker info. Reject.
        StructCfMarker const bMarker = src_.blockMarker(B);
        if (bMarker != StructCfMarker::Linear) {
            auto it = chainNonLinearMarker_.find(head);
            if (it != chainNonLinearMarker_.end()) continue;
            // Also check head's own marker (the chain starts there;
            // no entry exists yet for a fresh chain).
            if (src_.blockMarker(head) != StructCfMarker::Linear) continue;
        }
        // Admit.
        absorbMap_[P] = B;
        absorbedToHead_[B] = head;
        if (bMarker != StructCfMarker::Linear) {
            chainNonLinearMarker_[head] = bMarker;
        } else if (src_.blockMarker(head) != StructCfMarker::Linear) {
            chainNonLinearMarker_[head] = src_.blockMarker(head);
        }
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
        // order — Mir::blockSuccessors contract (see `mir.hpp:131`
        // "CondBr → [ifTrue, ifFalse]"). Reversing this contract
        // would silently flip every branch-fold; the
        // `CondBrTrueFoldsToThenArm` + `CondBrFalseFoldsToElseArm`
        // pair pins both polarities at the test layer.
        auto const succs = src_.blockSuccessors(b);
        if (succs.size() != 2) continue;
        MirBlockId const rawTgt = taken ? succs[0] : succs[1];
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
    result.ok = true;
    return result;
}

} // namespace dss::opt::passes
