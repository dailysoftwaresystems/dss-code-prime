#include "link/format/macho_object_reader.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/symbol_attrs.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Mach-O 64-bit MH_OBJECT reader -- the inverse of macho.cpp's MH_OBJECT
// writer. See the header for the reconstruction contract + scope + the six
// Mach-O-vs-ELF inversions. Every field is bounds-checked; any violation
// fails loud (F_* diagnostic + nullopt).

namespace dss::macho {

namespace {

using dss::report;

// -- Mach-O 64-bit structural constants (<mach-o/loader.h>,
//    <mach-o/nlist.h>, <mach-o/reloc.h>) ------------------------------
//
// The SAME record layout the writer in macho.cpp hardcodes. Re-declared
// locally rather than #include-pulled from `ffi/binary_readers/
// macho_reader.cpp`: `ffi` already depends UP on `link`
// (`link/object_format_schema.hpp`), so a `link` -> `ffi` include would
// form a dependency cycle. This mirrors the c164 ELF reader re-declaring
// the same constants to keep `ffi` off `link` (elf_object_reader.cpp).
constexpr std::size_t kMachHeader64Sz = 32;
constexpr std::size_t kLcPreambleSz   = 8;
constexpr std::size_t kSegCmd64HdrSz  = 72;  // segment_command_64 header
constexpr std::size_t kSection64Sz    = 80;  // one section_64 record
constexpr std::size_t kSymtabCmdSz    = 24;  // LC_SYMTAB command
constexpr std::size_t kNlist64Sz      = 16;  // one nlist_64 record
constexpr std::size_t kRelocInfoSz    = 8;   // one relocation_info record
constexpr std::size_t kName16Sz       = 16;  // section_64 / segname field

constexpr std::uint32_t kMachOMagic64 = 0xFEEDFACFu;  // 64-bit LE
constexpr std::uint32_t kMhObject     = 1;            // filetype MH_OBJECT

constexpr std::uint32_t kLcSegment64 = 0x19u;
constexpr std::uint32_t kLcSymtab    = 0x02u;

// mach_header_64 field offsets.
constexpr std::size_t kHdrMagicOff    = 0;
constexpr std::size_t kHdrFiletypeOff = 12;
constexpr std::size_t kHdrNcmdsOff    = 16;
constexpr std::size_t kHdrSizeCmdsOff = 20;

// nlist_64.n_type masks (<mach-o/nlist.h>).
constexpr std::uint8_t kNStabMask = 0xE0u;  // any stab bit -> debug entry
constexpr std::uint8_t kNPextBit  = 0x10u;  // private extern (Hidden)
constexpr std::uint8_t kNTypeMask = 0x0Eu;  // N_TYPE
constexpr std::uint8_t kNExtBit   = 0x01u;  // external
constexpr std::uint8_t kNTypeUndf = 0x00u;  // N_UNDF (undefined -> extern)
constexpr std::uint8_t kNTypeSect = 0x0Eu;  // N_SECT (defined in a section)

// section_64.flags: the low byte is the section TYPE; S_ZEROFILL (1) marks
// a bss-style section that reserves an addr span but stores NO file bytes.
constexpr std::uint32_t kSectTypeMask = 0x000000FFu;
constexpr std::uint32_t kSZerofill    = 1;

// relocation_info.r_info bit layout (LSB): r_symbolnum bits 0..23,
// r_pcrel bit 24, r_length bits 25..26, r_extern bit 27, r_type bits
// 28..31. The PACKED nativeId the format schema stores is
// (r_type<<28)|(r_length<<25)|(r_pcrel<<24) -- everything EXCEPT the
// walker-owned r_extern (bit 27) and r_symbolnum (bits 0..23).
constexpr std::uint32_t kRInfoSymbolnumMask = 0x00FFFFFFu;
constexpr std::uint32_t kRInfoExternBit     = 1u << 27;
constexpr std::uint32_t kRInfoNativeIdMask  = 0xF7000000u;

// Is this target-schema reloc formula a FUNCTION CALL / BRANCH (as opposed
// to a data-address computation)? The AGNOSTIC "extern is a function"
// signal on Mach-O: aarch64 ARM64_RELOC_BRANCH26 carries the
// `Aarch64Call26` formula. Mach-O declares NO PLT-variant native id (an
// extern call is BRANCH26 against the same id whether or not ld64 builds a
// stub), so this formula test is the whole signal -- no `pltNativeId` leg
// (unlike the ELF reader, whose x86_64 rel32 leans on a PLT variant).
[[nodiscard]] constexpr bool isCallBranchFormula(RelocFormulaKind k) noexcept {
    return k == RelocFormulaKind::Aarch64Call26;
}

// Overflow-safe [off, off+size) within [0, total) -- the c159-c164
// `rangeExceedsBuffer` shape (subtraction, never `off + size` which wraps
// on a hostile/corrupted header).
[[nodiscard]] constexpr bool
rangeExceedsBuffer(std::uint64_t off, std::uint64_t size, std::uint64_t total) noexcept {
    return off > total || size > total - off;
}

// LE scalar readers -- every call site is preceded by a rangeExceedsBuffer
// gate proving [o, o+N) is in-bounds.
[[nodiscard]] std::uint16_t rdU16(std::span<std::uint8_t const> b, std::size_t o) noexcept {
    return static_cast<std::uint16_t>(b[o]) | (static_cast<std::uint16_t>(b[o + 1]) << 8);
}
[[nodiscard]] std::uint32_t rdU32(std::span<std::uint8_t const> b, std::size_t o) noexcept {
    return  static_cast<std::uint32_t>(b[o])
         | (static_cast<std::uint32_t>(b[o + 1]) <<  8)
         | (static_cast<std::uint32_t>(b[o + 2]) << 16)
         | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}
[[nodiscard]] std::uint64_t rdU64(std::span<std::uint8_t const> b, std::size_t o) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(b[o + i]) << (i * 8);
    return v;
}

// Read a fixed 16-byte NUL-padded name field (section_64 sectname /
// segname). The field is exactly 16 bytes (its in-bounds-ness is proven by
// the section-record bound); the name is the bytes up to the first NUL (or
// all 16 if unterminated).
[[nodiscard]] std::string rdName16(std::span<std::uint8_t const> b, std::size_t o) {
    std::size_t n = 0;
    while (n < kName16Sz && b[o + n] != 0u) ++n;
    return std::string{reinterpret_cast<char const*>(&b[o]), n};
}

// NUL-terminated symbol name at strtab[index], bounded by [tabStart, tabEnd).
[[nodiscard]] std::string
rdName(std::span<std::uint8_t const> b, std::uint64_t tabStart, std::uint64_t tabEnd,
       std::uint32_t index) {
    if (tabEnd > b.size()) tabEnd = b.size();  // defense-in-depth
    std::uint64_t const start = tabStart + index;
    if (start >= tabEnd) return {};
    std::uint64_t end = start;
    while (end < tabEnd && b[end] != 0u) ++end;
    return std::string{reinterpret_cast<char const*>(&b[start]),
                       static_cast<std::size_t>(end - start)};
}

// section_64.align is a LOG2 field. Convert to an `Alignment` newtype. Only
// a re-layout hint (the merge re-lays-out every item), so a value beyond
// the newtype's 256-byte cap (or a wild log2) falls back to byte alignment
// -- never a correctness input on read-back (mirrors the ELF reader's
// alignFromSection contract).
[[nodiscard]] Alignment alignFromLog2(std::uint32_t log2) noexcept {
    if (log2 == 0u || log2 > 8u) return Alignment{};   // 1 byte, or > 256 -> byte
    return Alignment::fromBytes(static_cast<std::uint32_t>(1u) << log2)
        .value_or(Alignment{});
}

// N_PEXT (private extern) -> Hidden, else Default (matches the ffi
// macho_reader's visibility rule; Mach-O nlist has no protected/internal).
[[nodiscard]] constexpr SymbolVisibility machoVisibility(std::uint8_t nType) noexcept {
    return (nType & kNPextBit) != 0u ? SymbolVisibility::Hidden
                                     : SymbolVisibility::Default;
}

// One parsed section_64 (only the fields the reader consumes). The 1-based
// n_sect ordinal is this section's index in `sections` PLUS one.
struct Section {
    std::string   segName;
    std::string   sectName;
    std::uint64_t addr     = 0;
    std::uint64_t size     = 0;
    std::uint64_t offset   = 0;   // file offset of the section body
    std::uint32_t align    = 0;   // log2
    std::uint64_t reloff   = 0;   // file offset of this section's reloc table
    std::uint32_t nreloc   = 0;
    std::uint32_t flags    = 0;
    bool          zeroFill = false;
    std::optional<SectionKind> kind;  // resolved from (segment, name) via the schema
};

// One decoded nlist_64.
struct Nlist {
    std::uint32_t strx  = 0;
    std::uint8_t  type  = 0;
    std::uint8_t  sect  = 0;   // 1-based section ordinal (0 = NO_SECT)
    std::uint16_t desc  = 0;
    std::uint64_t value = 0;   // FLAT `.o`-space address
    std::string   name;
};

// A defined N_SECT|N_EXT symbol staged for atom slicing: its section
// -relative offset (n_value - section.addr) is an atom boundary.
struct DefSym {
    std::uint32_t    symIdx    = 0;
    std::uint64_t    secRelOff = 0;
    std::string      name;
    SymbolBinding    binding    = SymbolBinding::Global;
    SymbolVisibility visibility = SymbolVisibility::Default;
};

// A reconstructed [start, start+len) byte range within one section, plus
// the output-vector index of the AssembledFunction / AssembledData it
// backs -- used to route a relocation site to its owning item.
struct Interval {
    std::uint64_t start  = 0;
    std::uint64_t len    = 0;
    std::size_t   outIdx = 0;
};

// Sign-extend the low `width` bytes of `raw` to a signed 64-bit value --
// the inverse of the writer truncating an `int64` addend to `widthBytes`
// LE in the patched data slot (macho.cpp:1288). width is 4 or 8 (the
// non-pcrel Linear kinds a data slot uses -- schema invariant (a)).
[[nodiscard]] std::int64_t signExtendLE(std::uint64_t raw, std::uint8_t width) noexcept {
    if (width >= 8u) return static_cast<std::int64_t>(raw);
    unsigned const bits = static_cast<unsigned>(width) * 8u;
    std::uint64_t const mask = (static_cast<std::uint64_t>(1) << bits) - 1u;
    std::uint64_t v = raw & mask;
    std::uint64_t const signBit = static_cast<std::uint64_t>(1) << (bits - 1u);
    if ((v & signBit) != 0u) v |= ~mask;  // extend the sign into the high bytes
    return static_cast<std::int64_t>(v);
}

} // namespace

