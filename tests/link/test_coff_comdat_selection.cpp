// P69 fold 3d, lane p69/xa — D-LK-COFF-COMDAT-UNSUPPORTED-SELECTION: a COFF COMDAT that goes with another
// (ASSOCIATIVE), read as the section group it is and written back as one.
//
// clang-cl writes a dense switch's jump table in an `.rdata` COMDAT ASSOCIATIVE to its function's `.text` COMDAT:
// "linked iff the section it names is". Until P69 fold 3d the COFF reader refused every such object at read
// (F_CorruptedBinary, "COMDAT Selection 5"). Now (`coff_object_reader.cpp`, Gate 3):
//   * a table that goes with a function defined ONCE (selection NODUPLICATES) is an ordinary section of its unit;
//   * a table that goes with one COPY of several (selection ANY, an external COMDAT symbol) is a member of that
//     copy's SECTION GROUP, signed by the name -- so a link keeps the table of the copy it keeps and discards the
//     other copy's with it (`linker.cpp`, `discardLaterSectionGroups`);
// and the PE relocatable writer (`pe.cpp`, the group block at the data layouts) writes a kept group back: the copy
// its ANY COMDAT, the table a COMDAT ASSOCIATIVE to it.
//
// The fixtures are clang-cl's own objects (`tests/link/data/coff_comdat_{switch,inline}.source.c` state how each was
// built):
//
//   coff_comdat_switch_clangcl_O2.obj    md5 aaf7224524e10cb934405b29f7d7ad37   clang-cl 19.1.5 /O2 /GS-
//   coff_comdat_inline_a_clangcl_O2.obj  md5 483a68e38af0934746055fc6af1e9a31   clang-cl 19.1.5 /O2 /GS- /DUNIT=a
//   coff_comdat_inline_b_clangcl_O2.obj  md5 4b7baa967ac0c7357ebd1ce88f783f30   clang-cl 19.1.5 /O2 /GS- /DUNIT=b
//
// ✔MEASURED at the build (2026-10-21): the switch object holds `.text` #4 NODUPLICATES and `.rdata` #7 ASSOCIATIVE to
// #4 (ten relocations); each inline object `.text` #5 ANY (`pick`) and `.rdata` #8 ASSOCIATIVE to #5. Beside a cl
// `main`, link.exe 14.44 and lld-link 19.1.5 each link the switch object, and both inline objects in either order,
// to programs that exit 42. No guard enforces an md5: it records WHICH BYTES were measured (clang-cl stamps the
// object's TimeDateStamp, so a rebuild differs from these in those four bytes alone — ✔MEASURED 2026-10-21).
//
// ── GCC'S TABLE: a PC-RELATIVE field in a DATA section (`CoffJumpTable`) ──────────────────────────────────────────────
// The same switch under MinGW-w64 gcc, which writes no COMDAT and keeps the table in a plain `.rdata`, each entry the
// distance from the table to its case: ten IMAGE_REL_AMD64_REL32 relocations against `.text` (clang-cl's tables above
// are REL32 too). Until P69 fold 3d the PE image writer refused every pc-relative data field ("is pc-relative"), and
// the relocatable writer did too; each now writes the schema formula's value (`linearFieldValue`), from the field.
//
//   coff_comdat_switch_gcc_O0.obj  md5 36cbfac80c0c75ea94f997e1f882d42b  MinGW-w64 gcc 13.2.0 (x86_64-ucrt-posix-seh) -O0
//   coff_comdat_switch_gcc_O1.obj  md5 240daecfc9d6dde14cf9279f0a16f60b  MinGW-w64 gcc 13.2.0 (x86_64-ucrt-posix-seh) -O1
//
// ✔MEASURED at the build (2026-10-21; each built twice, byte-identical): `.rdata` ten REL32, `.pdata` three ADDR32NB;
// gcc's own program beside the switch `main`, and link.exe 14.44's and lld-link 19.1.5's beside a cl `main`, each exit
// 42. DSS at b5efdbb3 refused both objects: "data-item SymbolId #4 relocation 'rel32' is pc-relative".
//
// Every TEST but the native suites runs in-process on every leg: the fixtures are read, never compiled.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "program/program.hpp"

#include "../core/native_c_probe.hpp"
#include "host_native_target.hpp"
#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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

constexpr char const* kSwitch  = "coff_comdat_switch_clangcl_O2.obj";
constexpr char const* kInlineA = "coff_comdat_inline_a_clangcl_O2.obj";
constexpr char const* kInlineB = "coff_comdat_inline_b_clangcl_O2.obj";
constexpr char const* kGccO0   = "coff_comdat_switch_gcc_O0.obj";
constexpr char const* kGccO1   = "coff_comdat_switch_gcc_O1.obj";

[[nodiscard]] std::vector<std::uint8_t> bytesOf(fs::path const& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in.good()) {
        ADD_FAILURE() << "cannot open " << path.string();
        return {};
    }
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::vector<std::uint8_t> fixture(std::string_view name) {
    return bytesOf(dss::test::repoRoot() / "tests" / "link" / "data" / std::string{name});
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  <no diagnostic>"} : s;
}

struct Loaded {
    std::shared_ptr<TargetSchema const>       target;
    std::shared_ptr<ObjectFormatSchema const> object;   // pe64-x86_64-windows, the relocatable document
};

[[nodiscard]] Loaded load() {
    Loaded l;
    if (auto t = TargetSchema::loadShipped("x86_64"); t.has_value()) l.target = *t;
    if (auto f = ObjectFormatSchema::loadShipped("pe64-x86_64-windows"); f.has_value()) l.object = *f;
    EXPECT_TRUE(l.target && l.object) << "the shipped x86_64 target and pe64 object documents";
    return l;
}

[[nodiscard]] std::optional<AssembledModule> read(Loaded const& l, std::span<std::uint8_t const> bytes,
                                                  std::uint32_t cu, DiagnosticReporter& rep) {
    return pe::readRelocatableObject(bytes, *l.target, *l.object, rep, CompilationUnitId{cu});
}

// The input section the unit's function `name` was cut from.
[[nodiscard]] std::optional<std::uint32_t> sectionOfFunction(AssembledModule const& m, std::string_view name) {
    for (auto const& ms : m.symbols) {
        if (ms.name != name) continue;
        for (auto const& fn : m.functions) {
            if (fn.symbol == ms.symbol && fn.inputSection.has_value()) return fn.inputSection->section;
        }
    }
    return std::nullopt;
}

