// D-LK-OBJECT-READERS-MISREAD-COMMON-SYMBOLS (P69) — a COMMON (tentative) definition, from every object reader through
// the link and every writer.
//
// A common is storage an object DEFINES without placing: COFF writes it as an UNDEF external whose Value is its size,
// ELF as `SHN_COMMON` with its alignment in `st_value`, Mach-O as an `N_UNDF` external whose `n_value` is its size.
// ✔MEASURED 2026-10-06: cl 19.51 writes EVERY C tentative definition (`int x;` at file scope) this way, at /O2 and /Od
// (probe run 20261006-224902-a6201782), as gcc and clang do under -fcommon. Before P69 the COFF reader refused one,
// the ELF reader made it a definition with no storage, and the Mach-O reader read it as an import.
//
//   * THE LINK (`linker::link`, synthetic modules): two commons of one name are ONE object — the larger, at the wider
//     alignment, in `.bss`; a STRONG definition wins and keeps its bytes; a WEAK definition is what the link's
//     document says (`commonYieldsTo`, P69 round 4): the common outranks it on ELF, it replaces the common on Mach-O
//     and PE, and a document that says nothing is refused by name; a relocatable artifact hands a common on, a merge
//     folding two into the larger and the wider, and keeps one that no relocation names.
//   * EVERY WRITER WITH ITS READER: a common written into a COFF, an ELF and a Mach-O relocatable object reads back as
//     the same common — size, alignment, visibility — and the COFF one states its alignment in `-aligncomm:` only
//     where link.exe would align it less.
//   * NATIVE (Windows): two cl TUs that both tentatively define `shared_counter` and `shared_array` (of different
//     sizes) link under DSS to the program link.exe makes of them; mingw-w64 gcc -fcommon TUs likewise.
//   * THE ARCHIVE SEARCH (D-LK-ARCHIVE-SEARCH-FETCHED-A-DEFINITION-FOR-A-COMMON, P69 round 3): beside an archive member
//     that defines a common's name, the members' document says whether the search fetches it — ELF and Mach-O yes, as
//     GNU ld and Apple's ld do, each fetching the first member whose datum the common yields to; PE no, as link.exe,
//     lld-link and GNU ld's PE linker do. Whether a common yields to a definition is ONE answer
//     (`linker::commonYieldsToDefinition`) the search and the link's allocation both ask, and a link document that
//     does not give it is refused by name in both.
//   * NATIVE, EVERY HOST (P69 round 4): the host's reference compiler's common beside a weak definition and beside
//     archive members, under its own linker and under DSS, cell by cell.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/common_yield.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/format/elf.hpp"
#include "link/format/ar.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/format/macho.hpp"
#include "link/format/macho_object_reader.hpp"
#include "link/format/pe.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "program/compile_pipeline.hpp"
#include "program/program.hpp"
#include "host_translations.hpp"
#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"
#include "../core/native_c_probe.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <iostream>
#include <iterator>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

struct Loaded {
    std::shared_ptr<TargetSchema const>       target;
    std::shared_ptr<ObjectFormatSchema const> format;
};

[[nodiscard]] Loaded load(char const* target, char const* format) {
    Loaded l;
    auto t = TargetSchema::loadShipped(target);
    auto f = ObjectFormatSchema::loadShipped(format);
    EXPECT_TRUE(t.has_value() && f.has_value()) << target << " / " << format;
    if (t.has_value()) l.target = *t;
    if (f.has_value()) l.format = *f;
    return l;
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s;
}

[[nodiscard]] ExternImport commonRow(std::uint32_t id, std::string name, std::uint64_t size, std::uint64_t alignment,
                                     SymbolVisibility visibility = SymbolVisibility::Default) {
    ExternImport e;
    e.symbol           = SymbolId{id};
    e.mangledName      = std::move(name);
    e.isData           = true;
    e.commonSize       = size;
    e.commonAlignment  = alignment;
    e.commonVisibility = visibility;
    return e;
}

// A function `fnName` reading the datum `target` (`mov eax, [rip+disp32] ; ret`, riprel32 = kind 8 on x86_64).
[[nodiscard]] AssembledFunction reader(std::uint32_t id, std::uint32_t target) {
    AssembledFunction fn;
    fn.symbol = SymbolId{id};
    fn.bytes  = {0x8B, 0x05, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 2;
    rel.target = SymbolId{target};
    rel.kind   = RelocationKind{8};
    rel.addend = 0;
    fn.relocations.push_back(rel);
    return fn;
}

// CU `cu`: one function `fnName` reading the name `shared`, which this unit holds as a common of `size` / `align` —
// or, with `size` 0, as a plain reference.
[[nodiscard]] AssembledModule unitWithCommon(std::uint32_t cu, std::string fnName, std::uint64_t size,
                                             std::uint64_t align, bool entry) {
    AssembledModule m;
    m.cuId              = CompilationUnitId{cu};
    m.expectedFuncCount = 1;
    m.functions.push_back(reader(1, 2));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, std::move(fnName), SymbolBinding::Global, SymbolVisibility::Default});
    if (size != 0) {
        m.externImports.push_back(commonRow(2, "shared", size, align));
    } else {
        ExternImport ref;
        ref.symbol      = SymbolId{2};
        ref.mangledName = "shared";
        ref.isData      = true;
        m.externImports.push_back(std::move(ref));
    }
    if (entry) m.userEntrySymbol = SymbolId{1};
    return m;
}

// CU `cu`: a DEFINITION of `shared` — 4 initialized bytes, `binding`.
[[nodiscard]] AssembledModule unitDefiningShared(std::uint32_t cu, SymbolBinding binding) {
    AssembledModule m;
    m.cuId              = CompilationUnitId{cu};
    m.expectedFuncCount = 0;
    AssembledData d;
    d.symbol    = SymbolId{1};
    d.section   = DataSectionKind::Data;
    d.bytes     = {7, 0, 0, 0};
    d.alignment = Alignment::of<4>();
    m.dataItems.push_back(d);
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "shared", binding, SymbolVisibility::Default});
    return m;
}

// ── A minimal ELF64 reader: section headers and `.symtab` rows ─────────────
[[nodiscard]] std::uint64_t rdLE(std::vector<std::uint8_t> const& b, std::size_t off, int width) {
    std::uint64_t v = 0;
    for (int i = 0; i < width; ++i) v |= static_cast<std::uint64_t>(b[off + i]) << (i * 8);
    return v;
}
struct ElfSection {
    std::string   name;
    std::uint64_t addr = 0, offset = 0, size = 0;
};
[[nodiscard]] std::string cstrAt(std::vector<std::uint8_t> const& b, std::uint64_t off) {
    std::string s;
    for (std::uint64_t p = off; p < b.size() && b[p] != 0; ++p) s.push_back(static_cast<char>(b[p]));
    return s;
}
[[nodiscard]] std::vector<ElfSection> elfSections(std::vector<std::uint8_t> const& b) {
    std::uint64_t const shoff    = rdLE(b, 40, 8);
    auto const          shnum    = static_cast<std::uint16_t>(rdLE(b, 60, 2));
    auto const          shstrndx = static_cast<std::uint16_t>(rdLE(b, 62, 2));
    std::uint64_t const strOff   = rdLE(b, shoff + shstrndx * 64ull + 24, 8);
    std::vector<ElfSection> out;
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::uint64_t const off = shoff + static_cast<std::uint64_t>(i) * 64;
        out.push_back(ElfSection{cstrAt(b, strOff + rdLE(b, off, 4)), rdLE(b, off + 16, 8), rdLE(b, off + 24, 8),
                                 rdLE(b, off + 32, 8)});
    }
    return out;
}
struct ElfSym {
    std::string   name;
    std::uint8_t  bind = 0;
    std::uint8_t  other = 0;
    std::uint16_t shndx = 0;
    std::uint64_t value = 0, size = 0;
};
[[nodiscard]] std::vector<ElfSym> elfSymbolsNamed(std::vector<std::uint8_t> const& b, std::string const& name) {
    auto const secs = elfSections(b);
    ElfSection const* sy = nullptr;
    ElfSection const* st = nullptr;
    for (auto const& s : secs) {
        if (s.name == ".symtab") sy = &s;
        if (s.name == ".strtab") st = &s;
    }
    std::vector<ElfSym> out;
    if (sy == nullptr || st == nullptr) return out;
    for (std::uint64_t p = 24; p + 24 <= sy->size; p += 24) {
        std::uint64_t const off = sy->offset + p;
        ElfSym s{cstrAt(b, st->offset + rdLE(b, off, 4)), static_cast<std::uint8_t>(b[off + 4] >> 4), b[off + 5],
                 static_cast<std::uint16_t>(rdLE(b, off + 6, 2)), rdLE(b, off + 8, 8), rdLE(b, off + 16, 8)};
        if (s.name == name) out.push_back(std::move(s));
    }
    return out;
}
constexpr std::uint16_t kShnCommon = 0xfff2;

// THE ADDRESS `fnName`'s READ REACHES in a linked ELF image. An image's `.symtab` names its FUNCTIONS and no datum at
// all (D-LINK-ELF-IMAGE-NO-DATA-SYMBOLS-IN-SYMTAB, open), so the datum is found the way the CPU finds it: `reader`'s
// `mov eax, [rip+disp32]` reaches fn + 6 + disp32, the displacement the link patched.
[[nodiscard]] std::optional<std::uint64_t> datumReadBy(std::vector<std::uint8_t> const& b, std::string const& fnName) {
    auto const fn = elfSymbolsNamed(b, fnName);
    if (fn.size() != 1u) return std::nullopt;
    for (auto const& s : elfSections(b)) {
        if (s.addr == 0 || fn[0].value < s.addr || fn[0].value + 6 > s.addr + s.size) continue;
        std::uint64_t const at   = s.offset + (fn[0].value - s.addr);
        auto const          disp = static_cast<std::int32_t>(static_cast<std::uint32_t>(rdLE(b, at + 2, 4)));
        return fn[0].value + 6 + static_cast<std::uint64_t>(static_cast<std::int64_t>(disp));
    }
    return std::nullopt;
}

// The loaded section whose addresses hold `va`.
[[nodiscard]] std::optional<ElfSection> sectionHolding(std::vector<std::uint8_t> const& b, std::uint64_t va) {
    for (auto const& s : elfSections(b)) {
        if (s.addr != 0 && va >= s.addr && va < s.addr + s.size) return s;
    }
    return std::nullopt;
}

// How many bytes of its section lie at and after `va`: a common allocated there has at least its size of them.
[[nodiscard]] std::uint64_t storageFrom(ElfSection const& s, std::uint64_t va) { return s.addr + s.size - va; }

[[nodiscard]] ExternImport const* rowNamed(AssembledModule const& m, std::string const& name) {
    for (auto const& e : m.externImports) {
        if (e.mangledName == name) return &e;
    }
    return nullptr;
}

// The ELF exec document with `commonYieldsTo` (and its comment) removed: a LINK document that does not say which
// definitions a common yields to.
[[nodiscard]] std::shared_ptr<ObjectFormatSchema const> elfExecDocumentWithoutCommonYieldsTo() {
    auto const path = dss::test::configRoot() / "object-formats" / "elf64-x86_64-linux-exec.format.json";
    std::ifstream in{path};
    auto doc = nlohmann::json::parse(in);
    EXPECT_TRUE(doc.contains("commonYieldsTo"));
    doc.erase("commonYieldsTo");
    doc.erase("$commonYieldsToComment");
    auto const silent = ObjectFormatSchema::loadFromText(doc.dump(), "elf64-x86_64-linux-exec, commonYieldsTo removed");
    EXPECT_TRUE(silent.has_value());
    if (!silent.has_value()) return nullptr;
    return *silent;
}

// Whether `rep` holds the refusal that names the key `commonYieldsTo` (and, when given, `alsoNamed`).
[[nodiscard]] bool refusalNamesTheKey(DiagnosticReporter const& rep, std::string_view alsoNamed = {}) {
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_NoMatchingObjectFormat
            && d.actual.find("'commonYieldsTo'") != std::string::npos
            && (alsoNamed.empty() || d.actual.find(alsoNamed) != std::string::npos)) {
            return true;
        }
    }
    return false;
}

}  // namespace

// ══ The link ══════════════════════════════════════════════════════════════════

