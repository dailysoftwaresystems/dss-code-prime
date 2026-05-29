#include "mir/mir_verifier.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir_opcode.hpp"

#include <algorithm>
#include <format>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dss {

namespace {

// Emit a diagnostic anchored at a MIR instruction or block. MIR has no
// source-map yet (cycle 1 design decision; the optional injection slot
// will be added when a MirSourceMap type lands). Node identity goes
// into `d.actual` so the reporter's dedup key (which folds in `actual`)
// never collapses two distinct violations.
void reportAt(DiagnosticReporter& reporter, DiagnosticCode code,
              std::string actual) {
    ParseDiagnostic d;
    d.code     = code;
    d.severity = DiagnosticSeverity::Error;
    d.actual   = std::move(actual);
    reporter.report(std::move(d));
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

// Build the predecessor adjacency: preds[blockSlot] is the list of
// MirBlockIds that name this block as a successor. O(V + E).
[[nodiscard]] std::vector<std::vector<MirBlockId>>
buildPredecessors(Mir const& mir) {
    std::vector<std::vector<MirBlockId>> preds(mir.blockCount());
    for (std::uint32_t i = 1; i < mir.blockCount(); ++i) {
        MirBlockId const from{i, mir.id().v};
        for (MirBlockId const to : mir.blockSuccessors(from)) {
            if (to.v < preds.size()) preds[to.v].push_back(from);
        }
    }
    return preds;
}

// Reverse post-order over the CFG starting at `entry`. Used by both
// the dominator computation and the use-dom-def reachability discipline.
[[nodiscard]] std::vector<MirBlockId>
reversePostOrder(Mir const& mir, MirBlockId entry) {
    std::vector<MirBlockId> order;
    std::unordered_set<std::uint32_t> visited;
    // Iterative DFS with explicit post-order capture.
    struct Frame { MirBlockId block; std::size_t nextSucc; };
    std::vector<Frame> stack;
    auto push = [&](MirBlockId b) {
        if (visited.insert(b.v).second) stack.push_back({b, 0});
    };
    push(entry);
    while (!stack.empty()) {
        Frame& top = stack.back();
        auto succs = mir.blockSuccessors(top.block);
        if (top.nextSucc < succs.size()) {
            MirBlockId const s = succs[top.nextSucc++];
            push(s);
        } else {
            order.push_back(top.block);
            stack.pop_back();
        }
    }
    std::reverse(order.begin(), order.end());
    return order;
}

// Cooper-Harvey-Kennedy iterative dominators ("A Simple, Fast
// Dominance Algorithm"). Returns `idom[blockSlot] = MirBlockId` mapping
// each reachable block (in `order`) to its immediate dominator (the
// entry block's idom is itself). Unreachable blocks have InvalidMirBlock.
// `rpoIndex[blockSlot]` is the block's index in `order` (used by intersect).
[[nodiscard]] std::vector<MirBlockId>
computeIDoms(Mir const&                                  mir,
             MirBlockId                                  entry,
             std::vector<MirBlockId> const&              order,
             std::vector<std::vector<MirBlockId>> const& preds) {
    std::vector<MirBlockId> idom(mir.blockCount());
    std::vector<std::uint32_t> rpoIndex(mir.blockCount(),
        static_cast<std::uint32_t>(-1));
    for (std::uint32_t i = 0; i < order.size(); ++i) {
        rpoIndex[order[i].v] = i;
    }
    idom[entry.v] = entry;
    auto intersect = [&](MirBlockId b1, MirBlockId b2) {
        MirBlockId finger1 = b1;
        MirBlockId finger2 = b2;
        while (finger1.v != finger2.v) {
            while (rpoIndex[finger1.v] > rpoIndex[finger2.v]) {
                finger1 = idom[finger1.v];
            }
            while (rpoIndex[finger2.v] > rpoIndex[finger1.v]) {
                finger2 = idom[finger2.v];
            }
        }
        return finger1;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        // Iterate in RPO, skipping the entry.
        for (std::size_t i = 1; i < order.size(); ++i) {
            MirBlockId const b = order[i];
            MirBlockId newIdom{};
            for (MirBlockId const p : preds[b.v]) {
                if (rpoIndex[p.v] == static_cast<std::uint32_t>(-1)) continue;
                if (!idom[p.v].valid()) continue;  // pred not yet processed
                if (!newIdom.valid()) {
                    newIdom = p;
                } else {
                    newIdom = intersect(newIdom, p);
                }
            }
            if (newIdom.valid() && idom[b.v].v != newIdom.v) {
                idom[b.v] = newIdom;
                changed = true;
            }
        }
    }
    return idom;
}

// Does `a` dominate `b`? Walks idom-chain from b until reaching a or
// the entry's self-loop. Linear in tree depth.
[[nodiscard]] bool
dominates(MirBlockId a, MirBlockId b, std::vector<MirBlockId> const& idom) {
    if (a.v == b.v) return true;
    MirBlockId cur = b;
    while (idom[cur.v].valid() && idom[cur.v].v != cur.v) {
        cur = idom[cur.v];
        if (cur.v == a.v) return true;
    }
    return false;
}

// True iff `op` may LEGITIMATELY produce no value (the result-rule is
// `None` so the inst's typeId is intentionally Invalid).
[[nodiscard]] bool isValueless(MirOpcode op) noexcept {
    return opcodeInfo(op).result == MirResultRule::None;
}

} // namespace

bool MirVerifier::verify(DiagnosticReporter& reporter) const {
    std::size_t const errorsBefore = reporter.errorCount();
    checkStructuralInvariants(reporter);
    checkEntryBlocks(reporter);
    checkBlockTermination(reporter);
    checkStructCfMarkers(reporter);
    checkPhiIncomings(reporter);
    checkDomination(reporter);
    checkTypeInvariants(reporter);
    if (reporter.hitCap()) return false;
    return reporter.errorCount() == errorsBefore;
}

void MirVerifier::checkStructuralInvariants(DiagnosticReporter& reporter) const {
    // Re-run ML1's opcode/arity/result-rule checks on the frozen module.
    // `MirBuilder` enforces these at build time, but a `Mir` constructed
    // directly (test fixtures, future optimizer) bypasses the builder.
    forEachInst(mir_, [&](MirInstId id) {
        MirOpcode const op = mir_.instOpcode(id);
        if (op == MirOpcode::Invalid) {
            reportAt(reporter, DiagnosticCode::I_VerifierFailure,
                std::format("mir inst #{} has Invalid opcode", id.v));
            return;
        }
        MirOpcodeInfo const& info = opcodeInfo(op);
        // Operand count in [min, max]. Phi uses the phi pool — its
        // operand range covers (value, pred) pairs and is independently
        // bounds-checked when phiIncomings is consumed.
        if (op != MirOpcode::Phi) {
            auto operands = mir_.instOperands(id);
            if (operands.size() < info.minOperands || operands.size() > info.maxOperands) {
                reportAt(reporter, DiagnosticCode::I_VerifierFailure,
                    std::format("mir inst #{} ({}) operand count {} outside [{}, {}]",
                        id.v, info.mnemonic, operands.size(),
                        info.minOperands, info.maxOperands));
            }
        }
        // Result-type rule: R::Value requires a valid typeId; R::None
        // requires Invalid.
        bool const hasType = mir_.instType(id).valid();
        if (info.result == MirResultRule::Value && !hasType) {
            reportAt(reporter, DiagnosticCode::I_VerifierFailure,
                std::format("mir inst #{} ({}) is value-producing but has invalid typeId",
                    id.v, info.mnemonic));
        } else if (info.result == MirResultRule::None && hasType) {
            reportAt(reporter, DiagnosticCode::I_VerifierFailure,
                std::format("mir inst #{} ({}) is value-less but carries a typeId",
                    id.v, info.mnemonic));
        }
        // Const.payload must be a real literal-pool index.
        if (op == MirOpcode::Const) {
            std::uint32_t const idx = mir_.instPayload(id);
            if (idx >= mir_.literalPool().size()) {
                reportAt(reporter, DiagnosticCode::I_VerifierFailure,
                    std::format("mir inst #{} (const) payload {} out of "
                                "literal-pool range [0, {})",
                        id.v, idx, mir_.literalPool().size()));
            }
        }
    });
}

void MirVerifier::checkEntryBlocks(DiagnosticReporter& reporter) const {
    for (std::uint32_t fi = 0; fi < mir_.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir_.funcAt(fi);
        std::uint32_t const nBlocks = mir_.funcBlockCount(f);
        if (nBlocks == 0) {
            reportAt(reporter, DiagnosticCode::I_NoEntryBlock,
                std::format("mir func #{} has zero blocks", f.v));
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
            reportAt(reporter, DiagnosticCode::I_NoEntryBlock,
                std::format("mir func #{} has no block marked EntryBlock", f.v));
        } else if (entryCount > 1) {
            reportAt(reporter, DiagnosticCode::I_MultipleEntryBlocks,
                std::format("mir func #{} has {} blocks marked EntryBlock "
                            "(expected exactly 1)", f.v, entryCount));
        } else {
            MirBlockId const slot0 = mir_.funcBlockAt(f, 0);
            if (slot0.v != firstEntry.v) {
                reportAt(reporter, DiagnosticCode::I_EntryBlockNotFirst,
                    std::format("mir func #{} EntryBlock is block #{}, but "
                                "funcBlockAt(f, 0) is #{}",
                        f.v, firstEntry.v, slot0.v));
            }
        }
    }
}

void MirVerifier::checkBlockTermination(DiagnosticReporter& reporter) const {
    forEachBlock(mir_, [&](MirBlockId b) {
        std::uint32_t const n = mir_.blockInstCount(b);
        if (n == 0) {
            reportAt(reporter, DiagnosticCode::I_BlockNotTerminated,
                std::format("mir block #{} is empty (no terminator)", b.v));
            return;
        }
        MirInstId const last = mir_.blockInstAt(b, n - 1);
        if (!isTerminator(mir_.instOpcode(last))) {
            reportAt(reporter, DiagnosticCode::I_BlockNotTerminated,
                std::format("mir block #{} last inst #{} opcode {} is not a terminator",
                    b.v, last.v, opcodeInfo(mir_.instOpcode(last)).mnemonic));
        }
    });
}

void MirVerifier::checkStructCfMarkers(DiagnosticReporter& reporter) const {
    // Marker presence check: ExitBlock terminates in Return/Unreachable.
    // IfThen/IfElse/IfJoin and LoopHeader/LoopLatch/LoopExit need to
    // co-occur in the same function (cycle 1 weak check — full
    // structural validity needs the dom tree, but absence-of-matching-
    // marker is already a clear violation).
    for (std::uint32_t fi = 0; fi < mir_.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir_.funcAt(fi);
        std::uint32_t const nBlocks = mir_.funcBlockCount(f);
        bool hasIfThen = false, hasIfJoin = false;
        bool hasLoopHeader = false, hasLoopExit = false;
        for (std::uint32_t bi = 0; bi < nBlocks; ++bi) {
            MirBlockId const b = mir_.funcBlockAt(f, bi);
            StructCfMarker const m = mir_.blockMarker(b);
            if (m == StructCfMarker::ExitBlock) {
                std::uint32_t const n = mir_.blockInstCount(b);
                if (n > 0) {
                    MirOpcode const op = mir_.instOpcode(mir_.blockInstAt(b, n - 1));
                    if (op != MirOpcode::Return && op != MirOpcode::Unreachable) {
                        reportAt(reporter, DiagnosticCode::I_StructCfMismatch,
                            std::format("mir block #{} (ExitBlock) terminates in "
                                        "{}; expected Return or Unreachable",
                                b.v, opcodeInfo(op).mnemonic));
                    }
                }
            }
            if (m == StructCfMarker::IfThen)      hasIfThen     = true;
            if (m == StructCfMarker::IfJoin)      hasIfJoin     = true;
            if (m == StructCfMarker::LoopHeader)  hasLoopHeader = true;
            if (m == StructCfMarker::LoopExit)    hasLoopExit   = true;
        }
        if (hasIfThen && !hasIfJoin) {
            reportAt(reporter, DiagnosticCode::I_StructCfMismatch,
                std::format("mir func #{} has IfThen block(s) but no IfJoin", f.v));
        }
        if (hasLoopHeader && !hasLoopExit) {
            reportAt(reporter, DiagnosticCode::I_StructCfMismatch,
                std::format("mir func #{} has LoopHeader block(s) but no LoopExit", f.v));
        }
    }
}

void MirVerifier::checkPhiIncomings(DiagnosticReporter& reporter) const {
    auto preds = buildPredecessors(mir_);
    forEachInst(mir_, [&](MirInstId id) {
        if (mir_.instOpcode(id) != MirOpcode::Phi) return;
        MirBlockId const phiBlock = mir_.instBlock(id);
        auto const& blockPreds = preds[phiBlock.v];
        std::unordered_set<std::uint32_t> predSet;
        for (auto p : blockPreds) predSet.insert(p.v);
        for (MirPhiIncoming const& inc : mir_.phiIncomings(id)) {
            if (!predSet.contains(inc.pred.v)) {
                reportAt(reporter, DiagnosticCode::I_PhiPredNotInCfg,
                    std::format("mir inst #{} (phi in block #{}) names predecessor #{} "
                                "but that block is not a CFG-predecessor",
                        id.v, phiBlock.v, inc.pred.v));
            }
        }
    });
}

void MirVerifier::checkDomination(DiagnosticReporter& reporter) const {
    auto preds = buildPredecessors(mir_);
    // Per-function dominator tree + use-dom-def scan.
    for (std::uint32_t fi = 0; fi < mir_.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir_.funcAt(fi);
        if (mir_.funcBlockCount(f) == 0) continue;
        MirBlockId const entry = mir_.funcEntry(f);
        auto rpo  = reversePostOrder(mir_, entry);
        auto idom = computeIDoms(mir_, entry, rpo, preds);
        // Per-block instruction index (slot within block). Used to
        // check "if def and use are in the same block, def precedes use".
        std::unordered_map<std::uint32_t, std::uint32_t> instIndexInBlock;
        for (MirBlockId const b : rpo) {
            std::uint32_t const n = mir_.blockInstCount(b);
            for (std::uint32_t i = 0; i < n; ++i) {
                instIndexInBlock[mir_.blockInstAt(b, i).v] = i;
            }
        }
        // For each instruction, every value operand's defining block
        // must dominate the use block; if same block, def precedes use.
        for (MirBlockId const useBlock : rpo) {
            std::uint32_t const n = mir_.blockInstCount(useBlock);
            for (std::uint32_t i = 0; i < n; ++i) {
                MirInstId const use = mir_.blockInstAt(useBlock, i);
                MirOpcode const useOp = mir_.instOpcode(use);
                // Phi operands are tied to their predecessor edges; the
                // use is "at the end of the predecessor block", not at
                // the phi's position. Check that each incoming.value's
                // defining block dominates incoming.pred (or matches it
                // with def before pred's terminator).
                if (useOp == MirOpcode::Phi) {
                    for (MirPhiIncoming const& inc : mir_.phiIncomings(use)) {
                        if (!inc.value.valid()) continue;
                        MirBlockId const defBlock = mir_.instBlock(inc.value);
                        if (!dominates(defBlock, inc.pred, idom)) {
                            reportAt(reporter, DiagnosticCode::I_NotDominated,
                                std::format("mir inst #{} (phi in block #{}) "
                                            "incoming value #{} defined in block #{} "
                                            "does not dominate predecessor block #{}",
                                    use.v, useBlock.v, inc.value.v,
                                    defBlock.v, inc.pred.v));
                        }
                    }
                    continue;
                }
                // Regular operands.
                auto operands = mir_.instOperands(use);
                for (MirInstId const op : operands) {
                    if (!op.valid()) continue;
                    MirBlockId const defBlock = mir_.instBlock(op);
                    if (defBlock.v == useBlock.v) {
                        // Same-block: def index must be < use index.
                        auto defIdx = instIndexInBlock.find(op.v);
                        if (defIdx != instIndexInBlock.end() && defIdx->second >= i) {
                            reportAt(reporter, DiagnosticCode::I_NotDominated,
                                std::format("mir inst #{} uses value #{} defined "
                                            "later in the same block #{} "
                                            "(use at index {}, def at index {})",
                                    use.v, op.v, useBlock.v, i, defIdx->second));
                        }
                    } else if (!dominates(defBlock, useBlock, idom)) {
                        reportAt(reporter, DiagnosticCode::I_NotDominated,
                            std::format("mir inst #{} uses value #{} defined in "
                                        "block #{} which does not dominate use "
                                        "block #{}",
                                use.v, op.v, defBlock.v, useBlock.v));
                    }
                }
            }
        }
    }
}

void MirVerifier::checkTypeInvariants(DiagnosticReporter& reporter) const {
    if (interner_ == nullptr) return;
    // No TypeKind::Extension types in MIR — every extension type must
    // have been resolved to a core lattice kind at the HIR→MIR boundary.
    forEachInst(mir_, [&](MirInstId id) {
        TypeId const t = mir_.instType(id);
        if (!t.valid()) return;
        if (interner_->kind(t) == TypeKind::Extension) {
            reportAt(reporter, DiagnosticCode::I_ExtensionTypeInMir,
                std::format("mir inst #{} typeId {} resolves to TypeKind::Extension "
                            "(every extension type must be resolved to a core "
                            "kind at the HIR→MIR boundary)",
                    id.v, t.v));
        }
    });
    // Arg.argIndex < FnSig.paramCount; CondBr.condition is Bool;
    // Return value (when present) matches FnSig return type.
    for (std::uint32_t fi = 0; fi < mir_.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir_.funcAt(fi);
        TypeId const fnSig = mir_.funcSignature(f);
        if (!fnSig.valid() || interner_->kind(fnSig) != TypeKind::FnSig) {
            continue;  // malformed signature; structural rules cover this
        }
        auto operands = interner_->operands(fnSig);
        // FnSig operands: [return-type, param-type0, param-type1, ...].
        std::uint32_t const paramCount = static_cast<std::uint32_t>(
            operands.size() >= 1 ? operands.size() - 1 : 0);
        TypeId const returnTy = operands.empty() ? InvalidType : operands[0];
        std::uint32_t const nBlocks = mir_.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nBlocks; ++bi) {
            MirBlockId const b = mir_.funcBlockAt(f, bi);
            std::uint32_t const n = mir_.blockInstCount(b);
            for (std::uint32_t i = 0; i < n; ++i) {
                MirInstId const id = mir_.blockInstAt(b, i);
                MirOpcode const op = mir_.instOpcode(id);
                if (op == MirOpcode::Arg) {
                    std::uint32_t const idx = mir_.argIndex(id);
                    if (idx >= paramCount) {
                        reportAt(reporter, DiagnosticCode::I_ArgIndexOutOfRange,
                            std::format("mir inst #{} (arg) argIndex {} >= "
                                        "FnSig paramCount {} for func #{}",
                                id.v, idx, paramCount, f.v));
                    }
                } else if (op == MirOpcode::CondBr) {
                    auto condOps = mir_.instOperands(id);
                    if (!condOps.empty()) {
                        TypeId const ct = mir_.instType(condOps[0]);
                        if (ct.valid() && interner_->kind(ct) != TypeKind::Bool) {
                            reportAt(reporter, DiagnosticCode::I_TerminatorTypeMismatch,
                                std::format("mir inst #{} (condbr) condition value #{} "
                                            "has type kind {} (expected Bool)",
                                    id.v, condOps[0].v,
                                    static_cast<int>(interner_->kind(ct))));
                        }
                    }
                } else if (op == MirOpcode::Return) {
                    auto retOps = mir_.instOperands(id);
                    bool const hasValue = !retOps.empty();
                    bool const wantValue = returnTy.valid()
                        && interner_->kind(returnTy) != TypeKind::Void;
                    if (hasValue && !wantValue) {
                        reportAt(reporter, DiagnosticCode::I_TerminatorTypeMismatch,
                            std::format("mir inst #{} (return) has a value but "
                                        "func #{} returns void", id.v, f.v));
                    } else if (!hasValue && wantValue) {
                        reportAt(reporter, DiagnosticCode::I_TerminatorTypeMismatch,
                            std::format("mir inst #{} (return) has no value but "
                                        "func #{} returns a non-void type", id.v, f.v));
                    } else if (hasValue && wantValue) {
                        TypeId const vt = mir_.instType(retOps[0]);
                        if (vt.valid() && vt.v != returnTy.v) {
                            reportAt(reporter, DiagnosticCode::I_TerminatorTypeMismatch,
                                std::format("mir inst #{} (return) value type {} "
                                            "does not match func #{} return type {}",
                                    id.v, vt.v, f.v, returnTy.v));
                        }
                    }
                }
            }
        }
    }
}

} // namespace dss
