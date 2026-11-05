// THE CONFLICT-MARKER GUARD'S OWN SELF-TEST, ONE CASE PER ARM — the ctest entry
// `harness/conflict_markers_selftest`.
//
// WHAT THIS IS. `.harness-config/runner/actions/check-conflict-markers/check-conflict-markers.py` refuses every
// line of the tree's text files -- tracked, or untracked and not ignored -- that is a merge-conflict marker git
// writes: seven `<`, `|` or `>` at a line's start, and seven `=` alone between an opener and its closer. Run with
// `--self-test` it proves which lines are a marker, when a separator is one, and that a conflict git itself
// writes is refused line by line, and prints one verdict line per arm.
//
// This binary RUNS that self-test — once, by the command line the top-level CMakeLists composed at
// configure — and reads each arm's verdict into a case of its own. IT RE-IMPLEMENTS NO ARM. Everything
// it does beyond naming the arms is tests/test_support/script_selftest.hpp. The tree itself is judged by the
// entry `conflict_markers_guard`.
// ⓘ No label: it is a self-test, and its fixtures are the host's own (its git), so every leg runs it.

#include "dss_conflict_markers_selftest_command.hpp"

#include "script_selftest.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace {

// Every arm of the self-test: X(<the case>, "<the arm's name, exactly as the program prints it>").
// The order is the program's. GENERATED from the program's own verdict lines; an arm added to the
// program without a row here is a red of the set case below, by the arm's name.
#define DSS_CONFLICT_MARKERS_SELFTEST_ARMS(X)                                                                        \
    X(M1EachMarkerAsGitWritesItIsReadAsOneWithASideSNameAloneAndWithACrBeforeTheEnd,                                   \
      "M1 each marker as git writes it is read as one: with a side's name, alone, and with a CR before the end")       \
    X(M2EightOfACharacterSixSevenThenALetterASeparatorThenTextAndAMarkerNotAtTheStartAreNoMark,                        \
      "M2 eight of a character, six, seven then a letter, a separator then text, and a marker not at the start are no marker") \
    X(M3ASeparatorIsRefusedOnlyBetweenAnOpenerAndItsCloserAloneOrAfterTheCloserItIsAHeadingSUn,                        \
      "M3 a separator is refused only between an opener and its closer: alone, or after the closer, it is a heading's underline") \
    X(M4AConflictGitItselfWritesInTheDiff3StyleIsRefusedLineByLineTheOpenerTheBaseTheSeparator,                        \
      "M4 a conflict git itself writes, in the diff3 style, is refused line by line: the opener, the base, the separator and the closer, each named with its path and line") \
    X(M5AnUntrackedFileGitWouldAddIsReadAFileGitIgnoresAndABinaryFileAreNot,                                           \
      "M5 an untracked file git would add is read; a file git ignores and a binary file are not")                      \
    X(M6ATreeGitCannotReadIsUnmeasuredNeverClean, "M6 a tree git cannot read is UNMEASURED, never clean")              \
    X(M7ACleanTreeIsOkAndSaysHowManyTextFilesItReadAScanBelowItsFloorIsUnmeasured,                                     \
      "M7 a clean tree is OK and says how many text files it read; a scan below its floor is UNMEASURED")

#define DSS_CONFLICT_MARKERS_SELFTEST_ARM_NAME(caseName, armName) armName,
constexpr std::string_view kArmNames[] = {DSS_CONFLICT_MARKERS_SELFTEST_ARMS(DSS_CONFLICT_MARKERS_SELFTEST_ARM_NAME)};
#undef DSS_CONFLICT_MARKERS_SELFTEST_ARM_NAME

constexpr dss::test_support::ScriptSelfTest kEntry{
    .command          = dss::conflict_markers_selftest::kCommand,
    .workingDirectory = dss::conflict_markers_selftest::kWorkingDirectory,
    .standardOutput   = dss::conflict_markers_selftest::kStandardOutput,
    .marker           = "conflict-markers self-test: ",
    .armNames         = kArmNames,
    .tableName        = "DSS_CONFLICT_MARKERS_SELFTEST_ARMS",
};

// ONE run for the whole binary: every case reads the same output.
[[nodiscard]] dss::test_support::ScriptSelfTestRun const& theRun() {
    static dss::test_support::ScriptSelfTestRun const run = dss::test_support::runScriptSelfTest(kEntry);
    return run;
}

}  // namespace

#define DSS_CONFLICT_MARKERS_SELFTEST_CASE(caseName, armName) \
    TEST(ConflictMarkersSelfTest, caseName) { dss::test_support::expectTheScriptArmPassed(kEntry, theRun(), armName); }
DSS_CONFLICT_MARKERS_SELFTEST_ARMS(DSS_CONFLICT_MARKERS_SELFTEST_CASE)
#undef DSS_CONFLICT_MARKERS_SELFTEST_CASE

// The program's arms and this file's cases are the same set: an arm added to the program without a
// case here is a red, by the arm's name.
TEST(ConflictMarkersSelfTest, EveryArmTheProgramRanIsACaseAndEveryCaseAnArm) {
    dss::test_support::expectEveryScriptArmIsACaseAndEveryCaseAnArm(kEntry, theRun());
}

// The program exits non-zero exactly when an arm said FAILED; a traceback between two arms, or an arm
// count other than its own ratchet, is an exit code no arm's case reports, and this one does.
TEST(ConflictMarkersSelfTest, TheProgramsExitCodeAgreesWithItsArms) {
    dss::test_support::expectTheScriptsExitCodeAgreesWithItsArms(kEntry, theRun());
}
