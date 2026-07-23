#include "link/format/coff_object_reader.hpp"

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

// Windows COFF `.obj` reader -- the inverse of pe.cpp's Obj-arm writer. See
// the header for the reconstruction contract + scope + the COFF-vs-ELF/
// Mach-O inversions. Every field is bounds-checked; any violation fails
// loud (F_* diagnostic + nullopt).

namespace dss::pe {

namespace {

using dss::report;

// -- PE/COFF structural constants (PE Format spec, Microsoft Learn) ----
//
// The SAME record layout the writer in pe.cpp hardcodes. Re-declared
// locally rather than #include-pulled from `ffi/binary_readers/*`: `ffi`
// already depends UP on `link` (`link/object_format_schema.hpp`), so a
// `link` -> `ffi` include would form a dependency cycle. This mirrors the
// c164 ELF + c168 Mach-O readers re-declaring the same constants to keep
// `ffi` off `link`.
constexpr std::size_t kFileHeaderSz    = 20;  // IMAGE_FILE_HEADER
constexpr std::size_t kSectionHeaderSz = 40;  // IMAGE_SECTION_HEADER
constexpr std::size_t kSymbolSz        = 18;  // IMAGE_SYMBOL
constexpr std::size_t kRelocSz         = 10;  // IMAGE_RELOCATION

// IMAGE_FILE_HEADER field offsets.
constexpr std::size_t kFhNumSectionsOff  = 2;   // u16 NumberOfSections
constexpr std::size_t kFhSymTabPtrOff    = 8;   // u32 PointerToSymbolTable
constexpr std::size_t kFhNumSymbolsOff   = 12;  // u32 NumberOfSymbols
constexpr std::size_t kFhOptHdrSizeOff   = 16;  // u16 SizeOfOptionalHeader

// IMAGE_SECTION_HEADER field offsets.
constexpr std::size_t kShNameOff          = 0;   // 8 bytes (inline or /N)
constexpr std::size_t kShSizeOfRawDataOff = 16;  // u32
constexpr std::size_t kShPtrRawDataOff    = 20;  // u32
constexpr std::size_t kShPtrRelocsOff     = 24;  // u32
constexpr std::size_t kShNumRelocsOff     = 32;  // u16
constexpr std::size_t kShCharsOff         = 36;  // u32

// IMAGE_SYMBOL field offsets.
constexpr std::size_t kSymNameOff    = 0;   // 8 bytes (inline or [0][offset])
constexpr std::size_t kSymValueOff   = 8;   // u32
constexpr std::size_t kSymSectNumOff = 12;  // i16 (read as u16, specials below)
constexpr std::size_t kSymTypeOff    = 14;  // u16
constexpr std::size_t kSymClassOff   = 16;  // u8
constexpr std::size_t kSymNumAuxOff  = 17;  // u8

// IMAGE_RELOCATION field offsets.
constexpr std::size_t kRelVirtAddrOff = 0;  // u32 (section-relative patch site)
constexpr std::size_t kRelSymIdxOff   = 4;  // u32 (symtab index)
constexpr std::size_t kRelTypeOff     = 8;  // u16 (== schema nativeId)

// IMAGE_SYM_CLASS_*
constexpr std::uint8_t kSymClassExternal = 2;
constexpr std::uint8_t kSymClassStatic   = 3;  // documented; a LOCAL block label

// IMAGE_SECTION_NUMBER specials (SectionNumber is a signed i16).
constexpr std::uint16_t kSymUndefined = 0x0000u;  // extern
constexpr std::uint16_t kSymAbsolute  = 0xFFFFu;  // -1
constexpr std::uint16_t kSymDebug     = 0xFFFEu;  // -2

// IMAGE_SYM_TYPE_* -- the high byte carries the derived (DTYPE) hint;
// The IMAGE_SYMBOL derived-type (bits 4..5): DTYPE_FUNCTION marks the symbol
// as a FUNCTION (the isData signal). The full 2-bit mask distinguishes it from
// DT_ARY (0x30) / DT_PTR (0x10), which are DATA -- a bare `& DTYPE_FUNCTION`
// would misread DT_ARY as a function.
constexpr std::uint16_t kSymDtypeMask     = 0x30u;
constexpr std::uint16_t kSymDtypeFunction = 0x20u;

// Section Characteristics: a zero-fill (bss) section stores NO file bytes
// (PointerToRawData == 0, the span rides SizeOfRawData). The structural
// COFF analog of Mach-O's S_ZEROFILL -- used ONLY for the file-bounds
// exemption; the section KIND is resolved from the schema name map.
constexpr std::uint32_t kScnCntUninitializedData = 0x00000080u;

// IMAGE_SCN_LNK_COMDAT: this section participates in COMDAT
// duplicate-resolution. A real cl.exe/clang-cl `.obj` places each
// function-level-linked (`/Gy`) / `__declspec(selectany)` / inline / template
// body in its OWN COMDAT section; the duplicate-resolution policy lives in the
// section-definition auxiliary record's Selection byte (decoded below --
// D-LK-COFF-COMDAT-UNSUPPORTED-SELECTION). DSS's own writer emits NO COMDAT
// sections, so this bit is only ever set by a FOREIGN object
// (D-LK-COFF-READER-FOREIGN-OBJECT).
constexpr std::uint32_t kScnLnkComdat = 0x00001000u;

// IMAGE_COMDAT_SELECT_* -- the COMDAT Selection byte (aux format 5, offset 14).
// Maps to the universal SymbolBinding the format-blind merge already resolves.
constexpr std::uint8_t kComdatSelNoDuplicates = 1;  // -> Strong (Global)
constexpr std::uint8_t kComdatSelAny          = 2;  // -> Weak
constexpr std::uint8_t kComdatSelSameSize     = 3;  // -> Weak
constexpr std::uint8_t kComdatSelExactMatch   = 4;  // -> Weak
// ASSOCIATIVE(5) / LARGEST(6) / 0 / unknown -> FAIL LOUD (see the decode).

// Section-definition auxiliary record (aux format 5) field offset: the
// Selection byte sits at offset 14 of the 18-byte aux record (after Length[4],
// NumberOfRelocations[2], NumberOfLinenumbers[2], CheckSum[4], Number[2]).
constexpr std::size_t kAuxSectionDefSelectionOff = 14;

// Is this target-schema reloc formula a FUNCTION CALL / BRANCH (as opposed
// to a data-address computation)? The AGNOSTIC "extern is a function"
// signal a call/branch reloc would give. PE x86_64 declares NO such
// formula (REL32 is `linear`), so `callSignalNativeIds` is EMPTY there and
// the extern's isData comes from the IMAGE_SYMBOL type hint instead (see
// the header). Kept for a hypothetical call-branch PE target (agnostic --
// the same aarch64 CALL26 formula the ELF/Mach-O readers key on).
[[nodiscard]] constexpr bool isCallBranchFormula(RelocFormulaKind k) noexcept {
    return k == RelocFormulaKind::Aarch64Call26;
}

// Overflow-safe [off, off+size) within [0, total) -- the c159-c168
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

// NUL-terminated name at strtab[index], bounded by [tabStart, tabEnd).
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

// Decode a COFF 8-byte name field at `o`. Two encodings (pe.cpp:467):
//   * INLINE: the name (<= 8 bytes) NUL-padded in the field -- the bytes up
//     to the first NUL (or all 8 if unterminated).
//   * OFFSET: the first 4 bytes are ZERO, the next 4 are a string-table
//     offset (>= 4, past the u32 size prefix) -- the name is the
//     NUL-terminated string there. An offset < 4 (only via a corrupt/empty
//     field) yields an empty name (the writer never emits offset < 4).
// The 8-byte field's in-bounds-ness is proven by the caller's record bound;
// the string-table read is bounded by [strTabStart, strTabEnd).
[[nodiscard]] std::string
rdCoffName(std::span<std::uint8_t const> b, std::size_t o,
           std::uint64_t strTabStart, std::uint64_t strTabEnd) {
    if (rdU32(b, o) == 0u) {
        std::uint32_t const offset = rdU32(b, o + 4);
        if (offset < 4u) return {};
        return rdName(b, strTabStart, strTabEnd, offset);
    }
    std::size_t n = 0;
    while (n < 8u && b[o + n] != 0u) ++n;
    return std::string{reinterpret_cast<char const*>(&b[o]), n};
}

// Sign-extend the low `width` bytes of `raw` to a signed 64-bit value --
// the inverse of the writer truncating an int64 addend to `widthBytes` LE
// in the patched data slot (pe.cpp:1109). width is 4 or 8 (the non-pcrel
// Linear kinds a data slot uses -- schema invariant (a)).
[[nodiscard]] std::int64_t signExtendLE(std::uint64_t raw, std::uint8_t width) noexcept {
    if (width >= 8u) return static_cast<std::int64_t>(raw);
    unsigned const bits = static_cast<unsigned>(width) * 8u;
    std::uint64_t const mask = (static_cast<std::uint64_t>(1) << bits) - 1u;
    std::uint64_t v = raw & mask;
    std::uint64_t const signBit = static_cast<std::uint64_t>(1) << (bits - 1u);
    if ((v & signBit) != 0u) v |= ~mask;  // extend the sign into the high bytes
    return static_cast<std::int64_t>(v);
}

// One parsed IMAGE_SECTION_HEADER (only the fields the reader consumes).
// The 1-based COFF ordinal is this section's index in `sections` PLUS one.
struct Section {
    std::string   name;
    std::uint64_t rawSize   = 0;   // SizeOfRawData
    std::uint64_t rawPtr    = 0;   // PointerToRawData (0 for a bss/zero-fill)
    std::uint64_t relocPtr  = 0;   // PointerToRelocations
    std::uint32_t relocCount = 0;  // NumberOfRelocations
    std::uint32_t chars     = 0;   // Characteristics
    bool          zeroFill  = false;
    std::optional<SectionKind> kind;  // resolved from the name via the schema
};

// One decoded IMAGE_SYMBOL (only the fields the reader consumes).
struct Sym {
    std::string   name;
    std::uint64_t value   = 0;   // ALREADY section-relative (COFF convention)
    std::uint16_t sectNum = 0;   // 1-based ordinal / 0 / 0xFFFF / 0xFFFE
    std::uint16_t type    = 0;
    std::uint8_t  storage = 0;
};

// A defined symbol staged for atom slicing: its section-relative Value is an
// atom boundary (EXTERNAL) or an interior label (STATIC -- see the loop).
struct DefSym {
    std::uint32_t    symIdx    = 0;
    std::uint64_t    secRelOff = 0;
    std::string      name;
    SymbolBinding    binding    = SymbolBinding::Global;
    SymbolVisibility visibility = SymbolVisibility::Default;
};

// A reconstructed [start, start+len) byte range within one section, plus the
// output-vector index of the AssembledFunction / AssembledData it backs --
// used to route a relocation site to its owning item.
struct Interval {
    std::uint64_t start  = 0;
    std::uint64_t len    = 0;
    std::size_t   outIdx = 0;
};

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

