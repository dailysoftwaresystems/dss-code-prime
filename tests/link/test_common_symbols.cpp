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
//   * THE WEAK-NAME RULE (D-LK-WEAK-NAME-REFERENCE-BOUND-TO-THE-BODY-NOT-THE-NAME, P69 send-back 5; the suites
//     `WeakNameReferences` and `WeakNameReferencesNative` at the end of this file — a common is one of the winners a
//     weak name can lose to, and the units, families and image readers are this file's): a reference an object
//     writes THROUGH A WEAK NAME of a body that has another external name is resolved BY NAME — every writer with its
//     reader, an image link, a relocatable link and the link after it, and each host's reference linkers cell by cell.
//   * THE ID OF A SYMBOL-TABLE RECORD (D-LK-OBJECT-READERS-GAVE-RECORD-ZERO-THE-INVALID-SYMBOL-ID, P69; the suites
//     `RecordSymbolIds` and `RecordSymbolIdsNative` at the end of this file — the families, the object readers and
//     the reference toolchains are this file's): a definition that is its object's FIRST record keeps its name
//     through a relocatable link of that object alone, in every format whose symbol table reserves no record.

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
#include "link/pointer_reloc.hpp"
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
#include <unordered_map>
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

// A hand-built x86_64 unit stated by NAME: commons, plain references and data definitions (4 initialized bytes), and
// one-instruction functions — a marker (`xor eax, eax ; ret`), a `reader` of a datum, a caller (`call rel32 ; ret`,
// rel32 = kind 1 on x86_64). Rows are written in the order stated, and a function names what it reads or calls,
// which is stated before it.
class UnitOf {
public:
    explicit UnitOf(std::uint32_t cu) { m_.cuId = CompilationUnitId{cu}; }
    UnitOf& common(std::string const& name, std::uint64_t size = 4, std::uint64_t align = 4) {
        m_.externImports.push_back(commonRow(mint(name), name, size, align));
        return *this;
    }
    UnitOf& reference(std::string const& name, bool isData) {
        ExternImport e;
        e.symbol      = SymbolId{mint(name)};
        e.mangledName = name;
        e.isData      = isData;
        m_.externImports.push_back(std::move(e));
        return *this;
    }
    UnitOf& datum(std::string const& name, SymbolBinding binding, std::uint8_t value) {
        AssembledData d;
        d.symbol    = SymbolId{mint(name)};
        d.section   = DataSectionKind::Data;
        d.bytes     = {value, 0, 0, 0};
        d.alignment = Alignment::of<4>();
        m_.symbols.push_back(ModuleSymbol{d.symbol, name, binding, SymbolVisibility::Default});
        m_.dataItems.push_back(std::move(d));
        return *this;
    }
    // A second name of the definition `of`.
    UnitOf& alias(std::string const& name, std::string const& of, SymbolBinding binding) {
        m_.symbols.push_back(ModuleSymbol{SymbolId{ids_.at(of)}, name, binding, SymbolVisibility::Default});
        return *this;
    }
    UnitOf& marker(std::string const& fn) {
        AssembledFunction f;
        f.symbol = SymbolId{mint(fn)};
        f.bytes  = {0x31, 0xC0, 0xC3};
        return function(std::move(f), fn);
    }
    UnitOf& reads(std::string const& fn, std::string const& datumName) {
        std::uint32_t const target = ids_.at(datumName);
        return function(reader(mint(fn), target), fn);
    }
    UnitOf& calls(std::string const& fn, std::string const& callee) {
        AssembledFunction f;
        f.bytes = {0xE8, 0, 0, 0, 0, 0xC3};
        Relocation rel;
        rel.offset = 1;
        rel.target = SymbolId{ids_.at(callee)};
        rel.kind   = RelocationKind{1};
        rel.addend = 0;
        f.relocations.push_back(rel);
        f.symbol = SymbolId{mint(fn)};
        return function(std::move(f), fn);
    }
    [[nodiscard]] AssembledModule build() const {
        AssembledModule m   = m_;
        m.expectedFuncCount = m.functions.size();
        return m;
    }

private:
    std::uint32_t mint(std::string const& name) {
        std::uint32_t const id = next_++;
        ids_.emplace(name, id);
        return id;
    }
    UnitOf& function(AssembledFunction f, std::string const& name) {
        m_.symbols.push_back(ModuleSymbol{f.symbol, name, SymbolBinding::Global, SymbolVisibility::Default});
        m_.functions.push_back(std::move(f));
        return *this;
    }
    AssembledModule                                m_;
    std::uint32_t                                  next_ = 1;
    std::unordered_map<std::string, std::uint32_t> ids_;
};

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
// own 7 — and no common. CONTROLS, where the common OUTRANKS the weak definition — the ELF document's answer, the
// gABI's (the image twin is `ACommonWinsOverAWeakDefinition`), and on PE a weak definition whose own SPELLING yields to
// a common (`ModuleSymbol::yieldsToACommon`, what the COFF reader reads a weak external's name as — MinGW gcc's weak
// definition, which GNU ld's PE linker gives the common's 0): the artifact hands the COMMON on, as `ld -r` does
// (✔MEASURED 2026-10-08, GNU ld 2.42's ELF linker on x86_64 and aarch64, ld.lld 18.1.3, and GNU ld's PE linker on
// MinGW gcc 13.2.0's weak external, both unit orders: the artifact's symbol is the common, which a later link
// allocates, replaces by a strong definition without a duplicate, or folds with a larger common) — the 40-byte common
// row, no definition of the name, the weak definition's bytes kept under no external name, and BOTH units' reads on
// the common. Until round 4 every format took ELF's answer, so a Mach-O or PE link read 0 where Apple's ld and
// link.exe read the weak definition's value; until send-back 5 (review-xa4 NIT 15) the artifact ALLOCATED the common
// where it outranks, strong, so a later link beside a strong definition saw two.
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
        // The second unit defines the name WEAK (7) and reads it itself.
        std::vector<AssembledModule> mods{
            unitWithCommon(1, "reader_fn", 40, 32, /*entry=*/false),
            UnitOf(2).datum(arm.shared, SymbolBinding::Weak, 7).reads("weak_reader", arm.shared).build()};
        mods[0].externImports[0].mangledName = arm.shared;
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
        auto const*         row = rowNamed(*back, arm.shared);
        ModuleSymbol const* sym = nullptr;
        for (auto const& s : back->symbols) {
            if (s.name == arm.shared) sym = &s;
        }
        if (arm.weakWins) {
            EXPECT_EQ(row, nullptr) << "one object cannot carry a common and a definition of a name";
            ASSERT_NE(sym, nullptr) << "the artifact must DEFINE the name";
            AssembledData const* item = nullptr;
            for (auto const& d : back->dataItems) {
                if (d.symbol == sym->symbol) item = &d;
            }
            ASSERT_NE(item, nullptr);
            EXPECT_EQ(sym->binding, SymbolBinding::Weak) << "the weak definition, still weak";
            ASSERT_EQ(item->bytes.size(), 4u);
            EXPECT_EQ(item->bytes[0], 7u) << "the weak definition's own bytes";
            continue;
        }
        ASSERT_NE(row, nullptr) << "CONTROL: the artifact hands the common ON, as `ld -r` does";
        EXPECT_EQ(row->commonSize, 40u) << "CONTROL: the common's 40 bytes, still the final linker's to allocate";
        EXPECT_EQ(row->commonAlignment, 32u);
        EXPECT_EQ(sym, nullptr) << "CONTROL: nothing defines the name: the weak definition gave it up";
        std::size_t keptBodies = 0;
        for (auto const& d : back->dataItems) keptBodies += (d.bytes.size() == 4u && d.bytes[0] == 7u) ? 1u : 0u;
        EXPECT_EQ(keptBodies, 1u) << "CONTROL: the weak definition's bytes stay, under no external name";
        std::size_t reads = 0;
        for (auto const& fn : back->functions) {
            for (auto const& rel : fn.relocations) {
                EXPECT_EQ(rel.target, row->symbol) << "CONTROL: each unit's read reaches the common";
                ++reads;
            }
        }
        EXPECT_EQ(reads, 2u) << "CONTROL: the common unit's read and the weak unit's own";
    }
}

