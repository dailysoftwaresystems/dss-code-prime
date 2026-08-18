#include "opt/passes/mir_rebuild_helper.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "mir/mir_opcode.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <utility>

namespace dss::opt::passes {

namespace {

// ★★★ THE ONE PLACE THIS FILE COMPOSES A FATAL, and the pass attribution is a
// MANDATORY PARAMETER rather than an optional decoration
// (D-OPT-MIR-REBUILDER-FATAL-CANNOT-NAME-THE-PASS). Nine `fprintf` sites each
// free to forget the name is how the row happened; one composer that cannot be
// called without a name is how it stays closed — a fatal added here next year
// gets the attribution because there is no other way to raise one.
//
// `[[noreturn]]` so callers need no trailing `std::abort()` (a site that
// formatted the message and then FELL THROUGH would be a silent continuation
// past a substrate-contract violation — the exact failure class this file's
// aborts exist to prevent).
//
// `std::format`, not `fprintf("%s")`: every caller hands us a
// `std::string_view`, which is not guaranteed NUL-terminated, so `%s` on
// `.data()` is undefined behaviour — and this is the code path where the process
// is already dying, i.e. the worst place to add a second fault. `%.*s` would
// work but repeats a `static_cast<int>(sv.size()), sv.data()` pair at every
// site, which is exactly the per-site detail this helper exists to delete.
//
// The explicit flush matters: `std::abort()` is not required to flush C streams,
// and the whole value of this message is that it survives the process.
[[noreturn]] void rebuildFatal(std::string_view subject,
                               std::string_view passName,
                               std::string_view detail) {
    std::fputs(std::format("dss::opt::passes::{} fatal [pass={}]: {}\n",
                           subject, passName, detail).c_str(), stderr);
    std::fflush(stderr);
    std::abort();
}

// The subject every `MirFunctionRebuilder` (and `MirRebuildPolicy` default-arm)
// fatal reports under — one spelling, so the message is greppable.
constexpr std::string_view kRebuilderSubject = "MirFunctionRebuilder";

} // namespace

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
    // Same propagation discipline for `charTypesAliasAll` (per-language
    // C99 §6.5 ¶7 opt-in).
    builder.setAliasingMode(mir.aliasingMode());
    builder.setCharTypesAliasAll(mir.charTypesAliasAll());

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
                          mir.globalBinding(g), mir.globalVisibility(g),
                          mir.globalIsConst(g),
                          // TLS C1 (D-CSUBSET-THREAD-LOCAL, CRIT-3): preserve
                          // thread storage duration across the pass rebuild —
                          // EVERY optimized compile clones through here.
                          mirThreadStorageOf(mir.globalIsThreadLocal(g)),
                          // D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN: preserve the
                          // global's explicit alignment across the rebuild.
                          mir.globalAlignmentBytes(g));
    }
    return GlobalClonePrelude::Cloned;
}

