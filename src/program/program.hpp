#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "opt/optimizer.hpp"
#include "program/cli_args.hpp"      // CompileConfig
#include "program/input_resolver.hpp"

#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <vector>

// D-PERF-4-CU-PARALLELISM: forward-declare the substrate executor so the
// injection surface below is a bare pointer member — no need to pull the
// thread-pool header (with <thread>/<condition_variable>) into every
// program.hpp consumer. The compile driver includes the full header.
namespace dss::substrate { class IExecutor; }

namespace dss {

// Route decision for a list of source files — the SINGLE source of
// truth for the multi-vs-single-CU threshold, shared by the CLI
// dispatcher (`Program::run`) and the project driver
// (`Program::compileProject`, plan 06 AP2) so they can never drift.
//
//   > 1 source ⇒ N independent translation units the LINKER merges
//     into one image (`compileUnits`, `cc a.c b.c` semantics, LK11);
//   ≤ 1 source ⇒ the single-CU path (`compileFiles`), where N==1 is
//     the degenerate case and a multi-file CU5 unit (cross-file refs
//     resolved WITHIN the unit) is the >1-on-compileFiles shape that
//     this routing deliberately avoids for the project driver.
//
// The decision keys ONLY on translation-unit COUNT — never on any
// language / CPU / format identity (the standing agnosticism veto).
[[nodiscard]] inline bool routesToMultiUnit(std::size_t sourceCount) noexcept {
    return sourceCount > 1;
}

// Human-readable wall-clock duration for the `--time` CLI flag. Sub-second →
// "623ms"; under a minute → "2.314s"; a minute or more → "2m31.231s". Pure +
// deterministic (unit-tested) so the non-deterministic timer VALUE is the only
// thing that varies at runtime. A universal driver concern — no lang/target/format.
[[nodiscard]] inline std::string formatWallTime(long long milliseconds) {
    if (milliseconds < 1000) return std::format("{}ms", milliseconds);
    double const s = static_cast<double>(milliseconds) / 1000.0;
    if (s < 60.0) return std::format("{:.3f}s", s);
    long long const m  = static_cast<long long>(s) / 60;
    double const rem = s - static_cast<double>(m) * 60.0;
    return std::format("{}m{:06.3f}s", m, rem);
}

class DSS_EXPORT Program {
public:
    Program() = default;
    ~Program() = default;

    /// Entry point for the CLI. Parses arguments and dispatches compilation.
    int run(int argc, char* argv[]);

    /// Compile a project file (.dss-project.json). `reporterConfig` threads
    /// `--warnings-as-errors` + `--suppress=<code>` through every tier.
    /// (LK10 cycle 3 post-fold #2: overload pair collapsed to single
    /// signature with default — code-simplifier REQUIRED.)
    int compileProject(
        const std::string& projectFilePath,
        DiagnosticReporter::Config const& reporterConfig = {}
    );

    /// Rep-injection overload — caller owns `rep` and may inspect it
    /// after return (the same testability pattern as `compileFiles`).
    /// The Config-taking overload constructs `rep` internally and
    /// forwards here. Lets the program test suite assert the EXACT
    /// driver-tier code the AP2 wiring emits (D_FileNotFound /
    /// C_MalformedJson / C_MissingField / D_SchemaLoadFailed /
    /// D_ArtifactProfileNotSupported), not merely the exit code.
    int compileProject(
        const std::string& projectFilePath,
        DiagnosticReporter&             rep
    );

    /// Compile explicit source files for a language to one or more
    /// targets. `reporterConfig` is applied run-wide.
    int compileFiles(
        const std::vector<std::string>& sourceFiles,
        const std::string& languageName,
        const std::vector<std::string>& targets,
        DiagnosticReporter::Config const& reporterConfig = {}
    );

    /// Rep-injection overload — caller owns `rep` and may inspect it
    /// after return. The Config-taking overload is a thin wrapper
    /// that constructs `rep` internally and forwards here.
    ///
    /// D-CAP-MARKER-MULTI-TARGET-E2E-PIN (eb2c6c7 audit-fold 2026-06-01):
    /// the single-chokepoint cap-marker contract cannot be pinned
    /// from outside without post-run reporter inspection — the
    /// original Track 3 test became structurally impossible when
    /// `D_TargetMachineCodeMismatch` joined `kUnsuppressableCodes`
    /// (cap-gates bypassed). This overload lets tests reach
    /// `rep.all()` / `countCode(rep, P_TooManyDiagnostics)` directly.
    int compileFiles(
        const std::vector<std::string>& sourceFiles,
        const std::string& languageName,
        const std::vector<std::string>& targets,
        DiagnosticReporter&             rep
    );

