// Direct self-tests for the coverage-boundary vocabulary.
//
// D-TEST-INTEGRATED-RUNNER-BUILDS-ONLY-THE-HOST-RUNNABLE-SPEC-SO-ONE-RUNNER-SEES-A-CAPABILITY
//
// ★★★ WHY A UNIT SUITE EXISTS BESIDE THE END-TO-END ENTRY. The
// `integrated_tests/coverage-boundary` entry runs both corpus harnesses and
// judges the real thing, which is the measurement that matters — but most of
// this vocabulary's REFUSALS are unreachable from there. A torn line, an
// unknown field, a duplicated spec, a report claiming a spec its manifest never
// declared: each is a state a working pair of runners never produces, so the end
// -to-end entry can only ever exercise the happy path plus whichever refusal a
// mutant reaches. A refusal nothing exercises is a refusal nobody knows fires.
//
// ⚠ EVERY CASE ASSERTS ON MESSAGE CONTENT, not merely on the boolean. A parser
// that refused for the WRONG reason would still red — and would send the reader
// to the wrong file — which is the standard `test_mutate_target_schema.cpp`
// already sets for this directory.

#include "coverage_boundary.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using ::dss::test_support::ClauseVerdict;
using ::dss::test_support::CoverageReport;
using ::dss::test_support::findCoverageReport;
using ::dss::test_support::judgeCoverageBoundary;
using ::dss::test_support::kCoverageRunnerCli;
using ::dss::test_support::kCoverageRunnerInProcess;
using ::dss::test_support::parseCoverageLine;
using ::dss::test_support::renderCoverageLine;

