#pragma once

// Shared byte-reading helpers for linker-format tests. Hoisted from
// per-test-file duplicates once a 3rd consumer landed (LK9 SPIR-V
// tests joined Mach-O's macho_test_support.hpp and LK8 WASM's
// inline wrapper — code-simplifier REQUIRED fold, LK9 post-fold
// review). Mirrors the asm/asm_test_support.hpp + macho_test_
// support.hpp precedent.

#include <cstdint>
#include <span>

namespace dss::link_format::test {

inline std::uint16_t readU16LE(std::span<std::uint8_t const> bytes,
                                std::size_t off) {
    return static_cast<std::uint16_t>(bytes[off]) |
           static_cast<std::uint16_t>(
               static_cast<std::uint16_t>(bytes[off+1]) << 8);
}

inline std::uint32_t readU32LE(std::span<std::uint8_t const> bytes,
                                std::size_t off) {
    return static_cast<std::uint32_t>(bytes[off]) |
           (static_cast<std::uint32_t>(bytes[off+1]) << 8) |
           (static_cast<std::uint32_t>(bytes[off+2]) << 16) |
           (static_cast<std::uint32_t>(bytes[off+3]) << 24);
}

inline std::uint64_t readU64LE(std::span<std::uint8_t const> bytes,
                                std::size_t off) {
    std::uint64_t v = 0;
    for (int b = 0; b < 8; ++b)
        v |= static_cast<std::uint64_t>(bytes[off + b]) << (b * 8);
    return v;
}

}  // namespace dss::link_format::test
