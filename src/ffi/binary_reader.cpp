#include "ffi/binary_reader.hpp"

#include "core/types/parse_diagnostic.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <string_view>

namespace dss::ffi {

namespace {

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
          DiagnosticReporter&) {
    if (bytes.size() < kElf64EhdrSz) {
        return std::unexpected(BinaryReadError{
            BinaryReadErrorKind::CorruptedBinary,
            "ELF64 reader: file is shorter than Elf64_Ehdr (64 bytes)"});
    }
    // EI_CLASS at [4], EI_DATA at [5].
    if (bytes[4] != kEiClass64) {
        return std::unexpected(BinaryReadError{
            BinaryReadErrorKind::UnsupportedElfClass,
            "ELF reader: file is not ELFCLASS64 (EI_CLASS="
            + std::to_string(bytes[4]) + "); v1 supports 64-bit only"});
    }
    if (bytes[5] != kEiData2LSB) {
        return std::unexpected(BinaryReadError{
            BinaryReadErrorKind::UnsupportedElfClass,
            "ELF reader: file is not ELFDATA2LSB (EI_DATA="
            + std::to_string(bytes[5]) + "); v1 supports little-endian only"});
    }

    std::uint64_t const e_shoff     = readU64(bytes, 40);
    std::uint16_t const e_shentsize = readU16(bytes, 58);
    std::uint16_t const e_shnum     = readU16(bytes, 60);
    std::uint16_t const e_shstrndx  = readU16(bytes, 62);

    if (e_shentsize != kElf64ShdrSz) {
        return std::unexpected(BinaryReadError{
            BinaryReadErrorKind::CorruptedBinary,
            "ELF64 reader: e_shentsize=" + std::to_string(e_shentsize)
            + " (expected 64)"});
    }
    if (e_shoff == 0u || e_shnum == 0u) {
        return std::unexpected(BinaryReadError{
            BinaryReadErrorKind::CorruptedBinary,
            "ELF64 reader: no section header table (stripped binary?)"});
    }
    std::uint64_t const shtEnd = e_shoff
        + static_cast<std::uint64_t>(e_shnum) * e_shentsize;
    if (shtEnd > bytes.size()) {
        return std::unexpected(BinaryReadError{
            BinaryReadErrorKind::CorruptedBinary,
            "ELF64 reader: section header table runs past EOF"});
    }

    // Locate the shstrtab (section-name string table) so we can find
    // `.dynsym` + `.dynstr` by name.
    if (e_shstrndx >= e_shnum) {
        return std::unexpected(BinaryReadError{
            BinaryReadErrorKind::CorruptedBinary,
            "ELF64 reader: e_shstrndx out of range"});
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
    if (shstrtabOff + shstrtabSize > bytes.size()) {
        return std::unexpected(BinaryReadError{
            BinaryReadErrorKind::CorruptedBinary,
            "ELF64 reader: shstrtab runs past EOF"});
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
        return std::unexpected(BinaryReadError{
            BinaryReadErrorKind::SectionNotFound,
            "ELF64 reader: no `.dynsym` section found (stripped or static "
            "library?)"});
    }

    std::uint32_t const dynstrIdx = shtLink(dynsymIdx);
    if (dynstrIdx == 0u || dynstrIdx >= e_shnum) {
        return std::unexpected(BinaryReadError{
            BinaryReadErrorKind::CorruptedBinary,
            "ELF64 reader: .dynsym's sh_link does not point at a valid "
            ".dynstr"});
    }
    if (shtType(static_cast<std::uint16_t>(dynstrIdx)) != kShtStrtab) {
        return std::unexpected(BinaryReadError{
            BinaryReadErrorKind::CorruptedBinary,
            "ELF64 reader: section linked from .dynsym is not SHT_STRTAB"});
    }

    std::uint64_t const dynsymOff  = shtOffset(dynsymIdx);
    std::uint64_t const dynsymSize = shtSize(dynsymIdx);
    std::uint64_t const dynsymEnt  = shtEntsize(dynsymIdx);
    if (dynsymEnt != kElf64SymSz) {
        return std::unexpected(BinaryReadError{
            BinaryReadErrorKind::CorruptedBinary,
            "ELF64 reader: .dynsym sh_entsize=" + std::to_string(dynsymEnt)
            + " (expected 24)"});
    }
    if (dynsymOff + dynsymSize > bytes.size()) {
        return std::unexpected(BinaryReadError{
            BinaryReadErrorKind::CorruptedBinary,
            "ELF64 reader: .dynsym runs past EOF"});
    }

    std::uint64_t const dynstrOff  = shtOffset(static_cast<std::uint16_t>(dynstrIdx));
    std::uint64_t const dynstrSize = shtSize(static_cast<std::uint16_t>(dynstrIdx));
    if (dynstrOff + dynstrSize > bytes.size()) {
        return std::unexpected(BinaryReadError{
            BinaryReadErrorKind::CorruptedBinary,
            "ELF64 reader: .dynstr runs past EOF"});
    }

    // Iterate dynsym entries. Slot 0 is STN_UNDEF — skip.
    std::vector<ImportSurface> out;
    std::size_t const numSyms = static_cast<std::size_t>(dynsymSize / kElf64SymSz);
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

        if (st_name == 0u) continue;  // unnamed entries (section syms, etc.)
        if (stBind(st_info) == kStbLocal) continue;  // locals don't export

        ImportSurface row;
        row.mangledName = readNulTerminated(bytes,
                                             static_cast<std::size_t>(dynstrOff),
                                             static_cast<std::size_t>(dynstrOff + dynstrSize),
                                             st_name);
        if (row.mangledName.empty()) continue;  // corrupted name index
        row.libraryPath = std::string{libraryPathLabel};
        row.kind        = elfSttToKind(stType(st_info));
        row.visibility  = elfStvToVisibility(stVisibility(st_other));
        row.linkage     = elfStbToLinkage(stBind(st_info));
        out.push_back(std::move(row));
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

std::expected<std::vector<ImportSurface>, BinaryReadError>
readImportsFromBytes(std::span<std::uint8_t const> bytes,
                     std::string_view              libraryPathLabel,
                     DiagnosticReporter&           reporter) {
    if (bytes.empty()) {
        return std::unexpected(BinaryReadError{
            BinaryReadErrorKind::FileEmpty,
            std::string{"readImports: '"} + std::string{libraryPathLabel}
            + "' is zero bytes (truncated download / build artifact?)"});
    }
    auto const guess = guessFormat(bytes);
    switch (guess) {
        case FormatGuess::Elf:
            return readElf64(bytes, libraryPathLabel, reporter);
        case FormatGuess::Pe:
            return std::unexpected(BinaryReadError{
                BinaryReadErrorKind::UnsupportedFormat,
                std::string{"readImports: '"} + std::string{libraryPathLabel}
                + "' is a PE binary (MZ magic). PE export-table reader "
                  "is anchored at plan 11 §3 FF1-PE (not yet shipped)."});
        case FormatGuess::MachO:
            return std::unexpected(BinaryReadError{
                BinaryReadErrorKind::UnsupportedFormat,
                std::string{"readImports: '"} + std::string{libraryPathLabel}
                + "' is a Mach-O binary. Mach-O LC_SYMTAB reader is "
                  "anchored at plan 11 §3 FF1-MachO (not yet shipped)."});
        case FormatGuess::Unknown:
            break;
    }
    return std::unexpected(BinaryReadError{
        BinaryReadErrorKind::UnknownFormat,
        std::string{"readImports: '"} + std::string{libraryPathLabel}
        + "' has no recognised magic (expected ELF '\\x7FELF', PE 'MZ', "
          "or Mach-O 0xFEEDFACF/0xCAFEBABE)."});
}

std::expected<std::vector<ImportSurface>, BinaryReadError>
readImports(std::filesystem::path const& libraryPath,
            DiagnosticReporter&          reporter) {
    std::ifstream in(libraryPath, std::ios::binary);
    if (!in) {
        return std::unexpected(BinaryReadError{
            BinaryReadErrorKind::FileOpenFailed,
            "readImports: failed to open '"
            + libraryPath.generic_string() + "' for reading"});
    }
    std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>()};
    return readImportsFromBytes(
        std::span<std::uint8_t const>{bytes.data(), bytes.size()},
        libraryPath.generic_string(), reporter);
}

} // namespace dss::ffi