namespace {

// The fixture pair the whole suite reasons about: the boundary EXACTLY as this
// project has it on a host that can execute only `hostspec`.
//
// ⚠ THE SPECS ARE FICTIONAL ON PURPOSE. Nothing in this vocabulary parses a
// spec, and naming a real arch here would invite the next reader to believe the
// judgement keys on one — it does not, and the agnosticism bar is why.
CoverageReport inProcessReport() {
    CoverageReport r;
    r.runner    = std::string{kCoverageRunnerInProcess};
    r.exampleId = "lang/example";
    r.declared  = {"hostspec", "farspec-a", "farspec-b"};
    r.compiled  = {"hostspec", "farspec-a", "farspec-b"};
    r.spawned   = {"hostspec"};
    r.ran       = {"hostspec"};
    return r;
}

CoverageReport cliReport() {
    CoverageReport r;
    r.runner    = std::string{kCoverageRunnerCli};
    r.exampleId = "lang/example";
    r.declared  = {"hostspec", "farspec-a", "farspec-b"};
    r.compiled  = {"hostspec"};
    r.spawned   = {"hostspec"};
    r.ran       = {"hostspec"};
    return r;
}

std::vector<std::string> const kHostExcluded{"farspec-a", "farspec-b"};

// Find one clause's verdict by the prefix of its name, so a case names the
// clause it is about instead of an index into a vector.
ClauseVerdict clauseNamed(std::vector<ClauseVerdict> const& all,
                          std::string const& prefix) {
    for (auto const& v : all) {
        if (v.clause.rfind(prefix, 0) == 0) return v;
    }
    ADD_FAILURE() << "no clause whose name starts with '" << prefix
                  << "' — the judgement stopped producing it, so every case"
                     " below that names it would silently assert nothing";
    return ClauseVerdict{};
}

// ── the grammar itself ──────────────────────────────────────────────────────

TEST(CoverageBoundaryGrammar, RenderAndParseRoundTrip) {
    auto const original = inProcessReport();
    CoverageReport parsed;
    std::string    why;
    ASSERT_TRUE(parseCoverageLine(renderCoverageLine(original), parsed, why))
        << why;
    EXPECT_EQ(parsed.runner, original.runner);
    EXPECT_EQ(parsed.exampleId, original.exampleId);
    EXPECT_EQ(parsed.declared, original.declared);
    EXPECT_EQ(parsed.compiled, original.compiled);
    EXPECT_EQ(parsed.spawned, original.spawned);
    EXPECT_EQ(parsed.ran, original.ran);
}

// An EMPTY set must round-trip as emptiness rather than as a missing field —
// the one asymmetry that would let a truncated line read as "this runner built
// nothing", which is a legitimate state and therefore an unnoticeable lie.
TEST(CoverageBoundaryGrammar, AnEmptySetRoundTripsAsEmptyAndNotAsAbsent) {
    CoverageReport empty;
    empty.runner    = std::string{kCoverageRunnerCli};
    empty.exampleId = "lang/example";
    empty.declared  = {"hostspec"};
    auto const line = renderCoverageLine(empty);
    EXPECT_NE(line.find("compiled=-"), std::string::npos) << line;
    EXPECT_NE(line.find("spawned=-"), std::string::npos) << line;
    EXPECT_NE(line.find("ran=-"), std::string::npos) << line;
    CoverageReport parsed;
    std::string    why;
    ASSERT_TRUE(parseCoverageLine(line, parsed, why)) << why;
    EXPECT_TRUE(parsed.compiled.empty());
    EXPECT_TRUE(parsed.spawned.empty());
    EXPECT_TRUE(parsed.ran.empty());
}

TEST(CoverageBoundaryGrammar, ATrailingCarriageReturnIsToleratedNotRefused) {
    CoverageReport parsed;
    std::string    why;
    EXPECT_TRUE(parseCoverageLine(renderCoverageLine(cliReport()) + "\r",
                                  parsed, why))
        << why;
}

TEST(CoverageBoundaryGrammar, ALineWithoutTheMarkerIsRefused) {
    CoverageReport parsed;
    std::string    why;
    EXPECT_FALSE(parseCoverageLine("[  OK  ] Examples.RunFromManifest", parsed,
                                   why));
    EXPECT_NE(why.find("does not start with"), std::string::npos) << why;
}

TEST(CoverageBoundaryGrammar, ATornLineIsRefusedOnItsFieldCount) {
    CoverageReport parsed;
    std::string    why;
    EXPECT_FALSE(parseCoverageLine(
        "[coverage-boundary] runner=in-process example=lang/example", parsed,
        why));
    EXPECT_NE(why.find("fields"), std::string::npos) << why;
    EXPECT_NE(why.find("torn"), std::string::npos) << why;
}

// The CLOSED, ORDERED field list. A producer that grew a field, or reordered
// two, must not be read through the prefix it still happens to write.
TEST(CoverageBoundaryGrammar, AnUnknownFieldIsRefusedAndNamed) {
    CoverageReport parsed;
    std::string    why;
    EXPECT_FALSE(parseCoverageLine(
        "[coverage-boundary] runner=in-process example=lang/example"
        " declared=a linked=a spawned=a ran=a",
        parsed, why));
    EXPECT_NE(why.find("linked"), std::string::npos) << why;
    EXPECT_NE(why.find("CLOSED"), std::string::npos) << why;
}

TEST(CoverageBoundaryGrammar, AnUnknownRunnerIsRefused) {
    CoverageReport parsed;
    std::string    why;
    EXPECT_FALSE(parseCoverageLine(
        "[coverage-boundary] runner=some-third-harness example=lang/example"
        " declared=a compiled=a spawned=a ran=a",
        parsed, why));
    EXPECT_NE(why.find("some-third-harness"), std::string::npos) << why;
}

TEST(CoverageBoundaryGrammar, ADuplicatedSpecInASetIsRefused) {
    CoverageReport parsed;
    std::string    why;
    EXPECT_FALSE(parseCoverageLine(
        "[coverage-boundary] runner=cli-subprocess example=lang/example"
        " declared=a,a compiled=a spawned=a ran=a",
        parsed, why));
    EXPECT_NE(why.find("twice"), std::string::npos) << why;
}

TEST(CoverageBoundaryGrammar, CompiledOutsideDeclaredIsRefused) {
    CoverageReport parsed;
    std::string    why;
    EXPECT_FALSE(parseCoverageLine(
        "[coverage-boundary] runner=in-process example=lang/example"
        " declared=a compiled=a,b spawned=a ran=a",
        parsed, why));
    EXPECT_NE(why.find("never declared"), std::string::npos) << why;
}

// A spawn with no build behind it is the one shape that cannot be true, so it
// is an instrument fault rather than a finding about either runner.
TEST(CoverageBoundaryGrammar, SpawnedOutsideCompiledIsRefused) {
    CoverageReport parsed;
    std::string    why;
    EXPECT_FALSE(parseCoverageLine(
        "[coverage-boundary] runner=in-process example=lang/example"
        " declared=a,b compiled=a spawned=a,b ran=a,b",
        parsed, why));
    EXPECT_NE(why.find("never compiled"), std::string::npos) << why;
}

// An outcome from an attempt nobody made. The two nesting rules are separate
// refusals because they fail differently: `spawned` outside `compiled` is a
// runner that lost track of its builds, `ran` outside `spawned` is one that
// lost track of its execs.
TEST(CoverageBoundaryGrammar, RanOutsideSpawnedIsRefused) {
    CoverageReport parsed;
    std::string    why;
    EXPECT_FALSE(parseCoverageLine(
        "[coverage-boundary] runner=in-process example=lang/example"
        " declared=a,b compiled=a,b spawned=a ran=a,b",
        parsed, why));
    EXPECT_NE(why.find("attempt nobody made"), std::string::npos) << why;
}

// ── finding the line in a runner's captured output ──────────────────────────

TEST(CoverageBoundaryLookup, PicksTheLineForTheNamedExampleOutOfNoise) {
    std::string const body =
        "[==========] Running 1 test.\n"
        + renderCoverageLine([] {
              auto other = inProcessReport();
              other.exampleId = "lang/other";
              return other;
          }())
        + "\n" + renderCoverageLine(inProcessReport()) + "\n"
        + "[  PASSED  ] 1 test.\n";
    CoverageReport found;
    std::string    why;
    ASSERT_TRUE(findCoverageReport(body, "lang/example", found, why)) << why;
    EXPECT_EQ(found.exampleId, "lang/example");
    EXPECT_EQ(found.compiled.size(), 3u);
}

// ZERO lines is the failure this lookup exists to refuse: an instrument that
// observed nothing must never be read as one that observed a small coverage.
TEST(CoverageBoundaryLookup, NoLineIsARefusalAndNotAnEmptyReport) {
    CoverageReport found;
    std::string    why;
    EXPECT_FALSE(findCoverageReport("[  PASSED  ] 1 test.\n", "lang/example",
                                    found, why));
    EXPECT_NE(why.find("reported nothing"), std::string::npos) << why;
}

TEST(CoverageBoundaryLookup, TwoLinesForOneExampleIsARefusal) {
    std::string const body = renderCoverageLine(inProcessReport()) + "\n"
                           + renderCoverageLine(inProcessReport()) + "\n";
    CoverageReport found;
    std::string    why;
    EXPECT_FALSE(findCoverageReport(body, "lang/example", found, why));
    EXPECT_NE(why.find("refusing to guess"), std::string::npos) << why;
}

// A MALFORMED line carrying the marker must be a hard refusal, never a
// candidate stepped over: skipping it is how a producer that broke its own
// grammar reads as a producer that said nothing.
TEST(CoverageBoundaryLookup, AMalformedMarkedLineIsRefusedNotSkipped) {
    CoverageReport found;
    std::string    why;
    EXPECT_FALSE(findCoverageReport(
        "[coverage-boundary] runner=in-process example=lang/example\n",
        "lang/example", found, why));
    EXPECT_NE(why.find("malformed coverage line"), std::string::npos) << why;
}

// ── the clauses ─────────────────────────────────────────────────────────────

TEST(CoverageBoundaryClauses, TheBoundaryAsThisProjectHasItPassesEveryClause) {
    auto const verdicts =
        judgeCoverageBoundary(inProcessReport(), cliReport(), kHostExcluded);
    ASSERT_EQ(verdicts.size(), 5u)
        << "the clause count changed; a case below names a clause by prefix and"
           " would silently stop asserting";
    for (auto const& v : verdicts) {
        EXPECT_TRUE(v.ok) << v.clause << " — " << v.detail;
        EXPECT_FALSE(v.detail.empty())
            << v.clause
            << " passed with an empty detail — a passing clause that does not"
               " say what it saw is how a vacuous pass stays invisible";
    }
}

TEST(CoverageBoundaryClauses, C1RedsWhenTheRunnersDisagreeAboutTheManifest) {
    auto cli = cliReport();
    cli.declared.pop_back();  // this runner never heard of the last target
    auto const v = clauseNamed(judgeCoverageBoundary(inProcessReport(), cli,
                                                     kHostExcluded),
                               "C1");
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.detail.find("farspec-b"), std::string::npos) << v.detail;
}

