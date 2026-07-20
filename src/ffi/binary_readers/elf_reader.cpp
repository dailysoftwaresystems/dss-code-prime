#include "ffi/binary_readers/elf_reader.hpp"

#include "core/cpp_invariants.hpp"  // arithmetic-right-shift static_assert
#include "core/types/parse_diagnostic.hpp"
#include "ffi/binary_readers/reader_common.hpp"

#include <limits>
#include <string>

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
constexpr std::size_t kElf64DynSz   = 16;  // Elf64_Dyn (d_tag u64 + d_val u64)

constexpr std::uint8_t kEiClass64   = 2;   // ELFCLASS64
constexpr std::uint8_t kEiData2LSB  = 1;   // ELFDATA2LSB

// SHT_*
constexpr std::uint32_t kShtDynSym  = 11;
constexpr std::uint32_t kShtStrtab  = 3;
constexpr std::uint32_t kShtNoBits  = 8;   // not stored on disk
constexpr std::uint32_t kShtDynamic = 6;   // SHT_DYNAMIC (.dynamic)

// DT_* dynamic-table tags (gABI Fig. 5-10) — the SONAME extractor's tags.
constexpr std::uint64_t kDtNull     = 0;   // DT_NULL — end of the .dynamic array
constexpr std::uint64_t kDtSoname   = 14;  // DT_SONAME (d_val = .dynstr offset)

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

} // namespace

std::expected<std::vector<ImportSurface>, BinaryReadError>
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

    // D-FF1-READER-SONAME (c171): extract DT_SONAME (the loader-resolvable
    // library identity) from `.dynamic` if present — a `.so` built with
    // `-Wl,-soname` (or gcc's implicit default) carries one, and preferring it
    // over the file basename downstream mirrors what a real linker records as
    // DT_NEEDED. OPTIONAL + NON-FATAL: a missing/corrupt `.dynamic` or an
    // absent DT_SONAME leaves `soname` empty (the driver falls back to the
    // basename). DT_SONAME's d_val is an offset into the SAME `.dynstr` the
    // symbol names index.
    std::string soname;
    for (std::uint16_t i = 0; i < e_shnum; ++i) {
        if (shtType(i) != kShtDynamic) continue;
        std::uint64_t const dynOff = shtOffset(i);
        std::uint64_t const dynSz  = shtSize(i);
        if (rangeExceedsBuffer(dynOff, dynSz, bytes.size())) break;  // corrupt — skip, not fatal
        for (std::uint64_t e = 0; e + kElf64DynSz <= dynSz; e += kElf64DynSz) {
            std::uint64_t const tag =
                readU64(bytes, static_cast<std::size_t>(dynOff + e));
            if (tag == kDtNull) break;   // DT_NULL terminates the dynamic array
            if (tag == kDtSoname) {
                std::uint64_t const nameOff =
                    readU64(bytes, static_cast<std::size_t>(dynOff + e + 8));
                soname = readNulTerminated(
                    bytes, static_cast<std::size_t>(dynstrOff),
                    static_cast<std::size_t>(dynstrOff + dynstrSize), nameOff);
                break;
            }
        }
        break;   // one `.dynamic` section per image
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
        row.soname      = soname;   // DT_SONAME (empty if the .so declares none)
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

} // namespace dss::ffi
