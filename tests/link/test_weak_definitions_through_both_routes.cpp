// P69 (lane `cs`, fold f26b) — ONE PROGRAM, TWO ROUTES, ONE ANSWER: weak definitions of one name in several units.
//
// A program of several C units reaches the election of its names by one of two routes, and both must give it the
// same answer:
//   * the MERGED route — the driver compiles every unit and merges their MIR (`mergeCuMirs`, the default build of
//     several sources, `Program::compileUnits`);
//   * the UNIT route — the driver compiles each unit to a relocatable object alone, then links the objects
//     (`linker::link`, the route of every object a program is linked from).
// Each route elects through `linker::resolveSettlingCarriedNames`, which asks `linker::carriedNameGoesWithItsCarrier`
// of every name a copy carries, and refuses two different weak definitions of one name another unit uses through
// `linker::weakPairsTheLinkRefuses` (link/common_yield.hpp): one predicate and one rule, which each route reaches with
// its own definitions. These cells compile the SAME units both ways, in both unit orders:
//   * A WEAK ALIAS OF A COPY THAT LOSES (the review of checkpoint 32, F2) — b.c's `m` is a weak alias of its
//     select-any copy of `w`, which loses `w` to a.c's in order a b. PE discards a losing select-any copy WHOLE and a
//     weak alias is a weak definition of its OWN name, never withdrawn with it: it names the copy that won `w` (7 in
//     order a b, 9 in b a), and beside another unit's strong `m` every read follows the name (1). ✔MEASURED, lane cs
//     probe wk1 cells 3 and 1 (2026-10-20): clang-cl 19.1.5's objects under link.exe 14.51 and lld-link 19.1.5, /Od
//     and /O2, both orders — GNU ld refuses cell 3 (mingw-w64 gcc writes the alias's default as a label in the
//     discarded section), so clang's is the working reading. ELF keeps a losing weak copy's bytes, so the alias names
//     b.c's own copy (9 in both orders) — the ELF documents' `supersededDefinition` (keepsItsBytes). ✔MEASURED, lane
//     cs probe wk3 (run 20261022-012517-040e4c16): gcc 13.3.0 under GNU ld 2.42 and clang 18.1.3 under ld.lld 18.1.3,
//     -O0 and -O2, both orders — `m` and b.c's read of it are 9 while a.c reads its own 7 or b.c's 9 by order, and
//     beside a strong `m` every read is 1.
//   * TWO DIFFERENT WEAK DEFINITIONS OF ONE NAME ON PE — the first holds the name while only the units that define it
//     name it, and the pair is refused where another unit uses it (the PE documents' `weakDefinitionBesideWeak`).
//     ✔MEASURED, lane cs probes wk1 and wk2 (2026-10-20; mingw-w64 gcc 13.2.0 -O0/-O2 under GNU ld 2.42, clang-cl
//     19.1.5 under link.exe 14.51 and lld-link 19.1.5): wk2 cells 1 and 2 (data, functions; main reads the name) and
//     wk1 cell 2 are refused by all three linkers; wk2 cell 4, and cell 5 (a weak `m` beside a weak alias `m` of
//     another copy), named only by their definers, link under GNU ld, the first winning (5, and 9 in the other
//     order); wk1 cell 5, two weak aliases whose targets elect one copy, is one definition, and clang's linkers link
//     it (7, and 9).
// Every cell is BUILT both ways on every host — a refused cell must be refused by both routes, naming the rule — and
// a linked cell is RUN by both routes where its image runs (PE on Windows, ELF on Linux): both images exit with the
// value the references read.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "link/object_format_schema.hpp"
#include "program/program.hpp"
#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

struct Family {
    char const* label;
    char const* object;      // the relocatable document a unit is compiled to alone
    char const* image;       // the exec document both routes link
    char const* objectExt;   // the object's file extension
    char const* imageExt;    // the image's file extension
    bool        runsHere;    // whether this host runs the family's images
};

