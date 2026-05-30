#pragma once

// Shared helpers for Mach-O writer tests. Promoted from per-test-
// file duplicates after LK6 cycle 2c's 3 byte-pin tests tripped the
// 3rd-consumer threshold for the LC-scan walk (code-simplifier
// REQUIRED fold, post-fold review). Mirrors tests/asm/asm_test_
// support.hpp precedent.

#include "link_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dss::macho::test {

// Walk the Mach-O load-command sequence and return the byte offset
// of the first load command whose `cmd` field matches `cmd`. The
// returned offset points at the START of the load command (i.e. its
// `cmd` u32), so callers can read sub-fields by adding their
// documented offsets from `<mach-o/loader.h>`. Returns nullopt if
// the command is not present.
[[nodiscard]] inline std::optional<std::size_t>
findLoadCommand(std::span<std::uint8_t const> bytes, std::uint32_t cmd) {
    if (bytes.size() < 32) return std::nullopt;
    std::uint32_t const ncmds =
        static_cast<std::uint32_t>(bytes[16]) |
        (static_cast<std::uint32_t>(bytes[17]) << 8) |
        (static_cast<std::uint32_t>(bytes[18]) << 16) |
        (static_cast<std::uint32_t>(bytes[19]) << 24);
    std::size_t off = 32;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        if (off + 8 > bytes.size()) return std::nullopt;
        std::uint32_t const thisCmd =
            static_cast<std::uint32_t>(bytes[off]) |
            (static_cast<std::uint32_t>(bytes[off+1]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+2]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+3]) << 24);
        std::uint32_t const cmdsize =
            static_cast<std::uint32_t>(bytes[off+4]) |
            (static_cast<std::uint32_t>(bytes[off+5]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+6]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+7]) << 24);
        if (thisCmd == cmd) return off;
        if (cmdsize == 0) return std::nullopt;  // malformed
        off += cmdsize;
    }
    return std::nullopt;
}

// Walk the Mach-O LC_SEGMENT_64 sequence to locate the segment with
// the given name. Returns the byte offset of the LC_SEGMENT_64
// header (the cmd field), or nullopt if not found.
[[nodiscard]] inline std::optional<std::size_t>
findSegment(std::span<std::uint8_t const> bytes, std::string_view name) {
    if (bytes.size() < 32) return std::nullopt;
    std::uint32_t const ncmds =
        static_cast<std::uint32_t>(bytes[16]) |
        (static_cast<std::uint32_t>(bytes[17]) << 8) |
        (static_cast<std::uint32_t>(bytes[18]) << 16) |
        (static_cast<std::uint32_t>(bytes[19]) << 24);
    std::size_t off = 32;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        if (off + 24 > bytes.size()) return std::nullopt;
        std::uint32_t const thisCmd =
            static_cast<std::uint32_t>(bytes[off]) |
            (static_cast<std::uint32_t>(bytes[off+1]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+2]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+3]) << 24);
        std::uint32_t const cmdsize =
            static_cast<std::uint32_t>(bytes[off+4]) |
            (static_cast<std::uint32_t>(bytes[off+5]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+6]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+7]) << 24);
        if (thisCmd == 0x19u) {  // LC_SEGMENT_64
            std::string segName(
                reinterpret_cast<char const*>(&bytes[off + 8]),
                strnlen(reinterpret_cast<char const*>(
                            &bytes[off + 8]), 16));
            if (segName == name) return off;
        }
        if (cmdsize == 0) return std::nullopt;
        off += cmdsize;
    }
    return std::nullopt;
}

// Locate a section_64 record by (segment name, section name).
// Returns the byte offset of the section_64 record (NOT the
// containing LC_SEGMENT_64), or nullopt.
[[nodiscard]] inline std::optional<std::size_t>
findSection(std::span<std::uint8_t const> bytes,
            std::string_view segment, std::string_view section) {
    auto segOff = findSegment(bytes, segment);
    if (!segOff) return std::nullopt;
    if (*segOff + 68 > bytes.size()) return std::nullopt;
    std::uint32_t const nsects =
        static_cast<std::uint32_t>(bytes[*segOff + 64]) |
        (static_cast<std::uint32_t>(bytes[*segOff + 65]) << 8) |
        (static_cast<std::uint32_t>(bytes[*segOff + 66]) << 16) |
        (static_cast<std::uint32_t>(bytes[*segOff + 67]) << 24);
    std::size_t secOff = *segOff + 72;
    for (std::uint32_t s = 0; s < nsects; ++s) {
        if (secOff + 16 > bytes.size()) return std::nullopt;
        std::string secName(
            reinterpret_cast<char const*>(&bytes[secOff]),
            strnlen(reinterpret_cast<char const*>(
                        &bytes[secOff]), 16));
        if (secName == section) return secOff;
        secOff += 80;
    }
    return std::nullopt;
}

// readU32LE / readU64LE re-exported from the shared
// `link_test_support.hpp` substrate (LK9 post-fold review — 3rd-
// consumer threshold reached via spirv test). Keeping the old
// `dss::macho::test::readU32LE` symbol alive avoids breaking
// existing Mach-O test files.
inline std::uint32_t readU32LE(std::span<std::uint8_t const> bytes,
                                std::size_t off) {
    return dss::link_format::test::readU32LE(bytes, off);
}
inline std::uint64_t readU64LE(std::span<std::uint8_t const> bytes,
                                std::size_t off) {
    return dss::link_format::test::readU64LE(bytes, off);
}

}  // namespace dss::macho::test
