#include "asm/format/x86_variable_disasm.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_pass_util.hpp"

#include <cassert>
#include <cstdint>
#include <format>

namespace dss::x86_variable_disasm {

namespace {

using lir_pass_util::report;

// Read a 4-byte little-endian int32 starting at `bytes[offset]`.
// Caller ensures the bounds check.
[[nodiscard]] std::int32_t
readImm32LE(std::span<std::uint8_t const> bytes, std::size_t offset) noexcept {
    return static_cast<std::int32_t>(
          static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1]) <<  8)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24));
}

// Per-variant peel state — mirrors the encoder's EncodingState but
// inverse. We extract the ordinals the encoder wrote.
struct DisasmState {
    bool         hasRexW = false;
    bool         hasRexR = false;
    bool         hasRexB = false;
    bool         hasModRm = false;
    std::uint8_t modRmReg3 = 0;
    std::uint8_t modRmRm3  = 0;
    std::int32_t imm32     = 0;
    bool         hasImm32  = false;
    bool         hasDisp32 = false;
    // Re-composed hwEncodings (low 3 bits + REX high bit).
    std::uint8_t modRmRegFull() const noexcept {
        return static_cast<std::uint8_t>(modRmReg3 | (hasRexR ? 0x8 : 0));
    }
    std::uint8_t modRmRmFull() const noexcept {
        return static_cast<std::uint8_t>(modRmRm3 | (hasRexB ? 0x8 : 0));
    }
};

// Try to match ONE variant against the byte head. Returns the
// number of bytes consumed iff the prefix + opcode bytes match;
// std::nullopt otherwise. The caller uses this in a first-match
// loop over the opcode's declared variants.
[[nodiscard]] std::optional<DisasmState>
tryMatchPrefix(TargetEncodingVariant const& variant,
               std::span<std::uint8_t const> bytes,
               std::size_t&                  cursor) {
    DisasmState st;

    // 1) Optional REX prefix: byte starts with 0x40..0x4F.
    if (cursor < bytes.size() && (bytes[cursor] & 0xF0) == 0x40) {
        auto const rex = bytes[cursor];
        st.hasRexW = (rex & 0x08) != 0;
        st.hasRexR = (rex & 0x04) != 0;
        // X is reserved for SIB.index high bit. No slot consumes
        // it today, and the disasm does NOT verify its value —
        // when a future SIB-bearing variant lands, the matching
        // arm here must read the bit and pass it to the SIB walker
        // for cross-check.
        st.hasRexB = (rex & 0x01) != 0;
        // Must agree with the variant's declared REX.W. (R/B are
        // operand-derived, not template-declared.)
        if (st.hasRexW != variant.tmpl.rexW) {
            return std::nullopt;
        }
        ++cursor;
    } else if (variant.tmpl.rexW) {
        // Template says REX.W but no REX byte present.
        return std::nullopt;
    }

    // 2) Opcode bytes — must match the variant's declared opcode
    //    prefix exactly.
    for (auto const expectedByte : variant.tmpl.opcodeBytes) {
        if (cursor >= bytes.size() || bytes[cursor] != expectedByte) {
            return std::nullopt;
        }
        ++cursor;
    }
    return st;
}

} // namespace

