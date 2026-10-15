// THE `repo-guard` LABEL'S RULE, ONE CASE PER ARM — the ctest entry `harness/repo_guard_label_selftest`.
//
// WHAT THIS IS. The rule of the `repo-guard` label is stated where the label is given, in the top-level
// CMakeLists: an entry that SELF-TESTS a program never carries it, because a self-test's verdict reads
// its own fixtures and can depend on the host. `.harness-config/runner/actions/check-repo-guard-label/
// check-repo-guard-label.py` holds a configured build to it (`harness/repo_guard_label_check`, which reads
// that build's own `ctest --show-only=json-v1`). Run with `--self-test` it proves each refusal on
// synthetic listings, with no build, and prints one verdict line per arm.
//
// This binary RUNS that self-test — once, by the command line the top-level CMakeLists composed at
// configure — and reads each arm's verdict into a case of its own. IT RE-IMPLEMENTS NO ARM. Everything
// it does beyond naming the arms is tests/test_support/script_selftest.hpp.
// ⓘ No `repo-guard` label: it is a self-test, the very kind the rule keeps on every leg.

#include "dss_repo_guard_label_selftest_command.hpp"

#include "script_selftest.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace {

// Every arm of the self-test: X(<the case>, "<the arm's name, exactly as the program prints it>").
// The order is the program's. GENERATED from the program's own verdict lines; an arm added to the
// program without a row here is a red of the set case below, by the arm's name.
#define DSS_REPO_GUARD_LABEL_SELFTEST_ARMS(X)                                                                        \
    X(ASelfTestFlagOnTheCommandLineMakesAnEntryASelfTestWhateverItsNameRefusedWithTheLabel,                            \
      "a self-test flag on the command line makes an entry a self-test, whatever its name: refused with the label")    \
    X(SelftestAndEitherFlagFollowedByWordAreSelfTestFlagsToo,                                                          \
      "--selftest, and either flag followed by -<word>, are self-test flags too")                                      \
    X(ANameHoldingSelftestMakesADriverEntryASelfTestRefusedWithTheLabel,                                               \
      "a name holding selftest makes a driver entry a self-test: refused with the label")                              \
    X(ATreeCheckKeepsTheLabelAWordThatOnlyBeginsLikeAFlagIsNoneTheProgramSOwnPathIsNotReadAndA,                        \
      "a tree check keeps the label, a word that only begins like a flag is none, the program's own path is not read, and a self-test without the label passes (the control)") \
    X(AListingThatIsNoListingTooShortWithoutTheLabelOrWithoutASelfTestIsRefusedNeverJudged,                            \
      "a listing that is no listing, too short, without the label or without a self-test is refused, never judged")    \
    X(TheBuildSListingIsReadThroughCtestAndJudgedALabelledSelfTestIsAFailExit1NamingIt,                                \
      "the build's listing is read through ctest and judged: a labelled self-test is a FAIL (exit 1) naming it")       \
    X(CtestFailingOrAListingThatIsNotJsonCannotBeJudgedExit2NeverAPass,                                                \
      "ctest failing, or a listing that is not JSON, cannot be judged (exit 2), never a pass")                         \
    X(ABuildWhoseSelfTestsCarryNoLabelIsOkExit0AndSaysWhatItCountedTheControl,                                         \
      "a build whose self-tests carry no label is OK (exit 0) and says what it counted (the control)")

#define DSS_REPO_GUARD_LABEL_SELFTEST_ARM_NAME(caseName, armName) armName,
constexpr std::string_view kArmNames[] = {DSS_REPO_GUARD_LABEL_SELFTEST_ARMS(DSS_REPO_GUARD_LABEL_SELFTEST_ARM_NAME)};
#undef DSS_REPO_GUARD_LABEL_SELFTEST_ARM_NAME

constexpr dss::test_support::ScriptSelfTest kEntry{
    .command          = dss::repo_guard_label_selftest::kCommand,
    .workingDirectory = dss::repo_guard_label_selftest::kWorkingDirectory,
    .standardOutput   = dss::repo_guard_label_selftest::kStandardOutput,
    .marker           = "check-repo-guard-label self-test: ",
    .armNames         = kArmNames,
    .tableName        = "DSS_REPO_GUARD_LABEL_SELFTEST_ARMS",
};

// ONE run for the whole binary: every case reads the same output.
[[nodiscard]] dss::test_support::ScriptSelfTestRun const& theRun() {
    static dss::test_support::ScriptSelfTestRun const run = dss::test_support::runScriptSelfTest(kEntry);
    return run;
}

}  // namespace

#define DSS_REPO_GUARD_LABEL_SELFTEST_CASE(caseName, armName) \
    TEST(RepoGuardLabelSelfTest, caseName) { dss::test_support::expectTheScriptArmPassed(kEntry, theRun(), armName); }
DSS_REPO_GUARD_LABEL_SELFTEST_ARMS(DSS_REPO_GUARD_LABEL_SELFTEST_CASE)
#undef DSS_REPO_GUARD_LABEL_SELFTEST_CASE

// The program's arms and this file's cases are the same set: an arm added to the program without a
// case here is a red, by the arm's name.
TEST(RepoGuardLabelSelfTest, EveryArmTheProgramRanIsACaseAndEveryCaseAnArm) {
    dss::test_support::expectEveryScriptArmIsACaseAndEveryCaseAnArm(kEntry, theRun());
}

// The program exits non-zero exactly when an arm said FAILED; a traceback between two arms, or an arm
// count other than its own ratchet, is an exit code no arm's case reports, and this one does.
TEST(RepoGuardLabelSelfTest, TheProgramsExitCodeAgreesWithItsArms) {
    dss::test_support::expectTheScriptsExitCodeAgreesWithItsArms(kEntry, theRun());
}
