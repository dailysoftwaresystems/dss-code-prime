// D-C-TENTATIVE-DEFINITIONS-OF-ONE-NAME-IN-TWO-UNITS-REFUSED (P69 fold 3b, lane `xa`) — A UNIT DSS COMPILES WRITES
// ITS TENTATIVE DEFINITIONS AS COMMONS wherever its document states how a common is resolved.
//
// A tentative definition the link may fold (`int v;` at file scope: external, strong, not thread-local —
// `Mir::globalIsCommon`) used to reach every object DSS wrote as a zero-filled DEFINITION in `.bss`, so two DSS
// objects of one name were refused by every linker — DSS's own, GNU ld, ld.lld, link.exe, lld-link, Apple's ld —
// where the references' own objects link as ONE object: cl writes every tentative definition as a COMMON, Apple
// clang does by default, gcc and clang under `-fcommon` (✔MEASURED 2026-10-12, ELF x86_64, PE x86_64, Mach-O
// arm64/x86_64). The unit now states the COMMON row every object reader produces (`ExternImport::commonSize`): an
// image link allocates it once per name, a relocatable artifact or an archive member hands it on, and each writer
// spells it. Which documents: `ObjectFormatSchema::statesCommons` — a document that states how a common is
// resolved, by a link (`commonYieldsTo`) or by an archive's search (`archiveCommonResolution`).
//
//   * THE DOCUMENTS: every shipped document's answer, and the two keys that give it.
//   * THE UNIT: per document, the commons a unit states (size, alignment, visibility — the zero-fill definition's,
//     read off the same unit under a document that states no common) and what stays a definition: `static`, weak,
//     initialized, thread-local; the unit's own code reaches the common through its row. A document that states no
//     common keeps the definition — pinned on a DOCUMENT with the keys removed, never on a format's name.
//   * THE OBJECTS: each relocatable writer hands the commons on and its reader reads back the same rows.
//   * THE LINKS (DSS): two DSS objects of one name are one object — the largest, at the widest alignment; a source
//     beside a DSS object of the name, likewise; and DSS archives of commons, cell by cell as the references link
//     theirs (A a member whose only definition is a common, fetched for a reference; B two such members, one object;
//     C1/C2 a common member beside a definition in the main object, either side; D the members' document's
//     `archiveCommonResolution`).
//   * NATIVE: a DSS object holding a common, linked by the host's reference linker beside that reference's own
//     object, both orders — the TABLE's answer asserted on DSS's cell, the reference's own pair a CONTROL.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "asm/asm.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "ffi/abi/abi_catalog.hpp"
#include "link/format/coff_linker_directives.hpp"   // naturalCoffCommonAlignment
#include "link/object_format_schema.hpp"
#include "program/compile_pipeline.hpp"
#include "program/program.hpp"
#include "arm_verdict_ledger.hpp"
#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"
#include "host_native_target.hpp"
#include "host_translations.hpp"
#include "../core/native_c_probe.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s;
}

void writeText(fs::path const& p, std::string_view text) { std::ofstream(p, std::ios::binary) << text; }

// The document `name` as shipped, or with `removed` keys (and their `$<key>Comment`) erased: a document that states
// no common at all.
[[nodiscard]] std::shared_ptr<ObjectFormatSchema const> documentWithout(std::string const&              name,
                                                                         std::vector<std::string> const& removed) {
    auto const path = dss::test::configRoot() / "object-formats" / (name + ".format.json");
    std::ifstream in{path};
    auto doc = nlohmann::json::parse(in);
    for (auto const& key : removed) {
        doc.erase(key);
        doc.erase("$" + key + "Comment");
    }
    auto const loaded = ObjectFormatSchema::loadFromText(doc.dump(), name + " without its common keys");
    EXPECT_TRUE(loaded.has_value()) << name;
    return loaded.has_value() ? *loaded : nullptr;
}

[[nodiscard]] std::shared_ptr<ObjectFormatSchema const> shipped(std::string const& name) {
    auto const f = ObjectFormatSchema::loadShipped(name);
    EXPECT_TRUE(f.has_value()) << name;
    return f.has_value() ? *f : nullptr;
}

// ONE unit of C, assembled for `format` — the module the pipeline hands the link (no link, no write).
[[nodiscard]] std::optional<AssembledModule> assembled(std::string const& source, char const* target,
                                                       ObjectFormatSchema const& format, DiagnosticReporter& rep) {
    auto const t = TargetSchema::loadShipped(target);
    auto const g = GrammarSchema::loadShipped("c");
    if (!t.has_value() || !g.has_value()) {
        ADD_FAILURE() << "schemas of " << target << " / c";
        return std::nullopt;
    }
    auto const abi = dss::ffi::resolveAbi(**t, format, rep);
    if (!abi || abi->cc == nullptr) {
        ADD_FAILURE() << "no calling convention for " << target << " / " << format.name() << diagnosticsOf(rep);
        return std::nullopt;
    }
    auto const span = (*t)->callingConventions();
    auto const cc   = static_cast<std::uint16_t>(std::distance(span.data(), abi->cc));
    UnitBuilder builder{*g, DiagnosticBudget::libraryDefault()};
    builder.addInMemory(source, "unit.c");
    CompilationUnit cu = std::move(builder).finish();
    return assembleUnit(cu, **g, **t, format, cc, rep, CompileOptions{DiagnosticBudget::libraryDefault()});
}

[[nodiscard]] ExternImport const* commonNamed(AssembledModule const& m, std::string const& name) {
    for (auto const& e : m.externImports) {
        if (e.commonSize != 0u && e.mangledName == name) return &e;
    }
    return nullptr;
}
[[nodiscard]] ModuleSymbol const* rowNamed(AssembledModule const& m, std::string const& name) {
    for (auto const& s : m.symbols) {
        if (s.name == name) return &s;
    }
    return nullptr;
}
[[nodiscard]] AssembledData const* itemOf(AssembledModule const& m, SymbolId id) {
    for (auto const& d : m.dataItems) {
        if (d.symbol == id) return &d;
    }
    return nullptr;
}
[[nodiscard]] bool codeReaches(AssembledModule const& m, SymbolId id) {
    for (auto const& f : m.functions) {
        for (auto const& r : f.relocations) {
            if (r.target == id) return true;
        }
    }
    return false;
}

// The format's C name of `name` (Mach-O decorates it with `_`).
[[nodiscard]] std::string cName(ObjectFormatSchema const& f, std::string const& name) {
    return f.cSymbolDecoration().scheme == CSymbolDecorationScheme::LeadingUnderscore ? "_" + name : name;
}

