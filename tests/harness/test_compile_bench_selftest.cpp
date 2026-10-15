// THE COMPILER BENCH'S COMMAND LINE, ONE CASE PER ARM — the ctest entry `harness/compile_bench_selftest`.
//
// WHAT THIS IS. `.harness-config/runner/actions/compile-bench/compile-bench.py` times a RELEASE dsscp
// against the host's reference compilers, and writes where its command line says (`--out`, `--json`).
// Run with `--self-test` it proves, with no compiler and no build, that a path to write which begins
// with `-` — an option taken for a value, `--json --help` — is refused naming its flag in both value
// spellings, as `owning-tree.py`'s `written_path` states for every program; and prints one verdict line
// per arm.
//
// This binary RUNS that self-test — once, by the command line the top-level CMakeLists composed at
// configure — and reads each arm's verdict into a case of its own. IT RE-IMPLEMENTS NO ARM. Everything
// it does beyond naming the arms is tests/test_support/script_selftest.hpp.
// ⓘ No `repo-guard` label: it reads no source tree, and costs nothing on any leg.

#include "dss_compile_bench_selftest_command.hpp"

#include "script_selftest.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace {

// Every arm of the self-test: X(<the case>, "<the arm's name, exactly as the program prints it>").
// The order is the program's. GENERATED from the program's own verdict lines; an arm added to the
// program without a row here is a red of the set case below, by the arm's name.
#define DSS_COMPILE_BENCH_SELFTEST_ARMS(X)                                                                           \
    X(OutHelpIsAnOptionTakenForTheDirectoryToWriteRefusedNamingOut,                                                    \
      "--out --help is an option taken for the directory to write: refused, naming --out")                             \
    X(JsonHelpIsAnOptionTakenForTheFileToWriteRefusedNamingJson,                                                       \
      "--json --help is an option taken for the file to write: refused, naming --json")                                \
    X(OutXSpellsTheSameMistakeInlineRefusedNamingOut,                                                                  \
      "--out=-x spells the same mistake inline: refused, naming --out")                                                \
    X(JsonHelpSpellsTheSameMistakeInlineRefusedNamingJson,                                                             \
      "--json=--help spells the same mistake inline: refused, naming --json")                                          \
    X(APathHoldingADashIsKeptAndAnEmptyValueIsTheDefaultTheControl,                                                    \
      "a path holding a dash is kept, and an empty value is the default (the control)")

#define DSS_COMPILE_BENCH_SELFTEST_ARM_NAME(caseName, armName) armName,
constexpr std::string_view kArmNames[] = {DSS_COMPILE_BENCH_SELFTEST_ARMS(DSS_COMPILE_BENCH_SELFTEST_ARM_NAME)};
#undef DSS_COMPILE_BENCH_SELFTEST_ARM_NAME

constexpr dss::test_support::ScriptSelfTest kEntry{
    .command          = dss::compile_bench_selftest::kCommand,
    .workingDirectory = dss::compile_bench_selftest::kWorkingDirectory,
    .standardOutput   = dss::compile_bench_selftest::kStandardOutput,
    .marker           = "compile-bench self-test: ",
    .armNames         = kArmNames,
    .tableName        = "DSS_COMPILE_BENCH_SELFTEST_ARMS",
};

// ONE run for the whole binary: every case reads the same output.
[[nodiscard]] dss::test_support::ScriptSelfTestRun const& theRun() {
    static dss::test_support::ScriptSelfTestRun const run = dss::test_support::runScriptSelfTest(kEntry);
    return run;
}

}  // namespace

#define DSS_COMPILE_BENCH_SELFTEST_CASE(caseName, armName) \
    TEST(CompileBenchSelfTest, caseName) { dss::test_support::expectTheScriptArmPassed(kEntry, theRun(), armName); }
DSS_COMPILE_BENCH_SELFTEST_ARMS(DSS_COMPILE_BENCH_SELFTEST_CASE)
#undef DSS_COMPILE_BENCH_SELFTEST_CASE

// The program's arms and this file's cases are the same set: an arm added to the program without a
// case here is a red, by the arm's name.
TEST(CompileBenchSelfTest, EveryArmTheProgramRanIsACaseAndEveryCaseAnArm) {
    dss::test_support::expectEveryScriptArmIsACaseAndEveryCaseAnArm(kEntry, theRun());
}

// The program exits non-zero exactly when an arm said FAILED; a traceback between two arms, or an arm
// count other than its own ratchet, is an exit code no arm's case reports, and this one does.
TEST(CompileBenchSelfTest, TheProgramsExitCodeAgreesWithItsArms) {
    dss::test_support::expectTheScriptsExitCodeAgreesWithItsArms(kEntry, theRun());
}
