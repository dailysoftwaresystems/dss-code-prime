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
#include <string>
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

// D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12: the unsigned MAGNITUDE of an
// instruction's immediate-bearing operand, used by the variant matcher's
// `immMin`/`immMax` magnitude key. Reads the FIRST operand whose guard
// filter is `ImmInt` or `MemOffset` (the value-bearing slot a magnitude-
// keyed variant routes on). Returns nullopt when the guard declares no
// such operand (the variant cannot be magnitude-keyed; validate() rejects
// that combination, so a runtime nullopt here only arises from a guard
// whose value operand is out of bounds — treated as "no match" by the
// caller). An ImmInt's magnitude is its value clamped at 0 for negatives
// (a negative immediate never matches a non-negative [immMin,immMax]
// range; the encoder's own range gate is the real bound — the matcher
// only ROUTES). A MemOffset's magnitude is likewise its non-negative
// displacement (frame offsets are non-negative; a negative disp does not
// match the shifted-imm12 range and falls through to the signed Imm9
// variant). Source/target-agnostic: reads the LIR operand pool, never the
// arch.
[[nodiscard]] inline std::optional<std::uint32_t>
variantImmMagnitude(std::span<LirOperand const>        instOps,
                    std::span<OperandKindFilter const> guard) noexcept {
    for (std::size_t i = 0; i < guard.size() && i < instOps.size(); ++i) {
        if (guard[i] == OperandKindFilter::ImmInt
            && instOps[i].kind == LirOperandKind::ImmInt) {
            std::int32_t const v = instOps[i].immInt32;
            if (v < 0) return std::nullopt;
            return static_cast<std::uint32_t>(v);
        }
        if (guard[i] == OperandKindFilter::MemOffset
            && instOps[i].kind == LirOperandKind::MemOffset) {
            std::int32_t const v = instOps[i].offset;
            if (v < 0) return std::nullopt;
            return static_cast<std::uint32_t>(v);
        }
    }
    return std::nullopt;
}

// D-AS4-ARM64-NEGATIVE-DISP-LEA-NATIVE-SUB: the ABSOLUTE-VALUE magnitude of
// a NEGATIVE value-bearing operand — the sign-mirror of `variantImmMagnitude`
// (which reports non-negative magnitudes). Reads the FIRST operand whose
// guard filter is `ImmInt` or `MemOffset`. Returns the operand's |value| ONLY
// when the value is STRICTLY NEGATIVE; nullopt when it is non-negative (a
// non-negative value never matches a `negValue` variant — the POSITIVE
// sibling serves it) or when no value-bearing operand exists. Computed as
// `-(int64)v` so `INT32_MIN` (whose positive |value| overflows int32)
// widens cleanly into the uint32 magnitude range. Source/target-agnostic:
// reads the LIR operand pool, never the arch. Symmetric partner of
// `variantImmMagnitude` — together they split the signed value line into
// the non-negative half (immMin/immMax on the default axis) and the negative
// half (immMin/immMax on the negValue axis).
// ★ THIS FUNCTION NEEDED NO WIDENING when the sign axis generalized from
// "a negative memory displacement" to "a negative value-bearing operand"
// (D-ASM-ARM64-NEGATIVE-IMMEDIATE-UNENCODABLE, arm64 MOVN): it has read BOTH
// `ImmInt` and `MemOffset` since it was written, exactly like its
// non-negative twin. Only the validate() coherence rule was
// memoffset-specific — and only the axis's NAME said so.
[[nodiscard]] inline std::optional<std::uint32_t>
variantNegMagnitude(std::span<LirOperand const>        instOps,
                    std::span<OperandKindFilter const> guard) noexcept {
    for (std::size_t i = 0; i < guard.size() && i < instOps.size(); ++i) {
        if (guard[i] == OperandKindFilter::ImmInt
            && instOps[i].kind == LirOperandKind::ImmInt) {
            std::int32_t const v = instOps[i].immInt32;
            if (v >= 0) return std::nullopt;
            return static_cast<std::uint32_t>(-static_cast<std::int64_t>(v));
        }
        if (guard[i] == OperandKindFilter::MemOffset
            && instOps[i].kind == LirOperandKind::MemOffset) {
            std::int32_t const v = instOps[i].offset;
            if (v >= 0) return std::nullopt;
            return static_cast<std::uint32_t>(-static_cast<std::int64_t>(v));
        }
    }
    return std::nullopt;
}