// What a later link makes of that artifact is what it makes of `ld -r`'s (✔MEASURED 2026-10-08, GNU ld 2.42, x86_64
// and aarch64, both orders of the two objects): alone the program reads the common's zero; beside a STRONG definition
// it reads that definition's value, with no duplicate; beside a LARGER common the storage is the larger. The artifact —
// a 40-byte common beside a weak definition (5) that reads the name itself, in both unit orders — is written, read
// back and linked into an image three times. Until send-back 5 it held the common ALLOCATED, strong: the second link
// was refused (two strong definitions of one name) and the third kept 40 bytes where 64 were asked for.
TEST(CommonSymbols, AnArtifactsCommonOverAnOutrankedWeakDefinitionIsStillACommonToALaterLink) {
    auto const R = load("x86_64", "elf64-x86_64-linux");
    auto const X = load("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_TRUE(R.target && R.format && X.format);
    for (bool commonFirst : {true, false}) {
        SCOPED_TRACE(commonFirst ? "the common's unit first" : "the weak definition's unit first");
        std::vector<AssembledModule> units{
            unitWithCommon(1, "reader_fn", 40, 32, /*entry=*/false),
            UnitOf(2).datum("shared", SymbolBinding::Weak, 5).reads("weak_reader", "shared").build()};
        if (!commonFirst) std::swap(units[0], units[1]);
        DiagnosticReporter rep;
        auto const obj = linker::link(std::span<AssembledModule const>{units}, *R.target, *R.format, rep);
        ASSERT_TRUE(obj.ok()) << diagnosticsOf(rep);
        auto artifact = elf::readRelocatableObject(obj.bytes, *R.target, *R.format, rep);
        ASSERT_TRUE(artifact.has_value()) << diagnosticsOf(rep);
        artifact->cuId = CompilationUnitId{1};
        for (auto const& s : artifact->symbols) {
            if (s.name == "reader_fn") artifact->userEntrySymbol = s.symbol;
        }
        ASSERT_TRUE(artifact->userEntrySymbol.has_value());
        // What both of the artifact's functions read in an image of it and `beside`: the section, and the address.
        struct Read {
            ElfSection    section;
            std::uint64_t at    = 0;
            std::uint8_t  first = 0;   // the first byte there, 0 in zero-filled storage
        };
        auto const linked = [&](std::vector<AssembledModule> beside, char const* what) -> std::optional<Read> {
            std::vector<AssembledModule> mods{*artifact};
            for (auto& m : beside) mods.push_back(std::move(m));
            DiagnosticReporter linkRep;
            auto const img = linker::link(std::span<AssembledModule const>{mods}, *X.target, *X.format, linkRep);
            EXPECT_TRUE(img.ok()) << what << ":" << diagnosticsOf(linkRep);
            if (!img.ok()) return std::nullopt;
            auto const a = datumReadBy(img.bytes, "reader_fn");
            auto const b = datumReadBy(img.bytes, "weak_reader");
            EXPECT_TRUE(a.has_value() && b.has_value()) << what;
            if (!a.has_value() || !b.has_value()) return std::nullopt;
            EXPECT_EQ(*a, *b) << what << ": the weak definition's own unit reads what every other unit reads";
            auto const sec = sectionHolding(img.bytes, *a);
            EXPECT_TRUE(sec.has_value()) << what;
            if (!sec.has_value()) return std::nullopt;
            Read r{*sec, *a, 0};
            if (sec->name != ".bss") r.first = img.bytes[sec->offset + (*a - sec->addr)];
            return r;
        };
        auto const alone = linked({}, "alone");
        ASSERT_TRUE(alone.has_value());
        EXPECT_EQ(alone->section.name, ".bss") << "alone: the common, allocated by the final link";
        EXPECT_GE(storageFrom(alone->section, alone->at), 40u);
        EXPECT_LT(storageFrom(alone->section, alone->at), 64u) << "CONTROL of the larger common below";

        auto const strong = linked({unitDefiningShared(2, SymbolBinding::Global)}, "beside a strong definition");
        ASSERT_TRUE(strong.has_value()) << "a strong definition beside the artifact is no duplicate";
        EXPECT_EQ(strong->section.name, ".data");
        EXPECT_EQ(strong->first, 7u) << "the strong definition's own bytes, not the weak definition's 5";

        auto const larger = linked({UnitOf(2).common("shared", 64, 32).build()}, "beside a larger common");
        ASSERT_TRUE(larger.has_value());
        EXPECT_EQ(larger->section.name, ".bss");
        EXPECT_GE(storageFrom(larger->section, larger->at), 64u) << "the larger common's 64 bytes";
    }
}

// A weak name of a definition that has ANOTHER name gives up only the name: the body stays where its other name is.
// `shared` is the weak second name of `other` (5); beside a 40-byte common `shared`, on ELF, the artifact carries the
// common, `other` with its bytes, and no definition named `shared`.
TEST(CommonSymbols, AnOutrankedWeakNameOfADefinitionWithAnotherNameGivesOnlyTheNameUp) {
    auto const L = load("x86_64", "elf64-x86_64-linux");
    ASSERT_TRUE(L.target && L.format);
    std::vector<AssembledModule> mods{
        unitWithCommon(1, "reader_fn", 40, 32, /*entry=*/false),
        UnitOf(2).datum("other", SymbolBinding::Global, 5).alias("shared", "other", SymbolBinding::Weak).build()};
    DiagnosticReporter rep;
    auto const obj = linker::link(std::span<AssembledModule const>{mods}, *L.target, *L.format, rep);
    ASSERT_TRUE(obj.ok()) << diagnosticsOf(rep);
    auto const back = elf::readRelocatableObject(obj.bytes, *L.target, *L.format, rep);
    ASSERT_TRUE(back.has_value()) << diagnosticsOf(rep);
    auto const* row = rowNamed(*back, "shared");
    ASSERT_NE(row, nullptr) << "the artifact hands the common on";
    EXPECT_EQ(row->commonSize, 40u);
    ModuleSymbol const* other = nullptr;
    for (auto const& s : back->symbols) {
        EXPECT_NE(s.name, "shared") << "no definition keeps the name the common outranks";
        if (s.name == "other") other = &s;
    }
    ASSERT_NE(other, nullptr) << "the body keeps its other name";
    EXPECT_EQ(other->binding, SymbolBinding::Global);
    AssembledData const* item = nullptr;
    for (auto const& d : back->dataItems) {
        if (d.symbol == other->symbol) item = &d;
    }
    ASSERT_NE(item, nullptr);
    ASSERT_EQ(item->bytes.size(), 4u);
    EXPECT_EQ(item->bytes[0], 5u) << "and its bytes";
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
    EXPECT_TRUE(shared->recordStatesStorageDuration)
        << "an ELF symbol's TYPE states its storage duration, and GNU ld and ld.lld compare it with the definition's "
           "whether or not the unit reads the name";
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
    EXPECT_FALSE(shared->recordStatesStorageDuration) << "a Mach-O nlist has no type that says thread-local";
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
    EXPECT_FALSE(shared->recordStatesStorageDuration)
        << "a COFF symbol record states no storage duration: link.exe and lld-link link a common nothing reads "
           "beside a thread-local definition, and the program runs";
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
// reference linkers split (✔MEASURED 2026-10-07: GNU ld 2.42's ELF
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
// `elf_link_is_defined_archive_symbol`; ✔MEASURED 2026-10-07, gcc 13.3.0 + GNU ld 2.42:
// common-, weak- and function-then-strong archives all fetch the strong
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
// x86_64: weak-then-strong fetches the weak member, whose value the
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

// ══ What ends a common's search, and when it is walked (P69 send-back 5, review-xa4 MINOR 1) ═══════════════
//
// A common stays a common until a definition it YIELDS to is in the link, and the search asks that of the
// definitions already linked exactly as it asks it of a member (`linker::commonYieldsToDefinition`, the ONE answer).
// And the search answers every REFERENCE it holds before it walks the names still common. Until send-back 5 it took
// ANY linked definition of the name for an answer — a weak one the common outranks included — and walked a common
// where its row stood among the references, so what it fetched depended on the order of the linked object's rows.
// ✔MEASURED 2026-10-08 (gcc 13.3.0 + GNU ld 2.42, x86_64 and aarch64; Apple clang 21's ld-1267 on arm64 and x86_64
// and ld64-957.1, probe-reference-cc runs 20261008-081329-a00ad174 and 20261008-082117-c1373799; cl 19.44 + link.exe
// 14.44, lld-link 19.1.5, MinGW gcc 13.2.0 + GNU ld 2.42):
//   * a program holding `shared` as a common, linked beside an object that defines it WEAK, against an archive whose
//     member S defines it strong: GNU ld's ELF linker fetches S (the program reads 7); Apple's ld does not (the weak
//     definition has replaced the common); no PE linker fetches for a common;
//   * a program holding `shared` as a common and calling `f`, against an archive of W (`f` beside a WEAK `shared`) and
//     S, in both member orders: GNU ld fetches W and S (7); Apple's ld W alone, S never; the PE linkers W alone;
//   * two commons T1 and T2 against X (a strong T1 and a call of `u`), U (`u` beside a weak T2) and T (a strong T2),
//     in every member order: Apple's ld fetches T exactly when T precedes U, whatever X's place and the commons'
//     order — T2 was walked before X's `u` was answered; GNU ld loads all three in every order.

namespace {

using ObjectEncoder = std::function<std::vector<std::uint8_t>(AssembledModule const&, TargetSchema const&,
                                                              ObjectFormatSchema const&, DiagnosticReporter&)>;

// One format family as the search pins drive it.
struct SearchFamily {
    char const*   label;
    char const*   relocatable;     // the document of the linked objects and of the archives' members
    char const*   exec;            // the link's own document
    std::string   us;              // the C decoration of a name ("_" on Mach-O)
    bool          commonOutranksAWeakDefinition;   // so a weak definition does not end the common's search
    bool          fetchesForACommon;               // the members' `archiveCommonResolution`
    ObjectEncoder encode;
};

[[nodiscard]] std::vector<SearchFamily> const& searchFamilies() {
    static std::vector<SearchFamily> const families{
        {"ELF", "elf64-x86_64-linux", "elf64-x86_64-linux-exec", "", true, true,
         [](auto const& m, auto const& t, auto const& f, auto& r) { return elf::encode(m, t, f, r); }},
        {"Mach-O", "macho64-x86_64-darwin", "macho64-x86_64-darwin-exec", "_", false, true,
         [](auto const& m, auto const& t, auto const& f, auto& r) { return macho::encode(m, t, f, r); }},
        {"PE", "pe64-x86_64-windows", "pe64-x86_64-windows-exec", "", false, false,
         [](auto const& m, auto const& t, auto const& f, auto& r) { return pe::encode(m, t, f, r); }}};
    return families;
}

// An archive of `members`, written at `path`; empty on a failed write.
[[nodiscard]] fs::path archiveAt(fs::path const& path, std::vector<link::format::ArMemberInput> const& members,
                                 DiagnosticReporter& rep) {
    auto const archive = link::format::writeArArchive(members, rep);
    if (archive.empty()) return {};
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<char const*>(archive.data()), static_cast<std::streamsize>(archive.size()));
    return path;
}

// The external functions `units` define, sorted: each member of these pins defines one function of its own, so this
// is which members a search fetched.
[[nodiscard]] std::vector<std::string> functionsDefinedBy(std::vector<AssembledModule> const& units) {
    std::vector<std::string> names;
    for (auto const& m : units) {
        for (auto const& s : m.symbols) {
            if (s.binding == SymbolBinding::Local) continue;
            for (auto const& f : m.functions) {
                if (f.symbol == s.symbol) names.push_back(s.name);
            }
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

[[nodiscard]] std::vector<std::string> sorted(std::vector<std::string> names) {
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace

TEST(CommonSymbolsArchive, ADefinitionTheCommonOutranksDoesNotEndItsSearch) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "common-archive-outranked"};
    auto const dir = scratch.path();
    for (auto const& fam : searchFamilies()) {
        SCOPED_TRACE(fam.label);
        auto const L = load("x86_64", fam.relocatable);
        ASSERT_TRUE(L.target && L.format);
        auto const exec = ObjectFormatSchema::loadShipped(fam.exec);
        ASSERT_TRUE(exec.has_value()) << fam.exec;
        std::string const shared = fam.us + "shared", f = fam.us + "f", strong = fam.us + "m_strong",
                          mainName = fam.us + "main", callsF = fam.us + "calls_f";
        std::string const stem = fam.relocatable;
        DiagnosticReporter rep;
        // W: `f` beside a WEAK `shared` (3). S: a marker beside a strong `shared` (7).
        auto const memberW = fam.encode(UnitOf(1).marker(f).datum(shared, SymbolBinding::Weak, 3).build(), *L.target,
                                        *L.format, rep);
        auto const memberS = fam.encode(UnitOf(1).marker(strong).datum(shared, SymbolBinding::Global, 7).build(),
                                        *L.target, *L.format, rep);
        ASSERT_FALSE(memberW.empty() || memberS.empty()) << diagnosticsOf(rep);
        link::format::ArMemberInput const w{"w.o", memberW, {shared, f}};
        link::format::ArMemberInput const s{"s.o", memberS, {shared, strong}};

        // (1) An OBJECT that defines the name weak, linked beside the common's.
        {
            SCOPED_TRACE("a weak definition linked beside the common");
            std::vector<fs::path> const archives{archiveAt(dir / (stem + "-s.a"), {s}, rep)};
            ASSERT_FALSE(archives.front().empty()) << diagnosticsOf(rep);
            std::vector<AssembledModule> const clients{UnitOf(1).common(shared).reads(mainName, shared).build(),
                                                       UnitOf(2).datum(shared, SymbolBinding::Weak, 3).build()};
            DiagnosticReporter pullRep;
            auto const pulled = pullStaticArchiveMembers(clients, archives, {}, *L.target, **exec, pullRep);
            ASSERT_TRUE(pulled.has_value()) << diagnosticsOf(pullRep);
            bool const fetched = fam.commonOutranksAWeakDefinition && fam.fetchesForACommon;
            EXPECT_EQ(functionsDefinedBy(*pulled), fetched ? std::vector<std::string>{strong} : std::vector<std::string>{})
                << (fetched ? "the common outranks the weak definition, so it is still a common and S is fetched for it"
                            : "nothing is fetched: the weak definition replaced the common, or this family keeps one");
        }
        // (2) A MEMBER that defines it weak, pulled for a reference — in both orders of the archive's members and
        //     of the linked object's rows, which must not change what is fetched.
        for (bool weakMemberFirst : {true, false}) {
            for (bool commonRowFirst : {true, false}) {
                SCOPED_TRACE(std::string{weakMemberFirst ? "archive [W, S]" : "archive [S, W]"}
                             + (commonRowFirst ? ", the common's row first" : ", the reference's row first"));
                std::string const tag = std::string{weakMemberFirst ? "ws" : "sw"} + (commonRowFirst ? "c" : "r");
                std::vector<fs::path> const archives{archiveAt(
                    dir / (stem + "-" + tag + ".a"),
                    weakMemberFirst ? std::vector<link::format::ArMemberInput>{w, s}
                                    : std::vector<link::format::ArMemberInput>{s, w},
                    rep)};
                ASSERT_FALSE(archives.front().empty()) << diagnosticsOf(rep);
                UnitOf client(1);
                if (commonRowFirst) {
                    client.common(shared).reference(f, /*isData=*/false);
                } else {
                    client.reference(f, /*isData=*/false).common(shared);
                }
                client.reads(mainName, shared).calls(callsF, f);
                std::vector<AssembledModule> const clients{client.build()};
                DiagnosticReporter pullRep;
                auto const pulled = pullStaticArchiveMembers(clients, archives, {}, *L.target, **exec, pullRep);
                ASSERT_TRUE(pulled.has_value()) << diagnosticsOf(pullRep);
                bool const strongToo = fam.commonOutranksAWeakDefinition && fam.fetchesForACommon;
                EXPECT_EQ(functionsDefinedBy(*pulled),
                          strongToo ? sorted({f, strong}) : std::vector<std::string>{f})
                    << (strongToo ? "W for the reference, and S for the common W's weak definition did not replace"
                                  : "W for the reference alone: S is never fetched");
                // ...and the program the link then makes reads what the reference linker's reads.
                std::string_view const relocatable{fam.relocatable};
                if (relocatable.starts_with("pe")) continue;   // the native table runs the PE programs
                auto const clientObj = fam.encode(clients.front(), *L.target, *L.format, rep);
                ASSERT_FALSE(clientObj.empty()) << diagnosticsOf(rep);
                DiagnosticReporter linkRep;
                auto const image = dssLinkObject(dir, stem + "-" + tag + ".o", clientObj, archives,
                                                 (std::string{"x86_64:"} + fam.exec).c_str(), "", linkRep);
                ASSERT_FALSE(image.empty()) << diagnosticsOf(linkRep);
                auto const bytes = fileBytes(image);
                if (relocatable.starts_with("elf")) {
                    auto const read = mainReadIn(bytes);
                    ASSERT_TRUE(read.has_value());
                    EXPECT_EQ(read->section, ".data") << "S's strong definition wins";
                    EXPECT_EQ(read->value, 7u) << "GNU ld reads 7 in every one of these orders";
                } else {
                    auto const read = machoReadBy(bytes, mainName);
                    ASSERT_TRUE(read.has_value());
                    EXPECT_EQ(read->section, "__data") << "W's weak definition replaced the common";
                    EXPECT_EQ(read->value, 3u) << "Apple's ld reads the weak definition in every one of these orders";
                    EXPECT_FALSE(machoDefines(machoImage(bytes), strong)) << "S is in no order's image";
                }
            }
        }
    }
}

// The order of the search's two questions (the third measurement above): on Mach-O the member T, which defines the
// common T2 strong, is fetched exactly when it precedes U — whose weak T2 would otherwise have replaced the common
// once X's call of `u` was answered — in every order of the three members and of the object's two commons; `main`
// reads T's 7 then, and U's 5 otherwise. CONTROL: ELF, where a weak definition never ends a common's search, loads
// all three in every order and reads 7.
TEST(CommonSymbolsArchive, TheSearchAnswersItsReferencesBeforeItWalksItsCommons) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "common-archive-order"};
    auto const dir = scratch.path();
    for (auto const& fam : searchFamilies()) {
        std::string_view const relocatable{fam.relocatable};
        if (relocatable.starts_with("pe")) continue;   // a PE search fetches nothing for a common
        SCOPED_TRACE(fam.label);
        auto const L = load("x86_64", fam.relocatable);
        ASSERT_TRUE(L.target && L.format);
        auto const exec = ObjectFormatSchema::loadShipped(fam.exec);
        ASSERT_TRUE(exec.has_value()) << fam.exec;
        std::string const t1 = fam.us + "T1", t2 = fam.us + "T2", u = fam.us + "u", xCalls = fam.us + "x_calls_u",
                          mT = fam.us + "m_t", mainName = fam.us + "main", readsT1 = fam.us + "reads_t1";
        DiagnosticReporter rep;
        auto const objX = fam.encode(
            UnitOf(1).datum(t1, SymbolBinding::Global, 1).reference(u, /*isData=*/false).calls(xCalls, u).build(),
            *L.target, *L.format, rep);
        auto const objU =
            fam.encode(UnitOf(1).marker(u).datum(t2, SymbolBinding::Weak, 5).build(), *L.target, *L.format, rep);
        auto const objT =
            fam.encode(UnitOf(1).marker(mT).datum(t2, SymbolBinding::Global, 7).build(), *L.target, *L.format, rep);
        ASSERT_FALSE(objX.empty() || objU.empty() || objT.empty()) << diagnosticsOf(rep);
        std::unordered_map<char, link::format::ArMemberInput> const member{
            {'X', link::format::ArMemberInput{"x.o", objX, {t1, xCalls}}},
            {'U', link::format::ArMemberInput{"u.o", objU, {u, t2}}},
            {'T', link::format::ArMemberInput{"t.o", objT, {t2, mT}}}};
        std::string order = "TUX";
        do {
            for (bool t1First : {true, false}) {
                SCOPED_TRACE("archive [" + order + "], common " + (t1First ? "T1" : "T2") + " first");
                std::string const tag = order + (t1First ? "1" : "2");
                std::vector<link::format::ArMemberInput> members;
                for (char c : order) members.push_back(member.at(c));
                std::vector<fs::path> const archives{
                    archiveAt(dir / (std::string{fam.relocatable} + "-" + tag + ".a"), members, rep)};
                ASSERT_FALSE(archives.front().empty()) << diagnosticsOf(rep);
                UnitOf client(1);
                if (t1First) {
                    client.common(t1).common(t2);
                } else {
                    client.common(t2).common(t1);
                }
                client.reads(mainName, t2).reads(readsT1, t1);
                std::vector<AssembledModule> const clients{client.build()};
                DiagnosticReporter pullRep;
                auto const pulled = pullStaticArchiveMembers(clients, archives, {}, *L.target, **exec, pullRep);
                ASSERT_TRUE(pulled.has_value()) << diagnosticsOf(pullRep);
                bool const tFetched = fam.commonOutranksAWeakDefinition || order.find('T') < order.find('U');
                EXPECT_EQ(functionsDefinedBy(*pulled), tFetched ? sorted({xCalls, u, mT}) : sorted({xCalls, u}))
                    << (fam.commonOutranksAWeakDefinition
                            ? "CONTROL: a weak definition never ends a common's search here, so T is always fetched"
                            : "T is fetched exactly when it precedes U in the archive");
                auto const clientObj = fam.encode(clients.front(), *L.target, *L.format, rep);
                ASSERT_FALSE(clientObj.empty()) << diagnosticsOf(rep);
                DiagnosticReporter linkRep;
                auto const image = dssLinkObject(dir, std::string{fam.relocatable} + "-" + tag + ".o", clientObj,
                                                 archives, (std::string{"x86_64:"} + fam.exec).c_str(), "", linkRep);
                ASSERT_FALSE(image.empty()) << diagnosticsOf(linkRep);
                auto const    bytes = fileBytes(image);
                std::uint64_t value = 0;
                if (relocatable.starts_with("elf")) {
                    auto const read = mainReadIn(bytes);
                    ASSERT_TRUE(read.has_value());
                    value = read->value;
                } else {
                    auto const read = machoReadBy(bytes, mainName);
                    ASSERT_TRUE(read.has_value());
                    value = read->value;
                }
                EXPECT_EQ(value, tFetched ? 7u : 5u) << "T's strong 7 when T was fetched, U's weak 5 otherwise";
            }
        } while (std::next_permutation(order.begin(), order.end()));
    }
}

// The same question is open under a link document that does not say which definitions a common yields to when the
// weak definition is already LINKED and an archive member defines the name too: whether the name is still a common —
// so whether its search goes on — is the key's answer, and the search refuses by name rather than guess (naming the
// key and the member). CONTROLS: the same document beside a STRONG linked definition fetches nothing and says
// nothing; the shipped ELF document, which states the answer, fetches the member for the common the weak definition
// did not replace.
TEST(CommonSymbolsArchive, ASearchBesideALinkedWeakDefinitionUnderASilentDocumentIsRefusedByName) {
    auto const L = load("x86_64", "elf64-x86_64-linux");
    ASSERT_TRUE(L.target && L.format);
    auto const silent = elfExecDocumentWithoutCommonYieldsTo();
    ASSERT_TRUE(silent);
    auto const shipped = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(shipped.has_value());
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "common-archive-linked-weak"};
    auto const dir = scratch.path();
    DiagnosticReporter rep;
    auto const strongMember = elf::encode(memberDefiningShared("m_strong", SharedShape::Strong), *L.target, *L.format, rep);
    ASSERT_FALSE(strongMember.empty()) << diagnosticsOf(rep);
    std::vector<fs::path> const archives{archiveAt(
        dir / "libm_strong.a", {link::format::ArMemberInput{"m_strong.o", strongMember, {"shared", "m_strong"}}}, rep)};
    ASSERT_FALSE(archives.front().empty()) << diagnosticsOf(rep);
    std::vector<AssembledModule> const besideWeak{unitWithCommon(1, "main", 4, 4, /*entry=*/false),
                                                  unitDefiningShared(2, SymbolBinding::Weak)};
    std::vector<AssembledModule> const besideStrong{unitWithCommon(1, "main", 4, 4, /*entry=*/false),
                                                    unitDefiningShared(2, SymbolBinding::Global)};

    DiagnosticReporter refusedRep;
    auto const refused = pullStaticArchiveMembers(besideWeak, archives, {}, *L.target, *silent, refusedRep);
    EXPECT_FALSE(refused.has_value()) << "whether the name is still a common is the document's answer, which it lacks";
    EXPECT_TRUE(refusalNamesTheKey(refusedRep, "m_strong.o")) << diagnosticsOf(refusedRep);
    EXPECT_NE(diagnosticsOf(refusedRep).find("another linked unit defines it WEAK"), std::string::npos)
        << diagnosticsOf(refusedRep);

    DiagnosticReporter strongRep;
    auto const kept = pullStaticArchiveMembers(besideStrong, archives, {}, *L.target, *silent, strongRep);
    ASSERT_TRUE(kept.has_value()) << "CONTROL: a strong linked definition raises no question:" << diagnosticsOf(strongRep);
    EXPECT_TRUE(kept->empty()) << "CONTROL: the strong definition replaced the common, so nothing is fetched";
    EXPECT_FALSE(refusalNamesTheKey(strongRep));

    DiagnosticReporter statedRep;
    auto const fetched = pullStaticArchiveMembers(besideWeak, archives, {}, *L.target, **shipped, statedRep);
    ASSERT_TRUE(fetched.has_value()) << "CONTROL: the shipped document states the answer:" << diagnosticsOf(statedRep);
    EXPECT_EQ(functionsDefinedBy(*fetched), std::vector<std::string>{"m_strong"})
        << "CONTROL: the common outranks the linked weak definition, so the member is fetched for it";
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
// ✔MEASURED 2026-10-07 on both ISAs). DSS fetches the same
// member and refuses the same link BY NAME at the binding (`linker::reportThreadStorageDisagreements`,
// D-LK-THREAD-STORAGE-DISAGREEMENT-REFUSED-ONLY-BY-THE-WRITER-BACKSTOP) — before this round the ELF writer's backstop
// refused it naming SymbolIds. gcc writes the thread-local member, which DSS's relocatable writer
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
// A DSS main returns what `read_shared` reads. Eight cells: `m_weak` linked beside the common, four archives (their
// members in the order named), and — since send-back 5 — three in which a weak definition is already in the link when
// the common's search runs: `m_weak` linked directly against an archive of `m_strong`, and `m_weak_fn` (a weak
// definition beside a function `helper` the main calls, so the member is pulled for a REFERENCE) archived before and
// after `m_strong`. Each links under the host's reference linker — the CONTROL, which must answer what the rows'
// measurements recorded for it — and under DSS, which must answer the same:
//                          weak_    weak_   weak_then_  function_    function_  weak_direct_    weak_helper_  strong_then_
//                          direct   member  strong      then_strong  only       strong_member   then_strong   weak_helper
//   GNU ld (ELF)              0       0        7            7           0            7               7             7
//   Apple's ld (Mach-O)       5       5        5            7           0            5               5             5
//   link.exe (cl, PE)         5       0        0            0           0            5               5             5
//   GNU ld (MinGW gcc, PE)    0       0        0            0           0            0               0             0
// (✔MEASURED 2026-10-07 and, the last three columns, 2026-10-08 — GNU ld 2.42 on x86_64 and aarch64, Apple clang 21's
// ld-1267 and ld64-957.1 on arm64 and x86_64, cl 19.44 with link.exe 14.44 and lld-link 19.1.5, MinGW gcc 13.2.0 with
// GNU ld 2.42: GNU ld's common outranks a weak definition, which therefore never ends the common's search, and that
// search fetches only a global datum; Apple's ld lets a weak definition win, fetches one, and answers a reference
// before it walks its commons; link.exe lets cl's selectany win and fetches nothing for a common; a MinGW weak
// definition is a weak EXTERNAL, which the common satisfies.) On the Mac a THREAD-LOCAL member is linked too:
// Apple's ld links it for the common without a word, to a program that reads the TLV descriptor as the datum, and
// DSS must refuse it.
// ⓘ Every member's file name is at most 15 characters. lib.exe keeps a longer one in a longnames member, which DSS's
// archive reader does not read yet (D-FF1-AR-READER-REFUSES-THE-COFF-LONGNAMES-MEMBER, open: ✔MEASURED 2026-10-08, a
// 17-character `.obj` name refused this table's cl arm by name) — that refusal is its row's subject, not this table's.
namespace {

constexpr char const* kYieldMain       = "int read_shared(void);\nint main(void) { return read_shared(); }\n";
constexpr char const* kYieldMainHelper = "int read_shared(void);\nvoid helper(void);\n"
                                         "int main(void) { helper(); return read_shared(); }\n";
constexpr char const* kYieldCommon     = "int shared;\nint read_shared(void) { return shared; }\n";

struct YieldCell {
    char const*              name;
    std::vector<char const*> direct;     // objects linked beside `tu_common`
    std::vector<char const*> archived;   // one archive's members, in order; empty: no archive
    bool                     callsHelper = false;   // the main calls `helper`, which `m_weak_fn` defines
};

constexpr std::size_t kYieldCellCount = 8;

[[nodiscard]] std::vector<YieldCell> const& yieldCells() {
    static std::vector<YieldCell> const cells{{"weak_direct", {"m_weak"}, {}},
                                              {"weak_member", {}, {"m_weak"}},
                                              {"weak_then_strong", {}, {"m_weak", "m_strong"}},
                                              {"function_then_strong", {}, {"m_function", "m_strong"}},
                                              {"function_only", {}, {"m_function"}},
                                              {"weak_direct_strong_member", {"m_weak"}, {"m_strong"}},
                                              {"weak_helper_then_strong", {}, {"m_weak_fn", "m_strong"}, true},
                                              {"strong_then_weak_helper", {}, {"m_strong", "m_weak_fn"}, true}};
    return cells;
}

// One reference toolchain of this host, as the cells drive it. Every command runs in the arm's own directory and
// names its files relatively.
struct YieldArm {
    std::string                           label;
    std::string                           weakSpelling;   // how this compiler spells a WEAK definition
    std::string                           obj, lib, exe;  // its products' extensions
    char const*                           spec = nullptr; // the DSS image the cells link into
    std::array<unsigned, kYieldCellCount> expected{};     // per cell, in `yieldCells()` order
    bool                                  tlsCell = false;
    std::function<std::string(std::string const& stem, bool common)>                         compile;
    std::function<std::string(std::string const& lib, std::vector<std::string> const& members)> archive;
    // The program `out` of the main `mainSource` and `inputs`.
    std::function<std::string(std::string const& out, std::string const& mainSource,
                              std::vector<std::string> const& inputs)>
        link;
    std::function<bool(fs::path const& dir, std::string const& cmd, std::string const& log)> run;
};

// The reference toolchains this host's arms run on, and the ones it does not have — each with what was looked for.
struct YieldArms {
    std::vector<YieldArm>    arms;
    std::vector<std::string> absent;
};

[[nodiscard]] YieldArm gnuStyleArm(std::string label, std::string const& archFlags, char const* spec,
                                   std::array<unsigned, kYieldCellCount> expected, std::string exe, bool tlsCell) {
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
    a.link = [cc, ext = a.exe](std::string const& out, std::string const& mainSource,
                               std::vector<std::string> const& inputs) {
        std::string c = cc + " -O2 -o " + out + ext + " " + mainSource;
        for (auto const& i : inputs) c += " " + i;
        return c;
    };
    return a;
}

[[nodiscard]] YieldArms yieldArms(fs::path const& work) {
    YieldArms found;
    auto&     arms = found.arms;
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__)) || defined(__APPLE__)
    (void)work;
    auto const posixRun = [](fs::path const& dir, std::string const& cmd, std::string const& log) {
        namespace np = test_support::native_probe;
        return std::system(np::captureCmd("cd \"" + dir.string() + "\" && " + cmd, dir / log).c_str()) == 0;
    };
#if defined(__APPLE__)
    arms.push_back(gnuStyleArm("Apple clang -arch arm64 (Apple's ld)", "-arch arm64", "arm64:macho64-arm64-darwin-exec",
                               {5, 5, 5, 7, 0, 5, 5, 5}, "", true));
    arms.push_back(gnuStyleArm("Apple clang -arch x86_64 (Apple's ld)", "-arch x86_64",
                               "x86_64:macho64-x86_64-darwin-exec", {5, 5, 5, 7, 0, 5, 5, 5}, "", true));
#elif defined(__x86_64__)
    arms.push_back(gnuStyleArm("gcc (GNU ld, x86_64)", "", "x86_64:elf64-x86_64-linux-exec", {0, 0, 7, 7, 0, 7, 7, 7}, "",
                               false));
#else
    arms.push_back(gnuStyleArm("gcc (GNU ld, aarch64)", "", "arm64:elf64-aarch64-linux-exec", {0, 0, 7, 7, 0, 7, 7, 7},
                               "", false));
#endif
    for (auto& a : arms) a.run = posixRun;
#elif defined(_WIN32)
    namespace np = test_support::native_probe;
    auto const msvc = np::locateMsvcToolchain(work);
    // A toolchain that is absent is this host's fact, recorded for the caller to judge (a strict run fails on it);
    // one that is present and cannot be entered is a failure here and now.
    if (!msvc.toolAbsent() && !msvc.ok()) ADD_FAILURE() << msvc.describe();
    if (msvc.toolAbsent()) found.absent.push_back("cl (link.exe): " + msvc.detail);
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
        a.expected     = {5, 0, 0, 0, 0, 5, 5, 5};
        a.compile      = [](std::string const& stem, bool) { return "cl /nologo /c /O2 /MD " + stem + ".c"; };
        a.archive      = [](std::string const& lib, std::vector<std::string> const& members) {
            std::string c = "lib /nologo /OUT:" + lib + ".lib";
            for (auto const& m : members) c += " " + m + ".obj";
            return c;
        };
        a.link = [](std::string const& out, std::string const& mainSource, std::vector<std::string> const& inputs) {
            std::string c = "cl /nologo /O2 /MD " + mainSource;
            for (auto const& i : inputs) c += " " + i;
            return c + " /link /OUT:" + out + ".exe";
        };
        a.run = [tools](fs::path const& dir, std::string const& cmd, std::string const& log) {
            return runCapturing(tools.get(), dir, cmd, log);
        };
        arms.push_back(std::move(a));
    }
    if (std::system("where gcc >nul 2>&1") == 0) {
        auto a = gnuStyleArm("MinGW gcc (GNU ld, PE)", "", "x86_64:pe64-x86_64-windows-exec", {0, 0, 0, 0, 0, 0, 0, 0},
                             ".exe", false);
        a.run = [](fs::path const& dir, std::string const& cmd, std::string const& log) {
            return runCapturing(nullptr, dir, cmd, log);
        };
        arms.push_back(std::move(a));
    } else {
        found.absent.push_back("MinGW gcc (GNU ld, PE): no `gcc` on PATH");
    }
#else
    (void)work;
#endif
    return found;
}

}  // namespace

