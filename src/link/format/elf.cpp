#include "link/format/elf.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_pass_util.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// ELF64 relocatable (.o) writer — plan 14 LK1 cycle 1.
//
// Byte layout (gABI Ch. 4 + AMD64 psABI §4.4):
//   [0x00]      Elf64_Ehdr (64 B)
//   [0x40]      .text body                  (align 16, already at 64)
//   [...]       pad to 8
//   [...]       .rela.text                  (n_relocs × 24)
//   [...]       pad to 8
//   [...]       .symtab                     (n_syms × 24)
//   [...]       .strtab
//   [...]       .shstrtab
//   [e_shoff]   Section Header Table        (n_sections × 64)
//
// The walker is target-blind in shape — every ELF-specific number
// (machine, class, data encoding, section flags, reloc nativeIds,
// section names) is read from the format schema. The only
// hardcoded structural knowledge is the ELF64 binary record layout.

namespace dss::elf {

namespace {

using lir_pass_util::report;

// ── Elf64 type constants (gABI Ch. 4 + <elf.h>) ─────────────────

constexpr std::uint16_t ET_REL = 1;
constexpr std::uint32_t EV_CURRENT = 1;

// Elf64_Sym st_info encoding (gABI 4.18).
constexpr std::uint8_t STB_LOCAL  = 0;
constexpr std::uint8_t STB_GLOBAL = 1;
constexpr std::uint8_t STT_NOTYPE = 0;
constexpr std::uint8_t STT_FUNC   = 2;
constexpr std::uint8_t STT_SECTION = 3;
constexpr std::uint16_t SHN_UNDEF = 0;

constexpr std::uint8_t makeStInfo(std::uint8_t bind, std::uint8_t type) {
    return static_cast<std::uint8_t>((bind << 4) | (type & 0xF));
}

// Elf64_Rela r_info encoding (gABI 4.13).
constexpr std::uint64_t makeRelaInfo(std::uint32_t symIdx,
                                      std::uint32_t type) {
    return (static_cast<std::uint64_t>(symIdx) << 32)
         | static_cast<std::uint64_t>(type);
}

// ── Byte appenders (LE) ─────────────────────────────────────────

void appendU8(std::vector<std::uint8_t>& out, std::uint8_t v) {
    out.push_back(v);
}
void appendU16LE(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}
void appendU32LE(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}
void appendU64LE(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}
void appendI64LE(std::vector<std::uint8_t>& out, std::int64_t v) {
    appendU64LE(out, static_cast<std::uint64_t>(v));
}

void padTo(std::vector<std::uint8_t>& out, std::uint64_t alignment) {
    if (alignment <= 1) return;
    while (out.size() % alignment != 0) out.push_back(0);
}

// ── String-table builder ────────────────────────────────────────

// ELF string tables start with an empty string (NUL byte) so that
// st_name = 0 == "no name". Returns the offset of the appended
// name; same-name dedup is intentional (multiple symbols may share
// a section name like "" but symbol names should be unique anyway).
class StringTable {
public:
    StringTable() : bytes_{0} {}  // initial NUL

    [[nodiscard]] std::uint32_t add(std::string_view name) {
        if (name.empty()) return 0;
        auto it = offsets_.find(std::string{name});
        if (it != offsets_.end()) return it->second;
        std::uint32_t const offset = static_cast<std::uint32_t>(bytes_.size());
        bytes_.insert(bytes_.end(), name.begin(), name.end());
        bytes_.push_back(0);
        offsets_.emplace(std::string{name}, offset);
        return offset;
    }

    [[nodiscard]] std::span<std::uint8_t const> view() const noexcept {
        return bytes_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

private:
    std::vector<std::uint8_t>                  bytes_;
    std::unordered_map<std::string, std::uint32_t> offsets_;
};

// ── Section header record (in-memory before serialization) ──────

struct SectionHeader {
    std::uint32_t name_offset = 0;   // offset into .shstrtab
    std::uint32_t type = 0;          // sh_type
    std::uint64_t flags = 0;         // sh_flags
    std::uint64_t addr = 0;          // sh_addr (0 in ET_REL)
    std::uint64_t offset = 0;        // sh_offset (filled after layout)
    std::uint64_t size = 0;          // sh_size
    std::uint32_t link = 0;          // sh_link
    std::uint32_t info = 0;          // sh_info
    std::uint64_t addr_align = 0;    // sh_addralign
    std::uint64_t entry_size = 0;    // sh_entsize
};

void writeSectionHeader(std::vector<std::uint8_t>& out, SectionHeader const& h) {
    appendU32LE(out, h.name_offset);
    appendU32LE(out, h.type);
    appendU64LE(out, h.flags);
    appendU64LE(out, h.addr);
    appendU64LE(out, h.offset);
    appendU64LE(out, h.size);
    appendU32LE(out, h.link);
    appendU32LE(out, h.info);
    appendU64LE(out, h.addr_align);
    appendU64LE(out, h.entry_size);
}

void emit(DiagnosticReporter& reporter, DiagnosticCode code,
          std::string msg) {
    report(reporter, code, DiagnosticSeverity::Error, std::move(msg));
}

// Resolve a SectionKind row in the format schema, fail-loud if
// missing. The walker treats absent section declarations as a
// configuration error rather than substituting a default — silent
// defaults are exactly the silent-failure class the substrate
// discipline rejects.
[[nodiscard]] ObjectFormatSectionInfo const*
requireSection(ObjectFormatSchema const& fmt, SectionKind kind,
               DiagnosticReporter& reporter) {
    auto const* s = fmt.sectionByKind(kind);
    if (s == nullptr) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{"ELF writer requires section kind '"}
                 + std::string{sectionKindName(kind)}
                 + "' but object format '"
                 + std::string{fmt.name()}
                 + "' does not declare one");
    }
    return s;
}

} // namespace