// Two units hold a common `shared` of 4 and of 40 bytes (aligned 4 and 32): the image holds ONE `shared`, in `.bss`,
// as large as the larger and aligned to the wider — GNU ld's documented merge — and both units' reads reach it. The
// CONTROL links the 4-byte common alone, so "at least 40 bytes of `.bss` from the object on" is shown to tell the
// larger common from the smaller rather than to measure padding.
TEST(CommonSymbols, TwoCommonsOfOneNameAreOneObjectTheLargerAtTheWiderAlignment) {
    auto const L = load("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_TRUE(L.target && L.format);
    std::vector<AssembledModule> mods{unitWithCommon(1, "small_reader", 4, 4, /*entry=*/true),
                                      unitWithCommon(2, "large_reader", 40, 32, /*entry=*/false)};
    DiagnosticReporter rep;
    auto const img = linker::link(std::span<AssembledModule const>{mods}, *L.target, *L.format, rep);
    ASSERT_TRUE(img.ok()) << diagnosticsOf(rep);
    auto const small = datumReadBy(img.bytes, "small_reader");
    auto const large = datumReadBy(img.bytes, "large_reader");
    ASSERT_TRUE(small.has_value() && large.has_value()) << "both readers must be in the image's `.symtab`";
    EXPECT_EQ(*small, *large) << "one name, one object: both units' reads reach it";
    auto const bss = sectionHolding(img.bytes, *small);
    ASSERT_TRUE(bss.has_value()) << "the common's address lies in no loaded section";
    EXPECT_EQ(bss->name, ".bss") << "zero-filled storage";
    EXPECT_EQ(*small % 32u, 0u) << "at the WIDER alignment";
    EXPECT_GE(storageFrom(*bss, *small), 40u) << "the LARGER common's 40 bytes";

    std::vector<AssembledModule> alone{unitWithCommon(1, "small_reader", 4, 4, /*entry=*/true)};
    DiagnosticReporter crep;
    auto const cimg = linker::link(std::span<AssembledModule const>{alone}, *L.target, *L.format, crep);
    ASSERT_TRUE(cimg.ok()) << diagnosticsOf(crep);
    auto const only = datumReadBy(cimg.bytes, "small_reader");
    ASSERT_TRUE(only.has_value());
    auto const cbss = sectionHolding(cimg.bytes, *only);
    ASSERT_TRUE(cbss.has_value());
    EXPECT_LT(storageFrom(*cbss, *only), 40u) << "CONTROL: a 4-byte common alone leaves fewer than 40 bytes";
}

// A unit that DEFINES the name wins: the commons become references to its bytes (GNU ld's `--warn-common`: "turning a
// common symbol into a reference, because there is already a definition").
TEST(CommonSymbols, AStrongDefinitionWinsAndKeepsItsBytes) {
    auto const L = load("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_TRUE(L.target && L.format);
    std::vector<AssembledModule> mods{unitWithCommon(1, "small_reader", 4, 4, /*entry=*/true),
                                      unitWithCommon(2, "large_reader", 40, 32, /*entry=*/false),
                                      unitDefiningShared(3, SymbolBinding::Global)};
    DiagnosticReporter rep;
    auto const img = linker::link(std::span<AssembledModule const>{mods}, *L.target, *L.format, rep);
    ASSERT_TRUE(img.ok()) << diagnosticsOf(rep);
    auto const small = datumReadBy(img.bytes, "small_reader");
    auto const large = datumReadBy(img.bytes, "large_reader");
    ASSERT_TRUE(small.has_value() && large.has_value());
    EXPECT_EQ(*small, *large) << "both commons became references to the one definition";
    auto const data = sectionHolding(img.bytes, *small);
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(data->name, ".data") << "the definition's own initialized storage";
    EXPECT_EQ(img.bytes[data->offset + (*small - data->addr)], 7u) << "the definition's own bytes";
}

// On ELF a common wins over a WEAK definition (the System V gABI: "the link editor honors the common definition and
// ignores the weak ones"; the document's `commonYieldsTo` is `strongDefinition`, ✔MEASURED 2026-10-07 with GNU ld
// and ld.lld). The common's STB_GLOBAL binding once allocated is not observable here: the image's `.symtab` lists no
// datum (D-LINK-ELF-IMAGE-NO-DATA-SYMBOLS-IN-SYMTAB). Mach-O and PE answer the other way:
// `AWeakDefinitionReplacesTheCommonWhereTheLinksDocumentSaysSo`.
TEST(CommonSymbols, ACommonWinsOverAWeakDefinition) {
    auto const L = load("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_TRUE(L.target && L.format);
    std::vector<AssembledModule> mods{unitWithCommon(1, "large_reader", 40, 32, /*entry=*/true),
                                      unitDefiningShared(2, SymbolBinding::Weak)};
    DiagnosticReporter rep;
    auto const img = linker::link(std::span<AssembledModule const>{mods}, *L.target, *L.format, rep);
    ASSERT_TRUE(img.ok()) << diagnosticsOf(rep);
    auto const at = datumReadBy(img.bytes, "large_reader");
    ASSERT_TRUE(at.has_value());
    auto const bss = sectionHolding(img.bytes, *at);
    ASSERT_TRUE(bss.has_value());
    EXPECT_EQ(bss->name, ".bss") << "the common's storage, not the weak definition's initialized bytes";
    EXPECT_EQ(*at % 32u, 0u);
    EXPECT_GE(storageFrom(*bss, *at), 40u);
}

// Which definitions a common yields to is the LINK's document's answer (`commonYieldsTo`, P69 round 4,
// D-LK-COMMON-OUTRANKED-A-WEAK-DEFINITION-IN-EVERY-FORMAT). On Mach-O and PE a WEAK definition replaces the common as a
// strong one does (✔MEASURED 2026-10-07: Apple clang 21's ld-1267 and ld64-957.1, arm64 and x86_64; link.exe 14.44 and
// lld-link 19.1.5 on cl 19.44's `__declspec(selectany)`; both link orders), so a relocatable artifact linked from a
// unit holding `shared` as a 40-byte common and one defining it WEAK (7) carries the weak definition — still weak, its
// own 7 — and no common. CONTROLS: the ELF document's answer is the gABI's, so the same two units carry the common's
// 40 zero-filled bytes, strong (the image twin is `ACommonWinsOverAWeakDefinition`); and on PE a weak definition whose
// own SPELLING yields to a common (`ModuleSymbol::yieldsToACommon`, what the COFF reader reads a weak external's body
// as — MinGW gcc's weak definition, which GNU ld's PE linker gives the common's 0) loses to it too. Until round 4
// every format took ELF's answer, so a Mach-O or PE link read 0 where Apple's ld and link.exe read the weak
// definition's value.
TEST(CommonSymbols, AWeakDefinitionReplacesTheCommonWhereTheLinksDocumentSaysSo) {
    struct Arm {
        char const*    format;
        std::string    shared;          // the name as the format's C decoration spells it
        CommonYieldsTo yields;          // the document's answer
        bool           spellingYields;  // the weak definition's own spelling yields to a common
        bool           weakWins;
    };
    for (auto const& arm : {Arm{"macho64-x86_64-darwin", "_shared", CommonYieldsTo::AnyDefinition, false, true},
                            Arm{"pe64-x86_64-windows", "shared", CommonYieldsTo::AnyDefinition, false, true},
                            Arm{"pe64-x86_64-windows", "shared", CommonYieldsTo::AnyDefinition, true, false},
                            Arm{"elf64-x86_64-linux", "shared", CommonYieldsTo::StrongDefinition, false, false}}) {
        SCOPED_TRACE(std::string{arm.format} + (arm.spellingYields ? " (a spelling that yields)" : ""));
        auto const L = load("x86_64", arm.format);
        ASSERT_TRUE(L.target && L.format);
        EXPECT_EQ(L.format->commonYieldsTo(), arm.yields) << "the document's own answer";
        std::vector<AssembledModule> mods{unitWithCommon(1, "reader_fn", 40, 32, /*entry=*/false),
                                          unitDefiningShared(2, SymbolBinding::Weak)};
        mods[0].externImports[0].mangledName = arm.shared;
        mods[1].symbols[0].name              = arm.shared;
        mods[1].symbols[0].yieldsToACommon   = arm.spellingYields;
        DiagnosticReporter rep;
        auto const obj = linker::link(std::span<AssembledModule const>{mods}, *L.target, *L.format, rep);
        ASSERT_TRUE(obj.ok()) << diagnosticsOf(rep);
        DiagnosticReporter readRep;
        std::string_view const fmt{arm.format};
        auto const back = fmt.starts_with("macho") ? macho::readRelocatableObject(obj.bytes, *L.target, *L.format, readRep)
                        : fmt.starts_with("pe")    ? pe::readRelocatableObject(obj.bytes, *L.target, *L.format, readRep)
                                                   : elf::readRelocatableObject(obj.bytes, *L.target, *L.format, readRep);
        ASSERT_TRUE(back.has_value()) << diagnosticsOf(readRep);
        EXPECT_EQ(rowNamed(*back, arm.shared), nullptr) << "one object cannot carry a common and a definition of a name";
        ModuleSymbol const* sym = nullptr;
        for (auto const& s : back->symbols) {
            if (s.name == arm.shared) sym = &s;
        }
        ASSERT_NE(sym, nullptr) << "the artifact must DEFINE the name";
        AssembledData const* item = nullptr;
        for (auto const& d : back->dataItems) {
            if (d.symbol == sym->symbol) item = &d;
        }
        ASSERT_NE(item, nullptr);
        if (arm.weakWins) {
            EXPECT_EQ(sym->binding, SymbolBinding::Weak) << "the weak definition, still weak";
            ASSERT_EQ(item->bytes.size(), 4u);
            EXPECT_EQ(item->bytes[0], 7u) << "the weak definition's own bytes";
        } else {
            EXPECT_EQ(sym->binding, SymbolBinding::Global) << "CONTROL: the common, allocated, is strong";
            EXPECT_EQ(item->reservedSize, 40u) << "CONTROL: the common's 40 zero-filled bytes";
        }
    }
}

// A link whose document does not say which definitions a common yields to refuses the question the moment it arises
// — a common beside a WEAK definition of its name — naming the key, rather than give it either family's answer.
// CONTROL: the same document links a common beside a STRONG definition, which every linker answers alike.
TEST(CommonSymbols, ALinkWhoseDocumentLeavesTheQuestionOpenIsRefusedByName) {
    auto const silent = elfExecDocumentWithoutCommonYieldsTo();
    ASSERT_TRUE(silent);
    auto const target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    std::vector<AssembledModule> weak{unitWithCommon(1, "large_reader", 40, 32, /*entry=*/true),
                                      unitDefiningShared(2, SymbolBinding::Weak)};
    DiagnosticReporter rep;
    auto const refused = linker::link(std::span<AssembledModule const>{weak}, **target, *silent, rep);
    EXPECT_FALSE(refused.ok()) << "a link whose answer its document does not state must not be written";
    EXPECT_TRUE(refusalNamesTheKey(rep)) << diagnosticsOf(rep);
    std::vector<AssembledModule> strong{unitWithCommon(1, "large_reader", 40, 32, /*entry=*/true),
                                        unitDefiningShared(2, SymbolBinding::Global)};
    DiagnosticReporter controlRep;
    auto const linked = linker::link(std::span<AssembledModule const>{strong}, **target, *silent, controlRep);
    EXPECT_TRUE(linked.ok()) << "CONTROL: a strong definition raises no question:" << diagnosticsOf(controlRep);
    EXPECT_FALSE(refusalNamesTheKey(controlRep));
}

// The ONE answer to whether a common yields to a definition (`linker::commonYieldsToDefinition`), which the link's
// allocation and the archive search both ask, so the member the search fetches is the definition the link lets
// win: a STRONG definition, yes; a LOCAL symbol is no definition the common meets; a WEAK one, as the link's
// document says (no on ELF, yes on Mach-O and PE), and no on every document when its own spelling yields to a
// common; and no answer where the document does not say.
TEST(CommonSymbols, OneAnswerSaysWhetherACommonYieldsToADefinition) {
    auto const silent = elfExecDocumentWithoutCommonYieldsTo();
    ASSERT_TRUE(silent);
    struct Doc {
        char const*         name;   // null: the ELF exec document without the key
        std::optional<bool> weak;   // what a weak definition the document decides gets
    };
    auto const definition = [](SymbolBinding binding, bool spellingYields) {
        ModuleSymbol s{SymbolId{1}, "shared", binding, SymbolVisibility::Default};
        s.yieldsToACommon = spellingYields;
        return s;
    };
    for (auto const& doc : {Doc{"elf64-x86_64-linux-exec", false}, Doc{"macho64-x86_64-darwin-exec", true},
                            Doc{"pe64-x86_64-windows-exec", true}, Doc{nullptr, std::nullopt}}) {
        SCOPED_TRACE(doc.name != nullptr ? doc.name : "the ELF exec document without the key");
        std::shared_ptr<ObjectFormatSchema const> format = silent;
        if (doc.name != nullptr) {
            auto const shipped = ObjectFormatSchema::loadShipped(doc.name);
            ASSERT_TRUE(shipped.has_value());
            format = *shipped;
        }
        EXPECT_EQ(linker::commonYieldsToDefinition(*format, definition(SymbolBinding::Global, false)),
                  std::optional<bool>{true});
        EXPECT_EQ(linker::commonYieldsToDefinition(*format, definition(SymbolBinding::Local, false)),
                  std::optional<bool>{false});
        EXPECT_EQ(linker::commonYieldsToDefinition(*format, definition(SymbolBinding::Weak, true)),
                  std::optional<bool>{false})
            << "a weak definition whose own spelling yields to a common";
        EXPECT_EQ(linker::commonYieldsToDefinition(*format, definition(SymbolBinding::Weak, false)), doc.weak);
    }
}

// Every document a link resolves units for states which definitions a common yields to, as its linkers answer; an
// archive's members are each linked alone, so an archive document states nothing (and may not:
// `CommonSymbolsArchive.TheKeyIsRefusedOnAnImageAndOutsideItsValues`).
TEST(CommonSymbols, EachLinkDocumentStatesWhatItsLinkersDo) {
    struct Want {
        std::string                   format;
        std::optional<CommonYieldsTo> yields;
    };
    std::vector<Want> wants;
    for (char const* isa : {"x86_64", "aarch64"}) {
        for (char const* flavor : {"", "-exec", "-pie", "-dyn"}) {
            wants.push_back(Want{std::string{"elf64-"} + isa + "-linux" + flavor, CommonYieldsTo::StrongDefinition});
        }
    }
    for (char const* f : {"macho64-x86_64-darwin", "macho64-x86_64-darwin-exec", "macho64-x86_64-darwin-dylib",
                          "macho64-arm64-darwin", "macho64-arm64-darwin-exec", "macho64-arm64-darwin-dylib",
                          "pe64-x86_64-windows", "pe64-x86_64-windows-exec", "pe64-x86_64-windows-dll"}) {
        wants.push_back(Want{f, CommonYieldsTo::AnyDefinition});
    }
    for (char const* f : {"elf64-x86_64-linux-staticlib", "elf64-aarch64-linux-staticlib",
                          "macho64-x86_64-darwin-staticlib", "macho64-arm64-darwin-staticlib",
                          "pe64-x86_64-windows-staticlib"}) {
        wants.push_back(Want{f, std::nullopt});
    }
    for (auto const& w : wants) {
        auto const f = ObjectFormatSchema::loadShipped(w.format);
        ASSERT_TRUE(f.has_value()) << w.format;
        EXPECT_EQ((*f)->commonYieldsTo(), w.yields) << w.format;
    }
}

// A RELOCATABLE artifact hands a common on: two of one name fold into the larger at the wider alignment, still a
// common its final linker allocates, and one that no relocation names is still written (it is a definition).
TEST(CommonSymbols, ARelocatableArtifactHandsTheCommonOnFolded) {
    auto const L = load("x86_64", "elf64-x86_64-linux");
    ASSERT_TRUE(L.target && L.format);
    std::vector<AssembledModule> mods{unitWithCommon(1, "small_reader", 4, 4, /*entry=*/false),
                                      unitWithCommon(2, "large_reader", 40, 32, /*entry=*/false)};
    DiagnosticReporter rep;
    auto const obj = linker::link(std::span<AssembledModule const>{mods}, *L.target, *L.format, rep);
    ASSERT_TRUE(obj.ok()) << diagnosticsOf(rep);
    auto const shared = elfSymbolsNamed(obj.bytes, "shared");
    ASSERT_EQ(shared.size(), 1u);
    EXPECT_EQ(shared[0].shndx, kShnCommon) << "still a common: the final linker allocates it";
    EXPECT_EQ(shared[0].size, 40u);
    EXPECT_EQ(shared[0].value, 32u) << "st_value is a common's alignment";

    AssembledModule lone;
    lone.cuId = CompilationUnitId{1};
    lone.externImports.push_back(commonRow(5, "unreferenced", 12, 8));
    DiagnosticReporter loneRep;
    auto const loneObj = linker::link(std::span<AssembledModule const>{&lone, 1}, *L.target, *L.format, loneRep);
    ASSERT_TRUE(loneObj.ok()) << diagnosticsOf(loneRep);
    auto const kept = elfSymbolsNamed(loneObj.bytes, "unreferenced");
    ASSERT_EQ(kept.size(), 1u) << "the reference gate keeps a common no relocation names: it is a definition";
    EXPECT_EQ(kept[0].shndx, kShnCommon);
}

// ══ Every writer with its reader ══════════════════════════════════════════════

TEST(CommonSymbols, AnElfObjectsCommonReadsBackAsTheSameCommon) {
    auto const L = load("x86_64", "elf64-x86_64-linux");
    ASSERT_TRUE(L.target && L.format);
    AssembledModule m = unitWithCommon(1, "reader_fn", 40, 32, /*entry=*/false);
    m.externImports.push_back(commonRow(3, "hidden_common", 8, 8, SymbolVisibility::Hidden));
    DiagnosticReporter rep;
    auto const obj = elf::encode(m, *L.target, *L.format, rep);
    ASSERT_FALSE(obj.empty()) << diagnosticsOf(rep);
    auto const back = elf::readRelocatableObject(obj, *L.target, *L.format, rep);
    ASSERT_TRUE(back.has_value()) << diagnosticsOf(rep);
    auto const* shared = rowNamed(*back, "shared");
    ASSERT_NE(shared, nullptr);
    EXPECT_EQ(shared->commonSize, 40u);
    EXPECT_EQ(shared->commonAlignment, 32u);
    auto const* hidden = rowNamed(*back, "hidden_common");
    ASSERT_NE(hidden, nullptr);
    EXPECT_EQ(hidden->commonSize, 8u);
    EXPECT_EQ(hidden->commonVisibility, SymbolVisibility::Hidden);
}

TEST(CommonSymbols, AMachOObjectsCommonReadsBackAsTheSameCommon) {
    auto const L = load("x86_64", "macho64-x86_64-darwin");
    ASSERT_TRUE(L.target && L.format);
    AssembledModule m = unitWithCommon(1, "_reader_fn", 40, 32, /*entry=*/false);
    m.externImports[0].mangledName = "_shared";
    m.externImports.push_back(commonRow(3, "_hidden_common", 8, 8, SymbolVisibility::Hidden));
    DiagnosticReporter rep;
    auto const obj = macho::encode(m, *L.target, *L.format, rep);
    ASSERT_FALSE(obj.empty()) << diagnosticsOf(rep);
    auto const back = macho::readRelocatableObject(obj, *L.target, *L.format, rep);
    ASSERT_TRUE(back.has_value()) << diagnosticsOf(rep);
    auto const* shared = rowNamed(*back, "_shared");
    ASSERT_NE(shared, nullptr);
    EXPECT_EQ(shared->commonSize, 40u);
    EXPECT_EQ(shared->commonAlignment, 32u) << "n_desc carries the alignment's log2";
    auto const* hidden = rowNamed(*back, "_hidden_common");
    ASSERT_NE(hidden, nullptr);
    EXPECT_EQ(hidden->commonVisibility, SymbolVisibility::Hidden) << "a private-extern common";
}

// COFF states a common's alignment only in a directive, and only where link.exe would align the common less
// (`pe::naturalCoffCommonAlignment`); a hidden common is hidden the way any hidden definition is.
TEST(CommonSymbols, ACoffObjectsCommonReadsBackAsTheSameCommon) {
    auto const L = load("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(L.target && L.format);
    AssembledModule m = unitWithCommon(1, "reader_fn", 40, 32, /*entry=*/false);   // 32 is link.exe's own
    m.externImports.push_back(commonRow(3, "cwide", 4, 64));                         // wider than link.exe's 4
    m.externImports.push_back(commonRow(4, "chidden", 8, 8, SymbolVisibility::Hidden));
    DiagnosticReporter rep;
    auto const obj = pe::encode(m, *L.target, *L.format, rep);
    ASSERT_FALSE(obj.empty()) << diagnosticsOf(rep);
    // The directive section, read off the file: the last section, by the writer's ordinals.
    std::uint16_t const nSec = static_cast<std::uint16_t>(rdLE(obj, 2, 2));
    std::string directive;
    for (std::uint16_t i = 0; i < nSec; ++i) {
        std::size_t const h = 20u + i * 40u;
        std::string name;
        for (std::size_t k = 0; k < 8 && obj[h + k] != 0; ++k) name.push_back(static_cast<char>(obj[h + k]));
        if (name != ".drectve") continue;
        auto const size = rdLE(obj, h + 16, 4), raw = rdLE(obj, h + 20, 4);
        directive.assign(obj.begin() + static_cast<std::ptrdiff_t>(raw),
                         obj.begin() + static_cast<std::ptrdiff_t>(raw + size));
    }
    EXPECT_EQ(directive, " -exclude-symbols:chidden -aligncomm:cwide,6");
    auto const back = pe::readRelocatableObject(obj, *L.target, *L.format, rep);
    ASSERT_TRUE(back.has_value()) << diagnosticsOf(rep);
    auto const* shared = rowNamed(*back, "shared");
    ASSERT_NE(shared, nullptr);
    EXPECT_EQ(shared->commonSize, 40u);
    EXPECT_EQ(shared->commonAlignment, 32u);
    auto const* wide = rowNamed(*back, "cwide");
    ASSERT_NE(wide, nullptr);
    EXPECT_EQ(wide->commonAlignment, 64u) << "the directive carries what the size alone would not";
    auto const* hidden = rowNamed(*back, "chidden");
    ASSERT_NE(hidden, nullptr);
    EXPECT_EQ(hidden->commonVisibility, SymbolVisibility::Hidden);
}

// ══ Native: the programs the reference compilers write ═══════════════════════

namespace {

// Two TUs that BOTH tentatively define `shared_counter` and `shared_array` — 10 ints in one, 4 in the other — and a
// program that reads them through each. Exit 42: one counter and one array, seen alike from both TUs; otherwise
// 1000 + a bitset (1 a TU's bump did not reach the shared counter, 2 the counter's value is wrong, 4 a TU does not
// see the other's array element).
constexpr char const* kTu1 =
    "int shared_counter;\n"
    "int shared_array[10];\n"
    "int bump(void) { shared_array[9] = 9; return ++shared_counter; }\n";
constexpr char const* kTu3 =
    "int shared_counter;\n"
    "int shared_array[4];\n"
    "int peek(void) { shared_array[3] = 3; return shared_counter; }\n";
// cl's own main, the third TU of the cl arm (MSVC objects carry no startup dependency DSS lacks).
constexpr char const* kClMain =
    "extern int shared_counter;\n"
    "extern int shared_array[];\n"
    "int bump(void);\n"
    "int peek(void);\n"
    "int main(void) {\n"
    "    int bad = 0;\n"
    "    bump();\n"
    "    if (bump() != 2 || peek() != 2) bad |= 1;\n"
    "    if (shared_counter != 2) bad |= 2;\n"
    "    if (shared_array[9] != 9 || shared_array[3] != 3) bad |= 4;\n"
    "    return bad != 0 ? 1000 + bad : 42;\n"
    "}\n";

void writeText(fs::path const& p, std::string_view text) { std::ofstream(p, std::ios::binary) << text; }

[[nodiscard]] bool runCapturing(test_support::native_probe::MsvcTools const* tools, fs::path const& dir,
                                std::string const& cmd, std::string const& logName) {
    namespace np = test_support::native_probe;
    std::string const line = np::captureCmd("cd /d \"" + dir.string() + "\" && " + cmd, dir / logName);
    if (tools != nullptr) return tools->ready() && np::systemUnder(tools->env, line) == 0;
    return std::system(line.c_str()) == 0;
}

// DSS compiles and links `inputs` (sources and objects, in `dir`) into an exe under `<dir>/dss.out`; empty on a
// failed build.
[[nodiscard]] fs::path dssLinks(fs::path const& dir, std::vector<std::string> const& inputs, DiagnosticReporter& rep) {
    auto const out = dir / "dss.out";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    std::vector<std::string> paths;
    for (auto const& i : inputs) paths.push_back((dir / i).string());
    int const rc = p.compileFiles(paths, "c", std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, rep);
    if (rc != 0) return {};
    for (auto const& e : fs::directory_iterator(out)) {
        if (e.path().extension() == ".exe") return e.path();
    }
    return {};
}

void expect42(fs::path const& exe, char const* who) {
    auto const r = test_support::runBinary(exe);
    ASSERT_TRUE(r.spawned) << who << ": " << r.diagnostic;
    EXPECT_FALSE(r.timedOut) << who;
    EXPECT_EQ(r.exitCode, 42u) << who << " — 1000 + bits: 1 a TU's bump did not reach the shared counter, 2 the "
                                         "counter's value is wrong, 4 a TU does not see the other's array element";
}

}  // namespace

// cl writes `int shared_counter;` and both arrays as COFF commons (✔MEASURED, run 20261006-224902-a6201782). The
// control is link.exe's program of the same three objects; DSS must link them to the same answer.
TEST(CommonSymbolsNative, ClTentativeDefinitionsLinkUnderDssAsUnderLinkExe) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "common-cl"};
    auto const dir = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const tools = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(tools.ready()) << tools.describe();
    writeText(dir / "tu1.c", kTu1);
    writeText(dir / "tu3.c", kTu3);
    writeText(dir / "main.c", kClMain);
    ASSERT_TRUE(runCapturing(&tools, dir, "cl /nologo /c /O2 /MD tu1.c tu3.c main.c", "cl.txt"))
        << test_support::native_probe::tailOf(dir / "cl.txt", 30, "cl");
    ASSERT_TRUE(runCapturing(&tools, dir, "link /nologo /OUT:ref.exe main.obj tu1.obj tu3.obj", "link.txt"))
        << test_support::native_probe::tailOf(dir / "link.txt", 30, "link.exe");
    expect42(dir / "ref.exe", "link.exe");
    DiagnosticReporter rep;
    auto const exe = dssLinks(dir, {"main.obj", "tu1.obj", "tu3.obj"}, rep);
    ASSERT_FALSE(exe.empty()) << "DSS must link cl's tentative definitions:" << diagnosticsOf(rep);
    expect42(exe, "DSS");
}

// mingw-w64 gcc 13.2.0 -fcommon writes the same commons plus a `-aligncomm:` for each; the program's `main` is a DSS
// unit (gcc's own main calls mingw's `__main`, a startup DSS does not link).
TEST(CommonSymbolsNative, MingwCommonsLinkUnderDss) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "common-gnu"};
    auto const dir = scratch.path();
#if defined(_WIN32)
    if (std::system("where gcc >nul 2>&1") != 0) {
        GTEST_SKIP() << "no MinGW `gcc` on PATH -- the mingw arm is inert on this host";
    }
#else
    GTEST_SKIP() << "not a Windows host -- the mingw arm is inert here";
#endif
    writeText(dir / "tu1.c", kTu1);
    writeText(dir / "tu3.c", kTu3);
    writeText(dir / "main.c", kClMain);
    ASSERT_TRUE(runCapturing(nullptr, dir, "gcc -c -O2 -fcommon tu1.c tu3.c", "gcc.txt"))
        << test_support::native_probe::tailOf(dir / "gcc.txt", 30, "gcc");
    DiagnosticReporter rep;
    auto const exe = dssLinks(dir, {"main.c", "tu1.o", "tu3.o"}, rep);
    ASSERT_FALSE(exe.empty()) << "DSS must link gcc's -fcommon commons:" << diagnosticsOf(rep);
    expect42(exe, "DSS");
}