// The unit of the per-document pins: six tentative definitions the link may fold, and five that it may not.
constexpr char const* kUnit =
    "int plain;\n"
    "const int constq;\n"
    "_Alignas(64) int wide;\n"
    "char big[40];\n"
    "__attribute__((visibility(\"hidden\"))) int hid;\n"
    "extern int decl;\n"
    "int decl;\n"
    "static int priv;\n"
    "__attribute__((weak)) int weakv;\n"
    "int init = 7;\n"
    "int zero = 0;\n"
    "int use(void) { return plain + constq + wide + big[1] + hid + decl + priv + weakv + init + zero; }\n"
    "void bump(void) { plain++; wide++; big[39]++; hid++; decl++; priv++; weakv++; }\n"
    // The image documents start a program: the unit carries its entry under every document alike.
    "int main(void) { bump(); return use(); }\n";

struct Want {
    char const*      name;
    SymbolVisibility visibility;
};
constexpr Want kCommons[] = {{"plain", SymbolVisibility::Default}, {"constq", SymbolVisibility::Default},
                             {"wide", SymbolVisibility::Default},  {"big", SymbolVisibility::Default},
                             {"hid", SymbolVisibility::Hidden},    {"decl", SymbolVisibility::Default}};
constexpr char const* kDefinitions[] = {"priv", "weakv", "init", "zero"};

// The documents the unit pins run under, with the target each is compiled for.
struct Doc {
    char const* format;
    char const* target;
};
constexpr Doc kDocs[] = {
    {"elf64-x86_64-linux", "x86_64"},          {"elf64-aarch64-linux", "arm64"},
    {"macho64-arm64-darwin", "arm64"},          {"macho64-x86_64-darwin", "x86_64"},
    {"pe64-x86_64-windows", "x86_64"},          {"elf64-x86_64-linux-exec", "x86_64"},
    {"macho64-arm64-darwin-exec", "arm64"},     {"pe64-x86_64-windows-exec", "x86_64"},
    {"elf64-x86_64-linux-staticlib", "x86_64"}, {"macho64-arm64-darwin-staticlib", "arm64"},
    {"pe64-x86_64-windows-staticlib", "x86_64"},
};

}  // namespace

// ══ The documents ════════════════════════════════════════════════════════════

// Every shipped document answers, and the answer is the two keys': a document that states how a common is resolved —
// by a link (`commonYieldsTo`) or by an archive's search (`archiveCommonResolution`) — writes commons; the two that
// state neither (SPIR-V, wasm) keep the definition.
TEST(TentativeDefinitionForms, EachShippedDocumentStatesItThroughTheKeysThatResolveACommon) {
    std::set<std::string> commons, definitions;
    for (auto const& e : fs::directory_iterator(dss::test::configRoot() / "object-formats")) {
        std::string const file = e.path().filename().string();
        if (!file.ends_with(".format.json")) continue;
        std::string const name = file.substr(0, file.size() - std::string_view{".format.json"}.size());
        std::ifstream in{e.path()};
        auto const doc  = nlohmann::json::parse(in);
        auto const f    = shipped(name);
        ASSERT_NE(f, nullptr) << name;
        bool const states = doc.contains("commonYieldsTo") || doc.contains("archiveCommonResolution");
        EXPECT_EQ(f->statesCommons(), states) << name;
        (f->statesCommons() ? commons : definitions).insert(name);
    }
    EXPECT_EQ(commons.size(), 22u) << "the 18 link documents and the 4 archive documents";
    EXPECT_EQ(definitions, (std::set<std::string>{"spirv-1.6", "wasm32-v1"}));
}

// ══ The unit ═════════════════════════════════════════════════════════════════

// Under every document that writes commons, the six tentative definitions become COMMON rows — each with the size
// and the alignment its zero-filled definition has under the same document with the keys removed (the explicit
// `_Alignas(64)` included), and its declared visibility — with no data item and no definition row left behind, and
// the unit's own code reaching each through the row. `static`, weak and initialized objects stay definitions.
TEST(TentativeDefinitionForms, AUnitWritesEachFoldableTentativeDefinitionAsACommonWhereItsDocumentDoes) {
    for (Doc const& d : kDocs) {
        SCOPED_TRACE(d.format);
        auto const f = shipped(d.format);
        ASSERT_NE(f, nullptr);
        ASSERT_TRUE(f->statesCommons());
        auto const without = documentWithout(d.format, {"commonYieldsTo", "archiveCommonResolution"});
        ASSERT_NE(without, nullptr);
        ASSERT_FALSE(without->statesCommons());
        DiagnosticReporter rep, repWithout;
        auto const m  = assembled(kUnit, d.target, *f, rep);
        auto const md = assembled(kUnit, d.target, *without, repWithout);
        ASSERT_TRUE(m.has_value()) << diagnosticsOf(rep);
        ASSERT_TRUE(md.has_value()) << diagnosticsOf(repWithout);
        for (Want const& w : kCommons) {
            SCOPED_TRACE(w.name);
            std::string const n = cName(*f, w.name);
            // The definition the document without the keys keeps: the measure of the common.
            ModuleSymbol const* const defRow = rowNamed(*md, n);
            ASSERT_NE(defRow, nullptr) << "without the keys the tentative definition is a definition row";
            AssembledData const* const defItem = itemOf(*md, defRow->symbol);
            ASSERT_NE(defItem, nullptr);
            EXPECT_EQ(defItem->section, DataSectionKind::Bss);
            EXPECT_EQ(commonNamed(*md, n), nullptr) << "a document that states no common keeps the definition";

            ExternImport const* const c = commonNamed(*m, n);
            ASSERT_NE(c, nullptr) << "the unit must state `" << n << "` as a COMMON:" << diagnosticsOf(rep);
            EXPECT_EQ(c->commonSize, defItem->reservedSize);
            EXPECT_EQ(c->commonAlignment, defItem->alignment.bytes());
            EXPECT_EQ(c->commonVisibility, w.visibility) << "the common keeps its definition's visibility";
            EXPECT_TRUE(c->isData);
            EXPECT_TRUE(c->libraryPath.empty());
            EXPECT_EQ(rowNamed(*m, n), nullptr) << "a common is no definition row";
            EXPECT_EQ(itemOf(*m, c->symbol), nullptr) << "a common has no storage of the unit's own";
            EXPECT_TRUE(codeReaches(*m, c->symbol)) << "the unit's own code reaches the common through its row";
        }
        ExternImport const* const wide = commonNamed(*m, cName(*f, "wide"));
        ASSERT_NE(wide, nullptr);
        EXPECT_EQ(wide->commonAlignment, 64u) << "`_Alignas(64)` is the common's alignment";
        for (char const* name : kDefinitions) {
            SCOPED_TRACE(name);
            std::string const n = cName(*f, name);
            EXPECT_EQ(commonNamed(*m, n), nullptr) << "a static, weak or initialized object is never a common";
            ModuleSymbol const* const row = rowNamed(*m, n);
            if (std::string_view{name} == "priv" && row == nullptr) continue;   // a local may go unnamed
            ASSERT_NE(row, nullptr);
            EXPECT_NE(itemOf(*m, row->symbol), nullptr) << "it keeps its storage";
        }
    }
}