TEST(CoverageBoundaryClauses, C1RedsWhenTheTwoReportsNameDifferentExamples) {
    auto cli = cliReport();
    cli.exampleId = "lang/other";
    auto const v = clauseNamed(judgeCoverageBoundary(inProcessReport(), cli,
                                                     kHostExcluded),
                               "C1");
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.detail.find("different examples"), std::string::npos)
        << v.detail;
}

// The direction that matters: the in-process runner LOSES a spec the CLI runner
// still covers. That is a spec whose only remaining witness is the harness that
// was never meant to be the sole witness of anything.
TEST(CoverageBoundaryClauses, C2RedsWhenTheCliReachesWhatItsSiblingDoesNot) {
    auto inproc = inProcessReport();
    inproc.compiled = {"farspec-a", "farspec-b"};
    inproc.spawned  = {};
    inproc.ran      = {};
    auto const v = clauseNamed(judgeCoverageBoundary(inproc, cliReport(),
                                                     kHostExcluded),
                               "C2");
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.detail.find("hostspec"), std::string::npos) << v.detail;
}

// THE CLAUSE THE WHOLE FILE EXISTS FOR. The in-process runner stops compiling
// the cross-host specs — the exact drift a `runOn` gate moved above the compile
// would produce — and nothing else changes. Both suites would still be green.
TEST(CoverageBoundaryClauses, C3RedsWhenASpecIsCompiledByNeitherRunner) {
    auto inproc     = inProcessReport();
    inproc.compiled = {"hostspec"};
    auto const v = clauseNamed(judgeCoverageBoundary(inproc, cliReport(),
                                                     kHostExcluded),
                               "C3");
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.detail.find("farspec-a"), std::string::npos) << v.detail;
    EXPECT_NE(v.detail.find("farspec-b"), std::string::npos) << v.detail;
    EXPECT_NE(v.detail.find("witnessed by NOBODY"), std::string::npos)
        << v.detail;
}