std::optional<DisassembledInst>
disassemble(TargetSchema const&            schema,
            std::uint16_t                  opcode,
            TargetOpcodeInfo const*        info,
            std::span<std::uint8_t const>  bytes,
            DiagnosticReporter&            reporter) {
    assert(info != nullptr);

    // First-match dispatch over the variant list — same discipline
    // as the encoder. Each variant attempt advances a local cursor;
    // if the prefix doesn't match, we reset and try the next.
    for (std::size_t vi = 0; vi < info->encoding.variants.size(); ++vi) {
        auto const& variant = info->encoding.variants[vi];
        std::size_t cursor   = 0;
        auto state = tryMatchPrefix(variant, bytes, cursor);
        if (!state.has_value()) continue;

        // 3) ModR/M byte: emitted iff any slot writes ModRmReg /
        //    ModRmRm OR the template uses modrmRegExt.
        bool const needsModRm =
            variant.resultSlot.has_value()
            && (*variant.resultSlot == EncodingSlotKind::ModRmReg
                || *variant.resultSlot == EncodingSlotKind::ModRmRm);
        bool needsModRmFromWire = false;
        for (auto const& w : variant.wires) {
            if (w.slotKind == EncodingSlotKind::ModRmReg
                || w.slotKind == EncodingSlotKind::ModRmRm) {
                needsModRmFromWire = true;
                break;
            }
        }
        bool const expectModRm =
            needsModRm || needsModRmFromWire
            || variant.tmpl.modrmRegExt.has_value();
        if (expectModRm) {
            if (cursor >= bytes.size()) continue;  // truncated; try next variant
            auto const modrm = bytes[cursor++];
            std::uint8_t const mod   = (modrm >> 6) & 0x3;
            std::uint8_t const reg   = (modrm >> 3) & 0x7;
            std::uint8_t const rm    = modrm & 0x7;
            // mod=3 register-direct is the only mode the encoder
            // produces in cycle scope.
            if (mod != 0b11) continue;
            // If the template uses modrmRegExt, the reg field must
            // match the declared digit.
            if (variant.tmpl.modrmRegExt.has_value()) {
                if (reg != *variant.tmpl.modrmRegExt) continue;
            } else {
                state->modRmReg3 = reg;
            }
            state->modRmRm3 = rm;
            state->hasModRm = true;
        }

        // 4) Imm32 / Disp32: appended after ModR/M. The encoder
        //    appends ALL Imm32 wires first (in wire-declaration
        //    order) then unconditionally appends the single Disp32
        //    last. This loop relies on the same invariant —
        //    Imm32 wires must precede the Disp32 wire in declaration
        //    order. validate() doesn't yet enforce this; in cycle
        //    scope every shipped variant has at most one of each
        //    (mov-imm32 has Imm32, call has Disp32, never both).
        for (auto const& w : variant.wires) {
            if (w.slotKind == EncodingSlotKind::Imm32) {
                if (cursor + 4 > bytes.size()) {
                    // Truncated; can't be this variant.
                    state = std::nullopt;
                    break;
                }
                state->imm32 = readImm32LE(bytes, cursor);
                state->hasImm32 = true;
                cursor += 4;
            } else if (w.slotKind == EncodingSlotKind::Disp32) {
                if (cursor + 4 > bytes.size()) {
                    state = std::nullopt;
                    break;
                }
                // Encoder writes ZEROS here; the bytes value is
                // expected to be 0 — verify so a non-zero displacement
                // (which would indicate the encoder forgot to leave
                // the slot patchable) fails loud.
                std::int32_t const dispBytes = readImm32LE(bytes, cursor);
                if (dispBytes != 0) {
                    state = std::nullopt;
                    break;
                }
                state->hasDisp32 = true;
                cursor += 4;
            }
        }
        if (!state.has_value()) continue;

        // Compose the disassembled inst from `state`. Slot order
        // matches the variant: resultSlot first (if present), then
        // each wire.
        DisassembledInst result{};
        result.opcode        = opcode;
        result.variantIndex  = vi;
        result.bytesConsumed = cursor;

        auto const valueForSlot = [&](EncodingSlotKind slot)
                -> std::int64_t {
            switch (slot) {
                case EncodingSlotKind::ModRmReg: return state->modRmRegFull();
                case EncodingSlotKind::ModRmRm:  return state->modRmRmFull();
                case EncodingSlotKind::Imm32:    return state->imm32;
                case EncodingSlotKind::Disp32:
                    // Symbol-bearing slot: encoder writes ZEROS at
                    // this position so the linker can patch later.
                    // The zero-check on the actual bytes lives
                    // UPSTREAM at the slot-consume loop (line ~175);
                    // moving it here would re-read 4 bytes. The
                    // upstream guard is the contract — if it ever
                    // moves out of the consume loop, the symmetry
                    // with fixed32's Imm26 in-place check breaks.
                    return 0;
                case EncodingSlotKind::Rd:
                case EncodingSlotKind::Rn:
                case EncodingSlotKind::Rm:
                case EncodingSlotKind::Imm26:
                    // fixed32 slots — never reached here (validate
                    // rejects cross-shape variants).
                    return 0;
            }
            return 0;
        };

        if (variant.resultSlot.has_value()) {
            result.slots.push_back(DisassembledSlot{
                *variant.resultSlot,
                valueForSlot(*variant.resultSlot)
            });
        }
        for (auto const& w : variant.wires) {
            result.slots.push_back(DisassembledSlot{
                w.slotKind,
                valueForSlot(w.slotKind)
            });
        }
        return result;
    }

    // No variant matched the byte head.
    (void)schema;
    report(reporter, DiagnosticCode::A_RoundTripMismatch,
           DiagnosticSeverity::Error,
           std::format("round-trip: no encoding variant of opcode '{}' "
                       "matched the encoded bytes (declared variants: {})",
                       info->mnemonic, info->encoding.variants.size()));
    return std::nullopt;
}

} // namespace dss::x86_variable_disasm
