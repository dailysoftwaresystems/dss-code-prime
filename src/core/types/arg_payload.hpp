#pragma once

#include <cstdint>

// D-OPT-RELEASE-SYSV-MIXED-CLASS-REG-ARG-DROP (2026-07-07): shared payload
// encoding for MIR `Arg` instructions. An Arg carries TWO indices that were
// historically conflated into one u32 payload:
//
//   * `ordinal` (low 16) — the per-ABI-CLASS register ordinal. On a SysV-style
//     CC the GPR and FPR pools count INDEPENDENTLY, so `int a, double x` gives
//     a=gpr#0, x=fpr#0 — BOTH ordinal 0. lir_callconv assigns physical
//     registers from (register class, ordinal); this is the value it needs.
//
//   * `position` (high 16) — the FLAT index of this parameter's value in the
//     CALL's actual-argument operand list (operands[1 + position]; operand 0 is
//     the callee address). Declaration order == call-site operand-expansion
//     order == parameter-receive order, so `a`=pos 0, `x`=pos 1. The INLINER
//     needs this: it maps a callee `Arg` to the caller's actual argument, and
//     the ordinal is ambiguous across register classes (both class-0 above).
//
// Before this split the inliner read the ordinal AS the position, so a mixed
// int+FP callee spliced the WRONG actuals (`(int)x` read the int arg → 0, a
// silent miscompile under the release pipeline's Inlining pass; ms_x64 was
// immune only because its slot-aligned CC makes ordinal == position). The two
// indices are now independent so the mapping is unambiguous.
//
// For a SINGLE-CLASS signature (all-GPR or all-FPR) ordinal == position, so the
// `MirBuilder::addArg(ordinal, type)` convenience defaults position := ordinal
// and every pre-existing call site (and hand-built test) stays correct.
//
// Encoding: `(position << 16) | ordinal`, each a 16-bit field. A >65535 index
// (an absurd ~65k-argument call) fails loud at `addArg` rather than wrapping.

namespace dss::arg_payload {

inline constexpr std::uint32_t kFieldMax    = 0xFFFFu;
inline constexpr std::uint32_t kOrdinalMask = 0xFFFFu;
inline constexpr unsigned      kPositionShift = 16u;

[[nodiscard]] inline constexpr std::uint32_t
encode(std::uint32_t ordinal, std::uint32_t position) noexcept {
    return (position << kPositionShift) | (ordinal & kOrdinalMask);
}

[[nodiscard]] inline constexpr std::uint32_t
ordinal(std::uint32_t payload) noexcept {
    return payload & kOrdinalMask;
}

[[nodiscard]] inline constexpr std::uint32_t
position(std::uint32_t payload) noexcept {
    return payload >> kPositionShift;
}

} // namespace dss::arg_payload
