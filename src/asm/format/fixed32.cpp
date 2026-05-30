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

using lir_pass_util::report;

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
        // x86-variable slots — never reached on a fixed32 variant
        // (validate() rejects). Returning nullopt makes the walker
        // fail loud if a schema bypasses validate() somehow.
        case EncodingSlotKind::ModRmReg:
        case EncodingSlotKind::ModRmRm:
        case EncodingSlotKind::Imm32:
        case EncodingSlotKind::Disp32:
            return std::nullopt;
    }
    // Enum-drift fallback — a new EncodingSlotKind value added
    // without a matching arm above falls here. Returning nullopt
    // makes the walker fail loud (`orInto` emits a hard diagnostic).
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

    // Track which slot windows have been written — defense-in-depth
    // mirror of x86_variable's `wroteModRm*` guard. validate() should
    // have rejected schemas declaring two writers to the same slot,
    // but if a future synthetic Lir reaches the encoder, the
    // assertion fails loud rather than silently OR-corrupting bits.
    // One bit per EncodingSlotKind value — sized from the shared
    // `kEncodingSlotKindCount` so adding a new slot to the enum +
    // table is the SAME change, no manual array-size update.
    std::array<bool, kEncodingSlotKindCount> wroteSlot{};

    std::uint32_t word = selected->tmpl.fixedWord;

    auto const orInto = [&](EncodingSlotKind slot,
                            std::uint8_t value) -> bool {
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
        auto const slotIdx = static_cast<std::size_t>(slot);
        // `slotIdx < kSlotCount` holds by the closed-enum invariant
        // (every enum value has a row in `kEncodingSlotKindTable`)
        // — but check defensively in case a future entry skips a row.
        if (slotIdx >= wroteSlot.size()) {
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': slot kind ordinal {} "
                               "exceeds tracked-slot count {} "
                               "(enum-vs-table drift)",
                               info->mnemonic, slotIdx, wroteSlot.size()));
            return false;
        }
        if (wroteSlot[slotIdx]) {
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': second writer to "
                               "fixed32 slot '{}' (validate() "
                               "should have rejected the schema)",
                               info->mnemonic,
                               encodingSlotKindName(slot)));
            return false;
        }
        wroteSlot[slotIdx] = true;
        std::uint32_t const mask = ((1u << w->width) - 1u) << w->lsb;
        std::uint32_t const bits =
            (static_cast<std::uint32_t>(value) & ((1u << w->width) - 1u))
            << w->lsb;
        word = (word & ~mask) | bits;
        return true;
    };

    // Wire the result register into its declared slot.
    if (selected->resultSlot.has_value()) {
        auto const hw = hwEncodingOf(result, schema, info->mnemonic,
                                      kFixed32RegFieldBits, reporter);
        if (!hw.has_value()) return false;
        if (!orInto(*selected->resultSlot, *hw)) return false;
    }

    // Wire each declared source operand into its slot.
    // Plan 13 AS4: a `SymbolRef` operand on a symbol-bearing slot
    // (Imm26) emits a Relocation entry; the slot's bits stay 0 in
    // the emitted word (linker patches at link time). At most one
    // symbol-relative wire per fixed32 instruction (cycle-4 cap).
    std::optional<PendingRelocSlot> pendingReloc;
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
            if (!orInto(wire.slotKind, *hw)) return false;
        } else if (srcOp.kind == LirOperandKind::SymbolRef) {
            if (wire.slotKind != EncodingSlotKind::Imm26) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': SymbolRef operand wired "
                                   "to non-Imm26 slot '{}' — cycle-4 "
                                   "fixed32 supports symbol references "
                                   "only on Imm26 slots",
                                   info->mnemonic,
                                   encodingSlotKindName(wire.slotKind)));
                return false;
            }
            if (!wire.relocationKind.has_value()) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': wire to Imm26 has no "
                                   "`relocationKind` — validate() "
                                   "should have rejected this schema",
                                   info->mnemonic));
                return false;
            }
            if (pendingReloc.has_value()) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': second symbol-relative "
                                   "wire — only one per fixed32 "
                                   "instruction in cycle-4",
                                   info->mnemonic));
                return false;
            }
            // Mark the slot as "written" so the wroteSlot guard
            // catches a same-slot collision with a Reg wire.
            //
            // Convergence-fix A (code-reviewer + silent-failure
            // 2-agent): an out-of-range slotIdx must fail loud (the
            // enum-vs-table drift case `orInto` already guards in
            // the Reg arm). Silently skipping the check here would
            // let a future enum addition propagate as wrong-byte
            // encoding.
            auto const slotIdx = static_cast<std::size_t>(wire.slotKind);
            if (slotIdx >= wroteSlot.size()) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': SymbolRef slot kind "
                                   "ordinal {} exceeds tracked-slot "
                                   "count {} (enum-vs-table drift)",
                                   info->mnemonic, slotIdx,
                                   wroteSlot.size()));
                return false;
            }
            if (wroteSlot[slotIdx]) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': SymbolRef wire "
                                   "collides with already-written "
                                   "slot '{}'",
                                   info->mnemonic,
                                   encodingSlotKindName(wire.slotKind)));
                return false;
            }
            wroteSlot[slotIdx] = true;
            pendingReloc = PendingRelocSlot{
                *wire.relocationKind,
                SymbolId{srcOp.symbolV}
            };
        } else {
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': variant operand {} has "
                               "non-Reg, non-SymbolRef kind (ordinal {})",
                               info->mnemonic, wire.index,
                               static_cast<int>(srcOp.kind)));
            return false;
        }
    }

    // Emit the relocation BEFORE the word bytes — the reloc's offset
    // is `out.size()` at this point (the byte position the linker
    // patches into). INVARIANT (silent-failure F4): in cycle-4 the
    // symbol-bearing slot's bytes are coincident with the only word
    // this walker writes, so the offset captured here points AT the
    // patch site. When a future fixed32 instruction has multiple
    // words OR a non-trailing symbol slot, the offset computation
    // must move to a per-slot byte-offset table on the variant
    // template (anchored at plan 13 §3.1 D-AS4-3).
    if (pendingReloc.has_value()) {
        walker_util::appendPendingReloc(relocs, out, *pendingReloc);
    }
    asm_byte_emit::appendU32LE(out, word);
    return true;
}

} // namespace dss::fixed32
