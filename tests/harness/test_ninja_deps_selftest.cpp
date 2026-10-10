// THE NINJA DEPENDENCY-RECORD CHECK'S OWN SELF-TEST, ONE CASE PER ARM — the ctest entry
// `ninja_deps_selftest_guard`.
//
// WHAT THIS IS. `.harness-config/runner/actions/check-ninja-deps/check-ninja-deps.py`
// refuses a gate over a build directory whose objects recorded no header dependencies,
// and run with `--self-test` it proves its own rules on text and on throwaway trees: what
// counts as a zero record, when a named directory is checked, skipped or refused, and —
// on a tree whose rules are `deps = msvc` — which zero record is the CORRECT record of
// its unit and which is a lost one. Each of those proofs is an ARM, and the program
// prints one verdict line per arm.
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
    X(AMissingDirectoryIsFatalNeverASkip, "a MISSING directory is FATAL, never a skip")              \
    X(AllowNonNinjaDoesNotExcuseAMissingDirectory,                                                   \
      "--allow-non-ninja does NOT excuse a missing directory")                                       \
    X(ADirectoryWithNoBuildNinjaIsFatalByDefault, "a directory with no build.ninja is FATAL by default") \
    X(ADirectoryWithNoBuildNinjaIsASkipOnlyWhenAsked,                                                \
      "a directory with no build.ninja is a reported SKIP only when it was ASKED for")               \
    X(ARealNinjaTreeIsRun, "a real ninja tree is RUN")                                               \
    X(MsvcAZeroRecordOfAUnitWithNoIncludeIsExcused,                                                  \
      "msvc: a zero record of a unit with NO #include is excused")                                   \
    X(MsvcAZeroRecordIsExcusedWhenThePchAndTheToolchainAnswerEveryInclude,                           \
      "msvc: a zero record is excused when the precompiled header and the toolchain answer every include") \
    X(MsvcAZeroRecordIsExcusedForTheToolchainsHeadersAlone,                                          \
      "msvc: a zero record is excused for the toolchain's headers alone, on an edge with no precompiled header") \
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
    X(MsvcACompilerOutsideThePathsNinjaDropsExcusesNoToolchainHeader,                                \
      "msvc: a compiler outside the paths ninja's reader drops excuses no toolchain header")         \
    X(MsvcATreeWhoseCacheCannotNameItsCompilerExcusesNoToolchainHeader,                              \
      "msvc: a tree whose cache cannot name its compiler excuses no toolchain header")               \
    X(MsvcATreeWhoseRecordsNameAPathTheReaderDropsExcusesNoToolchainHeader,                          \
      "msvc: a tree whose records name a path the reader is taken to drop excuses no toolchain header") \
    X(MsvcAnObjectTheManifestCannotNameASourceForStillFails,                                         \
      "msvc: an object the manifest cannot name a source for still FAILS")                           \
    X(MsvcOnATreeThatAlsoDeclaresGccNoZeroRecordIsExcused,                                           \
      "msvc: on a tree that also declares deps = gcc, no zero record is excused")                    \
    X(AGccManifestIsNotReadAsDeclaringMsvc, "a gcc manifest is not read as declaring deps = msvc")   \
    X(AManifestOfBothFlavoursIsReadAsBoth, "a manifest of both flavours is read as both")            \
    X(TheSelfTestsTemporaryTreeIsRemoved, "the self-test's temporary tree is removed")               \
    X(DefaultBuildTreeFromItsOwnTree,                                                                \
      "default build tree: root CONTROL: cwd = its own tree -> its own tree")                        \
    X(DefaultBuildTreeFromInsideAnotherRepository,                                                   \
      "default build tree: root: cwd inside ANOTHER git repository -> still its own tree")           \
    X(DefaultBuildTreeFromInsideNoRepository,                                                        \
      "default build tree: root: cwd inside NO git repository -> still its own tree")                \
    X(AnExplicitBuildTreeArgumentWinsOverTheDefault,                                                 \
      "an EXPLICIT build-tree argument wins over the default")

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

// The program exits non-zero exactly when an arm failed; a traceback between two arms is
// an exit code no arm's case reports, and this one does.
TEST(NinjaDepsSelfTest, TheSelfTestsExitCodeAgreesWithItsArms) {
    dss::test_support::expectTheScriptsExitCodeAgreesWithItsArms(kEntry, theRun());
}