#if defined(_WIN32)
constexpr bool kWindowsHost = true;
#else
constexpr bool kWindowsHost = false;
#endif
#if defined(__linux__) && (defined(__x86_64__) || defined(_M_X64))
constexpr bool kLinuxX64Host = true;
#else
constexpr bool kLinuxX64Host = false;
#endif

constexpr Family kPe{"PE x86_64", "x86_64:pe64-x86_64-windows", "x86_64:pe64-x86_64-windows-exec", ".obj", ".exe",
                     kWindowsHost};
constexpr Family kElf{"ELF x86_64", "x86_64:elf64-x86_64-linux", "x86_64:elf64-x86_64-linux-exec", ".o", "",
                      kLinuxX64Host};

struct Unit {
    char const* file;   // "main.c", "a.c", "b.c" or "c.c"
    char const* text;
};

// A cell: its units in order a b (main first, then a.c, b.c, then any others), and what the program's image exits
// with in order a b and in order b a — nullopt where the link is refused.
struct Cell {
    char const*                  name;
    Family const*                family;
    std::vector<Unit>            units;
    std::optional<std::uint32_t> ab;
    std::optional<std::uint32_t> ba;
};

// ── the units ──────────────────────────────────────────────────────────────────────────────────────────────────────
constexpr char kPeCopyA[]  = "__declspec(selectany) int w = 7;\nint ua(void) { return w; }\n";
constexpr char kPeAliasB[] = "__declspec(selectany) int w = 9;\n"
                             "extern int m __attribute__((weak, alias(\"w\")));\n"
                             "int ub(void) { return m; }\n";
constexpr char kElfCopyA[]  = "__attribute__((weak)) int w = 7;\nint ua(void) { return w; }\n";
constexpr char kElfAliasB[] = "__attribute__((weak)) int w = 9;\n"
                              "extern int m __attribute__((weak, alias(\"w\")));\n"
                              "int ub(void) { return m; }\n";
constexpr char kStrongM[] = "int m = 1;\n";
// main reads `m` and b.c's read of it; the two must agree.
constexpr char kMainReadsMAndB[] = "extern int m;\nint ub(void);\n"
                                   "int main(void) { return m == ub() ? m : 100 + m; }\n";

constexpr char kWeakMA[] = "__attribute__((weak)) int m = 5;\nint ua(void) { return m; }\n";
constexpr char kWeakMB[] = "__attribute__((weak)) int m = 9;\nint ub(void) { return m; }\n";
constexpr char kWeakFA[] = "__attribute__((weak)) int f(void) { return 5; }\nint ua(void) { return f(); }\n";
constexpr char kWeakFB[] = "__attribute__((weak)) int f(void) { return 9; }\nint ub(void) { return f(); }\n";
constexpr char kCopyAndWeakMA[] = "__declspec(selectany) int w = 7;\n__attribute__((weak)) int m = 5;\n"
                                  "int ua(void) { return m; }\n";
constexpr char kAliasA[] = "__declspec(selectany) int w = 7;\n"
                           "extern int m __attribute__((weak, alias(\"w\")));\n"
                           "int ua(void) { return m; }\n";
constexpr char kMainReadsM[] = "extern int m;\nint main(void) { return m; }\n";
constexpr char kMainCallsF[] = "int f(void);\nint main(void) { return f(); }\n";
// main names neither `m` nor `f`: only the defining units name it.
constexpr char kMainReadsTheirReads[] = "int ua(void);\nint ub(void);\n"
                                        "int main(void) { return ua() == ub() ? ua() : 100 + ua(); }\n";
constexpr char kMainReadsMAndBoth[] = "extern int m;\nint ua(void);\nint ub(void);\n"
                                      "int main(void) { return (m == ua() && m == ub()) ? m : 100 + m; }\n";

