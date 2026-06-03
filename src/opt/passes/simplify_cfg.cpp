#include "opt/passes/simplify_cfg.hpp"

#include "mir/mir_cfg.hpp"
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

    void analyze(MirFuncId fn);

    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        // Walk reachable-from-entry blocks in RPO order, excluding
        // those flagged for elision by `analyze()`. A trampoline-
        // block id appears as a KEY in `jumpThreadMap_` — that map
        // IS the elided set. Single source of truth: querying it
        // here means the membership invariant can't drift between
        // two parallel containers.
        auto const rpo = mirReversePostOrder(src, src.funcEntry(fn));
        std::vector<MirBlockId> out;
        out.reserve(rpo.size());
        for (MirBlockId const b : rpo) {
            if (jumpThreadMap_.count(b) == 0) out.push_back(b);
        }
        return out;
    }

    [[nodiscard]] MirBlockId redirectBlockTarget(MirBlockId oldTarget) override {
        // Chase the trampoline chain to a surviving block. The map is
        // path-compressed in `analyze()` so a single lookup suffices.
        auto it = jumpThreadMap_.find(oldTarget);
        if (it == jumpThreadMap_.end()) return oldTarget;
        return it->second;
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

    std::size_t branchesFolded_     = 0;
    std::size_t blocksJumpThreaded_ = 0;
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
    mir = std::move(builder).finish();
    result.ok = true;
    return result;
}

} // namespace dss::opt::passes
