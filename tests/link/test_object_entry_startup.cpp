// P69 fold 3e, lane p69/xa (class C, N5): the C runtime's startup a linked object STATES as the image's entry is an
// input of the program's ONE entry election.
//
// ✔MEASURED before (lane `xa`'s n5_universe.py and build_startup_fixtures.py; cl 19.44 /MD objects, link.exe 14.44
// and lld-link 19.1.5, the request as the object's directive or as the link's own `/ENTRY:`, each object alone,
// beside an object defining `main`, and after it; both linkers agree in every cell):
//   * `/ENTRY:wmainCRTStartup` beside `main` runs `wmain` (22) in either order: the stated startup RESTRICTS the
//     program's entry to the functions it calls, and the precedence of a build that states none (`main` before
//     `wmain`, the pe64 exec document's `entryPrecedence`) does not apply. DSS refused the object
//     (K_LinkerDirectiveUnhonourable, "a DSS image's startup calls the language's `main`" -- false since DSS realizes
//     `wmain`'s verbs), and with the object's `wmain` an entry candidate the precedence would have taken `main`.
//   * `/ENTRY:wmainCRTStartup` with no `wmain` defined is refused by both (LNK2019 `wmain`).
//   * `/ENTRY:<a function>` starts the image at that function with no C runtime (66), `main` beside it an ordinary
//     function.
//   * `/ENTRY:WinMainCRTStartup` / `wWinMainCRTStartup` run `WinMain` (44) / `wWinMain` (55), beside `main` too. DSS's
//     language declares no such entry and no DSS startup makes their four arguments, so DSS refuses both by name,
//     with that reason and the row that holds the work.
// Now each `runtimeStartups` entry of the directive vocabulary states the verbs its startup calls an entry with, and
// the named inputs' STANDING entry request (`linker::standingEntryRequest`) scopes the election before it is held
// (`entryElectionScope`): a startup restricts it to its verbs -- and the archive search to their names -- and a
// function stated as the entry, or a startup no DSS image provides, leaves no C entry to elect (the link starts the
// image there, or refuses the request by name).
//
// The fixtures are `tests/link/data/object_entry_startup_*_x86_64_pe_cl_MD_O2.obj` (the source and its build:
// `tests/link/data/object_entry_startup.source.c`). Every cell but the native TEST's links in-process on every leg;
// the native one runs each image where the host is Windows x86_64.

#include "object_entry_support.hpp"

#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace dss;
using namespace dss::test_support::object_entry;

namespace {

constexpr char const* kPe = "x86_64:pe64-x86_64-windows-exec";

[[nodiscard]] std::string startupObject(std::string_view shape) {
    return "object_entry_startup_" + std::string{shape} + "_x86_64_pe_cl_MD_O2.obj";
}

// The three places of the stated object among the program's inputs: alone, before an object defining `main`, after.
struct Placement {
    char const*              label;
    std::vector<std::string> objects;
};
[[nodiscard]] std::vector<Placement> placementsOf(std::string_view shape) {
    std::string const x = startupObject(shape), m = startupObject("mainonly");
    return {{"alone", {x}}, {"beside main", {x, m}}, {"main first", {m, x}}};
}

[[nodiscard]] std::vector<Input> fixturesOf(std::vector<std::string> const& names) {
    std::vector<Input> in;
    for (auto const& n : names) in.push_back(fixture(n));
    return in;
}

// The PE image's imports of the C runtime's argument accessors: which startup's init calls the entry.
[[nodiscard]] std::size_t importsOf(fs::path const& image, char const* symbol) {
    return test_support::dependencyOccurrences(test_support::peImportedSymbols(bytesOf(image)), symbol);
}

}  // namespace