void cloneGlobalsRemappingInitFunc(Mir const& mir, MirBuilder& builder,
                                   std::span<std::uint32_t const> oldOrdinalToNew,
                                   std::string_view passName) {
    // Same alias-analysis polarity propagation every clone path performs
    // (D-OPT-LOAD-ALIAS-ANALYSIS-PIPELINE-PROPAGATE).
    builder.setAliasingMode(mir.aliasingMode());
    builder.setCharTypesAliasAll(mir.charTypesAliasAll());

    if (oldOrdinalToNew.size() != mir.moduleFuncCount()) {
        rebuildFatal("cloneGlobalsRemappingInitFunc", passName,
            std::format("remap has {} entries but the source module has {} "
                        "functions — the caller built the table from a "
                        "different module than it is cloning.",
                        oldOrdinalToNew.size(), mir.moduleFuncCount()));
    }

    std::uint32_t const newTag = builder.id().v;
    std::size_t const ng = mir.moduleGlobalCount();
    for (std::uint32_t i = 0; i < ng; ++i) {
        MirGlobalId const g = mir.globalAt(i);
        std::uint32_t const initIdx = mir.globalInitLiteralIndex(g);
        std::uint32_t newInitIdx = UINT32_MAX;
        if (initIdx != UINT32_MAX) {
            newInitIdx = builder.literalPoolAdd(mir.literalValue(initIdx));
        }
        MirFuncId const oldInitFunc = mir.globalInitFunc(g);
        MirFuncId newInitFunc{};
        if (oldInitFunc.valid()) {
            std::uint32_t const mapped = oldOrdinalToNew[oldInitFunc.v];
            if (mapped == UINT32_MAX) {
                rebuildFatal("cloneGlobalsRemappingInitFunc", passName,
                    std::format("global symbol {} is initialized by function "
                                "ordinal {}, which the rebuild DROPPED. A "
                                "module-init function is never a legitimate "
                                "drop target; re-pointing the global at "
                                "whichever function inherited the ordinal "
                                "would be a silent wrong-body call.",
                                mir.globalSymbol(g).v, oldInitFunc.v));
            }
            newInitFunc = MirFuncId{mapped, newTag};
        }
        builder.addGlobal(mir.globalType(g), mir.globalSymbol(g),
                          newInitIdx, newInitFunc,
                          mir.globalBinding(g), mir.globalVisibility(g),
                          mir.globalIsConst(g),
                          // TLS C1 (D-CSUBSET-THREAD-LOCAL, CRIT-3): preserve
                          // thread storage duration across the clone.
                          mirThreadStorageOf(mir.globalIsThreadLocal(g)),
                          // D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN: preserve the
                          // global's explicit alignment across the rebuild.
                          mir.globalAlignmentBytes(g));
    }
}

void cloneGlobalsVerbatim(Mir const& mir, MirBuilder& builder) {
    // Same alias-analysis polarity propagation as cloneGlobalsOrCarveOut
    // (D-OPT-LOAD-ALIAS-ANALYSIS-PIPELINE-PROPAGATE) — without it the
    // rebuilt module silently downgrades strict-TBAA / char-aliases-all
    // to the MirBuilder defaults.
    builder.setAliasingMode(mir.aliasingMode());
    builder.setCharTypesAliasAll(mir.charTypesAliasAll());

    // CONTRACT: call this AFTER every function has been re-added to
    // `builder` (the prune adds functions in source order, so the new
    // func ORDINAL equals the old one). A global's `initFunc` therefore
    // needs only its arena TAG re-stamped to the new module — the
    // optimizer mints a FRESH MirModuleId per rebuild (strong_ids.hpp:
    // "each rebuilt module gets a fresh MirModuleId"), so the OLD id's
    // tag is foreign and `MirBuilder::addGlobal`'s cross-module guard
    // would abort on it. The ordinal is the stable part; the tag re-stamp
    // is mechanical (mir_merge likewise re-targets initFunc into its builder's
    // id space, but there via a SYMBOL-keyed remap, because the merge
    // reorders/drops functions; here a tag re-stamp alone suffices precisely
    // because the prune preserves ordinals). NO ordinal remap — the prune
    // drops BLOCKS only, never functions, and never reorders them.
    std::uint32_t const newTag = builder.id().v;
    std::size_t const ng = mir.moduleGlobalCount();
    for (std::uint32_t i = 0; i < ng; ++i) {
        MirGlobalId const g = mir.globalAt(i);
        std::uint32_t const initIdx = mir.globalInitLiteralIndex(g);
        std::uint32_t newInitIdx = UINT32_MAX;
        if (initIdx != UINT32_MAX) {
            newInitIdx = builder.literalPoolAdd(mir.literalValue(initIdx));
        }
        MirFuncId const oldInitFunc = mir.globalInitFunc(g);
        MirFuncId const newInitFunc = oldInitFunc.valid()
            ? MirFuncId{oldInitFunc.v, newTag}   // same ordinal, new module tag
            : MirFuncId{};
        builder.addGlobal(mir.globalType(g), mir.globalSymbol(g),
                          newInitIdx, newInitFunc,
                          mir.globalBinding(g), mir.globalVisibility(g),
                          mir.globalIsConst(g),
                          // TLS C1 (D-CSUBSET-THREAD-LOCAL, CRIT-3): preserve
                          // thread storage duration across the verbatim clone.
                          mirThreadStorageOf(mir.globalIsThreadLocal(g)),
                          // D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN: preserve the
                          // global's explicit alignment across the rebuild.
                          mir.globalAlignmentBytes(g));
    }
}

