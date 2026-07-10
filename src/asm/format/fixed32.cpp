#include "asm/format/fixed32.hpp"

#include "asm/format/byte_emit.hpp"
#include "asm/format/walker_util.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <format>
#include <optional>
#include <span>

namespace dss::fixed32 {

namespace {

using dss::report;

// (`filterToLirKind`, `operandsMatchGuard`, and `hwEncodingOf`
// hoisted to `asm/format/walker_util.hpp` per D-AS3-2 closure.)
using walker_util::hwEncodingOf;
using walker_util::operandsMatchGuard;

// AArch64-style 5-bit register fields — capacity 0..31 (X0..X30 +
// XZR=31). Future SVE-style widening would gain its own slot
// vocabulary and a wider constant here.
constexpr std::uint8_t kFixed32RegFieldBits = 5;

// Per-slot bit-window descriptor. `lsb` is the bit position of the
// slot's least-significant bit inside the 32-bit fixed word. `width`
// is the slot's bit width.
struct SlotBitWindow {
    std::uint8_t lsb;
    std::uint8_t width;
};

// (`PendingRelocSlot` hoisted to walker_util.hpp; alias kept minimal.)
using walker_util::PendingRelocSlot;

// Map each fixed32 EncodingSlotKind to its bit window. The shape-vs-
// slot validate rule guarantees only Fixed32 slots reach this
// function for a fixed32 walker call.
[[nodiscard]] constexpr std::optional<SlotBitWindow>
windowFor(EncodingSlotKind s) noexcept {
    switch (s) {
        case EncodingSlotKind::Rd:    return SlotBitWindow{ 0,  5 };
        case EncodingSlotKind::Rn:    return SlotBitWindow{ 5,  5 };
        case EncodingSlotKind::Rm:    return SlotBitWindow{ 16, 5 };
        // Ra (D-LIR-MOD-MSUB-FUSION): the multiply-accumulate family's
        // third source register at bits 10..14 (MADD/MSUB Xd, Xn, Xm,
        // Xa). A plain 5-bit register window like Rd/Rn/Rm.
        case EncodingSlotKind::Ra:    return SlotBitWindow{ 10, 5 };
        // Imm26 (ARM64 branch displacement, e.g. `bl imm26`) — the
        // operand value at bits 0..25 of the fixed word. Cycle-4
        // scope: source is always a `SymbolRef`; the wire emits a
        // Relocation entry, the bits are left at 0 (linker patches).
        case EncodingSlotKind::Imm26: return SlotBitWindow{ 0,  26 };
        // Imm16 (D-LK10-ENTRY-ARM64): AArch64 MOVZ wide-immediate at
        // bits 5..20 of the fixed word (Rd at 0..4). Unlike Imm26 this
        // is NOT symbol-bearing — the walker writes the operand's
        // immediate value directly (range-checked in the wire loop).
        case EncodingSlotKind::Imm16: return SlotBitWindow{ 5,  16 };
        // Imm9 (D-LK10-ENTRY-ARM64): AArch64 unscaled LDUR/STUR signed
        // byte offset at bits 12..20. The wire-loop range-checks signed
        // -256..255 and writes the low 9 bits (two's-complement).
        case EncodingSlotKind::Imm9:  return SlotBitWindow{ 12, 9 };
        // Imm19 (D-AS3-BLOCK-REL-IMM19/26): AArch64 B.cond signed 19-bit
        // PC-relative branch offset at bits 5..23 (the cond nibble sits
        // at bits 0..3). BLOCK-RELATIVE — the walker writes ZERO bits
        // here (a BlockRef operand pushes a BlockRelPatch instead; the
        // asm.cpp resolver patches the 19-bit field at assemble time).
        // Present so windowFor knows the slot's bit-position for the
        // resolver-side lsb/width derivation symmetry; the encoder's
        // BlockRef arm bypasses orInto (no immediate written upfront).
        case EncodingSlotKind::Imm19: return SlotBitWindow{ 5,  19 };
        // MemBaseNoScale (D-LK10-ENTRY-ARM64): width-0 marker for a
        // memory-base operand on an ISA with no scale field (AArch64
        // unscaled). orInto with width 0 writes nothing but marks the
        // slot consumed (satisfying the "every guard position is wired"
        // validate rule). lsb is irrelevant at width 0.
        case EncodingSlotKind::MemBaseNoScale: return SlotBitWindow{ 0, 0 };
        // MemOffsetZero (D-AS4-ARM64-BASE-INDEX-LEA): width-0 marker for
        // a zero displacement on the base+index `lea` (AArch64 `ADD Xd,
        // Xn, Xm` has no disp field). orInto width 0 writes nothing; the
        // MemOffset wire arm range-checks the displacement IS zero first
        // (fail-loud on a nonzero disp). The displacement twin of
        // MemBaseNoScale.
        case EncodingSlotKind::MemOffsetZero: return SlotBitWindow{ 0, 0 };
        // Imm12 (D-LK10-ENTRY-ARM64): AArch64 ADD/SUB-immediate
        // unsigned 12-bit field at bits 10..21 (frame-size stack
        // adjust). Range-checked 0..4095 in the wire loop.
        case EncodingSlotKind::Imm12: return SlotBitWindow{ 10, 12 };
        // Imm12Scaled (D-ASM-AARCH64-LARGE-FRAME-IMM12): the SAME bit-
        // window as Imm12 (bits 10..21), but the MemOffset wire arm writes
        // the SCALED value `byteOffset / accessSizeBytes` here (vs Imm12's
        // raw byte value). Distinct slot, identical window — the encode
        // arithmetic differs, not the placement.
        case EncodingSlotKind::Imm12Scaled: return SlotBitWindow{ 10, 12 };
        // Imm12HiLo24 (D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12): the SAME
        // bits 10..21 window as Imm12. `windowFor` returns the per-WORD
        // window (the low 12 bits in word0, the high 12 bits in word1
        // both land in this same bit-window); the wire arm performs the
        // value SPLIT and calls `orInto` twice with this window — once
        // per word. Identical placement to Imm12, distinct slot + distinct
        // (word-pair) encode arithmetic.
        case EncodingSlotKind::Imm12HiLo24: return SlotBitWindow{ 10, 12 };
        // Imm32MovzMovk (D-ASM-AARCH64-FRAME-OFFSET-BEYOND-16MIB): the
        // MOVZ/MOVK halfword window — bits 5..20, the SAME window as Imm16
        // (AArch64 MOVZ/MOVK both carry imm16 at [20:5], Rd at [4:0]).
        // `windowFor` returns the per-WORD window; the wire arm (writeMovzMovk
        // helper) splits the 32-bit value lo16→word0 / hi16→word1 and calls
        // `orInto` twice with this window — once per MOVZ/MOVK word. The
        // third (operation) word carries no Imm32MovzMovk bits.
        case EncodingSlotKind::Imm32MovzMovk: return SlotBitWindow{ 5, 16 };
        // SymbolPatchMarker (D-AS4-3): width-0 symbol-patch marker, like
        // MemBaseNoScale. The walker writes NO bits (the linker patches
        // the whole field via the wire's relocationKind); the slot only
        // marks the position consumed + emits the relocation. lsb is
        // irrelevant at width 0.
        case EncodingSlotKind::SymbolPatchMarker: return SlotBitWindow{ 0, 0 };
        // x86-variable slots — never reached on a fixed32 variant
        // (validate() rejects cross-shape variants). Returning nullopt
        // makes the walker fail loud (`orInto`) if a schema bypasses
        // validate() somehow. Listed EXHAUSTIVELY (every x86-variable
        // EncodingSlotKind, no `default:`) so a newly-added enumerator
        // re-triggers -Wswitch / -Werror=switch here and forces a
        // deliberate fixed32-vs-x86 classification — the compile-time
        // lockstep gate D-AS-ENCODINGSLOT-EXHAUSTIVE-WARN.
        case EncodingSlotKind::ModRmReg:
        case EncodingSlotKind::ModRmRm:
        case EncodingSlotKind::Imm32:
        case EncodingSlotKind::Imm8:
        case EncodingSlotKind::Disp32:
        case EncodingSlotKind::ModRmRmMem:
        case EncodingSlotKind::MemBaseScale:
        case EncodingSlotKind::Disp32Mem:
        case EncodingSlotKind::SibIndex:
        case EncodingSlotKind::RipRelDisp32:
        case EncodingSlotKind::CondCodeNibble:
        case EncodingSlotKind::BlockRel32:
        // D-CSUBSET-BITFIELD-WIDE-UNIT: `mov r64, imm64` slots are
        // x86-variable (opcode-byte register + 8-byte immediate); no
        // fixed32 bit-window.
        case EncodingSlotKind::OpcodePlusReg:
        case EncodingSlotKind::Imm64:
        // TLS C1 (D-CSUBSET-THREAD-LOCAL): the absolute-SIB disp32 +
        // the relocated memory displacement are ModR/M-byte constructs
        // — x86-variable only, no fixed32 bit-window.
        case EncodingSlotKind::AbsoluteDisp32Mem:
        case EncodingSlotKind::MemRelocDisp32:
            return std::nullopt;
    }
    // Unreachable for any in-range EncodingSlotKind — the switch above
    // is exhaustive. Retained as the out-of-range-ordinal backstop and
    // to satisfy the compiler's reachability rule: a constexpr function
    // must return on every path, and GCC/Clang (-Wreturn-type) / MSVC
    // (C4715) do not treat an enum switch as exhaustive for control
    // flow. Mirrors `slotShapeFor`'s trailing return.
    return std::nullopt;
}

// (LE byte emission moved to `asm/format/byte_emit.hpp` — shared with
// the x86_variable walker.)

} // namespace

bool encode(Lir const&                  lir,
            TargetSchema const&         schema,
            LirInstId                   inst,
            TargetOpcodeInfo const*     info,
            // `lirToMir` + `srcMap` stamped by dispatch (asm.cpp);
            // reserved here for future per-byte-range stamping.
            std::span<MirInstId const>  /*lirToMir*/,
            std::vector<std::uint8_t>&  out,
            std::vector<Relocation>&    relocs,
            std::vector<SourceMapEntry>& /*srcMap*/,
            // D-AS3-BLOCK-REL-IMM19/26 (ARM64 control-flow): fixed32
            // now USES blockPatches — a `BlockRef` operand on the Imm19
            // (B.cond) or Imm26 (B) slot is an intra-function branch
            // resolved at assemble time. The walker pushes a
            // `BlockRelPatch{ kind = Arm64Imm19 | Arm64Imm26 }`; the
            // asm.cpp resolver patches the bit-field (different
            // arithmetic from x86's BlockRel32 — no +4 bias, /4 scale).
            std::vector<walker_util::BlockRelPatch>& blockPatches,
            std::vector<walker_util::BlockSymPatch>& blockSymPatches,
            DiagnosticReporter&         reporter) {
    assert(info != nullptr && "fixed32::encode requires non-null info");

    auto const instOps    = lir.instOperands(inst);
    LirReg const result = lir.instResult(inst);

    // First-match variant selection over (operandKinds, width) — the
    // FC3 c2 width axis (D-CSUBSET-32BIT-ALU-FORMS) is format-agnostic:
    // the fixed32 walker consults the SAME shared matcher as
    // x86_variable (arm64 W-forms = width:32 variants whose fixedWord
    // clears bit 31; width-absent variants match any width).
    std::uint8_t const instWidth = lirInstWidthBits(lir.instFlags(inst));
    TargetEncodingVariant const* selected = nullptr;
    for (auto const& v : info->encoding.variants) {
        if (walker_util::variantMatchesInst(instOps, instWidth, v)) {
            selected = &v;
            break;
        }
    }
    if (selected == nullptr) {
        report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
               DiagnosticSeverity::Error,
               std::format("opcode '{}': no encoding variant matches "
                           "this instruction's operand kinds at width "
                           "{} (declared variants: {})",
                           info->mnemonic, instWidth,
                           info->encoding.variants.size()));
        return false;
    }

