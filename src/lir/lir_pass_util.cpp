#include "lir/lir_callconv.hpp"
#include "lir/lir_pass_util.hpp"

#include <algorithm>
#include <format>
#include <utility>
#include <vector>

namespace dss::lir_pass_util {

// `report()` moved to `dss::report` in
// `core/types/diagnostic_reporter.hpp` at LK10 cycle 3 post-fold #2.

IncomingArgReg
incomingArgRegister(TargetSchema const&            schema,
                    TargetCallingConvention const& cc,
                    LirRegClass                    resultClass,
                    std::uint32_t                  payload) {
    // The arg-register pool for this parameter's class. Slot-aligned ccs
    // (Win64) keep argGprs/argFprs the same length so `payload` (a flat slot
    // index) selects the same slot in either; independent-counter ccs
    // (SysV/AAPCS64) pass `payload` as the per-class index. Either way the
    // per-class pool indexed by `payload` is the incoming register.
    //
    // ★ THE POOL COMES FROM THE PUBLISHED ROW TABLE, NOT FROM A TWO-WAY TEST.
    // This read `(resultClass == FPR) ? argFprs : argGprs`, so a VR-class
    // parameter was looked up in the INTEGER pool and this pass reported the
    // wrong register as occupied — the fifth copy of
    // D-LIR-ARG-PASSING-POOL-SELECTION-IS-TWO-WAY-AND-VR-FALLS-INTO-GPR.
    // ⚠ A class with NO row answers `StackPassed` here rather than a register.
    // This function's contract is a QUERY with no reporter, and the three
    // placement sites refuse such a class loudly; answering "not in a register"
    // is the conservative direction (a scratch pick stays available), whereas
    // naming another class's register is the wrong-register answer itself.
    auto const* pool = argRegisterPool(cc, resultClass);
    if (pool == nullptr || payload >= pool->size()) {
        return {IncomingArgRegKind::StackPassed, 0};
    }
    auto const ord = schema.registerByName((*pool)[payload]);
    if (!ord.has_value()) {
        return {IncomingArgRegKind::UnresolvableName, 0};
    }
    return {IncomingArgRegKind::Register, *ord};
}

// D-OPT-JCC-FALLTHROUGH. See the header for why this question is asked of the
// SCHEMA rather than answered by the transform.
//
// The non-operand routing axes are compared WHOLESALE (`width`, `immMin`,
// `immMax`, `negValue`, `memoryDestination`) because the selector keys on all
// of them: two variants that agree on the operand tuple but disagree on any
// other axis describe DIFFERENT instructions, and reaching one by dropping an
// operand off the other would encode something the pass never asked for.
bool
declaresFallthroughBranchForm(TargetSchema const& schema,
                              std::uint16_t       opcode,
                              std::size_t         opCount) noexcept {
    if (opCount < 2) return false;  // nothing to drop but the only target
    auto const* info = schema.opcodeInfo(opcode);
    if (info == nullptr) return false;

    auto sameRouting = [](TargetEncodingVariant const& a,
                          TargetEncodingVariant const& b) {
        return a.guardWidthBits == b.guardWidthBits
            && a.immMin == b.immMin
            && a.immMax == b.immMax
            && a.negValue == b.negValue
            && a.memoryDestination == b.memoryDestination;
    };

    for (auto const& lng : info->encoding.variants) {
        if (lng.operandKinds.size() != opCount) continue;
        if (lng.operandKinds.back() != OperandKindFilter::BlockRef) continue;
        for (auto const& shrt : info->encoding.variants) {
            if (shrt.operandKinds.size() + 1 != lng.operandKinds.size()) continue;
            if (!sameRouting(lng, shrt)) continue;
            if (!std::equal(shrt.operandKinds.begin(), shrt.operandKinds.end(),
                            lng.operandKinds.begin())) {
                continue;
            }
            return true;
        }
    }
    return false;
}

LirOperand
remapBlockRef(LirOperand const& op,
              std::unordered_map<std::uint32_t, LirBlockId> const& srcToDst) {
    if (op.kind == LirOperandKind::BlockRef) {
        auto it = srcToDst.find(op.blockSlot);
        if (it != srcToDst.end()) return LirOperand::makeBlockRef(it->second.v);
    }
    return op;
}

namespace {

// The dispatch proper. Split from `emitTerminator` so the POISON below is
// applied at ONE place rather than at each `return false` — a future refusal
// arm added here inherits it, and cannot be the arm that forgot.
// (D-LIR-2ADDR-IGNORES-EMIT-TERMINATOR-FAILURE.)
[[nodiscard]] bool
emitTerminatorDispatch(LirBuilder& b, std::uint16_t op,
                       TargetOpcodeInfo const* info,
                       std::span<LirBlockId const> succs,
                       std::span<LirOperand const> newOps,
                       std::uint32_t payload,
                       std::uint8_t  flags,
                       std::unordered_map<std::uint32_t, LirBlockId> const& srcToDst,
                       std::string_view passName,
                       DiagnosticReporter& reporter) {
    // Schema-driven dispatch via `info->terminatorKind` — shared with
    // the `.dsslir` parser. Earlier draft used a successor-count +
    // operand-emptiness heuristic that silently mis-classified any
    // opcode whose `ret` takes 0 operands (same silent-failure path
    // the parser dispatch was rewritten to close in ML8 cycle 3).
    if (info == nullptr) {
        report(reporter, DiagnosticCode::L_UnsupportedLoweringForOpcode,
               DiagnosticSeverity::Error,
               std::format("{}: terminator opcode {} has no schema entry",
                           passName, static_cast<unsigned>(op)));
        return false;
    }
    auto resolveAt = [&](std::size_t i) -> LirBlockId {
        return srcToDst.at(succs[i].v);
    };
    switch (info->terminatorKind) {
        case TargetTerminatorKind::Return:
            b.addReturn(op, newOps, payload, flags);
            return true;
        case TargetTerminatorKind::Unreachable:
            b.addUnreachable(op, payload, flags);
            return true;
        case TargetTerminatorKind::Br:
            if (succs.size() != 1) break;  // diagnostic below
            b.addBr(op, resolveAt(0), payload, flags);
            return true;
        case TargetTerminatorKind::CondBr:
            if (succs.size() != 2) break;
            b.addCondBr(op, newOps, resolveAt(0), resolveAt(1),
                        payload, flags);
            return true;
        case TargetTerminatorKind::Switch:
            report(reporter, DiagnosticCode::L_UnsupportedLoweringForOpcode,
                   DiagnosticSeverity::Error,
                   std::format("{}: Switch terminator opcode {} not yet "
                               "supported by this pass (reserved for LIR "
                               "Switch lowering)",
                               passName, static_cast<unsigned>(op)));
            return false;
        case TargetTerminatorKind::IndirectBr: {
            // D-CSUBSET-COMPUTED-GOTO: re-map the address operand(s) (already in
            // `newOps`) AND resolve EVERY address-taken successor through srcToDst.
            // Dropping a successor would delete a live `&&label` edge.
            if (succs.empty()) break;  // an IndirectBr must have ≥1 target
            std::vector<LirBlockId> targets;
            targets.reserve(succs.size());
            for (std::size_t i = 0; i < succs.size(); ++i) targets.push_back(resolveAt(i));
            b.addIndirectBr(op, newOps, targets, payload, flags);
            return true;
        }
        case TargetTerminatorKind::None:
            report(reporter, DiagnosticCode::L_UnsupportedLoweringForOpcode,
                   DiagnosticSeverity::Error,
                   std::format("{}: opcode {} has terminatorKind=none yet "
                               "reached the terminator-emit path (caller "
                               "must filter via info->isTerminator())",
                               passName, static_cast<unsigned>(op)));
            return false;
    }
    // Reached only when Br/CondBr had wrong successor count for its
    // declared kind — schema invariant violated upstream.
    report(reporter, DiagnosticCode::L_UnsupportedLoweringForOpcode,
           DiagnosticSeverity::Error,
           std::format("{}: terminator opcode {} ({}) has {} successors "
                       "(schema invariant violated)",
                       passName, static_cast<unsigned>(op),
                       targetTerminatorKindName(info->terminatorKind),
                       static_cast<unsigned>(succs.size())));
    return false;
}

} // namespace

bool emitTerminator(LirBuilder& b, std::uint16_t op,
                    TargetOpcodeInfo const* info,
                    std::span<LirBlockId const> succs,
                    std::span<LirOperand const> newOps,
                    std::uint32_t payload,
                    std::uint8_t  flags,
                    std::unordered_map<std::uint32_t, LirBlockId> const& srcToDst,
                    std::string_view passName,
                    DiagnosticReporter& reporter) {
    if (emitTerminatorDispatch(b, op, info, succs, newOps, payload, flags,
                               srcToDst, passName, reporter)) {
        return true;
    }
    // ★★★ D-LIR-2ADDR-IGNORES-EMIT-TERMINATOR-FAILURE — THE REFUSAL IS
    // TERMINAL FOR THE WHOLE REBUILD, SO SAY SO TO THE BUILDER AND NOT ONLY
    // TO THE CALLER.
    //
    // Every arm above reports a diagnostic and appends NOTHING, which leaves
    // the caller's open block without a terminator. Five passes call this,
    // each of which must then stop driving `b` — and the `[[nodiscard]]`
    // return can only make them LOOK at the answer, never act on it. A caller
    // that reads the bool, records a failure flag and carries on is the
    // original defect verbatim: it reported success and the process died
    // inside `LirBuilder::closeFunction`.
    //
    // Poisoning here removes that possibility for every present and future
    // caller in one line: `finish()` on a poisoned builder yields an empty
    // module, so the worst a forgotten bail can now produce is the same
    // already-diagnosed empty result the correct bail produces. The callers
    // still check — bailing early is cheaper and keeps the diagnostic count
    // at one — but their check is no longer what stands between a refusal and
    // a process kill.
    b.poison();
    return false;
}

void
copyModuleSideStructures(Lir const& src, LirBuilder& dst) {
    // Append every source pool entry in index order. The destination
    // builder is freshly constructed (empty pools), so the `*Add` calls
    // return 0, 1, 2, ... — reproducing the source indices that the
    // copied `LiteralIndex` operands and `regConstraints` handles
    // reference. (No dedup in either pool: they are by-index stores, not
    // value sets; preserving identity is the whole point.)
    auto const& literals = src.literalPool();
    for (std::uint32_t i = 0; i < literals.size(); ++i) {
        (void)dst.literalPoolAdd(literals.at(i));
    }
    // ⚠ The re-add goes through `regConstraintPoolAdd`, which RE-RESOLVES
    // every register name against the destination builder's schema. That
    // is deliberate rather than a raw copy: a pass rebuilding a module
    // under a schema whose register table disagrees with the one the
    // constraints were authored against is a real (if today unreachable)
    // configuration error, and re-resolution turns it into the loud abort
    // the authoring path already has instead of a silently stale ordinal.
    // The ordinals are recomputed, never trusted from the source.
    auto const& constraints = src.regConstraintPool();
    for (std::uint32_t i = 0; i < constraints.size(); ++i) {
        (void)dst.regConstraintPoolAdd(constraints.at(i));
    }
    // D-C-GNU-CONSTRUCTOR-ATTRIBUTE-IS-WARNED-AND-IGNORED-NOT-RUN: the THIRD side
    // structure, carried HERE for the same reason as the two above — this helper
    // is the one call every LIR rebuild pass already makes, so the schedule
    // crosses all five of them with no per-pass edit and no fifth chance to
    // forget. Keyed by SymbolId, so unlike the pools there is no index identity
    // to preserve and the order is free.
    for (auto const& e : src.staticInitSchedule()) {
        dst.staticInitAdd(e.symbol, e.schedule);
    }
}

void
carryInstSideData(Lir const& src, LirInstId srcInst,
                  LirBuilder& dst, LirInstId dstInst) {
    std::uint32_t const handle = src.instRegConstraintHandle(srcInst);
    if (handle == kLirNoRegConstraints) return;
    // The pools were copied index-preservingly by
    // `copyModuleSideStructures`, so the SOURCE index is also the
    // destination index. `setInstRegConstraints` range-checks it anyway —
    // if a caller skipped the pool copy, that guard is what turns the
    // mistake into an abort at build time instead of a dangling handle
    // that reaches the allocator.
    dst.setInstRegConstraints(
        dstInst, lirRegConstraintIndexForHandle(handle));
}

std::optional<std::uint16_t>
ClassMoveOpcodeCache::resolve(TargetSchema const& schema, LirRegClass cls) {
    auto const c = static_cast<std::size_t>(cls);
    if (c >= byClass_.size()) return std::nullopt;
    if (!byClass_[c].has_value()) {
        byClass_[c] = schema.regClassOpOpcode(static_cast<TargetRegClass>(c),
                                              RegClassOp::Move);
    }
    return *byClass_[c];
}

IdentityClassMoveVerdict
classifyIdentityClassMove(Lir const& lir, LirInstId inst,
                          TargetSchema const&   schema,
                          ClassMoveOpcodeCache& cache) {
    auto const* info = schema.opcodeInfo(lir.instOpcode(inst));
    if (info == nullptr) return IdentityClassMoveVerdict::NotIdentityClassMove;
    // A terminator is never a copy, and deleting one would leave the block
    // unterminated — the builder's abort, not a diagnostic.
    if (info->isTerminator()) {
        return IdentityClassMoveVerdict::NotIdentityClassMove;
    }

    LirReg const result = lir.instResult(inst);
    if (!result.valid() || result.isPhysical == 0) {
        return IdentityClassMoveVerdict::NotIdentityClassMove;
    }

    // ★ THE OPCODE IDENTITY TEST. Not a mnemonic, not an encoded byte — the
    // handle the SCHEMA names as this class's copy. On the shipped x86_64
    // target THREE opcodes (`mov`, `trunc`, `zext`) encode to bytes every
    // disassembler prints as `mov`, and only the first is a no-op when
    // source and destination coincide.
    auto const movOpcode = cache.resolve(schema, result.regClass());
    if (!movOpcode.has_value() || lir.instOpcode(inst) != *movOpcode) {
        return IdentityClassMoveVerdict::NotIdentityClassMove;
    }

    // Exactly one operand, a physical register identical to the result.
    auto const ops = lir.instOperands(inst);
    if (ops.size() != 1) return IdentityClassMoveVerdict::NotIdentityClassMove;
    if (ops[0].kind != LirOperandKind::Reg) {
        return IdentityClassMoveVerdict::NotIdentityClassMove;
    }
    if (ops[0].reg.isPhysical == 0 || !(ops[0].reg == result)) {
        return IdentityClassMoveVerdict::NotIdentityClassMove;
    }

    // ── in the POPULATION from here down; the rest is why R1 may refuse.

    // A declared side effect (or a declared implicit register read/clobber)
    // is an observable this rule cannot reason about from the operands.
    if (info->hasSideEffects || info->implicitRegisters.has_value()) {
        return IdentityClassMoveVerdict::RefusedSideEffects;
    }

    // ★ THE WIDTH TEST — the second, independent guard on the partial-
    // register-write hazard. A copy NARROWER than the register it names
    // writes bits it did not read (x86-64's 32-bit GPR forms zero the upper
    // half), so it is NOT a no-op even when source and destination are the
    // same register.
    auto const* regInfo =
        schema.registerInfo(static_cast<std::uint16_t>(result.id));
    if (regInfo == nullptr || regInfo->widthBytes == 0) {
        return IdentityClassMoveVerdict::RefusedUndeclaredRegisterWidth;
    }
    auto const regWidthBits =
        static_cast<std::uint32_t>(regInfo->widthBytes) * 8u;
    if (static_cast<std::uint32_t>(lirInstWidthBits(lir.instFlags(inst)))
        != regWidthBits) {
        return IdentityClassMoveVerdict::RefusedNarrowerThanRegister;
    }

    // ★ NEVER DELETE THE ONLY NAMER OF A SIDE-STRUCTURE ENTRY. The
    // per-instruction register-constraint pool is referenced by index from
    // the instruction stream and `verifyLirRebuild` counts the references on
    // both sides; orphaning an entry is `L_SideStructureReferenceLost`.
    // Keeping a redundant copy costs one instruction — the fail-safe arm.
    if (lir.instRegConstraintHandle(inst) != kLirNoRegConstraints) {
        return IdentityClassMoveVerdict::RefusedNamesConstraintPoolEntry;
    }
    return IdentityClassMoveVerdict::Deletable;
}

IdentityClassMoveCensus
censusIdentityClassMoves(Lir const& lir, TargetSchema const& schema) {
    IdentityClassMoveCensus out{};
    ClassMoveOpcodeCache    cache{};
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
            LirBlockId const blk = lir.funcBlockAt(fn, bi);
            for (std::uint32_t k = 0; k < lir.blockInstCount(blk); ++k) {
                LirInstId const inst = lir.blockInstAt(blk, k);
                // The superset, opcode-blind on purpose: it is the number the
                // opcode test has to REJECT down to.
                LirReg const res = lir.instResult(inst);
                auto const   ops = lir.instOperands(inst);
                if (res.valid() && ops.size() == 1
                    && ops[0].kind == LirOperandKind::Reg && ops[0].reg.valid()
                    && ops[0].reg == res) {
                    ++out.selfReferentialSingleOperand;
                }
                switch (classifyIdentityClassMove(lir, inst, schema, cache)) {
                case IdentityClassMoveVerdict::NotIdentityClassMove:
                    continue;
                case IdentityClassMoveVerdict::Deletable:
                    ++out.deletable;
                    break;
                case IdentityClassMoveVerdict::RefusedSideEffects:
                    ++out.refusedSideEffects;
                    break;
                case IdentityClassMoveVerdict::RefusedUndeclaredRegisterWidth:
                    ++out.refusedUndeclaredRegisterWidth;
                    break;
                case IdentityClassMoveVerdict::RefusedNarrowerThanRegister:
                    ++out.refusedNarrowerThanRegister;
                    break;
                case IdentityClassMoveVerdict::RefusedNamesConstraintPoolEntry:
                    ++out.refusedNamesConstraintPoolEntry;
                    break;
                }
                ++out.population;
            }
        }
    }
    return out;
}

void
carryInstSideData(Lir const& src, LirInstId srcInst, LirBuilder& dst) {
    // Deliberately does NOT short-circuit on "no constraints" before
    // calling `lastInst()`: a caller that has appended nothing is a
    // caller whose pairing is wrong, and it should abort on the empty
    // module rather than only on the modules that happen to carry
    // constraints (the "exercise the failure arm" rule — a guard that
    // only fires on the rare input is a guard nobody ever runs).
    carryInstSideData(src, srcInst, dst, dst.lastInst());
}

} // namespace dss::lir_pass_util
