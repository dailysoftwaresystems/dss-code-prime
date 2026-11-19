// THE ARTIFACT INVARIANT: A COMPILATION THAT RECORDED AN ERROR COMMITS NO
// ARTIFACT.
//
// ★★★ THE DEFECT, IN ONE SENTENCE: the tool emitted the loudest failure signal
// it has — rc=1, an error-severity diagnostic — and in the same breath printed
// `dsscp: artifact …` and left a runnable executable on disk. ✔MEASURED on the
// shipped CLI before the fix, with a FRESH `--output` directory each time, so
// it was never a leftover:
//
//     dsscp --compile a.c b.c --language c --target x86_64:pe64-…-exec
//     dsscp: artifact …/a.exe
//     error[K_SymbolRedefinedAcrossUnits]: symbol 'f' has multiple strong
//         (global) definitions across compilation units (CU #1 and CU #2).
//     rc=1                       … and a.exe ran, returning CU#1's answer.
//
// That is the fail-loud bar inverted, and by this project's definition it is a
// SILENT MISCOMPILE: two conflicting definitions, one elected without a
// decision anyone made, shipped in a file any build script keying on "did the
// binary appear" will happily consume.
//
// ★★ WHAT THIS FILE PINS, AND WHY IT IS TWO SEPARATE GATES RATHER THAN ONE.
// The repair is two invariants at two scopes, and each case below fails when
// its own gate is removed:
//
//   1. THE MERGE TIER'S SNAPSHOT GATE (`program.cpp`, at the `mergeCuMirs`
//      call site). `mergeCuMirs` reports each cross-unit strong redefinition
//      and then returns a well-formed module in which one definition silently
//      won. Every OTHER tier in the pipeline checkpoints `errorCount()`
//      against its entry snapshot; the merge was the one that did not, so the
//      pipeline optimized, lowered, assembled and linked a module already
//      known to be wrong. `NoArtifactIsWrittenForACrossUnitRedefinition`
//      asserts the run stops there — measured by the ABSENCE of the writer's
//      own notice, which is the only observable that separates "the merge
//      stopped it" from "the writer caught it downstream".
//
//   2. THE WRITER'S ARTIFACT GATE (`link/writer.cpp::writeBytes`). The one
//      place any artifact byte in this compiler reaches disk, now refusing
//      when the compilation has recorded an error. It is the net for tier 1
//      and for every future tier that forgets a snapshot. Its own direct pin
//      is `link/test_artifact_withheld_after_error`; here it is the reason
//      the ABSENT-notice assertion is falsifiable at all.
//
// ⚠ THE CONTROLS ARE NOT DECORATION. `AcleanCrossUnitBuildStillWritesIts
// Artifact` is what stops the invariant from being satisfied by a compiler
// that writes nothing ever — the failure mode a "no file exists" assertion
// cannot see on its own. It uses the SAME two-file shape, the SAME target and
// the SAME output directory mechanics as the failing case, differing only in
// whether the second unit redefines the symbol.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

// CU #1 — defines `f` and calls it. Identical in both the failing and the
// clean case except that the clean case's sibling does not redefine `f`.
constexpr char const* kMainDefinesF =
    "int f(void) { return 42; }\n"
    "int main(void) { return f(); }\n";

// CU #2, FAILING: a second STRONG definition of `f`. gcc 13.3.0, clang 18.1.3
// and MSVC 19.51 each refuse this program (measured separately) and each
// produces no output file for it.
constexpr char const* kSiblingRedefinesF = "int f(void) { return 7; }\n";

// CU #2, CONTROL: a different symbol, so the merge has nothing to elect.
constexpr char const* kSiblingCleanly = "int g(void) { return 7; }\n";

void writeFile(fs::path const& p, char const* text) {
    std::ofstream out{p, std::ios::binary};
    ASSERT_TRUE(out.good()) << "could not write " << p.string();
    out << text;
}

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& rep,
                                    DiagnosticCode            code) {
    std::size_t n = 0;
    for (auto const& d : rep.all()) {
        if (d.code == code) ++n;
    }
    return n;
}

