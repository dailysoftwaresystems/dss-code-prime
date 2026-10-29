// THE COMPILE PROFILER'S COMMAND LINE, ONE CASE PER ARM — the ctest entry
// `harness/profile_compile_selftest`.
//
// WHAT THIS IS. `.harness-config/runner/actions/profile-compile/profile-compile.py` profiles one fixed
// kit with a RELEASE dsscp, building into and writing where its command line says (`--build-dir`,
// `--out`). Run with `--self-test` it proves, with no kit and no build, that a path to write which
// begins with `-` — an option taken for a value, `--out --help` — is refused naming its flag, as
// `owning-tree.py`'s `written_path` states for every program, while a value that is no path may begin
// with one; and prints one verdict line per arm.
//
// This binary RUNS that self-test — once, by the command line the top-level CMakeLists composed at
// configure — and reads each arm's verdict into a case of its own. IT RE-IMPLEMENTS NO ARM. Everything
// it does beyond naming the arms is tests/test_support/script_selftest.hpp.
// ⓘ No `repo-guard` label: it reads no source tree, and costs nothing on any leg.

#include "dss_profile_compile_selftest_command.hpp"

#include "script_selftest.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace {

// Every arm of the self-test: X(<the case>, "<the arm's name, exactly as the program prints it>").
// The order is the program's. GENERATED from the program's own verdict lines; an arm added to the
// program without a row here is a red of the set case below, by the arm's name.
#define DSS_PROFILE_COMPILE_SELFTEST_ARMS(X)                                                                         \
    X(OutHelpIsAnOptionTakenForTheDirectoryToWriteRefusedNamingOut,                                                    \
      "--out --help is an option taken for the directory to write: refused, naming --out")                             \
    X(BuildDirXIsAnOptionTakenForTheBuildTreeToWriteRefusedNamingBuildDir,                                             \
      "--build-dir -x is an option taken for the build tree to write: refused, naming --build-dir")                    \
    X(OutHelpIsNoSpellingOfAValueHereRefusedAsAnUnknownArgument,                                                       \
      "--out=--help is no spelling of a value here: refused as an unknown argument")                                   \
    X(APathHoldingADashIsKeptTheControl, "a path holding a dash is kept (the control)")                                \
    X(AValueThatIsNoPathMayBeginWithTheControl, "a value that is no path may begin with `-` (the control)")

#define DSS_PROFILE_COMPILE_SELFTEST_ARM_NAME(caseName, armName) armName,
constexpr std::string_view kArmNames[] = {DSS_PROFILE_COMPILE_SELFTEST_ARMS(DSS_PROFILE_COMPILE_SELFTEST_ARM_NAME)};
#undef DSS_PROFILE_COMPILE_SELFTEST_ARM_NAME

constexpr dss::test_support::ScriptSelfTest kEntry{
    .command          = dss::profile_compile_selftest::kCommand,
    .workingDirectory = dss::profile_compile_selftest::kWorkingDirectory,
    .standardOutput   = dss::profile_compile_selftest::kStandardOutput,
    .marker           = "profile-compile self-test: ",
    .armNames         = kArmNames,
    .tableName        = "DSS_PROFILE_COMPILE_SELFTEST_ARMS",
};

// ONE run for the whole binary: every case reads the same output.
[[nodiscard]] dss::test_support::ScriptSelfTestRun const& theRun() {
    static dss::test_support::ScriptSelfTestRun const run = dss::test_support::runScriptSelfTest(kEntry);
    return run;
}

}  // namespace

#define DSS_PROFILE_COMPILE_SELFTEST_CASE(caseName, armName) \
    TEST(ProfileCompileSelfTest, caseName) { dss::test_support::expectTheScriptArmPassed(kEntry, theRun(), armName); }
DSS_PROFILE_COMPILE_SELFTEST_ARMS(DSS_PROFILE_COMPILE_SELFTEST_CASE)
#undef DSS_PROFILE_COMPILE_SELFTEST_CASE

// The program's arms and this file's cases are the same set: an arm added to the program without a
// case here is a red, by the arm's name.
TEST(ProfileCompileSelfTest, EveryArmTheProgramRanIsACaseAndEveryCaseAnArm) {
    dss::test_support::expectEveryScriptArmIsACaseAndEveryCaseAnArm(kEntry, theRun());
}

// The program exits non-zero exactly when an arm said FAILED; a traceback between two arms, or an arm
// count other than its own ratchet, is an exit code no arm's case reports, and this one does.
TEST(ProfileCompileSelfTest, TheProgramsExitCodeAgreesWithItsArms) {
    dss::test_support::expectTheScriptsExitCodeAgreesWithItsArms(kEntry, theRun());
}
