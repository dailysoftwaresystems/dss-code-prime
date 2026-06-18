#include "lir/lir_pass_util.hpp"

#include <format>
#include <utility>

namespace dss::lir_pass_util {

// `report()` moved to `dss::report` in
// `core/types/diagnostic_reporter.hpp` at LK10 cycle 3 post-fold #2.

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
copyLiteralPool(Lir const& src, LirBuilder& dst) {
    // Append every source pool entry in index order. The destination
    // builder is freshly constructed (empty pool), so `literalPoolAdd`
    // returns 0, 1, 2, ... — reproducing the source indices that the
    // copied `LiteralIndex` operands reference. (No dedup: the pool is a
    // by-index store, not a value set; preserving identity is the whole
    // point.)
    auto const& pool = src.literalPool();
    for (std::uint32_t i = 0; i < pool.size(); ++i) {
        (void)dst.literalPoolAdd(pool.at(i));
    }
}

} // namespace dss::lir_pass_util
