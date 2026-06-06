// DSS Code Prime — integration tests.
//
// Drives the compiled `dss-code-prime` executable as a SUBPROCESS
// (vs the in-process `Program::compileFiles` path exercised by
// `tests/examples/examples_runner`). The two paths share the
// curated `examples/` corpus but exercise different surfaces:
//
//   * In-process (tests/examples/) — API + library link path.
//     Fast, runs every ctest cycle. Catches API/regression bugs.
//   * Subprocess (here) — full CLI surface (argv parsing, exit
//     codes, filesystem layout, output routing). Slower per
//     example but catches CLI-level regressions the in-process
//     path misses.
//
// **Always against current host platform** (user invariant
// 2026-06-02): each example's manifest declares a per-target
// `runOn` list naming the host OSes that may spawn the produced
// binary. This runner picks the FIRST target whose `runOn`
// includes the current host and uses THAT target spec; if no
// target matches the host, the example is skipped with a loud
// diagnostic (not silent — every example must run somewhere).
//
// User invariant (2026-06-02): strict asserts on every observable
// — exit codes EQ, no timeouts, no spawn failures. Wrong-value
// breaks the run.

#include "run_binary.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int passes  = 0;
int failures = 0;

void check(std::string const& description, bool condition,
           std::string const& detail = "") {
    if (condition) {
        std::cout << "  [PASS] " << description << "\n";
        ++passes;
    } else {
        std::cout << "  [FAIL] " << description;
        if (!detail.empty()) std::cout << " — " << detail;
        std::cout << "\n";
        ++failures;
    }
}

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

// Quote a path for safe inclusion in a `std::system` command line
// — wraps in double quotes (PowerShell + cmd.exe + POSIX shells
// all honor "..." for argument grouping). The path's own contents
// don't need escaping for our cases (paths in test fixtures don't
// contain double quotes).
[[nodiscard]] std::string quote(std::string const& s) {
    return "\"" + s + "\"";
}

// Wrap a full command in the platform's `std::system()` shell
// conventions. On Windows, `std::system()` invokes `cmd /S /C`
// which strips a SINGLE leading + trailing pair of double quotes
// from the command line. When the command itself contains quoted
// tokens (e.g. an exe path with spaces), this strip eats the
// outer quotes we wanted preserved — the canonical fix is to add
// an extra outer pair. POSIX shells have no such stripping; the
// extra pair would be passed verbatim as args. Hence the
// platform-conditional wrap.
[[nodiscard]] std::string shellWrap(std::string const& cmd) {
#if defined(_WIN32)
    return "\"" + cmd + "\"";
#else
    return cmd;
#endif
}

struct ExampleTarget {
    std::string              spec;
    std::string              artifact;
    std::vector<std::string> runOn;
};

struct ExampleManifest {
    std::string                language;
    // Multi-CU (CU6): "sources":[...] makes each file its OWN CompilationUnit; the CLI's
    // `--compile a.c b.c` links them into one image (gcc/clang semantics — separate TUs,
    // the linker resolves cross-file references, LK11). The single "source":"x.c" form is
    // one file. `sources` is the canonical file list for either form (single → a 1-element
    // list); `multiCu` records which spelling the manifest used (diagnostic only — the CLI
    // derives multi-vs-single from the file count, not from this flag).
    std::vector<std::string>   sources;
    bool                       multiCu = false;
    std::int64_t               exitCode = 0;
    std::vector<ExampleTarget> targets;
};

[[nodiscard]] bool readManifest(fs::path const& path, ExampleManifest& out) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "  cannot open " << path.generic_string() << "\n";
        return false;
    }
    nlohmann::json j;
    try { in >> j; }
    catch (std::exception const& e) {
        std::cerr << "  JSON parse failed for " << path.generic_string()
                  << ": " << e.what() << "\n";
        return false;
    }
    out.language = j.value("language", "");
    // "sources":[...] (multi-CU, CU6) takes precedence over "source" (single file). Mirror
    // the in-process examples_runner so BOTH corpus harnesses accept the same manifests.
    if (j.contains("sources")) {
        if (!j.at("sources").is_array() || j.at("sources").empty()) {
            std::cerr << "  'sources' must be a non-empty array of file names in "
                      << path.generic_string() << "\n";
            return false;
        }
        for (auto const& s : j.at("sources")) {
            if (!s.is_string()) {
                std::cerr << "  'sources' entries must be strings in "
                          << path.generic_string() << "\n";
                return false;
            }
            out.sources.push_back(s.get<std::string>());
        }
        out.multiCu = true;
    } else if (auto single = j.value("source", std::string{}); !single.empty()) {
        out.sources = {single};
    } else {
        std::cerr << "  manifest requires 'source' (single CU) or 'sources' (multi-CU): "
                  << path.generic_string() << "\n";
        return false;
    }
    if (!j.contains("exitCode") || !j.at("exitCode").is_number_integer()) {
        std::cerr << "  missing integer 'exitCode' in "
                  << path.generic_string() << "\n";
        return false;
    }
    out.exitCode = j.at("exitCode").get<std::int64_t>();
    if (!j.contains("targets") || !j.at("targets").is_array()) {
        std::cerr << "  missing 'targets' array in "
                  << path.generic_string() << "\n";
        return false;
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
        out.targets.push_back(std::move(et));
    }
    return true;
}

