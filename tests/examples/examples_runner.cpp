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
#include "core/types/source_buffer.hpp"
#include "opt/optimizer.hpp"
#include "program/program.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
  #include <sys/resource.h>  // RLIMIT_STACK assertion in the harness-wiring pin
#endif

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
    // C11/C23 6.4.5 (wchar_platform_width): optional PER-TARGET expected exit code.
    // When present it OVERRIDES the manifest-level `exitCode` for THIS target only —
    // needed when one source returns a platform-divergent value (e.g.
    // `sizeof(wchar_t)` is 2 on pe64 but 4 on elf/mach-o). Absent ⇒ the manifest
    // `exitCode` applies (existing behavior). The differential (optimized) arm still
    // compares to the baseline, so it follows this per-target choice for free.
    std::optional<std::int64_t>  exitCodeOverride;
};

// D-OPT1-DIFFERENTIAL-VERIFY-RUNNER (OPT2 cycle 1): a per-manifest
// declaration of an OPTIMIZED arm whose binary must produce the
// SAME exit code + stdout as the baseline (Identity-only) arm. The
// 5 corpus negative pins (dce_negative_pin, const_fold_inside_expr,
// copy_prop_across_join, licm_conditional_mutation, cse_noncommutative)
// list the buggy-opt exit codes they would produce — making any
// regression bisectable via the diff between the two arms.
// An arm declares EXACTLY ONE OF:
//   * `passes`: an inline PassId-name array (resolved via
//     optPassIdFromName) → an ad-hoc pipeline (maxIterations 1,
//     default inlineThreshold), OR
//   * `shippedPipeline`: the NAME of a shipped config
//     (src/dss-config/pipelines/<name>.pipeline.json) → the arm runs
//     the SHIPPED pipeline ITSELF (name/maxIterations/inlineThreshold
//     loaded from the file — ZERO drift between corpus + shipped config).
// `buildPipeline` enforces the exactly-one-of (both / neither → fail).
struct OptimizedArm {
    std::string                  label;   // diagnostic-rendering name (free-form)
    std::vector<std::string>     passes;  // PassId names (inline-array form)
    bool                         hasPasses = false;          // `passes` key present
    std::optional<std::string>   shippedPipeline;            // shipped-config form
};