    // -- (0) Format sanity: this reader speaks PE/COFF only ----------
    if (objectFormatSchema.kind() != ObjectFormatKind::Pe) {
        return fail(DiagnosticCode::F_UnsupportedBinaryFormat,
            std::string{"pe::readRelocatableObject: object format schema '"}
                + std::string{objectFormatSchema.name()} + "' is kind "
                + std::string{objectFormatKindName(objectFormatSchema.kind())}
                + ", not PE/COFF -- the COFF reader cannot parse it.");
    }

    // -- (1) IMAGE_FILE_HEADER ---------------------------------------
    if (bytes.size() < kFileHeaderSz) {
        return fail(DiagnosticCode::F_CorruptedBinary,
            "pe::readRelocatableObject: file shorter than IMAGE_FILE_HEADER "
            "(20 bytes).");
    }
    std::uint16_t const numSections   = rdU16(bytes, kFhNumSectionsOff);
    std::uint64_t const symTabPtr     = rdU32(bytes, kFhSymTabPtrOff);
    std::uint32_t const numSymbols    = rdU32(bytes, kFhNumSymbolsOff);
    std::uint16_t const optHeaderSize = rdU16(bytes, kFhOptHdrSizeOff);
    // A `.obj` relocatable has SizeOfOptionalHeader == 0. A NON-zero optional
    // header means a PE IMAGE (an .exe/.dll -- a link OUTPUT, not a
    // relocatable input) -- fail loud like the ELF reader's `e_type != ET_REL`
    // and the Mach-O reader's `filetype != MH_OBJECT`.
    if (optHeaderSize != 0u) {
        return fail(DiagnosticCode::F_UnsupportedBinaryFormat,
            "pe::readRelocatableObject: SizeOfOptionalHeader="
            + std::to_string(optHeaderSize) + " is non-zero -- this is a PE "
              "IMAGE (executable / DLL, a link OUTPUT), not a relocatable "
              ".obj; only relocatable objects are read back into a mergeable "
              "module.");
    }