// A stated wide startup enters at `wmain`, alone and beside `main` in either order, with no precedence and no
// ambiguity: the image's init reads the WIDE vector and environment, never the narrow ones. The same for a DSS
// unit's `main` beside the object (the mixed route), and with no `--language` (the target's assembly dialect, which
// declares `wmain` too).
TEST(ObjectEntryStartup, AStatedWideStartupEntersAtWmain) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "object_entry_startup_wide"};
    int k = 0;
    auto cell = [&](std::string const& label, std::vector<Input> const& inputs, std::string const& language) {
        SCOPED_TRACE(label + " (" + grammarLabel(language) + ")");
        Built b;
        build(scratch.path() / std::to_string(k++), inputs, kPe, b, language);
        ASSERT_EQ(b.rc, 0) << b.diagnostics;
        ASSERT_FALSE(b.image.empty());
        EXPECT_FALSE(reported(b.rep, DiagnosticCode::K_ProgramEntryChosenByPrecedence)) << b.diagnostics;
        EXPECT_EQ(importsOf(b.image, "__p___wargv"), 1u);
        EXPECT_EQ(importsOf(b.image, "_get_initial_wide_environment"), 1u);
        EXPECT_EQ(importsOf(b.image, "__p___argv"), 0u) << "the narrow startup's vector is never read";
    };
    for (auto const& p : placementsOf("wmain")) cell(p.label, fixturesOf(p.objects), "c");
    cell("alone", fixturesOf({startupObject("wmain")}), "");
    cell("a DSS unit's main first",
         {written("main.c", "int main(void) { return 11; }\n"), fixture(startupObject("wmain"))}, "c");
    cell("a DSS unit's main after",
         {fixture(startupObject("wmain")), written("main.c", "int main(void) { return 11; }\n")}, "c");
}

// The startup's entry only an archive member defines is pulled for it, beside `main` too (✔MEASURED, n5_archive.py:
// link.exe and lld-link run 22 for the wide-only object, `main` and an archive of the `wmain` object, in either
// order): the stated startup's names seed the archive search as they restrict the election.
TEST(ObjectEntryStartup, AStatedStartupsEntryIsPulledFromAnArchive) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "object_entry_startup_archive"};
    Built lib;
    build(scratch.path() / "lib", fixturesOf({startupObject("wmain")}), "x86_64:pe64-x86_64-windows-staticlib", lib);
    ASSERT_EQ(lib.rc, 0) << lib.diagnostics;
    ASSERT_FALSE(lib.image.empty()) << "the archive of the wmain object";
    int k = 0;
    for (auto const& p : placementsOf("wideonly")) {
        SCOPED_TRACE(p.label);
        Built b;
        build(scratch.path() / std::to_string(k++), fixturesOf(p.objects), kPe, b, "c", {lib.image});
        ASSERT_EQ(b.rc, 0) << b.diagnostics;
        EXPECT_EQ(importsOf(b.image, "__p___wargv"), 1u) << "the image enters at the archive's wmain";
        EXPECT_EQ(importsOf(b.image, "__p___argv"), 0u);
    }
}

// A stated narrow startup enters at `main`.
TEST(ObjectEntryStartup, AStatedNarrowStartupEntersAtMain) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "object_entry_startup_narrow"};
    Built b;
    build(scratch.path(), fixturesOf({startupObject("main")}), kPe, b);
    ASSERT_EQ(b.rc, 0) << b.diagnostics;
    EXPECT_EQ(importsOf(b.image, "__p___argv"), 1u);
    EXPECT_EQ(importsOf(b.image, "__p___wargv"), 0u);
}

// A function stated as the entry starts the image with no C runtime: no C entry is elected, so no init reads any
// argument, and `main` beside it is an ordinary function.
TEST(ObjectEntryStartup, AStatedFunctionIsTheImagesEntry) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "object_entry_startup_raw"};
    int k = 0;
    for (auto const& p : placementsOf("raw")) {
        SCOPED_TRACE(p.label);
        Built b;
        build(scratch.path() / std::to_string(k++), fixturesOf(p.objects), kPe, b);
        ASSERT_EQ(b.rc, 0) << b.diagnostics;
        EXPECT_FALSE(reported(b.rep, DiagnosticCode::K_ProgramEntryChosenByPrecedence)) << b.diagnostics;
        for (char const* accessor : {"__p___argc", "__p___argv", "__p___wargv"}) {
            EXPECT_EQ(importsOf(b.image, accessor), 0u) << accessor << ": no C entry is called";
        }
    }
}