    // D-AS4-3 (multi-instruction-macro encoder): the instruction emits
    // ONE OR MORE 32-bit words. `words[i]` is the working value of word
    // i (seeded from the template's base bit pattern); single-word
    // opcodes (the vast majority) have words.size()==1 == the prior
    // `fixedWord` path, byte-identical.
    std::vector<std::uint32_t> words(selected->tmpl.wordCount());
    for (std::size_t i = 0; i < words.size(); ++i) {
        words[i] = selected->tmpl.wordAt(i);
    }

    // D-AS3-COND-CODE-ARM64 (mirror of x86_variable's condCodeFromPayload):
    // when the selected variant's template declares `condCodeFromPayload`,
    // read the inst's payload as a `TargetCondCode`, look up the schema's
    // `condCodeEncoding` nibble, and OR it into WORD 0 bits 0..3. AArch64
    // `B.cond` carries the 4-bit condition field at bits [3:0] of the
    // instruction word (word 0 of the jcc macro; word 1 is the trailing
    // unconditional `B`). Fail-loud when the target hasn't loaded a
    // `condCodeEncoding` table — a missing table would silently OR zero
    // (= TargetCondCode::Eq's nibble on x86, but ARM64's EQ is also 0) and
    // EVERY conditional branch would resolve as `b.eq`. Same payload
    // range-gate as x86 (the full TargetCondCode enum incl. the FC3.5
    // float arms; an undeclared float ENTRY also fails loud — the
    // composed-FCmp lowering must never reach a single-cc inst here).
    if (selected->tmpl.condCodeFromPayload) {
        auto const condValue = lir.instPayload(inst);
        if (condValue >= kTargetCondCodeCount) {
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': cond-code payload {} is out of "
                               "range [0..{}] for TargetCondCode "
                               "(eq/ne/slt/sle/sgt/sge/ult/ule/ugt/uge + "
                               "fogt/foge/foeq/fone/fune/fuo/ford)",
                               info->mnemonic, condValue,
                               kTargetCondCodeCount - 1));
            return false;
        }
        auto const condNibble = schema.condCodeEncoding(
            static_cast<TargetCondCode>(condValue));
        if (!condNibble.has_value()) {
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': variant declares "
                               "condCodeFromPayload but target schema '{}' "
                               "declares no `condCodeEncoding` entry for "
                               "cond '{}'",
                               info->mnemonic, schema.name(),
                               targetCondCodeName(static_cast<TargetCondCode>(
                                   condValue))));
            return false;
        }
        // Place the 4-bit cond nibble at the template's declared bit
        // position, optionally inverted (D-AS3-COND-CODE-ARM64). Both
        // knobs default to the B.cond shape (condBitPos=0, condInvert=
        // false), so the OR is `words[0] |= nibble` — byte-identical to
        // the pre-knob behavior. AArch64 `CSET` sets condBitPos=12 +
        // condInvert=true: `CSET Xd,cond` = `CSINC Xd,XZR,XZR,invert(cond)`
        // (the false-arm increments XZR→1), so the encoded condition is
        // the INVERSE of the requested one (e.g. GT=0xC → 0xD at bits
        // 12..15). The XOR-with-1 is the standard AArch64 condition-
        // inversion (it flips the low bit of the 4-bit code, pairing
        // EQ↔NE, GT↔LE, GE↔LT, etc.). The base word leaves the target
        // bits zero, so the OR is non-destructive.
        std::uint32_t condNib = static_cast<std::uint32_t>(*condNibble);
        if (selected->tmpl.condInvert) {
            condNib ^= 1u;
        }
        words[0] |= (condNib & 0x0Fu) << selected->tmpl.condBitPos;
    }

    // Track which slot windows have been written — defense-in-depth
    // mirror of x86_variable's `wroteModRm*` guard. validate() should
    // have rejected schemas declaring two writers to the same slot,
    // but if a future synthetic Lir reaches the encoder, the
    // assertion fails loud rather than silently OR-corrupting bits.
    // PER-WORD (D-AS4-3): one bitset per emitted word — the same slot
    // kind legitimately appears once in EACH word of a multi-word macro
    // (lea writes Rd into word 0 AND word 1). Each bitset is sized from
    // the shared `kEncodingSlotKindCount` so adding a slot to the enum +
    // table is the SAME change, no manual array-size update.
    std::vector<std::array<bool, kEncodingSlotKindCount>>
        wroteSlot(words.size());

    // `value` is uint32 so the SAME helper writes both 5-bit register
    // encodings (hwEncoding ≤ 31) and wider immediates (e.g. the
    // 16-bit MOVZ Imm16 slot). The width mask below clips to the
    // slot's declared bit-width; the wire loop range-checks an
    // immediate BEFORE calling this (a too-wide value fails loud,
    // never silently masked here). `wordIndex` selects which emitted
    // word the slot's bit-window is OR'd into.
    auto const orInto = [&](EncodingSlotKind slot,
                            std::uint32_t value,
                            std::uint8_t wordIndex) -> bool {
        auto const w = windowFor(slot);
        if (!w.has_value()) {
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': encoding slot '{}' is not "
                               "in the fixed32 vocabulary "
                               "(schema-vs-walker drift — validate() "
                               "should have rejected the schema)",
                               info->mnemonic,
                               encodingSlotKindName(slot)));
            return false;
        }
        if (wordIndex >= words.size()) {
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': slot '{}' wordIndex {} "
                               "exceeds the template's {} word(s) "
                               "(validate() should have rejected the schema)",
                               info->mnemonic, encodingSlotKindName(slot),
                               wordIndex, words.size()));
            return false;
        }
        auto const slotIdx = static_cast<std::size_t>(slot);
        // `slotIdx < kSlotCount` holds by the closed-enum invariant
        // (every enum value has a row in `kEncodingSlotKindTable`)
        // — but check defensively in case a future entry skips a row.
        if (slotIdx >= kEncodingSlotKindCount) {
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': slot kind ordinal {} "
                               "exceeds tracked-slot count {} "
                               "(enum-vs-table drift)",
                               info->mnemonic, slotIdx,
                               kEncodingSlotKindCount));
            return false;
        }
        if (wroteSlot[wordIndex][slotIdx]) {
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': second writer to "
                               "fixed32 slot '{}' in word {} (validate() "
                               "should have rejected the schema)",
                               info->mnemonic,
                               encodingSlotKindName(slot), wordIndex));
            return false;
        }
        wroteSlot[wordIndex][slotIdx] = true;
        std::uint32_t const mask = ((1u << w->width) - 1u) << w->lsb;
        std::uint32_t const bits =
            (static_cast<std::uint32_t>(value) & ((1u << w->width) - 1u))
            << w->lsb;
        words[wordIndex] = (words[wordIndex] & ~mask) | bits;
        return true;
    };

    // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12: write a value V into the
    // shifted-imm12 word-pair slot. V must be NON-NEGATIVE and fit
    // 0..0xFFFFFF (16 MiB-1); the low 12 bits go into `word[wordIndex]`'s
    // imm12 window (sh=0, the base word) and the high 12 bits into
    // `word[wordIndex+1]`'s imm12 window (sh=1, the LSL #12 word — its
    // fixedWords[wordIndex+1] sets bit 22). The two words form `op
    // Xd,Xn,#lo` then `op Xd,Xd,#hi,LSL #12` (the Rd-threading is the
    // variant's extraResultSlots, NOT this helper's concern). SCRATCH-
    // FREE: word1 reads its own dest. A value > 0xFFFFFF fails loud
    // (the residual D-ASM-AARCH64-FRAME-OFFSET-BEYOND-16MIB). `valueDesc`
    // names the operand (immediate vs memory offset) for the diagnostic.
    // Both halves OR through `orInto`, so the per-word wroteSlot collision
    // guard fires if a malformed schema double-writes either word.
    constexpr std::uint32_t kImm12HiLo24Max = 0xFFFFFFu;  // 16 MiB - 1
    auto const writeHiLo24 = [&](std::int64_t value,
                                 std::uint8_t wordIndex,
                                 std::string_view valueDesc) -> bool {
        if (value < 0 || value > static_cast<std::int64_t>(kImm12HiLo24Max)) {
            report(reporter, DiagnosticCode::A_ImmediateOperandOutOfRange,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': {} {} is out of range for the "
                               "shifted 'imm12.hilo24' word-pair (valid "
                               "0..{}, i.e. 24 bits / 16 MiB) — a larger "
                               "frame needs a third word or a MOVZ/MOVK "
                               "scratch materialization (not yet supported, "
                               "D-ASM-AARCH64-FRAME-OFFSET-BEYOND-16MIB)",
                               info->mnemonic, valueDesc, value,
                               kImm12HiLo24Max));
            return false;
        }
        // word[wordIndex+1] must exist — the variant's template MUST be a
        // 2-word macro whose word1 carries sh=1. A schema wiring this slot
        // into a single-word template is a config bug; orInto's wordIndex
        // bound check fails loud on the hi half. (Belt-and-suspenders: the
        // lo orInto already validates wordIndex itself.)
        std::uint32_t const v   = static_cast<std::uint32_t>(value);
        std::uint32_t const lo  = v & 0xFFFu;
        std::uint32_t const hi  = (v >> 12) & 0xFFFu;
        if (!orInto(EncodingSlotKind::Imm12HiLo24, lo, wordIndex)) return false;
        if (!orInto(EncodingSlotKind::Imm12HiLo24, hi,
                    static_cast<std::uint8_t>(wordIndex + 1)))
            return false;
        return true;
    };

    // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-16MIB: write a value V into the
    // MOVZ/MOVK 3-word materialization slot. V must be NON-NEGATIVE and fit
    // 0..0x7FFFFFFF (the int32 frame-size ceiling — a frame > 2 GiB flows
    // through `int32_t` as a NEGATIVE magnitude and never reaches this slot,
    // the residual D-ASM-AARCH64-FRAME-OFFSET-BEYOND-2GIB). The low 16 bits
    // go into `word[wordIndex]`'s imm16 window (the MOVZ word — its
    // fixedWords[wordIndex] is the `MOVZ Xs,#imm16` base) and the high 16
    // bits into `word[wordIndex+1]`'s imm16 window (the MOVK word — its
    // fixedWords[wordIndex+1] is the `MOVK Xs,#imm16,LSL #16` base, hw=01).
    // The scratch register Xs (x16 baked into the sub/add base words, or the
    // lea's threaded dest reg) is the variant's concern via the base words +
    // resultSlot/extraResultSlots, NOT this helper's — exactly like
    // `writeHiLo24` writes only the value split, never the Rd thread. The
    // THIRD word (the extended-register operation) carries no Imm32MovzMovk
    // bits. The halfword split mirrors `materializeViaMovkLadder`
    // (mir_to_lir.cpp): `chunk[k] = (V >> 16*k) & 0xFFFF`. `valueDesc` names
    // the operand (immediate vs memory offset) for the diagnostic. Both
    // halves OR through `orInto`, so the per-word wroteSlot collision guard
    // fires if a malformed schema double-writes either MOVZ/MOVK word.
    constexpr std::int64_t kMovzMovkMax = 0x7FFFFFFF;  // int32 frame ceiling
    auto const writeMovzMovk = [&](std::int64_t value,
                                   std::uint8_t wordIndex,
                                   std::string_view valueDesc) -> bool {
        if (value < 0 || value > kMovzMovkMax) {
            report(reporter, DiagnosticCode::A_ImmediateOperandOutOfRange,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': {} {} is out of range for the "
                               "MOVZ/MOVK 3-word materialization (valid "
                               "0..{}, i.e. the non-negative int32 frame "
                               "ceiling) — a frame > 2 GiB has no encoding "
                               "(D-ASM-AARCH64-FRAME-OFFSET-BEYOND-2GIB)",
                               info->mnemonic, valueDesc, value,
                               kMovzMovkMax));
            return false;
        }
        std::uint32_t const v   = static_cast<std::uint32_t>(value);
        std::uint32_t const lo  = v & 0xFFFFu;          // chunk 0 → MOVZ word
        std::uint32_t const hi  = (v >> 16) & 0xFFFFu;  // chunk 1 → MOVK word
        if (!orInto(EncodingSlotKind::Imm32MovzMovk, lo, wordIndex))
            return false;
        if (!orInto(EncodingSlotKind::Imm32MovzMovk, hi,
                    static_cast<std::uint8_t>(wordIndex + 1)))
            return false;
        return true;
    };

    // Wire the result register into its declared slot (word 0), plus any
    // extra placements (D-AS4-3 — a multi-word macro threading the SAME
    // destination register through later words, e.g. lea's ADD reads
    // Xd as both its dest AND its source base).
    if (selected->resultSlot.has_value()) {
        auto const hw = hwEncodingOf(result, schema, info->mnemonic,
                                      kFixed32RegFieldBits, reporter);
        if (!hw.has_value()) return false;
        if (!orInto(*selected->resultSlot, *hw, /*wordIndex=*/0)) return false;
        for (auto const& extra : selected->extraResultSlots) {
            if (!orInto(extra.slotKind, *hw, extra.wordIndex)) return false;
        }
    }

    // Wire each declared source operand into its slot.
    // Plan 13 AS4: a `SymbolRef` operand on a symbol-bearing slot emits
    // a Relocation entry; the slot's bits stay 0 in the emitted word
    // (linker patches at link time). D-AS4-3: an instruction may now
    // emit MULTIPLE relocations (lea = ADRP page-reloc on word 0 + ADD
    // lo12-reloc on word 1), accumulated here and stamped per-word in
    // the emit loop below.
    std::vector<PendingRelocSlot> pendingRelocs;
    // D-AS3-BLOCK-REL-IMM19/26: a `BlockRef` operand wired to an Imm19
    // (B.cond) or Imm26 (B) slot is an INTRA-FUNCTION branch — resolved
    // at assemble time (NOT a linker relocation). Accumulated here, the
    // patch stamped at the START of the wire's word in the emit loop
    // (the patch byte-offset is DERIVED from the emit cursor, never a
    // separately computed base+i*4 — same discipline as the per-word
    // reloc stamping). One macro word may carry its own block patch:
    // the jcc 2-word macro pushes a `B.cond` Imm19 patch at word 0 AND
    // a `B` Imm26 patch at word 1 (the fallthrough). `kind` selects the
    // resolver arithmetic. `wordIndex` defers the byte-offset to emit.
    struct PendingBlockPatch {
        std::uint32_t                     targetBlock;
        walker_util::BlockRelPatchKind    kind;
        std::uint8_t                      wordIndex;
    };
    std::vector<PendingBlockPatch> pendingBlockPatches;
    for (auto const& wire : selected->wires) {
        if (wire.index >= instOps.size()) {
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': variant operand wire index "
                               "{} exceeds operand count {}",
                               info->mnemonic, wire.index, instOps.size()));
            return false;
        }
        auto const& srcOp = instOps[wire.index];
        if (srcOp.kind == LirOperandKind::Reg) {
            auto const hw = hwEncodingOf(srcOp.reg, schema, info->mnemonic,
                                          kFixed32RegFieldBits, reporter);
            if (!hw.has_value()) return false;
            if (!orInto(wire.slotKind, *hw, wire.wordIndex)) return false;
        } else if (srcOp.kind == LirOperandKind::SymbolRef) {
            // D-AS4-3: a SymbolRef may be wired to ANY symbol-bearing
            // slot (Imm26 for BL, or the generic SymbolPatchMarker for
            // ADRP/ADD-lo12). The walker emits a Relocation + writes no
            // immediate bits; the linker patches the field per the
            // wire's relocationKind. Widened from the prior Imm26-only
            // restriction to the generic `isSymbolBearingSlot` predicate
            // — no per-slot enumeration, so any future symbol-patched
            // ISA field is admitted with its relocationKind.
            if (!isSymbolBearingSlot(wire.slotKind)) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': SymbolRef operand wired "
                                   "to non-symbol-bearing slot '{}' — a "
                                   "symbol reference needs a symbol-bearing "
                                   "slot (e.g. imm26 or sym.patch)",
                                   info->mnemonic,
                                   encodingSlotKindName(wire.slotKind)));
                return false;
            }
            if (!wire.relocationKind.has_value()) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': SymbolRef wire to slot "
                                   "'{}' has no `relocationKind` — "
                                   "validate() should have rejected this "
                                   "schema",
                                   info->mnemonic,
                                   encodingSlotKindName(wire.slotKind)));
                return false;
            }
            if (wire.wordIndex >= words.size()) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': SymbolRef wire wordIndex "
                                   "{} exceeds the template's {} word(s)",
                                   info->mnemonic, wire.wordIndex,
                                   words.size()));
                return false;
            }
            // Mark the (word, slot) as "written" so the per-word
            // wroteSlot guard catches a same-word same-slot collision.
            // An out-of-range slotIdx must fail loud (enum-vs-table
            // drift) — silently skipping would let a future enum
            // addition propagate as wrong-byte encoding.
            auto const slotIdx = static_cast<std::size_t>(wire.slotKind);
            if (slotIdx >= kEncodingSlotKindCount) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': SymbolRef slot kind "
                                   "ordinal {} exceeds tracked-slot "
                                   "count {} (enum-vs-table drift)",
                                   info->mnemonic, slotIdx,
                                   kEncodingSlotKindCount));
                return false;
            }
            if (wroteSlot[wire.wordIndex][slotIdx]) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': SymbolRef wire "
                                   "collides with already-written "
                                   "slot '{}' in word {}",
                                   info->mnemonic,
                                   encodingSlotKindName(wire.slotKind),
                                   wire.wordIndex));
                return false;
            }
            wroteSlot[wire.wordIndex][slotIdx] = true;
            pendingRelocs.push_back(PendingRelocSlot{
                *wire.relocationKind,
                SymbolId{srcOp.symbolV},
                wire.wordIndex
            });
        } else if (srcOp.kind == LirOperandKind::BlockRef) {
            // D-AS3-BLOCK-REL-IMM19/26 (ARM64 intra-function branch):
            // the BlockRef names a target basic block resolved at
            // assemble time. The slot MUST be Imm19 (B.cond) or Imm26
            // (B) — the two ARM64 block-relative branch displacement
            // fields. The walker writes NO immediate bits here (the
            // word's base pattern leaves the field zero); it records a
            // `BlockRelPatch` whose `kind` carries the per-ISA resolver
            // arithmetic, stamped at the word's start in the emit loop.
            //
            // (Imm26 is DUAL-USE: a SymbolRef operand on Imm26 above
            // emits a `call26` linker relocation [BL]; a BlockRef
            // operand on Imm26 here is the intra-function `B`. The
            // encoder distinguishes by OPERAND KIND, not by slot.)
            walker_util::BlockRelPatchKind patchKind;
            if (wire.slotKind == EncodingSlotKind::Imm19) {
                patchKind = walker_util::BlockRelPatchKind::Arm64Imm19;
            } else if (wire.slotKind == EncodingSlotKind::Imm26) {
                patchKind = walker_util::BlockRelPatchKind::Arm64Imm26;
            } else {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': BlockRef operand wired to "
                                   "slot '{}' — fixed32 supports intra-"
                                   "function branch targets only on the "
                                   "Imm19 (B.cond) or Imm26 (B) slots",
                                   info->mnemonic,
                                   encodingSlotKindName(wire.slotKind)));
                return false;
            }
            if (wire.wordIndex >= words.size()) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': BlockRef wire wordIndex {} "
                                   "exceeds the template's {} word(s)",
                                   info->mnemonic, wire.wordIndex,
                                   words.size()));
                return false;
            }
            // Mark the (word, slot) consumed so the per-word wroteSlot
            // guard catches a same-word same-slot collision. Out-of-range
            // slotIdx fails loud (enum-vs-table drift) — mirrors the
            // SymbolRef arm.
            auto const slotIdx = static_cast<std::size_t>(wire.slotKind);
            if (slotIdx >= kEncodingSlotKindCount) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': BlockRef slot kind ordinal "
                                   "{} exceeds tracked-slot count {} "
                                   "(enum-vs-table drift)",
                                   info->mnemonic, slotIdx,
                                   kEncodingSlotKindCount));
                return false;
            }
            if (wroteSlot[wire.wordIndex][slotIdx]) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': BlockRef wire collides with "
                                   "already-written slot '{}' in word {}",
                                   info->mnemonic,
                                   encodingSlotKindName(wire.slotKind),
                                   wire.wordIndex));
                return false;
            }
            wroteSlot[wire.wordIndex][slotIdx] = true;
            pendingBlockPatches.push_back(PendingBlockPatch{
                srcOp.blockSlot, patchKind, wire.wordIndex});
        } else if (srcOp.kind == LirOperandKind::ImmInt) {
            // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12: the shifted-imm12
            // word-pair slot. The callconv's prologue/epilogue `sub/add
            // sp,#frame` (an [reg, ImmInt] form) wires its frame size here
            // when it exceeds the single-word imm12 reach (4095). The
            // value is split lo/hi and written into BOTH words of the
            // 2-word macro (`sub sp,sp,#lo` then `sub sp,sp,#hi,LSL #12`).
            // Handled in its own arm BEFORE the Imm16/Imm12 reject so it
            // does not leak into the single-word range logic. The variant
            // selector (immMin/immMax magnitude key) routes a >4095 frame
            // to this 2-word variant and a ≤4095 frame to the single-word
            // Imm12 variant — the encoder writes whichever it is handed.
            if (wire.slotKind == EncodingSlotKind::Imm12HiLo24) {
                if (!writeHiLo24(static_cast<std::int64_t>(srcOp.immInt32),
                                 wire.wordIndex, "immediate"))
                    return false;
                continue;
            }
            // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-16MIB: the MOVZ/MOVK 3-word
            // form. The callconv's prologue/epilogue `sub/add sp,#frame`
            // (an [reg, ImmInt] form) wires its frame size here when it
            // exceeds the 2-word shifted-imm12 reach (0xFFFFFF). The value
            // materializes into the x16 scratch (MOVZ x16 + MOVK x16) across
            // words 0/1, and word2 is the extended-register `sub/add sp,sp,
            // x16`. Handled BEFORE the Imm16/Imm12 reject so the 3-word slot
            // never leaks into the single-word range logic. The variant
            // selector (immMin:0x1000000 magnitude key) routes a >16MiB
            // frame here; the encoder writes whichever it is handed.
            if (wire.slotKind == EncodingSlotKind::Imm32MovzMovk) {
                if (!writeMovzMovk(static_cast<std::int64_t>(srcOp.immInt32),
                                   wire.wordIndex, "immediate"))
                    return false;
                continue;
            }
            // Immediate operand → an UNSIGNED immediate fixed32 slot.
            // Cycle scope (D-LK10-ENTRY-ARM64): Imm16 (AArch64 MOVZ
            // wide-immediate) or Imm12 (AArch64 ADD/SUB-immediate frame
            // adjust). Mirrors the SymbolRef arm's slot restriction; a
            // future immediate slot adds its case + window when its
            // consumer cycle lands (the file-header convention). The
            // slot's bit-WIDTH defines the valid range — no hardcoded
            // per-slot bound.
            if (wire.slotKind != EncodingSlotKind::Imm16
                && wire.slotKind != EncodingSlotKind::Imm12) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': ImmInt operand wired to "
                                   "slot '{}' — fixed32 supports immediate "
                                   "operands only on the Imm16 (MOVZ) or "
                                   "Imm12 (ADD/SUB) slots in this cycle",
                                   info->mnemonic,
                                   encodingSlotKindName(wire.slotKind)));
                return false;
            }
            auto const w = windowFor(wire.slotKind);
            if (!w.has_value()) {
                // walker-internal drift: windowFor must know the slot
                // (added in lockstep with the enum). Fail loud rather
                // than silently skip the immediate.
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': slot '{}' has no bit-window "
                                   "(walker-vs-enum drift)", info->mnemonic,
                                   encodingSlotKindName(wire.slotKind)));
                return false;
            }
            // These forms take an UNSIGNED immediate. Reject negatives
            // and any value wider than the slot — silent truncation would
            // emit a WRONG machine-code constant (a wrong syscall number
            // / a wrong frame size). A wider constant needs a multi-
            // instruction or shifted materialization, anchored for a
            // future cycle. Unsuppressable (A_ImmediateOperandOutOfRange).
            std::int32_t const imm = srcOp.immInt32;
            std::uint32_t const maxVal = (1u << w->width) - 1u;
            if (imm < 0 || static_cast<std::uint32_t>(imm) > maxVal) {
                report(reporter, DiagnosticCode::A_ImmediateOperandOutOfRange,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': immediate {} is out of range "
                                   "for the {}-bit '{}' slot (valid 0..{}) — a "
                                   "wider constant needs a multi-instruction "
                                   "or shifted materialization, not yet "
                                   "supported",
                                   info->mnemonic, imm, w->width,
                                   encodingSlotKindName(wire.slotKind), maxVal));
                return false;
            }
            // orInto marks wroteSlot (same collision/double-write guard
            // as the Reg arm).
            if (!orInto(wire.slotKind, static_cast<std::uint32_t>(imm),
                        wire.wordIndex))
                return false;
        } else if (srcOp.kind == LirOperandKind::MemOffset) {
            // D-AS4-ARM64-NEGATIVE-DISP-LEA-NATIVE-SUB: a `negMemoffset`
            // variant (the arm64 `SUB Xd,Xn,#|disp|` negative-disp lea) writes
            // the ABSOLUTE VALUE of a NEGATIVE displacement into its (unsigned)
            // imm12 / shifted-imm12 / MOVZ-MOVK slot — the subtract semantics
            // live in the SUB base word, not the field, so the field carries
            // |disp|. The matcher (variantNegMagnitude) only routes a strictly-
            // negative memoffset here, but the encoder DEFENDS the invariant:
            // a non-negative disp on a negMemoffset variant is a lowering/
            // schema bug and fails LOUD (never silently negate a positive into
            // a wrong address). `effectiveDisp` is |disp| for the negMemoffset
            // path (computed as -(int64) so INT32_MIN widens cleanly) and the
            // raw signed disp otherwise — so every slot arm below feeds the
            // correct magnitude with no per-arm sign branching. A non-
            // negMemoffset variant is byte-identical to before (effectiveDisp
            // == srcOp.offset).
            std::int64_t effectiveDisp = srcOp.offset;
            if (selected->negMemoffset) {
                if (srcOp.offset >= 0) {
                    report(reporter, DiagnosticCode::A_ImmediateOperandOutOfRange,
                           DiagnosticSeverity::Error,
                           std::format("opcode '{}': the negative-displacement "
                                       "form (SUB base) requires a NEGATIVE "
                                       "memory offset (got {}) — a non-negative "
                                       "displacement must route to the positive "
                                       "ADD form; the sign matcher should have "
                                       "excluded this",
                                       info->mnemonic, srcOp.offset));
                    return false;
                }
                effectiveDisp = -static_cast<std::int64_t>(srcOp.offset);
            }
            // A memory displacement → one of two AArch64 displacement
            // fields, distinguished by slot:
            //   * Imm9  — SIGNED 9-bit unscaled LDUR/STUR byte offset
            //             (bits 12..20), range -256..255.
            //   * Imm12 — UNSIGNED 12-bit ADD-immediate field (bits
            //             10..21), range 0..4095. The frame-relative
            //             `lea Xd, [sp + #disp]` materialized by the
            //             callconv (alloca → ADD Xd, sp, #imm12) wires
            //             its non-negative frame offset here. (Same slot
            //             the `add`/`sub` immediate variants already use;
            //             frame offsets are non-negative, so the unsigned
            //             imm12 reach is the right fit — larger than Imm9.)
            //             D-AS4-ARM64-NEGATIVE-DISP-LEA-NATIVE-SUB: the SUB
            //             negative-disp lea also wires |disp| here for
            //             |disp| ≤ 4095.
            // A future scaled LDR/STR form adds its own slot when that
            // consumer lands. Mirrors the ImmInt arm's dual-slot shape.
            //
            // D-AS4-ARM64-BASE-INDEX-LEA: the THIRD slot is the width-0
            // MemOffsetZero marker (the base+index `lea` = `ADD Xd,Xn,Xm`
            // has NO displacement field). Validate the displacement IS
            // zero — a nonzero disp with an index has no single-ADD form,
            // so fail LOUD rather than silently drop the offset — then
            // write no bits (orInto width 0 marks the position consumed).
            // Handled before the Imm9/Imm12 reject so it does not leak
            // into the displacement-range logic.
            if (wire.slotKind == EncodingSlotKind::MemOffsetZero) {
                if (srcOp.offset != 0) {
                    report(reporter, DiagnosticCode::A_ImmediateOperandOutOfRange,
                           DiagnosticSeverity::Error,
                           std::format("opcode '{}': the base+index 'lea' "
                                       "(ADD Xd,Xn,Xm) requires a ZERO "
                                       "displacement (got {}) — AArch64 has no "
                                       "displacement field on the register-add "
                                       "form; a base+index+disp address needs a "
                                       "separate ADD (no consumer yet)",
                                       info->mnemonic, srcOp.offset));
                    return false;
                }
                if (!orInto(wire.slotKind, 0u, wire.wordIndex)) return false;
                continue;
            }
            // D-ASM-AARCH64-LARGE-FRAME-IMM12: the SCALED imm12 LDR/STR
            // form (`load_u`/`store_u`). The unsigned-offset LDR/STR
            // encodes its displacement as a SCALED field — `imm12 =
            // byteOffset / accessSizeBytes` — where accessSizeBytes is the
            // load/store access width in bytes (8 for a 64-bit LDR, 4 for
            // 32-bit, 2 for 16-bit, 1 for 8-bit). This is the form a frame
            // offset BEYOND the unscaled imm9 ±256 takes (the ≥9-fixed-
            // param AAPCS64 callee loading its 9th incoming-stack param at
            // `[sp + frameSize]`). The displacement MUST be NON-NEGATIVE
            // (the unsigned-offset form has no sign bit), ACCESS-SIZE-
            // ALIGNED (the field is scaled by the access size — a non-
            // multiple has no representation; an unaligned LDR is
            // architecturally undefined), and the scaled field MUST fit 12
            // bits (0..4095 ⇒ a 64-bit reach of 32760). Each is a distinct
            // fail-loud A_ImmediateOperandOutOfRange (a frame offset that
            // is negative-and-out-of-imm9, OR aligned-but >32760, OR non-
            // aligned-and-out-of-imm9 stays fail-loud — the residual
            // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12; the shifted
            // imm12<<12 form / scratch-register address materialization is
            // the future closing work). Handled in its own arm BEFORE the
            // Imm9/Imm12 reject so the scaled slot never leaks into the
            // unscaled range logic.
            if (wire.slotKind == EncodingSlotKind::Imm12Scaled) {
                auto const w = windowFor(wire.slotKind);
                if (!w.has_value()) {
                    report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                           DiagnosticSeverity::Error,
                           std::format("opcode '{}': slot '{}' has no bit-window "
                                       "(walker-vs-enum drift)", info->mnemonic,
                                       encodingSlotKindName(wire.slotKind)));
                    return false;
                }
                // accessSizeBytes from the inst's operation width (max(1,
                // width/8) — width is 8/16/32/64 here, so this is 1/2/4/8;
                // the max guards a 0 width defensively). Deriving the scale
                // from the SAME width axis the variant selector matched on
                // keeps the scaled encode width-exact (a 32-bit LDR scales
                // by 4, a 64-bit by 8) with no per-opcode constant.
                std::uint32_t const accessSizeBytes =
                    std::max(1u, static_cast<std::uint32_t>(instWidth) / 8u);
                std::int32_t const disp = srcOp.offset;
                if (disp < 0) {
                    report(reporter, DiagnosticCode::A_ImmediateOperandOutOfRange,
                           DiagnosticSeverity::Error,
                           std::format("opcode '{}': memory offset {} is negative "
                                       "but the scaled unsigned-offset LDR/STR "
                                       "'{}' slot encodes only non-negative "
                                       "displacements — a negative frame offset "
                                       "needs the signed unscaled imm9 form or a "
                                       "scratch-register address (not yet "
                                       "supported)",
                                       info->mnemonic, disp,
                                       encodingSlotKindName(wire.slotKind)));
                    return false;
                }
                if (static_cast<std::uint32_t>(disp) % accessSizeBytes != 0) {
                    report(reporter, DiagnosticCode::A_ImmediateOperandOutOfRange,
                           DiagnosticSeverity::Error,
                           std::format("opcode '{}': memory offset {} is not a "
                                       "multiple of the {}-byte access size for "
                                       "the scaled '{}' slot — the unsigned-"
                                       "offset LDR/STR field is scaled by the "
                                       "access size, so an unaligned offset has "
                                       "no encoding (an unaligned LDR is "
                                       "architecturally undefined); use the "
                                       "unscaled imm9 form",
                                       info->mnemonic, disp, accessSizeBytes,
                                       encodingSlotKindName(wire.slotKind)));
                    return false;
                }
                std::uint32_t const scaled =
                    static_cast<std::uint32_t>(disp) / accessSizeBytes;
                std::uint32_t const maxVal = (1u << w->width) - 1u;
                if (scaled > maxVal) {
                    report(reporter, DiagnosticCode::A_ImmediateOperandOutOfRange,
                           DiagnosticSeverity::Error,
                           std::format("opcode '{}': memory offset {} scales to "
                                       "{} which exceeds the unsigned {}-bit "
                                       "'{}' field (valid 0..{}, i.e. a byte "
                                       "reach of {}) — a larger frame needs the "
                                       "shifted imm12<<12 LDR form or a scratch-"
                                       "register address (not yet supported)",
                                       info->mnemonic, disp, scaled, w->width,
                                       encodingSlotKindName(wire.slotKind),
                                       maxVal, maxVal * accessSizeBytes));
                    return false;
                }
                if (!orInto(wire.slotKind, scaled, wire.wordIndex))
                    return false;
                continue;
            }
            // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12: the shifted-imm12
            // word-pair form for a frame-relative `lea Xd,[base,#disp]`
            // (the callconv's `emitFrameAddr` → `ADD Xd,Xn,#imm`). When the
            // frame offset exceeds the single-word imm12 reach (4095), the
            // variant selector routes the GEP/alloca-base lea to the 2-word
            // ADD-imm macro; the displacement is split lo/hi and written
            // into BOTH words (`ADD Xd,Xn,#lo` then `ADD Xd,Xd,#hi,LSL
            // #12`). The lea's RESULT register threads through word1 (Rd
            // AND Rn) via the variant's extraResultSlots — scratch-free.
            // Handled BEFORE the Imm9/Imm12 reject so the word-pair slot
            // never leaks into the single-word range logic. `writeHiLo24`
            // owns the non-negative + 24-bit-magnitude fail-loud gate.
            // D-AS4-ARM64-NEGATIVE-DISP-LEA-NATIVE-SUB: the negative-disp lea's
            // shifted `SUB Xd,Xn,#|disp|; SUB Xd,Xd,#hi,LSL#12` word-pair rides
            // the SAME imm12.hilo24 slot; `effectiveDisp` is already |disp| for
            // the negMemoffset variant, so writeHiLo24 splits the magnitude
            // identically to the positive ADD word-pair (the SUB base word
            // makes it a subtract).
            if (wire.slotKind == EncodingSlotKind::Imm12HiLo24) {
                if (!writeHiLo24(effectiveDisp,
                                 wire.wordIndex, "memory offset"))
                    return false;
                continue;
            }
            // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-16MIB: the MOVZ/MOVK 3-word
            // form for a frame-relative `lea Xd,[base,#disp]` whose disp
            // exceeds the 2-word shifted-imm12 reach (0xFFFFFF). The GEP's
            // high-element address (e.g. `int big[5000000]; big[4999999]` at
            // sp+~20MB). SCRATCH-FREE: the displacement materializes into the
            // lea's DEST reg Xd (MOVZ Xd + MOVK Xd, threaded via resultSlot/
            // extraResultSlots), then word2 is `add Xd, sp, Xd` (extended,
            // Rn=sp). Handled BEFORE the Imm9/Imm12 reject so the 3-word slot
            // never leaks into the single-word range logic. `writeMovzMovk`
            // owns the non-negative + int32-ceiling fail-loud gate.
            // D-AS4-ARM64-NEGATIVE-DISP-LEA-NATIVE-SUB: the negative-disp lea's
            // MOVZ/MOVK + extended-register `SUB Xd,Xn,Xd` 3-word form rides
            // the SAME imm32.movzmovk slot; `effectiveDisp` is |disp|, so the
            // magnitude materializes into the dest reg identically to the
            // positive ADD 3-word form (word2's SUB base subtracts it).
            if (wire.slotKind == EncodingSlotKind::Imm32MovzMovk) {
                if (!writeMovzMovk(effectiveDisp,
                                   wire.wordIndex, "memory offset"))
                    return false;
                continue;
            }
            bool const isSignedSlot = wire.slotKind == EncodingSlotKind::Imm9;
            bool const isUnsignedSlot = wire.slotKind == EncodingSlotKind::Imm12;
            if (!isSignedSlot && !isUnsignedSlot) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': MemOffset operand wired to "
                                   "slot '{}' — fixed32 supports memory "
                                   "displacements on the Imm9 slot (signed, "
                                   "unscaled LDUR/STUR) or the Imm12 slot "
                                   "(unsigned, ADD-immediate frame address)",
                                   info->mnemonic,
                                   encodingSlotKindName(wire.slotKind)));
                return false;
            }
            auto const w = windowFor(wire.slotKind);
            if (!w.has_value()) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': slot '{}' has no bit-window "
                                   "(walker-vs-enum drift)", info->mnemonic,
                                   encodingSlotKindName(wire.slotKind)));
                return false;
            }
            // D-AS4-ARM64-NEGATIVE-DISP-LEA-NATIVE-SUB: the value the field
            // receives. For the unsigned Imm12 slot the SUB negative-disp lea
            // wires |disp| here (effectiveDisp is already |disp| ≤ 4095 for the
            // negMemoffset variant, the magnitude range-checked below). For the
            // signed Imm9 slot negMemoffset is always false, so effectiveDisp
            // == srcOp.offset — the two's-complement range/write is unchanged.
            std::int32_t const disp = static_cast<std::int32_t>(effectiveDisp);
            if (isSignedSlot) {
                // SIGNED range derived from the slot WIDTH (two's-
                // complement): [-(2^(w-1)), 2^(w-1)-1] — for Imm9 that is
                // -256..255. Deriving from width (not a baked literal)
                // keeps the signed path as generic as the unsigned arm;
                // fail loud rather than silently truncate to a WRONG
                // stack slot. A larger frame needs the scaled LDR/STR
                // imm12 form (future).
                std::int32_t const lo = -(1 << (w->width - 1));
                std::int32_t const hi = (1 << (w->width - 1)) - 1;
                if (disp < lo || disp > hi) {
                    report(reporter, DiagnosticCode::A_ImmediateOperandOutOfRange,
                           DiagnosticSeverity::Error,
                           std::format("opcode '{}': memory offset {} is out "
                                       "of range for the signed {}-bit '{}' "
                                       "slot (valid {}..{}) — a larger frame "
                                       "needs the scaled LDR/STR imm12 form, "
                                       "not yet supported",
                                       info->mnemonic, disp, w->width,
                                       encodingSlotKindName(wire.slotKind),
                                       lo, hi));
                    return false;
                }
            } else {
                // UNSIGNED range derived from the slot WIDTH: 0..2^w-1 —
                // for Imm12 that is 0..4095. A negative frame offset is a
                // lowering bug (locals sit at non-negative SP offsets
                // post-prologue), and a larger frame needs the shifted
                // imm12<<12 form (future); fail loud rather than silently
                // wrap a negative into a huge positive offset.
                std::int64_t const maxVal = (std::int64_t{1} << w->width) - 1;
                if (disp < 0 || disp > maxVal) {
                    report(reporter, DiagnosticCode::A_ImmediateOperandOutOfRange,
                           DiagnosticSeverity::Error,
                           std::format("opcode '{}': memory offset {} is out "
                                       "of range for the unsigned {}-bit '{}' "
                                       "slot (valid 0..{}) — a larger frame "
                                       "needs the shifted ADD imm12<<12 form, "
                                       "not yet supported",
                                       info->mnemonic, disp, w->width,
                                       encodingSlotKindName(wire.slotKind),
                                       maxVal));
                    return false;
                }
            }
            // orInto masks to the slot's bit-window (two's-complement for
            // a negative Imm9 displacement) + marks wroteSlot.
            if (!orInto(wire.slotKind, static_cast<std::uint32_t>(disp),
                        wire.wordIndex))
                return false;
        } else if (srcOp.kind == LirOperandKind::MemBase) {
            // The memory-base SCALE marker. AArch64 unscaled addressing
            // has no scale field, so this operand encodes ZERO bits — it
            // is wired to the width-0 MemBaseNoScale slot purely to
            // satisfy the "every guard position is wired" validate rule.
            // Validate scale==1 here (the only place it is checked) and
            // write nothing.
            if (wire.slotKind != EncodingSlotKind::MemBaseNoScale) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': MemBase operand wired to "
                                   "slot '{}' — fixed32 supports memory-base "
                                   "operands only on the MemBaseNoScale slot "
                                   "(unscaled addressing) in this cycle",
                                   info->mnemonic,
                                   encodingSlotKindName(wire.slotKind)));
                return false;
            }
            if (srcOp.scale != 1u) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': fixed32 memory addressing "
                                   "supports only scale=1 (unscaled "
                                   "LDUR/STUR) — got scale {}",
                                   info->mnemonic, srcOp.scale));
                return false;
            }
            // orInto with the width-0 slot writes no bits but marks
            // wroteSlot[MemBaseNoScale] (collision/consumed tracking).
            if (!orInto(wire.slotKind, 0u, wire.wordIndex)) return false;
        } else {
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': variant operand {} has "
                               "unsupported kind for fixed32 (not Reg / "
                               "SymbolRef / BlockRef / ImmInt / MemOffset / "
                               "MemBase; ordinal {})",
                               info->mnemonic, wire.index,
                               static_cast<int>(srcOp.kind)));
            return false;
        }
    }

    // D-CSUBSET-COMPUTED-GOTO: a block-address `lea` carries a trailing
    // UNWIRED BlockRef operand (naming the target LIR block) ALONGSIDE its
    // SymbolRef (the synthetic per-block symbol, captured into
    // `pendingRelocs` as the relocation source above — on arm64 the
    // ADRP+ADD pair yields TWO relocs, both against the SAME synthetic
    // symbol). That BlockRef contributes NO bytes (a block reference is
    // never byte-encoded data); it is the SYMBOL ↔ BLOCK binding the
    // assembler records so the synthetic symbol can be assigned that
    // block's interior VA at link time. Scan for an UNWIRED BlockRef and
    // pair it with the captured symbol.
    //
    // CRUCIAL: only an UNWIRED BlockRef is the block-address binding. A
    // WIRED BlockRef is a branch displacement (jmp's Imm26 / jcc's Imm19),
    // already consumed by the wire loop above as an intra-function branch
    // patch — it has no SymbolRef and is NOT a block-sym binding. Skipping
    // wired BlockRefs keeps this scan from firing on every branch
    // instruction (which would fail-loud spuriously). The block-address
    // `lea` is the only opcode that carries an UNWIRED BlockRef. A binding
    // BlockRef WITHOUT a captured SymbolRef is malformed (no symbol to
    // bind) — fail loud. Byte-identical mirror of x86_variable's scan.
    std::vector<bool> wiredOperand(instOps.size(), false);
    for (auto const& wire : selected->wires) {
        if (wire.index < wiredOperand.size()) wiredOperand[wire.index] = true;
    }
    for (std::size_t oi = 0; oi < instOps.size(); ++oi) {
        if (instOps[oi].kind != LirOperandKind::BlockRef) continue;
        if (wiredOperand[oi]) continue;  // wired BlockRef = a branch displacement
        if (pendingRelocs.empty()) {
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': an unwired BlockRef operand "
                               "(block-address binding) appears with no "
                               "SymbolRef operand to bind — a block-address "
                               "`lea` must carry the synthetic per-block "
                               "symbol as its relocation source",
                               info->mnemonic));
            return false;
        }
        blockSymPatches.push_back(walker_util::BlockSymPatch{
            pendingRelocs.front().target, instOps[oi].blockSlot});
    }

    // Emit each word LE, stamping that word's relocations at the word's
    // START (D-AS4-3 — the multi-word / multi-relocation closure). For
    // each word i, every pending reloc whose `wordIndex == i` is appended
    // at the CURRENT `out.size()` — which, because word i's 4 bytes are
    // appended immediately after, is exactly word i's start (the byte
    // position the linker's `readInst32` reads). The per-word offset is
    // DERIVED from the emit cursor, never a separately computed
    // `base + i*4`. Single-word opcodes (words.size()==1, all relocs at
    // wordIndex 0) behave exactly as the prior single-trailing-slot model.
    for (std::size_t i = 0; i < words.size(); ++i) {
        for (auto const& pr : pendingRelocs) {
            if (pr.wordIndex == i) {
                walker_util::appendPendingReloc(relocs, out, pr);
            }
        }
        // D-AS3-BLOCK-REL-IMM19/26: stamp each intra-function branch
        // patch at the START of its word — `out.size()` here is exactly
        // word i's first byte (its 4 bytes are appended immediately
        // after), the offset the asm.cpp resolver read-modify-writes.
        // The word already carries zero in the Imm19/Imm26 field (the
        // base pattern leaves it clear), so no placeholder write is
        // needed beyond the word itself.
        for (auto const& bp : pendingBlockPatches) {
            if (bp.wordIndex == i) {
                blockPatches.push_back(walker_util::BlockRelPatch{
                    static_cast<std::uint32_t>(out.size()),
                    bp.targetBlock,
                    bp.kind});
            }
        }
        asm_byte_emit::appendU32LE(out, words[i]);
    }
    return true;
}

} // namespace dss::fixed32
