#include "program/program.hpp"

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"
#include "lsp/lsp_server.hpp"
#include "lsp/schema_cache.hpp"
#include "lsp/thread_pool.hpp"
#include "lsp/transport.hpp"
#include "program/cli_args.hpp"
#include "program/compile_pipeline.hpp"
#include "program/cross_validate_target_format.hpp"
#include "program/input_resolver.hpp"
#include "program/target_spec.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace dss {

namespace {

[[nodiscard]] int runLspMode(CliArgs const& args) {
    lsp::SchemaCache cache{args.lspSchemaDir};
    auto transport = std::make_unique<lsp::StdioTransport>();
    // Use hardware concurrency, clamped to [1, 8]. The clamp avoids
    // spawning 64-thread pools on big servers — the parser is CPU-
    // bound on small files, so 4-8 workers is the sweet spot.
    const auto hw = static_cast<std::size_t>(std::thread::hardware_concurrency());
    const auto workers = std::clamp<std::size_t>(hw == 0 ? 4 : hw, 1, 8);
    auto executor = std::make_unique<lsp::ThreadPool>(workers);
    lsp::LspServer server{std::move(transport), std::move(executor), cache};
    return server.run();
}

// Build a `DiagnosticReporter::Config` from the CLI's policy flags
// (`--warnings-as-errors`, `--suppress=<code>`). The Config is
// constructed at Program::run and passed into the policy-aware
// compile* overloads — that lets the CLI policy apply uniformly to
// the per-tier diagnostics drained into the run-wide reporter.
// (D-LK10-7 closure: the policy knob arriving at Program::compileFiles
// is the trigger that closes the D-LK10-7 anchor.)
[[nodiscard]] DiagnosticReporter::Config buildReporterConfig(CliArgs const& args) {
    DiagnosticReporter::Config cfg;
    cfg.policy.warningsAsErrors = args.warningsAsErrors;
    for (auto const code : args.suppress) {
        cfg.policy.suppress.insert(code);
    }
    return cfg;
}

// Drain reporter diagnostics to stderr. The driver is the boundary
// between in-memory diagnostic records and the operator's terminal;
// LSP mode owns its own emit path (LSP $/diagnostic), so this stderr
// flush is the CLI/embed path only. (silent-failure-hunter F3 fold:
// `severityName` prefix lets `grep error` filter correctly.)
void drainDiagnosticsToStderr(DiagnosticReporter const& rep) {
    for (auto const& d : rep.all()) {
        std::cerr << severityName(d.severity)
                  << "[" << diagnosticCodeName(d.code) << "] "
                  << d.actual << '\n';
    }
}

// Emit a driver-tier D_* diagnostic. Wraps `dss::report` so all
// driver-side fail-loud sites take the same shape (Error severity,
// ferried through the same reporter the kernel uses).
void emitDriver(DiagnosticReporter& rep,
                DiagnosticCode code,
                std::string msg) {
    dss::report(rep, code, DiagnosticSeverity::Error, std::move(msg));
}

// `copyDiagnostics` is declared in `compile_pipeline.hpp` (hoisted at
// post-fold review #1 to dedupe with the semantic-tier drain).
// program.cpp uses it for CU + per-Tree reporter drains.

// `forwardConfigDiagnostics` was hoisted to `grammar_schema.hpp` at
// the FF2 post-fold audit (silent-failure C1) once a second consumer
// (ffi/c_header_parser.cpp) arrived. The inline body lives alongside
// `ConfigDiagnostic` itself — its natural canonical home.

// Stamp `[target=<spec>]` context into every error message emitted
// inside the per-target loop. Caller passes a fresh reporter to
// `compileOneTarget`; this helper consolidates that scratch
// reporter into the run-wide reporter with the target prefix added.
// (silent-failure-hunter F9 fold, LK10 cycle 2 post-audit review.)
void mergeWithTargetContext(DiagnosticReporter const& src,
                            std::string const&        targetSpec,
                            DiagnosticReporter&       dst) {
    auto const prefix = "[target=" + targetSpec + "] ";
    for (auto const& d : src.all()) {
        ParseDiagnostic copy = d;
        copy.actual = prefix + copy.actual;
        dst.report(std::move(copy));
    }
}

// Map a `TargetSpec::parse` failure kind to a remediation-distinct
// human message. The split surfaces the actual root cause to the
// operator rather than the generic "malformed target spec". (silent-
// failure-hunter F7 fold, LK10 cycle 2 post-audit review.)
[[nodiscard]] std::string targetSpecErrorMessage(
        std::string const& spec, TargetSpecError e) {
    auto const example = " (e.g. 'x86_64:elf64-x86_64-linux')";
    switch (e) {
        case TargetSpecError::MissingColon:
            return "target spec '" + spec + "' is missing the ':' "
                   "separator — expected '<targetName>:<formatName>'"
                   + example + ".";
        case TargetSpecError::MultipleColons:
            return "target spec '" + spec + "' has more than one ':' "
                   "— the grammar accepts exactly one separator"
                   + example + ".";
        case TargetSpecError::EmptyTargetName:
            return "target spec '" + spec + "' has an empty target "
                   "half — the substring before ':' must name a "
                   "shipped target schema" + example + ".";
        case TargetSpecError::EmptyFormatName:
            return "target spec '" + spec + "' has an empty format "
                   "half — the substring after ':' must name a "
                   "shipped object-format schema" + example + ".";
        case TargetSpecError::WhitespaceInName:
            return "target spec '" + spec + "' contains whitespace "
                   "in a schema-name half — names cannot have spaces"
                   + example + ".";
    }
    return "target spec '" + spec + "' failed to parse.";
}

// Compile one resolved (CU, target, format) triple to one artifact.
// Returns true on success; emits via `reporter` on failure.
[[nodiscard]] bool compileOneTarget(CompilationUnit const& cu,
                                    GrammarSchema const&   grammar,
                                    std::string const&     sourceStem,
                                    std::string const&     targetSpecStr,
                                    DiagnosticReporter&    reporter) {
    auto parsed = TargetSpec::parse(targetSpecStr);
    if (!parsed) {
        emitDriver(reporter, DiagnosticCode::D_InvalidTargetSpec,
                   targetSpecErrorMessage(targetSpecStr, parsed.error()));
        return false;
    }

    auto targetR = TargetSchema::loadShipped(parsed->targetName);
    if (!targetR.has_value()) {
        forwardConfigDiagnostics(targetR.error(), reporter);
        emitDriver(reporter, DiagnosticCode::D_SchemaLoadFailed,
                   "target schema '" + parsed->targetName
                   + "' could not be loaded — check that "
                     "src/dss-config/targets/" + parsed->targetName
                   + ".target.json exists and parses cleanly.");
        return false;
    }
    auto formatR = ObjectFormatSchema::loadShipped(parsed->formatName);
    if (!formatR.has_value()) {
        forwardConfigDiagnostics(formatR.error(), reporter);
        emitDriver(reporter, DiagnosticCode::D_SchemaLoadFailed,
                   "object-format schema '" + parsed->formatName
                   + "' could not be loaded — check that "
                     "src/dss-config/object-formats/" + parsed->formatName
                   + ".format.json exists and parses cleanly.");
        return false;
    }

    // D-LK6-8.2 cross-validation: confirm the (target, format) pair's
    // machine identity matches before linking. Without this guard, a
    // hand-edited format JSON with the wrong `machine` value would
    // silently dispatch the linker to the wrong PLT-stub emitter,
    // producing SIGILL at runtime with no driver diagnostic.
    if (!crossValidateTargetFormat(**targetR, **formatR, reporter)) {
        return false;
    }

    // Output path convention (cycle 2 v1; plan 6 owns the
    // authoritative artifact-profile-driven scheme):
    //   <cwd>/target/<formatName>/<sourceStem><ext>
    // `formatName` already encodes machine+OS, so we don't add a
    // separate `<targetName>` subdir (redundant + bloats the path).
    auto const ext = parsed->outputExtension(**formatR);
    auto const outDir = fs::current_path() / "target" / parsed->formatName;
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) {
        // Driver-tier mkdir failure — distinct from the linker's
        // refuse-to-mkdir contract (which uses
        // `K_ImageWriteParentMissing`). Post-fold review #1 split:
        // `D_OutputDirCreateFailed` (this site) is remediation-
        // distinct from `D_FileNotFound` (input missing) and
        // `D_DirectoryScanFailed` (mid-scan failure on input dirs).
        emitDriver(reporter, DiagnosticCode::D_OutputDirCreateFailed,
                   "failed to create output directory '"
                   + outDir.generic_string() + "': " + ec.message());
        return false;
    }
    auto const outPath = outDir / (std::string{sourceStem} + std::string{ext});

