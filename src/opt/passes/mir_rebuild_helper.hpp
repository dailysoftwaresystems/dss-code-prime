#pragma once

// Shared MIR-tier function rebuilder substrate. Consumed by every
// MIR pass that follows the "rebuild-via-MirBuilder" pattern
// (ConstFold, DCE today; Mem2Reg + CopyProp + LICM + CSE tomorrow).
//
// Extracted when Mem2Reg arrived as the 3rd consumer — pre-extraction
// ConstFold + DCE carried 70%-identical `FunctionRebuilder` classes
// in anon namespace + drift had already occurred (DCE doesn't
// `rewrite_.emplace` terminators; ConstFold does). Closes
// D-OPT-MIR-REBUILDER-EXTRACT before the 3rd copy multiplies the
// drift.
//
// **The contract** (D-OPT2-REWRITE-MAP-COMPLETENESS):
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

#include "mir/mir.hpp"
#include "mir/mir_node.hpp"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace dss::opt::passes {

// Per-pass policy driving the rebuild. Pass-specific state lives on
// the concrete subclass; the rebuilder owns only the rewrite map +
// the per-function block map. The default-method implementations
// here cover the "no special handling" path used by ConstFold's
// default arms (a concrete policy overrides only the hooks it needs).
class MirRebuildPolicy {
public:
    virtual ~MirRebuildPolicy() = default;

    // Phase 1: returns the block list to walk. Called once per function.
    // ConstFold: all blocks (full module preserve). DCE: RPO-reachable
    // subset. Mem2Reg: all blocks (it inserts Phis, doesn't elide blocks).
    [[nodiscard]] virtual std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) = 0;

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

    // Phase 2: per-operand substitution hook applied during the
    // verbatim-copy arm AFTER rewriteOperand resolves old→new.
    // Default returns the operand unchanged (identity).
    // Copy-prop will override this to redirect uses to a dominating
    // definition. ConstFold + DCE + Mem2Reg use identity.
    [[nodiscard]] virtual MirInstId
    substituteOperand(MirInstId operand) {
        return operand;
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
