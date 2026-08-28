// D-DIAG-TWO-CODE-RENDERINGS — one diagnostic concept, ONE rendered spelling.
//
// ★★★ THE DEFECT. `program.cpp`'s `drainDiagnosticsToStderr` routes on
// `d.buffer.valid()`. A diagnostic WITH a source buffer went to
// `DiagnosticReporter::format`, whose header was
// `severityName` + `diagnosticCodePrefix` -> `error[P0009]: ` — the 4-hex-digit
// band form, closed with a COLON. A buffer-LESS one took the code-only
// one-liner built from `diagnosticCodeName` -> `error[K_SymbolUndefined] ` —
// the SYMBOLIC name, closed with a SPACE and no colon. So ONE code had TWO
// surface spellings AND two punctuations, and which pair a reader got depended
// on a property they could not see from the output.
//
// ⭐ THE CONSEQUENCE WAS STRUCTURAL, NOT STATISTICAL: any consumer matching
// `[A-Z][0-9A-F]{4}` could not see a buffer-less diagnostic AT ALL — and in the
// sqlite corpus arc 133 of 189 per-TU runs emitted only that invisible form, so
// every grep-based count in the arc had never looked at most of its population.
//
// ★ AND IT WAS NOT ONLY A MEASUREMENT PROBLEM — IT COST A REAL DEVELOPER A RED,
// IN THIS VERY DIRECTORY. `tests/program/test_suppress_request_ignored.cpp`
// still carries the note: "The first draft asserted `warning[D0021]` and went
// RED against a perfectly correct message." A person reading one rendering and
// writing an assertion against the other is exactly what the split guarantees.
//
// ★★ WHAT THIS FILE ASSERTS, AND WHY IT IS TOTAL RATHER THAN A SAMPLE. It runs
// two real driver compiles chosen to fail at DIFFERENT TIERS — one at the
// preprocessor (positioned, buffer-valid) and one at the linker (buffer-less) —
// captures what actually reaches `std::cerr`, and then requires, FOR EVERY
// DIAGNOSTIC THE REPORTER HOLDS, that stderr contains
// `severityName(d.severity) + "[" + diagnosticCodeName(d.code) + "]: "`.
// The expected token is DERIVED from the diagnostic, never typed, so the test
// cannot drift from the enum and cannot pass by agreeing with a stale literal.
//
// ⚠ THE ANTI-VACUITY CONTROL IS THE POINT OF THE FILE. An assertion that every
// rendered code is spelled canonically is satisfied TRIVIALLY by a run that
// only ever produced ONE KIND of diagnostic — which is precisely the shape that
// let the split survive: a test asserting only the positioned form stays green
// through a change that silently drops the buffer-less form's identity.
// `BothRenderingTiersWereActuallyExercised` therefore fails the run if the two
// compiles between them did not produce at least one buffer-VALID and at least
// one buffer-LESS diagnostic, so the coverage claim is measured, not assumed.
//
// ⚠ AND THE REMOVE-DIRECTION ASSERTION: no bracket in the output may hold a
// hex-band token. A test that only checked "the name is present" would stay
// green if the hex header came back alongside it.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

constexpr char const* kSpec = "x86_64:elf64-x86_64-linux-exec";

// Capture `std::cerr` for a scope. The driver's `drainDiagnosticsToStderr` is
// how a real run shows an operator anything, so this has to read what actually
// reaches the terminal rather than re-rendering from the reporter.
class CerrCapture {
public:
    CerrCapture() : old_(std::cerr.rdbuf(buf_.rdbuf())) {}
    ~CerrCapture() { std::cerr.rdbuf(old_); }
    CerrCapture(CerrCapture const&)            = delete;
    CerrCapture& operator=(CerrCapture const&) = delete;
    [[nodiscard]] std::string str() const { return buf_.str(); }

private:
    std::ostringstream buf_;
    std::streambuf*    old_;
};

// Fails inside the PREPROCESSOR, which owns a real span into real source, so
// every diagnostic it reports is buffer-VALID and takes the positioned renderer.
constexpr std::string_view kPositionedSource =
    "#error \"a positioned diagnostic with a real span\"\n"
    "int main(void) { return 0; }\n";

// Fails at the LINKER, which has no source buffer to point at, so its
// diagnostics are buffer-LESS and take the code-only one-liner.
constexpr std::string_view kBufferlessSource =
    "extern int dss_no_such_symbol_anywhere(void);\n"
    "int main(void) { return dss_no_such_symbol_anywhere(); }\n";

struct ProbeRun {
    int                rc = 0;
    std::string        stderrText;
    DiagnosticReporter rep{DiagnosticReporter::Config{}};
    std::size_t        buffered   = 0;
    std::size_t        bufferless = 0;
};