void MirRebuildPolicy::onZeroPhiIncomings(MirInstId oldPhi, MirBlockId oldBlock,
                                         MirFuncId oldFn, MirInstId newPhi,
                                         std::string_view drivingPassName) {
    rebuildFatal(kRebuilderSubject, drivingPassName,
        std::format("phi at OLD funcId v={}, OLD block v={}, OLD phi v={} "
                    "(new id v={}) ended phase 3 with zero accepted incomings "
                    "— every predecessor was rejected by "
                    "policy.acceptPhiIncoming(). Typical cause: a reachable "
                    "block with all-unreachable predecessors (a structural "
                    "violation the verifier should have rejected at the source "
                    "MIR).",
                    oldFn.v, oldBlock.v, oldPhi.v, newPhi.v));
}

MirInstId MirFunctionRebuilder::rewriteOperand(MirInstId oldOp) const {
    auto const it = rewrite_.find(oldOp.v);
    if (it == rewrite_.end()) {
        // ★ THE FATAL THAT MOTIVATED THE ROW. This text is identical for all
        // ~9 policies; `[pass=…]` is the only thing in it that says WHOSE
        // rebuild died, and last cycle its absence cost a whole `DSS_OPT_TRACE`
        // run to learn the one word `SimplifyCfg`.
        rebuildFatal(kRebuilderSubject, policy_.passName(),
            std::format("rewriteOperand: old MirInstId v={} has no rewrite "
                        "entry — scan-order violation OR operand referenced a "
                        "skipped instruction "
                        "(D-OPT2-REWRITE-MAP-COMPLETENESS).", oldOp.v));
    }
    return it->second;
}

