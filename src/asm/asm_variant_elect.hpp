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
// (`operandKinds`, `guard.width`, `guard.immMin/immMax`, `guard.negValue`) is the
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
// instruction's operation width (`lirInstWidthBits(flags)`) and
// `memoryIsDestination` its memory-direction axis
// (`lirInstMemoryIsDestination(flags)`) — BOTH read off the same `flags`
// byte, which is what keeps the lowering's election and the encoder's
// variant choice identical.
//
// Returns nullptr when no declared variant matches — which is exactly the
// condition the encoders report as `A_NoMatchingEncodingVariant`, and exactly
// the condition that eliminates a candidate opcode during election.
[[nodiscard]] inline TargetEncodingVariant const*
selectEncodingVariant(TargetOpcodeInfo const&     info,
                      std::span<LirOperand const> instOps,
                      std::uint8_t                instWidthBits,
                      bool memoryIsDestination = false) noexcept {
    for (auto const& v : info.encoding.variants) {
        if (walker_util::variantMatchesInst(instOps, instWidthBits,
                                            memoryIsDestination, v)) {
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
    // D-ASM-DIALECTS-DECLARE-A-REGISTER-CLASS-NO-INSTRUCTION-CAN-NAME: the
    // shape and the width fit, but a register the line names lives in a bank
    // this opcode's field does not draw from, or the destination it writes is
    // a different width than this variant writes.
    // [[D-ASM-ARRANGEMENT-ERASED-TO-A-WIDTH-BEFORE-ELECTION]] added the LANE
    // half to the same verdict rather than a fourth enumerator, because it is
    // the same sentence about the same fields: a lane arrangement is part of
    // how a field reads its register, exactly as its bank and its width are,
    // and splitting it out would ask a reader to know which of three things a
    // "profile" meant before they could read the message.
    WrongRegisterProfile,
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
    case ElectionRejection::WrongRegisterProfile:
        return "its encoding variant takes this operand shape at this width, "
               "but reads its registers differently than they were written — "
               "another bank, another destination width, or a lane "
               "arrangement where a scalar was written (or the reverse)";
    }
    return "unclassified";
}

// ★★★ THE DESTINATION AN ASSEMBLY LINE WROTE, AS THE DIALECT READ IT.
// D-ASM-DIALECTS-DECLARE-A-REGISTER-CLASS-NO-INSTRUCTION-CAN-NAME.
//
// Two facts, and neither of them is in `instOps`: a producer's destination is
// the LIR instruction's RESULT, which is not an operand. `LirRegClass::None`
// and width 0 both mean "the caller states none" — the shape every non-producer
// arm passes and every pre-existing caller behaved as.
struct ElectedDestination {
    LirRegClass  regClass  = LirRegClass::None;
    std::uint8_t widthBits = 0;
    // ★ THE LANE ARRANGEMENT THE DESTINATION WAS **WRITTEN** WITH, in bits per
    // lane — 0 when it was written with none.
    // [[D-ASM-ARRANGEMENT-ERASED-TO-A-WIDTH-BEFORE-ELECTION]].
    //
    // ⚠ UNLIKE `regClass` AND `widthBits` ABOVE, 0 IS A CLAIM AND NOT AN
    // ABSTENTION, and the asymmetry is the point rather than an oversight: a
    // spelling either carried an arrangement suffix or it did not, so there is
    // no state in which the caller "does not know". Reading 0 as "skip the
    // comparison" would restore exactly the leak this axis closes — ✔MEASURED
    // at the P54 base, `cnt d0, d1` compiled and emitted `cnt v0.8b, v1.8b`.
    std::uint8_t laneBits  = 0;
    // ★★★ THE DESTINATION REGISTER'S **ORDINAL**, so the shared-encoding role
    // axis can ask the target's table which register was written.
    // [[D-ASM-ARM64-SP-AND-XZR-SHARE-ENCODING-31-SO-MOV-SP-SILENTLY-BECOMES-ZERO]]
    //
    // ⚠ IT IS AN ORDINAL AND NOT A PRE-RESOLVED ROLE STRING, because the caller
    // that knows the spelling is not the one that owns the meaning: resolving
    // the role at the call site would put a second reader of `registers[]` in
    // `asm_template_to_lir.cpp`, and two readers of one table are how a fact
    // drifts. `nullopt` = the destination is not a register (a memory
    // destination, or a non-producer), which is exactly the state `regClass ==
    // None` above already describes.
    std::optional<std::uint16_t> regOrdinal;
};

// ★★★ THE OPERANDS AN ELECTION IS ASKED ABOUT, AND THE LANE ARRANGEMENT EACH
// WAS **WRITTEN** WITH. [[D-ASM-ARRANGEMENT-ERASED-TO-A-WIDTH-BEFORE-ELECTION]].
//
// ★★ WHY THE LANE FACT RIDES BESIDE THE OPERANDS RATHER THAN ON THEM. A
// `LirOperand` is what the ENCODER re-selects from post-regalloc, and a LIR
// instruction carries no lane shape — the same argument `destWidth`'s comment
// makes about a second width. Widening the LIR operand would put a fact in the
// hot type that only the text tier can produce and only the text tier reads,
// and the encoder's re-selection would still have nothing to read it from. So
// the lane widths travel as a side-channel to exactly the one tier that knows
// what the programmer wrote, and the election's answer is carried in the
// OPCODE, which the encoder does read.
//
// ⚠ AN EMPTY `laneBits` MEANS "EVERY OPERAND WAS WRITTEN WITHOUT AN
// ARRANGEMENT" — the state every caller that cannot spell one is in, and the
// STRICT reading rather than a permissive one. A short array reads 0 past its
// end for the same reason: 0 eliminates a lane-declaring candidate, so a
// truncation fails toward a loud refusal and never toward a wrong instruction.
struct ElectedOperands {
    std::span<LirOperand const>   ops;
    std::span<std::uint8_t const> laneBits;

    [[nodiscard]] std::uint8_t laneOf(std::size_t i) const noexcept {
        return i < laneBits.size() ? laneBits[i] : std::uint8_t{0};
    }
};

// ★★★ THE REGISTER-PROFILE AXIS — the one this anchor is named for, and the
// only election axis whose vocabulary was ALREADY DECLARED before it existed.
//
// ✔MEASURED 2026-09-02 (cycle P54): `electOpcode` keyed on operand SHAPE and
// WIDTH only. Three arm64 opcodes — `fmov`, `movq_xmm_to_gpr`,
// `movq_gpr_to_xmm` — all take `[reg]` at width 64 and differ ONLY in which
// bank each end lives in, so a dialect row naming all three was refused as
// AMBIGUOUS and a row naming one left `fmov x0, d1` electing the diagonal.
// `load_u`/`fldr_u` are the same statement about memory: identical operand
// shapes at identical widths, differing only in the DATA register's class.
//
// ★★ THE AXIS IS THE TARGET'S OWN, NOT A NEW KEY. `encoding.registerClass`,
// `wires[].regClass` and `resultRegClass` have declared exactly this since
// D-OPT-LIR-ARG-REGISTER-CLASS-MISMATCH-FAILLOUD; `encodingWireRegClass` and
// `encodingResultRegClass` are the target's own resolvers. Election simply
// stopped ignoring them. ⇒ no `if (arch ==)` anywhere, and any target that
// declares its banks gets the axis with no engine change.
//
// ★★ WHY IT LIVES HERE AND NOT IN `variantMatchesInst`. The class can never
// separate two variants of ONE opcode: two variants with the same
// `operandKinds` at the same width are refused by `validate()` regardless of
// their banks, so a class-blind matcher and a class-aware one pick the SAME
// variant within an opcode. Putting the axis in the shared matcher would
// therefore change nothing about variant choice while REPLACING the encoder's
// precise wrong-bank diagnostic (`hwEncodingOf`, which names both banks and
// the ordinal) with a generic "no matching variant". So election uses it to
// eliminate an OPCODE, and the encoder's gate stays the last word on a
// wrong-bank operand — two checks that cannot drift, because the encoder's
// re-selection is within the opcode election already fixed.
//
// ⚠ AN UNSTATED CLASS (`LirRegClass::None`) OR WIDTH (0) SKIPS ITS COMPARISON
// RATHER THAN FAILING IT, and that is deliberate: it is exactly the state
// every caller was in before this axis existed, so an operand carrying no
// class tag elects precisely as it always did — and `hwEncodingOf` still
// refuses it at encode time. Silence here is never the last word.
//
// ★★★ THE **LANE** AXIS SITS BESIDE THEM AND IS THE ONE ASYMMETRY IN THIS
// FUNCTION. [[D-ASM-ARRANGEMENT-ERASED-TO-A-WIDTH-BEFORE-ELECTION]].
//
// Class and width can be UNSTATED — a caller that never asked. A lane shape
// cannot: an operand spelling either carried an arrangement suffix or it did
// not, and both answers are facts. So this comparison is TOTAL, in both
// directions, and it is the totality that closes the anchor:
//
//   * a field declared SCALAR refuses an operand written with an arrangement.
//     ✔MEASURED at the P54 base, `fadd v0.8b, v1.8b, v2.8b` compiled and
//     emitted `fadd d0, d1, d2` (0x1E622820) — a scalar double add for a
//     byte-lane spelling, which both references reject. That direction was a
//     silent WRONG ANSWER, not merely an over-acceptance.
//   * a field declared LANES refuses an operand written without one.
//     ✔MEASURED, `cnt d0, d1` compiled and emitted `cnt v0.8b, v1.8b`.
//   * a field declared LANES **AT A WIDTH** refuses another lane width, while
//     a field declared LANES with no width takes any. ✔MEASURED, that split is
//     the machine's own: clang 18.1.3 assembles `mov v0.8b/.4h/.2s/.1d` to the
//     ONE word 0x0EA11C20 (the ORR alias is bitwise), while `addv h0, v1.4h` =
//     0x0E71B820 and `addv b0, v1.8b` = 0x0E31B820 are two instructions.
//
// AGNOSTIC by the same construction the class axis has: the numbers come from
// the target's own wires and the dialect's own arrangement table, and nothing
// here knows what a byte lane is.
//
// ★★★ AND THE **SHARED-ENCODING ROLE** AXIS, THE THIRD OF THE SAME KIND.
// [[D-ASM-ARM64-SP-AND-XZR-SHARE-ENCODING-31-SO-MOV-SP-SILENTLY-BECOMES-ZERO]]
//
// Two registers may be different registers at ONE number in an instruction's
// register field, and which one a field means is a property of the FIELD.
// ✔MEASURED at the P55 base: `mov x0, sp` emitted 0xAA1F03E0 (`mov x0, xzr` —
// x0 got ZERO) and `mov sp, x0` emitted 0xAA0003FF (a NO-OP), because `sp` and
// `xzr` share `hwEncoding` 31 and every election axis was blind to which one
// had been written. gas 2.42 and clang 18.1.3, probed SEPARATELY and agreeing
// on all 128 probes of that census, emit 0x910003E0 / 0x9100001F — the
// ADD-immediate alias, a DIFFERENT INSTRUCTION.
//
// ★★ IT ELIMINATES A CANDIDATE OPCODE, NEVER A VARIANT — the same soundness
// argument the class axis makes above, and here it is what makes the fix
// possible at all: the ORR form and the ADD form are two OPCODES, so election
// choosing between them hands the encoder an opcode whose own variant set it
// re-selects from unambiguously. A role axis inside `variantMatchesInst` would
// have had to carry the register TABLE into a matcher that has only operands.
//
// ★ `requiresRegRole` IS THE SECOND HALF AND IT IS NOT SYMMETRIC WITH THE
// OTHERS. The per-field roles say which registers a form ACCEPTS; that alone
// leaves the SP form accepting `mov x0, x1` too (neither operand carries a
// role, so both fit every field) and the election would report an ambiguity on
// the commonest line in the dialect. `requiresRegRole` states the ARM ARM's own
// alias condition — *this encoding is what the mnemonic means only when a
// register in this role is named* — so the choice is a refusal rather than a
// declaration order.
[[nodiscard]] inline bool
variantAcceptsRegisterProfile(TargetSchema const&          target,
                              TargetEncodingInfo const&    enc,
                              TargetEncodingVariant const& v,
                              ElectedOperands const&       instOps,
                              ElectedDestination const&    dest) noexcept {
    bool sawRequiredRole = v.requiresRegRole.empty();
    for (auto const& w : v.wires) {
        if (w.index >= instOps.ops.size()) continue;
        auto const& op = instOps.ops[w.index];
        if (op.kind != LirOperandKind::Reg) continue;
        auto const written = instOps.laneOf(w.index);
        if (w.lanes != (written != 0)) return false;
        if (w.lanes && w.laneBits != 0 && w.laneBits != written) return false;
        // ⚠ ONLY A **PHYSICAL** REGISTER HAS A ROLE, and the guard is not
        // defensive: a pre-regalloc `LirReg` numbers a VIRTUAL register in its
        // own space, so `id` would index the target's register table by
        // coincidence and read some unrelated row's role. An embedded inline-asm
        // template operand bound to a C value is exactly that shape.
        //
        // ★ A VREG IS ROLE-**LESS**, WHICH IS THE TRUE ANSWER AND NOT A SOFTENED
        // ONE: it will be assigned out of the calling convention's allocatable
        // pools, and a register that shares its encoding is in none of them, so
        // it can only ever become one of the ordinary registers that are the
        // same number in every reading. `mov sp, %0` is legal for exactly that
        // reason — the SP-reading `rn` field takes any non-31 register.
        if (op.reg.isPhysical != 0) {
            auto const ordinal = static_cast<std::uint16_t>(op.reg.id);
            if (!target.registerFitsFieldRole(ordinal, w.regRole)) return false;
            if (!sawRequiredRole
                && target.registerEncodingRole(ordinal) == v.requiresRegRole) {
                sawRequiredRole = true;
            }
        }
        auto const wants = encodingWireRegClass(enc, w);
        if (!wants.has_value()) continue;
        auto const has = static_cast<TargetRegClass>(op.reg.regClass());
        if (has == TargetRegClass::None) continue;
        if (has != *wants) return false;
    }
    // ⚠⚠ A VARIANT WITH NO `resultSlot` HAS NO RESULT FIELD, AND ITS
    // `ElectedDestination` IS AN **INPUT** — the shape every non-producer takes,
    // where the destination-position operand is prepended to the operand list
    // and reaches the variant as a WIRE. ✔MEASURED while building this axis:
    // asking the result-role question there refused `cmp sp, #16` (which both
    // references assemble, 0xF10043FF) because `sp` was tested against an empty
    // `resultRegRole` on a variant whose `rn` wire had already accepted it. The
    // early-out is the same one `destWidth` and `destLanes` below sit behind,
    // and for the same reason.
    if (!v.resultSlot.has_value()) return sawRequiredRole;
    if (dest.regOrdinal.has_value()) {
        if (!target.registerFitsFieldRole(*dest.regOrdinal, v.resultRegRole)) {
            return false;
        }
        // ★★★ AND EVERY **OTHER PLACEMENT** OF THE SAME RESULT REGISTER, because
        // a multi-word macro's words can read one encoding differently.
        // ✔MEASURED 2026-09-03: `adr xzr, main` compiled rc=0 and emitted
        // `adrp xzr, main` followed by `add sp, sp, #:lo12:main` — arm64's
        // `lea` places its destination in an ADRP word (Rd = the zero
        // register) AND an ADD-immediate word (Rd = the stack pointer), and
        // asking only the primary slot let the second word overwrite SP with a
        // relocated address. A register must fit them ALL, which leaves a
        // disagreeing macro accepting exactly the registers whose two readings
        // are the same register — the honest answer.
        for (auto const& x : v.extraResultSlots) {
            if (!target.registerFitsFieldRole(*dest.regOrdinal, x.regRole)) {
                return false;
            }
        }
        if (!sawRequiredRole
            && target.registerEncodingRole(*dest.regOrdinal)
                   == v.requiresRegRole) {
            sawRequiredRole = true;
        }
    }
    if (!sawRequiredRole) return false;
    if (dest.regClass != LirRegClass::None) {
        auto const wants = encodingResultRegClass(enc, v);
        if (wants.has_value()
            && static_cast<TargetRegClass>(dest.regClass) != *wants) {
            return false;
        }
    }
    // The WIDTH half of the same fact. Both sides must have stated it: an
    // undeclared `destWidth` is "as wide as the operation", which the width
    // axis already decided.
    if (v.destWidthBits != 0 && dest.widthBits != 0
        && v.destWidthBits != dest.widthBits) {
        return false;
    }
    // ⚠ THE LANE HALF IS ASKED ONLY OF A DESTINATION THAT IS A REGISTER, which
    // `regClass == None` is the caller's statement of (a memory destination and
    // a non-producer both pass the default `ElectedDestination`). Asking it of
    // a `store`'s absent result would refuse every lane-declaring variant on a
    // shape that has no result field at all.
    if (dest.regClass != LirRegClass::None) {
        if (v.destLanes != (dest.laneBits != 0)) return false;
        if (v.destLanes && v.destLaneBits != 0
            && v.destLaneBits != dest.laneBits) {
            return false;
        }
    }
    return true;
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
// ★★★ UNLESS THE DIALECT HAS DECLARED THE LIST **RANKED**
// (`AsmInstructionSpelling::opcodesAreRankedEncodings`,
// [[D-ASM-ARM64-LDR-TO-LDUR-CONVENIENCE-ALIAS-REFUSED]]), WHICH IS THE ONE
// CASE THE OBJECTION ABOVE DOES NOT COVER. It assumes a tie means the row
// cannot say which INSTRUCTION was written — true when the candidates are
// different instructions, and false when they are two ENCODINGS of one
// operation, because then the programmer never chose between them. ✔MEASURED
// 2026-09-03 on gas 2.42 and clang 18.1.3 (probed SEPARATELY, agreeing, and
// NEITHER warning): `ldr h0,[x1,#2]` is the scaled 0x7D400420 and
// `ldr h0,[x1,#1]` the unscaled 0x7C401020 — one spelling, the assembler
// picking the encoding, scaled preferred where both fit.
// ⚠ THE RANK CHOOSES ONLY AMONG CANDIDATES THAT ALL **FIT**: a candidate is
// reached only after its own variant guard accepted these operands, so the
// order can never select a form that cannot carry the value. That property is
// the guards' to keep, and it is why `guard.immMultipleOf` had to exist before
// this flag could be safe — a range-only guard matches offsets its encoder
// refuses, and ranking would then hand the operation to exactly that form.
//
// `rejections` (when non-null) collects one row per eliminated candidate for
// the caller's diagnostic.
[[nodiscard]] inline std::optional<ElectedOpcode>
electOpcode(TargetSchema const&               target,
            std::span<std::string const>      candidateNames,
            ElectedOperands const&            instOps,
            std::uint8_t                      instWidthBits,
            bool                              memoryIsDestination,
            ElectedDestination const&         destination,
            std::vector<ElectionRejectionRow>* rejections,
            std::string*                       ambiguousWith,
            bool                               rankedEncodings = false) {
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
            selectEncodingVariant(*info, instOps.ops, instWidthBits,
                                  memoryIsDestination);
        if (variant == nullptr) {
            if (rejections != nullptr) {
                rejections->push_back(
                    {name, ElectionRejection::NoMatchingVariant});
            }
            continue;
        }
        // ★ THE REGISTER-PROFILE AXIS ELIMINATES THE CANDIDATE, NEVER THE
        // VARIANT. `selectEncodingVariant` above already made the ONE choice
        // the encoder will re-make; asking the profile question about a
        // DIFFERENT variant of this opcode would elect bytes the encoder then
        // could not reach. If the variant this opcode offers cannot take these
        // banks, the OPCODE is not what was written.
        if (!variantAcceptsRegisterProfile(target, info->encoding, *variant,
                                           instOps, destination)) {
            if (rejections != nullptr) {
                rejections->push_back(
                    {name, ElectionRejection::WrongRegisterProfile});
            }
            continue;
        }
        // ★ A RANKED LIST RETURNS AT THE **FIRST** MATCH RATHER THAN COMPARING
        // — which is not merely the same answer reached differently: it means
        // a later candidate is never evaluated at all, so a fallback encoding
        // costs nothing on the common path and can never contribute an
        // ambiguity of its own.
        if (rankedEncodings) return ElectedOpcode{*ordinal, info, variant};
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

// ★★★ THE DESTINATION-WIDTH HONESTY GATE — the sibling of the one above, and
// the thing that makes a wrong-width election UNSAYABLE rather than merely
// unlikely. D-ASM-DIALECTS-DECLARE-A-REGISTER-CLASS-NO-INSTRUCTION-CAN-NAME.
//
// The axis in `variantAcceptsRegisterProfile` eliminates a candidate that
// DECLARES a `destWidth` and disagrees. That alone is not enough, because a
// candidate that declares NOTHING is eliminated by nothing — which is exactly
// the state every conversion opcode was in when a throwaway `fcvtzs` row
// compiled `fcvtzs %w0, %s1` rc=0 and emitted `fcvtzs x16, s29` (0x9E3803B0),
// the X form, ✔MEASURED. The programmer wrote a 32-bit destination and got a
// 64-bit one with no diagnostic.
//
// So the gate is stated from the other side, over the substrate's own
// vocabulary and with no per-target exception:
//
//   an instruction whose DESTINATION width differs from its OPERATION width
//   is a two-width instruction, and the only variant that may encode it is one
//   that SAYS which destination width it writes.
//
// ⇒ a one-width instruction (`destWidthBits == 0`, or equal to the operation
// width) is unaffected — every pre-existing spelling on both shipped dialects.
// A two-width instruction elected onto a silent variant is REFUSED, and a
// target closes that by declaring the key. ⚠ Deliberately NOT symmetric with
// `variantHonorsDeclaredWidth`, which asks whether the WIDTH reached the bytes:
// there is no natural destination width to fall back to, so the question here
// is whether the variant stated one at all.
[[nodiscard]] inline bool
variantHonorsDeclaredDestWidth(TargetEncodingVariant const& elected,
                               std::uint8_t declaredDestWidthBits,
                               std::uint8_t instWidthBits) noexcept {
    if (declaredDestWidthBits == 0
        || declaredDestWidthBits == instWidthBits) {
        return true;
    }
    return elected.destWidthBits == declaredDestWidthBits;
}

} // namespace dss::asm_elect
