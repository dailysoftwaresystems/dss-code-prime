#pragma once

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "ffi/binary_reader.hpp"

#include <cstdint>
#include <span>
#include <string>

// Per-format readers (`elf_reader.cpp`, `pe_reader.cpp`,
// `macho_reader.cpp`) all consume these byte-decode primitives +
// reporter wiring; centralising prevents three-way duplication and
// keeps each per-format TU at one architectural concern. Internal
// header (none of these symbols are `DSS_EXPORT`d — they live one
// scope above the per-format anonymous namespaces).
//
// Source/target/linker agnostic: the helpers operate on
// `std::span<uint8_t const>` byte buffers and emit through the
// codebase's existing `DiagnosticReporter`. No platform-specific
// headers, no target-arch references, no linker concepts.

namespace dss::ffi {

// ── Reporter wiring ─────────────────────────────────────────────

// Map `BinaryReadErrorKind` → structured `DiagnosticCode::F_*`. The
// kind enum is the function-return shape (compact); the F_* code is
// the in-reporter shape that `--suppress` / `--warnings-as-errors`
// consume. Closed-table dispatch — adding a new variant requires
// updating the switch AND the static_assert in this header.
[[nodiscard]] constexpr DiagnosticCode
toDiagnosticCode(BinaryReadErrorKind k) noexcept {
    switch (k) {
        case BinaryReadErrorKind::FileOpenFailed:      return DiagnosticCode::F_FileOpenFailed;
        case BinaryReadErrorKind::FileEmpty:           return DiagnosticCode::F_FileEmpty;
        case BinaryReadErrorKind::UnknownFormat:       return DiagnosticCode::F_UnknownBinaryFormat;
        case BinaryReadErrorKind::UnsupportedFormat:   return DiagnosticCode::F_UnsupportedBinaryFormat;
        case BinaryReadErrorKind::CorruptedBinary:     return DiagnosticCode::F_CorruptedBinary;
        case BinaryReadErrorKind::UnsupportedElfClass: return DiagnosticCode::F_UnsupportedElfClass;
        case BinaryReadErrorKind::SectionNotFound:     return DiagnosticCode::F_SectionNotFound;
    }
    // Unreachable per the closed enum; if a new variant lands without
    // updating this switch, emit `F_CorruptedBinary` as a fail-loud
    // (rather than `None` which would silently produce an uncoded
    // diagnostic that `--suppress` cannot target).
    return DiagnosticCode::F_CorruptedBinary;
}

static_assert(static_cast<std::uint8_t>(BinaryReadErrorKind::SectionNotFound) == 6u,
              "BinaryReadErrorKind grew without updating "
              "toDiagnosticCode — add a switch arm for the new variant.");

// Emit a binary-reader failure through the run-wide DiagnosticReporter
// AND return the structured BinaryReadError. Centralises the kind →
// F_* code mapping so every failure path produces a remediation-
// distinct diagnostic that downstream policy consumes.
[[nodiscard]] inline BinaryReadError
emitAndReturn(BinaryReadErrorKind kind, std::string detail,
              DiagnosticReporter& reporter) {
    dss::report(reporter, toDiagnosticCode(kind),
                DiagnosticSeverity::Error, detail);
    return BinaryReadError{kind, std::move(detail)};
}

// ── Little-endian byte readers ──────────────────────────────────
//
// All three on-disk binary formats (ELF / PE / Mach-O 64-bit) store
// scalars in little-endian. ELF technically supports big-endian
// (EI_DATA=2/MSB), but v1 enforces ELFDATA2LSB; Mach-O's mach_header
// has cputype + magic that disambiguate endianness via the magic
// value itself (`0xFEEDFACF` LE vs `0xCFFAEDFE` BE).

[[nodiscard]] inline std::uint16_t
readU16(std::span<std::uint8_t const> b, std::size_t off) noexcept {
    return  static_cast<std::uint16_t>(b[off + 0])
         | (static_cast<std::uint16_t>(b[off + 1]) << 8);
}
[[nodiscard]] inline std::uint32_t
readU32(std::span<std::uint8_t const> b, std::size_t off) noexcept {
    return  static_cast<std::uint32_t>(b[off + 0])
         | (static_cast<std::uint32_t>(b[off + 1]) <<  8)
         | (static_cast<std::uint32_t>(b[off + 2]) << 16)
         | (static_cast<std::uint32_t>(b[off + 3]) << 24);
}
[[nodiscard]] inline std::uint64_t
readU64(std::span<std::uint8_t const> b, std::size_t off) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(b[off + i]) << (i * 8);
    }
    return v;
}

// Read a NUL-terminated C string from a string table at `index`,
// bounded by the table's size. Returns empty string on out-of-range
// (caller-side check; we never read past `tableEnd`).
[[nodiscard]] inline std::string
readNulTerminated(std::span<std::uint8_t const> bytes,
                  std::size_t                   tableStart,
                  std::size_t                   tableEnd,
                  std::uint32_t                 index) {
    std::size_t const start = tableStart + index;
    if (start >= tableEnd) return {};
    std::size_t end = start;
    while (end < tableEnd && bytes[end] != 0u) ++end;
    return std::string{
        reinterpret_cast<char const*>(&bytes[start]),
        static_cast<std::size_t>(end - start)};
}

// ── Format detection ────────────────────────────────────────────

enum class FormatGuess : std::uint8_t {
    Unknown    = 0,
    Elf        = 1,
    Pe         = 2,
    MachO64    = 3,  // 0xFEEDFACF — 64-bit Mach-O (mach_header_64)
    MachOFat   = 4,  // 0xCAFEBABE — universal/FAT — UnsupportedFormat v1
    MachO32    = 5,  // 0xFEEDFACE — 32-bit Mach-O — UnsupportedFormat v1
};

[[nodiscard]] inline FormatGuess
guessFormat(std::span<std::uint8_t const> b) noexcept {
    if (b.size() >= 4
     && b[0] == 0x7Fu && b[1] == 'E' && b[2] == 'L' && b[3] == 'F') {
        return FormatGuess::Elf;
    }
    if (b.size() >= 2 && b[0] == 'M' && b[1] == 'Z') {
        return FormatGuess::Pe;
    }
    if (b.size() >= 4) {
        std::uint32_t const magic = readU32(b, 0);
        if (magic == 0xFEEDFACFu) return FormatGuess::MachO64;
        if (magic == 0xCAFEBABEu) return FormatGuess::MachOFat;
        if (magic == 0xFEEDFACEu) return FormatGuess::MachO32;
    }
    return FormatGuess::Unknown;
}

} // namespace dss::ffi
