#pragma once

// ONE repo-root resolver for the whole test-support layer.
// [[D-TEST-HELPERS-IGNORE-DSS-CONFIG-ROOT-OUT-OF-TREE]]
//
// WHY THIS EXISTS. Before this header, 19 files under `tests/` +
// `integrated_tests/` each carried their OWN copy of "walk up from
// `current_path()` looking for `src/dss-config/`". Only two of them had ever
// heard of `DSS_CONFIG_ROOT`. The other seventeen walked blind — so a build
// directory OUTSIDE the source tree (whose cwd has no `src/dss-config/`
// anywhere in its ancestry) failed 28 of 787 ctest entries, twelve of them as
// "Subprocess aborted" because the walkers called `std::abort()` on a miss.
// MEASURED by the operator on macOS arm64 at a3af1320: out-of-tree 759/787,
// in-tree 787/787, same commit, same compiler, same machine.
//
// ★ THE CONTROL RUN THAT KILLED THE OBVIOUS FIX. Re-running out-of-tree with
// `DSS_CONFIG_ROOT=<repo>` exported process-wide produced an IDENTICAL 28-test
// failure set. The documented override was already being SET (see
// `dss_add_test` in tests/CMakeLists.txt) — it just was not being READ by any
// of these seventeen helpers. Adding another place that sets it would have
// fixed nothing. The fix has to be that every test-side lookup reads the same
// resolver, which is this one. Do not re-open a local walk anywhere.
//
// ── TWO QUESTIONS, TWO RESOLVERS ────────────────────────────────────────
//   D-TEST-SHIPPED-CONFIG-EXPOSURE-UNFIXED-OUTSIDE-THE-SUITE-THAT-FLAKED
//
// ★★★ `$DSS_CONFIG_ROOT` ANSWERS "WHICH CONFIG TREE", NOT "WHICH CHECKOUT", AND
// CONFLATING THE TWO WAS A LATENT DEFECT THIS HEADER USED TO CARRY. Its
// documented meaning (`src/core/types/config_path_walk.hpp`) is "a directory
// that CONTAINS src/dss-config/" — it says nothing about `tests/corpus`,
// `tests/hir/lowering_goldens`, `examples/`, `scripts/`, `real-examples/` or
// `src/`, all of which this header's consumers reach through `repoRoot()`. While
// the only value anyone ever put in the variable was the repo root itself, the
// conflation cost nothing and was invisible. It stopped being invisible the
// moment the suite started running against a per-run SNAPSHOT of the config tree
// (`cmake/DssConfigSnapshot.cmake`): that root holds `src/dss-config` and
// `VERSION` and nothing else, so an env-first `repoRoot()` would have sent every
// golden-file lookup in the repository into a directory that does not have one.
//
// So the precedences are now stated separately, each for exactly the question it
// answers:
//
//   configRoot()  — THE SHIPPED CONFIG TREE
//     1. `$DSS_CONFIG_ROOT`      — the explicit, documented override
//     2. `repoRoot()/src/dss-config`
//
//   repoRoot()    — THE CHECKOUT THESE BINARIES WERE BUILT FROM
//     1. `DSS_TEST_REPO_ROOT`    — baked from ${CMAKE_SOURCE_DIR} at configure
//                                  time by `dss_add_test` (tests/CMakeLists.txt)
//     2. the cwd ancestor walk   — what every one of these helpers used to do
//
// ★ THE ENV-FIRST ARGUMENT SURVIVES INTACT WHERE IT APPLIES, which is why this
// is a narrowing rather than a reversal. `src/core/types/config_path_walk.cpp` —
// the COMPILER side of exactly this question — resolves the environment FIRST.
// If the tests preferred a different config tree than the compiler, a developer
// pointing `DSS_CONFIG_ROOT` at a second checkout would have the compiler
// reading tree A while its own tests read tree B, with nothing anywhere
// reporting the split. `configRoot()` still honours the override first, so the
// two halves of the program still consult one config tree. What no longer
// happens is the override silently relocating the corpus.
//
// ★ EVERY CANDIDATE IS VALIDATED, AND THAT IS WHAT MAKES ENV-FIRST SAFE.
// A candidate counts only if it is a directory containing `src/dss-config`. A
// stale, deleted, or simply wrong `DSS_CONFIG_ROOT` therefore FALLS THROUGH to
// the next candidate instead of poisoning the run — the same
// "a set-but-miss never worsens discovery" property `config_path_walk.cpp`
// gives the compiler. Without per-candidate validation, env-first would turn
// one stale shell export into 787 red tests.
// ⚠ AND THAT FALL-THROUGH IS PRECISELY WHY A MISSING SNAPSHOT MUST BE CAUGHT
// SOMEWHERE ELSE: it is silent by design here, so nothing in this header can
// tell you the isolation stopped working. The `config/snapshot` ctest fixture
// fails loud when the copy cannot be made, and `test_config_snapshot.cpp`
// asserts the override was READ rather than merely set.
//
// ★ NO CACHING, ON PURPOSE. Resolution is recomputed per call (a couple of
// `stat`s). A cached first answer would be captured from whichever test ran
// first and would then survive both `ScratchDir`'s cwd changes and the
// `ScopedEnv` juggling in `tests/core/test_config_path_walk.cpp` — i.e. it
// would make results depend on test ORDER, under a gate that deliberately runs
// `--gtest_shuffle` arms to catch exactly that class of bug.
//
// ★ FAILS LOUD, NEVER `abort()`. `std::abort()` kills the whole test BINARY, so
// one unresolvable path costs every sibling test in that executable its verdict
// — the harness cannot even report which unit failed. `repoRoot()` throws
// instead (GoogleTest reports a throw as a failure of that ONE test);
// `findRepoRoot()` is the non-throwing primitive for callers that already have
// an "empty means the caller reports it" contract.

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