TEST(CommonSymbolsNative, ACommonBesideWeakAndArchivedDefinitionsResolvesAsTheReferenceLinkerDoes) {
    namespace np = test_support::native_probe;
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "common-yields"};
    auto const dir    = scratch.path();
    auto const found  = yieldArms(dir);
    auto const& arms  = found.arms;
    auto const strict = test_support::readStrictArmVerdicts();
    ASSERT_FALSE(strict.malformed) << test_support::kStrictArmVerdictsEnv << "='" << strict.raw
                                   << "' is not a recognised value";
    // A reference toolchain this host does not have is its arm NOT RUN — said, never dropped: a strict run (the
    // gate's) fails on it, as it fails on any arm the machine could not supply; any other run prints it.
    for (auto const& missing : found.absent) {
        if (strict.on) {
            ADD_FAILURE() << "[native-arm] " << missing << " — the arm cannot run on this host, and "
                          << test_support::kStrictArmVerdictsEnv << " makes that a failure";
        } else {
            std::cout << "[native-arm] " << missing << ": not run — no such toolchain on this host\n";
        }
    }
    if (arms.empty() && !HasFailure()) GTEST_SKIP() << "no reference C toolchain on this host (see the [native-arm] lines)";
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
        writeText(d / "main_helper.c", kYieldMainHelper);
        writeText(d / "tu_common.c", kYieldCommon);
        writeText(d / "m_weak.c", arm.weakSpelling + " int shared = 5;\n");
        writeText(d / "m_weak_fn.c", arm.weakSpelling + " int shared = 5;\nvoid helper(void) {}\n");
        writeText(d / "m_strong.c", "int shared = 7;\n");
        writeText(d / "m_function.c", "int shared(void) { return 9; }\n");
        std::vector<std::string> stems{"m_weak", "m_weak_fn", "m_strong", "m_function"};
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
        // DSS's link of the main `mainSource`, `tu_common` and `direct`, against `library` when it names one; its
        // image, or nullopt after recording why the link was refused.
        auto const dssLink = [&](std::string const& cell, std::string const& mainSource,
                                 std::vector<std::string> const& direct, std::string const& library,
                                 DiagnosticReporter& rep) -> std::optional<fs::path> {
            auto const out = d / ("dss_" + cell);
            fs::create_directories(out);
            Program p;
            p.setOutputDir(out);
            if (!library.empty()) p.setResolveLibraries(std::vector<fs::path>{d / library});
            std::vector<std::string> inputs{(d / mainSource).string(), (d / ("tu_common" + arm.obj)).string()};
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
            std::string const mainSource = cell.callsHelper ? "main_helper.c" : "main.c";
            std::vector<std::string> refInputs{"tu_common" + arm.obj};
            refInputs.insert(refInputs.end(), direct.begin(), direct.end());
            if (!library.empty()) refInputs.push_back(library);
            std::string const refStem = std::string{"ref_"} + cell.name;
            ASSERT_TRUE(arm.run(d, arm.link(refStem, mainSource, refInputs), refStem + ".txt"))
                << np::tailOf(d / (refStem + ".txt"), 20, arm.label);
            auto const ref = runImage(d / (refStem + arm.exe));
            ASSERT_TRUE(ref.spawned) << ref.diagnostic;
            EXPECT_EQ(ref.exitCode, arm.expected[c]) << "the reference linker no longer answers what the rows measured";
            DiagnosticReporter rep;
            auto const image = dssLink(cell.name, mainSource, direct, library, rep);
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
            auto const image = dssLink("tls_member", "main.c", {}, "lib_tls_member" + arm.lib, rep);
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

// ══ THE WEAK-NAME RULE (P69 send-back 5) ══════════════════════════════════════════════════════════════════════════
//
// D-LK-WEAK-NAME-REFERENCE-BOUND-TO-THE-BODY-NOT-THE-NAME. One body, two external names — `other` (strong) and
// `shared` (WEAK) — and a function of the same object that reaches the body THROUGH `shared`. Every reference linker
// resolves that reference BY NAME, to whatever wins `shared` in the link (✔MEASURED 2026-10-08: the pair then an
// object defining `shared` strong / that object then the pair / the pair alone; a function and a datum alike):
//     GNU ld 2.42 on gcc 13.3's ELF objects and on MinGW gcc 13.2's COFF objects (`weak, alias`)       9 / 9 / 7
//     link.exe 14.44 and lld-link 19.1.5 on clang 19.1.5's COFF objects, both Windows targets          9 / 9 / 7
//     Apple's ld (ld-1267 on arm64 and x86_64, ld64-957.1) on an assembler-written pair                9 / 9 / 7
//     DSS until this rule, ELF and PE                                                                   7 / 7 / 7
// The object readers bound the relocation to the BODY when they read the object, before any link knew the name's
// winner, and without a word. It is also the shape of EVERY MinGW `__attribute__((weak))` definition: gcc writes the
// body under a name of its own and the weak name as a weak external beside it. THE RULE: such a relocation KEEPS THE
// NAME — the reader states a plain reference row of it — and the link binds the row as it binds any other unit's.
//   * TWO WEAK NAMES OF ONE NAME (each object defines `shared` weak over a body of its own and reaches it): GNU ld
//     (ELF and PE) and Apple's ld give BOTH objects the FIRST object's body; link.exe ("conflicting weak extern
//     definition") and lld-link ("duplicate symbol") refuse the link. DECIDED: the first unit's body — what every
//     reference that links the shape answers; a refusal is not a meaning.
//   * BESIDE A COMMON of the name: GNU ld (ELF and PE), link.exe and lld-link read the common's zero from both units
//     (the common outranks the weak name); Apple's ld reads the weak definition's value from both.
//   * A RELOCATABLE LINK hands the name on: `ld -r` (GNU ld's ELF and PE linkers, Apple's ld) keeps the weak name and
//     its relocation still names it, a referencing unit's inside the artifact too; the artifact linked alone reads
//     the body, beside the override the override. (GNU ld's own PE `-r` output crashes when linked ALONE — it leaves
//     the weak external's default index unrenumbered — so PE has no reference for that one cell.)

namespace {

// ── How a relocatable object's own symbol table spells one NAME ──────────────
struct NameRecord {
    bool undefined = false;   // a reference: no section, no storage, no section-backed default
    bool weak      = false;   // a WEAK DEFINITION: ELF STB_WEAK in a section, Mach-O N_WEAK_DEF, a COFF weak external
                              // whose default lies in a section
    bool common    = false;
};

[[nodiscard]] std::vector<NameRecord> machoRecordsNamed(std::vector<std::uint8_t> const& b, std::string const& name) {
    std::vector<NameRecord> out;
    if (b.size() < 32 || rdLE(b, 0, 4) != 0xFEEDFACFu) return out;   // MH_MAGIC_64
    std::uint64_t const ncmds = rdLE(b, 16, 4);
    std::uint64_t       at    = 32;
    for (std::uint64_t i = 0; i < ncmds && at + 8 <= b.size(); ++i) {
        std::uint64_t const cmd = rdLE(b, at, 4), size = rdLE(b, at + 4, 4);
        if (cmd == 0x2u) {   // LC_SYMTAB: nlist_64 records of 16
            std::uint64_t const symoff = rdLE(b, at + 8, 4), nsyms = rdLE(b, at + 12, 4), stroff = rdLE(b, at + 16, 4);
            for (std::uint64_t k = 0; k < nsyms; ++k) {
                std::uint64_t const n = symoff + k * 16;
                if (cstrAt(b, stroff + rdLE(b, n, 4)) != name) continue;
                std::uint64_t const type = b[n + 4] & 0x0Eu, desc = rdLE(b, n + 6, 2), value = rdLE(b, n + 8, 8);
                NameRecord r;
                r.undefined = type == 0u && value == 0u;
                r.common    = type == 0u && value != 0u;
                r.weak      = type == 0x0Eu && (desc & 0x0080u) != 0u;   // N_SECT with N_WEAK_DEF
                out.push_back(r);
            }
        }
        if (size == 0) break;
        at += size;
    }
    return out;
}

[[nodiscard]] std::vector<NameRecord> coffRecordsNamed(std::vector<std::uint8_t> const& b, std::string const& name) {
    std::vector<NameRecord> out;
    if (b.size() < 20) return out;
    std::uint64_t const symtab = rdLE(b, 8, 4), nsyms = rdLE(b, 12, 4), strtab = symtab + nsyms * 18;
    auto const nameAt = [&](std::uint64_t rec) -> std::string {
        if (rdLE(b, rec, 4) == 0u) return cstrAt(b, strtab + rdLE(b, rec + 4, 4));
        std::string s;
        for (std::uint64_t k = 0; k < 8 && b[rec + k] != 0; ++k) s.push_back(static_cast<char>(b[rec + k]));
        return s;
    };
    auto const sectionOf = [&](std::uint64_t rec) { return static_cast<std::int16_t>(rdLE(b, rec + 12, 2)); };
    for (std::uint64_t i = 0; i < nsyms; ++i) {
        std::uint64_t const rec = symtab + i * 18;
        std::uint64_t const cls = b[rec + 16], aux = b[rec + 17];
        if ((cls == 2u || cls == 105u) && nameAt(rec) == name) {   // EXTERNAL, WEAK_EXTERNAL
            NameRecord r;
            if (cls == 105u) {
                // A weak external is a weak DEFINITION when its default (the auxiliary record's TagIndex) lies in
                // a section, and a weak REFERENCE otherwise.
                std::uint64_t const tag = aux != 0u ? rdLE(b, rec + 18, 4) : nsyms;
                r.weak      = tag < nsyms && sectionOf(symtab + tag * 18) > 0;
                r.undefined = !r.weak;
            } else {
                r.undefined = sectionOf(rec) == 0 && rdLE(b, rec + 8, 4) == 0u;
                r.common    = sectionOf(rec) == 0 && rdLE(b, rec + 8, 4) != 0u;
            }
            out.push_back(r);
        }
        i += aux;
    }
    return out;
}

// Every record of `name` in the symbol table of the relocatable object `b` of `fam`.
[[nodiscard]] std::vector<NameRecord> recordsNamed(SearchFamily const& fam, std::vector<std::uint8_t> const& b,
                                                   std::string const& name) {
    std::string_view const fmt{fam.relocatable};
    if (fmt.starts_with("macho")) return machoRecordsNamed(b, name);
    if (fmt.starts_with("pe")) return coffRecordsNamed(b, name);
    std::vector<NameRecord> out;
    for (auto const& s : elfSymbolsNamed(b, name)) {
        NameRecord r;
        r.undefined = s.shndx == 0;
        r.common    = s.shndx == kShnCommon;
        r.weak      = s.bind == 2u && !r.undefined && !r.common;   // STB_WEAK
        out.push_back(r);
    }
    return out;
}

[[nodiscard]] std::optional<AssembledModule> readObject(SearchFamily const& fam, Loaded const& L,
                                                        std::vector<std::uint8_t> const& bytes, std::uint32_t cu,
                                                        DiagnosticReporter& rep) {
    std::string_view const fmt{fam.relocatable};
    if (fmt.starts_with("macho")) {
        return macho::readRelocatableObject(bytes, *L.target, *L.format, rep, CompilationUnitId{cu});
    }
    if (fmt.starts_with("pe")) return pe::readRelocatableObject(bytes, *L.target, *L.format, rep, CompilationUnitId{cu});
    return elf::readRelocatableObject(bytes, *L.target, *L.format, rep, CompilationUnitId{cu});
}

[[nodiscard]] ModuleSymbol const* definitionNamed(AssembledModule const& m, std::string const& name) {
    for (auto const& s : m.symbols) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

// The targets of the relocations of the function `m` defines as `fn`.
[[nodiscard]] std::vector<SymbolId> targetsOf(AssembledModule const& m, std::string const& fn) {
    std::vector<SymbolId> out;
    auto const*           s = definitionNamed(m, fn);
    if (s == nullptr) return out;
    for (auto const& f : m.functions) {
        if (f.symbol != s->symbol) continue;
        for (auto const& r : f.relocations) out.push_back(r.target);
    }
    return out;
}

// THE PAIR, as a reader states it and as each family's writer is given it: one body — a datum of `value`, or a
// function — under the strong name `strong` and the WEAK second name `weak`; `viaWeak` reaches it THROUGH the weak
// name (a plain reference row of the name) and, when named, `viaStrong` through the strong one.
[[nodiscard]] AssembledModule weakNamePair(std::uint32_t cu, bool datum, std::uint8_t value, std::string const& strong,
                                           std::string const& weak, std::string const& viaWeak,
                                           std::string const& viaStrong = {}) {
    UnitOf u(cu);
    if (datum) {
        u.datum(strong, SymbolBinding::Global, value);
    } else {
        u.marker(strong);
    }
    u.alias(weak, strong, SymbolBinding::Weak);
    if (!viaStrong.empty()) {
        if (datum) {
            u.reads(viaStrong, strong);
        } else {
            u.calls(viaStrong, strong);
        }
    }
    u.reference(weak, datum);
    if (datum) {
        u.reads(viaWeak, weak);
    } else {
        u.calls(viaWeak, weak);
    }
    return u.build();
}

// A unit that DEFINES `name` strong: a datum of `value`, or a function.
[[nodiscard]] AssembledModule definitionOf(std::uint32_t cu, bool datum, std::uint8_t value, std::string const& name) {
    UnitOf u(cu);
    if (datum) {
        u.datum(name, SymbolBinding::Global, value);
    } else {
        u.marker(name);
    }
    return u.build();
}

// A unit that only REFERS to `name`, from its function `via`.
[[nodiscard]] AssembledModule clientOf(std::uint32_t cu, bool datum, std::string const& name, std::string const& via) {
    UnitOf u(cu);
    u.reference(name, datum);
    if (datum) {
        u.reads(via, name);
    } else {
        u.calls(via, name);
    }
    return u.build();
}

// `hand` as the family's WRITER writes it and its READER reads it back, as unit `cu` — what a link is really given.
[[nodiscard]] std::optional<AssembledModule> asRead(SearchFamily const& fam, Loaded const& L, AssembledModule const& hand,
                                                    std::uint32_t cu) {
    DiagnosticReporter rep;
    auto const         bytes = fam.encode(hand, *L.target, *L.format, rep);
    EXPECT_FALSE(bytes.empty()) << diagnosticsOf(rep);
    EXPECT_EQ(rep.errorCount(), 0u) << diagnosticsOf(rep);
    if (bytes.empty()) return std::nullopt;
    auto back = readObject(fam, L, bytes, cu, rep);
    EXPECT_TRUE(back.has_value()) << diagnosticsOf(rep);
    return back;
}

[[nodiscard]] std::string keyText(LinkedSymbolKey const& k) {
    return std::to_string(k.cuId.v) + ":" + std::to_string(k.symbol.v);
}

// "cu:symbol" of `unit`'s definition `name` — a symbol row or, for a common, its row.
[[nodiscard]] std::string keyOfDefinition(AssembledModule const& unit, std::string const& name) {
    if (auto const* s = definitionNamed(unit, name)) return keyText(LinkedSymbolKey{unit.cuId, s->symbol});
    if (auto const* r = rowNamed(unit, name); r != nullptr && r->commonSize != 0u) {
        return keyText(LinkedSymbolKey{unit.cuId, r->symbol});
    }
    return "(no definition of " + name + ")";
}

// What the image link `img` bound the ONE relocation of `unit`'s function `fn` to: "cu:symbol" of the definition the
// link's own resolution record names for it — or of the relocation's own target, which the unit then holds itself.
[[nodiscard]] std::string boundTo(LinkedImage const& img, AssembledModule const& unit, std::string const& fn) {
    auto const targets = targetsOf(unit, fn);
    if (targets.size() != 1u) return "(" + fn + " has " + std::to_string(targets.size()) + " relocations)";
    for (auto const& ref : img.resolvedCrossCuRefs) {
        if (ref.reference.cuId.v == unit.cuId.v && ref.reference.symbol.v == targets[0].v) return keyText(ref.definition);
    }
    return keyText(LinkedSymbolKey{unit.cuId, targets[0]});
}

// The 4-byte value `fn`'s read reaches in an x86_64 image of `fam` — where this file can read an image (ELF and
// Mach-O; a PE image carries no symbol to find `fn` by).
[[nodiscard]] std::optional<std::uint64_t> valueReadBy(SearchFamily const& fam, std::vector<std::uint8_t> const& img,
                                                       std::string const& fn) {
    std::string_view const fmt{fam.relocatable};
    if (fmt.starts_with("pe")) return std::nullopt;
    if (fmt.starts_with("macho")) {
        auto const r = machoReadBy(img, fn);
        if (!r.has_value()) return std::nullopt;
        return r->value;
    }
    auto const at = datumReadBy(img, fn);
    if (!at.has_value()) return std::nullopt;
    auto const sec = sectionHolding(img, *at);
    if (!sec.has_value()) return std::nullopt;
    if (sec->name == ".bss") return std::uint64_t{0};
    return rdLE(img, sec->offset + (*at - sec->addr), 4);
}

}  // namespace

// THE WRITER'S ARM AND THE READER'S, per family. The pair is written by the family's object writer and read back by
// its reader. The object spells `shared` ONCE, as the weak definition — a second, undefined record of the name is
// what a writer with no arm for the reference row would add — and the reader states ONE plain reference row of it,
// under an id that is not the body's, which the function written through the name targets. CONTROL, in the same
// object: a function that reaches the body through the STRONG name targets the body, and no row of that name exists.
TEST(WeakNameReferences, EachWriterKeepsTheNameAndEachReaderStatesItsReference) {
    for (auto const& fam : searchFamilies()) {
        for (bool const datum : {true, false}) {
            SCOPED_TRACE(std::string{fam.label} + (datum ? ", a datum" : ", a function"));
            auto const L = load("x86_64", fam.relocatable);
            ASSERT_TRUE(L.target && L.format);
            std::string const other = fam.us + "other", shared = fam.us + "shared";
            auto const hand = weakNamePair(1, datum, 7, other, shared, fam.us + "via_shared", fam.us + "via_other");
            DiagnosticReporter rep;
            auto const bytes = fam.encode(hand, *L.target, *L.format, rep);
            ASSERT_FALSE(bytes.empty()) << diagnosticsOf(rep);
            EXPECT_EQ(rep.errorCount(), 0u) << diagnosticsOf(rep);

            auto const records = recordsNamed(fam, bytes, shared);
            ASSERT_EQ(records.size(), 1u) << "the object's symbol table must spell the weak name ONCE: the reference "
                                             "written through it names the weak definition's own record";
            EXPECT_TRUE(records[0].weak) << "the one record is the weak DEFINITION";
            EXPECT_FALSE(records[0].undefined);
            EXPECT_FALSE(records[0].common);

            auto const back = readObject(fam, L, bytes, 1, rep);
            ASSERT_TRUE(back.has_value()) << diagnosticsOf(rep);
            auto const* strongRow = definitionNamed(*back, other);
            auto const* weakRow   = definitionNamed(*back, shared);
            ASSERT_TRUE(strongRow != nullptr && weakRow != nullptr) << "both names are still DEFINED";
            EXPECT_EQ(strongRow->symbol, weakRow->symbol) << "two names of ONE body";
            EXPECT_EQ(strongRow->binding, SymbolBinding::Global);
            EXPECT_EQ(weakRow->binding, SymbolBinding::Weak);
            std::vector<ExternImport const*> rows;
            for (auto const& e : back->externImports) {
                if (e.mangledName == shared) rows.push_back(&e);
            }
            ASSERT_EQ(rows.size(), 1u) << "the weak name states ONE reference row";
            ExternImport const& row = *rows[0];
            EXPECT_NE(row.symbol, weakRow->symbol) << "the row's id is the NAME's, which no body holds";
            EXPECT_EQ(row.isData, datum) << "the row states what the body it names is";
            EXPECT_FALSE(row.isThreadLocal);
            EXPECT_EQ(row.binding, SymbolBinding::Global);
            EXPECT_TRUE(row.libraryPath.empty());
            EXPECT_TRUE(row.fallbackName.empty());
            EXPECT_EQ(row.commonSize, 0u) << "a plain reference, never a common";
            EXPECT_EQ(targetsOf(*back, fam.us + "via_shared"), std::vector<SymbolId>{row.symbol})
                << "the reference written through the weak name KEEPS THE NAME";
            EXPECT_EQ(targetsOf(*back, fam.us + "via_other"), std::vector<SymbolId>{strongRow->symbol})
                << "CONTROL: a reference written through the STRONG name binds to the body";
            EXPECT_EQ(rowNamed(*back, other), nullptr) << "CONTROL: a strong name states no reference row";
        }
    }
}

// AN IMAGE BINDS THE NAME TO ITS WINNER. Each unit is what its family's reader reads from its own object. Per cell,
// every function written through `shared` must have been bound — in the link's own resolution record, and for a
// datum in the ELF and Mach-O images' bytes — to the definition that wins the name:
//   * the pair ALONE (ONE unit, which names a definition of its own: the link resolves it like any other): its body;
//   * beside a STRONG definition, either order: that definition (9);
//   * beside a second pair defining the name weak too: the FIRST unit's body, for both units (the decision above);
//   * beside a COMMON of the name: the common where it outranks the weak name (ELF; PE, whose weak external yields
//     to a common), the weak definition where it replaces the common (Mach-O).
TEST(WeakNameReferences, AnImageBindsAReferenceThroughAWeakNameToTheNamesWinner) {
    enum class U { Pair, PairB, Override, Common };
    struct Cell {
        char const*    name;
        std::vector<U> units;    // in link order
        U              winner;   // the unit whose definition every reference through `shared` reaches
        std::uint64_t  value;    // what a read through `shared` reads
        bool           datumOnly = false;
    };
    for (auto const& fam : searchFamilies()) {
        bool const commonWins = std::string_view{fam.label} != "Mach-O";
        std::vector<Cell> const cells{
            {"the pair alone", {U::Pair}, U::Pair, 7},
            {"the pair, then a strong definition", {U::Pair, U::Override}, U::Override, 9},
            {"a strong definition, then the pair", {U::Override, U::Pair}, U::Override, 9},
            {"two weak names of the name, this pair first", {U::Pair, U::PairB}, U::Pair, 7},
            {"two weak names of the name, the other pair first", {U::PairB, U::Pair}, U::PairB, 22},
            {"the pair, then a common", {U::Pair, U::Common}, commonWins ? U::Common : U::Pair, commonWins ? 0u : 7u, true},
            {"a common, then the pair", {U::Common, U::Pair}, commonWins ? U::Common : U::Pair, commonWins ? 0u : 7u,
             true}};
        auto const L = load("x86_64", fam.relocatable);
        auto const X = load("x86_64", fam.exec);
        ASSERT_TRUE(L.target && L.format && X.format);
        std::string const shared = fam.us + "shared";
        for (bool const datum : {true, false}) {
            for (auto const& cell : cells) {
                if (cell.datumOnly && !datum) continue;
                SCOPED_TRACE(std::string{fam.label} + (datum ? ", a datum: " : ", a function: ") + cell.name);
                std::vector<AssembledModule> mods;
                std::vector<std::string>     user;      // per unit: its function that reaches `shared`, or empty
                std::vector<std::string>     defines;   // per unit: its own definition's name
                for (std::size_t i = 0; i < cell.units.size(); ++i) {
                    auto const      cu = static_cast<std::uint32_t>(i + 1);
                    AssembledModule hand;
                    switch (cell.units[i]) {
                    case U::Pair:
                        hand = weakNamePair(cu, datum, 7, fam.us + "other", shared, fam.us + "via_shared");
                        user.push_back(fam.us + "via_shared");
                        defines.push_back(fam.us + "other");
                        break;
                    case U::PairB:
                        hand = weakNamePair(cu, datum, 22, fam.us + "other_b", shared, fam.us + "via_shared_b");
                        user.push_back(fam.us + "via_shared_b");
                        defines.push_back(fam.us + "other_b");
                        break;
                    case U::Override:
                        hand = definitionOf(cu, datum, 9, shared);
                        user.emplace_back();
                        defines.push_back(shared);
                        break;
                    case U::Common:
                        hand = UnitOf(cu).common(shared).reads(fam.us + "via_common", shared).build();
                        user.push_back(fam.us + "via_common");
                        defines.push_back(shared);
                        break;
                    }
                    auto read = asRead(fam, L, hand, cu);
                    ASSERT_TRUE(read.has_value());
                    mods.push_back(std::move(*read));
                }
                // The image's entry: the first unit's function that reaches the name.
                for (std::size_t i = 0; i < mods.size(); ++i) {
                    if (user[i].empty()) continue;
                    auto const* fn = definitionNamed(mods[i], user[i]);
                    ASSERT_NE(fn, nullptr);
                    mods[i].userEntrySymbol = fn->symbol;
                    break;
                }
                DiagnosticReporter rep;
                auto const img = linker::link(std::span<AssembledModule const>{mods}, *L.target, *X.format, rep,
                                              ImageRequest{.artifactFileName = "weak_name_image"});
                ASSERT_TRUE(img.ok()) << diagnosticsOf(rep);
                std::size_t winner = 0;
                for (std::size_t i = 0; i < cell.units.size(); ++i) {
                    if (cell.units[i] == cell.winner) winner = i;
                }
                std::string const expected = keyOfDefinition(mods[winner], defines[winner]);
                for (std::size_t i = 0; i < mods.size(); ++i) {
                    if (user[i].empty()) continue;
                    EXPECT_EQ(boundTo(img, mods[i], user[i]), expected)
                        << user[i] << " must reach the definition that WINS the name (cu:symbol)";
                    if (!datum || std::string_view{fam.label} == "PE") continue;
                    auto const value = valueReadBy(fam, img.bytes, user[i]);
                    ASSERT_TRUE(value.has_value()) << user[i] << ": the image's read could not be followed";
                    EXPECT_EQ(*value, cell.value) << user[i] << " reads the winner's own bytes";
                }
            }
        }
    }
}

// A RELOCATABLE LINK HANDS THE NAME ON, AND THE LINK AFTER IT BINDS IT — what `ld -r` does (the measurements above).
// Per family, each unit read from its own object:
//   (1) the pair alone, and the pair with a CLIENT unit that only refers to the name: the artifact still spells
//       `shared` once, WEAK, beside the strong name on one body, and every reference to it — the client's too —
//       still names it (one plain reference row). Linked into an image alone the references reach the body; beside
//       a strong definition, in either order, they reach that definition.
//   (2) the pair with a strong definition INSIDE the artifact, either order: the name is that definition's, strong,
//       spelled once; the pair's reference is bound to it; the body keeps its strong name.
//   (3) the pair with a COMMON of the name, either order: where the common outranks the weak name (ELF; PE's weak
//       external) the artifact hands the COMMON on and the weak name is gone; where the weak definition replaces
//       the common (Mach-O) the artifact holds the weak definition and no common. Both units' reads name it.
TEST(WeakNameReferences, ARelocatableLinkHandsTheNameOnAndALaterLinkBindsIt) {
    for (auto const& fam : searchFamilies()) {
        bool const commonWins = std::string_view{fam.label} != "Mach-O";
        auto const L = load("x86_64", fam.relocatable);
        auto const X = load("x86_64", fam.exec);
        ASSERT_TRUE(L.target && L.format && X.format);
        for (bool const datum : {true, false}) {
            SCOPED_TRACE(std::string{fam.label} + (datum ? ", a datum" : ", a function"));
            std::string const other = fam.us + "other", shared = fam.us + "shared", viaShared = fam.us + "via_shared",
                              viaClient = fam.us + "via_client", viaCommon = fam.us + "via_common";
            auto const pairUnit = [&](std::uint32_t cu) {
                return asRead(fam, L, weakNamePair(cu, datum, 7, other, shared, viaShared), cu);
            };
            auto const clientUnit   = [&](std::uint32_t cu) { return asRead(fam, L, clientOf(cu, datum, shared, viaClient), cu); };
            auto const overrideUnit = [&](std::uint32_t cu) { return asRead(fam, L, definitionOf(cu, datum, 9, shared), cu); };
            auto const commonUnit   = [&](std::uint32_t cu) {
                return asRead(fam, L, UnitOf(cu).common(shared).reads(viaCommon, shared).build(), cu);
            };
            // The relocatable link of `units`: the artifact's bytes, or nullopt after recording why there are none.
            auto const artifactOf = [&](std::vector<std::optional<AssembledModule>> units,
                                        char const* what) -> std::optional<std::vector<std::uint8_t>> {
                std::vector<AssembledModule> mods;
                for (auto& u : units) {
                    EXPECT_TRUE(u.has_value()) << what;
                    if (!u.has_value()) return std::nullopt;
                    mods.push_back(std::move(*u));
                }
                DiagnosticReporter rep;
                auto const obj = linker::link(std::span<AssembledModule const>{mods}, *L.target, *L.format, rep);
                EXPECT_TRUE(obj.ok()) << what << ":" << diagnosticsOf(rep);
                if (!obj.ok()) return std::nullopt;
                return obj.bytes;
            };
            auto const symbolRowsNamed = [](AssembledModule const& m, std::string const& name) {
                std::size_t n = 0;
                for (auto const& s : m.symbols) n += s.name == name ? 1u : 0u;
                return n;
            };

            // (1) the name handed on.
            for (bool const withClient : {false, true}) {
                SCOPED_TRACE(withClient ? "(1) the pair and a client unit" : "(1) the pair alone");
                auto const bytes = withClient ? artifactOf({pairUnit(1), clientUnit(2)}, "the artifact")
                                              : artifactOf({pairUnit(1)}, "the artifact");
                ASSERT_TRUE(bytes.has_value());
                auto const records = recordsNamed(fam, *bytes, shared);
                ASSERT_EQ(records.size(), 1u) << "the artifact spells the weak name ONCE";
                EXPECT_TRUE(records[0].weak) << "and it is still the WEAK definition: a later link may override it";
                EXPECT_FALSE(records[0].undefined);
                EXPECT_FALSE(records[0].common);
                // What a later link makes of it, the artifact as its unit `artifactCu` beside `beside`.
                for (int order = 0; order < 3; ++order) {
                    SCOPED_TRACE(order == 0 ? "linked alone" : order == 1 ? "then a strong definition"
                                                                         : "after a strong definition");
                    DiagnosticReporter rep;
                    auto artifact = readObject(fam, L, *bytes, order == 2 ? 2u : 1u, rep);
                    ASSERT_TRUE(artifact.has_value()) << diagnosticsOf(rep);
                    auto const* strongRow = definitionNamed(*artifact, other);
                    auto const* weakRow   = definitionNamed(*artifact, shared);
                    ASSERT_TRUE(strongRow != nullptr && weakRow != nullptr) << "both names are still DEFINED";
                    EXPECT_EQ(strongRow->symbol, weakRow->symbol) << "two names of ONE body";
                    EXPECT_EQ(weakRow->binding, SymbolBinding::Weak);
                    auto const* row = rowNamed(*artifact, shared);
                    ASSERT_NE(row, nullptr) << "the reference through the weak name is handed on BY NAME";
                    EXPECT_EQ(row->commonSize, 0u);
                    EXPECT_EQ(targetsOf(*artifact, viaShared), std::vector<SymbolId>{row->symbol})
                        << "the pair's own reference still names the weak name";
                    if (withClient) {
                        EXPECT_EQ(targetsOf(*artifact, viaClient), std::vector<SymbolId>{row->symbol})
                            << "and so does the client unit's, inside the artifact";
                    }
                    auto const* entry = definitionNamed(*artifact, viaShared);
                    ASSERT_NE(entry, nullptr);
                    artifact->userEntrySymbol = entry->symbol;
                    std::vector<AssembledModule> mods;
                    std::string                  expected = keyOfDefinition(*artifact, other);
                    std::uint64_t                value    = 7;
                    if (order == 0) {
                        mods.push_back(*artifact);
                    } else {
                        auto strong = overrideUnit(order == 1 ? 2u : 1u);
                        ASSERT_TRUE(strong.has_value());
                        expected = keyOfDefinition(*strong, shared);
                        value    = 9;
                        if (order == 1) {
                            mods.push_back(*artifact);
                            mods.push_back(std::move(*strong));
                        } else {
                            mods.push_back(std::move(*strong));
                            mods.push_back(*artifact);
                        }
                    }
                    auto const img = linker::link(std::span<AssembledModule const>{mods}, *L.target, *X.format, rep,
                                                  ImageRequest{.artifactFileName = "weak_name_image"});
                    ASSERT_TRUE(img.ok()) << diagnosticsOf(rep);
                    for (auto const& fn : {viaShared, viaClient}) {
                        if (fn == viaClient && !withClient) continue;
                        EXPECT_EQ(boundTo(img, *artifact, fn), expected) << fn << " must reach the name's winner";
                        if (!datum || std::string_view{fam.label} == "PE") continue;
                        auto const read = valueReadBy(fam, img.bytes, fn);
                        ASSERT_TRUE(read.has_value()) << fn << ": the image's read could not be followed";
                        EXPECT_EQ(*read, value) << fn;
                    }
                }
            }

            // (2) a strong definition inside the artifact.
            for (bool const pairFirst : {true, false}) {
                SCOPED_TRACE(pairFirst ? "(2) the pair, then a strong definition" : "(2) a strong definition, then the pair");
                auto const bytes = pairFirst ? artifactOf({pairUnit(1), overrideUnit(2)}, "the artifact")
                                             : artifactOf({overrideUnit(1), pairUnit(2)}, "the artifact");
                ASSERT_TRUE(bytes.has_value());
                auto const records = recordsNamed(fam, *bytes, shared);
                ASSERT_EQ(records.size(), 1u) << "one object, one definition of the name";
                EXPECT_FALSE(records[0].weak) << "the STRONG definition won the name";
                EXPECT_FALSE(records[0].undefined);
                EXPECT_FALSE(records[0].common);
                DiagnosticReporter rep;
                auto const artifact = readObject(fam, L, *bytes, 1, rep);
                ASSERT_TRUE(artifact.has_value()) << diagnosticsOf(rep);
                EXPECT_EQ(symbolRowsNamed(*artifact, shared), 1u);
                auto const* def = definitionNamed(*artifact, shared);
                ASSERT_NE(def, nullptr);
                EXPECT_EQ(def->binding, SymbolBinding::Global);
                EXPECT_EQ(rowNamed(*artifact, shared), nullptr) << "the reference was bound inside the artifact";
                EXPECT_EQ(targetsOf(*artifact, viaShared), std::vector<SymbolId>{def->symbol})
                    << "the pair's reference through the weak name reaches the STRONG definition";
                auto const* strongRow = definitionNamed(*artifact, other);
                ASSERT_NE(strongRow, nullptr) << "the body keeps its strong name";
                EXPECT_NE(strongRow->symbol, def->symbol) << "and is not the definition that won";
                if (!datum) continue;
                // (At least 4 bytes each: a reader that takes an atom's end from the next boundary gives a section's
                // last datum the section's own padding too.)
                std::size_t seen = 0;
                for (auto const& d : artifact->dataItems) {
                    if (d.symbol == def->symbol) {
                        ASSERT_GE(d.bytes.size(), 4u);
                        EXPECT_EQ(d.bytes[0], 9u) << "the strong definition's own bytes";
                        ++seen;
                    }
                    if (d.symbol == strongRow->symbol) {
                        ASSERT_GE(d.bytes.size(), 4u);
                        EXPECT_EQ(d.bytes[0], 7u) << "the body's own bytes, under its strong name";
                        ++seen;
                    }
                }
                EXPECT_EQ(seen, 2u) << "both data are in the artifact";
            }

            // (3) a common of the name inside the artifact.
            if (!datum) continue;
            for (bool const pairFirst : {true, false}) {
                SCOPED_TRACE(pairFirst ? "(3) the pair, then a common" : "(3) a common, then the pair");
                auto const bytes = pairFirst ? artifactOf({pairUnit(1), commonUnit(2)}, "the artifact")
                                             : artifactOf({commonUnit(1), pairUnit(2)}, "the artifact");
                ASSERT_TRUE(bytes.has_value());
                auto const records = recordsNamed(fam, *bytes, shared);
                ASSERT_EQ(records.size(), 1u) << "one object cannot carry a common and a definition of one name";
                DiagnosticReporter rep;
                auto const artifact = readObject(fam, L, *bytes, 1, rep);
                ASSERT_TRUE(artifact.has_value()) << diagnosticsOf(rep);
                auto const* row = rowNamed(*artifact, shared);
                ASSERT_NE(row, nullptr);
                EXPECT_EQ(targetsOf(*artifact, viaShared), std::vector<SymbolId>{row->symbol})
                    << "the pair's read names what the name is now";
                EXPECT_EQ(targetsOf(*artifact, viaCommon), std::vector<SymbolId>{row->symbol})
                    << "and so does the common unit's";
                ASSERT_NE(definitionNamed(*artifact, other), nullptr) << "the body keeps its strong name";
                if (commonWins) {
                    EXPECT_TRUE(records[0].common) << "the common outranks the weak name: the artifact hands the COMMON on";
                    EXPECT_EQ(row->commonSize, 4u);
                    EXPECT_EQ(definitionNamed(*artifact, shared), nullptr) << "the weak name gave the name up";
                } else {
                    EXPECT_TRUE(records[0].weak) << "the weak definition replaces the common";
                    EXPECT_EQ(row->commonSize, 0u);
                    auto const* weakRow = definitionNamed(*artifact, shared);
                    ASSERT_NE(weakRow, nullptr);
                    EXPECT_EQ(weakRow->binding, SymbolBinding::Weak);
                }
            }
        }
    }
}

// ══ Native, every host: a reference written through a weak name, under the host's reference linkers and under DSS ══
//
// The host's reference compiler writes each object; a main returns what the object's own function reaches through
// the weak name. Every cell links under the reference linker — the CONTROL, which must answer what the row's
// measurements recorded for it — and under DSS (its own main, the reference compiler's objects), which must answer
// the same. Where the reference REFUSES a cell (link.exe and lld-link, two weak definitions of one name) it must
// still refuse it, and DSS answers the recorded decision: the first unit's body.
//                                                GNU ld    Apple's ld    GNU ld            link.exe, lld-link
//                                                (ELF)     (Mach-O)      (MinGW gcc, PE)   (clang, PE)
//   the pair then the override / reversed          9          9             9                 9
//   the pair alone                                 7          7             7                 7
//   two weak names of one name, a-b / b-a       22 / 44    22 / 44       22 / 44           refused (DSS 22 / 44)
//   the pair beside a common, either order         0         77             0                 0
//   a weak DEFINITION then the override / rev.     2          2             2                 2
//   a weak definition alone                        1          1             1                 1
//   two weak definitions, 1-2 / 2-1             22 / 44    22 / 44       22 / 44           refused (DSS 22 / 44)
//   the pair's artifact (DSS-written) alone        7          7             7                 7
//   the artifact beside the override, either       9          9             9                 9
// (function and datum pairs alike; ✔MEASURED 2026-10-08: GNU ld 2.42 with gcc 13.3 and with MinGW gcc 13.2, Apple
// clang 21's ld-1267 and ld64-957.1, clang 19.1.5 with link.exe 14.44 and lld-link 19.1.5.) The artifact cells link
// the relocatable object DSS writes of the pair's object — under DSS and under the reference final linker.
// ⓘ Apple clang refuses `__attribute__((alias))`, so on a Mac each pair is ASSEMBLER-written: one address under a
// strong external name and a `.weak_definition` name, reached by a branch or, for the datum, through its GOT entry
// (the access a compiler writes for a weak definition).

namespace {

// A body under a strong name and a WEAK second name, and the function that reaches it through the weak one.
struct WnPair {
    char const* stem;
    bool        datum;
    char const* strong;
    char const* weak;
    unsigned    value;
    char const* user;
};

[[nodiscard]] std::vector<WnPair> const& wnPairs() {
    static std::vector<WnPair> const pairs{{"ao", false, "other", "shared", 7, "call_shared"},
                                           {"ad", true, "other_d", "shared_d", 7, "read_shared_d"},
                                           {"a1", false, "other_a", "shared", 11, "call_a"},
                                           {"a2", false, "other_b", "shared", 22, "call_b"},
                                           {"d1", true, "other_da", "shared_d", 11, "read_a"},
                                           {"d2", true, "other_db", "shared_d", 22, "read_b"}};
    return pairs;
}

[[nodiscard]] std::string cPair(WnPair const& p) {
    std::string const strong{p.strong}, weak{p.weak}, user{p.user}, value = std::to_string(p.value);
    if (p.datum) {
        return "int " + strong + " = " + value + ";\nextern int " + weak + " __attribute__((weak, alias(\"" + strong
               + "\")));\nint " + user + "(void) { return " + weak + "; }\n";
    }
    return "int " + strong + "(void) { return " + value + "; }\nint " + weak + "(void) __attribute__((weak, alias(\""
           + strong + "\")));\nint " + user + "(void) { return " + weak + "(); }\n";
}

[[nodiscard]] std::string applePair(std::string const& arch, WnPair const& p) {
    bool const        arm = arch == "arm64";
    std::string const strong{p.strong}, weak{p.weak}, user{p.user}, value = std::to_string(p.value);
    std::string       s;
    if (p.datum) {
        s += "\t.data\n\t.globl _" + strong + "\n\t.globl _" + weak + "\n\t.weak_definition _" + weak
             + "\n\t.p2align 2\n_" + strong + ":\n_" + weak + ":\n\t.long " + value + "\n";
        s += "\t.text\n\t.globl _" + user + "\n\t.p2align 2\n_" + user + ":\n";
        s += arm ? "\tadrp x8, _" + weak + "@GOTPAGE\n\tldr x8, [x8, _" + weak + "@GOTPAGEOFF]\n\tldr w0, [x8]\n\tret\n"
                 : "\tmovq _" + weak + "@GOTPCREL(%rip), %rax\n\tmovl (%rax), %eax\n\tretq\n";
    } else {
        s += "\t.text\n\t.globl _" + strong + "\n\t.globl _" + weak + "\n\t.weak_definition _" + weak
             + "\n\t.p2align 2\n_" + strong + ":\n_" + weak + ":\n";
        s += arm ? "\tmov w0, #" + value + "\n\tret\n" : "\tmovl $" + value + ", %eax\n\tretq\n";
        s += "\t.globl _" + user + "\n\t.p2align 2\n_" + user + ":\n";
        s += arm ? "\tb _" + weak + "\n" : "\tjmp _" + weak + "\n";
    }
    return s + "\t.subsections_via_symbols\n";
}

// The cells' other units, C on every host.
struct WnSource {
    char const* stem;
    char const* text;
    bool        common = false;   // compiled -fcommon
};

[[nodiscard]] std::vector<WnSource> const& wnSources() {
    static std::vector<WnSource> const sources{
        {"main_fn", "int call_shared(void);\nint main(void) { return call_shared(); }\n"},
        {"main_d", "int read_shared_d(void);\nint main(void) { return read_shared_d(); }\n"},
        {"main_dup", "int call_a(void);\nint call_b(void);\nint main(void) { return call_a() + call_b(); }\n"},
        {"main_dupd", "int read_a(void);\nint read_b(void);\nint main(void) { return read_a() + read_b(); }\n"},
        {"main_cm", "int read_shared_d(void);\nint read_c(void);\n"
                    "int main(void) { return read_shared_d() * 10 + read_c(); }\n"},
        {"main_w", "int call_wf(void);\nint main(void) { return call_wf(); }\n"},
        {"so", "int shared(void) { return 9; }\n"},
        {"sd", "int shared_d = 9;\n"},
        {"cm", "int shared_d;\nint read_c(void) { return shared_d; }\n", true},
        {"wo", "__attribute__((weak)) int wf(void) { return 1; }\nint call_wf(void) { return wf(); }\n"},
        {"ws", "int wf(void) { return 2; }\n"},
        {"w1", "__attribute__((weak)) int shared(void) { return 11; }\nint call_a(void) { return shared(); }\n"},
        {"w2", "__attribute__((weak)) int shared(void) { return 22; }\nint call_b(void) { return shared(); }\n"}};
    return sources;
}

constexpr int kWnRefuses = -1;

struct WnCell {
    char const*              name;
    char const*              main;     // the main's stem
    std::vector<char const*> inputs;   // object stems in link order; `r_<pair>` is the artifact DSS writes of it
    int                      gnuElf, apple, gnuPe, msLink;   // each family's reference answer; kWnRefuses: no link
    int                      dssWhereRefused = 0;            // DSS's answer where the reference refuses: the decision
};

[[nodiscard]] std::vector<WnCell> const& wnCells() {
    static std::vector<WnCell> const cells{
        {"fn_pair_then_override", "main_fn", {"ao", "so"}, 9, 9, 9, 9},
        {"fn_override_then_pair", "main_fn", {"so", "ao"}, 9, 9, 9, 9},
        {"fn_pair_alone", "main_fn", {"ao"}, 7, 7, 7, 7},
        {"d_pair_then_override", "main_d", {"ad", "sd"}, 9, 9, 9, 9},
        {"d_override_then_pair", "main_d", {"sd", "ad"}, 9, 9, 9, 9},
        {"d_pair_alone", "main_d", {"ad"}, 7, 7, 7, 7},
        {"fn_two_weak_names_a_b", "main_dup", {"a1", "a2"}, 22, 22, 22, kWnRefuses, 22},
        {"fn_two_weak_names_b_a", "main_dup", {"a2", "a1"}, 44, 44, 44, kWnRefuses, 44},
        {"d_two_weak_names_a_b", "main_dupd", {"d1", "d2"}, 22, 22, 22, kWnRefuses, 22},
        {"d_two_weak_names_b_a", "main_dupd", {"d2", "d1"}, 44, 44, 44, kWnRefuses, 44},
        {"d_pair_then_common", "main_cm", {"ad", "cm"}, 0, 77, 0, 0},
        {"d_common_then_pair", "main_cm", {"cm", "ad"}, 0, 77, 0, 0},
        {"weak_definition_then_override", "main_w", {"wo", "ws"}, 2, 2, 2, 2},
        {"override_then_weak_definition", "main_w", {"ws", "wo"}, 2, 2, 2, 2},
        {"weak_definition_alone", "main_w", {"wo"}, 1, 1, 1, 1},
        {"two_weak_definitions_1_2", "main_dup", {"w1", "w2"}, 22, 22, 22, kWnRefuses, 22},
        {"two_weak_definitions_2_1", "main_dup", {"w2", "w1"}, 44, 44, 44, kWnRefuses, 44},
        {"fn_artifact_alone", "main_fn", {"r_ao"}, 7, 7, 7, 7},
        {"fn_artifact_then_override", "main_fn", {"r_ao", "so"}, 9, 9, 9, 9},
        {"fn_override_then_artifact", "main_fn", {"so", "r_ao"}, 9, 9, 9, 9},
        {"d_artifact_alone", "main_d", {"r_ad"}, 7, 7, 7, 7},
        {"d_artifact_then_override", "main_d", {"r_ad", "sd"}, 9, 9, 9, 9},
        {"d_override_then_artifact", "main_d", {"sd", "r_ad"}, 9, 9, 9, 9}};
    return cells;
}

enum class WnFamily { GnuElf, Apple, GnuPe, MsLink };

// One reference toolchain of this host. Every command runs in the arm's own directory and names its files relatively.
struct WnArm {
    std::string label;
    WnFamily    family = WnFamily::GnuElf;
    std::string obj, exe;                 // its products' extensions
    char const* spec         = nullptr;   // the DSS image the cells link into
    char const* artifactSpec = nullptr;   // the DSS relocatable artifact of a pair
    std::string asmArch;                  // Apple: the pairs are assembler-written for this -arch
    // The arm's own tool rewriting the object file `in` as `out` WITHOUT its local symbols — a relocatable LINK's
    // product, which on PE and Mach-O leaves an object whose FIRST record is an external (the `RecordSymbolIdsNative`
    // cells; an ELF table still opens with the null symbol, and its product is that suite's control). Unset: the arm
    // has no such tool.
    std::function<std::string(std::string const& out, std::string const& in)> withoutLocals;
    std::function<std::string(std::string const& stem, std::string const& sourceExt, bool common)> compile;
    std::function<std::string(std::string const& out, std::vector<std::string> const& objects)>    link;
    std::function<bool(fs::path const& dir, std::string const& cmd, std::string const& log)>       run;
};

struct WnArms {
    std::vector<WnArm>       arms;
    std::vector<std::string> absent;           // a reference toolchain of this host's leg that it does not have
    std::vector<std::string> optionalAbsent;   // one no leg owes (an optional component): said, never a failure
};

[[nodiscard, maybe_unused]] WnArm gnuStyleWnArm(std::string label, WnFamily family, std::string const& cc, std::string exe,
                                                char const* spec, char const* artifactSpec) {
    WnArm a;
    a.label        = std::move(label);
    a.family       = family;
    a.obj          = ".o";
    a.exe          = std::move(exe);
    a.spec         = spec;
    a.artifactSpec = artifactSpec;
    a.compile      = [cc](std::string const& stem, std::string const& sourceExt, bool common) {
        return cc + " -O0" + (common ? " -fcommon" : "") + " -c -o " + stem + ".o " + stem + sourceExt;
    };
    a.link = [cc, ext = a.exe](std::string const& out, std::vector<std::string> const& objects) {
        std::string c = cc + " -o " + out + ext;
        for (auto const& o : objects) c += " " + o;
        return c;
    };
    return a;
}

[[nodiscard]] WnArms wnArms(fs::path const& work) {
    WnArms found;
    auto&  arms = found.arms;
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__)) || defined(__APPLE__)
    (void)work;
    auto const posixRun = [](fs::path const& dir, std::string const& cmd, std::string const& log) {
        namespace np = test_support::native_probe;
        return std::system(np::captureCmd("cd \"" + dir.string() + "\" && " + cmd, dir / log).c_str()) == 0;
    };
#if defined(__APPLE__)
    {
        auto a = gnuStyleWnArm("Apple clang -arch arm64 (Apple's ld)", WnFamily::Apple, "cc -arch arm64", "",
                               "arm64:macho64-arm64-darwin-exec", "arm64:macho64-arm64-darwin");
        a.asmArch = "arm64";
        a.withoutLocals = [](std::string const& out, std::string const& in) {
            return "cc -arch arm64 -r -nostdlib -Wl,-x -o " + out + " " + in;
        };
        arms.push_back(std::move(a));
        auto b = gnuStyleWnArm("Apple clang -arch x86_64 (Apple's ld)", WnFamily::Apple, "cc -arch x86_64", "",
                               "x86_64:macho64-x86_64-darwin-exec", "x86_64:macho64-x86_64-darwin");
        b.asmArch = "x86_64";
        b.withoutLocals = [](std::string const& out, std::string const& in) {
            return "cc -arch x86_64 -r -nostdlib -Wl,-x -o " + out + " " + in;
        };
        arms.push_back(std::move(b));
    }
#elif defined(__x86_64__)
    arms.push_back(gnuStyleWnArm("gcc (GNU ld, x86_64)", WnFamily::GnuElf, "cc", "", "x86_64:elf64-x86_64-linux-exec",
                                 "x86_64:elf64-x86_64-linux"));
#else
    arms.push_back(gnuStyleWnArm("gcc (GNU ld, aarch64)", WnFamily::GnuElf, "cc", "", "arm64:elf64-aarch64-linux-exec",
                                 "arm64:elf64-aarch64-linux"));
#endif
#if !defined(__APPLE__)
    // GNU ld itself, as on MinGW: `ld -r -x`, the relocatable link's product of the one object.
    for (auto& a : arms) {
        a.withoutLocals = [](std::string const& out, std::string const& in) { return "ld -r -x -o " + out + " " + in; };
    }
#endif
    for (auto& a : arms) a.run = posixRun;
#elif defined(_WIN32)
    namespace np = test_support::native_probe;
    if (std::system("where gcc >nul 2>&1") == 0) {
        auto a = gnuStyleWnArm("MinGW gcc (GNU ld, PE)", WnFamily::GnuPe, "gcc", ".exe", "x86_64:pe64-x86_64-windows-exec",
                               "x86_64:pe64-x86_64-windows");
        a.run = [](fs::path const& dir, std::string const& cmd, std::string const& log) {
            return runCapturing(nullptr, dir, cmd, log);
        };
        // GNU ld itself, not the gcc driver: MinGW's driver adds an undefined `_pei386_runtime_relocator` to a
        // relocatable link (✔MEASURED 2026-10-08, gcc 13.2 with binutils 2.42).
        a.withoutLocals = [](std::string const& out, std::string const& in) { return "ld -r -x -o " + out + " " + in; };
        arms.push_back(std::move(a));
    } else {
        found.absent.push_back("MinGW gcc (GNU ld, PE): no `gcc` on PATH");
    }
    auto const msvc = np::locateMsvcToolchain(work);
    // A toolchain that is absent is this host's fact, recorded for the caller to judge (a strict run fails on it);
    // one that is present and cannot be entered is a failure here and now.
    if (!msvc.toolAbsent() && !msvc.ok()) ADD_FAILURE() << msvc.describe();
    if (msvc.toolAbsent()) found.absent.push_back("clang (link.exe) and clang (lld-link): " + msvc.detail);
    if (!msvc.toolAbsent() && msvc.ok()) {
        auto const tools = std::make_shared<np::MsvcTools>(np::msvcToolsIn(msvc, work));
        if (!tools->ready()) ADD_FAILURE() << tools->describe();
        auto const under = [tools](fs::path const& dir, std::string const& cmd, std::string const& log) {
            return runCapturing(tools.get(), dir, cmd, log);
        };
        // clang and lld-link are an OPTIONAL component of an MSVC installation, not one of a Windows leg's reference
        // toolchains (those are cl with link.exe, and MinGW gcc): an environment without them runs no clang arm and
        // says so — it is not a toolchain the leg owes, so not a strict failure either.
        bool const hasClang = tools->ready() && under(work, "clang --version", "has_clang.txt");
        bool const hasLld   = tools->ready() && under(work, "lld-link --version", "has_lld_link.txt");
        if (!hasClang) {
            found.optionalAbsent.push_back("clang (link.exe) and clang (lld-link): no `clang` in the MSVC environment");
        } else if (!hasLld) {
            found.optionalAbsent.push_back("clang (lld-link): no `lld-link` in the MSVC environment");
        }
        for (char const* linker : {"link", "lld-link"}) {
            bool const lld = std::string_view{linker} == "lld-link";
            if (!hasClang || (lld && !hasLld)) continue;
            WnArm a;
            a.label        = std::string{"clang --target=x86_64-pc-windows-msvc ("} + (lld ? "lld-link" : "link.exe") + ")";
            a.family       = WnFamily::MsLink;
            a.obj          = ".obj";
            a.exe          = ".exe";
            a.spec         = "x86_64:pe64-x86_64-windows-exec";
            a.artifactSpec = "x86_64:pe64-x86_64-windows";
            a.compile      = [](std::string const& stem, std::string const& sourceExt, bool common) {
                return std::string{"clang --target=x86_64-pc-windows-msvc -O0"} + (common ? " -fcommon" : "") + " -c -o "
                       + stem + ".obj " + stem + sourceExt;
            };
            // The clang DRIVER links, with the linker named: it names the C runtime's libraries itself. (A bare
            // `link main.obj ...` of these objects has no startup — clang's gcc-style driver writes no /DEFAULTLIB
            // directive: ✔MEASURED 2026-10-08, link.exe "unresolved external mainCRTStartup".)
            a.link = [l = std::string{linker}](std::string const& out, std::vector<std::string> const& objects) {
                std::string c = "clang --target=x86_64-pc-windows-msvc -fuse-ld=" + l + " -o " + out + ".exe";
                for (auto const& o : objects) c += " " + o;
                return c;
            };
            a.run = under;
            arms.push_back(std::move(a));
        }
    }
#else
    (void)work;
#endif
    return found;
}

}  // namespace

