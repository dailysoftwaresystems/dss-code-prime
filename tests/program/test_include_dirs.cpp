// SQLite-testfixture arc C3 (D-FFI-TCL-DESCRIPTOR): the `-I` / `--include-dir`
// quote-include search path, end-to-end through the production driver.
//
// A source in the fixture root includes "helper.h" that lives ONLY in a sibling
// `inc/` directory. Without an include dir, the quote-include fails loud
// (P0016 = P_PreprocessorIncludeError) — the SAME block that stops SQLite's
// testfixture TUs (test_backup.c etc.) from reaching their generated
// `sqlite3.h` in the build dir. WITH the include dir (the `-I` surface,
// Program::setIncludeDirs → applyIncludeDirs → UnitBuilder::addIncludeDir), the
// compile succeeds.
//
// Compile-only + a fixed cross-target so the test is HOST-AGNOSTIC (runs on
// every leg — no artifact is executed here; the run witnesses are the arc's
// empirical probe [pe64 -I → exit 42] + the WSL gate). RED-ON-DISABLE: drop the
// applyIncludeDirs threading in program.cpp (or any -I parse arm in cli_args)
// and the "with include dir" case regresses to the P0016 of case (1).

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "diagnostic_count.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"
#include "unc_spelling.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::test_support;
namespace fs = std::filesystem;

namespace {
fs::path writeFile(fs::path const& dir, std::string_view name,
                   std::string_view text) {
    auto const p = dir / std::string{name};
    std::ofstream f(p);
    f << text;
    return p;
}
}  // namespace

TEST(IncludeDirs, QuoteIncludeResolvesOnlyViaIncludeDir) {
    // A fixed ELF x86_64 target; no artifact is RUN, so this cross-compiles
    // cleanly on every host (x64 / arm64, Windows / Linux / macOS).
    std::string const target = "x86_64:elf64-x86_64-linux-exec";

    ScratchDir scratch{Location::InsideRepo, "include-dirs"};
    auto const dir = scratch.path();
    fs::create_directories(dir / "inc");
    writeFile(dir / "inc", "helper.h", "#define ANSWER 42\n");
    auto const mainSrc = writeFile(
        dir, "main.c", "#include \"helper.h\"\nint main(void){ return ANSWER; }\n");

    // (1) WITHOUT an include dir: helper.h is not in main.c's own directory, so
    //     the quote-include fails loud P0016 and the compile does not succeed.
    {
        Program p;
        p.setOutputDir(dir / "out_no");
        DiagnosticReporter rep;
        int const rc = p.compileFiles(
            std::vector<std::string>{mainSrc.string()}, "c",
            std::vector<std::string>{target}, rep);
        EXPECT_NE(rc, 0) << "a quote-include unreachable without -I must fail";
        EXPECT_GT(countCode(rep, DiagnosticCode::P_PreprocessorIncludeError), 0u)
            << "the fail-loud must be the P0016 quote-include-not-found";
    }

    // (2) WITH the include dir (the -I surface): the quote-include resolves and
    //     the compile succeeds (rc == 0 == emitted, no P0016). Dropping the
    //     applyIncludeDirs threading regresses this to case (1).
    {
        Program p;
        p.setOutputDir(dir / "out_yes");
        p.setIncludeDirs(std::vector<std::string>{(dir / "inc").string()});
        DiagnosticReporter rep;
        int const rc = p.compileFiles(
            std::vector<std::string>{mainSrc.string()}, "c",
            std::vector<std::string>{target}, rep);
        ASSERT_EQ(rc, 0) << "the -I include dir must resolve the quote-include";
        EXPECT_EQ(countCode(rep, DiagnosticCode::P_PreprocessorIncludeError), 0u);
    }
}

// ── The `-I` dir on a UNC share — the DRIVER's threading, not the resolver's ──
//
// ★★★ WHY THIS ARM IS IN THE DRIVER TIER AND NOT WITH THE OTHERS. The resolver
// arms in `tests/core` pin `resolveInDir`, and ✔MEASURED they were ALREADY GREEN
// while this exact scenario failed 0/30 through the real CLI. The defect was one
// tier out: `applyIncludeDirs` ran every `-I` value through a bare
// `fs::absolute`, which on a path model giving a UNC authority no `root_name()`
// does not fail but SUCCEEDS having re-rooted it onto the local drive
// (`//host/share/inc` -> `C:\host\share\inc`). Every unit below the driver was
// handed a directory that cannot exist and answered correctly about it.
//
// ⇒ A pin for a WIRING defect has to run the wiring. This one regresses the
// moment `applyIncludeDirs` goes back to bare `fs::absolute`, which no test in
// `tests/core` can see.
TEST(IncludeDirs, AUncIncludeDirResolvesTheQuoteInclude) {
    std::string const target = "x86_64:elf64-x86_64-linux-exec";

    ScratchDir scratch{Location::InsideRepo, "include-dirs-unc"};
    auto const dir = scratch.path();
    fs::create_directories(dir / "inc");
    writeFile(dir / "inc", "helper.h", "#define ANSWER 42\n");
    auto const mainSrc = writeFile(
        dir, "main.c", "#include \"helper.h\"\nint main(void){ return ANSWER; }\n");

    fs::path const uncInc = dss::test_support::uncSpellingOf(dir / "inc");
    if (uncInc.empty())
        GTEST_SKIP()
            << "D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED: this host offers "
               "no reachable UNC spelling of '"
            << (dir / "inc").string()
            << "', so the DRIVER's UNC `-I` threading WAS NOT MEASURED on this "
               "leg. This is an unmeasured property, NOT a passing one.";
    ASSERT_GE(dss::test_support::leadingSeparatorRun(uncInc), 2u)
        << "the fixture stopped producing a multi-separator root, so this test "
           "would pass without exercising the property: " << uncInc.string();

    // Both spellings of the root, because the driver stores what it was given.
    for (fs::path const& spelling : {uncInc, fs::path{uncInc}.make_preferred()}) {
        Program p;
        p.setOutputDir(dir / "out_unc");
        p.setIncludeDirs(std::vector<std::string>{spelling.string()});
        DiagnosticReporter rep;
        int const rc = p.compileFiles(
            std::vector<std::string>{mainSrc.string()}, "c",
            std::vector<std::string>{target}, rep);
        ASSERT_EQ(rc, 0)
            << "spelling '" << spelling.string()
            << "': a `-I` directory the OS enumerates was made useless by the "
               "driver. The signature is the search dir being re-rooted onto "
               "the local drive before the resolver ever sees it.";
        EXPECT_EQ(countCode(rep, DiagnosticCode::P_PreprocessorIncludeError), 0u)
            << "spelling '" << spelling.string() << "'";
    }
}
