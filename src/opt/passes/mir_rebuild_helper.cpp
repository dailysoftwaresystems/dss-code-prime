#include "opt/passes/mir_rebuild_helper.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "mir/mir_opcode.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <utility>

namespace dss::opt::passes {


GlobalClonePrelude
cloneGlobalsOrCarveOut(Mir const& mir, MirBuilder& builder,
                       DiagnosticReporter& reporter,
                       std::string_view passName) {
    // Propagate the module-level alias-analysis polarity through every
    // optimizer pass's rebuild. WITHOUT this line, MirBuilder defaults
    // to Permissive and a release pipeline `[ConstFold, ..., Cse, Licm,
    // ...]` silently downgrades strict-TBAA to Permissive after the
    // first rebuild — CSE/LICM later in the pipeline read the wrong
    // polarity. Closes D-OPT-LOAD-ALIAS-ANALYSIS-PIPELINE-PROPAGATE.
    builder.setAliasingMode(mir.aliasingMode());

    std::size_t const ng = mir.moduleGlobalCount();
    for (std::uint32_t i = 0; i < ng; ++i) {
        if (mir.globalInitFunc(mir.globalAt(i)).valid()) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::X_OptPassSkipped;
            d.severity = DiagnosticSeverity::Info;
            d.actual   = std::format(
                "opt::{}: skipped — module has >= 1 runtime-init "
                "global; func-id remap not yet implemented "
                "(D-OPT2-CONST-FOLD-RUNTIME-INIT-GLOBALS).", passName);
            reporter.report(std::move(d));
            return GlobalClonePrelude::CarvedOut;
        }
    }
    for (std::uint32_t i = 0; i < ng; ++i) {
        MirGlobalId const g = mir.globalAt(i);
        std::uint32_t const initIdx = mir.globalInitLiteralIndex(g);
        std::uint32_t newInitIdx = UINT32_MAX;
        if (initIdx != UINT32_MAX) {
            newInitIdx = builder.literalPoolAdd(mir.literalValue(initIdx));
        }
        builder.addGlobal(mir.globalType(g), mir.globalSymbol(g),
                          newInitIdx, MirFuncId{},
                          mir.globalBinding(g), mir.globalVisibility(g));
    }
    return GlobalClonePrelude::Cloned;
}

void MirRebuildPolicy::onZeroPhiIncomings(MirInstId oldPhi, MirBlockId oldBlock,
                                         MirFuncId oldFn, MirInstId newPhi) {
    std::fprintf(stderr,
        "dss::opt::passes::MirFunctionRebuilder fatal: phi at OLD funcId "
        "v=%u, OLD block v=%u, OLD phi v=%u (new id v=%u) ended phase 3 "
        "with zero accepted incomings — every predecessor was rejected by "
        "policy.acceptPhiIncoming(). Typical cause: a reachable block with "
        "all-unreachable predecessors (a structural violation the verifier "
        "should have rejected at the source MIR).\n",
        oldFn.v, oldBlock.v, oldPhi.v, newPhi.v);
    std::abort();
}

MirInstId MirFunctionRebuilder::rewriteOperand(MirInstId oldOp) const {
    auto const it = rewrite_.find(oldOp.v);
    if (it == rewrite_.end()) {
        std::fprintf(stderr,
            "dss::opt::passes::MirFunctionRebuilder fatal: rewriteOperand: "
            "old MirInstId v=%u has no rewrite entry — scan-order violation "
            "OR operand referenced a skipped instruction "
            "(D-OPT2-REWRITE-MAP-COMPLETENESS).\n", oldOp.v);
        std::abort();
    }
    return it->second;
}