    return compileSingleUnit(cu, grammar, **targetR, **formatR,
                             outPath, reporter);
}

} // namespace

int Program::run(int argc, char* argv[]) {
    // LK10 cycle 3: rich CLI argument dispatch.
    auto parsed = parseCliArgs(argc, argv);
    if (!parsed) {
        std::cerr << "error: " << parsed.error().detail << "\n\n"
                  << cliHelpText();
        return 2;
    }
    CliArgs const& args = *parsed;

    if (args.helpMode) {
        std::cout << cliHelpText();
        return 0;
    }
    if (args.lspMode) {
        return runLspMode(args);
    }
    // Build the diagnostic policy config BEFORE the dispatch fork —
    // every CLI-routed entry point (compileProject, transpile,
    // compileFiles, compileDirectory) honors `--warnings-as-errors`
    // and `--suppress=<code>`. Without this, the fail-loud paths
    // (compileProject + transpile) would build local reporters with
    // default config and silently ignore the user's policy. (silent-
    // failure audit H2 post-fold #1.)
    auto const cfg = buildReporterConfig(args);
    if (args.projectPath.has_value()) {
        return compileProject(*args.projectPath, cfg);
    }
    if (!args.transpileFiles.empty()) {
        return transpile(args.transpileFiles, args.languageName,
                         args.targets, cfg);
    }
    if (args.directoryPath.has_value()) {
        return compileDirectory(*args.directoryPath, args.languageName,
                                args.targets, args.directoryMode, cfg);
    }
    if (!args.sourceFiles.empty()) {
        return compileFiles(args.sourceFiles, args.languageName,
                            args.targets, cfg);
    }

    // No mode flags set — print the ready message + usage hint. The
    // back-compat path for `dss-code-prime` with zero arguments. The
    // parseCliArgs `NoModeSelected` guard already rejects the case
    // where the user supplied options without a mode flag, so we
    // know all CliArgs are at their defaults here.
    std::cout << "DSS Code Prime compiler ready.\n"
              << "Run `dss-code-prime --help` for usage.\n";
    return 0;
}

