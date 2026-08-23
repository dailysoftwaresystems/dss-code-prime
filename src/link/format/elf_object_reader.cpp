#include "link/format/elf_object_reader.hpp"
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

// ELF64 ET_REL reader -- the inverse of elf.cpp's ET_REL writer. See the
// header for the reconstruction contract + scope. Every field is
// bounds-checked; any violation fails loud (F_* diagnostic + nullopt).

namespace dss::elf {

namespace {

using dss::report;

// -- ELF64 structural constants (gABI Ch. 4) ---------------------
//
// The SAME record layout the writer in elf.cpp hardcodes. Re-declared
// locally rather than #include-pulled from `ffi/binary_readers/
// reader_common.hpp`: `ffi` already depends UP on `link`
// (`link/object_format_schema.hpp`), so a `link` -> `ffi` include would
// form a dependency cycle. This mirrors elf_reader.cpp re-declaring the
// same constants to keep `ffi` off `link`.
constexpr std::size_t kEhdrSz = 64;
constexpr std::size_t kShdrSz = 64;
constexpr std::size_t kSymSz  = 24;
constexpr std::size_t kRelaSz = 24;

constexpr std::uint8_t  kEiClass64  = 2;   // ELFCLASS64
constexpr std::uint8_t  kEiData2LSB = 1;   // ELFDATA2LSB
constexpr std::uint16_t kEtRel      = 1;   // ET_REL

constexpr std::uint16_t kShnUndef      = 0;
constexpr std::uint16_t kShnLoReserve  = 0xff00;  // >= here: reserved (ABS/COMMON/...)

constexpr std::uint32_t kShtSymtab = 2;
constexpr std::uint32_t kShtStrtab = 3;
constexpr std::uint32_t kShtRela   = 4;
constexpr std::uint32_t kShtNobits = 8;

// Elf64 sh_flags bits (gABI 4.8) -- the FALLBACK section-kind signal for a
// section NAME the format schema does not declare (the -ffunction-sections /
// -fdata-sections `.text.<fn>` / `.rodata.str1.1` / `.data.rel.ro.local`
// shapes real distro `.a` members carry).
constexpr std::uint64_t kShfWrite     = 0x1;
constexpr std::uint64_t kShfAlloc     = 0x2;
constexpr std::uint64_t kShfExecInstr = 0x4;
constexpr std::uint64_t kShfTls       = 0x400;

constexpr std::uint8_t kStbGlobal = 1;
constexpr std::uint8_t kStbWeak   = 2;
constexpr std::uint8_t kSttNoType  = 0;
constexpr std::uint8_t kSttObject  = 1;
constexpr std::uint8_t kSttFunc    = 2;
constexpr std::uint8_t kSttSection = 3;
constexpr std::uint8_t kSttFile    = 4;

// st_info / st_other decode (gABI 4.31).
[[nodiscard]] constexpr std::uint8_t stBind(std::uint8_t info) noexcept { return info >> 4; }
[[nodiscard]] constexpr std::uint8_t stType(std::uint8_t info) noexcept { return info & 0xFu; }
[[nodiscard]] constexpr std::uint8_t stVis(std::uint8_t other) noexcept { return other & 0x3u; }

// STV_* -> SymbolVisibility (the ELF numeric order differs from the
// enum's -- a switch, not a cast; matches ffi/elf_reader.cpp).
[[nodiscard]] constexpr SymbolVisibility stvToVisibility(std::uint8_t v) noexcept {
    switch (v) {
        case 1:  return SymbolVisibility::Internal;   // STV_INTERNAL
        case 2:  return SymbolVisibility::Hidden;     // STV_HIDDEN
        case 3:  return SymbolVisibility::Protected;  // STV_PROTECTED
        default: return SymbolVisibility::Default;    // STV_DEFAULT
    }
}
// STB_* -> SymbolBinding.
[[nodiscard]] constexpr SymbolBinding stbToBinding(std::uint8_t b) noexcept {
    switch (b) {
        case kStbGlobal: return SymbolBinding::Global;
        case kStbWeak:   return SymbolBinding::Weak;
        default:         return SymbolBinding::Local;
    }
}

// ── THE "EXTERN IS A FUNCTION" SIGNAL IS DECLARED, NOT DERIVED ──────────
//
// This file used to carry an `isCallBranchFormula` helper that answered the
// question from the TARGET row's arithmetic formula (`Aarch64Call26`). It is
// GONE (D-LK-MACHO-ISDATA-NO-CALL-SIGNAL), and it must not come back as a
// fallback: a formula describes ARITHMETIC, and the question is about ROLE.
// The proxy held on AArch64 only by accident -- that CPU's branch has its own
// instruction encoding, so its formula happens to be branch-specific. It
// carries no role wherever the branch shares its arithmetic with a data
// reference, which on x86_64 is `S + A - P` for both. The role now comes from
// the FORMAT row: `isCall` where the wire type is branch-only (aarch64's
// R_AARCH64_CALL26), and `pltNativeId` where the format instead spells an
// extern call with a distinct PLT-variant wire type (x86_64's R_X86_64_PLT32,
// emitted INSTEAD of PC32 against an undefined extern). Both are collected
// into `callSignalNativeIds` below; ELF x86_64 needs the second because its
// R_X86_64_PC32 genuinely serves data references too and therefore must NOT
// be declared `isCall`.

// Overflow-safe [off, off+size) within [0, total) -- the c159-c161
// `rangeExceedsBuffer` shape (subtraction, never `off + size` which
// wraps on a hostile/corrupted header).
[[nodiscard]] constexpr bool
rangeExceedsBuffer(std::uint64_t off, std::uint64_t size, std::uint64_t total) noexcept {
    return off > total || size > total - off;
}

// LE scalar readers -- every call site is preceded by a
// rangeExceedsBuffer gate proving [o, o+N) is in-bounds.
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

// ELF sh_addralign -> Alignment newtype. sh_addralign 0 or 1 means "no
// constraint" (byte). Values above the newtype's 256-byte cap or
// non-power-of-two (never emitted for a producer data section) fall back
// to byte alignment -- the field is only a re-layout hint (the merge
// re-lays-out every item), never a correctness input on read-back.
[[nodiscard]] Alignment alignFromSection(std::uint64_t shAddrAlign) noexcept {
    if (shAddrAlign <= 1u || shAddrAlign > 256u) return Alignment{};
    return Alignment::fromBytes(static_cast<std::uint32_t>(shAddrAlign))
        .value_or(Alignment{});
}

// NUL-terminated name at strtab[index], bounded by [tabStart, tabEnd).
[[nodiscard]] std::string
rdName(std::span<std::uint8_t const> b, std::uint64_t tabStart, std::uint64_t tabEnd,
       std::uint32_t index) {
    // Defense-in-depth: never walk past the buffer even if a caller passes a
    // tabEnd derived from an unvalidated (e.g. NOBITS) section header.
    if (tabEnd > b.size()) tabEnd = b.size();
    std::uint64_t const start = tabStart + index;
    if (start >= tabEnd) return {};
    std::uint64_t end = start;
    while (end < tabEnd && b[end] != 0u) ++end;
    return std::string{reinterpret_cast<char const*>(&b[start]),
                       static_cast<std::size_t>(end - start)};
}

// Decoded section header (only the fields the reader consumes).
struct Shdr {
    std::uint32_t nameOff   = 0;
    std::uint32_t type      = 0;
    std::uint64_t flags     = 0;
    std::uint64_t offset    = 0;
    std::uint64_t size      = 0;
    std::uint32_t link      = 0;
    std::uint32_t info      = 0;
    std::uint64_t addrAlign = 0;
    std::uint64_t entSize   = 0;
    std::string   name;
    std::optional<SectionKind> kind;  // resolved from `name` via the format schema
};

// Decoded symbol (Elf64_Sym).
struct Sym {
    std::uint32_t nameIdx = 0;
    std::uint8_t  info    = 0;
    std::uint8_t  other   = 0;
    std::uint16_t shndx   = 0;
    std::uint64_t value   = 0;
    std::uint64_t size    = 0;
    std::string   name;
};

// A reconstructed [start, start+len) byte range within one section, plus
// the output-vector index of the AssembledFunction / AssembledData it
// backs -- used to route a relocation site to its owning item.
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