TEST(WeakNameReferencesNative, AReferenceThroughAWeakNameResolvesAsTheReferenceLinkerDoes) {
    namespace np = test_support::native_probe;
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "weak-name"};
    auto const dir    = scratch.path();
    auto const found  = wnArms(dir);
    auto const& arms  = found.arms;
    auto const strict = test_support::readStrictArmVerdicts();
    ASSERT_FALSE(strict.malformed) << test_support::kStrictArmVerdictsEnv << "='" << strict.raw
                                   << "' is not a recognised value";
    // A reference toolchain this host does not have is its arm NOT RUN — said, never dropped: a strict run (the
    // gate's) fails on it, as it fails on any arm the machine could not supply; any other run prints it.
    for (auto const& missing : found.absent) {
        if (strict.on) {
            ADD_FAILURE() << "[native-arm] " << missing << " — the arm cannot run on this host, and "
                          << test_support::kStrictArmVerdictsEnv << " makes that a failure";
        } else {
            std::cout << "[native-arm] " << missing << ": not run — no such toolchain on this host\n";
        }
    }
    for (auto const& missing : found.optionalAbsent) {
        std::cout << "[native-arm] " << missing << ": not run — an optional component this host does not have\n";
    }
    if (arms.empty() && !HasFailure()) GTEST_SKIP() << "no reference C toolchain on this host (see the [native-arm] lines)";
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
        for (auto const& s : wnSources()) {
            writeText(d / (std::string{s.stem} + ".c"), s.text);
            ASSERT_TRUE(arm.run(d, arm.compile(s.stem, ".c", s.common), std::string{s.stem} + ".txt"))
                << np::tailOf(d / (std::string{s.stem} + ".txt"), 20, arm.label);
        }
        std::string const pairExt = arm.asmArch.empty() ? ".c" : ".s";
        for (auto const& p : wnPairs()) {
            writeText(d / (p.stem + pairExt), arm.asmArch.empty() ? cPair(p) : applePair(arm.asmArch, p));
            ASSERT_TRUE(arm.run(d, arm.compile(p.stem, pairExt, /*common=*/false), std::string{p.stem} + ".txt"))
                << np::tailOf(d / (std::string{p.stem} + ".txt"), 20, arm.label);
        }
        // The relocatable artifact DSS writes of ONE pair's object, as `r_<stem>` beside the others.
        auto const dssArtifactOf = [&](std::string const& stem) {
            auto const out = d / ("dss_r_" + stem);
            fs::create_directories(out);
            Program p;
            p.setOutputDir(out);
            DiagnosticReporter rep;
            if (p.compileFiles(std::vector<std::string>{(d / (stem + arm.obj)).string()}, "c",
                               std::vector<std::string>{arm.artifactSpec}, rep)
                != 0) {
                ADD_FAILURE() << "DSS must relink " << stem << arm.obj << " into a relocatable object:"
                              << diagnosticsOf(rep);
                return false;
            }
            for (auto const& e : fs::directory_iterator(out)) {
                if (!e.is_regular_file() || (e.path().extension() != ".o" && e.path().extension() != ".obj")) continue;
                std::error_code ec;
                fs::copy_file(e.path(), d / ("r_" + stem + arm.obj), fs::copy_options::overwrite_existing, ec);
                if (ec) ADD_FAILURE() << "the artifact of " << stem << " could not be copied: " << ec.message();
                return !ec;
            }
            ADD_FAILURE() << "DSS wrote no relocatable object for " << stem << arm.obj;
            return false;
        };
        ASSERT_TRUE(dssArtifactOf("ao"));
        ASSERT_TRUE(dssArtifactOf("ad"));
        // DSS's link of the cell: its own main, the reference compiler's objects (and DSS's artifact) in the cell's order.
        auto const dssLink = [&](WnCell const& cell, DiagnosticReporter& rep) -> std::optional<fs::path> {
            auto const out = d / (std::string{"dss_"} + cell.name);
            fs::create_directories(out);
            Program p;
            p.setOutputDir(out);
            std::vector<std::string> inputs{(d / (std::string{cell.main} + ".c")).string()};
            for (char const* o : cell.inputs) inputs.push_back((d / (std::string{o} + arm.obj)).string());
            if (p.compileFiles(inputs, "c", std::vector<std::string>{arm.spec}, rep) != 0) return std::nullopt;
            for (auto const& e : fs::directory_iterator(out)) {
                if (e.is_regular_file() && e.path().extension() == arm.exe) return e.path();
            }
            return std::nullopt;
        };
        for (auto const& cell : wnCells()) {
            SCOPED_TRACE(cell.name);
            int const expected = arm.family == WnFamily::GnuElf  ? cell.gnuElf
                                 : arm.family == WnFamily::Apple ? cell.apple
                                 : arm.family == WnFamily::GnuPe ? cell.gnuPe
                                                                 : cell.msLink;
            // CONTROL: the reference linker's program of the same objects.
            std::vector<std::string> objects{std::string{cell.main} + arm.obj};
            for (char const* o : cell.inputs) objects.push_back(std::string{o} + arm.obj);
            std::string const       refStem = std::string{"ref_"} + cell.name;
            bool const              linked  = arm.run(d, arm.link(refStem, objects), refStem + ".txt");
            std::optional<unsigned> reference;
            if (expected == kWnRefuses) {
                EXPECT_FALSE(linked) << "the reference linker now LINKS a cell it refused when the decision was "
                                        "recorded: read what its program answers and revisit the decision";
            } else {
                ASSERT_TRUE(linked) << np::tailOf(d / (refStem + ".txt"), 20, arm.label);
                auto const ref = runImage(d / (refStem + arm.exe));
                ASSERT_TRUE(ref.spawned) << ref.diagnostic;
                EXPECT_EQ(static_cast<unsigned>(ref.exitCode), static_cast<unsigned>(expected))
                    << "the reference linker no longer answers what the row measured";
                reference = static_cast<unsigned>(ref.exitCode);
            }
            DiagnosticReporter rep;
            auto const image = dssLink(cell, rep);
            ASSERT_TRUE(image.has_value()) << "DSS must link the cell:" << diagnosticsOf(rep);
            auto const r = runImage(*image);
            ASSERT_TRUE(r.spawned) << r.diagnostic;
            EXPECT_FALSE(r.timedOut);
            if (reference.has_value()) {
                EXPECT_EQ(static_cast<unsigned>(r.exitCode), *reference) << "DSS must give the reference linker's answer";
            } else {
                EXPECT_EQ(static_cast<unsigned>(r.exitCode), static_cast<unsigned>(cell.dssWhereRefused))
                    << "where the reference refuses, DSS gives the recorded decision: the first unit's body";
            }
            std::cout << "[native-arm] " << arm.label << " " << cell.name << ": reference linker "
                      << (reference.has_value() ? std::to_string(*reference) : std::string{"REFUSES"}) << ", DSS "
                      << r.exitCode << "\n";
        }
        ++ran;
    }
    if (ran == 0 && !HasFailure()) GTEST_SKIP() << "no arm could run on this host (see the [native-arm] lines)";
}

