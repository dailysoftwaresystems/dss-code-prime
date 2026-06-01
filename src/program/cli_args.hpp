#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "program/input_resolver.hpp"

#include <expected>
#include <optional>
#include <string>
#include <vector>

// CLI argument parsing — LK10 cycle 3 (plan 14 §3 LK10 cycle 3
// landed 2026-06-01). The dss-code-prime CLI's argv → structured
// `CliArgs` pivot.
//
// Standing rule check: source / target / linker-format agnostic.
// CliArgs carries opaque schema-name strings (`languageName`,
// `targets`); the dispatch at `Program::run` routes through the
// existing config-driven `compileFiles` / `compileDirectory`
// engine. No CLI flag branches on identity.

namespace dss {

// One CLI invocation's parsed flags. Mutually-exclusive modes
// are gated by the dispatch in `Program::run`, not by union types
// — the parser produces a single struct and the dispatcher
// detects "no mode flags" vs "mode flag + invalid combination"
// itself, surfacing structured `D_InvalidTargetSpec` /
// `D_PlanNotLanded` / etc. diagnostics.
// Optimization configuration — plan 22 OPT* wiring slot.
//
// `Debug` (the default) disables every optimizer pass; the pipeline
// produces unoptimized code suitable for stepping in a debugger.
// `Release` opts in to the full optimizer pipeline (const-fold + DCE +
// copy-prop + inlining + ... per plan 22 OPT1..OPT10 as each lands).
//
// Today plan 22 hasn't shipped any passes; the flag is parsed +
// stored + threaded into the pipeline as a no-op. When OPT1+ land,
// they'll read `CompileConfig` (or whatever the eventual single
// source of truth becomes) and decide which passes to run. Wiring
// the flag now keeps the CLI surface stable across plan 22's arrival.
enum class CompileConfig : std::uint8_t {
    Debug   = 0,  // default — no optimization
    Release = 1,  // full optimizer pipeline (plan 22 OPT1..OPT10)
};

[[nodiscard]] DSS_EXPORT std::string_view
    compileConfigName(CompileConfig c) noexcept;

struct DSS_EXPORT CliArgs {
    // ── Mode flags (mutually exclusive at dispatch time) ────────
    bool                     lspMode     = false;
    bool                     helpMode    = false;
    std::vector<std::string> sourceFiles; // populated by --compile <files>...
    std::vector<std::string> transpileFiles; // populated by --transpile <files>...
    std::optional<std::string> directoryPath; // populated by --directory <path>
    std::optional<std::string> projectPath;   // populated by --project <file>

    // ── Compile-mode shared options ─────────────────────────────
    std::string              languageName;     // --language <name>
    std::vector<std::string> targets;          // --target <spec> (repeatable)
    InputResolver::Mode      directoryMode =
        InputResolver::Mode::Recursive;        // --recursive / --no-recursive
    CompileConfig            config = CompileConfig::Debug;  // --config=release|debug

    // ── LSP options ─────────────────────────────────────────────
    std::optional<std::filesystem::path> lspSchemaDir;

    // ── Diagnostic policy (D-LK10-7 closure) ────────────────────
    //
    // `--warnings-as-errors` promotes every Warning to Error after
    // overrides + suppress apply (mirrors `clang -Werror`). `--suppress
    // =<code>` drops a specific diagnostic code silently (mirrors
    // `clang -Wno-<flag>` mechanism; here keyed on the structured
    // code, not a string flag). The resulting `DiagnosticReporter::
    // Config` is constructed at the `Program::run` site and threaded
    // into the policy-aware `compileFiles` / `compileDirectory`
    // overload.
    bool                          warningsAsErrors = false;
    std::vector<DiagnosticCode>   suppress;
};

// Parse-failure kinds. Mirror the `TargetSpecError` shape so the
// CLI driver can dispatch a remediation-distinct diagnostic per
// kind, not a generic "bad CLI args" message.
enum class CliArgsError : std::uint8_t {
    UnknownFlag         = 1,    // `-<x>` or `--<x>` that isn't recognized
    MissingFlagValue    = 2,    // --target ""  /  --target  (no next arg)
    DuplicateModeFlag   = 3,    // both --compile AND --directory etc.
    NoModeSelected      = 4,    // mode flags absent but mode-options set
    EmptyFileList       = 5,    // --compile / --transpile with no files
    EmptyTargetList     = 6,    // mode flag but no --target
    MissingLanguage     = 7,    // mode flag but no --language
    InvalidSuppressCode = 8,    // --suppress=<bad-code>
    InvalidConfig       = 9,    // --config=<not-debug-not-release>
    EmptyFilename       = 10,   // --compile "" or bare `-` as positional
    UnexpectedPositional = 11,  // bare positional outside --compile/--transpile
};

[[nodiscard]] DSS_EXPORT std::string_view
    cliArgsErrorName(CliArgsError e) noexcept;

// Free-form error context — captures the offending flag / value for
// the diagnostic message. The CLI parser does NOT emit diagnostics
// directly (it predates Program::run's reporter); the caller
// formats the message from this struct.
struct DSS_EXPORT CliArgsErrorInfo {
    CliArgsError kind = CliArgsError::UnknownFlag;
    std::string  detail;
};

// Parse `argc`/`argv` into a structured `CliArgs`. The `argv[0]`
// program name slot is conventionally skipped. Returns
// `std::expected` so the dispatcher can branch on success vs
// kind-of-failure.
[[nodiscard]] DSS_EXPORT std::expected<CliArgs, CliArgsErrorInfo>
parseCliArgs(int argc, char* argv[]);

// Help text shown on `--help` / `-h` / on bad CLI args. Centralised
// here so the test harness can pin its content stable across CLI
// extensions.
[[nodiscard]] DSS_EXPORT std::string cliHelpText();

} // namespace dss
