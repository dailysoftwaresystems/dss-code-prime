// P69 fold 3d, lane p69/xa: the sections of a mingw-w64 gcc object, each MODELLED or REFUSED BY NAME.
//
//   D-LK-COFF-READER-REFUSES-GNU-DOTTED-TEXT-SUBSECTIONS: gcc names a function's section by what the function is
//     (GNU's `.` subsections of `.text`: `main` and the constructors in `.text.startup`, a cold path in
//     `.text.unlikely`, a hot function in `.text.hot`, the destructors in `.text.exit`), and the COFF reader
//     resolved a section by its `$` group alone, so it refused every such object ("defined symbol 'main' lives in
//     section '.text.startup' which resolves to no known code/data section kind"). A base the document does not name
//     is now the row whose name is its longest `<row>.` prefix, as the ELF reader's rule (2) and GNU ld's PE script
//     read it. The row's own object is fold 3c's unwind census's `cleanup` function under -fexceptions, whose cold
//     path `c.cold` gcc -O2 puts in `.text.unlikely`: it reads, links and runs here.
//   THE SAME CHANGE, GATE 2: a relocation-bearing section whose name resolves to no row used to be SKIPPED with its
//     relocations, whatever it held. It is passed over now only when the unwind carry reads it (`.pdata` / `.xdata`)
//     or the object marks it not for the image (DISCARDABLE, LNK_REMOVE, LNK_INFO: the debug sections), and refused
//     otherwise. ✔MEASURED (lane `xa`'s census of 2276 x86_64 COFF objects of every producer): the sections that
//     refusal reaches are the initializer and terminator tables alone -- GNU's `.ctors` / `.dtors` (with a
//     `.NNNNN` priority), clang-cl's `.CRT$XC*` / `.CRT$XT*`, cl's `.CRT$XCU` under its static pointer -- and
//     before it a DSS link dropped each one and the functions it lists never ran, in silence (✔MEASURED, run 24's
//     dsscp: a DSS `main` beside cl's `.CRT$XCU` member printed `flag=0` and exited 0, link.exe's program 42;
//     D-LK-FOREIGN-OBJECT-INITIALIZER-TABLES-NEVER-RUN runs them).
//
// The fixtures (`tests/link/data/coff_gnu_subsections.source.c` states how each was built: each built twice,
// byte-identical, recording no host path):
//   coff_gnu_subsections_cold_x86_64_pe_gcc_O2.obj                    md5 c5b7543456011fb895b15f53003ab809
//   coff_gnu_subsections_cold_x86_64_pe_gcc_O2_function_sections.obj  md5 9a5edc2e486584c4e88dab5b33832182
//   coff_gnu_subsections_cold_x86_64_pe_gcc_O2_g.obj                  md5 8532e914f3f3a8455dd83ae53454bb12
//   coff_gnu_subsections_cleanup_x86_64_pe_gcc_O0_fexceptions.obj     md5 873462e041eaec047c33ded2d85a74e4
//   coff_gnu_subsections_cleanup_x86_64_pe_gcc_O2_fexceptions.obj     md5 ef54a09b5390aeffbc2120630ab4cc97
//   coff_gnu_subsections_ctor_x86_64_pe_gcc_O0.obj                    md5 2ec0fa476f0b54933f1866ad9b10126e
//   coff_gnu_subsections_ctor_x86_64_pe_gcc_O2.obj                    md5 4c3a9acc5adb835038853df071a7472e
//   coff_gnu_subsections_ctor_x86_64_pe_clangcl_MD_O2.obj             md5 5af8d168f3b6a395e8781f49dad7594e
//   coff_gnu_subsections_ctor_x86_64_pe_cl_MD_O2.obj                  md5 323404bb8184aaf7d5b2c894f2fef84c
// gcc + GNU ld links each cold object beside a gcc `main` to a program that exits 43, and each cleanup object beside
// gcc's `main` to one that exits 42; each ctor object, linked by gcc (the gcc ones) or link.exe (the clang-cl and cl
// ones), runs to 11. No guard enforces an md5. The largest is 4132 bytes (the -g object's DWARF).
//
// Every TEST but the `...Native` one runs in-process on every leg.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/object_format_schema.hpp"
#include "program/program.hpp"