// ══ THE ID OF A SYMBOL-TABLE RECORD ══════════════════════════════════════════════════════════════════════════════
//
// D-LK-OBJECT-READERS-GAVE-RECORD-ZERO-THE-INVALID-SYMBOL-ID (P69). An object reader names each symbol by the index
// of its record in the object's symbol table, and index 0 is not an id: it is the INVALID one, the mark of an
// unlabelled data item, which every object writer passes over when it gives symbols their records. ELF reserves
// record 0 (the null symbol). COFF and Mach-O do not — and a definition that happened to be an object's FIRST record
// was read under the invalid id and written back as unlabelled bytes: a relocatable link of that ONE object gave an
// artifact without the symbol, and said nothing. ✔MEASURED 2026-10-08: DSS's own data-only unit (`int shared = 7;`,
// whose datum its writers put at record 0) on PE and both Mach-O ISAs; Apple clang's x86_64 objects, which hold no
// local label before their first external; a MinGW object after `ld -r -x` or `strip --strip-unneeded`.
//
// THE RULE (`link/format/record_symbol_ids.hpp`): a record's id is its index, record 0 takes the first id past the
// table, and the ids no record holds come after it, from one counter. THE GATE (`linker.cpp`, the compound index):
// a NAME or a REFERENCE under the invalid id is refused by name before any writer runs, and an unlabelled data item
// — which nothing names and nothing refers to — declares nothing.
//
//   * `RecordSymbolIds`, synthetic, every leg. Per family — ELF on both ISAs as the CONTROL, Mach-O on both, PE — a
//     unit of ONE definition, a datum and a function: its writer puts it at record 0 where the format reserves
//     none; read back, nothing carries the invalid id; relinked ALONE into a relocatable artifact the name is still
//     there, and the link after it binds a second unit's reference to it; through the MERGE (beside that unit)
//     likewise. And the gate: a name row, an import row and a relocation under the invalid id are each refused,
//     once, by name; unlabelled data items link, two in one unit and across a merge.
//   * `RecordSymbolIdsNative`. Each host's reference tools write the object — as compiled, and without its local
//     symbols, which leaves the definition as the first record — and DSS writes one itself; DSS's artifact of each,
//     ALONE, links and runs under the reference linker and under DSS as the object itself does.

namespace {

// One family as the record-id pins drive it: the search pins' description of it, its target, and what its symbol
// table does with record 0.
struct RecordFamily {
    SearchFamily              fam;
    char const*               target;
    bool                      firstRecordIsASymbol;   // COFF and Mach-O: the table reserves no record
    std::vector<std::uint8_t> returns;                // a whole function body: return
};

[[nodiscard]] std::vector<RecordFamily> const& recordFamilies() {
    static std::vector<RecordFamily> const families{
        {{"ELF x86_64", "elf64-x86_64-linux", "elf64-x86_64-linux-exec", "", true, true,
          [](auto const& m, auto const& t, auto const& f, auto& r) { return elf::encode(m, t, f, r); }},
         "x86_64", false, {0xC3}},
        {{"ELF aarch64", "elf64-aarch64-linux", "elf64-aarch64-linux-exec", "", true, true,
          [](auto const& m, auto const& t, auto const& f, auto& r) { return elf::encode(m, t, f, r); }},
         "arm64", false, {0xC0, 0x03, 0x5F, 0xD6}},
        {{"Mach-O x86_64", "macho64-x86_64-darwin", "macho64-x86_64-darwin-exec", "_", false, true,
          [](auto const& m, auto const& t, auto const& f, auto& r) { return macho::encode(m, t, f, r); }},
         "x86_64", true, {0xC3}},
        {{"Mach-O arm64", "macho64-arm64-darwin", "macho64-arm64-darwin-exec", "_", false, true,
          [](auto const& m, auto const& t, auto const& f, auto& r) { return macho::encode(m, t, f, r); }},
         "arm64", true, {0xC0, 0x03, 0x5F, 0xD6}},
        {{"PE x86_64", "pe64-x86_64-windows", "pe64-x86_64-windows-exec", "", false, false,
          [](auto const& m, auto const& t, auto const& f, auto& r) { return pe::encode(m, t, f, r); }},
         "x86_64", true, {0xC3}}};
    return families;
}

// The name of the FIRST record of a relocatable object's symbol table — "" for an ELF object, whose record 0 is the
// reserved null symbol, and for a table with no record.
[[nodiscard]] std::string firstRecordName(std::vector<std::uint8_t> const& b) {
    if (b.size() >= 32 && rdLE(b, 0, 4) == 0xFEEDFACFu) {   // MH_MAGIC_64
        std::uint64_t const ncmds = rdLE(b, 16, 4);
        std::uint64_t       at    = 32;
        for (std::uint64_t i = 0; i < ncmds && at + 24 <= b.size(); ++i) {
            std::uint64_t const cmd = rdLE(b, at, 4), size = rdLE(b, at + 4, 4);
            if (cmd == 0x2u) {   // LC_SYMTAB
                std::uint64_t const symoff = rdLE(b, at + 8, 4), nsyms = rdLE(b, at + 12, 4), stroff = rdLE(b, at + 16, 4);
                if (nsyms == 0u || symoff + 16 > b.size()) return {};
                return cstrAt(b, stroff + rdLE(b, symoff, 4));
            }
            if (size == 0) break;
            at += size;
        }
        return {};
    }
    if (b.size() >= 4 && b[0] == 0x7Fu && b[1] == 'E' && b[2] == 'L' && b[3] == 'F') return {};
    if (b.size() < 20) return {};
    std::uint64_t const symtab = rdLE(b, 8, 4), nsyms = rdLE(b, 12, 4), strtab = symtab + nsyms * 18;   // COFF
    if (nsyms == 0u || symtab + 18 > b.size()) return {};
    if (rdLE(b, symtab, 4) == 0u) return cstrAt(b, strtab + rdLE(b, symtab + 4, 4));
    std::string s;
    for (std::uint64_t k = 0; k < 8 && b[symtab + k] != 0; ++k) s.push_back(static_cast<char>(b[symtab + k]));
    return s;
}

// A unit holding ONE definition, `name`: 4 bytes of data whose first is `value`, or a function that returns. A
// writer gives a unit's definitions their records in the order the unit states them, so this one's is the object's
// FIRST record wherever the format reserves none (asserted where it matters).
[[nodiscard]] AssembledModule loneDefinition(RecordFamily const& rf, std::uint32_t cu, bool datum, std::string const& name,
                                             std::uint8_t value) {
    AssembledModule m;
    m.cuId = CompilationUnitId{cu};
    if (datum) {
        AssembledData d;
        d.symbol    = SymbolId{1};
        d.section   = DataSectionKind::Data;
        d.bytes     = {value, 0, 0, 0};
        d.alignment = Alignment::of<4>();
        m.dataItems.push_back(std::move(d));
    } else {
        AssembledFunction f;
        f.symbol = SymbolId{1};
        f.bytes  = rf.returns;
        m.functions.push_back(std::move(f));
    }
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, name, SymbolBinding::Global, SymbolVisibility::Default});
    m.expectedFuncCount = m.functions.size();
    return m;
}

// A unit that only REFERS to `name`: `table`, 8 bytes holding its address through the target's absolute pointer
// relocation `pointer` (one shape for every ISA), and `entry`, a function that returns.
[[nodiscard]] AssembledModule pointerTo(RecordFamily const& rf, RelocationKind pointer, std::uint32_t cu, bool datum,
                                        std::string const& name) {
    AssembledModule m;
    m.cuId = CompilationUnitId{cu};
    ExternImport ref;
    ref.symbol      = SymbolId{1};
    ref.mangledName = name;
    ref.isData      = datum;
    m.externImports.push_back(std::move(ref));
    AssembledData table;
    table.symbol    = SymbolId{2};
    table.section   = DataSectionKind::Data;
    table.bytes.assign(8, std::uint8_t{0});
    table.alignment = Alignment::of<8>();
    Relocation rel;
    rel.offset = 0;
    rel.target = SymbolId{1};
    rel.kind   = pointer;
    rel.addend = 0;
    table.relocations.push_back(rel);
    m.dataItems.push_back(std::move(table));
    AssembledFunction f;
    f.symbol = SymbolId{3};
    f.bytes  = rf.returns;
    m.functions.push_back(std::move(f));
    m.symbols.push_back(ModuleSymbol{SymbolId{2}, rf.fam.us + "table", SymbolBinding::Global, SymbolVisibility::Default});
    m.symbols.push_back(ModuleSymbol{SymbolId{3}, rf.fam.us + "entry", SymbolBinding::Global, SymbolVisibility::Default});
    m.expectedFuncCount = m.functions.size();
    return m;
}

// The targets of the relocations of the data item `m` defines as `name`.
[[nodiscard]] std::vector<SymbolId> dataTargetsOf(AssembledModule const& m, std::string const& name) {
    std::vector<SymbolId> out;
    auto const*           s = definitionNamed(m, name);
    if (s == nullptr) return out;
    for (auto const& d : m.dataItems) {
        if (d.symbol != s->symbol) continue;
        for (auto const& r : d.relocations) out.push_back(r.target);
    }
    return out;
}