void MirFunctionRebuilder::rebuildFunction(MirFuncId oldFn) {
    dst_.addFunction(src_.funcSignature(oldFn), src_.funcSymbol(oldFn),
                     src_.funcBinding(oldFn), src_.funcVisibility(oldFn));

    // Phase 1: select + pre-create blocks. The policy decides which
    // blocks to walk (all blocks vs RPO-reachable subset etc.).
    auto const blocks = policy_.selectBlocks(src_, oldFn);
    blockMap_.clear();
    blockMap_.reserve(blocks.size());
    for (MirBlockId const oldB : blocks) {
        MirBlockId const newB = dst_.createBlock(src_.blockMarker(oldB));
        blockMap_.emplace(oldB.v, newB);
    }

    // Phase 2: fill blocks; defer Phi incomings to Phase 3. Capture
    // each phi's OLD block so the zero-incomings fail-loud can name
    // it in the diagnostic (a NEW arena id is not debuggable against
    // the source MIR).
    struct DeferredPhi {
        MirInstId oldPhi;
        MirInstId newPhi;
        MirBlockId oldBlock;
    };
    std::vector<DeferredPhi> deferredPhis;
    rewrite_.clear();
    rewrite_.reserve(src_.instCount());

    for (MirBlockId const oldB : blocks) {
        MirBlockId const newB = blockMap_.at(oldB.v);
        dst_.beginBlock(newB);
        // Mem2Reg's IDF-phi-insertion site (D-OPT-MIR-REBUILDER-
        // ONBLOCKBEGIN-HOOK). Default no-op for every other pass.
        policy_.onBlockBegin(oldB, newB, dst_, rewrite_, blockMap_);

        // Walk source-block insts. If a block-merge policy chooses to
        // absorb a successor, the loop continues with the absorbed
        // block's insts AFTER `oldB`'s non-terminator insts — the
        // merged block's terminator is the LAST absorbed block's
        // terminator (D-OPT5-BLOCK-MERGE).
        MirBlockId currentSource = oldB;
        std::uint32_t absorbDepth = 0;
        std::uint32_t const absorbCap =
            static_cast<std::uint32_t>(src_.blockCount()) + 1;
        while (true) {
            std::uint32_t const ninst = src_.blockInstCount(currentSource);
            std::optional<MirBlockId> absorbed;
            for (std::uint32_t i = 0; i < ninst; ++i) {
                MirInstId const oldId = src_.blockInstAt(currentSource, i);
                MirOpcode const op    = src_.instOpcode(oldId);

                if (opcodeInfo(op).isTerminator) {
                    // The terminator is the LAST inst of the block.
                    // Before emitting it, check whether the policy
                    // wants to absorb a successor — if so, skip the
                    // terminator and continue with that successor's
                    // insts in the next outer-loop iteration.
                    absorbed = policy_.absorbSuccessor(currentSource);
                    if (absorbed.has_value()) break;
                    // No absorb — fire the LICM hoist hook + emit the
                    // terminator. This is the merged-block's actual
                    // terminator (which may be the head's original
                    // terminator OR the tail of an absorb chain's).
                    policy_.onBlockBeforeTerminator(oldB, newB, dst_,
                                                    rewrite_, blockMap_);
                    emitTerminator(op, oldId);
                    break;
                }
                if (!policy_.shouldEmit(oldId)) continue;
                if (op == MirOpcode::Phi) {
                    MirInstId const newPhi = dst_.addPhi(src_.instType(oldId));
                    rewrite_.emplace(oldId.v, newPhi);
                    deferredPhis.push_back({oldId, newPhi, oldB});
                    continue;
                }
                emitValue(op, oldId);
            }
            if (!absorbed.has_value()) break;
            currentSource = *absorbed;
            if (++absorbDepth > absorbCap) {
                std::fprintf(stderr,
                    "dss::opt::passes::MirFunctionRebuilder fatal: "
                    "absorbSuccessor chain exceeded block count "
                    "walking from oldB v=%u — cycle in absorb chain "
                    "(substrate-contract violation).\n", oldB.v);
                std::abort();
            }
        }
    }

    // Phase 3: flush phi incomings via the now-complete rewrite map.
    // Phi-incoming preds also route through `redirectBlockTarget` —
    // an incoming-from-absorbed-block must redirect to the absorb
    // head (the surviving block that flows into the phi's owner).
    for (auto const& dp : deferredPhis) {
        std::size_t kept = 0;
        for (auto const& inc : src_.phiIncomings(dp.oldPhi)) {
            if (!policy_.acceptPhiIncoming(inc, blockMap_)) continue;
            MirBlockId const redirectedPred =
                policy_.redirectBlockTarget(inc.pred);
            auto const predIt = blockMap_.find(redirectedPred.v);
            if (predIt == blockMap_.end()) {
                // After `acceptPhiIncoming` admitted this incoming AND
                // `redirectBlockTarget` resolved its pred, the result
                // must be in the surviving blockMap. Reaching here
                // means a policy that accepts a phi incoming but
                // doesn't keep its redirected pred reachable —
                // substrate-contract violation. Fail loud rather than
                // silently drop one SSA edge (the `kept == 0`
                // fail-loud only fires when EVERY incoming is dropped;
                // a single-edge silent drop turns the phi value-wrong
                // without diagnostic).
                std::fprintf(stderr,
                    "dss::opt::passes::MirFunctionRebuilder fatal: phi "
                    "incoming pred old v=%u (redirected to v=%u) not in "
                    "blockMap_ after acceptPhiIncoming admitted it. "
                    "OLD phi v=%u, OLD block v=%u, OLD fn v=%u. Policy "
                    "must keep redirected preds in the surviving set.\n",
                    inc.pred.v, redirectedPred.v, dp.oldPhi.v,
                    dp.oldBlock.v, oldFn.v);
                std::abort();
            }
            MirInstId const newVal = mapOperand(inc.value);
            dst_.addPhiIncoming(dp.newPhi,
                                MirPhiIncoming{newVal, predIt->second});
            ++kept;
        }
        if (kept == 0) {
            policy_.onZeroPhiIncomings(dp.oldPhi, dp.oldBlock, oldFn, dp.newPhi);
        }
    }
}