// ══ The archive search: a common's name beside a member that defines it ══════
//
// D-LK-ARCHIVE-SEARCH-FETCHED-A-DEFINITION-FOR-A-COMMON (P69 round 3). A common DEFINES its name; whether a static
// link's archive search ALSO fetches a member that defines the name — whose definition then wins — is where the
// reference linkers split (✔MEASURED 2026-10-07, `.orchestrators/p69/work/xa/r3probe/commonpull/`: GNU ld 2.42's ELF
// linker fetches it on x86_64 and aarch64; link.exe 14.51, lld-link 19.1.5 and GNU ld 2.42's PE linker keep the
// common; ld.lld 18 keeps it unless `--fortran-common`). The archive members' document states its linkers' answer
// (`archiveCommonResolution`). Until round 3 a common was a row the search followed, so every format fetched.

namespace {

// A static library of ONE DSS-compiled C unit, `source`, built for `staticlibSpec` under `<dir>/<stem>`; its path,
// or empty.
[[nodiscard]] fs::path dssArchiveOf(fs::path const& dir, std::string const& stem, std::string_view source,
                                    char const* staticlibSpec, DiagnosticReporter& rep) {
    auto const libDir = dir / stem;
    fs::create_directories(libDir);
    auto const src = libDir / (stem + ".c");
    writeText(src, source);
    Program p;
    p.setOutputDir(libDir);
    if (p.compileFiles(std::vector<std::string>{src.string()}, "c", std::vector<std::string>{staticlibSpec}, rep)
        != 0) {
        return {};
    }
    for (auto const& e : fs::directory_iterator(libDir)) {
        if (e.path().extension() == ".a" || e.path().extension() == ".lib") return e.path();
    }
    return {};
}

// A static link of ONE relocatable object (`obj`, written as `<dir>/<objName>`) against `archives`, in that order, to
// `execSpec`, under `<dir>/<objName>.out`; the image's path (the one file whose extension is `imageExt`), or empty
// on a refused link.
[[nodiscard]] fs::path dssLinkObject(fs::path const& dir, std::string const& objName,
                                     std::vector<std::uint8_t> const& obj, std::vector<fs::path> const& archives,
                                     char const* execSpec, std::string_view imageExt, DiagnosticReporter& rep) {
    auto const objPath = dir / objName;
    {
        std::ofstream out(objPath, std::ios::binary);
        out.write(reinterpret_cast<char const*>(obj.data()), static_cast<std::streamsize>(obj.size()));
    }
    auto const outDir = dir / (objName + ".out");
    fs::create_directories(outDir);
    Program p;
    p.setOutputDir(outDir);
    if (!archives.empty()) p.setResolveLibraries(archives);
    if (p.compileFiles(std::vector<std::string>{objPath.string()}, "c", std::vector<std::string>{execSpec}, rep)
        != 0) {
        return {};
    }
    for (auto const& e : fs::directory_iterator(outDir)) {
        if (e.is_regular_file() && e.path().extension() == imageExt) return e.path();
    }
    return {};
}

[[nodiscard]] std::vector<std::uint8_t> fileBytes(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// (Used by the Linux-only thread-local arm below, so a Windows or macOS build sees no caller.)
[[nodiscard, maybe_unused]] bool sawCode(DiagnosticReporter const& rep, DiagnosticCode code) {
    for (auto const& d : rep.all()) {
        if (d.code == code) return true;
    }
    return false;
}

// The section and the 4-byte value `main`'s read reaches in a linked ELF image.
struct ElfRead {
    std::string   section;
    std::uint64_t value = 0;
};
[[nodiscard]] std::optional<ElfRead> mainReadIn(std::vector<std::uint8_t> const& img) {
    auto const at = datumReadBy(img, "main");
    if (!at.has_value()) return std::nullopt;
    auto const sec = sectionHolding(img, *at);
    if (!sec.has_value()) return std::nullopt;
    if (sec->name == ".bss") return ElfRead{sec->name, 0};
    return ElfRead{sec->name, rdLE(img, sec->offset + (*at - sec->addr), 4)};
}

// ── A minimal Mach-O 64 image reader: its sections and its defined symbols ──
struct MachOSection {
    std::string   segment, name;
    std::uint64_t addr = 0, size = 0;
    std::uint32_t offset = 0, flags = 0;
};
struct MachOImage {
    std::vector<MachOSection>                          sections;
    std::vector<std::pair<std::string, std::uint64_t>> defined;   // N_SECT symbols: name, value
};
[[nodiscard]] std::string fixedName16(std::vector<std::uint8_t> const& b, std::uint64_t off) {
    std::string s;
    for (std::uint64_t k = 0; k < 16 && off + k < b.size() && b[off + k] != 0; ++k) s.push_back(static_cast<char>(b[off + k]));
    return s;
}
[[nodiscard]] MachOImage machoImage(std::vector<std::uint8_t> const& b) {
    MachOImage img;
    if (b.size() < 32 || rdLE(b, 0, 4) != 0xFEEDFACFu) return img;   // MH_MAGIC_64
    std::uint64_t const ncmds = rdLE(b, 16, 4);
    std::uint64_t       at    = 32;
    for (std::uint64_t i = 0; i < ncmds && at + 8 <= b.size(); ++i) {
        std::uint64_t const cmd = rdLE(b, at, 4), size = rdLE(b, at + 4, 4);
        if (cmd == 0x19u) {   // LC_SEGMENT_64: 72 bytes, then `nsects` section_64 records of 80
            std::uint64_t const nsects = rdLE(b, at + 64, 4);
            for (std::uint64_t s = 0; s < nsects; ++s) {
                std::uint64_t const h = at + 72 + s * 80;
                img.sections.push_back(MachOSection{fixedName16(b, h + 16), fixedName16(b, h), rdLE(b, h + 32, 8),
                                                    rdLE(b, h + 40, 8), static_cast<std::uint32_t>(rdLE(b, h + 48, 4)),
                                                    static_cast<std::uint32_t>(rdLE(b, h + 64, 4))});
            }
        } else if (cmd == 0x2u) {   // LC_SYMTAB: nlist_64 records of 16
            std::uint64_t const symoff = rdLE(b, at + 8, 4), nsyms = rdLE(b, at + 12, 4), stroff = rdLE(b, at + 16, 4);
            for (std::uint64_t k = 0; k < nsyms; ++k) {
                std::uint64_t const n = symoff + k * 16;
                if ((b[n + 4] & 0x0Eu) == 0x0Eu) {   // N_SECT
                    img.defined.emplace_back(cstrAt(b, stroff + rdLE(b, n, 4)), rdLE(b, n + 8, 8));
                }
            }
        }
        if (size == 0) break;
        at += size;
    }
    return img;
}
[[nodiscard]] bool machoDefines(MachOImage const& img, std::string_view name) {
    for (auto const& d : img.defined) {
        if (d.first == name) return true;
    }
    return false;
}
// The section and the 4-byte value `fnName`'s `mov eax, [rip+disp32]` reaches in a linked x86_64 Mach-O image; a
// zero-fill section reads 0.
struct MachORead {
    std::string   section;
    std::uint64_t value = 0;
};
[[nodiscard]] std::optional<MachORead> machoReadBy(std::vector<std::uint8_t> const& b, std::string_view fnName) {
    auto const                   img = machoImage(b);
    std::optional<std::uint64_t> fn;
    for (auto const& d : img.defined) {
        if (d.first == fnName) fn = d.second;
    }
    if (!fn.has_value()) return std::nullopt;
    auto const holding = [&](std::uint64_t va) -> MachOSection const* {
        for (auto const& s : img.sections) {
            if (s.size != 0 && va >= s.addr && va < s.addr + s.size) return &s;
        }
        return nullptr;
    };
    MachOSection const* const text = holding(*fn);
    if (text == nullptr || text->offset == 0) return std::nullopt;
    std::uint64_t const at     = text->offset + (*fn - text->addr);
    auto const          disp   = static_cast<std::int32_t>(static_cast<std::uint32_t>(rdLE(b, at + 2, 4)));
    std::uint64_t const target = *fn + 6 + static_cast<std::uint64_t>(static_cast<std::int64_t>(disp));
    MachOSection const* const data = holding(target);
    if (data == nullptr) return std::nullopt;
    std::uint32_t const type = data->flags & 0xFFu;
    if (type == 0x1u || type == 0xCu) return MachORead{data->name, 0};   // S_ZEROFILL, S_GB_ZEROFILL
    return MachORead{data->name, rdLE(b, data->offset + (target - data->addr), 4)};
}

}  // namespace

// Every archive-member document states what its linkers do — the Mach-O ones since P69 round 4, when Apple's ld was
// measured (D-LK-MACHO-ARCHIVE-COMMON-RESOLUTION-UNMEASURED).
TEST(CommonSymbolsArchive, EachMembersDocumentStatesWhatItsLinkersDo) {
    struct Want {
        char const*                            format;
        std::optional<ArchiveCommonResolution> resolution;
    };
    for (auto const& w : {Want{"elf64-x86_64-linux-staticlib", ArchiveCommonResolution::FetchDefinition},
                          Want{"elf64-aarch64-linux-staticlib", ArchiveCommonResolution::FetchDefinition},
                          Want{"pe64-x86_64-windows-staticlib", ArchiveCommonResolution::KeepCommon},
                          Want{"macho64-x86_64-darwin-staticlib", ArchiveCommonResolution::FetchDefinition},
                          Want{"macho64-arm64-darwin-staticlib", ArchiveCommonResolution::FetchDefinition}}) {
        auto const f = ObjectFormatSchema::loadShipped(w.format);
        ASSERT_TRUE(f.has_value()) << w.format;
        EXPECT_EQ((*f)->archiveCommonResolution(), w.resolution) << w.format;
    }
}

// The key is read from an archive's MEMBER document, so an image document declaring it is refused, and so is a value
// outside the closed set. Its twin `commonYieldsTo` is read from the document of the link that resolves a common
// against other units, which an archive's member never is: refused on a member document, and outside its values.
TEST(CommonSymbolsArchive, TheKeyIsRefusedOnAnImageAndOutsideItsValues) {
    auto const withKey = [](std::string const& format, char const* key, nlohmann::json value) {
        auto const path = dss::test::configRoot() / "object-formats" / (format + ".format.json");
        std::ifstream in{path};
        auto doc = nlohmann::json::parse(in);
        doc[key] = std::move(value);
        return ObjectFormatSchema::loadFromText(doc.dump(), format + " + " + key);
    };
    auto const pathNamed = [](auto const& loaded, std::string_view path) {
        bool named = false;
        for (auto const& d : loaded.error()) named = named || d.path == path;
        return named;
    };
    auto const onImage = withKey("elf64-x86_64-linux-exec", "archiveCommonResolution", "fetchDefinition");
    ASSERT_FALSE(onImage.has_value()) << "an image document declaring it must be refused";
    EXPECT_TRUE(pathNamed(onImage, "/archiveCommonResolution"));
    auto const badValue = withKey("pe64-x86_64-windows-staticlib", "archiveCommonResolution", "fetch");
    ASSERT_FALSE(badValue.has_value()) << "a value outside the closed set must be refused";
    // CONTROL: the member document with a value from the set loads.
    EXPECT_TRUE(withKey("pe64-x86_64-windows-staticlib", "archiveCommonResolution", "fetchDefinition").has_value());

    auto const onArchive = withKey("macho64-x86_64-darwin-staticlib", "commonYieldsTo", "anyDefinition");
    ASSERT_FALSE(onArchive.has_value()) << "an archive document declaring `commonYieldsTo` must be refused";
    EXPECT_TRUE(pathNamed(onArchive, "/commonYieldsTo"));
    auto const badYields = withKey("elf64-x86_64-linux-exec", "commonYieldsTo", "weakDefinition");
    ASSERT_FALSE(badYields.has_value()) << "a value outside the closed set must be refused";
    EXPECT_TRUE(pathNamed(badYields, "/commonYieldsTo"));
    // CONTROL: an image document with either value from the set loads.
    EXPECT_TRUE(withKey("elf64-x86_64-linux-exec", "commonYieldsTo", "anyDefinition").has_value());
}

// GNU ld's ELF linker FETCHES the member, and its definition wins: `main` (a DSS-written object holding `shared` as a
// 4-byte common) reads the member's initialized `shared` (7) in `.data`. CONTROL: linked alone, `main` reads the
// common's own `.bss` storage — so it is the archive search that changed what `main` reads.
TEST(CommonSymbolsArchive, AnElfLinkFetchesTheMemberThatDefinesACommonsName) {
    auto const L = load("x86_64", "elf64-x86_64-linux");
    ASSERT_TRUE(L.target && L.format);
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "common-archive-elf"};
    auto const dir = scratch.path();
    DiagnosticReporter rep;
    auto const obj = elf::encode(unitWithCommon(1, "main", 4, 4, /*entry=*/false), *L.target, *L.format, rep);
    ASSERT_FALSE(obj.empty()) << diagnosticsOf(rep);
    auto const lib = dssArchiveOf(dir, "shareddef", "int shared = 7;\n", "x86_64:elf64-x86_64-linux-staticlib", rep);
    ASSERT_FALSE(lib.empty()) << diagnosticsOf(rep);

