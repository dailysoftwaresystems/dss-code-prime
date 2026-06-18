#pragma once

// Shared substrate for format walkers (plan 13 §3.1 D-AS3-2 —
// architect AS5 reviewed: "no blocker exists, extract now").
//
// Both encoders (`x86_variable` / `fixed32`) and both disassemblers
// (`x86_variable_disasm` / `fixed32_disasm`) duplicated the same
// helpers:
//   * `hwEncodingOf` — encoder-side: resolve a Reg operand's hwEncoding
//     ordinal with a target-blind register-table lookup + bit-width
//     defense (parameterized so x86's 4-bit limit and fixed32's 5-bit
//     limit both fit the same shape)
//   * `operandsMatchGuard` — encoder-side + disasm-side: per-position
//     LIR-operand-kind vs. variant-guard equality, with `filterToLirKind`
//     translating the closed `OperandKindFilter` vocabulary to the LIR
//     boundary
//   * `readU32LE` — disasm-side: read 4 little-endian bytes as a uint32
//
// Hoisted here so adding the third walker (RISC-V compressed 16-bit /
// VLIW bundle / etc., per D-AS3-2's trigger) reuses these directly
// instead of forking a third copy.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"
#include "lir/lir_reg.hpp"

#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace dss::walker_util {

// Resolve the operand's `hwEncoding` ordinal from the schema register
// table. `maxBitWidth` is the format-shape's encoding-field width
// (x86-variable: 4 bits / ordinal 0..15 for REX-extended ModR/M;
// fixed32: 5 bits / ordinal 0..31 for AArch64-style 5-bit reg fields).
// Emits `A_NoMatchingEncodingVariant` and returns nullopt on any of:
// non-physical register, unknown ordinal, or hwEncoding exceeding the
// shape's bit width.
[[nodiscard]] inline std::optional<std::uint8_t>
hwEncodingOf(LirReg                 reg,
             TargetSchema const&    schema,
             std::string_view       mnemonic,
             std::uint8_t           maxBitWidth,
             DiagnosticReporter&    reporter) {
    using dss::report;
    if (!reg.valid() || reg.isPhysical == 0) {
        report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
               DiagnosticSeverity::Error,
               std::format("opcode '{}': register operand is not a "
                           "physical register (post-regalloc invariant "
                           "broken)",
                           mnemonic));
        return std::nullopt;
    }
    auto const* info = schema.registerInfo(static_cast<std::uint16_t>(reg.id));
    if (info == nullptr) {
        report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
               DiagnosticSeverity::Error,
               std::format("opcode '{}': register ordinal {} not in "
                           "target schema '{}' register table",
                           mnemonic, static_cast<unsigned>(reg.id),
                           schema.name()));
        return std::nullopt;
    }
    std::uint16_t const cap =
        (maxBitWidth >= 16)
            ? 0xFFFFu
            : static_cast<std::uint16_t>((1u << maxBitWidth) - 1u);
    if (info->hwEncoding > cap) {
        report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
               DiagnosticSeverity::Error,
               std::format("opcode '{}': register '{}' hwEncoding {} "
                           "exceeds {} bits — shape cannot encode",
                           mnemonic, info->name, info->hwEncoding,
                           static_cast<unsigned>(maxBitWidth)));
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(info->hwEncoding);
}

// Map an `OperandKindFilter` (variant-guard vocabulary) to its
// `LirOperandKind` partner. Closed-enum switch — every new filter
// must declare its LIR partner here, or the compiler warns.
[[nodiscard]] constexpr std::optional<LirOperandKind>
filterToLirKind(OperandKindFilter f) noexcept {
    switch (f) {
        case OperandKindFilter::Reg:       return LirOperandKind::Reg;
        case OperandKindFilter::ImmInt:    return LirOperandKind::ImmInt;
        case OperandKindFilter::SymbolRef: return LirOperandKind::SymbolRef;
        case OperandKindFilter::MemBase:   return LirOperandKind::MemBase;
        case OperandKindFilter::MemOffset: return LirOperandKind::MemOffset;
        case OperandKindFilter::BlockRef:  return LirOperandKind::BlockRef;
        // D-CSUBSET-BITFIELD-WIDE-UNIT: the wide-pool-literal filter
        // (JSON `"imm64"`) matches a `LiteralIndex` LIR operand.
        case OperandKindFilter::LiteralIndex:
                                           return LirOperandKind::LiteralIndex;
    }
    return std::nullopt;
}

// Per-position kind-equality check: variant's `operandKinds` filter
// list must match the LIR instruction's source-operand kinds (same
// length AND per-position kind translation).
[[nodiscard]] inline bool
operandsMatchGuard(std::span<LirOperand const>          instOps,
                   std::span<OperandKindFilter const>   guard) noexcept {
    if (instOps.size() != guard.size()) return false;
    for (std::size_t i = 0; i < guard.size(); ++i) {
        auto const wanted = filterToLirKind(guard[i]);
        if (!wanted.has_value()) return false;
        if (instOps[i].kind != *wanted) return false;
    }
    return true;
}