#include "host_native_target.hpp"
#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

constexpr char const* kCold[] = {
    "coff_gnu_subsections_cold_x86_64_pe_gcc_O2.obj",
    "coff_gnu_subsections_cold_x86_64_pe_gcc_O2_function_sections.obj",
};
constexpr char const* kColdWithDebugInfo = "coff_gnu_subsections_cold_x86_64_pe_gcc_O2_g.obj";
constexpr char const* kCleanup[] = {
    "coff_gnu_subsections_cleanup_x86_64_pe_gcc_O0_fexceptions.obj",
    "coff_gnu_subsections_cleanup_x86_64_pe_gcc_O2_fexceptions.obj",
};
constexpr char const* kCleanupO2 = "coff_gnu_subsections_cleanup_x86_64_pe_gcc_O2_fexceptions.obj";

// Each object registering its constructors and destructors in tables, and the table its refusal names: the first
// section, in the object's order, that nothing places.
struct TableObject {
    char const* name;
    char const* table;
};
constexpr TableObject kTables[] = {
    {"coff_gnu_subsections_ctor_x86_64_pe_gcc_O0.obj", ".ctors"},
    {"coff_gnu_subsections_ctor_x86_64_pe_gcc_O2.obj", ".ctors"},
    {"coff_gnu_subsections_ctor_x86_64_pe_clangcl_MD_O2.obj", ".CRT$XCA00101"},
    {"coff_gnu_subsections_ctor_x86_64_pe_cl_MD_O2.obj", ".CRT$XCU"},
};

constexpr char const* kMain = "int ext(int v) { return v * 2; }\nint pick(int);\n"
                              "int main(void) { return pick(-3) + pick(4) + 50; }\n";
constexpr char const* kMainCleanup = "int c(void);\nstatic int unlocked;\nvoid unlock(int *p) { unlocked += *p; }\n"
                                     "void g(void) {}\nint main(void) { return c() == 1 && unlocked == 1 ? 42 : 9; }\n";
constexpr char const* kExec = "x86_64:pe64-x86_64-windows-exec";

[[nodiscard]] fs::path fixturePath(std::string_view name) {
    return dss::test::repoRoot() / "tests" / "link" / "data" / std::string{name};
}

[[nodiscard]] std::vector<std::uint8_t> bytesOf(fs::path const& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in.good()) {
        ADD_FAILURE() << "cannot open " << path.string();
        return {};
    }
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  <no diagnostic>"} : s;
}

[[nodiscard]] std::optional<AssembledModule> readObject(char const* name, DiagnosticReporter& rep) {
    auto const target = TargetSchema::loadShipped("x86_64");
    auto const format = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    EXPECT_TRUE(target.has_value() && format.has_value());
    if (!target.has_value() || !format.has_value()) return std::nullopt;
    auto const bytes = bytesOf(fixturePath(name));
    if (bytes.empty()) return std::nullopt;
    return pe::readRelocatableObject(bytes, **target, **format, rep, CompilationUnitId{1});
}

// The module's function `name`, or nullptr.
[[nodiscard]] AssembledFunction const* functionNamed(AssembledModule const& mod, std::string_view name) {
    auto const sym = std::find_if(mod.symbols.begin(), mod.symbols.end(),
                                  [&](ModuleSymbol const& s) { return s.name == name; });
    if (sym == mod.symbols.end()) return nullptr;
    auto const fn = std::find_if(mod.functions.begin(), mod.functions.end(),
                                 [&](AssembledFunction const& f) { return f.symbol.v == sym->symbol.v; });
    return fn == mod.functions.end() ? nullptr : &*fn;
}

// The name the module gives `id`: a symbol's, or an import's.
[[nodiscard]] std::string nameOf(AssembledModule const& mod, SymbolId id) {
    for (auto const& s : mod.symbols) {
        if (s.symbol == id && !s.name.empty()) return s.name;
    }
    for (auto const& e : mod.externImports) {
        if (e.symbol == id) return e.mangledName;
    }
    return "#" + std::to_string(id.v);
}