    auto const fetched = dssLinkObject(dir, "common.o", obj, {lib}, "x86_64:elf64-x86_64-linux-exec", "", rep);
    ASSERT_FALSE(fetched.empty()) << diagnosticsOf(rep);
    auto const read = mainReadIn(fileBytes(fetched));
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->section, ".data") << "the member's initialized definition, fetched for the common, wins";
    EXPECT_EQ(read->value, 7u);

    DiagnosticReporter aloneRep;
    auto const alone = dssLinkObject(dir, "alone.o", obj, {}, "x86_64:elf64-x86_64-linux-exec", "", aloneRep);
    ASSERT_FALSE(alone.empty()) << diagnosticsOf(aloneRep);
    auto const own = mainReadIn(fileBytes(alone));
    ASSERT_TRUE(own.has_value());
    EXPECT_EQ(own->section, ".bss") << "CONTROL: with no archive the common is the storage";
}

// link.exe, lld-link and GNU ld's PE linker KEEP the common and fetch nothing for it: the program returns the
// common's 0. CONTROL: an object naming `shared` as a plain REFERENCE fetches the same member (7), so the archive and
// its member are not what kept the common. The pe64 images run on Windows; elsewhere both links must still succeed.
TEST(CommonSymbolsArchive, APeLinkKeepsTheCommonAndFetchesNothing) {
    auto const L = load("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(L.target && L.format);
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "common-archive-pe"};
    auto const dir = scratch.path();
    DiagnosticReporter rep;
    auto const commonObj = pe::encode(unitWithCommon(1, "main", 4, 4, /*entry=*/false), *L.target, *L.format, rep);
    ASSERT_FALSE(commonObj.empty()) << diagnosticsOf(rep);
    auto const referenceObj =
        pe::encode(unitWithCommon(1, "main", /*size=*/0, 0, /*entry=*/false), *L.target, *L.format, rep);
    ASSERT_FALSE(referenceObj.empty()) << diagnosticsOf(rep);
    auto const lib = dssArchiveOf(dir, "shareddef", "int shared = 7;\n", "x86_64:pe64-x86_64-windows-staticlib", rep);
    ASSERT_FALSE(lib.empty()) << diagnosticsOf(rep);

    auto const kept = dssLinkObject(dir, "common.obj", commonObj, {lib},"x86_64:pe64-x86_64-windows-exec", ".exe", rep);
    ASSERT_FALSE(kept.empty()) << diagnosticsOf(rep);
    auto const fetched =
        dssLinkObject(dir, "reference.obj", referenceObj, {lib},"x86_64:pe64-x86_64-windows-exec", ".exe", rep);
    ASSERT_FALSE(fetched.empty()) << diagnosticsOf(rep);
#if defined(_WIN32)
    auto const k = test_support::runBinary(kept);
    ASSERT_TRUE(k.spawned) << k.diagnostic;
    EXPECT_EQ(k.exitCode, 0u) << "the common is kept, so `shared` is its zero — the member is not fetched for it";
    auto const f = test_support::runBinary(fetched);
    ASSERT_TRUE(f.spawned) << f.diagnostic;
    EXPECT_EQ(f.exitCode, 7u) << "CONTROL: a plain reference fetches the member and reads its 7";
#else
    GTEST_SKIP() << "both pe64 links succeeded; their images run on Windows";
#endif
}