// Full variant-guard match: operand kinds AND the FC3 c2 width axis
// (D-CSUBSET-32BIT-ALU-FORMS) AND the FC12-deferral-2 immediate-magnitude
// axis (D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12). `instWidthBits` is the
// instruction's operation width (`lirInstWidthBits(lir.instFlags(inst))` —
// 64 for every pre-FC3 / hand-built instruction). A variant with
// `guardWidthBits == 0` (no `width` key in the JSON) matches ANY width —
// full back-compat for every pre-existing variant; a width-keyed variant
// matches only its declared width (the 32-bit no-REX.W x86 forms / arm64
// W-forms vs their 64-bit siblings). A variant with absent immMin/immMax
// matches ANY immediate magnitude (every pre-existing variant); a
// magnitude-keyed variant matches only when its value-bearing operand's
// magnitude is in [immMin, immMax]. The D-AS4-ARM64-NEGATIVE-DISP-LEA-NATIVE-SUB
// SIGN axis (`negValue`) selects WHICH magnitude the range gate reads:
// the non-negative half (default) or the |value| of a strictly-negative
// operand (negValue=true, e.g. arm64's `SUB Xd,Xn,#|disp|` negative-disp
// lea vs its positive `ADD` sibling). Shared by BOTH walkers (x86_variable +
// fixed32) — all three axes are format-agnostic by construction.
//
// D-ASM-X86-CMP-AGAINST-MEMORY-DIRECTION-IS-UNELECTABLE adds a FOURTH axis,
// `memoryIsDestination` (`lirInstMemoryIsDestination(flags)`): whether the
// instruction's memory reference occupies its destination position. A variant
// with no `memoryDestination` key matches EITHER — full back-compat, and the
// property `store` needs, since one dialect reaches it through the memory-
// destination path and another through the register-destination path. Both
// callers derive the argument from the SAME `flags` byte they derive
// `instWidthBits` from, which is what keeps the text lowering's election and
// the encoder's variant choice identical.
[[nodiscard]] inline bool
variantMatchesInst(std::span<LirOperand const>  instOps,
                   std::uint8_t                 instWidthBits,
                   bool                         memoryIsDestination,
                   TargetEncodingVariant const& v) noexcept {
    if (v.guardWidthBits != 0 && v.guardWidthBits != instWidthBits) {
        return false;
    }
    if (!operandsMatchGuard(instOps, v.operandKinds)) {
        return false;
    }
    // D-ASM-X86-CMP-AGAINST-MEMORY-DIRECTION-IS-UNELECTABLE: the
    // MEMORY-DIRECTION axis. Absent ⇒ the variant does not discriminate and
    // matches either direction (every pre-existing variant, `store`
    // included). Present ⇒ it must equal the instruction's own flag. This is
    // the ONLY axis that can separate `cmp mem, reg` (39 /r) from
    // `cmp reg, mem` (3B /r): their operand lists are byte-identical, so
    // both directions would otherwise elect the first-declared variant and
    // one spelling would silently encode the other's instruction.
    if (v.memoryDestination.has_value()
        && *v.memoryDestination != memoryIsDestination) {
        return false;
    }
    // D-AS4-ARM64-NEGATIVE-DISP-LEA-NATIVE-SUB: the SIGN axis selects which
    // magnitude the imm-range gate reads. `negValue=false` (the default,
    // every pre-existing variant) reads the NON-NEGATIVE magnitude — a
    // negative value-bearing operand reports nullopt and matches no bounded
    // variant, EXACTLY as before this axis existed. `negValue=true` reads
    // the |value| of a STRICTLY-NEGATIVE operand — a non-negative operand
    // reports nullopt so a negValue variant never shadows its positive
    // sibling. The two axes partition the signed value line; the [immMin,
    // immMax] bound then applies to whichever half's magnitude was read.
    auto const magnitude = v.negValue
        ? variantNegMagnitude(instOps, v.operandKinds)
        : variantImmMagnitude(instOps, v.operandKinds);
    // A negValue variant is ALWAYS sign-gated (it must reject a
    // non-negative operand even with no immMin/immMax bound), so consult
    // the magnitude whenever the sign axis is on OR an imm-range is declared.
    if (v.negValue || v.immMin.has_value() || v.immMax.has_value()) {
        if (!magnitude.has_value()) return false;  // wrong sign / no operand
        if (v.immMin.has_value() && *magnitude < *v.immMin) return false;
        if (v.immMax.has_value() && *magnitude > *v.immMax) return false;
    }
    return true;
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

// D-CSUBSET-COMPUTED-GOTO (`&&label` block-address materialization):
// a pending SYNTHETIC-SYMBOL ↔ BLOCK binding accumulated by an encoder
// walker. The block-address `lea` (BOTH targets) carries a SymbolRef
// (operand 0 — a synthetic per-block local symbol, the relocation
// source) PLUS a trailing BlockRef naming the target LIR block. The
// BlockRef contributes NO bytes (a block reference is never byte-
// encoded data — unlike a register / immediate / displacement); it
// exists so the assembler can bind `symbol` to `targetBlock`'s byte
// offset. The encoder reads the BlockRef from the operand list, pairs
// it with the SymbolRef it already captured for the relocation, and
// pushes this record. `asm.cpp`'s per-function loop resolves it AFTER
// `blockOffsets` is complete: `blockSymbols += { symbol,
// blockOffsets[targetBlock] }`. Distinct from `BlockRelPatch` — that
// patches a code SITE (a branch displacement) at a known byte offset;
// this binds a SYMBOL to a block, so it has NO `patchOffset` (the
// linker, not the assembler, writes the symbol's bytes, via the
// adjacent `lea` relocation against `symbol`). The linker assigns
// `symbol` its interior-block VA before relocation resolution.
struct BlockSymPatch {
    SymbolId      symbol;       // the synthetic per-block local symbol
    std::uint32_t targetBlock;  // LirBlockId.v of the address-taken block
};

// ─────────────────────────────────────────────────────────────────────
// IMMEDIATE NARROWING — D-ASM-X86-IMMEDIATE-WINDOW-REFUSES-WHAT-GAS-TRUNCATES
// ─────────────────────────────────────────────────────────────────────
//
// ★★★ THE QUESTION THIS ANSWERS IS NOT "IS THE VALUE TOO BIG". It is
// "IS THIS FIELD THE VALUE'S OWN HOME?", and the config already answers it
// without a single new key. A wire names a SLOT (field width) and its
// variant names `guard.width` (the operation width). When the two are EQUAL
// the field is where the operation's whole value lives, so an overflowing
// value is a NARROWING — the reference assembles it, keeping the low bits.
// When they DIFFER the field is a fixed narrow PARAMETER of a wider
// operation (an x86 shift count's `ib` under a 64-bit shift), the value was
// never going to live there, and the reference REFUSES it outright.
//
// ✔MEASURED, GNU as 2.42, 20 spellings assembled ONE AT A TIME so no
// diagnostic could be misattributed. The equality rule reproduces every one:
//   NARROWING (field == op width)      `movb $300,%al`   -> b0 2c, warns
//                                      `movw $0x10000,%cx` -> 66 b9 00 00, warns
//                                      `movl $0x100000000,%eax` -> b8 00.., warns
//   PARAMETER (field != op width)      `shl  $256,%rax`  -> ERROR, refused
//                                      `shl  $-1,%rax`   -> ERROR, refused
//                                      `add  $128,%rax`  -> gas declines to
//                                          narrow into the imm8 form and picks
//                                          the imm32 one instead
// ⇒ this is the reference's OWN boundary, read off DSS's own config, not an
// approximation of it and not an x86 special case. Any target declaring a
// slot whose field width equals its variant's operation width gets the
// narrowing treatment by the same three lines.
//
// ⚠ WHY THE DEFAULT ARM IS THE REFUSAL. `guardWidthBits == 0` means the
// variant is width-ABSENT and matches an instruction of any width, so the
// encoder CANNOT PROVE the field is the value's home. An unprovable case
// takes the loud arm: refuse. Widening by default would have turned every
// pre-width-axis variant in every target into a silent truncator at once.

// The N of a slot's appended immediate field, in bits, or 0 when the slot
// is not an appended-immediate field at all. Single-sourced here so the
// window and the emitted byte count can never disagree — they used to be
// two independent literals in `x86_variable.cpp` (`-32768..65535` beside a
// `std::uint16_t` push), which is one edit away from a window that admits a
// value the field cannot hold.
//
// ⚠ `Imm32` IS LISTED BUT NOTHING ROUTES THROUGH THE RESOLVER FOR IT YET,
// AND THAT IS A DECISION RATHER THAN AN OMISSION. `wireImm32` carries NO
// window at all today: its value arrives as a `std::int32_t`, so it cannot
// overflow the field it is heading for, and the sign-extended `imm32` of a
// 64-bit operation (`movq $-1, %rax`) is a legitimate NEGATIVE parameter —
// the one shape the parameter arm's unsigned window would refuse. Giving it
// a window here would turn a correct instruction into an error. It stays
// listed because the width is a true fact about the slot and the next
// caller should read it, not re-derive it.
[[nodiscard]] constexpr std::uint8_t
immediateFieldBits(EncodingSlotKind s) noexcept {
    switch (s) {
        case EncodingSlotKind::Imm8:       return 8;
        case EncodingSlotKind::Imm16Bytes: return 16;
        case EncodingSlotKind::Imm32:      return 32;
        default:                           return 0;
    }
}

// The config-declared narrowing predicate. See the block comment above.
[[nodiscard]] constexpr bool
fieldCarriesWholeOperationValue(std::uint8_t fieldBits,
                                std::uint8_t guardWidthBits) noexcept {
    return fieldBits != 0 && fieldBits == guardWidthBits;
}

// The window, and THE TWO ARMS DO NOT SHARE ONE.
//
// NARROWING field (the field is the operation's own value): the union of the
// signed and the unsigned reading of the same N bits, [-2^(N-1), 2^N - 1].
// AT&T writes both `$-1` and `$65535` for the same halfword, so a window
// admitting only one of them would refuse input the reference takes.
// ★ THIS IS THE SAME NUMBER THE ENCODER USED TO REFUSE OUTSIDE OF — it was
// the ACCEPTANCE threshold and is now the SILENCE threshold. Nothing was
// widened; what the threshold GATES changed. Keeping it (rather than
// adopting gas's wider silent band, ✔MEASURED as |v| <= 2^N - 1) is what
// makes DSS loud on the case gas does not mention: `$-32769` into a 16-bit
// field, where 0x8000 of magnitude disappears with nothing on gas's stderr.
//
// PARAMETER field (a fixed narrow field of a WIDER operation): unsigned
// only, [0, 2^N - 1]. ★★ THIS IS NOT A NARROWER WINDOW FOR CAUTION'S SAKE,
// IT IS A DIFFERENT MACHINE FACT. The operation's own width carries the
// sign; a parameter field left over beside it is a MAGNITUDE, and a negative
// magnitude is meaningless rather than merely large. ✔MEASURED (GNU as
// 2.42): `shl $-1, %rax` and `shl $256, %rax` are BOTH `Error: operand type
// mismatch` — the reference refuses the negative one exactly as hard as the
// oversized one, while it happily assembles `movb $-1, %al` to `b0 ff` where
// the same 8 bits ARE the operation's value. Same field width, opposite
// answers, and only the config's `guard.width` tells them apart.
// ⚠ The `1 <= fieldBits < 64` precondition is ENFORCED rather than assumed:
// `1 << 64` and `1 << (0 - 1)` are both undefined behaviour, and this is a
// public helper in a shared header, so the next walker to reach for it will
// not have `resolveImmediateForField`'s guard in front of it. An
// out-of-contract width admits NOTHING, which routes the caller to its loud
// arm instead of to a silently wrong window.
[[nodiscard]] constexpr bool
immediateFitsFieldWindow(std::int64_t v, std::uint8_t fieldBits,
                         bool narrowingField) noexcept {
    if (fieldBits == 0 || fieldBits >= 64) return false;
    std::int64_t const lo =
        narrowingField ? -(std::int64_t{1} << (fieldBits - 1)) : 0;
    std::int64_t const hi = (std::int64_t{1} << fieldBits) - 1;
    return v >= lo && v <= hi;
}

// Resolve an immediate against its declared field. Returns the bit pattern
// to emit (already masked to `fieldBits`), or nullopt when the value is
// REFUSED — in which case a diagnostic has been reported.
//
// Three outcomes, and the middle one is the whole point of the row:
//   * inside the window            -> emit, silent
//   * outside, narrowing field     -> emit the LOW N BITS (the reference's
//                                     own bytes) + `A_ImmediateNarrowed-
//                                     ToOperandField` at Warning
//   * outside, parameter field     -> nullopt + `A_ImmediateOperandOutOf-
//                                     Range` at Error, exactly as before
[[nodiscard]] inline std::optional<std::uint64_t>
resolveImmediateForField(std::int64_t        v,
                         EncodingSlotKind    slot,
                         std::uint8_t        guardWidthBits,
                         std::string_view    mnemonic,
                         DiagnosticReporter& reporter) {
    using dss::report;
    std::uint8_t const fieldBits = immediateFieldBits(slot);
    if (fieldBits == 0) {
        report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
               DiagnosticSeverity::Error,
               std::format("opcode '{}': slot '{}' is not an appended "
                           "immediate field — no immediate window is "
                           "declared for it",
                           mnemonic, encodingSlotKindName(slot)));
        return std::nullopt;
    }
    bool const narrowing =
        fieldCarriesWholeOperationValue(fieldBits, guardWidthBits);
    std::uint64_t const mask = (fieldBits >= 64)
        ? ~std::uint64_t{0}
        : ((std::uint64_t{1} << fieldBits) - 1);
    std::uint64_t const bits = static_cast<std::uint64_t>(v) & mask;
    if (immediateFitsFieldWindow(v, fieldBits, narrowing)) return bits;

    if (!narrowing) {
        report(reporter, DiagnosticCode::A_ImmediateOperandOutOfRange,
               DiagnosticSeverity::Error,
               std::format("opcode '{}': immediate {} does not fit the "
                           "{}-bit '{}' field, and that field is a fixed "
                           "parameter of a {} operation rather than the "
                           "operation's own value — narrowing it would "
                           "change the instruction's meaning, so it is "
                           "refused",
                           mnemonic, v, fieldBits,
                           encodingSlotKindName(slot),
                           guardWidthBits == 0
                               ? std::string{"width-unconstrained"}
                               : std::format("{}-bit", guardWidthBits)));
        return std::nullopt;
    }

    report(reporter, DiagnosticCode::A_ImmediateNarrowedToOperandField,
           DiagnosticSeverity::Warning,
           std::format("opcode '{}': immediate {} narrowed to {} — it does "
                       "not fit the {}-bit operand field, so the {} "
                       "low-order bits are emitted and the instruction "
                       "carries a different constant than the one written. "
                       "GNU as narrows here too (silently, on the negative "
                       "side); write the value that fits, or use the wider "
                       "form of this instruction",
                       mnemonic, v, static_cast<std::int64_t>(bits),
                       fieldBits, fieldBits));
    return bits;
}

} // namespace dss::walker_util
