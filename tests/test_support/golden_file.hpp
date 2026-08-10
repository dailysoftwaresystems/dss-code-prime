#pragma once

// Shared golden-file infrastructure for corpus tests. Hoisted from
// `tests/analysis/syntactic/test_corpus.cpp` when the V2-4 Part B
// golden-DIAGNOSTIC harness became the SECOND consumer of this logic
// (the canonical "stop copy-pasting" trigger). Both the golden-TREE
// harness (test_corpus.cpp) and the golden-DIAGNOSTIC harness
// (test_diagnostic_corpus.cpp) consume these so the refresh discipline
// + the CRLF-normalized compare live in EXACTLY ONE place and cannot
// drift between the two.

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace dss::test_support {

// The `tests/corpus/` tree, via the ONE test-side resolver
// (`repo_root.hpp`: $DSS_CONFIG_ROOT → the CMake-baked repo root → the cwd
// ancestor walk). This used to be a private cwd-walk that ended in
// `std::abort()`, which is why an out-of-tree build reported twelve corpus
// suites as "Subprocess aborted": `abort()` takes down the whole test BINARY,
// so one unresolvable corpus root cost every sibling test in that executable
// its verdict — the harness could not even say which unit was unhappy.
// `repoRoot()` throws instead, and GoogleTest reports a throw as a failure of
// the one running test.
[[nodiscard]] inline std::filesystem::path findCorpusRoot() {
    return dss::test::corpusRoot();
}

// Throws rather than `abort()`ing for the same reason as above: a missing
// golden must cost ONE test its verdict, not the executable's whole roster.
[[nodiscard]] inline std::string readFile(std::filesystem::path const& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        throw std::runtime_error("cannot open " + path.string());
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return std::move(buf).str();
}

// Parse the `DSS_REFRESH_GOLDENS` env var. The intent is "set to `1` to
// refresh"; other values (incl. `0`/`false`) are explicitly REJECTED
// rather than treated as truthy. Loose `getenv != nullptr` semantics
// would let a stale/accidentally-exported `=0` silently overwrite
// goldens and skip the byte-compare across every corpus test — a CI
// footgun this guard exists to prevent.
[[nodiscard]] inline bool goldenRefreshRequested() {
    const char* raw = std::getenv("DSS_REFRESH_GOLDENS");
    if (raw == nullptr) return false;
    const std::string_view v{raw};
    if (v == "1" || v == "true" || v == "TRUE" || v == "yes") return true;
    if (v.empty() || v == "0" || v == "false" || v == "FALSE" || v == "no") {
        return false;
    }
    // Throws rather than `abort()`ing: the refusal must redden the test that
    // asked, not delete the verdicts of every other test in the binary.
    throw std::runtime_error(
        "DSS_REFRESH_GOLDENS has unexpected value '" + std::string{v}
        + "' — use '1' to refresh, unset (or '0') otherwise. "
          "Refusing to interpret to avoid silently masking drift.");
}

// Generic golden-text comparison. `actual` is the freshly-rendered text
// (a tree fingerprint, a `.diag` listing, …); `goldenPath` is its
// sibling golden. To regenerate after an intentional change, set
// `DSS_REFRESH_GOLDENS=1` and re-run ctest — this rewrites the golden in
// place AND fails the test by design (so a CI run that accidentally has
// the env var set can NEVER be a passing build — refresh is a developer
// action, not a CI mode). Missing golden = fail (better than silently
// skipping the pin on a new corpus file that forgot its companion). The
// compare is CRLF→LF normalized on BOTH sides so a Windows `autocrlf`
// checkout of an LF-committed golden still matches an LF-rendered actual.
inline void checkGoldenText(std::string const&            actual,
                            std::filesystem::path const&  goldenPath) {
    namespace fs = std::filesystem;
    if (goldenRefreshRequested()) {
        std::ofstream out{goldenPath, std::ios::binary};
        if (!out.is_open()) {
            ADD_FAILURE() << "cannot open golden file for write: "
                          << goldenPath.string();
            return;
        }
        out << actual;
        out.flush();
        if (!out.good()) {
            ADD_FAILURE() << "write to golden file failed (disk full / quota?): "
                          << goldenPath.string();
            return;
        }
        ADD_FAILURE() << "Refreshed " << goldenPath.string()
                      << " — this is a developer-only operation; the test "
                         "fails by design when refreshed so CI with "
                         "DSS_REFRESH_GOLDENS=1 cannot mask drift.";
        return;
    }
    if (!fs::exists(goldenPath)) {
        ADD_FAILURE() << "missing golden file " << goldenPath.string()
                      << " — generate it once via `DSS_REFRESH_GOLDENS=1 ctest`";
        return;
    }
    std::ifstream in{goldenPath, std::ios::binary};
    if (!in.is_open()) {
        ADD_FAILURE() << "cannot open golden file for read "
                         "(permission / sharing violation?): "
                      << goldenPath.string();
        return;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string expected = std::move(buf).str();
    std::string actualNorm = actual;
    std::erase(expected,   '\r');   // CRLF→LF: line-ending agnostic compare
    std::erase(actualNorm, '\r');
    EXPECT_EQ(actualNorm, expected)
        << "golden diverged from " << goldenPath.filename().string()
        << " — if the change is intentional, re-run with "
           "`DSS_REFRESH_GOLDENS=1` to update the golden";
}

} // namespace dss::test_support
