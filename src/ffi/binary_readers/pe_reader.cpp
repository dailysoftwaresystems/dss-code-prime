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
// (`.idata`) are deferred — the reader reads EXPORTS from a `.dll`.
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
//   [16..19] OrdinalBase        (see the ordinal note below)
//   [20..23] AddressTableEntries (count of Export Address Table entries)
//   [24..27] NumberOfNamePointers
//   [28..31] AddressOfFunctions   (RVA → the Export Address Table: u32 RVA × AddressTableEntries)
//   [32..35] AddressOfNames       (RVA → array of u32 RVAs to NUL-strings)
//   [36..39] AddressOfNameOrdinals (RVA → array of u16, one per name)
//
// ── c159 HARDENING (D-FF1-PE-READER production tier; closes
//    D-FF1-PE-OBJECT-EXPORTS) ──
// This reader is the exact INVERSE of the c152 `.edata` WRITER
// (`src/link/format/pe.cpp` (b4)). For each NAMED export at name-index
// `i` we resolve its ACTUAL address, not just its name:
//   unbiasedOrdinal = AddressOfNameOrdinals[i]   (u16; already the raw
//                     EAT index — the spec stores it MINUS OrdinalBase,
//                     so it indexes AddressOfFunctions directly)
//   eatRva          = AddressOfFunctions[unbiasedOrdinal]  (the address)
// then classify `eatRva` into a `SymbolKind` — the standard dumpbin /
// llvm-readobj approach, three closed cases:
//   (1) FORWARDER — eatRva lies INSIDE the export directory's own span
//       [exportRva, exportRva+exportSize). The RVA points at a
//       "DLL.Symbol" forward string (kernel32 HeapAlloc →
//       "NTDLL.RtlAllocateHeap"), NOT code. We read the string and
//       surface SymbolKind::Forwarder + forwardTarget. Checked FIRST:
//       the export directory itself lives in a data section, so a
//       forwarder RVA would otherwise misclassify as Object.
//   (2) FUNCTION — eatRva lands in a section whose Characteristics carry
//       IMAGE_SCN_MEM_EXECUTE (.text). Heuristic LIMIT: classification
//       is by section executability, the only signal the PE export
//       table carries (unlike ELF STT_FUNC/STT_OBJECT). A hand-crafted
//       binary that exports a function living in a non-standard
//       non-executable section, or data in an executable section, would
//       be classed by page protection — which is what the loader itself
//       sees, so this matches real consumer behavior (GetProcAddress
//       returns the same RVA regardless).
//   (3) OBJECT (data) — eatRva lands in any other mapped, non-executable
//       section (.rdata / .data / .bss). This is a PE DATA export
//       (`__declspec(dllexport)` on a global; msvcrt's `_iob` etc.).
//       (The shared SymbolKind enum spells the data kind `Object`, per
//       the ELF STT_OBJECT / Mach-O __data precedent — there is no
//       separate `Data` enumerator.)
// Fail-loud discipline (extends the v1's): the EAT and ordinal tables
// are bounds-checked as whole regions (a truncated .edata fails loud
// CorruptedBinary); a forwarder string that runs off EOF unterminated
// fails loud; individual per-name corruption (unresolvable name RVA, an
// ordinal ≥ AddressTableEntries, an EAT RVA in no section) is skipped +
// summarized via F_BinaryReaderPartialCorruption (the
// D-FF1-PARTIAL-CORRUPTION-LOUD discipline), never silently dropped.

constexpr std::size_t kPeDosHeaderSize  = 64;
constexpr std::size_t kPeCoffHeaderSize = 20;
constexpr std::size_t kPeSectionHdrSize = 40;
constexpr std::uint16_t kPeMagicPe32    = 0x010B;
constexpr std::uint16_t kPeMagicPe32Plus = 0x020B;
// IMAGE_SCN_MEM_EXECUTE — the section-Characteristics bit the loader
// maps as executable pages (PE/COFF §4.1). An export whose EAT RVA
// lands in a section carrying this bit is a Function; otherwise Object.
constexpr std::uint32_t kPeScnMemExecute = 0x20000000u;
// Section-header Characteristics field is the last u32 of the 40-byte
// IMAGE_SECTION_HEADER.
constexpr std::size_t kPeSectionCharsOff = 36;

