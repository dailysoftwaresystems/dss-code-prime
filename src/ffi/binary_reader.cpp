#include "ffi/binary_reader.hpp"

#include "core/cpp_invariants.hpp"  // arithmetic-right-shift static_assert
#include "core/types/parse_diagnostic.hpp"

#include <array>
#include <cstring>
#include <format>
#include <fstream>
#include <optional>
#include <string_view>

namespace dss::ffi {

// ── Reporter wiring (file-scope so both anon-namespace per-format
//    readers AND the file-scope public entry points can use it) ──

// Map an `BinaryReadErrorKind` to the structured `DiagnosticCode::F_*`
// value (plan 11 §2.6). The kind enum is the function-return shape
// (compact, semantic); the F_* code is the in-reporter shape that
// downstream diagnostic policy (`--suppress`, `--warnings-as-errors`)
// consumes. Wired at post-fold #1 — was previously a SoT gap.
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
    // Unreachable per the closed enum; if a new `BinaryReadErrorKind`
    // variant lands without updating this switch, emit
    // `F_CorruptedBinary` as a fail-loud (rather than `None` which
    // would silently produce an uncoded diagnostic that `--suppress`
    // cannot target). post-fold #2 silent-failure Q2 fix.
    return DiagnosticCode::F_CorruptedBinary;
}

static_assert(static_cast<std::uint8_t>(BinaryReadErrorKind::SectionNotFound) == 6u,
              "BinaryReadErrorKind grew without updating "
              "toDiagnosticCode — add a switch arm for the new variant.");

// Emit a binary-reader failure through the run-wide DiagnosticReporter
// AND return the structured BinaryReadError. Centralises the kind →
// F_* code mapping so every failure path produces a remediation-
// distinct diagnostic that downstream policy consumes. Lives at
// file-scope dss::ffi (NOT inside the anon namespace) so per-format
// readers (in the anon namespace) AND public entry points (at
// file-scope) all see the same single source of truth.
// (post-fold #2 fix — was inside anon-namespace, forcing readImports
// to inline-duplicate the body.)
[[nodiscard]] inline BinaryReadError
emitAndReturn(BinaryReadErrorKind kind, std::string detail,
              DiagnosticReporter& reporter) {
    dss::report(reporter, toDiagnosticCode(kind),
                DiagnosticSeverity::Error, detail);
    return BinaryReadError{kind, std::move(detail)};
}

namespace {

// (`rangeExceedsBuffer` hoisted to the public header at post-fold #2
// so tests can pin the u64-wrap-bypass case directly. The
// implementation lives in `binary_reader.hpp` as `constexpr inline`.)

// (The arithmetic-right-shift static_assert was hoisted to
// `core/cpp_invariants.hpp` at post-fold #2 — the per-TU assert
// here did NOT cover `elf.cpp::emitArm64PltStub` or
// `exec_reloc_apply.hpp::Aarch64AdrPrelPgHi21`, both of which
// rely on the same guarantee. The hoisted header is now
// `#include`d by every consumer.)

// ── ELF64 layout constants (gABI 4.10) ──────────────────────────
// Identical layout to what `src/link/format/elf.cpp` already emits.
// We re-declare the constants here rather than #include-pull from
// the linker tier — keeps `src/ffi/` free of upward-dependency on
// `src/link/`.

constexpr std::size_t kElfIdent     = 16;
constexpr std::size_t kElf64EhdrSz  = 64;
constexpr std::size_t kElf64ShdrSz  = 64;
constexpr std::size_t kElf64SymSz   = 24;

constexpr std::uint8_t kEiClass64   = 2;   // ELFCLASS64
constexpr std::uint8_t kEiData2LSB  = 1;   // ELFDATA2LSB

// SHT_*
constexpr std::uint32_t kShtDynSym  = 11;
constexpr std::uint32_t kShtStrtab  = 3;
constexpr std::uint32_t kShtNoBits  = 8;   // not stored on disk

// ELF symbol fields decode (gABI 4.31)
[[nodiscard]] constexpr std::uint8_t stBind(std::uint8_t info) noexcept { return info >> 4; }
[[nodiscard]] constexpr std::uint8_t stType(std::uint8_t info) noexcept { return info & 0xFu; }
[[nodiscard]] constexpr std::uint8_t stVisibility(std::uint8_t other) noexcept { return other & 0x3u; }

// STB_*
constexpr std::uint8_t kStbLocal    = 0;
constexpr std::uint8_t kStbGlobal   = 1;
constexpr std::uint8_t kStbWeak     = 2;
// STT_*
constexpr std::uint8_t kSttNoType   = 0;
constexpr std::uint8_t kSttObject   = 1;
constexpr std::uint8_t kSttFunc     = 2;
constexpr std::uint8_t kSttTls      = 6;
// STV_*
constexpr std::uint8_t kStvDefault  = 0;
constexpr std::uint8_t kStvInternal = 1;
constexpr std::uint8_t kStvHidden   = 2;
constexpr std::uint8_t kStvProtected= 3;

// ── Little-endian byte readers ──────────────────────────────────

[[nodiscard]] std::uint16_t readU16(std::span<std::uint8_t const> b, std::size_t off) noexcept {
    return  static_cast<std::uint16_t>(b[off + 0])
         | (static_cast<std::uint16_t>(b[off + 1]) << 8);
}
[[nodiscard]] std::uint32_t readU32(std::span<std::uint8_t const> b, std::size_t off) noexcept {
    return  static_cast<std::uint32_t>(b[off + 0])
         | (static_cast<std::uint32_t>(b[off + 1]) <<  8)
         | (static_cast<std::uint32_t>(b[off + 2]) << 16)
         | (static_cast<std::uint32_t>(b[off + 3]) << 24);
}
[[nodiscard]] std::uint64_t readU64(std::span<std::uint8_t const> b, std::size_t off) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(b[off + i]) << (i * 8);
    }
    return v;
}

