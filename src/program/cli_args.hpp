#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/resolve_library_spec.hpp"  // ResolveLibrarySpec (shared
                                                // with the manifest surface)
#include "program/input_resolver.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// CLI argument parsing — LK10 cycle 3 (plan 14 §3 LK10 cycle 3
// landed 2026-06-01). The dsscp CLI's argv → structured
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

// `ResolveLibrarySpec` — the `--resolve-library` value type — now lives in
// `core/types/resolve_library_spec.hpp`, which this header includes above.
// It moved DOWN when the project-config loader that parses the same value out
// of a manifest moved into `core`
// (D-LSP-PROJECT-CONFIG-LIVES-ABOVE-ITS-CONSUMERS); leaving it here would have
// kept `core` reaching up into the driver tier for it. The type is unchanged —
// still ONE type for the CLI, the manifest, `Program`, and `CompileOptions`.

struct DSS_EXPORT CliArgs {
    // ── Mode flags (mutually exclusive at dispatch time) ────────
    bool                     lspMode     = false;
    bool                     helpMode    = false;
    // FC18 companion (`--dump-predefined-macros`): print the EFFECTIVE
    // predefined-macro set for the resolved (language × target × object-format)
    // triple, then exit 0 having compiled nothing. DSS's answer to `gcc -dM -E`.
    //
    // ★ WHY IT IS A MODE AND NOT AN OPTION. The question it answers is about an
    // EMPTY translation unit — no source file participates, so there is nothing
    // for it to decorate a compile with, and letting it ride alongside
    // `--compile` would leave "does the compile still happen?" ambiguous. Being
    // a mode also means it inherits the tail validation that DEMANDS
    // `--language` and at least one `--target`: without a resolved triple there
    // is no effective set, only a language-only approximation that an operator
    // could mistake for a per-target answer.
    //
    // Repeatable `--target` prints ONE SECTION PER TARGET, because the whole
    // point of the instrument is that the answer DIFFERS per target — an
    // instrument that silently answered for only the first of three legs would
    // be the exact silent-partial it exists to rule out.
    //
    // NO `-dM` ALIAS, and none should be added: `-dM` is one letter-cluster of
    // gcc's `-d<CHARS>` dump family, and claiming that spelling would promise a
    // family DSS does not implement. (`--define` likewise has no `-D` alias.)
    bool                     dumpPredefinedMacros = false;
    std::vector<std::string> sourceFiles; // populated by --compile <files>...
    std::vector<std::string> transpileFiles; // populated by --transpile <files>...
    std::optional<std::string> directoryPath; // populated by --directory <path>
    std::optional<std::string> projectPath;   // populated by --project <file>

    // ── Compile-mode shared options ─────────────────────────────
    // --language <name>. EMPTY is a legal state for --compile / --directory
    // (D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET) and means "ask each --target
    // for the language it declares as its own assembly dialect" — the only way
    // one invocation can compile a `.s` for two different CPUs. Still REQUIRED
    // for --transpile / --dump-predefined-macros, where the target's answer
    // would be a confidently wrong one.
    std::string              languageName;
    std::vector<std::string> targets;          // --target <spec> (repeatable)
    InputResolver::Mode      directoryMode =
        InputResolver::Mode::Recursive;        // --recursive / --no-recursive
    CompileConfig            config = CompileConfig::Debug;  // --config=release|debug
    // c105 (D-PP-USER-DEFINE): `--define NAME[=VALUE]` (repeatable; VALUE
    // defaults to 1) — the universal -D. Entries are carried VERBATIM to the
    // preprocessor, which lowers each to a `#define` line in the synthetic
    // "<command-line>" prologue (ordinary macros: #undef-able, C 6.10.3p2
    // duplicate policy, LOUD on a config-predefine collision). The CLI layer
    // does only the STRUCTURAL checks (non-empty NAME; no '(' — a
    // function-like --define is not supported); tokenizer-true name
    // validation happens in the directive handler where the real lexer lives.
    std::vector<std::string> defines;          // --define NAME[=VALUE]

