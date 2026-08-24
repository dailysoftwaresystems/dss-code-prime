#include "link/format/macho_object_reader.hpp"
#include "link/format/object_atom_coverage.hpp"
#include "link/format/object_format_backends.hpp"

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
constexpr std::size_t kHdrFlagsOff    = 24;

// mach_header_64.flags: MH_SUBSECTIONS_VIA_SYMBOLS (<mach-o/loader.h>). The
// producer's statement that "the sections in this file may be divided into
// individual blocks, and those blocks may be moved or dead-stripped
// independently" -- i.e. EVERY defined symbol starts its own atom unless it
// says otherwise with N_ALT_ENTRY. This is the flag that makes an object's
// symbol table SUFFICIENT to slice it, which is exactly what a size-less
// nlist_64 otherwise is not (D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM).
constexpr std::uint32_t kMhSubsectionsViaSymbols = 0x00002000u;

// nlist_64.n_type masks (<mach-o/nlist.h>).
constexpr std::uint8_t kNStabMask = 0xE0u;  // any stab bit -> debug entry
constexpr std::uint8_t kNPextBit  = 0x10u;  // private extern (Hidden)
constexpr std::uint8_t kNTypeMask = 0x0Eu;  // N_TYPE
constexpr std::uint8_t kNExtBit   = 0x01u;  // external
constexpr std::uint8_t kNTypeUndf = 0x00u;  // N_UNDF (undefined -> extern)
constexpr std::uint8_t kNTypeSect = 0x0Eu;  // N_SECT (defined in a section)

// nlist_64.n_desc bits (<mach-o/nlist.h>). N_ALT_ENTRY marks a defined symbol
// as an ALTERNATE ENTRY POINT of the atom that precedes it rather than the
// start of an atom of its own -- the `.alt_entry` assembler directive, and the
// ONLY discriminator Mach-O has between an interior label and a whole body,
// since nlist_64 carries no size field.
//
// ⚠ 0x0200, NOT 0x0020. ✔MEASURED 2026-08-20, not read off a table: clang 19
// targeting arm64-apple-macos assembling an explicit `.alt_entry _inner`
// directive emits n_desc=0x0200 for `_inner`, while a `static` C function
// carrying `__attribute__((used))` -- which lowers to `.no_dead_strip` --
// emits n_desc=0x0020 in the SAME object. 0x0020 is N_NO_DEAD_STRIP, named
// here because it is the near-miss that would invert this reader on precisely
// the input this work exists to read: a `used` file-local helper (the shape a
// `static` function needs to survive -O2) would be classified as an interior
// label and its bytes dropped.
constexpr std::uint16_t kNDescAltEntry    = 0x0200u;  // N_ALT_ENTRY
constexpr std::uint16_t kNDescNoDeadStrip = 0x0020u;  // N_NO_DEAD_STRIP (NOT alt-entry)
// N_WEAK_DEF -- "coalesed symbol is a weak definition" in Apple's own header.
// The READ half of D-LK-OBJECT-WEAK-DEF-RELOCATABLE: it is the whole of
// Mach-O's weak-definition machinery at this tier, and lifting it is what
// makes a weak definition survive a DSS write/read round trip instead of
// coming back Global. Without this the writer's N_WEAK_DEF would be a bit
// nobody reads, which is indistinguishable from not emitting it.
// ✔MEASURED 2026-08-20 against the installed MacOSX.sdk `<mach-o/nlist.h>`
// (`#define N_WEAK_DEF 0x0080`) AND against a `/usr/bin/clang -c` object, whose
// `__attribute__((weak))` function reads n_type=0x0f n_desc=0x0080.
constexpr std::uint16_t kNDescWeakDef     = 0x0080u;  // N_WEAK_DEF
static_assert(kNDescWeakDef != kNDescNoDeadStrip
              && kNDescWeakDef != kNDescAltEntry,
              "N_WEAK_DEF (0x0080) is a distinct n_desc bit from "
              "N_NO_DEAD_STRIP (0x0020) and N_ALT_ENTRY (0x0200)");
static_assert(kNDescAltEntry != kNDescNoDeadStrip,
              "N_ALT_ENTRY (0x0200) and N_NO_DEAD_STRIP (0x0020) are different "
              "bits -- confusing them silently drops a `used` static function's "
              "body (D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM)");

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

