#include "program/program.hpp"

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/substrate/large_stack_call.hpp"  // D-PARSE-DEEP-FRONTEND-STACK: build CUs on a large stack
#include "core/substrate/phase_timers.hpp"      // c97: --time per-phase breakdown
#include "core/substrate/thread_pool.hpp"       // D-PERF-4-CU-PARALLELISM: per-CU build pool
#include "core/types/config_path_walk.hpp"      // findShippedConfigDir — shared src/dss-config/<dir> resolver
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/glob_match.hpp"  // D-AP2-SOURCES-GLOB: expand sources[] patterns
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/project_config.hpp"  // the ONE `.dss-project.json` parser
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_lattice.hpp"  // TypeLattice (fresh merge host)
#include "ffi/abi/abi_catalog.hpp"
#include "ffi/mangling/c_mangle.hpp"  // applyCMangling — the cross-CU merge-key mangling (D-LK-MACHO-CROSSCU-MANGLE-MERGE-KEY)
#include "ffi/shipped_lib_descriptor.hpp"  // isKnownSynthesizeRecipe (FC17.9a threads-shim vocab)
#include "link/object_format_schema.hpp"
#include "mir/merge/mir_merge.hpp"  // MergeCuInput, mergeCuMirs (N>1 whole-program merge)
#include "mir/merge/synth_pe_startup.hpp"  // realizeEntryShape (the argv spine)
#include "mir/merge/synth_stdio_shim.hpp"  // synthesizeStdioShim (D-FFI-PE-CRT-UCRT-MIGRATION Phase 3)
#include "mir/merge/synth_threads_shim.hpp"  // synthesizeThreadsShim (FC17.9a D-CSUBSET-C11-THREADS-HEADER)
#include "lsp/lsp_server.hpp"
#include "lsp/schema_cache.hpp"
#include "lsp/transport.hpp"
#include "program/build_scripts.hpp"  // runBuildScripts — the manifest's pre/post build hooks
#include "program/cli_args.hpp"
#include "program/compile_pipeline.hpp"
#include "program/cross_validate_target_format.hpp"
#include "program/input_resolver.hpp"
#include "program/target_spec.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <iostream>
#include <latch>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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
    auto executor = std::make_unique<substrate::ThreadPool>(workers);
    lsp::LspServer server{std::move(transport), std::move(executor), cache};
    return server.run();
}

// `buildReporterConfig` (CLI flags → `DiagnosticReporter::Config`) MOVED to
// `program/cli_args.{hpp,cpp}`, beside the parser that produces its input. It
// is a pure projection of `CliArgs` with no Program state, and being reachable
// from a test is what lets the `--max-diagnostics` pin drive argv → CliArgs →
// Config through the SHIPPED projection rather than re-typing the config.
// (D-LK10-7's policy knob still arrives here unchanged, at `Program::run`.)

// Drain reporter diagnostics to stderr. The driver is the boundary
// between in-memory diagnostic records and the operator's terminal;
// LSP mode owns its own emit path (LSP $/diagnostic), so this stderr
// flush is the CLI/embed path only. (silent-failure-hunter F3 fold:
// `severityName` prefix lets `grep error` filter correctly.)
//
// eb2c6c7 audit-fold (2026-06-01): `d.contextPrefix` is rendered
// between the code-band and `d.actual` so multi-target runs route
// per-target context to the operator's terminal. Pre-fold the prefix
// was baked into `actual` so this print site saw it for free; post-
// fold the prefix lives in its own field (excluded from dedup hash)
// and every render path must spell out the inclusion. LSP
// `composeMessage` performs the symmetric prepend.
// Render the reporter's diagnostics to stderr (plan 06 V2-4 Part A).
//
// Per-diagnostic routing — the agnostic split is on whether the
// diagnostic carries a source buffer, NOT on any language/target/format:
//   * buffer-VALID (parser/semantic errors, span into real source) →
//     DSS's OWN positioned renderer `DiagnosticReporter::format(d, bufs)`:
//     `--> file:line:col` + the source line + a `^` caret + related-
//     location notes. (No clang/LLVM: this is hand-written DSS code over
//     our SourceBuffer/SourceSpan; `bufs` resolves BufferId → buffer.)
//   * buffer-LESS (driver-tier `D_*` emitted via `emitDriver`, default
//     `BufferId{}`, no span) → the established code-only one-liner. These
//     have no source location to point at; routing them through the
//     positioned renderer would print a bogus `<unknown-buffer:0>` line
//     and a spurious `got ` prefix for driver prose.
//
// `bufs` is built by the driver from the compiled CUs' source buffers
// (see `runCusToTargets`). Pre-parse error sites (no CUs yet) pass an
// empty registry — their diagnostics are all buffer-less, so the split
// keeps them on the code-only path regardless.
// `firstIndex` exists for the ONE call site that drains a reporter somebody
// else already drained. `rep.all()` is a span and this function RENDERS rather
// than CONSUMES — nothing is cleared, so a second unqualified call reprints
// everything. The post-build hook seam is exactly that shape: the compile
// delegate drains on its way out (`runCusToTargets`), and a hook failing
// afterwards must report ITS diagnostic without replaying the whole successful
// compile's output ahead of it — which would bury the one line explaining the
// non-zero exit under a duplicate dump, and would re-render every
// buffer-bearing diagnostic through the EMPTY registry of the one-argument
// overload, printing the bogus `<unknown-buffer:0>` line this helper's routing
// exists to avoid. Callers that own the whole stream keep the default 0.
void drainDiagnosticsToStderr(DiagnosticReporter const& rep,
                              BufferRegistry const&     bufs,
                              std::size_t const         firstIndex = 0) {
    auto const all = rep.all();
    for (auto const& d : all.subspan(std::min(firstIndex, all.size()))) {
        if (d.buffer.valid()) {
            std::cerr << rep.format(d, bufs);
        } else {
            std::cerr << severityName(d.severity)
                      << "[" << diagnosticCodeName(d.code) << "] "
                      << d.contextPrefix
                      << d.actual << '\n';
        }
    }
}

// Overload for the pre-parse / buffer-less call sites (empty registry):
// every diagnostic at those sites is driver-tier (buffer-less), so this
// renders them code-only via the routing above. Keeps the 13 early-error
// sites a one-token change while the 2 post-parse sites (in
// `runCusToTargets`, which owns the CUs) pass the real registry.
void drainDiagnosticsToStderr(DiagnosticReporter const& rep,
                              std::size_t const         firstIndex = 0) {
    static BufferRegistry const kEmpty;
    drainDiagnosticsToStderr(rep, kEmpty, firstIndex);
}

// ── THE BUILD'S STATEMENT OF RECORD ABOUT WHAT IT PRODUCED ─────────────────
// D-HARNESS-FIXTURE-PATH-ASSUMES-THE-POSIX-ARTIFACT-SPELLING (TF-C118).
//
// ★ WHY THIS EXISTS. Until now a SUCCESSFUL build said nothing at all about
// the file it had written, so every consumer had to RECONSTRUCT the name —
// and reconstructing it requires the artifact-extension table, which is
// `TargetSpec::outputExtension` keyed on the closed object-format enum. Two
// partial copies of that table had already escaped into the sqlite harness,
// they disagreed with each other, and the disagreement threw away a REAL
// cross-host success: a Linux host cross-built the Windows `testfixture.exe`
// for pe64 with zero diagnostics, and the driver — looking for a suffix-less
// `testfixture` — recorded the leg as a build FAILURE. The fix is not a third
// copy of the table. It is for the compiler to SAY what it wrote.
//
// ★ WHY A PLAIN REPORT LINE AND NOT AN `info[...]` DIAGNOSTIC. A diagnostic is
// DROPPABLE, through three independent gates in `DiagnosticReporter::report`:
// `--suppress` naming the code, the per-code cap (50), and the global cap
// (1000, after which every further report is discarded in silence). Any one of
// them re-creates precisely the false negative this line exists to end — a
// build that succeeded, saying nothing, read as a build that produced nothing.
// Escaping all three would mean joining `kUnsuppressableCodes`, whose first
// Info-severity member is itself a KNOWN open question
// (D-FF2-UNSUPP-INFO-WAE-ASYMMETRY). This is a driver REPORT, not an opinion
// about the program, so it goes out the way `--time`'s report does: same
// stream, same `dss-code-prime: ` prefix, no policy in the way.
//
// ★ THE SHAPE, and each part of it is load-bearing for a machine reader:
//
//     dss-code-prime: artifact <targetSpec> <absolute path>
//
//   · a FIXED leading marker, so a consumer matches a prefix and never a
//     regex over prose;
//   · the TARGET SPEC second, because `TargetSpec::parse` REFUSES whitespace
//     in either half — the spec is a single token by construction, so a
//     multi-target build stays unambiguous and a consumer can select the line
//     belonging to the target it asked for;
//   · the PATH last, so a path containing spaces is still the unambiguous
//     REMAINDER of the line, and ABSOLUTE, because `--output` is stored
//     verbatim (`cli_args.cpp`) and would otherwise leave the reader guessing
//     which cwd to resolve against.
//
// Nothing here is keyed on language, processor or object format: the path was
// already computed from the closed format enum above, and this only reports
// it.
[[nodiscard]] std::string artifactPathForReport(fs::path const& p) {
    // `generic_string()` throws for a path the current locale cannot encode;
    // the u8 fallback mirrors `link/writer.cpp`'s `pathForDiag`, which the
    // write-failure diagnostics for this very artifact already use. Forward
    // slashes on every host — that is what this codebase prints, and every
    // consumer of the line (POSIX shell, PowerShell, .NET `Path`) takes them.
    try {
        return p.generic_string();
    } catch (...) {
        auto const u8 = p.u8string();
        return std::string(reinterpret_cast<char const*>(u8.data()), u8.size());
    }
}

// Called ONCE per artifact, and ONLY after the write path reported success.
void reportArtifactWritten(std::string const& targetSpec,
                           fs::path const&    outPath) {
    // Lexical only. `absolute` needs the cwd; `lexically_normal` folds the
    // `.`/`..` a relative `--output` may have contributed. Neither touches the
    // disk — the bytes are already committed, and re-statting here would only
    // create a way for the report to disagree with the write.
    std::error_code ec;
    fs::path abs = fs::absolute(outPath, ec);
    // Not a silent fallback: `outPath` IS the path the writer committed to, so
    // the line stays TRUE either way. Only the ABSOLUTE guarantee is lost, and
    // only when the process has no usable cwd to resolve against.
    if (ec) abs = outPath;
    std::cerr << "dss-code-prime: artifact " << targetSpec << ' '
              << artifactPathForReport(abs.lexically_normal()) << '\n';
}

// Emit a driver-tier D_* diagnostic. Wraps `dss::report` so all
// driver-side fail-loud sites take the same shape (Error severity,
// ferried through the same reporter the kernel uses).
void emitDriver(DiagnosticReporter& rep,
                DiagnosticCode code,
                std::string msg) {
    dss::report(rep, code, DiagnosticSeverity::Error, std::move(msg));
}

