// `examples/` curated-source ctest harness. One entry per
// `examples/<lang>/<name>/expected.json` (registered at cmake-time
// via `file(GLOB_RECURSE ...)` in this dir's CMakeLists.txt) drives
// the full DSS pipeline via `Program::compileFiles`, then spawns
// the produced binary via `run_binary.hpp` and asserts the exact
// OS exit code. All asserts are STRICT — wrong-diagnostic, missing-
// artifact, spawn-failure, timeout, or wrong-exit-code all break
// the test loud.
//
// User invariant (verbatim, 2026-06-02): "please don't forget to
// perform strict asserts on the example harness run results....
// this is important". Hence ASSERT_EQ on every observable, never
// EXPECT_GT.
//
// Cross-host policy: examples whose target spec produces a binary
// for a different host OS (e.g. ELF-Linux when running on Windows)
// compile but SKIP the run step. The compile step still asserts
// strict zero-diagnostic success — a regression in cross-format
// emission surfaces even on the wrong host. The `runOn` array in
// `expected.json` lists host OS names ("windows" / "linux" /
// "darwin") on which the binary should spawn.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "program/program.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;
using namespace dss::test_support;

namespace {

[[nodiscard]] std::string currentHostOs() noexcept {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

struct ExampleTarget {
    std::string                  spec;
    std::string                  artifact;
    std::vector<std::string>     runOn;  // host OS names allowed to spawn
};

struct ExampleManifest {
    std::string                  language;
    std::string                  source;
    std::int64_t                 exitCode = 0;
    // FF6 Slice 3 (2026-06-02): optional captured-stdout pin. When
    // present, the harness routes the child's STDOUT through an
    // anonymous pipe (`captureStdout=true`) and asserts the
    // drained bytes match this string exactly. When absent (the
    // pre-FF6 pattern), the child inherits the parent's stdio
    // and only the exit code is asserted — preserving every
    // existing example's behavior. Empty-string is a VALID pin
    // (asserts the binary printed nothing); the `has_value()`
    // gate distinguishes "no pin" from "pin to empty".
    std::optional<std::string>   expectedStdout;
    std::vector<ExampleTarget>   targets;
};

[[nodiscard]] ExampleManifest readManifest(fs::path const& path) {
    std::ifstream in(path);
    if (!in) {
        ADD_FAILURE() << "cannot open manifest " << path.generic_string();
        return {};
    }
    nlohmann::json j;
    try {
        in >> j;
    } catch (std::exception const& e) {
        ADD_FAILURE() << "manifest " << path.generic_string()
                      << " JSON parse failed: " << e.what();
        return {};
    }
    ExampleManifest m;
    m.language = j.value("language", "");
    m.source   = j.value("source", "");
    if (!j.contains("exitCode") || !j.at("exitCode").is_number_integer()) {
        ADD_FAILURE() << "manifest " << path.generic_string()
                      << " missing integer 'exitCode'";
        return m;
    }
    m.exitCode = j.at("exitCode").get<std::int64_t>();
    if (j.contains("expectedStdout")) {
        if (!j.at("expectedStdout").is_string()) {
            ADD_FAILURE() << "manifest " << path.generic_string()
                          << " 'expectedStdout' must be a string";
            return m;
        }
        m.expectedStdout = j.at("expectedStdout").get<std::string>();
    }
    if (!j.contains("targets") || !j.at("targets").is_array()
        || j.at("targets").empty()) {
        ADD_FAILURE() << "manifest " << path.generic_string()
                      << " requires non-empty 'targets' array";
        return m;
    }
    for (auto const& t : j.at("targets")) {
        ExampleTarget et;
        et.spec     = t.value("spec", "");
        et.artifact = t.value("artifact", "");
        if (t.contains("runOn") && t.at("runOn").is_array()) {
            for (auto const& s : t.at("runOn")) {
                if (s.is_string()) et.runOn.push_back(s.get<std::string>());
            }
        }
        m.targets.push_back(std::move(et));
    }
    return m;
}

void runOneTarget(fs::path const&        exampleDir,
                  ExampleManifest const& m,
                  ExampleTarget const&   t) {
    ASSERT_FALSE(t.spec.empty())
        << "target spec missing in manifest";
    ASSERT_FALSE(t.artifact.empty())
        << "artifact filename missing in target";

    ScratchDir scratch{Location::InsideRepo, "examples"};
    // Copy the source into scratch so the in-process compiler runs
    // against a writable scratch tree (its cwd-walk discovers
    // src/dss-config/ via the InsideRepo location).
    auto const srcPath = scratch.path() / m.source;
    fs::copy_file(exampleDir / m.source, srcPath,
                  fs::copy_options::overwrite_existing);
    scratch.useAsCwd();
    auto const outDir = scratch.path() / "out";

    Program            prog;
    DiagnosticReporter rep;
    prog.setOutputDir(outDir);
    int const rc = prog.compileFiles(
        {srcPath.generic_string()},
        m.language,
        {t.spec},
        rep);

    // Strict compile-side asserts (user invariant 2026-06-02):
    //   * rc == 0
    //   * zero error-severity diagnostics
    //   * artifact exists on disk with non-empty file
    std::ostringstream diagDump;
    for (auto const& d : rep.all()) {
        diagDump << "\n  " << diagnosticCodeName(d.code)
                 << " (severity=" << static_cast<int>(d.severity)
                 << "): " << d.actual;
    }
    ASSERT_EQ(rc, 0)
        << "compileFiles failed for spec=" << t.spec
        << " example=" << exampleDir.generic_string()
        << " diagnostics:" << diagDump.str();
    ASSERT_EQ(rep.errorCount(), 0u)
        << "expected zero error-severity diagnostics for spec="
        << t.spec << diagDump.str();

    auto const artifactPath = outDir / t.artifact;
    ASSERT_TRUE(fs::exists(artifactPath))
        << "artifact missing at " << artifactPath.generic_string();
    ASSERT_GT(fs::file_size(artifactPath), 0u)
        << "artifact is empty: " << artifactPath.generic_string();

    // Run-side asserts gated on host OS matching the target's
    // `runOn`. Cross-host compiles still verified above; the spawn
    // is skipped when the produced binary's loader doesn't match
    // the test host.
    auto const host = currentHostOs();
    bool const shouldRun = std::any_of(t.runOn.begin(), t.runOn.end(),
        [&](std::string const& s) { return s == host; });
    if (!shouldRun) {
        // Surface the skip in the test log so cross-host CI knows
        // why the run was elided (no silent skip).
        GTEST_LOG_(INFO) << "spec=" << t.spec
                        << " produced an artifact but runOn=["
                        << (t.runOn.empty() ? "" : t.runOn.front())
                        << "...] excludes host=" << host;
        return;
    }

    bool const captureStdout = m.expectedStdout.has_value();
    auto const result = runBinary(artifactPath,
                                  std::chrono::milliseconds{5000},
                                  captureStdout);
    // STRICT: spawn must succeed, no timeout, exact exit code.
    ASSERT_TRUE(result.spawned)
        << "spawn failed for " << artifactPath.generic_string()
        << " diag=" << result.diagnostic;
    ASSERT_FALSE(result.timedOut)
        << "spawn timed out for " << artifactPath.generic_string()
        << " diag=" << result.diagnostic;
    ASSERT_EQ(static_cast<std::int64_t>(result.exitCode), m.exitCode)
        << "exit-code mismatch for "
        << artifactPath.generic_string()
        << " (manifest expected " << m.exitCode
        << "; OS reported " << result.exitCode << ")"
        << " diag=" << result.diagnostic;
    // FF6 Slice 3 (2026-06-02): captured-stdout pin. The whole
    // point of capturing is to catch silent print failures the
    // exit-code pin can't see — a regression in FFI mangling,
    // .idata layout, CRT init, or puts itself would leave
    // `return 42` untouched while puts writes nothing. Comparing
    // the drained bytes loud-fails on every such regression.
    if (m.expectedStdout.has_value()) {
        ASSERT_EQ(result.capturedStdout, *m.expectedStdout)
            << "stdout mismatch for "
            << artifactPath.generic_string()
            << " (manifest expected "
            << m.expectedStdout->size() << " bytes; OS produced "
            << result.capturedStdout.size() << " bytes)";
    }
}

} // namespace

// argv[1] = absolute path to the example dir (registered by cmake).
//
// The harness reads `<dir>/expected.json`, drives every target spec
// in the manifest, and exits 0 only if every assertion passed.
TEST(Examples, RunFromManifest) {
    auto const argv0Dir =
        ::testing::internal::GetArgvs();
    ASSERT_GE(argv0Dir.size(), 2u)
        << "examples_runner needs the example directory as argv[1]";
    fs::path const exampleDir = argv0Dir[1];
    ASSERT_TRUE(fs::is_directory(exampleDir))
        << "argv[1] is not a directory: " << exampleDir.generic_string();
    auto const manifestPath = exampleDir / "expected.json";
    ASSERT_TRUE(fs::exists(manifestPath))
        << "expected.json missing in " << exampleDir.generic_string();
    auto const m = readManifest(manifestPath);
    ASSERT_FALSE(m.targets.empty()) << "manifest declared no targets";
    for (auto const& t : m.targets) {
        SCOPED_TRACE("target spec=" + t.spec);
        runOneTarget(exampleDir, m, t);
    }
}
