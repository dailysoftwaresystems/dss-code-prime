// THE MUTATION ARMS REGISTRY CHECK'S OWN SELF-TEST, ONE CASE PER ARM — the ctest entry
// `harness/arms_registry_selftest`.
//
// WHAT THIS IS. `.harness-config/runner/actions/check-arms-registry/check-arms-registry.py` checks what the mutation
// arms registry (tests/mutations/arms.registry) promises that `dssharness check-mutations` does not check itself:
// every A and M row's before-text stands exactly once in its site, each area's section is in order, the arms of one
// runner on one leg state one case count, and -- given a sweep's JSON -- each driven arm's reds against its C and G
// rows and its diagnostic in its run log. Run with `--self-test` it proves each check against a synthesized
// negative, beside a control that must stay clean, and prints one verdict line per arm.
//
// This binary RUNS that self-test — once, by the command line the top-level CMakeLists composed at
// configure — and reads each arm's verdict into a case of its own. IT RE-IMPLEMENTS NO ARM. Everything
// it does beyond naming the arms is tests/test_support/script_selftest.hpp. The tree itself is judged by the
// entry `arms_registry_guard`.
// ⓘ No label: it is a self-test, its fixtures are synthesized in a temporary directory, and every leg runs it.

#include "dss_arms_registry_selftest_command.hpp"

#include "script_selftest.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace {

// Every arm of the self-test: X(<the case>, "<the arm's name, exactly as the program prints it>").
// The order is the program's. GENERATED from the program's own verdict lines; an arm added to the
// program without a row here is a red of the set case below, by the arm's name.
#define DSS_ARMS_REGISTRY_SELFTEST_ARMS(X)                                                                             \
    X(R1TheControlRegistryStandingInOrderOneCountPerRunnerAndLegTheStandCheckFindsNothing,                             \
      "R1 the control registry, standing, in order, one count per runner and leg: the stand check finds nothing")      \
    X(R2TheControlRegistryTheOrderCheckFindsNothing, "R2 the control registry: the order check finds nothing")         \
    X(R3TheControlRegistryTheCasesCheckFindsNothing, "R3 the control registry: the cases check finds nothing")         \
    X(S1ABeforeTextTheSiteDoesNotHoldIsRefused, "S1 a before-text the site does not hold is refused")                 \
    X(S2ABeforeTextTheSiteHoldsTwiceIsRefused, "S2 a before-text the site holds twice is refused")                     \
    X(S3AnAfterTextThatIsTheBeforeTextIsRefusedTheMutantChangesNothing,                                                \
      "S3 an after-text that is the before-text is refused: the mutant changes nothing")                               \
    X(S4ACrlfSiteStillReadsAnLfTextOnce, "S4 a CRLF site still reads an LF text once")                                 \
    X(O1AnArmOutOfOrderInItsSectionIsRefused, "O1 an arm out of order in its section is refused")                      \
    X(O2AnArmInASectionAlreadyClosedIsRefused, "O2 an arm in a section already closed is refused")                     \
    X(O3SectionsOutOfAlphabeticalOrderAreRefused, "O3 sections out of alphabetical order are refused")                 \
    X(C1TwoCaseCountsForOneRunnerOnOneLegAreRefused, "C1 two case counts for one runner on one leg are refused")       \
    X(C2TwoCaseCountsForOneRunnerOnTwoLegsAreNotCompared,                                                              \
      "C2 two case counts for one runner on two legs are not compared")                                                \
    X(W1TheSweepControlTheDeclaredRedWithItsDiagnosticSaidIsClean,                                                     \
      "W1 the sweep control, the declared red with its diagnostic said, is clean")                                     \
    X(W2ARedNoCRowDeclaresIsReported, "W2 a red no C row declares is reported")                                        \
    X(W3ADeclaredRedThatDidNotRedIsReported, "W3 a declared red that did not red is reported")                         \
    X(W4AGRowSCaseRedIsReported, "W4 a G row's case red is reported")                                                  \
    X(W5TheDiagnosticNotInTheArmSRunIsReported, "W5 the diagnostic not in the arm's run is reported")

#define DSS_ARMS_REGISTRY_SELFTEST_ARM_NAME(caseName, armName) armName,
constexpr std::string_view kArmNames[] = {DSS_ARMS_REGISTRY_SELFTEST_ARMS(DSS_ARMS_REGISTRY_SELFTEST_ARM_NAME)};
#undef DSS_ARMS_REGISTRY_SELFTEST_ARM_NAME

constexpr dss::test_support::ScriptSelfTest kEntry{
    .command          = dss::arms_registry_selftest::kCommand,
    .workingDirectory = dss::arms_registry_selftest::kWorkingDirectory,
    .standardOutput   = dss::arms_registry_selftest::kStandardOutput,
    .marker           = "check-arms-registry self-test: ",
    .armNames         = kArmNames,
    .tableName        = "DSS_ARMS_REGISTRY_SELFTEST_ARMS",
};

// ONE run for the whole binary: every case reads the same output.
[[nodiscard]] dss::test_support::ScriptSelfTestRun const& theRun() {
    static dss::test_support::ScriptSelfTestRun const run = dss::test_support::runScriptSelfTest(kEntry);
    return run;
}

}  // namespace

#define DSS_ARMS_REGISTRY_SELFTEST_CASE(caseName, armName) \
    TEST(ArmsRegistrySelfTest, caseName) { dss::test_support::expectTheScriptArmPassed(kEntry, theRun(), armName); }
DSS_ARMS_REGISTRY_SELFTEST_ARMS(DSS_ARMS_REGISTRY_SELFTEST_CASE)
#undef DSS_ARMS_REGISTRY_SELFTEST_CASE

// The program's arms and this file's cases are the same set: an arm added to the program without a
// case here is a red, by the arm's name.
TEST(ArmsRegistrySelfTest, EveryArmTheProgramRanIsACaseAndEveryCaseAnArm) {
    dss::test_support::expectEveryScriptArmIsACaseAndEveryCaseAnArm(kEntry, theRun());
}

// The program exits non-zero exactly when an arm said FAILED; a traceback between two arms, or an arm
// count other than its own ratchet, is an exit code no arm's case reports, and this one does.
TEST(ArmsRegistrySelfTest, TheProgramsExitCodeAgreesWithItsArms) {
    dss::test_support::expectTheScriptsExitCodeAgreesWithItsArms(kEntry, theRun());
}
