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
#include "opt/optimizer.hpp"
#include "program/program.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
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

// D-LK10-ENTRY-ARM64 (v0.0.2 V2-1): host ARCH detection, returning the
// SAME identifier strings the target configs use as their `name` (so a
// spec's target prefix can be compared directly). Used by the cross-
// arch run gate: a binary whose target arch differs from the host's
// needs an emulator.
[[nodiscard]] std::string currentHostArch() noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "unknown";
#endif
}

// The target portion of a manifest spec ("arm64:elf64-aarch64-linux-exec"
// → "arm64"). This IS the arch identifier compared against
// currentHostArch().
[[nodiscard]] std::string specTargetArch(std::string const& spec) {
    auto const colon = spec.find(':');
    return colon == std::string::npos ? spec : spec.substr(0, colon);
}

// Resolve an executable name against $PATH (e.g. "qemu-aarch64"),
// returning its full path or "" if not found. Used to gate cross-arch
// example runs on emulator availability — absent ⇒ SkippedCrossHost
// (never a test failure). AGNOSTIC: the emulator name is supplied by
// the manifest, not hardcoded here.
[[nodiscard]] std::string findOnPath(std::string const& exe) {
    char const* const pathEnv = std::getenv("PATH");
    if (pathEnv == nullptr) return {};
#if defined(_WIN32)
    char const sep = ';';
    std::vector<std::string> const exts = {"", ".exe"};
#else
    char const sep = ':';
    std::vector<std::string> const exts = {""};
#endif
    std::string const path = pathEnv;
    std::size_t start = 0;
    while (start <= path.size()) {
        auto const end = path.find(sep, start);
        std::string const dir = path.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty()) {
            for (auto const& ext : exts) {
                fs::path const cand = fs::path(dir) / (exe + ext);
                std::error_code ec;
                if (fs::exists(cand, ec) && fs::is_regular_file(cand, ec)) {
                    return cand.generic_string();
                }
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

struct ExampleTarget {
    std::string                  spec;
    std::string                  artifact;
    std::vector<std::string>     runOn;  // host OS names allowed to spawn
    // D-LK10-ENTRY-ARM64: optional emulator command (e.g.
    // "qemu-aarch64") used when the target arch differs from the host
    // arch. Empty ⇒ native execution only (the pre-V2-1 default).
    std::string                  emulator;
    // Model 3 (2026-06-09): optional PER-TARGET expected stdout. When present it
    // OVERRIDES the manifest-level `expectedStdout` for THIS target only — needed
    // when one source prints platform-divergent bytes (e.g. `puts` emits
    // "hello\r\n" via Windows msvcrt CRLF translation but "hello\n" on
    // linux/macos). Absent ⇒ the manifest-level pin applies (existing behavior).
    std::optional<std::string>   expectedStdoutOverride;
};

// D-OPT1-DIFFERENTIAL-VERIFY-RUNNER (OPT2 cycle 1): a per-manifest
// declaration of an OPTIMIZED arm whose binary must produce the
// SAME exit code + stdout as the baseline (Identity-only) arm. The
// 5 corpus negative pins (dce_negative_pin, const_fold_inside_expr,
// copy_prop_across_join, licm_conditional_mutation, cse_noncommutative)
// list the buggy-opt exit codes they would produce — making any
// regression bisectable via the diff between the two arms.
struct OptimizedArm {
    std::string                  label;   // diagnostic-rendering name (free-form)
    std::vector<std::string>     passes;  // PassId names (resolved via optPassIdFromName)
};

struct ExampleManifest {
    std::string                  language;
    std::string                  source;
    // Multi-CU (CU6): "sources":[...] makes each file its OWN CompilationUnit, and the
    // linker MERGES them into one image (Program::compileUnits). The single "source":"x.c"
    // form stays one CU5 multi-file unit (compileFiles). `sources` is the canonical file
    // list for either form; `multiCu` selects the driver entry. Cross-file references in a
    // multi-CU example resolve at LINK time (a sibling CU's definition or a library).
    std::vector<std::string>     sources;
    bool                         multiCu = false;
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
    // Optional differential-verify arms. Each compiles the same
    // source with the listed pipeline + asserts baseline-equal
    // exit code + stdout.
    std::vector<OptimizedArm>    optimizedPipelines;
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
    // "sources":[...] (multi-CU, CU6) takes precedence over "source" (single CU5 unit).
    if (j.contains("sources")) {
        if (!j.at("sources").is_array() || j.at("sources").empty()) {
            ADD_FAILURE() << "manifest " << path.generic_string()
                          << " 'sources' must be a non-empty array of file names";
            return m;
        }
        for (auto const& s : j.at("sources")) {
            if (!s.is_string()) {
                ADD_FAILURE() << "manifest " << path.generic_string()
                              << " 'sources' entries must be strings";
                return m;
            }
            m.sources.push_back(s.get<std::string>());
        }
        m.multiCu = true;
    } else if (!m.source.empty()) {
        m.sources = {m.source};  // single-CU: the one "source" file
    } else {
        ADD_FAILURE() << "manifest " << path.generic_string()
                      << " requires 'source' (single CU) or 'sources' (multi-CU)";
        return m;
    }
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
        et.emulator = t.value("emulator", "");
        if (t.contains("runOn") && t.at("runOn").is_array()) {
            for (auto const& s : t.at("runOn")) {
                if (s.is_string()) et.runOn.push_back(s.get<std::string>());
            }
        }
        // Model 3: optional per-target stdout override (a string; empty-string is
        // a VALID pin — asserts the binary printed nothing on this target).
        if (t.contains("expectedStdout")) {
            if (!t.at("expectedStdout").is_string()) {
                ADD_FAILURE() << "manifest " << path.generic_string()
                              << " target 'expectedStdout' must be a string";
                return m;
            }
            et.expectedStdoutOverride = t.at("expectedStdout").get<std::string>();
        }
        m.targets.push_back(std::move(et));
    }
    // D-OPT1-DIFFERENTIAL-VERIFY-RUNNER. Manifest shape:
    //   "optimizedPipelines": [
    //     {"label": "constfold-only", "passes": ["ConstFold"]}
    //   ]
    if (j.contains("optimizedPipelines")) {
        if (!j.at("optimizedPipelines").is_array()) {
            ADD_FAILURE() << "manifest " << path.generic_string()
                          << " 'optimizedPipelines' must be an array";
            return m;
        }
        for (auto const& arm : j.at("optimizedPipelines")) {
            if (!arm.is_object() ||
                !arm.contains("label") || !arm.at("label").is_string() ||
                !arm.contains("passes") || !arm.at("passes").is_array()) {
                ADD_FAILURE() << "manifest " << path.generic_string()
                              << " each optimizedPipelines entry needs string"
                                 " 'label' + array 'passes'";
                return m;
            }
            OptimizedArm oa;
            oa.label = arm.at("label").get<std::string>();
            for (auto const& p : arm.at("passes")) {
                if (!p.is_string()) {
                    ADD_FAILURE() << "manifest " << path.generic_string()
                                  << " optimizedPipelines.passes entries must"
                                     " be strings";
                    return m;
                }
                oa.passes.push_back(p.get<std::string>());
            }
            m.optimizedPipelines.push_back(std::move(oa));
        }
    }
    return m;
}

// Resolve a list of pass-name strings into an OptPipeline. Fails the
// test loud (ADD_FAILURE) if any name is unrecognised — surfaces typos
// in manifests at runtime + catches drift between the JSON vocab and
// the PassId enum.
[[nodiscard]] std::optional<::dss::opt::OptPipeline>
buildPipeline(OptimizedArm const& arm, fs::path const& manifestPath) {
    ::dss::opt::OptPipeline p;
    p.name = arm.label;
    p.passes.reserve(arm.passes.size());
    for (auto const& name : arm.passes) {
        auto resolved = ::dss::opt::optPassIdFromName(name);
        if (!resolved.has_value()) {
            ADD_FAILURE() << "manifest " << manifestPath.generic_string()
                          << ": unknown PassId '" << name << "' in arm '"
                          << arm.label << "'";
            return std::nullopt;
        }
        p.passes.push_back(*resolved);
    }
    return p;
}

// Per-arm compile+spawn outcome. The tri-state distinguishes the
// three reasons the caller might receive no exit-code/stdout pair
// to compare. Conflating them (the original `skipped: bool` shape)
// would let a baseline that secretly failed to compile produce a
// silently-bypassed differential-verify assertion.
enum class ArmStatus {
    Ran,              // compile + spawn succeeded; exitCode + capturedStdout are populated
    SkippedCrossHost, // compile succeeded; binary exists; runOn excludes this host (expected)
    Poisoned,         // compile failed OR artifact missing — every EXPECT_ already fired
};

struct ArmResult {
    ArmStatus   status = ArmStatus::Poisoned;
    int         exitCode = 0;
    std::string capturedStdout;
};

[[nodiscard]] ArmResult
compileAndRunArm(fs::path const& exampleDir,
                 ExampleManifest const& m,
                 ExampleTarget const& t,
                 ::dss::opt::OptPipeline const* pipelineOverride,
                 char const* armLabel) {
    SCOPED_TRACE(std::string{"arm="} + armLabel);
    ArmResult armResult;
    ScratchDir scratch{Location::InsideRepo, "examples"};
    // Copy EVERY source file into the scratch dir (one file for the single-CU form, N for
    // a multi-CU example). The first source names the artifact stem.
    std::vector<std::string> srcPaths;
    srcPaths.reserve(m.sources.size());
    for (auto const& s : m.sources) {
        auto const sp = scratch.path() / s;
        fs::copy_file(exampleDir / s, sp, fs::copy_options::overwrite_existing);
        srcPaths.push_back(sp.generic_string());
    }
    scratch.useAsCwd();
    auto const outDir = scratch.path() / "out";

    Program            prog;
    DiagnosticReporter rep;
    prog.setOutputDir(outDir);
    if (pipelineOverride != nullptr) {
        prog.setOptimizerPipelineOverride(*pipelineOverride);
    }
    // Multi-CU example → compileUnits (one CU per file, merged at link); single → compileFiles.
    int const rc = m.multiCu
        ? prog.compileUnits(srcPaths, m.language, {t.spec}, rep)
        : prog.compileFiles(srcPaths, m.language, {t.spec}, rep);

    // Strict compile-side checks. EXPECT_ (not ASSERT_) because the
    // helper is non-void and must hand control back to the caller's
    // arm-comparison logic via a poisoned ArmResult on failure.
    std::ostringstream diagDump;
    for (auto const& d : rep.all()) {
        diagDump << "\n  " << diagnosticCodeName(d.code)
                 << " (severity=" << static_cast<int>(d.severity)
                 << "): " << d.actual;
    }
    EXPECT_EQ(rc, 0)
        << "compile failed for spec=" << t.spec
        << " arm=" << armLabel
        << " example=" << exampleDir.generic_string()
        << " diagnostics:" << diagDump.str();
    EXPECT_EQ(rep.errorCount(), 0u)
        << "expected zero error-severity diagnostics for spec="
        << t.spec << " arm=" << armLabel << diagDump.str();
    if (rc != 0 || rep.errorCount() != 0u) {
        armResult.status = ArmStatus::Poisoned;
        return armResult;
    }

    auto const artifactPath = outDir / t.artifact;
    if (!fs::exists(artifactPath)) {
        ADD_FAILURE() << "artifact missing at " << artifactPath.generic_string()
                      << " (arm=" << armLabel << ")";
        armResult.status = ArmStatus::Poisoned;
        return armResult;
    }
    // Use the no-throw overload — a non-existent path past the
    // EXPECT above would otherwise throw filesystem_error and abort
    // the gtest process rather than poison the arm cleanly. Per
    // [fs.op.file_size], on error sz is `static_cast<uintmax_t>(-1)`
    // = UINT64_MAX, so an unguarded `EXPECT_GT(sz, 0u)` would
    // spuriously PASS on file_size failure. Gate on sz_ec to make
    // both checks honest.
    std::error_code sz_ec;
    auto const sz = fs::file_size(artifactPath, sz_ec);
    if (sz_ec) {
        ADD_FAILURE() << "file_size failed: " << sz_ec.message()
                      << " (arm=" << armLabel << ")";
        armResult.status = ArmStatus::Poisoned;
        return armResult;
    }
    if (sz == 0u) {
        ADD_FAILURE() << "artifact is empty: " << artifactPath.generic_string()
                      << " (arm=" << armLabel << ")";
        armResult.status = ArmStatus::Poisoned;
        return armResult;
    }

    auto const host = currentHostOs();
    bool const shouldRun = std::any_of(t.runOn.begin(), t.runOn.end(),
        [&](std::string const& s) { return s == host; });
    if (!shouldRun) {
        GTEST_LOG_(INFO) << "spec=" << t.spec
                        << " arm=" << armLabel
                        << " produced an artifact but runOn=["
                        << (t.runOn.empty() ? "" : t.runOn.front())
                        << "...] excludes host=" << host;
        armResult.status = ArmStatus::SkippedCrossHost;
        return armResult;
    }

    // D-LK10-ENTRY-ARM64: cross-ARCH execution. The runOn gate matched
    // the host OS; now reconcile the host ARCH. A binary whose target
    // arch differs from the host's cannot exec natively — it needs an
    // emulator (e.g. qemu-aarch64 for an AArch64 ELF on x86_64). The
    // emulator is declared per-target in the manifest; if it isn't on
    // PATH we SkippedCrossHost (never a failure — a host without the
    // emulator simply can't run the cross-arch binary; the compile
    // step already asserted clean on every host).
    std::vector<std::string> launcherPrefix;
    if (std::string const targetArch = specTargetArch(t.spec);
        !targetArch.empty() && targetArch != currentHostArch()) {
        if (t.emulator.empty()) {
            GTEST_LOG_(INFO) << "spec=" << t.spec << " arm=" << armLabel
                << " targets arch '" << targetArch << "' != host '"
                << currentHostArch() << "' with no emulator declared — "
                   "skipping run";
            armResult.status = ArmStatus::SkippedCrossHost;
            return armResult;
        }
        auto const emuPath = findOnPath(t.emulator);
        if (emuPath.empty()) {
            GTEST_LOG_(INFO) << "spec=" << t.spec << " arm=" << armLabel
                << " emulator '" << t.emulator
                << "' not found on PATH — skipping cross-arch run";
            armResult.status = ArmStatus::SkippedCrossHost;
            return armResult;
        }
        launcherPrefix.push_back(emuPath);
    }

    // Model 3: capture stdout when EITHER the manifest-level pin OR this target's
    // override is present (so a per-target override alone still routes the pipe).
    bool const captureStdout =
        m.expectedStdout.has_value() || t.expectedStdoutOverride.has_value();
    auto const result = runBinary(artifactPath,
                                  std::chrono::milliseconds{5000},
                                  captureStdout,
                                  launcherPrefix);
    EXPECT_TRUE(result.spawned)
        << "spawn failed for " << artifactPath.generic_string()
        << " (arm=" << armLabel << ") diag=" << result.diagnostic;
    EXPECT_FALSE(result.timedOut)
        << "spawn timed out for " << artifactPath.generic_string()
        << " (arm=" << armLabel << ") diag=" << result.diagnostic;
    // Poison the arm if spawn failed or timed out — the binary never
    // actually ran, so result.exitCode + result.capturedStdout are
    // garbage. Without this, the differential-verify ASSERT_EQ would
    // compare two arms' bogus exit codes and could spuriously pass
    // when both arms time out (silent-bypass re-opens the very gap
    // the ArmStatus tri-state was introduced to close).
    if (!result.spawned || result.timedOut) {
        armResult.status = ArmStatus::Poisoned;
        return armResult;
    }
    armResult.status         = ArmStatus::Ran;
    armResult.exitCode       = result.exitCode;
    armResult.capturedStdout = result.capturedStdout;
    return armResult;
}

void runOneTarget(fs::path const&        exampleDir,
                  ExampleManifest const& m,
                  ExampleTarget const&   t) {
    ASSERT_FALSE(t.spec.empty())
        << "target spec missing in manifest";
    ASSERT_FALSE(t.artifact.empty())
        << "artifact filename missing in target";

    // Baseline arm: no pipeline override; compile_pipeline picks the
    // default. Two non-Ran outcomes are distinguished:
    //   Poisoned → compile failed; EXPECT already fired; return.
    //   SkippedCrossHost → compile clean; binary won't run on this
    //                      host; arm-comparison is N/A → return.
    auto const baseline = compileAndRunArm(exampleDir, m, t,
                                           /*pipelineOverride*/ nullptr,
                                           "baseline");
    if (baseline.status != ArmStatus::Ran) {
        return;
    }

    // Model 3: the per-target override (when present) is the authority for THIS
    // target's stdout; otherwise the manifest-level pin applies. The differential
    // arm below compares the optimized arm to the BASELINE's captured stdout, so it
    // follows this choice for free.
    std::optional<std::string> const effectiveStdout =
        t.expectedStdoutOverride.has_value() ? t.expectedStdoutOverride
                                             : m.expectedStdout;

    // Baseline strict pins against the manifest.
    ASSERT_EQ(static_cast<std::int64_t>(baseline.exitCode), m.exitCode)
        << "baseline exit-code mismatch (manifest=" << m.exitCode
        << "; OS=" << baseline.exitCode << ")";
    if (effectiveStdout.has_value()) {
        ASSERT_EQ(baseline.capturedStdout, *effectiveStdout)
            << "baseline stdout mismatch (expected=" << effectiveStdout->size()
            << " bytes; OS=" << baseline.capturedStdout.size() << " bytes)";
    }

    // D-OPT1-DIFFERENTIAL-VERIFY-RUNNER: each declared optimized arm
    // produces an artifact whose exit code + stdout MUST match the
    // baseline. Corpus negative pins (plan 22 §3.1) drive this — a
    // broken pass produces divergent output and the assert names
    // the pipeline.
    for (auto const& arm : m.optimizedPipelines) {
        SCOPED_TRACE("optimizedPipeline=" + arm.label);
        auto const pipeline = buildPipeline(arm, exampleDir / "expected.json");
        if (!pipeline.has_value()) continue;  // ADD_FAILURE already fired
        auto const optResult = compileAndRunArm(exampleDir, m, t, &*pipeline,
                                                 arm.label.c_str());
        // The two arms must share the runOn outcome: both runOn-true
        // (compare) or both runOn-false (skip). A SkippedCrossHost
        // baseline already returned above; here the optimized arm
        // must agree.
        ASSERT_EQ(static_cast<int>(optResult.status),
                  static_cast<int>(ArmStatus::Ran))
            << "differential-verify: optimized arm '" << arm.label
            << "' status=" << static_cast<int>(optResult.status)
            << " — baseline ran but optimized arm did not";
        ASSERT_EQ(optResult.exitCode, baseline.exitCode)
            << "differential-verify FAIL: optimized arm '" << arm.label
            << "' produced exit code " << optResult.exitCode
            << " vs baseline " << baseline.exitCode
            << " — pipeline regression";
        if (effectiveStdout.has_value()) {
            ASSERT_EQ(optResult.capturedStdout, baseline.capturedStdout)
                << "differential-verify FAIL: optimized arm '" << arm.label
                << "' stdout differs from baseline";
        }
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
