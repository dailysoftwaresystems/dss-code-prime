// ★★★ THE SOURCE-ARGUMENT SHAPE IS AN INPUT, AND THE CORPUS CAN ONLY SPELL ONE
// OF THE FOUR (D-HARNESS-EXAMPLE-RUNNERS-ALWAYS-COMPILE-AN-ABSOLUTE-SOURCE-PATH).
//
// ── WHAT THIS FILE IS FOR ───────────────────────────────────────────────────
// A user hands the driver a source path in one of exactly four shapes:
//
//   1. ABSOLUTE            `<dir>/main.c`
//   2. BARE-RELATIVE       `main.c`          — no directory component at all
//   3. DIRECTORY-RELATIVE  `sub/main.c`
//   4. DOT-RELATIVE        `./main.c`
//
// Both example runners construct shape 1 and only shape 1 — the in-process
// `tests/examples/examples_runner.cpp` builds `srcPaths` as `scratch.path() / s`
// and the CLI-subprocess `integrated_tests/runner.cpp` builds
// `(exampleDir / s).string()`. ⇒ EVERY manifest in the corpus, on every
// declared target arm, exercises the ABSOLUTE form and the other three are
// exercised by NOTHING.
//
// ⚠ NO CORPUS COUNT IS QUOTED HERE ON PURPOSE. `tests/examples/CMakeLists.txt`
// records why: a raw count of a growing corpus is a citation that rots in place.
// It rotted inside the cycle that wrote this file — the corpus measured 615
// manifests at the start and 616 by the gate, a concurrent lane having added an
// example. Read the glob, never a number in a comment.
//
// ★★ THAT HOLE HAS ALREADY SHIPPED A HIGH, USER-FACING DEFECT.
// `D-PP-BARE-RELATIVE-MAIN-PATH-DEFEATS-THE-INCLUDER-DIRECTORY-SEARCH`:
// `dss --compile main.c` refused a header sitting beside `main.c`
// (`error[P0016] quote include not found`) while `./main.c`, `sub/main.c` and an
// absolute path all resolved it — because `fs::path{"main.c"}.parent_path()` is
// the EMPTY path and four sites read empty as "no directory" when it means THE
// PROCESS WORKING DIRECTORY. It reached HEAD behind a green 1,539-test gate.
//
// ── THE CLAIM IS UNIVERSAL OVER THE SHAPES, SO IT IS PER-SHAPE ──────────────
// The P24 operator ruling: a UNIVERSAL claim is per-example, an EXISTENCE claim
// stays one. "The driver resolves an includer-relative header for EVERY source
// argument shape" is universal over a four-element set, so each shape is its own
// `TEST`, named for the shape, and a red names the shape that broke. It is NOT a
// loop inside one case: a loop reports the first failure and hides the rest.
//
// ── AND IT IS PER **ROUTE**, THE SAME WAY `test_driver_argument_supply` IS ───
// `Program` routes on the CU count: one source takes `compileFiles`, two or more
// take `compileUnits` and reach the merged lower. They are separate argument
// lists that a later edit can change independently, so each shape is pinned on
// BOTH — eight cases, not four.
//
// ── WHY THIS TIER AND NOT A CORPUS EXAMPLE ──────────────────────────────────
// Three alternative designs were killed by measurement; the measurements live in
// the registry row this file closes rather than being re-argued here. The short
// form: the shape is not a property of an EXAMPLE, it is a property of how a
// driver is INVOKED. The manifest vocabulary is a CLOSED key set — both runners'
// parsers refuse an unknown key, and a source-level pin holds the two runners'
// key sets equal — so adding a `sourceArgumentShape` key to serve one example
// would be exactly the invent-a-schema-key anti-pattern the P24 ruling names.
// And a new example would witness nothing: both runners would hand it an
// absolute path like every other one.
//
// ⇒ This is the DRIVER tier, which is where `tests/program/` already puts
// questions whose subject is the invocation rather than the corpus. Its
// CLI-subprocess counterpart is the `--only=cli` surface pin in
// `integrated_tests/runner.cpp` (`runSourceArgumentShapePin`): the two halves
// cover the in-process and the argv tier respectively, which is the divergence
// [[D-EXAMPLES-RUNNER-TWO-RUNNERS-MUST-AGREE]] is about.
//
// ── WHAT EACH CASE ASSERTS, AND WHY IT IS NOT "IT COMPILED" ─────────────────
//   * the compile succeeds with ZERO error diagnostics — a bare rc check would
//     be satisfied by a build that returned success over a reported failure;
//   * the artifact lands at the SAME output path for all four shapes — the
//     driver must derive the artifact stem from the source's stem and not from
//     the spelling it was handed, or `sub/main.c` would route a user's artifact
//     somewhere they never asked for;
//   * the produced binary RUNS and returns the value the SIBLING header defines
//     — so a header resolved from anywhere else answers wrong rather than
//     merely differently.
//
// ★ THE DECOY IS THE HALF THAT MAKES THE EXIT CODE DISCRIMINATING. For the two
// shapes whose includer directory is NOT the process working directory
// (absolute and `sub/main.c`), a second header of the SAME NAME is planted in
// the working directory carrying a DIFFERENT value. Resolving against the cwd
// instead of the includer's own directory then produces a binary that builds
// clean, runs, and returns the wrong number — a silent miscompile, which is the
// shape this project refuses. For the other two shapes the includer's directory
// IS the working directory, so no decoy is possible and none is planted; saying
// so beats planting a fixture that cannot discriminate.
//
// ── ONE VARIABLE, SPELLED AS A PREFIX ───────────────────────────────────────
// Every case builds the SAME tree and hands the driver the SAME file names. The
// only thing that changes between them is the PREFIX in front of `main.c`, which
// is what makes "one variable per arm" literally true here rather than a claim
// about the author's discipline.
//
// ── RED-ON-DISABLE (MEASURED) ───────────────────────────────────────────────
// Revert any `includingDirectoryOf` call site in
// `src/analysis/preprocess/preprocessor.cpp` to the pre-fix
// `fs::path{...}.parent_path()` and the BARE-RELATIVE cases here go red at the
// compile assertion with `P_PreprocessorIncludeError`. The other three shapes
// stay green — which is the point: they are the CONTROLS, and a "fix" that broke
// them would be caught here too.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "host_native_target.hpp"  // build for THIS machine — the pin spawns
#include "program/program.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using dss::DiagnosticReporter;
using dss::Program;
using dss::test_support::hostExeArtifact;
using dss::test_support::hostNativeTarget;
using dss::test_support::Location;
using dss::test_support::runBinary;
using dss::test_support::ScratchDir;