// Read a NUL-terminated C string from a string table at `index`,
// bounded by the table's size. Returns empty string on out-of-range
// (caller-side check; we never read past `tableEnd`).
[[nodiscard]] std::string readNulTerminated(
        std::span<std::uint8_t const> bytes,
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
    Unknown = 0,
    Elf     = 1,
    Pe      = 2,
    MachO   = 3,
};

[[nodiscard]] FormatGuess guessFormat(std::span<std::uint8_t const> b) noexcept {
    if (b.size() >= 4
     && b[0] == 0x7Fu && b[1] == 'E' && b[2] == 'L' && b[3] == 'F') {
        return FormatGuess::Elf;
    }
    if (b.size() >= 2 && b[0] == 'M' && b[1] == 'Z') {
        return FormatGuess::Pe;
    }
    if (b.size() >= 4) {
        std::uint32_t const magic = readU32(b, 0);
        if (magic == 0xFEEDFACFu || magic == 0xCAFEBABEu) {
            return FormatGuess::MachO;
        }
    }
    return FormatGuess::Unknown;
}

// ── ELF64 LE reader ─────────────────────────────────────────────

[[nodiscard]] SymbolKind elfSttToKind(std::uint8_t t) noexcept {
    switch (t) {
        case kSttFunc:   return SymbolKind::Function;
        case kSttObject: return SymbolKind::Object;
        case kSttTls:    return SymbolKind::Tls;
        default:         return SymbolKind::NoType;
    }
}
[[nodiscard]] SymbolVisibility elfStvToVisibility(std::uint8_t v) noexcept {
    switch (v) {
        case kStvHidden:    return SymbolVisibility::Hidden;
        case kStvProtected: return SymbolVisibility::Protected;
        case kStvInternal:  return SymbolVisibility::Internal;
        default:            return SymbolVisibility::Default;
    }
}
[[nodiscard]] SymbolLinkage elfStbToLinkage(std::uint8_t b) noexcept {
    switch (b) {
        case kStbWeak:  return SymbolLinkage::Weak;
        case kStbLocal: return SymbolLinkage::Local;
        default:        return SymbolLinkage::External;
    }
}

