// D-PROGRAM-PROJECT-WIDE-PARSE-GATE-MASKS-CENSUS — the closing pin.
//
// ★★★ THE DEFECT, IN ONE SENTENCE: one translation unit's PARSE error deleted
// every OTHER unit's semantic-and-later diagnostics for the whole run, and
// nothing in the output said so — so an absent `S0006` read as "DSS handles
// `__uint128_t`" when the truth was "semantic analysis never ran". Two registry
// rows carried a false blocking relationship on exactly that inference.
//
// ★★★ WHY THE OBVIOUS TEST WOULD BE VACUOUS, AND WHAT THIS ONE PINS INSTEAD.
// A test that asserts "N diagnostics appear" passes for the wrong reason: it is
// satisfied by a run where the gate let ONE unit through, which is what the
// DEFECT already did. The property that actually matters is a CROSS-UNIT one —
// *a parse failure in unit A does not suppress a LATER-TIER diagnostic in unit
// B* — so both units must be in ONE project run, and the assertion must be
// about B's SEMANTIC diagnostic, not about a count.
//
// ★★ THE INPUT IS THE ROW'S OWN MEASUREMENT, MINIATURISED. The row records a
// whole-project run that was SILENT about `src/util.c` and `src/mutex_unix.c`
// while those two compiled IN ISOLATION yielded 5 `S0006` and 1 `S0001`. The
// two units below are that shape: one that cannot parse, one that parses and
// then fails SEMANTIC analysis. Before the fix the run reported only the first.
//
// ⚠ THE ISOLATION CONTROL IS NOT DECORATION. `SemanticUnitAloneStillReports`
// is what makes the cross-unit assertion falsifiable: if `tu_semantic.c` ever
// stops producing `S_UndeclaredIdentifier` on its own — a grammar change, a new
// builtin, a renamed code — the cross-unit test would go green by having
// nothing to detect. The control fails first and names why.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

constexpr char const* kParseBroken =
    "int broken(void) {\n"
    "    return 1 +\n"
    "}\n";

// Parses cleanly; fails at SEMANTIC analysis, the first tier the old run-wide
// gate deleted.
constexpr char const* kSemanticBroken =
    "int later(void) {\n"
    "    return no_such_identifier_here;\n"
    "}\n";

constexpr char const* kClean = "int fine(void) { return 7; }\n";

void writeFile(fs::path const& p, char const* text) {
    std::ofstream out{p, std::ios::binary};
    ASSERT_TRUE(out.good()) << "could not write " << p.string();
    out << text;
}

[[nodiscard]] bool has(DiagnosticReporter const& rep, DiagnosticCode code) {
    for (auto const& d : rep.all()) {
        if (d.code == code) return true;
    }
    return false;
}

[[nodiscard]] std::string noticeText(DiagnosticReporter const& rep) {
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::D_LaterPhasesNotRun) return d.actual;
    }
    return {};
}

[[nodiscard]] int compileAll(std::vector<fs::path> const& srcs,
                             DiagnosticReporter&          rep) {
    std::vector<std::string> names;
    names.reserve(srcs.size());
    for (auto const& s : srcs) names.push_back(s.generic_string());
    // ★ `compileUnits`, NEVER `compileFiles`, AND THE DIFFERENCE IS THE WHOLE
    // SUBJECT. `compileFiles` folds every named file into ONE multi-file
    // compilation unit, so "one unit failed to parse" and "the project failed
    // to parse" become the same statement and the row's defect is not even
    // expressible. `compileUnits` is the CU6 model — one unit per file, which
    // is what the CLI's `--compile a.c b.c` and every project build take — and
    // it is the model in which one unit can fail while its siblings stay
    // measurable.
    Program prog;
    return prog.compileUnits(names, "c", {"x86_64:elf64-x86_64-linux-exec"}, rep);
}

}  // namespace

// ── THE CONTROL: the semantic unit really does produce a semantic error ─────
TEST(ParseGateScope, SemanticUnitAloneStillReports) {
    ScratchDir scratch{Location::InsideRepo, "parsegate-ctl"};
    fs::path const sem = scratch.path() / "tu_semantic.c";
    writeFile(sem, kSemanticBroken);
    scratch.useAsCwd();

    DiagnosticReporter rep{DiagnosticReporter::Config{}};
    EXPECT_NE(compileAll({sem}, rep), 0);
    EXPECT_TRUE(has(rep, DiagnosticCode::S_UndeclaredIdentifier))
        << "the control unit stopped producing a SEMANTIC diagnostic on its "
           "own, so the cross-unit test below has nothing left to detect and "
           "would pass vacuously — fix this input before trusting that one";
}

// ── THE ROW'S CLOSING EVIDENCE ──────────────────────────────────────────────
//
// One project run, two units. The parse error must still be reported (the gate
// still REFUSES), and the OTHER unit's semantic error must be reported too (the
// gate no longer deletes the census). Asserting both together is what makes it
// a scope test rather than a count test.
TEST(ParseGateScope, AParseFailureInOneUnitDoesNotSuppressAnotherUnitsSemanticError) {
    ScratchDir scratch{Location::InsideRepo, "parsegate"};
    fs::path const bad = scratch.path() / "tu_parse.c";
    fs::path const sem = scratch.path() / "tu_semantic.c";
    writeFile(bad, kParseBroken);
    writeFile(sem, kSemanticBroken);
    scratch.useAsCwd();

    DiagnosticReporter rep{DiagnosticReporter::Config{}};
    EXPECT_NE(compileAll({bad, sem}, rep), 0)
        << "a build containing an unparseable unit must still FAIL — the fix "
           "widens what gets measured, it does not soften the verdict";

    EXPECT_TRUE(has(rep, DiagnosticCode::P_MissingRequiredChild))
        << "the parse error itself vanished — the gate's refusal must survive "
           "the scope change";

    EXPECT_TRUE(has(rep, DiagnosticCode::S_UndeclaredIdentifier))
        << "THIS IS THE DEFECT: one unit's PARSE error suppressed a different "
           "unit's SEMANTIC diagnostic for the whole run. gcc 13.3 and clang "
           "18.1 both report both on this exact input "
           "(D-PROGRAM-PROJECT-WIDE-PARSE-GATE-MASKS-CENSUS)";
}

