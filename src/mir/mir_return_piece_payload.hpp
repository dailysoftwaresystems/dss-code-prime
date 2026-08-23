#pragma once

#include <cstdint>

// Plan 29 §4.4.5 (inline-asm P5, ruling 4): shared payload encoding for MIR
// `ReturnPiece` instructions. A result piece carries TWO facts and — until this
// split — only ONE of them was stored:
//
//   * `ordinal` (low 16) — the PER-CLASS result-register index. GPR and FPR
//     pools count independently, so `{long, double}` gives gpr#0 and fpr#0 —
//     BOTH ordinal 0. `lir_callconv::returnReg` indexes a pool with it.
//
//   * `regClass` (high 16, a `TargetRegClass`) — WHICH pool the ordinal indexes.
//     ✔MEASURED before the split: `addReturnPiece` took no class parameter;
//     `hir_to_mir` ran separate gpr/fpr counters, picked the ordinal from the
//     piece's class and then DISCARDED the class, re-encoding it as a TYPE
//     (`pieceType()`, Fpr→F32/F64/F128, everything else→I64). Recovery was
//     `regClassForCoreType(the MIR type)`.
//
// ★ WHY THE INFERENCE HAD TO GO. "The class follows from the type" holds only
// while the producer is a Call, whose piece types come from the ABI classifier.
// An inline-asm output constraint chooses its class INDEPENDENTLY of the value's
// type — `"=x"` legally binds an integer-typed value to an SSE register — so a
// type-derived class would index the GPR pool (the else-branch default) for a
// value the asm block leaves in an FPR. That is a silent miscompile with no
// diagnostic anywhere on the path, which is why the class is now carried rather
// than re-derived.
//
// ★ WHY THE TWO FIELDS SHARE `payload` RATHER THAN TAKING `payload2`. The three
// live MIR verbatim-copy sites (`mir_rebuild_helper.cpp`, `inlining.cpp` ×2)
// forward `instPayload` — so an encoding that rides `payload` survives every
// rebuild BY CONSTRUCTION, with no copy site's participation. `payload2` is
// carried too today, but "carried by every current site" is a census, not an
// invariant; `payload` is where the ordinal already lived. Same reasoning, same
// shape, and the same 16/16 split as `arg_payload.hpp` (which this mirrors).
//
// A >65535 ordinal (an absurd 65k-piece return) fails loud at `addReturnPiece`
// rather than wrapping into the class field.

namespace dss::return_piece_payload {

inline constexpr std::uint32_t kOrdinalMax    = 0xFFFFu;
inline constexpr std::uint32_t kOrdinalMask   = 0xFFFFu;
inline constexpr unsigned      kRegClassShift = 16u;

[[nodiscard]] inline constexpr std::uint32_t
encode(std::uint32_t ordinal, std::uint32_t regClass) noexcept {
    return (regClass << kRegClassShift) | (ordinal & kOrdinalMask);
}

[[nodiscard]] inline constexpr std::uint32_t
ordinal(std::uint32_t payload) noexcept {
    return payload & kOrdinalMask;
}

[[nodiscard]] inline constexpr std::uint32_t
regClass(std::uint32_t payload) noexcept {
    return payload >> kRegClassShift;
}

} // namespace dss::return_piece_payload