// Full variant-guard match: operand kinds AND the FC3 c2 width axis
// (D-CSUBSET-32BIT-ALU-FORMS). `instWidthBits` is the instruction's
// operation width (`lirInstWidthBits(lir.instFlags(inst))` — 64 for
// every pre-FC3 / hand-built instruction). A variant with
// `guardWidthBits == 0` (no `width` key in the JSON) matches ANY
// width — full back-compat for every pre-existing variant; a
// width-keyed variant matches only its declared width (the 32-bit
// no-REX.W x86 forms / arm64 W-forms vs their 64-bit siblings).
// Shared by BOTH walkers (x86_variable + fixed32) — the width axis
// is format-agnostic by construction.
[[nodiscard]] inline bool
variantMatchesInst(std::span<LirOperand const>  instOps,
                   std::uint8_t                 instWidthBits,
                   TargetEncodingVariant const& v) noexcept {
    if (v.guardWidthBits != 0 && v.guardWidthBits != instWidthBits) {
        return false;
    }
    return operandsMatchGuard(instOps, v.operandKinds);
}

// Read 4 little-endian bytes as a uint32. Caller guarantees the
// 4-byte window is in bounds.
[[nodiscard]] inline std::uint32_t
readU32LE(std::span<std::uint8_t const> bytes, std::size_t offset) noexcept {
    return  static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1]) <<  8)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

// One pending symbol-relative slot accumulated by an encoder walker
// during slot-wiring. When the encoder reaches the emit step, it
// calls `appendPendingReloc` to push a `Relocation` entry into the
// AssembledFunction.relocations list AT THE BYTE OFFSET that the
// linker will patch (the current `out.size()` — i.e. the position
// the placeholder bytes are ABOUT to be written into).
//
// D-AS4-3 (multi-instruction-macro / multi-relocation encoder): a
// single instruction may now accumulate MULTIPLE pending relocations
// (AArch64 `lea` emits two — `adr_prel_pg_hi21` on word 0 and
// `add_abs_lo12_nc` on word 1). `wordIndex` records which 32-bit
// word of a multi-word `fixed32` template the slot lives in, so the
// emit loop stamps each reloc at the START of its word (the byte
// offset the linker's `readInst32` reads). Single-word encoders
// (x86 Disp32, fixed32 Imm26) leave `wordIndex` at its default 0 —
// behaviour-identical to the prior single-trailing-slot model.
struct PendingRelocSlot {
    RelocationKind kind;
    SymbolId       target;
    std::uint8_t   wordIndex = 0;
};

// Push a `Relocation` entry at the current end of `out`. The reloc's
// `offset` points AT the bytes the linker will patch — capture
// happens BEFORE the placeholder bytes are appended. The CALLER
// controls the emit cursor: for a multi-word template the walker
// invokes this immediately before appending word `pending.wordIndex`,
// so `out.size()` is exactly that word's start (D-AS4-3 — the per-word
// byte offset is DERIVED from the emit cursor, never a separately
// computed `base + wordIndex*4`). `addend` is 0 in cycle scope
// (D-AS4-4 anchors wire-declared addend bias).
inline void
appendPendingReloc(std::vector<Relocation>&   relocs,
                   std::vector<std::uint8_t> const& out,
                   PendingRelocSlot const&    pending) {
    relocs.push_back(Relocation{
        static_cast<std::uint32_t>(out.size()),
        pending.target,
        pending.kind,
        /*addend=*/0,
    });
}

// D-CSUBSET-WHILE-LOOP-SUBSTRATE (step 13.5 cycle 1, 2026-06-03) +
// architect FOLD-NOW (post-fold): per-patch shape discriminator so
// the shared `asm.cpp` resolver dispatches via ISA arithmetic
// instead of hardcoding x86 rel32-after-disp semantics in the
// target-shared loop. ARM64 B / B.cc use different formulas
// (PC-of-instruction, no +4 bias; 19- or 26-bit displacement scaled
// by 4); adding the discriminator now (cost: 1 byte/patch) avoids
// a multi-file signature refactor when ARM64 control-flow lands.
enum class BlockRelPatchKind : std::uint8_t {
    // x86 rel32-after-disp: `disp = target - (patch + 4)`, written
    // as 4 LE bytes at `patch_offset`. Used by `E9 rel32` /
    // `0F 8x rel32` (jmp / jcc family).
    X86Rel32 = 0,
    // ARM64 placeholders (D-AS3-BLOCK-REL-IMM19/26 — close when
    // ARM64 control-flow lands). Mentioned to lock in the enum
    // shape; resolver MUST fail-loud on these until implemented.
    Arm64Imm19 = 1,  // B.cc — bits 23..5 of the 32-bit word, shift=2
    Arm64Imm26 = 2,  // B    — bits 25..0 of the 32-bit word, shift=2
};

// D-CSUBSET-WHILE-LOOP-SUBSTRATE (step 13.5 cycle 1, 2026-06-03):
// pending intra-function block-relative branch patch. Distinct from
// `PendingRelocSlot` because the target is an INTRA-FUNCTION basic
// block resolved at ASSEMBLE time (not link time) — no `SymbolId`,
// no `RelocationKind`, no entry in the function's relocation list.
// The asm.cpp per-function loop builds the block-offset table while
// emitting block-by-block, accumulates these patches as branches
// emit, then dispatches via `kind` to the right resolution formula
// (x86 / ARM64-19 / ARM64-26).
struct BlockRelPatch {
    std::uint32_t patchOffset;  // byte offset of the placeholder in out
    std::uint32_t targetBlock;  // LirBlockId.v of the branch target block
    BlockRelPatchKind kind = BlockRelPatchKind::X86Rel32;
};

} // namespace dss::walker_util
