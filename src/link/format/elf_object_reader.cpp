#include "link/format/elf_object_reader.hpp"

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

// Is this target-schema reloc formula a FUNCTION CALL / BRANCH (as opposed to
// a data-address computation)? The AGNOSTIC "extern is a function" signal:
// aarch64 R_AARCH64_CALL26 / JUMP26 carry the `Aarch64Call26` formula (a call
// with NO PLT-variant native id in the schema). Combined with the x86_64
// PLT-variant native id (a `pltNativeId` row), this is what distinguishes an
// extern FUNCTION from an extern DATA object without a hardcoded reloc number.
[[nodiscard]] constexpr bool isCallBranchFormula(RelocFormulaKind k) noexcept {
    return k == RelocFormulaKind::Aarch64Call26;
}

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
    if (objectFormatSchema.kind() != ObjectFormatKind::Elf) {
        return fail(DiagnosticCode::F_UnsupportedBinaryFormat,
            std::string{"elf::readRelocatableObject: object format schema '"}
                + std::string{objectFormatSchema.name()} + "' is kind "
                + std::string{objectFormatKindName(objectFormatSchema.kind())}
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
    // FUNCTION: the x86_64 PLT (call-through-stub) variant (`pltNativeId`) AND
    // the native id of any row whose target-schema formula is a call/branch
    // class (aarch64 CALL26, which has NO pltNativeId). This is the agnostic
    // "is a function" signal for isData inference below -- the plain PC32 a
    // DATA reference uses is deliberately NOT in this set.
    std::unordered_map<std::uint32_t, RelocationKind> nativeToKind;
    std::unordered_set<std::uint32_t> callSignalNativeIds;
    auto mapNative = [&](std::uint32_t nid, RelocationKind kind)
        -> std::optional<std::optional<AssembledModule>> {
        // Duplicate-nativeId guard: a native id mapping to two DIFFERENT kinds
        // is an ambiguous schema -- fail loud rather than let "last row wins"
        // silently mis-decode. Returns a wrapped failure to propagate, or
        // nullopt to continue.
        auto const ins = nativeToKind.emplace(nid, kind);
        if (!ins.second && ins.first->second.v != kind.v) {
            return std::optional<std::optional<AssembledModule>>{
                fail(DiagnosticCode::F_CorruptedBinary,
                     "elf::readRelocatableObject: object format schema '"
                     + std::string{objectFormatSchema.name()}
                     + "' maps native reloc id " + std::to_string(nid)
                     + " to two different RelocationKinds ("
                     + std::to_string(ins.first->second.v) + " and "
                     + std::to_string(kind.v) + ") -- ambiguous reverse map.")};
        }
        return std::nullopt;
    };
    for (auto const& r : objectFormatSchema.relocations()) {
        if (auto f = mapNative(r.nativeId, r.kind); f.has_value()) return *f;
        if (r.pltNativeId != 0u) {
            if (auto f = mapNative(r.pltNativeId, r.kind); f.has_value()) return *f;
            callSignalNativeIds.insert(r.pltNativeId);  // the PLT call-through-stub variant
        }
        if (auto const* tri = targetSchema.relocationInfo(r.kind);
            tri != nullptr && isCallBranchFormula(tri->formulaKind)) {
            callSignalNativeIds.insert(r.nativeId);      // a call/branch reloc (aarch64 CALL26)
        }
    }

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

        // A function MUST live in an executable section. A STT_FUNC anywhere
        // else is corrupt -- fail loud (never mis-slice code as data).
        if (type == kSttFunc && rk != SectionKind::Text) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "elf::readRelocatableObject: function symbol '" + sy.name
                + "' lives in section '" + sec.name
                + "' which does not resolve to an executable (Text) kind.");
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
        pushModuleSym();
    }

    // MEDIUM guard: overlapping STT_FUNC (st_value,st_size) ranges within one
    // section -- a relocation site inside the overlap would mis-route to
    // whichever function `findInterval` returns first. Fail loud on a
    // DIFFERENT-start intersection (a genuine nesting/partial overlap); EQUAL
    // -start ranges (aliases -- a weak alias + its target at the same address)
    // share the SAME item-relative offset, so routing to either is offset
    // -correct and is allowed.
    for (auto& [secIdx, ivs] : funcIntervalsBySec) {
        std::vector<Interval> sorted = ivs;
        std::sort(sorted.begin(), sorted.end(),
                  [](Interval const& a, Interval const& b) { return a.start < b.start; });
        for (std::size_t k = 1; k < sorted.size(); ++k) {
            Interval const& prev = sorted[k - 1];
            Interval const& cur = sorted[k];
            if (cur.start != prev.start && cur.start < prev.start + prev.len) {
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
        // (a fresh sorted copy -- `emitGap` appends to the live map vector).
        std::vector<Interval> covered;
        if (auto it = dataIntervalsBySec.find(si); it != dataIntervalsBySec.end())
            covered = it->second;
        std::sort(covered.begin(), covered.end(),
                  [](Interval const& a, Interval const& b) { return a.start < b.start; });
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
        std::uint64_t cursor = 0;
        for (auto const& iv : covered) {
            if (iv.start > cursor) emitGap(cursor, iv.start);
            cursor = std::max(cursor, iv.start + iv.len);
        }
        if (cursor < sec.size) emitGap(cursor, sec.size);
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
            SymbolId     relTarget = SymbolId{symIdx};
            std::int64_t relAddend = nativeAddend;
            if (!atomSymIdx.contains(symIdx)) {
                Sym const& tsym = syms[symIdx];
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

            // isData inference: an extern reached through a CALL/BRANCH-class
            // reloc (x86_64 PLT32, aarch64 CALL26) is a FUNCTION -- force
            // isData=false. A plain data-address reloc (PC32/abs64/GOT) leaves
            // the symtab-type seed intact (NOTYPE defaults to data). This is
            // the fix for the old "any non-PLT reloc => data" rule that
            // misclassified an address-taken extern function and all aarch64
            // extern calls.
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