// The same question is asked of a PULLED member's common, not only of the linked objects': `_main` (the linked
// object) calls `_f`, which archive A's one member defines beside a COMMON `_shared` (A indexes `_f` alone), and
// archive B's member defines `_shared` (7). Pulling `_f`'s member raises the question, and the Mach-O search fetches
// B's member (`fetchDefinition`, Apple's ld's answer since P69 round 4): `_f` reads its 7, in `__data`. CONTROL:
// without B nothing defines `_shared` but the member's common, and `_f` reads that common's zero-filled storage.
TEST(CommonSymbolsArchive, APulledMembersCommonFetchesItsDefinitionTooInAMachOLink) {
    auto const L = load("x86_64", "macho64-x86_64-darwin");
    ASSERT_TRUE(L.target && L.format);
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "common-archive-member"};
    auto const dir = scratch.path();
    DiagnosticReporter rep;

    // `_main`: `call _f ; ret` (rel32, kind 1 on x86_64) — the call is what pulls the member.
    AssembledModule caller;
    caller.cuId              = CompilationUnitId{1};
    caller.expectedFuncCount = 1;
    AssembledFunction mainFn;
    mainFn.symbol = SymbolId{1};
    mainFn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation call;
    call.offset = 1;
    call.target = SymbolId{2};
    call.kind   = RelocationKind{1};
    call.addend = 0;
    mainFn.relocations.push_back(call);
    caller.functions.push_back(mainFn);
    caller.symbols.push_back(ModuleSymbol{SymbolId{1}, "_main", SymbolBinding::Global, SymbolVisibility::Default});
    ExternImport f;
    f.symbol      = SymbolId{2};
    f.mangledName = "_f";
    f.isData      = false;
    caller.externImports.push_back(f);
    auto const callerObj = macho::encode(caller, *L.target, *L.format, rep);
    ASSERT_FALSE(callerObj.empty()) << diagnosticsOf(rep);

    // Archive A: `_f` reading `_shared`, which its object holds as a 4-byte common.
    AssembledModule member = unitWithCommon(1, "_f", 4, 4, /*entry=*/false);
    member.externImports[0].mangledName = "_shared";
    auto const memberObj = macho::encode(member, *L.target, *L.format, rep);
    ASSERT_FALSE(memberObj.empty()) << diagnosticsOf(rep);
    std::vector<link::format::ArMemberInput> const membersA{
        link::format::ArMemberInput{"f.o", memberObj, {"_f"}}};
    auto const archiveA = link::format::writeArArchive(membersA, rep);
    ASSERT_FALSE(archiveA.empty()) << diagnosticsOf(rep);
    auto const pathA = dir / "libf.a";
    {
        std::ofstream out(pathA, std::ios::binary);
        out.write(reinterpret_cast<char const*>(archiveA.data()), static_cast<std::streamsize>(archiveA.size()));
    }
    // Archive B: a DSS unit defining `shared` (`_shared` on Mach-O).
    auto const pathB =
        dssArchiveOf(dir, "shareddef", "int shared = 7;\n", "x86_64:macho64-x86_64-darwin-staticlib", rep);
    ASSERT_FALSE(pathB.empty()) << diagnosticsOf(rep);

    DiagnosticReporter fetchedRep;
    auto const fetched =
        dssLinkObject(dir, "caller.o", callerObj, {pathA, pathB}, "x86_64:macho64-x86_64-darwin-exec", "", fetchedRep);
    ASSERT_FALSE(fetched.empty()) << diagnosticsOf(fetchedRep);
    auto const read = machoReadBy(fileBytes(fetched), "_f");
    ASSERT_TRUE(read.has_value()) << "`_f`'s read must reach a section of the image";
    EXPECT_EQ(read->section, "__data") << "B's definition, fetched for the pulled member's common, wins";
    EXPECT_EQ(read->value, 7u);

    DiagnosticReporter controlRep;
    auto const kept =
        dssLinkObject(dir, "control.o", callerObj, {pathA}, "x86_64:macho64-x86_64-darwin-exec", "", controlRep);
    ASSERT_FALSE(kept.empty()) << "CONTROL: only the member's common defines `_shared`:" << diagnosticsOf(controlRep);
    auto const own = machoReadBy(fileBytes(kept), "_f");
    ASSERT_TRUE(own.has_value());
    EXPECT_EQ(own->value, 0u) << "CONTROL: the member's own common is the storage";
    EXPECT_NE(own->section, "__data");
}

// ══ Which member GNU ld fetches for a common's name (P69 round 4, review-xa3 MINOR 5) ══
//
// GNU ld's ELF linker does not fetch the first member the armap lists for a common's name: it fetches the first
// whose own symbol table DEFINES the name as a GLOBAL DATUM — not another common, not a weak definition, not a
// function — and goes on searching past every member that does not (bfd elflink.c,
// `elf_link_is_defined_archive_symbol`; ✔MEASURED 2026-10-07, gcc 13.3.0 + GNU ld 2.42,
// `.orchestrators/p69/work/xa/r4probe/m5`: common-, weak- and function-then-strong archives all fetch the strong
// member). Four members list `shared` in the armap, in that order, each beside a marker function of its own; only
// the last defines it as a global datum (7). It alone is fetched — its marker alone reaches the image — and `main`
// reads its 7. CONTROL: without it nothing qualifies, nothing is fetched, and `main` reads its own common's `.bss`.

namespace {

enum class SharedShape { Common, Weak, Function, Strong };

// A member defining `marker` (`xor eax, eax ; ret`) and `sharedName` as `shape` says.
[[nodiscard]] AssembledModule memberDefiningShared(std::string marker, SharedShape shape,
                                                   std::string const& sharedName = "shared") {
    AssembledModule m;
    m.cuId = CompilationUnitId{1};
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x31, 0xC0, 0xC3};
    m.functions.push_back(fn);
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, std::move(marker), SymbolBinding::Global, SymbolVisibility::Default});
    switch (shape) {
        case SharedShape::Common:
            m.externImports.push_back(commonRow(2, sharedName, 4, 4));
            break;
        case SharedShape::Weak:
        case SharedShape::Strong: {
            AssembledData d;
            d.symbol    = SymbolId{2};
            d.section   = DataSectionKind::Data;
            d.bytes     = {static_cast<std::uint8_t>(shape == SharedShape::Weak ? 3 : 7), 0, 0, 0};
            d.alignment = Alignment::of<4>();
            m.dataItems.push_back(d);
            m.symbols.push_back(ModuleSymbol{SymbolId{2}, sharedName,
                                             shape == SharedShape::Weak ? SymbolBinding::Weak : SymbolBinding::Global,
                                             SymbolVisibility::Default});
            break;
        }
        case SharedShape::Function: {
            AssembledFunction f;
            f.symbol = SymbolId{2};
            f.bytes  = {0xB8, 0x09, 0x00, 0x00, 0x00, 0xC3};
            m.functions.push_back(f);
            m.symbols.push_back(ModuleSymbol{SymbolId{2}, sharedName, SymbolBinding::Global, SymbolVisibility::Default});
            break;
        }
    }
    m.expectedFuncCount = m.functions.size();
    return m;
}

}  // namespace

TEST(CommonSymbolsArchive, AnElfLinkFetchesTheFirstMemberThatDefinesTheNameAsAGlobalDatum) {
    auto const L = load("x86_64", "elf64-x86_64-linux");
    ASSERT_TRUE(L.target && L.format);
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "common-archive-predicate"};
    auto const dir = scratch.path();
    DiagnosticReporter rep;
    auto const obj = elf::encode(unitWithCommon(1, "main", 4, 4, /*entry=*/false), *L.target, *L.format, rep);
    ASSERT_FALSE(obj.empty()) << diagnosticsOf(rep);
    struct Member {
        char const* file;
        char const* marker;
        SharedShape shape;
    };
    std::vector<Member> const all{{"common.o", "m_common", SharedShape::Common},
                                  {"weak.o", "m_weak", SharedShape::Weak},
                                  {"function.o", "m_function", SharedShape::Function},
                                  {"strong.o", "m_strong", SharedShape::Strong}};
    auto const archiveOf = [&](std::size_t count, std::string const& name) -> fs::path {
        std::vector<link::format::ArMemberInput> members;
        for (std::size_t k = 0; k < count; ++k) {
            auto bytes = elf::encode(memberDefiningShared(all[k].marker, all[k].shape), *L.target, *L.format, rep);
            if (bytes.empty()) return {};
            members.push_back(link::format::ArMemberInput{all[k].file, std::move(bytes), {"shared", all[k].marker}});
        }
        auto const archive = link::format::writeArArchive(members, rep);
        if (archive.empty()) return {};
        auto const path = dir / name;
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<char const*>(archive.data()), static_cast<std::streamsize>(archive.size()));
        return path;
    };
    auto const four = archiveOf(4, "libfour.a");
    ASSERT_FALSE(four.empty()) << diagnosticsOf(rep);
    auto const three = archiveOf(3, "libthree.a");
    ASSERT_FALSE(three.empty()) << diagnosticsOf(rep);

    auto const fetched = dssLinkObject(dir, "four.o", obj, {four}, "x86_64:elf64-x86_64-linux-exec", "", rep);
    ASSERT_FALSE(fetched.empty()) << diagnosticsOf(rep);
    auto const img = fileBytes(fetched);
    auto const read = mainReadIn(img);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->section, ".data") << "the strong member's definition, fetched for the common, wins";
    EXPECT_EQ(read->value, 7u);
    EXPECT_EQ(elfSymbolsNamed(img, "m_strong").size(), 1u) << "the member that defines a global datum is fetched";
    for (char const* never : {"m_common", "m_weak", "m_function"}) {
        EXPECT_TRUE(elfSymbolsNamed(img, never).empty())
            << never << ": a member whose `shared` is a common, weak or a function is not fetched for it";
    }

    DiagnosticReporter controlRep;
    auto const kept = dssLinkObject(dir, "three.o", obj, {three}, "x86_64:elf64-x86_64-linux-exec", "", controlRep);
    ASSERT_FALSE(kept.empty()) << diagnosticsOf(controlRep);
    auto const keptImg = fileBytes(kept);
    auto const own = mainReadIn(keptImg);
    ASSERT_TRUE(own.has_value());
    EXPECT_EQ(own->section, ".bss") << "CONTROL: no member qualifies, so the common is the storage";
    for (char const* never : {"m_common", "m_weak", "m_function"}) {
        EXPECT_TRUE(elfSymbolsNamed(keptImg, never).empty()) << never;
    }
}

