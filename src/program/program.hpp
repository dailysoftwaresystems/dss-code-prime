#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
// D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION (C):
// `DependencyArtifactCacheConfig` is a VALUE held by a member below, so the
// definition is needed rather than a forward declaration. `project_config.hpp`
// is already this tier's manifest vocabulary and `program.cpp` includes it.
#include "core/types/project_config.hpp"
#include "opt/optimizer.hpp"
#include "program/cli_args.hpp"      // CompileConfig
#include "program/input_resolver.hpp"

#include <cstddef>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <vector>

// D-PERF-4-CU-PARALLELISM: forward-declare the substrate executor so the
// injection surface below is a bare pointer member — no need to pull the
// thread-pool header (with <thread>/<condition_variable>) into every
// program.hpp consumer. The compile driver includes the full header.
namespace dss::substrate { class IExecutor; }

namespace dss {

// AP6: the `dependsOn` git-acquisition seam, forward-declared for the same
// reason `substrate::IExecutor` above is — the injection surface is a bare
// pointer member, so no consumer of `program.hpp` inherits `git_acquire.hpp`.
class IGitRunner;

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
    ///
    /// BUILD LIFECYCLE, in the order the seams run (see the dense notes at
    /// each seam in `program.cpp` for the reasoning):
    ///   0. `dependsOn` non-empty ⇒ REFUSE the build (`D_PlanNotLanded`) —
    ///      resolution has not landed, and accept-and-ignore would report
    ///      success for an artifact missing a declared prerequisite;
    ///   1. load + the AP2 language / AP3 format artifact-profile gates, the
    ///      flag-array merges, artifactName / per-format subdir, stackReserve;
    ///   2. `preBuildScripts` — spawned BEFORE the `sources[]` glob expansion,
    ///      so a hook that GENERATES sources composes with a pattern that
    ///      matches them (expansion is fail-loud on zero matches);
    ///   3. glob expansion, cross-entry dedup, and the multi-vs-single-CU route;
    ///   4. `postBuildScripts` — ONLY when the compile returned 0. A failing
    ///      post-build hook makes the overall result non-zero even though the
    ///      artifact exists and is kept.
    /// Both hook lists run in the PROCESS working directory — the same base
    /// relative `sources[]` entries and globs resolve against.
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

    /// D-AP2-OUTPUT-ROUTING (project artifactName + per-platform subdir):
    /// the OPTIONAL base NAME for the emitted binary — nullopt ⇒ the source
    /// stem (the unchanged default). `Program::compileProject` stamps this
    /// from the manifest's `artifactName`; the CLI path (`Program::run`)
    /// never sets it, so `--compile` output names stay byte-identical.
    /// Threaded to `compileOneTarget` (via `runCusToTargets`) alongside
    /// `outputDir`, where `artifactName.value_or(sourceStem)` names the file.
    void setArtifactName(std::optional<std::string> name) {
        artifactName_ = std::move(name);
    }
    [[nodiscard]] std::optional<std::string> const&
    artifactName() const noexcept { return artifactName_; }

    /// D-AP2-OUTPUT-ROUTING (project per-platform subdir): when set, EVERY
    /// target's artifact routes into a `<outputDir>/<formatName>/` subdir —
    /// including a SINGLE-target build (which is otherwise flat). Multi-target
    /// builds already subdir by formatName; this forces the same layout for
    /// single-target project builds so a project's output is consistently
    /// per-platform. `Program::compileProject` sets it true; the CLI path
    /// (`Program::run`) leaves it false, so `--compile` single-target output
    /// stays FLAT (byte-identical). Threaded to `compileOneTarget` (via
    /// `runCusToTargets`) where the subdir decision is
    /// `multiTargetBuild || perFormatOutputSubdir`.
    void setPerFormatOutputSubdir(bool on) noexcept { perFormatOutputSubdir_ = on; }
    [[nodiscard]] bool perFormatOutputSubdir() const noexcept {
        return perFormatOutputSubdir_;
    }

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