namespace {

// The value the header BESIDE the main source defines, and the value the decoy
// in the working directory defines. Distinct, neither of them 0 (which a
// do-nothing binary also returns) and neither of them 42 (the corpus's house
// exit code, so a copy-paste could not have produced either by accident).
constexpr std::uint32_t kSiblingAnswer = 73;
constexpr std::uint32_t kDecoyAnswer   = 11;

// The macro the header defines. Named once: the header text and the source that
// reads it are both derived from this, so the two cannot drift.
constexpr std::string_view kAnswerMacro = "DSS_SOURCE_ARG_SHAPE_ANSWER";

// The header's file name. It is the SAME name in the sibling directory and in
// the decoy, because a decoy under a different name would be unreachable by
// construction and would assert nothing.
constexpr std::string_view kHeaderName = "sibling.h";

constexpr std::string_view kMainName   = "main.c";
constexpr std::string_view kHelperName = "helper.c";

// The subdirectory the sources live in when the includer's directory must DIFFER
// from the process working directory.
constexpr std::string_view kSubdir = "sub";

void writeText(fs::path const& p, std::string const& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    f << text;
}

// The `sub/` spelling, DERIVED from the directory name rather than re-typed: a
// hand-written literal in a case below could drift from the directory the
// fixture creates, and the case would then fail for the wrong reason.
[[nodiscard]] std::string subdirPrefix() {
    return std::string{kSubdir} + "/";
}

[[nodiscard]] std::string headerText(std::uint32_t answer) {
    return "#define " + std::string{kAnswerMacro} + " "
         + std::to_string(answer) + "\n";
}

[[nodiscard]] std::string readAllText(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

[[nodiscard]] std::string allDiagnosticText(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) {
        out += "\n  ";
        out += d.contextPrefix;
        out += ' ';
        out += d.actual;
    }
    return out;
}

// ── THE FIXTURE ────────────────────────────────────────────────────────────
//
//   <scratch>/                     ← always the process working directory
//     sibling.h                    ← the DECOY (planted only when the sources
//                                     live one level down, see the ★ note)
//     [sub/]
//       main.c                     ← `#include "sibling.h"`, returns the macro
//       helper.c                   ← the second CU (merged route only)
//       sibling.h                  ← the header the answer must come from
//
struct ShapeFixture {
    ScratchDir scratch{Location::InsideRepo, "source-arg-shape"};
    fs::path   sourceDir;
    fs::path   outDir;
    bool       merged;

