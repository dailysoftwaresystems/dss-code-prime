#include "asm/format/fixed32.hpp"

#include "asm/format/byte_emit.hpp"
#include "asm/format/walker_util.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"

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
        // Imm12 (D-LK10-ENTRY-ARM64): AArch64 ADD/SUB-immediate
        // unsigned 12-bit field at bits 10..21 (frame-size stack
        // adjust). Range-checked 0..4095 in the wire loop.
        case EncodingSlotKind::Imm12: return SlotBitWindow{ 10, 12 };
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
        case EncodingSlotKind::Disp32:
        case EncodingSlotKind::ModRmRmMem:
        case EncodingSlotKind::MemBaseScale:
        case EncodingSlotKind::Disp32Mem:
        case EncodingSlotKind::SibIndex:
        case EncodingSlotKind::RipRelDisp32:
        case EncodingSlotKind::CondCodeNibble:
        case EncodingSlotKind::BlockRel32:
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
            DiagnosticReporter&         reporter) {
    assert(info != nullptr && "fixed32::encode requires non-null info");

    auto const instOps    = lir.instOperands(inst);
    LirReg const result = lir.instResult(inst);

    // First-match variant selection.
    TargetEncodingVariant const* selected = nullptr;
    for (auto const& v : info->encoding.variants) {
        if (operandsMatchGuard(instOps, v.operandKinds)) {
            selected = &v;
            break;
        }
    }
    if (selected == nullptr) {
        report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
               DiagnosticSeverity::Error,
               std::format("opcode '{}': no encoding variant matches "
                           "this instruction's operand kinds (declared "
                           "variants: {})",
                           info->mnemonic,
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
    // range-gate as x86 (0..9 = the 10-arm TargetCondCode enum).
    if (selected->tmpl.condCodeFromPayload) {
        auto const condValue = lir.instPayload(inst);
        if (condValue > 9u) {
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': cond-code payload {} is out of "
                               "range [0..9] for TargetCondCode "
                               "(eq/ne/slt/sle/sgt/sge/ult/ule/ugt/uge)",
                               info->mnemonic, condValue));
            return false;
        }
        auto const condNibble = schema.condCodeEncoding(
            static_cast<TargetCondCode>(condValue));
        if (!condNibble.has_value()) {
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': variant declares "
                               "condCodeFromPayload but target schema '{}' "
                               "has no `condCodeEncoding` table",
                               info->mnemonic, schema.name()));
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
            // A future scaled LDR/STR form adds its own slot when that
            // consumer lands. Mirrors the ImmInt arm's dual-slot shape.
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
            std::int32_t const disp = srcOp.offset;
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
