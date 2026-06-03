#pragma once

// Shared MIR-tier function rebuilder substrate. Every MIR-tier pass
// that rewrites a function via MirBuilder drives this rebuilder —
// centralizing the rewrite-map / terminator-clone / phi-deferral
// invariants prevents per-pass drift (D-OPT-MIR-REBUILDER-EXTRACT).
//
// **REWRITE-MAP COMPLETENESS CONTRACT** (D-OPT2-REWRITE-MAP-COMPLETENESS):
// every value-producing OLD-module inst must be in the rewrite map
// by the time a downstream consumer reads it. Phi back-edges are the
// one delayed case — handled in phase 3 only. A missing entry at
// emit time is a substrate-contract violation; `rewriteOperand`
// fails loud rather than propagating an invalid `MirInstId{}`.
//
// **The 3-phase rebuild**:
//   Phase 1 — pre-create every selected block so terminators can
//             target forward references (loop back-edges).
//   Phase 2 — fill blocks with instructions; Phi incomings deferred.
//   Phase 3 — flush Phi incomings via the now-complete rewrite map.

#include "core/types/diagnostic_reporter.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dss::opt::passes {

// Shared "clone globals OR carve out on runtime-init" prelude used
// by every MIR-tier rebuild pass (ConstFold, Dce, Mem2Reg, CopyProp).
// A module with any `globalInitFunc.valid()` global requires a two-
// pass func-id remap not yet implemented; carve-out emits Info
// `X_OptPassSkipped` so callers / diff-verify can observe the
// deliberate skip. The carve-out anchor is shared:
// D-OPT2-CONST-FOLD-RUNTIME-INIT-GLOBALS.
enum class GlobalClonePrelude : std::uint8_t { Cloned, CarvedOut };

[[nodiscard]] GlobalClonePrelude
cloneGlobalsOrCarveOut(Mir const& mir, MirBuilder& builder,
                       DiagnosticReporter& reporter,
                       std::string_view passName);

// Per-pass policy driving the rebuild. Pass-specific state lives on
// the concrete subclass; the rebuilder owns only the rewrite map +
// the per-function block map. Default-method implementations express
// the "no special handling" path; a concrete policy overrides only
// the hooks it needs.
class MirRebuildPolicy {
public:
    virtual ~MirRebuildPolicy() = default;