// Apple's ld fetches, for a common's name, the first member whose own symbol table defines it as a DATUM the common
// yields to — and on Mach-O a common yields to a WEAK definition too (`commonYieldsTo`: `anyDefinition`), so a weak
// member qualifies where GNU ld's predicate passes it over (✔MEASURED 2026-10-07, Apple clang 21's ld-1267, arm64 and
// x86_64, `.orchestrators/p69/work/xa/r4probe/m14mac`: weak-then-strong fetches the weak member, whose value the
// program reads; function-then-strong fetches the strong one; a function or a common alone, nothing). Four members
// list `_shared`, in this order — as a common, as a function, WEAK (3) and strong (7) — each beside a marker of its
// own: the weak member alone is fetched, and `_main` reads its 3 in `__data`. CONTROLS: without the weak member the
// strong one is fetched (7); with the common and the function alone nothing is, and `_main` reads its own common.
// Until P69 round 4 Apple's ld was unmeasured and this link was refused by name
// (D-LK-MACHO-ARCHIVE-COMMON-RESOLUTION-UNMEASURED).
TEST(CommonSymbolsArchive, AMachOLinkFetchesTheFirstMemberWhoseDatumTheCommonYieldsTo) {
    auto const L = load("x86_64", "macho64-x86_64-darwin");
    ASSERT_TRUE(L.target && L.format);
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "common-archive-macho"};
    auto const dir = scratch.path();
    AssembledModule m = unitWithCommon(1, "_main", 4, 4, /*entry=*/false);
    m.externImports[0].mangledName = "_shared";
    DiagnosticReporter rep;
    auto const obj = macho::encode(m, *L.target, *L.format, rep);
    ASSERT_FALSE(obj.empty()) << diagnosticsOf(rep);
    struct Member {
        char const* file;
        char const* marker;
        SharedShape shape;
    };
    auto const archiveOf = [&](std::vector<Member> const& members, std::string const& name) -> fs::path {
        std::vector<link::format::ArMemberInput> inputs;
        for (auto const& mm : members) {
            auto bytes = macho::encode(memberDefiningShared(mm.marker, mm.shape, "_shared"), *L.target, *L.format, rep);
            if (bytes.empty()) return {};
            inputs.push_back(link::format::ArMemberInput{mm.file, std::move(bytes), {"_shared", mm.marker}});
        }
        auto const archive = link::format::writeArArchive(inputs, rep);
        if (archive.empty()) return {};
        auto const path = dir / name;
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<char const*>(archive.data()), static_cast<std::streamsize>(archive.size()));
        return path;
    };
    Member const common{"common.o", "_m_common", SharedShape::Common};
    Member const function{"function.o", "_m_function", SharedShape::Function};
    Member const weak{"weak.o", "_m_weak", SharedShape::Weak};
    Member const strong{"strong.o", "_m_strong", SharedShape::Strong};
    struct Cell {
        char const*         stem;      // the archive is `lib<stem>.a`, the linked object `<stem>.o`
        std::vector<Member> members;
        char const*         fetched;   // the marker of the member fetched, or null
        std::uint64_t       value;     // what `_main` reads
    };
    std::vector<Cell> const cells{Cell{"four", {common, function, weak, strong}, "_m_weak", 3},
                                  Cell{"noweak", {common, function, strong}, "_m_strong", 7},
                                  Cell{"none", {common, function}, nullptr, 0}};
    for (auto const& cell : cells) {
        SCOPED_TRACE(cell.stem);
        auto const lib = archiveOf(cell.members, std::string{"lib"} + cell.stem + ".a");
        ASSERT_FALSE(lib.empty()) << diagnosticsOf(rep);
        DiagnosticReporter linkRep;
        auto const image = dssLinkObject(dir, std::string{cell.stem} + ".o", obj, {lib},
                                         "x86_64:macho64-x86_64-darwin-exec", "", linkRep);
        ASSERT_FALSE(image.empty()) << diagnosticsOf(linkRep);
        auto const bytes = fileBytes(image);
        auto const img   = machoImage(bytes);
        for (auto const& mm : cell.members) {
            bool const want = cell.fetched != nullptr && std::string_view{mm.marker} == cell.fetched;
            EXPECT_EQ(machoDefines(img, mm.marker), want)
                << mm.marker << (want ? ": the member fetched for the common" : ": a member not fetched");
        }
        auto const read = machoReadBy(bytes, "_main");
        ASSERT_TRUE(read.has_value()) << "`_main`'s read must reach a section of the image";
        EXPECT_EQ(read->value, cell.value);
        if (cell.fetched != nullptr) {
            EXPECT_EQ(read->section, "__data") << "the fetched member's definition wins over the common";
        } else {
            EXPECT_NE(read->section, "__data") << "CONTROL: no member qualifies, so the common is the storage";
        }
    }
}

// The archive search asks a member the link's own question (`linker::commonYieldsToDefinition`), so a link document
// that does not say which definitions a common yields to refuses the search too, by name — naming the key and the
// member — the moment a member defines the common's name WEAK, rather than fetch it or pass it over. CONTROL: the
// same document fetches a STRONG member, which raises no question.
TEST(CommonSymbolsArchive, AnArchiveSearchWhoseLinkDocumentLeavesTheQuestionOpenIsRefusedByName) {
    auto const L = load("x86_64", "elf64-x86_64-linux");
    ASSERT_TRUE(L.target && L.format);
    auto const silent = elfExecDocumentWithoutCommonYieldsTo();
    ASSERT_TRUE(silent);
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "common-archive-silent"};
    auto const dir = scratch.path();
    DiagnosticReporter rep;
    auto const archiveOf = [&](char const* marker, SharedShape shape) -> fs::path {
        auto bytes = elf::encode(memberDefiningShared(marker, shape), *L.target, *L.format, rep);
        if (bytes.empty()) return {};
        std::vector<link::format::ArMemberInput> members;
        members.push_back(link::format::ArMemberInput{std::string{marker} + ".o", std::move(bytes), {"shared", marker}});
        auto const archive = link::format::writeArArchive(members, rep);
        if (archive.empty()) return {};
        auto const path = dir / (std::string{"lib"} + marker + ".a");
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<char const*>(archive.data()), static_cast<std::streamsize>(archive.size()));
        return path;
    };
    std::vector<fs::path> const weak{archiveOf("m_weak", SharedShape::Weak)};
    ASSERT_FALSE(weak.front().empty()) << diagnosticsOf(rep);
    std::vector<fs::path> const strong{archiveOf("m_strong", SharedShape::Strong)};
    ASSERT_FALSE(strong.front().empty()) << diagnosticsOf(rep);
    std::vector<AssembledModule> const client{unitWithCommon(1, "main", 4, 4, /*entry=*/false)};

    DiagnosticReporter refusedRep;
    auto const refused = pullStaticArchiveMembers(client, weak, {}, *L.target, *silent, refusedRep);
    EXPECT_FALSE(refused.has_value()) << "a search whose answer the link's document does not state must not guess one";
    EXPECT_TRUE(refusalNamesTheKey(refusedRep, "m_weak.o")) << diagnosticsOf(refusedRep);

    DiagnosticReporter controlRep;
    auto const pulled = pullStaticArchiveMembers(client, strong, {}, *L.target, *silent, controlRep);
    ASSERT_TRUE(pulled.has_value()) << "CONTROL: a strong member raises no question:" << diagnosticsOf(controlRep);
    EXPECT_EQ(pulled->size(), 1u) << "CONTROL: the strong member is fetched for the common";
    EXPECT_FALSE(refusalNamesTheKey(controlRep));
}

// ══ A thread-local common (P69 round 4, review-xa3 MINOR 6) ══════════════════
//
// gcc and clang never write one: under `-fcommon` a `__thread` or `_Thread_local` tentative definition still goes to
// `.tbss` (✔MEASURED 2026-10-07, gcc 13.3.0 and clang 18.1.3); only gas's `.tls_common` writes an STT_TLS COMMON,
// which GNU ld allocates in the image's `.tbss`. DSS allocates a common in `.bss`, which a thread's copy never is, so
// an ELF object whose SHN_COMMON symbol is STT_TLS is refused by name, `K_CommonSymbolUnallocatable`. CONTROL: the
// same object, the type left STT_OBJECT, reads back as a common.
TEST(CommonSymbols, AnElfThreadLocalCommonIsRefusedByName) {
    auto const L = load("x86_64", "elf64-x86_64-linux");
    ASSERT_TRUE(L.target && L.format);
    DiagnosticReporter rep;
    auto const obj = elf::encode(unitWithCommon(1, "f", 4, 4, /*entry=*/false), *L.target, *L.format, rep);
    ASSERT_FALSE(obj.empty()) << diagnosticsOf(rep);
    // The `.symtab` row of `shared`, the common.
    std::optional<std::size_t> row;
    {
        auto const secs = elfSections(obj);
        ElfSection const* sy = nullptr;
        ElfSection const* st = nullptr;
        for (auto const& s : secs) {
            if (s.name == ".symtab") sy = &s;
            if (s.name == ".strtab") st = &s;
        }
        ASSERT_TRUE(sy != nullptr && st != nullptr);
        for (std::uint64_t p = 24; p + 24 <= sy->size; p += 24) {
            std::uint64_t const off = sy->offset + p;
            if (cstrAt(obj, st->offset + rdLE(obj, static_cast<std::size_t>(off), 4)) == "shared"
                && rdLE(obj, static_cast<std::size_t>(off + 6), 2) == kShnCommon) {
                row = static_cast<std::size_t>(off);
            }
        }
    }
    ASSERT_TRUE(row.has_value()) << "the writer states `shared` as an SHN_COMMON row";
    EXPECT_EQ(obj[*row + 4] & 0x0F, 1) << "STT_OBJECT";
    DiagnosticReporter plainRep;
    auto const plain = elf::readRelocatableObject(obj, *L.target, *L.format, plainRep);
    ASSERT_TRUE(plain.has_value()) << diagnosticsOf(plainRep);
    ASSERT_NE(rowNamed(*plain, "shared"), nullptr);
    EXPECT_EQ(rowNamed(*plain, "shared")->commonSize, 4u) << "CONTROL: an STT_OBJECT common reads as one";

    auto tls = obj;
    tls[*row + 4] = static_cast<std::uint8_t>((tls[*row + 4] & 0xF0) | 6);   // STT_TLS
    DiagnosticReporter tlsRep;
    auto const refused = elf::readRelocatableObject(tls, *L.target, *L.format, tlsRep);
    EXPECT_FALSE(refused.has_value());
    EXPECT_TRUE(sawCode(tlsRep, DiagnosticCode::K_CommonSymbolUnallocatable)) << diagnosticsOf(tlsRep);
    EXPECT_NE(diagnosticsOf(tlsRep).find("('shared')"), std::string::npos) << diagnosticsOf(tlsRep);
}

