#pragma once

#include "asm/disasm.hpp"
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"

#include <cstdint>
#include <optional>
#include <span>

// fixed32 round-trip oracle (plan 13 §2.9 + AS5).
//
// Inverse of `fixed32::encode`. Reads 4 LE bytes → 32-bit word,
// confirms the fixed-word bits match the variant's declared base,
// and extracts each declared slot's bit-field value (Rd 0..4, Rn
// 5..9, Rm 16..20, Imm26 0..25).

namespace dss::fixed32_disasm {

[[nodiscard]] DSS_EXPORT std::optional<DisassembledInst>
disassemble(TargetSchema const&            schema,
            std::uint16_t                  opcode,
            TargetOpcodeInfo const*        info,
            std::span<std::uint8_t const>  bytes,
            DiagnosticReporter&            reporter);

} // namespace dss::fixed32_disasm
