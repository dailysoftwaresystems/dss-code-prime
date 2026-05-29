#include "mir/mir_verifier.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir_opcode.hpp"

#include <algorithm>
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

// Build the predecessor adjacency: preds[blockSlot] is the list of
// MirBlockIds that name this block as a successor. O(V + E). An out-
// of-range successor block-id is reported as a structural violation
// (rather than silently dropped) so the caller sees ONE clear blame
// instead of a downstream PhiPredNotInCfg cascade.
[[nodiscard]] std::vector<std::vector<MirBlockId>>
buildPredecessors(Mir const& mir, DiagnosticReporter& reporter) {
    std::vector<std::vector<MirBlockId>> preds(mir.blockCount());
    for (std::uint32_t i = 1; i < mir.blockCount(); ++i) {
        MirBlockId const from{i, mir.id().v};
        for (MirBlockId const to : mir.blockSuccessors(from)) {
            if (to.v < preds.size()) {
                preds[to.v].push_back(from);
            } else {
                reportBlock(reporter, DiagnosticCode::I_VerifierFailure, from,
                    std::format("successor block #{} is out of range "
                                "(blockCount = {})",
                        to.v, preds.size()));
            }
        }
    }
    return preds;
}

// Reverse post-order over the CFG starting at `entry`. Unreachable
// blocks are excluded by construction.
[[nodiscard]] std::vector<MirBlockId>
reversePostOrder(Mir const& mir, MirBlockId entry) {
    std::vector<MirBlockId> order;
    if (!entry.valid()) return order;
    std::unordered_set<std::uint32_t> visited;
    struct Frame { MirBlockId block; std::size_t nextSucc; };
    std::vector<Frame> stack;
    auto push = [&](MirBlockId b) {
        if (b.valid() && visited.insert(b.v).second) stack.push_back({b, 0});
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
// each reachable block (in `order`) to its immediate dominator (entry's
// idom is itself). Unreachable blocks have InvalidMirBlock.
//
// Termination safety: the inner `intersect` walks idom-chains; an idom
// cycle (which can only arise on malformed direct-`Mir`-ctor input —
// the algorithm itself cannot produce one on a well-formed CFG) would
// loop forever. We guard with a bounded step count derived from the
// idom array size; on overflow the caller treats the result as "could
// not resolve" rather than infinite-loop.
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
    if (!entry.valid()) return idom;
    idom[entry.v] = entry;
    std::uint32_t const stepCap = static_cast<std::uint32_t>(idom.size() * 2 + 4);
    auto intersect = [&](MirBlockId b1, MirBlockId b2) {
        MirBlockId finger1 = b1;
        MirBlockId finger2 = b2;
        std::uint32_t steps = 0;
        while (finger1.v != finger2.v) {
            if (++steps > stepCap) return MirBlockId{};
            // Unreachable-block / invalid-idom safety: each finger
            // walks via idom-chain. An unreachable block has
            // rpoIndex == -1 (sentinel) and idom == invalid; both
            // exits prevent the unbounded `0xFFFFFFFF > anything`
            // loop the textbook formulation would hit.
            if (rpoIndex[finger1.v] == static_cast<std::uint32_t>(-1)
             || rpoIndex[finger2.v] == static_cast<std::uint32_t>(-1)) {
                return MirBlockId{};
            }
            while (rpoIndex[finger1.v] > rpoIndex[finger2.v]) {
                MirBlockId const next = idom[finger1.v];
                if (!next.valid()
                 || rpoIndex[next.v] == static_cast<std::uint32_t>(-1)) {
                    return MirBlockId{};
                }
                finger1 = next;
                if (++steps > stepCap) return MirBlockId{};
            }
            while (rpoIndex[finger2.v] > rpoIndex[finger1.v]) {
                MirBlockId const next = idom[finger2.v];
                if (!next.valid()
                 || rpoIndex[next.v] == static_cast<std::uint32_t>(-1)) {
                    return MirBlockId{};
                }
                finger2 = next;
                if (++steps > stepCap) return MirBlockId{};
            }
        }
        return finger1;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 1; i < order.size(); ++i) {
            MirBlockId const b = order[i];
            MirBlockId newIdom{};
            for (MirBlockId const p : preds[b.v]) {
                if (rpoIndex[p.v] == static_cast<std::uint32_t>(-1)) continue;
                if (!idom[p.v].valid()) continue;
                if (!newIdom.valid()) {
                    newIdom = p;
                } else {
                    MirBlockId const interBlock = intersect(newIdom, p);
                    if (!interBlock.valid()) continue;  // intersect bailed
                    newIdom = interBlock;
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

// Does `a` dominate `b`? Iteration-count guarded so a malformed idom
// (cycle, missing self-loop at entry, etc.) never loops forever — on
// overflow returns false (safe-conservative refusal).
[[nodiscard]] bool
dominates(MirBlockId a, MirBlockId b, std::vector<MirBlockId> const& idom) {
    if (!a.valid() || !b.valid()) return false;
    if (a.v == b.v) return true;
    MirBlockId cur = b;
    std::uint32_t steps = 0;
    std::uint32_t const stepCap = static_cast<std::uint32_t>(idom.size() + 2);
    while (idom[cur.v].valid() && idom[cur.v].v != cur.v) {
        if (++steps > stepCap) return false;
        cur = idom[cur.v];
        if (cur.v == a.v) return true;
    }
    return false;
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
            if (operands.size() < info.minOperands || operands.size() > info.maxOperands) {
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

void MirVerifier::checkStructCfMarkers(DiagnosticReporter& reporter) const {
    // Marker pairing — count-based. Strict count-equality catches a
    // pairing-mismatch the presence-only check would miss (two IfThen
    // blocks + one IfJoin should diagnose; one IfElse with no IfJoin
    // should diagnose). Full structural nesting validity needs the
    // dom tree, but count parity is the load-bearing structural
    // invariant ML2 emits + downstream passes assume.
    for (std::uint32_t fi = 0; fi < mir_.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir_.funcAt(fi);
        std::uint32_t const nBlocks = mir_.funcBlockCount(f);
        std::uint32_t nIfThen = 0, nIfElse = 0, nIfJoin = 0;
        std::uint32_t nLoopHeader = 0, nLoopLatch = 0, nLoopExit = 0;
        for (std::uint32_t bi = 0; bi < nBlocks; ++bi) {
            MirBlockId const b = mir_.funcBlockAt(f, bi);
            StructCfMarker const m = mir_.blockMarker(b);
            if (m == StructCfMarker::ExitBlock) {
                std::uint32_t const n = mir_.blockInstCount(b);
                if (n > 0) {
                    MirOpcode const op = mir_.instOpcode(mir_.blockInstAt(b, n - 1));
                    if (op != MirOpcode::Return && op != MirOpcode::Unreachable) {
                        reportBlock(reporter, DiagnosticCode::I_StructCfMismatch, b,
                            std::format("ExitBlock terminates in {}; expected "
                                        "Return or Unreachable",
                                opcodeInfo(op).mnemonic));
                    }
                }
            }
            switch (m) {
                case StructCfMarker::IfThen:     ++nIfThen; break;
                case StructCfMarker::IfElse:     ++nIfElse; break;
                case StructCfMarker::IfJoin:     ++nIfJoin; break;
                case StructCfMarker::LoopHeader: ++nLoopHeader; break;
                case StructCfMarker::LoopLatch:  ++nLoopLatch; break;
                case StructCfMarker::LoopExit:   ++nLoopExit; break;
                default: break;
            }
        }
        // If: IfThen-count == IfJoin-count (each then-arm joins);
        // IfElse-count ≤ IfJoin-count (else is optional). An orphan
        // IfElse without IfJoin is a violation; multiple IfElse
        // without IfJoin is a violation.
        if (nIfThen != nIfJoin) {
            reportFunc(reporter, DiagnosticCode::I_StructCfMismatch, f,
                std::format("IfThen-count {} != IfJoin-count {} (each then-arm "
                            "must have a join block)",
                    nIfThen, nIfJoin));
        }
        if (nIfElse > nIfJoin) {
            reportFunc(reporter, DiagnosticCode::I_StructCfMismatch, f,
                std::format("IfElse-count {} > IfJoin-count {} (each else-arm "
                            "must be paired with a join)",
                    nIfElse, nIfJoin));
        }
        // Loop: LoopHeader-count == LoopExit-count (each loop has an
        // exit); LoopLatch-count == LoopHeader-count (each loop has
        // a back-edge source).
        if (nLoopHeader != nLoopExit) {
            reportFunc(reporter, DiagnosticCode::I_StructCfMismatch, f,
                std::format("LoopHeader-count {} != LoopExit-count {}",
                    nLoopHeader, nLoopExit));
        }
        // LoopLatch is OPTIONAL — ML2's while-loop lowering marks the
        // back-edge source as `Linear`, not LoopLatch (the marker is
        // emitted only when a dedicated continue-target block exists,
        // as in do-while). Strict-equality would over-flag valid ML2
        // output. The weaker `>` check still catches a LoopLatch
        // without a header.
        if (nLoopLatch > nLoopHeader) {
            reportFunc(reporter, DiagnosticCode::I_StructCfMismatch, f,
                std::format("LoopLatch-count {} > LoopHeader-count {} (latch "
                            "block without an enclosing loop header)",
                    nLoopLatch, nLoopHeader));
        }
    }
}

void MirVerifier::checkPhiIncomings(DiagnosticReporter& reporter) const {
    auto preds = buildPredecessors(mir_, reporter);
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

void MirVerifier::checkDomination(DiagnosticReporter& reporter) const {
    auto preds = buildPredecessors(mir_, reporter);
    for (std::uint32_t fi = 0; fi < mir_.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir_.funcAt(fi);
        if (mir_.funcBlockCount(f) == 0) continue;
        MirBlockId const entry = mir_.funcEntry(f);
        if (!entry.valid()) {
            reportFunc(reporter, DiagnosticCode::I_NoEntryBlock, f,
                "funcEntry() returned InvalidMirBlock; skipping dominance check");
            continue;
        }
        auto rpo  = reversePostOrder(mir_, entry);
        auto idom = computeIDoms(mir_, entry, rpo, preds);
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
                        if (!dominates(defBlock, inc.pred, idom)) {
                            reportInst(reporter, DiagnosticCode::I_NotDominated, use,
                                std::format("(phi in block #{}) incoming value "
                                            "#{} defined in block #{} does not "
                                            "dominate predecessor block #{}",
                                    useBlock.v, inc.value.v,
                                    defBlock.v, inc.pred.v));
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
                    } else if (!dominates(defBlock, useBlock, idom)) {
                        reportInst(reporter, DiagnosticCode::I_NotDominated, use,
                            std::format("uses value #{} defined in block #{} "
                                        "which does not dominate use block #{}",
                                op.v, defBlock.v, useBlock.v));
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
        auto operands = interner_->operands(fnSig);
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
                        reportInst(reporter, DiagnosticCode::I_ArgIndexOutOfRange, id,
                            std::format("argIndex {} >= FnSig paramCount {} "
                                        "for func #{}",
                                idx, paramCount, f.v));
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
                    } else if (!hasValue && wantValue) {
                        reportInst(reporter,
                            DiagnosticCode::I_TerminatorTypeMismatch, id,
                            std::format("(return) has no value but func #{} "
                                        "returns a non-void type", f.v));
                    } else if (hasValue && wantValue) {
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

} // namespace dss