// ── THE "EXTERN IS A FUNCTION" SIGNAL IS DECLARED, NOT DERIVED ──────────
//
// This file used to carry an `isCallBranchFormula` helper that answered the
// question from the TARGET row's arithmetic formula (`Aarch64Call26`). It is
// GONE (D-LK-MACHO-ISDATA-NO-CALL-SIGNAL), and it must not come back as a
// fallback: a formula describes ARITHMETIC, and the question is about ROLE.
// The proxy held on AArch64 only by accident -- that CPU's branch has its own
// instruction encoding, so its formula happens to be branch-specific. On
// x86_64 it fails BY CONSTRUCTION: `S + A - P` is the identical arithmetic for
// a call and for a PC-relative data reference, so `linear` is the honest
// formula and carries no role at all. Every future target whose branch
// arithmetic is `linear` -- which is most of them -- would inherit the same
// defect from a fallback. The role now comes from the FORMAT row's `isCall`
// declaration (see `ObjectFormatRelocationInfo::isCall`), collected into
// `callSignalNativeIds` below.

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
    // The schema ROW the (segment, name) pair resolved to, or null when it
    // resolved to none. `kind` is a projection of it and stays for the dozen
    // read sites that only ever ask that question -- this pointer exists for
    // the sites that need a field the row owns and the enum cannot carry
    // (`entrySize`, today). Points into `ObjectFormatSchema::sections()`, which
    // outlives this reader's call by construction (the schema is a caller
    // parameter). D-LK-MACHO-COMPACT-UNWIND-SECTION-REFUSED-BLOCKS-EVERY-STOCK-MACOS-ARCHIVE.
    ObjectFormatSectionInfo const* schemaRow = nullptr;
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
    // Set only for a symbol the GEOMETRY FALLBACK promoted: it was first
    // recorded as a bodiless `ModuleSymbol` and only later found to start a
    // body, so the slicing loop must not push a SECOND one. Suppressing the
    // duplicate here rather than de-duplicating afterwards keeps `mod.symbols`
    // in symbol-table order.
    bool moduleSymbolAlreadyPushed = false;
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
// LE in the patched data slot (macho.cpp's IN-PLACE data-slot addend
// convention -- see `buildDataRelocTable`'s block comment: Mach-O has no
// RELA addend column, so a DATA slot carries its own addend and the final
// value is S + slot). width is 4 or 8 (the
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
    // ── SELF-GUARD (D-LINK-…-KIND-IDENTITY-BRANCHES, TF-C125) ──────────
    //
    // ★★ THIS GUARD SURVIVED THE IDENTITY-BRANCH REMOVAL, AND THE REASON IS
    // MEASURED FOR THIS SITE. The TF-C125 brief expected it to become
    // redundant: with walkers reached only through a backend the loader
    // resolved, a walker "can never be handed a schema of another kind", so
    // the guard would be unreachable by construction and safely deletable.
    //
    // That premise is FALSE here. `macho::readRelocatableObject` is a PUBLIC free function with
    // 10 direct call sites in `tests/`, none of which route through the
    // linker — and `MachoObjectReader.NonMachOFormatSchemaFailsLoud`
    // (tests/link/test_macho_object_reader.cpp) hands it a FOREIGN schema on purpose and asserts this
    // exact refusal. Deleting the guard would not remove dead code; it would
    // delete tested behaviour and leave a public entry point that mis-encodes
    // silently. Refused, with evidence.
    //
    // ⚠ THE CITATION ABOVE IS PER-SITE ON PURPOSE. The first version of this
    // comment was one block pasted into all eight guards, every copy naming
    // the ELF writer's test as its proof — so seven of the eight cited a
    // measurement that was not about them. An independent audit caught it.
    // A comment stamped MEASURED that names the wrong measurement is worse
    // than no comment, under this project's own rule.
    //
    // What it stops being is an IDENTITY branch. It no longer compares an
    // enumerator; it compares the schema's resolved backend against the
    // singleton THIS TU implements — a pointer identity on an opaque handle,
    // in the sanctioned realization tier, which is exactly the tier permitted
    // to know which format it is. Unreachable from the linker (the resolver
    // cannot produce a mismatched pair), live for every direct caller.
    if (objectFormatSchema.backend() != &link::format::machoBackend()) {
        return fail(DiagnosticCode::F_UnsupportedBinaryFormat,
            std::string{"macho::readRelocatableObject: object format schema '"}
                + std::string{objectFormatSchema.name()} + "' is kind "
                + std::string{link::objectFormatBackendName(objectFormatSchema.backend())}
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
        // Reject FAT/universal, big-endian, and 32-bit up front -- this
        // reader is 64-bit LE Mach-O only. Stated as ON-DISK BYTES,
        // because `rdU32` is little-endian and the three do not agree on
        // byte order: 32-bit thin is CE FA ED FE (LE-reads 0xFEEDFACE),
        // big-endian thin is FE ED FA CE (LE-reads 0xCEFAEDFE), and a
        // universal header is CA FE BA BE / CA FE BA BF -- which
        // LE-reads 0xBEBAFECA / 0xBFBAFECA, NOT the 0xCAFEBABE the
        // format names it by, since `fat_header` is big-endian on disk.
        // (See ffi/binary_readers/reader_common.hpp: naming the wrong
        // one of that pair in an equality test is a bug that already
        // happened once.) All three fall out of the single
        // `!= 0xFEEDFACF` gate above; none needs its own arm.
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
    // The producer's own statement about how its symbol table may be read --
    // see `kMhSubsectionsViaSymbols` and step (6)'s classification.
    bool const subsectionsViaSymbols =
        (rdU32(bytes, kHdrFlagsOff) & kMhSubsectionsViaSymbols) != 0u;
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
    std::unordered_map<std::string, ObjectFormatSectionInfo const*> pairToRow;
    for (auto const& row : objectFormatSchema.sections()) {
        pairToRow.emplace(pairKey(row.segment, row.name), &row);
    }
    for (auto& sec : sections) {
        if (auto it = pairToRow.find(pairKey(sec.segName, sec.sectName));
            it != pairToRow.end()) {
            sec.schemaRow = it->second;
            sec.kind      = it->second->kind;
        }
    }

    // -- (3b) UNWIND METADATA: say what is being left behind ----------
    //
    // D-LK-MACHO-COMPACT-UNWIND-SECTION-REFUSED-BLOCKS-EVERY-STOCK-MACOS-ARCHIVE.
    // A `SectionKind::Unwind` section is READ (its shape is now classified, so
    // the object links) but NOT CARRIED: DSS's image-side unwind table is built
    // from the neutral `CfiFunction` vocabulary that its own producers attach to
    // each `AssembledFunction`, and a foreign object states the same facts in
    // its format's own encoding, which nothing in this pipeline converts. So the
    // merged functions arrive in the image with no unwind description.
    //
    // ★★ THE ONE THING THIS MUST NOT BE IS QUIET. A dropped unwind table
    //    produces a binary that links, runs, and cannot be unwound -- no
    //    backtrace, no profiler stack, an exception thrown through the frame
    //    terminates -- and that is invisible until a core dump. It is exactly
    //    the failure `K_UnwindRuleUnrepresentable`'s own docblock was minted to
    //    name, so this borrows that code rather than inventing a synonym.
    //    ⓘ WARNING, not Error, and the split is the whole point of the row: the
    //    OBJECT is well-formed and every byte DSS does carry is correct, so
    //    refusing it would reject the ordinary output of the platform's own
    //    compiler over a capability gap. `--warnings-as-errors` is the knob for
    //    a build that will not accept the gap.
    //
    // ⚠ THE GAP IS FORMAT-WIDE AND PRE-EXISTING, NOT SOMETHING MACH-O DOES
    // WORSE. ✔MEASURED 2026-08-24: a `gcc -c` ELF member merged through
    // `--resolve-library` links and RUNS (exit 42), and `readelf
    // --debug-dump=frames` on the image shows ONE FDE -- for DSS's own `main`
    // -- while all three merged foreign functions have none. The ELF path drops
    // the same information in SILENCE today. ⇒
    // D-LK-MERGED-FOREIGN-FUNCTIONS-CARRY-NO-UNWIND-INFO-IN-THE-IMAGE.
    //
    // Counted from the SCHEMA's `entrySize`, never from a record layout typed
    // into this file: the row declares how wide one function's unwind record is,
    // so the count in the message is the format document's own arithmetic. A
    // body that is not a whole number of records is a corrupt object and fails
    // LOUD -- a fractional record means this reader and the producer disagree
    // about the section, and a warning that names a wrong count is worse than
    // no count.
    for (auto const& sec : sections) {
        if (!sec.kind.has_value() || *sec.kind != SectionKind::Unwind) continue;
        if (sec.size == 0u) continue;   // nothing described, nothing to say
        std::string const where = sec.segName + "," + sec.sectName;
        std::uint64_t const entry =
            sec.schemaRow != nullptr ? sec.schemaRow->entrySize : 0u;
        if (entry == 0u || (sec.size % entry) != 0u) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "macho::readRelocatableObject: unwind section '" + where
                + "' is " + std::to_string(sec.size) + " bytes, which is not a "
                "whole number of " + std::to_string(entry) + "-byte records as "
                "object format '" + std::string{objectFormatSchema.name()}
                + "' declares in that section's `entrySize` -- refusing to "
                "report a record count this reader and the producer do not "
                "agree on.");
        }
        report(reporter, DiagnosticCode::K_UnwindRuleUnrepresentable,
               DiagnosticSeverity::Warning,
               "macho::readRelocatableObject: section '" + where + "' carries "
               + std::to_string(sec.size / entry) + " function unwind "
               "record(s) in this object's own encoding. DSS reads the section "
               "-- so the object links -- but builds its image unwind table "
               "only from the neutral call-frame information its own producers "
               "attach, and converts no foreign encoding into it. The "
               "function(s) this object contributes will therefore appear in "
               "the image with NO unwind description: a backtrace stops at "
               "them and an exception thrown through them terminates. Anchored: "
               "D-LK-MERGED-FOREIGN-FUNCTIONS-CARRY-NO-UNWIND-INFO-IN-THE-IMAGE.");
    }

    // -- (4) Reverse reloc map (nativeId -> RelocationKind), from the
    //         FORMAT SCHEMA -- no hardcoded ARM64_RELOC_* numbers ---------
    //
    // `callSignalNativeIds` collects the native ids that PROVE an extern
    // reached through them is a FUNCTION: the rows the FORMAT declares
    // `"isCall": true` on -- i.e. wire relocations that can only ever target
    // executable code (ARM64_RELOC_BRANCH26, X86_64_RELOC_BRANCH). The
    // declaration is read, never inferred; see the note above the byte
    // helpers for why the target's arithmetic formula was the wrong
    // vocabulary for this question (D-LK-MACHO-ISDATA-NO-CALL-SIGNAL).
    //
    // Built by `ObjectFormatSchema::relocationDecodeTable()` — the SCHEMA's
    // own answer to "which rows decode, and which wire ids are call signals",
    // not a loop this reader owns. It used to be one, and it was WRONG in a
    // way the ELF reader's copy was not: it fed EVERY row into the map,
    // including an `emitOnly` EMISSION ALIAS, which `validate()` guarantees
    // carries a different `kind` from the row owning its wire id -- so a
    // Mach-O document declaring one would have been refused as an ambiguous
    // reverse map, rejecting every object of that format. LOUD, and no shipped
    // Mach-O document declares one, so nothing ever mis-decoded; the alias
    // capability was simply unavailable here. Also gone with the copy: the
    // omitted `pltNativeId` leg. Mach-O declares no PLT-variant id (an extern
    // call is BRANCH26/BRANCH against the same id whether or not ld64
    // synthesizes a stub), so the shared builder's leg costs nothing on every
    // shipped document -- but it is now read from the SCHEMA rather than
    // assumed absent by this reader.
    auto decode = objectFormatSchema.relocationDecodeTable();
    if (!decode) {
        return fail(DiagnosticCode::F_CorruptedBinary,
                    "macho::readRelocatableObject: " + decode.error());
    }
    auto const& nativeToKind        = decode->nativeToKind;
    auto const& callSignalNativeIds = decode->callSignalNativeIds;

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
    // Defined N_SECT symbols that START AN ATOM, grouped by 1-based n_sect
    // ordinal -- sliced by sorted n_value in the per-section pass below. Which
    // symbols qualify is decided in the loop and depends on whether the object
    // declares MH_SUBSECTIONS_VIA_SYMBOLS (see there).
    std::unordered_map<std::uint8_t, std::vector<DefSym>> defsBySection;
    // The DEMOTED half of that split, staged for the shared coverage guard --
    // see `link/format/object_atom_coverage.hpp`.
    std::vector<link::format::BodilessDefinedSymbol> bodilessDefined;
    // Mach-O's own coordinates alongside the neutral staging (kept HERE, not in
    // the shared struct, which is deliberately nothing but
    // `(sectionKey, byteOffset)`): the symtab index the fallback needs to turn a
    // promotion back into an atom, and whether this symbol is even ELIGIBLE for
    // the fallback -- see the staging site.
    std::vector<std::uint32_t>                       bodilessSymIdx;
    std::vector<bool>                                bodilessFallbackEligible;

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

        // ── IS THAT EVEN A QUESTION FOR THIS SECTION? ─────────────
        //
        // D-LK-MACHO-COMPACT-UNWIND-SECTION-REFUSED-BLOCKS-EVERY-STOCK-MACOS-ARCHIVE.
        // Everything below -- the atom-boundary rule, the bodiless-label
        // fallback, the coverage staging -- presumes the section HOLDS BYTES
        // THE IMAGE WILL CARRY. A classified section that carries no linkable
        // body does not: its content describes OTHER sections' code and is
        // consumed by the linker. Apple's clang puts a local label (`ltmp1`) at
        // offset 0 of `__LD,__compact_unwind` and sets MH_SUBSECTIONS_VIA_SYMBOLS
        // on the header, so that label is indistinguishable from an atom
        // boundary by the rule alone -- and treating it as one is what drove the
        // slicing loop into its "no known code/data section kind" refusal on
        // EVERY stock macOS object.
        //
        // ★ ONE GATE, THREE CONSEQUENCES, AND THE THIRD IS THE ONE MEASUREMENT
        //   CAUGHT. It mints no atom (the refusal above); it stages nothing for
        //   the coverage guard (a body no atom covers is the guard's whole
        //   subject, and this is not a body); and it publishes NO `ModuleSymbol`
        //   -- ✔MEASURED 2026-08-24, because the first version of this fix
        //   recorded the label and `nm -n` on the linked arm64 exec then showed
        //   `T ltmp1` AT THE ADDRESS OF DSS'S OWN ENTRY TRAMPOLINE -- where the
        //   same link with this gate in place shows the trampoline's own
        //   `_sym_*` name, and a no-archive control shows it too. The program
        //   still ran (nothing references the label), so that was a green build
        //   shipping a symbol table which states something false about the
        //   artifact.
        //
        // ★ THE PREDICATE IS THE TAXONOMY'S, NOT THIS READER'S
        //   (`sectionKindCarriesLinkableBody`), so a future kind is classified
        //   where the kinds live. This file must never test a section NAME.
        // ⚠ `kind.has_value()` IS LOAD-BEARING AND IS NOT THE SAME TEST. An
        //   UNRESOLVED section keeps the old path deliberately, so a symbol in a
        //   section no format row describes still reaches the slicing loop's
        //   loud refusal. "Classified as holding no body" and "unclassified" are
        //   different claims, and only the first one licenses skipping.
        // ⓘ A REFERENCE TO SUCH A LABEL NOW FAILS LOUD RATHER THAN BINDING
        //   WRONG: an r_extern=1 relocation naming it resolves to nothing and
        //   the link reports `K_SymbolUndefined` -- correct, because DSS
        //   genuinely did not carry the bytes that reference points into.
        if (sec.kind.has_value() && !sectionKindCarriesLinkableBody(*sec.kind)) {
            continue;
        }

        // ── Does this defined symbol START AN ATOM? ────────────────
        //
        // D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM. The
        // question Mach-O cannot answer from one nlist alone: a whole
        // file-local (`static`) FUNCTION and an interior `&&label` block symbol
        // are the SAME three numbers (bare N_SECT, a section ordinal, an
        // offset) because nlist_64 has no size field. Reading EXTERNAL-ness as
        // the answer -- which this reader used to do -- demotes every `static`
        // function to a bodiless label and drops its bytes.
        //
        // The format answers it with a PAIR of its own fields, and this reader
        // now honours both:
        //   * MH_SUBSECTIONS_VIA_SYMBOLS in the mach_header (the PRODUCER's
        //     declaration that its symbols carve the section into independently
        //     movable blocks), and
        //   * N_ALT_ENTRY in n_desc (the PER-SYMBOL exception: "I am an
        //     alternate entry into the atom before me, not an atom").
        // Apple's own clang sets the header flag on every ordinary `.o`, so
        // this is the rule a real archive member is written under -- not a DSS
        // convention. DSS's writer now emits the same pair (`macho.cpp`: the
        // flag comes from the format schema's `MachOIdentity::flags`, and the
        // synthetic per-block label loop stamps N_ALT_ENTRY).
        //
        // WITHOUT the flag the reader keeps the OLD, narrower rule: external =>
        // atom, otherwise => bodiless. That is not a guess about what the
        // producer meant -- with no subsection declaration the section is one
        // atom by definition, so a local symbol inside it is genuinely
        // ambiguous, and the shared coverage guard below is what refuses to let
        // the ambiguity cost bytes silently. Inventing a geometry rule here
        // would be inventing evidence the object does not carry.
        //
        // ⚠ N_ALT_ENTRY is honoured for EXTERNAL symbols too, not just locals:
        // `.alt_entry` + `.globl` on one label is legal and means the same
        // thing (✔MEASURED: clang emits n_desc=0x0200 on an external
        // `.alt_entry` symbol). Gating it on locals would slice an atom in half
        // at an alternate entry point.
        bool const altEntry = (s.desc & kNDescAltEntry) != 0u;
        bool const startsAtom = subsectionsViaSymbols ? !altEntry : isExt;

        if (startsAtom && (isExt || !s.name.empty())) {
            // An atom boundary. BINDING IS NOT VISIBILITY AND NOT ATOM-NESS: a
            // promoted file-local body is Local, so `resolveCrossCuDefs` skips
            // it ("module-private -- excluded") and a `static` helper can never
            // satisfy a SIBLING translation unit's extern. Global would make
            // two TUs' `static` helpers a redefinition conflict; Weak would let
            // one member's body be silently dropped as a shadowed duplicate.
            // Its own CU still resolves it: `buildCompoundIndex` declares every
            // `AssembledFunction` / `AssembledData` by SymbolId regardless of
            // binding.
            // N_WEAK_DEF lifts an EXTERNAL definition to Weak -- the READ half
            // of D-LK-OBJECT-WEAK-DEF-RELOCATABLE, the inverse of the writer's
            // `definedNDesc`. It is honoured only for an EXTERNAL symbol: the
            // Local arm above is a deliberate, load-bearing choice (a file-
            // local body must stay module-private), and a coalescible
            // file-local symbol is not a shape any producer emits.
            SymbolBinding const defBinding =
                !isExt                            ? SymbolBinding::Local
                : (s.desc & kNDescWeakDef) != 0u  ? SymbolBinding::Weak
                                                  : SymbolBinding::Global;
            defsBySection[s.sect].push_back(
                DefSym{i, secRelOff, s.name, defBinding,
                       machoVisibility(s.type)});
        } else if (!s.name.empty()) {
            // NOT an atom boundary -- an interior label. Either the object
            // declares subsections and this symbol says N_ALT_ENTRY, or it
            // declares none and this is the un-discriminated local case.
            // Recorded as a LOCAL ModuleSymbol so a relocation can still resolve
            // it by identity, but it never splits the function that contains its
            // interior offset (mirrors the ELF reader treating interior text
            // labels as ModuleSymbol only). Its interior-VA binding is the named
            // follow-up D-LINK-OBJECT-READERS-DROP-INTERIOR-SYMBOL-OFFSET.
            //
            // The binding is forced Local even for an EXTERNAL N_ALT_ENTRY
            // symbol, and that is deliberate: this reader cannot yet represent
            // "an interior offset within an atom", so publishing the name
            // cross-CU would bind a sibling's reference to the ATOM START --
            // a silent wrong address. Kept module-private, a sibling reference
            // instead fails loud (`K_SymbolUndefined`, nothing declares it),
            // which is the correct posture until the interior-VA follow-up lands.
            mod.symbols.push_back(ModuleSymbol{SymbolId{i}, s.name,
                SymbolBinding::Local, machoVisibility(s.type)});
            // ...and STAGE it for the coverage guard
            // (D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM).
            // A label lies INSIDE the function that contains it, so the
            // enclosing atom covers its offset and the guard stays silent; a
            // demoted whole function is covered by nothing, and the guard names
            // it rather than letting its bytes vanish. The staging is kept for
            // BOTH arms deliberately: an N_ALT_ENTRY symbol that turns out to be
            // covered by nothing means the producer's own subsection claim did
            // not hold, and that must be loud too. Only a KIND-RESOLVED section
            // is staged -- a symbol in an unmodeled metadata section
            // reconstructs no body BY DESIGN and is not a dropped body.
            // ⓘ Its CLASSIFIED sibling -- a section that resolved to a kind
            // carrying no linkable body -- never reaches this line at all; it
            // left the loop at the gate above.
            if (sec.kind.has_value()) {
                bodilessDefined.push_back(link::format::BodilessDefinedSymbol{
                    /*sectionKey=*/s.sect, /*sectionOffset=*/secRelOff,
                    /*declaredSize=*/std::nullopt, /*name=*/s.name,
                    /*sectionName=*/sec.segName + "," + sec.sectName});
                bodilessSymIdx.push_back(i);
                // ★★★ WHERE THE GEOMETRY FALLBACK SITS RELATIVE TO THE WIRE, and
                // it is BELOW it, never over it. Mach-O now carries a real
                // declaration -- MH_SUBSECTIONS_VIA_SYMBOLS in the header plus
                // N_ALT_ENTRY per symbol -- and geometry is an INFERENCE. An
                // inference must never overrule a producer that spoke:
                //   * `subsectionsViaSymbols && altEntry` -- the object said, in
                //     its own vocabulary, "this symbol is an alternate entry
                //     into the atom before me". Promoting it would SPLIT that
                //     atom at an interior entry point, which is the exact
                //     miscompile the run rule exists to avoid, except committed
                //     against an explicit statement rather than a guess.
                //   * `altEntry` WITHOUT the header flag -- still a positive
                //     per-symbol declaration, and honouring it only ever
                //     promotes LESS. Conservative in the safe direction.
                //   * neither -- the object said NOTHING about this symbol, so
                //     the demotion above rests on no evidence at all, and
                //     geometry is the only evidence there is.
                // ⚠ The STAGING is unconditional even so: an N_ALT_ENTRY symbol
                // that no atom covers means the producer's own subsection claim
                // did not hold, and that must still reach the post-condition and
                // fail LOUD rather than be quietly promoted into agreement.
                bodilessFallbackEligible.push_back(!altEntry);
            }
        }
    }

    // Per-section interval lists for relocation-site routing.
    std::unordered_map<std::uint8_t, std::vector<Interval>> funcIntervalsBySec;
    std::unordered_map<std::uint8_t, std::vector<Interval>> dataIntervalsBySec;

    auto pushModuleSym = [&](DefSym const& d) {
        // A geometry-promoted symbol already has its ModuleSymbol from the
        // classification loop (see `DefSym::moduleSymbolAlreadyPushed`).
        if (!d.name.empty() && !d.moduleSymbolAlreadyPushed) {
            mod.symbols.push_back(ModuleSymbol{SymbolId{d.symIdx}, d.name,
                                               d.binding, d.visibility});
        }
    };

    // Order each section's boundary set by offset, then by SYMTAB INDEX. The
    // index tie-break is not cosmetic: `std::sort` is not stable, so with offset
    // alone the ORDER OF EQUAL-OFFSET ALIASES was unspecified -- and that order
    // decides which atom `findInterval` hands a relocation whose site lies in
    // the span they share. Ties are common in foreign objects (clang's
    // section-start `ltmp0` sits at the same offset as the first function) and
    // impossible in DSS's own output, where every atom has a distinct offset.
    // Hoisted out of the slicing loop because (6.4) below reads the SAME order,
    // and it runs TWICE (again after (6.4) appends).
    auto sortBoundaries = [&] {
        for (auto& [ordinal, defs] : defsBySection) {
            std::sort(defs.begin(), defs.end(),
                      [](DefSym const& a, DefSym const& b) {
                          if (a.secRelOff != b.secRelOff) return a.secRelOff < b.secRelOff;
                          return a.symIdx < b.symIdx;
                      });
        }
    };
    sortBoundaries();

    // THE ATOM EXTENT RULE, stated once (THE key inversion -- nlist_64 has no
    // size field, so an atom's END comes from the NEXT boundary rather than from
    // the symbol). The k-th boundary of a SORTED `defs` ends at the next
    // STRICTLY-GREATER offset -- skipping equal-offset ALIASES so they share the
    // span, the ELF equal-start rule -- else at section.size.
    //
    // (6.4) and the slicing loop must agree on this EXACTLY: (6.4) decides which
    // symbols to promote by asking which offsets these extents cover, and a
    // different rule there would promote a symbol on the strength of an atom
    // that never materialises (or leave one alone on the strength of an atom
    // that does not reach it).
    auto atomEndFor = [](std::vector<DefSym> const& defs, std::size_t k,
                         std::uint64_t sectionSize) -> std::uint64_t {
        std::uint64_t const off = defs[k].secRelOff;
        for (std::size_t j = k + 1; j < defs.size(); ++j) {
            if (defs[j].secRelOff > off) return defs[j].secRelOff;
        }
        return sectionSize;
    };

    // -- (6.4) GEOMETRY FALLBACK: recover a body the wire could not name ---
    //
    // D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM, operator
    // ruling 2026-08-20: the fallback lands on ALL THREE readers. It applies to
    // exactly the symbols this object said NOTHING about -- see the eligibility
    // note at the staging site, which is where the relationship between this
    // inference and the MH_SUBSECTIONS_VIA_SYMBOLS / N_ALT_ENTRY facts is
    // decided. The shared header owns the inference and the argument for it
    // (`uncoveredDefinedSymbolsThatStartAnAtom`).
    //
    // ⚠ THE EXTENTS FED IN ARE PROSPECTIVE, computed from the boundary set
    // decided above with `atomEndFor` -- they must be the ones the slicer will
    // actually produce, which is why the extent rule was hoisted rather than
    // re-derived here. Promoting BEFORE slicing (rather than slicing, promoting
    // and re-slicing) means every body is cut exactly once and the bounds checks
    // below run over the final boundary set.
    {
        std::vector<link::format::BodilessDefinedSymbol> eligible;
        std::vector<std::size_t>                        eligibleOrigin;
        for (std::size_t i = 0; i < bodilessDefined.size(); ++i) {
            if (!bodilessFallbackEligible[i]) continue;
            eligible.push_back(bodilessDefined[i]);
            eligibleOrigin.push_back(i);
        }
        std::vector<link::format::ReconstructedAtomExtent> prospective;
        for (auto const& [ordinal, defs] : defsBySection) {
            std::uint64_t const secSize = sections[ordinal - 1u].size;
            for (std::size_t k = 0; k < defs.size(); ++k) {
                std::uint64_t const off = defs[k].secRelOff;
                std::uint64_t const end = atomEndFor(defs, k, secSize);
                // A corrupt offset past the section end is the SLICER's refusal
                // to make (it names the symbol and the size); skip it here
                // rather than fabricate a reversed extent.
                if (off > secSize || end < off) continue;
                prospective.push_back(link::format::ReconstructedAtomExtent{
                    ordinal, off, end - off});
            }
        }
        for (std::size_t e :
             link::format::uncoveredDefinedSymbolsThatStartAnAtom(eligible,
                                                                  prospective)) {
            auto const& c = bodilessDefined[eligibleOrigin[e]];
            // `Local`, for the same reason the boundary arm above spells out:
            // `resolveCrossCuDefs` skips Local, which is what stops a `static`
            // helper satisfying a sibling TU's extern. A symbol this reader
            // could name no reason for is never a reason to change linkage.
            defsBySection[static_cast<std::uint8_t>(c.sectionKey)].push_back(
                DefSym{bodilessSymIdx[eligibleOrigin[e]], c.sectionOffset, c.name,
                       SymbolBinding::Local, SymbolVisibility::Default,
                       /*moduleSymbolAlreadyPushed=*/true});
        }
        sortBoundaries();
    }

    // -- (6.44) EQUAL-OFFSET ALIAS IDENTITY: one atom, several names ------
    //
    // D-LINK-EQUAL-OFFSET-DEFINED-SYMBOLS-BECOME-TWIN-ATOMS. The boundary set is
    // FINAL here -- the wire's own plus whatever (6.4) recovered -- and this is
    // the last moment before bytes are cut, which is where the question belongs:
    // a name that aliases another must never mint a second body.
    //
    // ✔MEASURED, clang 19 (`-target arm64-apple-macos11 -c -O1`) on a three-line
    // C file: `ltmp0` (local, n_value 0) and the first EXTERNAL function
    // (n_value 0) are both boundaries, with that function's own `bl` relocation
    // at offset 8 inside the span they share. Two atoms over one span means
    // `findInterval` hands that reloc to exactly ONE of them and the other ships
    // an un-patched branch. The shared header owns the rule, the ranking, and
    // the argument for both (`resolveEqualOffsetAtomAliases`).
    //
    // Mach-O passes `declaredExtent = nullopt` because `nlist_64` has no size
    // field -- this reader derives an atom's end from the next boundary
    // (`atomEndFor`), so equal-offset candidates get equal extents by
    // construction and the conflicting-extent refusal cannot fire here.
    std::unordered_map<std::uint32_t, std::uint32_t> atomOwnerBySym;
    {
        std::vector<link::format::AtomStartCandidate> candidates;
        for (auto const& [ordinal, defs] : defsBySection) {
            Section const& sec = sections[ordinal - 1u];
            for (auto const& d : defs) {
                candidates.push_back(link::format::AtomStartCandidate{
                    /*sectionKey=*/ordinal, /*offset=*/d.secRelOff,
                    /*declaredExtent=*/std::nullopt, /*symbolId=*/d.symIdx,
                    /*binding=*/d.binding, /*visibility=*/d.visibility,
                    /*name=*/d.name,
                    /*sectionName=*/sec.segName + "," + sec.sectName});
            }
        }
        std::vector<std::uint32_t> owner;
        if (!link::format::resolveEqualOffsetAtomAliases(
                candidates, owner, "macho::readRelocatableObject", reporter)) {
            return std::nullopt;   // the resolver reported, naming both symbols
        }
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (owner[i] != candidates[i].symbolId) {
                atomOwnerBySym.emplace(candidates[i].symbolId, owner[i]);
            }
        }
    }
    // The atom identity a symbol resolves to: itself unless it aliases another.
    // Consulted at BOTH sites that need it -- the slicing loop (does this
    // boundary mint a body?) and the relocation pass (which atom does this
    // target name?) -- because a collapse without the target remap turns a
    // silent miscompile into a spurious `K_SymbolUndefined`.
    auto ownerOf = [&](std::uint32_t symIdx) -> std::uint32_t {
        auto const it = atomOwnerBySym.find(symIdx);
        return it == atomOwnerBySym.end() ? symIdx : it->second;
    };
    // Alias rows are appended AFTER the slicing loop, never during it: every
    // id -> row lookup over `AssembledModule::symbols` keeps the FIRST row for
    // an id, so the canonical name must be recorded before any alias of it.
    std::vector<ModuleSymbol> aliasRows;

    // Slice each section's atoms by SORTED n_value. What reached
    // `defsBySection` is every defined symbol that STARTS A BODY: the wire's
    // own boundary set, plus whatever (6.4) recovered.
    for (auto& [ordinal, defs] : defsBySection) {
        Section const& sec = sections[ordinal - 1u];
        std::optional<SectionKind> const rk = sec.kind;
        std::optional<DataSectionKind> const dk =
            rk.has_value() ? dataSectionKindOf(*rk) : std::nullopt;
        bool const isText = rk.has_value() && *rk == SectionKind::Text;

        for (std::size_t k = 0; k < defs.size(); ++k) {
            std::uint64_t const off = defs[k].secRelOff;
            std::uint64_t const end = atomEndFor(defs, k, sec.size);
            if (off > sec.size || end < off) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "macho::readRelocatableObject: defined symbol '"
                    + defs[k].name + "' offset " + std::to_string(off)
                    + " exceeds its section '" + sec.segName + "," + sec.sectName
                    + "' size " + std::to_string(sec.size) + ".");
            }
            std::uint64_t const len = end - off;

            // (6.44) THE ALIAS ARM: this boundary names a body another boundary
            // at the same offset already owns, so it mints NO atom -- it keeps
            // its NAME and takes the owner's identity. Placed after the offset
            // refusal so a corrupt offset is still named by the symbol that
            // carries it.
            if (std::uint32_t const owner = ownerOf(defs[k].symIdx);
                owner != defs[k].symIdx) {
                if (defs[k].moduleSymbolAlreadyPushed) {
                    // A (6.4) geometry promotion that turned out to alias. It
                    // cannot happen today -- the fallback promotes only symbols
                    // NO reconstructed atom covers, and an atom starting at this
                    // very offset covers it -- but if it ever does, the row it
                    // already pushed carries the WRONG id and retargeting it is
                    // the correct repair, not skipping it.
                    for (auto& ms : mod.symbols) {
                        if (ms.symbol == SymbolId{defs[k].symIdx}) {
                            ms.symbol = SymbolId{owner};
                            break;
                        }
                    }
                } else if (!defs[k].name.empty()) {
                    aliasRows.push_back(ModuleSymbol{SymbolId{owner}, defs[k].name,
                                                     defs[k].binding,
                                                     defs[k].visibility});
                }
                continue;
            }

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

    // Every canonical row is now recorded, so the aliases can follow: several
    // names, one SymbolId, the owning name first (see (6.44)).
    for (auto& ms : aliasRows) mod.symbols.push_back(std::move(ms));

    // -- (6.5) Coverage guard: no defined symbol's body was dropped -------
    //
    // D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM. The
    // demotion above is the ONLY place this reader turns a defined body-bearing
    // symbol into a bodiless one, so this is the first point at which the
    // question "did any named bytes end up in no atom?" is answerable. Asked
    // BEFORE the relocation pass so the refusal names the SYMBOL rather than
    // surfacing later as a reloc that routes to nothing (or, worse, not at all).
    // The check itself is shared and format-neutral -- see
    // `link/format/object_atom_coverage.hpp`.
    {
        std::vector<link::format::ReconstructedAtomExtent> extents;
        for (auto const* bySec : {&funcIntervalsBySec, &dataIntervalsBySec}) {
            for (auto const& [ordinal, ivs] : *bySec) {
                for (auto const& iv : ivs) {
                    extents.push_back(link::format::ReconstructedAtomExtent{
                        ordinal, iv.start, iv.len});
                }
            }
        }
        if (!link::format::everyDefinedSymbolIsCoveredByAnAtom(
                bodilessDefined, extents, "macho::readRelocatableObject",
                reporter)) {
            return std::nullopt;
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
        // ── A CLASSIFIED BODYLESS SECTION'S RELOCS GO WITH ITS BYTES ──
        //
        // D-LK-MACHO-COMPACT-UNWIND-SECTION-REFUSED-BLOCKS-EVERY-STOCK-MACOS-ARCHIVE.
        // A `SectionKind::Unwind` section's relocations exist to bind ITS OWN
        // record fields (each record's `functionStart`, and a personality or
        // LSDA pointer) to the code the records describe. Those bytes are not
        // reconstructed into any atom and never reach the image, so there is
        // nothing for the relocations to patch -- routing them anywhere would be
        // patching bytes that do not exist. They are consumed WITH the section,
        // and the loss is already stated once, at full volume and with a record
        // count, by the (3b) pass above; repeating it per relocation would bury
        // it. ⚠ THE TEST IS THE KIND, NEVER THE NAME, and never `nreloc == 0`:
        // an UNCLASSIFIED reloc-bearing section still falls through to the
        // refusal below, which is what keeps an unknown foreign section loud.
        if (sec.kind.has_value()
            && !sectionKindCarriesLinkableBody(*sec.kind)) {
            continue;
        }
        if (!patchesText && !patchesData) {
            // A reloc-bearing section that reconstructed NO atom. DSS output has
            // none -- every reloc it emits patches a __text function or a data
            // item, each an N_EXT atom. Fail loud rather than `continue`
            // (silent-failure-review fold): skipping would also bypass the
            // r_extern=0 guard below, so a foreign object's atom-less
            // section-relative relocs would vanish undiagnosed. ⓘ THE SECTION
            // THIS SENTENCE USED TO NAME IS NO LONGER ONE OF THEM: a
            // `__LD,__compact_unwind` now resolves to `SectionKind::Unwind` and
            // exits at the arm above, so what still reaches here is a section
            // whose kind NO format row describes -- anonymous `__cstring` /
            // jump-table content reached through a section symbol, which is the
            // gap-atom half of D-LK-MACHO-STATIC-SECTION-RELATIVE-RELOC.
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
            // (6.44): a target naming an ALIAS binds to the atom that owns the
            // body, because only the owner is a declared definition -- an id
            // that owns no body is `K_SymbolUndefined` at the linker's compound
            // index. The addend needs no adjustment: an alias shares its owner's
            // offset exactly, so the same S makes the same address.
            rel.target = SymbolId{ownerOf(rSymNum)};
            rel.kind   = kind;
            rel.addend = addend;
            if (patchesText) mod.functions[iv->outIdx].relocations.push_back(rel);
            else             mod.dataItems[iv->outIdx].relocations.push_back(rel);

            // isData inference: an extern reached through a relocation the
            // FORMAT declares `"isCall": true` on -- ARM64_RELOC_BRANCH26,
            // X86_64_RELOC_BRANCH -- is a FUNCTION, so force isData=false. A
            // plain address reloc (PAGE21/PAGEOFF12/UNSIGNED) leaves the DATA
            // seed intact. Mirrors the ELF reader's reloc-typed extern rule.
            // Mach-O's nlist_64 carries no STT_FUNC-style type hint, so this
            // relocation is the ONLY class signal a name-only reference offers.
            //
            // ── THE EMPTY-SET REFUSAL (D-LK-MACHO-ISDATA-NO-CALL-SIGNAL) ──
            // A Mach-O format that declares NO `isCall` row at all cannot
            // classify an extern it meets through a relocation, and the DATA
            // seed would then be a SILENT GUESS -- not a diagnostic, a wrong
            // answer. Refuse instead, naming the extern and the format.
            //
            // ⚠ THIS ARM IS NOT DEAD CODE AND IS NOT A HYPOTHETICAL. It is
            // what every macho64-x86_64 archive member that called a library
            // function hit until 2026-08-20, when the classification stopped
            // being derived from the target's arithmetic formula and started
            // being READ from the format's declared role. What changed is WHO
            // it can fire for: no shipped Mach-O format reaches it any more
            // (all eight declare their BRANCH row `isCall`), and it now guards
            // the case it should always have guarded -- a NEW Mach-O format
            // that forgets the declaration is refused rather than allowed to
            // guess. Its message therefore names the missing SCHEMA KEY, since
            // adding that key is the whole fix.
            if (auto ex = externBySym.find(rSymNum); ex != externBySym.end()) {
                if (callSignalNativeIds.empty()) {
                    return fail(DiagnosticCode::F_UnsupportedBinaryFormat,
                        "macho::readRelocatableObject: extern '"
                        + mod.externImports[ex->second].mangledName
                        + "' is reached by a relocation, but Mach-O format '"
                        + std::string{objectFormatSchema.name()}
                        + "' declares no relocation row with \"isCall\": true "
                        "-- a function-vs-data classification cannot be made, "
                        "and guessing DATA would silently mis-type the import "
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
