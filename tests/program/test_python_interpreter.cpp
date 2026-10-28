// THE BUILD'S PYTHON CHOICE, ITS OWN SELF-TEST, ONE CASE PER ARM -- the ctest entry `build/python_interpreter`.
//
// WHAT THIS IS. `cmake/DssPython.cmake` chooses the ONE interpreter every ctest entry that runs a Python
// program starts: the first on the leg's PATH at or above the floor `.harness-config/config.json` states, and
// never an App Execution Alias (on Windows, `%LOCALAPPDATA%\Microsoft\WindowsApps\python3.exe`: a reparse
// point the process loader resolves and a spawner that asks the file system finds no program at). Run with
// `-DDSS_PYTHON_MODE=selftest` it configures fixture projects that include it and proves that choice: on a
// clean configure (and the order it was told to search in: the PATH first), on a floor no interpreter meets, and
// -- on a host that has an alias directory -- on a cache
// that ALREADY names an alias (✔MEASURED 2026-10-20: a build configured before the rule held
// `_Python3_EXECUTABLE` = the alias, and seven self-test entries went red on windows-x86_64-release, each "was
// not found as an executable"), on an alias named by hand, and on one named in another case. Each proof is an ARM,
// and the script prints one
// verdict line per arm.
//
// This binary RUNS that self-test -- once, by the command line the top-level CMakeLists composed at configure
// -- and reads each arm's verdict into a case of its own. IT RE-IMPLEMENTS NO ARM: an arm's logic, its
// fixtures and its message live in the script (tests/test_support/script_selftest.hpp says why and how).
//
// ★ THE ALIAS ARMS EXIST WHERE ALIASES DO. The script runs them on a host with an alias directory -- Windows
// -- and this table names them in a build for that host, so the two tables are the same set on every host
// and the totality case below holds them to it.
//
// ★ IT FAILS, IT NEVER SKIPS: a command line that cannot be started, or output that cannot be read back, is
// every case's failure, with the reason.

#include "dss_python_interpreter_selftest_command.hpp"

#include "script_selftest.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace {

// Every arm of the self-test: X(<the case>, "<the arm's name, exactly as the script hands it to
// `_dss_py_t_arm`>"). The order is the script's.
#define DSS_PYTHON_SELFTEST_EVERY_HOST_ARMS(X)                                                              \
    X(ACleanConfigureChoosesAnInterpreterAtOrAboveTheFloor,                                                 \
      "configure: a clean configure chooses an interpreter at or above the floor config.json states")      \
    X(TheSearchAsksThePathBeforeTheRegistryAndTheFrameworks,                                                \
      "configure: the search asks the leg's PATH before the Windows registry and the macOS frameworks")    \
    X(AFloorNoInterpreterMeetsIsRefusedByTheFloorItNames,                                                   \
      "configure: a floor no interpreter meets is refused at configure, by the floor it names")
#ifdef _WIN32
#define DSS_PYTHON_SELFTEST_ALIAS_ARMS(X)                                                                   \
    X(ACacheNamingAnAppExecutionAliasIsSearchedAgain,                                                       \
      "configure: a cache naming an App Execution Alias is searched again, and the interpreter chosen lies " \
      "outside the alias directory")                                                                        \
    X(AnInterpreterNamedByHandInsideTheAliasDirectoryIsRefusedByName,                                       \
      "configure: an interpreter named by hand inside the App Execution Alias directory is refused by name") \
    X(AnInterpreterNamedInsideTheAliasDirectoryInAnotherCaseIsRefusedByName,                                \
      "configure: an interpreter named inside the App Execution Alias directory in another case is refused " \
      "by name")
#else
#define DSS_PYTHON_SELFTEST_ALIAS_ARMS(X)
#endif
#define DSS_PYTHON_SELFTEST_ARMS(X)       \
    DSS_PYTHON_SELFTEST_EVERY_HOST_ARMS(X) \
    DSS_PYTHON_SELFTEST_ALIAS_ARMS(X)

#define DSS_PYTHON_SELFTEST_ARM_NAME(caseName, armName) armName,
constexpr std::string_view kArmNames[] = {DSS_PYTHON_SELFTEST_ARMS(DSS_PYTHON_SELFTEST_ARM_NAME)};
#undef DSS_PYTHON_SELFTEST_ARM_NAME

// The entry, as the shared driver reads it. The marker is what the script prints before each verdict:
// `message(STATUS ...)` puts `-- ` in front of it, which is not matched, so the line is found wherever the
// marker stands.
constexpr dss::test_support::ScriptSelfTest kEntry{
    .command          = dss::python_interpreter_selftest::kCommand,
    .workingDirectory = dss::python_interpreter_selftest::kWorkingDirectory,
    .standardOutput   = dss::python_interpreter_selftest::kStandardOutput,
    .marker           = "dss-python selftest: ",
    .armNames         = kArmNames,
    .tableName        = "DSS_PYTHON_SELFTEST_ARMS",
};

// ONE run for the whole binary: the self-test configures its fixtures once, and every case reads the same
// output.
[[nodiscard]] dss::test_support::ScriptSelfTestRun const& theRun() {
    static dss::test_support::ScriptSelfTestRun const run = dss::test_support::runScriptSelfTest(kEntry);
    return run;
}

}  // namespace

#define DSS_PYTHON_SELFTEST_CASE(caseName, armName) \
    TEST(PythonInterpreter, caseName) { dss::test_support::expectTheScriptArmPassed(kEntry, theRun(), armName); }
DSS_PYTHON_SELFTEST_ARMS(DSS_PYTHON_SELFTEST_CASE)
#undef DSS_PYTHON_SELFTEST_CASE

// The script's arms and this file's cases are the same set: an arm added to the script without a case here
// is a red, by the arm's name.
TEST(PythonInterpreter, EveryArmTheSelfTestRanIsACaseAndEveryCaseAnArm) {
    dss::test_support::expectEveryScriptArmIsACaseAndEveryCaseAnArm(kEntry, theRun());
}

// The script also fails on an arm COUNT other than its own ratchet, with every arm green -- which no arm's
// case could notice, and this one does.
TEST(PythonInterpreter, TheSelfTestsExitCodeAgreesWithItsArms) {
    dss::test_support::expectTheScriptsExitCodeAgreesWithItsArms(kEntry, theRun());
}
