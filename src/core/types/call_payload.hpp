#pragma once

#include <cstdint>

// D-LANG-VARIADIC (step 13.4, 2026-06-02): shared payload encoding for
// MIR/LIR Call instructions. Both tiers stamp a single u32 payload
// describing the call's variadic shape:
//   * `isVariadic` — true iff the callee's FnSig was built variadic
//     (e.g. C-style `extern int printf(const char* fmt, ...)`);
//     downstream ABI lowering emits the platform's variadic-call
//     setup (SysV `mov al, <xmm-arg-count>`, Win64 vararg-double-spill).
//   * `fixedArgCount` — the number of declared (FIXED) parameters
//     of the callee's FnSig. Arguments at positions
//     [fixedArgCount, operandCount) are vararg-region arguments
//     (subject to default-promotion + the platform's vararg ABI).
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
//   * bits 0..29 (low) = fixedArgCount
//
// A non-variadic, non-sret call always encodes to payload == 0 — preserving the
// pre-13.4 default for every existing call site. `fixedArgCount` is only
// consulted when `isVariadic` is true.

namespace dss::call_payload {

inline constexpr std::uint32_t kIsVariadicBit       = 0x8000'0000u;
inline constexpr std::uint32_t kIndirectResultBit   = 0x4000'0000u;
inline constexpr std::uint32_t kFixedArgMask        = 0x3FFF'FFFFu;

[[nodiscard]] inline constexpr std::uint32_t
encode(bool isVariadic, std::uint32_t fixedArgCount,
       bool hasIndirectResult = false) noexcept {
    return (isVariadic ? kIsVariadicBit : 0u)
         | (hasIndirectResult ? kIndirectResultBit : 0u)
         | (fixedArgCount & kFixedArgMask);
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
fixedArgCount(std::uint32_t payload) noexcept {
    return payload & kFixedArgMask;
}

} // namespace dss::call_payload
