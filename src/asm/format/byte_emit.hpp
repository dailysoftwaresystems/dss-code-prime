#pragma once

#include <cstdint>
#include <vector>

// Format-walker byte-emission primitives (plan 13 AS3 followup).
//
// The x86-variable and fixed32 walkers both append little-endian
// 32-bit values to the output byte stream — `appendImm32LE` in
// `x86_variable.cpp` (after casting int32→uint32) and `appendWordLE`
// in `fixed32.cpp` were byte-for-byte identical. Hoisted here per
// simplifier review.
//
// These are intentionally plain free functions — no class, no
// template — because the substrate has zero shape-specific behavior.
// Adding a walker for a future shape (`vliw_bundle`, RISC-V
// compressed 16-bit, etc.) reuses these directly.

namespace dss::asm_byte_emit {

inline void appendU32LE(std::vector<std::uint8_t>& out, std::uint32_t v) noexcept {
    out.push_back(static_cast<std::uint8_t>(v         & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >>  8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

// `appendImm32LE` is the signed-int32 alias of `appendU32LE` — every
// existing caller wraps with a `static_cast<uint32_t>` of an
// `int32_t`. The cast is well-defined under C++20+ (two's
// complement); making the helper signed-aware avoids the cast at
// the call site.
inline void appendImm32LE(std::vector<std::uint8_t>& out, std::int32_t v) noexcept {
    appendU32LE(out, static_cast<std::uint32_t>(v));
}

} // namespace dss::asm_byte_emit
