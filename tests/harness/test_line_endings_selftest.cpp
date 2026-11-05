// THE LINE-ENDINGS GUARD'S OWN SELF-TEST, ONE CASE PER ARM — the ctest entry `harness/line_endings_selftest`.
//
// WHAT THIS IS. `.harness-config/runner/actions/check-line-endings/check-line-endings.py` refuses a tracked
// text blob that carries a CR, and (check F) a CR instrument that cannot see one: a detector verb, AS A WORD,
// beside a CR pattern on one line, unless an exemption token stands in the verb's own command or the line
// is marked as quoting the idiom. Run with `--selftest` it proves its byte tiers and each rule of check F,
// in both directions, on fixture repositories of its own, and prints one verdict line per arm.
//
// This binary RUNS that self-test — once, by the command line the top-level CMakeLists composed at
// configure — and reads each arm's verdict into a case of its own. IT RE-IMPLEMENTS NO ARM. Everything
// it does beyond naming the arms is tests/test_support/script_selftest.hpp.
//
// ⓘ A SECOND RUN, ON PURPOSE. The entry `line_endings_guard` judges the tree and runs the same self-test
// first, quietly: a guard whose own rules are red does not judge. This entry is where each arm is a case,
// so a mutation of the guard can be an arm of the mutation sweep.
// ⓘ No label: it is a self-test, and its fixtures are the host's own (its git, its file system), so
// every leg runs it.

#include "dss_line_endings_selftest_command.hpp"

#include "script_selftest.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace {

// Every arm of the self-test: X(<the case>, "<the arm's name, exactly as the program prints it>").
// The order is the program's. GENERATED from the program's own verdict lines; an arm added to the
// program without a row here is a red of the set case below, by the arm's name.
#define DSS_LINE_ENDINGS_SELFTEST_ARMS(X)                                                                            \
    X(S0aTheCrlfControlIsExactlyACrLfBLfCheckedAsBytesNotByTheCounter,                                                 \
      "S0a the CRLF control is exactly `a CR LF b LF` (checked as bytes, not by the counter)")                         \
    X(S0bTheLfTwinIsExactlyALfBLf, "S0b the LF twin is exactly `a LF b LF`")                                           \
    X(S1CrCountSeesTheCrOfTheCrlfControl, "S1 cr_count SEES the CR of the CRLF control")                               \
    X(S2CrCountInventsNoCrOnTheLfTwin, "S2 cr_count invents no CR on the LF twin")                                     \
    X(S3FilesOverTheCrlfControlExits1, "S3 --files over the CRLF control exits 1")                                     \
    X(S4FilesOverTheLfTwinExits0, "S4 --files over the LF twin exits 0")                                               \
    X(S5FilesOverAMissingPathIsUnmeasured2NeverAPass,                                                                  \
      "S5 --files over a missing path is UNMEASURED (2), never a pass")                                                \
    X(S6aCheckFSOwnGitGrepFiresOnThe3BlindLinesTheEngineTheAuditRuns,                                                  \
      "S6a check F's own `git grep` FIRES on the 3 blind lines (the engine the audit runs)")                           \
    X(S6bEverySafeFormIsARealBlindHitExemptedAndEachExemptionAlternativeIsTheOnlyOneSavingItsL,                        \
      "S6b every SAFE form is a real blind hit, exempted, and each exemption alternative is the ONLY one saving its line (removing it reds)") \
    X(S7TheSameLineMarkerExemptsAQuotedIdiom, "S7 the same-line marker exempts a quoted idiom")                        \
    X(S7bTheSameLineWithoutTheMarkerIsRefused, "S7b the SAME line without the marker is refused")                      \
    X(S7cALineInsideABeginEndRegionIsExemptInclusive, "S7c a line inside a BEGIN..END region is exempt (inclusive)")   \
    X(S7dTheLineAfterTheRegionSEndIsRefused, "S7d the line AFTER the region's END is refused")                         \
    X(S7eAQuotedRegionStillOpenAtEndOfFileIsRefusedByName,                                                             \
      "S7e a quoted region still OPEN at end of file is refused by name")                                              \
    X(S7fAnExemptionTokenInThePathDoesNotExemptTheTwinsRuleDid,                                                        \
      "S7f an exemption token in the PATH does not exempt (the twins' rule did)")                                      \
    X(S7gAnExemptionTokenInAnotherPipelineStageDoesNotExemptTheTwinsRuleDid,                                           \
      "S7g an exemption token in ANOTHER pipeline stage does not exempt (the twins' rule did)")                        \
    X(S18ACrPatternBesideADetectorVerbOnlyInsideAWordIsNoCrInstrumentEachSuchLineIsAHitOfTheQu,                        \
      "S18 a CR pattern beside a detector verb only INSIDE a word is no CR instrument: each such line is a hit of the query, and none is refused") \
    X(S19TheSamePatternBesideTheVerbAsAWordIsRefusedRgAndGrepCAlikeTheControl,                                         \
      "S19 the same pattern beside the verb AS A WORD is refused, rg and grep -c alike (the control)")                 \
    X(S8aGitSTopLevelForTheGuardSTreeIsTheTreeItsOwnFileLivesIn,                                                       \
      "S8a git's top level for the guard's tree IS the tree its own file lives in")                                    \
    X(S8bItsOwnFileNamedRelativeToThatRootIsARegularFileThereAndInTheIndex,                                            \
      "S8b its own file, named relative to that root, is a regular file there AND in the index")                       \
    X(S8cStandingInADecoyTreeReadTreeBytesReadsTheGuardSTreeABareRelativeReadGetsTheDecoy,                             \
      "S8c standing in a decoy tree, read_tree_bytes reads the GUARD's tree (a bare relative read gets the decoy)")    \
    X(S8RootControlCwdItsOwnTreeItsOwnTree, "S8 root CONTROL: cwd = its own tree -> its own tree")                     \
    X(S8RootCwdInsideAnotherGitRepositoryStillItsOwnTree,                                                              \
      "S8 root: cwd inside ANOTHER git repository -> still its own tree")                                              \
    X(S8RootCwdInsideNoGitRepositoryStillItsOwnTree, "S8 root: cwd inside NO git repository -> still its own tree")    \
    X(S8RootACallerSGitDirGitWorkTreeGitIndexFileNamingAnotherTreeAreIgnoredStillItsOwnTree,                           \
      "S8 root: a caller's GIT_DIR + GIT_WORK_TREE + GIT_INDEX_FILE naming another tree are IGNORED -> still its own tree") \
    X(S8RootItsFileCopiedIntoAnUntrackedTreeNestedInsideAnotherCheckoutIsRefusedGitAnswersForT,                        \
      "S8 root: its file COPIED into an untracked tree nested inside another checkout is REFUSED -- git answers for the enclosing one") \
    X(S9aCheckFPositiveControlATreeWhoseIndexHoldsThisGuardOk,                                                         \
      "S9a check F positive control: a tree whose index holds this guard -> OK")                                       \
    X(S9bTheGuardPresentButNotInTheIndexUnmeasuredNamingItsOwnFile,                                                    \
      "S9b ...the guard present but NOT in the index -> UNMEASURED, naming its own file")                              \
    X(S9cTheMarkerInNoFileUnmeasuredNeverOk, "S9c ...the marker in NO file -> UNMEASURED, never OK")                   \
    X(S10AGitThatCannotRunIsUnmeasuredNeverAnEmptyAnswer,                                                              \
      "S10 a git that cannot run is UNMEASURED, never an empty answer")                                                \
    X(S11DE1CTakeClosedRowSetsOnlyWCrlfWMixedConvictITextAndINoneLeaveE1TheLegacyGlobListStays,                        \
      "S11 D/E1/C take CLOSED row sets: only w/crlf|w/mixed convict, i/-text and i/none leave E1, the legacy glob list stays inside C") \
    X(S12CoreAutocrlfIsReadAsGitReadsABoolean, "S12 core.autocrlf is read as git reads a boolean")                     \
    X(S13E2SkipsBinaryNulInTheFirst8000BytesEmptyTextAndEolLfFilesAndConvictsACrOtherwise,                             \
      "S13 E2 skips binary (NUL in the first 8000 bytes), empty, -text and eol=lf files and convicts a CR otherwise")  \
    X(S14StaleCheckoutEvidenceAHistoryOffenderWithZeroCrOnDiskAndOnlyThat,                                             \
      "S14 stale-checkout evidence: a history offender with ZERO CR on disk, and only that")                           \
    X(S15AScanBelowItsFloorIsUnmeasured, "S15 a scan below its floor is UNMEASURED")                                   \
    X(S16ARelativeFilesPathResolvesAgainstWhereTheCallerStood,                                                         \
      "S16 a relative --files path resolves against where the CALLER stood")                                           \
    X(S17FilesFromACrlfListAndEmptyLinesReadCleanlyAnEmptyListExits2,                                                  \
      "S17 --files-from: a CRLF list and empty lines read cleanly; an empty list exits 2")

