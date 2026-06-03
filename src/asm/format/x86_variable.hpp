#pragma once

#include "asm/asm.hpp"
#include "asm/format/walker_util.hpp"
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"

#include <cstdint>
#include <span>
#include <vector>

// x86-variable format walker — plan 13 AS2.
//
// Encodes one LIR instruction whose target opcode declares
// `encoding.shape == X86Variable`. The walker reads the opcode's
// `encoding.variants[]` rows from `TargetSchema`, picks the first
// variant whose `operandKinds` guard matches the instruction's
// operand kinds, and emits the corresponding byte sequence per the
// variant's `template` + slot-wiring.
//
// **Owned architectural rules** (plan 13 §2.4 — NOT arch-identity
// branches, structural rules of the shape):
//   * REX prefix synthesis: REX.W from `template.rexW`; REX.R from
//     the high bit of the operand wired to `ModRmReg`; REX.B from
//     the high bit of the operand wired to `ModRmRm` — all derived
//     from `TargetRegisterInfo::hwEncoding`. Emitted as `0x40 | bits`
//     iff any of W/R/B is set.
//   * ModR/M assembly: mod=3 for register-direct slots (current
//     cycle's only mode; memory addressing modes join when their
//     LIR operand consumers — Load/Store/Lea — lower to bytes).
//   * Slot wiring: `ModRmReg` puts the operand's `hwEncoding & 7`
//     into ModR/M.reg (bits 3..5); `ModRmRm` puts it into ModR/M.rm
//     (bits 0..2); `Imm32` appends 4 little-endian bytes after the
//     ModR/M.
//
// All other arch-specific concerns (which mnemonics exist, what
// opcode bytes a given variant uses, whether a variant uses
// `modrmRegExt`) live in the JSON schema, NOT in this walker.

namespace dss::x86_variable {

// Encode one LIR instruction. Appends bytes to `out`; returns true on
// success, false (with an `A_*` diagnostic) on any failure (no variant
// guard matches, register has no registered `hwEncoding`, etc.). Pure
// function — all state lives in the out-parameters and the reporter.
//
// Cycle 2 scope: writes ONLY `out`. The `relocs` / `srcMap` output
// vectors are part of the cycle-1 plug-in signature; they begin
// receiving entries when symbol-referencing operands (Call/Lea/
// GlobalAddr) and SourceMap stamping land in later cycles.
[[nodiscard]] DSS_EXPORT bool
encode(Lir const&                  lir,
       TargetSchema const&         schema,
       LirInstId                   inst,
       TargetOpcodeInfo const*     info,
       std::span<MirInstId const>  lirToMir,
       std::vector<std::uint8_t>&  out,
       std::vector<Relocation>&    relocs,
       std::vector<SourceMapEntry>& srcMap,
       std::vector<walker_util::BlockRelPatch>& blockPatches,
       DiagnosticReporter&         reporter);

} // namespace dss::x86_variable