// Every regular file under `dir`, recursively — the artifact, a staging temp
// the writer failed to clean up, anything at all. The invariant is about what
// the build LEAVES, so the census must not be narrowed to one expected name.
[[nodiscard]] std::vector<std::string> filesUnder(fs::path const& dir) {
    std::vector<std::string> out;
    std::error_code          ec;
    if (!fs::exists(dir, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         !ec && it != fs::recursive_directory_iterator{}; it.increment(ec)) {
        if (it->is_regular_file(ec)) out.push_back(it->path().generic_string());
    }
    return out;
}

[[nodiscard]] std::string join(std::vector<std::string> const& v) {
    std::string s;
    for (auto const& e : v) {
        if (!s.empty()) s += ", ";
        s += e;
    }
    return s.empty() ? std::string{"<none>"} : s;
}

// `compileUnits`, NEVER `compileFiles`: the latter folds every named file into
// ONE multi-file compilation unit, in which "two units define `f`" cannot be
// said at all. `compileUnits` is the CU6 model the CLI's `--compile a.c b.c`
// and every project build take, and the only one in which the whole-program
// merge runs.
[[nodiscard]] int compileTwoUnits(fs::path const&     first,
                                  fs::path const&     second,
                                  fs::path const&     outDir,
                                  DiagnosticReporter& rep) {
    Program prog;
    prog.setOutputDir(outDir);
    return prog.compileUnits({first.generic_string(), second.generic_string()},
                             "c", {"x86_64:elf64-x86_64-linux-exec"}, rep);
}

}  // namespace

// ── THE CONTROL ─────────────────────────────────────────────────────────────
//
// FIRST in the file, deliberately: if a clean two-unit build stops producing an
// artifact, the invariant below is satisfied vacuously and this is the case
// that says so.
TEST(FailedBuildWritesNoArtifact, ACleanCrossUnitBuildStillWritesItsArtifact) {
    ScratchDir scratch{Location::InsideRepo, "noartifact-ctl"};
    fs::path const a   = scratch.path() / "a.c";
    fs::path const b   = scratch.path() / "b.c";
    fs::path const out = scratch.path() / "out";
    writeFile(a, kMainDefinesF);
    writeFile(b, kSiblingCleanly);
    scratch.useAsCwd();

    DiagnosticReporter rep{DiagnosticReporter::Config{}};
    EXPECT_EQ(compileTwoUnits(a, b, out, rep), 0)
        << "the control build must SUCCEED — an invariant about what a FAILED "
           "build leaves behind is worthless if every build now fails";
    EXPECT_EQ(rep.errorCount(), 0u);

    auto const files = filesUnder(out);
    EXPECT_FALSE(files.empty())
        << "a successful cross-unit build wrote NO artifact — the withholding "
           "gate is firing on a clean compilation, which would make the "
           "failing-case assertion below pass for entirely the wrong reason";
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_ArtifactWithheldAfterError), 0u)
        << "the writer withheld an artifact from a build that reported no "
           "error at all";
}

// ── THE DEFECT'S CLOSING PIN ────────────────────────────────────────────────
TEST(FailedBuildWritesNoArtifact, NoArtifactIsWrittenForACrossUnitRedefinition) {
    ScratchDir scratch{Location::InsideRepo, "noartifact"};
    fs::path const a   = scratch.path() / "a.c";
    fs::path const b   = scratch.path() / "b.c";
    fs::path const out = scratch.path() / "out";
    writeFile(a, kMainDefinesF);
    writeFile(b, kSiblingRedefinesF);
    scratch.useAsCwd();

    DiagnosticReporter rep{DiagnosticReporter::Config{}};
    EXPECT_NE(compileTwoUnits(a, b, out, rep), 0)
        << "two strong definitions of `f` across units must FAIL the build";
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolRedefinedAcrossUnits), 1u)
        << "the input stopped producing the diagnostic this case is built "
           "around, so the assertions below no longer measure anything";

    // THE INVARIANT.
    auto const files = filesUnder(out);
    EXPECT_TRUE(files.empty())
        << "A BUILD THAT REPORTED FAILURE LEFT AN ARTIFACT: " << join(files)
        << ". rc was non-zero and an error was reported, and the file the "
           "failure denies is on disk anyway — a build script keying on the "
           "binary's existence gets a program with one of two conflicting "
           "definitions silently elected.";

    // WHICH GATE STOPPED IT. The writer's notice fires only when the byte
    // commit is REACHED with errors already recorded — i.e. only when some
    // tier reported and let the pipeline run on. Its absence is the assertion
    // that the merge tier's own snapshot gate did the stopping, and it is what
    // reddens if that gate is removed while the writer's net stays.
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_ArtifactWithheldAfterError), 0u)
        << "the run reached the artifact writer with an error already "
           "recorded, so the merge tier reported a cross-unit redefinition "
           "and then let the optimizer, LIR, assembler and linker all run "
           "over a module it already knew was wrong. The artifact was still "
           "withheld (the writer's net held), but the work was wasted and "
           "every diagnostic those tiers produced describes a program the "
           "source does not name.";
}