// The input sections the unit's data items were cut from.
[[nodiscard]] std::vector<std::uint32_t> dataSections(AssembledModule const& m) {
    std::vector<std::uint32_t> out;
    for (auto const& d : m.dataItems) {
        if (d.inputSection.has_value()) out.push_back(d.inputSection->section);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// ── A small COFF reader of the test's own, so the writer is checked against what the OBJECT says, not against the
//    reader under test. ───────────────────────────────────────────────────────────────────────────────────────────
struct CoffSection {
    std::string   name;
    std::uint32_t size  = 0;
    std::uint32_t chars = 0;
    // From the section's own section-definition record (aux format 5), when it has one.
    std::optional<std::uint8_t> selection;
    std::uint16_t number = 0;
    std::string   comdatSymbol;   // the first symbol after the section's record defined in it
    std::vector<std::uint16_t> relocationTypes;   // each IMAGE_RELOCATION's Type, in the section's table
};

[[nodiscard]] std::uint16_t u16(std::vector<std::uint8_t> const& b, std::size_t o) {
    return static_cast<std::uint16_t>(b[o] | (b[o + 1] << 8));
}
[[nodiscard]] std::uint32_t u32(std::vector<std::uint8_t> const& b, std::size_t o) {
    return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8)
         | (static_cast<std::uint32_t>(b[o + 2]) << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}

[[nodiscard]] std::vector<CoffSection> sectionsOf(std::vector<std::uint8_t> const& b) {
    std::vector<CoffSection> out;
    if (b.size() < 20u) {
        ADD_FAILURE() << "not a COFF object: " << b.size() << " bytes";
        return out;
    }
    std::uint16_t const nsec   = u16(b, 2);
    std::uint32_t const symPtr = u32(b, 8);
    std::uint32_t const nsym   = u32(b, 12);
    std::size_t const   strTab = static_cast<std::size_t>(symPtr) + static_cast<std::size_t>(nsym) * 18u;
    auto const nameAt = [&](std::size_t o) -> std::string {
        if (u32(b, o) == 0u) {   // a long name: an offset into the string table
            std::size_t p = strTab + u32(b, o + 4);
            std::string s;
            while (p < b.size() && b[p] != 0u) s.push_back(static_cast<char>(b[p++]));
            return s;
        }
        std::string s;
        for (std::size_t i = 0; i < 8u && b[o + i] != 0u; ++i) s.push_back(static_cast<char>(b[o + i]));
        return s;
    };
    for (std::uint16_t i = 0; i < nsec; ++i) {
        std::size_t const h = 20u + u16(b, 16) + 40u * i;
        CoffSection s;
        std::string raw;
        for (std::size_t k = 0; k < 8u && b[h + k] != 0u; ++k) raw.push_back(static_cast<char>(b[h + k]));
        if (!raw.empty() && raw[0] == '/') {   // "/<decimal offset>" into the string table
            std::size_t p = strTab + static_cast<std::size_t>(std::stoul(raw.substr(1)));
            raw.clear();
            while (p < b.size() && b[p] != 0u) raw.push_back(static_cast<char>(b[p++]));
        }
        s.name  = raw;
        s.size  = u32(b, h + 16);
        s.chars = u32(b, h + 36);
        std::uint32_t const relPtr = u32(b, h + 24);
        std::uint16_t const nrel   = u16(b, h + 32);
        for (std::uint16_t r = 0; r < nrel; ++r) {
            std::size_t const at = static_cast<std::size_t>(relPtr) + 10u * r;   // VirtualAddress, SymbolTableIndex, Type
            if (at + 10u > b.size()) {
                ADD_FAILURE() << "section '" << s.name << "' states relocation #" << r << " past the object's end";
                break;
            }
            s.relocationTypes.push_back(u16(b, at + 8));
        }
        out.push_back(std::move(s));
    }
    std::vector<bool> awaiting(nsec + 1u, false);
    for (std::uint32_t k = 0; k < nsym;) {
        std::size_t const   o     = static_cast<std::size_t>(symPtr) + static_cast<std::size_t>(k) * 18u;
        std::string const   name  = nameAt(o);
        auto const          sect  = static_cast<std::int16_t>(u16(b, o + 12));
        std::uint8_t const  naux  = b[o + 17];
        if (sect >= 1 && sect <= nsec) {
            CoffSection& s = out[static_cast<std::size_t>(sect) - 1u];
            if (!s.selection.has_value() && naux >= 1u && name == s.name && b[o + 16] == 3u /*STATIC*/) {
                std::size_t const a = o + 18u;
                s.number    = u16(b, a + 12);
                s.selection = b[a + 14];
                awaiting[static_cast<std::size_t>(sect)] = true;
            } else if (awaiting[static_cast<std::size_t>(sect)]) {
                s.comdatSymbol = name;
                awaiting[static_cast<std::size_t>(sect)] = false;
            }
        }
        k += 1u + naux;
    }
    return out;
}

constexpr std::uint32_t kScnLnkComdat = 0x00001000u;
constexpr std::uint8_t  kSelAny = 2, kSelAssociative = 5;

}  // namespace

// ══ THE READ ═════════════════════════════════════════════════════════════════════════════════════════════════════════

// A table that goes with a function defined ONCE is an ordinary section of its unit: read, in no group. RED-ON-DISABLE:
// refuse ASSOCIATIVE (the reader before P69 fold 3d) -> the object does not read.
TEST(CoffComdatSelection, TheSwitchObjectsTableIsAnOrdinarySectionOfItsUnit) {
    auto const l = load();
    ASSERT_TRUE(l.target && l.object);
    auto const bytes = fixture(kSwitch);
    ASSERT_FALSE(bytes.empty());
    DiagnosticReporter rep;
    auto const unit = read(l, bytes, 1, rep);
    ASSERT_TRUE(unit.has_value()) << diagnosticsOf(rep);
    auto const pick = sectionOfFunction(*unit, "pick");
    ASSERT_TRUE(pick.has_value());
    auto const tables = dataSections(*unit);
    EXPECT_FALSE(tables.empty()) << "the jump table is read as an item of its own section";
    EXPECT_EQ(std::count(tables.begin(), tables.end(), *pick), 0) << "the table is not cut from the function's section";
    EXPECT_TRUE(unit->sectionGroupOfInputSection.empty())
        << "the function is never one of several copies, so nothing is discarded with it";
}

// Each inline unit carries ONE copy of `pick` and its table, as ONE group signed `pick`. RED-ON-DISABLE: state no
// group for the associative table -> the table's section reads ungrouped.
TEST(CoffComdatSelection, EachInlineUnitStatesItsCopyAndItsTableAsOneGroup) {
    auto const l = load();
    ASSERT_TRUE(l.target && l.object);
    for (auto const* name : {kInlineA, kInlineB}) {
        SCOPED_TRACE(name);
        auto const bytes = fixture(name);
        ASSERT_FALSE(bytes.empty());
        DiagnosticReporter rep;
        auto const unit = read(l, bytes, 1, rep);
        ASSERT_TRUE(unit.has_value()) << diagnosticsOf(rep);
        auto const pick = sectionOfFunction(*unit, "pick");
        ASSERT_TRUE(pick.has_value());
        ASSERT_EQ(unit->sectionGroupOfInputSection.size(), 2u) << "the copy's section and its table's";
        for (auto const& [section, signature] : unit->sectionGroupOfInputSection) {
            EXPECT_EQ(signature, "pick") << "section #" << section;
        }
        EXPECT_TRUE(unit->sectionGroupOfInputSection.contains(*pick));
        auto const tables = dataSections(*unit);
        ASSERT_EQ(tables.size(), 1u) << "one table";
        EXPECT_TRUE(unit->sectionGroupOfInputSection.contains(tables.front()));
    }
}

// ══ THE RELOCATABLE ARTIFACT: the kept group written back ════════════════════════════════════════════════════════════

namespace {

// The PE relocatable artifact DSS links from `names`, or empty after a failed assertion.
[[nodiscard]] std::vector<std::uint8_t> artifactOf(std::vector<char const*> const& names) {
    auto const l = load();
    if (!l.target || !l.object) return {};
    std::vector<AssembledModule> units;
    std::uint32_t cu = 1;
    for (auto const* name : names) {
        auto const bytes = fixture(name);
        DiagnosticReporter rep;
        auto unit = read(l, bytes, cu++, rep);
        EXPECT_TRUE(unit.has_value()) << name << diagnosticsOf(rep);
        if (!unit.has_value()) return {};
        units.push_back(std::move(*unit));
    }
    DiagnosticReporter rep;
    auto const linked = linker::link(std::span<AssembledModule const>{units}, *l.target, *l.object, rep);
    EXPECT_TRUE(linked.ok()) << diagnosticsOf(rep);
    if (!linked.ok()) return {};
    return linked.bytes;
}

}  // namespace

// Linked from both units, the artifact holds ONE copy of `pick` -- an ANY COMDAT -- and ONE table, a COMDAT
// ASSOCIATIVE to that copy's section: the link discarded the later unit's group whole, and the writer handed the kept
// one on as a group. RED-ON-DISABLE: (reader) state no group -> both tables reach the writer; (writer) lay a member
// out in the shared `.rdata` -> no associative section, and the next link keeps the table of a copy it discards.
TEST(CoffComdatSelection, ARelocatableLinkOfBothCopiesKeepsOneAndWritesItsTableAssociative) {
    auto const bytes = artifactOf({kInlineA, kInlineB});
    ASSERT_FALSE(bytes.empty());
    auto const sections = sectionsOf(bytes);
    std::vector<std::size_t> copies, tables;
    for (std::size_t i = 0; i < sections.size(); ++i) {
        auto const& s = sections[i];
        if ((s.chars & kScnLnkComdat) == 0u || !s.selection.has_value()) continue;
        if (*s.selection == kSelAny && s.comdatSymbol == "pick") copies.push_back(i);
        if (*s.selection == kSelAssociative) tables.push_back(i);
    }
    ASSERT_EQ(copies.size(), 1u) << "one copy of `pick`, a select-any COMDAT";
    ASSERT_EQ(tables.size(), 1u) << "one table, and it goes with a copy";
    EXPECT_EQ(sections[tables.front()].number, copies.front() + 1u)
        << "the table goes with the copy's section (Number is a 1-based ordinal)";
    EXPECT_GT(tables.front(), copies.front()) << "an associative section follows the one it goes with (LNK1243)";
    EXPECT_EQ(sections[tables.front()].size, 40u) << "ten 4-byte entries: the table, once";

    // And DSS reads its own artifact back as the same group.
    auto const l = load();
    ASSERT_TRUE(l.target && l.object);
    DiagnosticReporter rep;
    auto const back = read(l, bytes, 1, rep);
    ASSERT_TRUE(back.has_value()) << diagnosticsOf(rep);
    EXPECT_EQ(back->sectionGroupOfInputSection.size(), 2u);
    for (auto const& [section, signature] : back->sectionGroupOfInputSection) EXPECT_EQ(signature, "pick");
}

// ══ THE IMAGE: through the production driver ═════════════════════════════════════════════════════════════════════════

namespace {

constexpr char const* kMainSwitch =
    "int pick(int, int);\n"
    "int main(void) { static int const w[10] = {103, 500, 93, 111, 400, 33, 9, -101, 100, 4};\n"
    "  for (int k = 0; k < 10; ++k) { volatile int kk = k; if (pick(kk, 100) != w[k]) return 10 + k; }\n"
    "  return pick(10, 100) == -1 ? 42 : 9; }\n";
constexpr char const* kMainInline =
    "typedef int (*fn)(int, int); fn addr_a(void); fn addr_b(void);\n"
    "int main(void) { static int const w[10] = {103, 500, 93, 111, 400, 33, 9, -101, 100, 4};\n"
    "  fn a = addr_a(), b = addr_b(); if (a != b) return 7;\n"
    "  for (int k = 0; k < 10; ++k) { volatile int kk = k; if (a(kk, 100) != w[k]) return 10 + k; }\n"
    "  return a(10, 100) == -1 ? 42 : 9; }\n";
constexpr char const* kExit =
    "42 is every case right through the table the image keeps; 10..19 the case that read a wrong table entry; 7 the "
    "two units' `pick` are two functions; 9 the default case";

// DSS links the fixtures `names` beside `main`; the image's path, or empty after a failed assertion.
[[nodiscard]] fs::path dssImage(fs::path const& dir, std::vector<char const*> const& names, char const* main) {
    std::vector<std::string> inputs;
    for (auto const* name : names) {
        auto const bytes = fixture(name);
        if (bytes.empty()) return {};
        auto const obj = dir / name;
        std::ofstream(obj, std::ios::binary).write(reinterpret_cast<char const*>(bytes.data()),
                                                   static_cast<std::streamsize>(bytes.size()));
        inputs.push_back(obj.string());
    }
    auto const src = dir / "main.c";
    std::ofstream(src, std::ios::binary) << main;
    inputs.push_back(src.string());
    auto const out = dir / "dss.out";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    DiagnosticReporter rep;
    int const rc = p.compileUnits(inputs, "c", std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, rep);
    EXPECT_EQ(rc, 0) << "DSS links the reference objects:" << diagnosticsOf(rep);
    if (rc != 0) return {};
    for (auto const& e : fs::directory_iterator(out)) {
        if (e.is_regular_file() && e.path().extension() == ".exe") return e.path();
    }
    ADD_FAILURE() << "no image in " << out.string();
    return {};
}

struct ImageCase {
    char const*              label;
    std::vector<char const*> objects;
    char const*              main;
};
std::vector<ImageCase> const kImageCases = {
    {"the switch object", {kSwitch}, kMainSwitch},
    {"both inline units, a then b", {kInlineA, kInlineB}, kMainInline},
    {"both inline units, b then a", {kInlineB, kInlineA}, kMainInline},
};

[[nodiscard]] bool runsOnThisHost() {
    return test_support::hostNativeTarget().execTarget == "x86_64:pe64-x86_64-windows-exec";
}

}  // namespace

// Every leg: the image is made.
TEST(CoffComdatSelection, DssLinksEachClangClObjectBesideACMain) {
    for (auto const& c : kImageCases) {
        SCOPED_TRACE(c.label);
        test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-comdat-selection"};
        EXPECT_FALSE(dssImage(scratch.path(), c.objects, c.main).empty());
    }
}

// Windows x86_64: each image runs to 42, as link.exe's and lld-link's of the same objects do (MEASURED at the build).
TEST(CoffComdatSelectionNative, EachImageDssLinksRunsEveryCaseThroughItsTable) {
    if (!runsOnThisHost()) GTEST_SKIP() << "the image runs only where the host is Windows x86_64";
    for (auto const& c : kImageCases) {
        SCOPED_TRACE(c.label);
        test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-comdat-selection-run"};
        auto const image = dssImage(scratch.path(), c.objects, c.main);
        ASSERT_FALSE(image.empty());
        auto const r = test_support::runBinary(image);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << kExit;
    }
}

// ══ THE ARTIFACT IN THE REFERENCE LINKERS' HANDS, beside a SECOND copy ═══════════════════════════════════════════════
//
// DSS's artifact of unit a alone holds a copy of `pick` and its table; unit b holds another. Whichever copy a
// reference linker keeps, it must keep THAT copy's table and drop the other's: written into the shared `.rdata`, the
// artifact's table would outlive its copy when b comes first. Both orders, under link.exe (the MSVC developer
// environment) and lld-link (of any Visual Studio instance carrying the LLVM component, `locateLldLink`).

namespace {

// Writes the artifact of unit a, b's object and a cl-compiled `main` into `dir`; the object names, a then b.
[[nodiscard]] bool stage(fs::path const& dir, test_support::native_probe::MsvcTools const& tools) {
    auto const artifact = artifactOf({kInlineA});
    if (artifact.empty()) return false;
    std::ofstream(dir / "a_dss.obj", std::ios::binary)
        .write(reinterpret_cast<char const*>(artifact.data()), static_cast<std::streamsize>(artifact.size()));
    auto const b = fixture(kInlineB);
    if (b.empty()) return false;
    std::ofstream(dir / "b.obj", std::ios::binary)
        .write(reinterpret_cast<char const*>(b.data()), static_cast<std::streamsize>(b.size()));
    std::ofstream(dir / "main.c", std::ios::binary) << kMainInline;
    bool const built = tools.run("cl /nologo /c /O2 /GS- main.c /Fomain.obj");
    EXPECT_TRUE(built) << "cl compiles the C main";
    return built;
}

void linkAndRun(fs::path const& dir, test_support::native_probe::MsvcTools const& tools, std::string const& linker,
                char const* who) {
    for (auto const* order : {"a_dss.obj b.obj", "b.obj a_dss.obj"}) {
        SCOPED_TRACE(std::string{who} + ", objects " + order);
        std::string const exe = std::string{"r_"} + (order[0] == 'a' ? "ab" : "ba") + ".exe";
        ASSERT_TRUE(tools.run(linker + " /nologo /OUT:" + exe + " main.obj " + order))
            << who << " links DSS's artifact beside the second copy";
        auto const r = test_support::runBinary(dir / exe);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << kExit;
    }
}

}  // namespace

TEST(CoffComdatSelectionNative, LinkExeKeepsTheArtifactsTableWithItsCopyInEitherOrder) {
    if (!runsOnThisHost()) GTEST_SKIP() << "link.exe runs only where the host is Windows x86_64";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-comdat-selection-linkexe"};
    auto const dir  = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const tools = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(tools.ready()) << tools.describe();
    ASSERT_TRUE(stage(dir, tools));
    linkAndRun(dir, tools, "link", "link.exe");
}

TEST(CoffComdatSelectionNative, LldLinkKeepsTheArtifactsTableWithItsCopyInEitherOrder) {
    if (!runsOnThisHost()) GTEST_SKIP() << "lld-link runs only where the host is Windows x86_64";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-comdat-selection-lld"};
    auto const dir  = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const lld = test_support::native_probe::locateLldLink(dir);
    if (lld.toolAbsent()) GTEST_SKIP() << lld.detail << ": the link.exe arm stands alone on this host";
    ASSERT_TRUE(lld.ok()) << lld.describe();
    auto const tools = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(tools.ready()) << tools.describe();
    ASSERT_TRUE(stage(dir, tools));
    linkAndRun(dir, tools, "\"" + lld.lldLink.string() + "\"", "lld-link");
}

// ══ GCC'S TABLE: ten distances in `.rdata` (the file header states the fixtures and what was measured) ═══════════════

namespace {

constexpr std::uint16_t kImageRelAmd64Rel32 = 4;
constexpr std::uint32_t kScnCntCode         = 0x00000020u;

// How many of `unit`'s data items' relocations are pc-relative 4-byte fields: the table's entries.
[[nodiscard]] std::size_t distancesInData(Loaded const& l, AssembledModule const& unit) {
    std::size_t n = 0;
    for (auto const& d : unit.dataItems) {
        for (auto const& rel : d.relocations) {
            auto const* tri = l.target->relocationInfo(rel.kind);
            if (tri != nullptr && tri->pcRelative && tri->widthBytes == 4) ++n;
        }
    }
    return n;
}

}  // namespace

// The read: each object's table is ten pc-relative 4-byte fields in its data — the premise the image and the artifact
// below rest on.
TEST(CoffJumpTable, GccsTableIsReadAsTenDistancesInItsData) {
    auto const l = load();
    ASSERT_TRUE(l.target && l.object);
    for (auto const* name : {kGccO0, kGccO1}) {
        SCOPED_TRACE(name);
        auto const bytes = fixture(name);
        ASSERT_FALSE(bytes.empty());
        DiagnosticReporter rep;
        auto const unit = read(l, bytes, 1, rep);
        ASSERT_TRUE(unit.has_value()) << diagnosticsOf(rep);
        EXPECT_EQ(distancesInData(l, *unit), 10u);
        EXPECT_TRUE(unit->sectionGroupOfInputSection.empty()) << "gcc writes no COMDAT";
    }
}

// Every leg: the image is made. RED-ON-DISABLE: refuse a pc-relative data field in the image (the writer before P69
// fold 3d) -> "relocation 'rel32' is pc-relative", and no image.
TEST(CoffJumpTable, DssLinksEachGccObjectBesideACMain) {
    for (auto const* name : {kGccO0, kGccO1}) {
        SCOPED_TRACE(name);
        test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-jump-table"};
        EXPECT_FALSE(dssImage(scratch.path(), {name}, kMainSwitch).empty());
    }
}

// Windows x86_64: each image runs to 42, as gcc's own program and link.exe's and lld-link's do (MEASURED at the
// build): every case reads its own entry, measured from the field.
TEST(CoffJumpTableNative, EachGccImageDssLinksRunsEveryCaseThroughItsTable) {
    if (!runsOnThisHost()) GTEST_SKIP() << "the image runs only where the host is Windows x86_64";
    for (auto const* name : {kGccO0, kGccO1}) {
        SCOPED_TRACE(name);
        test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-jump-table-run"};
        auto const image = dssImage(scratch.path(), {name}, kMainSwitch);
        ASSERT_FALSE(image.empty());
        auto const r = test_support::runBinary(image);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << kExit;
    }
}

// The relocatable artifact DSS links from each object hands the table on as it came: ten REL32 records in a data
// section, read back as ten distances. RED-ON-DISABLE: refuse a pc-relative data field in the relocatable writer (its
// gate before P69 fold 3d) -> no artifact.
TEST(CoffJumpTable, ARelocatableLinkHandsTheTableOnAsTenDistances) {
    auto const l = load();
    ASSERT_TRUE(l.target && l.object);
    for (auto const* name : {kGccO0, kGccO1}) {
        SCOPED_TRACE(name);
        auto const bytes = artifactOf({name});
        ASSERT_FALSE(bytes.empty());
        std::size_t rel32InData = 0;
        for (auto const& s : sectionsOf(bytes)) {
            if ((s.chars & kScnCntCode) != 0u) continue;
            rel32InData += static_cast<std::size_t>(
                std::count(s.relocationTypes.begin(), s.relocationTypes.end(), kImageRelAmd64Rel32));
        }
        EXPECT_EQ(rel32InData, 10u) << "the table's ten entries, each an IMAGE_REL_AMD64_REL32 in a data section";
        DiagnosticReporter rep;
        auto const back = read(l, bytes, 1, rep);
        ASSERT_TRUE(back.has_value()) << diagnosticsOf(rep);
        EXPECT_EQ(distancesInData(l, *back), 10u);
    }
}

// Windows x86_64: link.exe, and lld-link where an instance carries it, link each artifact beside a cl `main` to
// a program that exits 42 — a reference linker computes every entry from the records DSS wrote.
TEST(CoffJumpTableNative, TheReferenceLinkersRunTheArtifactOfEachGccObject) {
    if (!runsOnThisHost()) GTEST_SKIP() << "link.exe runs only where the host is Windows x86_64";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-jump-table-refs"};
    auto const dir  = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const tools = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(tools.ready()) << tools.describe();
    std::ofstream(dir / "main.c", std::ios::binary) << kMainSwitch;
    ASSERT_TRUE(tools.run("cl /nologo /c /O2 /GS- main.c /Fomain.obj")) << "cl compiles the C main";
    auto const lld = test_support::native_probe::locateLldLink(dir);
    ASSERT_TRUE(lld.ok() || lld.toolAbsent()) << lld.describe();
    std::vector<std::pair<std::string, char const*>> linkers{{"link", "link.exe"}};
    if (lld.ok()) linkers.emplace_back("\"" + lld.lldLink.string() + "\"", "lld-link");
    for (auto const* name : {kGccO0, kGccO1}) {
        auto const artifact = artifactOf({name});
        ASSERT_FALSE(artifact.empty()) << name;
        std::string const obj = std::string{"dss_"} + name;
        std::ofstream(dir / obj, std::ios::binary)
            .write(reinterpret_cast<char const*>(artifact.data()), static_cast<std::streamsize>(artifact.size()));
        std::string const stem{std::string_view{name}.substr(0, std::string_view{name}.rfind('.'))};
        for (auto const& [linker, who] : linkers) {
            SCOPED_TRACE(std::string{who} + ", " + name);
            std::string const exe = "r_" + std::string{who}.substr(0, 3) + "_" + stem + ".exe";
            ASSERT_TRUE(tools.run(linker + " /nologo /OUT:" + exe + " main.obj " + obj))
                << who << " links DSS's artifact";
            auto const r = test_support::runBinary(dir / exe);
            ASSERT_TRUE(r.spawned) << r.diagnostic;
            EXPECT_FALSE(r.timedOut);
            EXPECT_EQ(r.exitCode, 42u) << kExit;
        }
    }
    // The lld-link half is a reading too: where it could not run, the test says so rather than passing on link.exe
    // alone (a failure above still fails it).
    if (linkers.size() == 1u) {
        GTEST_SKIP() << "link.exe judged each artifact; the lld-link half was NOT read: " << lld.detail;
    }
}

// ══ THE SELECTIONS THAT STATE WHICH COPY IS KEPT: LARGEST, EXACT_MATCH, SAME_SIZE ═════════════════════════════════════
//
// D-LK-COFF-COMDAT-UNSUPPORTED-SELECTION (P69 fold 3d). A COMDAT of selection LARGEST, EXACT_MATCH or SAME_SIZE is the
// LEADER of a section group whose rule picks the copy a link keeps (`AssembledModule::sectionGroupRuleOf`), and the
// link judges the copies in link order, each against the copy kept so far under THAT copy's rule
// (`linker.cpp`, `discardLaterSectionGroups`, which states the table). Until this fold the reader refused LARGEST,
// and every associative section of a SAME_SIZE or EXACT_MATCH copy.
//
// THE CELLS ARE THE UNION OF THE REFERENCES, every order of every cell ✔MEASURED at the fixtures' build (lane `xa`'s
// `sel_cells.py`, 2026-10-22; link.exe 14.44.35229, lld-link 19.1.5, each image run beside a cl /O2 `main`): a cell is
// accepted in an order iff a reference accepts it in that order, keeping the copy that reference keeps; where both
// accept and disagree -- only a copy of ANY beside one of LARGEST -- the decided fork, lld-link's answer (the larger
// copy, the first on a tie), which is LARGEST's documented meaning where link.exe keeps the LARGEST copy whenever it
// comes second, the smaller included. Notes the table rests on: relocations are not compared (the `exact_reloc` pair,
// an ADDR64 to `t1` and to `t2` over equal bytes, keeps the first under both), and no checksum is read (the
// `exact_checksum` copy, equal bytes and another aux CheckSum, is refused by link.exe and kept-from by lld-link).
//
// The fixtures are hand-assembled by clang 19.1.5 (`tests/link/data/coff_comdat_<name>.s`, each with its REBUILD line;
// each built twice byte-identical): a COMDAT `.data$x` holding `x` -- its first quad pointing, through a LOCAL label,
// at its own ASSOCIATIVE `.data$m`, which holds the copy's mark -- so `main` returning `x[2] * 10 + **(int **)x` names
// the copy kept AND shows its associate went with it. md5 at the build:
//   any_small 3bfc454b  any_big 3cc478d4  largest_small c4817869  largest_big 74fb31d8  largest_tie 36c02767
//   largest_three c51052a2  exact_a 332ef6f6  exact_b 52688d58  exact_other 85c64fb4  exact_checksum 9b85b28f
//   exact_big e2733ad4  exact_big_twin 4aad9dec  exact_big_other 65c8cfe8  exact_reloc1 462a38f5  exact_reloc2 20dfa24b
//   samesize_a f057ebee  samesize_b 86f0b86e  samesize_big 753138fc  samesize_three 635eac27  samesize_wide 97763252

namespace {

constexpr int kRefusedCell = -1;

struct SelectionCell {
    char const*              cell;
    std::vector<char const*> objects;   // `coff_comdat_<name>.obj`, in link order
    int                      dss;       // the union: the exit code, or kRefusedCell
    int                      linkExe;   // MEASURED
    int                      lldLink;   // MEASURED
};

std::vector<SelectionCell> const kSelectionCells = {
    {"any-any", {"any_small", "any_big"}, 11, 11, 11},
    {"any-any", {"any_big", "any_small"}, 77, 77, 77},
    {"any-largest-tie", {"any_small", "largest_tie"}, 11, 22, 11},
    {"any-largest-tie", {"largest_tie", "any_small"}, 22, 22, 22},
    {"any-larger-than-largest", {"any_big", "largest_small"}, 77, 11, 77},
    {"any-larger-than-largest", {"largest_small", "any_big"}, 77, 77, 77},
    {"any-smaller-than-largest", {"any_small", "largest_big"}, 22, 22, 22},
    {"any-smaller-than-largest", {"largest_big", "any_small"}, 22, 22, 22},
    {"any-exact", {"any_small", "exact_a"}, kRefusedCell, kRefusedCell, kRefusedCell},
    {"any-exact", {"exact_a", "any_small"}, kRefusedCell, kRefusedCell, kRefusedCell},
    {"any-samesize", {"any_small", "samesize_a"}, kRefusedCell, kRefusedCell, kRefusedCell},
    {"any-samesize", {"samesize_a", "any_small"}, kRefusedCell, kRefusedCell, kRefusedCell},
    {"largest-largest", {"largest_small", "largest_big"}, 22, 22, 22},
    {"largest-largest", {"largest_big", "largest_small"}, 22, 22, 22},
    {"largest-largest-tie", {"largest_small", "largest_tie"}, 11, 11, 11},
    {"largest-largest-tie", {"largest_tie", "largest_small"}, 22, 22, 22},
    {"largest-exact-larger", {"largest_small", "exact_big"}, 55, 55, kRefusedCell},
    {"largest-exact-larger", {"exact_big", "largest_small"}, kRefusedCell, kRefusedCell, kRefusedCell},
    {"largest-samesize-larger", {"largest_small", "samesize_wide"}, 66, 66, kRefusedCell},
    {"largest-samesize-larger", {"samesize_wide", "largest_small"}, kRefusedCell, kRefusedCell, kRefusedCell},
    {"exact-largest-identical", {"exact_a", "largest_three"}, 31, 31, kRefusedCell},
    {"exact-largest-identical", {"largest_three", "exact_a"}, 32, 32, kRefusedCell},
    {"samesize-largest-identical", {"samesize_three", "largest_three"}, 31, 31, kRefusedCell},
    {"samesize-largest-identical", {"largest_three", "samesize_three"}, 32, 32, kRefusedCell},
    {"exact-exact-identical", {"exact_a", "exact_b"}, 31, 31, 31},
    {"exact-exact-identical", {"exact_b", "exact_a"}, 32, 32, 32},
    {"exact-exact-other", {"exact_a", "exact_other"}, kRefusedCell, kRefusedCell, kRefusedCell},
    {"exact-exact-other", {"exact_other", "exact_a"}, kRefusedCell, kRefusedCell, kRefusedCell},
    {"exact-exact-checksum", {"exact_a", "exact_checksum"}, 31, kRefusedCell, 31},
    {"exact-exact-checksum", {"exact_checksum", "exact_a"}, 32, kRefusedCell, 32},
    {"exact-exact-relocation", {"exact_reloc1", "exact_reloc2"}, 51, 51, 51},
    {"exact-exact-relocation", {"exact_reloc2", "exact_reloc1"}, 72, 72, 72},
    {"exact-samesize-identical", {"exact_a", "samesize_three"}, kRefusedCell, kRefusedCell, kRefusedCell},
    {"exact-samesize-identical", {"samesize_three", "exact_a"}, kRefusedCell, kRefusedCell, kRefusedCell},
    {"samesize-samesize", {"samesize_a", "samesize_b"}, 11, kRefusedCell, 11},
    {"samesize-samesize", {"samesize_b", "samesize_a"}, 22, kRefusedCell, 22},
    {"samesize-samesize-size", {"samesize_a", "samesize_big"}, kRefusedCell, kRefusedCell, kRefusedCell},
    {"samesize-samesize-size", {"samesize_big", "samesize_a"}, kRefusedCell, kRefusedCell, kRefusedCell},
    {"any-largest-exact", {"any_small", "largest_big", "exact_a"}, 22, 22, kRefusedCell},
    {"any-largest-exact", {"any_small", "exact_a", "largest_big"}, kRefusedCell, kRefusedCell, kRefusedCell},
    {"any-largest-exact", {"largest_big", "any_small", "exact_a"}, 22, 22, kRefusedCell},
    {"any-largest-exact", {"largest_big", "exact_a", "any_small"}, 22, 22, kRefusedCell},
    {"any-largest-exact", {"exact_a", "any_small", "largest_big"}, kRefusedCell, kRefusedCell, kRefusedCell},
    {"any-largest-exact", {"exact_a", "largest_big", "any_small"}, kRefusedCell, kRefusedCell, kRefusedCell},
    {"largest-exact-exact-identical", {"largest_small", "exact_big", "exact_big_twin"}, 55, 55, kRefusedCell},
    {"largest-exact-exact-identical", {"largest_small", "exact_big_twin", "exact_big"}, 56, 56, kRefusedCell},
    {"largest-exact-exact-identical", {"exact_big", "largest_small", "exact_big_twin"}, kRefusedCell, kRefusedCell,
     kRefusedCell},
    {"largest-exact-exact-identical", {"exact_big", "exact_big_twin", "largest_small"}, kRefusedCell, kRefusedCell,
     kRefusedCell},
    {"largest-exact-exact-identical", {"exact_big_twin", "largest_small", "exact_big"}, kRefusedCell, kRefusedCell,
     kRefusedCell},
    {"largest-exact-exact-identical", {"exact_big_twin", "exact_big", "largest_small"}, kRefusedCell, kRefusedCell,
     kRefusedCell},
    {"largest-exact-exact-other", {"largest_small", "exact_big", "exact_big_other"}, kRefusedCell, kRefusedCell,
     kRefusedCell},
    {"largest-exact-exact-other", {"largest_small", "exact_big_other", "exact_big"}, kRefusedCell, kRefusedCell,
     kRefusedCell},
    {"largest-exact-exact-other", {"exact_big", "largest_small", "exact_big_other"}, kRefusedCell, kRefusedCell,
     kRefusedCell},
    {"largest-exact-exact-other", {"exact_big", "exact_big_other", "largest_small"}, kRefusedCell, kRefusedCell,
     kRefusedCell},
    {"largest-exact-exact-other", {"exact_big_other", "largest_small", "exact_big"}, kRefusedCell, kRefusedCell,
     kRefusedCell},
    {"largest-exact-exact-other", {"exact_big_other", "exact_big", "largest_small"}, kRefusedCell, kRefusedCell,
     kRefusedCell},
};

constexpr char const* kMainMark = "extern int x[];\nint main(void) { return x[2] * 10 + **(int **)x; }\n";
constexpr char const* kMainMarkThroughT =
    "extern int x[];\nint t1 = 5, t2 = 7;\nint main(void) { return **(int **)(x + 2) * 10 + **(int **)x; }\n";
constexpr char const* kMarkExit =
    "the exit is the kept copy's leader word * 10 + the mark of the associate it reached: a wrong leader is the "
    "wrong copy kept, a wrong mark another copy's associate kept with it";

[[nodiscard]] std::string fixtureName(char const* name) { return std::string{"coff_comdat_"} + name + ".obj"; }

[[nodiscard]] std::string labelOf(SelectionCell const& c) {
    std::string s = std::string{c.cell} + ":";
    for (auto const* o : c.objects) s += std::string{" "} + o;
    return s;
}

[[nodiscard]] char const* mainOf(SelectionCell const& c) {
    return std::string_view{c.cell} == "exact-exact-relocation" ? kMainMarkThroughT : kMainMark;
}

struct DssLink {
    int         rc = -1;
    std::string diagnostics;
    fs::path    image;
};

// DSS links the cell's objects, in order, beside its `main`.
[[nodiscard]] DssLink dssLinkCell(fs::path const& dir, SelectionCell const& c) {
    DssLink out;
    std::vector<std::string> inputs;
    for (auto const* name : c.objects) {
        auto const bytes = fixture(fixtureName(name));
        if (bytes.empty()) return out;
        auto const obj = dir / fixtureName(name);
        std::ofstream(obj, std::ios::binary).write(reinterpret_cast<char const*>(bytes.data()),
                                                   static_cast<std::streamsize>(bytes.size()));
        inputs.push_back(obj.string());
    }
    auto const src = dir / "main.c";
    std::ofstream(src, std::ios::binary) << mainOf(c);
    inputs.push_back(src.string());
    auto const outDir = dir / "dss.out";
    fs::create_directories(outDir);
    Program p;
    p.setOutputDir(outDir);
    DiagnosticReporter rep;
    out.rc          = p.compileUnits(inputs, "c", std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, rep);
    out.diagnostics = diagnosticsOf(rep);
    for (auto const& e : fs::directory_iterator(outDir)) {
        if (e.is_regular_file() && e.path().extension() == ".exe") out.image = e.path();
    }
    return out;
}

}  // namespace

// Every leg: DSS links each cell the union accepts and refuses, by name, each it does not -- naming the group and both
// units. RED-ON-DISABLE: (reader) state no rule -> every LARGEST and EXACT/SAME_SIZE cell keeps the first copy;
// (link) judge every carrier by the FIRST carrier's rule -> largest-exact-exact-other 123 links.
TEST(CoffComdatSelection, DssLinksEachCellTheReferencesUnionAcceptsAndRefusesTheRestByName) {
    for (auto const& c : kSelectionCells) {
        SCOPED_TRACE(labelOf(c));
        test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-comdat-selection-cell"};
        auto const r = dssLinkCell(scratch.path(), c);
        if (c.dss == kRefusedCell) {
            EXPECT_NE(r.rc, 0) << "the union refuses this order: no reference links it";
            EXPECT_NE(r.diagnostics.find("is defined as copies of one section group"), std::string::npos)
                << r.diagnostics;
            EXPECT_NE(r.diagnostics.find("K_SymbolRedefinedAcrossUnits"), std::string::npos) << r.diagnostics;
        } else {
            EXPECT_EQ(r.rc, 0) << "a reference links this order:" << r.diagnostics;
            EXPECT_FALSE(r.image.empty());
        }
    }
}

// Windows x86_64: each image DSS links runs to the exit the union names -- the copy kept and its own associate.
// The cells the union refuses are DSS's to refuse too: read by name, on every leg, by
// `CoffComdatSelection.DssLinksEachCellTheReferencesUnionAcceptsAndRefusesTheRestByName` -- this TEST runs only
// what DSS links.
TEST(CoffComdatSelectionNative, EachCellDssLinksKeepsTheCopyTheUnionKeeps) {
    if (!runsOnThisHost()) GTEST_SKIP() << "the image runs only where the host is Windows x86_64";
    for (auto const& c : kSelectionCells) {
        if (c.dss == kRefusedCell) continue;
        SCOPED_TRACE(labelOf(c));
        test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-comdat-selection-cell-run"};
        auto const r = dssLinkCell(scratch.path(), c);
        ASSERT_EQ(r.rc, 0) << r.diagnostics;
        ASSERT_FALSE(r.image.empty());
        auto const run = test_support::runBinary(r.image);
        ASSERT_TRUE(run.spawned) << run.diagnostic;
        EXPECT_FALSE(run.timedOut);
        EXPECT_EQ(run.exitCode, static_cast<std::uint32_t>(c.dss)) << kMarkExit;
    }
}

namespace {

// One reference linker over every cell: each order linked beside a cl `main` and run, against its MEASURED answer.
void referenceCells(fs::path const& dir, test_support::native_probe::MsvcTools const& tools,
                    std::string const& linker, bool linkExe) {
    std::ofstream(dir / "main.c", std::ios::binary) << kMainMark;
    std::ofstream(dir / "main_t.c", std::ios::binary) << kMainMarkThroughT;
    ASSERT_TRUE(tools.run("cl /nologo /c /O2 /GS- main.c /Fomain.obj")) << "cl compiles the C main";
    ASSERT_TRUE(tools.run("cl /nologo /c /O2 /GS- main_t.c /Fomain_t.obj")) << "cl compiles the C main";
    std::size_t k = 0;
    for (auto const& c : kSelectionCells) {
        SCOPED_TRACE(labelOf(c));
        std::string objects;
        for (auto const* name : c.objects) {
            auto const bytes = fixture(fixtureName(name));
            ASSERT_FALSE(bytes.empty());
            std::ofstream(dir / fixtureName(name), std::ios::binary)
                .write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            objects += " " + fixtureName(name);
        }
        std::string const exe   = "cell_" + std::to_string(k++) + ".exe";
        char const* const mainO = mainOf(c) == kMainMarkThroughT ? "main_t.obj" : "main.obj";
        bool const linked = tools.run(linker + " /nologo /OUT:" + exe + " " + mainO + objects);
        int const  want   = linkExe ? c.linkExe : c.lldLink;
        if (want == kRefusedCell) {
            EXPECT_FALSE(linked) << "MEASURED: this reference refuses this order";
            continue;
        }
        ASSERT_TRUE(linked) << "MEASURED: this reference links this order";
        auto const run = test_support::runBinary(dir / exe);
        ASSERT_TRUE(run.spawned) << run.diagnostic;
        EXPECT_FALSE(run.timedOut);
        EXPECT_EQ(run.exitCode, static_cast<std::uint32_t>(want)) << kMarkExit;
    }
}

}  // namespace

// Windows x86_64, link.exe: every cell as MEASURED -- the half of the union only link.exe accepts included.
TEST(CoffComdatSelectionNative, LinkExeJudgesEachCellAsMeasured) {
    if (!runsOnThisHost()) GTEST_SKIP() << "link.exe runs only where the host is Windows x86_64";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-comdat-selection-cells-linkexe"};
    auto const dir  = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const tools = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(tools.ready()) << tools.describe();
    referenceCells(dir, tools, "link", /*linkExe=*/true);
}

// Windows x86_64, lld-link: every cell as MEASURED -- the decided fork's answers are lld-link's.
TEST(CoffComdatSelectionNative, LldLinkJudgesEachCellAsMeasured) {
    if (!runsOnThisHost()) GTEST_SKIP() << "lld-link runs only where the host is Windows x86_64";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-comdat-selection-cells-lld"};
    auto const dir  = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const lld = test_support::native_probe::locateLldLink(dir);
    if (lld.toolAbsent()) GTEST_SKIP() << lld.detail;
    ASSERT_TRUE(lld.ok()) << lld.describe();
    auto const tools = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(tools.ready()) << tools.describe();
    referenceCells(dir, tools, "\"" + lld.lldLink.string() + "\"", /*linkExe=*/false);
}

// Every leg: the relocatable artifact writes each copy's rule back as its selection (LARGEST 6, EXACT_MATCH 4,
// SAME_SIZE 3, ANY 2), its associate ASSOCIATIVE to it; and of two LARGEST copies it holds the larger alone.
// RED-ON-DISABLE: write ANY for every select-any definition (the writer before this fold) -> the next link keeps the
// first copy where LARGEST keeps the larger.
TEST(CoffComdatSelection, ARelocatableArtifactWritesEachCopysSelectionBack) {
    struct Case {
        std::vector<char const*> objects;
        std::uint8_t             selection;
        std::uint32_t            leaderSize;
    };
    for (auto const& c : {Case{{"largest_small"}, 6, 12}, Case{{"exact_a"}, 4, 12}, Case{{"samesize_a"}, 3, 12},
                          Case{{"any_small"}, 2, 12}, Case{{"largest_small", "largest_big"}, 6, 16},
                          Case{{"largest_big", "largest_small"}, 6, 16}}) {
        std::vector<std::string>  names;
        std::vector<char const*>  files;
        for (auto const* o : c.objects) names.push_back(fixtureName(o));
        for (auto const& n : names) files.push_back(n.c_str());
        SCOPED_TRACE(names.front() + (names.size() > 1 ? " + " + names.back() : std::string{}));
        auto const bytes = artifactOf(files);
        ASSERT_FALSE(bytes.empty());
        auto const sections = sectionsOf(bytes);
        std::vector<std::size_t> leaders, associates;
        for (std::size_t i = 0; i < sections.size(); ++i) {
            auto const& s = sections[i];
            if ((s.chars & kScnLnkComdat) == 0u || !s.selection.has_value()) continue;
            if (s.comdatSymbol == "x" && *s.selection != kSelAssociative) leaders.push_back(i);
            if (*s.selection == kSelAssociative) associates.push_back(i);
        }
        ASSERT_EQ(leaders.size(), 1u) << "one copy of `x`";
        EXPECT_EQ(*sections[leaders.front()].selection, c.selection) << "the copy's rule, written back";
        EXPECT_EQ(sections[leaders.front()].size, c.leaderSize) << "the copy the rule keeps";
        ASSERT_EQ(associates.size(), 1u) << "one associate, the kept copy's";
        EXPECT_EQ(sections[associates.front()].number, leaders.front() + 1u);
    }
}

// ══ A COMPILED UNIT'S SELECT-ANY DEFINITION BESIDE A COPY OF ITS NAME ════════════════════════════════════════════════
//
// DSS's own `__declspec(selectany)` definition is, in DSS's relocatable object, a COMDAT of selection ANY; so the link
// judges it beside a foreign copy of its name as that copy (`linker.cpp`, `stateCompiledSelectAnyCopies`), by the
// table's First row. ✔MEASURED P69 fold 3d (lane `xa`'s `weakmix.py`, its `refs-run2.log`; link.exe 14.44.35229 and
// lld-link 19.1.5, DSS's object of the unit beside each fixture and a cl /O2 `main`, both orders, each image run):
// beside a SAME_SIZE or an EXACT_MATCH copy both refuse the pair in both orders, at an equal size too; beside a
// LARGEST copy lld-link keeps the larger, the first on a tie, and link.exe the LARGEST copy whenever DSS's object
// comes first -- the decided fork's cell. Until this fold DSS met the pair only in the name election, which compared
// the sizes and kept the first copy. `main` returns `x[2] * 10 +` the unit's own read of `x[2]`: 55 is DSS's copy
// kept, `n * 10 + n` a fixture's copy, and any other number a read left on a discarded copy.

namespace {

constexpr char const* kOwnMain =
    "extern int x[];\nint through_own(void);\nint main(void) { return x[2] * 10 + through_own(); }\n";
constexpr char const* kOwn12 = "__declspec(selectany) int x[3] = {0, 0, 5};\nint through_own(void) { return x[2]; }\n";
constexpr char const* kOwn16 =
    "__declspec(selectany) int x[4] = {0, 0, 5, 0};\nint through_own(void) { return x[2]; }\n";

struct OwnCopyCell {
    char const* cell;
    char const* own;       // the C unit
    char const* foreign;   // `coff_comdat_<name>.obj`
    bool        ownFirst;  // the C unit before the object in link order
    int         dss;       // the union: the exit code, or kRefusedCell
    int         linkExe;   // MEASURED, with DSS's object of the unit
    int         lldLink;   // MEASURED, with DSS's object of the unit
};

std::vector<OwnCopyCell> const kOwnCopyCells = {
    {"own12-any", kOwn12, "any_small", true, 55, 55, 55},
    {"own12-any", kOwn12, "any_small", false, 11, 11, 11},
    {"own12-samesize", kOwn12, "samesize_a", true, kRefusedCell, kRefusedCell, kRefusedCell},
    {"own12-samesize", kOwn12, "samesize_a", false, kRefusedCell, kRefusedCell, kRefusedCell},
    {"own12-exact", kOwn12, "exact_a", true, kRefusedCell, kRefusedCell, kRefusedCell},
    {"own12-exact", kOwn12, "exact_a", false, kRefusedCell, kRefusedCell, kRefusedCell},
    {"own12-largest-tie", kOwn12, "largest_small", true, 55, 11, 55},
    {"own12-largest-tie", kOwn12, "largest_small", false, 11, 11, 11},
    {"own12-smaller-than-largest", kOwn12, "largest_big", true, 22, 22, 22},
    {"own12-smaller-than-largest", kOwn12, "largest_big", false, 22, 22, 22},
    {"own16-larger-than-largest", kOwn16, "largest_small", true, 55, 11, 55},
    {"own16-larger-than-largest", kOwn16, "largest_small", false, 55, 55, 55},
    {"own16-samesize", kOwn16, "samesize_big", true, kRefusedCell, kRefusedCell, kRefusedCell},
    {"own16-samesize", kOwn16, "samesize_big", false, kRefusedCell, kRefusedCell, kRefusedCell},
};

[[nodiscard]] std::string ownLabelOf(OwnCopyCell const& c) {
    return std::string{c.cell} + (c.ownFirst ? ": the unit first" : ": the object first");
}

// DSS links `main`, then the cell's unit and object in the cell's order.
[[nodiscard]] DssLink dssLinkOwnCell(fs::path const& dir, OwnCopyCell const& c) {
    DssLink    out;
    auto const bytes = fixture(fixtureName(c.foreign));
    if (bytes.empty()) return out;
    auto const obj = dir / fixtureName(c.foreign);
    std::ofstream(obj, std::ios::binary).write(reinterpret_cast<char const*>(bytes.data()),
                                               static_cast<std::streamsize>(bytes.size()));
    auto const own  = dir / "own.c";
    auto const mainC = dir / "main.c";
    std::ofstream(own, std::ios::binary) << c.own;
    std::ofstream(mainC, std::ios::binary) << kOwnMain;
    std::vector<std::string> inputs{mainC.string()};
    inputs.push_back(c.ownFirst ? own.string() : obj.string());
    inputs.push_back(c.ownFirst ? obj.string() : own.string());
    auto const outDir = dir / "dss.out";
    fs::create_directories(outDir);
    Program p;
    p.setOutputDir(outDir);
    DiagnosticReporter rep;
    out.rc          = p.compileUnits(inputs, "c", std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, rep);
    out.diagnostics = diagnosticsOf(rep);
    for (auto const& e : fs::directory_iterator(outDir)) {
        if (e.is_regular_file() && e.path().extension() == ".exe") out.image = e.path();
    }
    return out;
}

}  // namespace

// Every leg: DSS links each order the union accepts and refuses, by name, each it does not. RED-ON-DISABLE: state no
// compiled copy -> the election compares the SAME_SIZE pair's equal sizes and links it.
TEST(CoffComdatSelection, ACompiledSelectAnyDefinitionIsJudgedAsTheCopyItsObjectIs) {
    for (auto const& c : kOwnCopyCells) {
        SCOPED_TRACE(ownLabelOf(c));
        test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-comdat-selection-own"};
        auto const r = dssLinkOwnCell(scratch.path(), c);
        if (c.dss == kRefusedCell) {
            EXPECT_NE(r.rc, 0) << "the union refuses this order: no reference links DSS's object of the unit in it";
            EXPECT_NE(r.diagnostics.find("is defined as copies of one section group"), std::string::npos)
                << r.diagnostics;
            EXPECT_NE(r.diagnostics.find("K_SymbolRedefinedAcrossUnits"), std::string::npos) << r.diagnostics;
        } else {
            EXPECT_EQ(r.rc, 0) << "a reference links this order:" << r.diagnostics;
            EXPECT_FALSE(r.image.empty());
        }
    }
}

// Windows x86_64: each image DSS links runs to the exit the union names -- the copy kept, read through `main` and
// through the unit's own read alike. RED-ON-DISABLE: state no compiled copy -> the first copy is kept beside a larger
// LARGEST one (55 for 22); follow no copy's own name -> the unit's own read of a lost copy is refused as its bytes.
// The orders the union refuses are DSS's to refuse too: read by name, on every leg, by
// `CoffComdatSelection.ACompiledSelectAnyDefinitionIsJudgedAsTheCopyItsObjectIs` -- this TEST runs only what DSS
// links.
TEST(CoffComdatSelectionNative, EachOwnCellDssLinksKeepsTheCopyTheUnionKeeps) {
    if (!runsOnThisHost()) GTEST_SKIP() << "the image runs only where the host is Windows x86_64";
    for (auto const& c : kOwnCopyCells) {
        if (c.dss == kRefusedCell) continue;
        SCOPED_TRACE(ownLabelOf(c));
        test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-comdat-selection-own-run"};
        auto const r = dssLinkOwnCell(scratch.path(), c);
        ASSERT_EQ(r.rc, 0) << r.diagnostics;
        ASSERT_FALSE(r.image.empty());
        auto const run = test_support::runBinary(r.image);
        ASSERT_TRUE(run.spawned) << run.diagnostic;
        EXPECT_FALSE(run.timedOut);
        EXPECT_EQ(run.exitCode, static_cast<std::uint32_t>(c.dss))
            << "55 is the unit's copy kept, n * 10 + n a fixture's, any other number a read of a discarded copy";
    }
}

namespace {

// DSS's relocatable object of `source`, written into `dir` as `name`; false after a failed assertion.
[[nodiscard]] bool ownObject(fs::path const& dir, char const* source, std::string const& name) {
    auto const work = dir / (name + ".dss");
    fs::create_directories(work);
    std::ofstream(work / "own.c", std::ios::binary) << source;
    Program p;
    p.setOutputDir(work);
    DiagnosticReporter rep;
    int const rc = p.compileUnits(std::vector<std::string>{(work / "own.c").string()}, "c",
                                  std::vector<std::string>{"x86_64:pe64-x86_64-windows"}, rep);
    EXPECT_EQ(rc, 0) << "DSS writes the unit's relocatable object:" << diagnosticsOf(rep);
    if (rc != 0) return false;
    for (auto const& e : fs::directory_iterator(work)) {
        if (e.is_regular_file() && e.path().extension() == ".obj") {
            fs::copy_file(e.path(), dir / name, fs::copy_options::overwrite_existing);
            return true;
        }
    }
    ADD_FAILURE() << "no object in " << work.string();
    return false;
}

// One reference linker over every own cell: DSS's object of the unit and the fixture in the cell's order, beside a cl
// `main`, against its MEASURED answer.
void referenceOwnCells(fs::path const& dir, test_support::native_probe::MsvcTools const& tools,
                       std::string const& linker, bool linkExe) {
    std::ofstream(dir / "main.c", std::ios::binary) << kOwnMain;
    ASSERT_TRUE(tools.run("cl /nologo /c /O2 /GS- main.c /Fomain.obj")) << "cl compiles the C main";
    ASSERT_TRUE(ownObject(dir, kOwn12, "own12.obj"));
    ASSERT_TRUE(ownObject(dir, kOwn16, "own16.obj"));
    std::size_t k = 0;
    for (auto const& c : kOwnCopyCells) {
        SCOPED_TRACE(ownLabelOf(c));
        auto const bytes = fixture(fixtureName(c.foreign));
        ASSERT_FALSE(bytes.empty());
        std::ofstream(dir / fixtureName(c.foreign), std::ios::binary)
            .write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        std::string const ownObj  = c.own == kOwn12 ? "own12.obj" : "own16.obj";
        std::string const objects = c.ownFirst ? ownObj + " " + fixtureName(c.foreign)
                                               : fixtureName(c.foreign) + " " + ownObj;
        std::string const exe     = "own_" + std::to_string(k++) + ".exe";
        bool const        linked  = tools.run(linker + " /nologo /OUT:" + exe + " main.obj " + objects);
        int const         want    = linkExe ? c.linkExe : c.lldLink;
        if (want == kRefusedCell) {
            EXPECT_FALSE(linked) << "MEASURED: this reference refuses this order";
            continue;
        }
        ASSERT_TRUE(linked) << "MEASURED: this reference links this order";
        auto const run = test_support::runBinary(dir / exe);
        ASSERT_TRUE(run.spawned) << run.diagnostic;
        EXPECT_FALSE(run.timedOut);
        EXPECT_EQ(run.exitCode, static_cast<std::uint32_t>(want));
    }
}

}  // namespace

// Windows x86_64, link.exe: DSS's object of each unit beside each fixture, as MEASURED.
TEST(CoffComdatSelectionNative, LinkExeJudgesDssObjectOfEachOwnCellAsMeasured) {
    if (!runsOnThisHost()) GTEST_SKIP() << "link.exe runs only where the host is Windows x86_64";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-comdat-selection-own-linkexe"};
    auto const dir  = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const tools = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(tools.ready()) << tools.describe();
    referenceOwnCells(dir, tools, "link", /*linkExe=*/true);
}

// Windows x86_64, lld-link: the same, as MEASURED -- the decided fork's answers are lld-link's.
TEST(CoffComdatSelectionNative, LldLinkJudgesDssObjectOfEachOwnCellAsMeasured) {
    if (!runsOnThisHost()) GTEST_SKIP() << "lld-link runs only where the host is Windows x86_64";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-comdat-selection-own-lld"};
    auto const dir  = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const lld = test_support::native_probe::locateLldLink(dir);
    if (lld.toolAbsent()) GTEST_SKIP() << lld.detail;
    ASSERT_TRUE(lld.ok()) << lld.describe();
    auto const tools = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(tools.ready()) << tools.describe();
    referenceOwnCells(dir, tools, "\"" + lld.lldLink.string() + "\"", /*linkExe=*/false);
}

// ══ A COMDAT WHOSE COMDAT SYMBOL IS FILE-LOCAL IS NO COPY ═════════════════════════════════════════════════════════════
//
// A COMDAT's copies are matched by its COMDAT symbol's name; where that symbol is STATIC no other object can carry it,
// so an external definition inside the section is one definition of its name like any other. ✔MEASURED P69 fold 3d
// (lane `xa`'s `exotic.py`; link.exe 14.44.35229 LNK2005, lld-link 19.1.5 "duplicate symbol"): every pair of the
// fixtures below is refused in both orders -- ANY, SAME_SIZE at an equal size, EXACT_MATCH of identical bytes,
// LARGEST. DSS at HEAD (b5efdbb3) linked the first three, keeping the first copy: the selection made `x` weak and
// the name election judged the pair. The fixtures (`tests/link/data/coff_comdat_static_<sel>_{1,2}.s`, clang 19.1.5,
// each built twice byte-identical), md5:
//   any 1 a756fdbc  any 2 d5c3dd3d  samesize 1 c085d9c6  samesize 2 0e35149d  exact 1 and 2 c7276a46
//   largest 1 70694728  largest 2 833ce1b3

namespace {

constexpr char const* kStaticSelections[] = {"any", "samesize", "exact", "largest"};
constexpr char const* kMainSlot           = "extern int x[];\nint main(void) { return x[2]; }\n";

[[nodiscard]] std::string staticFixture(char const* selection, int k) {
    return std::string{"coff_comdat_static_"} + selection + "_" + std::to_string(k) + ".obj";
}

// A fixture as an input of a link: the fixture's bytes, written under `file` (a second input of the same bytes needs
// a file name of its own).
struct FixtureFile {
    std::string fixture;
    std::string file;
};

// DSS links `main`, then the named fixtures in order.
[[nodiscard]] DssLink dssLinkFixtureFiles(fs::path const& dir, std::vector<FixtureFile> const& files,
                                          char const* main) {
    DssLink out;
    auto const mainC = dir / "main.c";
    std::ofstream(mainC, std::ios::binary) << main;
    std::vector<std::string> inputs{mainC.string()};
    for (auto const& [name, file] : files) {
        auto const bytes = fixture(name);
        if (bytes.empty()) return out;
        std::ofstream(dir / file, std::ios::binary).write(reinterpret_cast<char const*>(bytes.data()),
                                                          static_cast<std::streamsize>(bytes.size()));
        inputs.push_back((dir / file).string());
    }
    auto const outDir = dir / "dss.out";
    fs::create_directories(outDir);
    Program p;
    p.setOutputDir(outDir);
    DiagnosticReporter rep;
    out.rc          = p.compileUnits(inputs, "c", std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, rep);
    out.diagnostics = diagnosticsOf(rep);
    for (auto const& e : fs::directory_iterator(outDir)) {
        if (e.is_regular_file() && e.path().extension() == ".exe") out.image = e.path();
    }
    return out;
}

// DSS links `main`, then the named fixtures in order, each under its own name.
[[nodiscard]] DssLink dssLinkFixtures(fs::path const& dir, std::vector<std::string> const& names, char const* main) {
    std::vector<FixtureFile> files;
    for (auto const& name : names) files.push_back({name, name});
    return dssLinkFixtureFiles(dir, files, main);
}

}  // namespace

// Every leg: the reader states `x` STRONG and no group, and DSS refuses each pair in both orders as two strong
// definitions of one name. RED-ON-DISABLE: lift the selection's binding whatever the COMDAT symbol (HEAD's reader) ->
// the ANY, SAME_SIZE and EXACT_MATCH pairs link.
TEST(CoffComdatSelection, AnExternalDefinitionInACopyOfAFileLocalSymbolIsOneDefinitionOfItsName) {
    auto const l = load();
    ASSERT_TRUE(l.target && l.object);
    for (auto const* selection : kStaticSelections) {
        SCOPED_TRACE(selection);
        auto const bytes = fixture(staticFixture(selection, 1));
        ASSERT_FALSE(bytes.empty());
        DiagnosticReporter rep;
        auto const unit = read(l, bytes, 1, rep);
        ASSERT_TRUE(unit.has_value()) << diagnosticsOf(rep);
        bool named = false;
        for (auto const& s : unit->symbols) {
            if (s.name != "x") continue;
            named = true;
            EXPECT_EQ(s.binding, SymbolBinding::Global) << "a definition in no copy is strong";
        }
        EXPECT_TRUE(named) << "the reader states `x`";
        EXPECT_TRUE(unit->sectionGroupOfInputSection.empty()) << "a COMDAT of a file-local symbol is no group";
        for (bool const firstFirst : {true, false}) {
            SCOPED_TRACE(firstFirst ? "1 then 2" : "2 then 1");
            test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-comdat-static"};
            auto const r = dssLinkFixtures(
                scratch.path(),
                firstFirst ? std::vector<std::string>{staticFixture(selection, 1), staticFixture(selection, 2)}
                           : std::vector<std::string>{staticFixture(selection, 2), staticFixture(selection, 1)},
                kMainSlot);
            EXPECT_NE(r.rc, 0) << "both references refuse the pair: two strong definitions of `x`";
            EXPECT_NE(r.diagnostics.find("has multiple strong (Global) definitions"), std::string::npos)
                << r.diagnostics;
        }
    }
}

// Windows x86_64: link.exe and lld-link refuse each pair in both orders, as MEASURED.
TEST(CoffComdatSelectionNative, TheReferencesRefuseEachPairOfAFileLocalCopy) {
    if (!runsOnThisHost()) GTEST_SKIP() << "link.exe and lld-link run only where the host is Windows x86_64";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-comdat-static-refs"};
    auto const dir  = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const tools = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(tools.ready()) << tools.describe();
    std::ofstream(dir / "main.c", std::ios::binary) << kMainSlot;
    ASSERT_TRUE(tools.run("cl /nologo /c /O2 /GS- main.c /Fomain.obj")) << "cl compiles the C main";
    auto const lld = test_support::native_probe::locateLldLink(dir);
    ASSERT_TRUE(lld.ok() || lld.toolAbsent()) << lld.describe();
    std::vector<std::pair<std::string, char const*>> linkers{{"link", "link.exe"}};
    if (lld.ok()) linkers.emplace_back("\"" + lld.lldLink.string() + "\"", "lld-link");
    for (auto const* selection : kStaticSelections) {
        for (int const k : {1, 2}) {
            auto const bytes = fixture(staticFixture(selection, k));
            ASSERT_FALSE(bytes.empty());
            std::ofstream(dir / staticFixture(selection, k), std::ios::binary)
                .write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        for (auto const& [linker, who] : linkers) {
            for (bool const firstFirst : {true, false}) {
                SCOPED_TRACE(std::string{who} + ", " + selection + (firstFirst ? ", 1 then 2" : ", 2 then 1"));
                std::string const objects = firstFirst
                                                ? staticFixture(selection, 1) + " " + staticFixture(selection, 2)
                                                : staticFixture(selection, 2) + " " + staticFixture(selection, 1);
                EXPECT_FALSE(tools.run(linker + " /nologo /OUT:static.exe main.obj " + objects))
                    << "MEASURED: " << who << " refuses the pair";
            }
        }
    }
    // The lld-link half is a reading too: where it could not run, the test says so rather than passing on link.exe
    // alone (a failure above still fails it).
    if (linkers.size() == 1u) {
        GTEST_SKIP() << "link.exe judged each pair; the lld-link half was NOT read: " << lld.detail;
    }
}

// ══ AN ASSOCIATIVE SECTION THAT DEFINES AN EXTERNAL NAME ══════════════════════════════════════════════════════════════
//
// A section ASSOCIATIVE to another is kept iff the root of its chain is, and every name it defines goes with it: the
// associate of a section no link discards (a NODUPLICATES function's) is an ordinary section of its unit, and its names
// are defined; the associate of one of several copies is a member of that copy's group, so a name it defines is the
// KEPT copy's, and a name only a LOST copy's associate defines is defined by nothing the link keeps. ✔MEASURED P69 fold
// 3d (lane `xa`'s `assoc_ext_probe.py`, 2026-10-29; link.exe 14.44.35229, lld-link 19.1.5, each image run beside a cl
// /O2 `main`; DSS at fold 3d's reader in every cell as below), each order decided by the union over what WORKS:
//   * `nodup`: `y` in the associate of `f`'s NODUPLICATES `.text$mn` -- both references link it and read it (42).
//   * two ANY copies of `x`, each associate defining `y`: link.exe refuses (LNK2005 `y`); lld-link links the kept
//     copy's `y` (11, 22). One working reference makes the order required, and the answer is lld-link's.
//   * the kept copy (`any_c`) has no associate, and the lost one's defines `y`: lld-link refuses (undefined `y`);
//     link.exe LINKS, binding `y` to RVA 0, so the image reads its own DOS header ("MZ\x90\0", 0x00905A4D) as `y` --
//     exit 946060 = 3 * 10 + 0x00905A4D / 10, read from a discarded section. That does not work: the order is refused.
//   * the kept copy defines `y` (`any_b` first): both 22.
// Until P69 fold 3d the reader refused every associative section of a section of code or data (checkpoint 35's
// Gate 3: "has COMDAT Selection 5"). The fixtures (`tests/link/data/coff_assoc_ext_<name>.s`, each with its REBUILD
// line; each built twice byte-identical), md5 and size at the build:
//   coff_assoc_ext_nodup.obj   461 B  md5 4088fbd991ec03144c3bf3aea0eae363
//   coff_assoc_ext_any_a.obj   448 B  md5 3d796248cc07db3ec7ade4664bd58f77
//   coff_assoc_ext_any_b.obj   448 B  md5 cd8807e37ea63342d20f499ea79684e3
//   coff_assoc_ext_any_c.obj   350 B  md5 16882333c9b4b27afedb46b50774b3cd
// (clang 18.1.3 `--target=x86_64-pc-windows-msvc`, through `probe-reference-cc`'s keep mode on linux-x86_64-debug:
// windows-x86_64-release has no clang.)

namespace {

struct AssocExtCell {
    char const*              cell;
    char const*              main;
    std::vector<char const*> objects;   // `coff_assoc_ext_<name>.obj`, in link order after `main`
    int                      dss;       // the union over what works: the exit code, or kRefusedCell
    int                      linkExe;   // MEASURED
    int                      lldLink;   // MEASURED
};

constexpr char const* kMainAssocNodup = "int f(void);\nextern int y;\nint main(void) { return f() + y - 42; }\n";
constexpr char const* kMainAssocAny   = "extern int x, y;\nint main(void) { return x * 10 + y / 10; }\n";
// link.exe's image of `any_c any_b` (MEASURED): `y` bound to RVA 0, the image's DOS header read as its value.
constexpr int kLinkExeReadsItsDosHeader = 3 * 10 + 0x00905A4D / 10;

std::vector<AssocExtCell> const kAssocExtCells = {
    {"nodup", kMainAssocNodup, {"nodup"}, 42, 42, 42},
    {"any-any", kMainAssocAny, {"any_a", "any_b"}, 11, kRefusedCell, 11},
    {"any-any", kMainAssocAny, {"any_b", "any_a"}, 22, kRefusedCell, 22},
    {"kept-copy-without-the-name", kMainAssocAny, {"any_c", "any_b"}, kRefusedCell, kLinkExeReadsItsDosHeader,
     kRefusedCell},
    {"kept-copy-with-the-name", kMainAssocAny, {"any_b", "any_c"}, 22, 22, 22},
};

constexpr char const* kAssocExit =
    "nodup: f() + y - 42, 42 when `y` is the associate's; the copies: x * 10 + y / 10 -- 11 is a's copy with its own "
    "`y`, 22 b's, and 12 or 21 one copy's `x` beside the other copy's `y`";

[[nodiscard]] std::string assocFixture(char const* name) { return std::string{"coff_assoc_ext_"} + name + ".obj"; }

[[nodiscard]] std::string assocLabelOf(AssocExtCell const& c) {
    std::string s = std::string{c.cell} + ":";
    for (auto const* o : c.objects) s += std::string{" "} + o;
    return s;
}

[[nodiscard]] DssLink dssLinkAssocCell(fs::path const& dir, AssocExtCell const& c) {
    std::vector<std::string> names;
    for (auto const* o : c.objects) names.push_back(assocFixture(o));
    return dssLinkFixtures(dir, names, c.main);
}

// The group the input section holding the datum `name` belongs to, or none.
[[nodiscard]] std::optional<std::string> groupOfDatum(AssembledModule const& m, std::string_view name) {
    for (auto const& s : m.symbols) {
        if (s.name != name) continue;
        for (auto const& d : m.dataItems) {
            if (d.symbol != s.symbol || !d.inputSection.has_value()) continue;
            auto const it = m.sectionGroupOfInputSection.find(d.inputSection->section);
            if (it != m.sectionGroupOfInputSection.end()) return it->second;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<SymbolBinding> bindingOfName(AssembledModule const& m, std::string_view name) {
    for (auto const& s : m.symbols) {
        if (s.name == name) return s.binding;
    }
    return std::nullopt;
}

}  // namespace

// Every leg: the reader states the associate of a function defined once as an ordinary section of its unit (`y`
// strong, no group) and the associate of a copy as a member of that copy's group (`y` in `x`'s); and DSS links each
// order the union accepts and refuses the one it does not, `y` undefined. RED-ON-DISABLE: refuse an associative
// section rooted in NODUPLICATES (checkpoint 35's reader) -> `nodup` is refused; state no group for an associative
// section -> both copies' `y` reach the link, and the `any-any` orders are refused as two strong definitions.
TEST(CoffComdatSelection, AnAssociativeSectionsExternalNameGoesWithTheSectionItGoesWith) {
    auto const l = load();
    ASSERT_TRUE(l.target && l.object);
    {
        auto const bytes = fixture(assocFixture("nodup"));
        ASSERT_FALSE(bytes.empty());
        DiagnosticReporter rep;
        auto const unit = read(l, bytes, 1, rep);
        ASSERT_TRUE(unit.has_value()) << diagnosticsOf(rep);
        EXPECT_EQ(bindingOfName(*unit, "y"), std::optional<SymbolBinding>{SymbolBinding::Global})
            << "the associate of a function defined once defines `y` once, strong";
        EXPECT_TRUE(unit->sectionGroupOfInputSection.empty()) << "a NODUPLICATES section is no copy: no group";
    }
    {
        auto const bytes = fixture(assocFixture("any_a"));
        ASSERT_FALSE(bytes.empty());
        DiagnosticReporter rep;
        auto const unit = read(l, bytes, 1, rep);
        ASSERT_TRUE(unit.has_value()) << diagnosticsOf(rep);
        EXPECT_EQ(groupOfDatum(*unit, "y"), std::optional<std::string>{"x"})
            << "the associate of the copy of `x` is a member of `x`'s group, `y` with it";
    }
    for (auto const& c : kAssocExtCells) {
        SCOPED_TRACE(assocLabelOf(c));
        test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-assoc-ext-cell"};
        auto const r = dssLinkAssocCell(scratch.path(), c);
        if (c.dss == kRefusedCell) {
            EXPECT_NE(r.rc, 0) << "the union refuses this order: lld-link refuses it, and link.exe's image reads a "
                                  "discarded section";
            EXPECT_NE(r.diagnostics.find("undefined symbol 'y'"), std::string::npos) << r.diagnostics;
        } else {
            EXPECT_EQ(r.rc, 0) << "a reference links this order:" << r.diagnostics;
            EXPECT_FALSE(r.image.empty());
        }
    }
}

// Windows x86_64: each image DSS links runs to the exit the union names -- the kept copy's `x` with its own `y`.
TEST(CoffComdatSelectionNative, EachAssociativeNameCellDssLinksReadsTheKeptSectionsName) {
    if (!runsOnThisHost()) GTEST_SKIP() << "the image runs only where the host is Windows x86_64";
    for (auto const& c : kAssocExtCells) {
        if (c.dss == kRefusedCell) continue;
        SCOPED_TRACE(assocLabelOf(c));
        test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-assoc-ext-cell-run"};
        auto const r = dssLinkAssocCell(scratch.path(), c);
        ASSERT_EQ(r.rc, 0) << r.diagnostics;
        ASSERT_FALSE(r.image.empty());
        auto const run = test_support::runBinary(r.image);
        ASSERT_TRUE(run.spawned) << run.diagnostic;
        EXPECT_FALSE(run.timedOut);
        EXPECT_EQ(run.exitCode, static_cast<std::uint32_t>(c.dss)) << kAssocExit;
    }
}

// Windows x86_64: link.exe and lld-link judge each order as MEASURED, each image run -- link.exe's refusal of the two
// copies' `y` and its image of a lost copy's `y` included. The lld-link half is skipped BY NAME where it is absent.
TEST(CoffComdatSelectionNative, TheReferencesJudgeEachAssociativeNameCellAsMeasured) {
    if (!runsOnThisHost()) GTEST_SKIP() << "link.exe and lld-link run only where the host is Windows x86_64";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-assoc-ext-refs"};
    auto const dir  = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const tools = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(tools.ready()) << tools.describe();
    std::ofstream(dir / "main_nodup.c", std::ios::binary) << kMainAssocNodup;
    std::ofstream(dir / "main_any.c", std::ios::binary) << kMainAssocAny;
    ASSERT_TRUE(tools.run("cl /nologo /c /O2 /GS- main_nodup.c /Fomain_nodup.obj")) << "cl compiles the C main";
    ASSERT_TRUE(tools.run("cl /nologo /c /O2 /GS- main_any.c /Fomain_any.obj")) << "cl compiles the C main";
    for (auto const* name : {"nodup", "any_a", "any_b", "any_c"}) {
        auto const bytes = fixture(assocFixture(name));
        ASSERT_FALSE(bytes.empty());
        std::ofstream(dir / assocFixture(name), std::ios::binary)
            .write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    auto const lld = test_support::native_probe::locateLldLink(dir);
    ASSERT_TRUE(lld.ok() || lld.toolAbsent()) << lld.describe();
    std::vector<std::pair<std::string, bool>> linkers{{"link", true}};
    if (lld.ok()) linkers.emplace_back("\"" + lld.lldLink.string() + "\"", false);
    std::size_t k = 0;
    for (auto const& [linker, isLinkExe] : linkers) {
        for (auto const& c : kAssocExtCells) {
            SCOPED_TRACE(std::string{isLinkExe ? "link.exe, " : "lld-link, "} + assocLabelOf(c));
            std::string objects;
            for (auto const* o : c.objects) objects += " " + assocFixture(o);
            std::string const exe   = "assoc_" + std::to_string(k++) + ".exe";
            std::string const mainO = c.main == kMainAssocNodup ? "main_nodup.obj" : "main_any.obj";
            bool const        linked = tools.run(linker + " /nologo /OUT:" + exe + " " + mainO + objects);
            int const         want   = isLinkExe ? c.linkExe : c.lldLink;
            if (want == kRefusedCell) {
                EXPECT_FALSE(linked) << "MEASURED: this reference refuses this order";
                continue;
            }
            ASSERT_TRUE(linked) << "MEASURED: this reference links this order";
            auto const run = test_support::runBinary(dir / exe);
            ASSERT_TRUE(run.spawned) << run.diagnostic;
            EXPECT_FALSE(run.timedOut);
            EXPECT_EQ(run.exitCode, static_cast<std::uint32_t>(want)) << kAssocExit;
        }
    }
    if (linkers.size() == 1u) {
        GTEST_SKIP() << "link.exe judged each order; the lld-link half was NOT read: " << lld.detail;
    }
}

// ══ A LOST COPY'S WEAK ALIAS ══════════════════════════════════════════════════════════════════════════════════════════
//
// Lane cs's probe wk1, cell 3, as foreign objects (`tests/link/data/coff_carried_alias.source.c` states the units and
// how each object was built): unit a a select-any `w = 7` that it reads, unit b another `w = 9` with `m` a WEAK alias
// of it, which b reads. Beside a `main` returning `ua() * 1000 + ub() * 100 + w * 10 + m`, the order a b keeps a's `w`
// and discards b's copy, carrying `m`. What `m` then is depends on how the alias is carried:
//   * clang-cl 19.1.5 /O2: `m` a weak external whose default is `w`, the COMDAT's own symbol -- `m` reads the kept `w`;
//   * MinGW-w64 gcc 13.2.0 -O2: `m` a weak external whose default is `.weak.m.ub`, a label inside `w`'s COMDAT, which
//     goes with the copy -- `m` is undefined.
// ✔MEASURED P69 fold 3d (lane `xa`'s `carried.py`; link.exe 14.44.35229, lld-link 19.1.5, beside a cl /O2 `main`, each
// image run): clang-cl's objects 7777 in a b and 9999 in b a under both; gcc's refused in a b by both ("unresolved
// external m" / "undefined symbol: m"), and in b a run 9999 under lld-link and refused by link.exe. MinGW-w64 GNU
// ld 2.42 (gcc 13.2.0's driver, beside a gcc -O2 `main`; lane `xa`'s `carried_bfd.py`, 2026-11-03) refuses all four
// ("undefined reference to `m'"): its PE backend resolves no weak external across objects (the note at the
// weak-external arm of src/link/format/pe.cpp), so its column decides no cell's union here. md5:
//   a_clangcl 6528ce0c  b_clangcl 6848bbcb (clang-cl stamps TimeDateStamp: a rebuild differs there)
//   a_gcc 059495ed  b_gcc 4b8f802b (each built twice byte-identical)
// DSS at HEAD (b5efdbb3) ran gcc's objects to 7777 in the order a b: the election named the kept copy for `m`.

namespace {

constexpr char const* kCarriedMain = "int ua(void); int ub(void);\nextern int w;\nextern int m;\n"
                                     "int main(void) { return ua() * 1000 + ub() * 100 + w * 10 + m; }\n";

struct CarriedCell {
    char const* family;   // clangcl | gcc
    bool        aFirst;
    int         dss;      // the union: the exit code, or kRefusedCell
    int         linkExe;  // MEASURED
    int         lldLink;  // MEASURED
    int         gnuLd;    // MEASURED: MinGW-w64 GNU ld 2.42, through gcc 13.2.0's driver
};

std::vector<CarriedCell> const kCarriedCells = {
    {"clangcl", true, 7777, 7777, 7777, kRefusedCell},
    {"clangcl", false, 9999, 9999, 9999, kRefusedCell},
    {"gcc", true, kRefusedCell, kRefusedCell, kRefusedCell, kRefusedCell},
    {"gcc", false, 9999, kRefusedCell, 9999, kRefusedCell},
};

[[nodiscard]] std::string carriedFixture(char const* family, char unit) {
    return std::string{"coff_carried_alias_"} + unit + "_" + family + "_O2.obj";
}

[[nodiscard]] std::vector<std::string> carriedOrder(CarriedCell const& c) {
    return c.aFirst ? std::vector<std::string>{carriedFixture(c.family, 'a'), carriedFixture(c.family, 'b')}
                    : std::vector<std::string>{carriedFixture(c.family, 'b'), carriedFixture(c.family, 'a')};
}

[[nodiscard]] std::string carriedLabelOf(CarriedCell const& c) {
    return std::string{c.family} + (c.aFirst ? ": a then b" : ": b then a");
}

}  // namespace

// Every leg: the reader states each alias's default, and DSS links each order the union accepts and refuses the other
// naming the default left undefined. RED-ON-DISABLE: take every carried name with its copy -> clang-cl's a b refused;
// resolve a staying name to the copy's own name -> gcc's a b links (HEAD's 7777).
TEST(CoffComdatSelection, ALostCopysWeakAliasResolvesToItsDefault) {
    auto const l = load();
    ASSERT_TRUE(l.target && l.object);
    for (auto const& [family, defaultName] : {std::pair<char const*, char const*>{"clangcl", "w"},
                                              std::pair<char const*, char const*>{"gcc", ".weak.m.ub"}}) {
        SCOPED_TRACE(family);
        auto const bytes = fixture(carriedFixture(family, 'b'));
        ASSERT_FALSE(bytes.empty());
        DiagnosticReporter rep;
        auto const unit = read(l, bytes, 2, rep);
        ASSERT_TRUE(unit.has_value()) << diagnosticsOf(rep);
        bool named = false;
        for (auto const& s : unit->symbols) {
            if (s.name != "m") continue;
            named = true;
            EXPECT_EQ(s.binding, SymbolBinding::Weak);
            EXPECT_EQ(s.weakDefaultName, defaultName) << "the weak external's default, by name";
        }
        EXPECT_TRUE(named) << "the reader states `m` on its body";
    }
    for (auto const& c : kCarriedCells) {
        SCOPED_TRACE(carriedLabelOf(c));
        test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-carried-alias"};
        auto const r = dssLinkFixtures(scratch.path(), carriedOrder(c), kCarriedMain);
        if (c.dss == kRefusedCell) {
            EXPECT_NE(r.rc, 0) << "no reference links this order: `m`'s default went with the discarded copy";
            EXPECT_NE(r.diagnostics.find("K_SymbolUndefined"), std::string::npos) << r.diagnostics;
            EXPECT_NE(r.diagnostics.find("'m' of linked unit"), std::string::npos)
                << "the refusal names the name: " << r.diagnostics;
            EXPECT_NE(r.diagnostics.find("is a weak name whose default '.weak.m.ub' is a name of its copy in section "
                                         "group 'w', whose copy the link keeps from linked unit"),
                      std::string::npos)
                << "and the lost copy it came from: " << r.diagnostics;
        } else {
            EXPECT_EQ(r.rc, 0) << "a reference links this order:" << r.diagnostics;
            EXPECT_FALSE(r.image.empty());
        }
    }
}

// Windows x86_64: each image DSS links runs to the union's exit, and link.exe, lld-link and MinGW-w64 GNU ld judge
// each order as MEASURED. The orders the union refuses are DSS's to refuse too: read by name, on every leg, by
// `CoffComdatSelection.ALostCopysWeakAliasResolvesToItsDefault` -- this TEST runs only what DSS links.
TEST(CoffComdatSelectionNative, ALostCopysWeakAliasReadsWhatTheReferencesRead) {
    if (!runsOnThisHost()) GTEST_SKIP() << "the images run only where the host is Windows x86_64";
    for (auto const& c : kCarriedCells) {
        if (c.dss == kRefusedCell) continue;
        SCOPED_TRACE(carriedLabelOf(c));
        test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-carried-alias-run"};
        auto const r = dssLinkFixtures(scratch.path(), carriedOrder(c), kCarriedMain);
        ASSERT_EQ(r.rc, 0) << r.diagnostics;
        auto const run = test_support::runBinary(r.image);
        ASSERT_TRUE(run.spawned) << run.diagnostic;
        EXPECT_FALSE(run.timedOut);
        EXPECT_EQ(run.exitCode, static_cast<std::uint32_t>(c.dss)) << "ua * 1000 + ub * 100 + w * 10 + m";
    }
    // MinGW-w64 GNU ld, through gcc's driver beside a gcc `main`: each order against its MEASURED column.
    bool        gnuRead = false;
    std::string gnuDetail;
    {
        test_support::ScratchDir gnuScratch{test_support::Location::InsideRepo, "coff-carried-alias-gnu"};
        auto const gdir = gnuScratch.path();
        auto const gcc  = test_support::native_probe::locateMingwGcc(gdir);
        ASSERT_TRUE(gcc.ok() || gcc.toolAbsent()) << gcc.describe();
        if (gcc.ok()) {
            std::ofstream(gdir / "main.c", std::ios::binary) << kCarriedMain;
            std::string const at = "cd /d \"" + gdir.string() + "\" && ";
            ASSERT_EQ(std::system(("\"" + at + "gcc -O2 -c main.c -o main_gcc.o >nul 2>&1\"").c_str()), 0)
                << "gcc compiles the C main";
            std::size_t g = 0;
            for (auto const& c : kCarriedCells) {
                SCOPED_TRACE(carriedLabelOf(c) + ", GNU ld");
                std::string objects;
                for (auto const& name : carriedOrder(c)) {
                    auto const bytes = fixture(name);
                    ASSERT_FALSE(bytes.empty());
                    std::ofstream(gdir / name, std::ios::binary)
                        .write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                    objects += " " + name;
                }
                std::string const exe    = "carried_gnu_" + std::to_string(g++) + ".exe";
                bool const        linked =
                    std::system(("\"" + at + "gcc main_gcc.o" + objects + " -o " + exe + " >nul 2>&1\"").c_str()) == 0;
                if (c.gnuLd == kRefusedCell) {
                    EXPECT_FALSE(linked) << "MEASURED: GNU ld refuses this order";
                    continue;
                }
                ASSERT_TRUE(linked) << "MEASURED: GNU ld links this order";
                auto const run = test_support::runBinary(gdir / exe);
                ASSERT_TRUE(run.spawned) << run.diagnostic;
                EXPECT_FALSE(run.timedOut);
                EXPECT_EQ(run.exitCode, static_cast<std::uint32_t>(c.gnuLd));
            }
            gnuRead = true;
        } else {
            gnuDetail = gcc.detail;
        }
    }
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-carried-alias-refs"};
    auto const dir  = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const tools = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(tools.ready()) << tools.describe();
    std::ofstream(dir / "main.c", std::ios::binary) << kCarriedMain;
    ASSERT_TRUE(tools.run("cl /nologo /c /O2 /GS- main.c /Fomain.obj")) << "cl compiles the C main";
    auto const lld = test_support::native_probe::locateLldLink(dir);
    ASSERT_TRUE(lld.ok() || lld.toolAbsent()) << lld.describe();
    std::vector<std::pair<std::string, bool>> linkers{{"link", true}};
    if (lld.ok()) linkers.emplace_back("\"" + lld.lldLink.string() + "\"", false);
    std::size_t k = 0;
    for (auto const& c : kCarriedCells) {
        std::string objects;
        for (auto const& name : carriedOrder(c)) {
            auto const bytes = fixture(name);
            ASSERT_FALSE(bytes.empty());
            std::ofstream(dir / name, std::ios::binary)
                .write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            objects += " " + name;
        }
        for (auto const& [linker, linkExe] : linkers) {
            SCOPED_TRACE(carriedLabelOf(c) + (linkExe ? ", link.exe" : ", lld-link"));
            std::string const exe    = "carried_" + std::to_string(k++) + ".exe";
            bool const        linked = tools.run(linker + " /nologo /OUT:" + exe + " main.obj" + objects);
            int const         want   = linkExe ? c.linkExe : c.lldLink;
            if (want == kRefusedCell) {
                EXPECT_FALSE(linked) << "MEASURED: this reference refuses this order";
                continue;
            }
            ASSERT_TRUE(linked) << "MEASURED: this reference links this order";
            auto const run = test_support::runBinary(dir / exe);
            ASSERT_TRUE(run.spawned) << run.diagnostic;
            EXPECT_FALSE(run.timedOut);
            EXPECT_EQ(run.exitCode, static_cast<std::uint32_t>(want));
        }
    }
    // The lld-link and GNU ld halves are readings too: where one could not run, the test says so rather than passing
    // on the others (a failure above still fails it).
    if (linkers.size() == 1u || !gnuRead) {
        GTEST_SKIP() << "DSS and link.exe judged each order; the lld-link half was "
                     << (linkers.size() == 1u ? "NOT read: " + lld.detail : std::string{"read"})
                     << "; the GNU ld half was " << (gnuRead ? std::string{"read"} : "NOT read: " + gnuDetail);
    }
}

// Every leg: each table's union -- the `dss` column, DSS's answer, which is the DECISION where the references split
// (a meaning fork decided by their measurement and the vendors' documentation, as each table's notes say) -- agrees
// with the references' MEASURED columns: a cell is refused if and only if every reference refuses it, and a cell DSS
// links answers what one of the references that link it answers. So the in-process TESTs that require DSS to
// refuse, by name, each cell the union refuses cannot be emptied by a union typed wrong. RED-ON-DISABLE: type any
// refused cell's `dss` as an answer, or a linked cell's as refused or as an answer no reference gives.
TEST(CoffComdatSelection, EachCellsUnionAgreesWithTheReferencesMeasuredAnswers) {
    auto const agrees = [](int dss, std::initializer_list<int> references) {
        if (dss == kRefusedCell) {
            return std::all_of(references.begin(), references.end(), [](int r) { return r == kRefusedCell; });
        }
        return std::find(references.begin(), references.end(), dss) != references.end();
    };
    for (auto const& c : kSelectionCells) {
        EXPECT_TRUE(agrees(c.dss, {c.linkExe, c.lldLink}))
            << labelOf(c) << ": dss " << c.dss << ", link.exe " << c.linkExe << ", lld-link " << c.lldLink;
    }
    for (auto const& c : kOwnCopyCells) {
        EXPECT_TRUE(agrees(c.dss, {c.linkExe, c.lldLink}))
            << ownLabelOf(c) << ": dss " << c.dss << ", link.exe " << c.linkExe << ", lld-link " << c.lldLink;
    }
    for (auto const& c : kCarriedCells) {
        EXPECT_TRUE(agrees(c.dss, {c.linkExe, c.lldLink, c.gnuLd}))
            << carriedLabelOf(c) << ": dss " << c.dss << ", link.exe " << c.linkExe << ", lld-link " << c.lldLink
            << ", GNU ld " << c.gnuLd;
    }
}

// Every leg: a weak alias's default (`ModuleSymbol::weakDefaultName`) survives a rebuild of its row -- read from
// clang-cl's object (`tests/link/data/coff_weak_alias_of_strong.source.c`; `weak_m` a weak external whose default is
// the strong `strong_w`, md5 0836b857bcbc8f94e4ad0941b815d0bb), merged beside another unit, written as DSS's
// relocatable artifact and read back, it is named the same. RED-ON-DISABLE: the reader states no default, or the
// merge rebuilds the row field by field without it -> the name is empty at that step.
TEST(CoffComdatSelection, AWeakAliasKeepsItsDefaultThroughAMergeAndARelocatableArtifact) {
    auto const l = load();
    ASSERT_TRUE(l.target && l.object);
    auto const defaultOf = [](AssembledModule const& m) -> std::optional<std::string> {
        for (auto const& s : m.symbols) {
            if (s.name == "weak_m") return s.weakDefaultName;
        }
        return std::nullopt;
    };
    auto const aliasBytes = fixture("coff_weak_alias_of_strong_clangcl_O2.obj");
    auto const otherBytes = fixture(staticFixture("any", 1));
    ASSERT_FALSE(aliasBytes.empty() || otherBytes.empty());
    DiagnosticReporter rep;
    auto alias = read(l, aliasBytes, 1, rep);
    auto other = read(l, otherBytes, 2, rep);
    ASSERT_TRUE(alias.has_value() && other.has_value()) << diagnosticsOf(rep);
    EXPECT_EQ(defaultOf(*alias), std::optional<std::string>{"strong_w"}) << "as the reader states it";
    std::vector<AssembledModule> units;
    units.push_back(std::move(*alias));
    units.push_back(std::move(*other));
    auto const linked = linker::link(std::span<AssembledModule const>{units}, *l.target, *l.object, rep);
    ASSERT_TRUE(linked.ok()) << diagnosticsOf(rep);
    auto const back = read(l, linked.bytes, 1, rep);
    ASSERT_TRUE(back.has_value()) << diagnosticsOf(rep);
    EXPECT_EQ(defaultOf(*back), std::optional<std::string>{"strong_w"})
        << "merged, written as a relocatable artifact and read back";
}

// ══ A FRAME'S FUNCLET, ASSOCIATIVE ON CODE (ledgers 488, 489) ════════════════════════════════════════════════════════
//
// cl writes a C `__try` frame's `__finally` body and `__except` filter as funclets in `.text$x` sections that are
// COMDATs of selection ASSOCIATIVE to the function's section (lane hm, cl 19.51 /O2 /GS, run 20261104-205243-dd194040),
// with the frame's `.pdata`/`.xdata` associative too. The rule is Gate 3's, kind never consulted: an associated
// section is kept exactly when the root of its chain is. THE TABLE: {`__finally`, `__except`} x {the parent kept, the
// parent dropped as a duplicate} x {/Gy, no /Gy}, over `coff_seh_comdat_{a,b}_cl_O2.obj` and `..._a_cl_O2_GyOff.obj`
// (`coff_seh_comdat.source.c`): unit A's external functions are parents a link keeps (the only copy), and the C
// `inline` pair both units define is a copy per unit, the later unit's dropped. ✔MEASURED (dumpbin /symbols, run
// 20261104-214704-d4dd3ebb): A's external pair is NODUPLICATES under /Gy and in the ordinary `.text$mn` under /Gy-,
// each `inline` copy is ANY in both units, and B's copies are GS frames (`__GSHandlerCheck_SEH`) where A's are not
// (`__C_specific_handler`) -- so each order of the two units is a cell: A first keeps A's copies, B first keeps B's
// GS frames. Each funclet records that it RAN, so a native exit of 42 is every funclet run; 10 + k names the one
// that was not -- 10 the external `__finally`, 11 the external `__except` filter, 12 and 13 the kept `inline`
// copies'.
// ⚠ A NATIVE CELL DISPATCHES ONLY IF THE IMAGE CARRIES cl's `.pdata`/`.xdata` FOR THE FRAME with its handler
// relocated; fold 3c carries every unwind form but the CHAINED one (D-LK-COFF-FOREIGN-UNWIND-DROP, reopened for it,
// closed in fold 3e). `dumpbin /unwindinfo` decodes nothing in an object, so DSS's own reader is the instrument: it
// refuses a chained record by name at link.
namespace {

struct SehFlags {
    char const* label;
    char const* a;
    char const* b;
    std::size_t aAssociativeFunclets;   // A's `.text$x` COMDATs: its external pair's under /Gy, and its `inline` pair's
};
// ✔MEASURED (run 20261104-214436-0900150a): /O2 implies /Gy -- `/O2` and `/O2 /Gy` write identical objects -- so the
// cell without function-level linking is `/O2 /Gy-`, where A's external pair and its funclets sit in the ordinary
// `.text$mn` / `.text$x` (no COMDAT: the parent no link discards) and only the `inline` copies are COMDATs. B's
// `/Gy-` build is its `/O2` build byte for byte (runs 20261104-214611-5a1e9568, -214704-d4dd3ebb, -214810-38709078):
// B defines only the `inline` pair, a COMDAT either way, so both flag sets read B's one fixture.
// The fixtures (cl 19.51.36260, run 20261104-215318-f697d3fd of the committed source text, each built twice to the
// same bytes, B's /Gy- build among them): A /O2 6836 bytes
// md5 17f9a82a892f7d5ddf276cf204fa150b; A /O2 /Gy- 5707 bytes cf01edc9230b147bbeb0892d8e540fd8; B 3281 bytes
// a589106e0d757ee5196e8f0524ab9169.
constexpr SehFlags kSehFlags[] = {
    {"/O2 (which is /Gy)", "coff_seh_comdat_a_cl_O2.obj", "coff_seh_comdat_b_cl_O2.obj", 4},
    {"/O2 /Gy-", "coff_seh_comdat_a_cl_O2_GyOff.obj", "coff_seh_comdat_b_cl_O2.obj", 2},
};
constexpr char const* kMainSeh = "int seh_comdat_check(void);\nint main(void) { return seh_comdat_check(); }\n";
constexpr char const* kSehExit =
    "42 is every funclet run; 10 the external `__finally`, 11 the external `__except` filter, 12 and 13 the kept "
    "`inline` copies' -- an exception that never reached a funclet reads its number";

// The two link orders of a flag set: the first unit's `inline` copies are the ones kept.
[[nodiscard]] std::vector<std::vector<std::string>> ordersOf(SehFlags const& f) {
    return {{f.a, f.b}, {f.b, f.a}};
}

[[nodiscard]] std::string orderName(std::vector<std::string> const& order) {
    return order.front().find("_a_") != std::string::npos ? "A then B" : "B then A";
}

// A PE image's `.pdata` virtual size: one 12-byte RUNTIME_FUNCTION per function the image unwinds. 0 when absent.
[[nodiscard]] std::uint32_t pdataSizeOf(std::vector<std::uint8_t> const& img) {
    if (img.size() < 0x40u) return 0;
    std::uint32_t const pe = u32(img, 0x3C);
    if (img.size() < pe + 24u) return 0;
    std::uint16_t const nsec = u16(img, pe + 6);
    std::uint16_t const opt  = u16(img, pe + 20);
    for (std::uint16_t i = 0; i < nsec; ++i) {
        std::size_t const h = pe + 24u + opt + 40u * i;
        if (img.size() < h + 40u) return 0;
        std::string name;
        for (std::size_t k = 0; k < 8u && img[h + k] != 0u; ++k) name.push_back(static_cast<char>(img[h + k]));
        if (name == ".pdata") return u32(img, h + 8);
    }
    return 0;
}

// The `.pdata` bytes a unit's `inline` copies carry, read off the object: every `.pdata` section whose chain of
// associative sections roots in the section of `seh_inline_finally` or `seh_inline_except` (a funclet's records root
// there through the funclet's own section).
[[nodiscard]] std::uint32_t inlineCopiesPdataOf(std::vector<CoffSection> const& secs) {
    auto const rootOf = [&](std::size_t i) {
        for (std::size_t hops = 0; hops <= secs.size(); ++hops) {
            CoffSection const& s = secs[i];
            if (s.selection != kSelAssociative || s.number == 0u || s.number > secs.size()) return i;
            i = static_cast<std::size_t>(s.number) - 1u;
        }
        ADD_FAILURE() << "the object's associative sections form a cycle";
        return i;
    };
    std::uint32_t bytes = 0;
    for (std::size_t i = 0; i < secs.size(); ++i) {
        if (secs[i].name != ".pdata") continue;
        std::string const& root = secs[rootOf(i)].comdatSymbol;
        if (root == "seh_inline_finally" || root == "seh_inline_except") bytes += secs[i].size;
    }
    return bytes;
}

// The link order with a THIRD unit after the two: a second file of B's bytes, whose every definition is an `inline`
// copy a unit before it already holds -- so all of it is dropped, whichever unit's copies are kept.
[[nodiscard]] std::vector<FixtureFile> withBAgain(std::vector<std::string> const& order, SehFlags const& f) {
    std::vector<FixtureFile> files;
    for (auto const& name : order) files.push_back({name, name});
    files.push_back({f.b, "coff_seh_comdat_b_again.obj"});
    return files;
}

}  // namespace

// Every leg, THE READ: each object's `.text$x` COMDATs are of selection ASSOCIATIVE (A's external pair's under /Gy,
// each unit's `inline` pair's), each unit's `inline` copy is a code COMDAT of selection ANY -- the premise "the later
// unit's copy is a duplicate the link drops" is read off the objects, on both flag sets -- and each unit's copies
// carry `.pdata` of their own. THE LINK, in each order, two images that differ by what is DROPPED alone:
//   * a third unit of B's bytes after the two adds no `.pdata`: every copy it holds is dropped, and every funclet,
//     `.pdata` and `.xdata` section associative to it with it;
//   * B first carries A first's `.pdata` less A's copies' records plus B's (the counts read off the objects): the
//     copies of the unit that comes later are dropped with their records, A's as B's.
// Neither compares an image with one that leaves a unit out: B's copies name the /GS routines (`__GSHandlerCheck_SEH`),
// whose runtime member a dropped copy's reference fetches as a kept one's does. ✔MEASURED (P69 fold 3d, run 20261104-222355-dd564927, windows-x86_64-debug): A
// alone links 132 bytes of `.pdata`, A and B 240 in each order and on both flag sets; that the 108 are that member's
// records is INFERRED, and the third-unit cell is what reads that a dropped copy leaves none.
// RED-ON-DISABLE: refuse an associative section whose root is NODUPLICATES (HEAD's reader) -> the /Gy links are
// refused; state no group for an associative section -> the dropped copies' records outlive them.
TEST(CoffComdatSelection, EachFuncletGoesWithItsFunctionWhetherItsCopyIsKeptOrDropped) {
    for (auto const& f : kSehFlags) {
        SCOPED_TRACE(f.label);
        for (char const* object : {f.a, f.b}) {
            SCOPED_TRACE(object);
            auto const  secs        = sectionsOf(fixture(object));
            std::size_t associative = 0;
            for (auto const& s : secs) {
                if (s.name != ".text$x" || (s.chars & kScnLnkComdat) == 0u) continue;   // /Gy-: an ordinary section
                EXPECT_EQ(s.selection, kSelAssociative) << "a funclet's `.text$x` COMDAT is of selection ASSOCIATIVE";
                ++associative;
            }
            EXPECT_EQ(associative, object == f.a ? f.aAssociativeFunclets : std::size_t{2})
                << "the associative funclets: A's external pair's under /Gy, and each unit's `inline` pair's";
            for (char const* name : {"seh_inline_finally", "seh_inline_except"}) {
                EXPECT_TRUE(std::any_of(secs.begin(), secs.end(), [&](CoffSection const& s) {
                    return s.comdatSymbol == name && s.selection == kSelAny;
                })) << name << " is a code COMDAT of selection ANY: a copy per unit";
            }
            EXPECT_GT(inlineCopiesPdataOf(secs), 0u) << "the unit's `inline` copies carry `.pdata` of their own";
        }
        std::uint32_t const aCopies = inlineCopiesPdataOf(sectionsOf(fixture(f.a)));
        std::uint32_t const bCopies = inlineCopiesPdataOf(sectionsOf(fixture(f.b)));
        std::optional<std::uint32_t> aFirst;
        std::optional<std::uint32_t> bFirst;
        for (auto const& order : ordersOf(f)) {
            SCOPED_TRACE(orderName(order));
            test_support::ScratchDir both{test_support::Location::InsideRepo, "coff-seh-both"};
            auto const ab = dssLinkFixtures(both.path(), order, kMainSeh);
            ASSERT_EQ(ab.rc, 0) << "DSS links the two units:" << ab.diagnostics;
            std::uint32_t const twoUnits = pdataSizeOf(bytesOf(ab.image));
            EXPECT_GT(twoUnits, 0u) << "the image unwinds the frames";
            test_support::ScratchDir three{test_support::Location::InsideRepo, "coff-seh-three"};
            auto const abb = dssLinkFixtureFiles(three.path(), withBAgain(order, f), kMainSeh);
            ASSERT_EQ(abb.rc, 0) << "DSS links the two units and B's bytes again:" << abb.diagnostics;
            EXPECT_EQ(pdataSizeOf(bytesOf(abb.image)), twoUnits)
                << "every copy of the third unit is dropped, and every `.pdata` record associative to them with them";
            (order.front() == f.a ? aFirst : bFirst) = twoUnits;
        }
        ASSERT_TRUE(aFirst.has_value() && bFirst.has_value());
        EXPECT_EQ(std::int64_t{*bFirst} - std::int64_t{*aFirst}, std::int64_t{bCopies} - std::int64_t{aCopies})
            << "B first keeps B's copies' records in place of A's (" << bCopies << " and " << aCopies
            << " bytes): the later unit's copies are dropped with theirs";
    }
}

// Windows x86_64: each image RUNS every funclet -- the external pair's, kept, and the kept `inline` copies' (A's, or
// B's GS frames), beside the dropped ones -- and link.exe's program of the same objects in the same order beside a
// cl `main` does too, which is the reference that links the mix (ledger 483's condition).
TEST(CoffComdatSelectionNative, EachFuncletRunsWhetherItsCopyIsKeptOrDropped) {
    if (!runsOnThisHost()) GTEST_SKIP() << "the programs run only where the host is Windows x86_64";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-seh-native"};
    auto const dir = scratch.path();
    auto const run42 = [](fs::path const& image) {
        auto const r = test_support::runBinary(image, test_support::kRunBudget);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << kSehExit;
    };
    std::size_t k = 0;
    for (auto const& f : kSehFlags) {
        for (auto const& order : ordersOf(f)) {
            SCOPED_TRACE(std::string{f.label} + ", " + orderName(order));
            auto const sub = dir / ("dss" + std::to_string(k++));
            fs::create_directories(sub);
            auto const ab = dssLinkFixtures(sub, order, kMainSeh);
            ASSERT_EQ(ab.rc, 0) << ab.diagnostics;
            run42(ab.image);
        }
    }
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << "DSS's programs were read; link.exe's were NOT: " << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const tools = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(tools.ready()) << tools.describe();
    std::ofstream(dir / "refmain.c", std::ios::binary) << kMainSeh;
    ASSERT_TRUE(tools.run("cl /nologo /c /O2 /GS- /MD refmain.c /Forefmain.obj")) << "cl compiles the C main";
    for (auto const& f : kSehFlags) {
        for (char const* object : {f.a, f.b}) {
            auto const bytes = fixture(object);
            ASSERT_FALSE(bytes.empty());
            std::ofstream(dir / object, std::ios::binary)
                .write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        for (auto const& order : ordersOf(f)) {
            SCOPED_TRACE(std::string{"link.exe, "} + f.label + ", " + orderName(order));
            std::string const exe = "ref" + std::to_string(k++) + ".exe";
            ASSERT_TRUE(tools.run("link /nologo /OUT:" + exe + " refmain.obj " + order[0] + " " + order[1]))
                << "link.exe links the two units";
            run42(dir / exe);
        }
    }
}
