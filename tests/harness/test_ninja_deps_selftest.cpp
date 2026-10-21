// THE NINJA DEPENDENCY-RECORD CHECK'S OWN SELF-TEST, ONE CASE PER ARM — the ctest entry
// `ninja_deps_selftest_guard`.
//
// WHAT THIS IS. `.harness-config/runner/actions/check-ninja-deps/check-ninja-deps.py`
// refuses a gate over a build directory whose objects recorded no header dependencies,
// and run with `--self-test` it proves its own rules on text and on throwaway trees: what
// counts as a zero record, when a named directory is checked, skipped or refused, how a
// source's include directives and a command line's words are read, and — for an object
// whose own rule is `deps = msvc` — which zero record is the CORRECT record of its unit
// and which is a lost one; then that `check()` uses that verdict, what `--record` prints
// and refuses, and that a run which stops says where. Each of those proofs is an ARM, and
// the program prints one verdict line per arm.
//
// This binary RUNS that self-test — once, by the command line the top-level CMakeLists
// composed at configure — and reads each arm's verdict into a case of its own. IT
// RE-IMPLEMENTS NO ARM. Everything it does beyond naming the arms is
// tests/test_support/script_selftest.hpp, which says why a script entry is driven this
// way and what the four parts of the pattern are.
//
// ★ IT IS THE ENTRY, NOT A SECOND ONE. The entry of this name was the program started
// bare by ctest: one verdict for every arm, and no test binary — so no mutation of the
// program could be an arm of the mutation sweep. There is one entry and one run per leg.
// ⓘ Its name ends in `guard` and it carries NO `repo-guard` label: that label is given to
// the top directory's entries, and this one is registered beside the other gates on
// runner actions (tests/harness). So it runs on EVERY leg — the program compares paths by
// the host's own rules, and each host proves them.

#include "dss_ninja_deps_selftest_command.hpp"

#include "script_selftest.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace {

// Every arm of the self-test: X(<the case>, "<the arm's name, exactly as the program
// prints it>"). The order is the program's.
#define DSS_NINJA_DEPS_SELFTEST_ARMS(X)                                                              \
    X(AHealthyRecordIsCountedAndNotFlagged, "a healthy record is counted and not flagged")           \
    X(AValidRecordWithZeroDepsIsFlagged, "a VALID record with zero deps IS flagged")                 \
    X(StaleWithZeroDepsIsFlaggedToo, "STALE with zero deps is flagged too")                          \
    X(IndentedDependencyLinesAreNotObjects, "indented dependency lines are not objects")             \
    X(MixedInputReportsOnlyTheBrokenOnes, "mixed input reports only the broken ones")                \
    X(ATwoDigitCountIsNotConfusedWithZero, "a two-digit count is not confused with zero")            \
    X(EmptyInputParsesAsZeroObjects,                                                                 \
      "empty input parses as zero objects, which the caller treats as FATAL")                        \
    X(ARecordsOwnLinesAreReadUnderItsObject,                                                         \
      "a record's own lines are read under its object, and under no other")                          \
    X(OneRecordIsNamedByTheEndOfItsObjectsPath,                                                      \
      "one record is named by the end of its object's path, in either separator's spelling")         \
    X(ADirectiveTheAngleAndTheQuotedFormInAnySpacing,                                                \
      "a directive: the angle and the quoted form, in any spacing")                                  \
    X(ADirectiveACommentBeforeOrInsideItHidesNothing,                                                \
      "a directive: a comment before `#`, or inside the directive, hides nothing")                   \
    X(ADirectiveALineSpliceInsideItHidesNothing, "a directive: a line splice inside it hides nothing") \
    X(ADirectiveAByteOrderMarkBeforeTheFirstLineHidesNothing,                                        \
      "a directive: a byte order mark before the first line hides nothing")                          \
    X(ADirectiveTheDigraphIsTheHash, "a directive: `%:` is `#`")                                     \
    X(ADirectiveIncludeNextAndImportAreIncludes,                                                     \
      "a directive: `include_next` and `import` are includes")                                       \
    X(ADirectiveOneWhoseTextNamesNoFileIsReadAsNamingNone,                                           \
      "a directive: one whose text names no file is read as naming none")                            \
    X(ADirectiveACommentsTextAStringsAndARawStringsAreNone,                                          \
      "a directive: a comment's text, a string's and a raw string's are none")                       \
    X(ADirectiveAHashAfterCodeOnItsLineOpensNone,                                                    \
      "a directive: a `#` after code on its line opens none, and no other directive is an include")  \
    X(ADirectiveWhatOnlyLooksLikeACommentsOpeningHidesNothing,                                       \
      "a directive: what only looks like a comment's opening hides nothing after it")                \
    X(AnIncludeDirectoryIsReadInEverySpelling,                                                       \
      "an include directory is read in every spelling, glued or separated, quoted or not, from INCLUDES and from FLAGS") \
    X(AHeaderForcedInIsReadInEverySpelling, "a header forced in is read in every spelling")          \
    X(EachRulesOwnFlavourIsRead,                                                                     \
      "each rule's own `deps =` flavour is read, and a rule that declares none has none")            \
    X(NinjasAnswerIsHandedOnWhenItExitedZero,                                                        \
      "ninja's answer: what it printed is handed on when it exited zero")                            \
    X(NinjasAnswerAnExitCodeOtherThanZeroIsSaid,                                                     \
      "ninja's answer: an exit code other than zero is said, with the first line ninja printed")     \
    X(NinjasAnswerANinjaThatFailedAndPrintedNothingIsSaidByItsExitCode,                              \
      "ninja's answer: a ninja that failed and printed nothing is said by its exit code alone")      \
    X(NinjasAnswerANinjaThatIsNotThereOrDoesNotEndIsSaid,                                            \
      "ninja's answer: a ninja that is not there, or does not end, is said as that")                 \
    X(AMissingDirectoryIsFatalNeverASkip, "a MISSING directory is FATAL, never a skip")              \
    X(AllowNonNinjaDoesNotExcuseAMissingDirectory,                                                   \
      "--allow-non-ninja does NOT excuse a missing directory")                                       \
    X(ADirectoryWithNoBuildNinjaIsFatalByDefault, "a directory with no build.ninja is FATAL by default") \
    X(ADirectoryWithNoBuildNinjaIsASkipOnlyWhenAsked,                                                \
      "a directory with no build.ninja is a reported SKIP only when it was ASKED for")               \
    X(ARealNinjaTreeIsRun, "a real ninja tree is RUN")                                               \
    X(MsvcAZeroRecordOfAUnitThatNamesNoHeaderAtAllIsExcused,                                         \
      "msvc: a zero record of a unit that names no header at all is excused")                        \
    X(MsvcAUnitHandedItsOnlyHeaderByThePchItsEdgeNamesIsExcused,                                     \
      "msvc: a unit handed its only header by the precompiled header its edge names is excused")     \
    X(MsvcAZeroRecordIsExcusedWhenThePchAndTheToolchainAnswerEveryInclude,                           \
      "msvc: a zero record is excused when the precompiled header and the toolchain answer every include") \
    X(MsvcAZeroRecordIsExcusedForTheToolchainsHeadersAlone,                                          \
      "msvc: a zero record is excused for the toolchain's headers alone, on an edge with no precompiled header") \
    X(MsvcTheAngleFormDoesNotLookBesideTheSource, "msvc: the angle form does not look beside the source") \
    X(MsvcAPchWhoseProductIsItsMakersOwnOutputIsReadLikeAPhonyOne,                                   \
      "msvc: a precompiled header whose product is its maker's own output is read like a phony one") \
    X(MsvcAHeaderOutsideTheProjectUnderADroppedPathIsExcused,                                        \
      "msvc: a header outside the project under a path the reader drops is excused, though the command line states its directory") \
    X(MsvcAProjectHeaderThePchDidNotReadIsALostRecord,                                               \
      "msvc: a header of the project the precompiled header did not read is a LOST record")          \
    X(MsvcAHeaderBesideTheSourceIsFoundThereAndLost,                                                 \
      "msvc: a header beside the source is found there, and LOST")                                   \
    X(MsvcAnIncludeWhoseTextNamesNoFileIsRefused,                                                    \
      "msvc: an #include whose text names no file is refused, not guessed")                          \
    X(MsvcTheSameSourceOnAnEdgeThatNamesNoPchIsLost,                                                 \
      "msvc: the same source on an edge that names NO precompiled header is LOST")                   \
    X(MsvcAHeaderThePchsOwnRecordDoesNotNameIsLost,                                                  \
      "msvc: a header the precompiled header's own record does not name is LOST")                    \
    X(MsvcAnEdgeThatMakesAPchNeverHasACorrectZeroRecord,                                             \
      "msvc: an edge that MAKES a precompiled header never has a correct zero record")               \
    X(MsvcAHeaderTheCommandLineForcesInIsAnIncludeOfTheUnit,                                         \
      "msvc: a header the command line forces in is an include of the unit, and LOST")               \
    X(MsvcAForcedHeaderIsThePchsOnlyWhereTheProductIsAnInputOfTheEdge,                               \
      "msvc: a forced header is the precompiled header's only where the product is an input of the edge") \
    X(MsvcAForcedHeaderOtherThanTheOneYuNamesIsNotThePchs,                                           \
      "msvc: a forced header other than the one `/Yu` names is not the precompiled header's")        \
    X(MsvcACommandLineThatHoldsAnAtFileIsRefusedByName,                                              \
      "msvc: a command line that holds an @file is refused by name")                                 \
    X(MsvcADirectiveBehindACommentIsStillAnInclude,                                                  \
      "msvc: a directive behind a comment is still an include, and LOST")                            \
    X(MsvcAQuotedNameSpeltWithABackslashIsTheSameHeaderOnEveryHost,                                  \
      "msvc: a quoted name spelt with a backslash is the same header on every host")                 \
    X(MsvcASourceThatCannotBeReadExcusesNothing, "msvc: a source that cannot be read excuses nothing") \
    X(MsvcAnObjectTheManifestCannotNameASourceForStillFails,                                         \
      "msvc: an object the manifest cannot name a source for still FAILS")                           \
    X(MsvcAHeaderTheTreesRecordsNameElsewhereIsLost,                                                 \
      "msvc: a header the tree's records name elsewhere is one the reader keeps, and LOST")          \
    X(MsvcAHeaderInADirectoryTheTreesRecordsNameIsLost,                                              \
      "msvc: a header in a directory the tree's records name is one the reader keeps, and LOST")     \
    X(MsvcAHeaderIsPlacedFromARecordDeeperInItsOwnIncludeRoot,                                       \
      "msvc: a header is placed from a record deeper in its own include root")                       \
    X(MsvcATreeWhoseRecordsNameADroppedPathExcusesNoHeaderUnderOne,                                  \
      "msvc: a tree whose records name a path the reader is taken to drop excuses no header under one") \
    X(MsvcAProjectHeaderUnderADroppedPathIsATreeThatCannotBeGuarded,                                 \
      "msvc: a header of the project under a path the reader drops is a tree that cannot be guarded there") \
    X(MsvcAHeaderOfTheSourceDirectoryTheCacheNamesIsTheProjectsToo,                                  \
      "msvc: a header of the source directory the cache names is the project's too")                 \
    X(MsvcATreeWhoseCacheNamesNoSourceDirectoryExcusesNoDroppedHeader,                               \
      "msvc: a tree whose cache names no source directory excuses no header under a path the reader drops") \
    X(MsvcAnObjectOfAGccRuleIsLostByItsOwnRule,                                                      \
      "msvc: an object of a `deps = gcc` rule is LOST by its own rule, beside the msvc ones")        \
    X(MsvcAnObjectOfARuleThatDeclaresNoFlavourIsLost,                                                \
      "msvc: an object of a rule that declares no flavour is LOST, said as that")                    \
    X(MsvcAnEdgeThatBindsItsOwnFlavourIsJudgedByIt,                                                  \
      "msvc: an edge that binds its own flavour is judged by it")                                    \
    X(TheManifestAFileItIncludesThatCannotBeReadIsNamed,                                             \
      "the manifest: a file it includes that cannot be read is named, and excuses nothing")          \
    X(TheManifestAnIncludeOneLevelFurtherDownIsNamed,                                                \
      "the manifest: an include one level further down is named, and excuses nothing")               \
    X(ABuildDirectoryNamedRelativeToTheCallerIsReadTheSame,                                          \
      "a build directory named relative to where the caller stands is read the same")                \
    X(CheckATreeWhoseEveryZeroRecordIsCorrectIsOk,                                                   \
      "check: a tree whose every zero record is the correct one is OK, each excusal said")           \
    X(CheckATreeWithNoZeroRecordIsOk,                                                                \
      "check: a tree with no zero record is OK, and nothing is said to be excused")                  \
    X(CheckOneLostRecordFailsTheTree, "check: one lost record FAILS the tree, said with what it lost") \
    X(CheckAZeroRecordOnARuleThatIsNotMsvcIsSaidWithItsRule,                                         \
      "check: a zero record on a rule that is not `deps = msvc` is said with its rule")              \
    X(CheckAManifestThatCannotBeReadWholeFailsEveryZeroRecord,                                       \
      "check: a manifest that cannot be read whole FAILS every zero record")                         \
    X(CheckNinjasOwnFailureIsFatal, "check: ninja's own failure is FATAL, said with what ninja said") \
    X(CheckAnAnswerThatNamesNoObjectIsFatal, "check: an answer that names no object is FATAL")       \
    X(CheckEveryLineItPrintsPassesTheRedactorOnEachOfItsPaths,                                       \
      "check: every line it prints passes the redactor, on each of its paths")                       \
    X(RecordOneRecordIsPrintedWholeThroughTheRedactor,                                               \
      "record: one record is printed whole, every line through the redactor")                        \
    X(RecordARecordThatHoldsNothingToMaskIsPrintedAsRecorded,                                        \
      "record: a record that holds nothing to mask is printed as it was recorded")                   \
    X(RecordAnEndThatNamesNoRecordOrTwoPrintsNothing,                                                \
      "record: an end that names no record, or two, prints nothing")                                 \
    X(RecordADirectoryOutsideTheProgramsTreeIsRefused,                                               \
      "record: a directory outside the program's tree is refused before anything is read")           \
    X(RecordTheRedactorIsTheSiblingActionsOwn,                                                       \
      "record: the redactor is the sibling action's own, and the tree's path is not printed")        \
    X(RecordNinjasOwnFailureIsFatal, "record: ninja's own failure is FATAL, and nothing is printed") \
    X(MainEveryDirectoryNamedIsCheckedAndTheWorstExitCodeIsThePrograms,                              \
      "main: every directory named is checked, and the worst exit code is the program's")            \
    X(TheSelfTestsTemporaryTreeIsRemoved, "the self-test's temporary tree is removed")               \
    X(MainRecordWrittenTwiceIsRefused, "main: `--record` written twice is refused")                  \
    X(MainRecordReadsOneDirectory, "main: `--record` reads ONE directory, named on the command line") \
    X(MainRecordTakesNoOtherOption, "main: `--record` takes no other option")                        \
    X(MainAnUnknownArgumentIsRefused, "main: an unknown argument is refused, not ignored")           \
    X(AnExplicitBuildTreeArgumentWinsOverTheDefault,                                                 \
      "an EXPLICIT build-tree argument wins over the default")                                       \
    X(ASelfTestThatStopsSaysWhereAndWhy,                                                             \
      "a self-test that stops says where and why, and the arms that ran keep their verdicts")        \
    X(ASelfTestThatStopsBeforeItsFirstArmSaysSoInItsLast,                                            \
      "a self-test that stops before its first arm says so in its last")                             \
    X(AFailingArmsDetailIsPrintedOnOneLineThroughTheRedactor,                                        \
      "a failing arm's detail is printed on ONE line, through the redactor")                         \
    X(DefaultBuildTreeTheRootArmsCouldBeRun, "default build tree: the root arms could be run")       \
    X(DefaultBuildTreeFromItsOwnTree,                                                                \
      "default build tree: root CONTROL: cwd = its own tree -> its own tree")                        \
    X(DefaultBuildTreeFromInsideAnotherRepository,                                                   \
      "default build tree: root: cwd inside ANOTHER git repository -> still its own tree")           \
    X(DefaultBuildTreeFromInsideNoRepository,                                                        \
      "default build tree: root: cwd inside NO git repository -> still its own tree")                \
    X(TheSelfTestRanToItsEnd, "the self-test ran to its end")