// A thread-local tentative definition stays a definition — no reference writes one as a common, and two are refused
// (✔MEASURED 2026-10-12: GNU ld, ld.lld, link.exe, lld-link, Apple's ld) — beside a plain one that becomes a common
// in the same unit. The documents that carry thread-locals are the image ones.
TEST(TentativeDefinitionForms, AThreadLocalTentativeDefinitionStaysADefinition) {
    for (Doc const& d : {Doc{"elf64-x86_64-linux-exec", "x86_64"}, Doc{"elf64-aarch64-linux-exec", "arm64"},
                         Doc{"macho64-arm64-darwin-exec", "arm64"}, Doc{"pe64-x86_64-windows-exec", "x86_64"}}) {
        SCOPED_TRACE(d.format);
        auto const f = shipped(d.format);
        ASSERT_NE(f, nullptr);
        DiagnosticReporter rep;
        auto const m = assembled("_Thread_local int tl;\nint plain;\nint main(void) { return tl + plain; }\n",
                                 d.target, *f, rep);
        ASSERT_TRUE(m.has_value()) << diagnosticsOf(rep);
        EXPECT_EQ(commonNamed(*m, cName(*f, "tl")), nullptr);
        ModuleSymbol const* const row = rowNamed(*m, cName(*f, "tl"));
        ASSERT_NE(row, nullptr);
        AssembledData const* const item = itemOf(*m, row->symbol);
        ASSERT_NE(item, nullptr);
        EXPECT_EQ(item->section, DataSectionKind::Tbss);
        EXPECT_NE(commonNamed(*m, cName(*f, "plain")), nullptr) << "CONTROL: the plain one in the same unit";
    }
}

// ══ The objects ══════════════════════════════════════════════════════════════

namespace {

// The one regular file under `dir` whose extension is `ext` (empty: a file with none — an ELF or Mach-O image).
[[nodiscard]] fs::path onlyFileIn(fs::path const& dir, std::string_view ext) {
    fs::path    found;
    std::size_t n = 0;
    if (fs::exists(dir)) {
        for (auto const& e : fs::directory_iterator(dir)) {
            if (e.is_regular_file() && e.path().extension().string() == ext) {
                found = e.path();
                ++n;
            }
        }
    }
    return n == 1u ? found : fs::path{};
}

// DSS compiles `inputs` (sources and objects, in `dir`) for `spec` into `<dir>/<outName>/`; the artifact (its one
// file of extension `ext`), or empty.
[[nodiscard]] fs::path dssBuilds(fs::path const& dir, std::vector<std::string> const& inputs, std::string const& spec,
                                 std::string const& outName, std::string_view ext, DiagnosticReporter& rep,
                                 std::vector<fs::path> const& libraries = {}) {
    auto const out = dir / outName;
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    if (!libraries.empty()) p.setResolveLibraries(libraries);
    std::vector<std::string> paths;
    for (auto const& i : inputs) paths.push_back((dir / i).string());
    if (p.compileFiles(paths, "c", std::vector<std::string>{spec}, rep) != 0) return {};
    return onlyFileIn(out, ext);
}

struct Family {
    char const* label;
    char const* target;
    char const* object;
    char const* image;
    char const* archive;
    char const* objExt;
    char const* exeExt;
};

constexpr Family kFamilies[] = {
    {"ELF x86_64", "x86_64", "elf64-x86_64-linux", "elf64-x86_64-linux-exec", "elf64-x86_64-linux-staticlib", ".o", ""},
    {"ELF aarch64", "arm64", "elf64-aarch64-linux", "elf64-aarch64-linux-exec", "elf64-aarch64-linux-staticlib", ".o",
     ""},
    {"Mach-O arm64", "arm64", "macho64-arm64-darwin", "macho64-arm64-darwin-exec", "macho64-arm64-darwin-staticlib",
     ".o", ""},
    {"PE x86_64", "x86_64", "pe64-x86_64-windows", "pe64-x86_64-windows-exec", "pe64-x86_64-windows-staticlib",
     ".obj", ".exe"},
};

[[nodiscard]] std::string specOf(char const* target, char const* format) {
    return std::string{target} + ":" + format;
}

// Runs `image` where its family is the host's own (`hostNativeTarget`, the one chokepoint that says so): its exit
// code, or nullopt (built, not run).
[[nodiscard]] std::optional<std::uint32_t> ranTo(Family const& fam, fs::path const& image) {
    if (specOf(fam.target, fam.image) != test_support::hostNativeTarget().execTarget) return std::nullopt;
    auto const r = test_support::runBinary(image);
    EXPECT_TRUE(r.spawned) << fam.label << ": " << r.diagnostic;
    EXPECT_FALSE(r.timedOut) << fam.label;
    if (!r.spawned || r.timedOut) return std::nullopt;
    return r.exitCode;
}

// The accessor units: one object `v`, its address through `addr_<side>`.
[[nodiscard]] std::string accessor(char const* decl, char side) {
    return std::string{decl} + "\nvoid *addr_" + side + "(void) { return (void *)&v; }\n";
}
// 42 = both accessors name ONE object at a multiple of `wide`; 10 = two objects; 12 = one object, not at a multiple.
// Three pads, two of them tentative (`a_dss_pad` and `z_dss_pad`, whose names sort around `v`), so that an alignment
// a link gives by PLACEMENT alone — the one object at the start of its section — is not read as one it honoured: the
// shape whose CONTROL (the larger object alone) reads 12 under Apple's ld (✔MEASURED 2026-10-12, `gref2_macho.log`;
// without pads it read 42).
[[nodiscard]] std::string mainChecking(unsigned wide) {
    return "extern void *addr_a(void);\nextern void *addr_b(void);\n"
           "char a_dss_pad[3];\nchar z_dss_pad[5];\nchar dss_data_pad[3] = {1, 2, 3};\n"
           "int main(void) {\n  void *a = addr_a(), *b = addr_b();\n  if (a != b) return 10;\n"
           "  if ((unsigned long long)a % "
           + std::to_string(wide) + "u) return 12;\n  return 42;\n}\n";
}

}  // namespace