// Stamp `[target=<spec>]` context into every error message emitted
// inside the per-target loop. Caller passes a fresh reporter to
// `compileOneTarget`; this helper consolidates that scratch
// reporter into the run-wide reporter with the target prefix added.
// (silent-failure-hunter F9 fold, LK10 cycle 2 post-audit review.)
//
// D-MERGE-DEDUP-PREFIX-COLLISION fold (2026-06-01): prefix lands in
// the dedicated `contextPrefix` field on ParseDiagnostic — NOT in
// `actual` — so the dedup hash at the destination computes on the
// un-prefixed key. Two targets emitting the structurally-identical
// diagnostic now collapse at rep, instead of leaking through
// duplicate-with-different-prefix.
void mergeWithTargetContext(DiagnosticReporter const& src,
                            std::string const&        targetSpec,
                            DiagnosticReporter&       dst) {
    auto const prefix = "[target=" + targetSpec + "] ";
    for (auto const& d : src.all()) {
        ParseDiagnostic copy = d;
        copy.contextPrefix = prefix;
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

// ── TF-C74 (D-PROGRAM-UNKNOWN-OBJECT-FORMAT-SILENT): ONE wording per half ──
//
// The two halves of a `--target <targetName>:<formatName>` spec are now
// checked in TWO places — the pre-flight in `runCusToTargets` (which must
// reject before the CU build) and `compileOneTarget` (the authoritative site,
// which still checks because it is reachable on its own terms). Hoisting the
// CHECK must not fork the MESSAGE, and the only way that survives the next
// edit is for both sites to call the same emitter.
//
// Each emits TWO diagnostics and both are load-bearing: the config loader's
// own reason (`C_InvalidTargetName` / `C_InvalidFormatName`) says WHAT it
// could not find and is forwarded verbatim rather than swallowed; the driver's
// `D_SchemaLoadFailed` says what that means for this build and where to look.
// Neither is a duplicate of the other, so neither is dropped.
void emitTargetSchemaLoadFailed(DiagnosticReporter&               rep,
                                std::span<ConfigDiagnostic const> why,
                                std::string const&                targetName) {
    forwardConfigDiagnostics(why, rep);
    emitDriver(rep, DiagnosticCode::D_SchemaLoadFailed,
               "target schema '" + targetName
               + "' could not be loaded — the reason is in the configuration diagnostic(s) above (config: "
                 "src/dss-config/targets/" + targetName
               + ".target.json).");
}

void emitObjectFormatSchemaLoadFailed(DiagnosticReporter&               rep,
                                      std::span<ConfigDiagnostic const> why,
                                      std::string const&                formatName) {
    forwardConfigDiagnostics(why, rep);
    emitDriver(rep, DiagnosticCode::D_SchemaLoadFailed,
               "object-format schema '" + formatName
               + "' could not be loaded — the reason is in the configuration diagnostic(s) above (config: "
                 "src/dss-config/object-formats/" + formatName
               + ".format.json).");
}

// D-PERF-4-CU-PARALLELISM: worker count for the INTERNAL per-CU build pool
// (only consulted when NO executor is injected). Never more workers than there
// are CUs — extra workers would just block on the empty job queue. An explicit
// `--jobs` (jobsOverride > 0) pins the count within that ceiling; auto (0) uses
// min(hardware_concurrency, kMaxAutoWorkers) so a 64-core host doesn't spawn 64
// threads for a handful of TUs. Only ever called on the N>1 path (cuCount >= 2).
[[nodiscard]] std::size_t resolveCuPoolWidth(std::size_t cuCount,
                                             unsigned    jobsOverride) noexcept {
    constexpr std::size_t kMaxAutoWorkers = 16;
    std::size_t const ceiling = std::max<std::size_t>(std::size_t{1}, cuCount);
    if (jobsOverride > 0) {
        return std::min<std::size_t>(jobsOverride, ceiling);
    }
    std::size_t const hw = std::thread::hardware_concurrency();
    std::size_t const autoWidth =
        std::min<std::size_t>(hw == 0 ? std::size_t{1} : hw, kMaxAutoWorkers);
    return std::min<std::size_t>(autoWidth, ceiling);
}

// Compile one resolved (CU, target, format) triple to one artifact.
// Returns true on success; emits via `reporter` on failure.
//
// `outputDir` (D-LK10-ENTRY Slice C companion): when set, the
// emitted binary lands at `<outputDir>/<name><ext>` for
// single-target builds, or `<outputDir>/<formatName>/<name><ext>`
// for multi-target builds (the multi-target qualifier disambiguates
// same-named outputs across formats). When unset, the legacy
// `<cwd>/target/<formatName>/<name><ext>` convention applies —
// keeps existing call sites unchanged.
//
// The artifact base `<name>` = `artifactName.value_or(sourceStem)`: a
// project manifest's `artifactName` overrides the source stem; nullopt
// (the CLI path, and a project without the field) keeps the source stem
// (unchanged). `perFormatOutputSubdir` (D-AP2-OUTPUT-ROUTING) forces the
// `<formatName>/` subdir even for a single-target `--output` build — a
// PROJECT build sets it so every platform's artifact is consistently
// per-platform-subdir'd; the CLI path leaves it false, so `--compile`
// single-target output stays flat (byte-identical).
[[nodiscard]] bool compileOneTarget(std::span<CompilationUnit const> cus,
                                    GrammarSchema const&   grammar,
                                    std::string const&     sourceStem,
                                    std::string const&     targetSpecStr,
                                    DiagnosticReporter&    reporter,
                                    std::optional<std::filesystem::path> const& outputDir,
                                    bool                   multiTargetBuild,
                                    std::optional<std::string> const& artifactName,
                                    bool                   perFormatOutputSubdir,
                                    CompileOptions const&  compileOpts,
                                    // D-PERF-4-CU-PARALLELISM: the per-CU build
                                    // executor (nullptr ⇒ an internal pool sized
                                    // via `jobsOverride`) + the `--jobs` override.
                                    substrate::IExecutor*  injectedExecutor,
                                    unsigned               jobsOverride,
                                    // D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: the
                                    // per-PROGRAM image knobs (stack reserve),
                                    // forwarded verbatim to the link step. The
                                    // linker gates them against THIS target's
                                    // format capability — which is why the
                                    // request travels per-target rather than
                                    // being resolved once for the whole build:
                                    // a multi-target project may name one
                                    // format that can carry it and one that
                                    // cannot, and the second must fail loud.
                                    ImageRequest const&    imageRequest) {
    auto parsed = TargetSpec::parse(targetSpecStr);
    if (!parsed) {
        emitDriver(reporter, DiagnosticCode::D_InvalidTargetSpec,
                   targetSpecErrorMessage(targetSpecStr, parsed.error()));
        return false;
    }

    auto targetR = TargetSchema::loadShipped(parsed->targetName);
    if (!targetR.has_value()) {
        emitTargetSchemaLoadFailed(reporter, targetR.error(),
                                   parsed->targetName);
        return false;
    }
    auto formatR = ObjectFormatSchema::loadShipped(parsed->formatName);
    if (!formatR.has_value()) {
        emitObjectFormatSchemaLoadFailed(reporter, formatR.error(),
                                         parsed->formatName);
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

    // D-FF3-3 (commit 9440143): resolve the (target, format) calling
    // convention BEFORE dispatching to compileSingleUnit. Replaces
    // the previous silent dispatch to `callingConventions[0]` —
    // that hardcode produced SysV register assignments on PE+x86_64
    // targets when MS_x64 was required, etc. The resolved cc index
    // threads through compileSingleUnit → allocateRegisters → the
    // LirFuncAllocation::callingConventionIndex field that
    // materializeCallingConvention reads downstream.
    auto const abi = dss::ffi::resolveAbi(**targetR, **formatR, reporter);
    if (!abi) return false;

    // Post-fold-#5 silent-failure CRITICAL-1: operand-stack (WASM)
    // and result-id (SPIR-V) abi-models return cc=nullptr from
    // resolveAbi. The register-machine LIR pipeline downstream
    // would silently use cc index 0 of whatever the target schema
    // ships (or emit `R_NoCallingConventions` if none), producing
    // x86/ARM-shaped binaries for a WASM/SPIR-V target. Both
    // formats need their own MIR→IR lowering tier (plan 17 for
    // SPIR-V, plan 18 for WASM). Fail loud here rather than
    // dispatching a register-machine pipeline against a non-
    // register-machine target.
    if (abi->cc == nullptr) {
        // post-fold #6 silent-failure C2 fix: dedicated code
        // (D_TargetAbiModelUnsupportedByDriver) replaces the
        // previous D_PlanNotLanded reuse. This pairing is a
        // permanent architectural exclusion (plans 17/18 own their
        // own lowering tiers), NOT a pending-arrival surface;
        // grouping with D_PlanNotLanded would conflate the two
        // remediation classes + let `--suppress=D_PlanNotLanded`
        // (legitimate for compileProject stubs) silently mask
        // this architectural reject.
        emitDriver(reporter, DiagnosticCode::D_TargetAbiModelUnsupportedByDriver,
                   std::string{"target '"} + parsed->targetName
                       + "' has abiModel='"
                       + std::string{targetAbiModelName(
                             (*targetR)->abiModel())}
                       + "' — register-machine LIR pipeline does not "
                         "lower it. Plan 17 (SPIR-V) / plan 18 (WASM) "
                         "own this lowering.");
        return false;
    }
    auto const span = (*targetR)->callingConventions();
    std::uint16_t const ccIndex = static_cast<std::uint16_t>(
        std::distance(span.data(), abi->cc));

    // Output path convention (cycle 2 v1; plan 6 owns the
    // authoritative artifact-profile-driven scheme). The artifact base
    // `<name>` = `artifactName.value_or(sourceStem)` (a project manifest's
    // `artifactName` overrides the stem; nullopt keeps it — the CLI path):
    //   default      : <cwd>/target/<formatName>/<name><ext>
    //   --output dir : <dir>/<name><ext>               (single target, flat)
    //                  <dir>/<formatName>/<name><ext>  (multi target, OR any
    //                                                   project build via
    //                                                   perFormatOutputSubdir)
    // A PROJECT build sets `perFormatOutputSubdir` (D-AP2-OUTPUT-ROUTING) so
    // even its single-target output lands under `<formatName>/` — consistently
    // per-platform. The CLI single-target path leaves it false ⇒ flat
    // (unchanged). `formatName` already encodes machine+OS, so we don't add a
    // separate `<targetName>` subdir (redundant + bloats the path).
    auto const ext = parsed->outputExtension(**formatR);
    fs::path outDir;
    if (outputDir.has_value()) {
        outDir = (multiTargetBuild || perFormatOutputSubdir)
                   ? (*outputDir / parsed->formatName)
                   : *outputDir;
    } else {
        outDir = fs::current_path() / "target" / parsed->formatName;
    }
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
    auto const outPath =
        outDir / (artifactName.value_or(sourceStem) + std::string{ext});

    // Containment BOUNDARY (D-AP2-OUTPUT-ROUTING). The loader validates a
    // project's `artifactName` as a bare name (it rejects '/' and '\'), but a
    // separator DENYLIST does not prove containment — two OS-agnostic vectors
    // still escape the routed tree:
    //   * a differing ROOT-NAME (Windows drive-relative "D:app"): `operator/`
    //     REPLACES `outDir` when the RHS root-name differs ⇒ the artifact lands
    //     on another drive;
    //   * a bare ".." (no separator ⇒ survives the loader): `outDir / ".."`
    //     normalizes to outDir's PARENT.
    // Enforce the real invariant here, where `outDir` is known: the resolved
    // artifact must be a DIRECT CHILD of `outDir`. Comparing lexically-normalized
    // paths is OS-agnostic and uniformly also catches NTFS ADS. This is a NO-OP
    // for the CLI path — there `artifactName` is nullopt ⇒ the name is the source
    // STEM (always a bare filename) ⇒ always a direct child ⇒ never fires.
    if (outPath.lexically_normal().parent_path() != outDir.lexically_normal()) {
        emitDriver(reporter, DiagnosticCode::D_ArtifactNameEscapesOutputDir,
                   "artifact name '" + artifactName.value_or(sourceStem)
                   + "' resolves outside the output directory '"
                   + outDir.generic_string()
                   + "' — it must be a bare file name (no path separators, no "
                     "drive/root prefix, and not '.' or '..').");
        return false;
    }

    // D-HARNESS-FIXTURE-PATH-ASSUMES-THE-POSIX-ARTIFACT-SPELLING (TF-C118):
    // THE ONE report site for the three link/write dispatches below (static
    // archive / single CU / merged multi-CU). They are three routes to the
    // SAME `outPath`, so the report is attached at their only common ancestor
    // rather than copied into each — and it is attached to their RESULT, so
    // the line cannot be printed for a write that failed. Every `return` of a
    // link/write result below goes through here; a route added later that does
    // not is a route whose artifact goes unreported, which is the whole defect.
    auto const reported = [&](bool wrote) {
        if (wrote) reportArtifactWritten(targetSpecStr, outPath);
        return wrote;
    };

    // c165 (D-LK-STATIC-LINK): partition `--resolve-library` into DYNAMIC
    // libraries (`.so`/`.dll`/`.dylib` -- read for their export surface during
    // per-CU FFI synthesis, the c162 path) and STATIC `ar` archives (pulled +
    // merged at LINK time below). The dispatch is by MAGIC BYTES (`isArArchiveFile`
    // -- agnostic; never a `.a`/`.lib` extension). Feeding an archive to the
    // dynamic export reader would mis-bind its armap symbols as a runtime
    // DT_NEEDED (a `.a` is not loadable), so the per-CU build sees the DYNAMIC
    // subset only; the archives flow to `linkAndWriteWithStaticArchives`. An
    // unreadable path stays DYNAMIC -- the dynamic path's eager open-probe
    // (compile_pipeline step 2.5-pre) fails it loud, so a bad path is never
    // silently dropped.
    // D-FFI-DECLARED-IMPORT-NAME: a STATED import name is meaningful only on
    // the DYNAMIC side (it names a runtime dependency); a static archive is
    // merged into the image and records no import at all. The partition keeps
    // the whole spec on the dynamic side and takes only the PATH for archives,
    // so nothing is silently dropped where it would have had an effect.
    std::vector<std::filesystem::path> staticArchives;
    CompileOptions perCuOpts = compileOpts;
    {
        std::vector<ResolveLibrarySpec> dynamicLibs;
        for (auto const& lib : compileOpts.resolveLibraries) {
            if (isArArchiveFile(lib.path)) staticArchives.push_back(lib.path);
            else                           dynamicLibs.push_back(lib);
        }
        perCuOpts.resolveLibraries = std::move(dynamicLibs);
    }

    // Cycle 24/25 build-then-lower sequence. LOOP 1: build EVERY CU's MIR up front
    // (`buildCuMir` — sem→HIR→FFI→MIR→optimize), holding each `CuMirModule` (which keeps
    // its SemanticModel — the interner owner — alive).
    //
    // D-PERF-4-CU-PARALLELISM: for N>1, run the per-CU builds CONCURRENTLY on a thread pool.
    // Each `buildCuMir` is a PURE per-CU function — its TypeInterner, arenas, SemanticModel,
    // symbol table and per-CU SymbolId allocator are all private; the shipped-descriptor cache
    // is `thread_local`; module-id counters are atomic; PhaseTimers accumulate via relaxed
    // atomics with `thread_local` nesting. The ONE shared-mutable sink — the DiagnosticReporter
    // — is replaced by a PER-CU scratch reporter written only by that CU's job; the scratches
    // then drain into `reporter` in CU (index) ORDER after the join, so the diagnostic stream
    // AND the resulting artifact are byte-deterministic regardless of thread scheduling. N==1
    // stays INLINE (the hot single-file path: zero pool cost, diagnostics land straight in
    // `reporter`, byte-identical + fail-FAST exactly as the pre-parallel code).
    // D-CSUBSET-TESTTU-SILENT-EXIT1 fail-loud net: snapshot the reporter's error
    // count BEFORE the per-CU build/lower. Every genuine tier failure reports its
    // own K_/L_/A_/S_/H_ diagnostic; if a per-CU build (`buildCuMir`) or the
    // back-half lower (`lowerCuMirToAssembly`) returns a NULL module without any
    // new diagnostic, that is a substrate-contract violation (the D-PERF-4
    // buildCuMir-null contract) — emit `D_CompileUnitNullNoDiagnostic` so the
    // driver never exits 1 with ZERO output. A no-op on the happy path and on
    // every genuine (already-reported) failure. Mirrors the optimizer's
    // X_OptReturnFalseWithoutDiagnostic belt-and-suspenders guard.
    auto const errorsBeforeCuBuild = reporter.errorCount();
    auto const emitNullNoDiagnostic = [&](char const* where) {
        if (reporter.errorCount() == errorsBeforeCuBuild) {
            emitDriver(reporter, DiagnosticCode::D_CompileUnitNullNoDiagnostic,
                       std::string{"internal: "} + where
                           + " returned a null module without reporting any "
                             "diagnostic — substrate-contract violation "
                             "(D-CSUBSET-TESTTU-SILENT-EXIT1 fail-loud net).");
        }
    };
    // ── plan 29 P4: THE `encode` PIPELINE ENTRY ────────────────────────────
    //
    // ★★★ THE ONE PLACE THE TIER FACET IS READ, AND IT IS READ OFF THE ROOT
    // RULE. `pipelineEntry.byRule` is per-CONSTRUCT by design (plan 29 §1 —
    // "the pipeline entry point is a property of the CONSTRUCT, never of the
    // language"), and a TRANSLATION UNIT is a construct like any other: the
    // rule that owns it is the language's root. A dialect declaring
    // `{ "rule": "root", "tier": "encode" }` is saying "my translation unit
    // enters at the assembler's input", and this is where that is honoured.
    // ⚠ NO LANGUAGE IDENTITY ANYWHERE. The branch is on a closed enum resolved
    // at load; it never asks which language this is, and any language that
    // declares the tier takes it. `tierForRule` returning nullopt (every
    // shipped language except the `asm-*` dialects) leaves the ordinary path
    // byte-identical.
    // ⚠ AND AN UNHANDLED TIER FAILS LOUD RATHER THAN FALLING THROUGH. A
    // `mir`/`lir` row is already refused at LOAD (`kImplementedEntryTiers`), so
    // it cannot reach here — but if a future tier lands in the vocabulary
    // before its route does, falling through would silently run hand-written
    // code through the optimizer, which is the exact miscompile the facet
    // exists to prevent. The switch has no `default:`, so a new enumerator is a
    // COMPILE error here.
    if (auto const rootTier =
            grammar.pipelineEntry().tierForRule(grammar.rootCursor().rule());
        rootTier.has_value()) {
        switch (*rootTier) {
        case PipelineTier::Hir:
            break;   // the ordinary path below
        case PipelineTier::Encode: {
            if (cus.size() != 1) {
                emitDriver(reporter, DiagnosticCode::D_PlanNotLanded,
                           "a multi-CU standalone-assembly build is not yet "
                           "lowered — each unit's symbol space would have to be "
                           "namespaced before the modules merge");
                return false;
            }
            // D-ASM-EXTERN-CALL-CANNOT-BIND-A-LIBRARY: the WHOLE format schema
            // and the WHOLE per-CU options, by the same route `buildCuMir`
            // takes. The old call handed over `entryVerbs()` alone, so the
            // assembly tier could not name the active object format or its data
            // model and had nothing to ask the platform-realization oracle with
            // — which is why a `.s` calling libc could not bind ANY library.
            // `perCuOpts` (not `compileOpts`) deliberately: it is the DYNAMIC
            // half of `--resolve-library`, with `ar` archives already
            // partitioned out for the static path below.
            auto mod = assembleAsmUnit(cus[0], grammar, **targetR, **formatR,
                                       reporter, perCuOpts);
            if (!mod) {
                emitNullNoDiagnostic("the assembly unit build "
                                     "(assembleAsmUnit)");
                return false;
            }
            // D-ASM-RESOLVE-LIBRARY-SILENTLY-IGNORED-ON-ENCODE-TIER, the STATIC
            // half: this route used to call `linkAndWrite` directly, so a `.a` /
            // `.lib` named on `--resolve-library` was partitioned out of the
            // per-CU options above and then handed to NOBODY — accepted, and
            // dropped without a word, exactly like its dynamic sibling. Routing
            // through `linkAndWriteWithStaticArchives` is byte-identical to
            // `linkAndWrite` when the archive list is empty (its documented
            // contract), so the ordinary `.s` build is unchanged and the flag
            // now has an effect on both halves.
            return reported(linkAndWriteWithStaticArchives(
                std::move(*mod),
                std::span<std::filesystem::path const>{staticArchives},
                **targetR, **formatR, outPath, reporter, imageRequest));
        }
        case PipelineTier::Mir:
        case PipelineTier::Lir:
            emitDriver(reporter, DiagnosticCode::D_PlanNotLanded,
                       "the language's root declares a pipeline entry tier this "
                       "driver has no route for — refused rather than silently "
                       "taking the full pipeline, which would run the "
                       "construct through tiers it asked to skip");
            return false;
        }
    }

    std::vector<std::optional<CuMirModule>> cuMirSlots(cus.size());
    if (cus.size() <= 1) {
        if (!cus.empty()) {
            cuMirSlots[0] = buildCuMir(cus[0], grammar, **targetR, **formatR,
                                       ccIndex, reporter, perCuOpts);
            if (!cuMirSlots[0]) {              // front-half tier failure already reported
                emitNullNoDiagnostic("per-CU build (buildCuMir)");
                return false;
            }
        }
    } else {
        // Per-CU scratch reporters: inherit `reporter`'s POLICY (suppress / overrides /
        // warningsAsErrors) but RELAX the cap/dedup axes — the run-wide cap is enforced ONCE
        // when these drain into `reporter` below, so a per-CU cap can't asymmetrically truncate
        // one CU's diagnostics based on which thread finished first (mirrors the per-target
        // scratch discipline in `runCusToTargets`).
        auto cuScratchCfg = reporter.config();
        cuScratchCfg.maxDiagnostics = std::numeric_limits<std::size_t>::max();
        cuScratchCfg.maxPerCode     = std::numeric_limits<std::size_t>::max();
        cuScratchCfg.dedupWindow    = 0;
        std::vector<DiagnosticReporter> cuScratch;
        cuScratch.reserve(cus.size());
        for (std::size_t i = 0; i < cus.size(); ++i) cuScratch.emplace_back(cuScratchCfg);

        // Executor: the injected one (tests / a shared pool) or a fresh internal pool sized to
        // the CU count (+ the `--jobs` override). `std::optional<ThreadPool>::emplace` builds it
        // in place (ThreadPool is not movable); it joins its workers at end-of-scope.
        std::optional<substrate::ThreadPool> localPool;
        substrate::IExecutor* executor = injectedExecutor;
        if (executor == nullptr) {
            localPool.emplace(resolveCuPoolWidth(cus.size(), jobsOverride));
            executor = &*localPool;
        }

        // Submit one job per CU, writing BY INDEX `i` into its own slot + scratch — no shared
        // container is mutated, so there is nothing to lock. A `std::latch` counts completions;
        // the RAII guard fires `count_down()` even if `buildCuMir` throws (the ThreadPool worker
        // then logs the throw), so a throwing job can never DEADLOCK `done.wait()` — the slot
        // just stays nullopt and fails the compile below. `i` is captured BY VALUE so each job
        // owns its index; everything else is captured by reference and outlives `done.wait()`.
        std::latch done{static_cast<std::ptrdiff_t>(cus.size())};
        for (std::size_t i = 0; i < cus.size(); ++i) {
            executor->submit([&, i] {
                struct CountDownGuard {
                    std::latch& latch;
                    ~CountDownGuard() { latch.count_down(); }
                } const guard{done};
                cuMirSlots[i] = buildCuMir(cus[i], grammar, **targetR, **formatR,
                                           ccIndex, cuScratch[i], perCuOpts);
            });
        }
        done.wait();

        // Merge each CU's diagnostics into `reporter` in CU (index) ORDER — deterministic,
        // independent of which CU finished first. Merge EVERY CU BEFORE the failure check so a
        // failing build surfaces every CU's errors deterministically (not just the first-to-
        // fail's — an improvement the parallel model makes free: every CU is always built).
        bool allBuilt = true;
        for (std::size_t i = 0; i < cus.size(); ++i) {
            copyDiagnostics(cuScratch[i], reporter);
            if (!cuMirSlots[i].has_value()) allBuilt = false;
        }
        if (!allBuilt) {
            emitNullNoDiagnostic("a per-CU build (buildCuMir)");
            return false;  // ≥1 front-half tier failure — all diagnostics reported
        }
    }

    // Collect the built modules into the in-order vector the lower/merge path below consumes
    // (every slot is engaged here: N==1 built slot 0 inline, N>1 filled every slot + checked).
    std::vector<CuMirModule> cuMirs;
    cuMirs.reserve(cuMirSlots.size());
    for (auto& slot : cuMirSlots) cuMirs.push_back(std::move(*slot));

    // ── D-FF1-AR-STATICLIB-DRIVER-WIRING (c171): static-library output ──
    //
    // A `container: archive` format produces an `ar` STATIC LIBRARY (`.a`/
    // `.lib`): each CU lowers to its OWN relocatable member (NO cross-CU
    // merge — an archive PACKAGES separate objects; the FINAL foreign linker
    // pulls + merges only the members it needs), then `linkAndWriteStaticArchive`
    // bundles them (threading the ecosystem's `ar` flavor — SysV `.a` for
    // ELF/Mach-O, COFF `.lib` for PE). Dispatched on the FORMAT's declared
    // container (the §B format-container decision, user Option 1), NEVER the
    // artifactProfile (`artifact_profile.hpp`'s standing veto). `outPath`
    // already carries the `.a`/`.lib` extension (the outputExtension archive
    // arm) and `enforceArtifactProfileFormat` already validated the project's
    // `staticlib` profile against this format's served set.
    if ((*formatR)->isStaticArchive()) {
        std::string const memberExt =
            (*formatR)->kind() == ObjectFormatKind::Pe ? ".obj" : ".o";
        std::vector<AssembledModule> members;
        std::vector<std::string>     memberNames;
        members.reserve(cuMirs.size());
        memberNames.reserve(cuMirs.size());
        for (std::size_t i = 0; i < cuMirs.size(); ++i) {
            auto mod = lowerCuMirToAssembly(cuMirs[i], (*formatR)->processArgs(),
                                            (*formatR)->entryVerbs(),
                                            (*formatR)->sehPersonality(),
                                            (*formatR)->name(),
                                            (*formatR)->kind(), reporter);
            if (!mod) return false;  // back-half tier failure already reported
            members.push_back(std::move(*mod));
            // Member file name: distinct + valid `ar` name. The armap
            // (symbol → member index) drives a linker's member selection, so
            // the name is cosmetic; a lone CU takes `<stem><ext>`, multiple
            // CUs disambiguate by index.
            memberNames.push_back(
                cuMirs.size() == 1
                    ? std::string{sourceStem} + memberExt
                    : std::string{sourceStem} + "_" + std::to_string(i) + memberExt);
        }
        // ── D-FF1-STATICLIB-FAT-ARCHIVE: merge input static archives ──
        // When this static-library build is also handed INPUT `--resolve-library`
        // static archives, bundle EVERY member of each INTO this library (a
        // merged/"fat" archive, à la `libtool -static`). A static library
        // PACKAGES objects, so all members are carried — NOT the lazy
        // referenced-subset the exe/final-link path pulls — because a DOWNSTREAM
        // link against this library must be able to pull any of them (dropping an
        // unreferenced member would silently ship an incomplete library). Dynamic
        // `--resolve-library` libraries stay on the per-CU FFI path (a member may
        // reference libc externs, resolved at the FINAL link against this
        // library). Fails loud on any open / parse / member-read error (never a
        // silent member omission). CU-derived members lead; the input archives'
        // members follow.
        if (!staticArchives.empty()) {
            auto extracted = extractStaticArchiveMembers(
                std::span<std::filesystem::path const>{staticArchives},
                **targetR, **formatR, reporter);
            if (!extracted) return false;  // fail-loud already reported
            members.reserve(members.size() + extracted->modules.size());
            memberNames.reserve(memberNames.size() + extracted->names.size());
            for (std::size_t i = 0; i < extracted->modules.size(); ++i) {
                members.push_back(std::move(extracted->modules[i]));
                memberNames.push_back(std::move(extracted->names[i]));
            }
        }
        return reported(linkAndWriteStaticArchive(members, memberNames,
                                                  **targetR, **formatR, outPath,
                                                  reporter, imageRequest));
    }
    // N==1 (the CU5 multi-file-single-CU case): lower the sole CU + link it. UNCHANGED
    // from cycle 24 — byte-identical single-CU output. Routing N==1 through the merge
    // would re-intern CU0's types into a fresh host (a no-op for correctness, but extra
    // work + a different code path); keep the proven single-CU lowering for byte-identity.
    if (cuMirs.size() == 1) {
        auto mod = lowerCuMirToAssembly(cuMirs[0], (*formatR)->processArgs(),
                                        (*formatR)->entryVerbs(),
                                        (*formatR)->sehPersonality(),
                                        (*formatR)->name(),
                                        (*formatR)->kind(), reporter);
        if (!mod) {              // back-half tier failure already reported via `reporter`
            emitNullNoDiagnostic("back-half lower (lowerCuMirToAssembly)");
            return false;
        }
        // c165 (D-LK-STATIC-LINK): link against any `ar` static archives named on
        // `--resolve-library` (pull the referenced members + merge them in). With
        // no static archives this is `linkAndWrite({mod})`, unchanged.
        return reported(linkAndWriteWithStaticArchives(
            std::move(*mod), std::span<std::filesystem::path const>{staticArchives},
            **targetR, **formatR, outPath, reporter, imageRequest));
    }

    // N>1 (CU6 multi-CU): WHOLE-PROGRAM MIR MERGE (Cycle 25 Stage C). Fold the N per-CU
    // modules into ONE module over a fresh host lattice, resolving cross-CU calls to
    // DIRECT intra-module calls (no cycle-19 assembled-tier thunk), then lower that single
    // module ONCE and link it (the linker takes its single-module path). The merge reads
    // each CU's `nameOf` (SemanticModel symbol names + extern mangledNames) while cloning,
    // so `cuMirs` must stay alive through `mergeCuMirs` — it does (function-local, no CU's
    // lattice is moved out: the host is FRESH, leaving every SemanticModel intact).
    // D-LK-MACHO-CROSSCU-MANGLE-MERGE-KEY (c118): the active format's C mangling, applied
    // to every DEFINITION merge-key (nameOf below) AND the entry-name set, so definitions
    // match the externs' already-mangled `mangledName` on macho (identity on elf/pe). Both
    // the def↔extern resolution and the `main` entry match key on this same convention.
    // D-FFI-CMANGLING-RULE-NOT-CONFIG-DRIVEN (step C4): the C-symbol decoration
    // rule is now READ FROM THE SCHEMA rather than looked up in a C++ table keyed
    // on the format KIND. Both merge rails below (`nameOf` for definitions, the
    // entry-name set) take THIS one value, so they cannot diverge — and
    // `validate()` guarantees it is a real scheme, never the `Unspecified`
    // sentinel. (The `ObjectFormatKind fmtKind` local that fed the retired table
    // outlived its last reader and is removed here — MEASURED as the only
    // `warning C4189` this translation unit produces under /W4, i.e. a dead
    // schema read that every reader of the paragraph above would reasonably
    // assume still drove something.)
    CSymbolDecorationScheme const cSymDecor =
        (*formatR)->cSymbolDecoration().scheme;
    std::vector<MergeCuInput> mergeInputs;
    mergeInputs.reserve(cuMirs.size());
    for (auto& cuMir : cuMirs) {
        MergeCuInput in;
        in.mir      = &cuMir.mir;
        in.interner = &cuMir.model.lattice().interner();
        // nameOf: symbol id → the cross-CU MATCH KEY. Covers DEFINITIONS (SemanticModel
        // record) AND extern IMPORTS (the import's mangledName, when the symbol has no
        // record — an extern reference's SymbolId is not in the semantic symbol table).
        // ★ D-LK-MACHO-CROSSCU-MANGLE-MERGE-KEY (c118): a definition's key is its source
        // name run through the FORMAT'S C MANGLING (`applyCMangling`), so it matches the
        // extern's already-mangled `mangledName` — on Mach-O a shell.c reference to
        // `_sqlite3_libversion` now matches sqlite3.c's definition `sqlite3_libversion`
        // (mangled to `_sqlite3_libversion`). applyCMangling is config-driven in the
        // literal sense since TF-C122 (D-FFI-CMANGLING-RULE-NOT-CONFIG-DRIVEN C4): it
        // is handed the format's DECLARED `cSymbolDecoration.scheme`, not a C++ table
        // keyed on the format identity. `none` is IDENTITY (elf/pe -- their cross-CU
        // match is unchanged); `leading-underscore` adds one `_` (macho). Safe by construction — every format writer names its
        // on-binary defined symbols synthetically (`_sym_<id>` / `sym_<id>`), so this key
        // is a MATCH key only, never the emitted symbol name (no double-mangle). Capturing
        // `&cuMir` is safe — `cuMirs` is done growing.
        // TF-C88 (D-CSUBSET-ASM-LABEL-SYMBOL-RENAME): the DEFINITION arm routes
        // through
        // `linkNameFor`, which returns an explicit assembler name VERBATIM. That is
        // what keeps this key in the SAME space as the IMPORT arm below: an
        // extern's `mangledName` is itself built from `linkNameFor` at ingest, so a
        // labelled definition and a labelled reference to it produce the identical
        // string and `mir_merge` still collapses them. Honoring the label on one
        // arm only would leave `definedNames.count(e.mangledName)` missing, the
        // sibling-defined extern unstripped, and an intra-image call silently
        // emitted as a dynamic import. Byte-identical for every unlabelled symbol.
        in.nameOf = [cuMirP = &cuMir, cSymDecor](SymbolId s) -> std::string {
            // TF-C121 (D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME): the fourth
            // input is passed here too — a required parameter precisely so a rail
            // cannot quietly omit it and reintroduce the divergence described
            // above with a different override channel.
            if (SymbolRecord const* r = cuMirP->model.recordFor(s)) {
                return dss::ffi::linkNameFor(r->name, r->asmName, cSymDecor,
                                             r->linkName);
            }
            for (auto const& e : cuMirP->externImports) {
                if (e.symbol.v == s.v) return e.mangledName;
            }
            return std::string{};
        };
        in.externImports = cuMir.externImports;
        // FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER) + D-FFI-PE-CRT-UCRT-MIGRATION (Phase 3):
        // this CU's referenced-only shipped-library shim symbols (BOTH the <threads.h> and
        // <stdio.h> families), so the merge planning assigns them a merged id (else the
        // clone aborts on a shim GlobalAddr — the multi-CU threads defect).
        // Non-owning; `cuMir` (in `cuMirs`) outlives the merge.
        in.synthRecipes = &cuMir.libraryShimRecipes;
        mergeInputs.push_back(std::move(in));
    }

    // Fresh host TypeLattice for the merged module: seeded with CU0's id + source
    // language (cosmetic — the registry's sourceLanguage tags extension types; c-subset
    // has none). The merge re-interns ALL CUs (incl CU0) into this fresh host, so no
    // SemanticModel's lattice is mutated or moved — still "re-intern at merge", just a
    // fresh host rather than CU0's in-place. Agnostic: id + language string, no branch.
    TypeLattice host{cuMirs[0].cuId,
                     std::string{cuMirs[0].model.lattice().registry().sourceLanguage()}};

    // ── D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE: resolve the program entry BEFORE the
    //    merge, across EVERY CU, through the SAME single owner the single-CU path
    //    uses ──────────────────────────────────────────────────────────────────────
    //
    // ★★★ THIS REPLACED A NAME LIST HANDED TO `mergeCuMirs`, AND THE NAME LIST WAS
    // TWO DEFECTS AT ONCE.
    //
    //   (1) IT WAS FORMAT-BLIND. Every name the language declared was passed in, so
    //       `wmain` was an entry candidate on ELF and Mach-O. MEASURED 2026-08-10
    //       on HEAD `3e86a187`: `int wmain(int, unsigned short**)` with no `main`,
    //       built for `elf64-x86_64-linux-exec`, was SELECTED as the Linux program
    //       entry. Candidacy now requires the format to realize the verb the
    //       matched language row needs.
    //   (2) THE MERGE'S OWN SCAN HAS NO AMBIGUITY CHECK. `mergeCuMirs` walks CUs
    //       and functions and takes the FIRST name in the set it is given, so
    //       `main` in a.c and `wmain` in b.c silently picked one — while the
    //       single-CU path refused the same program. Two paths, two answers, and
    //       the merged one was the silent wrong-entry.
    //
    // Resolving HERE fixes both without touching the merge's contract: the scan runs
    // over each CU's SemanticModel (which has the signatures the merge cannot
    // express), and only the ONE winning name is handed to `mergeCuMirs` — so its
    // first-match-wins walk is now provably unambiguous rather than accidentally so.
    std::vector<EntryCandidate> cands;
    std::vector<std::uint32_t>  candSym;   // per-model record indices; unused here
    for (auto& cu : cuMirs) collectEntryCandidates(cu.model, cands, candSym);
    bool entryOk = true;
    auto const resolvedEntry = resolveProgramEntry(
        cands, (*formatR)->entryVerbs(), (*formatR)->name(), reporter, entryOk);
    if (!entryOk) return false;   // undefined / ambiguous entry — already reported.

    // The winning name, MANGLED to the merge's DEFINITION-KEY convention.
    //
    // ★★ THE MANGLING MUST GO THROUGH THE SAME FUNCTION `nameOf` USES, NOT MERELY
    // THE SAME IDEA. `MergeCuInput::nameOf` calls `dss::ffi::linkNameFor(name,
    // asmName, scheme, linkName)`, which honours a per-symbol `asm` label and a
    // descriptor `linkName` OVERRIDE; `applyCMangling` applies only the format's
    // C decoration and knows nothing about either. For a plain `main` the two agree,
    // which is exactly why using the wrong one is a latent trap rather than an
    // immediate failure — it would diverge only for an entry carrying an asm label.
    // Feed the SymbolRecord through `linkNameFor` so the key is the same string by
    // CONSTRUCTION (D-LK-MACHO-CROSSCU-MANGLE-MERGE-KEY, c118: macho keys `_main`,
    // identity on elf/pe).
    std::vector<std::string> entryNames;
    EntryMaterialization entryVerb = EntryMaterialization::None;
    if (resolvedEntry.has_value()) {
        std::string_view const winner = cands[resolvedEntry->index].name;
        for (auto& cu : cuMirs) {
            bool done = false;
            for (auto const& rec : cu.model.symbols()) {
                if (rec.kind != DeclarationKind::Function) continue;
                if (!rec.entryVerb.has_value() || rec.name != winner) continue;
                entryNames.push_back(dss::ffi::linkNameFor(
                    rec.name, rec.asmName, cSymDecor, rec.linkName));
                done = true;
                break;
            }
            if (done) break;
        }
        entryVerb = resolvedEntry->verb;
    }

    auto merged = mergeCuMirs(
        std::span<MergeCuInput const>{mergeInputs.data(), mergeInputs.size()},
        std::move(host),
        std::span<std::string const>{entryNames.data(), entryNames.size()},
        reporter);
    if (!merged) return false;  // merge failure (conflict / verify) already reported.

    // UCRT-P4 (D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE + D-FFI-PE-CRT-UCRT-MIGRATION):
    // MATERIALIZE the resolved entry's arguments per its verb × the format's declared
    // mechanism. On the CRT-accessor route (Windows: the PE OS entry carries no C
    // argument vector) this appends the pre-main init that calls the UCRT populate +
    // accessor exports and forwards (argc, argv) to the user entry, retargeting
    // `userEntrySymbol` to it. Runs BEFORE optimize so an appended init is DCE-rooted
    // (Global) + optimized + lowered like any other. The interner is the merged host's
    // (the type space the merged TypeIds index into).
    //
    // ⚠ CALLED UNCONDITIONALLY — deliberately, and this is a CHANGE from c111, which
    // guarded the call with `if (processArgs.has_value())`. That guard was harmless
    // while the pass only synthesized, but it would SILENTLY SKIP the pass on Mach-O,
    // whose exec formats declare NO `processArgs` (dyld delivers argc/argv in the
    // argument registers before any DSS code runs). A pass must not be keyed on a
    // field that is legitimately absent on a whole platform.
    //
    // ⓘ THE VERB IS AN INPUT, and there is no gate here any more. The signature check
    // ran at the SEMANTIC tier, per definition, with a source span
    // (`S_EntryShapeNotDeclared`); candidacy was decided by `resolveProgramEntry`
    // above. This call does exactly one job.
    if (!realizeEntryShape(merged->mir, merged->host.interner(),
                           merged->userEntrySymbol, merged->externImports,
                           entryVerb, (*formatR)->processArgs(), cSymDecor,
                           (*formatR)->name(), reporter)) {
        return false;  // unusable mechanism — fail-loud already reported.
    }

    // FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER / D-CSUBSET-C11-THREADS-MACHO): whole-program
    // counterpart of the single-CU shim synth. The per-CU {SymbolId, recipeId} tables do not
    // survive the merge's symbol remap, so RECONSTRUCT the merged recipe map from the merged
    // symbol NAMES (== the recipe ids; the descriptor pins `synthesize == name`): a merged
    // symbol whose name is a known threads recipe AND that is NOT a defined function / FFI
    // extern is a shim to define. The `symbolNames` values are format-C-MANGLED (macho adds a
    // leading `_`) while the recipe vocabulary + the synth switch key on the BARE C name, so
    // `unapplyCMangling` un-decorates before the vocab check AND the bare id is what the synth
    // pass receives — identity on pe/elf (their C mangling is identity, "main" == un-mangled),
    // strips the `_` on macho. Runs BEFORE optimize so the shims are DCE-rooted + optimized
    // (canonical markers re-derived).
    // A no-op when the map is empty. ★ The merge's step-3c pre-registers each referenced-
    // only shim symbol with a merged id + a `symbolNames` entry (else the clone would abort
    // on a shim `GlobalAddr`), so a shim that is NOT collapsed onto a genuine user def lands
    // here as a not-defined/not-imported vocab name and IS synthesized — multi-CU synthesis
    // works. ★ EVIDENCE, corrected 2026-07-25: this used to claim "a 2-file pe64 witness
    // runs → 42" for THREADS, and no such example was ever shipped — every c11_threads /
    // pthread / thread_local example is single-source, so the claim pointed at a witness
    // that does not exist. The real coverage is: for <threads.h>, the unit test
    // `MirMerge.MultiCuThreadsShimRegistersAndSynthesizes`; for <stdio.h>, a genuine 2-TU
    // runtime witness, `examples/c-subset/shipped_sprintf_ucrt_crosscu` (exit 42, release
    // arm, pe64) — which exercises THIS code path, and is red-on-disable proven: neutering
    // the va-leaf refusal in `opt/passes/inlining.cpp` fails it (exit 50) while the
    // single-source `shipped_sprintf_ucrt` still passes. That asymmetry matters because
    // this seam synthesizes PRE-optimize while compile_pipeline's synthesizes POST — see
    // D-MIR-SYNTH-SHIM-SEAM-OPTIMIZE-PLACEMENT-ASYMMETRY. A shim collapsed onto a real user def is a
    // DEFINED symbol → filtered out → correctly not re-synthesized.
    {
        std::unordered_set<std::uint32_t> definedOrImported;
        for (std::uint32_t i = 0; i < merged->mir.moduleFuncCount(); ++i)
            definedOrImported.insert(merged->mir.funcSymbol(merged->mir.funcAt(i)).v);
        for (auto const& e : merged->externImports) definedOrImported.insert(e.symbol.v);
        // D-FFI-PE-CRT-UCRT-MIGRATION (Phase 3): PARTITION the reconstructed recipe map by
        // `dss::ffi::shimFamilyOf` BEFORE calling either synth pass — the SAME split
        // `lowerCuMirToAssembly` applies to the single-CU `libraryShimRecipes` map
        // (compile_pipeline.cpp), for the identical reason: each pass fails loud on a
        // recipe id it has no switch arm for (its own anti-vocab-drift backstop), so one
        // pass seeing the other family's ids would abort a build that never should have
        // failed. A `nullopt` family is an INTERNAL INVARIANT BREACH (the descriptor
        // loader already rejects an unknown `synthesize` id at READ time via the same
        // closed-vocab table `shimFamilyOf` reads), never a silently-dropped recipe —
        // reported with the DRIVER-band internal-invariant code
        // `D_SynthRecipeFamilyUnknown`, the SAME code the single-CU seam emits, and not
        // the linker's `K_NoMatchingObjectFormat` (which would send an operator to the
        // object-format config for what is a recipe-table defect).
        std::unordered_map<std::uint32_t, std::string> mergedThreadsRecipes, mergedStdioRecipes;
        for (auto const& [symV, name] : merged->symbolNames) {
            std::string const bare = dss::ffi::unapplyCMangling(name, cSymDecor);
            if (!dss::ffi::isKnownSynthesizeRecipe(bare)
                || definedOrImported.find(symV) != definedOrImported.end()) {
                continue;
            }
            auto const family = dss::ffi::shimFamilyOf(bare);
            if (!family.has_value()) {
                dss::report(reporter, DiagnosticCode::D_SynthRecipeFamilyUnknown,
                            DiagnosticSeverity::Error,
                            std::format(
                                "synthesize recipe '{}' (symbol {{ {} }}) belongs to no "
                                "known shim family (D-FFI-PE-CRT-UCRT-MIGRATION) — "
                                "internal invariant breach: the descriptor loader should "
                                "have rejected an unknown recipe id at read time "
                                "(isKnownSynthesizeRecipe)",
                                bare, symV));
                return false;
            }
            switch (*family) {
            case dss::ffi::ShimFamily::Threads: mergedThreadsRecipes.emplace(symV, bare); break;
            case dss::ffi::ShimFamily::Stdio:   mergedStdioRecipes.emplace(symV, bare);   break;
            }
        }
        if (!synthesizeThreadsShim(merged->mir, merged->host.interner(),
                                   mergedThreadsRecipes, (*formatR)->librarySynthesis(),
                                   cSymDecor, merged->externImports, reporter)) {
            return false;  // internal invariant breach (vocab/switch drift) — reported.
        }
        // D-FFI-PE-CRT-UCRT-MIGRATION (Phase 3): the <stdio.h> printf-family shim sibling —
        // see `synth_stdio_shim.hpp` for the full contract. A clean no-op when
        // `mergedStdioRecipes` is empty. The va_list block is read from the SAME
        // resolved CC (`abi->cc`, D-FF3-3 above) the merged module's calling-convention
        // index was derived from — no second lookup, no format-name branch. It is passed
        // WHOLE, not narrowed to `.strategy`: `variadicUsesOverflowBase` is what selects
        // the shim's va leaf, and dropping it here would silently emit the home-base leaf
        // on an overflow-base target (see `CuMirModule::vaListLayout`). A CC that declares
        // no `vaListLayout` propagates as `nullopt`, NOT as a default-constructed layout:
        // "nothing declared" must stay distinguishable from a real declaration all the way
        // to the synth pass, which refuses it loudly (the single-CU seam threads the same
        // optional through `CuMirModule::vaListLayout`). Consulted only if a stdio recipe
        // actually appears.
        std::optional<VaListLayout> vaListLayout;
        if (abi->cc != nullptr && abi->cc->vaListLayout.has_value()) {
            vaListLayout = *abi->cc->vaListLayout;
        }
        if (!synthesizeStdioShim(merged->mir, merged->host.interner(),
                                 mergedStdioRecipes, vaListLayout,
                                 merged->externImports, reporter)) {
            return false;  // recipe/helper-import/va-strategy mismatch — reported.
        }
    }

    // Cycle 26 (D-OPT7-1): optimize the WHOLE-PROGRAM merged module with the configured
    // pipeline. The merge made every cross-CU call an intra-module DIRECT call, so the
    // inliner's `symToFunc` now resolves the callee — a cross-CU call becomes inline-
    // eligible exactly like an intra-CU one. `merged->host.interner()` is the type space
    // the merged TypeIds index into (the same interner `lowerMergedToAssembly` uses). The
    // optimizer runs MirVerifier after every pass (the merged-module safety net). DOUBLE-
    // OPT is correct: a cross-CU call's per-CU inline was a no-op (extern, unresolvable
    // per-CU); the merged inline does the work. Same pipeline resolution as the per-CU
    // path (`optimizeModule`), so the examples-runner's `["Inlining"]` override flows here
    // via `compileOpts.pipelineOverride`.
    //
    // D-OPT7-CROSSCU-LTO-SINGLE-OPTIMIZE (deferred, efficiency-only): the N>1 path
    // currently optimizes each CU's MIR in `buildCuMir` AND optimizes the merged module
    // here (DOUBLE-OPT). Correct but redundant work — a true LTO shape would skip the
    // per-CU optimize for multi-CU builds and optimize the whole-program merged module
    // ONCE. Not done now because `buildCuMir` is shared with the N==1 path (which must
    // still optimize per-CU) and the redundant per-CU pass is correctness-neutral.
    if (!optimizeModule(merged->mir, **targetR, merged->host.interner(),
                        compileOpts, reporter)) {
        return false;  // optimize / verify failure already reported via `reporter`
    }

    // c116 (D-WIN64-SEH-FUNCLETS): synthesize the SEH filter funclets + record the
    // scope ranges on the WHOLE-PROGRAM merged module (post-optimize, mirroring the
    // single-CU seam). Trigger = presence of SehTryBegin (a fast no-op otherwise).
    // Appends the __C_specific_handler personality import on demand.
    std::vector<MirSehScope> sehScopes;
    if (!synthesizeSehFunclets(merged->mir, merged->host.interner(),
                               merged->externImports,
                               (*formatR)->sehPersonality(), cSymDecor,
                               (*formatR)->name(), sehScopes, reporter)) {
        return false;  // unsupported SEH shape (c116b frontier) — fail-loud reported.
    }

    // D-FFI-EXTERN-CALL-DISPATCH: the merged module compiles to ONE
    // (target, format); pass that format's extern-call shape so MIR→LIR
    // selects the right call-site opcode for any surviving extern import.
    // D-CSUBSET-BITFIELD-ABI-EXACT: resolve + pass the FORMAT-determined bit-field
    // strategy (gnu_packed / msvc_straddle) so a bit-field global in the merged
    // whole-program image is laid out byte-ABI-exact for the active format.
    // D-CSUBSET-LONG-DOUBLE-IEEE128-ARITH (LD-2): resolve the F128 softcall
    // runtime library here (the merge lower body has no ObjectFormatKind in
    // scope) — exactly where externCallDispatch is pre-resolved from the
    // format. Empty = the format declares none (nullopt → F128 softcall fails
    // loud).
    std::string_view const wfLib =
        (*targetR)->wideFloatSoftcallLibrary((*formatR)->kind());
    std::optional<std::string> wideFloatSoftcallLibrary =
        wfLib.empty() ? std::nullopt
                      : std::optional<std::string>(std::string(wfLib));
    auto mod = lowerMergedToAssembly(*merged, grammar, **targetR,
                                     (*formatR)->dataModel(),
                                     effectiveBitFieldStrategy(**targetR, **formatR),
                                     ccIndex, cuMirs[0].cuId,
                                     (*formatR)->externCallDispatch(),
                                     (*formatR)->dataImportBinding(),
                                     // D-LK-ARM64-EXTERN-DATA-ADDR-PIE-GOT
                                     // (TF-C52): the format's extern-ADDRESS
                                     // binding (`got` on arm64 relocatable /
                                     // static-archive; nullopt elsewhere).
                                     (*formatR)->externAddrBinding(),
                                     // TLS C1 (D-CSUBSET-THREAD-LOCAL): the
                                     // format's thread-local access block.
                                     (*formatR)->tlsAccess(),
                                     std::move(sehScopes),
                                     std::move(wideFloatSoftcallLibrary),
                                     reporter);
    if (!mod) return false;  // back-half tier failure already reported via `reporter`
    // c165 (D-LK-STATIC-LINK): the merged whole-program client module links
    // against any `ar` static archives named on `--resolve-library` the same way
    // the single-CU path does (pull referenced members + merge). No archives =>
    // `linkAndWrite({mod})`, unchanged.
    return reported(linkAndWriteWithStaticArchives(
        std::move(*mod), std::span<std::filesystem::path const>{staticArchives},
        **targetR, **formatR, outPath, reporter, imageRequest));
}

// c9 (Phase-2): the ObjectFormatKind a target spec compiles to, or nullopt if the
// spec is malformed or its object-format schema won't load — those targets group
// under the nullopt (pure-existence) front-end build and STILL reach
// `compileOneTarget`, which re-parses the spec and emits the authoritative
// D_InvalidTargetSpec / D_SchemaLoadFailed (never silently dropped). Used to build
// the front-end once per DISTINCT object-format so `__has_include` is per-target
// truthful.
std::optional<ObjectFormatKind> formatKindOfSpec(std::string const& spec) {
    auto parsed = TargetSpec::parse(spec);
    if (!parsed) return std::nullopt;
    auto formatR = ObjectFormatSchema::loadShipped(parsed->formatName);
    if (!formatR.has_value()) return std::nullopt;
    return (*formatR)->kind();
}

// ASCII lower-case a copy. File extensions are ASCII, and the driver's
// extension match must agree byte-for-byte with `UnitBuilder::schemasForPath_`'s
// — a `.S` that the driver accepts and the builder then routes elsewhere would
// be the two-resolvers-disagree bug one layer up.
[[nodiscard]] std::string asciiLowerCopy(std::string_view in) {
    std::string out{in};
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// ── TF-C74: what identifies ONE front-end build ───────────────────────────
//
// The front-end is built once per DISTINCT key and CACHED. Before TF-C74 the
// key was the OBJECT FORMAT ALONE, which was correct only while the
// preprocessed text depended on nothing else per-target. It now depends on the
// TARGET's `predefinedMacros` too, and `arm64:elf64-aarch64-linux-exec` and
// `x86_64:elf64-x86_64-linux-exec` share one object format — so a format-only
// key would hand the second target the FIRST target's preprocessed text, with
// the first target's architecture macros baked in. That is a silent
// miscompile, strictly worse than a loud `#error`, so widening the key is a
// REQUIRED CORRECTNESS CHANGE, not an optimization.
//
// `targetName` is a CACHE KEY and nothing else — it is compared only against
// other target names, NEVER against a literal. Empty when the language has no
// preprocess pass, so such a build still collapses to ONE CU for all targets.
//
// ── TF-C97: `formatName` joins the key, for the SAME reason `targetName` did ──
//
// The preprocessed text now also depends on the OBJECT FORMAT's own
// `predefinedMacros` (`__LP64__`/`_LP64`), and those are declared per format
// FILE, not per format KIND. `format` (the kind) cannot stand in for the file:
// `x86_64:elf64-x86_64-linux-exec` and a hypothetical `elf32-*` file are both
// kind `elf` while declaring DIFFERENT data models — one key, two answers, and
// the second target would silently inherit the first's macros. That is the
// identical silent miscompile TF-C74 widened this key to prevent, one layer
// down, so this widening is a REQUIRED CORRECTNESS CHANGE too and not an
// optimization. It costs nothing on the shipped matrix: within one target,
// distinct format names already mean distinct legs.
//
// `formatName` is likewise a CACHE KEY and nothing else — compared only
// against other format names, never against a literal.
struct CuBuildKey {
    std::string                     targetName;
    std::string                     formatName;
    std::optional<ObjectFormatKind> format;
    // D-PP-HEADER-CASE-INSENSITIVE-PE: the active FORMAT FILE's declared
    // header-NAME case rule. It belongs in the KEY on the same principle every
    // other member is here for — it can change the preprocessed token stream
    // (`__has_include(<Windows.h>)` answers differently under it), so a CU
    // built under one value must never be reused under another. In practice it
    // is a function of `formatName` and adds no extra builds; carrying it makes
    // the key's rule ("everything that changes the preprocessed source") true
    // by construction rather than by an argument a reader has to reconstruct.
    HeaderNameMatching              headerNameMatching = kDefaultHeaderNameMatching;
    // ★★★ D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET: the SOURCE LANGUAGE this
    // target's CU is parsed under. `--language asm-x86_64-att` for every
    // target when the caller named one; the TARGET's own
    // `defaultAssemblyLanguage` when it did not — so `dss --compile hello.s
    // --target x86_64:… --target arm64:…` parses ONE file under TWO grammars.
    //
    // ⚠⚠ POPULATED UNCONDITIONALLY — READ THE NEXT SENTENCE BEFORE MOVING IT.
    // Every OTHER member of this key is populated only `if (ppEnabled)`,
    // because every other member exists to distinguish PREPROCESSED text and a
    // language without a preprocess pass produces identical CUs for every
    // target. An assembly dialect has NO preprocess pass, so under that gate
    // ALL targets would collapse onto the empty key, the first target's CU
    // would be handed to every other target, and a `.s` would be parsed under
    // the FIRST CPU's dialect and then compiled for the SECOND — a silent
    // miscompile of exactly the family TF-C74 widened this key to prevent. The
    // grammar is not a property of the preprocessed text; it is the property
    // that DECIDES what the text means. It therefore belongs in the key on its
    // own terms and is gated on nothing.
    std::string                     languageName;
    [[nodiscard]] bool operator<(CuBuildKey const& o) const noexcept {
        if (targetName != o.targetName) return targetName < o.targetName;
        if (formatName != o.formatName) return formatName < o.formatName;
        if (format != o.format) return format < o.format;
        if (headerNameMatching != o.headerNameMatching) {
            return headerNameMatching < o.headerNameMatching;
        }
        return languageName < o.languageName;
    }
};

// ★★ D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET: resolve the SOURCE LANGUAGE for
// ONE target, or fail loud naming the target.
//
// Two inputs, in strict precedence:
//   1. The CALLER's `--language <name>` (`explicitGrammar`), if given. It wins
//      outright, for every target, with no extension gate — naming the
//      language IS the interface for "this file is written for one CPU", the
//      analogue of `gcc -x`, and it is what `examples/asm/*/expected.json`
//      uses. A caller who says `--language c-subset` and hands over a `.s` has
//      asked for that and gets it.
//   2. Otherwise the TARGET's declared `defaultAssemblyLanguage` — a NAME the
//      target file carries as vocabulary. Because it is read PER TARGET, one
//      invocation resolves as many grammars as it has distinct CPUs.
//
// Under arm 2 the extension IS checked, and that check is the point: the
// caller named nothing, so a wrong answer here would be silent. Every input
// file's extension must be claimed by the resolved language's
// `fileExtensions`; anything else fails loud naming the target, the language
// it declared, and the offending file. `D_UnknownFileExtension` covers both
// failure shapes — "the target declares no language at all" and "the language
// it declares does not claim this extension" — because both are literally
// "this extension resolved to no source language"; the MESSAGES differ, the
// code does not.
//
// Returns null on failure, having emitted. AGNOSTIC: no language, CPU or
// format NAME is compared against a literal anywhere below — the target
// supplies the name, the grammar loader resolves it, and the extension match
// is a set-membership test over config-declared strings.
[[nodiscard]] std::shared_ptr<GrammarSchema const> resolveGrammarForTarget(
    std::shared_ptr<GrammarSchema const> const& explicitGrammar,
    TargetSchema const&                         target,
    std::vector<std::string> const&             sourceFiles,
    std::map<std::string, std::shared_ptr<GrammarSchema const>>& cache,
    DiagnosticReporter&                         rep) {
    if (explicitGrammar) return explicitGrammar;

    std::string const declared{target.defaultAssemblyLanguage()};
    if (declared.empty()) {
        emitDriver(
            rep, DiagnosticCode::D_UnknownFileExtension,
            "no source language: no --language was given and target '"
            + std::string{target.name()}
            + "' declares no 'defaultAssemblyLanguage', so there is nothing "
              "to parse these sources under. Either pass --language <name>, "
              "or declare the target's assembly dialect by name in "
              "src/dss-config/targets/" + std::string{target.name()}
            + ".target.json.");
        return nullptr;
    }

    auto it = cache.find(declared);
    if (it == cache.end()) {
        auto loaded = GrammarSchema::loadShipped(declared);
        if (!loaded.has_value()) {
            forwardConfigDiagnostics(loaded.error(), rep);
            emitDriver(rep, DiagnosticCode::D_SchemaLoadFailed,
                       "target '" + std::string{target.name()}
                       + "' declares defaultAssemblyLanguage '" + declared
                       + "', which could not be loaded — the reason is in the "
                         "configuration diagnostic(s) above (config: "
                         "src/dss-config/sources/" + declared + ".lang.json).");
            // Negative NOT memoized: a load failure is fatal for this run
            // anyway, and caching it would make a future retry-after-fix path
            // answer from a stale miss.
            return nullptr;
        }
        // D-CONFIG-WARNINGS-DISCARDED-ON-SUCCESSFUL-LOAD: a document that loads
        // CLEANLY can still have said something, and until now nothing read it.
        // ★ INSIDE THE CACHE-MISS BLOCK ON PURPOSE — one emission per document
        // per run. Forwarding after the cache lookup would repeat every warning
        // once per TARGET, which is how a real warning gets tuned out.
        forwardConfigDiagnostics((*loaded)->loadDiagnostics(), rep);
        it = cache.emplace(declared, *loaded).first;
    }
    auto const& schema = it->second;

    // Every input must be claimed by the language the TARGET chose. The
    // comparison is case-insensitive on the extension INCLUDING its dot, the
    // same rule `UnitBuilder::schemasForPath_` applies, so `foo.S` and `foo.s`
    // route identically on every host filesystem.
    for (auto const& file : sourceFiles) {
        std::string const ext = asciiLowerCopy(fs::path{file}.extension().string());
        bool claimed = false;
        for (std::string_view declaredExt : schema->fileExtensions()) {
            if (asciiLowerCopy(declaredExt) == ext) { claimed = true; break; }
        }
        if (claimed) continue;
        std::string known;
        for (std::string_view declaredExt : schema->fileExtensions()) {
            if (!known.empty()) known += ", ";
            known += std::string{declaredExt};
        }
        emitDriver(
            rep, DiagnosticCode::D_UnknownFileExtension,
            "no source language for '" + file + "': no --language was given, "
            "so target '" + std::string{target.name()}
            + "' selected its declared defaultAssemblyLanguage '" + declared
            + "' — which claims " + (known.empty() ? "no extensions" : known)
            + " and not '" + (ext.empty() ? std::string{"<none>"} : ext)
            + "'. Pass --language <name> to name the source language "
              "explicitly.");
        return nullptr;
    }
    return schema;
}

// Build N CUs' front-end (via `buildCus`), then compile them to each target — the
// linker MERGES the N CUs into ONE image per target (LK11). Shared by
// `compileFiles` (one CU5 multi-file CU → 1-element vector) and `compileUnits` (N
// single-file CUs); the only difference is the `buildCus` closure each passes.
// c9: `buildCus(key, …)` is invoked ONCE PER DISTINCT `CuBuildKey` among the
// targets (the front-end's `__has_include` depends on the active format, and —
// TF-C74 — its predefined macros depend on the target), so the CU is rebuilt only
// when the key actually changes the preprocessed source; the common single-target
// case builds exactly once. Returns 0 on success, 1 on any error.
// D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET: `grammar` used to be ONE
// `GrammarSchema const&` for the whole invocation, resolved before the
// per-target loop ever ran. It is now `explicitGrammar` — the caller's
// `--language`, or NULL meaning "ask each target" — and the closure receives
// the RESOLVED grammar for the key it is building. `sourceFiles` rides along
// for exactly one reason: when the language came from the target rather than
// the caller, every input's extension has to be checked against it, and that
// check needs the paths (see `resolveGrammarForTarget`).
int runCusToTargets(
    std::function<std::vector<CompilationUnit>(
        CuBuildKey const&, std::span<PredefinedMacroDef const>,
        std::span<PredefinedMacroDef const>,
        std::shared_ptr<GrammarSchema const> const&)> buildCus,
    std::shared_ptr<GrammarSchema const> const& explicitGrammar,
    std::vector<std::string> const&             sourceFiles,
    std::string const&                          sourceStem,
    std::vector<std::string> const&             targets,
    DiagnosticReporter&                         rep,
    std::optional<std::filesystem::path> const& outputDir,
    // D-AP2-OUTPUT-ROUTING: the project artifactName override (nullopt ⇒
    // source stem) + the force-`<formatName>/`-subdir flag, threaded verbatim
    // to `compileOneTarget` alongside `outputDir` (the CLI path passes
    // nullopt/false ⇒ output byte-identical).
    std::optional<std::string> const&           artifactName,
    bool                                        perFormatOutputSubdir,
    CompileConfig                               config,
    ::dss::opt::OptPipeline const*              pipelineOverride,
    std::vector<ResolveLibrarySpec> const&      resolveLibraries,
    // D-PERF-4-CU-PARALLELISM: the per-CU build executor (nullptr ⇒ internal
    // pool) + the `--jobs` override, threaded verbatim to `compileOneTarget`.
    substrate::IExecutor*                       executor,
    unsigned                                    jobsOverride,
    // D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: the per-PROGRAM image knobs
    // (stack reserve), threaded verbatim to `compileOneTarget` — which
    // hands them to the link step, where the format's DECLARED capability
    // gates them. Passed PER TARGET, not resolved once: two targets of one
    // build can differ in whether their format can carry the request.
    ImageRequest const&                         imageRequest) {
    // ── TF-C74 (a): resolve every target — and every object format — BEFORE
    //    the CU build ───────────────────────────────────────────────────────
    //
    // The CU build now needs each target's `predefinedMacros`, so target
    // schemas must be resolved FIRST. This also repairs a LATENT ordering
    // defect: the CU build used to run before any target schema loaded, and
    // the `if (rep.hasErrors()) return 1;` below aborts before
    // `compileOneTarget` ever emits the authoritative `D_SchemaLoadFailed` —
    // so a bad `--target` surfaced as whatever the front-end happened to
    // produce (on the sqlite corpus: a cascade of `#error architecture not
    // supported`) instead of saying the target was wrong. Emitting here means
    // a bad target spec is now diagnosed as a bad target spec.
    //
    // A target whose spec is malformed or whose schema won't load is FATAL
    // here; the pre-TF-C74 fallback (group it under the nullopt key and let
    // `compileOneTarget` re-diagnose it) can no longer work, because we would
    // have no predefine list to build its CU with — and silently building it
    // with SOME OTHER target's macros is exactly the miscompile this change
    // exists to prevent.
    //
    // ── TF-C74 (D-PROGRAM-UNKNOWN-OBJECT-FORMAT-SILENT): the FORMAT half ──
    //
    // The OBJECT FORMAT is resolved here for the same reason the target is,
    // and it is the same defect: the front-end's output depends on the ACTIVE
    // FORMAT (format-gated predefines, `__has_include` of a format-gated
    // shipped header, the per-format descriptor availability set), so an
    // unknown format used to build a CU with NO format at all — every gated
    // arm vanished, the source's own fail-loud `#else` fired, and the run
    // returned at the drain below with a pile of header errors and NOT ONE
    // WORD about the format. MEASURED before this change on a two-line arch
    // ladder: `--target arm64:macho64-arm64-darwn-exec` (one letter dropped)
    // produced exactly one diagnostic, `P_PreprocessorErrorDirective`, and
    // zero mention of `macho64-arm64-darwn-exec`.
    //
    // The format check keys on the FORMAT NAME and nothing else: the
    // target-name dedupe below cannot be allowed to gate it, because one
    // target may appear with several formats (`x86_64:elf…` + `x86_64:pe…`)
    // and a format validated behind a target-keyed skip would be silently
    // unchecked after the first — the very failure mode being closed. (It
    // was written ahead of a target-name dedupe `continue`; that `continue`
    // is now a memoized lookup for the pair check's sake — see below — but
    // the reason this check owns its own key is unchanged.) Its
    // `formatChecked` set keeps the diagnostic one-per-distinct-format-name.
    //
    // The format schema is retained for the DURATION OF THIS FUNCTION (the
    // PAIR check below needs it next to its target, and — TF-C97 — the CU-key
    // loop reads its `predefinedMacros`), then dropped: this pass owns
    // rejection, not caching. `compileOneTarget` remains the authoritative
    // consumer and still loads its own, so nothing downstream depends on a
    // copy kept alive here.
    //
    // ── TF-C74 (D-PROGRAM-TARGET-FORMAT-PAIR-VALIDATED-LATE): the PAIR ────
    //
    // FACET 3 of the same ordering defect, and the one the other two could
    // not see: a spec's two halves can each be individually VALID while
    // their COMBINATION is nonsense. `arm64:elf64-x86_64-linux-exec` names a
    // real target AND a real object format, so both checks above PASS — and
    // only `crossValidateTargetFormat` rejects it. That call still lives in
    // `compileOneTarget`, i.e. downstream of the CU build and behind the same
    // drain, so on a source whose headers cascade the run
    // ended with ONLY the header `#error`. MEASURED before this change, on
    // the arch ladder used by the witness: `--target
    // arm64:elf64-x86_64-linux-exec` produced exactly ONE diagnostic,
    // `P_PreprocessorErrorDirective`, and not one word about the mismatch —
    // while the SAME spec over a header-less source printed the full
    // `D_TargetMachineCodeMismatch`. The diagnostic was never missing, only
    // unreachable.
    //
    // The CALL is hoisted, never the MESSAGE: `crossValidateTargetFormat` is
    // already the one emitter, so both sites say the identical thing by
    // construction — the same rule the two half-checks bought with their
    // shared `emit*SchemaLoadFailed` helpers. `compileOneTarget` keeps its
    // call: it is a [[nodiscard]] helper that must stay correct when invoked
    // directly, and with all three facets now pre-flighted its copy is
    // DEFENSE IN DEPTH rather than a live path — which is exactly why it must
    // share the emitter instead of restating the message.
    //
    // ★ THE DE-DUPLICATION KEY IS THE ORDERED PAIR, and it cannot be either
    // half. This check's verdict is a RELATION over both names, so a key that
    // drops one half silently skips distinct pairs that happen to share the
    // other: keyed on the TARGET, `arm64:elf64-aarch64-linux-exec` (good) +
    // `arm64:elf64-x86_64-linux-exec` (bad) validates only the first; keyed on
    // the FORMAT, `arm64:elf64-aarch64-linux-exec` (good) +
    // `x86_64:elf64-aarch64-linux-exec` (bad) likewise. Both are the very
    // silent skip being closed, and both are MEASURED red-on-disable in
    // `TFC74PairCheckDedupeKeyIsTheOrderedPair` — each wrong key reds exactly
    // the leg aimed at it. That is also why the target-name dedupe below is
    // now a MEMOIZED LOOKUP instead of an early `continue`: the pair check has
    // to run for every spec, and a `continue` keyed on one half would have
    // re-introduced the bug one level up.
    std::map<std::string, std::shared_ptr<TargetSchema const>> targetByName;
    std::unordered_set<std::string>                            formatChecked;
    std::unordered_map<std::string,
                       std::shared_ptr<ObjectFormatSchema const>> formatByName;
    std::set<std::pair<std::string, std::string>>              pairChecked;
    for (auto const& spec : targets) {
        auto parsed = TargetSpec::parse(spec);
        if (!parsed) {
            emitDriver(rep, DiagnosticCode::D_InvalidTargetSpec,
                       targetSpecErrorMessage(spec, parsed.error()));
            continue;
        }
        if (formatChecked.insert(parsed->formatName).second) {
            auto formatR = ObjectFormatSchema::loadShipped(parsed->formatName);
            if (!formatR.has_value()) {
                emitObjectFormatSchemaLoadFailed(rep, formatR.error(),
                                                 parsed->formatName);
                // No `continue`: a spec can be wrong in BOTH halves, and the
                // operator deserves both names in one run rather than one
                // recompile per typo. The drain below aborts either way.
            } else {
                formatByName.emplace(parsed->formatName, *formatR);
            }
        }
        // Load each distinct target ONCE, memoized. (Was `if
        // (targetByName.count(...)) continue;` — see the pair-key note above
        // for why the `continue` had to go.) A failed load is still fatal for
        // THIS spec: with no target schema there is no pair to validate, and
        // the reason is already on the reporter.
        auto targetIt = targetByName.find(parsed->targetName);
        if (targetIt == targetByName.end()) {
            auto targetR = TargetSchema::loadShipped(parsed->targetName);
            if (!targetR.has_value()) {
                emitTargetSchemaLoadFailed(rep, targetR.error(),
                                           parsed->targetName);
                continue;
            }
            targetIt = targetByName.emplace(parsed->targetName, *targetR).first;
        }
        auto const formatIt = formatByName.find(parsed->formatName);
        if (formatIt != formatByName.end()
            && pairChecked.emplace(parsed->targetName,
                                   parsed->formatName).second) {
            // Unlike the two half-checks, the pair message names the target
            // and the two machine codes but NOT the format — so on a
            // multi-target build it cannot say by itself WHICH spec was
            // rejected. Route it through the same `[target=<spec>]` stamp the
            // per-target loop uses, so hoisting the call costs the operator
            // nothing the downstream path carried. Scratch config mirrors
            // that loop's: `rep`'s POLICY axes, cap/dedup relaxed because
            // those are enforced once at `rep` during the merge.
            auto pairCfg = rep.config();
            pairCfg.maxDiagnostics = std::numeric_limits<std::size_t>::max();
            pairCfg.maxPerCode     = std::numeric_limits<std::size_t>::max();
            pairCfg.dedupWindow    = 0;
            DiagnosticReporter pairScratch{pairCfg};
            // Verdict discarded deliberately: no `continue`, for the same
            // reason the format half has none — a spec wrong in more than one
            // way must report every reason in ONE run. The drain aborts.
            (void)crossValidateTargetFormat(*targetIt->second,
                                            *formatIt->second, pairScratch);
            mergeWithTargetContext(pairScratch, spec, rep);
        }
    }
    if (rep.hasErrors()) {
        drainDiagnosticsToStderr(rep);
        return 1;
    }

    // ── D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET (a'): resolve every target's
    //    SOURCE LANGUAGE before a single CU is built ─────────────────────────
    //
    // Its own pass, for the same reason TF-C74 (a) hoisted target+format
    // resolution above: this loop can FAIL, and failing after some CUs are
    // already built means the abort throws away those CUs' parse diagnostics
    // (the drain that would have copied them is downstream). Resolving first
    // makes the failure clean — every unresolvable target named in one run, no
    // half-built front-end behind it, nothing dropped.
    //
    // Index-parallel to `targets`. The answer is carried to BOTH consumers: the
    // CU build (through `CuBuildKey::languageName`) and `compileOneTarget`
    // (through this vector).
    std::vector<std::shared_ptr<GrammarSchema const>> grammarPerTarget;
    grammarPerTarget.reserve(targets.size());
    {
        // Memoizes `loadShipped` across targets naming the SAME language, so a
        // 5-target build of one CPU family loads its dialect once.
        std::map<std::string, std::shared_ptr<GrammarSchema const>> grammarByName;
        for (auto const& spec : targets) {
            // Every spec parsed and every target loaded above (a failure
            // returned at the drain), so both lookups resolve.
            auto const parsedSpec = TargetSpec::parse(spec);
            auto const& targetSchema = *targetByName.at(parsedSpec->targetName);
            // NO `continue`-on-failure short-circuit: a build whose targets are
            // wrong in several ways must report every one of them in ONE run,
            // the same discipline the target/format pre-flight above follows.
            // `nullptr` keeps the vector index-parallel; the drain below is
            // what stops us using it.
            grammarPerTarget.push_back(resolveGrammarForTarget(
                explicitGrammar, targetSchema, sourceFiles, grammarByName, rep));
        }
    }
    if (rep.hasErrors()) {
        drainDiagnosticsToStderr(rep);
        return 1;
    }

    // c9 + TF-C74 (b): build the front-end ONCE PER DISTINCT `CuBuildKey`. A
    // language WITHOUT a preprocess pass produces identical CUs for every
    // target (both `activeFormat` and the target predefines are inert) → the
    // key is {"", nullopt, <language>} for every target → a single build, no
    // waste. The COMMON case — ONE target — is exactly one build, as before c9.
    // What CHANGES vs c9: two targets of DIFFERENT architecture sharing one
    // object format (arm64:elf… + x86_64:elf…) now build TWICE, because their
    // architecture predefines differ and reusing one CU for both would splice
    // the wrong architecture's macros into the second image.
    // D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET adds the second way two targets
    // can diverge: a DIFFERENT SOURCE LANGUAGE, which splits the key even when
    // there is no preprocess pass at all. Two targets that resolve the same
    // language still share one build, so a `.s` for two formats of ONE CPU is
    // still built once.
    std::map<CuBuildKey, std::vector<CompilationUnit>> cuByKey;
    std::vector<CuBuildKey> keyPerTarget;
    keyPerTarget.reserve(targets.size());
    for (std::size_t ti = 0; ti < targets.size(); ++ti) {
        std::string const& spec = targets[ti];
        CuBuildKey key;
        std::span<PredefinedMacroDef const> targetPredefines;
        std::span<PredefinedMacroDef const> formatPredefines;
        // ★★ KEYED UNCONDITIONALLY — see `CuBuildKey::languageName` for why
        // this must NOT join the `ppEnabled` block below.
        auto const& resolvedGrammar = grammarPerTarget[ti];
        key.languageName = std::string{resolvedGrammar->name()};
        bool const ppEnabled = resolvedGrammar->preprocess().enabled;
        if (ppEnabled) {
            key.format = formatKindOfSpec(spec);
            // The spec parsed cleanly above (we returned otherwise), so this
            // lookup always resolves.
            auto const parsed = TargetSpec::parse(spec);
            key.targetName    = parsed->targetName;
            key.formatName    = parsed->formatName;
            targetPredefines  = targetByName.at(key.targetName)->predefinedMacros();
            // TF-C97: the FORMAT's own predefines. `formatByName` was populated
            // by the pre-flight above and every surviving spec's format is in
            // it — a failed load already returned at the drain.
            formatPredefines  = formatByName.at(key.formatName)->predefinedMacros();
            // D-PP-HEADER-CASE-INSENSITIVE-PE: read the rule off the FORMAT
            // FILE (never derived from `key.format`, the KIND — that would be
            // the identity branch the agnosticism bar forbids).
            key.headerNameMatching =
                formatByName.at(key.formatName)->headerNameMatching();
        }
        keyPerTarget.push_back(key);
        if (cuByKey.find(key) == cuByKey.end()) {
            cuByKey.emplace(key,
                            buildCus(key, targetPredefines, formatPredefines,
                                     resolvedGrammar));
        }
    }

    // Drain each built CU's driver-tier + per-Tree diagnostics (D_FileNotFound,
    // parser/lexer errors) into the run-wide reporter — over EVERY distinct build,
    // since a per-format-kind build carries its own parse diagnostics. Without this
    // drain a missing source file produces rc=1 with ZERO stderr (the substrate
    // silent-failure archetype). Build the BufferRegistry (BufferId -> source
    // buffer) alongside so positioned rendering resolves each diagnostic's `buffer`
    // (plan 06 V2-4 Part A); `add` keys on the buffer's own id, idempotent on a
    // header shared across trees / builds.
    BufferRegistry bufs;
    for (auto const& kv : cuByKey) {
        for (auto const& cu : kv.second) {
            copyDiagnostics(cu.driverDiagnostics(), rep);
            for (auto const& tree : cu.trees()) {
                copyDiagnostics(tree.diagnostics(), rep);
                // Defense-in-depth: a driver-produced tree always has a non-null
                // source, but `BufferRegistry::add` THROWS on null — guard so a
                // future tree-producer (or a hand-built test tree) can't abort the
                // diagnostic drain.
                if (auto src = tree.sourceShared()) {
                    bufs.add(std::move(src));
                }
            }
            // FC13: register the CU's auxiliary buffers — the preprocessor's origin
            // buffers (the original main file + every quote-`#include`'d header) —
            // so a remapped header-origin diagnostic renders against its real
            // buffer. `BufferRegistry::add` is idempotent on duplicate ids.
            for (auto const& b : cu.auxiliaryBuffers()) {
                if (b) bufs.add(b);
            }
        }
    }
    // If parsing already failed, the per-target loop would only produce derivative noise.
    if (rep.hasErrors()) {
        drainDiagnosticsToStderr(rep, bufs);
        return 1;
    }

    int exitCode = 0;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        std::string const& spec = targets[i];
        // Route this target to the CUs built for its object-format-kind (c9).
        std::vector<CompilationUnit> const& cus = cuByKey.at(keyPerTarget[i]);
        // Per-target scratch reporter inheriting `rep`'s POLICY axes (suppress / overrides
        // / warningsAsErrors) but with the CAP/DEDUP axes RELAXED — those run-wide limits
        // are enforced once at `rep` during merge (silent-failure-hunter F9 / H1 fix; see
        // D-MERGE-POLICY-IDEMPOTENCY / D-MERGE-SCRATCH-FRESH / D-COMPILE-ONE-TARGET-NO-LEAK).
        auto scratchCfg = rep.config();
        scratchCfg.maxDiagnostics = std::numeric_limits<std::size_t>::max();
        scratchCfg.maxPerCode     = std::numeric_limits<std::size_t>::max();
        scratchCfg.dedupWindow    = 0;
        DiagnosticReporter scratch{scratchCfg};
        CompileOptions compileOpts{DiagnosticBudget{rep.config()}};
        compileOpts.config           = config;
        compileOpts.pipelineOverride = pipelineOverride;
        compileOpts.resolveLibraries = resolveLibraries;  // c162 (D-FF1-READER-CONSUMER)
        // D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET: the grammar THIS target's
        // CU was parsed under — never the invocation's, which no longer
        // exists as a single value.
        bool const ok = compileOneTarget(
            std::span<CompilationUnit const>{cus.data(), cus.size()},
            *grammarPerTarget[i], sourceStem, spec, scratch,
            outputDir, /*multiTargetBuild*/ targets.size() > 1u,
            artifactName, perFormatOutputSubdir, compileOpts,
            executor, jobsOverride, imageRequest);
        mergeWithTargetContext(scratch, spec, rep);
        if (!ok || scratch.hasErrors()) exitCode = 1;
    }

    drainDiagnosticsToStderr(rep, bufs);
    return exitCode;
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
    // D-LK10-ENTRY Slice C companion: route emitted binaries.
    setOutputDir(args.outputDir);
    setUserDefines(args.defines);  // c105: --define NAME[=VALUE] → the CU builds
    setIncludeDirs(args.includeDirs);  // -I<dir> quote-include path (SQLite-testfixture arc C3)
    // D-OPT1-PIPELINE-CONFIG-FROM-COMPILECONFIG: thread the CLI's
    // `--config=<debug|release>` into the kernel so the right
    // shipped pipeline gets loaded at compile_pipeline step 3.5.
    setCompileConfig(args.config);
    setJobs(args.jobs);  // D-PERF-4-CU-PARALLELISM: --jobs N per-CU build pool width
    // D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: `--stack-reserve <bytes>` — the
    // per-PROGRAM stack reserve the emitted image should carry. Stamped HERE,
    // before the dispatch fork, so it applies to EVERY compile-producing mode.
    // `compileProject` then applies a manifest `stackReserve` ONLY IF this
    // stamp left it unset — the CLI WINS (see there).
    setStackReserveBytes(args.stackReserveBytes);
    // c162 (D-FF1-READER-CONSUMER): thread `--resolve-library <path>` into the
    // kernel so compile_pipeline step 2.5 reads each named binary's export
    // surface to resolve + validate this run's externs. The parser already
    // produced `ResolveLibrarySpec`s (path + the OPTIONAL declared import
    // name, D-FFI-DECLARED-IMPORT-NAME), so this is a straight stamp — no
    // re-parse, and no layer in between can drop the declared name.
    setResolveLibraries(args.resolveLibraries);
    // `--time`: report the compilation's wall-clock to stderr when this run
    // returns — covers EVERY compile-producing mode (project / transpile /
    // directory / compile) via ONE scoped reporter, no per-mode duplication.
    // A zero-arg run never reaches here with time==true (parseCliArgs rejects
    // options without a mode → NoModeSelected), so the destructor only emits a
    // line for a real compile. Universal driver concern — lang/target/format-neutral.
    //
    // c97 (compile-time-performance arc): below the total, a per-phase
    // breakdown from the always-on `substrate::PhaseTimers` accumulators.
    // EVERY phase prints (zero-run phases included) so the report's shape is
    // deterministic and pin-able; the `runs` count disambiguates multi-CU /
    // multi-target accumulation (e.g. `parse ... (2 runs)` for a 2-TU build)
    // and makes the oracle-reparse multiplier visible as its own row. The
    // trailing `[other]` row is the wall total minus the attributed sum —
    // driver/config-load/IO time no phase claims. Phase names are pipeline
    // verbs (see compilePhaseName) — lang/target/format-neutral.
    struct WallTimeReporter {
        bool const                                  enabled;
        std::chrono::steady_clock::time_point const start;
        ~WallTimeReporter() {
            if (!enabled) return;
            auto const ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - start).count();
            auto const ms = ns / 1'000'000;
            std::cerr << "dss-code-prime: compile time " << formatWallTime(ms) << "\n";
            std::uint64_t attributedNs = 0;
            for (std::size_t i = 0; i < substrate::kCompilePhaseCount; ++i) {
                auto const p   = static_cast<substrate::CompilePhase>(i);
                auto const row = substrate::PhaseTimers::read(p);
                attributedNs += row.nanoseconds;
                std::cerr << "dss-code-prime:   phase "
                          << std::format("{:<16}", substrate::compilePhaseName(p))
                          << std::format("{:>12}", formatWallTime(
                                 static_cast<long long>(row.nanoseconds / 1'000'000u)))
                          << std::format("  ({} run{})", row.runs,
                                         row.runs == 1 ? "" : "s")
                          << "\n";
            }
            auto const otherNs = ns > 0 && static_cast<std::uint64_t>(ns) > attributedNs
                                     ? static_cast<std::uint64_t>(ns) - attributedNs
                                     : 0u;
            std::cerr << "dss-code-prime:   phase "
                      << std::format("{:<16}", "[other]")
                      << std::format("{:>12}", formatWallTime(
                             static_cast<long long>(otherNs / 1'000'000u)))
                      << "\n";
        }
    } const wallTimeReporter{args.time, std::chrono::steady_clock::now()};
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
        // Each `--compile` file is its OWN translation unit; more than one file links them
        // into a single image (gcc/clang `cc a.c b.c` semantics — separate TUs, the LINKER
        // resolves cross-file references: a sibling CU's definition shadows a library
        // import, per the cross-CU resolution chain LK11). A single file is the degenerate
        // 1-TU case, kept on the unchanged `compileFiles` path — `compileUnits` with N==1
        // is behaviorally identical (both funnel one CU through `runCusToTargets`), so the
        // 38 single-source examples are untouched. The unity-build (many files → ONE CU5
        // unit) is deliberately NOT a CLI surface: no language's file model concatenates
        // translation units, so there is no source-agnostic CLI spelling for it; a future
        // explicit opt-in flag can route to `compileFiles` when a real consumer needs it.
        // The route keys on translation-unit COUNT, not on any language / CPU /
        // format identity — the standing agnosticism veto is held. `routesToMultiUnit`
        // (program.hpp) is the single source of truth for the threshold, shared with
        // `compileProject` (plan 06 AP2) so the two dispatch sites never drift.
        return routesToMultiUnit(args.sourceFiles.size())
            ? compileUnits(args.sourceFiles, args.languageName, args.targets, cfg)
            : compileFiles(args.sourceFiles, args.languageName, args.targets, cfg);
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
    // Thin wrapper around the rep-injection overload (mirrors
    // `compileFiles`). `reporterConfig` threads `--warnings-as-errors`
    // + `--suppress=<code>` through every tier; the rep-taking overload
    // lets tests inspect the emitted code after return.
    DiagnosticReporter rep = buildReporter(reporterConfig);
    return compileProject(projectFilePath, rep);
}

int Program::compileProject(
    const std::string& projectFilePath,
    DiagnosticReporter&             rep
) {
    // Plan 06 artifactProfile gates: load the `.dss-project.json`, then
    // enforce the requested `artifactProfile` through TWO generic set-
    // membership gates before delegating to the existing compile path —
    // AP2 = the LANGUAGE gate (profile ∈ AP1's
    // `GrammarSchema::artifactProfiles()` → D_ArtifactProfileNotSupported);
    // AP3 = the per-target FORMAT gate (profile ∈ the object format's
    // served set → D_ArtifactProfileFormatMismatch). Threading the
    // resolved profile onward to CODEGEN (entry-symbol / subsystem /
    // extension) is deferred — NOT to a fixed AP slice but to the first
    // non-format-redundant profile consumer (e.g. gui), since no shipped
    // profile's codegen differs from what its (target:format) already
    // encodes; building it now would be a dead knob (D-AP2-COMPILATION-CONTEXT).
    auto pcOpt = loadProjectConfig(fs::path{projectFilePath}, rep);
    if (!pcOpt.has_value()) {
        // loadProjectConfig already emitted the structural diagnostic
        // (D_FileNotFound / C_MalformedJson / C_MissingField).
        drainDiagnosticsToStderr(rep);
        return 1;
    }
    ProjectConfig const& pc = *pcOpt;

    // ── `dependsOn`: DECLARED-BUT-UNRESOLVED IS A LOUD REJECT, NOT A NO-OP ──
    //
    // The loader parses and shape-validates `dependsOn` (path-xor-git, `ref`
    // only with `git`, no unknown members). RESOLUTION — walking to the named
    // manifest, acquiring a git remote into `.dss-deps/<name>`, cycle
    // detection, and merging the dependency's sources or linking its artifact —
    // is a separate lane that has NOT landed in this driver. The codes that
    // lane will emit are already allocated and documented (0xD019..0xD020 in
    // `core/types/parse_diagnostic.hpp`); NONE of them fits here, because every
    // one of them describes an OUTCOME of a resolution attempt ("that directory
    // holds no manifest", "the graph has a cycle", "git is not on PATH"), and
    // this is the prior condition: no attempt is made at all.
    //
    // ★ WHY THIS IS A HARD REJECT RATHER THAN AN IGNORED FIELD. Accept-and-
    // ignore is the single worst behaviour available: the manifest states a
    // prerequisite, the build reports success, and the artifact is missing
    // exactly the thing the author declared it needs. The failure then surfaces
    // as an undefined symbol at link — or, far worse, as a link against a STALE
    // copy of the dependency that happens to be lying around — with nothing
    // anywhere pointing back at the key that was silently dropped. Rejecting
    // the key outright costs the author one line of diff and tells them the
    // truth; ignoring it costs them a debugging session and tells them a lie.
    //
    // ★ WHY `D_PlanNotLanded` (0xD009) AND NOT A NEW CODE. Its allocation note
    // defines it as "an entry point reached an arm whose backing plan substrate
    // is not yet shipped ... Future plan-gated arms re-use this code", and
    // distinguishes it from `D_TargetAbiModelUnsupportedByDriver` (a PERMANENT
    // architectural exclusion) — a distinction `ffi/ingest.cpp` also turns on
    // when it declines to reuse this code. Dependency resolution is squarely
    // the pending-arrival case: the feature is coming, the surface is stable,
    // and only the engine behind it is absent. Minting a fourth "not landed
    // yet" code would say nothing 0xD009 does not already say, and would leave
    // a dead ordinal behind the day the lane lands. It is also a member of
    // `kUnsuppressableCodes`, which is load-bearing here: `--suppress` must not
    // be able to convert this loud reject back into the silent no-op it exists
    // to replace.
    //
    // Placed FIRST — ahead of the profile gates, the flag merges and (crucially)
    // the pre-build scripts — because a build that cannot happen must not have
    // side effects on the way to saying so: running the author's codegen hook
    // for a build we are about to refuse writes files into their tree for
    // nothing.
    if (!pc.dependsOn.empty()) {
        DependencyEntry const& first = pc.dependsOn.front();
        std::string const firstName =
            first.path.has_value()
                ? ("path '" + *first.path + "'")
                : (first.git.has_value() ? ("git '" + *first.git + "'")
                                         : std::string{"<unnamed>"});
        emitDriver(rep, DiagnosticCode::D_PlanNotLanded,
                   "project 'dependsOn': dependency RESOLUTION is not yet "
                   "implemented in this driver — the manifest declares "
                   + std::to_string(pc.dependsOn.size())
                   + " dependency entr" + (pc.dependsOn.size() == 1 ? "y" : "ies")
                   + " (first: " + firstName
                   + "), and the driver cannot resolve, acquire or build any of "
                     "them. The build is REFUSED rather than run without them: "
                     "compiling as if the key were absent would report success "
                     "for an artifact that is missing a declared prerequisite. "
                     "Remove the 'dependsOn' key (and build the prerequisite "
                     "separately, wiring it in via 'resolveLibraries' or an "
                     "explicit source entry) until dependency resolution lands.");
        drainDiagnosticsToStderr(rep);
        return 1;
    }

    // Load the language grammar to read its declared `artifactProfiles`.
    // The delegated `compileFiles`/`compileUnits` re-loads it by name;
    // the redundant load is benign for a project-build entry point and
    // keeps the delegate signatures unchanged (no pre-loaded-grammar
    // overload to add this cycle).
    auto grammarR = GrammarSchema::loadShipped(pc.language);
    if (!grammarR.has_value()) {
        forwardConfigDiagnostics(grammarR.error(), rep);
        emitDriver(rep, DiagnosticCode::D_SchemaLoadFailed,
                   "language schema '" + pc.language
                   + "' could not be loaded — the reason is in the configuration diagnostic(s) above (config: "
                     "src/dss-config/sources/" + pc.language
                   + ".lang.json).");
        drainDiagnosticsToStderr(rep);
        return 1;
    }
    auto grammar = *grammarR;
    // D-CONFIG-WARNINGS-DISCARDED-ON-SUCCESSFUL-LOAD (see the accessor).
    forwardConfigDiagnostics(grammar->loadDiagnostics(), rep);

    // AP2 driver gate: the requested profile must be ∈ the language's
    // declared set. Empty set ⇒ reject (fail-closed). One predicate, no
    // per-profile-name branch — the agnosticism veto holds.
    if (!enforceArtifactProfile(grammar->artifactProfiles(),
                                pc.artifactProfile, pc.language, rep)) {
        drainDiagnosticsToStderr(rep);
        return 1;
    }

    // AP3 driver gate: the requested profile must be SERVED by EACH target's
    // object format (`project.artifactProfile ∈ format.artifactProfiles()`).
    // Symmetric with the language gate above — the same generic predicate,
    // no per-profile-name / format-identity branch. A spec that doesn't
    // parse, or a format that doesn't load, is SKIPPED here (the delegated
    // compile emits the precise D_InvalidTargetSpec / D_SchemaLoadFailed —
    // no duplication); such a target still fails the whole build downstream,
    // so nothing slips past silently. The format is re-loaded by the build
    // (the same benign redundancy as the grammar above).
    for (auto const& spec : pc.targets) {
        auto parsed = TargetSpec::parse(spec);
        if (!parsed.has_value()) continue;           // delegate → D_InvalidTargetSpec
        auto fmtR = ObjectFormatSchema::loadShipped(parsed->formatName);
        if (!fmtR.has_value()) continue;             // delegate → D_SchemaLoadFailed
        if (!enforceArtifactProfileFormat((*fmtR)->artifactProfiles(),
                                          pc.artifactProfile,
                                          parsed->formatName, rep)) {
            drainDiagnosticsToStderr(rep);
            return 1;
        }
    }

    // Thread the manifest's OPTIONAL compile-flag arrays onto the SAME
    // Program state the CLI stamps in `Program::run` (setIncludeDirs `-I` /
    // setUserDefines `--define` / setResolveLibraries `--resolve-library`),
    // which `compileFiles`/`compileUnits` read at CU-build time. MERGE
    // (append), not replace: `Program::run` may already have stamped the CLI
    // flags before dispatching here, so the manifest ADDS to them (the two
    // sources compose) rather than clobbering them. `resolveLibraries` entries
    // are strings mapped to filesystem paths exactly as the CLI stamp does.
    // Empty arrays (absent field, or a present `[]`) append nothing — a no-op.
    //
    // Contract: this APPENDS onto the PERSISTENT Program state (the setters
    // mutate the members read at build time), matching `Program::run`'s
    // single-use CLI-stamp contract — a Program is built fresh per invocation
    // (`Program::run` constructs one, tests construct one per call), so the
    // append runs exactly once. A REUSED Program passed through `compileProject`
    // twice would double-append; that is out of contract (single-use), not a
    // supported reuse mode.
    {
        std::vector<std::string> mergedIncludes = includeDirs();
        mergedIncludes.reserve(mergedIncludes.size() + pc.includes.size());
        mergedIncludes.insert(mergedIncludes.end(),
                              pc.includes.begin(), pc.includes.end());
        setIncludeDirs(std::move(mergedIncludes));

        std::vector<std::string> mergedDefines = userDefines();
        mergedDefines.reserve(mergedDefines.size() + pc.defines.size());
        mergedDefines.insert(mergedDefines.end(),
                             pc.defines.begin(), pc.defines.end());
        setUserDefines(std::move(mergedDefines));

        // D-FFI-DECLARED-IMPORT-NAME: the manifest parses into the SAME
        // `ResolveLibrarySpec` the CLI does, so the merge is a plain append —
        // a manifest entry's declared import name survives the join with the
        // CLI-stamped entries instead of being flattened back to a bare path.
        std::vector<ResolveLibrarySpec> mergedLibs = resolveLibraries();
        mergedLibs.reserve(mergedLibs.size() + pc.resolveLibraries.size());
        mergedLibs.insert(mergedLibs.end(),
                          pc.resolveLibraries.begin(), pc.resolveLibraries.end());
        setResolveLibraries(std::move(mergedLibs));
    }

    // D-AP2-OUTPUT-ROUTING (artifactName + per-platform subdir): stamp the
    // manifest's OPTIONAL `artifactName` (nullopt ⇒ the source stem names the
    // binary — unchanged) and FORCE the per-format subdir for EVERY project
    // build. Multi-target builds already subdir by formatName; setting this
    // makes a SINGLE-target project build subdir the same way, so a project's
    // output is consistently `<outputDir>/<formatName>/<artifactName-or-stem>
    // <ext>` per platform. The CLI path (`Program::run`) sets NEITHER, so a
    // `--compile` build's names + single-target flat layout stay byte-identical.
    setArtifactName(pc.artifactName);
    setPerFormatOutputSubdir(true);

    // D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: the manifest's OPTIONAL
    // `stackReserve` (bytes).
    //
    // PRECEDENCE — the CLI `--stack-reserve` WINS; the manifest value applies
    // ONLY IF `Program::run` left the stamp unset. The three flag ARRAYS above
    // MERGE because appending composes: `-I a` + manifest `["b"]` sensibly
    // means both. A SCALAR cannot compose — one of the two numbers must be the
    // answer — so it needs an explicit rule, and "an explicit command-line
    // argument overrides a committed configuration file" is the universal one
    // (cmake, cargo, gcc). It is also the only rule that lets a user probe a
    // different reserve (bisecting a stack overflow is exactly the motivating
    // workflow) without editing — and risking committing — the manifest.
    //
    // NOTE this reads the member rather than clobbering it, so the CLI stamp
    // survives; the manifest never overwrites a supplied flag.
    if (!stackReserveBytes().has_value() && pc.stackReserveBytes.has_value()) {
        setStackReserveBytes(pc.stackReserveBytes);
    }

    // ── PRE-BUILD HOOKS — AND THE ONE THING THAT FIXES THEIR POSITION ──────
    //
    // `runBuildScripts` spawns every APPLICABLE `preBuildScripts` entry in
    // manifest order, stopping at the first failure, and emits exactly one
    // remediation-distinct diagnostic when it does (`D_ScriptSpawnFailed` if the
    // OS never created the process, `D_ScriptExitedNonZero` if it ran and said
    // no). `false` means abandon the build — the `[[nodiscard]]` on the
    // declaration exists so that abandoning cannot be forgotten, since a
    // forgotten check is precisely a green compile of the STALE sources a failed
    // codegen step did not regenerate.
    //
    // ★ THIS MUST SIT ABOVE THE GLOB EXPANSION, AND THAT ORDER IS THE FEATURE.
    // The overwhelmingly common reason to declare a pre-build hook is to
    // GENERATE sources — a parser generator, a `*.in` substitution, a version
    // stamp — and the manifest then names those outputs with a pattern like
    // `"generated/*.c"`. Expansion is deliberately FAIL-LOUD on zero matches
    // (see the block below), so running the hooks after it would make the very
    // case the feature exists for the one case it cannot serve: the glob would
    // match nothing, the build would die naming a pattern that was about to
    // become correct, and the hook would never run at all. Anything that moves
    // this call below the expansion breaks generated-source projects outright;
    // `tests/program/test_project_config.cpp`'s
    // `PreBuildScriptGeneratesSourceThatCompilesAndRuns` is the pin that
    // catches it, and it catches it by RUNNING the produced binary.
    //
    // CWD — the child starts in the PROCESS working directory, which is the
    // same base a relative `sources[]` entry and every relative glob already
    // resolve against (see the expansion block below and
    // `docs/project-config-spec.md`). A hook that writes `generated/main.c` and
    // a manifest that reads `generated/*.c` must mean the same directory, or the
    // feature does not compose with itself. The empty path IS that instruction:
    // `substrate::spawnAndWaitInherit` documents `cwd` empty as "inherit the
    // caller's current directory". It is passed as the sentinel rather than a
    // materialized `fs::current_path()` on purpose — materializing introduces a
    // failure mode (a deleted cwd) whose only honest handling is a diagnostic
    // for a condition under which nothing else in this function works either,
    // and it would let the two answers drift apart. (A DEPENDENCY manifest's
    // hooks must run in THAT dependency's directory instead — which is why
    // `runBuildScripts` takes `cwd` as a parameter at all; that caller does not
    // exist yet, see the `dependsOn` reject above.)
    if (!runBuildScripts(pc.preBuildScripts, fs::path{}, rep)) {
        drainDiagnosticsToStderr(rep);
        return 1;
    }

    // D-AP2-SOURCES-GLOB: expand any glob pattern in `sources[]` into its
    // matching files BEFORE the multi-vs-single-CU routing count is taken — so a
    // `"src/**/*.c"` entry routes EXACTLY as if its matches had been listed
    // literally (a 2-match glob ⇒ 2 concrete sources ⇒ count 2 ⇒ `compileUnits`).
    // A driver pre-pass, NOT the loader: `parseProjectConfig` stays a pure JSON
    // parser (it holds the raw pattern), the filesystem side lives here.
    //   * LITERAL entry (no `* ? [` metacharacter) — kept VERBATIM (unchanged
    //     behavior; a missing literal still fails DOWNSTREAM at CU build, and is
    //     NOT newly rejected here).
    //   * GLOB entry — expanded against the filesystem (base = the process
    //     working directory, the same base a literal source uses; an absolute
    //     pattern resolves directly). Matches are sorted (deterministic CU order).
    //     ZERO matches is a FAIL-LOUD error (`D_FileNotFound` naming the pattern)
    //     — a source pattern that names nothing is a mistake, not an empty no-op.
    //     A mid-expansion filesystem I/O error fails loud (`D_DirectoryScanFailed`).
    // The delegate then drains `rep` (runCusToTargets), so these early fail-loud
    // sites drain here and return, mirroring the gate sites above.
    std::vector<std::string> expandedSources;
    expandedSources.reserve(pc.sources.size());
    for (auto const& entry : pc.sources) {
        if (!hasGlobMetacharacters(entry)) {
            expandedSources.push_back(entry);  // literal — verbatim, unchanged
            continue;
        }
        std::error_code ec;
        std::size_t const before = expandedSources.size();
        if (!expandGlob(entry, expandedSources, ec)) {
            emitDriver(rep, DiagnosticCode::D_DirectoryScanFailed,
                       "project sources: filesystem error expanding glob pattern '"
                       + entry + "': " + ec.message());
            drainDiagnosticsToStderr(rep);
            return 1;
        }
        if (expandedSources.size() == before) {
            emitDriver(rep, DiagnosticCode::D_FileNotFound,
                       "project sources: glob pattern '" + entry
                       + "' matched no files (relative to the working directory) "
                         "— a source pattern that matches nothing is an error; "
                         "check the pattern and that the files exist.");
            drainDiagnosticsToStderr(rep);
            return 1;
        }
    }

    // Cross-entry de-duplication: a file matched by TWO overlapping entries — two
    // overlapping globs (`src/*.c` + `src/**/*.c`), or a literal alongside a glob
    // that also matches it — must compile ONCE, not once per entry. Without this,
    // `compileUnits` would build a DUPLICATE CU per repeat ⇒ a duplicate-symbol
    // LINK error the diagnostic can't tie back to the manifest. A redundant
    // overlap should just work — the UNION of unique files, each compiled once —
    // matching build-system expectations. Dedup on a NORMALIZED key
    // (`lexically_normal().generic_string()`) because a literal is kept verbatim
    // (`./main.c`) while glob output is already normalized (`main.c`) — the SAME
    // file via different strings, which a plain string-dedup would miss. FIRST
    // occurrence wins (deterministic order) and keeps its ORIGINAL string (a
    // literal stays verbatim; only later duplicates are dropped). Absolute-vs-
    // relative spellings of the same file is an accepted un-caught extreme edge —
    // `lexically_normal` covers the realistic `./` / `..` / literal-vs-glob cases.
    {
        std::vector<std::string> deduped;
        deduped.reserve(expandedSources.size());
        std::unordered_set<std::string> seen;
        seen.reserve(expandedSources.size());
        for (auto& s : expandedSources) {
            std::string key = fs::path{s}.lexically_normal().generic_string();
            if (seen.insert(std::move(key)).second) {
                deduped.push_back(std::move(s));
            }
        }
        expandedSources = std::move(deduped);
    }

    // Route by the EXPANDED source COUNT via the shared `routesToMultiUnit`
    // threshold (identical to the CLI dispatcher): >1 source ⇒ N independent CUs
    // the linker merges (`compileUnits`, `cc a.c b.c` semantics); ≤1 ⇒ the
    // single-CU path (`compileFiles`). The delegate validates each
    // `<targetName>:<formatName>` spec (D_InvalidTargetSpec) and drains `rep` at
    // its end (runCusToTargets), so we do NOT drain here.
    //
    // The delegate's result is CAPTURED rather than returned directly — the
    // post-build hooks below need to know how the build went, and they need to
    // run after it.
    int const rc = routesToMultiUnit(expandedSources.size())
        ? compileUnits(expandedSources, pc.language, pc.targets, rep)
        : compileFiles(expandedSources, pc.language, pc.targets, rep);

    // ── POST-BUILD HOOKS — GATED ON A SUCCESSFUL COMPILE, AND ASYMMETRIC ────
    //
    // TWO decisions live here, and they point in opposite directions on
    // purpose.
    //
    // (1) THE HOOKS RUN ONLY WHEN `rc == 0`. A post-build step packages,
    // signs, copies, installs or deploys the artifact — every one of those is
    // an operation ON a thing the failed build did not produce, or worse, on a
    // STALE copy of it left behind by an earlier successful run. Running them
    // after a failed compile therefore either fails with a confusing second
    // error that buries the real one, or SUCCEEDS at shipping yesterday's
    // binary. This is also what every build system the author already knows
    // does: `make` stops the recipe at the first failing command, cargo's
    // post-build work never runs for a failed crate, and a CMake
    // `POST_BUILD` custom command is attached to a target that did not build.
    //
    // (2) A POST-BUILD FAILURE STILL FAILS THE WHOLE BUILD (`rc = 1`) EVEN
    // THOUGH THE ARTIFACT EXISTS ON DISK. The hook is part of what the author
    // declared "building this project" to mean; a packaging step that failed
    // means the project is not built, and reporting exit 0 with a diagnostic on
    // stderr invites every CI system on earth to proceed to the next stage. The
    // artifact deliberately stays where it is rather than being deleted — the
    // compile genuinely succeeded, the bytes are genuinely correct, and
    // unwinding a successful compile because a deploy script's credentials
    // expired would destroy work the operator can otherwise re-use after fixing
    // the hook. So: artifact present, exit non-zero, one diagnostic naming the
    // script. (`tests/program/test_project_config.cpp`'s
    // `PostBuildScriptFailureFailsBuildButKeepsArtifact` asserts BOTH halves —
    // the existence check is what proves the compile really did run first.)
    //
    // Same cwd sentinel and the same fail-loud codes as the pre-build call.
    //
    // `rep` was already drained by the delegate on its way out, and the drain
    // helper RENDERS rather than CONSUMES — so the post-build diagnostic is
    // drained FROM A MARK taken before the hooks run. Draining unqualified
    // here would reprint every diagnostic the successful compile produced
    // (warnings and notes survive `rc == 0`) and leave the single line naming
    // the failed hook at the bottom of a duplicated dump. The mark is taken
    // outside the `&&` because `runBuildScripts` is what appends to `rep`.
    std::size_t const preHookDiagnostics = rep.all().size();
    if (rc == 0 && !runBuildScripts(pc.postBuildScripts, fs::path{}, rep)) {
        drainDiagnosticsToStderr(rep, preHookDiagnostics);
        return 1;
    }
    return rc;
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
    DiagnosticReporter rep = buildReporter(reporterConfig);
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

// FF11: declare the language's SYSTEM include dirs (its
// `semantics.shippedLibDirs`, the /usr/include analogue) on `builder`
// so the angle form `#include <h>` resolves against them. Each config
// string is a subdirectory under `src/dss-config/`; `findShippedConfigDir`
// resolves it with the SHARED precedence — `$DSS_CONFIG_ROOT` first, then
// the 8-level cwd walk — so it works from repo root, build/, a nested
// ctest cwd, or anywhere at all when the override is set. A dir that
// resolves NOWHERE is skipped — a header miss then hard-fails downstream
// with F_ShippedHeaderNotFound, which is the correct fail-loud surface
// (vs. silently swallowing here). NO language branch: the dirs come
// entirely from the schema's per-language config.
//
// ⚠ THIS USED TO BE A PRIVATE COPY OF THE WALK THAT NEVER READ THE
// OVERRIDE, and the comment claimed it mirrored `findShippedConfig` while
// omitting the one branch that makes discovery cwd-independent. MEASURED:
// same binary, `DSS_CONFIG_ROOT` set in BOTH arms, `#include <stdio.h>` —
// cwd inside the repo gave rc 0, cwd `C:\` gave `error[F001A]: got
// stdio.h`. The shipped CLI could not resolve an angle include from any
// working directory outside its own source tree. Do not re-open a local
// walk here.
void applySystemDirs(UnitBuilder& builder, GrammarSchema const& grammar) {
    auto const& dirs = grammar.semantics().shippedLibDirs;
    if (dirs.empty()) return;
    std::error_code ec;
    for (std::string const& sub : dirs) {
        auto const resolved = findShippedConfigDir(sub);
        if (!resolved) continue;   // fail loud downstream, not here
        // ABSOLUTE, because `ResolutionContext::systemDirs` documents its
        // dirs as absolute: the cwd walk produced that for free, but a
        // RELATIVE `DSS_CONFIG_ROOT` (permitted — see config_path_walk.hpp)
        // would not. Same idiom as `applyIncludeDirs` below: on an `absolute`
        // failure keep the raw path rather than drop the dir.
        fs::path const abs = fs::absolute(*resolved, ec);
        builder.addSystemDir(ec ? *resolved : abs);
        ec.clear();
    }
}

// SQLite-testfixture arc C3: thread the CLI `-I` dirs (the C quote-include
// search path, `Options::includeDirs`) onto `builder` via `addIncludeDir`. Each
// is resolved to an ABSOLUTE path (a relative dir like `.` is cwd-relative, gcc
// semantics) so the include resolver's `dir / filename` probe is stable
// regardless of any later cwd change; the resolver searches these AFTER the
// including file's own directory (C 6.10.2, the quote form). A nonexistent dir
// is added anyway (gcc parity — it simply yields no hits, and a genuinely-
// missing header still fails loud P0016 downstream). Distinct from
// `applySystemDirs` (the angle-form `/usr/include` analogue via `addSystemDir`).
// AGNOSTIC: pure path plumbing — no language/target/format branch.
void applyIncludeDirs(UnitBuilder& builder,
                      std::vector<std::string> const& dirs) {
    std::error_code ec;
    for (std::string const& d : dirs) {
        fs::path const abs = fs::absolute(fs::path{d}, ec);
        builder.addIncludeDir(ec ? fs::path{d} : abs);
        ec.clear();
    }
}

int Program::compileFiles(
    const std::vector<std::string>& sourceFiles,
    const std::string& languageName,
    const std::vector<std::string>& targets,
    DiagnosticReporter::Config const& reporterConfig
) {
    // Thin wrapper around the rep-injection overload (closes
    // D-CAP-MARKER-MULTI-TARGET-E2E-PIN). Existing CLI / Python
    // call sites that pass `Config` (or use the default-arg)
    // continue unchanged.
    DiagnosticReporter rep = buildReporter(reporterConfig);
    return compileFiles(sourceFiles, languageName, targets, rep);
}

int Program::compileFiles(
    const std::vector<std::string>& sourceFiles,
    const std::string& languageName,
    const std::vector<std::string>& targets,
    DiagnosticReporter&             rep
) {
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

    // Load the language schema once for the whole call — WHEN THE CALLER NAMED
    // ONE. D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET: an EMPTY `languageName` is
    // now legal and means "ask each target", which is the only way one
    // invocation can compile a `.s` for two different CPUs (the two dialects
    // both claim `.s`, so the extension alone cannot answer). A named language
    // still wins outright — see `resolveGrammarForTarget`.
    std::shared_ptr<GrammarSchema const> grammar;
    if (!languageName.empty()) {
        auto grammarR = GrammarSchema::loadShipped(languageName);
        if (!grammarR.has_value()) {
            forwardConfigDiagnostics(grammarR.error(), rep);
            emitDriver(rep, DiagnosticCode::D_SchemaLoadFailed,
                       "language schema '" + languageName
                       + "' could not be loaded — the reason is in the configuration diagnostic(s) above (config: "
                         "src/dss-config/sources/" + languageName
                       + ".lang.json).");
            drainDiagnosticsToStderr(rep);
            return 1;
        }
        grammar = *grammarR;
        // D-CONFIG-WARNINGS-DISCARDED-ON-SUCCESSFUL-LOAD (see the accessor).
        forwardConfigDiagnostics(grammar->loadDiagnostics(), rep);
    }

    // Build ONE CompilationUnit for ALL source files — the CU5 multi-file-single-CU shape
    // (cross-file references resolved WITHIN the CU; each file routes to the language's
    // schema by extension). For one CU per file (a multi-CU image the linker MERGES, LK11)
    // use `compileUnits` — this entry's single-CU semantics are unchanged.
    // D-PARSE-DEEP-FRONTEND-STACK: the CU build (preprocess + parse + the
    // type-name oracle reparse) recurses over the expression tree on the
    // thread stack (the parser's residual paren/postfix arm), so a deeply-
    // nested-but-legal expression that the config-driven parser cap
    // (`parser.maxExpressionDepth`, c-subset = 1024) admits would overflow the
    // host's ~1 MB main stack here — symmetric to the downstream `analyze`
    // overflow. Run it on the same 64 MiB worker stack (synchronous join) so
    // parse and analysis are BOTH deep-safe and the cap is a real semantic
    // limit end-to-end.
    // c9 (Phase-2): the front-end build is a closure invoked once per distinct
    // object-format-kind by `runCusToTargets`, so `__has_include` is per-target
    // truthful. `setActiveFormat(kind)` is the only addition vs the pre-c9 build;
    // `kind` is nullopt for a non-preprocess language / undeterminable spec
    // (pure-existence, unchanged). The build still runs on the 64 MiB worker stack
    // (D-PARSE-DEEP-FRONTEND-STACK).
    auto buildCus = [&](CuBuildKey const&                   key,
                        std::span<PredefinedMacroDef const> targetPredefines,
                        std::span<PredefinedMacroDef const> formatPredefines,
                        // D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET: the grammar
                        // THIS key resolved to. Never the enclosing
                        // `grammar` — that is null on the ask-the-target path.
                        std::shared_ptr<GrammarSchema const> const& keyGrammar)
        -> std::vector<CompilationUnit> {
        auto cu = substrate::callOnLargeStack(
            substrate::kDeepRecursionStackBytes, [&] {
                // D-DIAG-VOLUME-CAP-ENFORCED-AT-SIX-STAGES-NOT-ONCE: the
                // operator's `--max-diagnostics` value crosses into the front
                // end HERE, and `rep` is its only holder at this point (the
                // Config overload is a thin wrapper that builds `rep` and
                // forwards). Taken from `rep`, never from a per-target scratch
                // -- those deliberately relax the volume axes to SIZE_MAX for
                // the merge, so a budget derived from one would hand the front
                // end no bound at all.
                UnitBuilder builder{keyGrammar, DiagnosticBudget{rep.config()}};
                applySystemDirs(builder, *keyGrammar);
                if (key.format) builder.setActiveFormat(*key.format);
                // D-PP-HEADER-CASE-INSENSITIVE-PE: the format FILE's own
                // header-name case rule (NOT derived from the format kind).
                builder.setHeaderNameMatching(key.headerNameMatching);
                // TF-C74: the active target's per-architecture identity macros.
                builder.setTargetPredefinedMacros(
                    {targetPredefines.begin(), targetPredefines.end()});
                // TF-C97: the active format's data-model macros.
                builder.setFormatPredefinedMacros(
                    {formatPredefines.begin(), formatPredefines.end()});
                builder.setUserDefines(userDefines());  // c105: --define
                applyIncludeDirs(builder, includeDirs());  // -I<dir> (arc C3)
                for (auto const& path : sourceFiles) {
                    builder.addFile(fs::path{path});
                }
                return std::move(builder).finish();
            });
        std::vector<CompilationUnit> v;
        v.push_back(std::move(cu));
        return v;
    };

    // Stem of the first source file names the artifact (one artifact per target). Plan 06
    // will eventually let artifact profiles override this.
    std::string const sourceStem = fs::path{sourceFiles.front()}.stem().string();
    return runCusToTargets(
        buildCus, grammar, sourceFiles, sourceStem, targets, rep,
        outputDir_, artifactName_, perFormatOutputSubdir_, compileConfig_,
        optimizerPipelineOverride_.has_value() ? &*optimizerPipelineOverride_ : nullptr,
        resolveLibraries_, executor_, jobs_,
        // D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: the CLI/manifest stack-reserve
        // request (nullopt = the format default stands).
        ImageRequest{stackReserveBytes_});
}

int Program::compileUnits(
    const std::vector<std::string>& sourceFiles,
    const std::string& languageName,
    const std::vector<std::string>& targets,
    DiagnosticReporter::Config const& reporterConfig
) {
    DiagnosticReporter rep = buildReporter(reporterConfig);
    return compileUnits(sourceFiles, languageName, targets, rep);
}

int Program::compileUnits(
    const std::vector<std::string>& sourceFiles,
    const std::string& languageName,
    const std::vector<std::string>& targets,
    DiagnosticReporter&             rep
) {
    if (sourceFiles.empty()) {
        emitDriver(rep, DiagnosticCode::D_EmptyInput,
                   "compileUnits: source file list is empty.");
        drainDiagnosticsToStderr(rep);
        return 1;
    }
    if (targets.empty()) {
        emitDriver(rep, DiagnosticCode::D_InvalidTargetSpec,
                   "compileUnits: targets list is empty — at least one "
                   "'<targetName>:<formatName>' entry required.");
        drainDiagnosticsToStderr(rep);
        return 1;
    }

    // D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET: empty ⇒ ask each target (see
    // the sibling note in `compileFiles`).
    std::shared_ptr<GrammarSchema const> grammar;
    if (!languageName.empty()) {
        auto grammarR = GrammarSchema::loadShipped(languageName);
        if (!grammarR.has_value()) {
            forwardConfigDiagnostics(grammarR.error(), rep);
            emitDriver(rep, DiagnosticCode::D_SchemaLoadFailed,
                       "language schema '" + languageName
                       + "' could not be loaded — the reason is in the configuration diagnostic(s) above (config: "
                         "src/dss-config/sources/" + languageName
                       + ".lang.json).");
            drainDiagnosticsToStderr(rep);
            return 1;
        }
        grammar = *grammarR;
        // D-CONFIG-WARNINGS-DISCARDED-ON-SUCCESSFUL-LOAD (see the accessor).
        forwardConfigDiagnostics(grammar->loadDiagnostics(), rep);
    }

    // Build ONE CompilationUnit PER source file — the multi-CU model the linker MERGES
    // into one image (CU6 + LK11). Distinct from `compileFiles` (one CU5 multi-file CU);
    // here cross-file references are resolved at LINK time (a sibling CU's definition or a
    // library import), not within a single CU.
    // c9 (Phase-2): build N single-file CUs once per distinct object-format-kind
    // (the closure `runCusToTargets` invokes), so `__has_include` is per-target
    // truthful. `setActiveFormat(kind)` is the only addition vs the pre-c9 build.
    auto buildCus = [&](CuBuildKey const&                   key,
                        std::span<PredefinedMacroDef const> targetPredefines,
                        std::span<PredefinedMacroDef const> formatPredefines,
                        // D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET: this key's
                        // resolved grammar (see the `compileFiles` twin).
                        std::shared_ptr<GrammarSchema const> const& keyGrammar)
        -> std::vector<CompilationUnit> {
        std::vector<CompilationUnit> cus;
        cus.reserve(sourceFiles.size());
        for (auto const& path : sourceFiles) {
            // D-PARSE-DEEP-FRONTEND-STACK: build each CU on the 64 MiB worker
            // stack (see compileFiles for the rationale) so a deeply-nested
            // expression parses without overflowing the host main stack.
            cus.push_back(substrate::callOnLargeStack(
                substrate::kDeepRecursionStackBytes, [&] {
                    // Same budget hop as `compileFiles` above
                    // (D-DIAG-VOLUME-CAP-ENFORCED-AT-SIX-STAGES-NOT-ONCE).
                    UnitBuilder builder{keyGrammar, DiagnosticBudget{rep.config()}};
                    applySystemDirs(builder, *keyGrammar);
                    if (key.format) builder.setActiveFormat(*key.format);
                    // D-PP-HEADER-CASE-INSENSITIVE-PE: the format FILE's own
                    // header-name case rule (NOT derived from the format kind).
                    builder.setHeaderNameMatching(key.headerNameMatching);
                    // TF-C74: the active target's per-architecture identity macros.
                    builder.setTargetPredefinedMacros(
                        {targetPredefines.begin(), targetPredefines.end()});
                    // TF-C97: the active format's data-model macros.
                    builder.setFormatPredefinedMacros(
                        {formatPredefines.begin(), formatPredefines.end()});
                    builder.setUserDefines(userDefines());  // c105: --define
                    applyIncludeDirs(builder, includeDirs());  // -I<dir> (arc C3)
                    builder.addFile(fs::path{path});
                    return std::move(builder).finish();
                }));
        }
        return cus;
    };

    std::string const sourceStem = fs::path{sourceFiles.front()}.stem().string();
    return runCusToTargets(
        buildCus, grammar, sourceFiles, sourceStem,
        targets, rep, outputDir_, artifactName_, perFormatOutputSubdir_,
        compileConfig_,
        optimizerPipelineOverride_.has_value() ? &*optimizerPipelineOverride_ : nullptr,
        resolveLibraries_, executor_, jobs_,
        // D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: the CLI/manifest stack-reserve
        // request (nullopt = the format default stands).
        ImageRequest{stackReserveBytes_});
}

// D-CAP-MARKER-COMPILE-DIR-PIN anchor: compileDirectory has NO
// rep-injection overload (intentional asymmetry — this entry point
// is CLI-only; tests reach the cap-marker contract via
// `compileFiles(..., DiagnosticReporter&)`). Trigger to add a
// parallel `compileDirectory(..., DiagnosticReporter&)` overload:
// first test or Python FFI consumer that needs post-run reporter
// inspection on the directory-scan path.
int Program::compileDirectory(
    const std::string& directoryPath,
    const std::string& languageName,
    const std::vector<std::string>& targets,
    InputResolver::Mode mode,
    DiagnosticReporter::Config const& reporterConfig
) {
    DiagnosticReporter rep = buildReporter(reporterConfig);

    // ── Which extensions does the scan collect? ───────────────────────────
    //
    // D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET. The scan runs ONCE, before any
    // target loop, so its filter cannot be per-target — but it must not be
    // narrower than the union of what the per-target languages will accept, or
    // a `.s` sitting in the directory is skipped SILENTLY (a scan filter drops
    // files without a word, which is exactly why the widening lives here and
    // not in the resolver).
    //
    //   • `--language X` named  ⇒ X's extensions. UNCHANGED, deliberately: the
    //     caller named one language, so a mixed C+assembly directory would
    //     have to build a mixed CU, and one CU carries one grammar to codegen.
    //     Widening here would trade a silent skip for a silent mis-parse.
    //   • `--language` omitted ⇒ the UNION over the named targets of each
    //     target's declared assembly language. That is the set of files this
    //     invocation can actually resolve, and a file whose extension only
    //     SOME target claims still fails loud, per target, downstream.
    std::vector<std::string> scanExtensions;
    std::shared_ptr<GrammarSchema const> grammar;
    if (!languageName.empty()) {
        auto grammarR = GrammarSchema::loadShipped(languageName);
        if (!grammarR.has_value()) {
            forwardConfigDiagnostics(grammarR.error(), rep);
            emitDriver(rep, DiagnosticCode::D_SchemaLoadFailed,
                       "compileDirectory: language schema '" + languageName
                       + "' could not be loaded.");
            drainDiagnosticsToStderr(rep);
            return 1;
        }
        grammar = *grammarR;
        // D-CONFIG-WARNINGS-DISCARDED-ON-SUCCESSFUL-LOAD (see the accessor).
        forwardConfigDiagnostics(grammar->loadDiagnostics(), rep);
        for (auto const& ext : grammar->fileExtensions()) {
            scanExtensions.push_back(ext);
        }
    } else {
        std::set<std::string> declaredNames;
        for (auto const& spec : targets) {
            auto parsed = TargetSpec::parse(spec);
            if (!parsed) continue;  // the delegate emits D_InvalidTargetSpec
            auto targetR = TargetSchema::loadShipped(parsed->targetName);
            if (!targetR.has_value()) continue;  // delegate → D_SchemaLoadFailed
            std::string name{(*targetR)->defaultAssemblyLanguage()};
            // An undeclared dialect contributes NOTHING to the filter and is
            // NOT diagnosed here: the delegate's `resolveGrammarForTarget`
            // owns that message (it names the target), and duplicating it
            // would make one misconfiguration print twice with two wordings.
            if (!name.empty()) declaredNames.insert(std::move(name));
        }
        for (auto const& name : declaredNames) {
            auto loaded = GrammarSchema::loadShipped(name);
            if (!loaded.has_value()) continue;  // delegate → D_SchemaLoadFailed
            // ⚠ AND ITS `loadDiagnostics()` ARE DELIBERATELY NOT FORWARDED
            // HERE, for the same reason the failure above is not reported here:
            // this loop only harvests file extensions for the directory scan,
            // and `resolveGrammarForTarget` loads the very same document again
            // per target and forwards there. Doing it in both places prints
            // every config warning twice for one misconfiguration.
            for (auto const& ext : (*loaded)->fileExtensions()) {
                scanExtensions.push_back(ext);
            }
        }
        if (scanExtensions.empty()) {
            emitDriver(rep, DiagnosticCode::D_UnknownFileExtension,
                       "compileDirectory: no --language was given and no named "
                       "target declares a 'defaultAssemblyLanguage', so the "
                       "directory scan has no file extensions to collect — it "
                       "would silently find nothing. Pass --language <name>.");
            drainDiagnosticsToStderr(rep);
            return 1;
        }
    }

    // Resolve files via the hoisted `InputResolver` (D-LK10-1
    // closure — landed at LK10 cycle 3). The recursive vs flat
    // policy axis is now an explicit caller parameter, mirroring
    // plan 00 §4.1.3's spec.
    std::vector<std::string> sourceFiles;
    if (!InputResolver::resolveDirectory(
            fs::path{directoryPath}, scanExtensions,
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