#define DSS_NINJA_DEPS_SELFTEST_ARM_NAME(caseName, armName) armName,
constexpr std::string_view kArmNames[] = {DSS_NINJA_DEPS_SELFTEST_ARMS(DSS_NINJA_DEPS_SELFTEST_ARM_NAME)};
#undef DSS_NINJA_DEPS_SELFTEST_ARM_NAME

constexpr dss::test_support::ScriptSelfTest kEntry{
    .command          = dss::ninja_deps_selftest::kCommand,
    .workingDirectory = dss::ninja_deps_selftest::kWorkingDirectory,
    .standardOutput   = dss::ninja_deps_selftest::kStandardOutput,
    .marker           = "ninja-deps self-test: ",
    .armNames         = kArmNames,
    .tableName        = "DSS_NINJA_DEPS_SELFTEST_ARMS",
};

// ONE run for the whole binary: the program builds its throwaway trees once, and every
// case reads the same output.
[[nodiscard]] dss::test_support::ScriptSelfTestRun const& theRun() {
    static dss::test_support::ScriptSelfTestRun const run = dss::test_support::runScriptSelfTest(kEntry);
    return run;
}

}  // namespace

#define DSS_NINJA_DEPS_SELFTEST_CASE(caseName, armName) \
    TEST(NinjaDepsSelfTest, caseName) { dss::test_support::expectTheScriptArmPassed(kEntry, theRun(), armName); }
DSS_NINJA_DEPS_SELFTEST_ARMS(DSS_NINJA_DEPS_SELFTEST_CASE)
#undef DSS_NINJA_DEPS_SELFTEST_CASE

// The program's arms and this file's cases are the same set: an arm added to the program
// without a case here is a red, by the arm's name.
TEST(NinjaDepsSelfTest, EveryArmTheSelfTestRanIsACaseAndEveryCaseAnArm) {
    dss::test_support::expectEveryScriptArmIsACaseAndEveryCaseAnArm(kEntry, theRun());
}

// The program exits non-zero exactly when an arm failed. A run that STOPS between two arms
// says so itself — the arm it stopped in fails with the cause, and so does its last arm —
// so what is left for this case is a program that dies outside its self-test altogether.
TEST(NinjaDepsSelfTest, TheSelfTestsExitCodeAgreesWithItsArms) {
    dss::test_support::expectTheScriptsExitCodeAgreesWithItsArms(kEntry, theRun());
}
