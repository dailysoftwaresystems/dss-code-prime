#include "program/cli_args.hpp"

#include "core/types/parse_diagnostic.hpp"

#include <charconv>
#include <cstdint>
#include <string_view>

namespace dss {

namespace {

[[nodiscard]] CliArgsErrorInfo make_error(CliArgsError kind, std::string detail) {
    return {kind, std::move(detail)};
}

// Try to consume the value of a `--flag <value>` pair. Returns the
// next arg's value + advances `i`; emits `MissingFlagValue` if no
// next arg exists. `--flag=<value>` is also accepted (the `=`-form
// is resolved before this helper is called).
[[nodiscard]] std::expected<std::string, CliArgsErrorInfo>
takeFlagValue(std::string_view flag, int& i, int argc, char* argv[]) {
    if (i + 1 >= argc) {
        return std::unexpected(make_error(
            CliArgsError::MissingFlagValue,
            std::string{flag} + " requires a value"));
    }
    ++i;
    std::string value{argv[i]};
    // Symmetric reject with the `=` form: `--target ""` (empty next
    // arg) is just as broken as `--target=`. Without this, the empty
    // string silently becomes a "valid" value and fails late at
    // downstream parsers (TargetSpec::parse, schema loaders). (silent-
    // failure audit post-fold #2 — comment-analyzer caught the
    // asymmetric reject this helper had relative to `equalsValue`.)
    if (value.empty()) {
        return std::unexpected(make_error(
            CliArgsError::MissingFlagValue,
            std::string{flag} + " requires a non-empty value"));
    }
    return value;
}

// Parse a diagnostic-code name string (e.g. "D_FileNotFound" or
// "0xD001") into a `DiagnosticCode`. The CLI accepts both the
// symbolic name (matching `diagnosticCodeName`) and the hex value.
// Used by `--suppress=<code>` (D-LK10-7 closure).
[[nodiscard]] std::optional<DiagnosticCode>
parseSuppressCode(std::string_view spec) noexcept {
    // Reject sentinel names up front:
    //   * `Unknown` is the fallback `diagnosticCodeName` returns for
    //     un-enumerated values — symbolic-name scan would match the
    //     first un-enumerated hole (silent-failure F1 post-fold).
    //   * `None` (0x0000) is the explicitly-enumerated "no diagnostic"
    //     sentinel; no real diagnostic ever emits it, so suppressing
    //     it is a useless silent-no-op (silent-failure audit post-fold
    //     #2 — pr-test-analyzer Rating 8 boundary).
    if (spec == "Unknown" || spec == "None") return std::nullopt;

    // Hex form: 0x___
    if (spec.size() > 2 && spec[0] == '0' && (spec[1] == 'x' || spec[1] == 'X')) {
        std::uint16_t v = 0;
        auto const [p, ec] = std::from_chars(
            spec.data() + 2, spec.data() + spec.size(), v, 16);
        if (ec != std::errc{} || p != spec.data() + spec.size()) {
            return std::nullopt;
        }
        // Validate that the parsed code is ACTUALLY enumerated.
        // `static_cast<DiagnosticCode>(0xFFFF)` is a valid C++ value but
        // names nothing the reporter could ever emit — accepting it
        // would silently insert a useless entry into the suppress set.
        // (silent-failure audit C1 post-fold #1: same class as F1's
        // symbolic-name guard, applied symmetrically to the hex form.)
        auto const code = static_cast<DiagnosticCode>(v);
        auto const name = diagnosticCodeName(code);
        if (name == "Unknown" || name == "None") return std::nullopt;
        return code;
    }
    // Symbolic name form: linear scan over the uint16 range. The
    // diagnostic-code enum is closed (~80 codes today). The scan
    // skips the "Unknown" fallback hits (only enumerated codes
    // produce a name that's not the sentinel), so any successful
    // match is a real code lookup. O(64K) per --suppress; runs
    // once per CLI invocation — ~ms range, never hot path.
    for (std::uint32_t v = 0; v < 0x10000u; ++v) {
        auto const code = static_cast<DiagnosticCode>(v);
        auto const name = diagnosticCodeName(code);
        if (name == "Unknown") continue;
        if (name == spec) return code;
    }
    return std::nullopt;
}

// Parse a CompileConfig name string ("debug" / "release"). v1 only
// accepts the two canonical values; future v1.x may add `relwithdebinfo`
// or `minsizerel` shapes when plan 22 OPT* lands more knobs.
[[nodiscard]] std::optional<CompileConfig>
parseCompileConfig(std::string_view spec) noexcept {
    // ASCII-lowercase the input once so `Debug`/`DEBUG`/`debug` all
    // accept uniformly. CLI flags are operator-typed; case-asymmetric
    // accept is operator-hostile and silently rejects perfectly
    // reasonable inputs. (silent-failure audit post-fold #2.)
    std::string lowered;
    lowered.reserve(spec.size());
    for (char c : spec) {
        lowered.push_back(static_cast<char>(
            (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c));
    }
    if (lowered == "debug")   return CompileConfig::Debug;
    if (lowered == "release") return CompileConfig::Release;
    return std::nullopt;
}

} // namespace

std::string_view compileConfigName(CompileConfig c) noexcept {
    switch (c) {
        case CompileConfig::Debug:   return "debug";
        case CompileConfig::Release: return "release";
    }
    return "unknown";
}

std::string_view cliArgsErrorName(CliArgsError e) noexcept {
    switch (e) {
        case CliArgsError::UnknownFlag:         return "UnknownFlag";
        case CliArgsError::MissingFlagValue:    return "MissingFlagValue";
        case CliArgsError::DuplicateModeFlag:   return "DuplicateModeFlag";
        case CliArgsError::NoModeSelected:      return "NoModeSelected";
        case CliArgsError::EmptyFileList:       return "EmptyFileList";
        case CliArgsError::EmptyTargetList:     return "EmptyTargetList";
        case CliArgsError::MissingLanguage:     return "MissingLanguage";
        case CliArgsError::InvalidSuppressCode: return "InvalidSuppressCode";
        case CliArgsError::InvalidConfig:       return "InvalidConfig";
        case CliArgsError::EmptyFilename:       return "EmptyFilename";
        case CliArgsError::UnexpectedPositional: return "UnexpectedPositional";
    }
    return "Unknown";
}

std::string cliHelpText() {
    // Cached as static-const so help text + repeated --help calls
    // don't reallocate. Help text is operator-visible documentation
    // — pin it stable across CLI extensions via the unit tests in
    // tests/program/test_cli_args.cpp.
    static std::string const text =
        "dss-code-prime — universal config-driven compiler\n"
        "\n"
        "Usage:\n"
        "  dss-code-prime --compile <file>... --language <name> "
            "--target <spec> [options]\n"
        "  dss-code-prime --transpile <file>... --language <name> "
            "--target <spec> [options]\n"
        "  dss-code-prime --directory <path> --language <name> "
            "--target <spec> [options]\n"
        "  dss-code-prime --project <file.dsp>     (plan 06 — not "
            "yet implemented)\n"
        "  dss-code-prime --lsp [--schema-dir=<path>]\n"
        "  dss-code-prime --help\n"
        "\n"
        "Modes (exactly one required):\n"
        "  --compile <file>...    explicit source-file list "
            "(native compilation)\n"
        "  --transpile <file>...  source-to-source translation "
            "(plan 10 — not yet implemented)\n"
        "  --directory <path>     recursively (or flat) scan a directory\n"
        "  --project <file.dsp>   load a project descriptor (plan 06 — "
            "not yet implemented)\n"
        "\n"
        "Common compile / transpile options:\n"
        "  --language <name>      source-language schema name "
            "(e.g. c-subset)\n"
        "  --target <spec>        <targetName>:<formatName> "
            "(repeatable; e.g. x86_64:elf64-x86_64-linux). The "
            "`=`-form is also accepted (--target=spec).\n"
        "  --recursive            scan subdirectories (default)\n"
        "  --no-recursive         only the top-level directory\n"
        "  --config=<debug|release>  build config "
            "(default: debug; release applies the full optimizer "
            "pipeline — plan 22)\n"
        "\n"
        "Diagnostic options:\n"
        "  --warnings-as-errors   promote every Warning to Error\n"
        "  --suppress=<code>      suppress a specific diagnostic code "
            "(repeatable; accepts D_FileNotFound or 0xD001)\n"
        "\n"
        "LSP / server mode:\n"
        "  --lsp                  run as a Language Server (stdio)\n"
        "  --schema-dir=<path>    path to additional shipped configs\n"
        "\n"
        "Examples:\n"
        "  dss-code-prime --compile hello.c --language c-subset "
            "--target x86_64:elf64-x86_64-linux\n"
        "  dss-code-prime --directory src/ --language c-subset "
            "--target x86_64:elf64-x86_64-linux --config=release\n";
    return text;
}

std::expected<CliArgs, CliArgsErrorInfo>
parseCliArgs(int argc, char* argv[]) {
    CliArgs out;

    // Mode-flag tracking: at most ONE of compile/transpile/directory/
    // project/lsp may be specified. The parser collects all mode
    // signals and surfaces conflicts after the loop (rather than at
    // first encounter — order-of-flags shouldn't matter for the
    // error message). Repeating the SAME mode flag is rejected as
    // a DuplicateModeFlag too (e.g. `--compile a.c --compile b.c`
    // — the user should write `--compile a.c b.c`).
    enum class Mode : std::uint8_t { None, Compile, Transpile, Directory, Project, Lsp };
    Mode mode = Mode::None;
    auto const setMode = [&](Mode m, std::string_view flag)
            -> std::expected<void, CliArgsErrorInfo> {
        if (mode != Mode::None) {
            return std::unexpected(make_error(
                CliArgsError::DuplicateModeFlag,
                std::string{"mode flag "} + std::string{flag}
                + (mode == m
                    ? " specified more than once — group positional "
                      "arguments under a single mode flag"
                    : " conflicts with an earlier mode flag — "
                      "exactly one of --compile / --transpile / "
                      "--directory / --project / --lsp may be "
                      "specified")));
        }
        mode = m;
        return {};
    };

    // Consume the positional file list following a mode flag like
    // `--compile` or `--transpile`. Stops at the next `-`-prefixed
    // arg. Empty strings and the bare `-` token are rejected as
    // EmptyFilename — protects against accidentally consuming a
    // shell-glob-expanded empty arg or treating `-` (stdin
    // shorthand) as a real filename. (silent-failure-hunter F2
    // post-fold.)
    auto const consumePositionals = [&](int& i, std::vector<std::string>& out,
                                         std::string_view flag)
            -> std::expected<void, CliArgsErrorInfo> {
        while (i + 1 < argc) {
            std::string_view const next{argv[i + 1]};
            // Check bare `-` and empty BEFORE the flag-prefix break:
            // `-` starts with "-" but is meant as the (rejected) stdin-
            // shorthand, not a flag. F2 post-fold ordering.
            if (next.empty() || next == "-") {
                return std::unexpected(make_error(
                    CliArgsError::EmptyFilename,
                    std::string{flag} + ": empty or bare-hyphen "
                    "positional argument is not a valid file path"));
            }
            if (next.starts_with("-")) break;
            ++i;
            out.emplace_back(argv[i]);
        }
        return {};
    };

    // Helper for the `=`-form of value flags. The non-`=` form is
    // handled by takeFlagValue() at each call site. silent-failure F4
    // post-fold: every value-bearing flag now accepts both
    // `--flag <value>` AND `--flag=value` shapes.
    //
    // Returns:
    //   * non-empty optional → matched the `prefix=<value>` form with
    //     a non-empty value
    //   * MissingFlagValue error → matched `prefix=` with an empty
    //     RHS (`--target=`, `--config=`, etc.). Symmetric reject with
    //     the non-`=` form: `takeFlagValue` rejects when no next arg
    //     exists AND (post-fold #2) when the next arg is itself empty.
    //     Without this symmetry, `--target=` would silently stuff `""`
    //     into `targets`, propagate, and fail late at the downstream
    //     `TargetSpec::parse` site. (silent-failure audit C2 post-fold #1.)
    //   * nullopt return → did NOT match the prefix-equals form
    auto const equalsValue = [](std::string_view a,
                                 std::string_view prefix)
            -> std::expected<std::optional<std::string_view>, CliArgsErrorInfo> {
        if (!a.starts_with(prefix) || a.size() <= prefix.size()
            || a[prefix.size()] != '=') {
            return std::optional<std::string_view>{std::nullopt};
        }
        auto const value = a.substr(prefix.size() + 1);
        if (value.empty()) {
            return std::unexpected(make_error(
                CliArgsError::MissingFlagValue,
                std::string{prefix} + "= requires a non-empty value"));
        }
        return std::optional<std::string_view>{value};
    };

    // Unified value-flag matcher: collapses the (space form ||
    // equals form) pair per flag into a single call site. Tries
    // `--flag value` first (consuming the next argv slot via
    // `takeFlagValue`), then `--flag=value`. Both forms produce a
    // non-empty value or fail loud with `MissingFlagValue`. Returns:
    //   * std::optional<std::string> with the value if matched
    //   * std::unexpected on empty value
    //   * nullopt return if `a` matches neither shape
    // Caller's call site shape:
    //   {
    //       auto m = valueFlag(a, i, "--language"); if (!m) return propagate;
    //       if (m->has_value()) { out.languageName = std::move(**m); continue; }
    //   }
    // (code-simplifier REQUIRED post-fold #2 — eliminates 5 (space +
    // equals) pairs of inline branches.)
    // `i` + `a` are bound to loop-local state at each call site;
    // pass them as explicit parameters so the lambda is reusable
    // across iterations without per-iteration recapture.
    auto const valueFlag = [&](std::string_view a, int& i,
                                std::string_view flag)
            -> std::expected<std::optional<std::string>, CliArgsErrorInfo> {
        if (a == flag) {
            auto v = takeFlagValue(a, i, argc, argv);
            if (!v) return std::unexpected(v.error());
            return std::optional<std::string>{std::move(*v)};
        }
        auto eqV = equalsValue(a, flag);
        if (!eqV) return std::unexpected(eqV.error());
        if (eqV->has_value()) {
            return std::optional<std::string>{std::string{**eqV}};
        }
        return std::optional<std::string>{std::nullopt};
    };

    for (int i = 1; i < argc; ++i) {
        std::string_view const a{argv[i]};

        // ── Help ────────────────────────────────────────────────
        if (a == "--help" || a == "-h") {
            out.helpMode = true;
            continue;
        }

        // ── LSP ─────────────────────────────────────────────────
        if (a == "--lsp") {
            if (auto e = setMode(Mode::Lsp, "--lsp"); !e) return std::unexpected(e.error());
            out.lspMode = true;
            continue;
        }
        {
            auto m = valueFlag(a, i, "--schema-dir");
            if (!m) return std::unexpected(m.error());
            if (m->has_value()) {
                out.lspSchemaDir = std::filesystem::path{std::move(**m)};
                continue;
            }
        }

        // ── Compile mode ────────────────────────────────────────
        if (a == "--compile") {
            if (auto e = setMode(Mode::Compile, "--compile"); !e) return std::unexpected(e.error());
            if (auto e = consumePositionals(i, out.sourceFiles, "--compile"); !e) return std::unexpected(e.error());
            continue;
        }

        // ── Transpile mode (plan 10 — fail-loud at dispatch) ────
        if (a == "--transpile") {
            if (auto e = setMode(Mode::Transpile, "--transpile"); !e) return std::unexpected(e.error());
            if (auto e = consumePositionals(i, out.transpileFiles, "--transpile"); !e) return std::unexpected(e.error());
            continue;
        }

        // ── Directory mode ──────────────────────────────────────
        {
            auto m = valueFlag(a, i, "--directory");
            if (!m) return std::unexpected(m.error());
            if (m->has_value()) {
                if (auto e = setMode(Mode::Directory, "--directory"); !e) return std::unexpected(e.error());
                out.directoryPath = std::move(**m);
                continue;
            }
        }
        {
            auto m = valueFlag(a, i, "--dir");
            if (!m) return std::unexpected(m.error());
            if (m->has_value()) {
                if (auto e = setMode(Mode::Directory, "--directory"); !e) return std::unexpected(e.error());
                out.directoryPath = std::move(**m);
                continue;
            }
        }

        // ── Project mode ────────────────────────────────────────
        {
            auto m = valueFlag(a, i, "--project");
            if (!m) return std::unexpected(m.error());
            if (m->has_value()) {
                if (auto e = setMode(Mode::Project, "--project"); !e) return std::unexpected(e.error());
                out.projectPath = std::move(**m);
                continue;
            }
        }

        // ── Compile-mode shared options ─────────────────────────
        {
            auto m = valueFlag(a, i, "--language");
            if (!m) return std::unexpected(m.error());
            if (m->has_value()) {
                out.languageName = std::move(**m);
                continue;
            }
        }
        {
            auto m = valueFlag(a, i, "--lang");
            if (!m) return std::unexpected(m.error());
            if (m->has_value()) {
                out.languageName = std::move(**m);
                continue;
            }
        }
        {
            auto m = valueFlag(a, i, "--target");
            if (!m) return std::unexpected(m.error());
            if (m->has_value()) {
                out.targets.push_back(std::move(**m));
                continue;
            }
        }
        if (a == "--recursive") {
            out.directoryMode = InputResolver::Mode::Recursive;
            continue;
        }
        if (a == "--no-recursive" || a == "--flat") {
            out.directoryMode = InputResolver::Mode::Flat;
            continue;
        }

        // ── --config=<debug|release> (plan 22 OPT* wiring) ──────
        {
            auto m = valueFlag(a, i, "--config");
            if (!m) return std::unexpected(m.error());
            if (m->has_value()) {
                auto const cfg = parseCompileConfig(**m);
                if (!cfg.has_value()) {
                    return std::unexpected(make_error(
                        CliArgsError::InvalidConfig,
                        std::string{"--config: '"} + **m
                        + "' is not a recognized configuration "
                          "(accepted: debug, release)"));
                }
                out.config = *cfg;
                continue;
            }
        }

        // ── Diagnostic policy (D-LK10-7 closure) ────────────────
        if (a == "--warnings-as-errors") {
            out.warningsAsErrors = true;
            continue;
        }
        {
            auto m = valueFlag(a, i, "--suppress");
            if (!m) return std::unexpected(m.error());
            if (m->has_value()) {
                auto const code = parseSuppressCode(**m);
                if (!code.has_value()) {
                    return std::unexpected(make_error(
                        CliArgsError::InvalidSuppressCode,
                        std::string{"--suppress: '"} + **m
                        + "' is not a recognized diagnostic-code name or hex value"));
                }
                out.suppress.push_back(*code);
                continue;
            }
        }

        // ── Unknown flag (fail loud) ────────────────────────────
        if (a.starts_with("-")) {
            return std::unexpected(make_error(
                CliArgsError::UnknownFlag,
                std::string{"unknown flag '"} + std::string{a}
                + "' (value flags also accept the '=' form, "
                  "e.g. --target=x86_64:elf64-x86_64-linux)"));
        }

        // Bare positional (post-flag) — distinct from UnknownFlag:
        // remediation is "move this token to follow --compile/
        // --transpile/--directory", NOT "spell the flag right"
        // (type-design audit post-fold #2 — UnknownFlag was overloaded).
        return std::unexpected(make_error(
            CliArgsError::UnexpectedPositional,
            std::string{"unexpected positional argument '"}
            + std::string{a} + "' — file lists belong after --compile "
              "or --transpile"));
    }

    // ── Post-parse validation ───────────────────────────────────
    //
    // `--schema-dir=<path>` is meaningful only in --lsp mode (the LSP
    // server reads it to locate `*.grammar.json` / `*.target.json` /
    // `*.format.json` at request-handling time). Outside --lsp the
    // flag has nowhere to apply; accepting it would silently drop the
    // user's directive (the LK10 cycle 3 dispatch only reads
    // `lspSchemaDir` inside `runLspMode`). (silent-failure audit H1
    // post-fold #1.)
    if (out.lspSchemaDir.has_value() && !out.lspMode) {
        return std::unexpected(make_error(
            CliArgsError::NoModeSelected,
            "--schema-dir=<path> is only meaningful with --lsp — "
            "pass --lsp to enter LSP mode"));
    }
    if (out.helpMode || out.lspMode) {
        // Help + LSP modes don't need compile options.
        return out;
    }
    if (mode == Mode::None) {
        // silent-failure F3 post-fold: if the user supplied
        // mode-options (language/targets/files/policy) without
        // selecting a mode flag, their input is being silently
        // discarded. Surface this as a hard NoModeSelected error.
        bool const hasOptions =
            !out.languageName.empty()
         || !out.targets.empty()
         || !out.sourceFiles.empty()
         || !out.transpileFiles.empty()
         || !out.suppress.empty()
         || out.warningsAsErrors
         || out.config != CompileConfig::Debug
         || out.directoryMode != InputResolver::Mode::Recursive;
        if (hasOptions) {
            return std::unexpected(make_error(
                CliArgsError::NoModeSelected,
                "mode-specific options were supplied but no mode flag "
                "was selected — pick exactly one of --compile / "
                "--transpile / --directory / --project / --lsp, or "
                "pass --help for usage"));
        }
        // No-arg invocation is allowed — falls back to the "ready"
        // message at the dispatch level.
        return out;
    }
    if (mode == Mode::Project) {
        // compileProject fails-loud at the dispatch layer with
        // `D_PlanNotLanded`. No further CLI validation needed.
        return out;
    }
    // Compile / Transpile / Directory modes require language + at
    // least one target.
    if (out.languageName.empty()) {
        return std::unexpected(make_error(
            CliArgsError::MissingLanguage,
            "--language <name> is required for compile / transpile / "
            "directory mode"));
    }
    if (out.targets.empty()) {
        return std::unexpected(make_error(
            CliArgsError::EmptyTargetList,
            "at least one --target <spec> is required for compile / "
            "transpile / directory mode (e.g. --target "
            "x86_64:elf64-x86_64-linux)"));
    }
    if (mode == Mode::Compile && out.sourceFiles.empty()) {
        return std::unexpected(make_error(
            CliArgsError::EmptyFileList,
            "--compile mode requires at least one source file"));
    }
    if (mode == Mode::Transpile && out.transpileFiles.empty()) {
        return std::unexpected(make_error(
            CliArgsError::EmptyFileList,
            "--transpile mode requires at least one source file"));
    }
    return out;
}

} // namespace dss