    /// D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION (C): the
    /// cross-build artifact cache policy, read off the ROOT `.dss-project.json`
    /// by `Program::compileProject` and PROPAGATED onto every dependency's own
    /// fresh `Program` by `Resolver::buildNode_` (the setCompileConfig pattern,
    /// on the propagation list that already carries config / jobs / executor).
    ///
    /// nullopt, or an engaged value whose `enabled` is false, ⇒ NO cache: the
    /// build looks nothing up and writes nothing, byte-identically to every
    /// build that predates this knob. It is nullopt on the CLI path, so
    /// `--compile` is untouched.
    ///
    /// ⚠ A DEPENDENCY'S OWN MANIFEST COPY IS NEVER READ — B.10's ruling for
    /// `targets[]` and U-9's for `output`, applied to the same kind of fact.
    /// Caching is a property of the BUILD; a graph whose nodes each declared a
    /// policy would make "is this build cached?" a question with N answers.
    void setDependencyArtifactCache(
        std::optional<DependencyArtifactCacheConfig> policy) {
        dependencyArtifactCache_ = std::move(policy);
    }
    [[nodiscard]] std::optional<DependencyArtifactCacheConfig> const&
    dependencyArtifactCache() const noexcept {
        return dependencyArtifactCache_;
    }

    /// D-OPT11-LAZY-IMPORT-EDGE: the link-time-optimization TOPOLOGY, stamped
    /// from `CliArgs::lto` by `Program::run` before dispatch (the
    /// setCompileConfig pattern). `Full` (the default) is what every build did
    /// before the flag existed; `Thin` adds the per-TU on-demand import stage
    /// between the CU pool and the whole-program merge.
    void setLtoMode(LtoModeArg m) noexcept { ltoMode_ = m; }
    [[nodiscard]] LtoModeArg ltoMode() const noexcept { return ltoMode_; }

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
    ///
    /// D-FFI-DECLARED-IMPORT-NAME: each entry may additionally STATE the
    /// runtime identity to record for the symbols read out of it (the CLI
    /// `<path>=<import-name>` suffix / the manifest's `{"path","importName"}`
    /// object). That is what `ResolveLibrarySpec` carries.
    void setResolveLibraries(std::vector<ResolveLibrarySpec> libs) {
        resolveLibraries_ = std::move(libs);
    }
    /// Plain-path convenience overload — the "no identity stated anywhere"
    /// shorthand, exactly parallel to the manifest's plain-string entry form.
    /// Every entry gets an EMPTY `declaredImportName`, so the recorded import
    /// identity is decided exactly as it was before this capability landed
    /// (embedded soname, else basename). Kept as a first-class spelling rather
    /// than a migration shim: the overwhelming majority of callers resolve a
    /// DSS-BUILT library whose own soname is already correct, and making them
    /// write an empty second member would be noise.
    ///
    /// NOTE (overload-resolution): `ResolveLibrarySpec` is an aggregate with no
    /// converting constructor from `std::filesystem::path`, so a braced call
    /// like `setResolveLibraries({somePath})` resolves UNAMBIGUOUSLY to this
    /// overload — the spec overload is not viable for it.
    void setResolveLibraries(std::vector<std::filesystem::path> libs) {
        resolveLibraries_.clear();
        resolveLibraries_.reserve(libs.size());
        for (auto& p : libs) {
            resolveLibraries_.push_back(ResolveLibrarySpec{std::move(p), {}});
        }
    }
    [[nodiscard]] std::vector<ResolveLibrarySpec> const&
    resolveLibraries() const noexcept { return resolveLibraries_; }

