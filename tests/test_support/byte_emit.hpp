#pragma once

#include <cstdint>
#include <vector>

// D-FF1-TEST-BYTE-EMIT CLOSED (FF1-MachO cycle 2026-06-01):
// little-endian byte-emit helpers used by binary-reader tests that
// synthesize minimal ELF/PE/Mach-O binaries in-test. The 3rd consumer
// (`tests/ffi/test_binary_reader_macho.cpp`) lands with FF1-MachO,
// satisfying the hoist precedent (same shape as
// `diagnostic_count.hpp` + `scratch_dir.hpp` — pull the helper once
// 3 structurally-identical consumers exist).
//
// Distinct from `src/link/format/byte_emit.hpp` + `src/asm/format/byte_emit.hpp`
// (production helpers that write into pre-sized buffers via
// `readU32LEAt`/`writeU32LEAt`). The test-side helpers append onto
// growing `std::vector<u8>` for the build-then-read pattern; signatures
// don't compose with the production helpers and the use-case is purely
// fixture construction.

namespace dss::test_support {

// Append a u16/u32/u64 little-endian to a byte vector.
inline void appU16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}
inline void appU32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
inline void appU64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

// Back-patch primitive: write u32 LE at a fixed offset. Used by
// fixture builders that need to fill in RVAs / offsets after the
// dependent regions are laid out.
inline void putU32(std::vector<std::uint8_t>& b, std::size_t off,
                   std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b[off + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}

// Back-patch u64 sibling for fixtures that need 64-bit offsets
// (Mach-O LC_SYMTAB has 64-bit field positions in some commands).
inline void putU64(std::vector<std::uint8_t>& b, std::size_t off,
                   std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        b[off + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}

} // namespace dss::test_support
