// ★★★ THE THREE CLI MODES ARE THREE WAYS TO GATHER FILES, NOT THREE COMPILERS.
//
// `--compile`, `--directory` and `--project` differ in ONE thing: how the set of
// source files is named. Once that set exists, the translation-unit semantics
// are a property of the SET, never of the mode that produced it. This file pins
// that as an equality over all three, on one fixture, with one expected answer.
//
// ── WHY THIS FILE EXISTS ────────────────────────────────────────────────────
// ✔MEASURED 2026-08-25 (cycle P36): `--directory` violated it. `compileDirectory`
// resolved its file list and then called `compileFiles` UNCONDITIONALLY — the
// unity-build path (many files → ONE CU), which the dispatcher's own comment
// calls "deliberately NOT a CLI surface" on the stated grounds that no
// language's file model concatenates translation units. Every source after the
// first was dropped.
//
// ⚠ AND IT FAILED IN BOTH DIRECTIONS, WHICH IS WHY A LINK-ERROR TEST WOULD NOT
// HAVE BEEN ENOUGH. On a 257-source directory whose dropped functions happened
// to be unreferenced, the driver exited **0**, emitted **zero diagnostics**,
// wrote an artifact, and `nm` found **none** of the 256 functions in it — a
// wrong image reported as success. On a three-source directory whose dropped
// functions WERE referenced, it failed loudly at link with `undefined symbol`.
// Whether the defect is silent or loud depended only on whether the discarded
// code was called. ⇒ this pin asserts the RUNTIME ANSWER, not merely rc == 0.
//
// ★★★ THE PREDICATE WAS ALREADY TESTED. THE CALLERS WERE NOT.
// `routesToMultiUnit` describes itself as "the single source of truth for the
// threshold, shared with `compileProject` so the two dispatch sites never
// drift", and `test_project_config.cpp` pins that predicate directly. There are
// THREE dispatch sites. Testing a shared predicate proves what the predicate
// ANSWERS; it proves nothing about who ASKS it — and the site that never asked
// is exactly the site that drifted. That gap is this file's subject: the claim
// here is about the CALLERS, so it is one case per caller.
//
// ── WHY THE FIXTURE USES A DUPLICATE `static` ───────────────────────────────
// Two files each define `static int g(void)` returning a different value. Under
// correct separate-TU semantics both are legal and distinct, and the program
// answers 12. Under a unity build they collide — so this fixture cannot pass by
// accident on a merged path: it is the C rule itself, not a proxy for it.
//
// ── AND WHY IT IS ONE CASE PER MODE, NOT A LOOP ─────────────────────────────
// The P24 operator ruling: a UNIVERSAL claim is per-example. "Every mode routes
// on the file COUNT" is universal over a three-element set, so each mode is its
// own `TEST` and a red names the mode that broke. A loop reports the first
// failure and hides the rest.

#include "core/types/diagnostic_reporter.hpp"
#include "host_native_target.hpp"  // build for THIS machine — the pin spawns
#include "program/program.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
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

// ga() answers 1 and gb() answers 2, each through its OWN file-local `g`.
// main returns ga()*10 + gb(), so the one correct answer is 12 — and it is
// reachable only if both files became their own translation unit.
constexpr int kExpectedExit = 12;

void writeText(fs::path const& p, std::string_view text) {
    std::ofstream out{p, std::ios::binary};
    out << text;
}

struct ModeFixture {
    ScratchDir scratch{Location::InsideRepo, "mode-routing"};
    fs::path   sourceDir = scratch.path() / "src";
    fs::path   outDir    = scratch.path() / "out";

    ModeFixture() {
        fs::create_directories(sourceDir);
        fs::create_directories(outDir);
        // ⚠ The two `g`s are file-local and DIFFER. In one merged unit they are
        // a redefinition; in two translation units they are ordinary C.
        writeText(sourceDir / "a.c",
                  "static int g(void) { return 1; }\n"
                  "int ga(void) { return g(); }\n");
        writeText(sourceDir / "b.c",
                  "static int g(void) { return 2; }\n"
                  "int gb(void) { return g(); }\n");
        writeText(sourceDir / "main.c",
                  "int ga(void);\n"
                  "int gb(void);\n"
                  "int main(void) { return ga() * 10 + gb(); }\n");
    }

    // Sorted, so the explicit list matches what a directory scan yields and the
    // artifact stem is the same for every mode. A mode that disagreed only
    // because its files arrived in a different ORDER would be a real finding,
    // but it is not the finding this file is about.
    [[nodiscard]] std::vector<std::string> sources() const {
        return {(sourceDir / "a.c").generic_string(),
                (sourceDir / "b.c").generic_string(),
                (sourceDir / "main.c").generic_string()};
    }

    // The artifact is named for the FIRST source's stem — derived, never
    // re-typed, so a change to that rule reddens here instead of silently
    // moving a user's binary.
    //
    // ⚠ THE OUTPUT LAYOUT IS NOT THE SAME ACROSS MODES, AND THAT IS DELIBERATE
    // RATHER THAN THE DRIFT THIS FILE HUNTS. `compileProject` sets the
    // per-format subdirectory flag (see `Program::setPerFormatSubdir`), so a
    // manifest build lands at `<out>/<format>/<stem>` while the two CLI list
    // modes land at `<out>/<stem>`. ✔MEASURED 2026-08-25 when this pin first
    // ran: the project arm HAD built correctly and only the expected PATH was
    // wrong. ⇒ the subdirectory is a parameter, so that a mode moving a user's
    // binary still reddens while a documented layout difference does not.
    [[nodiscard]] fs::path artifact(std::string_view formatSubdir = {}) const {
        return formatSubdir.empty()
                   ? outDir / hostExeArtifact("a")
                   : outDir / formatSubdir / hostExeArtifact("a");
    }
};