namespace dss::test {

namespace detail {

// A candidate is the repo root iff it is a directory that CONTAINS
// `src/dss-config`. Cheap, and it is the same predicate the compiler-side
// resolver implies by probing `src/dss-config/<subdir>/<leaf>`.
[[nodiscard]] inline bool isRepoRoot(std::filesystem::path const& candidate) {
    if (candidate.empty()) return false;
    std::error_code ec;
    return std::filesystem::is_directory(candidate / "src" / "dss-config", ec);
}

// The value baked in by CMake, or nullptr for a build that did not define it
// (a hand-rolled compile line — the walk is what covers that case).
[[nodiscard]] inline char const* bakedRepoRoot() {
#ifdef DSS_TEST_REPO_ROOT
    return DSS_TEST_REPO_ROOT;
#else
    return nullptr;
#endif
}

// `$DSS_CONFIG_ROOT` as a VALIDATED repo-shaped root, or nullopt. Shared by
// `findConfigRoot()` and by the diagnostics, so there is one reading of the
// variable rather than three.
[[nodiscard]] inline std::optional<std::filesystem::path> envConfigTreeRoot() {
    if (char const* env = std::getenv("DSS_CONFIG_ROOT");
        env != nullptr && env[0] != '\0') {
        std::filesystem::path const candidate{env};
        if (isRepoRoot(candidate)) return candidate;
    }
    return std::nullopt;
}

} // namespace detail

// Resolve the checkout these binaries were built from, or `nullopt`.
// Non-throwing: this is the primitive the throwing accessor and every "return
// empty, caller reports" helper share, so there is exactly ONE copy of the
// precedence rules above.
//
// ⚠ DELIBERATELY DOES NOT CONSULT `$DSS_CONFIG_ROOT` — see the two-questions
// note in the header block. That variable names a CONFIG tree, and under the
// per-run config snapshot it names one that holds no corpus, no examples and no
// sources.
[[nodiscard]] inline std::optional<std::filesystem::path> findRepoRoot() {
    namespace fs = std::filesystem;

    // (1) baked at configure time
    if (char const* baked = detail::bakedRepoRoot();
        baked != nullptr && baked[0] != '\0') {
        fs::path const candidate{baked};
        if (detail::isRepoRoot(candidate)) return candidate;
    }

    // (2) the ancestor walk these helpers used to do individually.
    // 12 hops, not 8: the sites being consolidated here used BOTH bounds (8 in
    // golden_file.hpp / test_import_resolver, 12 in test_header_name_matching /
    // test_hir_lowering_c / test_pe_crt_costate_binding), and narrowing a
    // 12-hop site to 8 would delete reach it has today. Widening an 8-hop site
    // can only add hops that were previously a hard failure, so the superset is
    // the direction that cannot regress anyone.
    std::error_code ec;
    fs::path here = fs::current_path(ec);
    for (int i = 0; i < 12 && !here.empty(); ++i) {
        if (detail::isRepoRoot(here)) return here;
        fs::path const parent = here.parent_path();
        if (parent == here) break;   // hit the filesystem root
        here = parent;
    }

    return std::nullopt;
}

// Resolve the SHIPPED CONFIG TREE, or `nullopt`. The override first, then
// whatever tree belongs to the resolved checkout.
[[nodiscard]] inline std::optional<std::filesystem::path> findConfigRoot() {
    if (auto const viaEnv = detail::envConfigTreeRoot()) {
        return *viaEnv / "src" / "dss-config";
    }
    if (auto const root = findRepoRoot()) {
        return *root / "src" / "dss-config";
    }
    return std::nullopt;
}

// The diagnostic for an unresolvable repo root. Names BOTH sources with the
// value each actually held, plus the cwd, because the failure is always "which
// of these was I expecting to work" and a message that omits one sends the
// reader to the wrong half of the system. `$DSS_CONFIG_ROOT` is reported too,
// and reported as what it is — a CONFIG override that is not a repo-root
// candidate — because the commonest wrong guess about this failure is that
// setting it should have fixed it.
[[nodiscard]] inline std::string repoRootDiagnostic() {
    std::error_code ec;
    char const* const env   = std::getenv("DSS_CONFIG_ROOT");
    char const* const baked = detail::bakedRepoRoot();

    std::string msg =
        "could not locate the repo root (a directory containing "
        "src/dss-config). Tried, in order:\n"
        "  1. DSS_TEST_REPO_ROOT (baked by CMake) = ";
    msg += (baked == nullptr) ? "<not defined at compile time>"
         : (baked[0] == '\0') ? "<empty>"
                              : baked;
    msg += "\n  2. ancestor walk (12 hops) from cwd = ";
    msg += std::filesystem::current_path(ec).string();
    msg += "\nEach candidate is validated as a directory containing "
           "src/dss-config; both failed that check.\n"
           "  note: $DSS_CONFIG_ROOT = ";
    msg += (env == nullptr) ? "<unset>"
         : (env[0] == '\0') ? "<empty>"
                            : env;
    msg += " — that variable selects the shipped CONFIG tree (configRoot()) and "
           "is NOT a repo-root candidate; under the per-run config snapshot it "
           "names a root that holds no corpus and no sources.";
    return msg;
}

// The diagnostic for an unresolvable config tree.
[[nodiscard]] inline std::string configRootDiagnostic() {
    char const* const env = std::getenv("DSS_CONFIG_ROOT");
    std::string       msg =
        "could not locate the shipped config tree (src/dss-config). Tried, in "
        "order:\n  1. $DSS_CONFIG_ROOT = ";
    msg += (env == nullptr) ? "<unset>"
         : (env[0] == '\0') ? "<empty>"
                            : env;
    msg += " (validated as a directory CONTAINING src/dss-config)\n"
           "  2. the repo root, resolved as:\n";
    msg += repoRootDiagnostic();
    return msg;
}

// The repo root. Throws `std::runtime_error` carrying `repoRootDiagnostic()`
// if nothing resolves — GoogleTest reports that as a failure of the running
// test, which is the behaviour `std::abort()` denied every sibling test.
[[nodiscard]] inline std::filesystem::path repoRoot() {
    if (auto const root = findRepoRoot()) return *root;
    throw std::runtime_error(repoRootDiagnostic());
}

// The shipped-config tree — `$DSS_CONFIG_ROOT/src/dss-config` when the override
// is set and valid, else `<repo>/src/dss-config`.
[[nodiscard]] inline std::filesystem::path configRoot() {
    if (auto const cfg = findConfigRoot()) return *cfg;
    throw std::runtime_error(configRootDiagnostic());
}

// `<repo>/tests/corpus` — the golden-file corpus tree. Off `repoRoot()`, never
// off the config override: the corpus belongs to the checkout, not to whichever
// config tree this run happens to be reading.
[[nodiscard]] inline std::filesystem::path corpusRoot() {
    return repoRoot() / "tests" / "corpus";
}

} // namespace dss::test