    // `subdirectory` false ⇒ the sources sit in the working directory itself, so
    // no decoy is possible; true ⇒ they sit one level down and the decoy goes in
    // the working directory.
    //
    // `mergedRoute` decides whether `main.c` CALLS the second CU. It is not a
    // cosmetic difference: a sole-CU build of a `main.c` that referenced an
    // undefined `shape_helper` would fail at link for a reason that has nothing
    // to do with the shape under test.
    ShapeFixture(bool subdirectory, bool mergedRoute) : merged(mergedRoute) {
        sourceDir = subdirectory ? scratch.path() / std::string{kSubdir}
                                 : scratch.path();
        outDir    = scratch.path() / "out";
        writeText(sourceDir / std::string{kHeaderName},
                  headerText(kSiblingAnswer));
        if (subdirectory) {
            writeText(scratch.path() / std::string{kHeaderName},
                      headerText(kDecoyAnswer));
        }
        writeText(sourceDir / std::string{kMainName},
                  "#include \"" + std::string{kHeaderName} + "\"\n"
                  + (merged ? "extern int shape_helper(void);\n" : "")
                  + "int main(void) { return " + std::string{kAnswerMacro}
                  + (merged ? " + shape_helper()" : "") + "; }\n");
        if (merged) {
            writeText(sourceDir / std::string{kHelperName},
                      "int shape_helper(void) { return 0; }\n");
        }
        // LAST, deliberately: every path above is absolute, and the cwd move is
        // what makes the three relative spellings below mean anything.
        scratch.useAsCwd();
    }

    // The absolute prefix, for the shape that spells one.
    [[nodiscard]] std::string absolutePrefix() const {
        return sourceDir.generic_string() + "/";
    }

