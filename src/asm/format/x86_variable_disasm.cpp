#include "asm/format/x86_variable_disasm.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_pass_util.hpp"

#include <cassert>
#include <cstdint>
#include <format>

namespace dss::x86_variable_disasm {

namespace {

using dss::report;

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
    // FC3.5 sweep-c1: the 1-byte immediate (shift-count ib).
    std::uint8_t imm8      = 0;
    bool         hasImm8   = false;
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

    // 0) Mandatory legacy-prefix bytes (FC2 Part B) — the encoder
    //    emits them BEFORE the REX prefix, so the disasm consumes
    //    them first. A variant whose declared prefix doesn't match
    //    the byte head cannot be this variant.
    for (auto const expectedByte : variant.tmpl.mandatoryPrefix) {
        if (cursor >= bytes.size() || bytes[cursor] != expectedByte) {
            return std::nullopt;
        }
        ++cursor;
    }

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
            } else if (w.slotKind == EncodingSlotKind::Imm8) {
                // FC3.5: the 1-byte shift-count ib — emitted right
                // after ModR/M (the encoder appends imm8s before any
                // imm32s; shipped variants carry at most one imm).
                if (cursor + 1 > bytes.size()) {
                    state = std::nullopt;
                    break;
                }
                state->imm8 = bytes[cursor];
                state->hasImm8 = true;
                cursor += 1;
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

        // Defensive: each accessor returns nullopt when the
        // corresponding state field was never populated. Without
        // these guards, a future encoder variant whose ModRm/Imm32
        // slot consume path didn't fire would silently return 0 (the
        // default-initialized field value) — which collides with
        // legitimate values and round-trips as "match." (silent-
        // failure-hunter HIGH-3.)
        auto const valueForSlot = [&](EncodingSlotKind slot)
                -> std::optional<std::int64_t> {
            switch (slot) {
                case EncodingSlotKind::ModRmReg:
                    return state->hasModRm
                        ? std::optional<std::int64_t>{state->modRmRegFull()}
                        : std::nullopt;
                case EncodingSlotKind::ModRmRm:
                    return state->hasModRm
                        ? std::optional<std::int64_t>{state->modRmRmFull()}
                        : std::nullopt;
                case EncodingSlotKind::Imm32:
                    return state->hasImm32
                        ? std::optional<std::int64_t>{state->imm32}
                        : std::nullopt;
                case EncodingSlotKind::Imm8:
                    return state->hasImm8
                        ? std::optional<std::int64_t>{state->imm8}
                        : std::nullopt;
                case EncodingSlotKind::Disp32:
                    // Symbol-bearing slot: encoder writes ZEROS at
                    // this position so the linker can patch later.
                    // `nullopt` (D-AS5-3) explicitly marks "value
                    // undefined here — consult the Relocation entry"
                    // and prevents collision with a legitimate Imm32
                    // value of 0. The zero-check on the actual bytes
                    // lives upstream at the slot-consume loop.
                    return std::nullopt;
                case EncodingSlotKind::Rd:
                case EncodingSlotKind::Rn:
                case EncodingSlotKind::Rm:
                case EncodingSlotKind::Ra:
                case EncodingSlotKind::Imm26:
                    // fixed32 slots. Validate-time rules reject
                    // cross-shape variants, but if a future variant
                    // drift reached this arm, returning nullopt
                    // would silently masquerade as a SymbolRef slot
                    // (the roundTripVerify accepts nullopt for
                    // SymbolRef). Fail loud — silent-failure-hunter
                    // CRITICAL-2 convergence.
                    report(reporter, DiagnosticCode::A_RoundTripMismatch,
                           DiagnosticSeverity::Error,
                           std::format("round-trip: opcode '{}' variant "
                                       "{} declared a fixed32 slot kind "
                                       "'{}' in an x86-variable walker "
                                       "(substrate-invariant violation: "
                                       "validate() should have rejected "
                                       "this cross-shape variant before "
                                       "the walker ran)",
                                       info->mnemonic, vi,
                                       encodingSlotKindName(slot)));
                    return std::nullopt;
                // Remaining slots disassemble to nullopt (value undefined
                // here). Behavior-PRESERVED from the prior trailing
                // fallback — two sub-classes, both undecoded by this x86
                // walker today:
                //   * x86-variable slots not yet decoded (ModRmRmMem,
                //     Disp32Mem, SibIndex, MemBaseScale, RipRelDisp32,
                //     CondCodeNibble, BlockRel32) — the disasm-completeness
                //     gap tracked by D-AS5-MULTIWORD-DISASM.
                //   * fixed32 slots (Imm16/Imm9/Imm12/Imm19/MemBaseNoScale/
                //     SymbolPatchMarker) that the Rd/Rn/Rm/Imm26 arm above
                //     would fail-loud on (CRITICAL-2): a silent `nullopt`
                //     here would let such a slot masquerade as a patched
                //     SymbolRef in `roundTripVerify` (the exact hazard the
                //     loud arm guards). They reach here only under a cross-
                //     shape variant validate() already rejects, so
                //     converging them onto that fail-loud arm is a behavior
                //     change deferred out of this hygiene cycle
                //     (D-AS-DISASM-FIXED32-SLOT-FAILLOUD).
                // Listed EXHAUSTIVELY (no `default:`) so a new enumerator
                // re-triggers the -Werror=switch gate here.
                case EncodingSlotKind::ModRmRmMem:
                case EncodingSlotKind::MemBaseScale:
                case EncodingSlotKind::Disp32Mem:
                case EncodingSlotKind::SibIndex:
                case EncodingSlotKind::RipRelDisp32:
                case EncodingSlotKind::CondCodeNibble:
                case EncodingSlotKind::BlockRel32:
                case EncodingSlotKind::Imm16:
                case EncodingSlotKind::Imm9:
                case EncodingSlotKind::MemBaseNoScale:
                case EncodingSlotKind::Imm12:
                case EncodingSlotKind::SymbolPatchMarker:
                case EncodingSlotKind::Imm19:
                    return std::nullopt;
            }
            // Unreachable for any in-range EncodingSlotKind (the switch is
            // exhaustive). Out-of-range-ordinal backstop + satisfies
            // GCC-Clang -Wreturn-type / MSVC C4715 (an enum switch is not
            // treated as exhaustive for control flow).
            return std::nullopt;
        };

        if (variant.resultSlot.has_value()) {
            result.result = DisassembledSlot{
                *variant.resultSlot,
                valueForSlot(*variant.resultSlot)
            };
        }
        for (auto const& w : variant.wires) {
            result.wires.push_back(DisassembledSlot{
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