// Drive ONE example through the CLI subprocess path:
//   1. spawn `dss-code-prime --compile <src> --language <l> --target <spec> --output <outdir>`
//   2. check rc == 0
//   3. check artifact file exists at outdir/<artifact>
//   4. spawn artifact, capture exit code via run_binary.hpp
//   5. ASSERT exit code == manifest.exitCode
//
// All checks are strict — wrong values fail the test.
void runExampleViaCli(std::string const& compiler,
                      fs::path const&    exampleDir,
                      fs::path const&    outputBase,
                      ExampleManifest const& m) {
    auto const host = currentHostOs();
    ExampleTarget const* target = nullptr;
    for (auto const& t : m.targets) {
        for (auto const& osName : t.runOn) {
            if (osName == host) {
                target = &t;
                break;
            }
        }
        if (target != nullptr) break;
    }
    if (target == nullptr) {
        std::cout << "  [SKIP] " << exampleDir.filename().generic_string()
                  << " — no target's runOn includes host=" << host
                  << " (cross-host compile-only is exercised by"
                  << " tests/examples/ in-process runner)\n";
        return;
    }

    auto const exampleName = exampleDir.filename().generic_string();
    // Target spec format is `<cpu>:<format>`; the `:` is illegal
    // in Windows path components. Substitute `_` to derive a
    // filesystem-safe sub-directory name. The substitution is
    // local to disk layout and never leaks back into the CLI's
    // --target argument (which keeps the canonical `:`-form).
    auto const specDir = [&]() {
        std::string s = target->spec;
        for (auto& c : s) if (c == ':') c = '_';
        return s;
    }();
    auto const outDir =
        outputBase / "ex" / exampleName / specDir;
    fs::create_directories(outDir);
    // Build the CLI invocation. The compiler binary path may
    // contain spaces (Visual Studio tooling drops it under
    // "C:\Program Files (x86)\..."); quote both the binary AND
    // every path argument. Redirect stdout+stderr to a file so
    // failures retain the compiler diagnostics for diagnosis.
    //
    // `--compile <file>...` takes a space-separated file list in ONE invocation; a multi-CU
    // example passes ALL its sources here and the CLI links each as its own translation unit
    // (the driver routes >1 file to compileUnits — gcc/clang semantics). Each path is quoted
    // independently.
    std::string compileArgs;
    for (auto const& s : m.sources) {
        compileArgs += " " + quote((exampleDir / s).string());
    }
    auto const cliLog = outDir / "cli.log";
    std::string cmd = quote(compiler)
        + " --compile"   + compileArgs
        + " --language " + m.language
        + " --target "   + target->spec
        + " --output "   + quote(outDir.string())
        + " > " + quote(cliLog.string()) + " 2>&1";

    int const sysRc = std::system(shellWrap(cmd).c_str());

    auto const compileOk = [&]() -> bool {
        if (sysRc == 0) return true;
        check(exampleName + ": compile rc == 0 (rc=" + std::to_string(sysRc)
              + ", cli log: " + cliLog.generic_string() + ")",
              false);
        return false;
    }();
    if (!compileOk) return;
    check(exampleName + ": compile exits 0", true);

    auto const artifactPath = outDir / target->artifact;
    bool const artifactExists = fs::exists(artifactPath);
    check(exampleName + ": artifact exists at "
          + artifactPath.generic_string(), artifactExists);
    if (!artifactExists) return;

    bool const artifactNonEmpty =
        artifactExists && fs::file_size(artifactPath) > 0u;
    check(exampleName + ": artifact non-empty", artifactNonEmpty);
    if (!artifactNonEmpty) return;

    auto const result = dss::test_support::runBinary(artifactPath);
    check(exampleName + ": spawn succeeded (diag='"
          + result.diagnostic + "')", result.spawned);
    if (!result.spawned) return;

    check(exampleName + ": no timeout", !result.timedOut);
    if (result.timedOut) return;

    bool const exitMatches =
        static_cast<std::int64_t>(result.exitCode) == m.exitCode;
    check(exampleName + ": OS exit code == "
          + std::to_string(m.exitCode)
          + " (got " + std::to_string(result.exitCode) + ")",
          exitMatches);
}