// A stated startup whose entry no input defines is refused by the election, naming the entry the startup calls and
// whose directive restricted it (both references: LNK2019 `wmain`, beside `main` too).
TEST(ObjectEntryStartup, AStartupWhoseEntryNoInputDefinesIsRefused) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "object_entry_startup_wide_only"};
    int k = 0;
    for (auto const& p : placementsOf("wideonly")) {
        SCOPED_TRACE(p.label);
        Built b;
        build(scratch.path() / std::to_string(k++), fixturesOf(p.objects), kPe, b);
        EXPECT_NE(b.rc, 0);
        EXPECT_TRUE(b.image.empty());
        std::string const why = textOf(b.rep, DiagnosticCode::K_ProgramEntryUndefined);
        EXPECT_NE(why.find("'wmain'"), std::string::npos) << b.diagnostics;
        EXPECT_NE(why.find("/ENTRY:wmainCRTStartup"), std::string::npos) << b.diagnostics;
    }
}

// The GUI startups are refused by name, with the true reason: what each calls, that no DSS startup makes it, and the
// row that holds the work.
TEST(ObjectEntryStartup, TheGuiStartupsAreRefusedByName) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "object_entry_startup_gui"};
    int k = 0;
    for (auto const& [shape, callee] : {std::pair{"winmain", "`WinMain`"}, std::pair{"wwinmain", "`wWinMain`"}}) {
        for (auto const& p : placementsOf(shape)) {
            SCOPED_TRACE(std::string{shape} + ", " + p.label);
            Built b;
            build(scratch.path() / std::to_string(k++), fixturesOf(p.objects), kPe, b);
            EXPECT_NE(b.rc, 0);
            EXPECT_TRUE(b.image.empty());
            std::string const why = textOf(b.rep, DiagnosticCode::K_LinkerDirectiveUnhonourable);
            EXPECT_NE(why.find(callee), std::string::npos) << b.diagnostics;
            EXPECT_NE(why.find("D-LK-PE-IMAGE-CANNOT-HONOUR-EVERY-LINKER-DIRECTIVE"), std::string::npos)
                << b.diagnostics;
        }
    }
}

// Each image runs as both references' image of the same objects does (Windows x86_64 hosts).
TEST(ObjectEntryStartupNative, EachProgramExitsAsTheReferencesDo) {
    if (test_support::currentHostOs() != "windows" || test_support::currentHostArch() != "x86_64") {
        GTEST_SKIP() << "the images run on Windows x86_64 hosts only";
    }
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "object_entry_startup_native"};
    int k = 0;
    auto run = [&](std::string const& label, std::vector<Input> const& inputs, std::uint32_t exit,
                   std::vector<fs::path> const& archives = {}) {
        SCOPED_TRACE(label);
        Built b;
        build(scratch.path() / std::to_string(k++), inputs, kPe, b, "c", archives);
        ASSERT_EQ(b.rc, 0) << b.diagnostics;
        auto const r = test_support::runBinary(b.image, test_support::kRunBudget, /*captureStdout=*/false,
                                               /*launcher=*/{}, /*programArgs=*/{}, /*launcherExecsImage=*/false);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, exit) << "link.exe and lld-link: " << exit;
    };
    for (auto const& p : placementsOf("wmain")) run(std::string{"wmain, "} + p.label, fixturesOf(p.objects), 22u);
    run("main", fixturesOf({startupObject("main")}), 33u);
    for (auto const& p : placementsOf("raw")) run(std::string{"raw, "} + p.label, fixturesOf(p.objects), 66u);
    run("a DSS unit's main beside wmain",
        {written("main.c", "int main(void) { return 11; }\n"), fixture(startupObject("wmain"))}, 22u);
    Built lib;
    build(scratch.path() / "lib", fixturesOf({startupObject("wmain")}), "x86_64:pe64-x86_64-windows-staticlib", lib);
    ASSERT_EQ(lib.rc, 0) << lib.diagnostics;
    for (auto const& p : placementsOf("wideonly")) {
        run(std::string{"the wide-only object, "} + p.label + ", wmain in an archive", fixturesOf(p.objects), 22u,
            {lib.image});
    }
}