// ══ Native on Linux: gcc's own -fcommon objects (P69 round 4, review-xa3 MINOR 7) ═════
//
// The ELF twin of `ClTentativeDefinitionsLinkUnderDssAsUnderLinkExe`: the host's gcc writes the two TUs' counter and
// arrays as SHN_COMMON symbols under `-fcommon`, and DSS links them under its own `main` — exec and PIE — to the
// program GNU ld makes of the same objects with the same main (the CONTROL), run natively: x86_64 on the WSL leg,
// aarch64 on the arm64 VPS leg.
TEST(CommonSymbolsNative, GccCommonsLinkUnderDssAsUnderGnuLd) {
#if !defined(__linux__) || !(defined(__x86_64__) || defined(__aarch64__))
    GTEST_SKIP() << "not a Linux x86_64 or aarch64 host -- the gcc -fcommon arm runs on the Linux legs";
#else
    test_support::ScratchDir scratch{test_support::Location::Temp, "common-gcc-elf"};
    auto const dir = scratch.path();
    namespace np = test_support::native_probe;
    writeText(dir / "check.c", "int main(void){return 0;}\n");
    if (std::system(np::captureCmd("cc -o \"" + (dir / "check").string() + "\" \"" + (dir / "check.c").string() + "\"",
                                   dir / "check.txt")
                        .c_str())
        != 0) {
        GTEST_SKIP() << "the host's `cc` cannot build a trivial program: " << np::tailOf(dir / "check.txt", 5, "cc");
    }
    writeText(dir / "tu1.c", kTu1);
    writeText(dir / "tu3.c", kTu3);
    writeText(dir / "main.c", kClMain);
    for (char const* tu : {"tu1", "tu3"}) {
        std::string const cc = "cc -c -O2 -fcommon -o \"" + (dir / (std::string{tu} + ".o")).string() + "\" \""
                             + (dir / (std::string{tu} + ".c")).string() + "\"";
        ASSERT_EQ(std::system(np::captureCmd(cc, dir / (std::string{tu} + ".txt")).c_str()), 0)
            << cc << np::tailOf(dir / (std::string{tu} + ".txt"), 20, "cc");
    }
    // The objects really hold commons: what this arm is about.
    for (char const* tu : {"tu1", "tu3"}) {
        auto const bytes = fileBytes(dir / (std::string{tu} + ".o"));
        auto const counter = elfSymbolsNamed(bytes, "shared_counter");
        ASSERT_EQ(counter.size(), 1u) << tu;
        EXPECT_EQ(counter[0].shndx, kShnCommon) << tu << ": gcc -fcommon writes the tentative definition as SHN_COMMON";
    }
    // CONTROL: GNU ld's program of the same objects.
    std::string const ref = "cc -O2 -fcommon -o \"" + (dir / "ref").string() + "\" \"" + (dir / "main.c").string()
                          + "\" \"" + (dir / "tu1.o").string() + "\" \"" + (dir / "tu3.o").string() + "\"";
    ASSERT_EQ(std::system(np::captureCmd(ref, dir / "ref.txt").c_str()), 0) << ref << np::tailOf(dir / "ref.txt", 20);
    auto const refRun = test_support::runBinary(dir / "ref");
    ASSERT_TRUE(refRun.spawned) << refRun.diagnostic;
    EXPECT_EQ(refRun.exitCode, 42u) << "GNU ld";
#if defined(__x86_64__)
    std::vector<char const*> const specs{"x86_64:elf64-x86_64-linux-exec", "x86_64:elf64-x86_64-linux-pie"};
#else
    std::vector<char const*> const specs{"arm64:elf64-aarch64-linux-exec", "arm64:elf64-aarch64-linux-pie"};
#endif
    for (char const* spec : specs) {
        SCOPED_TRACE(spec);
        auto const out = dir / (std::string{"dss-"} + (std::string_view{spec}.ends_with("pie") ? "pie" : "exec"));
        fs::create_directories(out);
        Program p;
        p.setOutputDir(out);
        DiagnosticReporter rep;
        int const rc = p.compileFiles(std::vector<std::string>{(dir / "main.c").string(), (dir / "tu1.o").string(),
                                                               (dir / "tu3.o").string()},
                                      "c", std::vector<std::string>{spec}, rep);
        ASSERT_EQ(rc, 0) << "DSS must link gcc's -fcommon commons:" << diagnosticsOf(rep);
        fs::path image;
        for (auto const& e : fs::directory_iterator(out)) {
            if (e.is_regular_file() && e.path().extension().empty()) image = e.path();
        }
        ASSERT_FALSE(image.empty()) << "no image under " << out.string();
        auto const r = test_support::runBinary(image);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << "1000 + bits: 1 a TU's bump did not reach the shared counter, 2 the counter's "
                                      "value is wrong, 4 a TU does not see the other's array element";
    }
#endif
}

// ══ A common against a thread-local archive member (P69 round 4, review-xa3 MINOR 5) ═══
//
// GNU ld's predicate fetches a member whose `shared` is a THREAD-LOCAL global datum too, and then refuses the link
// ("shared: TLS definition in libtls.a(m_tls.o) section .tdata mismatches non-TLS reference in tu_common.o",
// ✔MEASURED 2026-10-07 on both ISAs, `.orchestrators/p69/work/xa/r4probe/m5` and `m5a64`). DSS fetches the same
// member and refuses the same link BY NAME at the binding (`linker::reportThreadStorageDisagreements`,
// D-LK-THREAD-STORAGE-DISAGREEMENT-REFUSED-ONLY-BY-THE-WRITER-BACKSTOP) — before this round the ELF writer's backstop
// refused it naming SymbolIds (`r4probe/m5dss`). gcc writes the thread-local member, which DSS's relocatable writer
// cannot (D-LK-RELOCATABLE-TBSS-UNSUPPORTED). CONTROL: the same common against an ordinary member links under GNU ld
// and under DSS to 7.
TEST(CommonSymbolsNative, ACommonAgainstAThreadLocalMemberIsRefusedByName) {
#if !defined(__linux__) || !(defined(__x86_64__) || defined(__aarch64__))
    GTEST_SKIP() << "not a Linux x86_64 or aarch64 host -- the gcc thread-local member arm runs on the Linux legs";
#else
    test_support::ScratchDir scratch{test_support::Location::Temp, "common-tls-member"};
    auto const dir = scratch.path();
    namespace np = test_support::native_probe;
    auto const at = [&](std::string const& name) { return "\"" + (dir / name).string() + "\""; };
    writeText(dir / "check.c", "int main(void){return 0;}\n");
    if (std::system(np::captureCmd("cc -o " + at("check") + " " + at("check.c"), dir / "check.txt").c_str()) != 0) {
        GTEST_SKIP() << "the host's `cc` cannot build a trivial program: " << np::tailOf(dir / "check.txt", 5, "cc");
    }
    writeText(dir / "tu_common.c", "int shared;\nint read_shared(void) { return shared; }\n");
    writeText(dir / "m_tls.c", "__thread int shared = 7;\n");
    writeText(dir / "m_plain.c", "int shared = 7;\n");
    writeText(dir / "main.c", "int read_shared(void);\nint main(void) { return read_shared(); }\n");
    auto const sh = [&](std::string const& cmd, std::string const& log) {
        return std::system(np::captureCmd(cmd, dir / log).c_str()) == 0;
    };
    ASSERT_TRUE(sh("cc -c -O2 -fcommon -o " + at("tu_common.o") + " " + at("tu_common.c"), "tu_common.txt"))
        << np::tailOf(dir / "tu_common.txt", 20, "cc");
    auto const common = elfSymbolsNamed(fileBytes(dir / "tu_common.o"), "shared");
    ASSERT_EQ(common.size(), 1u);
    EXPECT_EQ(common[0].shndx, kShnCommon) << "gcc -fcommon writes `int shared;` as SHN_COMMON";
    for (char const* m : {"tls", "plain"}) {
        std::string const stem = std::string{"m_"} + m;
        ASSERT_TRUE(sh("cc -c -O2 -o " + at(stem + ".o") + " " + at(stem + ".c"), stem + ".txt"))
            << np::tailOf(dir / (stem + ".txt"), 20, "cc");
        ASSERT_TRUE(sh("ar rcs " + at(std::string{"lib"} + m + ".a") + " " + at(stem + ".o"), stem + "-ar.txt"))
            << np::tailOf(dir / (stem + "-ar.txt"), 20, "ar");
    }
    // The reference: GNU ld refuses the thread-local member, and links the ordinary one to 7.
    EXPECT_FALSE(sh("cc -O2 -o " + at("ref_tls") + " " + at("main.c") + " " + at("tu_common.o") + " " + at("libtls.a"),
                    "ref_tls.txt"))
        << "GNU ld must refuse a thread-local definition for the common";
    EXPECT_NE(np::tailOf(dir / "ref_tls.txt", 20).find("mismatches non-TLS reference"), std::string::npos)
        << np::tailOf(dir / "ref_tls.txt", 20);
    ASSERT_TRUE(sh("cc -O2 -o " + at("ref_plain") + " " + at("main.c") + " " + at("tu_common.o") + " "
                       + at("libplain.a"),
                   "ref_plain.txt"))
        << np::tailOf(dir / "ref_plain.txt", 20);
    auto const refRun = test_support::runBinary(dir / "ref_plain");
    ASSERT_TRUE(refRun.spawned) << refRun.diagnostic;
    EXPECT_EQ(refRun.exitCode, 7u) << "GNU ld, the ordinary member";
#if defined(__x86_64__)
    char const* const spec = "x86_64:elf64-x86_64-linux-exec";
#else
    char const* const spec = "arm64:elf64-aarch64-linux-exec";
#endif
    for (char const* m : {"tls", "plain"}) {
        SCOPED_TRACE(m);
        auto const out = dir / (std::string{"dss-"} + m);
        fs::create_directories(out);
        Program p;
        p.setOutputDir(out);
        p.setResolveLibraries(std::vector<fs::path>{dir / (std::string{"lib"} + m + ".a")});
        DiagnosticReporter rep;
        int const rc = p.compileFiles(std::vector<std::string>{(dir / "main.c").string(), (dir / "tu_common.o").string()},
                                      "c", std::vector<std::string>{spec}, rep);
        if (std::string_view{m} == "tls") {
            EXPECT_NE(rc, 0) << "DSS must refuse the thread-local member's definition for the common";
            std::vector<std::string> named;
            for (auto const& d : rep.all()) {
                if (d.code == DiagnosticCode::K_ExternImportAttributeConflict) named.push_back(d.actual);
            }
            ASSERT_EQ(named.size(), 1u) << diagnosticsOf(rep);
            EXPECT_NE(named[0].find("symbol 'shared'"), std::string::npos) << named[0];
            // The common has yielded to the member's definition (`allocateCommonDefinitions`, the link's first pass),
            // so the link refuses the ordinary reference it now is, in the function that reads it.
            EXPECT_NE(named[0].find("(in `read_shared`) refers to it as an ORDINARY object"), std::string::npos)
                << named[0];
            EXPECT_NE(named[0].find("has THREAD STORAGE DURATION"), std::string::npos) << named[0];
            EXPECT_FALSE(sawCode(rep, DiagnosticCode::K_RelocationKindMismatch))
                << "the writer's backstop must not be what refuses it:" << diagnosticsOf(rep);
            continue;
        }
        ASSERT_EQ(rc, 0) << "CONTROL: DSS links the ordinary member:" << diagnosticsOf(rep);
        fs::path image;
        for (auto const& e : fs::directory_iterator(out)) {
            if (e.is_regular_file() && e.path().extension().empty()) image = e.path();
        }
        ASSERT_FALSE(image.empty()) << "no image under " << out.string();
        auto const r = test_support::runBinary(image);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 7u) << "CONTROL: DSS, the ordinary member";
    }
#endif
}