// Read a NUL-terminated forwarder string starting at `fileOff`, bounded
// by the buffer. Unlike `readNulTerminated` (which stops at the buffer
// end and returns whatever it has), this REQUIRES the terminator to
// appear before EOF: a string that runs off the end unterminated is a
// truncated `.edata` (W4 cuts a real DLL mid-forwarder-string), so it
// returns std::nullopt for the caller to fail loud. An empty forwarder
// (a lone NUL) is legal-but-degenerate and returns "".
[[nodiscard]] std::optional<std::string>
readForwarderString(std::span<std::uint8_t const> bytes,
                    std::uint64_t                 fileOff) {
    if (fileOff >= bytes.size()) return std::nullopt;
    std::uint64_t end = fileOff;
    while (end < bytes.size() && bytes[end] != 0u) ++end;
    if (end >= bytes.size()) return std::nullopt;   // no NUL before EOF
    return std::string{
        reinterpret_cast<char const*>(&bytes[fileOff]),
        static_cast<std::size_t>(end - fileOff)};
}

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
            + " sections x 40 bytes) runs past EOF", reporter));
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

    // ── Section-membership classifier (c159) ──
    // Return the Characteristics of the section whose VIRTUAL range
    // contains `rva`, else nullopt. Deliberately distinct from
    // `rvaToFileOff` (which resolves within SizeOfRawData of RAW data):
    // an export's EAT RVA may point into a zero-fill data section (a
    // `.bss` data export has VirtualSize > 0 but SizeOfRawData == 0), so
    // classification must use the in-memory VIRTUAL footprint — the same
    // range the loader maps + protects. Uses max(VirtualSize,
    // SizeOfRawData) so both zero-fill (VirtualSize > raw) and
    // raw-padded (raw > VirtualSize) sections resolve.
    auto const sectionCharsForRva =
        [&](std::uint32_t rva) -> std::optional<std::uint32_t> {
        for (std::uint16_t i = 0; i < numberOfSections; ++i) {
            std::uint64_t const shdrOff =
                sectionTableOff + static_cast<std::uint64_t>(i) * kPeSectionHdrSize;
            std::uint32_t const virtualSize    = readU32(bytes, shdrOff +  8);
            std::uint32_t const virtualAddress = readU32(bytes, shdrOff + 12);
            std::uint32_t const sizeOfRawData  = readU32(bytes, shdrOff + 16);
            std::uint32_t const span = (virtualSize > sizeOfRawData)
                ? virtualSize : sizeOfRawData;
            if (rva >= virtualAddress
             && static_cast<std::uint64_t>(rva)
                  < static_cast<std::uint64_t>(virtualAddress) + span) {
                return readU32(bytes, shdrOff + kPeSectionCharsOff);
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
    std::uint32_t const addressTableEntries   = readU32(bytes, *exportDirFileOff + 20);
    std::uint32_t const numberOfNamePointers  = readU32(bytes, *exportDirFileOff + 24);
    std::uint32_t const addressOfFunctions    = readU32(bytes, *exportDirFileOff + 28);
    std::uint32_t const addressOfNames        = readU32(bytes, *exportDirFileOff + 32);
    std::uint32_t const addressOfNameOrdinals = readU32(bytes, *exportDirFileOff + 36);

    // D-FF1-READER-SONAME (c171): the export directory's Name field (offset 12)
    // is the DLL's OWN name (the import DllName a client's `.idata` records) --
    // its loader-resolvable identity, preferred over the file basename
    // downstream. OPTIONAL + NON-FATAL: a zero or unresolvable NameRva leaves
    // `dllName` empty (the driver falls back to the basename).
    std::string dllName;
    {
        std::uint32_t const nameRva = readU32(bytes, *exportDirFileOff + 12);
        if (nameRva != 0u) {
            if (auto const nameOff = rvaToFileOff(nameRva);
                nameOff && *nameOff < bytes.size()) {
                dllName = readNulTerminated(
                    bytes, static_cast<std::size_t>(*nameOff), bytes.size(), 0);
            }
        }
    }

    if (numberOfNamePointers == 0u) {
        // Export directory exists but has no NAMED exports — only
        // ordinal-only exports. The reader surfaces named exports →
        // empty surface (structurally valid; the v1 contract).
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
            + std::to_string(numberOfNamePointers) + " x 4 bytes) "
            "runs past EOF", reporter));
    }

    // ── EAT (AddressOfFunctions) — c159 ──
    // The Export Address Table: `addressTableEntries` u32 RVAs, one per
    // export ordinal. A named export resolves its ordinal through the
    // ordinal table below, then indexes here for its actual address.
    // Bounds-checked as a whole region (mirrors the names-table check) —
    // a truncated EAT fails loud (W4 mid-EAT cut).
    auto const eatFileOff = rvaToFileOff(addressOfFunctions);
    if (!eatFileOff) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::SectionNotFound,
            "PE reader: AddressOfFunctions (EAT) RVA "
            + std::to_string(addressOfFunctions)
            + " does not lie in any section's raw data", reporter));
    }
    std::uint64_t const eatBytes =
        static_cast<std::uint64_t>(addressTableEntries) * 4u;
    if (rangeExceedsBuffer(*eatFileOff, eatBytes, bytes.size())) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "PE reader: Export Address Table ("
            + std::to_string(addressTableEntries) + " x 4 bytes) "
            "runs past EOF", reporter));
    }

    // ── Ordinal table (AddressOfNameOrdinals) — c159 ──
    // One u16 per named export; entry i is the UNBIASED index into the
    // EAT for name i (the spec stores the ordinal minus OrdinalBase).
    auto const ordTableFileOff = rvaToFileOff(addressOfNameOrdinals);
    if (!ordTableFileOff) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::SectionNotFound,
            "PE reader: AddressOfNameOrdinals RVA "
            + std::to_string(addressOfNameOrdinals)
            + " does not lie in any section's raw data", reporter));
    }
    std::uint64_t const ordTableBytes =
        static_cast<std::uint64_t>(numberOfNamePointers) * 2u;
    if (rangeExceedsBuffer(*ordTableFileOff, ordTableBytes, bytes.size())) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "PE reader: name-ordinal table ("
            + std::to_string(numberOfNamePointers) + " x 2 bytes) "
            "runs past EOF", reporter));
    }

    // ── Walk names → resolve address → classify (c159) ──
    std::vector<ImportSurface> out;
    out.reserve(numberOfNamePointers);
    // D-FF1-PARTIAL-CORRUPTION-LOUD: count silent-skip cases so the
    // parse can emit a Warning summarizing how many entries were
    // dropped. Per-name corruption (name RVA doesn't resolve, file
    // offset OOB, empty-string read, an ordinal ≥ the EAT size, or an
    // EAT RVA that lands in no mapped section) is skipped + counted —
    // one compact Warning surfaces the partial loss. (Whole-region
    // truncation of the EAT / ordinal / forwarder-string reads fails
    // loud above / below instead — the structural-vs-per-entry split.)
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

        // name[i] → unbiased ordinal → EAT RVA (the export's address).
        std::uint16_t const unbiasedOrdinal = readU16(bytes,
            *ordTableFileOff + static_cast<std::uint64_t>(i) * 2u);
        if (unbiasedOrdinal >= addressTableEntries) {
            // The ordinal indexes past the EAT — an internal inconsistency
            // (not truncation: both counts come from the directory header).
            ++corruptedNameSkips;
            continue;
        }
        std::uint32_t const eatRva = readU32(bytes,
            *eatFileOff + static_cast<std::uint64_t>(unbiasedOrdinal) * 4u);

        row.libraryPath = std::string{libraryPathLabel};
        row.soname      = dllName;   // export-directory DllName (empty if none)
        row.visibility  = SymbolVisibility::Default;   // PE export table carries no STV_* granularity
        row.linkage     = SymbolLinkage::External;     // an export is external by definition

        // Classify. Forwarder FIRST: the export directory lives in a
        // (data) section, so an in-span RVA would otherwise misclassify
        // as Object.
        if (static_cast<std::uint64_t>(eatRva) >= exportRva
         && static_cast<std::uint64_t>(eatRva) < static_cast<std::uint64_t>(exportRva) + exportSize) {
            auto const fwdFileOff = rvaToFileOff(eatRva);
            if (!fwdFileOff) {
                return std::unexpected(emitAndReturn(
                    BinaryReadErrorKind::CorruptedBinary,
                    "PE reader: forwarder RVA " + std::to_string(eatRva)
                    + " (export '" + row.mangledName + "') lies in the "
                      "export-directory span but resolves to no section's "
                      "raw data", reporter));
            }
            auto const fwd = readForwarderString(bytes, *fwdFileOff);
            if (!fwd) {
                return std::unexpected(emitAndReturn(
                    BinaryReadErrorKind::CorruptedBinary,
                    "PE reader: forwarder string for export '"
                    + row.mangledName + "' runs off EOF unterminated "
                      "(truncated .edata)", reporter));
            }
            row.kind          = SymbolKind::Forwarder;
            row.forwardTarget = std::move(*fwd);
        } else if (auto const chars = sectionCharsForRva(eatRva)) {
            row.kind = (*chars & kPeScnMemExecute)
                ? SymbolKind::Function   // EAT RVA in an executable section
                : SymbolKind::Object;    // EAT RVA in a non-executable data section
        } else {
            // A non-forwarder EAT RVA in no mapped section — corrupt.
            ++corruptedNameSkips;
            continue;
        }
        out.push_back(std::move(row));
    }

    if (corruptedNameSkips > 0) {
        dss::report(reporter,
            DiagnosticCode::F_BinaryReaderPartialCorruption,
            DiagnosticSeverity::Warning,
            "PE reader: '" + std::string{libraryPathLabel}
            + "': skipped " + std::to_string(corruptedNameSkips)
            + " export-name entries with corrupted RVA / OOB file "
              "offset / empty-string reads / out-of-range ordinal / "
              "unmapped address (truncated .edata or out-of-section "
              "pointers). Surfaced "
            + std::to_string(out.size())
            + " valid exports.");
    }

    return out;
}

} // namespace dss::ffi
