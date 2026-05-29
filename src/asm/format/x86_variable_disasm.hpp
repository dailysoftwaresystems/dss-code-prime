#pragma once

#include "asm/disasm.hpp"
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"

#include <cstdint>
#include <optional>
#include <span>

// x86-variable round-trip oracle (plan 13 §2.9 + AS5).
//
// Inverse of `x86_variable::encode`. Reads bytes starting at the
// head of `bytes`, peels off the optional REX prefix, matches the
// declared opcode bytes, decodes ModR/M, extracts immediates and
// disp32, and returns the per-slot values the encoder must have
// consumed. Same shape-keyed pattern as the encoder; no
// arch-identity branches.

namespace dss::x86_variable_disasm {

[[nodiscard]] DSS_EXPORT std::optional<DisassembledInst>
disassemble(TargetSchema const&            schema,
            std::uint16_t                  opcode,
            TargetOpcodeInfo const*        info,
            std::span<std::uint8_t const>  bytes,
            DiagnosticReporter&            reporter);

} // namespace dss::x86_variable_disasm
