#include "lir/lir_callconv.hpp"
#include "lir/lir_pass_util.hpp"

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

LirOperand
remapBlockRef(LirOperand const& op,
              std::unordered_map<std::uint32_t, LirBlockId> const& srcToDst) {
    if (op.kind == LirOperandKind::BlockRef) {
        auto it = srcToDst.find(op.blockSlot);
        if (it != srcToDst.end()) return LirOperand::makeBlockRef(it->second.v);
    }
    return op;
}

bool emitTerminator(LirBuilder& b, std::uint16_t op,
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
