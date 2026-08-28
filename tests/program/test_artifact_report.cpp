// D-HARNESS-FIXTURE-PATH-ASSUMES-THE-POSIX-ARTIFACT-SPELLING (TF-C118) —
// THE BUILD'S STATEMENT OF RECORD ABOUT WHAT IT PRODUCED.
//
// THE DEFECT THIS CLOSES, MEASURED. On a WSL x86_64 host the sqlite harness
// cross-built the WINDOWS testfixture for pe64: 189 TUs compiled, the link ran,
// ZERO `error[` diagnostics, and `…/pe64-x86_64-windows-exec/testfixture.exe`
// appeared on disk (`file(1)`: PE32+ executable, 5,387,264 bytes). The driver
// reported `build FAILED — 0 error[ but no executable at …/testfixture` and
// marked the leg POISONED. It was looking for a suffix-less name, because
// nothing in the build had ever TOLD it what the artifact was called — so the
// driver had to reconstruct the name, and to reconstruct it, it needed a copy
// of DSS's artifact-extension table. A false negative on the project's headline
// capability (`D-HARNESS-CROSS-HOST-ANY-TARGET`), produced by the instrument.
//
// WHAT IS PINNED HERE, and why each pin is the one that would have caught it:
//
//   1. THE SUFFIX ACTUALLY DIFFERS ACROSS TARGETS, and the report tracks it.
//      A pe target's artifact is `<name>.exe`; an ELF exec's is `<name>` with
//      no suffix at all. That disagreement is the entire root cause, so both
//      arms are asserted in the same table — a report that hardcoded either
//      spelling reds on the other.
//   2. THE REPORTED PATH IS THE FILE ON DISK. Not "a plausible path": the test
//      stats exactly what the compiler printed. A report that drifted from the
//      writer would pass a string-shape assertion and fail this one.
//   3. IT IS ABSOLUTE even when `--output` was relative (the CLI stores that
//      argument verbatim), so a consumer never has to guess a cwd.
//   4. ONE LINE PER ARTIFACT, TAGGED BY TARGET, so a multi-target build is
//      unambiguous.
//   5. A BUILD THAT WROTE NOTHING SAYS NOTHING — asserted at BOTH failure
//      distances: a front-end failure that never reaches the writer, and a
//      LINK failure that does reach it. Without (5) the line would be an
//      announcement of intent rather than a record of fact, and the harness
//      would trade its old false negative for a new false positive.
//
// RED-ON-DISABLE, both directions, MEASURED (verbatim messages in the cycle
// report):
//   · neutralise the report — make `compileOneTarget`'s `reported` lambda a
//     pass-through — and the three positive tests fail with 0 artifact lines
//     where 1 (or 2) were required, while both negative tests stay green;
//   · move the report out from behind its `if (wrote)` guard and 5b fails,
//     quoting the announced-but-never-written `…/hello.exe` beside the
//     `K_InvalidStackReserveRequest` that refused it.
// 5a does NOT move under either disable, and that is why 5b exists: a
// front-end failure returns before the writer is ever reached, so only 5b
// actually exercises the guard.

#include "core/types/diagnostic_reporter.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"
#include "unc_spelling.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::test_support;
namespace fs = std::filesystem;

namespace {

// ★ THE MARKER IS SPELLED OUT HERE, DELIBERATELY, rather than imported from the
// emitter. It is a WIRE FORMAT: two shipped shell drivers parse it, and a
// constant shared with the producer would let the producer redefine the
// protocol and stay green. `harness/test_sqlite_harness_legs` §10 holds the
// other end — it requires this exact spelling to appear in BOTH drivers AND in
// `src/program/program.cpp`, so changing it in one place reds there.
constexpr std::string_view kMarker = "dsscp: artifact ";

struct ReportedArtifact {
    std::string spec;
    std::string path;
};

// Every artifact line in a captured stderr, in emission order.
//
// The parse is the one a consumer must be able to perform: fixed prefix, then
// ONE whitespace-free token (the target spec — `TargetSpec::parse` refuses
// whitespace in either half, which is what makes this safe), then the path as
// the whole REMAINDER of the line. Splitting on the last space instead would
// break the moment an output directory contains one.
[[nodiscard]] std::vector<ReportedArtifact> artifactLines(std::string const& text) {
    std::vector<ReportedArtifact> out;
    std::istringstream in{text};
    std::string        line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind(kMarker, 0) != 0) continue;
        auto const rest  = line.substr(kMarker.size());
        auto const space = rest.find(' ');
        if (space == std::string::npos) {
            ADD_FAILURE() << "artifact line carries no path: " << line;
            continue;
        }
        out.push_back({rest.substr(0, space), rest.substr(space + 1)});
    }
    return out;
}

