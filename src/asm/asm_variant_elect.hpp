#pragma once

// ★★★ THE ONE PLACE THAT ASKS "CAN THIS OPCODE ENCODE THIS OPERAND SHAPE?"
//
// Two tiers ask that question about the SAME instruction and they must agree
// byte-for-byte:
//
//   * the ENCODERS (`format/x86_variable.cpp`, `format/fixed32.cpp`) ask it to
//     pick the variant whose template they are about to emit;
//   * the ASSEMBLY-TEXT LOWERING (`asm_text_to_lir.cpp`) asks it EARLIER, to
//     decide WHICH TARGET OPCODE a dialect mnemonic denotes — AT&T `movq` is
//     the target's `mov` for `movq %rax,%rcx`, its `load` for `movq (%rdi),%rax`
//     and its `store` for `movq %rax,(%rdi)`. The dialect names the candidate
//     SET; the target's own `encoding.variants[].guard` picks the member.
//
// ⚠ WHY THIS IS ONE FUNCTION AND NOT TWO COPIES (operator ruling, 2026-08-12,
// following the `linkNameFor` precedent in `ffi/c_mangle.hpp`): if the lowering
// rolled its own matcher, a divergence between the two would not be a build
// error or a diagnostic — the lowering would bind `movq (%rdi),%rax` to some
// opcode, the encoder would then pick a DIFFERENT variant of it (or none), and
// the failure mode is a green build that emitted the wrong instruction. Callers
// that must agree byte-for-byte drift when each rolls its own.
//
// There is deliberately NO new guard vocabulary here. Every axis
// (`operandKinds`, `guard.width`, `guard.immMin/immMax`, `negMemoffset`) is the
// target's own, evaluated by the target's own predicate
// (`walker_util::variantMatchesInst`); this header only adds the LOOP and the
// candidate-set walk on top of it.

#include "asm/format/walker_util.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir_node.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace dss::asm_elect {

// FIRST-MATCH variant selection within ONE opcode. `instWidthBits` is the
// instruction's operation width (`lirInstWidthBits(flags)`).
//
// Returns nullptr when no declared variant matches — which is exactly the
// condition the encoders report as `A_NoMatchingEncodingVariant`, and exactly
// the condition that eliminates a candidate opcode during election.
[[nodiscard]] inline TargetEncodingVariant const*
selectEncodingVariant(TargetOpcodeInfo const&     info,
                      std::span<LirOperand const> instOps,
                      std::uint8_t                instWidthBits) noexcept {
    for (auto const& v : info.encoding.variants) {
        if (walker_util::variantMatchesInst(instOps, instWidthBits, v)) {
            return &v;
        }
    }
    return nullptr;
}

// One elected (opcode, variant) pair. `info` and `variant` point into the
// TargetSchema, which outlives every caller here.
struct ElectedOpcode {
    std::uint16_t                opcode  = 0;
    TargetOpcodeInfo const*      info    = nullptr;
    TargetEncodingVariant const* variant = nullptr;
};

// Why a candidate was eliminated — so the refusal can say which of the
// dialect's opcodes was tried and what stopped it, instead of "no match".
enum class ElectionRejection : std::uint8_t {
    UnknownToTarget,    // the target declares no opcode by that name
    NoEncodingDeclared, // declared, but `encoding.shape == None`
    NoMatchingVariant,  // declared + encodable, but no variant takes this shape
};

struct ElectionRejectionRow {
    std::string       opcodeName;
    ElectionRejection why{};
};

[[nodiscard]] constexpr std::string_view
electionRejectionText(ElectionRejection why) noexcept {
    switch (why) {
    case ElectionRejection::UnknownToTarget:
        return "the target declares no opcode by that name";
    case ElectionRejection::NoEncodingDeclared:
        return "the target declares the opcode but gives it no encoding";
    case ElectionRejection::NoMatchingVariant:
        return "no encoding variant of it accepts this operand shape at this "
               "width";
    }
    return "unclassified";
}

