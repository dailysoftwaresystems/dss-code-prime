#pragma once

#include "asm/asm.hpp"
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

// Round-trip oracle disassembler (plan 13 §2.9 + AS5).
//
// TEST-ONLY. Not part of the production assembly pipeline. After the
// encoder produces bytes for an instruction, the oracle re-extracts
// the per-slot operand values from those bytes (using the SAME
// JSON-declared variant rows the encoder consumed) and asserts they
// match what the encoder was supposed to write. Catches the silent-
// failure class where the encoder produces valid-but-WRONG bytes
// (correct instruction length, wrong register / immediate / symbol
// in a slot).
//
// Design discipline matches the encoder side:
//   * SHAPE-keyed dispatch — one disasm function per TargetEncoding
//     Shape (x86_variable / fixed32). Plan 13 §2.4: closed shape-
//     keyed vocabulary, NOT identity-keyed.
//   * JSON-driven — the same `TargetEncodingVariant` rows the
//     encoder reads drive the inverse extraction. No hand-rolled
//     mnemonic tables.
//   * No partial parse — the disasm must consume EXACTLY the bytes
//     the encoder produced for one instruction; any leftover bytes,
//     missing bytes, or shape mismatch produces a diagnostic.

namespace dss {

// One disassembled slot. `kind` names the encoding-slot variety
// the bytes were extracted from. `value` carries the recovered
// integer:
//   * Reg slots: the operand's raw `hwEncoding` ordinal.
//   * Imm32 slots: the 32-bit immediate (sign-extended to i64).
//   * Symbol-bearing slots (Disp32 / Imm26): always `std::nullopt`
//     because the encoder writes ZEROS at the slot's byte position
//     (the Relocation entry carries the symbol identity, not the
//     bytes). An explicit `nullopt` distinguishes "symbol-bearing,
//     value-undefined" from "legitimately-zero Imm32" — that
//     distinction was sentinel-collision-prone in the previous
//     `int64_t = 0` shape (D-AS5-3 closure).
struct DSS_EXPORT DisassembledSlot {
    EncodingSlotKind             kind = EncodingSlotKind::ModRmReg;
    std::optional<std::int64_t>  value;
};

// Result of inverting one encoded instruction. The variant index
// names which row of `TargetOpcodeInfo::encoding.variants` the bytes
// matched. `result` is populated when the variant declares a
// `resultSlot`; `wires` mirrors the variant's `wires` list 1:1 (same
// index ordering). This shape MIRRORS `TargetEncodingVariant`
// (encoder side) so the round-trip oracle's iteration is structural
// rather than positional (D-AS5-3 closure: previously a single flat
// `slots` vector with a positional "first slot = result when present"
// contract — error-prone for callers).
struct DSS_EXPORT DisassembledInst {
    std::uint16_t                  opcode        = 0;
    std::size_t                    variantIndex  = 0;
    std::size_t                    bytesConsumed = 0;
    std::optional<DisassembledSlot> result;
    std::vector<DisassembledSlot>   wires;
};

// Disassemble exactly one instruction from the head of `bytes`. The
// per-variant inverse-walker is shape-dispatched.
//
// Returns nullopt + emits `A_RoundTripMismatch` when the bytes don't
// match any variant (or when bytes are truncated). The diagnostic
// names the opcode and which slot/byte failed.
//
// `opcode` is supplied by the caller because the disasm operates on
// ONE instruction's bytes — the caller knows which opcode the
// encoder said it was emitting. The oracle's job is to verify the
// bytes actually MATCH that opcode's encoding row, not to identify
// the opcode from scratch.
[[nodiscard]] DSS_EXPORT std::optional<DisassembledInst>
disassembleInst(TargetSchema const&         schema,
                std::uint16_t               opcode,
                std::span<std::uint8_t const> bytes,
                DiagnosticReporter&         reporter);

// `roundTripVerify` is a TEST-ONLY helper — its declaration lives
// in `tests/asm/asm_test_support.hpp`, NOT here (per architect AS5
// review: production callers must not be able to pass a `Lir`
// reference into a function that exists only to verify encoder
// output; the linker — plan 14 — explicitly does NOT hold a Lir).
// Use `disassembleInst` above for plan-14-side byte-window
// inspection.

} // namespace dss
