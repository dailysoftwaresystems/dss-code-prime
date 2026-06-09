#include "asm/format/fixed32_disasm.hpp"

#include "asm/format/walker_util.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_pass_util.hpp"

#include <cassert>
#include <cstdint>
#include <format>

namespace dss::fixed32_disasm {

namespace {

using dss::report;

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
        // D-LK10-ENTRY-ARM64: MOVZ Imm16 at bits 5..20. NOT
        // symbol-bearing — extracted as a normal value (the Imm26
        // must-be-zero symbol-slot path below does not apply).
        case EncodingSlotKind::Imm16: return SlotBitWindow{ 5,  16 };
        // Every remaining slot decodes to nullopt. This is an
        // intentionally PARTIAL mirror of `fixed32::windowFor`: the
        // round-trip decoder only needs the register/immediate windows
        // the shipped fixed32 opcodes actually use (Rd/Rn/Rm/Imm26/Imm16).
        //   * x86-variable slots (ModRm*, Imm32, Disp32*, SibIndex,
        //     RipRelDisp32, CondCodeNibble, BlockRel32, MemBaseScale)
        //     never appear on a fixed32 variant — validate() rejects
        //     cross-shape variants.
        //   * The other fixed32 slots (Imm9/Imm12/Imm19/MemBaseNoScale/
        //     SymbolPatchMarker) are not decoded by this mirror yet — the
        //     disasm-completeness gap tracked by D-AS5-MULTIWORD-DISASM.
        // Both return nullopt — behavior unchanged from the prior
        // enum-drift fallback. Listed EXHAUSTIVELY (no `default:`) so the
        // D-AS-ENCODINGSLOT-EXHAUSTIVE-WARN gate flags a new enumerator.
        case EncodingSlotKind::ModRmReg:
        case EncodingSlotKind::ModRmRm:
        case EncodingSlotKind::Imm32:
        case EncodingSlotKind::Disp32:
        case EncodingSlotKind::ModRmRmMem:
        case EncodingSlotKind::MemBaseScale:
        case EncodingSlotKind::Disp32Mem:
        case EncodingSlotKind::SibIndex:
        case EncodingSlotKind::RipRelDisp32:
        case EncodingSlotKind::CondCodeNibble:
        case EncodingSlotKind::BlockRel32:
        case EncodingSlotKind::Imm9:
        case EncodingSlotKind::MemBaseNoScale:
        case EncodingSlotKind::Imm12:
        case EncodingSlotKind::SymbolPatchMarker:
        case EncodingSlotKind::Imm19:
            return std::nullopt;
    }
    // Unreachable for any in-range EncodingSlotKind (the switch is
    // exhaustive). Retained as the out-of-range-ordinal backstop and to
    // satisfy constexpr's every-path-returns rule / GCC-Clang
    // -Wreturn-type / MSVC C4715 (an enum switch is not treated as
    // exhaustive for control flow). Mirrors `slotShapeFor`.
    return std::nullopt;
}

// (`readU32LE` hoisted to `asm/format/walker_util.hpp` per D-AS3-2
// closure — architect AS5 review.)
using walker_util::readU32LE;

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

        // Extract each slot's value from the bit field. The width
        // mask `((1 << width) - 1)` guarantees the recovered value
        // is in-range for the slot's window. `shapeOk` above
        // guarantees `windowFor` returns a value for every slot
        // this variant uses — but `extract` is a private helper
        // whose contract MUST be loud about extension violations
        // (review consensus: a silent-zero return contradicts the
        // file header's "no silent fallbacks" promise). If a
        // future refactor calls `extract` with an out-of-shape
        // slot, the assertion fires in debug + the diagnostic
        // surfaces in release.
        auto const extract = [&](EncodingSlotKind kind) -> std::int64_t {
            auto const w = windowFor(kind);
            assert(w.has_value()
                && "fixed32_disasm::extract: shapeOk gate must "
                   "ensure windowFor returns a value");
            if (!w.has_value()) {
                report(reporter, DiagnosticCode::A_RoundTripMismatch,
                       DiagnosticSeverity::Error,
                       std::format("fixed32 disasm: extract called "
                                   "with slot kind '{}' that has no "
                                   "bit-window (internal-invariant: "
                                   "shapeOk gate skipped or new "
                                   "EncodingSlotKind added without "
                                   "updating windowFor)",
                                   encodingSlotKindName(kind)));
                return 0;
            }
            std::uint32_t const bits =
                (word >> w->lsb) & ((1u << w->width) - 1u);
            return static_cast<std::int64_t>(bits);
        };

        DisassembledInst result{};
        result.opcode        = opcode;
        result.variantIndex  = vi;
        // D-AS5-MULTIWORD-DISASM (deferred): this disassembler reads a
        // SINGLE 32-bit word. A multi-word `fixed32` macro (D-AS4-3,
        // e.g. AArch64 `lea` = ADRP+ADD) has `tmpl.fixedWords.size() > 1`
        // and would need a per-word match loop + summed bytesConsumed;
        // until then, asking this oracle to decode a multi-word opcode
        // fails loud (the variant's `fixedWord` base is 0, so no match →
        // A_RoundTripMismatch) rather than silently mis-decoding. The
        // shipped multi-word opcode (lea) is proven by exact byte-pins +
        // the linker reloc-formula tests, not the round-trip oracle.
        // When multi-word disasm lands, the symbol-bearing-slot
        // special-case in the wire loop below (currently Imm26-only)
        // must ALSO generalize to `isSymbolBearingSlot(wire.slotKind)`
        // so the `SymbolPatchMarker` slot disassembles to `std::nullopt`
        // (consult the Relocation, not the bytes) like Imm26 does today.
        result.bytesConsumed = 4;
        if (variant.resultSlot.has_value()) {
            result.result = DisassembledSlot{
                *variant.resultSlot, extract(*variant.resultSlot)
            };
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
                // D-AS5-3: `nullopt` marks "symbol-bearing slot —
                // consult the Relocation entry, not the bytes."
                result.wires.push_back(DisassembledSlot{
                    wire.slotKind, std::nullopt
                });
            } else {
                result.wires.push_back(DisassembledSlot{
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