[[nodiscard]] std::vector<Cell> cells() {
    return {
        // F2: a weak alias of a copy that loses.
        {"wk1 cell 3: b.c's weak alias of its select-any copy, main reads it", &kPe,
         {{"main.c", kMainReadsMAndB}, {"a.c", kPeCopyA}, {"b.c", kPeAliasB}}, 7u, 9u},
        {"wk1 cell 1: the same beside c.c's strong m", &kPe,
         {{"main.c", kMainReadsMAndB}, {"a.c", kPeCopyA}, {"b.c", kPeAliasB}, {"c.c", kStrongM}}, 1u, 1u},
        // wk1 cell 4: the same, and only b.c reads `m` — b.c reads the copy that won `w` (clang-cl /Od and /O2 under
        // link.exe and lld-link: ua = ub = w = 7, and 9 in the other order).
        {"wk1 cell 4: b.c's weak alias of its select-any copy, only b.c reads it", &kPe,
         {{"main.c", kMainReadsTheirReads}, {"a.c", kPeCopyA}, {"b.c", kPeAliasB}}, 7u, 9u},
        {"ELF: b.c's weak alias of its weak copy, main reads it", &kElf,
         {{"main.c", kMainReadsMAndB}, {"a.c", kElfCopyA}, {"b.c", kElfAliasB}}, 9u, 9u},
        {"ELF: the same beside c.c's strong m", &kElf,
         {{"main.c", kMainReadsMAndB}, {"a.c", kElfCopyA}, {"b.c", kElfAliasB}, {"c.c", kStrongM}}, 1u, 1u},
        // Two different weak definitions of one name on PE.
        {"wk2 cell 1: two weak data m, main reads m", &kPe,
         {{"main.c", kMainReadsM}, {"a.c", kWeakMA}, {"b.c", kWeakMB}}, std::nullopt, std::nullopt},
        {"wk2 cell 2: two weak functions f, main calls f", &kPe,
         {{"main.c", kMainCallsF}, {"a.c", kWeakFA}, {"b.c", kWeakFB}}, std::nullopt, std::nullopt},
        {"wk2 cell 4: two weak data m, only their units read m", &kPe,
         {{"main.c", kMainReadsTheirReads}, {"a.c", kWeakMA}, {"b.c", kWeakMB}}, 5u, 9u},
        {"wk2 cell 5: a weak m beside a weak alias m of another copy, only their units read m", &kPe,
         {{"main.c", kMainReadsTheirReads}, {"a.c", kCopyAndWeakMA}, {"b.c", kPeAliasB}}, 5u, 9u},
        {"wk1 cell 2: a weak m beside a weak alias m of another copy, main reads m", &kPe,
         {{"main.c", kMainReadsM}, {"a.c", kCopyAndWeakMA}, {"b.c", kPeAliasB}}, std::nullopt, std::nullopt},
        {"wk1 cell 5: two weak aliases m whose targets elect one copy, main reads m", &kPe,
         {{"main.c", kMainReadsMAndBoth}, {"a.c", kAliasA}, {"b.c", kPeAliasB}}, 7u, 9u},
    };
}

void writeText(fs::path const& p, std::string_view text) { std::ofstream(p, std::ios::binary) << text; }

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  (no diagnostics)"} : s;
}

[[nodiscard]] bool refusedByThePairRule(DiagnosticReporter const& rep) {
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_SymbolRedefinedAcrossUnits
            && d.actual.find("weakDefinitionBesideWeak") != std::string::npos)
            return true;
    }
    return false;
}

struct Route {
    bool                built = false;
    fs::path            image;
    DiagnosticReporter  rep;
};

