#include "ffi/binary_readers/elf_reader.hpp"

#include "core/cpp_invariants.hpp"  // arithmetic-right-shift static_assert
#include "core/types/parse_diagnostic.hpp"
#include "ffi/binary_readers/reader_common.hpp"

#include <limits>
#include <string>
#include <utility>
#include <vector>

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

// ── Symbol versioning (gABI + the Sun/GNU version extension) ─────
// D-FFI-BINARY-READER-SURFACES-NO-SYMBOL-VERSION (TF-C124). The two
// sections that answer "at what version does this library EXPORT this
// name": `.gnu.version` is one u16 per `.dynsym` row, and for a DEFINED
// row that index selects a `.gnu.version_d` entry. `.gnu.version_r`
// (SHT_GNU_verneed, 0x6ffffffe) is deliberately NOT read here — it
// describes the versions this library REQUIRES OF OTHERS, attached to
// its SHN_UNDEF rows, which are exactly the rows this reader filters out
// as not-exports.
constexpr std::uint32_t kShtGnuVersym = 0x6fffffff;  // SHT_GNU_versym
constexpr std::uint32_t kShtGnuVerdef = 0x6ffffffd;  // SHT_GNU_verdef

constexpr std::size_t kElf64VerdefSz  = 20;  // Elf64_Verdef
constexpr std::size_t kElf64VerdauxSz = 8;   // Elf64_Verdaux
constexpr std::size_t kVersymEntSz    = 2;   // one u16 per .dynsym row

// `.gnu.version` slot decode. Bit 15 is VERSYM_HIDDEN — SET means this
// definition is a NON-default (`sym@VER`) compat instance; clear means it
// is the default (`sym@@VER`). The low 15 bits are the version index:
// 0 = VER_NDX_LOCAL, 1 = VER_NDX_GLOBAL (both mean "no version" for an
// export — index 1 names the verdef BASE entry, which is the FILE's own
// name, not a version anyone can request), >= 2 selects a real entry.
constexpr std::uint16_t kVersymHidden   = 0x8000;
constexpr std::uint16_t kVersymIdxMask  = 0x7fff;
constexpr std::uint16_t kVerNdxGlobal   = 1;

// Elf64_Verdef.vd_flags — VER_FLG_BASE marks the entry that names the
// FILE itself rather than a requestable version.
constexpr std::uint16_t kVerFlgBase = 0x1;

// DT_* dynamic-table tags (gABI Fig. 5-10) — the SONAME extractor's tags.
constexpr std::uint64_t kDtNull     = 0;   // DT_NULL — end of the .dynamic array
constexpr std::uint64_t kDtSoname   = 14;  // DT_SONAME (d_val = .dynstr offset)

// ELF symbol fields decode (gABI 4.31)
[[nodiscard]] constexpr std::uint8_t stBind(std::uint8_t info) noexcept { return info >> 4; }
[[nodiscard]] constexpr std::uint8_t stType(std::uint8_t info) noexcept { return info & 0xFu; }
[[nodiscard]] constexpr std::uint8_t stVisibility(std::uint8_t other) noexcept { return other & 0x3u; }

