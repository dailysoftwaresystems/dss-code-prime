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
//
// Encoding:
//   * bit 31  (high)   = isVariadic flag
//   * bits 0..30 (low) = fixedArgCount
//
// A non-variadic call always encodes to payload == 0 — preserving the
// pre-13.4 default for every existing call site. `fixedArgCount` is
// only consulted when `isVariadic` is true.

namespace dss::call_payload {

inline constexpr std::uint32_t kIsVariadicBit = 0x8000'0000u;
inline constexpr std::uint32_t kFixedArgMask  = 0x7FFF'FFFFu;

[[nodiscard]] inline constexpr std::uint32_t
encode(bool isVariadic, std::uint32_t fixedArgCount) noexcept {
    return (isVariadic ? kIsVariadicBit : 0u)
         | (fixedArgCount & kFixedArgMask);
}

[[nodiscard]] inline constexpr bool
isVariadic(std::uint32_t payload) noexcept {
    return (payload & kIsVariadicBit) != 0u;
}

[[nodiscard]] inline constexpr std::uint32_t
fixedArgCount(std::uint32_t payload) noexcept {
    return payload & kFixedArgMask;
}

} // namespace dss::call_payload