// The MERGED route: every unit, in order, compiled as its OWN translation unit and the units merged into one
// image in `dir` (`Program::compileUnits` -- the route `cc a.c b.c` and the project driver take). Never
// `compileFiles`: of several sources it builds ONE unit, whose files resolve each other's names within it, so
// no election of a unit's definitions would be asked.
void merged(Route& out, Family const& fam, std::vector<Unit> const& units, fs::path const& dir) {
    fs::create_directories(dir);
    std::vector<std::string> paths;
    for (Unit const& u : units) {
        writeText(dir / u.file, u.text);
        paths.push_back((dir / u.file).string());
    }
    Program p;
    p.setOutputDir(dir);
    out.built = p.compileUnits(paths, "c", std::vector<std::string>{fam.image}, out.rep) == 0;
    out.image = dir / (std::string{"main"} + fam.imageExt);
    if (out.built) EXPECT_TRUE(fs::exists(out.image)) << out.image;
}

// The UNIT route: each unit compiled ALONE to a relocatable object, then the objects, in order, linked in `dir`.
void unitByUnit(Route& out, Family const& fam, std::vector<Unit> const& units, fs::path const& dir) {
    fs::create_directories(dir / "objects");
    std::vector<std::string> objects;
    for (Unit const& u : units) {
        fs::path const unitDir = dir / "objects" / fs::path{u.file}.stem();
        fs::create_directories(unitDir);
        writeText(unitDir / u.file, u.text);
        Program            p;
        DiagnosticReporter rep;
        p.setOutputDir(unitDir);
        int const rc = p.compileFiles(std::vector<std::string>{(unitDir / u.file).string()}, "c",
                                      std::vector<std::string>{fam.object}, rep);
        fs::path const object = unitDir / (fs::path{u.file}.stem().string() + fam.objectExt);
        ASSERT_EQ(rc, 0) << u.file << " does not compile alone to an object:" << diagnosticsOf(rep);
        ASSERT_TRUE(fs::exists(object)) << object;
        objects.push_back(object.string());
    }
    Program p;
    p.setOutputDir(dir);
    out.built = p.compileFiles(objects, "c", std::vector<std::string>{fam.image}, out.rep) == 0;
    out.image = dir / (std::string{"main"} + fam.imageExt);
    if (out.built) EXPECT_TRUE(fs::exists(out.image)) << out.image;
}

[[nodiscard]] std::optional<std::uint32_t> exitOf(fs::path const& image) {
    auto const r = test_support::runBinary(image);
    EXPECT_TRUE(r.spawned) << image << ": " << r.diagnostic;
    EXPECT_FALSE(r.timedOut) << image;
    if (!r.spawned || r.timedOut) return std::nullopt;
    return r.exitCode;
}

}  // namespace