int Program::compileProject(
    const std::string& projectFilePath,
    DiagnosticReporter::Config const& reporterConfig
) {
    // Plan 06 (.dsp parser + artifact profile) is unstarted; cycle 2
    // anchors the fail-loud surface so a caller passing a project
    // path gets a structural diagnostic rather than silent success
    // and a zero-byte target dir. Policy-aware overload (H2 fold):
    // `--warnings-as-errors` + `--suppress=<code>` apply here too —
    // a user who explicitly suppresses `D_PlanNotLanded` gets the
    // diagnostic dropped before stderr, not silently ignored.
    DiagnosticReporter rep{reporterConfig};
    emitDriver(rep, DiagnosticCode::D_PlanNotLanded,
               "compileProject: .dsp project-file parsing is not "
               "yet implemented — plan 06 (artifact profile) owns "
               "the .dsp schema. Path: '" + projectFilePath + "'.");
    drainDiagnosticsToStderr(rep);
    // Unconditional non-zero exit — `D_PlanNotLanded` signals "the
    // requested operation cannot be performed by this build", a hard
    // capability gap that `--suppress` MUST NOT absorb into a silent
    // success exit. (silent-failure audit post-fold #2: H2's
    // `errorCount() == 0 ? 0 : 1` allowed `--suppress=D_PlanNotLanded`
    // to exit 0 with zero-byte output, exactly the silent-failure
    // class the fold was meant to close.) The user's `--suppress`
    // still hides the message via the policy applied at emit time;
    // it just no longer hides the exit code.
    return 1;
}