    // Phase 1: returns the block list to walk. Called once per function.
    // ConstFold: all blocks (full module preserve). DCE: RPO-reachable
    // subset. Mem2Reg: all blocks (it inserts Phis, doesn't elide blocks).
    [[nodiscard]] virtual std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) = 0;

    // Phase 2: per-block prologue hook called after `dst.beginBlock(newB)`
    // and before the block's instruction loop runs. Default = no-op.
    // Mem2Reg overrides this to insert Phi placeholders at the head of
    // each block in the iterated dominance frontier of a promotable
    // alloca's def-blocks. The rewrite map is mutable so the hook can
    // record any (oldId → newId) entries the same block's subsequent
    // instructions will consume. `blockMap` is provided so the policy
    // can resolve OLD-block-id refs (e.g. inserted-phi marker keys
    // that the rename walk already mapped to OLD block ids).
    virtual void onBlockBegin(
        MirBlockId /*oldB*/, MirBlockId /*newB*/,
        MirBuilder& /*dst*/,
        std::unordered_map<std::uint32_t, MirInstId>& /*rewrite*/,
        std::unordered_map<std::uint32_t, MirBlockId> const& /*blockMap*/) {}

    // Phase 2: pre-emit filter for non-Phi, non-terminator value-producing
    // insts. Returns false → skip this inst entirely (caller's counter
    // increments). ConstFold default = always true; DCE = liveInsts.count.
    [[nodiscard]] virtual bool shouldEmit(MirInstId /*oldId*/) {
        return true;
    }

    // Phase 2: pre-emit rewrite hook. Returns a new MirInstId if the
    // pass handled the inst entirely (rebuilder skips the verbatim copy).
    // Returns nullopt → fall through to verbatim copy.
    // ConstFold: emits Const on fold, returns new id. DCE: nullopt always.
    // Mem2Reg: emits the alloca-slot's current SSA value for Load ops,
    //          omits Store ops (returns "skip" via {InvalidMirInstId});
    //          this distinction is encoded via `shouldEmit` for skips
    //          and `tryRewrite` for substitutions.
    [[nodiscard]] virtual std::optional<MirInstId>
    tryRewrite(MirOpcode /*op*/, MirInstId /*oldId*/,
               MirBuilder& /*dst*/,
               std::unordered_map<std::uint32_t, MirInstId> const& /*rewrite*/) {
        return std::nullopt;
    }

    // Pre-rewrite operand substitution hook (applied BEFORE
    // `rewriteOperand` maps old→new). Default = identity. CopyProp
    // overrides this for SSA Phi-collapse: a Phi whose non-self
    // incomings all resolve to the same value `V` redirects every
    // use of the Phi to `V` directly. The hook works in OLD-id space
    // so the resolution chain (P1 → P2 → V) is transitively
    // path-compressed during analysis on the immutable source MIR;
    // `rewriteOperand` then maps the resolved old target to its NEW
    // id (which has already been emitted in phase 2 by SSA's
    // def-dominates-use invariant + RPO block order). This shape is
    // what allows the Phi-collapse pass to leave the dead Phi
    // instruction in the rebuilt module — a subsequent DCE pass
    // sweeps it because it has zero uses.
    [[nodiscard]] virtual MirInstId
    substituteOldOperand(MirInstId oldOp) {
        return oldOp;
    }

    // Post-rewrite operand substitution hook (applied AFTER
    // `rewriteOperand` resolves old→new). Default = identity.
    // Reserved for per-use dominating-definition substitution
    // (D-OPT-COPYPROP-BLOCK-CURSOR — general operand-substitution
    // variant beyond Phi-collapse).
    [[nodiscard]] virtual MirInstId
    substituteOperand(MirInstId operand) {
        return operand;
    }

    // Successor-block redirect hook applied during terminator
    // emission. Default = identity. SimplifyCFG overrides this for
    // empty-block jump-threading: a block B whose only inst is a
    // `Br(S)` (no phi at S uses B as a pred) becomes a "trampoline" —
    // any predecessor branching to B is redirected to S directly,
    // B is elided from `selectBlocks`, and `blockMap_.at(redirected)`
    // succeeds because S is in the surviving-blocks set.
    [[nodiscard]] virtual MirBlockId
    redirectBlockTarget(MirBlockId oldTarget) {
        return oldTarget;
    }

    // Per-terminator-opcode full-replacement hook. Default = nullopt
    // → standard `emitTerminator` arm. SimplifyCFG overrides for
    // branch-folding: `CondBr(Const(true|false), T, F)` → `Br(T|F)`.
    // The hook receives the SAME ids the standard path would consume;
    // returning a new MirInstId records it in the rewrite map (per
    // `recordTerminatorInRewrite`) and skips the standard emit arm.
    [[nodiscard]] virtual std::optional<MirInstId>
    tryRewriteTerminator(MirOpcode /*op*/, MirInstId /*oldId*/,
                         MirBuilder& /*dst*/,
                         std::unordered_map<std::uint32_t, MirInstId> const& /*rewrite*/,
                         std::unordered_map<std::uint32_t, MirBlockId> const& /*blockMap*/) {
        return std::nullopt;
    }

    // Phase 3: per-phi-incoming filter. Default = accept everything.
    // DCE: skip incomings whose pred isn't in the (RPO-reachable)
    // blockMap. ConstFold: identity (all preds reachable).
    [[nodiscard]] virtual bool acceptPhiIncoming(
        MirPhiIncoming const& /*inc*/,
        std::unordered_map<std::uint32_t, MirBlockId> const& /*blockMap*/) {
        return true;
    }

    // Phase 3: called when a phi ended up with zero accepted incomings.
    // Default: structural violation, std::abort with diagnostic
    // naming the OLD function/block/phi (so a maintainer can locate
    // the bad MIR), not just the new arena id.
    virtual void onZeroPhiIncomings(MirInstId oldPhi, MirBlockId oldBlock,
                                    MirFuncId oldFn, MirInstId newPhi);

    // Phase 2: whether terminator results should be recorded in the
    // rewrite map. ConstFold: true (defensive — terminator results
    // are never read but for consistency). DCE: false (saves a few
    // hash inserts; terminator results are never used). New passes
    // should default to true unless they explicitly verify nothing
    // downstream reads terminator-as-operand.
    [[nodiscard]] virtual bool recordTerminatorInRewrite() const noexcept {
        return true;
    }
};

// The concrete rebuilder. Owns the rewrite map + block map. Drives
// the 3-phase walk + calls into `policy` at the documented hook
// points. Pass-specific counters live on the policy (the rebuilder
// is policy-agnostic about WHY a hook returned false; counting is the
// policy's concern).
class MirFunctionRebuilder {
public:
    MirFunctionRebuilder(Mir const& src, MirBuilder& dst,
                         MirRebuildPolicy& policy)
        : src_(src), dst_(dst), policy_(policy) {}

    // Run the 3-phase rebuild for a single old-module function.
    void rebuildFunction(MirFuncId oldFn);

    // Post-rebuild accessors so a pass that inserts NEW SSA values
    // (e.g. Mem2Reg's IDF phis — net-new defs that don't correspond
    // to any old-module instruction) can resolve old→new ids when
    // wiring up their own phi incomings. Read-only; the rebuilder
    // remains the sole writer of these maps during rebuild.
    [[nodiscard]] std::unordered_map<std::uint32_t, MirInstId> const&
    rewriteMap() const noexcept { return rewrite_; }
    [[nodiscard]] std::unordered_map<std::uint32_t, MirBlockId> const&
    blockMap() const noexcept { return blockMap_; }

private:
    [[nodiscard]] MirInstId rewriteOperand(MirInstId oldOp) const;
    void emitValue(MirOpcode op, MirInstId oldId);
    void emitTerminator(MirOpcode op, MirInstId oldId);

    Mir const&        src_;
    MirBuilder&       dst_;
    MirRebuildPolicy& policy_;
    std::unordered_map<std::uint32_t, MirInstId>  rewrite_;
    std::unordered_map<std::uint32_t, MirBlockId> blockMap_;
};

} // namespace dss::opt::passes