// Each relocatable writer hands the unit's commons on, and its reader reads back the rows the unit stated: size,
// alignment (on PE the 64 of `_Alignas(64)` rides the document's `commonAlignment` directive, wider than the 4 a
// COFF linker gives a 4-byte common), visibility; and what is no common reads back as no common.
TEST(TentativeDefinitionObjects, EachWriterHandsTheCommonsOnAndItsReaderReadsThemBack) {
    test_support::ScratchDir scratch{test_support::Location::Temp, "tentative-objects"};
    auto const dir = scratch.path();
    writeText(dir / "unit.c", kUnit);
    for (Doc const& d : {Doc{"elf64-x86_64-linux", "x86_64"}, Doc{"elf64-aarch64-linux", "arm64"},
                         Doc{"macho64-arm64-darwin", "arm64"}, Doc{"macho64-x86_64-darwin", "x86_64"},
                         Doc{"pe64-x86_64-windows", "x86_64"}}) {
        SCOPED_TRACE(d.format);
        auto const f = shipped(d.format);
        auto const t = TargetSchema::loadShipped(d.target);
        ASSERT_TRUE(f && t.has_value());
        DiagnosticReporter rep;
        auto const unit = assembled(kUnit, d.target, *f, rep);
        ASSERT_TRUE(unit.has_value()) << diagnosticsOf(rep);
        auto const obj = dssBuilds(dir, {"unit.c"}, specOf(d.target, d.format), std::string{"o-"} + d.format,
                                   std::string_view{d.format}.starts_with("pe") ? ".obj" : ".o", rep);
        ASSERT_FALSE(obj.empty()) << diagnosticsOf(rep);
        auto const back = readObjectInputModules(std::span<fs::path const>{&obj, 1}, **t, *f, rep);
        ASSERT_TRUE(back.has_value() && back->size() == 1u) << diagnosticsOf(rep);
        for (Want const& w : kCommons) {
            SCOPED_TRACE(w.name);
            std::string const   n    = cName(*f, w.name);
            ExternImport const* said = commonNamed(*unit, n);
            ExternImport const* read = commonNamed(back->front(), n);
            ASSERT_NE(said, nullptr);
            ASSERT_NE(read, nullptr) << "the object must hand the common on";
            EXPECT_EQ(read->commonSize, said->commonSize);
            // What each format SPELLS of a common's alignment, read back as its linkers read it — never less than
            // the unit said, so the alignment the unit states holds:
            //   * ELF: `st_value` is the alignment itself;
            //   * Mach-O: its log2 in four bits of `n_desc`, where 0 reads as "the size rounded up to a power of
            //     two, at most 2^15" (ld64's reading, which the reader shares) — a 1-byte alignment has no spelling
            //     of its own and comes back as that wider one;
            //   * COFF: nothing below what link.exe gives a common of its size (`naturalCoffCommonAlignment`), and
            //     a directive above it — so the reader aligns it at least that much.
            std::string_view const format{d.format};
            std::uint64_t          expected = said->commonAlignment;
            if (format.starts_with("macho") && said->commonAlignment == 1u) {
                expected = std::min<std::uint64_t>(std::bit_ceil(said->commonSize), std::uint64_t{1} << 15);
            }
            if (format.starts_with("pe")) {
                expected = std::max<std::uint64_t>(said->commonAlignment,
                                                   pe::naturalCoffCommonAlignment(said->commonSize));
            }
            EXPECT_GE(read->commonAlignment, said->commonAlignment) << "the unit's alignment holds";
            EXPECT_EQ(read->commonAlignment, expected);
            EXPECT_EQ(read->commonVisibility, said->commonVisibility);
        }
        for (char const* name : {"init", "zero", "weakv"}) {
            EXPECT_EQ(commonNamed(back->front(), cName(*f, name)), nullptr) << name;
        }
    }
}

// ══ The links DSS makes ══════════════════════════════════════════════════════

// Two DSS objects holding `v` link as ONE object — the shape every linker refused while a unit wrote `.bss`; with
// sizes 1 (`_Alignas(4096) char v;`) and 40 (`char v[40];`) it is the larger, at the wider alignment, the merge's
// rule; and a SOURCE beside a DSS object of the name, likewise (the shape refused by name as two strong
// definitions). Every family is built on every leg; each image runs where its family runs.
TEST(TentativeDefinitionLinks, TwoDssObjectsOfOneNameAreOneObject) {
    test_support::ScratchDir scratch{test_support::Location::Temp, "tentative-links"};
    auto const dir = scratch.path();
    writeText(dir / "a_plain.c", accessor("int v;", 'a'));
    writeText(dir / "b_plain.c", accessor("int v;", 'b'));
    writeText(dir / "a_small.c", accessor("_Alignas(4096) char v;", 'a'));
    writeText(dir / "b_large.c", accessor("char v[40];", 'b'));
    writeText(dir / "main4.c", mainChecking(4));
    writeText(dir / "main4096.c", mainChecking(4096));
    for (Family const& fam : kFamilies) {
        SCOPED_TRACE(fam.label);
        auto const objSpec = specOf(fam.target, fam.object);
        auto const objOf   = [&](char const* stem) {
            DiagnosticReporter rep;
            auto const o = dssBuilds(dir, {std::string{stem} + ".c"}, objSpec,
                                     std::string{"obj-"} + fam.object + "-" + stem, fam.objExt, rep);
            EXPECT_FALSE(o.empty()) << stem << diagnosticsOf(rep);
            if (o.empty()) return std::string{};
            auto const local = std::string{stem} + "-" + fam.object + fam.objExt;
            fs::copy_file(o, dir / local, fs::copy_options::overwrite_existing);
            return local;
        };
        std::string const aPlain = objOf("a_plain"), bPlain = objOf("b_plain");
        std::string const aSmall = objOf("a_small"), bLarge = objOf("b_large");
        ASSERT_FALSE(aPlain.empty() || bPlain.empty() || aSmall.empty() || bLarge.empty());
        struct Cell {
            char const*              name;
            std::vector<std::string> inputs;
        };
        std::vector<Cell> const cells{{"pair", {"main4.c", aPlain, bPlain}},
                                      {"pair_other_order", {"main4.c", bPlain, aPlain}},
                                      {"larger_wider", {"main4096.c", aSmall, bLarge}},
                                      {"larger_wider_other_order", {"main4096.c", bLarge, aSmall}},
                                      {"source_beside_object", {"main4.c", "a_plain.c", bPlain}}};
        for (Cell const& cell : cells) {
            SCOPED_TRACE(cell.name);
            DiagnosticReporter rep;
            auto const image = dssBuilds(dir, cell.inputs, specOf(fam.target, fam.image),
                                         std::string{"img-"} + fam.image + "-" + cell.name, fam.exeExt, rep);
            ASSERT_FALSE(image.empty()) << "DSS must link two units' commons of one name:" << diagnosticsOf(rep);
            if (auto const exit = ranTo(fam, image)) {
                EXPECT_EQ(*exit, 42u) << "10 = two objects, 12 = one object NOT at the alignment the cell asks";
            }
        }
    }
}

