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
//
// TWO structures escape that rule and MUST go through `readU32BE`
// below: the `ar` armap, and the Mach-O `fat_header` — see the
// kMachOFatMagic block near `guessFormat`. Reaching for `readU32` on
// either reads a byte-swapped value that silently never matches.

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

// Big-endian u32. Two structures in these formats are stored
// big-endian and both decode through here:
//   * the `ar` armap (System V / GNU archive symbol index) -- its count
//     + member-header offsets; everything else in `ar` is ASCII-decimal.
//   * the Mach-O `fat_header` + `fat_arch` / `fat_arch_64` entries --
//     big-endian ON DISK by definition, independent of the slices they
//     wrap (see the kMachOFatMagic block near `guessFormat`).
// Kept beside the LE readers so every reader shares one byte-decode
// home with its siblings.
[[nodiscard]] inline std::uint32_t
readU32BE(std::span<std::uint8_t const> b, std::size_t off) noexcept {
    return  (static_cast<std::uint32_t>(b[off + 0]) << 24)
         | (static_cast<std::uint32_t>(b[off + 1]) << 16)
         | (static_cast<std::uint32_t>(b[off + 2]) <<  8)
         |  static_cast<std::uint32_t>(b[off + 3]);
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
    MachO64    = 3,  // 0xFEEDFACF (stored LE) — 64-bit Mach-O (mach_header_64)
    MachOFat   = 4,  // 0xCAFEBABE / 0xCAFEBABF (stored BE) — universal/FAT — UnsupportedFormat v1
    MachO32    = 5,  // 0xFEEDFACE (stored LE) — 32-bit Mach-O — UnsupportedFormat v1
    Ar         = 6,  // "!<arch>\n" -- Unix `ar` static archive (.a / COFF .lib)
};

// The 8-byte global magic that opens every `ar` archive, GNU / BSD /
// COFF alike: the ASCII "!<arch>\n" (0x0A newline terminator).
constexpr std::uint8_t kArMagic[8] = {
    '!', '<', 'a', 'r', 'c', 'h', '>', 0x0Au};

// ── Mach-O universal ("fat") archive magics — BIG-ENDIAN ON DISK ────
//
// READ THIS BEFORE TOUCHING THE MACH-O ARMS OF `guessFormat`. The two
// Mach-O header families do NOT agree on byte order, and the asymmetry
// is not a quirk of any one file — it is in the format definition:
//
//   * A THIN header (`mach_header` / `mach_header_64`) stores its magic
//     in the SLICE's own byte order. Every target DSS emits is
//     little-endian, so `0xFEEDFACF` lands on disk as `CF FA ED FE` and
//     the little-endian `readU32` is the right lens for it.
//   * A FAT header (`struct fat_header`, Apple `<mach-o/fat.h>`) is
//     defined BIG-ENDIAN ON DISK — ALWAYS, whatever the slices inside
//     it are. Both dyld and `lipo` read it through
//     `OSSwapBigToHostInt32`. So `0xCAFEBABE` lands on disk as
//     `CA FE BA BE` and MUST be matched with `readU32BE`.
//
// Matching a fat magic through `readU32` is therefore not a style
// choice, it is a live bug: the little-endian read of `CA FE BA BE`
// yields `0xBEBAFECA`, so a `readU32(b,0) == 0xCAFEBABE` test never
// fires on any real universal binary. It fires only on the byte-SWAPPED
// spelling `BE BA FE CA` (`FAT_CIGAM` — the value a little-endian HOST
// computes after an in-host-order load, never a byte sequence a
// producer writes). That inversion made `FormatGuess::MachOFat`
// unreachable, which in turn made the D-FF1-MACHO-FAT "run `lipo -thin`
// first" remediation in `binary_reader.cpp` dead code — operators
// feeding a universal `.dylib` got `F_UnknownBinaryFormat` ("no
// recognised magic") from a message that listed `0xCAFEBABE` among the
// magics it recognised.
//
//   FAT_MAGIC    0xCAFEBABE -> CA FE BA BE (20-byte `fat_arch` entries)
//   FAT_MAGIC_64 0xCAFEBABF -> CA FE BA BF (32-byte `fat_arch_64`
//                              entries; what `lipo` emits once a slice
//                              offset/size exceeds 4 GiB)
//
// Classifying these does NOT mean DSS reads universal binaries — it
// still routes to `UnsupportedFormat`. It makes the ALREADY-INTENDED,
// already-anchored failure path REACHABLE so the operator is told to
// slice instead of being told the file is gibberish.
constexpr std::uint32_t kMachOFatMagic   = 0xCAFEBABEu;
constexpr std::uint32_t kMachOFatMagic64 = 0xCAFEBABFu;

[[nodiscard]] inline FormatGuess
guessFormat(std::span<std::uint8_t const> b) noexcept {
    if (b.size() >= 8
     && b[0] == kArMagic[0] && b[1] == kArMagic[1] && b[2] == kArMagic[2]
     && b[3] == kArMagic[3] && b[4] == kArMagic[4] && b[5] == kArMagic[5]
     && b[6] == kArMagic[6] && b[7] == kArMagic[7]) {
        return FormatGuess::Ar;
    }
    if (b.size() >= 4
     && b[0] == 0x7Fu && b[1] == 'E' && b[2] == 'L' && b[3] == 'F') {
        return FormatGuess::Elf;
    }
    if (b.size() >= 2 && b[0] == 'M' && b[1] == 'Z') {
        return FormatGuess::Pe;
    }
    if (b.size() >= 4) {
        // THIN Mach-O: magic stored in the slice's own (little-endian)
        // order.
        std::uint32_t const thinMagic = readU32(b, 0);
        if (thinMagic == 0xFEEDFACFu) return FormatGuess::MachO64;
        if (thinMagic == 0xFEEDFACEu) return FormatGuess::MachO32;
        // FAT/universal: `fat_header` is big-endian ON DISK by
        // definition — see the kMachOFatMagic block above for why this
        // read must NOT reuse `thinMagic`. Deliberately not also
        // matching the byte-swapped spelling: `BE BA FE CA`
        // (`FAT_CIGAM`) is a host-order artifact, not a file, and
        // accepting it would hand the operator a `lipo -thin`
        // instruction that cannot work.
        std::uint32_t const fatMagic = readU32BE(b, 0);
        if (fatMagic == kMachOFatMagic || fatMagic == kMachOFatMagic64)
            return FormatGuess::MachOFat;
    }
    return FormatGuess::Unknown;
}

} // namespace dss::ffi