int Program::transpile(
    const std::vector<std::string>& sourceFiles,
    const std::string& languageName,
    const std::vector<std::string>& targets,
    DiagnosticReporter::Config const& reporterConfig
) {
    // Plan 10 (source-translation, ST1..ST6) owns the actual
    // transpile engine: `*.map.json` rule files + HIR→HIR walker +
    // target-language CST builder + pretty-printer. v1: fails loud
    // with `D_PlanNotLanded` citing plan 10. The CLI dispatcher
    // routes `--transpile <files>` here so the surface is parsable
    // and stable across plan 10's arrival. Policy-aware overload
    // (H2 fold): see compileProject for rationale.
    DiagnosticReporter rep{reporterConfig};
    std::string detail =
        "transpile: source-to-source translation is not yet "
        "implemented — plan 10 (`*.map.json` + HIR pivot + target "
        "CST builder, ST1..ST6) owns the engine. ";
    detail += "Inputs: " + std::to_string(sourceFiles.size())
            + " source file(s)";
    if (!languageName.empty()) {
        detail += ", source language '" + languageName + "'";
    }
    if (!targets.empty()) {
        detail += ", " + std::to_string(targets.size())
                + " target(s) (first: '" + targets.front() + "')";
    }
    detail += ".";
    emitDriver(rep, DiagnosticCode::D_PlanNotLanded, std::move(detail));
    drainDiagnosticsToStderr(rep);
    // Unconditional non-zero — see compileProject for rationale.
    return 1;
}

int Program::compileFiles(
    const std::vector<std::string>& sourceFiles,
    const std::string& languageName,
    const std::vector<std::string>& targets,
    DiagnosticReporter::Config const& reporterConfig
) {
    DiagnosticReporter rep{reporterConfig};

    if (sourceFiles.empty()) {
        emitDriver(rep, DiagnosticCode::D_EmptyInput,
                   "compileFiles: source file list is empty.");
        drainDiagnosticsToStderr(rep);
        return 1;
    }
    if (targets.empty()) {
        emitDriver(rep, DiagnosticCode::D_InvalidTargetSpec,
                   "compileFiles: targets list is empty — at least "
                   "one '<targetName>:<formatName>' entry required.");
        drainDiagnosticsToStderr(rep);
        return 1;
    }

    // Load the language schema once for the whole call.
    auto grammarR = GrammarSchema::loadShipped(languageName);
    if (!grammarR.has_value()) {
        forwardConfigDiagnostics(grammarR.error(), rep);
        emitDriver(rep, DiagnosticCode::D_SchemaLoadFailed,
                   "language schema '" + languageName
                   + "' could not be loaded — check that "
                     "src/dss-config/sources/" + languageName
                   + ".lang.json exists and parses cleanly.");
        drainDiagnosticsToStderr(rep);
        return 1;
    }
    auto grammar = *grammarR;

    // Build ONE CompilationUnit for all source files. Multi-file
    // CU is the canonical shape (HR11/CU5 — same engine the multi-
    // language tests exercise); each file routes to the language's
    // schema by extension. Cross-CU symbol merge (multi-CU →
    // single image) is LK11; cycle 2 stays single-CU.
    UnitBuilder builder{grammar};
    for (auto const& path : sourceFiles) {
        builder.addFile(fs::path{path});
    }
    auto cu = std::move(builder).finish();

    // Drain the CU's driver-tier diagnostics (D_FileNotFound,
    // D_DuplicateFile, parser/lexer errors per Tree) into the
    // run-wide reporter. Without this drain, a missing source
    // file produces rc=1 with ZERO stderr output — the substrate
    // silent-failure rule's archetype. (code-reviewer F1 fold.)
    copyDiagnostics(cu.driverDiagnostics(), rep);
    for (auto const& tree : cu.trees()) {
        copyDiagnostics(tree.diagnostics(), rep);
    }
    // If the CU's parse stage already failed (file-not-found,
    // unparseable file, …), the per-target loop below would only
    // produce derivative noise. Surface the upstream failure and
    // stop here so the operator sees the actual root cause.
    if (rep.hasErrors()) {
        drainDiagnosticsToStderr(rep);
        return 1;
    }

    // Stem of the first source file names the artifact (the
    // multi-file CU produces ONE artifact per target; the stem is
    // the conventional human-readable label). Plan 06 will
    // eventually let artifact profiles override this.
    std::string const sourceStem =
        fs::path{sourceFiles.front()}.stem().string();

    int exitCode = 0;
    for (auto const& spec : targets) {
        // Per-target scratch reporter — its contents are merged
        // into the run-wide reporter with a `[target=<spec>]` prefix
        // so interleaved diagnostics from multi-target runs can be
        // routed by tooling. (silent-failure-hunter F9 fold.)
        DiagnosticReporter scratch;
        bool const ok = compileOneTarget(cu, *grammar, sourceStem,
                                          spec, scratch);
        mergeWithTargetContext(scratch, spec, rep);
        if (!ok || scratch.hasErrors()) exitCode = 1;
    }

    drainDiagnosticsToStderr(rep);
    return exitCode;
}