// DSS ARCHIVES of commons, cell by cell as the references link theirs (✔MEASURED 2026-10-15, `main.o lib.a`: GNU ld
// on ELF and PE, ld.lld, link.exe, lld-link, Apple's ld-1267; cl's, gcc -fcommon's and Apple clang's members):
//   A  a member whose only definition of `v` is a common is fetched for main's reference to `v` — 42 under every one
//      of them (`-ld_classic` alone refuses it: its archive's table of contents omits commons);
//   B  two such members, both fetched, are ONE object — 42 everywhere (refused by all as two definitions);
//   C1 main DEFINES `v = 7`, a common member fetched for `f` yields to it — 42;
//   C2 main holds `v` as a common, a member DEFINING `v = 7` fetched for `f` wins — 42;
//   D  main's common beside a member defining `v = 7` and nothing else main names: the members' document's
//      `archiveCommonResolution` — fetched (ELF, Mach-O: 42), kept (PE: 10), as GNU ld's ELF linker and Apple's ld
//      fetch it and link.exe, lld-link, GNU ld's PE linker and ld.lld keep the common.
// Before this fold, main's own `int v;` was a definition, so D read 10 on every format and B was refused.
TEST(TentativeDefinitionLinks, DssArchivesOfCommonsLinkAsTheReferencesDo) {
    test_support::ScratchDir scratch{test_support::Location::Temp, "tentative-archives"};
    auto const dir = scratch.path();
    struct Member {
        char const* name;
        char const* source;
    };
    struct Cell {
        char const*         name;
        char const*         mainSource;
        std::vector<Member> members;
    };
    std::vector<Cell> const cells{
        {"A", "extern int v;\nint main(void) { v = 5; return v == 5 ? 42 : 1; }\n", {{"a_com", "int v;\n"}}},
        {"B", "void *a1(void);\nvoid *a2(void);\nint main(void) { return a1() == a2() ? 42 : 10; }\n",
         {{"b_m1", "int v;\nvoid *a1(void) { return (void *)&v; }\n"},
          {"b_m2", "int v;\nvoid *a2(void) { return (void *)&v; }\n"}}},
        {"C1", "int v = 7;\nint f(void);\nint main(void) { return f() == 7 ? 42 : 10; }\n",
         {{"c1_m", "int v;\nint f(void) { return v; }\n"}}},
        {"C2", "int v;\nint f(void);\nint main(void) { return f() == 7 && v == 7 ? 42 : 10; }\n",
         {{"c2_m", "int v = 7;\nint f(void) { return v; }\n"}}},
        {"D", "int v;\nint main(void) { return v == 7 ? 42 : 10; }\n", {{"d_m", "int v = 7;\n"}}},
    };
    for (Family const& fam : kFamilies) {
        SCOPED_TRACE(fam.label);
        auto const archiveDoc = shipped(fam.archive);
        auto const t          = TargetSchema::loadShipped(fam.target);
        ASSERT_TRUE(archiveDoc && t.has_value());
        ASSERT_TRUE(archiveDoc->statesCommons());
        ASSERT_TRUE(archiveDoc->archiveCommonResolution().has_value());
        bool const fetches = *archiveDoc->archiveCommonResolution() == ArchiveCommonResolution::FetchDefinition;
        for (Cell const& cell : cells) {
            SCOPED_TRACE(cell.name);
            std::vector<AssembledModule> modules;
            std::vector<std::string>     names;
            for (Member const& m : cell.members) {
                DiagnosticReporter rep;
                auto mod = assembled(m.source, fam.target, *archiveDoc, rep);
                ASSERT_TRUE(mod.has_value()) << m.name << diagnosticsOf(rep);
                modules.push_back(std::move(*mod));
                names.push_back(std::string{m.name} + (std::string_view{fam.object}.starts_with("pe") ? ".obj" : ".o"));
            }
            auto const lib = dir / (std::string{"lib"} + cell.name + "-" + fam.archive
                                    + (std::string_view{fam.object}.starts_with("pe") ? ".lib" : ".a"));
            DiagnosticReporter arRep;
            ASSERT_TRUE(linkAndWriteStaticArchive(modules, names, **t, *archiveDoc, lib, arRep))
                << diagnosticsOf(arRep);
            writeText(dir / (std::string{"main_"} + cell.name + ".c"), cell.mainSource);
            DiagnosticReporter rep;
            auto const image = dssBuilds(dir, {std::string{"main_"} + cell.name + ".c"}, specOf(fam.target, fam.image),
                                         std::string{"arch-"} + cell.name + "-" + fam.image, fam.exeExt, rep, {lib});
            ASSERT_FALSE(image.empty()) << "DSS must link the archive cell:" << diagnosticsOf(rep);
            unsigned const want = std::string_view{cell.name} == "D" && !fetches ? 10u : 42u;
            if (auto const exit = ranTo(fam, image)) EXPECT_EQ(*exit, want);
        }
    }
}


