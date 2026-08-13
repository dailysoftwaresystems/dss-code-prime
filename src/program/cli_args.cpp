#include "program/cli_args.hpp"

#include "core/types/ascii_case.hpp"
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
    // Case-insensitive accept via `asciiToLower` (hoisted to
    // `core/types/ascii_case.hpp` at D-LK6-1 post-fold #2 — was
    // a duplicated loop; second consumer is `parseRelocFormulaKind`).
    auto const lowered = asciiToLower(spec);
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
        case CliArgsError::InvalidDefine:        return "InvalidDefine";
        case CliArgsError::InvalidJobs:          return "InvalidJobs";
        case CliArgsError::InvalidStackReserve:  return "InvalidStackReserve";
        case CliArgsError::InvalidResolveLibrary: return "InvalidResolveLibrary";
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
        "  dss-code-prime --compile <file>... [--language <name>] "
            "--target <spec> [options]\n"
        "  dss-code-prime --transpile <file>... --language <name> "
            "--target <spec> [options]\n"
        "  dss-code-prime --directory <path> [--language <name>] "
            "--target <spec> [options]\n"
        "  dss-code-prime --project <file.dss-project.json>\n"
        "  dss-code-prime --dump-predefined-macros --language <name> "
            "--target <spec>\n"
        "  dss-code-prime --lsp [--schema-dir=<path>]\n"
        "  dss-code-prime --help\n"
        "\n"
        "Modes (exactly one required):\n"
        "  --compile <file>...    explicit source-file list "
            "(native compilation)\n"
        "  --transpile <file>...  source-to-source translation "
            "(plan 10 — not yet implemented)\n"
        "  --directory <path>     recursively (or flat) scan a directory\n"
        "  --project <file>       load a project config "
            "(.dss-project.json: language/artifactProfile/targets/"
            "sources — plan 06)\n"
        // FC18 companion: the ONLY way to see the effective predefined-macro
        // set by inspection. Discoverability matters more here than for most
        // flags — the claim "DSS defines none of the C23 __STDC_NO_* macros"
        // was previously verifiable only by reading three config files.
        "  --dump-predefined-macros  print the EFFECTIVE predefined-macro "
            "set for the resolved (language x target x object-format) triple "
            "and exit without compiling — DSS's `gcc -dM -E`. Requires "
            "--language and at least one --target (repeatable: one section "
            "per target). Each line carries the macro's ORIGIN "
            "(language/target/format/command-line) and KIND; `--define` "
            "entries are included, so the output is what a translation unit "
            "actually SEES, not merely what the configs declare. An "
            "offset-derived kind (line/file) and a function-like macro have "
            "no single value and say so rather than print a fabricated one. "
            "There is no `-dM` alias.\n"
        "\n"
        "Common compile / transpile options:\n"
        "  --language <name>      source-language schema name "
            "(e.g. c-subset). REQUIRED for --transpile and "
            "--dump-predefined-macros. OPTIONAL for --compile / --directory: "
            "omitted, each --target supplies the language it declares as its "
            "own assembly dialect ('defaultAssemblyLanguage'), which is how "
            "one invocation compiles a .s for two different CPUs — both "
            "shipped dialects claim .s, so the extension alone cannot name "
            "one. A named --language wins over every target's default.\n"
        "  --target <spec>        <targetName>:<formatName> "
            "(repeatable; e.g. x86_64:elf64-x86_64-linux). The "
            "`=`-form is also accepted (--target=spec).\n"
        "  --recursive            scan subdirectories (default)\n"
        "  --no-recursive         only the top-level directory\n"
        "  --output <dir>         route emitted binaries into "
            "<dir> (default: <cwd>/target/<formatName>/). Multi-"
            "target builds get a per-format subdirectory.\n"
        "  --config=<debug|release>  build config "
            "(default: debug; release applies the full optimizer "
            "pipeline — plan 22)\n"
        "  --jobs <N>             per-CU build parallelism "
            "(default: auto = min(cores, TUs, 16); --jobs 1 = serial). "
            "Only the multi-source build parallelizes; the `=`-form is "
            "also accepted.\n"
        // D-CLI-HELP-OMITS-DEFINE-FLAG: `--define` has been parsed since c105
        // but was absent here, so the only discoverable spelling was the
        // source. There is no `-D` alias, and reaching for one is the mistake
        // this entry exists to prevent.
        "  --define NAME[=VALUE]  predefine an object-like macro "
            "(repeatable; VALUE defaults to `1`). NAME must be a single "
            "identifier; a function-like `--define` is unsupported — declare "
            "a config predefinedMacros entry for that. There is no `-D` "
            "alias. The `=`-form is also accepted (--define=NAME[=VALUE]).\n"
        "  --resolve-library <path>[=<import-name>]  read a binary's "
            "(.so/.dll/.dylib) export table to resolve + validate this "
            "build's externs against it (repeatable; typically a DSS-built "
            "library). The optional `=<import-name>` STATES the runtime "
            "identity to record (DT_NEEDED / LC_LOAD_DYLIB / PE import "
            "name) instead of the binary's own embedded soname — for "
            "cross-compiling against a stand-in library whose on-disk "
            "identity is not the target's. Split on the LAST `=`; both "
            "sides must be non-empty.\n"
        "  --stack-reserve <bytes>  stack reserve to request in the emitted "
            "image, in bytes (e.g. 4194304 = 4 MiB). Overrides the object "
            "format's default; a project manifest's `stackReserve` key is "
            "the file-driven equivalent and this flag WINS over it. Only "
            "formats that declare the capability accept it (a PE .exe does; "
            "an ELF image has no stack-size field -- there it is a runtime "
            "`ulimit -s` property) -- a request a format cannot carry is "
            "REFUSED, never silently dropped. The `=`-form is also "
            "accepted.\n"
        "\n"
        "Diagnostic options:\n"
        "  --warnings-as-errors   promote every Warning to Error\n"
        "  --suppress=<code>      suppress a specific diagnostic code "
            "(repeatable; accepts D_FileNotFound or 0xD001)\n"
        "  --time                 print the compilation wall-clock time to "
            "stderr after the compile finishes\n"
        "\n"
        "LSP / server mode:\n"
        "  --lsp                  run as a Language Server (stdio)\n"
        "  --schema-dir=<path>    path to additional shipped configs\n"
        "\n"
        "Examples:\n"
        "  dss-code-prime --compile hello.c --language c-subset "
            "--target x86_64:elf64-x86_64-linux\n"
        "  dss-code-prime --directory src/ --language c-subset "
            "--target x86_64:elf64-x86_64-linux --config=release\n"
        "  dss-code-prime --dump-predefined-macros --language c-subset "
            "--target x86_64:pe64-x86_64-windows-exec --define SQLITE_TEST\n"
        "  dss-code-prime --compile boot.s "
            "--target x86_64:elf64-x86_64-linux "
            "--target arm64:elf64-aarch64-linux   (no --language: each "
            "target picks its own assembly dialect)\n";
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
    enum class Mode : std::uint8_t { None, Compile, Transpile, Directory, Project, Lsp,
                                     DumpPredefinedMacros };
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
                      "--directory / --project / --lsp / "
                      "--dump-predefined-macros may be "
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

        // ── Predefined-macro dump (a MODE: prints, compiles nothing) ──
        if (a == "--dump-predefined-macros") {
            if (auto e = setMode(Mode::DumpPredefinedMacros,
                                 "--dump-predefined-macros");
                !e) {
                return std::unexpected(e.error());
            }
            out.dumpPredefinedMacros = true;
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
        {
            // D-LK10-ENTRY Slice C companion: `--output <dir>` /
            // `--output=<dir>` routes emitted binaries.
            auto m = valueFlag(a, i, "--output");
            if (!m) return std::unexpected(m.error());
            if (m->has_value()) {
                out.outputDir = std::filesystem::path{std::move(**m)};
                continue;
            }
        }
        {
            // c105 (D-PP-USER-DEFINE): `--define NAME[=VALUE]` (repeatable).
            // STRUCTURAL checks only here — a non-empty NAME and no '(' in
            // NAME (a function-like --define is unsupported; config
            // predefinedMacros carry the params axis). Tokenizer-true name
            // validation happens in the preprocessor's directive handler.
            auto m = valueFlag(a, i, "--define");
            if (!m) return std::unexpected(m.error());
            if (m->has_value()) {
                std::string d = std::move(**m);
                auto const eq = d.find('=');
                std::string_view const name =
                    std::string_view{d}.substr(0, eq);
                if (name.empty()) {
                    return std::unexpected(CliArgsErrorInfo{
                        CliArgsError::InvalidDefine,
                        "--define requires a non-empty macro NAME "
                        "(--define NAME[=VALUE]); got: '" + d + "'"});
                }
                if (name.find('(') != std::string_view::npos) {
                    return std::unexpected(CliArgsErrorInfo{
                        CliArgsError::InvalidDefine,
                        "a function-like --define is not supported (got '" + d
                            + "'); declare a config predefinedMacros entry "
                              "with 'params' instead"});
                }
                // c105 audit M1: NAME must be a single C-family identifier —
                // trailing junk (`--define "FOO,BAR=1"`) would otherwise flow
                // into the prologue as `#define FOO,BAR 1`, which the directive
                // handler parses as name FOO with replacement `, BAR 1`: a
                // SILENTLY WRONG macro (gcc hard-errors here: "macro names
                // must be identifiers"). A byte-class check is honest at this
                // C-family CLI surface (identifiers are ASCII-classed in every
                // C dialect; the engine-tier tokenizer still re-validates the
                // first token).
                bool nameOk = !(name[0] >= '0' && name[0] <= '9');
                for (char const c : name) {
                    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                          || (c >= '0' && c <= '9') || c == '_')) {
                        nameOk = false;
                        break;
                    }
                }
                if (!nameOk) {
                    return std::unexpected(CliArgsErrorInfo{
                        CliArgsError::InvalidDefine,
                        "--define NAME must be a single identifier "
                        "([A-Za-z_][A-Za-z0-9_]*); got: '" + d + "'"});
                }
                out.defines.push_back(std::move(d));
                continue;
            }
        }
        {
            // `-I<dir>` / `-I <dir>` / `--include-dir <dir>` / `--include-dir=<dir>`
            // (repeatable): the C quote-include search path (gcc/clang's `-I`).
            // Each dir is threaded to every CU's include dirs (searched AFTER the
            // including file's own directory, C 6.10.2 quote form). The attached
            // `-I<dir>` form is what real build recipes emit (SQLite's testfixture
            // uses `-I.`); the spaced + long forms round it out. Structural only
            // here (a non-empty dir); a nonexistent dir is NOT an error (gcc
            // parity — it simply contributes no hits, and a genuinely-missing
            // header still fails loud P0016 downstream).
            if (a == "-I") {                                   // -I <dir> (space)
                auto v = takeFlagValue(a, i, argc, argv);
                if (!v) return std::unexpected(v.error());
                out.includeDirs.push_back(std::move(*v));
                continue;
            }
            if (a.size() > 2 && a.substr(0, 2) == "-I") {       // -I<dir> (attached)
                out.includeDirs.push_back(std::string{a.substr(2)});
                continue;
            }
            auto m = valueFlag(a, i, "--include-dir");          // --include-dir <dir>/=<dir>
            if (!m) return std::unexpected(m.error());
            if (m->has_value()) {
                out.includeDirs.push_back(std::move(**m));
                continue;
            }
        }
        {
            // c162 (D-FF1-READER-CONSUMER): `--resolve-library <path>`
            // (repeatable). Names a binary whose export surface resolves
            // this build's source-declared externs. Structural check only
            // here (non-empty value, enforced by `valueFlag`); the binary
            // is opened + read at compile time by the FF5 ingest consumer,
            // which fails loud on a missing/unreadable file or an absent
            // symbol.
            //
            // D-FFI-DECLARED-IMPORT-NAME: the value's OPTIONAL `=<import-name>`
            // suffix STATES the runtime identity to record for every symbol
            // read out of `<path>`, outranking the binary's own embedded
            // soname (`ffi::BinaryLibrarySource` docblock). SPELLING CHOICE:
            // `<path>[=<import-name>]` reuses this file's ONE value-bearing-
            // flag convention verbatim -- `valueFlag` already accepts both
            // `--resolve-library V` and `--resolve-library=V`, and `--define
            // NAME[=VALUE]` already establishes "an OPTIONAL `=`-separated
            // second component inside the value". So no new flag, no new
            // shape, and the `=`-form of the flag composes for free
            // (`--resolve-library=/p/lib.dylib=libtcl8.6.dylib` -- `equalsValue`
            // strips only the flag's own first `=`).
            auto m = valueFlag(a, i, "--resolve-library");
            if (!m) return std::unexpected(m.error());
            if (m->has_value()) {
                std::string v = std::move(**m);
                ResolveLibrarySpec spec;
                // LAST `=`, not the first: an import name (a soname / DLL name
                // / install name) does not contain `=`, whereas a PATH can.
                // The opposite direction from `--define`, where the VALUE is
                // the `=`-tolerant side -- documented on `CliArgs::
                // resolveLibraries` together with the residual ambiguity.
                auto const eq = v.rfind('=');
                if (eq == std::string::npos) {
                    spec.path = std::move(v);   // plain form: nothing stated
                } else {
                    std::string path = v.substr(0, eq);
                    std::string name = v.substr(eq + 1);
                    // Both sides fail LOUD. An empty path reads nothing; an
                    // empty name would record an import library the loader can
                    // never resolve -- a link that succeeds and an artifact
                    // that dies at load, the exact silent failure this flag
                    // exists to prevent.
                    if (path.empty()) {
                        return std::unexpected(CliArgsErrorInfo{
                            CliArgsError::InvalidResolveLibrary,
                            "--resolve-library requires a non-empty PATH before "
                            "the '=' (--resolve-library <path>[=<import-name>]); "
                            "got: '" + v + "'"});
                    }
                    if (name.empty()) {
                        return std::unexpected(CliArgsErrorInfo{
                            CliArgsError::InvalidResolveLibrary,
                            "--resolve-library requires a non-empty IMPORT NAME "
                            "after the '=' (--resolve-library "
                            "<path>[=<import-name>]); got: '" + v
                            + "'. Omit the '=' entirely to record the binary's "
                              "own embedded soname."});
                    }
                    spec.path               = std::move(path);
                    spec.declaredImportName = std::move(name);
                }
                out.resolveLibraries.push_back(std::move(spec));
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
        if (a == "--time") {
            out.time = true;
            continue;
        }
        {
            // D-PERF-4-CU-PARALLELISM: `--jobs N` / `--jobs=N` — worker count for
            // the per-CU build pool. Must be a positive integer (>= 1); a non-
            // numeric value, trailing junk, or 0 fails loud (InvalidJobs) rather
            // than silently falling back to auto (which would mask a typo'd -j).
            auto m = valueFlag(a, i, "--jobs");
            if (!m) return std::unexpected(m.error());
            if (m->has_value()) {
                std::string const& v = **m;
                unsigned n = 0;
                auto const [p, ec] = std::from_chars(
                    v.data(), v.data() + v.size(), n);
                if (ec != std::errc{} || p != v.data() + v.size() || n == 0) {
                    return std::unexpected(make_error(
                        CliArgsError::InvalidJobs,
                        std::string{"--jobs: '"} + v
                        + "' is not a positive integer worker count "
                          "(e.g. --jobs 4; use --jobs 1 for a serial build)"));
                }
                out.jobs = n;
                continue;
            }
        }
        {
            // D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: `--stack-reserve <bytes>`
            // / `--stack-reserve=<bytes>` — the per-PROGRAM stack reserve the
            // emitted image should carry (the DSS spelling of MSVC `/STACK` /
            // GNU ld `-Wl,--stack`). Must be a positive integer byte count; a
            // non-numeric value, trailing junk, or 0 fails loud
            // (InvalidStackReserve) rather than silently falling back to the
            // format default — a request that vanishes is the exact failure
            // this capability exists to prevent.
            //
            // Deliberately NOT validated for RANGE or ALIGNMENT here: those
            // bounds are declared by the chosen OBJECT FORMAT
            // (`stackReserveControl`), which the CLI parser has not resolved
            // yet, so the linker gate owns that check. Parsing what is
            // syntactically a byte count is all this layer can honestly do.
            auto m = valueFlag(a, i, "--stack-reserve");
            if (!m) return std::unexpected(m.error());
            if (m->has_value()) {
                std::string const& v = **m;
                std::uint64_t n = 0;
                auto const [p, ec] = std::from_chars(
                    v.data(), v.data() + v.size(), n);
                // `ec` catches non-numeric AND overflow; because `n` is
                // UNSIGNED, from_chars also rejects a leading '-' as
                // invalid_argument, so negatives need no separate branch.
                // `p != end` catches trailing junk (`4096x` must not silently
                // become 4096).
                if (ec != std::errc{} || p != v.data() + v.size() || n == 0) {
                    return std::unexpected(make_error(
                        CliArgsError::InvalidStackReserve,
                        std::string{"--stack-reserve: '"} + v
                        + "' is not a positive integer byte count "
                          "(e.g. --stack-reserve 4194304 for 4 MiB). The "
                          "value must be in the range the target's object "
                          "format declares; a format that cannot carry a "
                          "stack reserve rejects the request outright."));
                }
                out.stackReserveBytes = n;
                continue;
            }
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
    // D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: `--stack-reserve` asks for a field
    // in an EMITTED IMAGE, so it is meaningless in any mode that emits no
    // image — `--transpile` writes translated SOURCE, `--lsp` serves a
    // protocol, `--help` prints text. Accepting it there would silently
    // discard the request, which is the same silent-drop this capability
    // exists to eliminate (the linker gate can only refuse a request that
    // reaches it; in these modes nothing ever does). Mirrors the established
    // `--schema-dir` mode-gate above, and uses the same error kind.
    //
    // Mode::None is deliberately EXCLUDED: bare `--stack-reserve` with no
    // mode flag falls through to the generic no-mode guard below, whose
    // message ("pick exactly one of ...") is the more useful one there.
    if (out.stackReserveBytes.has_value() && mode != Mode::None
     && mode != Mode::Compile && mode != Mode::Directory
     && mode != Mode::Project) {
        return std::unexpected(make_error(
            CliArgsError::NoModeSelected,
            "--stack-reserve requests a field in an emitted IMAGE, so it is "
            "only meaningful for a mode that produces one (--compile / "
            "--directory / --project). Transpile emits source and --lsp "
            "emits nothing, so the request would be silently discarded."));
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
         || !out.resolveLibraries.empty()  // c162 (D-FF1-READER-CONSUMER)
         || out.warningsAsErrors
         || out.time
         || out.jobs != 0  // D-PERF-4: --jobs supplied without a mode flag
         // D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: --stack-reserve without a
         // mode flag would silently discard the request.
         || out.stackReserveBytes.has_value()
         || out.config != CompileConfig::Debug
         || out.directoryMode != InputResolver::Mode::Recursive;
        if (hasOptions) {
            return std::unexpected(make_error(
                CliArgsError::NoModeSelected,
                "mode-specific options were supplied but no mode flag "
                "was selected — pick exactly one of --compile / "
                "--transpile / --directory / --project / --lsp / "
                "--dump-predefined-macros, or pass --help for usage"));
        }
        // No-arg invocation is allowed — falls back to the "ready"
        // message at the dispatch level.
        return out;
    }
    if (mode == Mode::Project) {
        // `--project <file>` carries its own config (language, targets,
        // sources) inside the `.dss-project.json`; the CLI does not
        // require --language/--target here. `compileProject` (plan 06
        // AP2) loads + validates that file and fails loud on any
        // structural / profile error. No further CLI validation needed.
        return out;
    }
    // Compile / Transpile / Directory / DumpPredefinedMacros modes require
    // language + at least one target. `--dump-predefined-macros` joins the list
    // for the same reason the other three are on it — the effective
    // predefined-macro set is a property of a (language x target x
    // object-format) TRIPLE, so without both halves there is nothing to answer.
    // ★ The message NAMES every mode it can be reached from: a mode-list that
    // omits the mode the operator actually typed reads as "this flag is not
    // supported", which is the opposite of what happened.
    // ★★ D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET: `--language` is now OPTIONAL
    // for --compile and --directory, and REQUIRED for the other two. The split
    // is not a convenience — it is where a second answer exists.
    //
    // `--compile` / `--directory` have a TARGET, and a target declares the
    // name of the source language that spells its own assembly
    // (`defaultAssemblyLanguage`). That second answer is the ONLY way one
    // invocation can compile a `.s` for two different CPUs: both shipped
    // dialects claim `.s`/`.S`, so the extension cannot name one and a
    // single `--language` would force the wrong dialect on every target but
    // one. Omitting the flag now means "ask each target"; naming it still
    // wins outright. If neither answer exists, the driver fails loud NAMING
    // THE TARGET — the flag moved from CLI-required to driver-enforced, not
    // from required to optional-and-forgiving.
    //
    // `--transpile` and `--dump-predefined-macros` keep the hard requirement,
    // and for opposite-looking but identical reasons: the target's declared
    // language is its ASSEMBLY dialect, which answers "what is this `.s`?" and
    // answers nothing about a source-to-source translation's INPUT language or
    // about which language's predefined macros to print. Defaulting either to
    // the assembly dialect would produce a confidently wrong answer where
    // there is currently a precise error.
    bool const languageOptional =
        mode == Mode::Compile || mode == Mode::Directory;
    if (out.languageName.empty() && !languageOptional) {
        return std::unexpected(make_error(
            CliArgsError::MissingLanguage,
            "--language <name> is required for transpile / "
            "dump-predefined-macros mode (for compile / directory mode it is "
            "OPTIONAL — omitted, each --target supplies the source language "
            "it declares as its assembly dialect)"));
    }
    if (out.targets.empty()) {
        return std::unexpected(make_error(
            CliArgsError::EmptyTargetList,
            "at least one --target <spec> is required for compile / "
            "transpile / directory / dump-predefined-macros mode (e.g. "
            "--target x86_64:elf64-x86_64-linux)"));
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
