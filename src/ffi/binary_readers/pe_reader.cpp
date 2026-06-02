#include "ffi/binary_readers/pe_reader.hpp"

#include "core/cpp_invariants.hpp"  // arithmetic-right-shift static_assert
#include "core/types/parse_diagnostic.hpp"
#include "ffi/binary_readers/reader_common.hpp"

#include <format>
#include <optional>
#include <string>

namespace dss::ffi {

namespace {

// ── PE export-table reader (FF1-PE) ─────────────────────────────
// Plan 11 FF1-PE: parse a PE32 / PE32+ binary's `.edata` export
// directory and emit one ImportSurface row per exported name. The
// PE format is structurally simpler than ELF for export-walking:
// the export directory is reached directly via DataDirectories[0]
// (no string-table indirection like ELF's .dynstr). Imports
// (`.idata`) are deferred — v1 reads EXPORTS from a `.dll`.
//
// Layout audited from the official PE/COFF spec rev 8.3 (Microsoft):
//   DOS header [64] → PE signature @ offset[0x3C] → COFF header [20]
//   → Optional header (96 / 240 bytes for PE32 / PE32+)
//   → Section table (N × 40 bytes)
//   → DataDirectories[0] = (RVA, size) of .edata export directory
// Inside .edata (Microsoft PE/COFF spec rev 8.3, "Export Directory Table"):
//   [ 0.. 3] ExportFlags        (skipped)
//   [ 4.. 7] TimeDateStamp      (skipped)
//   [ 8..11] MajorVersion + MinorVersion (skipped)
//   [12..15] NameRva            (DLL name; skipped — we use libraryPathLabel)
//   [16..19] OrdinalBase        (skipped — v1 doesn't read ordinals)
//   [20..23] AddressTableEntries (count of function RVAs)
//   [24..27] NumberOfNamePointers
//   [28..31] AddressOfFunctions   (RVA — skipped, v1 only reads names)
//   [32..35] AddressOfNames       (RVA → array of u32 RVAs to NUL-strings)
//   [36..39] AddressOfNameOrdinals (RVA → array of u16; skipped)
//
// All exported NAMES become Function/Default/External rows. PE has no
// type info equivalent to ELF's STT_OBJECT vs STT_FUNC + no visibility
// granularity vs ELF's STV_*. v1 maps every export to Function/Default.
// If FFI ever needs PE Object exports (rare), refine then — anchor
// D-FF1-PE-OBJECT-EXPORTS reserved.

constexpr std::size_t kPeDosHeaderSize  = 64;
constexpr std::size_t kPeCoffHeaderSize = 20;
constexpr std::size_t kPeSectionHdrSize = 40;
constexpr std::uint16_t kPeMagicPe32    = 0x010B;
constexpr std::uint16_t kPeMagicPe32Plus = 0x020B;

} // namespace

std::expected<std::vector<ImportSurface>, BinaryReadError>
readPe(std::span<std::uint8_t const> bytes,
       std::string_view              libraryPathLabel,
       DiagnosticReporter&           reporter) {
    // ── DOS header + PE signature pointer ──
    if (bytes.size() < kPeDosHeaderSize) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "PE reader: file shorter than DOS header (64 bytes)", reporter));
    }
    if (bytes[0] != 'M' || bytes[1] != 'Z') {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "PE reader: DOS header magic is not 'MZ'", reporter));
    }
    std::uint32_t const peOffset = readU32(bytes, 0x3C);

    // ── PE signature ──
    if (rangeExceedsBuffer(peOffset, 4, bytes.size())) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "PE reader: PE-signature offset (at DOS[0x3C]="
            + std::to_string(peOffset)
            + ") points past EOF", reporter));
    }
    if (bytes[peOffset + 0] != 'P' || bytes[peOffset + 1] != 'E'
     || bytes[peOffset + 2] != 0   || bytes[peOffset + 3] != 0) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "PE reader: PE signature missing at offset "
            + std::to_string(peOffset), reporter));
    }

    // ── COFF header ──
    std::uint64_t const coffOff = static_cast<std::uint64_t>(peOffset) + 4u;
    if (rangeExceedsBuffer(coffOff, kPeCoffHeaderSize, bytes.size())) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "PE reader: COFF header runs past EOF", reporter));
    }
    std::uint16_t const numberOfSections = readU16(bytes, coffOff + 2);
    std::uint16_t const sizeOfOptHeader  = readU16(bytes, coffOff + 16);

    // ── Optional header (PE32 / PE32+) ──
    std::uint64_t const optHeaderOff = coffOff + kPeCoffHeaderSize;
    if (rangeExceedsBuffer(optHeaderOff, sizeOfOptHeader, bytes.size())) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "PE reader: optional header (declared size "
            + std::to_string(sizeOfOptHeader) + ") runs past EOF", reporter));
    }
    if (sizeOfOptHeader < 2) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "PE reader: optional header too small to read magic "
            "(sizeOfOptHeader=" + std::to_string(sizeOfOptHeader) + ")",
            reporter));
    }
    std::uint16_t const optMagic = readU16(bytes, optHeaderOff + 0);
    bool isPe32Plus;
    if      (optMagic == kPeMagicPe32Plus) isPe32Plus = true;
    else if (optMagic == kPeMagicPe32)     isPe32Plus = false;
    else {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            std::format("PE reader: optional header magic 0x{:04X} "
                        "is neither PE32 (0x010B) nor PE32+ (0x020B)",
                        optMagic),
            reporter));
    }

    // DataDirectories[0] = Export Table. Per PE/COFF spec:
    //   PE32:  DataDirectory at optHeaderOff + 96
    //   PE32+: DataDirectory at optHeaderOff + 112
    // Each DataDirectory entry is (RVA u32, Size u32) — 8 bytes.
    std::uint64_t const exportDirOff =
        optHeaderOff + (isPe32Plus ? 112u : 96u);
    if (rangeExceedsBuffer(exportDirOff, 8, bytes.size())
     || exportDirOff + 8 > optHeaderOff + sizeOfOptHeader) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "PE reader: DataDirectories[0] (Export) runs past "
            "the optional-header range or EOF", reporter));
    }
    std::uint32_t const exportRva  = readU32(bytes, exportDirOff + 0);
    std::uint32_t const exportSize = readU32(bytes, exportDirOff + 4);
    if (exportRva == 0u || exportSize == 0u) {
        // No exports — valid for a non-DLL or import-only binary.
        // Return empty surface (success — caller decides whether
        // empty exports are acceptable for the consumer's purpose).
        return std::vector<ImportSurface>{};
    }

    // ── Section table → RVA→file-offset converter ──
    std::uint64_t const sectionTableOff =
        optHeaderOff + sizeOfOptHeader;
    std::uint64_t const sectionTableBytes =
        static_cast<std::uint64_t>(numberOfSections)
            * static_cast<std::uint64_t>(kPeSectionHdrSize);
    if (rangeExceedsBuffer(sectionTableOff, sectionTableBytes, bytes.size())) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "PE reader: section table (" + std::to_string(numberOfSections)
            + " sections × 40 bytes) runs past EOF", reporter));
    }
    auto const rvaToFileOff =
        [&](std::uint32_t rva) -> std::optional<std::uint64_t> {
        for (std::uint16_t i = 0; i < numberOfSections; ++i) {
            std::uint64_t const shdrOff =
                sectionTableOff + static_cast<std::uint64_t>(i) * kPeSectionHdrSize;
            // Section header layout (40 bytes):
            //   [ 0.. 7] Name (8 bytes)
            //   [ 8..11] VirtualSize
            //   [12..15] VirtualAddress (RVA)
            //   [16..19] SizeOfRawData
            //   [20..23] PointerToRawData (file offset)
            std::uint32_t const virtualSize    = readU32(bytes, shdrOff +  8);
            std::uint32_t const virtualAddress = readU32(bytes, shdrOff + 12);
            std::uint32_t const sizeOfRawData  = readU32(bytes, shdrOff + 16);
            std::uint32_t const ptrToRawData   = readU32(bytes, shdrOff + 20);
            // PE/COFF spec: VirtualSize may exceed SizeOfRawData
            // (zero-fill after raw); for RVA-to-file lookup we must
            // only resolve RVAs that lie within SizeOfRawData of the
            // section's raw data (otherwise the file offset is
            // meaningless). The export directory + names + name-ptrs
            // always live in raw data, so this is correct.
            std::uint32_t const cover = (sizeOfRawData < virtualSize)
                ? sizeOfRawData : virtualSize;
            if (rva >= virtualAddress
             && static_cast<std::uint64_t>(rva)
                  < static_cast<std::uint64_t>(virtualAddress) + cover) {
                return static_cast<std::uint64_t>(ptrToRawData)
                     + (rva - virtualAddress);
            }
        }
        return std::nullopt;
    };

    // ── Export directory ──
    auto const exportDirFileOff = rvaToFileOff(exportRva);
    if (!exportDirFileOff) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::SectionNotFound,
            "PE reader: export-directory RVA "
            + std::to_string(exportRva)
            + " does not lie in any section's raw data", reporter));
    }
    if (rangeExceedsBuffer(*exportDirFileOff, 40, bytes.size())) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "PE reader: export directory header runs past EOF "
            "(fileOff=" + std::to_string(*exportDirFileOff) + ")", reporter));
    }
    std::uint32_t const numberOfNamePointers = readU32(bytes, *exportDirFileOff + 24);
    std::uint32_t const addressOfNames       = readU32(bytes, *exportDirFileOff + 32);

    if (numberOfNamePointers == 0u) {
        // Export directory exists but has no NAMED exports — only
        // ordinal-only exports. v1 doesn't read ordinals → empty surface.
        return std::vector<ImportSurface>{};
    }
    auto const namesTableFileOff = rvaToFileOff(addressOfNames);
    if (!namesTableFileOff) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::SectionNotFound,
            "PE reader: AddressOfNames RVA " + std::to_string(addressOfNames)
            + " does not lie in any section's raw data", reporter));
    }
    // Multiplication overflow guard.
    std::uint64_t const namesTableBytes =
        static_cast<std::uint64_t>(numberOfNamePointers) * 4u;
    if (rangeExceedsBuffer(*namesTableFileOff, namesTableBytes, bytes.size())) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "PE reader: names table ("
            + std::to_string(numberOfNamePointers) + " × 4 bytes) "
            "runs past EOF", reporter));
    }

    // ── Walk names ──
    std::vector<ImportSurface> out;
    out.reserve(numberOfNamePointers);
    // D-FF1-PARTIAL-CORRUPTION-LOUD: count silent-skip cases so the
    // parse can emit a Warning summarizing how many entries were
    // dropped. Three skip reasons all indicate corruption (RVA
    // doesn't resolve, file offset OOB, NUL-string read empty);
    // collapsing them into one counter keeps the diagnostic compact
    // while still surfacing the partial-loss signal.
    std::uint32_t corruptedNameSkips = 0;
    for (std::uint32_t i = 0; i < numberOfNamePointers; ++i) {
        std::uint32_t const nameRva = readU32(bytes,
            *namesTableFileOff + static_cast<std::uint64_t>(i) * 4u);
        auto const nameFileOff = rvaToFileOff(nameRva);
        if (!nameFileOff) { ++corruptedNameSkips; continue; }
        if (*nameFileOff >= bytes.size()) { ++corruptedNameSkips; continue; }
        ImportSurface row;
        row.mangledName = readNulTerminated(
            bytes, /*tableStart=*/static_cast<std::size_t>(*nameFileOff),
            /*tableEnd=*/bytes.size(), /*index=*/0);
        if (row.mangledName.empty()) { ++corruptedNameSkips; continue; }
        row.libraryPath = std::string{libraryPathLabel};
        row.kind        = SymbolKind::Function;       // v1: all PE exports are Function/Default
        row.visibility  = SymbolVisibility::Default;
        row.linkage     = SymbolLinkage::External;
        out.push_back(std::move(row));
    }

    if (corruptedNameSkips > 0) {
        dss::report(reporter,
            DiagnosticCode::F_BinaryReaderPartialCorruption,
            DiagnosticSeverity::Warning,
            "PE reader: '" + std::string{libraryPathLabel}
            + "': skipped " + std::to_string(corruptedNameSkips)
            + " export-name entries with corrupted RVA / OOB file "
              "offset / empty-string reads (truncated .edata or "
              "out-of-section name pointers). Surfaced "
            + std::to_string(out.size())
            + " valid exports.");
    }

    return out;
}

} // namespace dss::ffi
