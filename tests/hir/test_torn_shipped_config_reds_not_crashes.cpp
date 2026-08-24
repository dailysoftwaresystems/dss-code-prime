// A TORN SHIPPED CONFIG MUST RED A SUITE, NOT KILL IT.
//
// D-TEST-A-TORN-SHIPPED-CONFIG-CRASHES-A-SUITE-INSTEAD-OF-REDDING-IT
//
// ★★ WHY THIS IS A SUBPROCESS PIN AND CANNOT BE ANYTHING ELSE. The property is
// about how a test BINARY terminates — exit 1 with GoogleTest's own report,
// versus `0xC0000409` (STATUS_STACK_BUFFER_OVERRUN, what Windows reports for the
// `__fastfail` UCRT's `abort()` raises) with no reporter run at all. A process
// cannot observe its own abort, so the subject has to be a child.
//
// ✔MEASURED 2026-08-24, BEFORE the repair, with each subject pointed at a
// private config root whose `sources/c.lang.json` had been emptied:
//
//     dss_tokenizer_test_tokenizer.exe    rc=-1073740791  hex=0xC0000409
//     dss_hir_test_hir_lowering_c.exe     rc=-1073740791  hex=0xC0000409
//
// and AFTER it, from the same private root:
//
//     tokenizer  rc=1   131 cases ran, 61 passed, every failure NAMED
//     hir        rc=1   287 cases ran, each failure NAMED
//
// ★ THE CONFIG IS EMPTIED, NEVER DELETED, and the difference decides whether
// this test proves anything (✔MEASURED, P31 lane H). `$DSS_CONFIG_ROOT` is a
// PREFERENCE, not a pin: a set-but-MISS falls THROUGH to `findShippedConfig`'s
// cwd ancestor walk, which from a build tree reaches the REAL `src/dss-config`
// and rescues the child. Both arms then pass and the pin asserts nothing. An
// EMPTY file is a HIT, so the override is honoured and the load fails for the
// reason this test is about.
//
// ⇒ Which is also why the child's own output is required to NAME THE SCRATCH
// PATH. Without that clause every claim here could be false while the test still
// passed, because a child that quietly read the repo's real tree would look
// identical from the outside. (The clause is lifted from
// `tests/test_support/private_config_root.hpp`, which learned it the same way.)
//
// ⚠ IT WRITES ONLY INSIDE ITS OWN `ScratchDir`. The live `src/dss-config/` is
// shared by every concurrent workstream in this tree; a pin that tore the real
// document would red every other suite in the run — the very failure class the
// row beside this one was raised on.

#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scoped_env.hpp"
#include "scratch_dir.hpp"
#include "test_wait_budget.hpp"   // kRunBudget — the ONE measured spawn budget

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

// The code Windows reports when a process calls `__fastfail`, which is where
// UCRT's `abort()` ends up. Named, because "the test failed" and "the test
// binary was executed by the OS" are different verdicts and only one of them
// tells the operator that the INPUT was bad.
constexpr std::uint32_t kFastFail = 0xC0000409u;

