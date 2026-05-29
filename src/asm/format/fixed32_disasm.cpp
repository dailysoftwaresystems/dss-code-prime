#include "asm/format/fixed32_disasm.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_pass_util.hpp"

#include <cassert>
#include <cstdint>
#include <format>

namespace dss::fixed32_disasm {

namespace {

using lir_pass_util::report;

struct SlotBitWindow { std::uint8_t lsb; std::uint8_t width; };

// Mirror of `fixed32::windowFor` (kept local until D-AS3-2 lifts
// walker substrate to a shared header).
[[nodiscard]] constexpr std::optional<SlotBitWindow>
windowFor(EncodingSlotKind s) noexcept {
    switch (s) {
        case EncodingSlotKind::Rd:    return SlotBitWindow{ 0,  5 };
        case EncodingSlotKind::Rn:    return SlotBitWindow{ 5,  5 };
        case EncodingSlotKind::Rm:    return SlotBitWindow{ 16, 5 };
        case EncodingSlotKind::Imm26: return SlotBitWindow{ 0,  26 };
        case EncodingSlotKind::ModRmReg:
        case EncodingSlotKind::ModRmRm:
        case EncodingSlotKind::Imm32:
        case EncodingSlotKind::Disp32:
            return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] std::uint32_t
readU32LE(std::span<std::uint8_t const> bytes, std::size_t offset) noexcept {
    return  static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1]) <<  8)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

} // namespace

std::optional<DisassembledInst>
disassemble(TargetSchema const&            schema,
            std::uint16_t                  opcode,
            TargetOpcodeInfo const*        info,
            std::span<std::uint8_t const>  bytes,
            DiagnosticReporter&            reporter) {
    assert(info != nullptr);

    if (bytes.size() < 4) {
        report(reporter, DiagnosticCode::A_RoundTripMismatch,
               DiagnosticSeverity::Error,
               std::format("round-trip: fixed32 opcode '{}' needs 4 "
                           "bytes but only {} available",
                           info->mnemonic, bytes.size()));
        return std::nullopt;
    }
    std::uint32_t const word = readU32LE(bytes, 0);

    // First-match dispatch over variants. For each candidate, build
    // the union of all slot bit-masks, mask them out of the word,
    // and compare what's left to the variant's fixedWord. Match iff
    // they're equal AND every slot's extracted value is in-range.
    for (std::size_t vi = 0; vi < info->encoding.variants.size(); ++vi) {
        auto const& variant = info->encoding.variants[vi];
        std::uint32_t slotMask = 0;
        bool          shapeOk  = true;

        auto const collectMask = [&](EncodingSlotKind kind) {
            auto const w = windowFor(kind);
            if (!w.has_value()) { shapeOk = false; return; }
            std::uint32_t const m =
                ((1u << w->width) - 1u) << w->lsb;
            slotMask |= m;
        };
        if (variant.resultSlot.has_value()) {
            collectMask(*variant.resultSlot);
        }
        for (auto const& wire : variant.wires) {
            collectMask(wire.slotKind);
        }
        if (!shapeOk) continue;
        // Bits OUTSIDE every declared slot must equal the variant's
        // fixedWord base.
        std::uint32_t const baseBits = word & ~slotMask;
        if (baseBits != (variant.tmpl.fixedWord & ~slotMask)) continue;

        // Extract each slot's value from the bit field.
        auto const extract = [&](EncodingSlotKind kind) -> std::int64_t {
            auto const w = windowFor(kind);
            std::uint32_t const bits =
                (word >> w->lsb) & ((1u << w->width) - 1u);
            return static_cast<std::int64_t>(bits);
        };

        DisassembledInst result{};
        result.opcode        = opcode;
        result.variantIndex  = vi;
        result.bytesConsumed = 4;
        if (variant.resultSlot.has_value()) {
            result.slots.push_back(DisassembledSlot{
                *variant.resultSlot, extract(*variant.resultSlot)
            });
        }
        for (auto const& wire : variant.wires) {
            // Convergence-fix A (3-agent: silent-failure #1 +
            // pr-test #3 + architect): symbol-bearing Imm26 slots —
            // the encoder writes ZEROS at the bit window (linker
            // patches at link time). The disasm MUST verify those
            // bits are actually zero — silently force-zeroing
            // (cycle-original behavior) would let any encoder
            // regression that leaves stale bits in the Imm26 slot
            // for `bl sym` pass the round-trip oracle. Mirror the
            // x86_variable disasm's Disp32-must-be-zero guard.
            if (wire.slotKind == EncodingSlotKind::Imm26) {
                std::int64_t const actualBits = extract(wire.slotKind);
                if (actualBits != 0) {
                    report(reporter, DiagnosticCode::A_RoundTripMismatch,
                           DiagnosticSeverity::Error,
                           std::format("round-trip: opcode '{}' Imm26 "
                                       "symbol slot must be all zeros "
                                       "(linker patches at link time) "
                                       "but bytes carry value {} — "
                                       "encoder failed to zero the "
                                       "symbol slot",
                                       info->mnemonic, actualBits));
                    return std::nullopt;
                }
                result.slots.push_back(DisassembledSlot{
                    wire.slotKind, /*value=*/0
                });
            } else {
                result.slots.push_back(DisassembledSlot{
                    wire.slotKind, extract(wire.slotKind)
                });
            }
        }
        return result;
    }

    (void)schema;
    report(reporter, DiagnosticCode::A_RoundTripMismatch,
           DiagnosticSeverity::Error,
           std::format("round-trip: no encoding variant of opcode '{}' "
                       "matched fixed32 word 0x{:08X} (declared variants: {})",
                       info->mnemonic, word, info->encoding.variants.size()));
    return std::nullopt;
}

} // namespace dss::fixed32_disasm