// Swap `std::cerr`'s streambuf for the duration of a compile. The report and
// the diagnostic drain are the only things the driver writes there, so the
// capture is exact and the artifact-line COUNT is meaningful.
class CerrCapture {
public:
    CerrCapture() : old_{std::cerr.rdbuf(buf_.rdbuf())} {}
    CerrCapture(CerrCapture const&)            = delete;
    CerrCapture& operator=(CerrCapture const&) = delete;
    ~CerrCapture() { std::cerr.rdbuf(old_); }
    [[nodiscard]] std::string text() const { return buf_.str(); }

private:
    std::ostringstream buf_;
    std::streambuf*    old_;
};

[[nodiscard]] fs::path writeSrc(fs::path const& dir, std::string_view name,
                                std::string_view text) {
    auto const p = dir / std::string{name};
    std::ofstream f(p);
    f << text;
    return p;
}

// A program with a `main`, so every target below is a real EXEC link rather
// than a relocatable object drop.
constexpr std::string_view kMainSrc = "int main(void) { return 0; }\n";

}  // namespace

// ── 1 + 2. The suffix differs per target, and the report tracks the writer ──
//
// The two rows ARE the defect: `.exe` vs no suffix at all is exactly the
// disagreement that made a successful cross-build read as a failure.
TEST(ArtifactReport, ReportsTheExactFileItWroteForEveryTargetSuffix) {
    struct Case {
        char const* spec;
        char const* expectedFileName;  // the WHOLE name, suffix included
    };
    constexpr Case kCases[] = {
        {"x86_64:pe64-x86_64-windows-exec", "hello.exe"},
        {"x86_64:elf64-x86_64-linux-exec", "hello"},
    };

    for (auto const& c : kCases) {
        ScratchDir scratch{Location::InsideRepo, "artifact-report"};
        auto const src = writeSrc(scratch.path(), "hello.c", kMainSrc);
        scratch.useAsCwd();
        auto const outDir = scratch.path() / "out";

        Program prog;
        prog.setOutputDir(outDir);
        DiagnosticReporter rep;
        std::string        captured;
        int                rc = 1;
        {
            CerrCapture cap;
            rc       = prog.compileFiles({src.generic_string()}, "c",
                                         {c.spec}, rep);
            captured = cap.text();
        }
        ASSERT_EQ(rc, 0) << c.spec << " must build; stderr:\n" << captured;

        auto const lines = artifactLines(captured);
        ASSERT_EQ(lines.size(), 1u)
            << "a single-target build must report EXACTLY ONE artifact for "
            << c.spec << "; stderr:\n"
            << captured;
        EXPECT_EQ(lines[0].spec, c.spec)
            << "the report must carry the target spec the caller asked for, or "
               "a multi-target build cannot be disambiguated";

        // The suffix, stated. This row is what a driver used to have to know.
        auto const expected = (outDir / c.expectedFileName).generic_string();
        EXPECT_EQ(lines[0].path, expected)
            << "reported path disagrees with the artifact-extension table for "
            << c.spec;

        // …and the file is really there, at the path that was PRINTED. A
        // report that drifted from the writer passes the string check above
        // and dies here.
        EXPECT_TRUE(fs::exists(fs::path{lines[0].path}))
            << "the compiler reported an artifact at '" << lines[0].path
            << "' but nothing is there — the report must name the file that "
               "was written, never a file that was merely intended";
    }
}