// `rare`, `often` and `pick` are each a function of the module, each with the frame its object describes.
void expectTheThreeFunctionsWithTheirFrames(AssembledModule const& mod) {
    for (char const* want : {"rare", "often", "pick"}) {
        auto const* fn = functionNamed(mod, want);
        if (fn == nullptr) {
            ADD_FAILURE() << want << " is a function of the module";
            continue;
        }
        EXPECT_TRUE(fn->cfi.has_value()) << want << " arrives with the frame its object's unwind record describes";
    }
}

// ONE CELL, one function: a cell's ASSERT ends the cell, never the test, so one object's red cannot hide what every
// later object does.

// One cold object: it reads, with its three functions and their frames.
void readsAsTheRow(char const* name) {
    DiagnosticReporter rep;
    auto const mod = readObject(name, rep);
    ASSERT_TRUE(mod.has_value()) << "GNU ld links this object:" << diagnosticsOf(rep);
    expectTheThreeFunctionsWithTheirFrames(*mod);
}

// One object registering a table: refused under K_ObjectSectionNotModelled, naming the table and what it is.
void refusedNamingTheTable(char const* name, char const* table) {
    DiagnosticReporter rep;
    auto const mod = readObject(name, rep);
    EXPECT_FALSE(mod.has_value()) << "the object is refused";
    auto const& all     = rep.all();
    auto const  refusal = std::find_if(all.begin(), all.end(), [](auto const& d) {
        return d.code == DiagnosticCode::K_ObjectSectionNotModelled;
    });
    ASSERT_NE(refusal, all.end()) << diagnosticsOf(rep);
    EXPECT_NE(refusal->actual.find(std::string{"section '"} + table + "'"), std::string::npos)
        << "the refusal names the table: " << refusal->actual;
    EXPECT_NE(refusal->actual.find("initializer or terminator table"), std::string::npos)
        << "and says what it is: " << refusal->actual;
}

// One program: DSS's image of a DSS `main` beside the object runs to the exit gcc's program of it has.
void runsAsTheReferenceDoes(fs::path const& dir, char const* object, char const* mainText, std::uint32_t exit) {
    fs::create_directories(dir);
    auto const mainSrc = dir / "main.c";
    std::ofstream(mainSrc, std::ios::binary) << mainText;
    auto const out = dir / "dss.out";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    DiagnosticReporter rep;
    ASSERT_EQ(p.compileUnits({mainSrc.string(), fixturePath(object).string()}, "c", std::vector<std::string>{kExec},
                             rep),
              0)
        << diagnosticsOf(rep);
    fs::path image;
    for (auto const& e : fs::directory_iterator(out)) {
        if (e.is_regular_file() && e.path().extension() == ".exe") image = e.path();
    }
    ASSERT_FALSE(image.empty());
    auto const r = test_support::runBinary(image);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, exit) << "gcc's program of the same object exits " << exit;
}

}  // namespace

// Every leg: each subsection gcc names (`.text.unlikely`, `.text.hot`, and their `-ffunction-sections` forms
// `.text.unlikely.rare`, `.text.hot.often`) reads as `.text`, and each function's unwind record (`.pdata.unlikely`,
// `.xdata.hot.often`, ...) reaches it. RED-ON-DISABLE: resolve a section by its `$` group alone (the reader before
// this fold) -> "defined symbol 'rare' lives in section '.text.unlikely' which resolves to no known code/data
// section kind".
TEST(CoffGnuSubsections, EachDottedSubsectionOfARowReadsAsTheRow) {
    for (char const* name : kCold) {
        SCOPED_TRACE(name);
        readsAsTheRow(name);
    }
}

// Every leg: the same object built with -g carries nine DWARF sections, each marked DISCARDABLE and six of them with
// relocations (MEASURED: `.debug_info` 16, `.debug_line` 6, `.debug_frame` 6, `.debug_aranges` 4, `.debug_rnglists`
// 3, `.debug_loclists` 2); the reader passes over them and reads the code as without them. (No DSS output carries them:
// D-LK-FOREIGN-NON-ALLOC-SECTIONS-ARE-NOT-CARRIED.) RED-ON-DISABLE: refuse a section the object marks not for the
// image -> the object is refused naming '.debug_info'.
TEST(CoffGnuSubsections, ASectionTheObjectMarksNotForTheImageIsPassedOver) {
    DiagnosticReporter rep;
    auto const mod = readObject(kColdWithDebugInfo, rep);
    ASSERT_TRUE(mod.has_value()) << "GNU ld links this object:" << diagnosticsOf(rep);
    expectTheThreeFunctionsWithTheirFrames(*mod);
}