// What of `m` carries the INVALID id — empty when nothing does.
[[nodiscard]] std::string whatCarriesTheInvalidId(AssembledModule const& m) {
    std::string out;
    for (auto const& f : m.functions) {
        if (!f.symbol.valid()) out += " a function;";
        for (auto const& r : f.relocations) {
            if (!r.target.valid()) out += " a function's relocation;";
        }
    }
    for (auto const& d : m.dataItems) {
        if (!d.symbol.valid()) out += " a data item;";
        for (auto const& r : d.relocations) {
            if (!r.target.valid()) out += " a data item's relocation;";
        }
    }
    for (auto const& s : m.symbols) {
        if (!s.symbol.valid()) out += " the name '" + s.name + "';";
    }
    for (auto const& e : m.externImports) {
        if (!e.symbol.valid()) out += " the import '" + e.mangledName + "';";
    }
    return out;
}

}  // namespace

// A DEFINITION AT RECORD 0, ALONE AND THROUGH THE MERGE. Per family and for a datum and a function:
//   (1) the object: the definition is its first record (the PREMISE, asserted; on ELF the first record is the null
//       symbol — the control), and reads back under a valid id, as the name of one body;
//   (2) ALONE: the relocatable link of that one unit still spells the name, as a definition — the defect's artifact
//       held no symbol, and the link had said nothing;
//   (3) the link after it: a client unit's reference is bound to the artifact's definition in an image (CONTROL:
//       to the object's own, linked without the detour);
//   (4) through the MERGE: the client and the object in one relocatable link, either order — the artifact holds the
//       definition under its name, and the client's pointer names it.
TEST(RecordSymbolIds, ADefinitionAtRecordZeroKeepsItsNameAloneAndThroughTheMerge) {
    for (auto const& rf : recordFamilies()) {
        auto const& fam = rf.fam;
        auto const  L   = load(rf.target, fam.relocatable);
        auto const  X   = load(rf.target, fam.exec);
        ASSERT_TRUE(L.target && L.format && X.format) << fam.label;
        auto const pointer = linker::absolutePointerRelocKind(*L.target, 8);
        ASSERT_TRUE(pointer.has_value()) << fam.label << ": the target declares no absolute pointer relocation";
        for (bool const datum : {true, false}) {
            SCOPED_TRACE(std::string{fam.label} + (datum ? ", a datum" : ", a function"));
            std::string const shared = fam.us + "shared", table = fam.us + "table", entry = fam.us + "entry";

            // `bytes` read as unit `cu`, with nothing of it under the invalid id.
            auto const read = [&](std::vector<std::uint8_t> const& bytes, std::uint32_t cu) {
                DiagnosticReporter r;
                auto               back = readObject(fam, L, bytes, cu, r);
                EXPECT_TRUE(back.has_value()) << diagnosticsOf(r);
                if (back.has_value()) {
                    EXPECT_EQ(whatCarriesTheInvalidId(*back), "") << "a reader gave the INVALID id to something it names";
                }
                return back;
            };
            // `m` defines `shared`: a row under a valid id, which is the id of exactly ONE body of the right kind.
            auto const expectDefined = [&](AssembledModule const& m, char const* where) {
                auto const* row = definitionNamed(m, shared);
                ASSERT_NE(row, nullptr) << where << ": the definition lost its NAME";
                EXPECT_TRUE(row->symbol.valid()) << where;
                EXPECT_EQ(row->binding, SymbolBinding::Global) << where;
                std::size_t bodies = 0;
                for (auto const& d : m.dataItems) {
                    if (d.symbol != row->symbol) continue;
                    ++bodies;
                    EXPECT_TRUE(datum) << where << ": the body is a data item";
                    ASSERT_GE(d.bytes.size(), 4u) << where;
                    EXPECT_EQ(d.bytes[0], 7u) << where << ": the datum's own bytes";
                }
                for (auto const& f : m.functions) {
                    if (f.symbol != row->symbol) continue;
                    ++bodies;
                    EXPECT_FALSE(datum) << where << ": the body is a function";
                }
                EXPECT_EQ(bodies, 1u) << where << ": the name must be the name of ONE body";
            };

            // (1) the object.
            DiagnosticReporter rep;
            auto const object = fam.encode(loneDefinition(rf, 1, datum, shared, 7), *L.target, *L.format, rep);
            ASSERT_FALSE(object.empty()) << diagnosticsOf(rep);
            ASSERT_EQ(rep.errorCount(), 0u) << diagnosticsOf(rep);
            if (rf.firstRecordIsASymbol) {
                ASSERT_EQ(firstRecordName(object), shared)
                    << "the PREMISE of this pin: the definition is the object's FIRST record";
            } else {
                ASSERT_EQ(firstRecordName(object), "") << "CONTROL: an ELF table's record 0 is the null symbol";
            }
            ASSERT_EQ(recordsNamed(fam, object, shared).size(), 1u);
            auto const back = read(object, 1);
            ASSERT_TRUE(back.has_value());
            expectDefined(*back, "the object read back");
            if (HasFatalFailure()) return;

            // (2) ALONE.
            std::vector<AssembledModule> const alone{*back};
            DiagnosticReporter                 aloneRep;
            auto const artifact = linker::link(std::span<AssembledModule const>{alone}, *L.target, *L.format, aloneRep);
            ASSERT_TRUE(artifact.ok()) << diagnosticsOf(aloneRep);
            auto const records = recordsNamed(fam, artifact.bytes, shared);
            ASSERT_EQ(records.size(), 1u) << "the relocatable artifact of the object ALONE must still hold its symbol";
            EXPECT_FALSE(records[0].undefined) << "as a DEFINITION";
            EXPECT_FALSE(records[0].weak);
            EXPECT_FALSE(records[0].common);
            auto const artifactUnit = read(artifact.bytes, 1);
            ASSERT_TRUE(artifactUnit.has_value());
            expectDefined(*artifactUnit, "the artifact read back");
            if (HasFatalFailure()) return;

            // (3) the link after it.
            auto const clientUnit = [&](bool entered) {
                auto client = asRead(fam, L, pointerTo(rf, *pointer, 2, datum, shared), 2);
                if (!client.has_value()) return client;
                EXPECT_EQ(whatCarriesTheInvalidId(*client), "");
                if (entered) {
                    auto const* entryRow = definitionNamed(*client, entry);
                    EXPECT_NE(entryRow, nullptr);
                    if (entryRow != nullptr) client->userEntrySymbol = entryRow->symbol;
                }
                return client;
            };
            auto const bindsTheClient = [&](AssembledModule const& definer, char const* where) {
                auto const client = clientUnit(/*entered=*/true);
                ASSERT_TRUE(client.has_value()) << where;
                auto const* reference = rowNamed(*client, shared);
                ASSERT_NE(reference, nullptr) << where << ": the client's reference is an import row of the name";
                ASSERT_EQ(dataTargetsOf(*client, table), std::vector<SymbolId>{reference->symbol}) << where;
                std::vector<AssembledModule> const mods{*client, definer};
                DiagnosticReporter                 r;
                auto const img = linker::link(std::span<AssembledModule const>{mods}, *L.target, *X.format, r,
                                              ImageRequest{.artifactFileName = "record_ids_image"});
                ASSERT_TRUE(img.ok()) << where << ":" << diagnosticsOf(r);
                auto const* def = definitionNamed(definer, shared);
                ASSERT_NE(def, nullptr) << where;
                std::string bound = "(unbound)";
                for (auto const& ref : img.resolvedCrossCuRefs) {
                    if (ref.reference.cuId.v == client->cuId.v && ref.reference.symbol.v == reference->symbol.v) {
                        bound = keyText(ref.definition);
                    }
                }
                EXPECT_EQ(bound, keyText(LinkedSymbolKey{definer.cuId, def->symbol}))
                    << where << ": the client's reference must reach the definition (cu:symbol)";
            };
            bindsTheClient(*artifactUnit, "the artifact, then an image");
            if (HasFatalFailure()) return;
            bindsTheClient(*back, "CONTROL: the object itself, then an image");
            if (HasFatalFailure()) return;

            // (4) through the MERGE.
            for (bool const clientFirst : {true, false}) {
                SCOPED_TRACE(clientFirst ? "merged, the client first" : "merged, the object first");
                auto const client = clientUnit(/*entered=*/false);
                ASSERT_TRUE(client.has_value());
                std::vector<AssembledModule> const mods = clientFirst ? std::vector<AssembledModule>{*client, *back}
                                                                      : std::vector<AssembledModule>{*back, *client};
                DiagnosticReporter r;
                auto const merged = linker::link(std::span<AssembledModule const>{mods}, *L.target, *L.format, r);
                ASSERT_TRUE(merged.ok()) << diagnosticsOf(r);
                auto const mergedRecords = recordsNamed(fam, merged.bytes, shared);
                ASSERT_EQ(mergedRecords.size(), 1u) << "one object, one record of the name";
                EXPECT_FALSE(mergedRecords[0].undefined) << "the DEFINITION, not a reference left over";
                auto const mergedUnit = read(merged.bytes, 1);
                ASSERT_TRUE(mergedUnit.has_value());
                expectDefined(*mergedUnit, "the merged artifact read back");
                if (HasFatalFailure()) return;
                EXPECT_EQ(rowNamed(*mergedUnit, shared), nullptr) << "the reference was bound inside the artifact";
                EXPECT_EQ(dataTargetsOf(*mergedUnit, table),
                          std::vector<SymbolId>{definitionNamed(*mergedUnit, shared)->symbol})
                    << "the client's pointer names the definition";
            }
        }
    }
}

// THE GATE. `SymbolId{}` marks an item nothing names and nothing refers to; a unit that binds a NAME to it, or
// REFERS to it, is refused by name before any writer runs — once: the link's other "undeclared target" checks leave
// the invalid id to the gate. Per family, each against a CONTROL that links:
//   (a) a symbol row under the invalid id, on a datum and on a function — what a reader that named record 0 by its
//       index handed the link;
//   (b) an import row under it that the unit refers to: the row is refused as a NAME and the relocation as a
//       REFERENCE, two facts, each said once (a row nothing refers to never reaches the gate or a writer: the
//       reference gate drops it first, like any unreferenced import);
//   (c) a relocation whose target it is — in a data item (alone, and beside an unlabelled data item, which is not
//       what the relocation names) and, on x86_64, in a function;
//   (d) unlabelled data items themselves link: two in ONE unit, and across a merge, their bytes in the artifact.
TEST(RecordSymbolIds, TheLinkRefusesANameOrAReferenceUnderTheInvalidId) {
    struct Outcome {
        bool                      ok = false;
        std::size_t               errors = 0;
        std::string               said;
        std::vector<std::uint8_t> bytes;
    };
    for (auto const& rf : recordFamilies()) {
        auto const& fam = rf.fam;
        auto const  L   = load(rf.target, fam.relocatable);
        ASSERT_TRUE(L.target && L.format) << fam.label;
        auto const pointer = linker::absolutePointerRelocKind(*L.target, 8);
        ASSERT_TRUE(pointer.has_value()) << fam.label;
        std::string const shared = fam.us + "shared";
        auto const linkOf = [&](std::vector<AssembledModule> const& units) {
            DiagnosticReporter rep;
            auto const obj = linker::link(std::span<AssembledModule const>{units}, *L.target, *L.format, rep);
            return Outcome{obj.ok(), rep.errorCount(), diagnosticsOf(rep), obj.bytes};
        };
        auto const expectLinked = [](Outcome const& o, char const* cell) {
            EXPECT_TRUE(o.ok) << cell << ":" << o.said;
            EXPECT_EQ(o.errors, 0u) << cell << ":" << o.said;
        };
        // Refused, with ONE diagnostic for each of `whats` and no other, each naming what it refuses.
        auto const expectRefusedNaming = [](Outcome const& o, std::vector<std::string> const& whats, char const* cell) {
            EXPECT_FALSE(o.ok) << cell << ": the link must REFUSE the unit";
            EXPECT_EQ(o.errors, whats.size()) << cell << ": each refusal is said ONCE:" << o.said;
            EXPECT_NE(o.said.find("K_SymbolUndefined"), std::string::npos) << cell << ":" << o.said;
            EXPECT_NE(o.said.find("INVALID symbol id"), std::string::npos) << cell << ":" << o.said;
            for (auto const& what : whats) {
                EXPECT_NE(o.said.find(what), std::string::npos)
                    << cell << ": the refusal must NAME " << what << ":" << o.said;
            }
        };
        auto const expectRefusedOnceNaming = [&](Outcome const& o, std::string const& what, char const* cell) {
            expectRefusedNaming(o, {what}, cell);
        };

        // (a) a NAME.
        for (bool const datum : {true, false}) {
            SCOPED_TRACE(std::string{fam.label} + (datum ? ", (a) a datum's name" : ", (a) a function's name"));
            auto unit = loneDefinition(rf, 1, datum, shared, 7);
            expectLinked(linkOf({unit}), "CONTROL: the definition under an id of its own");
            if (datum) {
                unit.dataItems[0].symbol = SymbolId{};
            } else {
                unit.functions[0].symbol = SymbolId{};
            }
            unit.symbols[0].symbol = SymbolId{};
            expectRefusedOnceNaming(linkOf({unit}), "the name '" + shared + "'", "a symbol row under the invalid id");
        }

        // (b) an IMPORT ROW.
        {
            SCOPED_TRACE(std::string{fam.label} + ", (b) an import row");
            auto unit = pointerTo(rf, *pointer, 1, /*datum=*/true, shared);
            expectLinked(linkOf({unit}), "CONTROL: the reference under an id of its own");
            unit.externImports[0].symbol                = SymbolId{};
            unit.dataItems[0].relocations[0].target     = SymbolId{};
            expectRefusedNaming(linkOf({unit}), {"the import '" + shared + "'", "data item '" + fam.us + "table'"},
                                "an import row under the invalid id, and the relocation that names it");
        }

        // (c) a REFERENCE.
        {
            SCOPED_TRACE(std::string{fam.label} + ", (c) a relocation");
            auto unit = pointerTo(rf, *pointer, 1, /*datum=*/true, shared);
            unit.externImports.clear();
            unit.dataItems[0].relocations[0].target = SymbolId{};
            std::string const holder = "data item '" + fam.us + "table'";
            expectRefusedOnceNaming(linkOf({unit}), holder, "a data item's relocation to the invalid id");
            AssembledData unlabelled;   // its symbol is the invalid id: nothing names it
            unlabelled.section   = DataSectionKind::Data;
            unlabelled.bytes     = {1, 2, 3, 4};
            unlabelled.alignment = Alignment::of<4>();
            unit.dataItems.push_back(unlabelled);
            expectRefusedOnceNaming(linkOf({unit}), holder,
                                    "the same relocation beside an unlabelled data item, which it does not name");
            if (std::string_view{rf.target} == "x86_64") {
                AssembledModule code;
                code.cuId = CompilationUnitId{1};
                code.functions.push_back(reader(1, 0));   // reads the invalid id
                code.symbols.push_back(
                    ModuleSymbol{SymbolId{1}, fam.us + "read_nothing", SymbolBinding::Global, SymbolVisibility::Default});
                code.expectedFuncCount = 1;
                expectRefusedOnceNaming(linkOf({code}), "function '" + fam.us + "read_nothing'",
                                        "a function's relocation to the invalid id");
            }
        }

        // (d) unlabelled data.
        {
            SCOPED_TRACE(std::string{fam.label} + ", (d) unlabelled data");
            using Pattern = std::array<std::uint8_t, 4>;
            auto const unlabelled = [](Pattern const& bytes) {
                AssembledData d;   // its symbol is the invalid id: nothing names it, nothing refers to it
                d.section = DataSectionKind::Data;
                d.bytes.assign(bytes.begin(), bytes.end());
                d.alignment = Alignment::of<4>();
                return d;
            };
            auto const holds = [](std::vector<std::uint8_t> const& bytes, Pattern const& pattern) {
                return std::search(bytes.begin(), bytes.end(), pattern.begin(), pattern.end()) != bytes.end();
            };
            Pattern const first{0xDE, 0xC0, 0xAD, 0x0B}, second{0xEF, 0xBE, 0x0D, 0xF0}, third{0x0D, 0xD0, 0xFE, 0xCA};
            auto two = loneDefinition(rf, 1, /*datum=*/false, fam.us + "entry", 0);
            two.dataItems.push_back(unlabelled(first));
            two.dataItems.push_back(unlabelled(second));
            auto const alone = linkOf({two});
            expectLinked(alone, "two unlabelled data items in ONE unit do not collide");
            EXPECT_TRUE(holds(alone.bytes, first) && holds(alone.bytes, second)) << "their bytes are in the artifact";
            auto other = loneDefinition(rf, 2, /*datum=*/false, fam.us + "other", 0);
            other.dataItems.push_back(unlabelled(third));
            auto const merged = linkOf({two, other});
            expectLinked(merged, "unlabelled data items of two units, merged");
            EXPECT_TRUE(holds(merged.bytes, first) && holds(merged.bytes, second) && holds(merged.bytes, third))
                << "every unlabelled item's bytes are in the merged artifact";
        }
    }
}