int Program::compileDirectory(
    const std::string& directoryPath,
    const std::string& languageName,
    const std::vector<std::string>& targets,
    InputResolver::Mode mode,
    DiagnosticReporter::Config const& reporterConfig
) {
    DiagnosticReporter rep{reporterConfig};

    auto grammarR = GrammarSchema::loadShipped(languageName);
    if (!grammarR.has_value()) {
        forwardConfigDiagnostics(grammarR.error(), rep);
        emitDriver(rep, DiagnosticCode::D_SchemaLoadFailed,
                   "compileDirectory: language schema '" + languageName
                   + "' could not be loaded.");
        drainDiagnosticsToStderr(rep);
        return 1;
    }
    auto grammar = *grammarR;

    // Resolve files via the hoisted `InputResolver` (D-LK10-1
    // closure — landed at LK10 cycle 3). The recursive vs flat
    // policy axis is now an explicit caller parameter, mirroring
    // plan 00 §4.1.3's spec. `fileExtensions()` returns a
    // `std::span<std::string const>` directly compatible with the
    // resolver's signature — no intermediate copy needed.
    std::vector<std::string> sourceFiles;
    if (!InputResolver::resolveDirectory(
            fs::path{directoryPath}, grammar->fileExtensions(),
            mode, sourceFiles, rep)) {
        drainDiagnosticsToStderr(rep);
        return 1;
    }

    // Delegate to compileFiles — same CU + per-target loop shape.
    // Pass the reporter config through so `--warnings-as-errors`
    // + `--suppress=<code>` apply uniformly across the directory
    // scan AND the per-tier IR drains.
    return compileFiles(sourceFiles, languageName, targets, reporterConfig);
}

} // namespace dss

// C-compatible API implementations
extern "C" {
    int dss_compile_project(const char* projectFilePath) {
        dss::Program program;
        return program.compileProject(projectFilePath);
    }

    int dss_compile_directory(const char* directoryPath, const char* languageName,
                            const char** targets, int targetCount) {
        dss::Program program;
        std::vector<std::string> targetList(targets, targets + targetCount);
        return program.compileDirectory(directoryPath, languageName, targetList);
    }

    const char* dss_version() {
        return "0.1.0";
    }
}