    // -- (0) Format sanity: this reader speaks ELF only --------------
    // ── SELF-GUARD (D-LINK-…-KIND-IDENTITY-BRANCHES, TF-C125) ──────────
    //
    // ★★ THIS GUARD SURVIVED THE IDENTITY-BRANCH REMOVAL, AND THE REASON IS
    // MEASURED FOR THIS SITE. The TF-C125 brief expected it to become
    // redundant: with walkers reached only through a backend the loader
    // resolved, a walker "can never be handed a schema of another kind", so
    // the guard would be unreachable by construction and safely deletable.
    //
    // That premise is FALSE here. `elf::readRelocatableObject` is a PUBLIC free function with
    // 26 direct call sites in `tests/`, none of which route through the
    // linker — and `RelocatableObjectReader.NonElfFormatSchemaFailsLoud`
    // (tests/link/test_relocatable_object_reader.cpp) hands it a FOREIGN schema on purpose and asserts this
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
    if (objectFormatSchema.backend() != &link::format::elfBackend()) {
        return fail(DiagnosticCode::F_UnsupportedBinaryFormat,
            std::string{"elf::readRelocatableObject: object format schema '"}
                + std::string{objectFormatSchema.name()} + "' is kind "
                + std::string{link::objectFormatBackendName(objectFormatSchema.backend())}
                + ", not ELF -- the ELF reader cannot parse it.");
    }

    // -- (1) Elf64_Ehdr ----------------------------------------------
    if (bytes.size() < kEhdrSz) {
        return fail(DiagnosticCode::F_CorruptedBinary,
            "elf::readRelocatableObject: file shorter than Elf64_Ehdr (64 bytes).");
    }
    if (bytes[0] != 0x7Fu || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F') {
        return fail(DiagnosticCode::F_UnknownBinaryFormat,
            "elf::readRelocatableObject: missing 0x7F 'E' 'L' 'F' magic.");
    }
    if (bytes[4] != kEiClass64) {
        return fail(DiagnosticCode::F_UnsupportedElfClass,
            "elf::readRelocatableObject: not ELFCLASS64 (EI_CLASS="
            + std::to_string(bytes[4]) + ") -- the reader supports 64-bit only.");
    }
    if (bytes[5] != kEiData2LSB) {
        return fail(DiagnosticCode::F_UnsupportedElfClass,
            "elf::readRelocatableObject: not ELFDATA2LSB (EI_DATA="
            + std::to_string(bytes[5]) + ") -- little-endian only.");
    }
    std::uint16_t const eType = rdU16(bytes, 16);
    if (eType != kEtRel) {
        return fail(DiagnosticCode::F_UnsupportedBinaryFormat,
            "elf::readRelocatableObject: e_type=" + std::to_string(eType)
            + " is not ET_REL (1) -- only relocatable objects are read back into "
              "a mergeable module (executables / shared libraries are link "
              "OUTPUTS, not inputs).");
    }
    std::uint64_t const eShoff     = rdU64(bytes, 40);
    std::uint16_t const eShentsize = rdU16(bytes, 58);
    std::uint16_t const eShnum     = rdU16(bytes, 60);
    std::uint16_t const eShstrndx  = rdU16(bytes, 62);
    if (eShentsize != kShdrSz) {
        return fail(DiagnosticCode::F_CorruptedBinary,
            "elf::readRelocatableObject: e_shentsize=" + std::to_string(eShentsize)
            + " (expected 64).");
    }
    if (eShoff == 0u || eShnum == 0u) {
        return fail(DiagnosticCode::F_CorruptedBinary,
            "elf::readRelocatableObject: no section header table (e_shoff / "
            "e_shnum zero).");
    }
    std::uint64_t const shtBytes =
        static_cast<std::uint64_t>(eShnum) * static_cast<std::uint64_t>(kShdrSz);
    if (rangeExceedsBuffer(eShoff, shtBytes, bytes.size())) {
        return fail(DiagnosticCode::F_CorruptedBinary,
            "elf::readRelocatableObject: section header table runs past EOF "
            "(e_shoff=" + std::to_string(eShoff) + " + "
            + std::to_string(shtBytes) + " > file " + std::to_string(bytes.size())
            + ").");
    }
    if (eShstrndx >= eShnum) {
        return fail(DiagnosticCode::F_CorruptedBinary,
            "elf::readRelocatableObject: e_shstrndx out of range.");
    }