// Read `.dynsym` + `.dynstr` from an ELF64 LE binary; emit one
// `ImportSurface` row per non-empty-name dynamic symbol.
//
// The dynsym's slot 0 is the STN_UNDEF sentinel (all zeros); we
// skip it. Symbols with empty names (st_name = 0) are also skipped
// — those are typically section symbols or local entries that
// shouldn't surface as imports.
[[nodiscard]] std::expected<std::vector<ImportSurface>, BinaryReadError>
readElf64(std::span<std::uint8_t const> bytes,
          std::string_view              libraryPathLabel,
          DiagnosticReporter&           reporter) {
    if (bytes.size() < kElf64EhdrSz) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "ELF64 reader: file is shorter than Elf64_Ehdr (64 bytes)", reporter));
    }
    // EI_CLASS at [4], EI_DATA at [5].
    if (bytes[4] != kEiClass64) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::UnsupportedElfClass,
            "ELF reader: file is not ELFCLASS64 (EI_CLASS="
            + std::to_string(bytes[4]) + "); v1 supports 64-bit only", reporter));
    }
    if (bytes[5] != kEiData2LSB) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::UnsupportedElfClass,
            "ELF reader: file is not ELFDATA2LSB (EI_DATA="
            + std::to_string(bytes[5]) + "); v1 supports little-endian only", reporter));
    }

    std::uint64_t const e_shoff     = readU64(bytes, 40);
    std::uint16_t const e_shentsize = readU16(bytes, 58);
    std::uint16_t const e_shnum     = readU16(bytes, 60);
    std::uint16_t const e_shstrndx  = readU16(bytes, 62);

    if (e_shentsize != kElf64ShdrSz) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "ELF64 reader: e_shentsize=" + std::to_string(e_shentsize)
            + " (expected 64)", reporter));
    }
    if (e_shoff == 0u || e_shnum == 0u) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "ELF64 reader: no section header table (stripped binary?)", reporter));
    }
    // Multiplication overflow guard. `e_shnum` is u16 (≤65535) and
    // `e_shentsize` is u16 (always 64 here); their product fits u32
    // comfortably (max 4.2 MB). Promote both to u64 explicitly so
    // the multiplication itself doesn't overflow on any platform.
    std::uint64_t const shtBytes =
        static_cast<std::uint64_t>(e_shnum) * static_cast<std::uint64_t>(e_shentsize);
    if (rangeExceedsBuffer(e_shoff, shtBytes, bytes.size())) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "ELF64 reader: section header table runs past EOF "
            "(e_shoff=" + std::to_string(e_shoff)
            + " + " + std::to_string(shtBytes) + " bytes > file "
            + std::to_string(bytes.size()) + ")", reporter));
    }

    // Locate the shstrtab (section-name string table) so we can find
    // `.dynsym` + `.dynstr` by name.
    if (e_shstrndx >= e_shnum) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "ELF64 reader: e_shstrndx out of range", reporter));
    }
    auto const sectionHeaderAt = [&](std::uint16_t idx) -> std::size_t {
        return static_cast<std::size_t>(e_shoff) + idx * kElf64ShdrSz;
    };
    auto const shtName    = [&](std::uint16_t idx) { return readU32(bytes, sectionHeaderAt(idx) +  0); };
    auto const shtType    = [&](std::uint16_t idx) { return readU32(bytes, sectionHeaderAt(idx) +  4); };
    auto const shtOffset  = [&](std::uint16_t idx) { return readU64(bytes, sectionHeaderAt(idx) + 24); };
    auto const shtSize    = [&](std::uint16_t idx) { return readU64(bytes, sectionHeaderAt(idx) + 32); };
    auto const shtLink    = [&](std::uint16_t idx) { return readU32(bytes, sectionHeaderAt(idx) + 40); };
    auto const shtEntsize = [&](std::uint16_t idx) { return readU64(bytes, sectionHeaderAt(idx) + 56); };

    std::uint64_t const shstrtabOff  = shtOffset(e_shstrndx);
    std::uint64_t const shstrtabSize = shtSize(e_shstrndx);
    if (rangeExceedsBuffer(shstrtabOff, shstrtabSize, bytes.size())) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "ELF64 reader: shstrtab runs past EOF", reporter));
    }
    auto const shtNameStr = [&](std::uint16_t idx) -> std::string {
        return readNulTerminated(bytes,
                                  static_cast<std::size_t>(shstrtabOff),
                                  static_cast<std::size_t>(shstrtabOff + shstrtabSize),
                                  shtName(idx));
    };

    // Find `.dynsym` (and its linked `.dynstr` via sh_link).
    std::uint16_t dynsymIdx = std::numeric_limits<std::uint16_t>::max();
    for (std::uint16_t i = 0; i < e_shnum; ++i) {
        if (shtType(i) == kShtDynSym && shtNameStr(i) == ".dynsym") {
            dynsymIdx = i;
            break;
        }
    }
    if (dynsymIdx == std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::SectionNotFound,
            "ELF64 reader: no `.dynsym` section found (stripped or static "
            "library?)", reporter));
    }

    std::uint32_t const dynstrIdx = shtLink(dynsymIdx);
    if (dynstrIdx == 0u || dynstrIdx >= e_shnum) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "ELF64 reader: .dynsym's sh_link does not point at a valid "
            ".dynstr", reporter));
    }
    if (shtType(static_cast<std::uint16_t>(dynstrIdx)) != kShtStrtab) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "ELF64 reader: section linked from .dynsym is not SHT_STRTAB", reporter));
    }

    std::uint64_t const dynsymOff  = shtOffset(dynsymIdx);
    std::uint64_t const dynsymSize = shtSize(dynsymIdx);
    std::uint64_t const dynsymEnt  = shtEntsize(dynsymIdx);
    if (dynsymEnt != kElf64SymSz) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "ELF64 reader: .dynsym sh_entsize=" + std::to_string(dynsymEnt)
            + " (expected 24)", reporter));
    }
    if (rangeExceedsBuffer(dynsymOff, dynsymSize, bytes.size())) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "ELF64 reader: .dynsym runs past EOF", reporter));
    }
    // Section size must be a multiple of the entry size — a partial
    // tail entry would be silently truncated by `numSyms = size / 24`.
    if ((dynsymSize % kElf64SymSz) != 0u) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "ELF64 reader: .dynsym size=" + std::to_string(dynsymSize)
            + " is not a multiple of sh_entsize=" + std::to_string(kElf64SymSz)
            + " — corrupted (truncated final entry)", reporter));
    }

    std::uint64_t const dynstrOff  = shtOffset(static_cast<std::uint16_t>(dynstrIdx));
    std::uint64_t const dynstrSize = shtSize(static_cast<std::uint16_t>(dynstrIdx));
    if (rangeExceedsBuffer(dynstrOff, dynstrSize, bytes.size())) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "ELF64 reader: .dynstr runs past EOF", reporter));
    }

    // Iterate dynsym entries. Slot 0 is STN_UNDEF — skip.
    std::vector<ImportSurface> out;
    std::size_t const numSyms = static_cast<std::size_t>(dynsymSize / kElf64SymSz);
    // D-FF1-PARTIAL-CORRUPTION-LOUD: count entries that fail the
    // empty-name post-read check (the only PARTIAL-corruption arm
    // — the unnamed/local skips are structural filters by design,
    // not corruption). Counter is emitted as a Warning at end-of-
    // parse so operators can investigate library integrity without
    // aborting the parse (the surviving rows are still useful).
    std::uint32_t corruptedNameSkips = 0;
    for (std::size_t i = 1; i < numSyms; ++i) {
        std::size_t const symOff = static_cast<std::size_t>(dynsymOff)
                                  + i * kElf64SymSz;
        std::uint32_t const st_name  = readU32(bytes, symOff +  0);
        std::uint8_t  const st_info  = bytes[symOff +  4];
        std::uint8_t  const st_other = bytes[symOff +  5];
        // st_shndx, st_value, st_size — unused for import-surface
        // reporting (we only care about NAME + KIND + VISIBILITY +
        // LINKAGE for the FF1 surface; the symbol's runtime VA is
        // dyld's concern, not ours).

        if (st_name == 0u) continue;  // unnamed entries (section syms, etc.) — by-design
        if (stBind(st_info) == kStbLocal) continue;  // locals don't export — by-design

        ImportSurface row;
        row.mangledName = readNulTerminated(bytes,
                                             static_cast<std::size_t>(dynstrOff),
                                             static_cast<std::size_t>(dynstrOff + dynstrSize),
                                             st_name);
        if (row.mangledName.empty()) {
            // st_name was non-zero, so the entry CLAIMS to be named,
            // but the string-table read returned empty — corruption.
            ++corruptedNameSkips;
            continue;
        }
        row.libraryPath = std::string{libraryPathLabel};
        row.kind        = elfSttToKind(stType(st_info));
        row.visibility  = elfStvToVisibility(stVisibility(st_other));
        row.linkage     = elfStbToLinkage(stBind(st_info));
        out.push_back(std::move(row));
    }

    if (corruptedNameSkips > 0) {
        dss::report(reporter,
            DiagnosticCode::F_BinaryReaderPartialCorruption,
            DiagnosticSeverity::Warning,
            "ELF64 reader: '" + std::string{libraryPathLabel}
            + "': skipped " + std::to_string(corruptedNameSkips)
            + " .dynsym entries with corrupted name indices (non-zero "
              "st_name resolved to empty string — possibly truncated "
              ".dynstr or out-of-bounds name offset). Surfaced "
            + std::to_string(out.size())
            + " valid symbols.");
    }

    return out;
}

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

[[nodiscard]] std::expected<std::vector<ImportSurface>, BinaryReadError>
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

} // namespace

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

// (`emitAndReturn` hoisted to file-scope BEFORE the anon namespace
// at post-fold #2 — the per-format readers in the anon namespace
// + the public entry points below share a single source of truth.)

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
        case FormatGuess::MachO:
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::UnsupportedFormat,
                std::string{"readImports: '"} + std::string{libraryPathLabel}
                + "' is a Mach-O binary. Mach-O LC_SYMTAB reader is "
                  "anchored at plan 11 §3 FF1-MachO (not yet shipped).",
                reporter));
        case FormatGuess::Unknown:
            break;
    }
    return std::unexpected(emitAndReturn(
        BinaryReadErrorKind::UnknownFormat,
        std::string{"readImports: '"} + std::string{libraryPathLabel}
        + "' has no recognised magic (expected ELF '\\x7FELF', PE 'MZ', "
          "or Mach-O 0xFEEDFACF/0xCAFEBABE).",
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