void MirFunctionRebuilder::emitValue(MirOpcode op, MirInstId oldId) {
    // Value origins: leaves are always copied verbatim.
    if (op == MirOpcode::Const) {
        MirInstId const newId = dst_.addConst(
            src_.literalValue(src_.constLiteralIndex(oldId)),
            src_.instType(oldId));
        rewrite_.emplace(oldId.v, newId);
        return;
    }
    if (op == MirOpcode::Arg) {
        MirInstId const newId = dst_.addArg(src_.argIndex(oldId),
                                            src_.instType(oldId));
        rewrite_.emplace(oldId.v, newId);
        return;
    }
    if (op == MirOpcode::GlobalAddr) {
        MirInstId const newId = dst_.addGlobalAddr(
            src_.globalAddrSymbol(oldId), src_.instType(oldId));
        rewrite_.emplace(oldId.v, newId);
        return;
    }

    // Per-pass full-inst-replacement hook. Returns nullopt → verbatim
    // copy. ConstFold emits a Const for foldable expressions; CopyProp
    // uses substituteOldOperand instead so the dead Phi stays for DCE.
    if (auto rewritten = policy_.tryRewrite(op, oldId, dst_, rewrite_);
        rewritten.has_value()) {
        rewrite_.emplace(oldId.v, *rewritten);
        return;
    }

    // Verbatim copy with operand-level substitution applied.
    auto const oldOps = src_.instOperands(oldId);
    std::vector<MirInstId> newOps;
    newOps.reserve(oldOps.size());
    for (auto o : oldOps) {
        newOps.push_back(mapOperand(o));
    }
    MirInstId const newId = dst_.addInst(op, newOps, src_.instType(oldId),
                                         src_.instPayload(oldId),
                                         src_.instFlags(oldId));
    rewrite_.emplace(oldId.v, newId);
}

void MirFunctionRebuilder::emitTerminator(MirOpcode op, MirInstId oldId) {
    auto const oldOps  = src_.instOperands(oldId);
    auto const oldBlk  = src_.instBlock(oldId);
    auto const oldSucc = src_.blockSuccessors(oldBlk);
    bool const record  = policy_.recordTerminatorInRewrite();
    auto remember = [&](MirInstId newId) {
        if (record) rewrite_.emplace(oldId.v, newId);
    };
    // Per-terminator full-replacement hook (branch-folding etc.).
    // Returning a value short-circuits the standard emit arms.
    if (auto const rewritten = policy_.tryRewriteTerminator(
            op, oldId, dst_, rewrite_, blockMap_); rewritten.has_value()) {
        remember(*rewritten);
        return;
    }
    auto mapSucc = [&](MirBlockId oldS) -> MirBlockId {
        MirBlockId const redirected = policy_.redirectBlockTarget(oldS);
        auto const it = blockMap_.find(redirected.v);
        if (it == blockMap_.end()) {
            std::fprintf(stderr,
                "dss::opt::passes::MirFunctionRebuilder fatal: emitTerminator "
                "successor old v=%u (redirected to v=%u) not in blockMap_ — "
                "either `selectBlocks` omitted a reachable block OR a policy's "
                "`redirectBlockTarget` returned an elided block. Originating "
                "terminator: old MirInstId v=%u.\n",
                oldS.v, redirected.v, oldId.v);
            std::abort();
        }
        return it->second;
    };
    switch (op) {
        case MirOpcode::Br: {
            MirInstId const newId = dst_.addBr(mapSucc(oldSucc[0]));
            remember(newId);
            return;
        }
        case MirOpcode::CondBr: {
            MirInstId const cond = mapOperand(oldOps[0]);
            MirInstId const newId = dst_.addCondBr(
                cond, mapSucc(oldSucc[0]), mapSucc(oldSucc[1]));
            remember(newId);
            return;
        }
        case MirOpcode::Switch: {
            MirInstId const disc = mapOperand(oldOps[0]);
            std::vector<std::pair<MirInstId, MirBlockId>> cases;
            std::size_t const ncases = oldSucc.size() - 1;
            cases.reserve(ncases);
            for (std::size_t i = 0; i < ncases; ++i) {
                cases.emplace_back(
                    mapOperand(oldOps[1 + i]),
                    mapSucc(oldSucc[i]));
            }
            MirInstId const newId = dst_.addSwitch(
                disc, cases, mapSucc(oldSucc[ncases]));
            remember(newId);
            return;
        }
        case MirOpcode::Return: {
            std::optional<MirInstId> retVal;
            if (!oldOps.empty()) {
                retVal = mapOperand(oldOps[0]);
            }
            MirInstId const newId = dst_.addReturn(retVal);
            remember(newId);
            return;
        }
        case MirOpcode::Unreachable: {
            MirInstId const newId = dst_.addUnreachable();
            remember(newId);
            return;
        }
        default:
            std::fprintf(stderr,
                "dss::opt::passes::MirFunctionRebuilder fatal: emitTerminator: "
                "MirOpcode %d marked isTerminator but no clone arm — add an "
                "arm here when introducing a new terminator opcode.\n",
                static_cast<int>(op));
            std::abort();
    }
}

} // namespace dss::opt::passes
