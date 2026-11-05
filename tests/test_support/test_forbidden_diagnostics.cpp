// Self-test for `tests/test_support/forbidden_diagnostics.hpp` (P69): the judgement of `forbidDiagnostics`, the
// absence assertion both corpus runners apply. An absence is satisfied by nothing happening, so each way the
// judgement can fail toward silence is pinned here — a code it does not find, a compile it does not look at, a log
// it reads no code from — on the shapes the runners hand it: code names in report order, the arm's prerequisites
// recorded before its own compile, a CLI log as the CLI writes it.

#include "forbidden_diagnostics.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using dss::test_support::CompileSaid;
using dss::test_support::ForbiddenSaid;

namespace {

constexpr char const* kWarned  = "H_NonVoidFunctionEndReachable";
constexpr char const* kNotSaid = "H_UnreachableCode";

}  // namespace

// The rule itself: a forbidden code the compile said is NAMED — once however many times it was said, at the place the
// manifest declared it — and a code nobody forbade, or a forbidden one nobody said, is not.
TEST(ForbiddenDiagnostics, AForbiddenCodeACompileSaidIsNamedOnceInDeclarationOrder) {
    using dss::test_support::forbiddenCodesSaid;
    std::vector<std::string> const said{kWarned, "S_UnknownAttribute", kWarned, "X_OptFixpointTruncated"};
    EXPECT_EQ(forbiddenCodesSaid({kNotSaid, kWarned}, said), (std::vector<std::string>{kWarned}))
        << "the said code is named, once; the unsaid one is not";
    EXPECT_EQ(forbiddenCodesSaid({"X_OptFixpointTruncated", kNotSaid, kWarned}, said),
              (std::vector<std::string>{"X_OptFixpointTruncated", kWarned}))
        << "in the manifest's declaration order, not the order the compile said them";
    EXPECT_TRUE(forbiddenCodesSaid({kNotSaid}, said).empty()) << "a code nobody said is not named";
    EXPECT_TRUE(forbiddenCodesSaid({}, said).empty()) << "an empty list forbids nothing";
    EXPECT_TRUE(forbiddenCodesSaid({kWarned}, {}).empty()) << "a compile that said nothing said nothing forbidden";
}

// EVERY COMPILE OF AN ARM IS JUDGED. A `dependsOn` library the arm builds from the example's own sources is the
// example's compile: its report, a nested prerequisite's, and the arm's own are judged by the one list, and each that
// said a forbidden code is named, in the order the runner recorded them (prerequisites as built, the arm's own last).
TEST(ForbiddenDiagnostics, EveryCompileOfAnArmIsJudgedItsPrerequisitesToo) {
    using dss::test_support::forbiddenDiagnosticsOfAnArm;
    std::vector<std::string> const forbidden{kNotSaid, kWarned};
    CompileSaid const nested{"the build of its dependsOn artifact libinner.a (spec=x86_64:elf64-x86_64-linux-staticlib)",
                             {"S_UnknownAttribute", kNotSaid}};
    CompileSaid const prerequisite{"the build of its dependsOn artifact libouter.a (spec=x86_64:elf64-x86_64-linux-staticlib)",
                                   {kWarned, kWarned}};
    CompileSaid const ownClean{"the compile", {"S_UnknownAttribute"}};
    CompileSaid const ownWarned{"the compile", {kWarned}};

    // Only a prerequisite said it: the arm is still judged by it.
    EXPECT_EQ(forbiddenDiagnosticsOfAnArm(forbidden, {prerequisite, ownClean}),
              (std::vector<ForbiddenSaid>{{prerequisite.who, {kWarned}}}))
        << "a prerequisite's report is the example's report";
    // A nested prerequisite, the prerequisite and the arm's own compile: each named, in recorded order.
    EXPECT_EQ(forbiddenDiagnosticsOfAnArm(forbidden, {nested, prerequisite, ownWarned}),
              (std::vector<ForbiddenSaid>{{nested.who, {kNotSaid}}, {prerequisite.who, {kWarned}},
                                          {ownWarned.who, {kWarned}}}))
        << "no compile's report hides another's";
    // Only the arm's own said it.
    EXPECT_EQ(forbiddenDiagnosticsOfAnArm(forbidden, {ownWarned}),
              (std::vector<ForbiddenSaid>{{ownWarned.who, {kWarned}}}));
    // Nothing forbidden was said by any compile.
    EXPECT_TRUE(forbiddenDiagnosticsOfAnArm({"X_OptFixpointTruncated"}, {nested, prerequisite, ownWarned}).empty());
    EXPECT_TRUE(forbiddenDiagnosticsOfAnArm(forbidden, {}).empty()) << "an arm that compiled nothing said nothing";
}