// ══ Native, every host: an object whose FIRST record is a definition ════════════════════════════════════════════
//
// Per reference toolchain of this host, for a datum (`int shared = 7;`) and a function, three objects of the one
// definition: the reference compiler's own; that object WITHOUT its local symbols, where the arm has the tool (GNU
// ld's `-r -x` on Linux and MinGW, Apple's through `cc -r -Wl,-x`) — a relocatable LINK's product, which on PE and
// Mach-O leaves the definition as the object's first record; and the object DSS itself compiles, whose writers put
// the definition first. Each object, in turn:
//   * CONTROL: the reference linker's program of the reference's main and the object                       -> 42
//   * DSS's relocatable artifact of the object ALONE, linked by the REFERENCE linker with the reference's main -> 42
//     (the cell that found the defect: Apple's ld refused the artifact of an x86_64 object for an undefined symbol)
//   * that artifact linked by DSS with its own main                                                        -> 42
//   * the object itself linked by DSS with its own main                                                    -> 42
// Each PE and Mach-O arm must have run the cells on a DSS-written object whose first record is the definition, and —
// where a reference tool writes one — on a REFERENCE-written one: an arm on which the shape did not occur fails
// rather than passing unseen. ✔MEASURED 2026-10-08, which reference objects have the shape: Apple clang's x86_64
// objects as compiled (a datum and a function); an arm64 FUNCTION without its locals (an arm64 DATUM never — a
// local `l001` stays before it); a MinGW datum and function after `ld -r -x`. Before the fix it was the DATUM cells
// that failed (the artifact held no `shared`; GNU ld and Apple's ld refused it for an undefined symbol) — a function
// at record 0 kept its name, and its cells are this suite's controls. (ELF is the control throughout: its first
// record is the null symbol.)
//
// ★ THE CELL THAT FOUND A SECOND DEFECT, NOW A FULL CELL. Apple's relocatable link of an x86_64 FUNCTION object writes
// `__TEXT,__eh_frame` with four relocations per FDE (two SUBTRACTOR / UNSIGNED pairs; the section stores only the
// addends), and until P69 DSS's Mach-O reader refused such a section — while Apple's ld links the product and the
// program runs (✔MEASURED 2026-10-08, Apple clang 21.0.0, ld-1267 and `-ld_classic`, with and without `-x`):
//     D-LK-MACHO-LD-R-EH-FRAME-RELOCATIONS-REFUSED-AT-READ
// The reader now applies the pairs its format document declares (`macho.differenceRelocations`) before it decodes
// the section, so that object runs the same cells as every other and DSS's programs of it exit 42; the reader-level
// pins, which run on every host over Apple's own bytes, are the suite `MachoUnwindRelocations`
// (`test_macho_object_reader.cpp`). The scope was measured, not assumed: the same product of GNU ld (`ld -r -x`,
// whose `.eh_frame` keeps its relocations too) is read green on ELF, both ISAs, and on MinGW PE; an arm64 Mach-O
// object carries `__eh_frame` only for a function the compact encoding cannot describe (none here), and where it
// does the same suite reads it.
TEST(RecordSymbolIdsNative, AnObjectWhoseFirstRecordIsADefinitionRelinksWhole) {
    namespace np = test_support::native_probe;
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "record-ids"};
    auto const dir    = scratch.path();
    auto const found  = wnArms(dir);
    auto const& arms  = found.arms;
    auto const strict = test_support::readStrictArmVerdicts();
    ASSERT_FALSE(strict.malformed) << test_support::kStrictArmVerdictsEnv << "='" << strict.raw
                                   << "' is not a recognised value";
    for (auto const& missing : found.absent) {
        if (strict.on) {
            ADD_FAILURE() << "[native-arm] " << missing << " — the arm cannot run on this host, and "
                          << test_support::kStrictArmVerdictsEnv << " makes that a failure";
        } else {
            std::cout << "[native-arm] " << missing << ": not run — no such toolchain on this host\n";
        }
    }
    for (auto const& missing : found.optionalAbsent) {
        std::cout << "[native-arm] " << missing << ": not run — an optional component this host does not have\n";
    }
    if (arms.empty() && !HasFailure()) GTEST_SKIP() << "no reference C toolchain on this host (see the [native-arm] lines)";
    struct Kind {
        char const* stem;
        char const* definition;
        char const* main;
    };
    std::vector<Kind> const kinds{
        {"datum", "int shared = 7;\n", "extern int shared;\nint main(void) { return shared + 35; }\n"},
        {"function", "int shared(void) { return 7; }\n", "int shared(void);\nint main(void) { return shared() + 35; }\n"}};
    std::size_t ran = 0;
    for (std::size_t k = 0; k < arms.size(); ++k) {
        auto const& arm = arms[k];
        SCOPED_TRACE(arm.label);
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
        // What the program at `exe` exits with; nullopt after recording why it did not run.
        auto const exitOf = [&](fs::path const& exe, char const* who) -> std::optional<unsigned> {
            auto const r = test_support::runBinary(exe, test_support::kRunBudget, /*captureStdout=*/false, launcher,
                                                   /*programArgs=*/{}, launcherExecsImage);
            EXPECT_TRUE(r.spawned) << who << ": " << r.diagnostic;
            EXPECT_FALSE(r.timedOut) << who;
            if (!r.spawned || r.timedOut) return std::nullopt;
            return static_cast<unsigned>(r.exitCode);
        };
        // DSS builds `inputs` for `spec` under `<d>/<outName>`: the one product with extension `ext` (either of
        // `.o` / `.obj` when `ext` is empty), or nullopt after recording why there is none.
        fs::path const d = dir / ("arm" + std::to_string(k));
        fs::create_directories(d);
        auto const dssBuilds = [&](std::string const& outName, std::vector<std::string> const& inputs, char const* spec,
                                   std::optional<std::string> const& ext) -> std::optional<fs::path> {
            auto const out = d / outName;
            fs::create_directories(out);
            Program p;
            p.setOutputDir(out);
            DiagnosticReporter rep;
            if (p.compileFiles(inputs, "c", std::vector<std::string>{spec}, rep) != 0) {
                ADD_FAILURE() << "DSS must build " << outName << ":" << diagnosticsOf(rep);
                return std::nullopt;
            }
            for (auto const& e : fs::directory_iterator(out)) {
                if (!e.is_regular_file()) continue;
                auto const x = e.path().extension();
                if (ext.has_value() ? x == *ext : (x == ".o" || x == ".obj")) return e.path();
            }
            ADD_FAILURE() << "DSS wrote no product for " << outName;
            return std::nullopt;
        };
        for (auto const& kind : kinds) {
            SCOPED_TRACE(kind.stem);
            std::string const def = kind.stem, mainStem = std::string{"main_"} + kind.stem;
            writeText(d / (def + ".c"), kind.definition);
            writeText(d / (mainStem + ".c"), kind.main);
            ASSERT_TRUE(arm.run(d, arm.compile(def, ".c", /*common=*/false), def + ".txt"))
                << np::tailOf(d / (def + ".txt"), 20, arm.label);
            ASSERT_TRUE(arm.run(d, arm.compile(mainStem, ".c", /*common=*/false), mainStem + ".txt"))
                << np::tailOf(d / (mainStem + ".txt"), 20, arm.label);
            // The objects of the one definition, each with who wrote it.
            struct Object {
                std::string stem;
                bool        referenceWritten;
            };
            std::vector<Object> objects{{def, true}};
            if (arm.withoutLocals) {
                std::string const stripped = def + "_x";
                ASSERT_TRUE(arm.run(d, arm.withoutLocals(stripped + arm.obj, def + arm.obj), stripped + ".txt"))
                    << np::tailOf(d / (stripped + ".txt"), 20, arm.label);
                objects.push_back({stripped, true});
            }
            {
                auto const own = dssBuilds("dss_object_" + def, {(d / (def + ".c")).string()}, arm.artifactSpec, std::nullopt);
                ASSERT_TRUE(own.has_value());
                std::error_code ec;
                fs::copy_file(*own, d / ("dss_" + def + arm.obj), fs::copy_options::overwrite_existing, ec);
                ASSERT_FALSE(ec) << ec.message();
                objects.push_back({"dss_" + def, false});
            }
            bool referenceWroteItFirst = false, dssWroteItFirst = false;
            for (auto const& object : objects) {
                SCOPED_TRACE(object.stem);
                std::string const first = firstRecordName(fileBytes(d / (object.stem + arm.obj)));
                bool const definitionIsFirst = first == "shared" || first == "_shared";
                if (definitionIsFirst) (object.referenceWritten ? referenceWroteItFirst : dssWroteItFirst) = true;
                // CONTROL: the reference linker's program of the reference's main and the object.
                std::string const refStem = "ref_" + object.stem;
                ASSERT_TRUE(arm.run(d, arm.link(refStem, {mainStem + arm.obj, object.stem + arm.obj}), refStem + ".txt"))
                    << "CONTROL: " << np::tailOf(d / (refStem + ".txt"), 20, arm.label);
                auto const reference = exitOf(d / (refStem + arm.exe), "CONTROL: the reference linker's program");
                ASSERT_TRUE(reference.has_value());
                EXPECT_EQ(*reference, 42u) << "CONTROL: the reference linker's program of the object";
                // DSS's relocatable artifact of the object ALONE...
                auto const artifact = dssBuilds("dss_alone_" + object.stem, {(d / (object.stem + arm.obj)).string()},
                                                arm.artifactSpec, std::nullopt);
                ASSERT_TRUE(artifact.has_value());
                std::string const artifactStem = "r_" + object.stem;
                std::error_code   ec;
                fs::copy_file(*artifact, d / (artifactStem + arm.obj), fs::copy_options::overwrite_existing, ec);
                ASSERT_FALSE(ec) << ec.message();
                // ...linked by the REFERENCE linker with the reference's main,
                std::string const refArtifactStem = "ref_" + artifactStem;
                bool const        refLinked =
                    arm.run(d, arm.link(refArtifactStem, {mainStem + arm.obj, artifactStem + arm.obj}), refArtifactStem + ".txt");
                EXPECT_TRUE(refLinked) << "the reference linker must link DSS's artifact of the object ALONE: "
                                       << np::tailOf(d / (refArtifactStem + ".txt"), 20, arm.label);
                std::optional<unsigned> underReference;
                if (refLinked) underReference = exitOf(d / (refArtifactStem + arm.exe), "the artifact under the reference linker");
                if (underReference.has_value()) EXPECT_EQ(*underReference, 42u) << "the artifact under the reference linker";
                // ...and by DSS with its own main;
                std::optional<unsigned> underDss;
                if (auto const exe = dssBuilds("dss_artifact_" + object.stem,
                                               {(d / (mainStem + ".c")).string(), (d / (artifactStem + arm.obj)).string()},
                                               arm.spec, arm.exe)) {
                    underDss = exitOf(*exe, "the artifact under DSS");
                }
                EXPECT_TRUE(underDss.has_value() && *underDss == 42u) << "DSS's link of its own artifact of the object";
                // and the object itself linked by DSS with its own main.
                std::optional<unsigned> merged;
                if (auto const exe = dssBuilds("dss_merged_" + object.stem,
                                               {(d / (mainStem + ".c")).string(), (d / (object.stem + arm.obj)).string()},
                                               arm.spec, arm.exe)) {
                    merged = exitOf(*exe, "the object under DSS");
                }
                EXPECT_TRUE(merged.has_value() && *merged == 42u) << "DSS's link of the object beside its own main";
                auto const shown = [](std::optional<unsigned> const& v) {
                    return v.has_value() ? std::to_string(*v) : std::string{"none"};
                };
                std::cout << "[native-arm] " << arm.label << " " << object.stem << ": first record '" << first
                          << "', reference linker " << shown(reference) << ", DSS's artifact of it alone under the reference "
                          << "linker " << shown(underReference) << " and under DSS " << shown(underDss)
                          << ", the object under DSS " << shown(merged) << "\n";
            }
            if (arm.family != WnFamily::GnuElf) {
                EXPECT_TRUE(dssWroteItFirst) << "DSS's own object no longer has its definition as the FIRST record: the "
                                                "cell this suite exists for did not run on a DSS-written object";
                // ✔MEASURED 2026-10-08 (Apple clang 21, ld-1267): no tool of Apple's leaves an arm64 DATUM as its
                // object's first record — the compiler's own object opens with `ltmp0`, and `ld -r -x` and
                // `strip -x` keep a local `l001` before the datum. Every other (arm, kind) with the tool has a
                // reference-written object of the shape: an x86_64 object as compiled, an arm64 function and a
                // MinGW object without their locals.
                bool const referenceHasNone =
                    arm.family == WnFamily::Apple && arm.asmArch == "arm64" && std::string_view{kind.stem} == "datum";
                if (referenceHasNone) {
                    std::cout << "[native-arm] " << arm.label << " " << kind.stem << ": no reference-written object has "
                              << "the definition as its first record (measured: Apple's tools keep a local before an "
                              << "arm64 datum) — the DSS-written object is this arm's cell"
                              << (referenceWroteItFirst ? "; TODAY one does, and its cells ran" : "") << "\n";
                } else if (arm.withoutLocals) {
                    EXPECT_TRUE(referenceWroteItFirst)
                        << "no reference-written object of this arm has its definition as the FIRST record — not as "
                           "compiled and not without its local symbols: the cell did not run on a reference's object";
                }
            }
        }
        ++ran;
    }
    if (ran == 0 && !HasFailure()) GTEST_SKIP() << "no arm could run on this host (see the [native-arm] lines)";
}

// ══ THE ORDER OF THE ARCHIVE SEARCH'S ROUNDS: references, weak ones too, BEFORE commons ══════════════════════════
//
// `pullStaticArchiveMembers` answers what a link still lacks in rounds: the worklist (every strong reference), the
// WEAK round (a name held only as a weak reference, where the members' document says `archiveWeakReferenceSearch:
// fetchMember`), the COMMONS round (a name still a common, where it says `archiveCommonResolution: fetchDefinition`),
// then the fallbacks. Both of the middle rounds fetch only in a Mach-O link, so only there is their order a fact a
// program can see — and it is the reference's, by measurement.
// ✔MEASURED 2026-10-08 on Apple's ld (Apple clang 21: ld-1267 on arm64 and x86_64, and ld64-957.1; 56 cells, two
// probes that agree cell for cell). A unit holds `w` only as a weak reference and `c` as a common:
//     archive [m1: w = 1] [m2: c = 5, w = 2]    REFUSED, duplicate `w` — m1 came in for the reference, then m2 for
//                                               the common (commons first would have loaded m2 alone)
//     archive [m2] [m1]                         m2 alone: w = 2, c = 5
//     archive [m0: c = 5] [m15: w = 1, c = 6]   m15 ALONE: w = 1, c = 6 — the reference fetched it, and its `c` ended
//                                               the common's search (commons first would have loaded m0, then m15
//                                               for the reference: a duplicate `c`)
//     archive [m15] [m0]                        m15 alone
// and a STRONG reference to `w` answers the same in every cell: Apple's ld answers a reference, weak or strong,
// before it walks its commons. (ELF and PE: one of the two rounds fetches nothing there — no member is fetched for
// a weak reference on ELF and PE, none for a common on PE — so the order shows in no program, and none is pinned.)
//   * `ArchiveSearchRoundOrder.AWeakReferenceIsAnsweredBeforeACommon`: the four archives against the weak and the
//     strong client, under both Mach-O ISAs' documents, on every leg — which members the search fetches, and what
//     the link then says or binds.
//   * `ArchiveSearchRoundOrderNative`: the same cells on a Mac, Apple clang's objects and Apple's archives, under
//     Apple's ld (the CONTROL) and under DSS.

namespace {

// The client of the order pins: `table`, two pointers — to `w`, which the unit holds only as a REFERENCE (weak, or
// strong for the control), and to `c`, which it holds as a COMMON — and `entry`, a function that returns. The search
// gives every member it fetches a unit id from a process-wide counter that starts at 1, and this unit is LINKED with
// them, so its own id is one that counter does not reach in a test process.
[[nodiscard]] AssembledModule orderClient(RecordFamily const& rf, RelocationKind pointer, bool weak) {
    AssembledModule m;
    m.cuId = CompilationUnitId{0x7FFFFF00u};
    ExternImport w;
    w.symbol      = SymbolId{1};
    w.mangledName = rf.fam.us + "w";
    w.isData      = true;
    if (weak) w.binding = SymbolBinding::Weak;
    m.externImports.push_back(std::move(w));
    m.externImports.push_back(commonRow(2, rf.fam.us + "c", 4, 4));
    AssembledData table;
    table.symbol    = SymbolId{3};
    table.section   = DataSectionKind::Data;
    table.bytes.assign(16, std::uint8_t{0});
    table.alignment = Alignment::of<8>();
    for (std::uint32_t slot = 0; slot < 2; ++slot) {
        Relocation rel;
        rel.offset = slot * 8;
        rel.target = SymbolId{slot + 1};
        rel.kind   = pointer;
        rel.addend = 0;
        table.relocations.push_back(rel);
    }
    m.dataItems.push_back(std::move(table));
    AssembledFunction f;
    f.symbol = SymbolId{4};
    f.bytes  = rf.returns;
    m.functions.push_back(std::move(f));
    m.symbols.push_back(ModuleSymbol{SymbolId{3}, rf.fam.us + "table", SymbolBinding::Global, SymbolVisibility::Default});
    m.symbols.push_back(ModuleSymbol{SymbolId{4}, rf.fam.us + "entry", SymbolBinding::Global, SymbolVisibility::Default});
    m.expectedFuncCount = m.functions.size();
    m.userEntrySymbol   = SymbolId{4};
    return m;
}

// An archive member of the order pins: the data it defines (4 bytes each, the first is the value), and `tag`, a
// datum only this member defines — which says, of a fetched set, that this member is in it.
struct OrderMember {
    std::string                                        tag;
    std::vector<std::pair<std::string, std::uint8_t>> defines;
};

[[nodiscard]] AssembledModule orderMemberUnit(std::string const& us, OrderMember const& member) {
    AssembledModule m;
    m.cuId = CompilationUnitId{1};
    std::uint32_t next = 1;
    auto const datum = [&](std::string const& name, std::uint8_t value) {
        AssembledData d;
        d.symbol    = SymbolId{next++};
        d.section   = DataSectionKind::Data;
        d.bytes     = {value, 0, 0, 0};
        d.alignment = Alignment::of<4>();
        m.symbols.push_back(ModuleSymbol{d.symbol, us + name, SymbolBinding::Global, SymbolVisibility::Default});
        m.dataItems.push_back(std::move(d));
    };
    for (auto const& [name, value] : member.defines) datum(name, value);
    datum(member.tag, 0);
    return m;
}

// The tags of the members in `units`, sorted: which members a search fetched.
[[nodiscard]] std::vector<std::string> orderMembersIn(std::vector<AssembledModule> const& units, std::string const& us) {
    std::vector<std::string> tags;
    for (auto const& m : units) {
        for (auto const& s : m.symbols) {
            std::string_view name{s.name};
            if (!name.starts_with(us)) continue;
            name.remove_prefix(us.size());
            if (name.starts_with("fetched_")) tags.emplace_back(name);
        }
    }
    std::sort(tags.begin(), tags.end());
    return tags;
}

// One cell of the order pins: an archive's members in order, the members the search must fetch, and what the link
// of the client with them answers — refused for a duplicate of `duplicate`, or `w` and `c` bound to these values.
struct OrderCell {
    char const*              name;
    std::vector<char const*> archive;   // tags, in archive order
    std::vector<std::string> fetched;   // tags, sorted
    char const*              duplicate; // the name the link refuses twice-defined; nullptr: it links
    unsigned                 w = 0, c = 0;
};

[[nodiscard]] std::vector<OrderCell> const& orderCells() {
    static std::vector<OrderCell> const cells{
        {"[m1: w] [m2: c, w]", {"fetched_m1", "fetched_m2"}, {"fetched_m1", "fetched_m2"}, "w"},
        {"[m2: c, w] [m1: w]", {"fetched_m2", "fetched_m1"}, {"fetched_m2"}, nullptr, 2, 5},
        {"[m0: c] [m15: w, c]", {"fetched_m0", "fetched_m15"}, {"fetched_m15"}, nullptr, 1, 6},
        {"[m15: w, c] [m0: c]", {"fetched_m15", "fetched_m0"}, {"fetched_m15"}, nullptr, 1, 6}};
    return cells;
}

[[nodiscard]] std::vector<OrderMember> const& orderMembers() {
    static std::vector<OrderMember> const members{{"fetched_m0", {{"c", 5}}},
                                                  {"fetched_m1", {{"w", 1}}},
                                                  {"fetched_m2", {{"c", 5}, {"w", 2}}},
                                                  {"fetched_m15", {{"w", 1}, {"c", 6}}}};
    return members;
}

}  // namespace

TEST(ArchiveSearchRoundOrder, AWeakReferenceIsAnsweredBeforeACommon) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "archive-round-order"};
    auto const dir = scratch.path();
    std::size_t families = 0;
    for (auto const& rf : recordFamilies()) {
        auto const& fam = rf.fam;
        if (!std::string_view{fam.relocatable}.starts_with("macho")) continue;   // where both rounds fetch
        ++families;
        auto const L = load(rf.target, fam.relocatable);
        ASSERT_TRUE(L.target && L.format) << fam.label;
        auto const exec = ObjectFormatSchema::loadShipped(fam.exec);
        ASSERT_TRUE(exec.has_value()) << fam.exec;
        auto const pointer = linker::absolutePointerRelocKind(*L.target, 8);
        ASSERT_TRUE(pointer.has_value()) << fam.label;
        // Each member, as the family's writer writes it.
        std::unordered_map<std::string, link::format::ArMemberInput> archived;
        for (auto const& member : orderMembers()) {
            DiagnosticReporter rep;
            auto const bytes = fam.encode(orderMemberUnit(fam.us, member), *L.target, *L.format, rep);
            ASSERT_FALSE(bytes.empty()) << fam.label << " " << member.tag << ":" << diagnosticsOf(rep);
            std::vector<std::string> names{fam.us + member.tag};
            for (auto const& [name, value] : member.defines) names.push_back(fam.us + name);
            archived.emplace(member.tag, link::format::ArMemberInput{member.tag + ".o", bytes, names});
        }
        for (bool const weak : {true, false}) {
            for (auto const& cell : orderCells()) {
                SCOPED_TRACE(std::string{fam.label} + (weak ? ", a WEAK reference: " : ", CONTROL, a strong reference: ")
                             + cell.name);
                std::vector<link::format::ArMemberInput> members;
                std::string                              tag = weak ? "weak" : "strong";
                for (char const* m : cell.archive) {
                    members.push_back(archived.at(m));
                    tag += std::string{"-"} + m;
                }
                DiagnosticReporter rep;
                std::vector<fs::path> const archives{archiveAt(dir / (std::string{fam.relocatable} + "-" + tag + ".a"), members, rep)};
                ASSERT_FALSE(archives.front().empty()) << diagnosticsOf(rep);
                std::vector<AssembledModule> const clients{orderClient(rf, *pointer, weak)};
                DiagnosticReporter pullRep;
                auto const pulled = pullStaticArchiveMembers(clients, archives, {}, *L.target, **exec, pullRep);
                ASSERT_TRUE(pulled.has_value()) << diagnosticsOf(pullRep);
                EXPECT_EQ(orderMembersIn(*pulled, fam.us), cell.fetched)
                    << "which members the search fetches is the ORDER of its rounds: a reference, weak or strong, is "
                       "answered before a common";
                // What the link of the client with the fetched members then answers.
                std::vector<AssembledModule> mods{clients.front()};
                mods.insert(mods.end(), pulled->begin(), pulled->end());
                DiagnosticReporter linkRep;
                auto const img = linker::link(std::span<AssembledModule const>{mods}, *L.target, **exec, linkRep,
                                              ImageRequest{.artifactFileName = "round_order_image"});
                if (cell.duplicate != nullptr) {
                    EXPECT_FALSE(img.ok()) << "both definitions of `" << cell.duplicate << "` came in: the link refuses";
                    std::string const said = diagnosticsOf(linkRep);
                    EXPECT_NE(said.find("K_SymbolRedefinedAcrossUnits"), std::string::npos) << said;
                    EXPECT_NE(said.find("\"" + fam.us + cell.duplicate + "\""), std::string::npos) << said;
                    continue;
                }
                ASSERT_TRUE(img.ok()) << diagnosticsOf(linkRep);
                // The value of the datum the client's row `id` was bound to.
                auto const boundValue = [&](std::uint32_t id) -> std::optional<unsigned> {
                    for (auto const& ref : img.resolvedCrossCuRefs) {
                        if (ref.reference.cuId.v != mods.front().cuId.v || ref.reference.symbol.v != id) continue;
                        for (auto const& m : mods) {
                            if (m.cuId.v != ref.definition.cuId.v) continue;
                            for (auto const& d : m.dataItems) {
                                if (d.symbol == ref.definition.symbol && !d.bytes.empty()) return d.bytes[0];
                            }
                        }
                    }
                    return std::nullopt;
                };
                auto const w = boundValue(1), c = boundValue(2);
                ASSERT_TRUE(w.has_value()) << "the reference to `w` is bound to no fetched member's definition";
                ASSERT_TRUE(c.has_value()) << "the common `c` is bound to no fetched member's definition";
                EXPECT_EQ(*w, cell.w) << "`w`";
                EXPECT_EQ(*c, cell.c) << "`c`: the fetched member's strong definition replaced the common";
            }
        }
    }
    EXPECT_EQ(families, 2u) << "both Mach-O ISAs' documents";
}

// The same cells on a Mac. Apple clang writes `tu_weak` (the weak reference to `w` and the common `c`, -fcommon, read
// by `read_w` / `read_c`) and `tu_strong` (the control), and the four members; `ar` writes each archive; a main
// returns `read_w() * 16 + read_c()`. Apple's ld is the CONTROL and must still answer what was measured; DSS, its
// own main beside Apple's objects and archive, must answer the same — and refuse the cell Apple's ld refuses, for
// the same duplicate.
//                                      [m1][m2]    [m2][m1]    [m0][m15]    [m15][m0]
//   Apple's ld, weak and strong        refused        37           22           22
TEST(ArchiveSearchRoundOrderNative, AppleLdAnswersAReferenceBeforeACommonAndSoDoesDss) {
    namespace np = test_support::native_probe;
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "archive-round-order-native"};
    auto const dir    = scratch.path();
    auto const found  = yieldArms(dir);
    auto const strict = test_support::readStrictArmVerdicts();
    ASSERT_FALSE(strict.malformed) << test_support::kStrictArmVerdictsEnv << "='" << strict.raw
                                   << "' is not a recognised value";
    std::vector<YieldArm const*> arms;
    for (auto const& a : found.arms) {
        if (std::string_view{a.spec}.find("macho") != std::string_view::npos) arms.push_back(&a);
    }
    if (arms.empty()) {
        GTEST_SKIP() << "the two rounds both fetch only in a Mach-O link, and this host has no Apple toolchain — the "
                        "synthetic half (ArchiveSearchRoundOrder) runs the Mach-O documents on every host";
    }
    struct Cell {
        char const*              name;
        std::vector<std::string> members;
        int                      expected;   // -1: refused, for a duplicate `_w`
    };
    std::vector<Cell> const cells{{"m1_m2", {"m1", "m2"}, -1}, {"m2_m1", {"m2", "m1"}, 37},
                                  {"m0_m15", {"m0", "m15"}, 22}, {"m15_m0", {"m15", "m0"}, 22}};
    std::size_t ran = 0;
    for (std::size_t k = 0; k < arms.size(); ++k) {
        auto const& arm = *arms[k];
        SCOPED_TRACE(arm.label);
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
        writeText(d / "main.c", "int read_w(void);\nint read_c(void);\nint main(void) { return read_w() * 16 + read_c(); }\n");
        writeText(d / "tu_weak.c", "extern int w __attribute__((weak));\nint c;\n"
                                   "int read_w(void) { return &w ? w : 0; }\nint read_c(void) { return c; }\n");
        writeText(d / "tu_strong.c", "extern int w;\nint c;\n"
                                     "int read_w(void) { return w; }\nint read_c(void) { return c; }\n");
        writeText(d / "m0.c", "int c = 5;\n");
        writeText(d / "m1.c", "int w = 1;\n");
        writeText(d / "m2.c", "int c = 5;\nint w = 2;\n");
        writeText(d / "m15.c", "int w = 1;\nint c = 6;\n");
        for (char const* s : {"tu_weak", "tu_strong"}) {
            ASSERT_TRUE(arm.run(d, arm.compile(s, /*common=*/true), std::string{s} + ".txt"))
                << np::tailOf(d / (std::string{s} + ".txt"), 20, arm.label);
        }
        for (char const* s : {"m0", "m1", "m2", "m15"}) {
            ASSERT_TRUE(arm.run(d, arm.compile(s, /*common=*/false), std::string{s} + ".txt"))
                << np::tailOf(d / (std::string{s} + ".txt"), 20, arm.label);
        }
        for (auto const& cell : cells) {
            std::string const lib = std::string{"lib_"} + cell.name;
            ASSERT_TRUE(arm.run(d, arm.archive(lib, cell.members), lib + ".txt"))
                << np::tailOf(d / (lib + ".txt"), 20, arm.label);
            for (char const* unit : {"tu_weak", "tu_strong"}) {
                std::string const tag = std::string{unit} + "_" + cell.name;
                SCOPED_TRACE(tag);
                // CONTROL: Apple's ld.
                std::string const refStem = "ref_" + tag;
                bool const linked = arm.run(d, arm.link(refStem, "main.c", {std::string{unit} + arm.obj, lib + arm.lib}),
                                            refStem + ".txt");
                std::optional<unsigned> reference;
                if (cell.expected < 0) {
                    EXPECT_FALSE(linked) << "Apple's ld now LINKS the cell it refused for a duplicate `_w` when the "
                                            "order was measured: read what its program answers and revisit the order";
                } else {
                    ASSERT_TRUE(linked) << np::tailOf(d / (refStem + ".txt"), 20, arm.label);
                    auto const ref = runImage(d / (refStem + arm.exe));
                    ASSERT_TRUE(ref.spawned) << ref.diagnostic;
                    EXPECT_EQ(static_cast<unsigned>(ref.exitCode), static_cast<unsigned>(cell.expected))
                        << "Apple's ld no longer answers what was measured";
                    reference = static_cast<unsigned>(ref.exitCode);
                }
                // DSS: its own main, Apple's object, Apple's archive.
                auto const out = d / ("dss_" + tag);
                fs::create_directories(out);
                Program p;
                p.setOutputDir(out);
                p.setResolveLibraries(std::vector<fs::path>{d / (lib + arm.lib)});
                DiagnosticReporter rep;
                int const rc = p.compileFiles(std::vector<std::string>{(d / "main.c").string(),
                                                                       (d / (std::string{unit} + arm.obj)).string()},
                                              "c", std::vector<std::string>{arm.spec}, rep);
                if (cell.expected < 0) {
                    EXPECT_NE(rc, 0) << "DSS must refuse the cell Apple's ld refuses";
                    bool named = false;
                    for (auto const& diag : rep.all()) {
                        named = named || (diag.severity == DiagnosticSeverity::Error
                                          && diag.actual.find("\"_w\"") != std::string::npos);
                    }
                    EXPECT_TRUE(named) << "the refusal must name the twice-defined `_w`:" << diagnosticsOf(rep);
                    std::cout << "[native-arm] " << arm.label << " " << tag << ": Apple's ld REFUSES, DSS "
                              << (rc != 0 ? "REFUSES" : "links") << "\n";
                    continue;
                }
                ASSERT_EQ(rc, 0) << "DSS must link the cell:" << diagnosticsOf(rep);
                fs::path image;
                for (auto const& e : fs::directory_iterator(out)) {
                    if (e.is_regular_file() && e.path().extension() == arm.exe) image = e.path();
                }
                ASSERT_FALSE(image.empty()) << "no image under " << out.string();
                auto const r = runImage(image);
                ASSERT_TRUE(r.spawned) << r.diagnostic;
                EXPECT_FALSE(r.timedOut);
                ASSERT_TRUE(reference.has_value());
                EXPECT_EQ(static_cast<unsigned>(r.exitCode), *reference) << "DSS must give Apple's ld's answer";
                std::cout << "[native-arm] " << arm.label << " " << tag << ": Apple's ld " << *reference << ", DSS "
                          << r.exitCode << "\n";
            }
        }
        ++ran;
    }
    if (ran == 0 && !HasFailure()) GTEST_SKIP() << "no arm could run on this host (see the [native-arm] lines)";
}