// Every leg: gcc -O2 -fexceptions's `cleanup` function reads -- `c` from `.text` with the handler its unwind record
// names (`__gcc_personality_seh0`, carried with its language data), its cold path `c.cold` from `.text.unlikely`
// with the record `.pdata.unlikely` gives it. RED-ON-DISABLE: resolve a section by its `$` group alone -> "defined
// symbol 'c.cold' lives in section '.text.unlikely' which resolves to no known code/data section kind" (✔MEASURED
// so in fold 3c's unwind census).
TEST(CoffGnuSubsections, AColdPathReadsWithItsFrameBesideItsHandledFunction) {
    DiagnosticReporter rep;
    auto const mod = readObject(kCleanupO2, rep);
    ASSERT_TRUE(mod.has_value()) << "GNU ld links this object:" << diagnosticsOf(rep);
    auto const* c = functionNamed(*mod, "c");
    ASSERT_NE(c, nullptr) << "c is a function of the module";
    EXPECT_TRUE(c->cfi.has_value()) << "c arrives with its frame";
    ASSERT_TRUE(c->languageHandler.has_value()) << "c arrives with the handler its unwind record names";
    EXPECT_EQ(nameOf(*mod, c->languageHandler->personality), "__gcc_personality_seh0");
    EXPECT_FALSE(c->languageHandler->data.empty()) << "and the language data the handler reads";
    auto const* cold = functionNamed(*mod, "c.cold");
    ASSERT_NE(cold, nullptr) << "c.cold is a function of the module";
    EXPECT_TRUE(cold->cfi.has_value()) << "c.cold arrives with its own frame";
}

// Every leg: an object registering constructors and destructors in tables of addresses -- gcc's `.ctors` /
// `.dtors` at -O0 (its functions in `.text`) and at -O2 (in `.text.startup` / `.text.exit`), clang-cl's
// `.CRT$XC*` / `.CRT$XT*`, cl's `.CRT$XCU` holding a static pointer -- is refused, naming the first table, under K_ObjectSectionNotModelled. RED-ON-DISABLE:
// skip a relocation-bearing section no row names, whatever it holds (Gate 2 before this fold) -> each object reads,
// and the program a link makes of it runs none of its constructors.
TEST(CoffGnuSubsections, AnInitializerTableIsRefusedByName) {
    for (auto const& object : kTables) {
        SCOPED_TRACE(object.name);
        refusedNamingTheTable(object.name, object.table);
    }
}

// Windows x86_64: DSS's program of a DSS `main` beside each object runs as gcc's program of the same object beside
// gcc's `main` does -- each cold object to 43 (`pick(-3)` runs the cold path, `ext(3) + rare(-3)` = 6 - 18, and
// `pick(4)` the hot one, 5), each cleanup object to 42 (`c()` returns 1 and its cleanup ran once). The cleanup objects
// call gcc's handler `__gcc_personality_seh0` and `_Unwind_Resume`, which DSS's PE platform runtime defines.
TEST(CoffGnuSubsectionsNative, EachProgramOfTheSubsectionsRunsAsTheReferencesDoes) {
    if (std::string{test_support::hostNativeTarget().execTarget} != kExec) {
        GTEST_SKIP() << "the images run only where the host is Windows x86_64";
    }
    struct Case {
        char const*   object;
        char const*   main;
        std::uint32_t exit;
    };
    Case const cases[] = {{kCold[0], kMain, 43u}, {kCold[1], kMain, 43u}, {kColdWithDebugInfo, kMain, 43u},
                          {kCleanup[0], kMainCleanup, 42u}, {kCleanup[1], kMainCleanup, 42u}};
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-gnu-subsections-run"};
    std::size_t k = 0;
    for (auto const& cell : cases) {
        SCOPED_TRACE(cell.object);
        runsAsTheReferenceDoes(scratch.path() / std::to_string(k++), cell.object, cell.main, cell.exit);
    }
}