// ── AN ERROR THE VOLUME GATES ELIDE STILL FAILS THE BUILD, UNDER ITS OWN NAME ──
//
// (P69 fold f26e, lane `cs`, D9(c).) Every tier decides "did this stage fail" from
// `DiagnosticReporter::errorCount()`, and the artifact writer refuses on the same count. The volume gates (the
// per-code cap, the dedup window, the global cap) elide a diagnostic into a marker, and `--max-per-code 0` is legal
// ("show none, count them"), so under it the FIRST Error of every code outside the unsuppressable table is elided.
// ✔MEASURED before the fix (six one-Error sources, the five below and `int *p = 5;`, at the defaults and under
// `maxPerCode = 0`, x86_64:elf64-x86_64-linux-exec, MinGW gcc 13.2 build): the assignment to a `const` object BUILT
// GREEN -- rc 0, errorCount() 0, an artifact on disk -- and the other four failed only because a LATER tier reported
// its own Error over the model the elided one had left broken: the undeclared identifier as H_TypeUnresolved, the
// argument count as H_VerifierFailure, the missing member and the syntax error as H_UnsupportedLoweringForKind. The
// reporter now counts what its gates drop and every relay carries the count, so the tier whose Error was elided
// stops the build itself.
// Each source runs TWICE: at the defaults (the control: its own codes are STORED, so the input reaches the tier this
// case is about) and under `maxPerCode = 0` (its own codes are stored nowhere and still COUNTED). In both: rc 1, no
// artifact, and no code from any tier after the source's own.
namespace {

struct OneErrorSource {
    char const*                 tag;
    char const*                 text;
    std::vector<DiagnosticCode> own;   // what the defaults report for it, every one an Error
};

[[nodiscard]] std::vector<OneErrorSource> const& oneErrorSources() {
    static std::vector<OneErrorSource> const sources = {
        {"undeclared", "int main(void) { return undeclared; }\n", {DiagnosticCode::S_UndeclaredIdentifier}},
        {"argcount", "int f(int a) { return a; }\nint main(void) { return f(1, 2); }\n",
         {DiagnosticCode::S_ArgCountMismatch}},
        {"constassign", "int main(void) { const int c = 1; c = 2; return c; }\n", {DiagnosticCode::S_ConstViolation}},
        {"nomember", "struct S { int a; };\nint main(void) { struct S s = {1}; return s.b; }\n",
         {DiagnosticCode::S_UndeclaredIdentifier}},
        {"syntax", "int main(void) { return 1 }\n",
         {DiagnosticCode::P_UnexpectedToken, DiagnosticCode::P_MissingRequiredChild}},
    };
    return sources;
}

// Every code the run met -- stored, or counted by a gate that dropped it.
[[nodiscard]] std::set<DiagnosticCode> codesOf(DiagnosticReporter const& rep) {
    std::set<DiagnosticCode> out;
    for (auto const& d : rep.all()) out.insert(d.code);
    for (DiagnosticCode const c : rep.droppedCodes()) out.insert(c);
    return out;
}

[[nodiscard]] std::string namesOf(std::set<DiagnosticCode> const& codes) {
    std::string s;
    for (DiagnosticCode const c : codes) {
        if (!s.empty()) s += ", ";
        s += diagnosticCodeName(c);
    }
    return s.empty() ? std::string{"<none>"} : s;
}

}  // namespace