// ══ A COMMON NOTHING READS, BESIDE A THREAD-LOCAL DEFINITION OF ITS NAME ═════════════════════════════════════════
//
// D-LK-THREAD-STORAGE-DISAGREEMENT-REFUSED-ONLY-BY-THE-WRITER-BACKSTOP (the unread arm, P69). Whether a link judges a
// reference no code reads is what the reference's OBJECT RECORD states (`ExternImport::recordStatesStorageDuration`,
// `link/thread_storage_agreement.hpp`): an ELF symbol is typed, a COFF symbol and a Mach-O nlist are not.
// ✔MEASURED 2026-10-08 — a tentative `int c;` nothing reads, beside a thread-local definition of `c`, both orders:
//     GNU ld 2.42 and ld.lld 18.1.3 (gcc 13.3.0 -fcommon, ELF)                 REFUSE the pair
//     link.exe 14.44 and lld-link 19.1.5 (cl 19.44; clang 19.1.5 -fcommon)     link it: 0, and 5 beside a reader
//     Apple's ld (ld-1267 on both ISAs, ld64-957.1; Apple clang 21 -fcommon)   link it: 0, and 5 beside a reader
//   * `CommonSymbols.AnUnreadElfCommonKeepsWhatItsRecordStatesThroughARelocatableArtifact`: the fact is the
//     RECORD's, so a relocatable artifact states it again — the common relinked alone, and folded with another
//     unit's reference of its name in either order, is still refused by the link after it.
//   * `CommonSymbolsNative.AnUnreadCommonBesideAThreadLocalDefinitionIsWhatItsRecordStates`: gcc's objects on the
//     Linux legs (GNU ld refuses; DSS refuses, by name) and cl's on the Windows legs (link.exe links its all-cl
//     program to 0 and, with a thread-local reader, to 5; DSS answers the same for cl's common beside a thread-local
//     definition DSS itself compiles — the one input of the pair it cannot read yet is cl's own `.tls$` definition,
//     which is another row's subject and is named in the test).

TEST(CommonSymbols, AnUnreadElfCommonKeepsWhatItsRecordStatesThroughARelocatableArtifact) {
    auto const L = load("x86_64", "elf64-x86_64-linux");
    auto const X = load("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_TRUE(L.target && L.format && X.format);
    auto const& elfFamily = searchFamilies().front();
    ASSERT_EQ(std::string_view{elfFamily.label}, "ELF");
    // U: the common `c`, which nothing of its unit reads, beside a function. V: a unit whose function reads `c`
    // through a plain reference. Each as the ELF writer writes it and the ELF reader reads it.
    auto const unitU = [&](std::uint32_t cu) {
        return asRead(elfFamily, L, UnitOf(cu).common("c").marker("u_entry").build(), cu);
    };
    auto const unitV = [&](std::uint32_t cu) {
        return asRead(elfFamily, L, UnitOf(cu).reference("c", /*isData=*/true).reads("v_reads_c", "c").build(), cu);
    };
    {
        auto const u = unitU(1);
        ASSERT_TRUE(u.has_value());
        auto const* row = rowNamed(*u, "c");
        ASSERT_NE(row, nullptr);
        ASSERT_EQ(row->commonSize, 4u);
        ASSERT_TRUE(row->recordStatesStorageDuration) << "the PREMISE: the ELF reader read the common's record";
        auto const v = unitV(2);
        ASSERT_TRUE(v.has_value());
        auto const* plain = rowNamed(*v, "c");
        ASSERT_NE(plain, nullptr);
        EXPECT_FALSE(plain->recordStatesStorageDuration) << "an undefined ELF symbol is untyped: it states nothing";
    }
    // A definition of `c`, thread-local or ordinary, as the unit after `cu`.
    auto const definitionOfC = [](std::uint32_t cu, bool threadLocal) {
        AssembledModule m;
        m.cuId = CompilationUnitId{cu};
        AssembledData d;
        d.symbol    = SymbolId{1};
        d.section   = threadLocal ? DataSectionKind::Tdata : DataSectionKind::Data;
        d.bytes     = {5, 0, 0, 0};
        d.alignment = Alignment::of<4>();
        m.dataItems.push_back(std::move(d));
        m.symbols.push_back(ModuleSymbol{SymbolId{1}, "c", SymbolBinding::Global, SymbolVisibility::Default});
        return m;
    };
    struct Cell {
        char const* name;
        int         shape;      // 0: U alone; 1: U then V; 2: V then U
        bool        readInside;
    };
    for (Cell const& cell : {Cell{"the common's unit alone", 0, false}, Cell{"the common, then a unit that reads the name", 1, true},
                             Cell{"a unit that reads the name, then the common", 2, true}}) {
        SCOPED_TRACE(cell.name);
        std::vector<AssembledModule> inputs;
        if (cell.shape == 0) {
            auto u = unitU(1);
            ASSERT_TRUE(u.has_value());
            inputs.push_back(std::move(*u));
        } else {
            auto u = unitU(cell.shape == 1 ? 1u : 2u);
            auto v = unitV(cell.shape == 1 ? 2u : 1u);
            ASSERT_TRUE(u.has_value() && v.has_value());
            if (cell.shape == 1) {
                inputs.push_back(std::move(*u));
                inputs.push_back(std::move(*v));
            } else {
                inputs.push_back(std::move(*v));
                inputs.push_back(std::move(*u));
            }
        }
        DiagnosticReporter rep;
        auto const artifact = linker::link(std::span<AssembledModule const>{inputs}, *L.target, *L.format, rep);
        ASSERT_TRUE(artifact.ok()) << diagnosticsOf(rep);
        ASSERT_EQ(elfSymbolsNamed(artifact.bytes, "c").size(), 1u);
        EXPECT_EQ(elfSymbolsNamed(artifact.bytes, "c")[0].shndx, kShnCommon) << "the artifact hands the COMMON on";
        for (bool const threadLocal : {true, false}) {
            SCOPED_TRACE(threadLocal ? "then a THREAD-LOCAL definition" : "CONTROL: then an ordinary definition");
            DiagnosticReporter readRep;
            auto unit = elf::readRelocatableObject(artifact.bytes, *L.target, *L.format, readRep, CompilationUnitId{1});
            ASSERT_TRUE(unit.has_value()) << diagnosticsOf(readRep);
            auto const* row = rowNamed(*unit, "c");
            ASSERT_NE(row, nullptr);
            EXPECT_EQ(row->commonSize, 4u);
            EXPECT_TRUE(row->recordStatesStorageDuration) << "the artifact's own record states the fact again";
            auto const* entry = definitionNamed(*unit, "u_entry");
            ASSERT_NE(entry, nullptr);
            unit->userEntrySymbol = entry->symbol;
            std::vector<AssembledModule> const mods{*unit, definitionOfC(2, threadLocal)};
            DiagnosticReporter linkRep;
            auto const img = linker::link(std::span<AssembledModule const>{mods}, *L.target, *X.format, linkRep,
                                          ImageRequest{.artifactFileName = "unread_common_image"});
            if (!threadLocal) {
                EXPECT_TRUE(img.ok()) << diagnosticsOf(linkRep);
                EXPECT_EQ(linkRep.errorCount(), 0u) << diagnosticsOf(linkRep);
                continue;
            }
            EXPECT_FALSE(img.ok()) << "the link after the artifact must still refuse the pair";
            std::vector<std::string> named;
            for (auto const& d : linkRep.all()) {
                if (d.code == DiagnosticCode::K_ExternImportAttributeConflict) named.push_back(d.actual);
            }
            ASSERT_EQ(named.size(), 1u) << diagnosticsOf(linkRep);
            EXPECT_NE(named[0].find("symbol 'c'"), std::string::npos) << named[0];
            EXPECT_NE(named[0].find("has THREAD STORAGE DURATION"), std::string::npos) << named[0];
            EXPECT_NE(named[0].find(cell.readInside ? "refers to it as an ORDINARY object"
                                                    : "holds it as an ORDINARY object"),
                      std::string::npos)
                << named[0];
        }
    }
}

TEST(CommonSymbolsNative, AnUnreadCommonBesideAThreadLocalDefinitionIsWhatItsRecordStates) {
    namespace np = test_support::native_probe;
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    test_support::ScratchDir scratch{test_support::Location::Temp, "unread-common-tls"};
    auto const dir = scratch.path();
    auto const at = [&](std::string const& name) { return "\"" + (dir / name).string() + "\""; };
    auto const sh = [&](std::string const& cmd, std::string const& log) {
        return std::system(np::captureCmd(cmd, dir / log).c_str()) == 0;
    };
    writeText(dir / "main.c", "int main(void) { return 0; }\n");
    writeText(dir / "unread.c", "int c;\n");
    writeText(dir / "tls.c", "__thread int c = 5;\n");
    writeText(dir / "plain.c", "int c = 5;\n");
    ASSERT_TRUE(sh("cc -c -O2 -fcommon -o " + at("unread.o") + " " + at("unread.c"), "unread.txt"))
        << np::tailOf(dir / "unread.txt", 20, "cc");
    for (char const* s : {"tls", "plain"}) {
        ASSERT_TRUE(sh("cc -c -O2 -o " + at(std::string{s} + ".o") + " " + at(std::string{s} + ".c"), std::string{s} + ".txt"))
            << np::tailOf(dir / (std::string{s} + ".txt"), 20, "cc");
    }
    auto const common = elfSymbolsNamed(fileBytes(dir / "unread.o"), "c");
    ASSERT_EQ(common.size(), 1u);
    EXPECT_EQ(common[0].shndx, kShnCommon) << "gcc -fcommon writes `int c;` as SHN_COMMON though nothing reads it";
#if defined(__x86_64__)
    char const* const spec = "x86_64:elf64-x86_64-linux-exec";
#else
    char const* const spec = "arm64:elf64-aarch64-linux-exec";
#endif
    struct Cell {
        char const*              name;
        std::vector<std::string> objects;
        bool                     refused;
    };
    for (Cell const& cell : {Cell{"unread_then_tls", {"unread.o", "tls.o"}, true}, Cell{"tls_then_unread", {"tls.o", "unread.o"}, true},
                             Cell{"unread_then_plain", {"unread.o", "plain.o"}, false}}) {
        SCOPED_TRACE(cell.name);
        // The reference: GNU ld.
        std::string ref = "cc -O2 -o " + at(std::string{"ref_"} + cell.name) + " " + at("main.c");
        for (auto const& o : cell.objects) ref += " " + at(o);
        bool const linked = sh(ref, std::string{"ref_"} + cell.name + ".txt");
        if (cell.refused) {
            EXPECT_FALSE(linked) << "GNU ld must refuse a common nothing reads against a thread-local definition";
            EXPECT_NE(np::tailOf(dir / (std::string{"ref_"} + cell.name + ".txt"), 20).find("mismatches non-TLS reference"),
                      std::string::npos)
                << np::tailOf(dir / (std::string{"ref_"} + cell.name + ".txt"), 20);
        } else {
            ASSERT_TRUE(linked) << np::tailOf(dir / (std::string{"ref_"} + cell.name + ".txt"), 20);
            auto const r = test_support::runBinary(dir / (std::string{"ref_"} + cell.name));
            ASSERT_TRUE(r.spawned) << r.diagnostic;
            EXPECT_EQ(r.exitCode, 0u) << "CONTROL: GNU ld, the ordinary definition";
        }
        // DSS: its own main, gcc's objects.
        auto const out = dir / (std::string{"dss-"} + cell.name);
        fs::create_directories(out);
        Program p;
        p.setOutputDir(out);
        std::vector<std::string> inputs{(dir / "main.c").string()};
        for (auto const& o : cell.objects) inputs.push_back((dir / o).string());
        DiagnosticReporter rep;
        int const rc = p.compileFiles(inputs, "c", std::vector<std::string>{spec}, rep);
        if (cell.refused) {
            EXPECT_NE(rc, 0) << "DSS must refuse the pair GNU ld refuses";
            std::vector<std::string> named;
            for (auto const& d : rep.all()) {
                if (d.code == DiagnosticCode::K_ExternImportAttributeConflict) named.push_back(d.actual);
            }
            ASSERT_EQ(named.size(), 1u) << diagnosticsOf(rep);
            EXPECT_NE(named[0].find("symbol 'c'"), std::string::npos) << named[0];
            EXPECT_NE(named[0].find("holds it as an ORDINARY object"), std::string::npos) << named[0];
            EXPECT_NE(named[0].find("has THREAD STORAGE DURATION"), std::string::npos) << named[0];
            std::cout << "[native-arm] gcc (GNU ld) " << cell.name << ": GNU ld REFUSES, DSS REFUSES\n";
            continue;
        }
        ASSERT_EQ(rc, 0) << "CONTROL: DSS links the ordinary definition:" << diagnosticsOf(rep);
        fs::path image;
        for (auto const& e : fs::directory_iterator(out)) {
            if (e.is_regular_file() && e.path().extension().empty()) image = e.path();
        }
        ASSERT_FALSE(image.empty()) << "no image under " << out.string();
        auto const r = test_support::runBinary(image);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_EQ(r.exitCode, 0u) << "CONTROL: DSS, the ordinary definition";
    }
#elif defined(_WIN32)
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "unread-common-tls"};
    auto const dir    = scratch.path();
    auto const strict = test_support::readStrictArmVerdicts();
    ASSERT_FALSE(strict.malformed) << test_support::kStrictArmVerdictsEnv << "='" << strict.raw
                                   << "' is not a recognised value";
    auto const msvc = np::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) {
        // cl with link.exe is one of a Windows leg's reference toolchains: its absence is said, and a strict run
        // (the gate's) fails on it.
        if (strict.on) {
            FAIL() << "[native-arm] cl (link.exe): " << msvc.detail << " — the arm cannot run on this host, and "
                   << test_support::kStrictArmVerdictsEnv << " makes that a failure";
        }
        GTEST_SKIP() << "[native-arm] cl (link.exe): not run — " << msvc.detail;
    }
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const tools = np::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(tools.ready()) << tools.describe();
    // THE REFERENCE: an all-cl program, under link.exe.
    writeText(dir / "unread.c", "int c;\n");
    writeText(dir / "tls.c", "__declspec(thread) int c = 5;\n");
    writeText(dir / "reader.c", "extern __declspec(thread) int c;\nint get(void) { return c; }\n");
    writeText(dir / "main0.c", "int main(void) { return 0; }\n");
    writeText(dir / "main5.c", "int get(void);\nint main(void) { return get(); }\n");
    ASSERT_TRUE(runCapturing(&tools, dir, "cl /nologo /c /O2 /MD /GS- unread.c tls.c reader.c main0.c main5.c", "cl.txt"))
        << np::tailOf(dir / "cl.txt", 30, "cl");
    struct RefCell {
        char const*              name;
        char const*              main;
        std::vector<std::string> objects;
        unsigned                 expected;
    };
    for (RefCell const& cell : {RefCell{"unread_then_tls", "main0", {"unread.obj", "tls.obj"}, 0u},
                                RefCell{"tls_then_unread", "main0", {"tls.obj", "unread.obj"}, 0u},
                                RefCell{"with_a_thread_local_reader", "main5", {"unread.obj", "tls.obj", "reader.obj"}, 5u}}) {
        SCOPED_TRACE(std::string{"link.exe, "} + cell.name);
        std::string ref = std::string{"link /nologo /OUT:ref_"} + cell.name + ".exe " + cell.main + ".obj";
        for (auto const& o : cell.objects) ref += " " + o;
        ASSERT_TRUE(runCapturing(&tools, dir, ref, std::string{"ref_"} + cell.name + ".txt"))
            << "link.exe no longer links the pair: "
            << np::tailOf(dir / (std::string{"ref_"} + cell.name + ".txt"), 30, "link.exe");
        auto const refRun = test_support::runBinary(dir / (std::string{"ref_"} + cell.name + ".exe"));
        ASSERT_TRUE(refRun.spawned) << refRun.diagnostic;
        EXPECT_EQ(refRun.exitCode, cell.expected) << "link.exe no longer answers what was measured";
        std::cout << "[native-arm] cl (link.exe) " << cell.name << ": link.exe " << refRun.exitCode << "\n";
    }
    // The PREMISE of DSS's cells: cl wrote `int c;` as a COMMON nothing of its unit reads, and its record states no
    // storage duration.
    {
        auto const L = load("x86_64", "pe64-x86_64-windows");
        ASSERT_TRUE(L.target && L.format);
        DiagnosticReporter readRep;
        auto const unit = pe::readRelocatableObject(fileBytes(dir / "unread.obj"), *L.target, *L.format, readRep);
        ASSERT_TRUE(unit.has_value()) << diagnosticsOf(readRep);
        auto const* row = rowNamed(*unit, "c");
        ASSERT_NE(row, nullptr);
        EXPECT_EQ(row->commonSize, 4u) << "cl writes a tentative definition as a common";
        EXPECT_FALSE(row->recordStatesStorageDuration);
        for (auto const& f : unit->functions) {
            for (auto const& r : f.relocations) EXPECT_NE(r.target.v, row->symbol.v) << "nothing of the unit reads it";
        }
    }
    // DSS: cl's common nothing reads, beside a thread-local definition DSS ITSELF compiles — link.exe's answers (0,
    // and 5 where the program reads the variable as the thread-local it is). cl's own `tls.obj` cannot be the
    // definition here: DSS's COFF reader refuses a definition in `.tls$` at the read, common or no common
    // (✔MEASURED 2026-10-08; a disclosed row of its own, not this one's subject) —
    //     D-LK-FOREIGN-OBJECT-THREAD-LOCAL-DEFINITION-REFUSED-AT-READ
    // — so the definition is in the unit DSS compiles, which is every input of this pair DSS can read today.
    writeText(dir / "dmain0.c", "_Thread_local int c = 5;\nint main(void) { return 0; }\n");
    writeText(dir / "dmain5.c", "_Thread_local int c = 5;\nint main(void) { return c; }\n");
    struct DssCell {
        char const* name;
        char const* main;
        bool        besideTheCommon;
        unsigned    expected;   // link.exe's answer for the same program
    };
    for (DssCell const& cell : {DssCell{"the_common_beside_the_definition", "dmain0", true, 0u},
                                DssCell{"the_common_beside_the_definition_read", "dmain5", true, 5u},
                                DssCell{"CONTROL_the_definition_alone", "dmain0", false, 0u},
                                DssCell{"CONTROL_the_definition_alone_read", "dmain5", false, 5u}}) {
        SCOPED_TRACE(std::string{"DSS, "} + cell.name);
        auto const out = dir / (std::string{"dss_"} + cell.name);
        fs::create_directories(out);
        Program p;
        p.setOutputDir(out);
        std::vector<std::string> inputs{(dir / (std::string{cell.main} + ".c")).string()};
        if (cell.besideTheCommon) inputs.push_back((dir / "unread.obj").string());
        DiagnosticReporter rep;
        ASSERT_EQ(p.compileFiles(inputs, "c", std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, rep), 0)
            << "DSS must link the pair link.exe links:" << diagnosticsOf(rep);
        fs::path image;
        for (auto const& e : fs::directory_iterator(out)) {
            if (e.is_regular_file() && e.path().extension() == ".exe") image = e.path();
        }
        ASSERT_FALSE(image.empty()) << "no image under " << out.string();
        auto const r = test_support::runBinary(image);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, cell.expected) << "DSS must give link.exe's answer";
        std::cout << "[native-arm] cl's unread common, DSS's thread-local definition, " << cell.name << ": DSS "
                  << r.exitCode << " (link.exe: " << cell.expected << ")\n";
    }
#else
    GTEST_SKIP() << "the native cells are gcc's objects on the Linux legs and cl's on the Windows legs; on Apple's ld "
                    "the pair was measured (linked: 0, and 5 beside a thread-local reader), and the rule it follows — "
                    "a Mach-O record states no storage duration — is pinned on the Mach-O documents by "
                    "ThreadStorageAgreement.AnUnreadReferenceIsJudgedWhereItsRecordStatesItsStorageDuration";
#endif
}