    // c162 (D-FF1-READER-CONSUMER): `--resolve-library <path>` (repeatable;
    // the `=`-form is also accepted). Each names a real binary (a `.so` /
    // `.dll` / `.dylib`, typically a DSS-BUILT library) whose EXPORT surface
    // is READ (via the FF1 binary reader) to resolve this build's
    // source-declared externs -- binding each matched extern to that library
    // AND validating (fail loud) that the library actually exports it. A
    // DSS-built library has no shipped JSON descriptor, so reading its real
    // export table is the only way to link against it (a genuine,
    // non-duplicative capability). Threaded to `CompileOptions.resolveLibraries`.
    //
    // D-FFI-DECLARED-IMPORT-NAME: the full spelling is
    // `--resolve-library <path>[=<import-name>]`, mirroring `--define
    // NAME[=VALUE]` -- one value-bearing flag whose value carries an OPTIONAL
    // `=`-separated second component, so no new flag enters the surface. With
    // the suffix, `<import-name>` is the runtime identity RECORDED for every
    // symbol read out of `<path>`, outranking the binary's own embedded
    // soname; without it, nothing is stated and the pre-existing precedence
    // stands byte-for-byte.
    //
    // SPLIT ON THE LAST `=`, the OPPOSITE of `--define` (which splits on the
    // first, because a macro VALUE may contain `=`). Here it is the PATH that
    // may contain `=` while an import name -- a soname / DLL name / install
    // name -- realistically never does. A Windows drive letter uses `:`, not
    // `=`, so `C:\lib\foo.dll` is unaffected. RESIDUAL, and LOUD not silent: a
    // path that really does contain `=` AND no intended override is split at
    // that `=`, and the truncated path then fails the compile-time open probe
    // with `F_FileOpenFailed` naming the truncated path (see
    // `compile_pipeline.cpp` step 2.5-pre). Use the project-manifest object
    // form, which has no separator at all, for such a path.
    std::vector<ResolveLibrarySpec> resolveLibraries;

    // `-I<dir>` / `-I <dir>` / `--include-dir <dir>` (repeatable): the C
    // quote-include search path (gcc/clang's `-I`). Each dir is threaded to
    // every CompilationUnit's include dirs (UnitBuilder::addIncludeDir),
    // searched AFTER the including file's own directory (C 6.10.2 quote form).
    // Needed for multi-directory source trees — e.g. SQLite's testfixture
    // compiles `src/test*.c` with `-I. -I<src> -I<ext/...>` so a TU's
    // `#include "sqlite3.h"` reaches the generated header in the build dir.
    // Carried verbatim; the driver resolves each to an absolute path (relative
    // dirs are cwd-relative, gcc semantics) when it threads them to the builder.
    std::vector<std::string> includeDirs;       // -I<dir> / --include-dir <dir>

    // ── Output routing (D-LK10-ENTRY Slice C companion) ─────────
    //
    // `--output <dir>` (or `--output=<dir>`) routes every emitted
    // binary into the named directory. Without this flag, the
    // driver emits to `<cwd>/target/<formatName>/<binary><ext>`
    // (the cycle-2 v1 convention; plan 6 owns the authoritative
    // artifact-profile-driven scheme).
    //
    // Directory creation: the driver calls `fs::create_directories`
    // on the resolved output path — intermediates are auto-created.
    // Failure surfaces as `D_OutputDirCreateFailed` (existing code
    // for the legacy `<cwd>/target/...` path; same code on the
    // `--output` path). Empty value rejects as
    // `CliArgsError::MissingFlagValue` (consistent with
    // `--target=""` / `--language=""`).
    //
    // When multiple targets are declared via repeated `--target`,
    // the driver appends the FORMAT NAME as a subdirectory
    // (`<output>/<formatName>/<binary>`) so multi-target builds
    // don't collide on the same file name. With a single target
    // the binary lands directly in `<output>/<binary>`. (Format
    // name encodes machine+OS, so it's the right disambiguator —
    // matches the legacy convention's `<formatName>` subdir.)
    std::optional<std::filesystem::path> outputDir;

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