    /// AP6 — the PER-TARGET `--resolve-library` ADDITIONS channel: extra
    /// `ResolveLibrarySpec`s for ONE `<targetName>:<formatName>` spec, MERGED
    /// with (never replacing) the program-wide `setResolveLibraries` list at
    /// the per-target build. An EMPTY map (the default) adds nothing to
    /// anyone, so every existing build is byte-identical.
    ///
    /// ★ INTERNAL, AND DELIBERATELY NOT USER-FACING. There is no manifest
    /// field and no CLI flag behind this, and there must not be: the
    /// dependency resolver is the ONLY producer of per-target libraries, so a
    /// user-facing per-target declaration would be a mechanism with no
    /// consumer. The manifest and the CLI stay PROGRAM-WIDE — both are
    /// statements about the program, and neither author knows which target a
    /// dependency artifact will be built for.
    ///
    /// ★ WHY IT EXISTS AT ALL. A dependency is built ONCE PER CONSUMER TARGET
    /// (the U-2 consumer-driven derivation), so the artifact to link for
    /// `x86_64:elf64-…` is a DIFFERENT FILE from the one for `arm64:macho64-…`.
    /// Broadcasting the union through the program-wide list would hand every
    /// target every other target's binaries — an ELF fed to a PE link, which
    /// binds SILENTLY today (registry
    /// `D-FFI-RESOLVE-LIBRARY-WRONG-FORMAT-GUARD-IS-INCIDENTAL`).
    ///
    /// ★ WHY KEYED BY SPEC STRING RATHER THAN INDEX-PARALLEL TO `targets`. An
    /// index-parallel channel adds an invariant the caller can break —
    /// `additions.size() == targets.size()` — and its breach is a
    /// MIS-ALIGNMENT, i.e. target 0 quietly linking target 1's libraries. The
    /// key is the same spec string the resolver already derives against, so
    /// the invariant does not exist to be broken; the index-parallel form is
    /// derived INSIDE `runCusToTargets`, where it cannot be the wrong length.
    /// A key naming a target the build does not compile is refused LOUD
    /// (`D_InvalidTargetSpec`) rather than dropped.
    ///
    /// Source / target / format agnostic: the key is opaque spec TEXT compared
    /// for equality against this build's own specs — never parsed, never
    /// matched against any language, CPU or object-format identity.
    void setResolveLibraryAdditionsByTarget(
        std::map<std::string, std::vector<ResolveLibrarySpec>> byTarget) {
        resolveLibraryAdditionsByTarget_ = std::move(byTarget);
    }
    [[nodiscard]] std::map<std::string, std::vector<ResolveLibrarySpec>> const&
    resolveLibraryAdditionsByTarget() const noexcept {
        return resolveLibraryAdditionsByTarget_;
    }

    /// AP6 — WHAT THE LAST BUILD ACTUALLY WROTE. Index-parallel to the
    /// `targets` argument of the most recent `compileFiles` / `compileUnits`
    /// call (including the one `compileProject` delegates to): entry i is the
    /// artifact path for `targets[i]`, or `nullopt` if that target failed.
    ///
    /// EMPTY means the run never reached the per-target loop (an unparseable
    /// spec, an unloadable schema, a front-end failure) — distinct from a
    /// sized vector of `nullopt`, which means targets were attempted and none
    /// produced an artifact. Cleared and re-sized by each run, so it always
    /// describes the latest one.
    ///
    /// ★ WHY THE DRIVER HANDS THIS BACK INSTEAD OF LETTING CALLERS COMPUTE IT.
    /// The path is the join of `outputDir` (or `<cwd>/target` when absent),
    /// the single-vs-multi-target rule, the forced per-format subdir, the
    /// format's declared extension, and `artifactName.value_or(sourceStem)`.
    /// AP6 must thread a built dependency's artifact into the build that
    /// depends on it; re-deriving that formula at the consumer would create a
    /// second copy of a fact that drifts the first time any part of the
    /// convention changes. The producer answers, once.
    [[nodiscard]] std::vector<std::optional<std::filesystem::path>> const&
    artifactPaths() const noexcept { return artifactPaths_; }

    /// AP6 — inject the `git` acquisition surface `dependsOn` resolution uses.
    /// nullptr (the default) ⇒ `compileProject` uses a `SystemGitRunner`, i.e.
    /// real `git` on PATH.
    ///
    /// ★ WHY IT IS INJECTABLE AT ALL, which `git_acquire.hpp` argues at length:
    /// B.4's cache machine has FOUR outcomes and TWO of them are network
    /// FAILURES. A test driving real git can reach the first two on a good day
    /// and NEITHER of the last two deterministically — you cannot ask a working
    /// network to fail on cue — so the state machine is exercised against a
    /// scripted fake with zero network and zero dependence on `git` being
    /// installed.
    ///
    /// NON-OWNING: the caller owns the runner's lifetime across the call
    /// (mirrors `setExecutor`). Propagated verbatim onto the fresh `Program`
    /// each dependency is built on, so a fake injected at the root reaches
    /// every node of the graph.
    void setGitRunner(IGitRunner* g) noexcept { gitRunner_ = g; }
    [[nodiscard]] IGitRunner* gitRunner() const noexcept { return gitRunner_; }