// ── THE SCOPE NOTICE ────────────────────────────────────────────────────────
//
// The census being WHOLE is half the fix; the other half is that a run which
// could not measure everything must SAY which phases it skipped. Without this,
// `L_*` and `K_*` are still absent for a reason the reader cannot see — the
// original defect, one tier down.
TEST(ParseGateScope, TheRunAnnouncesWhichPhasesDidNotRun) {
    ScratchDir scratch{Location::InsideRepo, "parsegate-notice"};
    fs::path const bad = scratch.path() / "tu_parse.c";
    fs::path const ok  = scratch.path() / "tu_clean.c";
    writeFile(bad, kParseBroken);
    writeFile(ok, kClean);
    scratch.useAsCwd();

    DiagnosticReporter rep{DiagnosticReporter::Config{}};
    EXPECT_NE(compileAll({bad, ok}, rep), 0);

    std::string const notice = noticeText(rep);
    ASSERT_FALSE(notice.empty())
        << "the run skipped the cross-unit tiers and said nothing — an absent "
           "link-tier diagnostic is once again indistinguishable from an "
           "absent link-tier PROBLEM";
    // The counts, so a reader can tell a one-unit loss from a whole-project one.
    EXPECT_NE(notice.find("1 of 2"), std::string::npos)
        << "the notice stopped naming how much was excluded: " << notice;
    // The phases, so a reader can tell WHICH families are missing.
    EXPECT_NE(notice.find("link"), std::string::npos)
        << "the notice stopped naming the phases that did not run: " << notice;
    // And the reading rule, which is the whole point of the row.
    EXPECT_NE(notice.find("NOT MEASURED"), std::string::npos)
        << "the notice stopped saying that an absent diagnostic means "
           "unmeasured rather than absent: " << notice;
}

// ── NO ARTIFACT, AND NO DERIVATIVE CASCADE ──────────────────────────────────
//
// ★★ THE HAZARD THIS PINS IS THE ONE THE ORIGINAL AUTHOR WAS RIGHT ABOUT.
// Letting a knowingly-incomplete program reach the cross-unit tiers turns a
// clean refusal into a pile of invented complaints — a missing entry point, an
// unresolved symbol that is unresolved only BECAUSE a unit was excluded. Here
// `tu_uses.c` calls a function defined ONLY in the excluded unit, which is the
// worst case: if the link ever runs, this test reds with a `K_*` that is about
// the exclusion rather than about the source.
TEST(ParseGateScope, AnExcludedUnitProducesNoDerivativeLinkErrorAndNoArtifact) {
    ScratchDir scratch{Location::InsideRepo, "parsegate-cascade"};
    fs::path const bad  = scratch.path() / "tu_parse.c";
    fs::path const uses = scratch.path() / "tu_uses.c";
    writeFile(bad, "int helper(void) {\n    return 1 +\n}\n");
    writeFile(uses, "int helper(void);\nint main(void) { return helper(); }\n");
    scratch.useAsCwd();

    DiagnosticReporter rep{DiagnosticReporter::Config{}};
    EXPECT_NE(compileAll({bad, uses}, rep), 0);

    for (auto const& d : rep.all()) {
        EXPECT_NE(d.code, DiagnosticCode::K_SymbolUndefined)
            << "the link ran over a program that is knowingly missing a "
               "translation unit, so it reported an undefined symbol caused by "
               "the EXCLUSION rather than by the source — this is exactly the "
               "derivative noise the run-wide gate existed to prevent, and the "
               "per-unit gate must prevent it too";
    }

    // `<cwd>/target/<formatName>/` is where an artifact would land for a build
    // with no `--output`; a failed build must leave it empty of one.
    fs::path const artifact =
        scratch.path() / "target" / "elf64-x86_64-linux-exec" / "tu_parse";
    EXPECT_FALSE(fs::exists(artifact))
        << "a build missing a translation unit wrote an executable anyway — a "
           "half-program on disk from a failed build is strictly worse than "
           "the census it was traded for";
}

// ── THE HEALTHY PATH IS UNTOUCHED ───────────────────────────────────────────
//
// The scope change must be invisible to every build that parses. Without this,
// a regression that made `analysisOnly` always-on would leave the three tests
// above green while silently ending artifact emission for everyone.
TEST(ParseGateScope, AProjectWhereEveryUnitParsesStillLinksAndSaysNothing) {
    ScratchDir scratch{Location::InsideRepo, "parsegate-healthy"};
    fs::path const a = scratch.path() / "a.c";
    fs::path const b = scratch.path() / "b.c";
    writeFile(a, kClean);
    writeFile(b, "int fine(void);\nint main(void) { return fine(); }\n");
    scratch.useAsCwd();

    DiagnosticReporter rep{DiagnosticReporter::Config{}};
    EXPECT_EQ(compileAll({a, b}, rep), 0)
        << "a project whose units all parse stopped building";
    EXPECT_FALSE(has(rep, DiagnosticCode::D_LaterPhasesNotRun))
        << "a healthy build announced that phases did not run — the notice "
           "must fire only when something really was skipped, or it becomes "
           "noise and stops being read";
}
