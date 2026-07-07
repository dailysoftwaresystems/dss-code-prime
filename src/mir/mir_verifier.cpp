#include "mir/mir_verifier.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir_cfg.hpp"   // shared mirReversePostOrder
#include "mir/mir_dom.hpp"   // shared computeMirDomTree + mirBuildPredecessors
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"  // deriveStructCfMarkers + structCfMarkerName

#include <format>
#include <unordered_set>
#include <vector>

namespace dss {

namespace {

// Centralized diagnostic emission. The node-kind-typed overloads
// format the "mir inst/block/func #N" prefix once so callsites don't
// re-format the same "actual" prefix every time. Future-proof for the
// pending MirSourceMap injection (the optional `MirSourceMap const*`
// can be added here without touching the rules).
void report(DiagnosticReporter& reporter, DiagnosticCode code,
            std::string actual) {
    ParseDiagnostic d;
    d.code     = code;
    d.severity = DiagnosticSeverity::Error;
    d.actual   = std::move(actual);
    reporter.report(std::move(d));
}

void reportInst(DiagnosticReporter& reporter, DiagnosticCode code,
                MirInstId id, std::string detail) {
    report(reporter, code, std::format("mir inst #{}: {}", id.v, detail));
}

void reportBlock(DiagnosticReporter& reporter, DiagnosticCode code,
                 MirBlockId id, std::string detail) {
    report(reporter, code, std::format("mir block #{}: {}", id.v, detail));
}

void reportFunc(DiagnosticReporter& reporter, DiagnosticCode code,
                MirFuncId id, std::string detail) {
    report(reporter, code, std::format("mir func #{}: {}", id.v, detail));
}

// Iterate over real instruction slots (slot 0 is the sentinel).
// Strong-id constructor is `(value, arenaTag)` — value first.
template <typename Fn>
void forEachInst(Mir const& mir, Fn fn) {
    for (std::uint32_t i = 1; i < mir.instCount(); ++i) {
        fn(MirInstId{i, mir.id().v});
    }
}

// Iterate over real block slots (slot 0 is the sentinel).
template <typename Fn>
void forEachBlock(Mir const& mir, Fn fn) {
    for (std::uint32_t i = 1; i < mir.blockCount(); ++i) {
        fn(MirBlockId{i, mir.id().v});
    }
}

} // namespace

bool MirVerifier::verify(DiagnosticReporter& reporter) const {
    std::size_t const errorsBefore = reporter.errorCount();
    checkStructuralInvariants(reporter);
    checkEntryBlocks(reporter);
    checkBlockTermination(reporter);
    checkPhiIncomings(reporter);
    checkSehStructure(reporter);
    // StructCfMarker equality lives INSIDE checkDomination — the
    // derivation needs the same per-function preds/RPO/dom the
    // use-dom-def scan computes, so they share one computation.
    checkDomination(reporter);
    checkTypeInvariants(reporter);
    if (reporter.hitCap()) return false;
    return reporter.errorCount() == errorsBefore;
}

void MirVerifier::checkStructuralInvariants(DiagnosticReporter& reporter) const {
    forEachInst(mir_, [&](MirInstId id) {
        MirOpcode const op = mir_.instOpcode(id);
        if (op == MirOpcode::Invalid) {
            reportInst(reporter, DiagnosticCode::I_VerifierFailure, id,
                "Invalid opcode");
            return;
        }
        MirOpcodeInfo const& info = opcodeInfo(op);
        if (op != MirOpcode::Phi) {
            auto operands = mir_.instOperands(id);
            // c70 (D-MIR-VERIFIER-UNBOUNDED-OPERAND-SENTINEL): a VARIADIC-operand
            // opcode declares `maxOperands == kMirUnboundedOperands` (0xFF); the
            // upper bound does NOT apply to it — mirror the builder's exemption
            // (mir.cpp addInst: `maxOperands != kMirUnboundedOperands && count >
            // maxOperands`). Without this the verifier read the 0xFF sentinel as a
            // literal max of 255 and rejected a legitimately-variadic Switch/
            // IndirectBr with >255 operands (sqlite has a switch with ~348 cases →
            // 349 operands) that the builder had already accepted.
            bool const overMax = info.maxOperands != kMirUnboundedOperands
                                 && operands.size() > info.maxOperands;
            if (operands.size() < info.minOperands || overMax) {
                reportInst(reporter, DiagnosticCode::I_VerifierFailure, id,
                    std::format("opcode {} operand count {} outside [{}, {}]",
                        info.mnemonic, operands.size(),
                        info.minOperands, info.maxOperands));
            }
        }
        bool const hasType = mir_.instType(id).valid();
        if (info.result == MirResultRule::Value && !hasType) {
            reportInst(reporter, DiagnosticCode::I_VerifierFailure, id,
                std::format("opcode {} is value-producing but has invalid typeId",
                    info.mnemonic));
        } else if (info.result == MirResultRule::None && hasType) {
            reportInst(reporter, DiagnosticCode::I_VerifierFailure, id,
                std::format("opcode {} is value-less but carries a typeId",
                    info.mnemonic));
        }
        if (op == MirOpcode::Const) {
            std::uint32_t const idx = mir_.instPayload(id);
            if (idx >= mir_.literalPool().size()) {
                reportInst(reporter, DiagnosticCode::I_VerifierFailure, id,
                    std::format("const payload {} out of literal-pool range [0, {})",
                        idx, mir_.literalPool().size()));
            }
        }
    });
    // CFG-successor range validation. `mirBuildPredecessors` (the
    // shared dom helper) silently skips out-of-range successor edges
    // — the diagnostic is the verifier's responsibility, emitted
    // here once per bad edge so downstream consumers see the actual
    // corruption (the edge), not a cascade of follow-on phi / dom
    // failures. The check was previously embedded in `buildPredecessors`
    // before extraction to `mir_dom.hpp` (D-OPT-DOMTREE-EXTRACTION).
    std::size_t const blockCount = mir_.blockCount();
    for (std::uint32_t i = 1; i < blockCount; ++i) {
        MirBlockId const from{i, mir_.id().v};
        for (MirBlockId const to : mir_.blockSuccessors(from)) {
            if (to.v >= blockCount) {
                reportBlock(reporter, DiagnosticCode::I_VerifierFailure, from,
                    std::format("mir cfg edge #{} → #{}: successor block "
                                "out of range (blockCount = {})",
                                from.v, to.v, blockCount));
            }
        }
    }
}

void MirVerifier::checkEntryBlocks(DiagnosticReporter& reporter) const {
    for (std::uint32_t fi = 0; fi < mir_.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir_.funcAt(fi);
        std::uint32_t const nBlocks = mir_.funcBlockCount(f);
        if (nBlocks == 0) {
            reportFunc(reporter, DiagnosticCode::I_NoEntryBlock, f,
                "function has zero blocks");
            continue;
        }
        std::uint32_t entryCount = 0;
        MirBlockId firstEntry{};
        for (std::uint32_t bi = 0; bi < nBlocks; ++bi) {
            MirBlockId const b = mir_.funcBlockAt(f, bi);
            if (mir_.blockMarker(b) == StructCfMarker::EntryBlock) {
                ++entryCount;
                if (!firstEntry.valid()) firstEntry = b;
            }
        }
        if (entryCount == 0) {
            reportFunc(reporter, DiagnosticCode::I_NoEntryBlock, f,
                "no block marked EntryBlock");
        } else if (entryCount > 1) {
            reportFunc(reporter, DiagnosticCode::I_MultipleEntryBlocks, f,
                std::format("{} blocks marked EntryBlock (expected exactly 1)",
                    entryCount));
        } else {
            MirBlockId const slot0 = mir_.funcBlockAt(f, 0);
            if (slot0.v != firstEntry.v) {
                reportFunc(reporter, DiagnosticCode::I_EntryBlockNotFirst, f,
                    std::format("EntryBlock is #{}, but funcBlockAt(f, 0) is #{}",
                        firstEntry.v, slot0.v));
            }
        }
    }
}

void MirVerifier::checkBlockTermination(DiagnosticReporter& reporter) const {
    forEachBlock(mir_, [&](MirBlockId b) {
        std::uint32_t const n = mir_.blockInstCount(b);
        if (n == 0) {
            reportBlock(reporter, DiagnosticCode::I_BlockNotTerminated, b,
                "block is empty (no terminator)");
            return;
        }
        MirInstId const last = mir_.blockInstAt(b, n - 1);
        if (!isTerminator(mir_.instOpcode(last))) {
            reportBlock(reporter, DiagnosticCode::I_BlockNotTerminated, b,
                std::format("last inst #{} opcode {} is not a terminator",
                    last.v, opcodeInfo(mir_.instOpcode(last)).mnemonic));
        }
    });
}

void MirVerifier::checkPhiIncomings(DiagnosticReporter& reporter) const {
    auto preds = mirBuildPredecessors(mir_);
    forEachInst(mir_, [&](MirInstId id) {
        if (mir_.instOpcode(id) != MirOpcode::Phi) return;
        MirBlockId const phiBlock = mir_.instBlock(id);
        if (phiBlock.v >= preds.size()) {
            reportInst(reporter, DiagnosticCode::I_VerifierFailure, id,
                std::format("phi's enclosing block #{} is out of range",
                    phiBlock.v));
            return;
        }
        auto const& blockPreds = preds[phiBlock.v];
        std::unordered_set<std::uint32_t> predSet;
        for (auto p : blockPreds) predSet.insert(p.v);
        for (MirPhiIncoming const& inc : mir_.phiIncomings(id)) {
            if (!predSet.contains(inc.pred.v)) {
                reportInst(reporter, DiagnosticCode::I_PhiPredNotInCfg, id,
                    std::format("(phi in block #{}) names predecessor #{} but "
                                "that block is not a CFG-predecessor",
                        phiBlock.v, inc.pred.v));
            }
        }
    });
}

// c115 SEH (D-WIN64-SEH-FUNCLETS): the region-skeleton pairing rules. Runs
// verify-after-every-pass, so an optimizer transform that damages the skeleton
// (SimplifyCfg merging a filter/handler block, a rebuild dropping a marker's
// pairing) reds AT the pass that did it. Zero-cost when no SehTryBegin exists.
void MirVerifier::checkSehStructure(DiagnosticReporter& reporter) const {
    // One flat scan; bail before building predecessors when SEH-free.
    bool anySeh = false;
    forEachInst(mir_, [&](MirInstId id) {
        if (mir_.instOpcode(id) == MirOpcode::SehTryBegin) anySeh = true;
    });
    if (!anySeh) return;

    auto const preds = mirBuildPredecessors(mir_);
    auto predCount = [&](MirBlockId b) -> std::size_t {
        return b.v < preds.size() ? preds[b.v].size() : 0u;
    };

    for (std::uint32_t fi = 0; fi < mir_.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir_.funcAt(fi);
        // Gather the function's region ids (SehTryBegin payloads) + check the
        // per-Begin skeleton; then validate End pairing + Code/Info containment.
        std::unordered_set<std::uint32_t> regionIds;
        bool fnHasBegin = false;
        for (std::uint32_t bi = 0; bi < mir_.funcBlockCount(f); ++bi) {
            MirBlockId const b = mir_.funcBlockAt(f, bi);
            std::uint32_t const n = mir_.blockInstCount(b);
            if (n == 0) continue;
            MirInstId const term = mir_.blockInstAt(b, n - 1);
            if (mir_.instOpcode(term) != MirOpcode::SehTryBegin) continue;
            fnHasBegin = true;
            std::uint32_t const region = mir_.instPayload(term);
            regionIds.insert(region);

            auto const succs = mir_.blockSuccessors(b);
            if (succs.size() != 2) continue;   // structural check owns arity
            MirBlockId const filterBB = succs[1];
            if (predCount(filterBB) != 1) {
                reportInst(reporter, DiagnosticCode::I_SehStructure, term,
                    std::format("SEH region {}: filter block #{} must have "
                                "exactly one predecessor (its SehTryBegin), "
                                "found {}",
                        region, filterBB.v, predCount(filterBB)));
                continue;
            }
            std::uint32_t const fn2 = mir_.blockInstCount(filterBB);
            MirInstId const fterm = fn2 > 0
                ? mir_.blockInstAt(filterBB, fn2 - 1) : MirInstId{};
            if (!fterm.valid()
                || mir_.instOpcode(fterm) != MirOpcode::SehFilterReturn) {
                reportInst(reporter, DiagnosticCode::I_SehStructure, term,
                    std::format("SEH region {}: filter block #{} must terminate "
                                "in SehFilterReturn", region, filterBB.v));
                continue;
            }
            if (mir_.instPayload(fterm) != region) {
                reportInst(reporter, DiagnosticCode::I_SehStructure, fterm,
                    std::format("SehFilterReturn payload {} does not match its "
                                "SehTryBegin region {}",
                        mir_.instPayload(fterm), region));
            }
            auto const fsuccs = mir_.blockSuccessors(filterBB);
            if (fsuccs.size() == 1 && predCount(fsuccs[0]) != 1) {
                reportInst(reporter, DiagnosticCode::I_SehStructure, fterm,
                    std::format("SEH region {}: handler block #{} must have "
                                "exactly one predecessor (its filter), found {}",
                        region, fsuccs[0].v, predCount(fsuccs[0])));
            }
        }
        // SehTryEnd pairing + intrinsic containment (per function).
        for (std::uint32_t bi = 0; bi < mir_.funcBlockCount(f); ++bi) {
            MirBlockId const b = mir_.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < mir_.blockInstCount(b); ++ii) {
                MirInstId const inst = mir_.blockInstAt(b, ii);
                MirOpcode const op = mir_.instOpcode(inst);
                if (op == MirOpcode::SehTryEnd
                    && !regionIds.contains(mir_.instPayload(inst))) {
                    reportInst(reporter, DiagnosticCode::I_SehStructure, inst,
                        std::format("SehTryEnd names region {} but this function "
                                    "has no SehTryBegin with that id",
                            mir_.instPayload(inst)));
                }
                if ((op == MirOpcode::SehExceptionCode
                     || op == MirOpcode::SehExceptionInfo)
                    && !fnHasBegin) {
                    reportInst(reporter, DiagnosticCode::I_SehStructure, inst,
                        "SehExceptionCode/Info in a function with no SehTryBegin "
                        "(the HIR-tier context rule should have rejected this)");
                }
            }
        }
    }
}

