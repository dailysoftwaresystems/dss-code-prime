// Tests for the ONE test-side repo-root resolver (`repo_root.hpp`).
//
// This file carries TWO suites with deliberately different run conditions,
// separated because they need opposite environments:
//
//   * `RepoRoot.*`        — precedence and validation, run by the ordinary
//                           ctest entry with the usual dss_add_test wiring.
//   * `BakedRootOnly.*`   — THE REGRESSION GUARD for the out-of-tree defect.
//                           Run ONLY by the `regression/out_of_tree_baked_root`
//                           entry, whose cwd has no `src/dss-config` in any
//                           ancestor and whose `DSS_CONFIG_ROOT` is stripped.
//                           The two entries select their suite with
//                           `--gtest_filter` (see tests/CMakeLists.txt).
//
// WHY THE GUARD EXISTS. A build directory OUTSIDE the source tree used to fail
// 29 of 787 ctest entries, because ~19 test-side helpers each walked up from
// `current_path()` for `src/dss-config` and never read the documented
// `DSS_CONFIG_ROOT`. Every one of those now routes through `repo_root.hpp`.
// The failure mode this guard defends against is the SILENT one: if someone
// deletes the baked `DSS_TEST_REPO_ROOT` compile definition, an ordinary ctest
// run stays green forever — the environment variable and the cwd walk both
// still work in-tree — and the out-of-tree build silently rots again. This
// entry is the only place where NEITHER of those two fallbacks is available,
// so it is the only place that can observe the baked value doing its job.

#include "golden_file.hpp"   // findCorpusRoot / readFile — the real consumers
#include "repo_root.hpp"
#include "scoped_env.hpp"    // the ONE env override (this file used to carry a copy)
#include "scratch_dir.hpp"   // a real second root, so "honoured" differs from "ignored"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <stdlib.h>
#else
#include <stdlib.h>
#endif

namespace fs = std::filesystem;

using dss::test_support::Location;
using dss::test_support::ScopedEnv;
using dss::test_support::ScratchDir;

// ── ordinary conditions ─────────────────────────────────────────────────

TEST(RepoRoot, ResolvesAndPointsAtRealTrees) {
    const auto root = dss::test::repoRoot();
    EXPECT_TRUE(fs::is_directory(root)) << root.string();
    EXPECT_TRUE(fs::is_directory(dss::test::configRoot()))
        << dss::test::configRoot().string();
    EXPECT_TRUE(fs::is_directory(dss::test::corpusRoot()))
        << dss::test::corpusRoot().string();
}

// The property that makes env-first SAFE on the CONFIG side. Without
// per-candidate validation, one stale shell export would redden every test in
// the suite instead of falling through to the checkout's own tree.
TEST(RepoRoot, StaleConfigOverrideFallsThroughInsteadOfPoisoningTheRun) {
    ScopedEnv guard("DSS_CONFIG_ROOT",
                    (fs::temp_directory_path() / "dss-nonexistent-root")
                        .string());
    EXPECT_EQ(dss::test::configRoot(),
              dss::test::repoRoot() / "src" / "dss-config")
        << "a DSS_CONFIG_ROOT that does not contain src/dss-config must be "
           "REJECTED by validation and fall through to the next candidate";
}

// An empty value is not an override. Guards the `env[0] != '\0'` check.
TEST(RepoRoot, EmptyConfigOverrideIsIgnored) {
    const auto expected = dss::test::repoRoot() / "src" / "dss-config";
    ScopedEnv  guard("DSS_CONFIG_ROOT", "");
    EXPECT_EQ(dss::test::configRoot(), expected);
}

// An explicit, VALID override must win on the CONFIG side — this is the half of
// the contract the operator's control run expected and did not get, and it is
// what the per-run config snapshot depends on.
TEST(RepoRoot, ValidConfigOverrideIsHonoured) {
    // A root that really does hold `src/dss-config` and is NOT the checkout: an
    // override equal to the checkout could not tell "honoured" from "ignored".
    ScratchDir      scratch(Location::Temp, "repo-root-config-override");
    const fs::path  other = scratch.path() / "src" / "dss-config";
    std::error_code ec;
    fs::create_directories(other, ec);
    ASSERT_FALSE(ec) << ec.message();

    ScopedEnv guard("DSS_CONFIG_ROOT", scratch.path().string());
    EXPECT_EQ(dss::test::configRoot(), other)
        << "an explicit, valid DSS_CONFIG_ROOT must select the config tree";
}

// ★★★ THE SPLIT ITSELF, and it is the assertion that lets the whole suite run
// against a config-only snapshot.
// D-TEST-SHIPPED-CONFIG-EXPOSURE-UNFIXED-OUTSIDE-THE-SUITE-THAT-FLAKED points
// `$DSS_CONFIG_ROOT` at `<build>/dss-config-snapshot`, a root that holds
// `src/dss-config` and `VERSION` and NOTHING ELSE. `repoRoot()` reaches
// tests/corpus, tests/hir/lowering_goldens, examples/, scripts/, real-examples/
// and src/ — so if the override relocated the checkout as well, every golden
// file in the repository would resolve into a directory that does not have one.
TEST(RepoRoot, ConfigOverrideDoesNotRelocateTheCheckout) {
    const auto realRoot   = dss::test::repoRoot();
    const auto realCorpus = dss::test::corpusRoot();

    ScratchDir      scratch(Location::Temp, "repo-root-split");
    std::error_code ec;
    fs::create_directories(scratch.path() / "src" / "dss-config", ec);
    ASSERT_FALSE(ec) << ec.message();

    ScopedEnv guard("DSS_CONFIG_ROOT", scratch.path().string());

    EXPECT_EQ(dss::test::repoRoot(), realRoot)
        << "$DSS_CONFIG_ROOT names the shipped CONFIG tree; it must not move the "
           "checkout the corpus and the sources live in";
    EXPECT_EQ(dss::test::corpusRoot(), realCorpus)
        << "the golden-file corpus followed the config override — under the "
           "per-run config snapshot that directory does not exist, and every "
           "golden test in the repository would fail for the wrong reason";
    // ...while the config side DID follow it, so this is a split and not an
    // override that quietly stopped working.
    EXPECT_EQ(dss::test::configRoot(), scratch.path() / "src" / "dss-config");
}