    // `--max-diagnostics <count>` / `--max-diagnostics=<count>`: the run-wide
    // GLOBAL diagnostic cap — `DiagnosticReporter::Config::maxDiagnostics`.
    // Once the reporter has accumulated this many, it stops accumulating and
    // starts COUNTING: one `P_TooManyDiagnostics` marker carries the limit and
    // the running dropped total (`DiagnosticReporter::noteCapDrop_`). This flag
    // is the operator's half of that marker's remedy sentence — before it
    // existed the notice could only name a C++ field no CLI user can reach.
    //
    // ★ std::optional, and the nullopt arm is the WHOLE POINT: absent means
    // "do not write the field at all", so `DiagnosticReporter::Config`'s own
    // in-class initializer stays the SINGLE source of truth for the default.
    // Spelling the default here — as a literal, or even as a copy of
    // `Config{}.maxDiagnostics` — would create the second place it is written
    // down, which is the exact drift this flag exists to make visible. The
    // `--help` text derives the number it prints from `Config{}` for the same
    // reason. Mirrors `stackReserveBytes` above (absent = the downstream
    // default stands).
    //
    // ★ ZERO IS LEGAL, and that is a decision about what `report()` DOES, not
    // a matter of taste. The gate is `all_.size() >= cfg_.maxDiagnostics` on
    // unsigned operands, so at 0 the first cap-eligible report trips the cap,
    // is counted, and is replaced by the marker — no crash, no fatal path
    // (`noteCapDrop_` pushes the marker BEFORE it counts, so its
    // `capMarkerIndex_ >= all_.size()` guard holds), and no undefined corner.
    // The result is coherent and otherwise unobtainable: "print no diagnostics,
    // just tell me how many there were". Nor can 0 silence what must never be
    // silenced — `DiagnosticDelivery::Guaranteed` and `kUnsuppressableCodes`
    // bypass the cap at EVERY value, 0 included. And 0 is not a sentinel for
    // anything here (absence is nullopt), which is precisely the difference
    // from `--jobs 0`: there 0 IS the AUTO sentinel, so accepting it would
    // silently reinterpret an explicit request, and that is why THAT flag
    // rejects it. Rejecting 0 here would instead make the CLI a narrower,
    // disagreeing source of truth about a domain the library already defines.
    //
    // ⚠ CONSEQUENCE THE OPERATOR SHOULD KNOW, inherent to the cap at any finite
    // value rather than to 0: the `P_TooManyDiagnostics` marker is ERROR
    // severity, so a cap small enough to trip turns an otherwise-clean run into
    // a failing one. That is the cap working — the run genuinely cannot be
    // certified clean once its own report of the elision is the loudest thing
    // in the stream — not a defect of this flag.
    //
    // ✔MEASURED, so the docblock does not overstate the reach: this value lands
    // on the run-wide AGGREGATION reporter only. Every compile phase runs
    // against a scratch reporter whose cap `runCusToTargets` / `compileOneTarget`
    // explicitly relax to SIZE_MAX, so `--max-diagnostics` does NOT change what
    // `HirVerifier` / `MirVerifier` observe through `hitCap()` — their reporters
    // are uncappable by construction on every CLI path.
    std::optional<std::size_t>    maxDiagnostics;

    // `--time` prints the compilation's wall-clock duration to stderr after the
    // compile finishes (any compile-producing mode). Diagnostic-neutral, off by
    // default; `--time` with no mode flag is a hard NoModeSelected error.
    bool                          time = false;