// ══ Native: the reference linkers with a DSS object ══════════════════════════
//
// THE ROUND TRIP THAT MATTERS TO A USER: a DSS object holding `v` as a common, linked by the host's REFERENCE linker
// beside that reference compiler's own object of `v`, in both link orders. Every cell is asserted against the TABLE
// (what the reference's own objects gave in that cell when it was measured, with the versions it names); the
// reference's own objects in the same cell are its CONTROL, and a control that moved is said as the reference's
// change, never as DSS's (`reference moved:`, with the live tools' version lines).
//   pair        — `int v;` beside the reference's `int v;`: ONE object (42) under every linker.
//   dss_pair    — TWO DSS objects of `int v;` under the reference linker: ONE object (42), as its own pair is.
//   definition  — `int v;` beside the reference's `int v = 7;`: the common yields to it (42).
//   aligned     — `_Alignas(4096) char v;` beside the reference's `char v[40];`: one object, at 4096 where the
//                 linker keeps the widest alignment (42: GNU ld on ELF, and on PE, where it honours the object's
//                 `-aligncomm:v,12`), NOT at 4096 where it keeps the larger's own (12: Apple's ld, both orders, arm64
//                 and x86_64) or ignores the directive (12: link.exe, with LNK4229). ✔MEASURED on the references' own
//                 objects: ELF 2026-10-12 (gcc 13.3.0 and clang 18.1.3 under GNU ld 2.42 and ld.lld 18.1.3, 8 of 8),
//                 Mach-O 2026-10-12 (Apple clang 21.0.0, ld-1267, arm64 and x86_64), PE 2026-10-12 and 2026-10-15
//                 (MinGW gcc 13.2.0 under GNU ld 2.42; link.exe 14.44). gcc on aarch64 is held to x86_64's answer:
//                 NOT measured there — its control is the reading.
//   large_alone — the CONTROL of the aligned cell: the reference's `char v[40];` alone under the 4096 check reads 12,
//                 so the aligned cell can fail.
namespace {

struct NativeArm {
    std::string              label;
    std::string              measuredWith;      // the versions the table was measured with
    std::vector<std::string> versionCommands;   // the live tools' banners, read when a cell fails
    std::string              arch;              // the images' architecture (the cross-arch gate's key)
    std::string              dssSpec;           // DSS's relocatable object beside the reference's
    std::string              obj, exe;          // the reference's products' extensions
    unsigned                 aligned = 0;       // the table's `aligned` cell, both orders
    std::function<std::string(std::string const& stem)>                                        compile;   // a common
    std::function<std::string(std::string const& out, std::vector<std::string> const& inputs)> link;
    std::function<bool(fs::path const& dir, std::string const& cmd, std::string const& log)>   run;
};

struct NativeArms {
    std::vector<NativeArm>   arms;
    std::vector<std::string> absent;   // each with what was looked for
};

[[nodiscard, maybe_unused]] NativeArm gnuStyle(std::string label, std::string driver, std::string arch,
                                               std::string dssSpec, std::string exe, unsigned aligned) {
    NativeArm a;
    a.label   = std::move(label);
    a.arch    = std::move(arch);
    a.dssSpec = std::move(dssSpec);
    a.obj     = ".o";
    a.exe     = std::move(exe);
    a.aligned = aligned;
    a.compile = [driver](std::string const& stem) {
        return driver + " -O1 -fcommon -c " + stem + ".c -o " + stem + ".o";
    };
    a.link = [driver, ext = a.exe](std::string const& out, std::vector<std::string> const& inputs) {
        std::string c = driver + " -o " + out + ext;
        for (auto const& i : inputs) c += " " + i;
        return c;
    };
    return a;
}

[[nodiscard]] NativeArms nativeArms(fs::path const& work) {
    NativeArms found;
    namespace np = test_support::native_probe;
#if defined(__APPLE__) || (defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__)))
    (void)work;
    auto const posixRun = [](fs::path const& dir, std::string const& cmd, std::string const& log) {
        // LC_ALL=C: a reference tool's words are read as measured, whatever the leg's locale.
        return std::system(np::captureCmd("export LC_ALL=C; cd \"" + dir.string() + "\" && " + cmd, dir / log).c_str())
               == 0;
    };
    if (std::system("cc --version >/dev/null 2>&1") != 0) {
        found.absent.push_back("cc (the host's reference linker): no `cc` on PATH");
        return found;
    }
#if defined(__APPLE__)
    found.arms.push_back(gnuStyle("Apple clang -arch arm64 (Apple's ld)", "cc -arch arm64", "arm64",
                                  "arm64:macho64-arm64-darwin", "", 12));
    found.arms.push_back(gnuStyle("Apple clang -arch x86_64 (Apple's ld)", "cc -arch x86_64", "x86_64",
                                  "x86_64:macho64-x86_64-darwin", "", 12));
#elif defined(__x86_64__)
    found.arms.push_back(
        gnuStyle("gcc -fcommon (GNU ld, x86_64)", "cc", "x86_64", "x86_64:elf64-x86_64-linux", "", 42));
#else
    found.arms.push_back(
        gnuStyle("gcc -fcommon (GNU ld, aarch64)", "cc", "arm64", "arm64:elf64-aarch64-linux", "", 42));
#endif
    for (auto& a : found.arms) {
        a.run = posixRun;
#if defined(__APPLE__)
        a.measuredWith = "Apple clang 21.0.0 with ld-1267";
#else
        a.measuredWith = "gcc 13.3.0 with GNU ld 2.42";
#endif
        a.versionCommands = {"cc --version", "ld -v"};
    }
#elif defined(_WIN32)
    auto const msvc = np::locateMsvcToolchain(work);
    // Absent: this host's fact, judged by the caller (a strict run fails on it); present and not enterable: a
    // failure here and now.
    if (!msvc.toolAbsent() && !msvc.ok()) ADD_FAILURE() << msvc.describe();
    if (msvc.toolAbsent()) found.absent.push_back("cl (link.exe): " + msvc.detail);
    if (!msvc.toolAbsent() && msvc.ok()) {
        auto const tools = std::make_shared<np::MsvcTools>(np::msvcToolsIn(msvc, work));
        if (!tools->ready()) ADD_FAILURE() << tools->describe();
        NativeArm a;
        a.label           = "cl (link.exe)";
        a.measuredWith    = "cl 19.44 with link.exe 14.44";
        a.versionCommands = {"cl", "link"};
        a.arch            = "x86_64";
        a.dssSpec         = "x86_64:pe64-x86_64-windows";
        a.obj             = ".obj";
        a.exe             = ".exe";
        a.aligned         = 12;
        // cl writes EVERY tentative definition as a common, at every optimization level.
        a.compile = [](std::string const& stem) { return "cl /nologo /c /O1 /MD /Fo" + stem + ".obj " + stem + ".c"; };
        a.link    = [](std::string const& out, std::vector<std::string> const& inputs) {
            std::string c = "link /nologo /OUT:" + out + ".exe";
            for (auto const& i : inputs) c += " " + i;
            return c;
        };
        a.run = [tools](fs::path const& dir, std::string const& cmd, std::string const& log) {
            std::string const line = np::captureCmd("cd /d \"" + dir.string() + "\" && " + cmd, dir / log);
            return tools->ready() && np::systemUnder(tools->env, line) == 0;
        };
        found.arms.push_back(std::move(a));
    }
    if (std::system("where gcc >nul 2>&1") == 0) {
        auto a = gnuStyle("MinGW gcc -fcommon (GNU ld, PE)", "gcc", "x86_64", "x86_64:pe64-x86_64-windows", ".exe", 42);
        a.measuredWith    = "MinGW gcc 13.2.0 with GNU ld 2.42";
        a.versionCommands = {"gcc --version", "ld -v"};
        a.run = [](fs::path const& dir, std::string const& cmd, std::string const& log) {
            return std::system(np::captureCmd("cd /d \"" + dir.string() + "\" && " + cmd, dir / log).c_str()) == 0;
        };
        found.arms.push_back(std::move(a));
    } else {
        found.absent.push_back("MinGW gcc (GNU ld, PE): no `gcc` on PATH");
    }
#else
    (void)work;
#endif
    return found;
}

[[nodiscard]] std::string firstLineOf(fs::path const& p) {
    std::ifstream in{p};
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) return line;
    }
    return "(it printed nothing)";
}

// The live tools' version lines, asked only inside a failure's message (gtest builds it only when the assertion
// fails, so a green run pays nothing).
[[nodiscard]] std::string liveVersions(NativeArm const& arm, fs::path const& dir) {
    std::string out;
    for (std::size_t i = 0; i < arm.versionCommands.size(); ++i) {
        std::string const log = "live-version-" + std::to_string(i) + ".txt";
        (void)arm.run(dir, arm.versionCommands[i], log);   // a banner's exit status is not the question
        out += (i == 0 ? "[`" : "; `") + arm.versionCommands[i] + "`: " + firstLineOf(dir / log);
    }
    return out + "]";
}

}  // namespace