// ── 3. Absolute even when `--output` was not ────────────────────────────────
//
// `CliArgs` stores `--output` verbatim, so `outPath` is relative whenever the
// caller's argument was. A consumer of the report has no way to know which
// directory the compiler was run from, so resolving it is the compiler's job.
TEST(ArtifactReport, TheReportedPathIsAbsoluteEvenWhenOutputWasRelative) {
    ScratchDir scratch{Location::InsideRepo, "artifact-report"};
    auto const src = writeSrc(scratch.path(), "hello.c", kMainSrc);
    scratch.useAsCwd();

    Program prog;
    prog.setOutputDir(fs::path{"out"});  // RELATIVE, on purpose
    DiagnosticReporter rep;
    std::string        captured;
    int                rc = 1;
    {
        CerrCapture cap;
        rc = prog.compileFiles({src.generic_string()}, "c",
                               {"x86_64:pe64-x86_64-windows-exec"}, rep);
        captured = cap.text();
    }
    ASSERT_EQ(rc, 0) << "stderr:\n" << captured;

    auto const lines = artifactLines(captured);
    ASSERT_EQ(lines.size(), 1u) << "stderr:\n" << captured;
    EXPECT_TRUE(fs::path{lines[0].path}.is_absolute())
        << "reported '" << lines[0].path
        << "' — a relative path makes the report unusable to anyone who does "
           "not already know the compiler's cwd";
    EXPECT_EQ(lines[0].path,
              (scratch.path() / "out" / "hello.exe").generic_string());
    EXPECT_TRUE(fs::exists(fs::path{lines[0].path}));
}

// ── 4. One line per artifact, tagged by target ──────────────────────────────
TEST(ArtifactReport, AMultiTargetBuildReportsEveryArtifactTaggedByItsTarget) {
    ScratchDir scratch{Location::InsideRepo, "artifact-report"};
    auto const src = writeSrc(scratch.path(), "hello.c", kMainSrc);
    scratch.useAsCwd();
    auto const outDir = scratch.path() / "out";

    Program prog;
    prog.setOutputDir(outDir);
    DiagnosticReporter rep;
    std::string        captured;
    int                rc = 1;
    {
        CerrCapture cap;
        rc = prog.compileFiles({src.generic_string()}, "c",
                               {"x86_64:pe64-x86_64-windows-exec",
                                "x86_64:elf64-x86_64-linux-exec"},
                               rep);
        captured = cap.text();
    }
    ASSERT_EQ(rc, 0) << "stderr:\n" << captured;

    auto const lines = artifactLines(captured);
    ASSERT_EQ(lines.size(), 2u)
        << "two targets, two artifacts, two lines; stderr:\n"
        << captured;
    // Multi-target routes each artifact into its own `<formatName>/` subdir —
    // and the report is what tells a consumer WHICH line is which.
    EXPECT_EQ(lines[0].spec, "x86_64:pe64-x86_64-windows-exec");
    EXPECT_EQ(lines[0].path,
              (outDir / "pe64-x86_64-windows-exec" / "hello.exe").generic_string());
    EXPECT_EQ(lines[1].spec, "x86_64:elf64-x86_64-linux-exec");
    EXPECT_EQ(lines[1].path,
              (outDir / "elf64-x86_64-linux-exec" / "hello").generic_string());
    for (auto const& l : lines) EXPECT_TRUE(fs::exists(fs::path{l.path}));
}

// ── 5a. A front-end failure announces no artifact ───────────────────────────
//
// The build never reaches the writer. Nothing may claim a file.
TEST(ArtifactReport, AFrontEndFailureReportsNoArtifact) {
    ScratchDir scratch{Location::InsideRepo, "artifact-report"};
    auto const src = writeSrc(scratch.path(), "broken.c",
                              "int main(void) { return @@@; }\n");
    scratch.useAsCwd();
    auto const outDir = scratch.path() / "out";

    Program prog;
    prog.setOutputDir(outDir);
    DiagnosticReporter rep;
    std::string        captured;
    int                rc = 0;
    {
        CerrCapture cap;
        rc = prog.compileFiles({src.generic_string()}, "c",
                               {"x86_64:pe64-x86_64-windows-exec"}, rep);
        captured = cap.text();
    }
    ASSERT_NE(rc, 0) << "the source is ill-formed; stderr:\n" << captured;
    EXPECT_TRUE(artifactLines(captured).empty())
        << "a failed build claimed an artifact; stderr:\n" << captured;
}