TEST(RepoRoot, RepoDiagnosticNamesItsCandidatesAndTheConfigOverride) {
    const auto msg = dss::test::repoRootDiagnostic();
    EXPECT_NE(msg.find("DSS_TEST_REPO_ROOT"), std::string::npos)   << msg;
    EXPECT_NE(msg.find("ancestor walk"), std::string::npos)        << msg;
    // Named, and named as what it IS — the commonest wrong guess about this
    // failure is that setting DSS_CONFIG_ROOT should have fixed it.
    EXPECT_NE(msg.find("DSS_CONFIG_ROOT"), std::string::npos)      << msg;
    EXPECT_NE(msg.find("NOT a repo-root candidate"), std::string::npos) << msg;
}

TEST(RepoRoot, ConfigDiagnosticNamesBothCandidates) {
    const auto msg = dss::test::configRootDiagnostic();
    EXPECT_NE(msg.find("DSS_CONFIG_ROOT"), std::string::npos)          << msg;
    EXPECT_NE(msg.find("the repo root, resolved as"), std::string::npos) << msg;
}

// ── THE REGRESSION GUARD ────────────────────────────────────────────────
// Runs only under `regression/out_of_tree_baked_root`.

// PRECONDITIONS FIRST, AS A TEST. An arm that silently loses its own setup
// still reports green, and a green guard that proves nothing is worse than no
// guard — it actively certifies the thing it stopped checking. These two
// assertions are what stop that: if ctest's ENVIRONMENT_MODIFICATION ever stops
// unsetting the variable, or the chosen working directory ever gains repo
// ancestry, THIS reddens and tells you the sibling assertions below went
// vacuous.
TEST(BakedRootOnly, PreconditionsHold) {
    char const* const env = std::getenv("DSS_CONFIG_ROOT");
    ASSERT_TRUE(env == nullptr || env[0] == '\0')
        << "DSS_CONFIG_ROOT must be UNSET for this arm (got '" << env
        << "') — with it set, the arm passes through candidate (1) and proves "
           "nothing about the baked root";

    std::error_code ec;
    fs::path        here = fs::current_path(ec);
    ASSERT_FALSE(here.empty());
    for (int i = 0; i < 12 && !here.empty(); ++i) {
        ASSERT_FALSE(fs::is_directory(here / "src" / "dss-config", ec))
            << "cwd " << fs::current_path(ec).string()
            << " has repo ancestry at " << here.string()
            << " — the ancestor walk can satisfy resolution here, so this arm "
               "would pass via candidate (3) rather than the baked root";
        const fs::path parent = here.parent_path();
        if (parent == here) break;
        here = parent;
    }
}

// THE ASSERTION THE WHOLE ARM EXISTS FOR: with no environment and no repo
// ancestry, resolution still succeeds — which can only be the CMake-baked
// DSS_TEST_REPO_ROOT. Delete that compile definition and this fails.
TEST(BakedRootOnly, ResolvesFromBakedDefinitionAlone) {
    ASSERT_TRUE(dss::test::findRepoRoot().has_value())
        << dss::test::repoRootDiagnostic();

    const auto root = dss::test::repoRoot();
    EXPECT_TRUE(fs::is_directory(root / "src" / "dss-config")) << root.string();
    EXPECT_TRUE(fs::is_directory(dss::test::configRoot()));
    EXPECT_TRUE(fs::is_directory(dss::test::corpusRoot()));
}

// Through the REAL helper the twelve aborting suites use, not just the raw
// resolver. `findCorpusRoot()` is `golden_file.hpp`'s entry point — the one
// whose 8-hop walk ended in `std::abort()` and produced "Subprocess aborted"
// out-of-tree. Reading an actual file through it is the difference between
// "the resolver returns a path" and "a corpus consumer can still work".
TEST(BakedRootOnly, RealCorpusHelperResolvesAndReadsAFile) {
    const auto corpus = dss::test_support::findCorpusRoot();
    ASSERT_TRUE(fs::is_directory(corpus)) << corpus.string();

    const auto sample = corpus / "c" / "mini_calc.c";
    ASSERT_TRUE(fs::exists(sample))
        << "corpus resolved to " << corpus.string()
        << " but the sample file is missing — resolution returned a path that "
           "is not the real corpus";
    EXPECT_FALSE(dss::test_support::readFile(sample).empty());
}

// The configRoot() half, likewise resolved to a real file on disk rather than
// to a directory that merely exists.
TEST(BakedRootOnly, ConfigRootResolvesAShippedConfigFile) {
    const auto shipped =
        dss::test::configRoot() / "sources" / "c.lang.json";
    EXPECT_TRUE(fs::exists(shipped)) << shipped.string();
}