    // Section-header array [20, 20 + 40*NumberOfSections) (optHdr == 0).
    std::uint64_t const sectTableOff = kFileHeaderSz;  // + optHeaderSize (== 0)
    std::uint64_t const sectTableBytes =
        static_cast<std::uint64_t>(numSections) * kSectionHeaderSz;
    if (rangeExceedsBuffer(sectTableOff, sectTableBytes, bytes.size())) {
        return fail(DiagnosticCode::F_CorruptedBinary,
            "pe::readRelocatableObject: section header table ["
            + std::to_string(sectTableOff) + ", +"
            + std::to_string(sectTableBytes) + ") for "
            + std::to_string(numSections) + " sections runs past EOF (file "
            + std::to_string(bytes.size()) + ").");
    }

    // Symbol table [PointerToSymbolTable, +18*NumberOfSymbols).
    if (numSymbols > 0u && symTabPtr == 0u) {
        return fail(DiagnosticCode::F_CorruptedBinary,
            "pe::readRelocatableObject: PointerToSymbolTable is 0 but "
            "NumberOfSymbols=" + std::to_string(numSymbols) + " -- corrupt "
            "file header.");
    }
    std::uint64_t const symTabBytes =
        static_cast<std::uint64_t>(numSymbols) * kSymbolSz;
    if (rangeExceedsBuffer(symTabPtr, symTabBytes, bytes.size())) {
        return fail(DiagnosticCode::F_CorruptedBinary,
            "pe::readRelocatableObject: symbol table (PointerToSymbolTable="
            + std::to_string(symTabPtr) + " + " + std::to_string(symTabBytes)
            + " bytes for " + std::to_string(numSymbols) + " entries) runs "
              "past EOF.");
    }
    // COFF string table: immediately after the symbol table -- a u32 total
    // size prefix (INCLUSIVE of the 4 bytes) + NUL-terminated long names. A
    // symbol/section long name is the offset (>= 4) into this table.
    std::uint64_t const strTabStart = symTabPtr + symTabBytes;
    std::uint64_t strTabEnd = strTabStart;
    if (numSymbols > 0u) {
        // The 4-byte size prefix must be present (the writer always emits at
        // least the prefix). Its declared size must not run past EOF.
        if (rangeExceedsBuffer(strTabStart, 4u, bytes.size())) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "pe::readRelocatableObject: COFF string-table size prefix at "
                + std::to_string(strTabStart) + " runs past EOF.");
        }
        std::uint64_t const strTabSize = rdU32(bytes, static_cast<std::size_t>(strTabStart));
        if (strTabSize >= 4u
            && rangeExceedsBuffer(strTabStart, strTabSize, bytes.size())) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "pe::readRelocatableObject: COFF string table (size="
                + std::to_string(strTabSize) + " at offset "
                + std::to_string(strTabStart) + ") runs past EOF.");
        }
        strTabEnd = strTabStart + std::max<std::uint64_t>(strTabSize, 4u);
    }

    // -- (2) Section headers: resolve names + bodies + reloc tables --
    //
    // Each file-backed section's [PointerToRawData, +SizeOfRawData) must be
    // in-bounds (except a bss/zero-fill section: PointerToRawData == 0, the
    // span rides SizeOfRawData only). Its reloc table [PointerToRelocations,
    // +10*NumberOfRelocations) must be in-bounds. The section KIND is
    // resolved from the schema name map below (agnostic).
    std::vector<Section> sections(numSections);  // 1-based ordinal = index + 1
    for (std::uint16_t i = 0; i < numSections; ++i) {
        std::size_t const so =
            static_cast<std::size_t>(sectTableOff) + static_cast<std::size_t>(i) * kSectionHeaderSz;
        Section& sec = sections[i];
        sec.rawSize    = rdU32(bytes, so + kShSizeOfRawDataOff);
        sec.rawPtr     = rdU32(bytes, so + kShPtrRawDataOff);
        sec.relocPtr   = rdU32(bytes, so + kShPtrRelocsOff);
        sec.relocCount = rdU16(bytes, so + kShNumRelocsOff);
        sec.chars      = rdU32(bytes, so + kShCharsOff);
        sec.zeroFill   = (sec.chars & kScnCntUninitializedData) != 0u;
        sec.name       = rdCoffName(bytes, so + kShNameOff, strTabStart, strTabEnd);
        // A file-backed section's body must lie within the file. A zero-fill
        // (bss) section carries no file bytes (PointerToRawData == 0), so it
        // is exempt -- exactly like the ELF reader exempts SHT_NOBITS and the
        // Mach-O reader exempts S_ZEROFILL.
        if (!sec.zeroFill
            && rangeExceedsBuffer(sec.rawPtr, sec.rawSize, bytes.size())) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "pe::readRelocatableObject: section '" + sec.name + "' body ["
                + std::to_string(sec.rawPtr) + ", +" + std::to_string(sec.rawSize)
                + ") runs past EOF.");
        }
        std::uint64_t const relocBytes =
            static_cast<std::uint64_t>(sec.relocCount) * kRelocSz;
        if (rangeExceedsBuffer(sec.relocPtr, relocBytes, bytes.size())) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "pe::readRelocatableObject: section '" + sec.name
                + "' relocation table (PointerToRelocations="
                + std::to_string(sec.relocPtr) + ", NumberOfRelocations="
                + std::to_string(sec.relocCount) + ") runs past EOF.");
        }
    }

    // -- (3) Resolve each section's SectionKind from the NAME, agnostic --
    //
    // COFF has NO segment (unlike Mach-O's (segment,section) pair). The
    // object schema declares TWO rows named `.rdata` -- `rodata` (no
    // relocations) and `relro` (reloc-bearing const data, RelRoConst) -- that
    // are header-identical. `nameToKind` is FIRST-WINS (mirror elf:308) so it
    // maps `.rdata` -> the rodata (first) row. `nameToRelroKind` holds the
    // RelRoConst-kind row per name. The disambiguator is RELOC-PRESENCE: a
    // `.rdata` section carrying its own IMAGE_RELOCATION table takes the relro
    // row, a reloc-free one the rodata row. This is the COFF analog of
    // Mach-O's segment-pair key AND the semantic essence -- relro IS "const
    // data that carries load-time relocations" -- so a re-emission routes a
    // reloc-bearing const item to a section that permits relocations. Agnostic
    // (schema rows + universal reloc-presence; no hardcoded `.rdata`).
    std::unordered_map<std::string, SectionKind> nameToKind;
    std::unordered_map<std::string, SectionKind> nameToRelroKind;
    for (auto const& row : objectFormatSchema.sections()) {
        nameToKind.emplace(row.name, row.kind);
        if (row.kind == SectionKind::RelRoConst) {
            nameToRelroKind.emplace(row.name, row.kind);
        }
    }
    for (auto& sec : sections) {
        // GATE 1 (D-LK-COFF-READER-FOREIGN-OBJECT): COFF `$`-grouped section
        // names. A real cl.exe/clang-cl object emits `.text$mn` / `.rdata$r` /
        // `.xdata` -- the `$<suffix>` groups contributions the FINAL linker
        // concatenates within the base section (`$` is the COFF group
        // separator, the analog of ELF's `.` in `.text.<fn>`). Truncate at the
        // FIRST `$` to the BASE name, then route the BASE through the EXISTING
        // two-map reloc-presence logic. DSS emits ungrouped names (no `$`), so
        // this is a strict superset -- a `$`-less name is its own base. We do
        // NOT clone ELF's single-kind longest-prefix resolver: the two
        // header-identical `.rdata` rows (rodata vs relro) are disambiguated
        // ONLY by reloc-presence, which the longest-prefix collapse would lose.
        std::string const base = sec.name.substr(0, sec.name.find('$'));
        if (auto it = nameToKind.find(base); it != nameToKind.end()) {
            sec.kind = it->second;
        }
        if (sec.relocCount > 0u) {
            if (auto it = nameToRelroKind.find(base); it != nameToRelroKind.end()) {
                sec.kind = it->second;  // reloc-bearing const -> the relro row
            }
        }
    }

    // -- (4) Reverse reloc map (nativeId -> RelocationKind), from the
    //         FORMAT SCHEMA -- no hardcoded IMAGE_REL_AMD64_* numbers --------
    //
    // `callSignalNativeIds` collects the native ids that mark an extern as a
    // FUNCTION: a row whose TARGET-schema formula is a call/branch class. PE
    // x86_64 declares NONE (REL32 is `linear`), so this set is EMPTY there and
    // the extern's isData comes from the IMAGE_SYMBOL type hint (step 5/7). PE
    // declares no `pltNativeId` variant either. Kept general for a
    // hypothetical call-branch PE target (agnostic).
    std::unordered_map<std::uint32_t, RelocationKind> nativeToKind;
    std::unordered_set<std::uint32_t> callSignalNativeIds;
    for (auto const& r : objectFormatSchema.relocations()) {
        auto const ins = nativeToKind.emplace(r.nativeId, r.kind);
        if (!ins.second && ins.first->second.v != r.kind.v) {
            // A native id mapping to two DIFFERENT kinds is an ambiguous
            // schema -- fail loud rather than let "last row wins" silently
            // mis-decode (mirrors the ELF / Mach-O duplicate-nativeId guard).
            return fail(DiagnosticCode::F_CorruptedBinary,
                "pe::readRelocatableObject: object format schema '"
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

    // -- (5) Decode every IMAGE_SYMBOL; assign SymbolId = symtab index ----
    //
    // DSS emits ZERO auxiliary records, so a record's ordinal equals its
    // symbol index. A foreign object's NumberOfAuxSymbols>0 record is
    // followed by that many AUX slots that are NOT IMAGE_SYMBOLs: they are
    // SKIPPED (not decoded), and a reloc naming an aux slot fails loud below.
    std::vector<Sym>  syms(numSymbols);
    std::vector<bool> auxSlot(numSymbols, false);
    // A `std::size_t` cursor (not u32): the `+= 1 + numAux` skip past a
    // foreign record's auxiliary slots can never wrap on a hostile
    // NumberOfSymbols (which would spin the loop) -- 64-bit index arithmetic.
    for (std::size_t i = 0; i < numSymbols;) {
        std::size_t const so =
            static_cast<std::size_t>(symTabPtr) + i * kSymbolSz;
        Sym& s = syms[i];
        s.name    = rdCoffName(bytes, so + kSymNameOff, strTabStart, strTabEnd);
        s.value   = rdU32(bytes, so + kSymValueOff);
        s.sectNum = rdU16(bytes, so + kSymSectNumOff);
        s.type    = rdU16(bytes, so + kSymTypeOff);
        s.storage = bytes[so + kSymClassOff];
        std::uint8_t const numAux = bytes[so + kSymNumAuxOff];
        for (std::size_t a = 1; a <= numAux && i + a < numSymbols; ++a) {
            auxSlot[i + a] = true;
        }
        i += static_cast<std::size_t>(1) + numAux;
    }

    // -- (5.5) GATE 3: COMDAT selection -> the section's external-symbol binding.
    //
    // (D-LK-COFF-READER-FOREIGN-OBJECT.) A real cl.exe/clang-cl `.obj` places
    // each `/Gy` / `__declspec(selectany)` / inline / template body in its OWN
    // IMAGE_SCN_LNK_COMDAT section. COFF encodes the duplicate-resolution policy
    // NOT on the symbol but in the section-definition AUXILIARY record (aux
    // format 5) of the section's STATIC section symbol: the `Selection` byte.
    // We map it to the universal SymbolBinding the merge already resolves, so
    // the EXISTING all-weak dedup (resolveCrossCuDefs lowest-key-wins +
    // linker.cpp isShadowedDuplicate body-drop) folds cross-object COMDAT
    // duplicates with ZERO merge change. The selection switch is a TOTAL
    // enumeration -- every kind-resolved COMDAT section maps to a binding or
    // FAILS LOUD; a selection is never silently defaulted (a wrong default
    // silently mis-dedups).
    std::unordered_map<std::uint16_t, SymbolBinding> comdatBindingBySection;
    for (std::uint16_t si = 0; si < numSections; ++si) {
        Section const& sec = sections[si];
        if ((sec.chars & kScnLnkComdat) == 0u) continue;  // not a COMDAT section
        // Only a COMDAT section whose KIND resolved (real code/data --
        // `.text$mn`/`.data`/`.rdata`) reconstructs a BODY whose external
        // symbol we lift + whose wrong-size selection is a miscompile risk. A
        // COMDAT section with UNRESOLVED kind is unmodeled metadata -- a real
        // `/Gy` object marks its `.pdata`/`.xdata` COMDAT with
        // ASSOCIATIVE(5) selection tying them to the function COMDAT -- which
        // reconstructs NO body + has NO external defined symbol to lift, and is
        // SKIPPED whole by Gate 2 (D-LK-COFF-FOREIGN-UNWIND-DROP). Reading its
        // selection would fail loud on ASSOCIATIVE for a section we drop anyway
        // (blocking every `/Gy`-compiled object). Gate 3 owns kind-RESOLVED
        // COMDAT (the miscompile surface); Gate 2 owns the kind-UNRESOLVED
        // metadata -- the same kind split both gates key on.
        if (!sec.kind.has_value()) continue;
        std::uint16_t const ordinal = static_cast<std::uint16_t>(si + 1u);
        // The section-definition symbol: a STATIC symbol naming THIS ordinal
        // with an auxiliary record (its FIRST aux is the format-5 section
        // definition, PE/COFF spec 5.5.5). Its Selection byte is the policy.
        std::optional<std::uint8_t> selection;
        for (std::uint32_t k = 0; k < numSymbols; ++k) {
            if (auxSlot[k]) continue;                          // an aux slot, not a symbol
            Sym const& s = syms[k];
            if (s.storage != kSymClassStatic || s.sectNum != ordinal) continue;
            std::size_t const auxIdx = static_cast<std::size_t>(k) + 1u;
            if (auxIdx >= numSymbols || !auxSlot[auxIdx]) continue;  // no aux -> not the section symbol
            // The aux record sits at symtab index auxIdx; [ao, ao+18) is in
            // bounds (auxIdx < numSymbols, and the symbol table [symTabPtr,
            // +18*numSymbols) was proven file-backed in (1)).
            std::size_t const ao =
                static_cast<std::size_t>(symTabPtr) + auxIdx * kSymbolSz;
            selection = bytes[ao + kAuxSectionDefSelectionOff];
            break;
        }
        if (!selection.has_value()) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "pe::readRelocatableObject: section '" + sec.name + "' is flagged "
                "IMAGE_SCN_LNK_COMDAT but carries no section-definition auxiliary "
                "record (aux format 5) to read its COMDAT Selection from -- "
                "refusing to default a selection (a wrong default would silently "
                "mis-dedup). D-LK-COFF-COMDAT-UNSUPPORTED-SELECTION.");
        }
        switch (*selection) {
            case kComdatSelNoDuplicates:
                // NODUPLICATES: a genuine duplicate IS an error. Keep the
                // symbol STRONG (Global) so the existing all-strong merge fires
                // K_SymbolRedefinedAcrossUnits on a duplicate -- which IS the
                // NODUPLICATES contract.
                comdatBindingBySection.emplace(ordinal, SymbolBinding::Global);
                break;
            case kComdatSelAny:
            case kComdatSelSameSize:
            case kComdatSelExactMatch:
                // ANY / SAME_SIZE / EXACT_MATCH: duplicates are legal; the
                // linker keeps one and drops the rest. Lift to WEAK so the
                // existing all-weak merge dedup keeps one body + drops the
                // shadow (ZERO merge change).
                comdatBindingBySection.emplace(ordinal, SymbolBinding::Weak);
                break;
            default:
                // LARGEST(6) / ASSOCIATIVE(5) / 0 / unknown -> FAIL LOUD.
                // Lifting LARGEST to Weak would be a SILENT WRONG-SIZE
                // MISCOMPILE: cuId is minted in PULL order (compile_pipeline.cpp
                // readArchiveMemberModule), NOT size order, so weak lowest-key
                // -wins can keep the SMALLER copy. ASSOCIATIVE ties a section's
                // liveness to another section, which the symbol-atomic model
                // does not represent. Both are C++ selectany/RTTI/vtable
                // constructs essentially absent from C/SQLite -- fail-loud is
                // the correct best-long-term stance for this target (the
                // size-aware successor is D-LK-COFF-COMDAT-UNSUPPORTED-SELECTION).
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "pe::readRelocatableObject: section '" + sec.name + "' has "
                    "COMDAT Selection " + std::to_string(*selection)
                    + " (LARGEST / ASSOCIATIVE / unknown), which the format-blind "
                    "all-weak merge cannot honor without size-aware selection: "
                    "cuId is minted in PULL order, not size order, so a weak lift "
                    "could keep the smaller copy (a silent wrong-size "
                    "miscompile). D-LK-COFF-COMDAT-UNSUPPORTED-SELECTION.");
        }
    }

    // -- (6) Reconstruct externs, then stage defined symbols per section --
    AssembledModule mod;
    mod.cuId = cuId;

    // symtab index -> the extern's position in mod.externImports (for the
    // isData inference in step 7).
    std::unordered_map<std::uint32_t, std::size_t> externBySym;
    // Defined symbols grouped by 1-based section ordinal -- atom boundaries,
    // sliced by sorted Value in the per-section pass below. A STATIC (local)
    // block label is NOT an atom boundary (see the loop).
    std::unordered_map<std::uint16_t, std::vector<DefSym>> defsBySection;

    for (std::uint32_t i = 0; i < numSymbols; ++i) {
        if (auxSlot[i]) continue;  // an auxiliary record, not a symbol
        Sym const& s = syms[i];
        bool const isExt = (s.storage == kSymClassExternal);
        SymbolBinding const binding =
            isExt ? SymbolBinding::Global : SymbolBinding::Local;

        if (s.sectNum == kSymUndefined) {
            // An UNDEFINED symbol -> an extern import. A nameless slot carries
            // no import identity.
            if (s.name.empty()) continue;
            ExternImport ext;
            ext.symbol      = SymbolId{i};
            ext.mangledName = s.name;
            // isData from the COFF derived-type hint: the canonical function
            // test is `(type & DTYPE_MASK) == DTYPE_FUNCTION` (bits 4..5 = the
            // derived type; DT_ARY=0x30 is DATA, not a function -- a plain
            // `& 0x20` would misread it). A DTYPE_FUNCTION extern -> isData
            // =false, else DATA. COFF carries this hint (UNLIKE Mach-O x86_64,
            // which cannot distinguish call from data by reloc formula), and
            // the c170 writer fold now EMITS it on function externs, so the
            // function/data class round-trips FAITHFULLY -- no silent default.
            // An extern that reaches the walker unresolved is rejected loud by
            // the linker's unbound-extern gate regardless.
            ext.isData      = (s.type & kSymDtypeMask) != kSymDtypeFunction;
            externBySym.emplace(i, mod.externImports.size());
            mod.externImports.push_back(std::move(ext));
            continue;
        }
        if (s.sectNum == kSymAbsolute || s.sectNum == kSymDebug) {
            // ABSOLUTE (-1) / DEBUG (-2): not a section-backed body. Record a
            // ModuleSymbol so a reloc target still resolves by identity (the
            // ELF SHN_ABS / Mach-O N_ABS analog).
            if (!s.name.empty()) {
                mod.symbols.push_back(ModuleSymbol{SymbolId{i}, s.name, binding,
                                                   SymbolVisibility::Default});
            }
            continue;
        }
        // A DEFINED symbol: SectionNumber is a 1-based ordinal.
        if (s.sectNum < 1u || s.sectNum > numSections) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "pe::readRelocatableObject: defined symbol '" + s.name
                + "' names section ordinal " + std::to_string(s.sectNum)
                + " out of range [1, " + std::to_string(numSections)
                + "] -- corrupt SectionNumber.");
        }
        // Value is ALREADY section-relative (the COFF convention -- NO
        // subtraction, unlike Mach-O's flat n_value).
        if (isExt) {
            // Externally-visible defined symbol -> an atom boundary. GATE 3:
            // a COMDAT section lifts the binding per its Selection policy (Weak
            // for ANY/SAME_SIZE/EXACT_MATCH so the all-weak dedup folds
            // duplicates; Global for NODUPLICATES / a non-COMDAT section).
            SymbolBinding extBinding = SymbolBinding::Global;
            if (auto it = comdatBindingBySection.find(s.sectNum);
                it != comdatBindingBySection.end()) {
                extBinding = it->second;
            }
            defsBySection[s.sectNum].push_back(
                DefSym{i, s.value, s.name, extBinding,
                       SymbolVisibility::Default});
        } else if (!s.name.empty()) {
            // A STATIC (local) defined symbol -- an interior `&&label` /
            // jump-table block label (DSS's only STATIC-defined shape,
            // pe.cpp:1224) or a foreign static. Recorded as a LOCAL
            // ModuleSymbol but NOT an atom boundary, so it never splits the
            // function/data item that contains its interior offset (mirrors
            // the Mach-O N_SECT-local rule, macho:545-554).
            mod.symbols.push_back(ModuleSymbol{SymbolId{i}, s.name,
                SymbolBinding::Local, SymbolVisibility::Default});
        }
    }

    // Per-section interval lists for relocation-site routing.
    std::unordered_map<std::uint16_t, std::vector<Interval>> funcIntervalsBySec;
    std::unordered_map<std::uint16_t, std::vector<Interval>> dataIntervalsBySec;

    auto pushModuleSym = [&](DefSym const& d) {
        if (!d.name.empty()) {
            mod.symbols.push_back(ModuleSymbol{SymbolId{d.symIdx}, d.name,
                                               d.binding, d.visibility});
        }
    };

    // Slice each section's atoms by SORTED Value (THE key inversion --
    // IMAGE_SYMBOL has no size field, like Mach-O's nlist_64). Only EXTERNAL
    // defined symbols reached `defsBySection` (a STATIC symbol became an
    // interior-label ModuleSymbol above, never an atom -- the Mach-O
    // N_EXT-vs-local rule). Each atom spans [off_k, next STRICTLY-GREATER off)
    // (the last -> the section's SizeOfRawData). Equal-offset ALIASES share
    // the same span (both get identical bytes -- the ELF equal-start rule).
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
            // equal-offset aliases so they share the span), else the section
            // size (SizeOfRawData).
            std::uint64_t end = sec.rawSize;
            for (std::size_t j = k + 1; j < defs.size(); ++j) {
                if (defs[j].secRelOff > off) { end = defs[j].secRelOff; break; }
            }
            if (off > sec.rawSize || end < off) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "pe::readRelocatableObject: defined symbol '" + defs[k].name
                    + "' offset " + std::to_string(off) + " exceeds its section '"
                    + sec.name + "' size " + std::to_string(sec.rawSize) + ".");
            }
            std::uint64_t const len = end - off;

            if (isText) {
                // A function body -- slice [off, end) out of the file-backed
                // `.text`. A Text section must never be zero-fill.
                if (sec.zeroFill
                    || rangeExceedsBuffer(off, len, sec.rawSize)
                    || rangeExceedsBuffer(sec.rawPtr, sec.rawSize, bytes.size())) {
                    return fail(DiagnosticCode::F_CorruptedBinary,
                        "pe::readRelocatableObject: function symbol '"
                        + defs[k].name + "' range [+" + std::to_string(off)
                        + ", +" + std::to_string(len) + ") is not a file-backed "
                        "slice of section '" + sec.name + "'.");
                }
                std::size_t const bodyOff = static_cast<std::size_t>(sec.rawPtr + off);
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
                    "pe::readRelocatableObject: defined symbol '" + defs[k].name
                    + "' lives in section '" + sec.name + "' which resolves to "
                    "no known code/data section kind -- refusing to silently "
                    "drop a body (add the section's kind to the format schema).");
            }
            // A data object -> an AssembledData item. File-backed sections
            // slice their bytes; a zero-fill (bss) section reserves the size
            // with empty bytes (the reservedSize invariant).
            AssembledData di;
            di.symbol  = SymbolId{defs[k].symIdx};
            di.section = *dk;
            if (isZeroFill(*dk)) {
                di.reservedSize = len;
            } else {
                if (sec.zeroFill
                    || rangeExceedsBuffer(off, len, sec.rawSize)
                    || rangeExceedsBuffer(sec.rawPtr, sec.rawSize, bytes.size())) {
                    return fail(DiagnosticCode::F_CorruptedBinary,
                        "pe::readRelocatableObject: data symbol '" + defs[k].name
                        + "' range [+" + std::to_string(off) + ", +"
                        + std::to_string(len) + ") is not a file-backed slice of "
                        "section '" + sec.name + "'.");
                }
                std::size_t const bodyOff = static_cast<std::size_t>(sec.rawPtr + off);
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
    // Each IMAGE_SECTION_HEADER names its OWN IMAGE_RELOCATION table
    // (PointerToRelocations / NumberOfRelocations). VirtualAddress is
    // section-relative (the writer emits `fnStart+rel.offset` for `.text`,
    // `itemOff+rel.offset` for data). We route it to the reconstructed atom
    // whose byte range contains it (offset made item-relative). A section with
    // relocs but NO reconstructed atom fails loud (mirror the c168 fold --
    // never silently drop a section's relocations).
    auto findInterval = [](std::vector<Interval> const& ivs, std::uint64_t off)
        -> Interval const* {
        for (auto const& iv : ivs) {
            if (off >= iv.start && off < iv.start + iv.len) return &iv;
        }
        return nullptr;
    };

    for (std::size_t si = 0; si < sections.size(); ++si) {
        Section const& sec = sections[si];
        if (sec.relocCount == 0u) continue;
        std::uint16_t const ordinal = static_cast<std::uint16_t>(si + 1u);
        auto const fIt = funcIntervalsBySec.find(ordinal);
        auto const dIt = dataIntervalsBySec.find(ordinal);
        bool const patchesText = (fIt != funcIntervalsBySec.end() && !fIt->second.empty());
        bool const patchesData = (dIt != dataIntervalsBySec.end() && !dIt->second.empty());
        if (!patchesText && !patchesData) {
            // GATE 2 (D-LK-COFF-READER-FOREIGN-OBJECT): a reloc-bearing section
            // that reconstructed NO atom. The skip is gated on KIND-UNRESOLVED,
            // never atom-absence.
            if (!sec.kind.has_value()) {
                // An UNMODELED metadata section whose BASE name is absent from
                // the schema (`.pdata` / `.xdata` / `.debug$S` / `.debug$T` /
                // `.drectve` / `.chks64`) -- kind stayed nullopt. These carry
                // their OWN relocations (`.pdata`/`.xdata` are 0x40000040, NOT
                // discardable -- so the DISCARDABLE bit alone cannot gate this)
                // but hold no reconstructable code/data body the AssembledModule
                // models. SKIP the section AND its reloc table: DSS has no
                // representation for CodeView / SEH-unwind / linker-directive
                // metadata. Dropping `.pdata`/`.xdata` drops unwind for a
                // foreign function (fine for a leaf/exit-42 link; a general
                // limitation -- D-LK-COFF-FOREIGN-UNWIND-DROP).
                continue;
            }
            // A KIND-RESOLVED section (a real `.rdata`/`.data`/`.text`) that
            // reconstructed NO atom -- e.g. an ANONYMOUS `.rdata` string-literal
            // section owned by no symbol. COFF has NO gap-atom analog to the ELF
            // c167 reconstruction, so its relocations cannot be attached. FAIL
            // LOUD rather than skip: skipping a resolved-kind section's relocs
            // would be a SILENT DROP (the never-silently-drop contract). The
            // closing work is COFF gap atoms -- D-LK-COFF-READER-ANONYMOUS-GAP-ATOMS.
            return fail(DiagnosticCode::F_CorruptedBinary,
                "pe::readRelocatableObject: section '" + sec.name + "' carries "
                + std::to_string(sec.relocCount) + " relocation(s) but "
                "reconstructed no atom to attach them to (its kind resolved, so "
                "it is real code/data, not skippable metadata) -- refusing to "
                "silently drop a section's relocations. "
                "D-LK-COFF-READER-ANONYMOUS-GAP-ATOMS.");
        }
        std::vector<Interval> const& ivs = patchesText ? fIt->second : dIt->second;

        for (std::uint32_t e = 0; e < sec.relocCount; ++e) {
            std::size_t const ro = static_cast<std::size_t>(sec.relocPtr)
                                 + static_cast<std::size_t>(e) * kRelocSz;
            std::uint64_t const va       = rdU32(bytes, ro + kRelVirtAddrOff);
            std::uint32_t const symIdx   = rdU32(bytes, ro + kRelSymIdxOff);
            std::uint32_t const nativeId = rdU16(bytes, ro + kRelTypeOff);

            if (symIdx >= numSymbols) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "pe::readRelocatableObject: relocation in section '"
                    + sec.name + "' names symbol #" + std::to_string(symIdx)
                    + " past the symbol table (" + std::to_string(numSymbols)
                    + ").");
            }
            if (auxSlot[symIdx]) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "pe::readRelocatableObject: relocation in section '"
                    + sec.name + "' names symbol #" + std::to_string(symIdx)
                    + " which is an AUXILIARY record slot, not a symbol.");
            }
            auto const kindIt = nativeToKind.find(nativeId);
            if (kindIt == nativeToKind.end()) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "pe::readRelocatableObject: relocation Type "
                    + std::to_string(nativeId) + " in section '" + sec.name
                    + "' is not declared by PE format '"
                    + std::string{objectFormatSchema.name()}
                    + "' -- cannot map it back to a universal RelocationKind.");
            }
            RelocationKind const kind = kindIt->second;
            auto const* tri = targetSchema.relocationInfo(kind);
            if (tri == nullptr) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "pe::readRelocatableObject: RelocationKind "
                    + std::to_string(kind.v) + " has no TargetRelocationInfo on '"
                    + std::string{targetSchema.name()}
                    + "' -- cannot resolve its addend width / bias.");
            }

            Interval const* iv = findInterval(ivs, va);
            if (iv == nullptr) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "pe::readRelocatableObject: relocation at section offset "
                    + std::to_string(va) + " in '" + sec.name
                    + "' lies in no reconstructed "
                    + std::string{patchesText ? "function" : "data item"}
                    + " -- refusing to silently drop it.");
            }

            // Addend. COFF has no addend column:
            //   * a DATA-section reloc's addend lives IN the patched slot
            //     bytes (widthBytes LE at VirtualAddress -- the writer's
            //     in-place convention); the target-schema addendBias is
            //     un-baked so a re-emission re-adds it once (0 for the
            //     non-pcrel absolute kinds a data slot uses).
            //   * a `.text` reloc carries addend 0 (the writer rejects a
            //     non-zero `.text` addend; link.exe applies the rel32 RIP bias
            //     intrinsically).
            std::int64_t addend = 0;
            if (patchesData) {
                std::uint8_t const w = tri->widthBytes;
                if (w == 0u
                    || rangeExceedsBuffer(va, w, sec.rawSize)
                    || rangeExceedsBuffer(sec.rawPtr, sec.rawSize, bytes.size())) {
                    return fail(DiagnosticCode::F_CorruptedBinary,
                        "pe::readRelocatableObject: data relocation at section "
                        "offset " + std::to_string(va) + " in '" + sec.name
                        + "' has a " + std::to_string(w) + "-byte slot that "
                        "runs past the section -- cannot read the in-place "
                        "addend.");
                }
                std::uint64_t raw = 0;
                std::size_t const slot = static_cast<std::size_t>(sec.rawPtr + va);
                for (std::uint8_t b = 0; b < w; ++b) {
                    raw |= static_cast<std::uint64_t>(bytes[slot + b]) << (8u * b);
                }
                addend = signExtendLE(raw, w)
                       - static_cast<std::int64_t>(tri->addendBias);
            }

            Relocation rel;
            rel.offset = static_cast<std::uint32_t>(va - iv->start);
            rel.target = SymbolId{symIdx};
            rel.kind   = kind;
            rel.addend = addend;
            if (patchesText) mod.functions[iv->outIdx].relocations.push_back(rel);
            else             mod.dataItems[iv->outIdx].relocations.push_back(rel);

            // isData inference: an extern reached through a CALL/BRANCH-class
            // reloc is a FUNCTION -- force isData=false. On PE x86_64
            // callSignalNativeIds is EMPTY (REL32 is `linear`), so this is a
            // no-op and the type-hint seed (step 6) stands -- NO fail-loud (the
            // COFF-vs-Mach-O difference: COFF carries the type hint). Kept
            // agnostic for a hypothetical call-branch PE target.
            if (auto ex = externBySym.find(symIdx); ex != externBySym.end()) {
                if (callSignalNativeIds.contains(nativeId)) {
                    mod.externImports[ex->second].isData = false;
                }
            }
        }
    }

    mod.expectedFuncCount = mod.functions.size();
    return mod;
}

} // namespace dss::pe