TEST(TentativeDefinitionsNative, ADssCommonBesideTheReferencesOwnObjectLinksUnderTheReferenceLinker) {
    namespace np = test_support::native_probe;
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "tentative-native"};
    auto const dir    = scratch.path();
    auto const found  = nativeArms(dir);
    auto const strict = test_support::readStrictArmVerdicts();
    ASSERT_FALSE(strict.malformed) << test_support::kStrictArmVerdictsEnv << "='" << strict.raw
                                   << "' is not a recognised value";
    // A reference toolchain this host does not have is its arm NOT RUN — said, never dropped: a strict run (the
    // gate's) fails on it; any other run prints it.
    for (auto const& missing : found.absent) {
        if (strict.on) {
            ADD_FAILURE() << "[native-arm] " << missing << " — the arm cannot run on this host, and "
                          << test_support::kStrictArmVerdictsEnv << " makes that a failure";
        } else {
            std::cout << "[native-arm] " << missing << ": not run — no such toolchain on this host\n";
        }
    }
    if (found.arms.empty() && !HasFailure()) {
        GTEST_SKIP() << "no reference C toolchain on this host (see the [native-arm] lines)";
    }
    for (std::size_t k = 0; k < found.arms.size(); ++k) {
        NativeArm const& arm = found.arms[k];
        SCOPED_TRACE(arm.label);
        // How THIS host runs the arm's images: natively, or through the one cross-arch gate.
        std::vector<std::string> launcher;
        bool                     launcherExecsImage = false;
        if (arm.arch != test_support::currentHostArch()) {
            auto const gate = test_support::crossArchDecisionForThisHost(arm.arch, /*manifestEmulator=*/"");
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
        fs::path const d = dir / ("arm" + std::to_string(k));
        fs::create_directories(d);
        writeText(d / "dss_plain.c", accessor("int v;", 'a'));
        writeText(d / "dss_plain_b.c", accessor("int v;", 'b'));
        writeText(d / "dss_small.c", accessor("_Alignas(4096) char v;", 'a'));
        writeText(d / "ref_plain_a.c", accessor("int v;", 'a'));
        writeText(d / "ref_small_a.c", accessor("_Alignas(4096) char v;", 'a'));
        writeText(d / "ref_large_a.c", accessor("char v[40];", 'a'));
        writeText(d / "ref_plain.c", accessor("int v;", 'b'));
        writeText(d / "ref_defined.c", accessor("int v = 7;", 'b'));
        writeText(d / "ref_large.c", accessor("char v[40];", 'b'));
        writeText(d / "ref_echo.c", "extern void *addr_a(void);\nvoid *addr_b(void) { return addr_a(); }\n");
        writeText(d / "main4.c", mainChecking(4));
        writeText(d / "main4096.c", mainChecking(4096));
        for (char const* stem : {"ref_plain_a", "ref_small_a", "ref_large_a", "ref_plain", "ref_defined", "ref_large",
                                 "ref_echo", "main4", "main4096"}) {
            std::string const log = std::string{stem} + "-cc.txt";
            ASSERT_TRUE(arm.run(d, arm.compile(stem), log)) << np::tailOf(d / log, 20, arm.label);
        }
        for (char const* stem : {"dss_plain", "dss_plain_b", "dss_small"}) {
            DiagnosticReporter rep;
            auto const o =
                dssBuilds(d, {std::string{stem} + ".c"}, arm.dssSpec, std::string{"out-"} + stem, arm.obj, rep);
            ASSERT_FALSE(o.empty()) << stem << diagnosticsOf(rep);
            fs::copy_file(o, d / (std::string{stem} + arm.obj), fs::copy_options::overwrite_existing);
        }
        // The reference's link of `inputs` into `out`, run: its exit code, or nullopt after saying why not.
        auto const linkAndRun = [&](std::string const& out,
                                    std::vector<std::string> const& inputs) -> std::optional<unsigned> {
            std::string const log = out + "-link.txt";
            if (!arm.run(d, arm.link(out, inputs), log)) {
                ADD_FAILURE() << arm.label << " refused `" << out << "`" << np::tailOf(d / log, 20, arm.label);
                return std::nullopt;
            }
            auto const r = test_support::runBinary(d / (out + arm.exe), test_support::kRunBudget,
                                                   /*captureStdout=*/false, launcher, /*programArgs=*/{},
                                                   launcherExecsImage);
            EXPECT_TRUE(r.spawned) << out << ": " << r.diagnostic;
            EXPECT_FALSE(r.timedOut) << out;
            if (!r.spawned || r.timedOut) return std::nullopt;
            return static_cast<unsigned>(r.exitCode);
        };
        auto const o = [&](char const* stem) { return std::string{stem} + arm.obj; };
        struct Cell {
            char const*              name;
            unsigned                 table;
            std::vector<std::string> reference;   // the CONTROL: the reference's own objects
            std::vector<std::string> dss;         // DSS's object(s) in the reference's place, then the other order
            std::vector<std::string> dssReversed;
        };
        std::vector<Cell> const cells{
            {"pair", 42u, {o("main4"), o("ref_plain_a"), o("ref_plain")}, {o("main4"), o("dss_plain"), o("ref_plain")},
             {o("main4"), o("ref_plain"), o("dss_plain")}},
            {"dss_pair", 42u, {o("main4"), o("ref_plain_a"), o("ref_plain")},
             {o("main4"), o("dss_plain"), o("dss_plain_b")}, {o("main4"), o("dss_plain_b"), o("dss_plain")}},
            {"definition", 42u, {o("main4"), o("ref_plain_a"), o("ref_defined")},
             {o("main4"), o("dss_plain"), o("ref_defined")}, {o("main4"), o("ref_defined"), o("dss_plain")}},
            {"aligned", arm.aligned, {o("main4096"), o("ref_small_a"), o("ref_large")},
             {o("main4096"), o("dss_small"), o("ref_large")}, {o("main4096"), o("ref_large"), o("dss_small")}},
            {"large_alone", 12u, {o("main4096"), o("ref_large_a"), o("ref_echo")}, {}, {}},
        };
        for (Cell const& cell : cells) {
            SCOPED_TRACE(cell.name);
            if (auto const control = linkAndRun(std::string{"ref_"} + cell.name, cell.reference)) {
                EXPECT_EQ(*control, cell.table)
                    << "reference moved: " << arm.label << " " << liveVersions(arm, d) << " answered " << *control
                    << " in the cell `" << cell.name << "`, the table (measured with " << arm.measuredWith
                    << ") says " << cell.table
                    << " — the REFERENCE's answer changed under the table, which is NOT DSS differing from the "
                       "reference: read what the live tool does, then restate the table and the row";
            }
            int order = 0;
            for (auto const* inputs : {&cell.dss, &cell.dssReversed}) {
                ++order;
                if (inputs->empty()) continue;
                std::string const out = std::string{"dss_"} + cell.name + "_" + std::to_string(order);
                if (auto const exit = linkAndRun(out, *inputs)) {
                    EXPECT_EQ(*exit, cell.table)
                        << "a DSS object's common under " << arm.label << " in the cell `" << cell.name
                        << "` (order " << order << ") answered " << *exit << ", the table says " << cell.table
                        << " (10 = two objects, 12 = one object not at 4096)";
                }
            }
        }
    }
}