[[nodiscard]] bool contains(std::string const& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

// A private config tree with `sources/c.lang.json` EMPTIED. Returns the
// TREE ROOT (the directory that CONTAINS `src/dss-config`), which is what
// `$DSS_CONFIG_ROOT` means.
[[nodiscard]] fs::path stageTornConfigRoot(dss::test_support::ScratchDir const& dir) {
    fs::path const treeRoot = dir.path() / "torn-config-root";
    fs::path const dst      = treeRoot / "src" / "dss-config";
    fs::create_directories(dst.parent_path());

    std::error_code ec;
    fs::copy(dss::test::configRoot(), dst, fs::copy_options::recursive, ec);
    if (ec) {
        throw std::runtime_error("could not stage a private config tree at "
                                 + dst.generic_string() + ": " + ec.message());
    }

    fs::path const doc = dst / "sources" / "c.lang.json";
    if (!fs::is_regular_file(doc)) {
        throw std::runtime_error("staged tree has no " + doc.generic_string()
                                 + " — the copy did not land where the loader "
                                   "looks, so emptying it would prove nothing");
    }
    // Truncate in place. The file must still EXIST — see the header.
    { std::ofstream truncate(doc, std::ios::binary | std::ios::trunc); }
    if (fs::file_size(doc) != 0) {
        throw std::runtime_error("failed to empty " + doc.generic_string());
    }
    return treeRoot;
}

// Run `subject` against the torn tree and assert it REPORTS rather than DIES.
void expectRedNotCrash(fs::path const& subject, std::string_view label) {
    dss::test_support::ScratchDir dir{dss::test_support::Location::Temp,
                                      "torn-shipped-config"};
    fs::path const treeRoot = stageTornConfigRoot(dir);

    dss::test_support::ScopedEnv const configRoot{"DSS_CONFIG_ROOT",
                                                  treeRoot.generic_string()};

    // ★ `kRunBudget`, the suite's ONE measured "how long may the compiled
    // program take to terminate" budget -- never a literal here. A wall-clock
    // number written into a test is sized on the machine that wrote it and reds
    // on the slowest leg that runs it, NAMING THE WRONG EVENT
    // (D-TEST-A-NEW-WALL-CLOCK-LITERAL-IN-A-TEST-IS-UNGUARDED). ✔MEASURED on
    // this host with the config torn: tokenizer 208 ms, hir 33 ms -- every case
    // fails at its first load, so the subjects are FAST in this arm. A timeout
    // here therefore means the staging did not take and the child read the real
    // tree (where the hir suite runs ~30 s), which `timedOut` reports as
    // exactly that rather than as a wrong verdict.
    auto const r = dss::test_support::runBinary(
        subject, dss::test_support::kRunBudget, /*captureStdout=*/true,
        /*launcherPrefix=*/{}, /*programArgs=*/{"--gtest_brief=1"});

    ASSERT_TRUE(r.spawned) << label << ": " << r.diagnostic;
    ASSERT_FALSE(r.timedOut) << label << ": " << r.diagnostic;

    // ── THE CLAIM ────────────────────────────────────────────────────────────
    EXPECT_NE(r.exitCode, kFastFail)
        << label << " died at 0xC0000409 (__fastfail) instead of reporting a "
                    "bad input. An abort unwinds nothing, so GoogleTest never "
                    "names the case that was running and ctest files it as an "
                    "abnormal termination rather than a test failure.";
    EXPECT_EQ(r.exitCode, 1u)
        << label << " exited " << r.exitCode
        << "; a torn shipped config must be an ordinary GoogleTest failure.\n"
        << r.capturedStdout;

    EXPECT_TRUE(contains(r.capturedStdout, "[  FAILED  ]"))
        << label << " produced no GoogleTest failure report:\n"
        << r.capturedStdout;

    // ── AND THE CLAUSE WITHOUT WHICH THE CLAIM IS UNPROVEN ───────────────────
    // The child must have read OUR tree. A `$DSS_CONFIG_ROOT` that missed would
    // fall through to the cwd walk onto the repo's real config and pass.
    EXPECT_TRUE(contains(r.capturedStdout, "torn-config-root"))
        << label << " never named the staged config tree, so it did not read "
                    "it — this run proves nothing about a torn config:\n"
        << r.capturedStdout;
}

} // namespace

TEST(TornShippedConfig, TheTokenizerSuiteRedsInsteadOfCrashing) {
    // Its fixtures used a NON-FATAL `EXPECT_TRUE` and then handed a null schema
    // to `Tokenizer`, whose `tokenizerFatal("schema is null")` correctly
    // refuses — by aborting. The product guard is right and stays; the fixture
    // no longer drives it there.
    expectRedNotCrash(DSS_TOKENIZER_TEST_BINARY, "tokenizer/test_tokenizer");
}

TEST(TornShippedConfig, TheHirLoweringSuiteRedsInsteadOfCrashing) {
    // Its fixtures called `std::abort()` themselves, one line after
    // `ADD_FAILURE()` — so the failure was recorded and then thrown away with
    // the process.
    expectRedNotCrash(DSS_HIR_LOWERING_C_TEST_BINARY, "hir/test_hir_lowering_c");
}