// V2-4 Part C (D-DIAG-CLI-POSITION-RENDER-AND-ASSERT): one declared
// expected diagnostic for an EXPECT-ERROR example. `code` is the
// DiagnosticCode NAME (e.g. "S_UndeclaredIdentifier"); line/col are the
// 1-based start position the compiler's own `SourceBuffer::lineCol` must
// resolve the diagnostic's span to. This is the driver/e2e-tier twin of
// the analyzer-tier golden harness — it pins a SPECIFIC diagnostic at a
// SPECIFIC location through the full `Program::compileFiles` path.
struct ExpectedDiagnostic {
    std::string   code;
    std::uint32_t line = 0;
    std::uint32_t col  = 0;
    // D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN (#4): false ⇒ the diagnostic is emitted
    // at a tier with NO source span (e.g. `L_OverAlignedStackLocal` from the LIR
    // calling-convention frame layout). Such a diagnostic's default span resolves
    // to 1:1, but pinning a fabricated coordinate would be dishonest — instead the
    // set-equality below matches the CODE only for these (position-independent).
    // Default true = a real positioned diagnostic (every existing expect-error
    // example). Parsed from an optional `"positioned": false` manifest key.
    bool          positioned = true;
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
    // V2-4 Part C: when NON-EMPTY this is an EXPECT-ERROR example — the
    // source is malformed, the compile MUST fail, and the produced
    // diagnostics MUST equal this declared set EXACTLY (code + 1-based
    // line:col). Mutually exclusive with the run path: no binary is
    // spawned and `exitCode` is not required.
    std::vector<ExpectedDiagnostic> expectDiagnostics;
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
    // V2-4 Part C: parse the expect-error diagnostics FIRST — their
    // presence makes this an error manifest, which relaxes the exitCode
    // requirement (no binary is produced or run).
    if (j.contains("expectDiagnostics")) {
        if (!j.at("expectDiagnostics").is_array() || j.at("expectDiagnostics").empty()) {
            ADD_FAILURE() << "manifest " << path.generic_string()
                          << " 'expectDiagnostics' must be a non-empty array";
            return m;
        }
        for (auto const& d : j.at("expectDiagnostics")) {
            if (!d.is_object()
                || !d.contains("code") || !d.at("code").is_string()
                || !d.contains("line") || !d.at("line").is_number_unsigned()
                || !d.contains("col")  || !d.at("col").is_number_unsigned()) {
                ADD_FAILURE() << "manifest " << path.generic_string()
                              << " each expectDiagnostics entry needs string 'code'"
                                 " + unsigned 'line' + unsigned 'col'";
                return m;
            }
            ExpectedDiagnostic ed;
            ed.code = d.at("code").get<std::string>();
            ed.line = d.at("line").get<std::uint32_t>();
            ed.col  = d.at("col").get<std::uint32_t>();
            // #4: optional — a span-less-tier diagnostic is matched by code only.
            if (d.contains("positioned")) {
                if (!d.at("positioned").is_boolean()) {
                    ADD_FAILURE() << "manifest " << path.generic_string()
                                  << " expectDiagnostics 'positioned' must be a boolean";
                    return m;
                }
                ed.positioned = d.at("positioned").get<bool>();
            }
            m.expectDiagnostics.push_back(std::move(ed));
        }
    }
    // exitCode: required for a run example; not for an expect-error one.
    if (j.contains("exitCode")) {
        if (!j.at("exitCode").is_number_integer()) {
            ADD_FAILURE() << "manifest " << path.generic_string()
                          << " 'exitCode' must be an integer";
            return m;
        }
        m.exitCode = j.at("exitCode").get<std::int64_t>();
    } else if (m.expectDiagnostics.empty()) {
        ADD_FAILURE() << "manifest " << path.generic_string()
                      << " missing integer 'exitCode' (required unless"
                         " 'expectDiagnostics' is present)";
        return m;
    }
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
        // C11/C23 6.4.5: optional per-target exit-code override (an integer) — a
        // source whose return value is platform-divergent (e.g. sizeof(wchar_t)).
        if (t.contains("exitCode")) {
            if (!t.at("exitCode").is_number_integer()) {
                ADD_FAILURE() << "manifest " << path.generic_string()
                              << " target 'exitCode' must be an integer";
                return m;
            }
            et.exitCodeOverride = t.at("exitCode").get<std::int64_t>();
        }
        m.targets.push_back(std::move(et));
    }
    // D-OPT1-DIFFERENTIAL-VERIFY-RUNNER. Manifest shape — each arm
    // declares EXACTLY ONE OF `passes` (inline array) or `shippedPipeline`
    // (a config name); the exactly-one-of is enforced in `buildPipeline`:
    //   "optimizedPipelines": [
    //     {"label": "constfold-only", "passes": ["ConstFold"]},
    //     {"label": "release", "shippedPipeline": "release"}
    //   ]
    if (j.contains("optimizedPipelines")) {
        if (!j.at("optimizedPipelines").is_array()) {
            ADD_FAILURE() << "manifest " << path.generic_string()
                          << " 'optimizedPipelines' must be an array";
            return m;
        }
        for (auto const& arm : j.at("optimizedPipelines")) {
            if (!arm.is_object()
                || !arm.contains("label") || !arm.at("label").is_string()) {
                ADD_FAILURE() << "manifest " << path.generic_string()
                              << " each optimizedPipelines entry needs string"
                                 " 'label' + exactly one of 'passes' /"
                                 " 'shippedPipeline'";
                return m;
            }
            OptimizedArm oa;
            oa.label = arm.at("label").get<std::string>();
            if (arm.contains("passes")) {
                if (!arm.at("passes").is_array()) {
                    ADD_FAILURE() << "manifest " << path.generic_string()
                                  << " optimizedPipelines 'passes' must be an"
                                     " array";
                    return m;
                }
                oa.hasPasses = true;
                for (auto const& p : arm.at("passes")) {
                    if (!p.is_string()) {
                        ADD_FAILURE() << "manifest " << path.generic_string()
                                      << " optimizedPipelines.passes entries"
                                         " must be strings";
                        return m;
                    }
                    oa.passes.push_back(p.get<std::string>());
                }
            }
            if (arm.contains("shippedPipeline")) {
                if (!arm.at("shippedPipeline").is_string()) {
                    ADD_FAILURE() << "manifest " << path.generic_string()
                                  << " optimizedPipelines 'shippedPipeline'"
                                     " must be a string";
                    return m;
                }
                oa.shippedPipeline = arm.at("shippedPipeline").get<std::string>();
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
    // EXACTLY-ONE-OF `passes` / `shippedPipeline` — fail loud on both or
    // neither (a manifest typo must surface, never silently pick a
    // default). `hasPasses` (not `passes`-non-empty) is the presence
    // signal: an empty inline `passes: []` is still "the inline form".
    bool const hasPasses  = arm.hasPasses;
    bool const hasShipped = arm.shippedPipeline.has_value();
    if (hasPasses == hasShipped) {
        ADD_FAILURE() << "manifest " << manifestPath.generic_string()
                      << ": optimizedPipelines arm '" << arm.label
                      << "' must declare EXACTLY ONE OF 'passes' or"
                         " 'shippedPipeline' (got "
                      << (hasPasses ? "both" : "neither") << ")";
        return std::nullopt;
    }

    // Shipped-config form: load the pipeline ITSELF (name/maxIterations/
    // inlineThreshold from the file — ZERO drift between the corpus arm
    // and the shipped configuration it claims to exercise).
    if (hasShipped) {
        auto loaded = ::dss::opt::loadShippedPipeline(*arm.shippedPipeline);
        if (!loaded.has_value()) {
            std::ostringstream diag;
            for (auto const& d : loaded.error()) diag << "\n    " << d.message;
            ADD_FAILURE() << "manifest " << manifestPath.generic_string()
                          << ": arm '" << arm.label
                          << "' shippedPipeline '" << *arm.shippedPipeline
                          << "' failed to load:" << diag.str();
            return std::nullopt;
        }
        return std::move(*loaded);
    }

    // Inline-array form: resolve each PassId name.
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
    // Mirror the EXAMPLE DIR's file neighborhood into the scratch dir
    // (every regular file except the manifest) — not just the declared
    // sources. The CLI-subprocess runner (integrated_tests) compiles IN
    // the example dir, where a quote include resolves against the
    // includer's directory; the in-process scratch must offer the same
    // neighbor files or an include-bearing example (e.g.
    // include_typedef_cast's myint.h) false-fails here only. The entry
    // files passed to the driver stay exactly `m.sources`. CONTRACT: the
    // mirror is the IMMEDIATE dir only (no subdirectories) — an example
    // whose includes live in a subdir needs this loop made recursive
    // (recreating relative paths) in the same change.
    for (auto const& entry : fs::directory_iterator(exampleDir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().filename() == "expected.json") continue;
        fs::copy_file(entry.path(),
                      scratch.path() / entry.path().filename(),
                      fs::copy_options::overwrite_existing);
    }
    // Every DECLARED source must have been among the copied files — a
    // manifest typo fails loud here, not as a confusing driver error.
    std::vector<std::string> srcPaths;
    srcPaths.reserve(m.sources.size());
    for (auto const& s : m.sources) {
        auto const sp = scratch.path() / s;
        if (!fs::exists(sp)) {
            ADD_FAILURE() << "manifest source '" << s
                          << "' not found in example dir "
                          << exampleDir.generic_string();
            armResult.status = ArmStatus::Poisoned;
            return armResult;
        }
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
    // The per-target exit-code override (when present) is the authority for THIS
    // target; otherwise the manifest-level `exitCode`. The differential arm below
    // compares the optimized arm to the BASELINE exit code, so it follows this
    // choice for free.
    std::int64_t const effectiveExit =
        t.exitCodeOverride.has_value() ? *t.exitCodeOverride : m.exitCode;

    // Baseline strict pins against the manifest (per-target override applied).
    ASSERT_EQ(static_cast<std::int64_t>(baseline.exitCode), effectiveExit)
        << "baseline exit-code mismatch (expected=" << effectiveExit
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

// V2-4 Part C (D-DIAG-CLI-POSITION-RENDER-AND-ASSERT): drive the FULL
// Program::compileFiles path on a malformed source and assert the EXACT
// positioned diagnostic set. The compile MUST fail; no binary is spawned.
// AGNOSTIC: the expected diagnostics are JSON-declared and compared
// generically (code NAME + 1-based line:col) — no language/code hardcoded.
void runErrorTarget(fs::path const&        exampleDir,
                    ExampleManifest const& m,
                    ExampleTarget const&   t) {
    ASSERT_FALSE(t.spec.empty())
        << "expect-error target needs a 'spec' to drive the compile";
    // Single-source keeps position resolution unambiguous: one source
    // buffer, so a diagnostic's span offset maps to exactly this file.
    ASSERT_EQ(m.sources.size(), 1u)
        << "expectDiagnostics examples must be single-source";

    ScratchDir scratch{Location::InsideRepo, "examples"};
    auto const srcRel  = m.sources.front();
    auto const srcPath = scratch.path() / srcRel;
    fs::copy_file(exampleDir / srcRel, srcPath,
                  fs::copy_options::overwrite_existing);
    scratch.useAsCwd();

    Program            prog;
    DiagnosticReporter rep;
    prog.setOutputDir(scratch.path() / "out");
    int const rc =
        prog.compileFiles({srcPath.generic_string()}, m.language, {t.spec}, rep);

    // A malformed source MUST be rejected — both the driver return code and
    // the error-severity count confirm the compile failed (not a silent pass).
    EXPECT_NE(rc, 0)
        << "expect-error example compiled with rc=0 (should be rejected): "
        << exampleDir.generic_string();
    EXPECT_GT(rep.errorCount(), 0u)
        << "expect-error example produced no error-severity diagnostics: "
        << exampleDir.generic_string();

    // Resolve every diagnostic's START position through the SAME
    // SourceBuffer::lineCol the compiler uses, so the asserted 1-based
    // line:col matches its convention exactly (read binary so the byte
    // offsets line up with the compiler's own buffer).
    std::ifstream in(srcPath, std::ios::binary);
    std::string const srcBytes{std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>()};
    auto const srcBuf = SourceBuffer::fromString(srcBytes, srcRel);

    // #4: codes declared `positioned:false` are emitted at a span-less tier, so
    // their position is not asserted — both sides render them code-only. Every
    // other code keeps its precise `code line:col` pin.
    std::set<std::string> codeOnly;
    for (auto const& e : m.expectDiagnostics)
        if (!e.positioned) codeOnly.insert(e.code);

    auto const render = [&codeOnly](std::string_view code,
                                    std::uint32_t line, std::uint32_t col) {
        std::string s(code);
        if (codeOnly.count(s) != 0) return s;   // position-independent match
        s += ' ';
        s += std::to_string(line);
        s += ':';
        s += std::to_string(col);
        return s;
    };

    std::vector<std::string> actual;
    actual.reserve(rep.all().size());
    for (auto const& d : rep.all()) {
        auto const lc = srcBuf->lineCol(d.span.start());
        actual.push_back(render(diagnosticCodeName(d.code), lc.line, lc.column));
    }
    std::vector<std::string> expected;
    expected.reserve(m.expectDiagnostics.size());
    for (auto const& e : m.expectDiagnostics) {
        expected.push_back(render(e.code, e.line, e.col));
    }
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());

    auto const join = [](std::vector<std::string> const& v) {
        std::string s;
        for (auto const& x : v) { s += "\n    "; s += x; }
        return s;
    };
    // EXACT set equality — the strongest pin (the user's strict-assert
    // invariant): every produced diagnostic is declared and vice versa,
    // each at its precise positioned line:col.
    ASSERT_EQ(actual, expected)
        << "expect-error diagnostic-set mismatch for "
        << exampleDir.generic_string()
        << "\n  expected:" << join(expected)
        << "\n  actual:"   << join(actual);
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
        // V2-4 Part C: an `expectDiagnostics` manifest asserts a failed
        // compile + positioned diagnostics; otherwise the source must
        // compile + run cleanly.
        if (m.expectDiagnostics.empty()) {
            runOneTarget(exampleDir, m, t);
        } else {
            runErrorTarget(exampleDir, m, t);
        }
    }
}

// D-ASM-AARCH64-FRAME-OFFSET-BEYOND-16MIB (harness-wiring pin): the shared
// spawn chokepoint `runBinary` must apply the generous-stack bump so BOTH
// harnesses that spawn through it — this in-process runner AND the separate
// `integrated_tests` CLI-subprocess runner — inherit it. The integrated_tests
// runner originally LACKED the bump, so the native arm64-Linux leg SIGSEGV'd
// (exit 139) on the ~20 MB-frame `large_frame_beyond_16mib` example, while
// every other leg skipped that target as cross-arch/cross-format and stayed
// green. This pin is the EVERY-LEG wiring guard (the native arm64 leg running
// the example to exit 42 is the runtime witness for the load-bearing rlimit
// raise). Red-on-disable: drop the `ensureGenerousSpawnStack()` call in
// `runBinary` → QEMU_STACK_SIZE unset here AND the native large-frame run
// crashes on the arm64 leg.
TEST(RunHarnessStack, GenerousSpawnStackBumpIsWired) {
    dss::test_support::ensureGenerousSpawnStack();

    char const* const qss = std::getenv("QEMU_STACK_SIZE");
    ASSERT_NE(qss, nullptr)
        << "the qemu cross-arch corpus path needs QEMU_STACK_SIZE set";
    EXPECT_STREQ(qss, "268435456");

#if !defined(_WIN32)
    // The load-bearing mechanism for the NATIVE large-frame run: the parent's
    // RLIMIT_STACK soft limit is raised toward 256 MiB so a posix_spawn child
    // inherits a stack large enough for the ~20 MB frame.
    struct rlimit rl{};
    ASSERT_EQ(::getrlimit(RLIMIT_STACK, &rl), 0);
    rlim_t const want = static_cast<rlim_t>(268435456);  // 256 MiB
    rlim_t const atLeast =
        (rl.rlim_max == RLIM_INFINITY) ? want
                                       : std::min<rlim_t>(want, rl.rlim_max);
    EXPECT_GE(rl.rlim_cur, atLeast)
        << "RLIMIT_STACK soft limit must be raised toward 256 MiB";
#endif
}