// SHN_* special section indices (gABI 4.6). SHN_UNDEF is the one this
// reader must filter on: an `st_shndx` of 0 means the entry is a
// REFERENCE the library makes, not a definition it offers.
constexpr std::uint16_t kShnUndef   = 0;

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
    auto const shtInfo    = [&](std::uint16_t idx) { return readU32(bytes, sectionHeaderAt(idx) + 44); };
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

    // ── D-FFI-BINARY-READER-SURFACES-NO-SYMBOL-VERSION (TF-C124) ─────
    // Per-symbol EXPORT versions: `.gnu.version` (one u16 per `.dynsym`
    // row) resolved through `.gnu.version_d` (the versions this library
    // DEFINES). Without this every `--resolve-library` import was
    // unversioned no matter what the library recorded, so only a shipped
    // DESCRIPTOR could ever pin one — and no third-party library this
    // project acquires (Tcl, zlib) has a descriptor.
    //
    // OPTIONAL + NON-FATAL, exactly like DT_SONAME above: a library with
    // no version script has neither section, and every row then carries
    // `elfSymbolVersion == nullopt`, which is the truth about it. Only a
    // versym index that resolves to NO verdef entry is an anomaly, and
    // that is counted and reported as partial corruption at end-of-parse.
    bool          haveVersym = false;
    std::uint64_t versymOff  = 0;
    std::uint64_t versymCnt  = 0;
    // A `.gnu.version` section we REFUSED to decode. Distinct from "the
    // library has none": an absent section is the truth about an unversioned
    // library, whereas a malformed one means version information EXISTS and
    // we are dropping it. Dropping it silently would downgrade every export
    // to unversioned with nothing anywhere to say so — the same shape as the
    // misbind D-LK-ELF-SYMBOL-VERSIONING exists to prevent.
    std::string versymRejectReason;
    // (index, name) for every REQUESTABLE version this library defines.
    // A handful of entries even for glibc (~40), so a linear scan beats
    // dragging a hash container into the FF1 tier.
    std::vector<std::pair<std::uint16_t, std::string>> verdefs;
    bool haveVerdef = false;
    for (std::uint16_t i = 0; i < e_shnum; ++i) {
        std::uint32_t const ty = shtType(i);
        if (ty == kShtGnuVersym && !haveVersym) {
            std::uint64_t const off = shtOffset(i);
            std::uint64_t const sz  = shtSize(i);
            // A versym section whose entsize is not 2 is not the table the
            // gABI describes; refusing it (rather than dividing by the
            // claimed size) keeps a malformed image from producing
            // confidently-wrong version strings. Refusing is not the same as
            // staying quiet about it — see `versymRejectReason`.
            if (shtEntsize(i) != kVersymEntSz) {
                versymRejectReason = "sh_entsize=" + std::to_string(shtEntsize(i))
                                   + " (the gABI fixes it at 2)";
                continue;
            }
            if (rangeExceedsBuffer(off, sz, bytes.size())) {
                versymRejectReason = "the section runs past EOF (sh_offset="
                                   + std::to_string(off) + " + "
                                   + std::to_string(sz) + " > file "
                                   + std::to_string(bytes.size()) + ")";
                continue;
            }
            versymRejectReason.clear();
            versymOff  = off;
            versymCnt  = sz / kVersymEntSz;
            haveVersym = true;
            continue;
        }
        if (ty != kShtGnuVerdef || haveVerdef) continue;
        std::uint64_t const vdOff = shtOffset(i);
        std::uint64_t const vdSz  = shtSize(i);
        if (rangeExceedsBuffer(vdOff, vdSz, bytes.size())) continue;
        // Version NAMES live in the strtab this section links to — read
        // `sh_link` rather than assuming `.dynstr`. They coincide in every
        // real `.so`, but the assumption is free to avoid and a wrong
        // string table yields silently wrong version names, not an error.
        std::uint32_t const vdStrIdx = shtLink(i);
        if (vdStrIdx == 0u || vdStrIdx >= e_shnum) continue;
        if (shtType(static_cast<std::uint16_t>(vdStrIdx)) != kShtStrtab) continue;
        std::uint64_t const vdStrOff = shtOffset(static_cast<std::uint16_t>(vdStrIdx));
        std::uint64_t const vdStrSz  = shtSize(static_cast<std::uint16_t>(vdStrIdx));
        if (rangeExceedsBuffer(vdStrOff, vdStrSz, bytes.size())) continue;
        std::uint64_t const vdEnd = vdOff + vdSz;
        // `sh_info` is the verdef COUNT; it also bounds the walk so a
        // self-referential `vd_next` cannot spin forever.
        std::uint32_t const vdCount = shtInfo(i);
        std::uint64_t cur = vdOff;
        for (std::uint32_t n = 0; n < vdCount; ++n) {
            if (cur + kElf64VerdefSz > vdEnd) break;
            std::uint16_t const vd_flags = readU16(bytes, static_cast<std::size_t>(cur + 2));
            std::uint16_t const vd_ndx   = readU16(bytes, static_cast<std::size_t>(cur + 4));
            std::uint32_t const vd_aux   = readU32(bytes, static_cast<std::size_t>(cur + 12));
            std::uint32_t const vd_next  = readU32(bytes, static_cast<std::size_t>(cur + 16));
            // The FIRST Verdaux is the version's OWN name; any further ones
            // are its PARENT versions (the `DSSVER_2.0 -> DSSVER_1.0`
            // inheritance chain), which name no symbol and are not read.
            // Skip the VER_FLG_BASE entry: it holds the FILE's name
            // (`libz.so.1`) at index 1 == VER_NDX_GLOBAL, i.e. the index
            // that means "unversioned" — recording it would turn every
            // unversioned export into one versioned at its own soname.
            bool const requestable =
                (vd_flags & kVerFlgBase) == 0 && vd_ndx > kVerNdxGlobal;
            if (requestable && vd_aux != 0
                && cur + vd_aux + kElf64VerdauxSz <= vdEnd) {
                std::uint32_t const vda_name =
                    readU32(bytes, static_cast<std::size_t>(cur + vd_aux));
                std::string nm = readNulTerminated(
                    bytes, static_cast<std::size_t>(vdStrOff),
                    static_cast<std::size_t>(vdStrOff + vdStrSz), vda_name);
                if (!nm.empty()) verdefs.emplace_back(vd_ndx, std::move(nm));
            }
            if (vd_next == 0) break;   // end of the chain
            cur += vd_next;
        }
        // One `.gnu.version_d` per image — but mark it and KEEP SCANNING.
        // Breaking out of the section walk here would be a silent loss: GNU
        // ld happens to place `.gnu.version` before `.gnu.version_d` (this
        // host's libz.so.1: sections 6 then 7), so an early exit works right
        // up until a linker that orders them the other way, at which point
        // every version in the image disappears with nothing reporting it.
        // Section ORDER is a linker convention, not a gABI guarantee.
        haveVerdef = true;
    }
    auto const verdefName = [&](std::uint16_t idx) -> std::string const* {
        for (auto const& v : verdefs) if (v.first == idx) return &v.second;
        return nullptr;
    };

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
    // D-FFI-BINARY-READER-SURFACES-NO-SYMBOL-VERSION: a versym slot that
    // names a version index the `.gnu.version_d` chain never defines. The
    // row still surfaces (unversioned) — dropping a real export over a
    // broken side-table would be worse — but the operator is told, because
    // silently downgrading a versioned symbol to unversioned is the exact
    // misbind D-LK-ELF-SYMBOL-VERSIONING exists to prevent.
    std::uint32_t danglingVersymSkips = 0;
    for (std::size_t i = 1; i < numSyms; ++i) {
        std::size_t const symOff = static_cast<std::size_t>(dynsymOff)
                                  + i * kElf64SymSz;
        std::uint32_t const st_name  = readU32(bytes, symOff +  0);
        std::uint8_t  const st_info  = bytes[symOff +  4];
        std::uint8_t  const st_other = bytes[symOff +  5];
        std::uint16_t const st_shndx = readU16(bytes, symOff +  6);
        // st_value, st_size — unused for import-surface reporting (we
        // only care about NAME + KIND + VISIBILITY + LINKAGE for the
        // FF1 surface; the symbol's runtime VA is dyld's concern, not
        // ours). `st_shndx` IS read: it is the definedness bit (below).

        if (st_name == 0u) continue;  // unnamed entries (section syms, etc.) — by-design
        if (stBind(st_info) == kStbLocal) continue;  // locals don't export — by-design
        // D-LK-ELF-EMITS-ONE-DT-NEEDED-WHEN-TWO-LIBRARIES-ARE-REFERENCED.
        // `.dynsym` is NOT an export table: unlike PE's `.edata` and
        // Mach-O's export trie, it holds BOTH the definitions a library
        // offers AND the references it makes (its own imports), told
        // apart only by `st_shndx` (SHN_UNDEF == a reference). Reading
        // every row as an export made this reader claim a library
        // EXPORTS symbols it merely IMPORTS — the ONE sibling of the
        // three FF1 readers that disagreed with `ImportSurface`'s
        // documented contract ("a single symbol the dynamic library
        // exports"; `import_surface.hpp`). PE reads the export
        // directory and Mach-O filters `(N_EXT && N_TYPE == N_SECT)` /
        // walks the export trie, so neither could ever surface a
        // reference.
        //
        // MEASURED consequence, and it is not confined to DT_NEEDED:
        // `ingest()` binds a governed extern to the FIRST source whose
        // surface carries the name (first-source-wins,
        // `ffi/ingest.cpp`). libtcl8.6.so's `.dynsym` carries 14 zlib
        // names as SHN_UNDEF rows, so `--resolve-library libtcl8.6.so
        // --resolve-library libz.so.1` bound `deflateBound` to
        // libtcl8.6.so — the WRONG owning library — and libz.so.1 then
        // appeared in no `ExternImport.libraryPath` at all, so the ELF
        // writer emitted one DT_NEEDED because it was handed one
        // library. Swapping the two `--resolve-library` arguments
        // produced BOTH DT_NEEDEDs: the emitted dependency set was a
        // function of command-line ORDER. The image loaded anyway only
        // because ld.so's flat global scope reaches libz through
        // libtcl's own DT_NEEDED; on a host where it is not otherwise
        // reachable the artifact fails at load with nothing in the
        // build to explain why.
        //
        // A SHN_UNDEF row is skipped SILENTLY and that is deliberate:
        // it is a structural filter, exactly like the two skips above,
        // not an anomaly. The loudness lives downstream and is
        // IMPROVED by this filter — an extern that matches no real
        // export now reaches the shipped-descriptor oracle and the link
        // tier, which reject a genuinely undefined symbol with
        // K_SymbolUndefined instead of silently binding it to a library
        // that does not define it.
        //
        // Only SHN_UNDEF is filtered. SHN_ABS / SHN_COMMON / SHN_XINDEX
        // and every real section index are DEFINITIONS and stay.
        if (st_shndx == kShnUndef) continue;

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
        // D-FFI-BINARY-READER-SURFACES-NO-SYMBOL-VERSION: this row's
        // versym slot, resolved through the verdef chain read above. The
        // slot is indexed by the symbol's `.dynsym` position — the same `i`
        // — which is why this must be read INSIDE the loop rather than
        // alongside the section scan. A `.gnu.version` shorter than
        // `.dynsym` leaves the tail unversioned (a real, if unusual,
        // shape); a version index the verdef chain does not define is the
        // anomaly, counted above.
        if (haveVersym && i < versymCnt) {
            std::uint16_t const versym =
                readU16(bytes, static_cast<std::size_t>(versymOff)
                                   + i * kVersymEntSz);
            std::uint16_t const verIdx = versym & kVersymIdxMask;
            if (verIdx > kVerNdxGlobal) {
                if (std::string const* nm = verdefName(verIdx); nm != nullptr) {
                    row.elfSymbolVersion = ElfSymbolVersion{
                        *nm, (versym & kVersymHidden) == 0};
                } else {
                    ++danglingVersymSkips;
                }
            }
        }
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
    if (!haveVersym && !versymRejectReason.empty()) {
        dss::report(reporter,
            DiagnosticCode::F_BinaryReaderPartialCorruption,
            DiagnosticSeverity::Warning,
            "ELF64 reader: '" + std::string{libraryPathLabel}
            + "': a `.gnu.version` (SHT_GNU_versym) section is present but "
              "was not decodable — " + versymRejectReason
            + ". Every symbol is surfaced UNVERSIONED, so an import bound "
              "from this library will request no version and bind whatever "
              "the loader considers default. The library's version "
              "information EXISTS and is being dropped; this is not the same "
              "as a library that was never versioned.");
    }
    if (danglingVersymSkips > 0) {
        dss::report(reporter,
            DiagnosticCode::F_BinaryReaderPartialCorruption,
            DiagnosticSeverity::Warning,
            "ELF64 reader: '" + std::string{libraryPathLabel}
            + "': " + std::to_string(danglingVersymSkips)
            + " .dynsym entries carry a `.gnu.version` index that no "
              ".gnu.version_d entry defines — those symbols are surfaced "
              "UNVERSIONED (possibly truncated .gnu.version_d, or a "
              "mismatched sh_info count). An import bound from such a row "
              "will request no version and so bind the library's DEFAULT, "
              "which is right for most symbols and wrong for any whose "
              "default has moved.");
    }

    return out;
}

} // namespace dss::ffi