// The documents' side of the pair rule: the three PE documents a link resolves units for state it per kind
// (overridable refused where another unit names the name, select-any first wins); the PE archive document, and every
// ELF and Mach-O document, state nothing (the first holds the name); a statement on an archive document, an answer
// that is no answer, and a per-kind statement that leaves a kind out are refused at load.
// RED-ON-DISABLE: drop the loader's reading of the key, and the PE documents state nothing.
TEST(WeakDefinitionsThroughBothRoutes, ThePeDocumentsStateThePairRule) {
    for (char const* name : {"pe64-x86_64-windows", "pe64-x86_64-windows-exec", "pe64-x86_64-windows-dll"}) {
        SCOPED_TRACE(name);
        auto const f = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(f.has_value());
        auto const& s = (*f)->weakDefinitionBesideWeak();
        EXPECT_EQ(s.answerFor(WeakDefinitionKind::Overridable), WeakBesideWeak::RefusedWhereAnotherUnitNamesIt);
        EXPECT_EQ(s.answerFor(WeakDefinitionKind::SelectAny), WeakBesideWeak::FirstWins);
    }
    for (char const* name : {"pe64-x86_64-windows-staticlib", "elf64-x86_64-linux", "elf64-x86_64-linux-exec",
                             "macho64-arm64-darwin", "macho64-arm64-darwin-exec"}) {
        SCOPED_TRACE(name);
        auto const f = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(f.has_value());
        EXPECT_FALSE((*f)->weakDefinitionBesideWeak().stated()) << "the first holds the name";
    }
    auto const stating = [](char const* name, std::optional<nlohmann::json> const& statement) {
        std::ifstream in{dss::test::configRoot() / "object-formats" / (std::string{name} + ".format.json"),
                         std::ios::binary};
        auto doc = nlohmann::json::parse(in);
        doc.erase("weakDefinitionBesideWeak");
        if (statement.has_value()) doc["weakDefinitionBesideWeak"] = *statement;
        return ObjectFormatSchema::loadFromText(doc.dump(), name);
    };
    EXPECT_TRUE(stating("pe64-x86_64-windows-exec", std::nullopt).has_value()) << "the control: no statement loads";
    EXPECT_TRUE(stating("pe64-x86_64-windows-exec", nlohmann::json("firstWins")).has_value())
        << "the control: one answer for every definition loads";
    EXPECT_FALSE(stating("pe64-x86_64-windows-staticlib", nlohmann::json("firstWins")).has_value())
        << "an archive's members are each linked alone: the statement would never be read";
    EXPECT_FALSE(stating("pe64-x86_64-windows-exec", nlohmann::json("sometimes")).has_value());
    EXPECT_FALSE(stating("pe64-x86_64-windows-exec",
                         nlohmann::json{{"overridable", "refusedWhereAnotherUnitNamesIt"}})
                     .has_value())
        << "a per-kind statement states every kind";
}

// RED-ON-DISABLE: let the MERGED route state no weak kind for a name a copy carries, and wk1 cell 3 is refused there
// (the alias withdrawn with its copy, main's read of `m` left with no definition) while the unit route runs it; let
// either route skip the pair rule, and that route links the wk2 cells 1 and 2 and wk1 cell 2 the other refuses.
TEST(WeakDefinitionsThroughBothRoutes, BothRoutesGiveEachCellTheReferencesAnswer) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "weak-definitions-both-routes"};
    std::size_t index = 0;
    for (Cell const& cell : cells()) {
        for (bool const ba : {false, true}) {
            SCOPED_TRACE(std::string{cell.family->label} + ", " + cell.name + (ba ? ", order b a" : ", order a b"));
            std::vector<Unit> units = cell.units;
            if (ba) std::swap(units[1], units[2]);   // main stays first; a.c and b.c trade places
            fs::path const dir = scratch.path() / ("cell" + std::to_string(index++));
            Route m, u;
            merged(m, *cell.family, units, dir / "merged");
            unitByUnit(u, *cell.family, units, dir / "units");
            std::optional<std::uint32_t> const want = ba ? cell.ba : cell.ab;
            if (!want.has_value()) {
                EXPECT_FALSE(m.built) << "the merged route linked a pair the references refuse";
                EXPECT_FALSE(u.built) << "the unit route linked a pair the references refuse";
                EXPECT_TRUE(refusedByThePairRule(m.rep)) << "merged route:" << diagnosticsOf(m.rep);
                EXPECT_TRUE(refusedByThePairRule(u.rep)) << "unit route:" << diagnosticsOf(u.rep);
                continue;
            }
            ASSERT_TRUE(m.built) << "the merged route refused a program the references link:" << diagnosticsOf(m.rep);
            ASSERT_TRUE(u.built) << "the unit route refused a program the references link:" << diagnosticsOf(u.rep);
            if (!cell.family->runsHere) continue;   // built both ways; this host does not run the family's images
            auto const mergedExit = exitOf(m.image);
            auto const unitExit   = exitOf(u.image);
            EXPECT_EQ(mergedExit, unitExit) << "the two routes give one program two answers";
            EXPECT_EQ(mergedExit, want) << "the merged route reads another definition than the references";
            EXPECT_EQ(unitExit, want) << "the unit route reads another definition than the references";
        }
    }
}