// Assert the whole round trip: accepted, no error diagnostics, artifact where
// the naming rule says, and — the load-bearing part — the RIGHT ANSWER.
void expectBuiltAndAnswers(ModeFixture const& fx, int rc, char const* mode,
                           DiagnosticReporter const* rep         = nullptr,
                           std::string_view          formatSubdir = {}) {
    SCOPED_TRACE(std::string{"mode="} + mode);
    auto const artifactPath = fx.artifact(formatSubdir);

    ASSERT_EQ(rc, 0) << "the driver must accept this file set through " << mode
                     << ". A refusal naming an undefined symbol is this mode "
                        "discarding every source after the first";
    if (rep != nullptr) {
        EXPECT_EQ(rep->errorCount(), 0u)
            << "a zero exit alongside error-severity diagnostics is a driver "
               "reporting success over a reported failure";
    }

    ASSERT_TRUE(fs::exists(artifactPath))
        << "no artifact at " << artifactPath.generic_string();

    auto const r = runBinary(artifactPath);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    ASSERT_FALSE(r.timedOut) << r.diagnostic;
    EXPECT_EQ(r.exitCode, kExpectedExit)
        << "the program must answer " << kExpectedExit
        << " (ga()=1 via a.c's own `g`, gb()=2 via b.c's own `g`). A different "
           "answer means the two file-local `g` definitions were merged into "
           "one translation unit, which is not C — and it is a build that "
           "succeeds, runs, and answers wrong with no diagnostic anywhere";
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
// One case per DISPATCH SITE. Each names the mode it speaks for.
// ════════════════════════════════════════════════════════════════════════════

TEST(ModeRoutingAgreement, ExplicitFileListCompilesEachSourceAsItsOwnUnit) {
    ModeFixture fx;
    Program     prog;
    prog.setOutputDir(fx.outDir);
    DiagnosticReporter rep;
    int const rc = prog.compileUnits(fx.sources(), "c",
                                     {std::string{hostNativeTarget().execTarget}},
                                     rep);
    expectBuiltAndAnswers(fx, rc, "--compile (compileUnits)", &rep);
}

// ★ THE REGRESSION PIN. This is the case that was red: `compileDirectory` called
// `compileFiles` unconditionally instead of routing on the file count.
TEST(ModeRoutingAgreement, DirectoryScanCompilesEachSourceAsItsOwnUnit) {
    ModeFixture fx;
    Program     prog;
    prog.setOutputDir(fx.outDir);
    int const rc = prog.compileDirectory(
        fx.sourceDir.generic_string(), "c",
        {std::string{hostNativeTarget().execTarget}});
    expectBuiltAndAnswers(fx, rc, "--directory (compileDirectory)");
}

// The third dispatch site. `compileProject` already routed on the count, and
// this pin is what keeps that true: the predicate's own comment counted TWO
// callers when there were three, and the uncounted one is the one that broke.
TEST(ModeRoutingAgreement, ProjectManifestCompilesEachSourceAsItsOwnUnit) {
    ModeFixture fx;
    // Sources are spelled ABSOLUTE deliberately: a manifest's relative entries
    // resolve against the PROCESS working directory, and this pin is about
    // routing, not about path resolution -- which has its own file.
    std::string json = R"({"language": "c", "artifactProfile": "cli", )"
                       R"("targets": [")"
                     + std::string{hostNativeTarget().execTarget}
                     + R"("], "sources": [)";
    auto const srcs = fx.sources();
    for (std::size_t i = 0; i < srcs.size(); ++i) {
        json += (i ? ", \"" : "\"") + srcs[i] + "\"";
    }
    json += "]}";

    fs::path const manifest = fx.scratch.path() / "p.dss-project.json";
    writeText(manifest, json);

    Program prog;
    prog.setOutputDir(fx.outDir);
    DiagnosticReporter rep;
    int const rc = prog.compileProject(manifest.generic_string(), rep);
    // The format name, taken from the target spec rather than re-typed.
    std::string_view const spec{hostNativeTarget().execTarget};
    auto const colon = spec.find(':');
    ASSERT_NE(colon, std::string_view::npos) << "target spec must be <arch>:<format>";
    expectBuiltAndAnswers(fx, rc, "--project (compileProject)", &rep,
                          spec.substr(colon + 1));
}

// ⚠ A SET OF SIZE ONE MUST STILL TAKE THE SINGLE-CU ROUTE — the threshold is
// `> 1`, and a fix that routed EVERYTHING to `compileUnits` would pass the two
// cases above while changing the 38 single-source corpus examples underneath.
// A guard that cannot fail in the other direction is half a guard.
TEST(ModeRoutingAgreement, ADirectoryHoldingOneSourceStillTakesTheSingleCuRoute) {
    ScratchDir scratch{Location::InsideRepo, "mode-routing"};
    fs::path const dir = scratch.path() / "solo";
    fs::path const out = scratch.path() / "out";
    fs::create_directories(dir);
    fs::create_directories(out);
    writeText(dir / "main.c", "int main(void) { return 7; }\n");

    Program prog;
    prog.setOutputDir(out);
    int const rc = prog.compileDirectory(
        dir.generic_string(), "c",
        {std::string{hostNativeTarget().execTarget}});
    ASSERT_EQ(rc, 0);

    auto const exe = out / hostExeArtifact("main");
    ASSERT_TRUE(fs::exists(exe)) << exe.generic_string();
    auto const r = runBinary(exe);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_EQ(r.exitCode, 7);
}