// THE CLI'S LOG. What the CLI reported is read off the `<severity>[<CodeName>]:` headers it opens every diagnostic
// with, at column 0 — at every severity, with repeats; a header quoted inside a source excerpt, an indented one, and
// prose in brackets are not reports. The forbidden codes are then judged over what was read.
TEST(ForbiddenDiagnostics, ACliLogReportsTheCodesOfItsColumnZeroHeaders) {
    using dss::test_support::diagnosticCodesInCliLog;
    using dss::test_support::forbiddenCodesSaid;
    std::string const log =
        "warning[H_NonVoidFunctionEndReachable]: [target=t] control can reach the end\n"
        // (planted DATA in two adjacent literals, one compiled string: spelled as one piece it reads to the
        //  positional-citation guard as a citation of a source line)
        "  --> main.c" ":1:20\n"
        "   |\n"
        " 1 | int helper(void) { } /* warning[H_UnreachableCode]: quoted, not reported */\n"
        "  warning[S_UnknownAttribute]: indented, so not a header\n"
        "info[X_OptFixpointTruncated]: an advisory at another severity\r\n"
        "error[S_TypeMismatch]: and an error\n"
        "warning[H_NonVoidFunctionEndReachable]: a second report of the first\n"
        "warning[not a code]: prose in brackets\n"
        "warning[S_UnusedVariable]: the log's last line";
    auto const reported = diagnosticCodesInCliLog(log);
    EXPECT_EQ(reported, (std::vector<std::string>{"H_NonVoidFunctionEndReachable", "X_OptFixpointTruncated",
                                                  "S_TypeMismatch", "H_NonVoidFunctionEndReachable",
                                                  "S_UnusedVariable"}))
        << "the report headers, at every severity, and nothing else";
    EXPECT_EQ(forbiddenCodesSaid({kNotSaid, "S_TypeMismatch", kWarned}, reported),
              (std::vector<std::string>{"S_TypeMismatch", kWarned}))
        << "a forbidden code the log reports is named, once, in declaration order";
    EXPECT_TRUE(forbiddenCodesSaid({kNotSaid, "S_UnknownAttribute"}, reported).empty())
        << "a code the log only QUOTES, or indents, is not reported";
    EXPECT_TRUE(diagnosticCodesInCliLog("").empty());
    EXPECT_TRUE(diagnosticCodesInCliLog("warning[]: an empty bracket\n[H_UnreachableCode]: no severity\n").empty());
}

// The verdict in words is one sentence for both runners: each compile, and what it said that was forbidden.
TEST(ForbiddenDiagnostics, TheVerdictNamesEachCompileAndItsCodes) {
    using dss::test_support::describeForbiddenSaid;
    EXPECT_EQ(describeForbiddenSaid({{"the compile", {kWarned}}}), std::string{"the compile emitted "} + kWarned);
    EXPECT_EQ(describeForbiddenSaid({{"the build of its dependsOn artifact lib.a (spec=s)", {kNotSaid, kWarned}},
                                     {"the compile", {kWarned}}}),
              std::string{"the build of its dependsOn artifact lib.a (spec=s) emitted "} + kNotSaid + ", " + kWarned
                  + "; the compile emitted " + kWarned);
    EXPECT_TRUE(describeForbiddenSaid({}).empty());
}