void runAllExamples(std::string const& compiler,
                    fs::path const&    examplesRoot,
                    fs::path const&    outputBase) {
    if (!fs::is_directory(examplesRoot)) {
        std::cerr << "[ERROR] examples root not a directory: "
                  << examplesRoot.generic_string() << "\n";
        ++failures;
        return;
    }
    // Walk examples/<lang>/<name>/expected.json. The tree depth
    // is fixed (2 levels) so a recursive iterator is overkill;
    // a nested loop is clearer.
    std::vector<fs::path> manifestPaths;
    for (auto const& langEntry : fs::directory_iterator(examplesRoot)) {
        if (!langEntry.is_directory()) continue;
        for (auto const& nameEntry :
             fs::directory_iterator(langEntry.path())) {
            if (!nameEntry.is_directory()) continue;
            auto const mp = nameEntry.path() / "expected.json";
            if (fs::exists(mp)) manifestPaths.push_back(mp);
        }
    }
    // Deterministic order for reproducible logs.
    std::sort(manifestPaths.begin(), manifestPaths.end());

    if (manifestPaths.empty()) {
        std::cerr << "[ERROR] no examples found under "
                  << examplesRoot.generic_string() << "\n";
        ++failures;
        return;
    }
    std::cout << "[Test 3] Examples corpus via CLI subprocess ("
              << manifestPaths.size() << " manifests)\n";
    for (auto const& mp : manifestPaths) {
        ExampleManifest m;
        if (!readManifest(mp, m)) {
            ++failures;
            continue;
        }
        runExampleViaCli(compiler, mp.parent_path(), outputBase, m);
    }
    std::cout << "\n";
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: integrated_tests <path-to-dss-code-prime> "
                  << "<path-to-examples-root>\n";
        return 1;
    }

    std::string const compiler = fs::absolute(argv[1]).string();
    fs::path const examplesRoot = fs::absolute(argv[2]);
    fs::path const outputBase   =
        fs::temp_directory_path() / "dss-integrated-tests";

    fs::remove_all(outputBase);
    fs::create_directories(outputBase);

    std::cout << "=== DSS Code Prime — Integration Tests ===\n"
              << "Compiler:      " << compiler << "\n"
              << "Examples root: " << examplesRoot.generic_string() << "\n"
              << "Output:        " << outputBase.string() << "\n"
              << "Host OS:       " << currentHostOs() << "\n\n";

    // ── Test 1: Default invocation prints ready message ──
    std::cout << "[Test 1] Default invocation\n";
    {
        std::string cmd = quote(compiler) + " > "
            + quote((outputBase / "default_output.txt").string())
            + " 2>&1";
        int const rc = std::system(shellWrap(cmd).c_str());
        check("Exit code is 0", rc == 0, "got " + std::to_string(rc));
        std::ifstream f((outputBase / "default_output.txt").string());
        std::string line;
        std::getline(f, line);
        check("Prints ready message",
              line.find("DSS Code Prime compiler ready.")
                  != std::string::npos,
              "got: " + line);
    }
    std::cout << "\n";

    // ── Test 2: Unknown flag rejected with non-zero exit ──
    std::cout << "[Test 2] Unknown flag is rejected\n";
    {
        std::string cmd = quote(compiler) + " --no-such-flag > "
            + quote((outputBase / "unknown_flag.txt").string())
            + " 2>&1";
        int const rc = std::system(shellWrap(cmd).c_str());
        // `std::system` exit-code encoding varies across hosts
        // (POSIX shifts; Windows passes through). Either form
        // satisfies "non-zero" — that's what we pin.
        check("Exit code indicates rejection",
              rc != 0,
              "got " + std::to_string(rc));
        std::ifstream f((outputBase / "unknown_flag.txt").string());
        std::string body((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
        check("Mentions 'unknown flag'",
              body.find("unknown flag") != std::string::npos,
              "stderr: " + body);
    }
    std::cout << "\n";

    // ── Test 3: Examples corpus via CLI subprocess ──
    runAllExamples(compiler, examplesRoot, outputBase);

    std::cout << "===========================================\n"
              << "Results: " << passes << " passed, "
              << failures << " failed\n";

    fs::remove_all(outputBase);

    return failures > 0 ? 1 : 0;
}
