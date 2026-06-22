#pragma once

#include <cstdint>

// D-LANG-VARIADIC (step 13.4, 2026-06-02): shared payload encoding for
// MIR/LIR Call instructions. Both tiers stamp a single u32 payload
// describing the call's variadic shape:
//   * `isVariadic` — true iff the callee's FnSig was built variadic
//     (e.g. C-style `extern int printf(const char* fmt, ...)`);
//     downstream ABI lowering emits the platform's variadic-call
//     setup (SysV `mov al, <xmm-arg-count>`, Win64 vararg-double-spill).
//   * `fixedOperandCount` — the number of Call OPERANDS produced by the
//     callee's FIXED (non-variadic) parameters. This is an OPERAND count,
//     NOT a parameter count: a by-value struct fixed param expands to
//     several scalar register-piece operands (FC12a-struct), so one
//     fixed param can contribute >1 operand. Operands at positions
//     [fixedOperandCount, operandCount) within the arg region are the
//     vararg-region OPERANDS (subject to default-promotion + the
//     platform's vararg ABI). lir_callconv iterates the arg operands and
//     uses this boundary (in operand units) to decide which FPR-class
//     operands belong to the variadic region for the SysV AL count.
//   * `hasIndirectResult` — FC7 C3 (AAPCS64/Apple x8 sret): the call returns a
//     by-value aggregate via the cc's `indirectResultRegister` (x8), and the
//     sret-pointer operand is PREPENDED at operand position [callee+1] (the same
//     slot the SysV/Win64 hidden-arg path uses). lir_callconv reads this bit to
//     ROUTE that prepended operand to x8 (not arg-register 0) and to start the
//     real-arg index at the operand after it. Independent of `isVariadic`.
//
// Encoding:
//   * bit 31  (high)   = isVariadic flag
//   * bit 30           = hasIndirectResult flag (x8-sret reroute)
//   * bits 0..29 (low) = fixedOperandCount (30-bit field, mask 0x3FFF'FFFF)
//
// A non-variadic, non-sret call always encodes to payload == 0 — preserving the
// pre-13.4 default for every existing call site. `fixedOperandCount` is only
// consulted when `isVariadic` is true.

namespace dss::call_payload {

inline constexpr std::uint32_t kIsVariadicBit       = 0x8000'0000u;
inline constexpr std::uint32_t kIndirectResultBit   = 0x4000'0000u;
inline constexpr std::uint32_t kFixedOperandMask    = 0x3FFF'FFFFu;

[[nodiscard]] inline constexpr std::uint32_t
encode(bool isVariadic, std::uint32_t fixedOperandCount,
       bool hasIndirectResult = false) noexcept {
    return (isVariadic ? kIsVariadicBit : 0u)
         | (hasIndirectResult ? kIndirectResultBit : 0u)
         | (fixedOperandCount & kFixedOperandMask);
}

[[nodiscard]] inline constexpr bool
isVariadic(std::uint32_t payload) noexcept {
    return (payload & kIsVariadicBit) != 0u;
}

[[nodiscard]] inline constexpr bool
hasIndirectResult(std::uint32_t payload) noexcept {
    return (payload & kIndirectResultBit) != 0u;
}

[[nodiscard]] inline constexpr std::uint32_t
fixedOperandCount(std::uint32_t payload) noexcept {
    return payload & kFixedOperandMask;
}

} // namespace dss::call_payload