// A tentative definition beside a definition of its name SMALLER or LESS ALIGNED than it — the shape that was SILENT
// (the review of fold 3a, F4): the definition stands, and the tentative definition's unit reads and writes the object
// as the one it declared. Every reference linker links it (✔MEASURED 2026-10-15, `int v[1] = {1};` beside
// `int v[10];` whose unit writes v[9], and `char w = 1;` beside `_Alignas(64) char w;`: Apple's ld-1267 warns
// "tentative definition of '_v' with size 40 ... is being replaced by real definition of smaller size 4", GNU ld
// 2.42 warns of the alignment and, in one order, of the size; ld.lld 18.1.3, link.exe 14.44, lld-link 19.1.5 and
// MinGW's GNU ld say nothing — and clang's objects under ld.lld crash). DSS links it as they do and SAYS it
// (`K_CommonSymbolLargerThanItsDefinition`, a warning) at both of its doors: the unit linker (each source its own
// object) and the merge of sources compiled together. A tentative definition no larger and no more aligned than the
// definition is not said — nor one whose only "wider" alignment is the one its FORMAT derives from its size (a COFF
// common's, which no unit states: `int v[10];` beside `int v[10] = {1};` on PE). Built on every leg, never run: what
// the program does is the overflow.
namespace {

[[nodiscard]] std::size_t saidLarger(DiagnosticReporter const& rep, std::string const& name, std::string_view what) {
    std::size_t n = 0;
    for (auto const& d : rep.all()) {
        if (d.code != DiagnosticCode::K_CommonSymbolLargerThanItsDefinition) continue;
        EXPECT_EQ(d.severity, DiagnosticSeverity::Warning) << "said, never refused: every reference links it";
        if (d.actual.find("'" + name + "'") != std::string::npos && d.actual.find(what) != std::string::npos) ++n;
    }
    return n;
}

}  // namespace

TEST(TentativeDefinitionLinks, ATentativeDefinitionLargerThanTheDefinitionItYieldsToLinksAndIsSaid) {
    test_support::ScratchDir scratch{test_support::Location::Temp, "tentative-larger"};
    auto const               dir = scratch.path();
    writeText(dir / "def.c", "int v[1] = {1};\nchar w = 1;\n");
    writeText(dir / "com.c", "int v[10];\nvoid touch(void) { v[9] = 77; }\n");
    writeText(dir / "com_wide.c", "_Alignas(64) char w;\nvoid touch(void) { w = 3; }\n");
    writeText(dir / "com_same.c", "int v[1];\nvoid touch(void) { v[0] = 2; }\n");
    writeText(dir / "def_array.c", "int v[10] = {1};\nchar w = 1;\n");
    writeText(dir / "com_array.c", "int v[10];\nvoid touch(void) { v[9] = 2; }\n");
    writeText(dir / "def_bytes.c", "int v[1] = {1};\nchar b[40] = {1};\n");
    writeText(dir / "com_bytes.c", "char b[40];\nvoid touch(void) { b[39] = 2; }\n");
    writeText(dir / "main.c", "extern int v[];\nvoid touch(void);\nint main(void) { touch(); return v[0]; }\n");
    for (Family const& fam : kFamilies) {
        SCOPED_TRACE(fam.label);
        auto const f = shipped(fam.image);
        ASSERT_NE(f, nullptr);
        std::string const v = cName(*f, "v"), w = cName(*f, "w"), b = cName(*f, "b");
        struct Cell {
            char const* name;
            char const* definition;
            char const* tentative;
            std::string named;
            char const* what;   // in the sentence: the tentative definition's extent and alignment
            bool        said;
        };
        // The larger cell's sentence names the size alone: the alignment said is the one the common's object
        // STATES, which a COFF object states for `int v[10];` nowhere (the unit linker's door), while the merge reads
        // the unit's own layout.
        for (Cell const& cell : {Cell{"larger", "def.c", "com.c", v, "is 40 bytes", true},
                                 Cell{"wider", "def.c", "com_wide.c", w, "aligned to 64", true},
                                 Cell{"same", "def.c", "com_same.c", v, "", false},
                                 Cell{"array", "def_array.c", "com_array.c", v, "", false},
                                 Cell{"bytes", "def_bytes.c", "com_bytes.c", b, "", false}}) {
            SCOPED_TRACE(cell.name);
            // (1) each source its own object: the unit linker's door.
            DiagnosticReporter objRep;
            auto const objOf = [&](char const* source) {
                auto const o = dssBuilds(dir, {source}, specOf(fam.target, fam.object),
                                         std::string{"obj-"} + cell.name + "-" + fam.object + "-" + source,
                                         fam.objExt, objRep);
                EXPECT_FALSE(o.empty()) << source << diagnosticsOf(objRep);
                if (o.empty()) return std::string{};
                auto const local = std::string{cell.name} + "-" + source + "-" + fam.object + fam.objExt;
                fs::copy_file(o, dir / local, fs::copy_options::overwrite_existing);
                return local;
            };
            std::string const defObj = objOf(cell.definition), comObj = objOf(cell.tentative);
            ASSERT_FALSE(defObj.empty() || comObj.empty());
            for (bool const definitionFirst : {true, false}) {
                SCOPED_TRACE(definitionFirst ? "the objects, the definition first" : "the objects, the common first");
                DiagnosticReporter rep;
                std::vector<std::string> inputs{"main.c"};
                inputs.push_back(definitionFirst ? defObj : comObj);
                inputs.push_back(definitionFirst ? comObj : defObj);
                auto const image = dssBuilds(dir, inputs, specOf(fam.target, fam.image),
                                             std::string{"img-"} + cell.name + "-" + fam.image
                                                 + (definitionFirst ? "-def" : "-com"),
                                             fam.exeExt, rep);
                ASSERT_FALSE(image.empty()) << "every reference linker links it:" << diagnosticsOf(rep);
                if (cell.said) {
                    EXPECT_EQ(saidLarger(rep, cell.named, cell.what), 1u) << diagnosticsOf(rep);
                } else {
                    EXPECT_EQ(saidLarger(rep, cell.named, ""), 0u) << diagnosticsOf(rep);
                }
            }
            // (2) the sources compiled together: the merge's door.
            DiagnosticReporter rep;
            auto const image = dssBuilds(dir, {"main.c", cell.definition, cell.tentative}, specOf(fam.target, fam.image),
                                         std::string{"img-"} + cell.name + "-" + fam.image + "-sources", fam.exeExt,
                                         rep);
            ASSERT_FALSE(image.empty()) << "every reference linker links it:" << diagnosticsOf(rep);
            if (cell.said) {
                EXPECT_EQ(saidLarger(rep, cell.named, cell.what), 1u) << diagnosticsOf(rep);
            } else {
                EXPECT_EQ(saidLarger(rep, cell.named, ""), 0u) << diagnosticsOf(rep);
            }
        }
    }
}
