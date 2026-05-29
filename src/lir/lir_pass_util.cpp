#include "lir/lir_pass_util.hpp"

#include <format>
#include <utility>

namespace dss::lir_pass_util {

void report(DiagnosticReporter& reporter, DiagnosticCode code,
            DiagnosticSeverity severity, std::string actual) {
    ParseDiagnostic d;
    d.code     = code;
    d.severity = severity;
    d.actual   = std::move(actual);
    reporter.report(std::move(d));
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
                    std::unordered_map<std::uint32_t, LirBlockId> const& srcToDst,
                    std::string_view passName,
                    DiagnosticReporter& reporter) {
    switch (succs.size()) {
        case 0:
            if (info != nullptr
                && info->minSuccessors == 0 && info->maxSuccessors == 0
                && info->result == TargetResultRule::None
                && newOps.empty()) {
                b.addUnreachable(op);
            } else {
                b.addReturn(op, newOps);
            }
            return true;
        case 1:
            b.addBr(op, srcToDst.at(succs[0].v));
            return true;
        case 2:
            b.addCondBr(op, newOps,
                        srcToDst.at(succs[0].v),
                        srcToDst.at(succs[1].v), payload);
            return true;
        default:
            report(reporter, DiagnosticCode::L_UnsupportedLoweringForOpcode,
                   DiagnosticSeverity::Error,
                   std::format("{}: terminator opcode {} has {} successors; "
                               "only 0/1/2 supported",
                               passName,
                               static_cast<unsigned>(op),
                               static_cast<unsigned>(succs.size())));
            return false;
    }
}

} // namespace dss::lir_pass_util