// ★ ELECT ONE OPCODE FROM A CANDIDATE SET, BY ASKING THE TARGET.
//
// Walks `candidateNames` IN DECLARATION ORDER and returns the first whose own
// `encoding.variants[]` can take `instOps` at `instWidthBits`. Declaration
// order is the dialect's tie-break and is the only ordering fact the engine
// consumes — it never inspects a mnemonic's spelling.
//
// ⚠ AMBIGUITY IS NOT RESOLVED SILENTLY. When more than one candidate matches,
// `ambiguousWith` is filled with the SECOND matcher's name and nullopt is
// returned: two target opcodes that both accept one operand shape mean the
// dialect row cannot say which instruction the programmer wrote, and picking
// the first would be a coin flip baked into config order.
//
// `rejections` (when non-null) collects one row per eliminated candidate for
// the caller's diagnostic.
[[nodiscard]] inline std::optional<ElectedOpcode>
electOpcode(TargetSchema const&               target,
            std::span<std::string const>      candidateNames,
            std::span<LirOperand const>       instOps,
            std::uint8_t                      instWidthBits,
            std::vector<ElectionRejectionRow>* rejections,
            std::string*                       ambiguousWith) {
    std::optional<ElectedOpcode> winner;
    for (auto const& name : candidateNames) {
        auto const ordinal = target.opcodeByMnemonic(name);
        if (!ordinal) {
            if (rejections != nullptr) {
                rejections->push_back(
                    {name, ElectionRejection::UnknownToTarget});
            }
            continue;
        }
        auto const* info = target.opcodeInfo(*ordinal);
        if (info == nullptr || info->encoding.variants.empty()) {
            if (rejections != nullptr) {
                rejections->push_back(
                    {name, ElectionRejection::NoEncodingDeclared});
            }
            continue;
        }
        auto const* variant =
            selectEncodingVariant(*info, instOps, instWidthBits);
        if (variant == nullptr) {
            if (rejections != nullptr) {
                rejections->push_back(
                    {name, ElectionRejection::NoMatchingVariant});
            }
            continue;
        }
        if (winner.has_value()) {
            if (ambiguousWith != nullptr) *ambiguousWith = name;
            return std::nullopt;
        }
        winner = ElectedOpcode{*ordinal, info, variant};
    }
    return winner;
}

// ★★ THE WIDTH-HONESTY GATE — a TEXT-LOWERING check, never an encoder one.
//
// ✔MEASURED 2026-08-13: arm64's `mov` ships exactly two variants and NEITHER
// carries `guard.width`, while `variantMatchesInst` treats `guardWidthBits == 0`
// as "matches ANY width". So a dialect row declaring `mov` at width 32 elects
// the width-ABSENT `["reg"]` variant and encodes `ORR Xd, XZR, Xm` — a 64-bit
// move. The programmer wrote `mov w0, w1`, whose whole observable difference
// from `mov x0, x1` is that it ZEROES bits 63:32. x86's `lea` has the identical
// shape (four variants, none width-keyed), so `leal` would silently emit the
// REX.W form.
//
// That is a SILENT MISCOMPILE with no diagnostic anywhere, and it is not
// arm64's or x86's problem to fix one target at a time — it is a property of
// "a dialect declares a width; the target may not discriminate on it". So the
// gate is stated once, here, over the substrate's own vocabulary:
//
//   an ELECTED variant that carries no width key encodes at the target's
//   NATURAL width for that form. That is honest only when the dialect asked
//   for the width a flags-less LIR instruction already means
//   (`lirInstWidthBits(0)`). Any other declared width would be dropped on the
//   floor between the dialect document and the emitted bytes.
//
// Returns true when the pairing is honest. A target that later declares the
// missing width-keyed variant lights this up with no engine change — which is
// the point: the gate never needs a per-target exception.
[[nodiscard]] inline bool
variantHonorsDeclaredWidth(TargetEncodingVariant const& elected,
                           std::uint8_t declaredWidthBits) noexcept {
    if (elected.guardWidthBits != 0) {
        // The matcher already required equality to select it.
        return true;
    }
    return declaredWidthBits == lirInstWidthBits(0);
}

} // namespace dss::asm_elect