void compileCapturing(ProbeRun& run, ScratchDir& scratch, std::string_view source) {
    fs::path const src = scratch.path() / "code_rendering_probe.c";
    {
        std::ofstream out{src, std::ios::binary};
        ASSERT_TRUE(out.good());
        out << source;
    }
    scratch.useAsCwd();
    {
        CerrCapture cap;
        Program     prog;
        prog.setOutputDir(scratch.path() / "out");
        run.rc = prog.compileFiles({src.generic_string()}, "c", {kSpec}, run.rep);
        run.stderrText = cap.str();
    }
    for (auto const& d : run.rep.all()) {
        (d.buffer.valid() ? run.buffered : run.bufferless) += 1;
    }
}

// The canonical header token for a diagnostic, DERIVED from the diagnostic.
[[nodiscard]] std::string canonicalHeader(ParseDiagnostic const& d) {
    return std::string{severityName(d.severity)} + "["
         + std::string{diagnosticCodeName(d.code)} + "]: ";
}

} // namespace

TEST(DiagnosticCodeRendering, EveryTierSpellsItsCodeTheSameWay) {
    ProbeRun positioned;
    ProbeRun bufferless;
    {
        ScratchDir scratch{Location::InsideRepo, "code-rendering"};
        compileCapturing(positioned, scratch, kPositionedSource);
    }
    {
        ScratchDir scratch{Location::InsideRepo, "code-rendering"};
        compileCapturing(bufferless, scratch, kBufferlessSource);
    }

    ASSERT_NE(positioned.rc, 0) << "the positioned probe was supposed to fail";
    ASSERT_NE(bufferless.rc, 0) << "the buffer-less probe was supposed to fail";

    for (ProbeRun const* run : {&positioned, &bufferless}) {
        for (auto const& d : run->rep.all()) {
            auto const want = canonicalHeader(d);
            EXPECT_NE(run->stderrText.find(want), std::string::npos)
                << "a diagnostic reached stderr spelled differently from every "
                   "other tier. Expected the canonical header token "
                << want << " (buffer "
                << (d.buffer.valid() ? "VALID -> positioned renderer"
                                     : "LESS -> code-only one-liner")
                << "). D-DIAG-TWO-CODE-RENDERINGS: the bracket holds "
                   "`diagnosticCodeName` and closes with `]: ` on EVERY "
                   "surface — see the contract in diagnostic_reporter.hpp.\n"
                   "stderr was:\n"
                << run->stderrText;
        }
    }
}

TEST(DiagnosticCodeRendering, BothRenderingTiersWereActuallyExercised) {
    // ★ WITHOUT THIS, THE TEST ABOVE IS VACUOUS. Its assertion is satisfied by
    // a run that produced only ONE kind of diagnostic — and "only ever saw the
    // positioned form" is exactly how the split survived for an entire arc.
    ProbeRun positioned;
    ProbeRun bufferless;
    {
        ScratchDir scratch{Location::InsideRepo, "code-rendering"};
        compileCapturing(positioned, scratch, kPositionedSource);
    }
    {
        ScratchDir scratch{Location::InsideRepo, "code-rendering"};
        compileCapturing(bufferless, scratch, kBufferlessSource);
    }

    EXPECT_GT(positioned.buffered, 0u)
        << "the preprocessor probe produced no BUFFER-VALID diagnostic, so the "
           "positioned renderer was never exercised and the spelling assertion "
           "covers only one tier. stderr was:\n"
        << positioned.stderrText;
    EXPECT_GT(bufferless.bufferless, 0u)
        << "the link probe produced no BUFFER-LESS diagnostic, so the code-only "
           "one-liner was never exercised — which is the half that was "
           "invisible to every hex-keyed consumer. stderr was:\n"
        << bufferless.stderrText;
}

TEST(DiagnosticCodeRendering, NoBracketHoldsAHexBandTokenAnyMore) {
    // The REMOVE-direction assertion. "The name is present" stays true if the
    // hex header comes back beside it, so the old spelling is asserted ABSENT.
    // ⚠ Anchored to a BRACKET, not to a bare `[A-Z][0-9A-F]{4}` anywhere in the
    // text: the elision marker deliberately prints `P0009 (P_UnexpectedToken)`
    // in its PROSE to keep the name-to-hex correspondence reachable from a log,
    // and that is contract, not a relapse (diagnostic_reporter.hpp says so).
    static std::regex const hexBracket{R"(\[[A-Z][0-9A-F]{4}\])"};

    for (std::string_view source : {kPositionedSource, kBufferlessSource}) {
        ProbeRun run;
        {
            ScratchDir scratch{Location::InsideRepo, "code-rendering"};
            compileCapturing(run, scratch, source);
        }
        ASSERT_FALSE(run.rep.all().empty())
            << "probe produced no diagnostics at all, so this assertion would "
               "pass vacuously. stderr was:\n"
            << run.stderrText;
        std::smatch m;
        std::string const text = run.stderrText;
        EXPECT_FALSE(std::regex_search(text, m, hexBracket))
            << "a bracketed hex-band code token is back in the output ("
            << m.str()
            << "). Every diagnostic header spells its code with "
               "`diagnosticCodeName`; `diagnosticCodePrefix` belongs to "
               "`hir_text.cpp`'s `@diag` surface, not to a diagnostic header.\n"
               "stderr was:\n"
            << text;
    }
}