TEST(FailedBuildWritesNoArtifact, AnErrorTheVolumeGatesElideStillFailsTheBuild) {
    for (OneErrorSource const& src : oneErrorSources()) {
        for (bool const elideEvery : {false, true}) {
            SCOPED_TRACE(std::string{src.tag} + (elideEvery ? " under maxPerCode = 0" : " at the defaults"));
            ScratchDir scratch{Location::InsideRepo,
                               std::string{"noartifact-elided-"} + src.tag + (elideEvery ? "-0" : "-d")};
            fs::path const a   = scratch.path() / "a.c";
            fs::path const out = scratch.path() / "out";
            writeFile(a, src.text);
            scratch.useAsCwd();

            DiagnosticReporter::Config cfg{};      // the shipped defaults…
            if (elideEvery) cfg.maxPerCode = 0;    // …or `--max-per-code 0`
            DiagnosticReporter rep{cfg};
            Program            prog;
            prog.setOutputDir(out);
            int const rc = prog.compileUnits({a.generic_string()}, "c", {"x86_64:elf64-x86_64-linux-exec"}, rep);
            std::set<DiagnosticCode> const codes = codesOf(rep);

            EXPECT_EQ(rc, 1) << "codes met: " << namesOf(codes);
            auto const files = filesUnder(out);
            EXPECT_TRUE(files.empty()) << "a build of a source with an Error wrote " << join(files);
            for (DiagnosticCode const own : src.own) {
                if (!elideEvery) {
                    EXPECT_GE(countCode(rep, own), 1u)
                        << "the defaults no longer report " << diagnosticCodeName(own)
                        << ", so the elided half below measures nothing; codes met: " << namesOf(codes);
                } else {
                    EXPECT_EQ(countCode(rep, own), 0u)
                        << diagnosticCodeName(own) << " was STORED under maxPerCode = 0: the input no longer "
                        << "reaches the per-code gate";
                    EXPECT_GE(rep.droppedFor(own).errors, 1u)
                        << "the source's own Error " << diagnosticCodeName(own)
                        << " was elided and not counted; codes met: " << namesOf(codes);
                }
            }
            for (DiagnosticCode const c : codes) {
                bool const said = std::ranges::find(src.own, c) != src.own.end()
                               || c == DiagnosticCode::P_DiagnosticsElided
                               || diagnosticCodeName(c).starts_with("D_");
                EXPECT_TRUE(said) << diagnosticCodeName(c) << " is from a tier after the source's own: the run "
                                  << "went on past the Error that should have stopped it; codes met: "
                                  << namesOf(codes);
            }
        }
    }
}

// ── THE NON-DESTRUCTIVE CLAUSE ──────────────────────────────────────────────
//
// ★★ THE OTHER HALF OF THE DECISION, AND IT IS A DECISION RATHER THAN AN
// OMISSION. A failing rebuild over a previously GOOD artifact must leave that
// artifact BYTE-IDENTICAL. ✔MEASURED separately per reference: gcc 13.3.0,
// clang 18.1.3 and MSVC 19.51 all LEAVE the previous output byte-identical
// when the FRONT END fails, and all three UNLINK it when `ld`/`link` fails.
// There is therefore no unanimous reference answer keyed on anything but WHICH
// STAGE failed, and DSS takes the non-destructive half everywhere: this
// compiler never destroys an artifact it did not write, it only ever declines
// to write a new one. The alternative — deleting on a failure detected before
// the output file was ever opened — destroys a working user binary and cannot
// give it back.
TEST(FailedBuildWritesNoArtifact, AFailingRebuildLeavesThePreviousArtifactIntact) {
    ScratchDir scratch{Location::InsideRepo, "noartifact-stale"};
    fs::path const a   = scratch.path() / "a.c";
    fs::path const bok = scratch.path() / "b.c";
    fs::path const out = scratch.path() / "out";
    writeFile(a, kMainDefinesF);
    writeFile(bok, kSiblingCleanly);
    scratch.useAsCwd();

    {
        DiagnosticReporter rep{DiagnosticReporter::Config{}};
        ASSERT_EQ(compileTwoUnits(a, bok, out, rep), 0)
            << "the seeding build must succeed for this case to mean anything";
    }
    auto const before = filesUnder(out);
    ASSERT_FALSE(before.empty()) << "nothing to preserve — no artifact seeded";
    std::vector<std::string> beforeBytes;
    for (auto const& p : before) {
        std::ifstream in{p, std::ios::binary};
        beforeBytes.emplace_back((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
        ASSERT_FALSE(beforeBytes.back().empty()) << "seeded artifact is empty";
    }

    // Now break it, into the SAME output directory.
    writeFile(bok, kSiblingRedefinesF);
    {
        DiagnosticReporter rep{DiagnosticReporter::Config{}};
        EXPECT_NE(compileTwoUnits(a, bok, out, rep), 0);
    }

    auto const after = filesUnder(out);
    EXPECT_EQ(after, before)
        << "the failing rebuild changed which files exist: was [" << join(before)
        << "], now [" << join(after)
        << "]. It must neither add one (the invariant above) nor remove one "
           "(a compiler that deletes a working binary on a build it aborted "
           "before opening the file destroys something it cannot give back).";
    for (std::size_t i = 0; i < after.size() && i < beforeBytes.size(); ++i) {
        std::ifstream in{after[i], std::ios::binary};
        std::string const nowBytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
        EXPECT_EQ(nowBytes, beforeBytes[i])
            << "the failing rebuild REWROTE " << after[i]
            << " — the previous good artifact must survive byte-identical";
    }
}