    // AP6 (`.plans/06-artifact-profile-plan` §5.1 B.4): `--force-git-cache`
    // bypasses the `.dss-deps` cache-HIT short-circuit and re-fetches every git
    // `dependsOn` entry, even one whose recorded commit still matches. Threaded
    // to `Program::setForceGitCache`.
    //
    // ⚠ THE NAME MEANS "FORCE A REFRESH OF THE CACHE", NOT "FORCE USE OF THE
    // CACHE" — it reads naturally as the second and does the first. The
    // OPERATOR CHOSE IT; it ships as spelled, and the mitigation is the
    // imperative gloss in `--help` ("re-fetch git dependencies even when the
    // cache is valid"), never a rename. B.4's own implementer note says so in
    // as many words, so this comment exists to stop the rename being
    // re-proposed by the next reader who notices.
    //
    // WHAT IT DOES **NOT** CHANGE: every other rule of the cache machine. A
    // fetch that fails with a usable checkout present still emits
    // `D_DependencyGitFetchFallback` at Info and still BUILDS — so
    // `--force-git-cache` on an offline machine with a checkout is not an
    // error, which is the whole offline-build guarantee holding under the flag.
    //
    // U-10: a silent NO-OP when the project declares no git dependency,
    // consistent with an empty `preBuildScripts`. That is a property of the
    // resolver opening its cache lazily, not of a check here.
    bool                          forceGitCache = false;

    // `--jobs N` / `--jobs=N` (D-PERF-4-CU-PARALLELISM): worker-thread count for
    // the per-CU build pool (the multi-TU `compileUnits` path builds each CU's
    // MIR concurrently). 0 (the default / absent) = AUTO = min(hardware_
    // concurrency, CU count, 16). `--jobs 1` forces a single-worker (serialized)
    // build — the deterministic baseline. A non-numeric or zero value fails loud
    // (InvalidJobs); the number is stored verbatim and clamped to the CU count at
    // pool construction. Threaded to `Program::setJobs`.
    unsigned                      jobs = 0;

