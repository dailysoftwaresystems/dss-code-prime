#include "program/program.hpp"

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"
#include "lir/lir_pass_util.hpp"
#include "lsp/lsp_server.hpp"
#include "lsp/schema_cache.hpp"
#include "lsp/thread_pool.hpp"
#include "lsp/transport.hpp"
#include "program/compile_pipeline.hpp"
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

// LSP mode is invoked by `dss-code-prime --lsp [--schema-dir=PATH]`.
// Order-independent.
struct LspFlags {
    bool                                 lspMode = false;
    std::optional<std::filesystem::path> schemaDir;
    std::vector<std::string>             unknown;
};

[[nodiscard]] LspFlags parseLspFlags(int argc, char* argv[]) {
    LspFlags out;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        if (a == "--lsp") {
            out.lspMode = true;
        } else if (a.starts_with("--schema-dir=")) {
            out.schemaDir = std::filesystem::path{
                std::string{a.substr(std::string_view{"--schema-dir="}.size())}};
        } else if (a.starts_with("--")) {
            // Track unknown long-flags so a typo (`--Lsp`) doesn't
            // silently fall through into a different code path.
            out.unknown.emplace_back(a);
        }
    }
    return out;
}

[[nodiscard]] int runLspMode(LspFlags const& flags) {
    lsp::SchemaCache cache{flags.schemaDir};
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

// Emit a driver-tier D_* diagnostic. Wraps `lir_pass_util::report`
// so all driver-side fail-loud sites take the same shape (Error
// severity, ferried through the same reporter the kernel uses).
void emitDriver(DiagnosticReporter& rep,
                DiagnosticCode code,
                std::string msg) {
    lir_pass_util::report(rep, code,
                          DiagnosticSeverity::Error,
                          std::move(msg));
}

// Copy every diagnostic from `src` into `dst`. Caller-side fold for
// the "compileFiles must surface upstream tier diagnostics" rule
// (code-reviewer F1 + silent-failure F1): UnitBuilder /
// SemanticModel / lowering tiers carry their own reporters that the
// driver's stderr-drain doesn't see by default. (LK10 cycle 2
// post-audit review.)
void copyDiagnostics(DiagnosticReporter const& src,
                     DiagnosticReporter&       dst) {
    for (auto const& d : src.all()) dst.report(d);
}

// Forward every ConfigDiagnostic from a failed `loadShipped` into
// the driver reporter. Mirrors `copyDiagnostics` for the per-row
// C_* / config-side detail the loader produced — without this, the
// driver only emits the wrapping `D_SchemaLoadFailed` and the
// actual config bug (bad JSON line, missing field, etc.) is silently
// dropped. (code-reviewer F2 fold, LK10 cycle 2 post-audit review.)
void forwardConfigDiagnostics(std::span<ConfigDiagnostic const> diags,
                              DiagnosticReporter&               dst) {
    for (auto const& cd : diags) {
        ParseDiagnostic p;
        p.code     = cd.code;
        p.severity = cd.severity;
        p.actual   = cd.path;
        if (!cd.message.empty()) {
            if (!p.actual.empty()) p.actual += " — ";
            p.actual += cd.message;
        }
        dst.report(std::move(p));
    }
}

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
        // Driver-tier mkdir failure — `D_FileNotFound` is the
        // closest semantic match (the parent path's filesystem is
        // unreachable / unwritable). NOT `K_ImageWriteParentMissing`
        // (that code documents the linker writer's refuse-to-mkdir
        // contract; semantically opposite of an auto-mkdir failure).
        // (silent-failure-hunter F2 fold.)
        emitDriver(reporter, DiagnosticCode::D_FileNotFound,
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
    const auto flags = parseLspFlags(argc, argv);
    if (flags.lspMode) {
        return runLspMode(flags);
    }
    // Reject unknown long-flags loudly so a typo (`--Lsp`) doesn't
    // silently fall through into the placeholder path below.
    if (!flags.unknown.empty()) {
        for (auto const& u : flags.unknown) {
            std::cerr << "unrecognized flag: " << u << std::endl;
        }
        std::cerr << "usage: dss-code-prime [--lsp [--schema-dir=PATH]]"
                  << std::endl;
        return 2;
    }

    // LK10 cycle 3 will wire CLI compile flags (e.g. `--compile
    // FILES... --target T:F --output PATH`) into compileFiles /
    // compileDirectory here. Until then the no-arg invocation
    // remains a no-op identity check; the programmatic API
    // (compileFiles / compileDirectory / compileProject) is the
    // load-bearing entry surface for library + FFI consumers.
    std::cout << "DSS Code Prime compiler ready." << std::endl;
    return 0;
}

int Program::compileProject(const std::string& projectFilePath) {
    // Plan 06 (.dsp parser + artifact profile) is unstarted; cycle 2
    // anchors the fail-loud surface so a caller passing a project
    // path gets a structural diagnostic rather than silent success
    // and a zero-byte target dir.
    DiagnosticReporter rep;
    emitDriver(rep, DiagnosticCode::D_PlanNotLanded,
               "compileProject: .dsp project-file parsing is not "
               "yet implemented — plan 06 (artifact profile) owns "
               "the .dsp schema. Path: '" + projectFilePath + "'.");
    drainDiagnosticsToStderr(rep);
    return 1;
}

int Program::compileFiles(
    const std::vector<std::string>& sourceFiles,
    const std::string& languageName,
    const std::vector<std::string>& targets
) {
    DiagnosticReporter rep;

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
    const std::vector<std::string>& targets
) {
    DiagnosticReporter rep;

    fs::path const root{directoryPath};
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        emitDriver(rep, DiagnosticCode::D_FileNotFound,
                   "compileDirectory: '" + directoryPath
                   + "' does not exist or is not a directory.");
        drainDiagnosticsToStderr(rep);
        return 1;
    }

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

    // Resolve files: recurse `directoryPath`, keep regular files
    // whose extension matches the schema's declared
    // `fileExtensions`. Plan 00 §4.1.3 specifies an `InputResolver`
    // utility for this; LK10 cycle 3 will hoist it once CLI flags
    // (--recursive / --extension) give it a second policy axis.
    // Anchor: D-LK10-1.
    std::vector<std::string> sourceFiles;
    auto const exts = grammar->fileExtensions();
    fs::recursive_directory_iterator it(root, ec);
    if (ec) {
        emitDriver(rep, DiagnosticCode::D_FileNotFound,
                   "compileDirectory: failed to open directory '"
                   + directoryPath + "': " + ec.message());
        drainDiagnosticsToStderr(rep);
        return 1;
    }
    fs::recursive_directory_iterator const end{};
    for (; it != end; it.increment(ec)) {
        if (ec) {
            // Mid-scan failure (permission denied on a subdir, race
            // delete, ...) — surface loudly. Without this the scan
            // silently truncates and operator sees a partial build
            // with zero error indication. (silent-failure-hunter F6
            // fold, LK10 cycle 2 post-audit review.)
            emitDriver(rep, DiagnosticCode::D_FileNotFound,
                       "compileDirectory: directory-scan interrupted "
                       "after partial enumeration of '" + directoryPath
                       + "': " + ec.message());
            drainDiagnosticsToStderr(rep);
            return 1;
        }
        if (!it->is_regular_file()) continue;
        auto const ext = it->path().extension().string();
        auto const match = std::any_of(exts.begin(), exts.end(),
            [&](auto const& e) { return e == ext; });
        if (match) sourceFiles.push_back(it->path().generic_string());
    }
    // Deterministic order for reproducible builds.
    std::sort(sourceFiles.begin(), sourceFiles.end());

    if (sourceFiles.empty()) {
        emitDriver(rep, DiagnosticCode::D_EmptyInput,
                   "compileDirectory: no files in '" + directoryPath
                   + "' match the '" + languageName
                   + "' language's fileExtensions.");
        drainDiagnosticsToStderr(rep);
        return 1;
    }

    // Delegate to compileFiles — same CU + per-target loop shape.
    return compileFiles(sourceFiles, languageName, targets);
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