void MirFunctionRebuilder::rebuildFunction(MirFuncId oldFn) {
    // TF-C78 (D-CSUBSET-NOINLINE): `funcNoInline` rides along with
    // binding/visibility. ★ THIS IS THE LOAD-BEARING PROPAGATION SITE — this
    // rebuilder is the shared substrate under EVERY optimizer pass, so a flag
    // dropped here is erased by the very release pipeline it exists to
    // constrain: the first pass to rebuild the module (ConstFold, Mem2Reg, …)
    // would hand a cleared flag to the NEXT Inlining iteration, which would then
    // splice the body the source forbade — silently.
    // TF-C81 (D-CSUBSET-ALWAYSINLINE): `funcAlwaysInline` rides along too — but
    // ★ ITS FAILURE MODE IS NOT ITS NEIGHBOUR'S, AND THE DIFFERENCE WAS
    // MEASURED RATHER THAN ASSUMED.
    //
    // For `noInline` the flag must keep refusing on EVERY iteration, so clearing
    // it here re-arms the inliner on iteration 2 and the end-to-end release
    // outcome breaks. For `alwaysInline` the flag only has to be present at the
    // FIRST inlining opportunity: `Inlining` runs first in iteration 1, before
    // any rebuild has touched the module, so the splice has already happened by
    // the time a cleared flag could matter.
    //
    // MEASURED CONSEQUENCE: dropping this argument leaves
    // `MirLoweringCSubsetLinkage.AlwaysInlineBypassesThresholdInShippedRelease`
    // GREEN — the end-to-end pin CANNOT detect this hop. Only the dedicated
    // flag-survival assertions catch it
    // (`MirRebuildHelper.RebuildFunctionPreservesAlwaysInline` and the
    // survives-the-rebuild check inside
    // `Inlining.AlwaysInlineCalleeBypassesCostThreshold`). So the propagation
    // pin is MORE load-bearing here than it was for `noInline`, not less: TF-C78
    // had two indistinguishable failures, this one has a failure the end-to-end
    // test is blind to entirely.
    //
    // The bit still matters in every shape where the first iteration does NOT
    // finish the job — a callee that becomes inlinable only after an earlier
    // pass simplifies a caller, or a cross-CU module merged after one round of
    // optimization — which is exactly why it is carried rather than argued away.
    // TF-C85: `funcNoOptimize` rides along on the same terms and for the same
    // reason — the merged/rebuilt module is what the NEXT pass sees, so a flag
    // cleared here would let iteration 2 start optimizing a function the source
    // excluded.
    // TF-C92 (D-CSUBSET-NO-SANITIZE-THREAD): `funcNoSanitizeThread` rides along
    // too, and ★ ITS PIN IS THE MOST LOAD-BEARING OF THE FOUR, not the least.
    // The other three flags each reach a consumer whose OUTPUT changes when the
    // flag is lost (a splice that happens, a threshold that re-applies, a function
    // that gets optimized), so at least one end-to-end fixture can in principle go
    // red. This flag reaches NO pass at all — DSS ships no sanitizer — so dropping
    // this argument changes NOTHING observable anywhere in the pipeline except the
    // `.dssir` text of a module that has been rebuilt. There is no end-to-end
    // witness to fall back on, by construction: `MirRebuildHelper.
    // RebuildFunctionPreservesNoSanitizeThread` is the ONLY thing standing between
    // this argument and silent deletion.
    dst_.addFunction(src_.funcSignature(oldFn), src_.funcSymbol(oldFn),
                     src_.funcBinding(oldFn), src_.funcVisibility(oldFn),
                     src_.funcNoInline(oldFn), src_.funcAlwaysInline(oldFn),
                     src_.funcNoOptimize(oldFn),
                     src_.funcNoSanitizeThread(oldFn));

    // ★★ TF-C85 (D-OPT-NOOPTIMIZE-NEUTERS-POLICY): THE per-function optimizer
    // opt-out, applied at the ONE shared chokepoint under all 8 rebuild passes.
    //
    // ★ THE FUNCTION IS STILL REBUILT — ONLY THE POLICY IS SWAPPED. This is the
    // whole design, and the obvious alternative is a bug: `return`ing before the
    // `addFunction` above (or before the block walk) does NOT "leave the
    // function alone", it DELETES it, because `dst_` is a fresh module that
    // contains exactly what this rebuilder puts in it. Swapping in the identity
    // policy instead reproduces the source function instruction for instruction
    // while every pass-specific hook — ConstFold's folds, DCE's liveness filter,
    // Mem2Reg's phi insertion, LICM's hoists, SimplifyCfg's merges/redirects,
    // CopyProp's and CSE's operand substitutions — is simply not consulted.
    //
    // Restored on the way out (RAII-free but exception-free code path; the
    // rebuild has no early returns after this point), so the next function in
    // the same rebuild gets the pass's real policy back.
    bool const neuter =
        src_.funcNoOptimize(oldFn) && !policy_.mandatoryNormalization();
    if (neuter) policy_.onFunctionNeutered(oldFn);
    active_ = neuter ? static_cast<MirRebuildPolicy*>(&identity_) : &policy_;

    // Phase 1: select + pre-create blocks. The policy decides which
    // blocks to walk (all blocks vs RPO-reachable subset etc.).
    auto const blocks = active_->selectBlocks(src_, oldFn);
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
        active_->onBlockBegin(oldB, newB, dst_, rewrite_, blockMap_);

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
                    absorbed = active_->absorbSuccessor(currentSource);
                    if (absorbed.has_value()) break;
                    // No absorb — fire the LICM hoist hook + emit the
                    // terminator. This is the merged-block's actual
                    // terminator (which may be the head's original
                    // terminator OR the tail of an absorb chain's).
                    active_->onBlockBeforeTerminator(oldB, newB, dst_,
                                                    rewrite_, blockMap_);
                    emitTerminator(op, oldId);
                    break;
                }
                if (!active_->shouldEmit(oldId)) continue;
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
                rebuildFatal(kRebuilderSubject, policy_.passName(),
                    std::format("absorbSuccessor chain exceeded block count "
                                "walking from oldB v={} — cycle in absorb "
                                "chain (substrate-contract violation).",
                                oldB.v));
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
            if (!active_->acceptPhiIncoming(inc, dp.oldBlock, blockMap_)) continue;
            MirBlockId const redirectedPred =
                active_->redirectBlockTarget(inc.pred);
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
                rebuildFatal(kRebuilderSubject, policy_.passName(),
                    std::format("phi incoming pred old v={} (redirected to "
                                "v={}) not in blockMap_ after "
                                "acceptPhiIncoming admitted it. OLD phi v={}, "
                                "OLD block v={}, OLD fn v={}. Policy must keep "
                                "redirected preds in the surviving set.",
                                inc.pred.v, redirectedPred.v, dp.oldPhi.v,
                                dp.oldBlock.v, oldFn.v));
            }
            MirInstId const newVal = mapOperand(inc.value);
            dst_.addPhiIncoming(dp.newPhi,
                                MirPhiIncoming{newVal, predIt->second});
            ++kept;
        }
        if (kept == 0) {
            // `policy_`, NOT `active_`: under the TF-C85 neuter `active_` is the
            // identity substitute, and the reader needs the PIPELINE pass to
            // locate the abort (see `onZeroPhiIncomings`' `drivingPassName`).
            active_->onZeroPhiIncomings(dp.oldPhi, dp.oldBlock, oldFn, dp.newPhi,
                                        policy_.passName());
        }
    }
    // TF-C85: hand the pass its own policy back for the next function.
    active_ = &policy_;
}

