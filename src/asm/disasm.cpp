#include "asm/disasm.hpp"

#include "asm/format/fixed32_disasm.hpp"
#include "asm/format/x86_variable_disasm.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"

#include <format>

namespace dss {

namespace {

using dss::report;

} // namespace

std::optional<DisassembledInst>
disassembleInst(TargetSchema const&         schema,
                std::uint16_t               opcode,
                std::span<std::uint8_t const> bytes,
                DiagnosticReporter&         reporter) {
    auto const* info = schema.opcodeInfo(opcode);
    if (info == nullptr) {
        report(reporter, DiagnosticCode::A_RoundTripMismatch,
               DiagnosticSeverity::Error,
               std::format("round-trip: opcode {} is not declared in "
                           "target schema '{}'",
                           opcode, schema.name()));
        return std::nullopt;
    }

    switch (info->encoding.shape) {
        case TargetEncodingShape::None:
            report(reporter, DiagnosticCode::A_RoundTripMismatch,
                   DiagnosticSeverity::Error,
                   std::format("round-trip: opcode '{}' has no encoding "
                               "declared — nothing to disassemble",
                               info->mnemonic));
            return std::nullopt;
        case TargetEncodingShape::X86Variable:
            return x86_variable_disasm::disassemble(schema, opcode, info,
                                                     bytes, reporter);
        case TargetEncodingShape::Fixed32:
            return fixed32_disasm::disassemble(schema, opcode, info,
                                                bytes, reporter);
    }
    // Enum-drift fallback — mirrors the encoder dispatch's structure.
    report(reporter, DiagnosticCode::A_RoundTripMismatch,
           DiagnosticSeverity::Error,
           std::format("round-trip: opcode '{}' has unknown encoding "
                       "shape ordinal {} (internal-invariant: new "
                       "TargetEncodingShape value was added without "
                       "updating the disassembler dispatch)",
                       info->mnemonic,
                       static_cast<int>(info->encoding.shape)));
    return std::nullopt;
}

// `roundTripVerify` has moved to `tests/asm/asm_test_support.hpp`
// (architect AS5 review — production library must not expose a
// test-only helper that takes a `Lir` reference; the linker —
// plan 14 — explicitly does NOT hold a Lir). The function is
// declared `inline` in the test support header so each test TU
// emits its own copy; production callers needing to inspect
// disassembled bytes use `disassembleInst` above directly.

} // namespace dss
