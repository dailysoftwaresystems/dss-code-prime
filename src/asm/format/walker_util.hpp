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
//     limit both fit the same shape) + the REGISTER-CLASS gate
//     (D-OPT-LIR-ARG-REGISTER-CLASS-MISMATCH-FAILLOUD): every register
//     either belongs to the bank its destination field declares, or the
//     instruction is refused. Both walkers resolve every register they
//     emit through this one function, which is what makes one check
//     here cover every register field of every shape.
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
#include "asm/format/byte_emit.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"
#include "lir/lir_reg.hpp"

#include <array>
#include <cstddef>
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
// non-physical register, unknown ordinal, hwEncoding exceeding the
// shape's bit width, or — D-OPT-LIR-ARG-REGISTER-CLASS-MISMATCH-FAILLOUD —
// a register whose CLASS is not the one the destination field draws from.
//
// ★★★ THE CLASS GATE IS THE ONLY THING BETWEEN A WRONG-BANK OPERAND AND A
// SILENT MISCOMPILE, and this is the choke point: both walkers resolve every
// register they will ever emit through this one function, so a check here
// covers the result slot, every source wire, and every future shape's
// register field with no per-walker repetition.
//
// The bug it is named for: `cvttss2si %xmm15`. A GPR ordinal (`r15` = 15)
// reached an XMM field, 15 is a legal value for that field, and the encoder
// wrote it. The bytes decoded cleanly, the disassembler round-trip agreed with
// itself, and the program read whatever `%xmm15` held. There is no later stage
// that could have noticed.
//
// ★★ TWO COMPARISONS, AND THEY ARE ORTHOGONAL — WHICH TOOK A MUTANT TO GET
// RIGHT. A `LirReg` carries THREE facts about its class, not two: the operand's
// own class TAG, the class the target's `registers[]` row for that ORDINAL
// declares, and the bank the destination FIELD draws from. The first draft
// compared tag-vs-field and table-vs-field; ✔MEASURED by disabling the first,
// the suite stayed GREEN, because a tag that agrees with its ordinal makes
// table-vs-field subsume tag-vs-field entirely. A comparison no test can
// distinguish from its own absence asserts nothing.
//
// The orthogonal pair, each with a case ONLY it catches:
//   (1) TAG vs TABLE — the operand lies about ITSELF: its tag says `fpr` while
//       its ordinal names a GPR row. Independent of the field: an `xmm1`
//       ordinal tagged `gpr` reaching an `fpr` field passes (2) and fails here.
//       This is the ordinal-collision family — the register FILES number from
//       zero independently, so the class tag is the only thing that says which
//       file an ordinal came from, and a producer that picks from the wrong
//       pool gets a plausible number.
//   (2) TABLE vs FIELD — the wrong-BANK operand, the anchor's own defect: an
//       `r15` honestly tagged `gpr` reaching an XMM field passes (1) and fails
//       here. The comparison is against the TABLE, not the tag, because the
//       table is the authority on what a register IS; (1) has already proved
//       the tag agrees with it.
//
// `expected` is `optional` only because a caller could reach here with an
// unresolved bank; `validate()` proves every register-bearing field resolves,
// so a nullopt here means a schema bypassed validation — which is reported,
// never assumed benign. `fieldName` names the destination field in the
// message; the caller passes its `EncodingSlotKind` spelling.
[[nodiscard]] inline std::optional<std::uint8_t>
hwEncodingOf(LirReg                        reg,
             TargetSchema const&           schema,
             std::string_view              mnemonic,
             std::uint8_t                  maxBitWidth,
             std::optional<TargetRegClass> expected,
             std::string_view              fieldName,
             DiagnosticReporter&           reporter) {
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
    if (!expected.has_value()) {
        report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
               DiagnosticSeverity::Error,
               std::format("opcode '{}': field '{}' takes a register but the "
                           "target schema declares no register bank for it — "
                           "`validate()` refuses such a document, so this "
                           "schema reached the encoder unvalidated "
                           "(D-OPT-LIR-ARG-REGISTER-CLASS-MISMATCH-FAILLOUD)",
                           mnemonic, fieldName));
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
    // (1) TAG vs TABLE — does the operand agree with itself?
    auto const taggedClass = static_cast<TargetRegClass>(reg.regClass());
    if (taggedClass != info->regClass) {
        report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
               DiagnosticSeverity::Error,
               std::format("opcode '{}': the operand wired to field '{}' is "
                           "tagged '{}'-class, but ordinal {} is '{}' — a "
                           "'{}'-class register in the target's `registers[]` "
                           "table. The operand's class tag and its ordinal "
                           "disagree, so one of them names a register the "
                           "producer did not mean; the register files number "
                           "from zero independently, so the ordinal alone "
                           "cannot say which file it came from "
                           "(D-OPT-LIR-ARG-REGISTER-CLASS-MISMATCH-FAILLOUD)",
                           mnemonic, fieldName,
                           targetRegClassName(taggedClass),
                           static_cast<unsigned>(reg.id), info->name,
                           targetRegClassName(info->regClass)));
        return std::nullopt;
    }
    // (2) TABLE vs FIELD — is this register in the bank the field draws from?
    if (info->regClass != *expected) {
        report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
               DiagnosticSeverity::Error,
               std::format("opcode '{}': field '{}' encodes a register from "
                           "the '{}' bank, but the LIR operand is a '{}'-class "
                           "register (ordinal {}, '{}'). The field would accept "
                           "the ordinal and name a DIFFERENT physical register "
                           "— the wrong-register-class miscompile, refused "
                           "(D-OPT-LIR-ARG-REGISTER-CLASS-MISMATCH-FAILLOUD)",
                           mnemonic, fieldName,
                           targetRegClassName(*expected),
                           targetRegClassName(info->regClass),
                           static_cast<unsigned>(reg.id), info->name));
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
//
// ★★ A `memoffset` POSITION TAKES A SYMBOLIC DISPLACEMENT TOO
// (`LirOperandKind::MemSymbolOffset`, `msg+4(%rip)` —
// D-ASM-RIP-RELATIVE-SPELLING-NEEDS-AN-IP-REGISTER). The filter names the
// displacement POSITION of a memory operand, and a displacement is a number or
// a link-time address; which one reaches the field is the encoder's business
// (a literal, or a relocation). Accepting both here is what lets every memory
// form a target declares take a symbolic displacement with no variant of its
// own — and the value axes still refuse it wherever a variant bounds its
// displacement, because a link-time value has no magnitude to check.
[[nodiscard]] inline bool
operandsMatchGuard(std::span<LirOperand const>          instOps,
                   std::span<OperandKindFilter const>   guard) noexcept {
    if (instOps.size() != guard.size()) return false;
    for (std::size_t i = 0; i < guard.size(); ++i) {
        auto const wanted = filterToLirKind(guard[i]);
        if (!wanted.has_value()) return false;
        if (instOps[i].kind == *wanted) continue;
        if (guard[i] == OperandKindFilter::MemOffset
            && instOps[i].kind == LirOperandKind::MemSymbolOffset) {
            continue;
        }
        return false;
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
    // The sign split is `variantValueMagnitude`'s (target_schema.hpp), shared
    // with the LIR frame chokepoint so the two tiers read one value line.
    for (std::size_t i = 0; i < guard.size() && i < instOps.size(); ++i) {
        if (guard[i] == OperandKindFilter::ImmInt
            && instOps[i].kind == LirOperandKind::ImmInt) {
            return variantValueMagnitude(/*negValue=*/false, instOps[i].immInt32);
        }
        if (guard[i] == OperandKindFilter::MemOffset
            && instOps[i].kind == LirOperandKind::MemOffset) {
            return variantValueMagnitude(/*negValue=*/false, instOps[i].offset);
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
            return variantValueMagnitude(/*negValue=*/true, instOps[i].immInt32);
        }
        if (guard[i] == OperandKindFilter::MemOffset
            && instOps[i].kind == LirOperandKind::MemOffset) {
            return variantValueMagnitude(/*negValue=*/true, instOps[i].offset);
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
    // The four value axes are ONE predicate in `target_schema.hpp`
    // (`variantValueAxesAdmit`), because the LIR frame chokepoint asks the same
    // question about an access it has not emitted yet and must get this
    // matcher's answer. [[D-ASM-ARM64-LDR-TO-LDUR-CONVENIENCE-ALIAS-REFUSED]]'s
    // divisibility half lives there too: without it a bounded variant matches an
    // offset its own encoder then refuses — and because election commits to an
    // opcode, the dialect's other candidate (the unscaled form that could have
    // carried it) is never tried.
    return variantValueAxesAdmit(v, magnitude);
}

// ★★★ WHY NO VARIANT MATCHED, WHEN THE ANSWER IS "THE VALUE".
// [[D-ASM-ARM64-LDR-TO-LDUR-CONVENIENCE-ALIAS-REFUSED]], 2026-09-03.
//
// ★★ THE PROBLEM THIS SOLVES, AND IT IS A DIAGNOSTIC ONE RATHER THAN A
// CORRECTNESS ONE. `variantMatchesInst` weighs four kinds of axis — operand
// SHAPE, WIDTH, memory DIRECTION and the operand's VALUE — and when every
// variant is eliminated the walkers report one sentence: *no encoding variant
// matches this instruction's operand kinds at width N*. That sentence is TRUE
// only when the shape or the width was the cause. When the shape and the width
// fit and the VALUE was rejected, it names the wrong thing and drops the one
// datum the reader needs — which value, and against which bound.
//
// ⚠ IT MATTERED LITTLE UNTIL THE VALUE AXES BECAME PRECISE ENOUGH TO ELIMINATE
// A VARIANT ITS OWN SLOT WOULD HAVE EXPLAINED. Before `immMultipleOf`, a scaled
// memory variant matched almost any offset and the refusal came from the SLOT
// handler, which names the offset, the access size and the reach. With the
// guard mirroring the slot, the variant is eliminated one step EARLIER and that
// message becomes unreachable. So the axis has to carry its own explanation.
//
// Returns the FIRST variant that matched every non-value axis, or nullptr when
// the shape/width/direction really were the cause (in which case the walkers'
// existing sentence is the right one). Source/target/format-agnostic: it reads
// the target's own guard and the LIR operand pool.
[[nodiscard]] inline TargetEncodingVariant const*
variantRejectedOnValueOnly(TargetOpcodeInfo const&      info,
                           std::span<LirOperand const>  instOps,
                           std::uint8_t                 instWidthBits,
                           bool memoryIsDestination) noexcept {
    for (auto const& v : info.encoding.variants) {
        if (!v.negValue && !v.immMin.has_value() && !v.immMax.has_value()
            && !v.immMultipleOf.has_value()) {
            continue;  // declares no value axis; it lost on something else
        }
        if (v.guardWidthBits != 0 && v.guardWidthBits != instWidthBits) continue;
        if (!operandsMatchGuard(instOps, v.operandKinds)) continue;
        if (v.memoryDestination.has_value()
            && *v.memoryDestination != memoryIsDestination) {
            continue;
        }
        return &v;
    }
    return nullptr;
}

// The value the axes above were read against, as a SIGNED number — the one a
// message should quote, because it is what the programmer or the lowering
// wrote. `variantImmMagnitude`/`variantNegMagnitude` report unsigned
// magnitudes per sign half, which is right for RANGE arithmetic and wrong for
// a sentence: quoting 8 for an offset of -8 is a small lie in exactly the
// situation a reader is already confused.
[[nodiscard]] inline std::optional<std::int32_t>
variantValueOperand(std::span<LirOperand const>        instOps,
                    std::span<OperandKindFilter const> guard) noexcept {
    for (std::size_t i = 0; i < guard.size() && i < instOps.size(); ++i) {
        if (guard[i] == OperandKindFilter::ImmInt
            && instOps[i].kind == LirOperandKind::ImmInt) {
            return instOps[i].immInt32;
        }
        if (guard[i] == OperandKindFilter::MemOffset
            && instOps[i].kind == LirOperandKind::MemOffset) {
            return instOps[i].offset;
        }
    }
    return std::nullopt;
}

// The sentence a value-axis rejection deserves: which value, and which of the
// variant's own declared bounds it fell outside. Built entirely from the
// target's declared numbers, so it stays true for any target that keys on them.
[[nodiscard]] inline std::string
describeValueAxisRejection(TargetEncodingVariant const& v,
                           std::int32_t                 value) {
    if (v.immMultipleOf.has_value()
        && value >= 0
        && (static_cast<std::uint32_t>(value) % *v.immMultipleOf) != 0) {
        return std::format(
            "{} is not a multiple of the {}-byte access size this encoding "
            "scales by, so it has no representation in the scaled field",
            value, *v.immMultipleOf);
    }
    if (value < 0 && !v.negValue) {
        return std::format(
            "{} is negative and this encoding's field is unsigned", value);
    }
    // The mirror of the line above, and it is stated rather than left to the
    // fallthrough: a `negValue` variant serves the NEGATIVE half of the value
    // line only, so a non-negative operand reaching it has the wrong SIGN, not
    // a value out of range. Saying "outside the declared range" there would
    // send a reader hunting for a bound that is not the problem.
    if (value >= 0 && v.negValue) {
        return std::format(
            "{} is non-negative and this encoding carries only negative values",
            value);
    }
    auto const magnitude = static_cast<std::uint32_t>(
        value < 0 ? -static_cast<std::int64_t>(value) : value);
    if (v.immMin.has_value() && magnitude < *v.immMin) {
        return std::format("{} is below this encoding's reach (from {})",
                           value, *v.immMin);
    }
    if (v.immMax.has_value() && magnitude > *v.immMax) {
        return std::format("{} is past this encoding's reach (up to {})",
                           value, *v.immMax);
    }
    return std::format("{} is outside this encoding's declared value range",
                       value);
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
    // ARM64 (D-AS3-BLOCK-REL-IMM19-26 — RESOLVED since 2026-06-08; the
    // "fail-loud placeholder" this comment used to describe is long gone,
    // and `blockRelFieldGeometry` below now carries each arm's numbers).
    Arm64Imm19 = 1,  // B.cc — bits 23..5 of the 32-bit word, shift=2
    Arm64Imm26 = 2,  // B    — bits 25..0 of the 32-bit word, shift=2
    // AArch64 `TBZ`/`TBNZ` — bits 18..5 of the 32-bit word, shift=2, ±32 KiB.
    // The ISA's NARROWEST PC-relative branch field (ARM ARM C6.2.x, the
    // test-and-branch family), declared here because the reach of a field is
    // a fact about the ISA and not about which opcode DSS happens to wire it
    // to today. Nothing in either shipped target declares a test-and-branch
    // opcode yet — see the note on `Arm64Imm14`'s row in
    // `blockRelFieldGeometry` for why the vocabulary carries it regardless.
    Arm64Imm14 = 3,
};

// ─────────────────────────────────────────────────────────────────────
// D-CSUBSET-LONG-BRANCH — THE BRANCH ISLAND, AND WHAT IT IS MADE OF
// ─────────────────────────────────────────────────────────────────────
//
// ★★★ THE WIDEST FIELD DOES NOT NEED A WIDER FIELD — IT NEEDS A NEARER
// TARGET. A branch whose field is already the ISA's widest block-relative one
// has no wider field to escape into, which is where the escape election
// correctly stops. It does NOT follow that the branch then needs an absolute
// address: `assemble()` knows every byte offset in the function, so the same
// field aimed at a NEARER point of the same function is still just a
// subtraction. The nearer point is an ISLAND holding the same branch again:
//
//     B island                  island: B far
//
// the same field twice, chained as far as the distance demands. No wider
// field, no absolute address, no relocation, no synthetic symbol, no symbol
// allocator, and — the part that matters most on AArch64 — NO SCRATCH
// REGISTER. Every veneer shape that materializes an absolute address
// (`ADRP`+`ADD`+`BR`, a literal-pool `LDR`+`BR`) needs a GPR, and the
// assembler runs AFTER register allocation with no liveness to consult, so
// such a veneer is a silent wrong answer rather than a refusal. An island is
// a branch, and a branch reads no register.
//
// ★★★ AND ITS BYTES ARE QUOTED, NOT SYNTHESIZED. Writing an unconditional
// branch's opcode into the shared resolver would hardcode one ISA into it —
// the exact break `BlockRelPatchKind` exists to prevent. The body below is
// COPIED from a word (or byte sequence) the OPCODE ITSELF declares: AArch64
// `jcc`'s own trailing `B <ifFalse>` word, x86 `jcc`'s own
// `prefixOpcodeBytes: [0xE9]`, either target's one-word/one-byte `jmp`
// template. A target that declares no self-contained unconditional branch on
// the overflowing opcode simply gets no island, and keeps its loud refusal.
inline constexpr std::size_t kMaxBranchIslandBytes = 8;

struct BranchIslandBody {
    // The quoted bytes, with the block-relative field left ZERO.
    std::array<std::uint8_t, kMaxBranchIslandBytes> bytes{};
    // How many of them are real. ZERO means "this opcode declares no island
    // body", which is the conservative default: no island is minted and the
    // out-of-range refusal stands exactly as it did before islands existed.
    std::uint8_t      byteCount   = 0;
    // Where the block-relative field starts INSIDE `bytes`.
    std::uint8_t      fieldOffset = 0;
    // Which field it is — so the island's own displacement is computed from
    // the same geometry row as every other block-relative write.
    BlockRelPatchKind kind        = BlockRelPatchKind::X86Rel32;

    [[nodiscard]] constexpr bool declared() const noexcept {
        return byteCount != 0;
    }
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
    // D-CSUBSET-LONG-BRANCH: does an ESCAPE exist for this patch when its
    // displacement leaves the field's reach? Set by the WALKER, because only
    // the walker can see the opcode's declared encoding variants and decide
    // whether a strictly-wider block-relative slot is available to escape
    // into. FALSE is the conservative default: a patch that declares no
    // escape keeps the loud `A_ImmediateOperandOutOfRange` refusal it has
    // always had. The resolver never invents an escape — it only honours one
    // the config proved exists.
    bool relaxable = false;
    // D-CSUBSET-LONG-BRANCH: does this patch's OPCODE declare, on any of its
    // encoding variants, a self-contained word carrying a block-relative field
    // that out-reaches THIS patch's own? Distinct from `relaxable`, which is
    // additionally gated on this being the narrowest wire of the instruction —
    // the escape rescues one wire, but the question of whether a wider field
    // was declared at all is asked of every patch, and only the walker can
    // answer it because only the walker sees the variants.
    //
    // It exists for ONE purpose: the out-of-range refusal must prescribe a
    // remedy that can actually work. TRUE means a wider field IS declared on
    // this opcode and this wire simply is not the one the escape rescues — a
    // pure CONFIG gap. FALSE means the opcode declares no such word, which is
    // where the two halves of D-CSUBSET-LONG-BRANCH part: either the TARGET
    // has a wider field nobody declared here (still config), or this already
    // IS the target's widest block-relative field and no encoding row can ever
    // rescue it, because its escape is a different ADDRESSING MODE (an
    // indirect branch through a materialized absolute address) needing a
    // per-block symbol the assembler cannot mint. The refusal says both, and
    // says WHICH is ruled out, rather than guessing at the one it cannot see.
    //
    // FALSE is the conservative default, so a walker that declares no escape
    // vocabulary at all (`x86_variable`, whose widest block-relative slot is
    // also its only one) is correct by construction rather than by remembering.
    bool widerFieldDeclared = false;
    // D-CSUBSET-LONG-BRANCH: the LIR instruction this patch came from —
    // the STABLE IDENTITY across the relaxation re-encodes. `asm.cpp` stamps
    // it centrally over the patches a single `encodeInst` appended, so no
    // walker has to remember to. The promoted-instruction set is keyed on
    // this value, which is why it must survive a re-encode unchanged
    // (byte offsets do not — that is the whole reason the set is keyed on
    // the instruction rather than on the patch site).
    std::uint32_t instV = 0;
    // D-CSUBSET-LONG-BRANCH: the self-contained unconditional branch this
    // patch's OPCODE declares, or an undeclared body when it declares none.
    // Stamped by the WALKER for the same reason `relaxable` is: only the
    // walker sees the opcode's encoding variants, and the island's bytes are
    // QUOTED from them. The resolver materializes islands out of this and
    // never invents one.
    //
    // ⓘ COST: `kMaxBranchIslandBytes + 3` bytes on every block-relative patch,
    // and a patch exists once per BRANCH rather than once per instruction —
    // the 262146-instruction fixture in `tests/asm/test_asm_long_branch.cpp`
    // carries five of them.
    BranchIslandBody island{};
};

// ─────────────────────────────────────────────────────────────────────
// D-CSUBSET-LONG-BRANCH — THE REACH OF A BLOCK-RELATIVE FIELD, AS DATA
// ─────────────────────────────────────────────────────────────────────
//
// ★★★ THE REACH OF A FIELD IS A PROPERTY OF THE FIELD, NOT OF A CPU NAME.
// Before this table the `asm.cpp` resolver carried the lsb / width / scale /
// PC-bias of each patch kind as literals inside its own `switch` arms — two
// arms that happened to agree on ARM64's `>>2, no bias` and one that spelled
// x86's `+4, whole word`. Adding an escape path to that shape would have
// meant a THIRD copy of the same four numbers. They live here once, keyed by
// the kind the encoder already selects from `wire.slotKind` (config-driven),
// and every consumer — range check, patch write, escape election — reads the
// same row.
//
// `wholeField` distinguishes the two write disciplines the resolver needs:
//   * true  — the displacement IS the four bytes at `patchOffset` (x86
//             rel32): write all 32 bits, no read-modify-write.
//   * false — the displacement is a bit-window inside a 32-bit LE word
//             (AArch64 Imm19 / Imm26): READ the word, replace only
//             [lsb, lsb+width), write it back. A whole-word store here
//             would clobber the opcode and the cond nibble.
struct BlockRelFieldGeometry {
    std::uint8_t lsb;         // first bit of the field inside its 32-bit word
    std::uint8_t width;       // field width in bits
    std::uint8_t scaleLog2;   // displacement = delta >> scaleLog2
    std::int8_t  pcBias;      // bytes added to patchOffset before subtracting
    bool         wholeField;  // the field is the whole 4-byte little-endian word
};

[[nodiscard]] constexpr BlockRelFieldGeometry
blockRelFieldGeometry(BlockRelPatchKind kind) noexcept {
    switch (kind) {
        // x86 rel32-after-disp: `disp = target - (patch + 4)`, unscaled,
        // written as the whole 4-byte LE field at `patchOffset`.
        case BlockRelPatchKind::X86Rel32:
            return BlockRelFieldGeometry{ 0, 32, 0, 4, true };
        // AArch64 `B.cond`: bits 5..23 of the word, scaled by 4, PC-relative
        // to the instruction ITSELF (no bias). ±1 MiB.
        case BlockRelPatchKind::Arm64Imm19:
            return BlockRelFieldGeometry{ 5, 19, 2, 0, false };
        // AArch64 `B`: bits 0..25, scaled by 4, no bias. ±128 MiB.
        case BlockRelPatchKind::Arm64Imm26:
            return BlockRelFieldGeometry{ 0, 26, 2, 0, false };
        // AArch64 `TBZ`/`TBNZ`: bits 5..18, scaled by 4, no bias. ±32 KiB.
        //
        // ⓘ WHY A ROW WITH NO SHIPPED OPCODE. This is the ISA's narrowest
        // block-relative field, and its reach is what makes the BRANCH-ISLAND
        // machinery reachable by a test at all: the widest fields' edges
        // (±128 MiB, ±2 GiB) cost a 134 MB and a 2 GiB function body to reach,
        // which is a property of those fields and not of a lane's budget. A
        // ±32 KiB field reaches the SAME code path with an 8192-word body. The
        // numbers are the ARM ARM's, not an invention — declaring a field DSS
        // does not yet wire is the same posture `immediateFieldBits` already
        // takes for `Imm32` ("the width is a true fact about the slot").
        case BlockRelPatchKind::Arm64Imm14:
            return BlockRelFieldGeometry{ 5, 14, 2, 0, false };
    }
    // Enum-drift backstop: a new kind added without a row here returns a
    // ZERO-WIDTH field, whose signed range is empty, so the resolver refuses
    // every displacement loudly rather than silently writing nothing.
    return BlockRelFieldGeometry{ 0, 0, 0, 0, false };
}

// The inclusive signed displacement range (in SCALED units — the value that
// actually lands in the field) a geometry admits. A zero-width field yields
// an empty range `[0, -1]`, which no displacement satisfies.
[[nodiscard]] constexpr std::int64_t
blockRelFieldMin(BlockRelFieldGeometry g) noexcept {
    return g.width == 0 ? 0 : -(std::int64_t{1} << (g.width - 1));
}
[[nodiscard]] constexpr std::int64_t
blockRelFieldMax(BlockRelFieldGeometry g) noexcept {
    return g.width == 0 ? -1 : (std::int64_t{1} << (g.width - 1)) - 1;
}

// How far, in BYTES, a kind can branch in the positive direction. Used to
// ORDER two block-relative slots when the walker elects an escape: an escape
// is only an escape if it reaches STRICTLY further than the field it rescues.
// Comparing byte reach rather than bit width is what makes the ordering
// correct across kinds with different scales (a 19-bit field scaled by 4
// reaches further than an unscaled 19-bit one would).
[[nodiscard]] constexpr std::int64_t
blockRelByteReach(BlockRelPatchKind kind) noexcept {
    auto const g = blockRelFieldGeometry(kind);
    return blockRelFieldMax(g) << g.scaleLog2;
}

// D-CSUBSET-LONG-BRANCH: how far along the way to its target the resolver aims
// when it asks for an island — the CHAIN STRIDE, in bytes.
//
// ★★★ IT IS HALF THE REACH, AND THE HALF IS THE WHOLE TERMINATION ARGUMENT.
// Placing an island at the full reach leaves it EXACTLY at the edge, and every
// later pass of the relaxation loop can only GROW the function — one more
// escape word, one more island cluster — which pushes that island out of reach
// again and asks for another. At half the reach the placed island keeps a
// whole half-reach of slack, so the growth relaxation can add (bounded by the
// island bound stated in `asm.cpp`) cannot undo a placement. Each hop then
// advances at least `stride` bytes towards the target, which is what bounds
// the chain length at `ceil(distance / stride)`.
//
// The floor of 1 exists so a hypothetical field whose reach is smaller than an
// instruction still makes progress rather than asking for an island at the
// site it is already standing on; such a field cannot reach any island at all,
// so it falls out to the loud refusal, which is the correct answer for it.
[[nodiscard]] constexpr std::int64_t
blockRelIslandStride(BlockRelPatchKind kind) noexcept {
    std::int64_t const half = blockRelByteReach(kind) / 2;
    return half > 0 ? half : 1;
}

// D-CSUBSET-LONG-BRANCH: how many kinds the enum declares. Used only to WALK
// the ordinals, because the enum alone cannot be iterated and a kind added
// without a `blockRelFieldGeometry` row would silently take the zero-width
// backstop — an outcome that is safe (it refuses) but wants to be LOUD at
// build time rather than discovered in a binary.
// `AsmLongBranch.EveryBlockRelPatchKindHasAGeometryRow` is that ratchet.
inline constexpr std::size_t kBlockRelPatchKindCount = 4;

// ─────────────────────────────────────────────────────────────────────
// D-CSUBSET-LONG-BRANCH — THE FIELD WRITE, ONCE, FOR EVERY WRITER
// ─────────────────────────────────────────────────────────────────────
//
// ★★★ THE SECOND WRITER IS WHY THIS IS A FUNCTION. Until branch islands the
// patch-resolve loop in `asm.cpp` was the only place a block-relative field
// was ever written, so its two write disciplines (whole 4-byte field vs.
// read-modify-write of a bit-window) could live inline. An island cluster's
// JUMP-OVER carries a displacement that is known at EMIT time — the cluster's
// own size — so it is written by the emitter, not by the resolver. Two writers
// of one field shape is the N-transforms-on-one-value shape this project keeps
// naming, so there is one function and both call it.
//
// `wholeField` picks the discipline:
//   * true  — the displacement IS the four bytes at `patchOffset` (x86 rel32).
//   * false — the displacement is [lsb, lsb+width) inside a 32-bit LE word;
//             the opcode / cond nibble / register bits already in that word
//             must survive, so it is a read-modify-write.
inline void
writeBlockRelField(std::vector<std::uint8_t>& out,
                   std::uint32_t              patchOffset,
                   BlockRelFieldGeometry      g,
                   std::int64_t               disp) noexcept {
    std::uint32_t const o = patchOffset;
    std::uint32_t word =
        static_cast<std::uint32_t>(out[o])
      | (static_cast<std::uint32_t>(out[o + 1]) << 8)
      | (static_cast<std::uint32_t>(out[o + 2]) << 16)
      | (static_cast<std::uint32_t>(out[o + 3]) << 24);
    std::uint32_t const mask =
        (g.width >= 32u) ? 0xFFFFFFFFu : ((1u << g.width) - 1u);
    std::uint32_t const value = static_cast<std::uint32_t>(disp) & mask;
    word = g.wholeField
         ? value
         : ((word & ~(mask << g.lsb)) | (value << g.lsb));
    asm_byte_emit::writeU32LEAt(out, o, word);
}


// ★★★ THE QUESTION "IS THERE A WIDER FIELD TO ESCAPE INTO" CANNOT BE ASKED OF
// THIS TABLE, AND THE ATTEMPT IS RECORDED HERE BECAUSE IT LOOKED RIGHT.
// ✔MEASURED 2026-09-16 (cycle P68 round 3, lane `cfgbr`) by a pin written
// BEFORE the code it checks: a `blockRelWiderKindExists(kind)` that scanned
// every row for a greater `blockRelByteReach` answered TRUE for `Arm64Imm26`,
// because `X86Rel32` (±2 GiB) out-reaches it (±128 MiB) — IN A DIFFERENT ISA.
// The table is a vocabulary of every shape the assembler knows, not of one
// target's, so a table-wide maximum is a cross-ISA leak: it would have told a
// reader whose arm64 `B` overflowed to go declare a wider word that AArch64
// does not have. The question is therefore asked where it can be answered —
// of the OPCODE's own encoding variants, by `electEscapeWord` — and its answer
// rides to the resolver on `BlockRelPatch::widerFieldDeclared`.

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