// ── 5b. A LINK failure announces no artifact ────────────────────────────────
//
// The other failure distance, and the one that actually exercises the guard:
// the output path has been computed, the CUs have compiled, and the LINK
// refuses. An out-of-range `--stack-reserve` is the cheapest deterministic way
// to fail exactly there (`K_InvalidStackReserveRequest` — the format declares
// its own minimum/granularity, so 1 byte is refused rather than rounded).
TEST(ArtifactReport, ALinkFailureAfterThePathIsKnownReportsNoArtifact) {
    ScratchDir scratch{Location::InsideRepo, "artifact-report"};
    auto const src = writeSrc(scratch.path(), "hello.c", kMainSrc);
    scratch.useAsCwd();
    auto const outDir = scratch.path() / "out";

    Program prog;
    prog.setOutputDir(outDir);
    prog.setStackReserveBytes(1u);  // below every declared minimum
    DiagnosticReporter rep;
    std::string        captured;
    int                rc = 0;
    {
        CerrCapture cap;
        rc = prog.compileFiles({src.generic_string()}, "c",
                               {"x86_64:pe64-x86_64-windows-exec"}, rep);
        captured = cap.text();
    }
    ASSERT_NE(rc, 0) << "the link must refuse the request; stderr:\n" << captured;
    EXPECT_TRUE(artifactLines(captured).empty())
        << "the link failed but an artifact was announced; stderr:\n"
        << captured;
    EXPECT_FALSE(fs::exists(outDir / "hello.exe"))
        << "sanity: the refused link must not have written anything";
}

// ── 6. A UNC `--output` is reported where it was actually written ────────────
//
// ★★★ [[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]] — THE REPORT TIER. This
// line existed to tell a user WHERE the artifact is, and on a UNC output it named
// somewhere else. `reportArtifactWritten` ran the path through a bare
// `fs::absolute`, which on a path model that gives a UNC authority no
// `root_name()` does not fail but SUCCEEDS having re-rooted it onto the local
// drive: an artifact written to `\host\share\out\hello.exe` was announced as
// `C:\host\share\out\hello.exe`. Nothing downstream checks a diagnostic, so the
// only symptom was a user following the line to a file that is not there.
//
// ⚠ THIS ARM DELIBERATELY DOES NOT ASSERT `is_absolute()`, WHICH ITS SIBLING
// ABOVE DOES. On the toolchain that builds DSS a UNC path answers `is_absolute()`
// FALSE — so importing that assertion would fail on a CORRECT report, and
// "fixing" it by re-rooting is precisely the defect. What the report owes is that
// the path still names the machine it was written to, and that the file is there.
TEST(ArtifactReport, AUncOutputIsReportedWhereItWasWritten) {
    ScratchDir scratch{Location::InsideRepo, "artifact-report-unc"};
    auto const src = writeSrc(scratch.path(), "hello.c", kMainSrc);

    fs::path const uncOut = dss::test_support::uncSpellingOf(scratch.path());
    if (uncOut.empty())
        GTEST_SKIP()
            << "D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED: this host offers "
               "no reachable UNC spelling of '"
            << scratch.path().string()
            << "', so the artifact REPORT's multi-separator-root arm WAS NOT "
               "MEASURED on this leg. This is an unmeasured property, NOT a "
               "passing one.";
    ASSERT_GE(dss::test_support::leadingSeparatorRun(uncOut), 2u)
        << "the fixture stopped producing a multi-separator root, so this test "
           "would pass without exercising the property: " << uncOut.string();

    Program prog;
    prog.setOutputDir(uncOut / "out");
    DiagnosticReporter rep;
    std::string        captured;
    int                rc = 1;
    {
        CerrCapture cap;
        rc = prog.compileFiles({src.generic_string()}, "c",
                               {"x86_64:pe64-x86_64-windows-exec"}, rep);
        captured = cap.text();
    }
    ASSERT_EQ(rc, 0) << "stderr:\n" << captured;

    auto const lines = artifactLines(captured);
    ASSERT_EQ(lines.size(), 1u) << "stderr:\n" << captured;
    EXPECT_GE(dss::test_support::leadingSeparatorRun(fs::path{lines[0].path}), 2u)
        << "reported '" << lines[0].path
        << "' — the leading separator run was collapsed, so the line names a "
           "path on the local drive instead of the machine the artifact was "
           "written to";
    EXPECT_TRUE(fs::exists(fs::path{lines[0].path}))
        << "the report names '" << lines[0].path
        << "', which does not exist — a report a user cannot follow";
}