void MirVerifier::checkDomination(DiagnosticReporter& reporter) const {
    auto preds = mirBuildPredecessors(mir_);
    for (std::uint32_t fi = 0; fi < mir_.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir_.funcAt(fi);
        if (mir_.funcBlockCount(f) == 0) continue;
        MirBlockId const entry = mir_.funcEntry(f);
        if (!entry.valid()) {
            reportFunc(reporter, DiagnosticCode::I_NoEntryBlock, f,
                "funcEntry() returned InvalidMirBlock; skipping dominance check");
            continue;
        }
        auto rpo  = mirReversePostOrder(mir_, entry);
        MirDomTree const domState = computeMirDomTree(mir_, entry, rpo, preds);
        // Emit `I_VerifierFailure` for every block whose idom couldn't
        // be computed (intersect bailed). Without this signal the
        // caller would silently see an under-conservative idom and
        // miss real use-dom-def violations on that block's operands.
        for (std::uint32_t bi = 0; bi < domState.gaveUp.size(); ++bi) {
            if (!domState.gaveUp[bi]) continue;
            reportBlock(reporter, DiagnosticCode::I_VerifierFailure,
                MirBlockId{bi, mir_.id().v},
                "dominator analysis gave up (idom intersect bailed — input "
                "likely has an idom cycle from direct-`Mir`-ctor construction)");
        }
        // Vector-indexed same-block-position map (replaces unordered_map
        // for a tighter inner loop). Slot 0 unused.
        std::vector<std::uint32_t> indexInBlock(mir_.instCount(),
            static_cast<std::uint32_t>(-1));
        for (MirBlockId const b : rpo) {
            std::uint32_t const n = mir_.blockInstCount(b);
            for (std::uint32_t i = 0; i < n; ++i) {
                indexInBlock[mir_.blockInstAt(b, i).v] = i;
            }
        }
        // Block LAYOUT-position map (funcBlockAt order — the order the
        // MIR→LIR lowering + every MirFunctionRebuilder emits blocks).
        // Indexed by block .v; slot 0 unused. Drives the layout rule
        // below: a cross-block operand whose definition DOMINATES its use
        // (SSA-legal) must ALSO be laid out before it, or no linear
        // consumer can resolve it (D-OPT2 layout contract, I0010).
        std::vector<std::uint32_t> layoutPos(mir_.blockCount(),
            static_cast<std::uint32_t>(-1));
        {
            std::uint32_t const nBlocksF = mir_.funcBlockCount(f);
            for (std::uint32_t bi = 0; bi < nBlocksF; ++bi) {
                layoutPos[mir_.funcBlockAt(f, bi).v] = bi;
            }
        }
        // Orphan-block diagnostic: any block in the function that is
        // NOT in `rpo` is unreachable from entry. ExitBlock + LoopExit
        // / IfJoin blocks ARE reachable via the CFG (their preds carry
        // br/condbr), so genuine reachable blocks pass; only orphans
        // fail.
        std::unordered_set<std::uint32_t> reachable;
        for (MirBlockId const b : rpo) reachable.insert(b.v);
        std::uint32_t const nBlocks = mir_.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nBlocks; ++bi) {
            MirBlockId const b = mir_.funcBlockAt(f, bi);
            if (!reachable.contains(b.v)) {
                reportBlock(reporter, DiagnosticCode::I_UnreachableBlock, b,
                    std::format("block in func #{} is not reachable from entry",
                        f.v));
            }
        }
        // StructCfMarker equality (the derivation model, D-OPT4-1): the
        // verifier RECOMPUTES the canonical derivation independently —
        // never trusting a producer-supplied vector — and requires
        // stored == derived for every REACHABLE block. PLACEMENT
        // PRINCIPLE: producers rederive at their own sites (lowering /
        // SimplifyCfg / inliner / merge call rederiveStructCfMarkers
        // after finish()); a central rederive-before-verify here would
        // make this equality tautological. Unreachable blocks are
        // skipped — I_UnreachableBlock (above) owns them. This subsumed
        // the old count-parity switch, the ExitBlock-terminator rule,
        // and the LoopHeader-back-edge rule (a no-back-edge "header" now
        // simply derives non-LoopHeader).
        // Sharing: the derivation reuses THIS function's preds/rpo/dom
        // (one computation per function per verify; the post-dominator
        // tree is the only addition, built inside the derivation).
        {
            auto const derived = deriveStructCfMarkers(mir_, f, preds, rpo, domState);
            for (MirBlockId const b : rpo) {
                StructCfMarker const stored = mir_.blockMarker(b);
                if (b.v >= derived.size()) continue;  // defensive — derived is blockCount-sized
                if (stored != derived[b.v]) {
                    reportBlock(reporter, DiagnosticCode::I_StructCfMismatch, b,
                        std::format("stored marker {} != derived marker {} "
                                    "(markers must equal the canonical CFG "
                                    "derivation - mir_struct_markers.hpp)",
                            structCfMarkerName(stored),
                            structCfMarkerName(derived[b.v])));
                }
            }
        }
        // Use-dom-def scan over reachable blocks.
        for (MirBlockId const useBlock : rpo) {
            std::uint32_t const n = mir_.blockInstCount(useBlock);
            for (std::uint32_t i = 0; i < n; ++i) {
                MirInstId const use = mir_.blockInstAt(useBlock, i);
                MirOpcode const useOp = mir_.instOpcode(use);
                if (useOp == MirOpcode::Phi) {
                    for (MirPhiIncoming const& inc : mir_.phiIncomings(use)) {
                        if (!inc.value.valid()) continue;
                        MirBlockId const defBlock = mir_.instBlock(inc.value);
                        MirDomResult const dr = mirDominatesBlock(defBlock, inc.pred, domState);
                        if (dr == MirDomResult::DoesNot) {
                            reportInst(reporter, DiagnosticCode::I_NotDominated, use,
                                std::format("(phi in block #{}) incoming value "
                                            "#{} defined in block #{} does not "
                                            "dominate predecessor block #{}",
                                    useBlock.v, inc.value.v,
                                    defBlock.v, inc.pred.v));
                        } else if (dr == MirDomResult::GaveUp) {
                            reportInst(reporter, DiagnosticCode::I_VerifierFailure, use,
                                std::format("dominance check aborted for phi-"
                                            "incoming value #{} against pred #{} "
                                            "(idom chain step-cap exceeded)",
                                    inc.value.v, inc.pred.v));
                        }
                    }
                    continue;
                }
                auto operands = mir_.instOperands(use);
                for (MirInstId const op : operands) {
                    if (!op.valid()) continue;
                    MirBlockId const defBlock = mir_.instBlock(op);
                    if (defBlock.v == useBlock.v) {
                        std::uint32_t const defIdx =
                            (op.v < indexInBlock.size())
                                ? indexInBlock[op.v]
                                : static_cast<std::uint32_t>(-1);
                        if (defIdx != static_cast<std::uint32_t>(-1)
                         && defIdx >= i) {
                            reportInst(reporter, DiagnosticCode::I_NotDominated, use,
                                std::format("uses value #{} defined later in the "
                                            "same block #{} (use at index {}, "
                                            "def at index {})",
                                    op.v, useBlock.v, i, defIdx));
                        }
                    } else {
                        MirDomResult const dr = mirDominatesBlock(defBlock, useBlock, domState);
                        if (dr == MirDomResult::DoesNot) {
                            reportInst(reporter, DiagnosticCode::I_NotDominated, use,
                                std::format("uses value #{} defined in block #{} "
                                            "which does not dominate use block #{}",
                                    op.v, defBlock.v, useBlock.v));
                        } else if (dr == MirDomResult::GaveUp) {
                            reportInst(reporter, DiagnosticCode::I_VerifierFailure, use,
                                std::format("dominance check aborted for value "
                                            "#{} (def block #{}, use block #{}) "
                                            "— idom chain step-cap exceeded",
                                    op.v, defBlock.v, useBlock.v));
                        } else {
                            // LAYOUT RULE (I_LayoutUseBeforeDef, D-OPT2
                            // layout contract). GATED on Dominates: a
                            // non-dominating def already reported above —
                            // one bad operand must not double-report. When
                            // the def DOES dominate (SSA-legal), the linear
                            // lowering ALSO requires it to be laid out
                            // before the use. Phi incomings are EXEMPT
                            // (handled by the Phi arm above): a loop
                            // back-edge legitimately carries a def whose
                            // layout FOLLOWS the use, and the dominance arm
                            // owns that semantics — only NON-Phi linear
                            // operands flow here. Defensive index guard:
                            // both blocks are reachable (in rpo ⊆ this
                            // function), so both have a valid layoutPos.
                            std::uint32_t const defPos =
                                (defBlock.v < layoutPos.size())
                                    ? layoutPos[defBlock.v]
                                    : static_cast<std::uint32_t>(-1);
                            std::uint32_t const usePos =
                                (useBlock.v < layoutPos.size())
                                    ? layoutPos[useBlock.v]
                                    : static_cast<std::uint32_t>(-1);
                            if (defPos != static_cast<std::uint32_t>(-1)
                             && usePos != static_cast<std::uint32_t>(-1)
                             && defPos >= usePos) {
                                reportInst(reporter,
                                    DiagnosticCode::I_LayoutUseBeforeDef, use,
                                    std::format("uses value #{} defined in block "
                                                "#{} (layout pos {}) which "
                                                "dominates but is laid out at or "
                                                "after use block #{} (layout pos "
                                                "{}) — no linear consumer can "
                                                "resolve a def emitted after its "
                                                "use",
                                        op.v, defBlock.v, defPos,
                                        useBlock.v, usePos));
                            }
                        }
                    }
                }
            }
        }
    }
}