    /// Compile each source file as its OWN CompilationUnit (CU6 multi-CU model), then
    /// link the N CUs into ONE image per target — the linker merges them (LK11): a
    /// cross-file reference resolves to a sibling CU's definition or a library import at
    /// LINK time. Distinct from `compileFiles`, which builds ONE CU5 multi-file CU
    /// (cross-file refs resolved within the unit). Same overload shape as `compileFiles`.
    int compileUnits(
        const std::vector<std::string>& sourceFiles,
        const std::string& languageName,
        const std::vector<std::string>& targets,
        DiagnosticReporter::Config const& reporterConfig = {}
    );
    int compileUnits(
        const std::vector<std::string>& sourceFiles,
        const std::string& languageName,
        const std::vector<std::string>& targets,
        DiagnosticReporter&             rep
    );

    /// Compile every matching source file in a directory.
    /// `mode` selects recursive vs flat scan (D-LK10-1 closure axis).
    int compileDirectory(
        const std::string& directoryPath,
        const std::string& languageName,
        const std::vector<std::string>& targets,
        InputResolver::Mode mode = InputResolver::Mode::Recursive,
        DiagnosticReporter::Config const& reporterConfig = {}
    );

    /// Source-to-source transpilation entry point — plan 10 owns
    /// the actual translation engine (`*.map.json` + HIR→HIR walker
    /// + target-CST builder + pretty-printer). v1: fails loud with
    /// `D_PlanNotLanded` citing plan 10. Plan 10 ships the engine
    /// behind this API as ST1..ST6.
    int transpile(
        const std::vector<std::string>& sourceFiles,
        const std::string& languageName,
        const std::vector<std::string>& targets,
        DiagnosticReporter::Config const& reporterConfig = {}
    );

    /// `--output <dir>` (D-LK10-ENTRY Slice C companion): routes
    /// emitted binaries into the named directory. When set, the
    /// output-path convention becomes `<outputDir>/<binary>` for
    /// single-target builds and `<outputDir>/<formatName>/<binary>`
    /// for multi-target builds (to disambiguate same-named outputs
    /// across formats). When unset (the default), the existing
    /// `<cwd>/target/<formatName>/<binary>` convention applies.
    ///
    /// The driver auto-creates the output directory tree
    /// (`fs::create_directories`); failure surfaces as
    /// `D_OutputDirCreateFailed` (same code as the legacy
    /// `<cwd>/target/...` path's mkdir failure).
    void setOutputDir(std::optional<std::filesystem::path> dir) {
        outputDir_ = std::move(dir);
    }
    [[nodiscard]] std::optional<std::filesystem::path> const&
    outputDir() const noexcept { return outputDir_; }

    /// D-OPT1-DIFFERENTIAL-VERIFY-RUNNER (OPT2 cycle 1): override the
    /// MIR-optimizer pipeline for the next compileFiles/Directory call.
    /// When set, replaces the JSON-loaded default at compile_pipeline
    /// step 3.5. Used by the examples_runner's differential-verify arm
    /// + MIR unit tests; production callers leave it unset (the JSON
    /// registry resolves the pipeline by name from CompileConfig).
    void setOptimizerPipelineOverride(std::optional<::dss::opt::OptPipeline> p) {
        optimizerPipelineOverride_ = std::move(p);
    }
    [[nodiscard]] std::optional<::dss::opt::OptPipeline> const&
    optimizerPipelineOverride() const noexcept {
        return optimizerPipelineOverride_;
    }

    /// D-OPT1-PIPELINE-CONFIG-FROM-COMPILECONFIG: the build configuration.
    /// Debug → "debug" pipeline (no optimization); Release → "release"
    /// pipeline (full optimizer). `Program::run` stamps this from
    /// `CliArgs::config` before dispatching to `compileFiles`; tests
    /// can override directly.
    void setCompileConfig(CompileConfig c) noexcept { compileConfig_ = c; }
    [[nodiscard]] CompileConfig compileConfig() const noexcept { return compileConfig_; }