// A GAIN must not red. If the CLI runner grew to compile every declared spec,
// the boundary narrows to nothing and every clause still holds — a guard that
// fired on an improvement would be a ratchet pointing the wrong way.
TEST(CoverageBoundaryClauses, TeachingTheCliToCompileEverythingStaysGreen) {
    auto cli     = cliReport();
    cli.compiled = {"hostspec", "farspec-a", "farspec-b"};
    for (auto const& v :
         judgeCoverageBoundary(inProcessReport(), cli, kHostExcluded)) {
        EXPECT_TRUE(v.ok) << v.clause << " — " << v.detail;
    }
}

TEST(CoverageBoundaryClauses, C4RedsWhenARunnerTriesToSpawnAHostExcludedSpec) {
    auto inproc   = inProcessReport();
    inproc.spawned = {"hostspec", "farspec-a"};
    auto const v = clauseNamed(judgeCoverageBoundary(inproc, cliReport(),
                                                     kHostExcluded),
                               "C4");
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.detail.find("in-process:farspec-a"), std::string::npos)
        << v.detail;
}

// C5 is the anti-vacuity clause: with no spec run by both runners the subset
// clause holds trivially and the judgement asserts nothing about "both runners
// for the host target".
TEST(CoverageBoundaryClauses, C5RedsWhenTheTwoRunnersShareNoExecutedSpec) {
    auto cli = cliReport();
    cli.ran  = {};
    auto const v = clauseNamed(judgeCoverageBoundary(inProcessReport(), cli,
                                                     kHostExcluded),
                               "C5");
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.detail.find("asserts nothing"), std::string::npos) << v.detail;
}

}  // namespace