void MirVerifier::checkTypeInvariants(DiagnosticReporter& reporter) const {
    if (interner_ == nullptr) return;
    forEachInst(mir_, [&](MirInstId id) {
        TypeId const t = mir_.instType(id);
        if (!t.valid()) return;
        if (interner_->kind(t) == TypeKind::Extension) {
            reportInst(reporter, DiagnosticCode::I_ExtensionTypeInMir, id,
                std::format("typeId {} resolves to TypeKind::Extension (every "
                            "extension type must be resolved to a core kind at "
                            "the HIR→MIR boundary)",
                    t.v));
        }
    });
    for (std::uint32_t fi = 0; fi < mir_.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir_.funcAt(fi);
        TypeId const fnSig = mir_.funcSignature(f);
        if (!fnSig.valid() || interner_->kind(fnSig) != TypeKind::FnSig) {
            continue;
        }
        // FnSig layout convention (HR4-established, project-wide,
        // language-agnostic): `operands[0]` is the return type;
        // `operands[1..]` are the parameter types. Documented here
        // so the verifier doesn't silently misfire if any future
        // language schema deviates — adding a `TypeInterner::
        // fnSigReturnType()`/`fnSigParamCount()` accessor pair and
        // routing through it is the long-term cure (tracked as a
        // type-lattice followup; tier-2 — no current consumer would
        // benefit).
        auto operands = interner_->operands(fnSig);
        TypeId const returnTy = operands.empty() ? InvalidType : operands[0];
        std::uint32_t const nBlocks = mir_.funcBlockCount(f);
        // FC7 (D-FC7-SYSV-STRUCT-ARG-MULTIREG): the physical-arg count is NOT the
        // FnSig param count — a by-value struct param expands to MULTIPLE register
        // `Arg`s (one per SysV eightbyte / AAPCS64 piece). The `Arg` payload is the
        // PER-CLASS (or, for a slot-aligned CC, flat) physical register ordinal,
        // which HIR→MIR emits with a MONOTONIC per-class counter — so every payload
        // is < the number of `Arg` instructions in the function (a per-class payload
        // < its class count <= the total; a flat payload < the total). Bound the
        // check on THAT count, not the FnSig paramCount, so a multi-register struct
        // param verifies while a stray out-of-range `Arg` is still rejected.
        std::uint32_t argCount = 0;
        // FC7 C3 (AAPCS64/Apple x8 sret): a function that reads the indirect-result
        // register (ReadIndirectResult at entry) is a register-based-sret struct
        // returner — its by-value aggregate result is written THROUGH x8 and the MIR
        // `Return` is legitimately VOID (the SysV/Win64 hidden-arg path instead
        // returns the sret pointer). This op is the CC-config-free marker that lets
        // the return check below accept a void return for a non-void (struct) func.
        bool hasIndirectResultRead = false;
        for (std::uint32_t bi = 0; bi < nBlocks; ++bi) {
            MirBlockId const b = mir_.funcBlockAt(f, bi);
            std::uint32_t const n = mir_.blockInstCount(b);
            for (std::uint32_t i = 0; i < n; ++i) {
                MirOpcode const o = mir_.instOpcode(mir_.blockInstAt(b, i));
                if (o == MirOpcode::Arg) ++argCount;
                else if (o == MirOpcode::ReadIndirectResult)
                    hasIndirectResultRead = true;
            }
        }
        // D-OPT-RELEASE-SYSV-MIXED-CLASS-REG-ARG-DROP: no two Args may share a
        // flat call-operand `position` (arg_payload.hpp). A duplicate is the
        // signature of a payload wipe at a rebuild/merge site (both defaulting
        // to a colliding ordinal), which would make the inliner map two callee
        // params to the same actual. NOT a `position < argCount` check: the
        // x8-sret slot / straddle carrier legitimately consume positions with
        // no Arg, so positions can exceed the Arg count.
        std::unordered_set<std::uint32_t> seenArgPositions;
        for (std::uint32_t bi = 0; bi < nBlocks; ++bi) {
            MirBlockId const b = mir_.funcBlockAt(f, bi);
            std::uint32_t const n = mir_.blockInstCount(b);
            for (std::uint32_t i = 0; i < n; ++i) {
                MirInstId const id = mir_.blockInstAt(b, i);
                MirOpcode const op = mir_.instOpcode(id);
                if (op == MirOpcode::Arg) {
                    std::uint32_t const idx = mir_.argIndex(id);
                    if (idx >= argCount) {
                        reportInst(reporter, DiagnosticCode::I_ArgIndexOutOfRange, id,
                            std::format("argIndex {} >= physical-arg count {} "
                                        "for func #{}",
                                idx, argCount, f.v));
                    }
                    std::uint32_t const pos = mir_.argPosition(id);
                    if (!seenArgPositions.insert(pos).second) {
                        reportInst(reporter, DiagnosticCode::I_ArgPositionDuplicate, id,
                            std::format("two Args share flat call-operand "
                                        "position {} in func #{} — a payload "
                                        "wipe at a rebuild/merge site "
                                        "(D-OPT-RELEASE-SYSV-MIXED-CLASS-REG-"
                                        "ARG-DROP)",
                                pos, f.v));
                    }
                } else if (op == MirOpcode::CondBr) {
                    auto condOps = mir_.instOperands(id);
                    if (!condOps.empty()) {
                        TypeId const ct = mir_.instType(condOps[0]);
                        if (ct.valid() && interner_->kind(ct) != TypeKind::Bool) {
                            reportInst(reporter,
                                DiagnosticCode::I_TerminatorTypeMismatch, id,
                                std::format("(condbr) condition value #{} has "
                                            "type kind {} (expected Bool)",
                                    condOps[0].v,
                                    static_cast<int>(interner_->kind(ct))));
                        }
                    }
                } else if (op == MirOpcode::Return) {
                    auto retOps = mir_.instOperands(id);
                    bool const hasValue  = !retOps.empty();
                    bool const wantValue = returnTy.valid()
                        && interner_->kind(returnTy) != TypeKind::Void;
                    if (hasValue && !wantValue) {
                        reportInst(reporter,
                            DiagnosticCode::I_TerminatorTypeMismatch, id,
                            std::format("(return) has a value but func #{} returns void",
                                f.v));
                    } else if (!hasValue && wantValue && !hasIndirectResultRead) {
                        // x8-sret functions (hasIndirectResultRead) legitimately
                        // return void — the result is written through the indirect-
                        // result register, not returned. Every other non-void func
                        // with a value-less return is a real lowering bug.
                        reportInst(reporter,
                            DiagnosticCode::I_TerminatorTypeMismatch, id,
                            std::format("(return) has no value but func #{} "
                                        "returns a non-void type", f.v));
                    } else if (hasValue && wantValue) {
                        TypeKind const rk = interner_->kind(returnTy);
                        if (rk == TypeKind::Struct || rk == TypeKind::Union) {
                            // FC7 C1c (D-FC7-SYSV-STRUCT-RETURN-IN-REGS): a by-value
                            // struct/union return is EITHER the first-class aggregate
                            // VALUE (a single operand of the return type — the const-
                            // fold / .dssir-text form, never lowered to LIR as-is) OR
                            // the lowered ABI form: N register PIECES (I64/F64) or an
                            // sret POINTER (>16B). Each operand must be one of those —
                            // a DIFFERENT aggregate type is a real mismatch (and a
                            // single struct-typed value reaching codegen is caught at
                            // the HIR→MIR lowering, which always emits pieces/sret).
                            for (MirInstId const opnd : retOps) {
                                TypeId const vt = mir_.instType(opnd);
                                if (!vt.valid() || vt.v == returnTy.v) continue;
                                TypeKind const vk = interner_->kind(vt);
                                if (vk != TypeKind::I64 && vk != TypeKind::F64
                                    && vk != TypeKind::F32 && vk != TypeKind::Ptr) {
                                    reportInst(reporter,
                                        DiagnosticCode::I_TerminatorTypeMismatch, id,
                                        std::format("(return) of by-value struct/union "
                                                    "func #{} must carry the aggregate "
                                                    "value, register pieces (I64/F64), "
                                                    "or an sret pointer, not a kind-{} "
                                                    "value (FC7 C1c)",
                                            f.v, static_cast<int>(vk)));
                                }
                            }
                        } else if (retOps.size() != 1) {
                            reportInst(reporter,
                                DiagnosticCode::I_TerminatorTypeMismatch, id,
                                std::format("(return) of scalar func #{} must carry "
                                            "exactly one value, has {}",
                                    f.v, retOps.size()));
                        } else {
                            TypeId const vt = mir_.instType(retOps[0]);
                            if (vt.valid() && vt.v != returnTy.v) {
                                reportInst(reporter,
                                    DiagnosticCode::I_TerminatorTypeMismatch, id,
                                    std::format("(return) value type {} does not "
                                                "match func #{} return type {}",
                                        vt.v, f.v, returnTy.v));
                            }
                        }
                    }
                }
            }
        }
    }
}

} // namespace dss