    /// c105 (D-PP-USER-DEFINE): the CLI `--define NAME[=VALUE]` entries,
    /// stamped from `CliArgs::defines` by `Program::run` before dispatch
    /// (the setOutputDir/setCompileConfig pattern). Every CU build threads
    /// them to the preprocessor's "<command-line>" prologue.
    void setUserDefines(std::vector<std::string> d) { userDefines_ = std::move(d); }
    [[nodiscard]] std::vector<std::string> const& userDefines() const noexcept {
        return userDefines_;
    }

    /// The CLI `-I<dir>` / `--include-dir <dir>` quote-include search path
    /// (the C 6.10.2 quote form), stamped from `CliArgs::includeDirs` by
    /// `Program::run` before dispatch (the setUserDefines pattern). Every CU
    /// build threads them onto the `UnitBuilder` via `addIncludeDir`.
    void setIncludeDirs(std::vector<std::string> d) { includeDirs_ = std::move(d); }
    [[nodiscard]] std::vector<std::string> const& includeDirs() const noexcept {
        return includeDirs_;
    }

    /// c162 (D-FF1-READER-CONSUMER): the `--resolve-library <path>` binaries
    /// whose export surfaces resolve + validate this run's source-declared
    /// externs. `Program::run` stamps this from `CliArgs::resolveLibraries`;
    /// the in-process round-trip harness (and tests) sets it directly before
    /// building the `main` that links against a DSS-built library. Threaded to
    /// `CompileOptions.resolveLibraries` at the per-target build.
    void setResolveLibraries(std::vector<std::filesystem::path> libs) {
        resolveLibraries_ = std::move(libs);
    }
    [[nodiscard]] std::vector<std::filesystem::path> const&
    resolveLibraries() const noexcept { return resolveLibraries_; }

    /// D-PERF-4-CU-PARALLELISM: inject an executor for the per-CU build loop.
    /// The N>1 path (`compileUnits`) builds every CU's MIR concurrently; each
    /// `buildCuMir` is a pure per-CU function (own interner/arenas/SemanticModel
    /// + a private scratch reporter), so the jobs share no mutable state and the
    /// driver merges their diagnostics back in CU (source) ORDER after the join
    /// — byte-deterministic regardless of thread scheduling. Tests inject a
    /// `SynchronousExecutor` (the single-threaded reference the pool path is
    /// compared against) or a `ThreadPool`. nullptr (the default) ⇒ the driver
    /// constructs an internal pool sized from `--jobs` / hardware_concurrency.
    /// NON-OWNING: the caller owns the executor's lifetime across the compile
    /// call (mirrors the `pipelineOverride` non-owning-injection pattern).
    void setExecutor(substrate::IExecutor* e) noexcept { executor_ = e; }
    [[nodiscard]] substrate::IExecutor* executor() const noexcept { return executor_; }

    /// D-PERF-4-CU-PARALLELISM: the CLI `--jobs N` worker-count override for the
    /// INTERNAL per-CU build pool (ignored when an executor is injected via
    /// `setExecutor`). 0 (the default) ⇒ auto = min(hardware_concurrency, CU
    /// count, 16). `Program::run` stamps this from `CliArgs::jobs`.
    void setJobs(unsigned n) noexcept { jobs_ = n; }
    [[nodiscard]] unsigned jobs() const noexcept { return jobs_; }

private:
    std::optional<std::filesystem::path>   outputDir_;
    std::optional<::dss::opt::OptPipeline> optimizerPipelineOverride_;
    CompileConfig                          compileConfig_ = CompileConfig::Debug;
    std::vector<std::string>               userDefines_;  // c105: --define
    std::vector<std::string>               includeDirs_;  // -I<dir> quote-include search path
    std::vector<std::filesystem::path>     resolveLibraries_;  // c162: --resolve-library
    substrate::IExecutor*                  executor_ = nullptr;  // D-PERF-4 (non-owning; tests inject)
    unsigned                               jobs_     = 0;         // D-PERF-4: --jobs (0 = auto)
};

} // namespace dss

// C-compatible API for FFI consumers (Python, C#, etc.)
extern "C" {
    DSS_EXPORT int dss_compile_project(const char* projectFilePath);
    DSS_EXPORT int dss_compile_directory(const char* directoryPath, const char* languageName,
                                          const char** targets, int targetCount);
    DSS_EXPORT const char* dss_version();
}