    /// AP6 — `--force-git-cache`: bypass the `.dss-deps` cache-hit
    /// short-circuit and re-fetch every git dependency, even one whose
    /// recorded commit still matches. It forces a REFRESH; it does not force
    /// USE of the cache (the name reads the other way, which is why `--help`
    /// carries an imperative gloss). Every other rule of B.4 is unchanged by
    /// it — a fetch that fails with a usable checkout present still emits
    /// `D_DependencyGitFetchFallback` and still builds.
    ///
    /// U-10: a silent no-op when the project declares no git dependency,
    /// consistent with an empty `preBuildScripts`. That falls out of the cache
    /// being opened LAZILY rather than from a check anybody has to remember.
    void setForceGitCache(bool on) noexcept { forceGitCache_ = on; }
    [[nodiscard]] bool forceGitCache() const noexcept { return forceGitCache_; }

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

    /// D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: the per-PROGRAM stack reserve
    /// this build requests of the emitted image, in BYTES. nullopt (the
    /// default) ⇒ the object format's declared default stands.
    ///
    /// Sources + PRECEDENCE: `Program::run` stamps the CLI
    /// `--stack-reserve`; `Program::compileProject` then applies the
    /// manifest's `stackReserve` ONLY IF the CLI supplied none — i.e. the
    /// CLI WINS. (The three flag ARRAYS merge, because appending composes;
    /// a scalar cannot merge, so it needs an explicit precedence rule, and
    /// "explicit command line beats committed file" is the universal one.)
    ///
    /// The value is carried, not honoured, at this tier: whether the target
    /// format can express a stack reserve at all — and whether the value is
    /// within the range that format DECLARES — is decided at the linker
    /// gate, which REFUSES rather than drops.
    void setStackReserveBytes(std::optional<std::uint64_t> n) noexcept {
        stackReserveBytes_ = n;
    }
    [[nodiscard]] std::optional<std::uint64_t>
    stackReserveBytes() const noexcept { return stackReserveBytes_; }

private:
    std::optional<std::filesystem::path>   outputDir_;
    std::optional<std::string>             artifactName_;             // D-AP2-OUTPUT-ROUTING: project binary base name (nullopt = source stem)
    bool                                   perFormatOutputSubdir_ = false;  // D-AP2-OUTPUT-ROUTING: project ⇒ force <formatName>/ subdir
    // D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION (C):
    // nullopt ⇒ no cross-build artifact cache at all (the CLI path, and every
    // manifest without the member).
    std::optional<DependencyArtifactCacheConfig> dependencyArtifactCache_;
    std::optional<::dss::opt::OptPipeline> optimizerPipelineOverride_;
    CompileConfig                          compileConfig_ = CompileConfig::Debug;
    LtoModeArg                             ltoMode_ = LtoModeArg::Full;
    std::vector<std::string>               userDefines_;  // c105: --define
    std::vector<std::string>               includeDirs_;  // -I<dir> quote-include search path
    std::vector<ResolveLibrarySpec>        resolveLibraries_;  // c162: --resolve-library
    // AP6: per-target ADDITIONS to the line above, keyed by target spec
    // (internal channel — no CLI / manifest surface), and the artifact paths
    // the last run produced (index-parallel to that run's `targets`).
    std::map<std::string, std::vector<ResolveLibrarySpec>>
                                           resolveLibraryAdditionsByTarget_;
    std::vector<std::optional<std::filesystem::path>> artifactPaths_;
    substrate::IExecutor*                  executor_ = nullptr;  // D-PERF-4 (non-owning; tests inject)
    // AP6: the `dependsOn` git seam (non-owning; tests inject a scripted fake)
    // and `--force-git-cache`.
    IGitRunner*                            gitRunner_     = nullptr;
    bool                                   forceGitCache_ = false;
    unsigned                               jobs_     = 0;         // D-PERF-4: --jobs (0 = auto)
    // D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: --stack-reserve / manifest
    // `stackReserve` (nullopt = the format's declared default).
    std::optional<std::uint64_t>           stackReserveBytes_;
};

} // namespace dss

// C-compatible API for FFI consumers (Python, C#, etc.)
extern "C" {
    DSS_EXPORT int dss_compile_project(const char* projectFilePath);
    DSS_EXPORT int dss_compile_directory(const char* directoryPath, const char* languageName,
                                          const char** targets, int targetCount);
    DSS_EXPORT const char* dss_version();
}