    // What the DRIVER is handed. `prefix` is the single variable across the
    // eight cases; the CU count follows from `merged`, so a caller cannot ask
    // for the merged route and then hand over one file.
    [[nodiscard]] std::vector<std::string>
    sources(std::string_view prefix) const {
        std::vector<std::string> out{std::string{prefix}
                                     + std::string{kMainName}};
        if (merged) {
            out.push_back(std::string{prefix} + std::string{kHelperName});
        }
        return out;
    }
};

// Drive ONE shape through ONE route and assert the whole round trip.
void expectShapeBuildsAndRuns(ShapeFixture const& fx, std::string_view prefix,
                              char const* shape) {
    auto const sources = fx.sources(prefix);
    SCOPED_TRACE(std::string{"shape="} + shape
                 + " sources[0]=" + sources.front());
    std::string const spec{hostNativeTarget().execTarget};

    Program prog;
    prog.setOutputDir(fx.outDir);
    DiagnosticReporter rep;
    // The CU count IS the route: one source takes `compileFiles`, more than one
    // takes `compileUnits` and reaches the merged lower.
    int const rc = (sources.size() == 1)
                       ? prog.compileFiles(sources, "c-subset", {spec}, rep)
                       : prog.compileUnits(sources, "c-subset", {spec}, rep);

    ASSERT_EQ(rc, 0)
        << "the driver must accept this source-argument shape. A refusal here "
           "naming a quote include is the includer-directory derivation reading "
           "an EMPTY parent path as 'no directory' when it means the process "
           "working directory "
           "(D-PP-BARE-RELATIVE-MAIN-PATH-DEFEATS-THE-INCLUDER-DIRECTORY-SEARCH)"
        << allDiagnosticText(rep);
    EXPECT_EQ(rep.errorCount(), 0u)
        << "a zero exit with error-severity diagnostics is a driver that "
           "returned success over a reported failure"
        << allDiagnosticText(rep);

    // The stem is DERIVED from the source's name, never re-typed: the driver's
    // rule is "the artifact is named for the source's stem", and a hand-written
    // "main" here would keep passing if that rule changed.
    auto const exe =
        fx.outDir / hostExeArtifact(fs::path{kMainName}.stem().string());
    ASSERT_TRUE(fs::exists(exe))
        << "the artifact must land at " << exe.generic_string()
        << " for EVERY shape. A different path means the driver derived the "
           "output from the SPELLING it was handed rather than from the "
           "source's stem, so `sub/main.c` would route a user's artifact "
           "somewhere they never asked for";

    auto const r = runBinary(exe);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    ASSERT_FALSE(r.timedOut) << r.diagnostic;
    EXPECT_EQ(r.exitCode, kSiblingAnswer)
        << "the exit code is the value the header BESIDE the main source "
           "defines. Reading " << kDecoyAnswer
        << " means the quote include resolved against the process working "
           "directory instead of the includer's own directory — a build that "
           "succeeds, runs, and answers wrong, with no diagnostic anywhere";
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
// ROUTE: N==1 sole CU → `Program::compileFiles`.
// ════════════════════════════════════════════════════════════════════════════
//
// This is the route the closed defect arrived through: `dss --compile main.c`.

TEST(SourceArgumentShape, AbsolutePathCompilesAndResolvesTheSiblingHeader) {
    ShapeFixture fx{/*subdirectory=*/true, /*mergedRoute=*/false};
    expectShapeBuildsAndRuns(fx, fx.absolutePrefix(), "absolute");
}

TEST(SourceArgumentShape, BareRelativeNameCompilesAndResolvesTheSiblingHeader) {
    // ★ THE ONE THIS ROW EXISTS FOR. No directory component at all, so the
    // includer's directory is the process working directory and nothing else.
    ShapeFixture fx{/*subdirectory=*/false, /*mergedRoute=*/false};
    expectShapeBuildsAndRuns(fx, "", "bare-relative");
}

TEST(SourceArgumentShape,
     DirectoryRelativePathCompilesAndResolvesTheSiblingHeader) {
    ShapeFixture fx{/*subdirectory=*/true, /*mergedRoute=*/false};
    expectShapeBuildsAndRuns(fx, subdirPrefix(), "directory-relative");
}

TEST(SourceArgumentShape, DotRelativeNameCompilesAndResolvesTheSiblingHeader) {
    ShapeFixture fx{/*subdirectory=*/false, /*mergedRoute=*/false};
    expectShapeBuildsAndRuns(fx, "./", "dot-relative");
}

// ════════════════════════════════════════════════════════════════════════════
// ROUTE: N>1 whole-program merge → `Program::compileUnits`.
// ════════════════════════════════════════════════════════════════════════════
//
// ★ THE SECOND SOURCE IS NOT DECORATION, for the reason
// `test_driver_argument_supply` records: `Program` routes on the CU COUNT, so a
// one-file list never reaches the merged path at all. ✔MEASURED over the shipped
// corpus 2026-08-23, a DATED observation and not a live fact: 17 manifests
// declare `sources` (plural), and every one of them is handed absolute paths too
// — so this route is exactly as blind as the other, and a pin on one says
// nothing about the other.

TEST(SourceArgumentShape, MergedRouteAbsolutePathsResolveTheSiblingHeader) {
    ShapeFixture fx{/*subdirectory=*/true, /*mergedRoute=*/true};
    expectShapeBuildsAndRuns(fx, fx.absolutePrefix(), "absolute (merged)");
}

TEST(SourceArgumentShape, MergedRouteBareRelativeNamesResolveTheSiblingHeader) {
    ShapeFixture fx{/*subdirectory=*/false, /*mergedRoute=*/true};
    expectShapeBuildsAndRuns(fx, "", "bare-relative (merged)");
}

TEST(SourceArgumentShape,
     MergedRouteDirectoryRelativePathsResolveTheSiblingHeader) {
    ShapeFixture fx{/*subdirectory=*/true, /*mergedRoute=*/true};
    expectShapeBuildsAndRuns(fx, subdirPrefix(), "directory-relative (merged)");
}

TEST(SourceArgumentShape, MergedRouteDotRelativeNamesResolveTheSiblingHeader) {
    ShapeFixture fx{/*subdirectory=*/false, /*mergedRoute=*/true};
    expectShapeBuildsAndRuns(fx, "./", "dot-relative (merged)");
}

// ════════════════════════════════════════════════════════════════════════════
// THE FIXTURE'S OWN PREMISE — without this the eight cases above could all pass
// while asserting nothing.
// ════════════════════════════════════════════════════════════════════════════
//
// ★★ THE DECOY MUST REALLY BE A DECOY. Every subdirectory case above claims that
// reading `kDecoyAnswer` would mean "resolved against the working directory".
// That claim is only meaningful if a header of that name really is sitting in
// the working directory and really does carry the other value — a fixture that
// silently stopped planting it would turn the exit-code assertion into a
// tautology, and nothing else in this file would notice.
TEST(SourceArgumentShape, TheFixturePlantsADecoyThatDiffersFromTheSibling) {
    ShapeFixture fx{/*subdirectory=*/true, /*mergedRoute=*/true};
    auto const decoy   = fx.scratch.path() / std::string{kHeaderName};
    auto const sibling = fx.sourceDir / std::string{kHeaderName};
    ASSERT_TRUE(fs::exists(decoy))
        << "no decoy in the working directory: the subdirectory cases' "
           "exit-code assertion would hold whichever header was resolved";
    ASSERT_TRUE(fs::exists(sibling));
    ASSERT_NE(decoy, sibling)
        << "the decoy and the sibling must be two DIFFERENT files";

    EXPECT_EQ(readAllText(decoy), headerText(kDecoyAnswer));
    EXPECT_EQ(readAllText(sibling), headerText(kSiblingAnswer));
    EXPECT_NE(readAllText(decoy), readAllText(sibling))
        << "the two headers carry the same bytes, so resolving either one "
           "produces the same exit code and the subdirectory cases discriminate "
           "nothing";
}

// ★★ AND THE MERGED FIXTURE MUST REALLY BE MERGED. `Program` routes on the CU
// COUNT alone, so a merged case whose fixture quietly stopped emitting the
// second source would take the SOLE-CU path and pass — reporting coverage of a
// route it never entered. This is the same premise `test_driver_argument_supply`
// states in prose for its own two-source helper, asserted here instead.
// ⚠ THE TWO FIXTURES ARE SCOPED APART ON PURPOSE. `ShapeFixture` makes its own
// directory the process working directory, and `ScratchDir(InsideRepo)` roots
// its base at the cwd it finds — so two live at once would NEST, and the outer
// one's destructor would be removing a tree the inner one had just restored a
// cwd into. One at a time keeps each fixture's claim about itself.
TEST(SourceArgumentShape, TheMergedFixtureReallyHandsTheDriverTwoSources) {
    {
        ShapeFixture merged{/*subdirectory=*/false, /*mergedRoute=*/true};
        EXPECT_EQ(merged.sources("").size(), 2u)
            << "the merged fixture must hand the driver TWO sources, or every "
               "`MergedRoute` case above is a second copy of the sole-CU one";
        EXPECT_TRUE(fs::exists(merged.sourceDir / std::string{kHelperName}));
    }
    {
        ShapeFixture sole{/*subdirectory=*/false, /*mergedRoute=*/false};
        EXPECT_EQ(sole.sources("").size(), 1u)
            << "the sole-CU fixture must hand the driver ONE source, or the "
               "four cases that claim the `compileFiles` route are taking the "
               "merged one";
        EXPECT_FALSE(fs::exists(sole.sourceDir / std::string{kHelperName}))
            << "the sole-CU fixture must not leave a second source on disk";
    }
}