std::optional<AssembledModule>
readRelocatableObject(std::span<std::uint8_t const> bytes,
                      TargetSchema const&            targetSchema,
                      ObjectFormatSchema const&      objectFormatSchema,
                      DiagnosticReporter&            reporter,
                      CompilationUnitId              cuId) {
    auto fail = [&](DiagnosticCode code, std::string detail)
        -> std::optional<AssembledModule> {
        report(reporter, code, DiagnosticSeverity::Error, std::move(detail));
        return std::nullopt;
    };

    // -- (0) Format sanity: this reader speaks Mach-O only -----------
    if (objectFormatSchema.kind() != ObjectFormatKind::MachO) {
        return fail(DiagnosticCode::F_UnsupportedBinaryFormat,
            std::string{"macho::readRelocatableObject: object format schema '"}
                + std::string{objectFormatSchema.name()} + "' is kind "
                + std::string{objectFormatKindName(objectFormatSchema.kind())}
                + ", not Mach-O -- the Mach-O reader cannot parse it.");
    }

    // -- (1) mach_header_64 ------------------------------------------
    if (bytes.size() < kMachHeader64Sz) {
        return fail(DiagnosticCode::F_CorruptedBinary,
            "macho::readRelocatableObject: file shorter than mach_header_64 "
            "(32 bytes).");
    }
    std::uint32_t const magic = rdU32(bytes, kHdrMagicOff);
    if (magic != kMachOMagic64) {
        // Reject FAT (0xCAFEBABE), big-endian (0xCEFAEDFE), and 32-bit
        // (0xFEEDFACE) up front -- this reader is 64-bit LE Mach-O only.
        return fail(DiagnosticCode::F_UnknownBinaryFormat,
            "macho::readRelocatableObject: header magic is not 0xFEEDFACF "
            "(64-bit little-endian Mach-O) -- FAT / big-endian / 32-bit are "
            "not read.");
    }
    std::uint32_t const filetype = rdU32(bytes, kHdrFiletypeOff);
    if (filetype != kMhObject) {
        // An MH_EXECUTE / MH_DYLIB is a link OUTPUT, not a relocatable input
        // -- fail loud like the ELF reader's `e_type != ET_REL` check.
        return fail(DiagnosticCode::F_UnsupportedBinaryFormat,
            "macho::readRelocatableObject: mach_header_64.filetype="
            + std::to_string(filetype) + " is not MH_OBJECT (1) -- only "
              "relocatable objects are read back into a mergeable module "
              "(executables / dylibs are link OUTPUTS, not inputs).");
    }
    std::uint32_t const ncmds      = rdU32(bytes, kHdrNcmdsOff);
    std::uint32_t const sizeofcmds = rdU32(bytes, kHdrSizeCmdsOff);
    if (rangeExceedsBuffer(kMachHeader64Sz, sizeofcmds, bytes.size())) {
        return fail(DiagnosticCode::F_CorruptedBinary,
            "macho::readRelocatableObject: sizeofcmds="
            + std::to_string(sizeofcmds) + " runs past EOF (file "
            + std::to_string(bytes.size()) + ").");
    }

    // -- (2) Walk load commands: collect sections + LC_SYMTAB --------
    //
    // Every section_64's body [offset, offset+size) must be file-backed and
    // in-bounds (except S_ZEROFILL); its reloc table [reloff, reloff+nreloc
    // *8) must be in-bounds. The LC_SYMTAB's symbol + string tables are
    // bounds-checked after the walk.
    std::vector<Section> sections;             // 1-based n_sect = index + 1
    std::optional<std::uint64_t> symoff;
    std::uint32_t nsyms   = 0;
    std::uint64_t stroff  = 0;
    std::uint64_t strsize = 0;
    bool          sawSymtab = false;

    std::size_t       lcOff = kMachHeader64Sz;
    std::size_t const lcEnd = kMachHeader64Sz + static_cast<std::size_t>(sizeofcmds);
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        if (rangeExceedsBuffer(lcOff, kLcPreambleSz, bytes.size())
            || lcOff + kLcPreambleSz > lcEnd) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "macho::readRelocatableObject: load command #"
                + std::to_string(i) + " preamble runs past the sizeofcmds "
                "region.");
        }
        std::uint32_t const cmd     = rdU32(bytes, lcOff + 0);
        std::uint32_t const cmdsize = rdU32(bytes, lcOff + 4);
        if (cmdsize < kLcPreambleSz) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "macho::readRelocatableObject: load command #"
                + std::to_string(i) + " cmdsize=" + std::to_string(cmdsize)
                + " is smaller than the 8-byte preamble.");
        }
        if (rangeExceedsBuffer(lcOff, cmdsize, bytes.size())
            || lcOff + cmdsize > lcEnd) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "macho::readRelocatableObject: load command #"
                + std::to_string(i) + " body (cmdsize="
                + std::to_string(cmdsize) + ") runs past the sizeofcmds "
                "region.");
        }
        // Every field read below is inside [lcOff, lcOff+cmdsize), proven
        // in-buffer by the check above.
        if (cmd == kLcSegment64) {
            if (cmdsize < kSegCmd64HdrSz) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "macho::readRelocatableObject: LC_SEGMENT_64 #"
                    + std::to_string(i) + " cmdsize=" + std::to_string(cmdsize)
                    + " is smaller than the 72-byte segment header.");
            }
            std::uint32_t const nsects = rdU32(bytes, lcOff + 64);
            std::uint64_t const roomForSects =
                (static_cast<std::uint64_t>(cmdsize) - kSegCmd64HdrSz) / kSection64Sz;
            if (static_cast<std::uint64_t>(nsects) > roomForSects) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "macho::readRelocatableObject: LC_SEGMENT_64 #"
                    + std::to_string(i) + " nsects=" + std::to_string(nsects)
                    + " does not fit in cmdsize=" + std::to_string(cmdsize)
                    + " (over-claimed section count).");
            }
            for (std::uint32_t s = 0; s < nsects; ++s) {
                std::size_t const secOff =
                    lcOff + kSegCmd64HdrSz + static_cast<std::size_t>(s) * kSection64Sz;
                Section sec;
                sec.sectName = rdName16(bytes, secOff + 0);
                sec.segName  = rdName16(bytes, secOff + 16);
                sec.addr     = rdU64(bytes, secOff + 32);
                sec.size     = rdU64(bytes, secOff + 40);
                sec.offset   = rdU32(bytes, secOff + 48);
                sec.align    = rdU32(bytes, secOff + 52);
                sec.reloff   = rdU32(bytes, secOff + 56);
                sec.nreloc   = rdU32(bytes, secOff + 60);
                sec.flags    = rdU32(bytes, secOff + 64);
                sec.zeroFill = (sec.flags & kSectTypeMask) == kSZerofill;
                // A file-backed section's body must lie within the file. A
                // zero-fill section carries no file bytes (its offset may be 0
                // / past EOF legitimately), so it is exempt -- exactly like the
                // ELF reader exempts SHT_NOBITS.
                if (!sec.zeroFill
                    && rangeExceedsBuffer(sec.offset, sec.size, bytes.size())) {
                    return fail(DiagnosticCode::F_CorruptedBinary,
                        "macho::readRelocatableObject: section '" + sec.segName
                        + "," + sec.sectName + "' body ["
                        + std::to_string(sec.offset) + ", +"
                        + std::to_string(sec.size) + ") runs past EOF.");
                }
                std::uint64_t const relocBytes =
                    static_cast<std::uint64_t>(sec.nreloc) * kRelocInfoSz;
                if (rangeExceedsBuffer(sec.reloff, relocBytes, bytes.size())) {
                    return fail(DiagnosticCode::F_CorruptedBinary,
                        "macho::readRelocatableObject: section '" + sec.segName
                        + "," + sec.sectName + "' relocation table (reloff="
                        + std::to_string(sec.reloff) + ", nreloc="
                        + std::to_string(sec.nreloc) + ") runs past EOF.");
                }
                sections.push_back(std::move(sec));
            }
        } else if (cmd == kLcSymtab) {
            if (cmdsize < kSymtabCmdSz) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "macho::readRelocatableObject: LC_SYMTAB cmdsize="
                    + std::to_string(cmdsize) + " (expected 24).");
            }
            symoff    = rdU32(bytes, lcOff + 8);
            nsyms     = rdU32(bytes, lcOff + 12);
            stroff    = rdU32(bytes, lcOff + 16);
            strsize   = rdU32(bytes, lcOff + 20);
            sawSymtab = true;
        }
        lcOff += cmdsize;
    }

    if (!sawSymtab) {
        return fail(DiagnosticCode::F_SectionNotFound,
            "macho::readRelocatableObject: no LC_SYMTAB -- a relocatable "
            "object without a symbol table has no linkable identities to "
            "reconstruct.");
    }
    std::uint64_t const symBytes =
        static_cast<std::uint64_t>(nsyms) * kNlist64Sz;
    if (rangeExceedsBuffer(*symoff, symBytes, bytes.size())) {
        return fail(DiagnosticCode::F_CorruptedBinary,
            "macho::readRelocatableObject: symbol table (symoff="
            + std::to_string(*symoff) + " + " + std::to_string(symBytes)
            + " bytes for " + std::to_string(nsyms) + " entries) runs past "
            "EOF.");
    }
    if (rangeExceedsBuffer(stroff, strsize, bytes.size())) {
        return fail(DiagnosticCode::F_CorruptedBinary,
            "macho::readRelocatableObject: string table (stroff="
            + std::to_string(stroff) + " + " + std::to_string(strsize)
            + " bytes) runs past EOF.");
    }
    std::uint64_t const strEnd = stroff + strsize;

    // -- (3) Resolve each section's SectionKind from the (SEGMENT,
    //         SECTION) NAME PAIR -- the Mach-O identity, agnostic --------
    //
    // The two `__const` rows differ ONLY by segment (rodata = `__TEXT,
    // __const`, relro = `__DATA,__const`), so the pair -- never the section
    // name alone -- is the key. An unmapped section leaves kind = nullopt.
    auto pairKey = [](std::string_view seg, std::string_view name) -> std::string {
        std::string k{seg};
        k.push_back('\0');   // a NUL cannot appear inside a Mach-O 16-byte name
        k.append(name);
        return k;
    };
    std::unordered_map<std::string, SectionKind> pairToKind;
    for (auto const& row : objectFormatSchema.sections()) {
        pairToKind.emplace(pairKey(row.segment, row.name), row.kind);
    }
    for (auto& sec : sections) {
        if (auto it = pairToKind.find(pairKey(sec.segName, sec.sectName));
            it != pairToKind.end()) {
            sec.kind = it->second;
        }
    }

    // -- (4) Reverse reloc map (nativeId -> RelocationKind), from the
    //         FORMAT SCHEMA -- no hardcoded ARM64_RELOC_* numbers ---------
    //
    // `callSignalNativeIds` collects the native ids that mark an extern as a
    // FUNCTION: a row whose TARGET-schema formula is a call/branch class
    // (aarch64 CALL26). Mach-O has NO PLT-variant id, so this formula test
    // is the whole signal (unlike the ELF reader's PLT leg).
    std::unordered_map<std::uint32_t, RelocationKind> nativeToKind;
    std::unordered_set<std::uint32_t> callSignalNativeIds;
    for (auto const& r : objectFormatSchema.relocations()) {
        auto const ins = nativeToKind.emplace(r.nativeId, r.kind);
        if (!ins.second && ins.first->second.v != r.kind.v) {
            // A native id mapping to two DIFFERENT kinds is an ambiguous
            // schema -- fail loud rather than let "last row wins" silently
            // mis-decode (mirrors the ELF reader's duplicate-nativeId guard).
            return fail(DiagnosticCode::F_CorruptedBinary,
                "macho::readRelocatableObject: object format schema '"
                + std::string{objectFormatSchema.name()}
                + "' maps native reloc id " + std::to_string(r.nativeId)
                + " to two different RelocationKinds ("
                + std::to_string(ins.first->second.v) + " and "
                + std::to_string(r.kind.v) + ") -- ambiguous reverse map.");
        }
        if (auto const* tri = targetSchema.relocationInfo(r.kind);
            tri != nullptr && isCallBranchFormula(tri->formulaKind)) {
            callSignalNativeIds.insert(r.nativeId);
        }
    }

    // -- (5) Decode every nlist_64; assign SymbolId = symtab index ---
    std::vector<Nlist> syms(nsyms);
    for (std::uint32_t i = 0; i < nsyms; ++i) {
        std::size_t const so = static_cast<std::size_t>(*symoff)
                             + static_cast<std::size_t>(i) * kNlist64Sz;
        Nlist& s = syms[i];
        s.strx  = rdU32(bytes, so + 0);
        s.type  = bytes[so + 4];
        s.sect  = bytes[so + 5];
        s.desc  = rdU16(bytes, so + 6);
        s.value = rdU64(bytes, so + 8);
        s.name  = rdName(bytes, stroff, strEnd, s.strx);
    }

    // -- (6) Reconstruct externs, then stage defined symbols per section --
    AssembledModule mod;
    mod.cuId = cuId;

    // symtab index -> the extern's position in mod.externImports (for the
    // isData inference in step 7).
    std::unordered_map<std::uint32_t, std::size_t> externBySym;
    // Defined N_SECT|N_EXT symbols grouped by 1-based n_sect ordinal --
    // atom boundaries, sliced by sorted n_value in the per-section pass
    // below. A NAMELESS or LOCAL (block) N_SECT symbol is NOT an atom
    // boundary (see the loop) -- so it never splits a function.
    std::unordered_map<std::uint8_t, std::vector<DefSym>> defsBySection;

    for (std::uint32_t i = 0; i < nsyms; ++i) {
        Nlist const& s = syms[i];
        // Stab (debug) entries carry no reconstructible body. They still
        // occupy a symtab index (a reloc naming one fails loud in step 7).
        if ((s.type & kNStabMask) != 0u) continue;
        std::uint8_t const typeBits = s.type & kNTypeMask;
        bool const isExt = (s.type & kNExtBit) != 0u;

        if (typeBits == kNTypeUndf) {
            // N_UNDF -> an undefined reference -> an extern import. A nameless
            // UND slot (index 0 / padding) carries no import identity.
            if (s.name.empty()) continue;
            ExternImport ext;
            ext.symbol      = SymbolId{i};
            ext.mangledName = s.name;
            // Mach-O nlist carries NO type hint (no STT_FUNC), so seed DATA
            // and force to false (function) ONLY when a CALL/BRANCH-class
            // reloc targets it (step 7) -- the agnostic function signal.
            ext.isData      = true;
            externBySym.emplace(i, mod.externImports.size());
            mod.externImports.push_back(std::move(ext));
            continue;
        }

        if (typeBits != kNTypeSect) {
            // N_ABS / N_INDR / N_PBUD: not a section-backed body. Record a
            // ModuleSymbol so a reloc target still resolves by identity (the
            // reserved-index analog of the ELF reader's SHN_ABS handling).
            if (!s.name.empty()) {
                mod.symbols.push_back(ModuleSymbol{SymbolId{i}, s.name,
                    isExt ? SymbolBinding::Global : SymbolBinding::Local,
                    machoVisibility(s.type)});
            }
            continue;
        }

        // N_SECT -- defined in a section. n_sect is a 1-based ordinal.
        if (s.sect == 0u || static_cast<std::size_t>(s.sect) > sections.size()) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "macho::readRelocatableObject: defined symbol '" + s.name
                + "' names section ordinal " + std::to_string(s.sect)
                + " out of range [1, " + std::to_string(sections.size())
                + "] -- corrupt n_sect.");
        }
        Section const& sec = sections[s.sect - 1u];
        // n_value is a FLAT `.o`-space address; the section-relative offset
        // is n_value - section.addr. A flat address BELOW the section base is
        // corrupt (never over-/under-flow the slice).
        if (s.value < sec.addr) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "macho::readRelocatableObject: defined symbol '" + s.name
                + "' n_value=" + std::to_string(s.value)
                + " is below its section '" + sec.segName + "," + sec.sectName
                + "' addr=" + std::to_string(sec.addr) + ".");
        }
        std::uint64_t const secRelOff = s.value - sec.addr;

        if (isExt) {
            // Externally-visible defined symbol -> an atom boundary.
            defsBySection[s.sect].push_back(
                DefSym{i, secRelOff, s.name, SymbolBinding::Global,
                       machoVisibility(s.type)});
        } else if (!s.name.empty()) {
            // A LOCAL N_SECT symbol -- an interior `&&label` block symbol
            // (DSS's only N_SECT-local shape) or a foreign static. Recorded as
            // a LOCAL ModuleSymbol but NOT an atom boundary, so it never
            // splits the function that contains its interior offset. Its
            // interior-VA binding is the named follow-up (mirrors the ELF
            // reader treating interior text labels as ModuleSymbol only).
            mod.symbols.push_back(ModuleSymbol{SymbolId{i}, s.name,
                SymbolBinding::Local, machoVisibility(s.type)});
        }
    }

    // Per-section interval lists for relocation-site routing.
    std::unordered_map<std::uint8_t, std::vector<Interval>> funcIntervalsBySec;
    std::unordered_map<std::uint8_t, std::vector<Interval>> dataIntervalsBySec;

    auto pushModuleSym = [&](DefSym const& d) {
        if (!d.name.empty()) {
            mod.symbols.push_back(ModuleSymbol{SymbolId{d.symIdx}, d.name,
                                               d.binding, d.visibility});
        }
    };

    // Slice each section's atoms by SORTED n_value (THE key inversion --
    // nlist_64 has no size field). Each atom spans [off_k, next distinct
    // off) (the last -> section.size). Equal-offset ALIASES share the same
    // span (like the ELF equal-start rule) -- both get identical bytes.
    for (auto& [ordinal, defs] : defsBySection) {
        Section const& sec = sections[ordinal - 1u];
        std::optional<SectionKind> const rk = sec.kind;
        std::optional<DataSectionKind> const dk =
            rk.has_value() ? dataSectionKindOf(*rk) : std::nullopt;
        bool const isText = rk.has_value() && *rk == SectionKind::Text;

        std::sort(defs.begin(), defs.end(),
                  [](DefSym const& a, DefSym const& b) {
                      return a.secRelOff < b.secRelOff;
                  });

        for (std::size_t k = 0; k < defs.size(); ++k) {
            std::uint64_t const off = defs[k].secRelOff;
            // The atom ends at the next STRICTLY-GREATER offset (skipping
            // equal-offset aliases so they share the span), else section.size.
            std::uint64_t end = sec.size;
            for (std::size_t j = k + 1; j < defs.size(); ++j) {
                if (defs[j].secRelOff > off) { end = defs[j].secRelOff; break; }
            }
            if (off > sec.size || end < off) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "macho::readRelocatableObject: defined symbol '"
                    + defs[k].name + "' offset " + std::to_string(off)
                    + " exceeds its section '" + sec.segName + "," + sec.sectName
                    + "' size " + std::to_string(sec.size) + ".");
            }
            std::uint64_t const len = end - off;

            if (isText) {
                // A function body -- slice [off, end) out of the file-backed
                // `__text`. A Text section must never be zero-fill.
                if (sec.zeroFill
                    || rangeExceedsBuffer(off, len, sec.size)
                    || rangeExceedsBuffer(sec.offset, sec.size, bytes.size())) {
                    return fail(DiagnosticCode::F_CorruptedBinary,
                        "macho::readRelocatableObject: function symbol '"
                        + defs[k].name + "' range [" + std::to_string(off)
                        + ", +" + std::to_string(len) + ") is not a file-backed "
                        "slice of section '" + sec.segName + "," + sec.sectName
                        + "'.");
                }
                std::size_t const bodyOff =
                    static_cast<std::size_t>(sec.offset + off);
                AssembledFunction fn;
                fn.symbol = SymbolId{defs[k].symIdx};
                fn.bytes.assign(bytes.begin() + bodyOff,
                                bytes.begin() + bodyOff + static_cast<std::size_t>(len));
                funcIntervalsBySec[ordinal].push_back(
                    Interval{off, len, mod.functions.size()});
                mod.functions.push_back(std::move(fn));
                pushModuleSym(defs[k]);
                continue;
            }

            if (!dk.has_value()) {
                // A defined body in a section that resolves to no known code/
                // data kind must NEVER be silently dropped to a bodiless
                // ModuleSymbol (the "never a silent partial reconstruction"
                // contract) -- fail loud so the shape is recovered (a new
                // schema row) rather than mis-linked to an empty def.
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "macho::readRelocatableObject: defined symbol '"
                    + defs[k].name + "' lives in section '" + sec.segName + ","
                    + sec.sectName + "' which resolves to no known code/data "
                    "section kind -- refusing to silently drop a body (add the "
                    "section's kind to the format schema).");
            }

            // A data object -> an AssembledData item. File-backed sections
            // slice their bytes; zero-fill (bss) reserves the size with empty
            // bytes (the reservedSize invariant).
            AssembledData di;
            di.symbol    = SymbolId{defs[k].symIdx};
            di.section   = *dk;
            di.alignment = alignFromLog2(sec.align);
            if (isZeroFill(*dk)) {
                di.reservedSize = len;
            } else {
                if (sec.zeroFill
                    || rangeExceedsBuffer(off, len, sec.size)
                    || rangeExceedsBuffer(sec.offset, sec.size, bytes.size())) {
                    return fail(DiagnosticCode::F_CorruptedBinary,
                        "macho::readRelocatableObject: data symbol '"
                        + defs[k].name + "' range [" + std::to_string(off)
                        + ", +" + std::to_string(len) + ") is not a file-backed "
                        "slice of section '" + sec.segName + "," + sec.sectName
                        + "'.");
                }
                std::size_t const bodyOff =
                    static_cast<std::size_t>(sec.offset + off);
                di.bytes.assign(bytes.begin() + bodyOff,
                                bytes.begin() + bodyOff + static_cast<std::size_t>(len));
            }
            dataIntervalsBySec[ordinal].push_back(
                Interval{off, len, mod.dataItems.size()});
            mod.dataItems.push_back(std::move(di));
            pushModuleSym(defs[k]);
        }
    }

    // -- (7) Reconstruct relocations from every section's reloc table ----
    //
    // Each section_64 names its OWN relocation_info table (reloff/nreloc).
    // r_address is section-relative (the writer emits `fnStart+rel.offset`
    // for __text, `itemOff+rel.offset` for data). We route it to the
    // reconstructed atom whose byte range contains it (offset made item
    // -relative). A section with relocs but NO reconstructed atom (anonymous
    // content reached via a section symbol -- the gap-atom / foreign-clang
    // follow-up) is skipped; DSS output has none. A per-entry miss WITHIN a
    // reconstructed section still fails loud (never silently drop a reloc).
    auto findInterval = [](std::vector<Interval> const& ivs, std::uint64_t off)
        -> Interval const* {
        for (auto const& iv : ivs) {
            if (off >= iv.start && off < iv.start + iv.len) return &iv;
        }
        return nullptr;
    };

    for (std::size_t si = 0; si < sections.size(); ++si) {
        Section const& sec = sections[si];
        if (sec.nreloc == 0u) continue;
        std::uint8_t const ordinal = static_cast<std::uint8_t>(si + 1u);
        auto const fIt = funcIntervalsBySec.find(ordinal);
        auto const dIt = dataIntervalsBySec.find(ordinal);
        bool const patchesText = (fIt != funcIntervalsBySec.end() && !fIt->second.empty());
        bool const patchesData = (dIt != dataIntervalsBySec.end() && !dIt->second.empty());
        if (!patchesText && !patchesData) {
            // A reloc-bearing section that reconstructed NO atom. DSS output has
            // none -- every reloc it emits patches a __text function or a data
            // item, each an N_EXT atom. Fail loud rather than `continue`
            // (silent-failure-review fold): skipping would also bypass the
            // r_extern=0 guard below, so a foreign object's atom-less
            // section-relative relocs would vanish undiagnosed. A foreign
            // metadata section with relocs (e.g. `__compact_unwind`) rides the
            // named follow-up D-LK-MACHO-STATIC-SECTION-RELATIVE-RELOC.
            return fail(DiagnosticCode::F_CorruptedBinary,
                "macho::readRelocatableObject: section '" + sec.segName + ","
                + sec.sectName + "' carries " + std::to_string(sec.nreloc)
                + " relocation(s) but reconstructed no atom to attach them to -- "
                "refusing to silently drop a section's relocations.");
        }
        std::vector<Interval> const& ivs = patchesText ? fIt->second : dIt->second;

        for (std::uint32_t e = 0; e < sec.nreloc; ++e) {
            std::size_t const ro = static_cast<std::size_t>(sec.reloff)
                                 + static_cast<std::size_t>(e) * kRelocInfoSz;
            std::uint64_t const rAddress = rdU32(bytes, ro + 0);
            std::uint32_t const rInfo    = rdU32(bytes, ro + 4);
            bool const          rExtern  = (rInfo & kRInfoExternBit) != 0u;
            std::uint32_t const rSymNum  = rInfo & kRInfoSymbolnumMask;
            std::uint32_t const nativeId = rInfo & kRInfoNativeIdMask;

            // DSS ALWAYS writes r_extern=1 (every reloc points at a symbol).
            // An r_extern=0 SECTION-INDEX relocation is a foreign-clang shape
            // (r_symbolnum is a 1-based section number, not a symtab index) --
            // the section-relative-redirect analog is a named follow-up; fail
            // loud rather than mis-bind it as a symbol.
            if (!rExtern) {
                return fail(DiagnosticCode::F_UnsupportedBinaryFormat,
                    "macho::readRelocatableObject: relocation in section '"
                    + sec.segName + "," + sec.sectName
                    + "' has r_extern=0 (a SECTION-INDEX relocation) -- DSS "
                      "output is always symbol-relative (r_extern=1); the "
                      "section-relative-redirect shape is the named follow-up "
                      "D-LK-MACHO-STATIC-SECTION-RELATIVE-RELOC.");
            }
            if (rSymNum >= nsyms) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "macho::readRelocatableObject: relocation in section '"
                    + sec.segName + "," + sec.sectName + "' names symbol #"
                    + std::to_string(rSymNum) + " past the symbol table ("
                    + std::to_string(nsyms) + ").");
            }
            auto const kindIt = nativeToKind.find(nativeId);
            if (kindIt == nativeToKind.end()) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "macho::readRelocatableObject: relocation nativeId "
                    + std::to_string(nativeId) + " in section '" + sec.segName
                    + "," + sec.sectName + "' is not declared by Mach-O format '"
                    + std::string{objectFormatSchema.name()}
                    + "' -- cannot map it back to a universal RelocationKind.");
            }
            RelocationKind const kind = kindIt->second;
            auto const* tri = targetSchema.relocationInfo(kind);
            if (tri == nullptr) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "macho::readRelocatableObject: RelocationKind "
                    + std::to_string(kind.v) + " has no TargetRelocationInfo on '"
                    + std::string{targetSchema.name()}
                    + "' -- cannot resolve its addend width / bias.");
            }

            Interval const* iv = findInterval(ivs, rAddress);
            if (iv == nullptr) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "macho::readRelocatableObject: relocation at section offset "
                    + std::to_string(rAddress) + " in '" + sec.segName + ","
                    + sec.sectName + "' lies in no reconstructed "
                    + std::string{patchesText ? "function" : "data item"}
                    + " -- refusing to silently drop it.");
            }

            // Addend. Mach-O has no RELA addend column:
            //   * a DATA-section reloc's addend lives IN the patched slot bytes
            //     (widthBytes LE at r_address -- the writer's in-place
            //     convention); the target-schema addendBias is un-baked so a
            //     re-emission re-adds it once (0 for the non-pcrel absolute
            //     kinds a data slot uses -- schema invariant (c)).
            //   * a __text call/branch reloc carries addend 0 (the writer
            //     rejects a non-zero __text addend -- an arm64 instruction
            //     immediate cannot hold one in place).
            std::int64_t addend = 0;
            if (patchesData) {
                std::uint8_t const w = tri->widthBytes;
                if (w == 0u
                    || rangeExceedsBuffer(rAddress, w, sec.size)
                    || rangeExceedsBuffer(sec.offset, sec.size, bytes.size())) {
                    return fail(DiagnosticCode::F_CorruptedBinary,
                        "macho::readRelocatableObject: data relocation at "
                        "section offset " + std::to_string(rAddress) + " in '"
                        + sec.segName + "," + sec.sectName + "' has a "
                        + std::to_string(w) + "-byte slot that runs past the "
                        "section -- cannot read the in-place addend.");
                }
                std::uint64_t raw = 0;
                std::size_t const slot = static_cast<std::size_t>(sec.offset + rAddress);
                for (std::uint8_t b = 0; b < w; ++b) {
                    raw |= static_cast<std::uint64_t>(bytes[slot + b]) << (8u * b);
                }
                addend = signExtendLE(raw, w)
                       - static_cast<std::int64_t>(tri->addendBias);
            }

            Relocation rel;
            rel.offset = static_cast<std::uint32_t>(rAddress - iv->start);
            rel.target = SymbolId{rSymNum};
            rel.kind   = kind;
            rel.addend = addend;
            if (patchesText) mod.functions[iv->outIdx].relocations.push_back(rel);
            else             mod.dataItems[iv->outIdx].relocations.push_back(rel);

            // isData inference: an extern reached through a CALL/BRANCH-class
            // reloc (aarch64 BRANCH26) is a FUNCTION -- force isData=false. A
            // plain address reloc (PAGE21/PAGEOFF12/UNSIGNED) leaves the DATA
            // seed intact. Mirrors the ELF reader's reloc-typed extern rule.
            //
            // On a target whose Mach-O schema declares NO call-branch-formula
            // reloc AT ALL (x86_64 Mach-O: X86_64_RELOC_BRANCH is a plain
            // `linear` rel32, indistinguishable from a PC-relative data ref),
            // an extern reached by a reloc CANNOT be classified function-vs
            // -data -- the DATA seed would be a SILENT guess. Fail loud instead
            // (silent-failure-review fold): unreachable today (the shipped
            // x86_64 Mach-O schema is leaf-only, no extern-call dispatch, so no
            // extern is ever reached), but a future x86_64 Mach-O call leg must
            // confront the ambiguity, not inherit a silent misclassification.
            // The named follow-up is D-LK-MACHO-ISDATA-NO-CALL-SIGNAL.
            if (auto ex = externBySym.find(rSymNum); ex != externBySym.end()) {
                if (callSignalNativeIds.empty()) {
                    return fail(DiagnosticCode::F_UnsupportedBinaryFormat,
                        "macho::readRelocatableObject: extern '"
                        + mod.externImports[ex->second].mangledName
                        + "' is reached by a relocation, but Mach-O format '"
                        + std::string{objectFormatSchema.name()}
                        + "' declares no call-branch-formula relocation -- a "
                        "function-vs-data classification cannot be inferred "
                        "(D-LK-MACHO-ISDATA-NO-CALL-SIGNAL).");
                }
                if (callSignalNativeIds.contains(nativeId)) {
                    mod.externImports[ex->second].isData = false;
                }
            }
        }
    }

    mod.expectedFuncCount = mod.functions.size();
    return mod;
}

} // namespace dss::macho