// TF-C85: the ONE hook `MirRebuildPolicy` leaves without a default. "All blocks
// in natural order" is the verbatim answer — the same one ConstFold's policy
// gives, and the only one consistent with copying a function unchanged.
std::vector<MirBlockId>
MirIdentityRebuildPolicy::selectBlocks(Mir const& src, MirFuncId fn) {
    std::vector<MirBlockId> out;
    std::uint32_t const n = src.funcBlockCount(fn);
    out.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) out.push_back(src.funcBlockAt(fn, i));
    return out;
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
        // Carry BOTH the class ordinal AND the flat call-operand position —
        // Arg has a dedicated builder (addInst refuses it), so this re-encode
        // must thread the position or it silently defaults to the ordinal and
        // the inliner mis-maps mixed-class actuals. Every pass drives this
        // rebuilder (Identity is the FIRST release pass), so a wipe here kills
        // the position before Inlining runs (D-OPT-RELEASE-SYSV-MIXED-CLASS-
        // REG-ARG-DROP).
        MirInstId const newId = dst_.addArg(src_.argIndex(oldId),
                                            src_.instType(oldId),
                                            src_.argPosition(oldId));
        rewrite_.emplace(oldId.v, newId);
        return;
    }
    if (op == MirOpcode::GlobalAddr) {
        MirInstId const newId = dst_.addGlobalAddr(
            src_.globalAddrSymbol(oldId), src_.instType(oldId));
        rewrite_.emplace(oldId.v, newId);
        return;
    }
    if (op == MirOpcode::BlockAddress) {
        // D-CSUBSET-COMPUTED-GOTO: the payload is a BLOCK id that this rebuild
        // RE-NUMBERS — a verbatim `addInst` copy would carry the OLD id and point
        // the address at the wrong/elided block (the FC7 clone-site silent-
        // miscompile class). Re-map through blockMap_ (honoring a policy's
        // redirectBlockTarget, exactly as the terminator successors do). MF-B
        // guarantees an address-taken target is never elided/merged, so it is in
        // the map; abort loud if not (a policy bug) rather than miscompile.
        MirBlockId const oldTarget = src_.blockAddressTarget(oldId);
        MirBlockId const redirected = active_->redirectBlockTarget(oldTarget);
        auto const it = blockMap_.find(redirected.v);
        if (it == blockMap_.end()) {
            rebuildFatal(kRebuilderSubject, policy_.passName(),
                std::format("BlockAddress target old v={} (redirected v={}) not "
                            "in blockMap_ — an address-taken block was "
                            "elided/merged (MF-B guard missing in this pass). "
                            "Originating BlockAddress: old MirInstId v={}.",
                            oldTarget.v, redirected.v, oldId.v));
        }
        MirInstId const newId = dst_.addBlockAddress(it->second, src_.instType(oldId),
                                                     src_.instFlags(oldId));
        rewrite_.emplace(oldId.v, newId);
        return;
    }

    // Per-pass full-inst-replacement hook. Returns nullopt → verbatim
    // copy. ConstFold emits a Const for foldable expressions; CopyProp
    // uses substituteOldOperand instead so the dead Phi stays for DCE.
    if (auto rewritten = active_->tryRewrite(op, oldId, dst_, rewrite_);
        rewritten.has_value()) {
        rewrite_.emplace(oldId.v, *rewritten);
        return;
    }

    // Inline-asm P5: an `InlineAsm`'s payload indexes the SOURCE module's
    // descriptor pool, and this rebuild is building a DIFFERENT module whose pool
    // starts empty -- forwarding the index below would name the wrong descriptor
    // or none, i.e. silently drop the template and the clobber list. Re-add the
    // descriptor to the destination instead. `MirBuilder::addInst` REFUSES the
    // opcode, so deleting this arm aborts rather than miscompiling.
    if (op == MirOpcode::InlineAsm) {
        auto const asmOps = src_.instOperands(oldId);
        std::vector<MirInstId> newAsmOps;
        newAsmOps.reserve(asmOps.size());
        for (auto a : asmOps) newAsmOps.push_back(mapOperand(a));
        // ★ WHOLE, BY VALUE — never field-by-field. `tests/opt/
        // test_inline_asm_rebuild_carriage.cpp` pins `isExtended` and the inputs'
        // `tiedOutput`; ✔MEASURED 2026-08-17 that a field-by-field copy omitting
        // those two reds both pins at this site. That is the silent-drop class
        // `mir_asm_descriptor.hpp` guards, and it is why this call passes the
        // descriptor whole rather than reconstructing it.
        MirInstId const newId = dst_.addInlineAsm(src_.asmDescriptor(oldId), newAsmOps,
                                                  src_.instType(oldId),
                                                  src_.instFlags(oldId));
        rewrite_.emplace(oldId.v, newId);
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
                                         src_.instFlags(oldId),
                                         // D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN:
                                         // preserve the Alloca's effective-
                                         // alignment channel across every MIR
                                         // rebuild (else a release pipeline drops
                                         // the over-alignment → silent under-align).
                                         src_.instPayload2(oldId));
    rewrite_.emplace(oldId.v, newId);
}

