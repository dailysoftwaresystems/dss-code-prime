#include "ffi/binary_reader.hpp"

#include "ffi/binary_readers/elf_reader.hpp"
#include "ffi/binary_readers/macho_reader.hpp"
#include "ffi/binary_readers/pe_reader.hpp"
#include "ffi/binary_readers/reader_common.hpp"

#include <fstream>
#include <iterator>
#include <string>

namespace dss::ffi {

// ── Slim dispatch ────────────────────────────────────────────────
//
// D-FF1-NEST split (FF1-MachO cycle 2026-06-01): the per-format
// reader bodies (`readElf64`, `readPe`, `readMacho`) live in
// `binary_readers/{elf,pe,macho}_reader.{hpp,cpp}` after the 3rd-
// reader split. This TU now holds ONLY the format-dispatch entry
// points + the kind→name conversion. Per-format constants, helpers,
// and bytes-walkers live with their reader.

std::string_view binaryReadErrorKindName(BinaryReadErrorKind k) noexcept {
    switch (k) {
        case BinaryReadErrorKind::FileOpenFailed:      return "FileOpenFailed";
        case BinaryReadErrorKind::FileEmpty:           return "FileEmpty";
        case BinaryReadErrorKind::UnknownFormat:       return "UnknownFormat";
        case BinaryReadErrorKind::UnsupportedFormat:   return "UnsupportedFormat";
        case BinaryReadErrorKind::CorruptedBinary:     return "CorruptedBinary";
        case BinaryReadErrorKind::UnsupportedElfClass: return "UnsupportedElfClass";
        case BinaryReadErrorKind::SectionNotFound:     return "SectionNotFound";
    }
    return "Unknown";
}

std::expected<std::vector<ImportSurface>, BinaryReadError>
readImportsFromBytes(std::span<std::uint8_t const> bytes,
                     std::string_view              libraryPathLabel,
                     DiagnosticReporter&           reporter) {
    if (bytes.empty()) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::FileEmpty,
            std::string{"readImports: '"} + std::string{libraryPathLabel}
            + "' is zero bytes (truncated download / build artifact?)",
            reporter));
    }
    auto const guess = guessFormat(bytes);
    switch (guess) {
        case FormatGuess::Elf:
            return readElf64(bytes, libraryPathLabel, reporter);
        case FormatGuess::Pe:
            return readPe(bytes, libraryPathLabel, reporter);
        case FormatGuess::MachO64:
            return readMacho(bytes, libraryPathLabel, reporter);
        case FormatGuess::MachOFat:
            // D-FF1-MACHO-FAT anchor: universal binaries package
            // multiple per-arch slices behind one outer wrapper.
            // v1 expects the caller to extract the desired arch
            // slice (e.g. via `lipo -thin`) before feeding the
            // bytes to readImports. Surfacing UnsupportedFormat
            // with a remediation-specific message guides the
            // operator to the slice action. Trigger to ship FAT
            // reading: first FFI consumer corpus that ships
            // universal binaries the toolchain operator cannot
            // pre-slice (e.g. macOS SDK fixtures bundled in CI).
            // D-FF1-MACHO-VARIANT-KIND anchor (companion): split
            // BinaryReadErrorKind::UnsupportedFormat into
            // UnsupportedFormatVariant when a programmatic
            // consumer (e.g. driver wrapper) needs to recover
            // differently from FAT vs 32-bit (auto-slice FAT,
            // hard-fail 32-bit). Today both share kind +
            // distinguish via detail.
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::UnsupportedFormat,
                std::string{"readImports: '"} + std::string{libraryPathLabel}
                + "' is a Mach-O FAT/universal binary (magic 0xCAFEBABE). "
                  "v1 supports single-arch Mach-O only — slice the "
                  "required arch first (e.g. `lipo -thin arm64 in.dylib "
                  "-output out.dylib`). Anchor D-FF1-MACHO-FAT.",
                reporter));
        case FormatGuess::MachO32:
            // D-FF1-MACHO-32 anchor: 32-bit Mach-O is recognised
            // (so we don't misreport as UnknownFormat) but not
            // supported. Surfacing UnsupportedFormat keeps parity
            // with the ELF32 reject (UnsupportedElfClass) — both
            // are "magic ok, class/width unsupported v1". Trigger
            // to ship 32-bit reading: first 32-bit Mach-O input
            // appears in a real corpus the toolchain operator
            // cannot pre-convert (rare on modern macOS; iOS sim).
            // See D-FF1-MACHO-VARIANT-KIND companion anchor above.
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::UnsupportedFormat,
                std::string{"readImports: '"} + std::string{libraryPathLabel}
                + "' is a 32-bit Mach-O binary (magic 0xFEEDFACE). "
                  "v1 supports 64-bit Mach-O only "
                  "(mach_header_64 / 0xFEEDFACF). Anchor D-FF1-MACHO-32.",
                reporter));
        case FormatGuess::Unknown:
            break;
    }
    return std::unexpected(emitAndReturn(
        BinaryReadErrorKind::UnknownFormat,
        std::string{"readImports: '"} + std::string{libraryPathLabel}
        + "' has no recognised magic (expected ELF '\\x7FELF', PE 'MZ', "
          "or Mach-O 0xFEEDFACF/0xCAFEBABE/0xFEEDFACE).",
        reporter));
}

std::expected<std::vector<ImportSurface>, BinaryReadError>
readImports(std::filesystem::path const& libraryPath,
            DiagnosticReporter&          reporter) {
    std::ifstream in(libraryPath, std::ios::binary);
    if (!in) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::FileOpenFailed,
            "readImports: failed to open '"
            + libraryPath.generic_string() + "' for reading",
            reporter));
    }
    std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>()};
    return readImportsFromBytes(
        std::span<std::uint8_t const>{bytes.data(), bytes.size()},
        libraryPath.generic_string(), reporter);
}

} // namespace dss::ffi