std::vector<std::uint8_t>
encode(AssembledModule const&    module,
       TargetSchema const&       targetSchema,
       ObjectFormatSchema const& objectFormatSchema,
       DiagnosticReporter&       reporter) {
    (void)targetSchema;  // Reserved: future cycles read the formula
                         // text when applying relocations in-place
                         // (currently the assembler stamped the
                         // bytes; the ELF writer just serializes the
                         // Rela record).

    auto const& fmt = objectFormatSchema;
    if (fmt.kind() != ObjectFormatKind::Elf) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{"elf::encode called with non-ELF format '"}
                 + std::string{fmt.name()}
                 + "' (kind="
                 + std::string{objectFormatKindName(fmt.kind())}
                 + ")");
        return {};
    }

    // Resolve the section schema rows we need. Failure here means
    // the format JSON is incomplete; we surface as K_* + bail.
    auto const* secText      = requireSection(fmt, SectionKind::Text,      reporter);
    auto const* secRela      = requireSection(fmt, SectionKind::RelocTable, reporter);
    auto const* secSymtab    = requireSection(fmt, SectionKind::Symtab,    reporter);
    auto const* secStrtab    = requireSection(fmt, SectionKind::Strtab,    reporter);
    auto const* secShStrtab  = requireSection(fmt, SectionKind::ShStrtab,  reporter);
    if (!secText || !secRela || !secSymtab || !secStrtab || !secShStrtab) {
        return {};
    }

    // ── Build .text + per-function symbols ─────────────────────
    //
    // Concatenate every AssembledFunction's bytes into one .text
    // section, recording each function's start offset for its
    // symbol's `st_value`.
    std::vector<std::uint8_t> text;
    struct FuncSymRecord {
        SymbolId     symId{};
        std::uint64_t valueInText = 0;
        std::uint64_t size = 0;
    };
    std::vector<FuncSymRecord> funcSyms;
    funcSyms.reserve(module.functions.size());

    std::vector<std::uint64_t> funcTextStart;
    funcTextStart.reserve(module.functions.size());

    for (auto const& fn : module.functions) {
        std::uint64_t const start = text.size();
        funcTextStart.push_back(start);
        text.insert(text.end(), fn.bytes.begin(), fn.bytes.end());
        funcSyms.push_back({fn.symbol, start,
                            static_cast<std::uint64_t>(fn.bytes.size())});
    }

    // ── Build .strtab + .symtab ────────────────────────────────
    //
    // Symbol layout: STN_UNDEF (idx 0) → STT_SECTION for .text
    // (LOCAL) → defined function symbols (GLOBAL) → undefined extern
    // symbols (GLOBAL, SHN_UNDEF). `.symtab.sh_info` = index of
    // first non-LOCAL symbol.

    StringTable strtab;
    std::vector<std::uint8_t> symtab;

    // Helper: emit one Elf64_Sym record (24 bytes).
    auto appendSym = [&](std::uint32_t nameOff, std::uint8_t info,
                          std::uint8_t other, std::uint16_t shndx,
                          std::uint64_t value, std::uint64_t size) {
        appendU32LE(symtab, nameOff);
        appendU8(symtab, info);
        appendU8(symtab, other);
        appendU16LE(symtab, shndx);
        appendU64LE(symtab, value);
        appendU64LE(symtab, size);
    };

    // Symbol index 0: STN_UNDEF (24 zero bytes).
    appendSym(0, 0, 0, 0, 0, 0);

    // Index 1: STT_SECTION for .text — the relocation base most
    // ELF consumers expect for section-relative references. Its
    // st_shndx points at the .text section index; we pin that to
    // IDX_TEXT (=1) by the section ordering below.
    appendSym(0, makeStInfo(STB_LOCAL, STT_SECTION),
              0, /*shndx=.text*/ 1, 0, 0);
    std::uint32_t const firstNonLocalSymIdx = 2;

    // Map each function's SymbolId to its symtab index (for relocs).
    std::unordered_map<SymbolId, std::uint32_t> symIdxBySymbol;

    // Defined function symbols (GLOBAL + STT_FUNC + shndx=.text).
    for (auto const& f : funcSyms) {
        std::string const symName =
            std::string{"sym_"} + std::to_string(f.symId.v);
        std::uint32_t const nameOff = strtab.add(symName);
        std::uint32_t const idx =
            static_cast<std::uint32_t>(symtab.size() / 24);
        appendSym(nameOff, makeStInfo(STB_GLOBAL, STT_FUNC), 0,
                  /*shndx=.text*/ 1, f.valueInText, f.size);
        symIdxBySymbol.emplace(f.symId, idx);
    }

    // Undefined extern symbols referenced by any relocation but not
    // defined by any function.
    for (auto const& fn : module.functions) {
        for (auto const& rel : fn.relocations) {
            if (symIdxBySymbol.contains(rel.target)) continue;
            std::string const symName =
                std::string{"sym_"} + std::to_string(rel.target.v);
            std::uint32_t const nameOff = strtab.add(symName);
            std::uint32_t const idx =
                static_cast<std::uint32_t>(symtab.size() / 24);
            appendSym(nameOff, makeStInfo(STB_GLOBAL, STT_NOTYPE), 0,
                      SHN_UNDEF, 0, 0);
            symIdxBySymbol.emplace(rel.target, idx);
        }
    }

    // ── Build .rela.text ───────────────────────────────────────
    //
    // For each AssembledFunction's relocations, compute the absolute
    // offset within .text (the function's range start + the local
    // offset) and translate `kind → nativeId` via the format schema.

    std::vector<std::uint8_t> relaText;
    for (std::size_t fi = 0; fi < module.functions.size(); ++fi) {
        auto const& fn = module.functions[fi];
        std::uint64_t const fnStart = funcTextStart[fi];
        for (auto const& rel : fn.relocations) {
            auto const* fmtReloc = fmt.relocationByKind(rel.kind);
            if (fmtReloc == nullptr) {
                // Should have been caught by `link()` substrate but
                // re-check defensively — silent skip is exactly the
                // failure class the substrate discipline rejects.
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     "elf::encode: relocation kind "
                         + std::to_string(rel.kind.v)
                         + " not declared by ELF format '"
                         + std::string{fmt.name()} + "'");
                continue;
            }
            auto it = symIdxBySymbol.find(rel.target);
            if (it == symIdxBySymbol.end()) {
                emit(reporter, DiagnosticCode::K_SymbolUndefined,
                     "elf::encode: relocation target symbol #"
                         + std::to_string(rel.target.v)
                         + " has no symtab entry");
                continue;
            }
            std::uint32_t const symIdx = it->second;
            std::uint64_t const rOffset = fnStart + rel.offset;
            appendU64LE(relaText, rOffset);
            appendU64LE(relaText, makeRelaInfo(symIdx, fmtReloc->nativeId));
            appendI64LE(relaText, rel.addend);
        }
    }

    // ── Section ordering + .shstrtab ───────────────────────────
    //
    // Order: SHT_NULL, .text, .rela.text, .symtab, .strtab, .shstrtab.
    StringTable shstrtab;
    SectionHeader hNull{};
    SectionHeader hText{};
    SectionHeader hRela{};
    SectionHeader hSymtab{};
    SectionHeader hStrtab{};
    SectionHeader hShStrtab{};
    hText.name_offset      = shstrtab.add(secText->name);
    hRela.name_offset      = shstrtab.add(secRela->name);
    hSymtab.name_offset    = shstrtab.add(secSymtab->name);
    hStrtab.name_offset    = shstrtab.add(secStrtab->name);
    hShStrtab.name_offset  = shstrtab.add(secShStrtab->name);

    constexpr std::uint16_t IDX_TEXT     = 1;
    constexpr std::uint16_t IDX_SYMTAB   = 3;
    constexpr std::uint16_t IDX_STRTAB   = 4;
    constexpr std::uint16_t IDX_SHSTRTAB = 5;
    // IDX_TEXT == 1 is the pinned ordering for the .text STT_SECTION
    // symbol emitted above (its st_shndx field is set to 1 directly).

    hText.type       = secText->type;
    hText.flags      = secText->flags;
    hText.addr_align = secText->addrAlign;
    hText.entry_size = secText->entrySize;
    hText.size       = text.size();

    hRela.type       = secRela->type;
    hRela.flags      = secRela->flags;
    hRela.addr_align = secRela->addrAlign;
    hRela.entry_size = secRela->entrySize;  // 24 for Elf64_Rela
    hRela.link       = IDX_SYMTAB;
    hRela.info       = IDX_TEXT;
    hRela.size       = relaText.size();

    hSymtab.type       = secSymtab->type;
    hSymtab.flags      = secSymtab->flags;
    hSymtab.addr_align = secSymtab->addrAlign;
    hSymtab.entry_size = secSymtab->entrySize;  // 24 for Elf64_Sym
    hSymtab.link       = IDX_STRTAB;
    hSymtab.info       = firstNonLocalSymIdx;
    hSymtab.size       = symtab.size();

    hStrtab.type       = secStrtab->type;
    hStrtab.flags      = secStrtab->flags;
    hStrtab.addr_align = std::max<std::uint64_t>(1, secStrtab->addrAlign);
    hStrtab.entry_size = secStrtab->entrySize;
    hStrtab.size       = strtab.size();

    hShStrtab.type       = secShStrtab->type;
    hShStrtab.flags      = secShStrtab->flags;
    hShStrtab.addr_align = std::max<std::uint64_t>(1, secShStrtab->addrAlign);
    hShStrtab.entry_size = secShStrtab->entrySize;
    hShStrtab.size       = shstrtab.size();

    // ── Layout pass: compute sh_offset for each section ────────
    constexpr std::uint64_t kEhdrSize = 64;
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kEhdrSize + text.size() + relaText.size()
                  + symtab.size() + strtab.size() + shstrtab.size() + 6 * 64);
    bytes.resize(kEhdrSize);  // placeholder; rewritten below

    // Single layout lambda — `vector<uint8_t> const&` decays to
    // `span<uint8_t const>` so both the in-memory section bodies
    // (text / relaText / symtab) and the StringTable views share
    // one code path.
    auto layoutSection = [&](SectionHeader& h, std::span<std::uint8_t const> body) {
        if (h.addr_align > 1) padTo(bytes, h.addr_align);
        h.offset = bytes.size();
        bytes.insert(bytes.end(), body.begin(), body.end());
    };

    layoutSection(hText, text);
    layoutSection(hRela, relaText);
    layoutSection(hSymtab, symtab);
    layoutSection(hStrtab, strtab.view());
    layoutSection(hShStrtab, shstrtab.view());

    padTo(bytes, 8);  // SHT alignment
    std::uint64_t const shoff = bytes.size();
    // Single source of truth for the section count so future cycles
    // (LK1 ELF executable adding .data/.rodata/.bss, LK6 dynamic
    // linking adding .dynamic/.dynsym/.dynstr) cannot drift between
    // the Ehdr's e_shnum and the actual table size.
    SectionHeader const* const headers[] = {
        &hNull, &hText, &hRela, &hSymtab, &hStrtab, &hShStrtab
    };
    std::uint16_t const sectionCount =
        static_cast<std::uint16_t>(std::size(headers));
    for (auto const* h : headers) writeSectionHeader(bytes, *h);

    // ── Elf64_Ehdr (overwrite the leading 64 zero bytes) ───────
    auto const& id = fmt.elf();
    std::vector<std::uint8_t> ehdr;
    ehdr.reserve(kEhdrSize);
    // e_ident
    ehdr.push_back(0x7F); ehdr.push_back('E');
    ehdr.push_back('L');  ehdr.push_back('F');
    ehdr.push_back(id.fileClass);
    ehdr.push_back(id.dataEncoding);
    ehdr.push_back(static_cast<std::uint8_t>(EV_CURRENT));
    ehdr.push_back(id.osabi);
    ehdr.push_back(id.abiVersion);
    for (int i = 0; i < 7; ++i) ehdr.push_back(0);  // EI_PAD
    // e_type, e_machine, e_version
    appendU16LE(ehdr, ET_REL);
    appendU16LE(ehdr, id.machine);
    appendU32LE(ehdr, EV_CURRENT);
    // e_entry, e_phoff, e_shoff
    appendU64LE(ehdr, 0);
    appendU64LE(ehdr, 0);
    appendU64LE(ehdr, shoff);
    // e_flags, e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx
    appendU32LE(ehdr, 0);
    appendU16LE(ehdr, static_cast<std::uint16_t>(kEhdrSize));
    appendU16LE(ehdr, 0);
    appendU16LE(ehdr, 0);
    appendU16LE(ehdr, 64);  // sizeof(Elf64_Shdr)
    appendU16LE(ehdr, sectionCount);
    appendU16LE(ehdr, IDX_SHSTRTAB);
    std::memcpy(bytes.data(), ehdr.data(), kEhdrSize);

    return bytes;
}

} // namespace dss::elf