void MirFunctionRebuilder::emitTerminator(MirOpcode op, MirInstId oldId) {
    auto const oldOps  = src_.instOperands(oldId);
    auto const oldBlk  = src_.instBlock(oldId);
    auto const oldSucc = src_.blockSuccessors(oldBlk);
    // ★★★ UNCONDITIONAL, AND THE `if` THAT USED TO GUARD IT WAS A PROCESS ABORT
    // ON A LEGAL PROGRAM (D-OPT-ASM-GOTO-WITH-OUTPUTS-ABORTS-THE-MIR-REBUILDER).
    // A `ReturnPiece` ANCHORS to its producer through its one operand, and an
    // `asm goto` with outputs makes that producer the block's TERMINATOR — so a
    // policy answering "nothing reads a terminator as an operand" was answering
    // a question about the MIR, wrongly, from a place that cannot see it. The
    // full reasoning (and why neither `false` answer survived measurement) is in
    // the header where the hook used to be declared.
    auto remember = [&](MirInstId newId) {
        rewrite_.emplace(oldId.v, newId);
    };
    // Per-terminator full-replacement hook (branch-folding etc.).
    // Returning a value short-circuits the standard emit arms.
    if (auto const rewritten = active_->tryRewriteTerminator(
            op, oldId, dst_, rewrite_, blockMap_); rewritten.has_value()) {
        remember(*rewritten);
        return;
    }
    auto mapSucc = [&](MirBlockId oldS) -> MirBlockId {
        MirBlockId const redirected = active_->redirectBlockTarget(oldS);
        auto const it = blockMap_.find(redirected.v);
        if (it == blockMap_.end()) {
            rebuildFatal(kRebuilderSubject, policy_.passName(),
                std::format("emitTerminator successor old v={} (redirected to "
                            "v={}) not in blockMap_ — either `selectBlocks` "
                            "omitted a reachable block OR a policy's "
                            "`redirectBlockTarget` returned an elided block. "
                            "Originating terminator: old MirInstId v={}.",
                            oldS.v, redirected.v, oldId.v));
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
            // FC7 C1c: a by-value struct returned IN REGISTERS carries N eightbyte/
            // HFA PIECE operands (every operand is a return-register value), not one.
            // Map EVERY operand — taking only oldOps[0] silently dropped pieces
            // 1..N-1, a miscompile masked on x86_64 (the dropped piece's value often
            // still aliased its arg register at the return register) but exposed on
            // AAPCS64's distinct arg/return mapping. `addReturnMulti` covers 0/1/N.
            std::vector<MirInstId> retVals;
            retVals.reserve(oldOps.size());
            for (MirInstId const o : oldOps) retVals.push_back(mapOperand(o));
            MirInstId const newId = dst_.addReturnMulti(retVals);
            remember(newId);
            return;
        }
        case MirOpcode::Unreachable: {
            MirInstId const newId = dst_.addUnreachable();
            remember(newId);
            return;
        }
        case MirOpcode::IndirectBr: {
            // D-CSUBSET-COMPUTED-GOTO (MF-A, 2nd clone site): re-map the address
            // operand AND every address-taken successor through mapSucc (dropping a
            // successor deletes a live `&&label` edge → reachability/DCE prune it).
            MirInstId const addr = mapOperand(oldOps[0]);
            std::vector<MirBlockId> targets;
            targets.reserve(oldSucc.size());
            for (MirBlockId const s : oldSucc) targets.push_back(mapSucc(s));
            MirInstId const newId = dst_.addIndirectBr(addr, targets);
            remember(newId);
            return;
        }
        case MirOpcode::SehTryBegin: {
            // c115 SEH (D-WIN64-SEH-FUNCLETS): succs [tryEntry, filterEntry];
            // the region-id payload clones VERBATIM (region ids are function-
            // scoped and rebuilds are whole-function, so no renumbering).
            MirInstId const newId = dst_.addSehTryBegin(
                mapSucc(oldSucc[0]), mapSucc(oldSucc[1]),
                src_.instPayload(oldId));
            remember(newId);
            return;
        }
        case MirOpcode::InlineAsmGoto: {
            // Inline-asm P5. The result pieces live at the heads of the successor
            // blocks, which the rebuilder clones as ORDINARY blocks -- so the edge
            // structure comes across through `mapSucc` and nothing needs to be
            // re-placed. `cloneInlineAsmGoto` (not `addInlineAsmGoto`) is what says
            // that: re-running the placement rule here would interpose a second
            // landing block on every optimizer pass.
            std::vector<MirInstId> newOps;
            newOps.reserve(oldOps.size());
            for (MirInstId const a : oldOps) newOps.push_back(mapOperand(a));
            std::vector<MirBlockId> succs;
            succs.reserve(oldSucc.size());
            for (MirBlockId const sB : oldSucc) succs.push_back(mapSucc(sB));
            MirInstId const newId = dst_.cloneInlineAsmGoto(
                src_.asmDescriptor(oldId), newOps, succs, src_.instFlags(oldId));
            remember(newId);
            return;
        }
        case MirOpcode::SehFilterReturn: {
            // operand [filterValue i32]; succ [handlerEntry]; payload verbatim.
            MirInstId const newId = dst_.addSehFilterReturn(
                mapOperand(oldOps[0]), mapSucc(oldSucc[0]),
                src_.instPayload(oldId));
            remember(newId);
            return;
        }
        default:
            rebuildFatal(kRebuilderSubject, policy_.passName(),
                std::format("emitTerminator: MirOpcode {} marked isTerminator "
                            "but no clone arm — add an arm here when "
                            "introducing a new terminator opcode.",
                            static_cast<int>(op)));
    }
}

} // namespace dss::opt::passes