// ══ Native, every host: a common beside a WEAK definition and beside archive members (P69 round 4) ═════════
//
// D-LK-COMMON-OUTRANKED-A-WEAK-DEFINITION-IN-EVERY-FORMAT and D-LK-MACHO-ARCHIVE-COMMON-RESOLUTION-UNMEASURED. The
// host's reference compiler writes `int shared;` as a COMMON (`tu_common`, whose `read_shared` reads it: -fcommon, and
// cl writes every tentative definition so), and three members: `m_weak` (a WEAK definition, 5 —
// `__attribute__((weak))`, cl `__declspec(selectany)`), `m_strong` (7) and `m_function` (a FUNCTION named `shared`).
// A DSS `main.c` returns what `read_shared` reads. Five cells: `m_weak` linked beside the common, and four archives,
// their members in the order named. Each links under the host's reference linker — the CONTROL, which must answer
// what the rows' measurements recorded for it — and under DSS, which must answer the same:
//                            weak_direct  weak_member  weak_then_strong  function_then_strong  function_only
//   GNU ld (ELF)                  0            0              7                   7                  0
//   Apple's ld (Mach-O)           5            5              5                   7                  0
//   link.exe (cl, PE)             5            0              0                   0                  0
//   GNU ld (MinGW gcc, PE)        0            0              0                   0                  0
// (✔MEASURED 2026-10-07, `.orchestrators/p69/work/xa/r4probe/m5`, `m14mac`, `m15`: GNU ld's common outranks a weak
// definition and its archive search fetches only a global datum; Apple's ld lets a weak definition win, and fetches
// one; link.exe lets cl's selectany win and keeps a common against any archive; a MinGW weak definition is a weak
// EXTERNAL, which the common satisfies.) On the Mac a THREAD-LOCAL member is linked too: Apple's ld links it for the
// common without a word, to a program that reads the TLV descriptor as the datum, and DSS must refuse it.
namespace {

constexpr char const* kYieldMain   = "int read_shared(void);\nint main(void) { return read_shared(); }\n";
constexpr char const* kYieldCommon = "int shared;\nint read_shared(void) { return shared; }\n";

struct YieldCell {
    char const*              name;
    std::vector<char const*> direct;     // objects linked beside `tu_common`
    std::vector<char const*> archived;   // one archive's members, in order; empty: no archive
};

[[nodiscard]] std::vector<YieldCell> const& yieldCells() {
    static std::vector<YieldCell> const cells{{"weak_direct", {"m_weak"}, {}},
                                              {"weak_member", {}, {"m_weak"}},
                                              {"weak_then_strong", {}, {"m_weak", "m_strong"}},
                                              {"function_then_strong", {}, {"m_function", "m_strong"}},
                                              {"function_only", {}, {"m_function"}}};
    return cells;
}

// One reference toolchain of this host, as the cells drive it. Every command runs in the arm's own directory and
// names its files relatively.
struct YieldArm {
    std::string             label;
    std::string             weakSpelling;   // how this compiler spells a WEAK definition
    std::string             obj, lib, exe;  // its products' extensions
    char const*             spec = nullptr; // the DSS image the cells link into
    std::array<unsigned, 5> expected{};     // per cell, in `yieldCells()` order
    bool                    tlsCell = false;
    std::function<std::string(std::string const& stem, bool common)>                         compile;
    std::function<std::string(std::string const& lib, std::vector<std::string> const& members)> archive;
    std::function<std::string(std::string const& out, std::vector<std::string> const& inputs)>  link;   // after main.c
    std::function<bool(fs::path const& dir, std::string const& cmd, std::string const& log)>     run;
};

[[nodiscard]] YieldArm gnuStyleArm(std::string label, std::string const& archFlags, char const* spec,
                                   std::array<unsigned, 5> expected, std::string exe, bool tlsCell) {
    YieldArm a;
    a.label        = std::move(label);
    a.weakSpelling = "__attribute__((weak))";
    a.obj          = ".o";
    a.lib          = ".a";
    a.exe          = std::move(exe);
    a.spec         = spec;
    a.expected     = expected;
    a.tlsCell      = tlsCell;
    std::string const cc = std::string{a.exe.empty() ? "cc" : "gcc"} + (archFlags.empty() ? "" : " " + archFlags);
    a.compile = [cc](std::string const& stem, bool common) {
        return cc + " -O2" + (common ? " -fcommon" : "") + " -c -o " + stem + ".o " + stem + ".c";
    };
    a.archive = [](std::string const& lib, std::vector<std::string> const& members) {
        std::string c = "ar rcs " + lib + ".a";
        for (auto const& m : members) c += " " + m + ".o";
        return c;
    };
    a.link = [cc, ext = a.exe](std::string const& out, std::vector<std::string> const& inputs) {
        std::string c = cc + " -O2 -o " + out + ext + " main.c";
        for (auto const& i : inputs) c += " " + i;
        return c;
    };
    return a;
}

[[nodiscard]] std::vector<YieldArm> yieldArms(fs::path const& work) {
    std::vector<YieldArm> arms;
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__)) || defined(__APPLE__)
    (void)work;
    auto const posixRun = [](fs::path const& dir, std::string const& cmd, std::string const& log) {
        namespace np = test_support::native_probe;
        return std::system(np::captureCmd("cd \"" + dir.string() + "\" && " + cmd, dir / log).c_str()) == 0;
    };
#if defined(__APPLE__)
    arms.push_back(gnuStyleArm("Apple clang -arch arm64 (Apple's ld)", "-arch arm64", "arm64:macho64-arm64-darwin-exec",
                               {5, 5, 5, 7, 0}, "", true));
    arms.push_back(gnuStyleArm("Apple clang -arch x86_64 (Apple's ld)", "-arch x86_64",
                               "x86_64:macho64-x86_64-darwin-exec", {5, 5, 5, 7, 0}, "", true));
#elif defined(__x86_64__)
    arms.push_back(gnuStyleArm("gcc (GNU ld, x86_64)", "", "x86_64:elf64-x86_64-linux-exec", {0, 0, 7, 7, 0}, "", false));
#else
    arms.push_back(gnuStyleArm("gcc (GNU ld, aarch64)", "", "arm64:elf64-aarch64-linux-exec", {0, 0, 7, 7, 0}, "", false));
#endif
    for (auto& a : arms) a.run = posixRun;
#elif defined(_WIN32)
    namespace np = test_support::native_probe;
    auto const msvc = np::locateMsvcToolchain(work);
    // A toolchain that is absent is this host's fact; one that is present and cannot be entered is a failure.
    if (!msvc.toolAbsent() && !msvc.ok()) ADD_FAILURE() << msvc.describe();
    if (!msvc.toolAbsent() && msvc.ok()) {
        auto const tools = std::make_shared<np::MsvcTools>(np::msvcToolsIn(msvc, work));
        if (!tools->ready()) ADD_FAILURE() << tools->describe();
        YieldArm a;
        a.label        = "cl (link.exe)";
        a.weakSpelling = "__declspec(selectany)";
        a.obj          = ".obj";
        a.lib          = ".lib";
        a.exe          = ".exe";
        a.spec         = "x86_64:pe64-x86_64-windows-exec";
        a.expected     = {5, 0, 0, 0, 0};
        a.compile      = [](std::string const& stem, bool) { return "cl /nologo /c /O2 /MD " + stem + ".c"; };
        a.archive      = [](std::string const& lib, std::vector<std::string> const& members) {
            std::string c = "lib /nologo /OUT:" + lib + ".lib";
            for (auto const& m : members) c += " " + m + ".obj";
            return c;
        };
        a.link = [](std::string const& out, std::vector<std::string> const& inputs) {
            std::string c = "cl /nologo /O2 /MD main.c";
            for (auto const& i : inputs) c += " " + i;
            return c + " /link /OUT:" + out + ".exe";
        };
        a.run = [tools](fs::path const& dir, std::string const& cmd, std::string const& log) {
            return runCapturing(tools.get(), dir, cmd, log);
        };
        arms.push_back(std::move(a));
    }
    if (std::system("where gcc >nul 2>&1") == 0) {
        auto a = gnuStyleArm("MinGW gcc (GNU ld, PE)", "", "x86_64:pe64-x86_64-windows-exec", {0, 0, 0, 0, 0}, ".exe",
                             false);
        a.run = [](fs::path const& dir, std::string const& cmd, std::string const& log) {
            return runCapturing(nullptr, dir, cmd, log);
        };
        arms.push_back(std::move(a));
    }
#else
    (void)work;
#endif
    return arms;
}

}  // namespace

TEST(CommonSymbolsNative, ACommonBesideWeakAndArchivedDefinitionsResolvesAsTheReferenceLinkerDoes) {
    namespace np = test_support::native_probe;
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "common-yields"};
    auto const dir  = scratch.path();
    auto const arms = yieldArms(dir);
    if (arms.empty()) GTEST_SKIP() << "no reference C toolchain on this host";
    auto const strict = test_support::readStrictArmVerdicts();
    ASSERT_FALSE(strict.malformed) << test_support::kStrictArmVerdictsEnv << "='" << strict.raw
                                   << "' is not a recognised value";
    std::size_t ran = 0;
    for (std::size_t k = 0; k < arms.size(); ++k) {
        auto const& arm = arms[k];
        SCOPED_TRACE(arm.label);
        // How THIS host runs the arm's images: natively, or through the one cross-arch gate.
        std::vector<std::string> launcher;
        bool                     launcherExecsImage = false;
        if (std::string const arch = test_support::specTargetArch(arm.spec); arch != test_support::currentHostArch()) {
            auto const gate = test_support::crossArchDecisionForThisHost(arch, /*manifestEmulator=*/"");
            if (!gate.runs) {
                if (strict.on && test_support::armVerdictIsEnvironmentalSkip(gate.skip)) {
                    ADD_FAILURE() << gate.why;
                } else {
                    std::cout << "[native-arm] " << arm.label << ": not run — " << gate.why << "\n";
                }
                continue;
            }
            launcher           = gate.launcherPrefix;
            launcherExecsImage = gate.launcherExecsImage;
        }
        auto const runImage = [&](fs::path const& exe) {
            return test_support::runBinary(exe, test_support::kRunBudget, /*captureStdout=*/false, launcher,
                                           /*programArgs=*/{}, launcherExecsImage);
        };
        fs::path const d = dir / ("arm" + std::to_string(k));
        fs::create_directories(d);
        writeText(d / "main.c", kYieldMain);
        writeText(d / "tu_common.c", kYieldCommon);
        writeText(d / "m_weak.c", arm.weakSpelling + " int shared = 5;\n");
        writeText(d / "m_strong.c", "int shared = 7;\n");
        writeText(d / "m_function.c", "int shared(void) { return 9; }\n");
        std::vector<std::string> stems{"m_weak", "m_strong", "m_function"};
        if (arm.tlsCell) {
            writeText(d / "m_tls.c", "__thread int shared = 5;\n");
            stems.push_back("m_tls");
        }
        ASSERT_TRUE(arm.run(d, arm.compile("tu_common", /*common=*/true), "tu_common.txt"))
            << np::tailOf(d / "tu_common.txt", 20, arm.label);
        for (auto const& s : stems) {
            ASSERT_TRUE(arm.run(d, arm.compile(s, /*common=*/false), s + ".txt"))
                << np::tailOf(d / (s + ".txt"), 20, arm.label);
        }
        // DSS's link of `main.c`, `tu_common` and `direct`, against `library` when it names one; its exit code, or
        // nullopt after recording why the link was refused.
        auto const dssLink = [&](std::string const& cell, std::vector<std::string> const& direct,
                                 std::string const& library, DiagnosticReporter& rep) -> std::optional<fs::path> {
            auto const out = d / ("dss_" + cell);
            fs::create_directories(out);
            Program p;
            p.setOutputDir(out);
            if (!library.empty()) p.setResolveLibraries(std::vector<fs::path>{d / library});
            std::vector<std::string> inputs{(d / "main.c").string(), (d / ("tu_common" + arm.obj)).string()};
            for (auto const& o : direct) inputs.push_back((d / o).string());
            if (p.compileFiles(inputs, "c", std::vector<std::string>{arm.spec}, rep) != 0) return std::nullopt;
            for (auto const& e : fs::directory_iterator(out)) {
                if (e.is_regular_file() && e.path().extension() == arm.exe) return e.path();
            }
            return std::nullopt;
        };
        auto const& cells = yieldCells();
        for (std::size_t c = 0; c < cells.size(); ++c) {
            auto const& cell = cells[c];
            SCOPED_TRACE(cell.name);
            std::vector<std::string> direct;
            for (char const* o : cell.direct) direct.push_back(std::string{o} + arm.obj);
            std::string library;
            if (!cell.archived.empty()) {
                std::string const stem = std::string{"lib_"} + cell.name;
                std::vector<std::string> const members(cell.archived.begin(), cell.archived.end());
                ASSERT_TRUE(arm.run(d, arm.archive(stem, members), stem + ".txt"))
                    << np::tailOf(d / (stem + ".txt"), 20, arm.label);
                library = stem + arm.lib;
            }
            // CONTROL: the reference linker's program of the same objects.
            std::vector<std::string> refInputs{"tu_common" + arm.obj};
            refInputs.insert(refInputs.end(), direct.begin(), direct.end());
            if (!library.empty()) refInputs.push_back(library);
            std::string const refStem = std::string{"ref_"} + cell.name;
            ASSERT_TRUE(arm.run(d, arm.link(refStem, refInputs), refStem + ".txt"))
                << np::tailOf(d / (refStem + ".txt"), 20, arm.label);
            auto const ref = runImage(d / (refStem + arm.exe));
            ASSERT_TRUE(ref.spawned) << ref.diagnostic;
            EXPECT_EQ(ref.exitCode, arm.expected[c]) << "the reference linker no longer answers what the rows measured";
            DiagnosticReporter rep;
            auto const image = dssLink(cell.name, direct, library, rep);
            ASSERT_TRUE(image.has_value()) << "DSS must link the cell:" << diagnosticsOf(rep);
            auto const r = runImage(*image);
            ASSERT_TRUE(r.spawned) << r.diagnostic;
            EXPECT_FALSE(r.timedOut);
            EXPECT_EQ(r.exitCode, ref.exitCode) << "DSS must give the reference linker's answer";
            std::cout << "[native-arm] " << arm.label << " " << cell.name << ": reference linker " << ref.exitCode
                      << ", DSS " << r.exitCode << "\n";
        }
        if (arm.tlsCell) {
            SCOPED_TRACE("tls_member");
            ASSERT_TRUE(arm.run(d, arm.archive("lib_tls_member", {"m_tls"}), "lib_tls_member.txt"))
                << np::tailOf(d / "lib_tls_member.txt", 20, arm.label);
            DiagnosticReporter rep;
            auto const image = dssLink("tls_member", {}, "lib_tls_member" + arm.lib, rep);
            EXPECT_FALSE(image.has_value()) << "DSS must refuse a thread-local definition for the common";
            // The refusal must be an ERROR naming the common's symbol — the datum the reference program reads its
            // TLV descriptor through — whichever check reaches it first; every error is printed, so which check
            // that was is on the record.
            bool named = false;
            for (auto const& diag : rep.all()) {
                if (diag.severity != DiagnosticSeverity::Error) continue;
                named = named || diag.actual.find("_shared") != std::string::npos;
                std::cout << "[native-arm] " << arm.label << " tls_member: DSS refused, "
                          << diagnosticCodeName(diag.code) << ": " << diag.actual << "\n";
            }
            EXPECT_TRUE(named) << "the refusal must name the common's symbol:" << diagnosticsOf(rep);
        }
        ++ran;
    }
    if (ran == 0 && !HasFailure()) GTEST_SKIP() << "no arm could run on this host (see the [native-arm] lines)";
}