#define DSS_LINE_ENDINGS_SELFTEST_ARM_NAME(caseName, armName) armName,
constexpr std::string_view kArmNames[] = {DSS_LINE_ENDINGS_SELFTEST_ARMS(DSS_LINE_ENDINGS_SELFTEST_ARM_NAME)};
#undef DSS_LINE_ENDINGS_SELFTEST_ARM_NAME

constexpr dss::test_support::ScriptSelfTest kEntry{
    .command          = dss::line_endings_selftest::kCommand,
    .workingDirectory = dss::line_endings_selftest::kWorkingDirectory,
    .standardOutput   = dss::line_endings_selftest::kStandardOutput,
    .marker           = "line-endings self-test: ",
    .armNames         = kArmNames,
    .tableName        = "DSS_LINE_ENDINGS_SELFTEST_ARMS",
};

// ONE run for the whole binary: every case reads the same output.
[[nodiscard]] dss::test_support::ScriptSelfTestRun const& theRun() {
    static dss::test_support::ScriptSelfTestRun const run = dss::test_support::runScriptSelfTest(kEntry);
    return run;
}

}  // namespace

#define DSS_LINE_ENDINGS_SELFTEST_CASE(caseName, armName) \
    TEST(LineEndingsSelfTest, caseName) { dss::test_support::expectTheScriptArmPassed(kEntry, theRun(), armName); }
DSS_LINE_ENDINGS_SELFTEST_ARMS(DSS_LINE_ENDINGS_SELFTEST_CASE)
#undef DSS_LINE_ENDINGS_SELFTEST_CASE

// The program's arms and this file's cases are the same set: an arm added to the program without a
// case here is a red, by the arm's name.
TEST(LineEndingsSelfTest, EveryArmTheProgramRanIsACaseAndEveryCaseAnArm) {
    dss::test_support::expectEveryScriptArmIsACaseAndEveryCaseAnArm(kEntry, theRun());
}

// The program exits non-zero exactly when an arm said FAILED; a traceback between two arms, or an arm
// count other than its own ratchet, is an exit code no arm's case reports, and this one does.
TEST(LineEndingsSelfTest, TheProgramsExitCodeAgreesWithItsArms) {
    dss::test_support::expectTheScriptsExitCodeAgreesWithItsArms(kEntry, theRun());
}
