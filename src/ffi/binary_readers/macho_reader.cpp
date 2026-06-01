#include "ffi/binary_readers/macho_reader.hpp"

#include "core/cpp_invariants.hpp"  // arithmetic-right-shift static_assert
#include "core/types/parse_diagnostic.hpp"
#include "ffi/binary_readers/reader_common.hpp"

#include <optional>
#include <string>

namespace dss::ffi {

namespace {

// ── Mach-O 64 layout constants ──────────────────────────────────
// Audited against Apple's `<mach-o/loader.h>` + `<mach-o/nlist.h>`
// (Darwin xnu source — public). Field offsets are reproduced here
// rather than #include-pulled to keep `src/ffi/` free of host-OS
// SDK dependencies (the reader runs on Linux / Windows / macOS hosts
// over a Mach-O byte buffer — host agnostic by construction).
//
//   mach_header_64 (32 bytes):
//     [ 0.. 3] magic       (0xFEEDFACF — 64-bit LE)
//     [ 4.. 7] cputype     (skipped — surfaced via libraryPathLabel)
//     [ 8..11] cpusubtype  (skipped)
//     [12..15] filetype    (skipped — MH_DYLIB / MH_EXECUTE / etc.)
//     [16..19] ncmds       (count of load commands)
//     [20..23] sizeofcmds  (total byte size of load commands)
//     [24..27] flags       (skipped)
//     [28..31] reserved    (always 0)
//
//   load_command (every LC, 8 bytes preamble):
//     [ 0.. 3] cmd
//     [ 4.. 7] cmdsize
//
//   LC_SYMTAB command (LC_SYMTAB == 0x2, cmdsize == 24):
//     [ 0.. 3] cmd (= LC_SYMTAB)
//     [ 4.. 7] cmdsize (= 24)
//     [ 8..11] symoff   (file offset to symbol table)
//     [12..15] nsyms    (count of nlist_64 entries)
//     [16..19] stroff   (file offset to string table)
//     [20..23] strsize  (size of string table in bytes)
//
//   LC_DYSYMTAB command (LC_DYSYMTAB == 0xB, cmdsize == 80) —
//   v1 uses 8 fields (the external-defined slice index/count); the
//   remaining 64 bytes (local syms, undefs, ToC, mod table, refs,
//   indirect syms) are skipped (deferred surfaces).
//     [ 0.. 3] cmd (= LC_DYSYMTAB)
//     [ 4.. 7] cmdsize (= 80)
//     [ 8..11] ilocalsym
//     [12..15] nlocalsym
//     [16..19] iextdefsym  (start index in `LC_SYMTAB` table)
//     [20..23] nextdefsym  (count of externally-defined entries)
//     [...]    further fields skipped
//
//   nlist_64 (16 bytes each):
//     [ 0.. 3] n_strx   (index into string table; 0 = unnamed)
//     [ 4]     n_type   (low 3 bits = type-or-flags; high bits stab)
//     [ 5]     n_sect   (1-based section index; 0 = NO_SECT)
//     [ 6.. 7] n_desc   (weak-def flags; skipped v1)
//     [ 8..15] n_value  (symbol VA; skipped v1)

constexpr std::size_t   kMachOHeaderSize  = 32;
constexpr std::size_t   kMachOLcPreamble  = 8;
constexpr std::size_t   kMachOSymtabCmdSz = 24;
constexpr std::size_t   kMachONlist64Sz   = 16;

constexpr std::uint32_t kMachOMagic64     = 0xFEEDFACFu;

constexpr std::uint32_t kLcSymtab         = 0x2u;
constexpr std::uint32_t kLcDysymtab       = 0xBu;

// n_type masks (Apple `<mach-o/nlist.h>`):
//   N_STAB == 0xE0 — stab debugging entries (skip wholesale)
//   N_PEXT == 0x10 — private extern (visibility = Hidden)
//   N_TYPE == 0x0E — type mask: N_UNDF=0, N_ABS=2, N_SECT=0xE, N_PBUD=0xC, N_INDR=0xA
//   N_EXT  == 0x01 — external bit (low bit; symbol is exported)
constexpr std::uint8_t  kNStabMask        = 0xE0u;
constexpr std::uint8_t  kNPextBit         = 0x10u;
constexpr std::uint8_t  kNTypeMask        = 0x0Eu;
constexpr std::uint8_t  kNExtBit          = 0x01u;

constexpr std::uint8_t  kNTypeUndf        = 0x00u;  // undefined (imported, not exported)
constexpr std::uint8_t  kNTypeSect        = 0x0Eu;  // defined in section — the export case

// POD wrapper over the raw `n_type` byte with named accessors. The
// raw `&`/`==` idiom is repeated across the symbol-walk loop + the
// visibility helper; the type pins the bit semantics into the type
// system rather than living in repeated bit-ops + comments.
struct NType {
    std::uint8_t raw;
    [[nodiscard]] constexpr bool isStab() const noexcept {
        return (raw & kNStabMask) != 0u;
    }
    [[nodiscard]] constexpr bool isPrivateExtern() const noexcept {
        return (raw & kNPextBit) != 0u;
    }
    [[nodiscard]] constexpr bool isExternal() const noexcept {
        return (raw & kNExtBit) != 0u;
    }
    [[nodiscard]] constexpr std::uint8_t typeBits() const noexcept {
        return raw & kNTypeMask;
    }
    [[nodiscard]] constexpr bool isSectionDefined() const noexcept {
        return typeBits() == kNTypeSect;
    }
    [[nodiscard]] constexpr bool isUndefined() const noexcept {
        return typeBits() == kNTypeUndf;
    }
    [[nodiscard]] constexpr SymbolVisibility toVisibility() const noexcept {
        return isPrivateExtern() ? SymbolVisibility::Hidden
                                 : SymbolVisibility::Default;
    }
};

} // namespace

std::expected<std::vector<ImportSurface>, BinaryReadError>
readMacho(std::span<std::uint8_t const> bytes,
          std::string_view              libraryPathLabel,
          DiagnosticReporter&           reporter) {
    // ── mach_header_64 + magic ──
    if (bytes.size() < kMachOHeaderSize) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "Mach-O reader: file is shorter than mach_header_64 (32 bytes)",
            reporter));
    }
    std::uint32_t const magic = readU32(bytes, 0);
    if (magic != kMachOMagic64) {
        // guessFormat routes only 0xFEEDFACF here; this arm guards a
        // TOCTOU between guess and read on the same byte buffer.
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "Mach-O reader: header magic is not 0xFEEDFACF "
            "(64-bit LE Mach-O)", reporter));
    }
    std::uint32_t const ncmds      = readU32(bytes, 16);
    std::uint32_t const sizeofcmds = readU32(bytes, 20);
    if (rangeExceedsBuffer(kMachOHeaderSize, sizeofcmds, bytes.size())) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "Mach-O reader: sizeofcmds=" + std::to_string(sizeofcmds)
            + " runs past EOF (file size " + std::to_string(bytes.size())
            + ")", reporter));
    }

    // ── Walk load commands; collect LC_SYMTAB + (optional) LC_DYSYMTAB ──
    std::optional<std::size_t> symtabOff;
    std::optional<std::size_t> dysymtabOff;
    std::size_t                lcOff      = kMachOHeaderSize;
    std::size_t const          lcEnd      = kMachOHeaderSize + sizeofcmds;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        if (rangeExceedsBuffer(lcOff, kMachOLcPreamble, bytes.size())
         || lcOff + kMachOLcPreamble > lcEnd) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "Mach-O reader: load command #" + std::to_string(i)
                + " preamble runs past sizeofcmds region", reporter));
        }
        std::uint32_t const cmd     = readU32(bytes, lcOff + 0);
        std::uint32_t const cmdsize = readU32(bytes, lcOff + 4);
        if (cmdsize < kMachOLcPreamble) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "Mach-O reader: load command #" + std::to_string(i)
                + " cmdsize=" + std::to_string(cmdsize)
                + " is smaller than the 8-byte preamble (corrupted)",
                reporter));
        }
        if (rangeExceedsBuffer(lcOff, cmdsize, bytes.size())
         || lcOff + cmdsize > lcEnd) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "Mach-O reader: load command #" + std::to_string(i)
                + " body (cmdsize=" + std::to_string(cmdsize)
                + ") runs past sizeofcmds region", reporter));
        }
        if (cmd == kLcSymtab) {
            if (cmdsize < kMachOSymtabCmdSz) {
                return std::unexpected(emitAndReturn(
                    BinaryReadErrorKind::CorruptedBinary,
                    "Mach-O reader: LC_SYMTAB cmdsize=" + std::to_string(cmdsize)
                    + " (expected 24)", reporter));
            }
            symtabOff = lcOff;
        } else if (cmd == kLcDysymtab) {
            // LC_DYSYMTAB minimum: preamble (8) + the 4 fields we
            // read after it (ilocalsym/nlocalsym/iextdefsym/nextdefsym,
            // 4 × u32 = 16 bytes) = 24 bytes. The full Apple
            // LC_DYSYMTAB struct is 80 bytes (18 u32 fields after
            // preamble) — we intentionally read only the 4 fields
            // needed for the extdef filter v1; the remaining
            // sub-tables (local syms, undefs, ToC, mod table, refs,
            // indirect syms) are deferred surfaces.
            if (cmdsize < kMachOLcPreamble + 16u) {
                return std::unexpected(emitAndReturn(
                    BinaryReadErrorKind::CorruptedBinary,
                    "Mach-O reader: LC_DYSYMTAB cmdsize=" + std::to_string(cmdsize)
                    + " is too small to hold iextdefsym/nextdefsym",
                    reporter));
            }
            dysymtabOff = lcOff;
        }
        lcOff += cmdsize;
    }

    if (!symtabOff) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::SectionNotFound,
            "Mach-O reader: no LC_SYMTAB found "
            "(stripped binary?)", reporter));
    }

    // ── LC_SYMTAB fields ──
    std::uint32_t const symoff  = readU32(bytes, *symtabOff +  8);
    std::uint32_t const nsyms   = readU32(bytes, *symtabOff + 12);
    std::uint32_t const stroff  = readU32(bytes, *symtabOff + 16);
    std::uint32_t const strsize = readU32(bytes, *symtabOff + 20);

    std::uint64_t const symtabBytes =
        static_cast<std::uint64_t>(nsyms) * static_cast<std::uint64_t>(kMachONlist64Sz);
    if (rangeExceedsBuffer(symoff, symtabBytes, bytes.size())) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "Mach-O reader: symbol table (symoff=" + std::to_string(symoff)
            + " + " + std::to_string(symtabBytes) + " bytes for "
            + std::to_string(nsyms) + " entries) runs past EOF", reporter));
    }
    if (rangeExceedsBuffer(stroff, strsize, bytes.size())) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "Mach-O reader: string table (stroff=" + std::to_string(stroff)
            + " + " + std::to_string(strsize) + " bytes) runs past EOF",
            reporter));
    }

    // ── Walk symbols ──
    // Filter strategy: if LC_DYSYMTAB present + nextdefsym > 0, use
    // its [iextdefsym, iextdefsym+nextdefsym) slice (the canonical
    // "externally-defined" symbols). Otherwise fall back to scanning
    // ALL of LC_SYMTAB and filtering via (N_EXT bit set) && (N_TYPE
    // == N_SECT) — defined-and-external symbols.
    std::uint32_t walkStart = 0;
    std::uint32_t walkEnd   = nsyms;
    bool          dysymtabFilter = false;
    if (dysymtabOff) {
        std::uint32_t const iextdefsym = readU32(bytes, *dysymtabOff + 16);
        std::uint32_t const nextdefsym = readU32(bytes, *dysymtabOff + 20);
        if (nextdefsym > 0u) {
            // Slice must lie within [0, nsyms).
            std::uint64_t const sliceEnd =
                static_cast<std::uint64_t>(iextdefsym)
                + static_cast<std::uint64_t>(nextdefsym);
            if (sliceEnd > static_cast<std::uint64_t>(nsyms)) {
                return std::unexpected(emitAndReturn(
                    BinaryReadErrorKind::CorruptedBinary,
                    "Mach-O reader: LC_DYSYMTAB extdef slice "
                    "[" + std::to_string(iextdefsym) + ", "
                    + std::to_string(sliceEnd) + ") exceeds nsyms="
                    + std::to_string(nsyms), reporter));
            }
            walkStart      = iextdefsym;
            walkEnd        = static_cast<std::uint32_t>(sliceEnd);
            dysymtabFilter = true;
        }
        // nextdefsym == 0 in a valid LC_DYSYMTAB means "no externally-
        // defined symbols". Fall through to the N_EXT walk (which
        // will also surface nothing) — keeping the behavior uniform.
    }

    std::vector<ImportSurface> out;
    out.reserve(walkEnd - walkStart);
    // D-FF1-PARTIAL-CORRUPTION-MACHO: same counter pattern as ELF+PE.
    // Skip cases collapse into one Warning summarizing partial loss.
    std::uint32_t corruptedNameSkips = 0;
    for (std::uint32_t i = walkStart; i < walkEnd; ++i) {
        std::size_t const symOff = static_cast<std::size_t>(symoff)
                                  + static_cast<std::size_t>(i) * kMachONlist64Sz;
        std::uint32_t const n_strx = readU32(bytes, symOff + 0);
        NType const nt{bytes[symOff + 4]};

        // Cheapest gate first: unnamed entries (by-design, not corruption).
        if (n_strx == 0u) continue;
        // Stab debug entries — never exports. Applied to BOTH paths
        // (defense-in-depth: a malformed LC_DYSYMTAB could include
        // stab entries inside its extdef slice).
        if (nt.isStab()) continue;
        // Defense-in-depth N_TYPE filter: even on the dysymtabFilter
        // path, require N_SECT. A malformed/corrupted DYSYMTAB whose
        // slice contains N_UNDF/N_ABS/N_INDR entries would otherwise
        // surface them as Function/Default exports with garbage
        // `n_value`. Real linkers pre-filter the slice; trust-but-
        // verify is cheap.
        if (!nt.isSectionDefined()) continue;
        // Fallback walk only: also require the N_EXT bit. LC_DYSYMTAB
        // membership is the authoritative external-visibility signal
        // when present; the bit on individual rows can be inconsistent.
        if (!dysymtabFilter && !nt.isExternal()) continue;

        ImportSurface row;
        row.mangledName = readNulTerminated(bytes,
                                             static_cast<std::size_t>(stroff),
                                             static_cast<std::size_t>(stroff + strsize),
                                             n_strx);
        if (row.mangledName.empty()) {
            // n_strx was non-zero but the string-table read returned
            // empty — out-of-range or truncated string table.
            ++corruptedNameSkips;
            continue;
        }
        row.libraryPath = std::string{libraryPathLabel};
        // v1: all defined exports map to Function. Anchor
        // D-FF1-MACHO-SECT-KIND reserves the section-table walk
        // (n_sect → __TEXT/__DATA/__TLS section name → SymbolKind).
        row.kind        = SymbolKind::Function;
        row.visibility  = nt.toVisibility();
        // v1: linkage is always External for exports. Anchor
        // D-FF1-MACHO-WEAK-DEF reserves the `n_desc & N_WEAK_DEF`
        // read that would surface weak symbols as SymbolLinkage::Weak.
        row.linkage     = SymbolLinkage::External;
        out.push_back(std::move(row));
    }

    if (corruptedNameSkips > 0) {
        dss::report(reporter,
            DiagnosticCode::F_BinaryReaderPartialCorruption,
            DiagnosticSeverity::Warning,
            "Mach-O reader: '" + std::string{libraryPathLabel}
            + "': skipped " + std::to_string(corruptedNameSkips)
            + " LC_SYMTAB entries with corrupted name indices "
              "(non-zero n_strx resolved to empty string — possibly "
              "truncated string table or out-of-bounds name offset). "
              "Surfaced " + std::to_string(out.size())
            + " valid symbols.");
    }

    return out;
}

} // namespace dss::ffi