    // `--stack-reserve <bytes>` / `--stack-reserve=<bytes>`
    // (D-SQLITE-PE64-FULL-TIER-STACK-DEPTH): the per-PROGRAM stack reserve
    // this build asks the emitted image to carry, in BYTES. nullopt (the
    // default / absent) = take the object format's declared default.
    //
    // The required stack is a property of the PROGRAM (its deepest call
    // chain), not of the format, which is why it cannot be a fixed number in
    // a `.format.json` — MSVC spells the same request `/STACK`, GNU ld
    // `-Wl,--stack`. Zero, non-numeric, and trailing junk fail loud
    // (InvalidStackReserve); the value is NOT rounded here — whether it is
    // in range and correctly aligned is decided at the linker gate against
    // the bounds the chosen FORMAT declares, and a format that declares no
    // stack-reserve capability at all REFUSES the request rather than
    // dropping it. Threaded to `Program::setStackReserveBytes`.
    //
    // PRECEDENCE: this CLI flag WINS over a project manifest's
    // `stackReserve` key when both are present (see
    // `Program::compileProject`).
    std::optional<std::uint64_t>  stackReserveBytes;
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
    MissingLanguage     = 7,    // --transpile / --dump-predefined-macros with
                                // no --language (compile / directory fall back
                                // to the target's declared assembly dialect)
    InvalidSuppressCode = 8,    // --suppress=<bad-code>
    InvalidConfig       = 9,    // --config=<not-debug-not-release>
    EmptyFilename       = 10,   // --compile "" or bare `-` as positional
    UnexpectedPositional = 11,  // bare positional outside --compile/--transpile
    InvalidDefine       = 12,   // c105: --define with an empty NAME, or a '('
                                // in NAME (a function-like --define is not
                                // supported — use a config predefine)
    InvalidJobs         = 13,   // D-PERF-4: --jobs with a non-numeric value, a
                                // zero, or trailing junk (`--jobs 0`, `--jobs x`)
    InvalidStackReserve = 14,   // D-SQLITE-PE64-FULL-TIER-STACK-DEPTH:
                                // --stack-reserve with a non-numeric value, a
                                // zero, or trailing junk. RANGE/alignment is
                                // NOT decided here — that is the linker gate's
                                // job, against the format's declared bounds.
    InvalidResolveLibrary = 15, // D-FFI-DECLARED-IMPORT-NAME:
                                // `--resolve-library <path>=<import-name>` with
                                // an EMPTY side — `=libfoo.so` (no path to read)
                                // or `libfoo.so=` (no identity to record).
                                // Neither is usable and neither is silently
                                // droppable: an empty path would read nothing,
                                // an empty name would record an unresolvable
                                // DT_NEEDED. A value with NO `=` at all is the
                                // plain form and is NOT an error.
    InvalidMaxDiagnostics = 16, // --max-diagnostics with a non-numeric value,
                                // trailing junk, or a count that overflows
                                // std::size_t. ZERO IS NOT AN ERROR — see the
                                // `CliArgs::maxDiagnostics` docblock for why
                                // `report()` defines it. Deliberately its OWN
                                // enumerator rather than InvalidSuppressCode:
                                // the two share only the word "diagnostic",
                                // and the remediation differs completely
                                // ("write a number" vs "spell the code name").
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

// Project a parsed `CliArgs` onto the `DiagnosticReporter::Config` the driver
// hands to every compile-producing entry point (`compileProject` /
// `transpile` / `compileDirectory` / `compileFiles` / `compileUnits`). Carries
// `--warnings-as-errors`, `--suppress=<code>` and `--max-diagnostics=<count>`.
//
// Lives HERE, beside the parser that produces its input, rather than as a
// file-local helper in `program.cpp` where it started. It is a pure projection
// of `CliArgs` — no Program state, no I/O, no ordering — and being reachable
// is what lets a test drive argv → CliArgs → Config through the SHIPPED code
// instead of re-typing the config by hand, which would be a pin over a stub
// rather than over the projection.
//
// Absent `--max-diagnostics` deliberately leaves `Config::maxDiagnostics`
// UNWRITTEN so the struct's own in-class initializer supplies the default;
// there is no second spelling of that number anywhere in the driver.
[[nodiscard]] DSS_EXPORT DiagnosticReporter::Config
buildReporterConfig(CliArgs const& args);

// Build the run's `DiagnosticReporter` from that projection — and, in the
// same breath, TELL THE OPERATOR about any `--suppress=<code>` request the
// reporter is about to ignore (`D_SuppressRequestIgnored`, one per refused
// code, carrying the code's recorded rationale). The build then proceeds
// normally: the request is refused, not the compilation.
//
// ★ WHY THIS FUNCTION EXISTS AT ALL — the emit site is forced, and the
// forcing is worth writing down because two closer-looking sites are wrong.
//
//   * NOT `parseSuppressCode` / `parseCliArgs`. There is no reporter there
//     and there cannot be one: `parseCliArgs` returns
//     `std::unexpected(error)` and runs to completion BEFORE any
//     `DiagnosticReporter` exists. A warning emitted at parse time would
//     need an arg-parser-private channel or a bespoke `std::cerr` write —
//     i.e. a second diagnostic system, exempt from `--suppress`,
//     `--warnings-as-errors`, the cap and the dedup window. The whole point
//     of this diagnostic is that it is ORDINARY.
//
//   * NOT `DiagnosticReporter`'s constructor, which looks like the perfect
//     chokepoint and is a trap. ✔MEASURED: `program.cpp` builds per-target
//     and per-pair SCRATCH reporters by CLONING the live config
//     (`auto scratchCfg = rep.config();`, likewise `pairCfg`), and
//     `compilation_unit.cpp` does the same per CU. Emitting from the
//     constructor would therefore re-announce the same refusal once per
//     target × per CU — and those clones deliberately set `dedupWindow = 0`
//     and an unbounded `maxPerCode`, so nothing would collapse the
//     duplicates and `mergeWithTargetContext` would carry every one of them
//     into the run-wide reporter. On a 100-TU build that is a hundred copies
//     of one line.
//
// So the emit belongs where a reporter is FIRST built FROM the CLI's own
// projection, exactly once per invocation. That is here — the five
// `Config`-taking entry-point overloads in `program.cpp` (`compileProject`,
// `transpile`, `compileDirectory`, `compileFiles`, `compileUnits`) exist
// precisely to turn a `Config` into a reporter, and they are the ONLY way
// `--suppress` reaches one; their `DiagnosticReporter&`-taking siblings take
// a reporter the caller already owns and never see a `Config`. Routing all
// five through this one function keeps the emit single-sited while covering
// every path that honours `--suppress`.
[[nodiscard]] DSS_EXPORT DiagnosticReporter
buildReporter(DiagnosticReporter::Config const& cfg);

} // namespace dss
