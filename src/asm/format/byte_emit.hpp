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

// D-CSUBSET-BITFIELD-WIDE-UNIT (v0.0.2 FC8): append 8 little-endian
// bytes — the `io` immediate of x86 `mov r64, imm64` (REX.W B8+rd io).
// The FIRST 64-bit immediate any walker emits. Generic over the
// 8-byte-immediate forms of any ISA (no x86-specific behavior), so it
// lives in the shared byte-emit substrate alongside `appendU32LE`.
inline void appendU64LE(std::vector<std::uint8_t>& out, std::uint64_t v) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((v >> shift) & 0xFFu));
    }
}

// D-CSUBSET-WHILE-LOOP-SUBSTRATE (step 13.5 cycle 1, 2026-06-03):
// in-place 4-byte LE write at a known offset. Used by asm.cpp's
// per-function patch-resolve loop to overwrite BlockRel32 placeholder
// zeros with the resolved displacement. The asm tier intentionally
// owns this helper (mirroring `dss::byte_emit::writeU32LEAt` in
// `link/format/byte_emit.hpp`) so the asm-tier patch code does not
// take a depend-up on the link tier.
inline void writeU32LEAt(std::vector<std::uint8_t>& buf,
                         std::size_t off, std::uint32_t v) noexcept {
    buf[off + 0] = static_cast<std::uint8_t>(v         & 0xFFu);
    buf[off + 1] = static_cast<std::uint8_t>((v >>  8) & 0xFFu);
    buf[off + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    buf[off + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
}

} // namespace dss::asm_byte_emit