    // -- (2) Section headers + names (via .shstrtab) -----------------
    std::vector<Shdr> secs(eShnum);
    for (std::uint16_t i = 0; i < eShnum; ++i) {
        std::size_t const base = static_cast<std::size_t>(eShoff) + i * kShdrSz;
        Shdr& s = secs[i];
        s.nameOff   = rdU32(bytes, base + 0);
        s.type      = rdU32(bytes, base + 4);
        s.flags     = rdU64(bytes, base + 8);
        s.offset    = rdU64(bytes, base + 24);
        s.size      = rdU64(bytes, base + 32);
        s.link      = rdU32(bytes, base + 40);
        s.info      = rdU32(bytes, base + 44);
        s.addrAlign = rdU64(bytes, base + 48);
        s.entSize   = rdU64(bytes, base + 56);
        // Every non-NOBITS section's body must lie within the file.
        if (s.type != kShtNobits
            && rangeExceedsBuffer(s.offset, s.size, bytes.size())) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "elf::readRelocatableObject: section #" + std::to_string(i)
                + " body [" + std::to_string(s.offset) + ", +"
                + std::to_string(s.size) + ") runs past EOF.");
        }
    }
    // CRITICAL bounds belt: the `.shstrtab` (the e_shstrndx section) is used
    // to resolve every section name below, so its body MUST be file-backed
    // and in-bounds -- exactly like the symtab's linked strtab is checked
    // (~below). The header loop deliberately SKIPS the file-bounds check for
    // NOBITS sections, so a crafted `.o` whose e_shstrndx section is
    // SHT_NOBITS with sh_offset/sh_size past EOF would otherwise let `rdName`
    // walk past the buffer. Require SHT_STRTAB (which is non-NOBITS, hence
    // already range-checked in the header loop).
    if (secs[eShstrndx].type != kShtStrtab) {
        return fail(DiagnosticCode::F_CorruptedBinary,
            "elf::readRelocatableObject: e_shstrndx section #"
            + std::to_string(eShstrndx) + " is not SHT_STRTAB (type "
            + std::to_string(secs[eShstrndx].type) + ") -- the section-name "
            "string table must be a real, file-backed string table.");
    }
    std::uint64_t const shstrOff = secs[eShstrndx].offset;
    std::uint64_t const shstrEnd = shstrOff + secs[eShstrndx].size;  // in-bounds (SHT_STRTAB checked)
    // Resolve each section's name + universal SectionKind (name -> kind via
    // the FORMAT SCHEMA rows: agnostic, no hardcoded ".rodata"). An
    // unmapped name (`.comment`, `.eh_frame`, `.note.*`) leaves kind = nullopt.
    std::unordered_map<std::string, SectionKind> nameToKind;
    for (auto const& row : objectFormatSchema.sections()) nameToKind.emplace(row.name, row.kind);
    for (auto& s : secs) {
        s.name = rdName(bytes, shstrOff, shstrEnd, s.nameOff);
        if (auto it = nameToKind.find(s.name); it != nameToKind.end()) s.kind = it->second;
    }

    // -- (3) Locate .symtab + its .strtab (by TYPE, not name) --------
    std::optional<std::uint16_t> symtabIdx;
    for (std::uint16_t i = 0; i < eShnum; ++i) {
        if (secs[i].type == kShtSymtab) { symtabIdx = i; break; }
    }
    if (!symtabIdx.has_value()) {
        return fail(DiagnosticCode::F_SectionNotFound,
            "elf::readRelocatableObject: no SHT_SYMTAB section -- a relocatable "
            "object without a symbol table has no linkable identities to "
            "reconstruct.");
    }
    Shdr const& symtab = secs[*symtabIdx];
    if (symtab.entSize != kSymSz) {
        return fail(DiagnosticCode::F_CorruptedBinary,
            "elf::readRelocatableObject: .symtab sh_entsize="
            + std::to_string(symtab.entSize) + " is not 24 (Elf64_Sym).");
    }
    if ((symtab.size % kSymSz) != 0u) {
        return fail(DiagnosticCode::F_CorruptedBinary,
            "elf::readRelocatableObject: .symtab size=" + std::to_string(symtab.size)
            + " is not a multiple of 24 (truncated final entry).");
    }
    if (symtab.link == 0u || symtab.link >= eShnum
        || secs[symtab.link].type != kShtStrtab) {
        return fail(DiagnosticCode::F_CorruptedBinary,
            "elf::readRelocatableObject: .symtab sh_link does not name a valid "
            "SHT_STRTAB.");
    }
    std::uint64_t const strOff = secs[symtab.link].offset;
    std::uint64_t const strEnd = strOff + secs[symtab.link].size;  // in-bounds (checked)

    // -- (4) Decode every symbol; assign SymbolId = symtab index -----
    std::size_t const numSyms = static_cast<std::size_t>(symtab.size / kSymSz);
    std::vector<Sym> syms(numSyms);
    for (std::size_t i = 0; i < numSyms; ++i) {
        std::size_t const so = static_cast<std::size_t>(symtab.offset) + i * kSymSz;
        Sym& sy = syms[i];
        sy.nameIdx = rdU32(bytes, so + 0);
        sy.info    = bytes[so + 4];
        sy.other   = bytes[so + 5];
        sy.shndx   = rdU16(bytes, so + 6);
        sy.value   = rdU64(bytes, so + 8);
        sy.size    = rdU64(bytes, so + 16);
        sy.name    = rdName(bytes, strOff, strEnd, sy.nameIdx);
    }

    // -- (5) Reverse reloc maps (nativeId -> RelocationKind), from the
    //         FORMAT SCHEMA -- no hardcoded R_X86_64 numbers here -----
    //
    // `callSignalNativeIds` collects the native ids that mark an extern as a
    // FUNCTION -- the UNION of the format's two declared call signals: the
    // x86_64 PLT (call-through-stub) variant (`pltNativeId`) AND the native id
    // of any row the format declares `"isCall": true` on (aarch64 CALL26,
    // which has NO pltNativeId). Both are DECLARATIONS read from the schema,
    // never inferred from the target's arithmetic (see the note above the byte
    // helpers). The plain PC32 a DATA reference uses is deliberately in
    // neither, which is exactly why elf64-x86_64 must not declare `isCall`.
    //
    // Built by `ObjectFormatSchema::relocationDecodeTable()` — the SCHEMA's
    // own answer to "which rows decode, and which wire ids are call signals",
    // not a loop this reader owns. The three object readers each carried a
    // copy of that loop and two of them omitted the `emitOnly` exclusion this
    // one had; the copy is gone from all three (see `RelocationDecodeTable`).
    auto decode = objectFormatSchema.relocationDecodeTable();
    if (!decode) {
        return fail(DiagnosticCode::F_CorruptedBinary,
                    "elf::readRelocatableObject: " + decode.error());
    }
    auto const& nativeToKind         = decode->nativeToKind;
    auto const& callSignalNativeIds  = decode->callSignalNativeIds;

    // -- (6) Reconstruct functions / data items / externs / symbols --
    AssembledModule mod;
    mod.cuId = cuId;

    // Per-section interval lists for relocation-site routing.
    std::unordered_map<std::uint16_t, std::vector<Interval>> funcIntervalsBySec;
    std::unordered_map<std::uint16_t, std::vector<Interval>> dataIntervalsBySec;
    // symtab index -> the extern's position in mod.externImports (for isData).
    std::unordered_map<std::uint32_t, std::size_t> externBySym;
    // symtab indices that became a reconstructed ATOM (a sliced STT_FUNC body or
    // a sized data OBJECT). A relocation whose target IS such an atom keeps its
    // by-identity binding (step 7). A section-defined target that is NOT an atom
    // -- a SECTION symbol (`R_X86_64_PC32 .rodata-4`) or a size-0 `.LC` marker --
    // is a SECTION-RELATIVE reference: step 7 redirects it to the atom (named or
    // the synthetic gap atom minted below) that owns its `sym.value + addend`
    // byte, with a residual addend. This is how gcc names string literals / jump
    // tables / local objects; without the redirect the merge cannot bind them.
    std::unordered_set<std::uint32_t> atomSymIdx;
    // Every DEFINED symbol recorded WITHOUT an atom, staged for the shared
    // coverage guard -- see `link/format/object_atom_coverage.hpp`.
    std::vector<link::format::BodilessDefinedSymbol> bodilessDefined;
    std::vector<std::uint32_t>                       bodilessSymIdx;

    auto sliceInBounds = [&](Shdr const& sec, std::uint64_t off, std::uint64_t len)
        -> bool {
        // The slice must lie within the section's declared span...
        if (rangeExceedsBuffer(off, len, sec.size)) return false;
        // ...and the section must be a FILE-BACKED region wholly inside the
        // buffer. NOBITS sections carry no file bytes (their sh_offset/sh_size
        // are NOT validated in the header loop above), so a code/data body
        // symbol claiming to live in one -- or a corrupt sh_offset -- would
        // otherwise read past EOF. This is the ONE memory-safety belt the
        // header-loop's non-NOBITS bounds check cannot cover.
        if (sec.type == kShtNobits) return false;
        if (rangeExceedsBuffer(sec.offset, sec.size, bytes.size())) return false;
        return true;
    };

    // Recover a section's universal SectionKind. A real distro `.a` member is
    // typically built with -ffunction-sections / -fdata-sections, so a code /
    // data body lives in `.text.<fn>` / `.rodata.str1.1` / `.data.rel.ro.local`
    // / TLS `.tdata`/`.tbss` -- names the format schema does NOT declare
    // verbatim. Recovery precedence: (1) exact schema-name match (already in
    // `sec.kind`); (2) the LONGEST schema-name prefix (`<name>.` -- so
    // `.data.rel.ro.local` binds RelRoConst, not the shorter `.data`); (3) the
    // sh_flags/type fallback (SHF_EXECINSTR -> Text; SHF_TLS -> Tdata/Tbss;
    // SHF_ALLOC[+WRITE] -> Bss(NOBITS)/Data/Rodata). Without this, such a
    // defined FUNC/OBJECT would silently reduce to a bodiless ModuleSymbol
    // (contradicting the "never a silent partial reconstruction" contract) --
    // the caller fails loud instead when this returns nullopt for a body.
    auto resolveSectionKind = [&](Shdr const& sec) -> std::optional<SectionKind> {
        if (sec.kind.has_value()) return sec.kind;                 // (1) exact
        std::optional<SectionKind> best;                           // (2) longest prefix
        std::size_t bestLen = 0;
        for (auto const& row : objectFormatSchema.sections()) {
            std::size_t const n = row.name.size();
            if (sec.name.size() > n + 1 && sec.name[n] == '.'
                && sec.name.compare(0, n, row.name) == 0 && n + 1 > bestLen) {
                best = row.kind;
                bestLen = n + 1;
            }
        }
        if (best.has_value()) return best;
        if (sec.flags & kShfExecInstr) return SectionKind::Text;   // (3) flags
        if (sec.flags & kShfTls)
            return (sec.type == kShtNobits) ? SectionKind::ThreadBss
                                            : SectionKind::ThreadData;
        if (sec.flags & kShfAlloc) {
            if (sec.type == kShtNobits) return SectionKind::Bss;
            return (sec.flags & kShfWrite) ? SectionKind::Data : SectionKind::Rodata;
        }
        return std::nullopt;  // non-ALLOC (debug/metadata) -- not a runtime body
    };

    // -- (6.44) EQUAL-OFFSET ALIAS IDENTITY: one atom, several names ------
    //
    // D-LINK-EQUAL-OFFSET-DEFINED-SYMBOLS-BECOME-TWIN-ATOMS. Two defined symbols
    // sharing an `st_value` are two NAMES for one body -- gcc's
    // `__attribute__((alias("g")))` emits exactly that, the alias and its target
    // carrying the same `st_value` AND the same `st_size`. Slicing one atom per
    // symbol produced byte-identical twins over one span, and `findInterval`
    // hands a relocation inside that span to exactly ONE of them, leaving the
    // other's copy un-patched. The shared header owns the rule, the ranking and
    // the argument for both (`resolveEqualOffsetAtomAliases`).
    //
    // ★★ WHY ELF NEEDS A PRE-PASS WHERE COFF AND MACH-O DO NOT. Those two build
    // a per-section BOUNDARY SET first and slice from it, so the alias question
    // is answered on a finished list. ELF slices during the symtab walk itself
    // -- `st_size` makes every symbol self-describing -- so the question has to
    // be answered BEFORE the walk, over the same symbols the walk will turn into
    // atoms. `elfAtomStart` is that predicate, and it mirrors the walk's two
    // atom-creating arms below (a sized STT_FUNC in a Text section; a sized
    // OBJECT in a data-kind section). The two agreeing is not assumed: the
    // post-walk check below fails loud if an alias's owner never materialised.
    //
    // ★ A ZERO-EXTENT SYMBOL IS NEVER A CANDIDATE, matching the shared header's
    // marker rule. An ARM `$d`/`$x` mapping symbol sits at the exact offset of
    // the function it precedes; folding it into that function's identity would
    // put its name AHEAD of the real one in the id -> row lookup, and the
    // emitted symbol would take the mapping symbol's name.
    //
    // ★ ELF IS THE ONLY READER THAT CAN HIT THE CONFLICTING-EXTENT REFUSAL,
    // because it is the only one whose symbols DECLARE an extent -- see the
    // resolver's "what still fails loud".
    auto elfAtomStart =
        [&](std::size_t i) -> std::optional<link::format::AtomStartCandidate> {
        Sym const&         sy   = syms[i];
        std::uint8_t const type = stType(sy.info);
        if (type == kSttFile || type == kSttSection) return std::nullopt;
        if (sy.shndx == kShnUndef || sy.shndx >= kShnLoReserve
            || sy.shndx >= eShnum) {
            return std::nullopt;
        }
        if (sy.size == 0) return std::nullopt;
        Shdr const&                      sec = secs[sy.shndx];
        std::optional<SectionKind> const rk  = resolveSectionKind(sec);
        // Mirrors the walk's two atom-creating arms EXACTLY, including the arm
        // that is a REFUSAL rather than an atom: a STT_FUNC outside a Text
        // section is rejected by the walk, so calling it an atom start here
        // would let the alias resolver's own refusal pre-empt the more specific
        // one with the more useful message.
        bool const isAtom =
            (rk == SectionKind::Text)
                ? (type == kSttFunc)
                : (type != kSttFunc && rk.has_value()
                   && dataSectionKindOf(*rk).has_value());
        if (!isAtom) return std::nullopt;
        return link::format::AtomStartCandidate{
            /*sectionKey=*/sy.shndx, /*offset=*/sy.value,
            /*declaredExtent=*/sy.size, /*symbolId=*/static_cast<std::uint32_t>(i),
            /*binding=*/stbToBinding(stBind(sy.info)),
            /*visibility=*/stvToVisibility(stVis(sy.other)),
            /*name=*/sy.name, /*sectionName=*/sec.name};
    };
    std::unordered_map<std::uint32_t, std::uint32_t> atomOwnerBySym;
    {
        std::vector<link::format::AtomStartCandidate> candidates;
        for (std::size_t i = 0; i < numSyms; ++i) {
            if (auto c = elfAtomStart(i); c.has_value()) {
                candidates.push_back(std::move(*c));
            }
        }
        std::vector<std::uint32_t> owner;
        if (!link::format::resolveEqualOffsetAtomAliases(
                candidates, owner, "elf::readRelocatableObject", reporter)) {
            return std::nullopt;   // the resolver reported, naming both symbols
        }
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (owner[i] != candidates[i].symbolId) {
                atomOwnerBySym.emplace(candidates[i].symbolId, owner[i]);
            }
        }
    }
    // The atom identity a symbol resolves to: itself unless it aliases another.
    // Consulted at BOTH sites that need it -- the walk (does this symbol slice a
    // body?) and the relocation pass (which atom does this target name?) --
    // because a collapse without the target remap turns a silent miscompile into
    // a spurious `K_SymbolUndefined`.
    auto ownerOf = [&](std::uint32_t symIdx) -> std::uint32_t {
        auto const it = atomOwnerBySym.find(symIdx);
        return it == atomOwnerBySym.end() ? symIdx : it->second;
    };
    // Alias rows are appended AFTER the walk, never during it: every id -> row
    // lookup over `AssembledModule::symbols` keeps the FIRST row for an id, so
    // the canonical name must be recorded before any alias of it.
    std::vector<ModuleSymbol> aliasRows;
    std::vector<std::uint32_t> aliasOwners;

    for (std::size_t i = 0; i < numSyms; ++i) {
        Sym const& sy = syms[i];
        std::uint8_t const type = stType(sy.info);
        // STT_FILE / reserved-index (SHN_ABS/COMMON/...) symbols carry no
        // reconstructible body. They still occupy a symtab index (a reloc
        // could target one -- if so, the routing pass below fails loud).
        if (type == kSttFile) continue;
        if (sy.shndx == kShnUndef) {
            // An undefined reference -> an extern import. Unnamed UND slot 0
            // (STN_UNDEF) and any nameless UND entry carry no import identity.
            if (sy.name.empty()) continue;
            ExternImport ext;
            ext.symbol      = SymbolId{static_cast<std::uint32_t>(i)};
            ext.mangledName = sy.name;
            // isData seed from the symtab type: STT_FUNC -> false; STT_OBJECT
            // -> true; STT_NOTYPE (DSS + gcc emit externs as NOTYPE) -> DATA by
            // default, overridden to false (function) ONLY when a CALL/BRANCH
            // -class reloc targets it (the reloc pass below). This is the fix
            // for the old "any non-PLT reloc => data" rule, which misclassified
            // an address-taken extern function and EVERY aarch64 extern call
            // (aarch64 declares no pltNativeId).
            ext.isData      = (stType(sy.info) != kSttFunc);
            externBySym.emplace(static_cast<std::uint32_t>(i), mod.externImports.size());
            mod.externImports.push_back(std::move(ext));
            continue;
        }
        if (sy.shndx >= kShnLoReserve) {
            // A reserved section index (SHN_ABS absolute value, SHN_COMMON
            // tentative definition, ...): not a section-backed body. Recorded
            // as a ModuleSymbol so a reloc target still resolves by identity.
            // (SHN_COMMON allocation -- pick-max-size across CUs into `.bss` --
            // is a linker/merge concern, deliberately left to the c165
            // static-link rather than fabricated as a fixed `.bss` item here.)
            if (!sy.name.empty() && type != kSttSection) {
                mod.symbols.push_back(ModuleSymbol{SymbolId{static_cast<std::uint32_t>(i)},
                                                   sy.name, stbToBinding(stBind(sy.info)),
                                                   stvToVisibility(stVis(sy.other))});
            }
            continue;
        }
        if (sy.shndx >= eShnum) {
            // An out-of-range (non-reserved) section index is corrupt -- fail
            // loud rather than silently drop the symbol.
            return fail(DiagnosticCode::F_CorruptedBinary,
                "elf::readRelocatableObject: symbol '" + sy.name
                + "' names section #" + std::to_string(sy.shndx)
                + " past the section table (" + std::to_string(eShnum)
                + ") -- corrupt st_shndx.");
        }
        Shdr const& sec = secs[sy.shndx];
        std::optional<SectionKind> const rk = resolveSectionKind(sec);

        if (type == kSttSection) {
            // A section symbol: a relocation base, not a body. Record a
            // ModuleSymbol so a section-relative reloc target resolves to a
            // named LOCAL identity (the merge's handling of section-relative
            // references is c165's static-link concern).
            std::string const nm = sy.name.empty() ? sec.name : sy.name;
            mod.symbols.push_back(ModuleSymbol{SymbolId{static_cast<std::uint32_t>(i)},
                                               nm, SymbolBinding::Local,
                                               stvToVisibility(stVis(sy.other))});
            continue;
        }

        // A DEFINED symbol in a real section. Route by the RECOVERED kind
        // (`rk` -- exact name, then `.text.`/`.data.`-style prefix, then flags).
        auto pushModuleSym = [&] {
            if (!sy.name.empty())
                mod.symbols.push_back(ModuleSymbol{SymbolId{static_cast<std::uint32_t>(i)},
                                                   sy.name, stbToBinding(stBind(sy.info)),
                                                   stvToVisibility(stVis(sy.other))});
        };
        // Stage a BODILESS defined symbol for the shared coverage guard
        // (D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM) --
        // called at every site below that records a ModuleSymbol WITHOUT
        // reconstructing an atom for it. Only a KIND-RESOLVED section is staged:
        // a DWARF/metadata symbol reconstructs no body BY DESIGN and is not a
        // dropped body. `st_size` is passed through, so the guard's own
        // "zero extent is a MARKER, not a body" rule covers this reader's
        // mapping symbols (`$d`/`$x`) and size-less text labels -- the SAME
        // judgment the slicing arms below already make, expressed once.
        //
        // ⓘ ELF is the reader this defect does NOT affect: it slices any
        // STT_FUNC with a non-empty extent regardless of BINDING, so a
        // file-local function is an atom here. The staging exists so the guard
        // is genuinely reader-agnostic rather than a COFF/Mach-O special case --
        // and so a future ELF arm that starts demoting bodies cannot do it
        // quietly.
        auto stageBodiless = [&] {
            if (sy.name.empty() || !rk.has_value()) return;
            bodilessDefined.push_back(link::format::BodilessDefinedSymbol{
                /*sectionKey=*/sy.shndx, /*sectionOffset=*/sy.value,
                /*declaredSize=*/sy.size, /*name=*/sy.name,
                /*sectionName=*/sec.name});
            // ...and the symtab index alongside it, kept HERE rather than in
            // the shared struct because it is ELF's own coordinate: the shared
            // staging is deliberately nothing but `(sectionKey, byteOffset)`.
            // The geometry fallback below returns indices into
            // `bodilessDefined`, and turning one back into an atom needs the
            // symbol it came from.
            bodilessSymIdx.push_back(static_cast<std::uint32_t>(i));
        };

        // A function MUST live in an executable section. A STT_FUNC anywhere
        // else is corrupt -- fail loud (never mis-slice code as data).
        if (type == kSttFunc && rk != SectionKind::Text) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "elf::readRelocatableObject: function symbol '" + sy.name
                + "' lives in section '" + sec.name
                + "' which does not resolve to an executable (Text) kind.");
        }

        // (6.44) THE ALIAS ARM: this symbol names a body another symbol at the
        // same `st_value` already owns, so it slices NO atom -- it keeps its
        // NAME and takes the owner's identity. Placed after the executable
        // -section refusal so a mis-sectioned function is still named.
        if (std::uint32_t const owner = ownerOf(static_cast<std::uint32_t>(i));
            owner != static_cast<std::uint32_t>(i)) {
            if (!sy.name.empty()) {
                aliasRows.push_back(ModuleSymbol{SymbolId{owner}, sy.name,
                                                 stbToBinding(stBind(sy.info)),
                                                 stvToVisibility(stVis(sy.other))});
            }
            aliasOwners.push_back(owner);
            continue;
        }

        if (rk == SectionKind::Text) {
            // A function body -- slice `.text[value, value+size)`. Only
            // STT_FUNC with a non-empty extent becomes a function; a
            // zero-size / NOTYPE text label (e.g. a computed-goto block
            // symbol) is recorded as a ModuleSymbol only (its interior-VA
            // binding is a named follow-up), never sliced.
            if (type == kSttFunc && sy.size > 0) {
                if (!sliceInBounds(sec, sy.value, sy.size)) {
                    return fail(DiagnosticCode::F_CorruptedBinary,
                        "elf::readRelocatableObject: function symbol '" + sy.name
                        + "' range [" + std::to_string(sy.value) + ", +"
                        + std::to_string(sy.size) + ") exceeds its section '"
                        + sec.name + "'.");
                }
                std::size_t const bodyOff =
                    static_cast<std::size_t>(sec.offset + sy.value);
                AssembledFunction fn;
                fn.symbol = SymbolId{static_cast<std::uint32_t>(i)};
                fn.bytes.assign(bytes.begin() + bodyOff,
                                bytes.begin() + bodyOff + sy.size);
                funcIntervalsBySec[sy.shndx].push_back(
                    Interval{sy.value, sy.size, mod.functions.size()});
                atomSymIdx.insert(static_cast<std::uint32_t>(i));
                mod.functions.push_back(std::move(fn));
            } else {
                stageBodiless();   // no atom -- the guard decides (see above)
            }
            pushModuleSym();
            continue;
        }

        std::optional<DataSectionKind> const dk =
            rk.has_value() ? dataSectionKindOf(*rk) : std::nullopt;
        if (dk.has_value() && sy.size == 0) {
            // A ZERO-EXTENT defined data symbol is a MARKER, not a body -- an
            // ARM `$d`/`$x`/`$a`/`$t` mapping symbol (LOCAL NOTYPE size 0, one
            // per section incl. `.eh_frame`, gcc/clang aarch64), or an empty
            // object. Recording an empty AssembledData would (a) be a bodiless
            // item and (b) populate the section's interval map, which would
            // spuriously route a metadata section's RELA (`.rela.eh_frame`) to
            // an empty item and fail loud. ModuleSymbol only -- and NO
            // interval. (Agnostic: keyed on the zero extent, never the `$`
            // name.) Not a dropped body -- there are no bytes.
            stageBodiless();       // st_size == 0 -- the guard skips it as a MARKER
            pushModuleSym();
            continue;
        }
        if (dk.has_value()) {
            // A data object -> an AssembledData item. File-backed sections
            // slice their bytes; zero-fill (.bss / .tbss) reserve the size
            // with empty bytes (the `reservedSize` invariant).
            AssembledData di;
            di.symbol    = SymbolId{static_cast<std::uint32_t>(i)};
            di.section   = *dk;
            di.alignment = alignFromSection(sec.addrAlign);  // section-granular (see header)
            if (isZeroFill(*dk)) {
                di.reservedSize = sy.size;
            } else {
                if (!sliceInBounds(sec, sy.value, sy.size)) {
                    return fail(DiagnosticCode::F_CorruptedBinary,
                        "elf::readRelocatableObject: data symbol '" + sy.name
                        + "' range [" + std::to_string(sy.value) + ", +"
                        + std::to_string(sy.size) + ") exceeds its section '"
                        + sec.name + "'.");
                }
                std::size_t const bodyOff =
                    static_cast<std::size_t>(sec.offset + sy.value);
                di.bytes.assign(bytes.begin() + bodyOff,
                                bytes.begin() + bodyOff + sy.size);
            }
            dataIntervalsBySec[sy.shndx].push_back(
                Interval{sy.value, sy.size, mod.dataItems.size()});
            atomSymIdx.insert(static_cast<std::uint32_t>(i));
            mod.dataItems.push_back(std::move(di));
            pushModuleSym();
            continue;
        }

        // `rk` resolved to no producer kind (or nullopt). A defined body that
        // is real runtime code/data -- an allocated (SHF_ALLOC) OBJECT/NOTYPE
        // with a non-zero extent -- must NEVER be silently dropped to a
        // bodiless ModuleSymbol (the "never a silent partial reconstruction"
        // contract). Fail loud so the shape is recovered (a new schema row or
        // a resolveSectionKind arm) rather than mis-linked to an empty def. A
        // non-allocated symbol (DWARF/debug/metadata) is recorded as a
        // ModuleSymbol only -- it is not a program body.
        bool const allocated = (sec.flags & kShfAlloc) != 0u;
        bool const isRuntimeBody =
            allocated && sy.size > 0
            && (type == kSttObject || type == kSttNoType);
        if (isRuntimeBody) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "elf::readRelocatableObject: defined symbol '" + sy.name
                + "' (an allocated " + std::to_string(sy.size)
                + "-byte body in section '" + sec.name + "', sh_flags="
                + std::to_string(sec.flags)
                + ") resolves to no known code/data section kind -- refusing to "
                  "silently drop a code/data body. Add the section's kind (a "
                  "format schema row or a resolveSectionKind arm).");
        }
        stageBodiless();   // self-gates on a RESOLVED kind (see above)
        pushModuleSym();
    }

    // (6.44) Every canonical row is now recorded, so the aliases can follow:
    // several names, one SymbolId, the owning name first.
    //
    // ⚠ AND THE OWNER MUST ACTUALLY HAVE MATERIALISED. `elfAtomStart` mirrors
    // the walk's two atom-creating arms, and this is what stops that mirroring
    // from being an assumption: if the pre-pass called something an atom start
    // that the walk did not slice, an alias would have been folded onto an
    // identity that owns no bytes -- silently, since the alias itself asked for
    // nothing. Checked here rather than in the arm because the owner may sit
    // LATER in symbol-table order than the alias that names it.
    for (std::uint32_t owner : aliasOwners) {
        if (!atomSymIdx.contains(owner)) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "elf::readRelocatableObject: symbol '" + syms[owner].name
                + "' was chosen to own the atom at offset "
                + std::to_string(syms[owner].value) + " of section #"
                + std::to_string(syms[owner].shndx)
                + " but the walk reconstructed no atom for it -- the alias "
                  "pre-pass and the slicing arms disagree about what starts a "
                  "body. This is the READER contradicting itself, not an object "
                  "it cannot classify.");
        }
    }
    for (auto& ms : aliasRows) mod.symbols.push_back(std::move(ms));

    // MEDIUM guard: overlapping STT_FUNC (st_value,st_size) ranges within one
    // section -- a relocation site inside the overlap would mis-route to
    // whichever function `findInterval` returns first.
    //
    // ★ EQUAL-START PAIRS USED TO BE WAVED THROUGH HERE, on the reasoning that
    // two aliases share an item-relative offset so routing to either is
    // offset-correct. That reasoning was HALF the fact: it is true of the
    // OFFSET and false of the RELOCATION, which reaches only the twin
    // `findInterval` returns first and leaves the other un-patched. (6.44) now
    // collapses an equal-start pair to one atom before the walk slices it, and
    // refuses the pair outright when their declared extents disagree -- so an
    // equal start can no longer arrive here at all, and the carve-out that let
    // it pass is gone rather than left standing as an untrue permission.
    for (auto& [secIdx, ivs] : funcIntervalsBySec) {
        std::vector<Interval> sorted = ivs;
        std::sort(sorted.begin(), sorted.end(),
                  [](Interval const& a, Interval const& b) { return a.start < b.start; });
        for (std::size_t k = 1; k < sorted.size(); ++k) {
            Interval const& prev = sorted[k - 1];
            Interval const& cur = sorted[k];
            if (cur.start < prev.start + prev.len) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "elf::readRelocatableObject: overlapping STT_FUNC ranges in "
                    "section #" + std::to_string(secIdx) + " ([+"
                    + std::to_string(prev.start) + ",+"
                    + std::to_string(prev.start + prev.len) + ") and [+"
                    + std::to_string(cur.start) + ",...)) -- a relocation in the "
                    "overlap would mis-route. Overlapping/nested function "
                    "symbols are a deferred shape.");
            }
        }
    }

    // -- (6.4) GEOMETRY FALLBACK: recover a body no ELF field could name ---
    //
    // D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM, operator
    // ruling 2026-08-20: the fallback lands on ALL THREE readers. The shared
    // header owns the inference and the argument for it
    // (`uncoveredDefinedSymbolsThatStartAnAtom`) -- a symbol no reconstructed
    // atom covers cannot be interior to one, so it starts a body, and promoting
    // preserves its bytes where demoting drops them.
    //
    // ★★ ELF IS THE READER WHERE THIS FIRES LEAST, AND THE ONE SHAPE IT FIRES
    // ON IS WORTH NAMING -- an unreachable branch would be the vacuous coverage
    // this repo keeps finding. `st_size` means almost every defined symbol here
    // already declares its own extent and is sliced by the arms above, and a
    // ZERO-size symbol is a MARKER the shared rule skips. What is left is a
    // symbol in a TEXT section with a NON-ZERO st_size that is not STT_FUNC:
    // the `rk == Text` arm slices only `STT_FUNC && size > 0`, so an
    // STT_NOTYPE / STT_OBJECT body in `.text` -- an assembler label given a
    // `.size` but no `.type @function`, or a jump table emitted into `.text` as
    // `@object` -- reaches `stageBodiless()` with real bytes and no atom. That
    // used to be a refusal; it is recovered now, and the pin is
    // `ElfSizedNoTypeTextBodyIsRecoveredByGeometry`.
    //
    // ★★ NO RE-SLICE, unlike COFF and Mach-O. Those two derive an atom's END
    // from the NEXT boundary, so adding a boundary changes its neighbour and the
    // section must be cut again. ELF derives it from `st_size`, so a promoted
    // symbol's extent is a property of the symbol alone and no existing atom
    // moves. That is why this sits AFTER the slicing loop here and BEFORE it
    // there -- the same decision, applied in each format's own vocabulary.
    //
    // ⓘ BEFORE the gap-atom pass below, deliberately: a NAMED body must claim
    // its bytes before the anonymous pass sweeps whatever is left, or a symbol
    // the object does name would end up inside a nameless synthetic atom.
    {
        std::vector<link::format::ReconstructedAtomExtent> extents;
        for (auto const* bySec : {&funcIntervalsBySec, &dataIntervalsBySec}) {
            for (auto const& [secIdx, ivs] : *bySec) {
                for (auto const& iv : ivs) {
                    extents.push_back(link::format::ReconstructedAtomExtent{
                        secIdx, iv.start, iv.len});
                }
            }
        }
        for (std::size_t idx :
             link::format::uncoveredDefinedSymbolsThatStartAnAtom(bodilessDefined,
                                                                  extents)) {
            auto const&         c      = bodilessDefined[idx];
            std::uint32_t const symIdx = bodilessSymIdx[idx];
            Shdr const&         psec   = secs[c.sectionKey];
            // Only a sized symbol reaches here (the shared rule skips a declared
            // ZERO extent as a marker, and ELF stages `st_size` verbatim), so
            // the extent is the symbol's own -- no next-boundary inference.
            std::uint64_t const size = c.declaredSize.value_or(0u);
            if (size == 0u || !sliceInBounds(psec, c.sectionOffset, size)) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "elf::readRelocatableObject: defined symbol '" + c.name
                    + "' range [" + std::to_string(c.sectionOffset) + ", +"
                    + std::to_string(size) + ") exceeds its section '"
                    + psec.name + "' -- cannot recover it as a body.");
            }
            // A TEXT section is the only place this can land: a DATA symbol with
            // a non-zero size was already sliced by the `dk` arm above, and an
            // unresolved-kind section is never staged. Asserted rather than
            // assumed, so a future staging site cannot widen this quietly into
            // fabricating a function out of data.
            std::optional<SectionKind> const prk = resolveSectionKind(psec);
            if (prk != SectionKind::Text) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "elf::readRelocatableObject: defined symbol '" + c.name
                    + "' was recovered by atom-coverage geometry in section '"
                    + psec.name + "', which is not a Text kind -- a data body "
                    "must be sliced by its own st_size in the symbol loop, not "
                    "recovered here. "
                    "D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM.");
            }
            std::size_t const bodyOff =
                static_cast<std::size_t>(psec.offset + c.sectionOffset);
            AssembledFunction fn;
            fn.symbol = SymbolId{symIdx};
            fn.bytes.assign(bytes.begin() + bodyOff,
                            bytes.begin() + bodyOff + static_cast<std::size_t>(size));
            funcIntervalsBySec[static_cast<std::uint16_t>(c.sectionKey)].push_back(
                Interval{c.sectionOffset, size, mod.functions.size()});
            atomSymIdx.insert(symIdx);
            mod.functions.push_back(std::move(fn));
            // NO second ModuleSymbol: `pushModuleSym()` already recorded this
            // symbol on the way through the loop above, and a duplicate name in
            // `mod.symbols` is a duplicate definition to the cross-CU resolve.
        }
    }

    // -- (6.5) Synthetic gap atoms: reconstruct ANONYMOUS data-section content
    //          (string literals, switch jump tables, `.rodata` constants) that
    //          is owned by NO sized symbol. gcc references such content via a
    //          SECTION symbol + addend (`R_X86_64_PC32 .rodata-4`), so without
    //          the bytes the reference dangles. Every maximal uncovered
    //          [gapStart,gapEnd) region of a file-backed, allocated DATA section
    //          becomes one synthetic anonymous AssembledData atom (a fresh
    //          SymbolId PAST the symtab range -- collision-free -- and NO
    //          ModuleSymbol, so it is module-private and never cross-CU folded);
    //          its interval joins `dataIntervalsBySec` so the step-7 redirect
    //          routes a section-relative reference to it. Only DATA sections gap
    //          -fill: a `.text` gap is inter-function padding (a code reference
    //          into it is corrupt -> step 7 fails loud, never fabricates a code
    //          atom from padding). A section fully covered by named symbols
    //          (every DSS-written `.o`) yields NO gaps -- this is inert there.
    // A section's kind resolved ONLY by DECLARED name (exact schema row or a
    // `<row>.`-style prefix -- `.rodata` / `.rodata.str1.1` / `.data.rel.ro`),
    // NEVER the SHF_ALLOC flags fallback. Gap-filling must not fire on an
    // allocated METADATA section the flags fallback would mis-type as Rodata
    // (`.eh_frame` is SHF_ALLOC PROGBITS): fabricating a data atom there would
    // un-skip its `.rela.eh_frame` (whose FDE relocs target `.text`) and wrongly
    // route it. Bodies still reconstruct via the flags fallback in the symbol
    // loop (a real OBJECT there is genuine data); only ANONYMOUS gap-fill is
    // restricted to declared data sections.
    auto declaredDataKind = [&](Shdr const& sec) -> std::optional<DataSectionKind> {
        std::optional<SectionKind> k = sec.kind;                    // exact schema name
        if (!k.has_value()) {                                       // longest `<row>.` prefix
            std::size_t bestLen = 0;
            for (auto const& row : objectFormatSchema.sections()) {
                std::size_t const n = row.name.size();
                if (sec.name.size() > n + 1 && sec.name[n] == '.'
                    && sec.name.compare(0, n, row.name) == 0 && n + 1 > bestLen) {
                    k = row.kind;
                    bestLen = n + 1;
                }
            }
        }
        return k.has_value() ? dataSectionKindOf(*k) : std::nullopt;
    };
    std::uint32_t nextSyntheticId = static_cast<std::uint32_t>(numSyms);
    for (std::uint16_t si = 0; si < eShnum; ++si) {
        Shdr const& sec = secs[si];
        if ((sec.flags & kShfAlloc) == 0u || sec.size == 0u
            || sec.type == kShtNobits) {
            continue;  // runtime, file-backed content only
        }
        std::optional<DataSectionKind> const dk = declaredDataKind(sec);
        if (!dk.has_value() || isZeroFill(*dk)) continue;  // a DECLARED DATA (not Text/Bss) kind
        if (rangeExceedsBuffer(sec.offset, sec.size, bytes.size())) continue;  // belt
        // Covered = the named data atoms already reconstructed for this section
        // (a fresh COPY -- `emitGap` appends to the live map vector, and the
        // shared helper must never see the gaps it is in the middle of
        // producing).
        std::vector<link::format::ReconstructedAtomExtent> covered;
        if (auto it = dataIntervalsBySec.find(si); it != dataIntervalsBySec.end()) {
            for (auto const& iv : it->second) {
                covered.push_back(link::format::ReconstructedAtomExtent{
                    si, iv.start, iv.len});
            }
        }
        auto emitGap = [&](std::uint64_t gapStart, std::uint64_t gapEnd) {
            if (gapEnd <= gapStart) return;
            AssembledData di;
            di.symbol    = SymbolId{nextSyntheticId++};
            di.section   = *dk;
            di.alignment = alignFromSection(sec.addrAlign);
            std::size_t const b0 = static_cast<std::size_t>(sec.offset + gapStart);
            di.bytes.assign(bytes.begin() + b0,
                            bytes.begin() + b0 + static_cast<std::size_t>(gapEnd - gapStart));
            dataIntervalsBySec[si].push_back(
                Interval{gapStart, gapEnd - gapStart, mod.dataItems.size()});
            mod.dataItems.push_back(std::move(di));
        };
        // WHICH bytes are unowned is the shared question -- see
        // `unownedByteRangesOfSection`. It used to be a cursor walk written
        // here; COFF needed the identical answer, and two copies of one rule is
        // one rule that can disagree with itself.
        for (auto const& gap : link::format::unownedByteRangesOfSection(
                 si, sec.size, covered)) {
            emitGap(gap.start, gap.start + gap.len);
        }
    }

    // -- (6.6) Coverage guard: no defined symbol's body was dropped -------
    //
    // D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM. Placed
    // AFTER the gap-atom pass, not before it: a `.LC` string literal owned by no
    // symbol becomes an atom there, and asking the coverage question before that
    // would judge the reconstruction half-finished. The check itself is shared
    // and format-neutral -- see `link/format/object_atom_coverage.hpp`.
    {
        std::vector<link::format::ReconstructedAtomExtent> extents;
        for (auto const* bySec : {&funcIntervalsBySec, &dataIntervalsBySec}) {
            for (auto const& [secIdx, ivs] : *bySec) {
                for (auto const& iv : ivs) {
                    extents.push_back(link::format::ReconstructedAtomExtent{
                        secIdx, iv.start, iv.len});
                }
            }
        }
        if (!link::format::everyDefinedSymbolIsCoveredByAnAtom(
                bodilessDefined, extents, "elf::readRelocatableObject",
                reporter)) {
            return std::nullopt;
        }
    }

    // -- (7) Reconstruct relocations from every SHT_RELA section -----
    //
    // A RELA's sh_info names the section its entries patch. We route each
    // entry to the reconstructed function / data item whose byte range
    // contains r_offset (offset made item-relative), un-baking the target
    // schema's addendBias so the reconstructed addend is DSS-native again.
    // A RELA targeting a NON-reconstructed section (`.eh_frame`) is skipped
    // (that section is not part of the mergeable body -- documented).
    auto findInterval = [](std::vector<Interval> const& ivs, std::uint64_t off)
        -> Interval const* {
        for (auto const& iv : ivs) {
            if (off >= iv.start && off < iv.start + iv.len) return &iv;
        }
        return nullptr;
    };

    for (std::uint16_t si = 0; si < eShnum; ++si) {
        Shdr const& rela = secs[si];
        if (rela.type != kShtRela) continue;
        if ((rela.size % kRelaSz) != 0u) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "elf::readRelocatableObject: RELA section '" + rela.name
                + "' size=" + std::to_string(rela.size)
                + " is not a multiple of 24.");
        }
        // sh_link must name THIS symtab; sh_info names the patched section.
        std::uint32_t const tgtSec = rela.info;
        if (tgtSec >= eShnum) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "elf::readRelocatableObject: RELA section '" + rela.name
                + "' sh_info=" + std::to_string(tgtSec) + " out of range.");
        }
        Shdr const& patched = secs[tgtSec];  // for diagnostics
        // Route each entry to whichever body kind was RECONSTRUCTED for the
        // patched section -- keyed on the interval maps the symbol loop built,
        // NOT on a re-derived section kind. A RELA whose target section
        // produced NO reconstructed body (`.rela.eh_frame` / `.rela.debug_*` --
        // unwind/debug metadata we deliberately do not model, and which the
        // sh_flags fallback would otherwise mis-type as `.rodata`) is skipped:
        // there is nothing to attach it to and it is not part of the mergeable
        // module. A per-entry miss WITHIN a reconstructed section still fails
        // loud below (the never-silently-drop-a-reloc guarantee for real code/
        // data). A `.text.<fn>` / `.data.*` (-ffunction-sections) section
        // reconstructs its bodies via resolveSectionKind in the symbol loop, so
        // its interval map is populated and routes correctly here.
        std::uint16_t const tgt = static_cast<std::uint16_t>(tgtSec);
        auto const fIt = funcIntervalsBySec.find(tgt);
        auto const dIt = dataIntervalsBySec.find(tgt);
        bool const patchesText = (fIt != funcIntervalsBySec.end() && !fIt->second.empty());
        bool const patchesData = (dIt != dataIntervalsBySec.end() && !dIt->second.empty());
        if (!patchesText && !patchesData) continue;  // section not reconstructed
        std::vector<Interval> const* ivs = patchesText ? &fIt->second : &dIt->second;

        std::size_t const nEntries = static_cast<std::size_t>(rela.size / kRelaSz);
        for (std::size_t e = 0; e < nEntries; ++e) {
            std::size_t const ro = static_cast<std::size_t>(rela.offset) + e * kRelaSz;
            std::uint64_t const rOffset = rdU64(bytes, ro + 0);
            std::uint64_t const rInfo   = rdU64(bytes, ro + 8);
            std::int64_t  const rAddend = static_cast<std::int64_t>(rdU64(bytes, ro + 16));
            std::uint32_t const symIdx  = static_cast<std::uint32_t>(rInfo >> 32);
            std::uint32_t const rType   = static_cast<std::uint32_t>(rInfo & 0xFFFFFFFFu);

            if (symIdx >= numSyms) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "elf::readRelocatableObject: relocation in '" + rela.name
                    + "' names symbol #" + std::to_string(symIdx)
                    + " past the symbol table (" + std::to_string(numSyms) + ").");
            }
            auto const kindIt = nativeToKind.find(rType);
            if (kindIt == nativeToKind.end()) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "elf::readRelocatableObject: relocation type "
                    + std::to_string(rType) + " in '" + rela.name
                    + "' is not declared by ELF format '"
                    + std::string{objectFormatSchema.name()}
                    + "' -- cannot map it back to a universal RelocationKind.");
            }
            RelocationKind const kind = kindIt->second;
            // Un-bake the psABI bias the writer added (r_addend = addend +
            // addendBias) so the reconstructed addend is DSS-native.
            auto const* tri = targetSchema.relocationInfo(kind);
            if (tri == nullptr) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "elf::readRelocatableObject: RelocationKind "
                    + std::to_string(kind.v) + " has no TargetRelocationInfo on '"
                    + std::string{targetSchema.name()}
                    + "' -- cannot un-bake the addend bias.");
            }
            std::int64_t const nativeAddend =
                rAddend - static_cast<std::int64_t>(tri->addendBias);

            Interval const* iv = findInterval(*ivs, rOffset);
            if (iv == nullptr) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "elf::readRelocatableObject: relocation at offset "
                    + std::to_string(rOffset) + " in '" + rela.name
                    + "' lies in no reconstructed "
                    + std::string{patchesText ? "function" : "data item"}
                    + " of section '" + patched.name
                    + "' -- refusing to silently drop it.");
            }
            // Section-relative resolution: a target symbol that is NOT a
            // reconstructed atom but IS section-defined -- a SECTION symbol
            // (`.rodata`) or a size-0 `.LC` marker -- references `sym.value +
            // nativeAddend` bytes INTO that section, not a named identity the
            // merge can bind. Redirect it to the atom (a named data/func item OR a
            // step-6.5 synthetic gap atom) that OWNS that byte, with a residual
            // addend = offset-within-that-atom. A reconstructed-atom target keeps
            // its by-identity binding; an extern (SHN_UNDEF) / absolute
            // (SHN_ABS/COMMON) target is untouched. A `.text` section-relative
            // reference (a jump-table entry -> a case block inside a function)
            // resolves to the containing FUNCTION at an interior offset.
            //
            // (6.44) FIRST: a target naming an ALIAS binds to the atom that owns
            // the body, because only the owner is a declared definition -- an id
            // that owns no body is `K_SymbolUndefined` at the linker's compound
            // index. The addend needs no adjustment: an alias shares its owner's
            // `st_value` exactly, so the same S makes the same address. (The
            // section-relative redirect below would reach the same atom by a
            // longer route, computing a residual that works out to the same
            // number -- doing it by identity says what is meant and keeps the
            // three readers' handling of one shape identical.)
            std::uint32_t const targetSym = ownerOf(symIdx);
            SymbolId     relTarget = SymbolId{targetSym};
            std::int64_t relAddend = nativeAddend;
            if (!atomSymIdx.contains(targetSym)) {
                Sym const& tsym = syms[targetSym];
                bool const sectionDefined = tsym.shndx != kShnUndef
                                         && tsym.shndx < kShnLoReserve
                                         && tsym.shndx < eShnum;
                if (sectionDefined) {
                    // The RESIDUAL is invariant under the redirect: the format
                    // applies `value = S + A + (pcRel?-P) + addendBias`, so binding
                    // to `atom` (S = section_base + atom.start) instead of the
                    // section symbol (S = section_base + tsym.value) requires
                    // A' = tsym.value + nativeAddend - atom.start to keep `value`
                    // identical -- independent of pcRel / P / addendBias.
                    std::int64_t const bindBase =
                        static_cast<std::int64_t>(tsym.value) + nativeAddend;
                    // The SEARCH offset is the target's TRUE section offset L, which
                    // depends on how the reference is consumed. A code / absolute
                    // reference resolves to `tsym.value + nativeAddend` (the
                    // addendBias already compensates a code load's RIP+4 bias). A
                    // DATA-section PC-relative SELF-reference -- a `.rodata`
                    // jump-table entry, whose displacement is relative to the
                    // TABLE base (= the containing atom's base = P - relInAtom), not
                    // RIP -- resolves to `rawAddend - relInAtom`; rawAddend =
                    // nativeAddend + addendBias. Getting L right picks the correct
                    // atom when a section holds several (multi-function `.text`); the
                    // residual above stays bindBase - atom.start regardless.
                    std::int64_t searchOff = bindBase;
                    if (tri->pcRelative && patchesData) {
                        std::int64_t const relInAtom =
                            static_cast<std::int64_t>(rOffset)
                            - static_cast<std::int64_t>(iv->start);
                        searchOff = bindBase
                                  + static_cast<std::int64_t>(tri->addendBias)
                                  - relInAtom;
                    }
                    Interval const* hit = nullptr;
                    bool hitFunc = false;
                    if (searchOff >= 0) {
                        std::uint64_t const off = static_cast<std::uint64_t>(searchOff);
                        if (auto funcsIt = funcIntervalsBySec.find(tsym.shndx);
                            funcsIt != funcIntervalsBySec.end()) {
                            if (Interval const* h = findInterval(funcsIt->second, off)) {
                                hit = h; hitFunc = true;
                            }
                        }
                        if (hit == nullptr) {
                            if (auto datasIt = dataIntervalsBySec.find(tsym.shndx);
                                datasIt != dataIntervalsBySec.end()) {
                                hit = findInterval(datasIt->second, off);
                            }
                        }
                    }
                    if (hit == nullptr) {
                        return fail(DiagnosticCode::F_CorruptedBinary,
                            "elf::readRelocatableObject: section-relative relocation "
                            "in '" + rela.name + "' targets section symbol '"
                            + tsym.name + "' + offset " + std::to_string(searchOff)
                            + " (section #" + std::to_string(tsym.shndx) + " '"
                            + secs[tsym.shndx].name + "'), which lands in no "
                            "reconstructed atom -- cannot bind the reference (a "
                            "reference into unmodeled/metadata section content).");
                    }
                    relTarget = hitFunc ? mod.functions[hit->outIdx].symbol
                                        : mod.dataItems[hit->outIdx].symbol;
                    relAddend = bindBase - static_cast<std::int64_t>(hit->start);
                }
            }
            Relocation rel;
            rel.offset = static_cast<std::uint32_t>(rOffset - iv->start);
            rel.target = relTarget;
            rel.kind   = kind;
            rel.addend = relAddend;
            if (patchesText) mod.functions[iv->outIdx].relocations.push_back(rel);
            else             mod.dataItems[iv->outIdx].relocations.push_back(rel);

            // isData inference: an extern reached through one of the format's
            // DECLARED call signals -- x86_64 PLT32 (`pltNativeId`), aarch64
            // CALL26 (`"isCall": true`) -- is a FUNCTION, so force
            // isData=false. A plain data-address reloc (PC32/abs64/GOT) leaves
            // the symtab-type seed intact (NOTYPE defaults to data). This is
            // the fix for the old "any non-PLT reloc => data" rule that
            // misclassified an address-taken extern function and all aarch64
            // extern calls.
            //
            // ⓘ NO EMPTY-SET REFUSAL HERE, and the asymmetry with the Mach-O
            // reader is a real difference rather than an oversight: ELF's
            // st_info carries an INDEPENDENT class hint (STT_FUNC/STT_OBJECT),
            // so an ELF format with no declared call signal still classifies
            // every typed extern correctly and only leaves STT_NOTYPE on its
            // DATA seed. Mach-O's nlist_64 has no such field, which is why
            // there the missing declaration is unrecoverable.
            if (auto ex = externBySym.find(symIdx); ex != externBySym.end()) {
                if (callSignalNativeIds.contains(rType)) {
                    mod.externImports[ex->second].isData = false;
                }
            }
        }
    }

    mod.expectedFuncCount = mod.functions.size();
    return mod;
}

} // namespace dss::elf
