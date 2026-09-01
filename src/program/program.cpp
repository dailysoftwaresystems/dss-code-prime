#include "program/program.hpp"

#include "core/crypto/sha256.hpp"  // the ONE content digest — the artifact cache's input closure
#include "core/substrate/path_identity.hpp"

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/preprocess/preprocessor.hpp"   // PreScanMemoCounters, for the --time report
#include "core/substrate/large_stack_call.hpp"  // D-PARSE-DEEP-FRONTEND-STACK: build CUs on a large stack
#include "core/substrate/phase_timers.hpp"      // c97: --time per-phase breakdown
#include "core/substrate/thread_pool.hpp"       // D-PERF-4-CU-PARALLELISM: per-CU build pool
#include "core/types/config_path_walk.hpp"      // findShippedConfigDir — shared src/dss-config/<dir> resolver
#include "core/types/diagnostic_reporter.hpp"
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
#include "program/cross_validate_language_target.hpp"
#include "program/cross_validate_target_format.hpp"
#include "program/dependency_resolver.hpp"  // AP6: `dependsOn` resolution
#include "program/git_acquire.hpp"          // SystemGitRunner — the default git seam
#include "program/input_resolver.hpp"
#include "program/project_sources.hpp"  // D-AP2-SOURCES-GLOB + AP6 M4: sources[] → files
// D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF: the shipped runtime's
// content-addressed object cache — the key, the two roots, and the archive
// sibling lookup the runtime units are compiled against.
#include "program/runtime_object_cache.hpp"
#include "program/target_spec.hpp"

// The SHALLOW read of a language document's `language.fileExtensions` — see
// `LanguageBlockExtensionReader`. `src/program/CMakeLists.txt` already carries
// the PRIVATE `nlohmann_json::nlohmann_json` edge and keeps its own honest
// include list by `grep -rn '^#include <nlohmann' src/program/`; this file is
// the second entry that instrument reports.
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>  // the staging directory's uniqueness counter
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
// D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION (C):
// `serveArtifactFromCache` and `buildDependencyArtifactKey` return
// `std::expected`. It arrived transitively through `runtime_object_cache.hpp`
// and compiled — which is exactly how this class of fragility survives review
// until an unrelated include is pruned (the house rule, stated at the top of
// that same header).
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <latch>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
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

// ✅ THE TWO SYSTEM-DIR HELPERS ARE NO LONGER DECLARED HERE, and the deleted
// forward declarations are the defect rather than a detail of it —
// D-LSP-HAS-NO-SYSTEM-INCLUDE-DIRS-AND-DROPS-THE-CU-DRIVER-DIAGNOSTICS. They
// were defined at `dss` namespace scope FURTHER DOWN THIS FILE with no header
// declaring them, so a signature existed that no owner published: the LSP,
// which builds a `UnitBuilder` per open document, could not call them and grew
// no system dirs at all, while `dump_predefined_macros.cpp` re-walked
// `shippedLibDirs` with a private loop. The walk now lives once in
// `core/types/config_path_walk.hpp` (`resolveSystemDirs`) and its builder
// binding once beside `UnitBuilder::addSystemDir`
// (`analysis/compilation_unit/compilation_unit.hpp`, `applySystemDirs`) — both
// reached through includes this file already had.

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
            // D-DIAG-TWO-CODE-RENDERINGS hazard (H1) — the PUNCTUATION was the
            // other half of the split, and it is easy to miss because the token
            // gets all the attention. This arm emitted `] ` (bracket-space, NO
            // colon) while the positioned renderer emitted `]: `, so a census
            // that generalised the bracket CONTENTS but kept a `:` anchored in
            // its pattern still could not see a buffer-less diagnostic. Both
            // paths now spell the code with `diagnosticCodeName` AND close it
            // with `]: `; the contract is stated once in
            // `diagnostic_reporter.hpp`. The location-free LAYOUT is unchanged
            // and deliberately so — see the routing comment above.
            std::cerr << severityName(d.severity)
                      << "[" << diagnosticCodeName(d.code) << "]: "
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
// stream, same `dsscp: ` prefix, no policy in the way.
//
// ★ THE SHAPE, and each part of it is load-bearing for a machine reader:
//
//     dsscp: artifact <targetSpec> <absolute path>
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
    // [[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]]: `core::genericSpelling`,
    // NOT `generic_string()`. ✔MEASURED — the latter collapses a leading
    // separator RUN, so a path naming another machine printed as one on the
    // local drive root. The exported spelling substitutes the model's OWN
    // separator per character and leaves the run intact, giving the same
    // forward-slash result this line has always printed for an ordinary path.
    try {
        return core::genericSpelling(p);
    } catch (...) {
        auto const u8 = p.u8string();
        return std::string(reinterpret_cast<char const*>(u8.data()), u8.size());
    }
}

// Called ONCE per artifact, and ONLY after the write path reported success.
// ★★★ D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF: TRUE ONLY INSIDE THE NESTED
// BUILD THAT MATERIALISES A SHIPPED RUNTIME ARCHIVE. Declared up here because
// its two readers sit at opposite ends of this file — `compileOneTarget`'s
// artifact report, immediately below, and `resolveShippedRuntimeArchives`, which
// owns it. The full argument for why it exists (without it the nested build
// resolves its own runtime and recurses forever) is on
// `ShippedRuntimeBuildGuard`, which is the ONLY thing that may write it.
extern thread_local bool gCompilingShippedRuntimeUnit;

void reportArtifactWritten(std::string const& targetSpec,
                           fs::path const&    outPath) {
    // Lexical only. `absolute` needs the cwd; `lexically_normal` folds the
    // `.`/`..` a relative `--output` may have contributed. Neither touches the
    // disk — the bytes are already committed, and re-statting here would only
    // create a way for the report to disagree with the write.
    std::error_code ec;
    // [[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]]: NOT bare `fs::absolute`.
    // ⚠ The "stays TRUE either way" claim below held only for the FAILURE arm.
    // On a UNC `--output` the bare call SUCCEEDS having re-rooted the path, so
    // this line reported `C:\host\share\…` for an artifact written to
    // `\\host\share\…` — a diagnostic naming a file that is not there, which is
    // worse than losing the absolute guarantee it was written to protect.
    fs::path abs = core::absoluteKeepingRoot(outPath, ec);
    // Not a silent fallback: `outPath` IS the path the writer committed to, so
    // the line stays TRUE either way. Only the ABSOLUTE guarantee is lost, and
    // only when the process has no usable cwd to resolve against.
    if (ec) abs = outPath;
    std::cerr << "dsscp: artifact " << targetSpec << ' '
              << artifactPathForReport(core::normalizeKeepingRoot(abs))
              << '\n';
}

// Emit a driver-tier D_* diagnostic. Wraps `dss::report` so all
// driver-side fail-loud sites take the same shape (Error severity,
// ferried through the same reporter the kernel uses).
void emitDriver(DiagnosticReporter& rep,
                DiagnosticCode code,
                std::string msg) {
    dss::report(rep, code, DiagnosticSeverity::Error, std::move(msg));
}

// ── D-PROGRAM-PROJECT-WIDE-PARSE-GATE-MASKS-CENSUS: SAY WHAT WAS NOT MEASURED ─
//
// ★★★ THE DEFECT THIS EXISTS TO END. A driver that stops early deletes every
// diagnostic the phases it skipped would have produced, and NOTHING in the
// output says so — so an absent `S0006` reads as "DSS is fine with
// `__uint128_t`" when the truth is "semantic analysis never ran". ✔MEASURED
// before this landed: a two-TU build where TU-1 has a parse error and TU-2 has
// an `S0001` reported the parse errors and was TOTALLY SILENT about TU-2, while
// compiling TU-2 alone reported `S0001` at 2:12. Two registry rows carried a
// false blocking relationship because of exactly that inference, and a cycle
// was ordered around it.
//
// ⚠ `Info`, because the notice must not gate `hasErrors()` — it is emitted AT
// the gates that read `hasErrors()`, so an Error-severity notice would be a
// diagnostic that re-triggers the condition it describes. `Guaranteed`, because
// a run that hit a volume cap is exactly the run whose completeness is most in
// doubt: the one notice saying "this stream is incomplete" must not itself be
// the diagnostic the cap drops. Both fields mirror `P_DiagnosticsElided`, which
// is the same fact one tier down.
void emitPhasesNotRun(DiagnosticReporter& rep, std::string msg) {
    ParseDiagnostic d;
    d.code     = DiagnosticCode::D_LaterPhasesNotRun;
    d.severity = DiagnosticSeverity::Info;
    d.delivery = DiagnosticDelivery::Guaranteed;
    d.actual   = std::move(msg);
    rep.report(std::move(d));
}

// The sentence every altitude ends with. One wording, one place — a reader who
// learns what an absent diagnostic means here must not have to re-learn it from
// a differently-phrased sibling.
constexpr char const* kNotMeasuredNotAbsent =
    " A diagnostic that would have come from a phase named above as NOT RUN is "
    "MISSING FROM THIS RUN BECAUSE IT WAS NOT MEASURED — never because the "
    "condition it reports is absent.";

// The run stopped BEFORE any translation unit was built: no source was parsed,
// so no tier past the driver's own pre-flight ran for anything. `what` names the
// altitude in the driver's own vocabulary.
void emitStoppedBeforeCuBuild(DiagnosticReporter& rep,
                              char const*         what,
                              std::size_t         sourceCount) {
    emitPhasesNotRun(
        rep,
        std::string{"the build stopped at "} + what
            + ", before any translation unit was built: parsing, semantic "
              "analysis, HIR, MIR, LIR, assembly and link ran for NONE of the "
            + std::to_string(sourceCount) + " source file(s)."
            + kNotMeasuredNotAbsent);
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
    // ★★ THE BUFFERS TRAVEL WITH THE DIAGNOSTICS, AND FIRST. A diagnostic
    // whose `BufferId` names a buffer only `src` retains renders as
    // `--> <unknown-buffer:N>:offset K` at `dst` — the whole failure mode the
    // reporter-side retention exists to end (see
    // `DiagnosticReporter::sourceBuffers()`). Every mid-compile fragment
    // buffer — today an embedded assembly template, tomorrow any other tier
    // that parses text the CUs do not own — is registered on THIS scratch
    // reporter, so a merge that carried only `all()` would move the
    // diagnostics out of reach of their own source.
    // ⚠ Ordered before the diagnostic loop so a `dst` that is already capped
    // still gains the buffers: the cap drops diagnostics, not sources, and a
    // partial render must not also lose its context.
    dst.sourceBuffers().addAll(src.sourceBuffers());
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

// D-PERF-4-CU-PARALLELISM: worker count for the INTERNAL per-CU build pools
// (only consulted when NO executor is injected). Never more workers than there
// are jobs — extra workers would just block on the empty job queue. An explicit
// `--jobs` (jobsOverride > 0) pins the count within that ceiling; auto (0) uses
// min(hardware_concurrency, kMaxAutoWorkers). Called on both N>1 batch paths
// (the front-half CU build and the back-half per-CU MIR build).
//
// ★ WHY THE AUTO CEILING IS 32 AND NOT `hardware_concurrency()` ITSELF.
// The bound here is a MEMORY bound, not a CPU one: each in-flight CU owns its
// own interner, symbol table and arenas, so peak RSS scales with the number of
// CONCURRENT units, not with the core count. ✔MEASURED on the SQLite corpus
// (103 CUs, release config, this host): peak working set 5.0 GB at width 16 and
// 8.6 GB at width 32 — sublinear, because the pool's steady state is bounded by
// how many units are simultaneously at their peak rather than by the width. 32
// is chosen as the largest width whose measured peak still fits the 16 GB
// machine class this compiler ships to with room for the OS and a linker; the
// old value of 16 left half of a 32-core host idle for no memory benefit it
// could point at. `--jobs N` remains the operator's override in BOTH directions
// — `--jobs 64` opts a big-memory host in, `--jobs 8` opts a small one out —
// and it is deliberately NOT clamped to this ceiling, because an operator who
// names a width has measured their own machine and this constant has not.
//
// ⚠ NOT A WORKER-COUNT FIX FOR THE BACK HALF. The back half's achieved
// concurrency was ~3.7x while the cap already allowed 16, so its width was
// never the binding constraint; raising the cap does not address that, and the
// `--time` job-batch rows exist to give the next tuning pass the per-job wall
// distribution instead of another guess.
[[nodiscard]] std::size_t resolveCuPoolWidth(std::size_t cuCount,
                                             unsigned    jobsOverride) noexcept {
    constexpr std::size_t kMaxAutoWorkers = 32;
    std::size_t const ceiling = std::max<std::size_t>(std::size_t{1}, cuCount);
    if (jobsOverride > 0) {
        return std::min<std::size_t>(jobsOverride, ceiling);
    }
    std::size_t const hw = std::thread::hardware_concurrency();
    std::size_t const autoWidth =
        std::min<std::size_t>(hw == 0 ? std::size_t{1} : hw, kMaxAutoWorkers);
    return std::min<std::size_t>(autoWidth, ceiling);
}

// ══ PER-JOB WALL TIMING FOR THE CU BATCHES ═══════════════════════════════════
//
// The `--time` phase table says how long each pipeline VERB took; it cannot say
// whether a batch's workers were busy or blocked, because a phase's numbers are
// summed over whichever threads happened to run it. These records close that
// gap: one row per BATCH of per-CU jobs, carrying the width the batch actually
// ran with, the batch's own wall time, and EVERY job's individual wall time.
//
// ★ WHY PER-JOB AND NOT JUST AN AGGREGATE. The back half's measured concurrency
// was ~3.7x against a cap of 16 — so the interesting question is the SHAPE of
// the job-duration distribution (a long tail of one huge translation unit
// serializes the batch's ending no matter how many workers exist), and an
// average cannot show a tail. The report prints the batch wall, the sum of job
// walls, their ratio (achieved concurrency) and the slowest job, which is the
// minimum needed to tell "not enough workers" from "one job is the critical
// path".
//
// AGNOSTIC: `stage` is a PIPELINE-STAGE label — the same vocabulary class as a
// CompilePhase verb — never a language, CPU or object-format identity.
struct JobBatchRecord {
    std::string_view           stage;
    std::size_t                width       = 0;
    std::uint64_t              batchWallNs = 0;
    std::vector<std::uint64_t> jobWallNs;   // index-parallel to the batch's jobs
};

// Appended once per batch, by the thread that joined it. The driver's batch
// sites are serial with respect to one another (the per-key CU build loop and
// the per-target compile loop both run on one thread), so contention is nil —
// the mutex is here because `Program` is a library type an embedder may drive
// from several threads, and an unguarded `push_back` would then be a race in
// somebody else's process rather than a visible one in ours.
std::mutex                  gJobBatchMutex;
std::vector<JobBatchRecord> gJobBatches;

void recordJobBatch(JobBatchRecord rec) {
    std::lock_guard const lk{gJobBatchMutex};
    gJobBatches.push_back(std::move(rec));
}

[[nodiscard]] std::vector<JobBatchRecord> readJobBatches() {
    std::lock_guard const lk{gJobBatchMutex};
    return gJobBatches;
}

// The DIRECTORY one target's artifact is written into — BOTH arms stated in one
// place, because the second one used to be implicit.
//
// `--output <dir>` given ⇒ `<dir>` (flat) or `<dir>/<formatName>` when the build
// has several targets, or when the caller forces the per-format subdir (every
// PROJECT build does, D-AP2-OUTPUT-ROUTING). `--output` ABSENT ⇒ the base is
// `<cwd>/target`, ALWAYS per-format-subdir'd. `formatName` already encodes
// machine+OS, so no separate `<targetName>` component is added (redundant +
// bloats the path).
//
// ★ WHY THIS IS A NAMED FUNCTION AND NOT FOUR LINES INLINE (AP6). It is now half
// of `compileOneTarget`'s RETURN VALUE — a contract another build tier reads —
// and "when `--output` is absent the artifacts are under `<cwd>/target`" was a
// fact you could only learn by finding the `else` branch. A caller that must
// state where a build's outputs will land (AP6's dependency artifacts) needs the
// rule to be nameable, and a rule with a name has exactly one definition. Pure:
// no filesystem effect, no diagnostics — the mkdir + containment check stay at
// the call site, where the reporter is.
// ── D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION (C) ────────
//
// What one target's build carries in order to participate in the cross-build
// artifact cache. The KEY is computed at the `buildCus` boundary in
// `runCusToTargets` — where every CU is front-ended, no target has been
// compiled, and the union of `cu.inputDigest()` is therefore visible — and the
// eviction policy comes from the ROOT manifest. DISENGAGED ⇒ this build does
// not participate: nothing is looked up and nothing is stored.
//
// ★★ THE KEY IS BUILT AT THAT BOUNDARY AND USED **HERE**, IN
// `compileOneTarget`, AND THE SPLIT IS THE POINT. The boundary is the only
// place the input closure exists; `compileOneTarget` is the only place the
// artifact's final on-disk path exists (see `resolveArtifactOutputDir`'s own
// note on why that formula has exactly one owner). Serving a hit means writing
// the cached bytes to THAT path, so a consult that lived entirely at the
// boundary would need a second copy of the path formula — the drift this
// codebase rejects everywhere.
struct ArtifactCacheTicket {
    dss::runtime::RuntimeObjectKey key;
    dss::runtime::CacheEviction    eviction;
};

// The manifest's eviction vocabulary → the store's.
//
// ★ A `switch` WITH NO `default`, NOT A TERNARY, AND THAT IS THE POINT. The
// ternary this replaced mapped `Retain` to `Retain` and EVERYTHING ELSE to
// `PruneSuperseded`, so a third manifest token would have silently acquired
// pruning semantics nobody wrote — the "enumerate the members, never the
// complement" trap this repo has paid for before. With no `default` arm, adding
// an enumerator is a COMPILE error at exactly the site that must decide.
[[nodiscard]] dss::runtime::CacheEviction
storeEvictionFor(DependencyArtifactCacheEviction manifestPolicy) noexcept {
    switch (manifestPolicy) {
        case DependencyArtifactCacheEviction::PruneSuperseded:
            return dss::runtime::CacheEviction::PruneSuperseded;
        case DependencyArtifactCacheEviction::Retain:
            return dss::runtime::CacheEviction::Retain;
    }
    // Unreachable for every declared enumerator; present because a value
    // outside the enumeration is undefined behaviour rather than a case, and
    // the pruning arm is the one that DELETES files.
    return dss::runtime::CacheEviction::Retain;
}

// Consult the cache for `ticket` and, on a VERIFIED hit, place the cached bytes
// at `outPath`.
//   * `true`      — served; the artifact is on disk and the pipeline is skipped.
//   * `false`     — an ordinary MISS. Compile.
//   * error       — a REFUSAL, and it FAILS THE TARGET. The entry at the key's
//                   path cannot be shown to be this key's (absent, unreadable
//                   or differing sidecar), which is the one arm where the
//                   question is whether the BYTES ARE RIGHT rather than whether
//                   an optimization is available.
[[nodiscard]] std::expected<bool, std::string>
serveArtifactFromCache(ArtifactCacheTicket const& ticket,
                       fs::path const&            outPath);

// Add a freshly written artifact to the cache. Best-effort by contract: the
// bytes are already correct and already on disk, so a failure costs a note and
// a recompile next time, never this build.
void storeBuiltArtifactInCache(ArtifactCacheTicket const& ticket,
                               fs::path const&            artifactPath);

[[nodiscard]] fs::path resolveArtifactOutputDir(
    std::optional<std::filesystem::path> const& outputDir,
    std::string const&                          formatName,
    bool                                        multiTargetBuild,
    bool                                        perFormatOutputSubdir) {
    if (outputDir.has_value()) {
        return (multiTargetBuild || perFormatOutputSubdir)
                 ? (*outputDir / formatName)
                 : *outputDir;
    }
    return fs::current_path() / "target" / formatName;
}

// Compile one resolved (CU, target, format) triple to one artifact.
// Returns THE ARTIFACT'S PATH on success; `std::nullopt` on failure,
// with the reason already emitted via `reporter`.
//
// ★ WHY A PATH AND NOT A `bool` (AP6). The final on-disk path is
// computed HERE and nowhere else — from `outputDir`, the
// single-vs-multi-target rule, `perFormatOutputSubdir`, the format's
// own extension, and `artifactName.value_or(sourceStem)`. A consumer
// that needs to know what was built (AP6 threads a built dependency's
// artifact into the build that depends on it) would otherwise have to
// RE-DERIVE that formula, giving the repo a second copy of a fact that
// can drift from the first — the failure mode this codebase rejects
// everywhere. Returning it makes the producer the only source.
// `std::optional` rather than an out-parameter for a reason the type
// enforces: an out-parameter can be READ after a failed call, and the
// value it would hold is the path of an artifact that was never
// written. A disengaged optional cannot be misread that way.
//
// `outputDir` (D-LK10-ENTRY Slice C companion): when set, the
// emitted binary lands at `<outputDir>/<name><ext>` for
// single-target builds, or `<outputDir>/<formatName>/<name><ext>`
// for multi-target builds (the multi-target qualifier disambiguates
// same-named outputs across formats). When unset, the base is
// `<cwd>/target` — see `resolveArtifactOutputDir`, which states both
// arms in one place rather than leaving the fallback implicit.
//
// The artifact base `<name>` = `artifactName.value_or(sourceStem)`: a
// project manifest's `artifactName` overrides the source stem; nullopt
// (the CLI path, and a project without the field) keeps the source stem
// (unchanged). `perFormatOutputSubdir` (D-AP2-OUTPUT-ROUTING) forces the
// `<formatName>/` subdir even for a single-target `--output` build — a
// PROJECT build sets it so every platform's artifact is consistently
// per-platform-subdir'd; the CLI path leaves it false, so `--compile`
// single-target output stays flat (byte-identical).
[[nodiscard]] std::optional<std::filesystem::path>
compileOneTarget(                   std::span<CompilationUnit const> cus,
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
                                    ImageRequest const&    imageRequest,
                                    // D-PROGRAM-PROJECT-WIDE-PARSE-GATE-MASKS-CENSUS:
                                    // MEASURE, DO NOT EMIT. Set when a sibling
                                    // translation unit failed to parse and was
                                    // excluded, so this program is knowingly
                                    // incomplete: run the per-unit front half
                                    // (semantic analysis, HIR, MIR) so its
                                    // diagnostics reach the operator, then stop
                                    // BEFORE the first cross-unit step and
                                    // write nothing. It is never set by a
                                    // healthy build, so the ordinary path is
                                    // byte-identical.
                                    bool                   analysisOnly,
                                    // Clause (C) of
                                    // D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION:
                                    // this target's cross-build artifact cache
                                    // ticket, or DISENGAGED for every build
                                    // that does not participate — which is
                                    // every CLI build, every root project
                                    // build, and every dependency build under
                                    // a manifest that declares no
                                    // `dependencyArtifactCache`. Disengaged ⇒
                                    // byte-identical behaviour.
                                    std::optional<ArtifactCacheTicket> const&
                                                           cacheTicket) {
    // ★★★ [[D-PP-SEMANTIC-DIAGNOSTIC-POSITION-UNREMAPPED]] — EVERY DIAGNOSTIC
    // THIS COMPILE PRODUCES LEAVES IN ORIGIN COORDINATES, WHATEVER TIER MADE IT
    // AND WHICHEVER EXIT IT LEAVES BY.
    //
    // Below this line run the semantic tier, CST→HIR, HIR→MIR, the optimizer,
    // MIR→LIR, the assembly engine and the linker. Each positions its
    // diagnostics from a CU tree, and when that tree was preprocessed those are
    // SYNTHESIZED coordinates — a line shifted by the built-in predefine
    // prologue plus one line per `--define`, and, for anything inside an
    // `#include`d header, the MAIN file's name. The semantic tier converts its
    // own on the way out of `analyze`; this guard is what makes the guarantee
    // hold for the tiers that have no single exit of their own. ✔MEASURED
    // through the CLI: an ASM-tier `A0008` showed BOTH shapes (line +2 under two
    // `--define`s, and a header-origin refusal attributed to the main file), so
    // a semantic-only fix would have been a slice of the defect, not the defect.
    //
    // A DESTRUCTOR and not a line before each `return`: this function has many
    // exits, several of them early refusals, and "remember to convert on the way
    // out" is the kind of obligation that is satisfied on the day it is written
    // and broken by the next exit anybody adds. The conversion is a no-op for a
    // CU with no preprocessed tree, and idempotent, so the semantic tier's own
    // earlier call is not a competing policy.
    class LeaveInOriginCoordinates {
    public:
        LeaveInOriginCoordinates(std::span<CompilationUnit const> units,
                                 DiagnosticReporter&              rep) noexcept
            : cus_{units}, reporter_{&rep} {}
        LeaveInOriginCoordinates(LeaveInOriginCoordinates const&)            = delete;
        LeaveInOriginCoordinates& operator=(LeaveInOriginCoordinates const&) = delete;
        ~LeaveInOriginCoordinates() {
            for (CompilationUnit const& cu : cus_) {
                cu.remapPreprocessedPositions(*reporter_);
            }
        }
    private:
        std::span<CompilationUnit const> cus_;
        DiagnosticReporter*              reporter_;
    };
    LeaveInOriginCoordinates const leaveInOriginCoordinates{cus, reporter};

    auto parsed = TargetSpec::parse(targetSpecStr);
    if (!parsed) {
        emitDriver(reporter, DiagnosticCode::D_InvalidTargetSpec,
                   targetSpecErrorMessage(targetSpecStr, parsed.error()));
        return std::nullopt;
    }

    auto targetR = TargetSchema::loadShipped(parsed->targetName);
    if (!targetR.has_value()) {
        emitTargetSchemaLoadFailed(reporter, targetR.error(),
                                   parsed->targetName);
        return std::nullopt;
    }
    auto formatR = ObjectFormatSchema::loadShipped(parsed->formatName);
    if (!formatR.has_value()) {
        emitObjectFormatSchemaLoadFailed(reporter, formatR.error(),
                                         parsed->formatName);
        return std::nullopt;
    }

    // D-LK6-8.2 cross-validation: confirm the (target, format) pair's
    // machine identity matches before linking. Without this guard, a
    // hand-edited format JSON with the wrong `machine` value would
    // silently dispatch the linker to the wrong PLT-stub emitter,
    // producing SIGILL at runtime with no driver diagnostic.
    if (!crossValidateTargetFormat(**targetR, **formatR, reporter)) {
        return std::nullopt;
    }

    // ★ THE LANGUAGE↔TARGET ARCHITECTURE GATE, ROOT ARM
    // (D-ISA-LANGUAGE-BOUND-TO-ARCHITECTURE). Its sibling directly above asks
    // whether the TARGET and the FORMAT agree; this asks whether the LANGUAGE
    // may be compiled for the TARGET at all. A language that declares no `isa`
    // is PORTABLE and this is a single empty-string test — which is what every
    // C, T-SQL and toy compile in the repo takes.
    //
    // ★ IT LIVES HERE BECAUSE THIS IS THE ONE CHOKEPOINT WHERE A GRAMMAR AND A
    // TARGET MEET. `compileOneTarget` is reached by EVERY build in the
    // process: the CLI `--compile` path, a `.dss-project.json` build, and a
    // dependency's own sub-build (which runs a fresh `Program` through
    // `compileFiles`/`compileUnits`). Placing the gate at the project entry
    // point instead would have left `dss --compile hello.s --target arm64:…`
    // — the single most direct way to hit this mistake — completely
    // unguarded, and would have made the same source mean different things
    // depending on which entry point invoked it.
    //
    // ⓘ The DEPENDENCY arm is NOT this call. `dependency_resolver.cpp` runs
    // the same gate at RESOLVE time so a dependency is refused BEFORE it is
    // cloned, hooked and built, and so the message names the dependency's own
    // manifest rather than the project the operator invoked. This site is the
    // backstop that also covers a root build with no `dependsOn` at all.
    //
    // ⓘ `grammar.name()` is the language's OWN declared name (`AsmX86_64Att`),
    // not the document stem the operator typed (`asm-x86_64-att`), and here
    // that is deliberate: this value is PROSE IN A DIAGNOSTIC and nothing else
    // — it is compared against no path, no `--language` argument and no cache
    // key, so the identity this function actually holds is the right one to
    // print. The message carries the actionable half either way: both declared
    // ISA values, and where each is declared.
    //
    // ⚠ NOT AN ARGUMENT FOR USING `name()` ANYWHERE ELSE. `CuBuildKey::
    // languageName` used to cite this site as precedent and was corrected in
    // cycle P36: a KEY needs an identity that is unique by construction, which
    // the declared name is not. The rule is the USE, not the call site — a
    // name that will be printed, versus a name that will be resolved.
    if (!crossValidateLanguageTarget(grammar, grammar.name(), **targetR,
                                     targetSpecStr, /*subject=*/{}, reporter)) {
        return std::nullopt;
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
    if (!abi) return std::nullopt;

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
        return std::nullopt;
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
    auto const ext = outputExtensionFor(**formatR);
    fs::path const outDir = resolveArtifactOutputDir(
        outputDir, parsed->formatName, multiTargetBuild, perFormatOutputSubdir);
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
                   + core::genericSpelling(outDir) + "': " + ec.message());
        return std::nullopt;
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
    //
    // ⚠ THE "NEVER FIRES ON THE CLI PATH" CLAIM ABOVE WAS FALSE, AND THE
    // COUNTEREXAMPLE IS ONE CHARACTER LONG. ✔MEASURED on the shipped CLI:
    // `--output .` with a bare source stem was REFUSED —
    //   "artifact name 'a' resolves outside the output directory '.'"
    // — because the two sides of the comparison were normalized DIFFERENTLY.
    // `("." / "a.o").lexically_normal()` is `a.o`, whose `parent_path()` is the
    // EMPTY path, while `fs::path{"."}.lexically_normal()` is `.` — and `"" !=
    // "."`, so a perfectly contained artifact read as an escape. The containment
    // rule was right; only the basis it compared against was.
    //
    // ★ THE FIX IS TO DERIVE THE BASIS THROUGH THE SAME CONSTRUCTION THE
    // ARTIFACT PATH TAKES, rather than to normalize `outDir` on its own and hope
    // the two spellings agree. Appending a bare name and taking the parent puts
    // both sides through the identical `operator/` + `lexically_normal()`
    // pipeline, so they cannot disagree about how a relative, `.`-rooted or
    // trailing-separator directory spells itself. Every escape vector the block
    // above names still fires: a bare ".." normalizes the parent away, and a
    // differing root-name ("D:app") makes `operator/` REPLACE `outDir` so the
    // parents differ by root.
    //
    // ★★ AND BOTH SIDES GO THROUGH `core::normalizeKeepingRoot`, NOT
    // `lexically_normal()`.
    // [[D-PATH-MULTI-SEPARATOR-ROOT-COLLAPSED-BY-STDLIB-PATH-TRANSFORMS]]
    // Normalising IDENTICALLY is necessary but not
    // sufficient: `lexically_normal()` collapses a leading separator RUN, so for
    // an `outDir` naming a share it maps TWO different locations onto ONE
    // spelling. ✔MEASURED 2026-08-28 — `//host/share/out` normalises to
    // `\host\share\out`, which is what a genuinely LOCAL `/host/share/out`
    // normalises to as well. An artifact name carrying that root would then
    // match the basis and be judged CONTAINED while resolving onto the local
    // drive — the escape this block exists to refuse, admitted by the
    // comparison rather than by the rule. `normalizeKeepingRoot` is identical to
    // `lexically_normal()` for every path with a run below 2, so the `--output .`
    // repair described above is untouched.
    auto const containmentBasis =
        core::normalizeKeepingRoot(outDir / "dss-containment-probe")
            .parent_path();
    if (core::normalizeKeepingRoot(outPath).parent_path() != containmentBasis) {
        emitDriver(reporter, DiagnosticCode::D_ArtifactNameEscapesOutputDir,
                   "artifact name '" + artifactName.value_or(sourceStem)
                   + "' resolves outside the output directory '"
                   + core::genericSpelling(outDir)
                   + "' — it must be a bare file name (no path separators, no "
                     "drive/root prefix, and not '.' or '..').");
        return std::nullopt;
    }

    // D-HARNESS-FIXTURE-PATH-ASSUMES-THE-POSIX-ARTIFACT-SPELLING (TF-C118):
    // THE ONE report site for the three link/write dispatches below (static
    // archive / single CU / merged multi-CU). They are three routes to the
    // SAME `outPath`, so the report is attached at their only common ancestor
    // rather than copied into each — and it is attached to their RESULT, so
    // the line cannot be printed for a write that failed. Every `return` of a
    // link/write result below goes through here; a route added later that does
    // not is a route whose artifact goes unreported, which is the whole defect.
    // It is ALSO the one place the success return value is minted (AP6): the
    // path is returned exactly when the write succeeded, by the same `wrote`
    // that decides whether to print the report line. Two facts, one predicate —
    // a route that returned a path without reporting (or reported without
    // returning) would need to bypass this lambda, which is the same bypass the
    // note above already forbids.
    // ⓘ D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF: the REPORT is suppressed —
    // and only the report — for the nested build that materialises a shipped
    // runtime archive. `dsscp: artifact …` announces what the OPERATOR'S build
    // produced; a runtime archive is an internal cache entry they never named,
    // and printing it would make an ordinary `hello.c` claim to have written
    // two extra artifacts on the first (cold) run and none afterwards. The
    // RETURN VALUE is untouched, so `Program::artifactPaths()` still answers
    // where the archive landed — which is how the cache finds the bytes to
    // store. The `wrote` predicate remains the single owner of both facts.
    // ⓘ D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION (C): it
    // is ALSO where a freshly written artifact enters the cross-build cache,
    // and for the same single-owner reason. `wrote` is the one predicate that
    // knows an artifact exists at `outPath`; a store attached anywhere else
    // would either need to re-derive that fact or would cache a file some
    // route wrote without reporting.
    bool servedFromCache = false;
    auto const reported = [&](bool wrote) -> std::optional<std::filesystem::path> {
        if (!wrote) return std::nullopt;
        // ⛔ AND NOT STORED WHEN THIS TARGET REPORTED AN ERROR. Storing there
        // would put a FAILING build's bytes in the cache, and the next build
        // would serve them, emit none of the errors (they were the previous
        // run's) and go GREEN — the silent wrong answer this whole mechanism is
        // built to make impossible, reachable through the cache instead of
        // through the key. `runCusToTargets` asserts the state exists in the
        // very conjunction it applies one line after this call: *"a scratch
        // error alongside a written file means the build is failing, and
        // handing a consumer that file would let a failed dependency be linked
        // into something that then 'succeeds'"*.
        //
        // ⚠⚠ AND I MUST SAY SO: **THIS GUARD IS DEFENCE IN DEPTH AND NO TEST
        // WITNESSES IT.** ✔MEASURED 2026-08-31 by REMOVE-direction mutation —
        // delete the `!reporter.hasErrors()` clause and
        // `program/test_dependency_artifact_cache` stays GREEN (build rc 0,
        // `program.cpp.obj` md5 moved and returned, controls green either side).
        // ✔TWO ROUTES TO THE STATE WERE MEASURED AND BOTH ABORT BEFORE THE
        // WRITE: a `stackReserve` the dependency's format cannot carry, and an
        // `S_UnknownAttribute` warning promoted by `warningsAsErrors`. It is
        // KEPT on the neighbour's own claim and on the asymmetry the store
        // states (a wrongly-cached artifact ships wrong bytes; a wrongly-
        // uncached one costs one recompile) — but the claim here is UNWITNESSED
        // rather than load-bearing, and it must not be deleted on the strength
        // of the mutant alone. [[feedback-a-brief-premise-is-a-hypothesis]].
        //
        // ⓘ WARNINGS ARE NOT ERRORS AND DO NOT BLOCK THE STORE, and the
        // consequence — a warm build re-emits none of them — is the ORDINARY
        // semantics of an up-to-date build rather than a loss this mechanism
        // invents: `make` does not re-warn about an object file it did not
        // rebuild either. What must never be lost is a build that FAILED, and
        // that is what the guard covers.
        //
        // ⓘ NOT re-stored when the bytes came OUT of the cache: the entry is
        // already there and was verified byte-for-byte on the way in, so a
        // second store would read the artifact back only to re-confirm a
        // sidecar it has just matched.
        if (cacheTicket.has_value() && !servedFromCache
            && !reporter.hasErrors()) {
            storeBuiltArtifactInCache(*cacheTicket, outPath);
        }
        if (!gCompilingShippedRuntimeUnit) {
            reportArtifactWritten(targetSpecStr, outPath);
        }
        return outPath;
    };

    // ── THE CACHE CONSULT ───────────────────────────────────────────────────
    //
    // ★★ HERE AND NOT EARLIER: `outPath` and its containment check are the two
    // things a served hit needs, and both are decided immediately above. ★★ AND
    // HERE AND NOT LATER: everything below this point is the pipeline the hit
    // exists to skip — semantic analysis, CST→HIR, HIR→MIR, the optimizer,
    // MIR→LIR, assembly and the link. The front end has already run (the CUs
    // arrived built), which is exactly the cost a hit still pays and the reason
    // the key is COMPLETE BY CONSTRUCTION rather than verified against a record.
    //
    // ⚠ A REFUSAL FAILS THE TARGET and does not fall through to compiling —
    // see `serveArtifactFromCache`. A MISS is silent and ordinary.
    if (cacheTicket.has_value()) {
        auto const served = serveArtifactFromCache(*cacheTicket, outPath);
        if (!served.has_value()) {
            // ⓘ `D_FileReadFailed` is the CLOSEST code the driver band carries
            // rather than an exact one — the same judgement, and the same
            // reasoning, as the runtime cache's wiring records at its own
            // unverifiable-entry arm. Nothing is lost to a reader: the message
            // carries the path, the 16-character index, the full identity
            // digest, the remedy and the anchor.
            emitDriver(reporter, DiagnosticCode::D_FileReadFailed,
                       served.error());
            return std::nullopt;
        }
        if (*served) {
            servedFromCache = true;
            return reported(true);
        }
    }

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
    // ── D-OPT7-CROSSCU-THUNK-RESERVED-FOR-SEPARATE-COMPILATION: the THIRD arm
    // of the same dispatch ────
    //
    // A relocatable OBJECT named on `--resolve-library` is neither a dynamic
    // library nor an archive, and before this arm it took the dynamic path and
    // died on `F_SectionNotFound` -- "no `.dynsym` section found" -- a message
    // that blames a missing section when the real fact is that the dispatcher
    // had no arm for the file's actual shape. ✔MEASURED on the shipped CLI
    // before the arm landed.
    //
    // ★ THE ORDER OF THE THREE PROBES IS NOT ARBITRARY. `ar` first, because an
    // archive's global magic is unambiguous and its members are objects (a
    // relocatable probe must never see the container). Then the object probe,
    // which is asked OF THE FORMAT. Dynamic last, as the residual -- keeping the
    // pre-existing rule that an unreadable path stays dynamic so its eager
    // open-probe fails loud rather than being silently diverted.
    //
    // The whole spec rides to the dynamic side; the object and archive sides
    // take only the PATH, because a merged input records no runtime import for a
    // STATED import name to name (D-FFI-DECLARED-IMPORT-NAME). Nothing is
    // dropped where it would have had an effect.
    std::vector<std::filesystem::path> staticArchives;
    CompileOptions perCuOpts = compileOpts;
    {
        std::vector<ResolveLibrarySpec> dynamicLibs;
        for (auto const& lib : compileOpts.resolveLibraries) {
            if (isArArchiveFile(lib.path)) {
                staticArchives.push_back(lib.path);
            } else if (isRelocatableObjectFile(lib.path, **formatR)) {
                // Joins the SAME list a `--compile`-named object lands in --
                // one channel, so there is one definition of what an object
                // input means and no second engine to keep in step.
                perCuOpts.objectInputs.push_back(lib.path);
            } else {
                dynamicLibs.push_back(lib);
            }
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
    // ⚠ D-LK-OBJECT-INPUT-COMPILE-FLAG-SURFACE: `!cus.empty()` GUARDS THE WHOLE
    // DISPATCH, because a pipeline ENTRY TIER is a property of a SOURCE
    // LANGUAGE and an all-object link has no source. Without the guard an
    // `--compile a.o` under a target whose `defaultAssemblyLanguage` declares
    // `{"rule": "root", "tier": "encode"}` — which is every target, since no
    // `--language` is needed to link objects — reached the `Encode` arm with
    // ZERO CUs and was refused as "a multi-CU standalone-assembly build",
    // naming a build the operator never asked for. The grammar is still
    // resolved and still cross-validated against the target above; it simply
    // has nothing to say about which tier to enter when nothing is parsed.
    if (auto const rootTier =
            grammar.pipelineEntry().tierForRule(grammar.rootCursor().rule());
        !cus.empty() && rootTier.has_value()) {
        switch (*rootTier) {
        case PipelineTier::Hir:
            break;   // the ordinary path below
        case PipelineTier::Encode: {
            if (cus.size() != 1) {
                emitDriver(reporter, DiagnosticCode::D_PlanNotLanded,
                           "a multi-CU standalone-assembly build is not yet "
                           "lowered — each unit's symbol space would have to be "
                           "namespaced before the modules merge");
                return std::nullopt;
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
                return std::nullopt;
            }
            // D-PROGRAM-PROJECT-WIDE-PARSE-GATE-MASKS-CENSUS: the same
            // measure-only stop the full pipeline takes below, at this tier's
            // own equivalent point — the assembly unit has been built and every
            // diagnostic it produced is reported; the LINK is what must not
            // happen for a knowingly-incomplete program.
            if (analysisOnly) return std::nullopt;
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
                // D-OPT7-CROSSCU-THUNK-RESERVED-FOR-SEPARATE-COMPILATION: an
                // assembly unit may name
                // pre-assembled objects exactly as a C unit may — the `encode`
                // tier reaches the linker through this same composition, which
                // is why the parameter is threaded here rather than defaulted
                // away on the route least likely to be exercised.
                std::span<std::filesystem::path const>{perCuOpts.objectInputs},
                std::span<std::filesystem::path const>{staticArchives},
                // D-LK-ARCHIVE-MEMBER-EXTERN-CANNOT-BIND-A-RESOLVE-LIBRARY: the
                // DYNAMIC half, so a pulled member's extern can rebind to a
                // library the operator named. `perCuOpts` (not `compileOpts`)
                // for the same reason it is `perCuOpts` above — the `ar`
                // archives are already partitioned out into `staticArchives`.
                std::span<ResolveLibrarySpec const>{perCuOpts.resolveLibraries},
                **targetR, **formatR, outPath, reporter, imageRequest));
        }
        case PipelineTier::Mir:
        case PipelineTier::Lir:
            emitDriver(reporter, DiagnosticCode::D_PlanNotLanded,
                       "the language's root declares a pipeline entry tier this "
                       "driver has no route for — refused rather than silently "
                       "taking the full pipeline, which would run the "
                       "construct through tiers it asked to skip");
            return std::nullopt;
        }
    }

    std::vector<std::optional<CuMirModule>> cuMirSlots(cus.size());
    if (cus.size() <= 1) {
        if (!cus.empty()) {
            cuMirSlots[0] = buildCuMir(cus[0], grammar, **targetR, **formatR,
                                       ccIndex, reporter, perCuOpts);
            if (!cuMirSlots[0]) {              // front-half tier failure already reported
                emitNullNoDiagnostic("per-CU build (buildCuMir)");
                return std::nullopt;
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
        std::size_t const     poolWidth = resolveCuPoolWidth(cus.size(), jobsOverride);
        if (executor == nullptr) {
            localPool.emplace(poolWidth);
            executor = &*localPool;
        }

        // Submit one job per CU, writing BY INDEX `i` into its own slot + scratch — no shared
        // container is mutated, so there is nothing to lock. A `std::latch` counts completions;
        // the RAII guard fires `count_down()` even if `buildCuMir` throws (the ThreadPool worker
        // then logs the throw), so a throwing job can never DEADLOCK `done.wait()` — the slot
        // just stays nullopt and fails the compile below. `i` is captured BY VALUE so each job
        // owns its index; everything else is captured by reference and outlives `done.wait()`.
        // `jobWallNs[i]` is written by index for the same reason every other per-CU datum is
        // (see `JobBatchRecord`): it is per-job instrumentation, so it must not be able to
        // perturb the ORDER of anything the compile observes.
        std::vector<std::uint64_t> jobWallNs(cus.size(), 0);
        auto const                 batchStart = std::chrono::steady_clock::now();
        std::latch done{static_cast<std::ptrdiff_t>(cus.size())};
        for (std::size_t i = 0; i < cus.size(); ++i) {
            executor->submit([&, i] {
                struct CountDownGuard {
                    std::latch& latch;
                    ~CountDownGuard() { latch.count_down(); }
                } const guard{done};
                auto const jobStart = std::chrono::steady_clock::now();
                cuMirSlots[i] = buildCuMir(cus[i], grammar, **targetR, **formatR,
                                           ccIndex, cuScratch[i], perCuOpts);
                jobWallNs[i] = static_cast<std::uint64_t>(
                    std::max<long long>(0, std::chrono::duration_cast<
                        std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - jobStart).count()));
            });
        }
        done.wait();
        recordJobBatch(JobBatchRecord{
            "back-half cu mir",
            // An INJECTED executor's width is the injector's business and is
            // not knowable from here; report 0 rather than the width this
            // driver would have chosen, which it did not use.
            injectedExecutor != nullptr ? std::size_t{0} : poolWidth,
            static_cast<std::uint64_t>(std::max<long long>(
                0, std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - batchStart).count())),
            std::move(jobWallNs)});

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
            return std::nullopt;  // ≥1 front-half tier failure — all diagnostics reported
        }
    }

    // ── D-PROGRAM-PROJECT-WIDE-PARSE-GATE-MASKS-CENSUS: THE MEASURE-ONLY STOP ──
    //
    // Everything above is PER UNIT: `buildCuMir` ran semantic analysis, HIR and
    // MIR for each surviving CU and every one of their diagnostics is already
    // on `reporter`. Everything below is CROSS-UNIT, starting at `mergeCuMirs`,
    // and this program is knowingly missing a unit — so a "no entry point" or
    // an unresolved cross-unit symbol from here down would be a complaint about
    // the EXCLUSION, not about the source. That is the derivative noise the
    // old run-wide gate was built to avoid, and it is avoided here instead,
    // one tier lower, having kept the census the old gate deleted.
    //
    // ⚠ NO `emitNullNoDiagnostic` HERE, AND THAT IS NOT AN OVERSIGHT. Its
    // contract is "a null module with no diagnostic behind it is a substrate
    // bug"; this null has the excluded unit's own parse errors behind it, on
    // the RUN-WIDE reporter, which is where the caller put them and where the
    // operator reads them. Emitting the internal-error code here would report a
    // driver defect for a build that is behaving exactly as designed.
    if (analysisOnly) return std::nullopt;

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
        // D-PROGRAM-TIER-RETAINS-FORMAT-IDENTITY-BRANCHES: this line READ
        // `kind() == ObjectFormatKind::Pe ? ".obj" : ".o"`. The member
        // extension is a NAMING fact about the ecosystem this archive belongs
        // to, so the archive format declares it (`archiveMemberExtension`,
        // presence-paired with `container: archive` and refused on any format
        // that packages no members).
        std::string const memberExt{(*formatR)->archiveMemberExtension()};
        std::vector<AssembledModule> members;
        std::vector<std::string>     memberNames;
        members.reserve(cuMirs.size());
        memberNames.reserve(cuMirs.size());
        // ── D-STATICLIB-MEMBER-NAME-DERIVES-FROM-THE-FIRST-SOURCE ──────────
        //
        // ★★ A MEMBER IS NAMED AFTER THE SOURCE THAT PRODUCED IT, AND UNTIL
        // NOW NONE OF THEM WAS. Every CU-derived member took the DRIVER'S
        // `sourceStem` — `fs::path{sourceFiles.front()}.stem()`, the FIRST file
        // of the whole command — plus its index, so ✔MEASURED before this
        // change: `--compile u1.c u2.c --target …-staticlib` emitted `u1_0.obj`
        // and `u1_1.obj`, and `u2.c`'s member was named after `u1`. The armap
        // indexes members BY POSITION, so selection was never wrong; what was
        // wrong is everything a HUMAN reads — an `ar t` listing, and every
        // member-read refusal, which names the member and so pointed at a file
        // that did not produce it. A diagnostic that misdirects is worse than
        // one that says nothing.
        //
        // ★ THE NAME COMES FROM THE CU, NOT FROM A STEM LIST THREADED BESIDE
        // IT. `CompilationUnit::primarySourceName()` answers from the unit's
        // own first tree; a parallel `std::vector<std::string>` would be a
        // second owner of that fact, and an index-parallel side list is exactly
        // what desynchronizes without reddening anything. See the accessor.
        //
        // ⓘ The FALLBACK is the old behaviour and is not dead: a CU with no
        // trees (or a hand-built one with no source buffer) can name nothing,
        // and `sourceStem` is still the best available answer there.
        //
        // ⓘ UNIQUENESS is now earned rather than assumed. The old `_<i>`
        // suffix made every multi-CU name distinct by construction; real stems
        // do not (`a/u.c` and `b/u.c` share one), so the suffix is applied ONLY
        // where a name would otherwise repeat — a two-source archive of
        // DIFFERENT stems now reads `u1.obj` / `u2.obj`, and one of the SAME
        // stem reads `u.obj` / `u_1.obj`. Applied to the CU-derived names
        // alone: an extracted archive member and a `--compile`d object input
        // carry the name their INPUT gave them, and renaming those would
        // misdirect in the other direction.
        auto const uniqueMemberName = [&memberNames](std::string base,
                                                     std::string const& ext) {
            auto taken = [&memberNames](std::string const& n) {
                for (auto const& m : memberNames) if (m == n) return true;
                return false;
            };
            if (!taken(base + ext)) return base + ext;
            for (std::size_t n = 1;; ++n) {
                std::string cand = base + "_" + std::to_string(n) + ext;
                if (!taken(cand)) return cand;
            }
        };
        for (std::size_t i = 0; i < cuMirs.size(); ++i) {
            // P10: a static-archive member is the FINAL module of its own
            // artifact (nothing downstream merges it — the foreign linker
            // pulls members whole), so it gets the PROGRAM-stage schedule
            // before lowering, exactly like the N==1 sole CU. Skipping this
            // would silently ship release archives optimized at the unit
            // schedule only — the archive twin of the old double-opt defect.
            if (!optimizeModule(cuMirs[i].mir, **targetR,
                                cuMirs[i].model.lattice().interner(), compileOpts,
                                PipelineStage::Program, reporter,
                                cuMirs[i].externImports)) {
                return std::nullopt;  // optimize-stage failure already reported
            }
            auto mod = lowerCuMirToAssembly(
                cuMirs[i], (*formatR)->processArgs(), (*formatR)->entryVerbs(),
                (*formatR)->sehPersonality(), (*formatR)->name(),
                cuMirs[i].target->wideFloatSoftcallLibrary(
                    (*formatR)->kind()),
                reporter);
            if (!mod) return std::nullopt;  // back-half tier failure already reported
            members.push_back(std::move(*mod));
            // Member file name: THIS CU's own source stem (see the block above),
            // uniquified only where two sources share one stem.
            std::string_view const own =
                i < cus.size() ? cus[i].primarySourceName() : std::string_view{};
            std::string stem = own.empty()
                ? std::string{sourceStem}
                : fs::path{own}.stem().string();
            if (stem.empty()) stem = std::string{sourceStem};
            memberNames.push_back(uniqueMemberName(std::move(stem), memberExt));
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
            if (!extracted) return std::nullopt;  // fail-loud already reported
            members.reserve(members.size() + extracted->modules.size());
            memberNames.reserve(memberNames.size() + extracted->names.size());
            for (std::size_t i = 0; i < extracted->modules.size(); ++i) {
                members.push_back(std::move(extracted->modules[i]));
                memberNames.push_back(std::move(extracted->names[i]));
            }
        }
        // ── D-OPT7-CROSSCU-THUNK-RESERVED-FOR-SEPARATE-COMPILATION:
        // pre-assembled OBJECT inputs become
        //    MEMBERS of this library ─────────────────────────────────────────
        //
        // ⚠ THIS ARM EXISTS BECAUSE ITS ABSENCE WAS A SILENT DROP. `objectInputs`
        // is consumed by `linkAndWriteWithStaticArchives`, and a `container:
        // archive` build never reaches that composition — it routes here. So an
        // object named on a static-library build would have been accepted,
        // ignored, and the library shipped without it: rc 0, no diagnostic, a
        // member the operator asked for simply absent. That is the exact shape
        // the fat-archive arm above was written to avoid, and it is the reason
        // an object input has to be answered on EVERY route that produces an
        // artifact rather than only on the one it was first needed for.
        //
        // Packaged, never merged: an archive bundles objects, so each object
        // input becomes its own member under its own file name, exactly as an
        // extracted archive member does. CU-derived members lead, then the input
        // archives' members, then these.
        if (!perCuOpts.objectInputs.empty()) {
            auto objects = readObjectInputModules(
                std::span<std::filesystem::path const>{perCuOpts.objectInputs},
                **targetR, **formatR, reporter);
            if (!objects) return std::nullopt;  // fail-loud already reported
            members.reserve(members.size() + objects->size());
            memberNames.reserve(memberNames.size() + objects->size());
            for (std::size_t i = 0; i < objects->size(); ++i) {
                members.push_back(std::move((*objects)[i]));
                memberNames.push_back(
                    perCuOpts.objectInputs[i].filename().string());
            }
        }
        return reported(linkAndWriteStaticArchive(members, memberNames,
                                                  **targetR, **formatR, outPath,
                                                  reporter, imageRequest));
    }

    // ── D-LK-OBJECT-INPUT-COMPILE-FLAG-SURFACE: THE ALL-OBJECT LINK ─────────
    //
    // ★★ NO CU AT ALL, WHICH IS A LEGITIMATE BUILD AND NOT AN ERROR. `dsscp
    // --compile a.o b.o --target …-exec` is `cc a.o b.o` — the operator gathered
    // only pre-assembled objects, so nothing is parsed, nothing is lowered, and
    // the whole job is the LINK. Every arm below this one starts from a lowered
    // CU module and has nothing to start from here: the `cuMirs.size() == 1`
    // arm does not fire, and the N>1 whole-program merge would fold ZERO
    // modules into one and lower the empty result.
    //
    // ★ THE FIRST OBJECT IS ELECTED CLIENT, THE REST STAY OBJECT INPUTS. The
    // link composition below is `[client, objects…, pulled…]`, and it needs a
    // client to seed the archive pull with; with no CU there is no natural one,
    // so the first object takes the role. That is a NAMING of one of the
    // inputs, never a promotion: `linkAndWriteWithStaticArchives` merges the
    // client and the object inputs by the same `mergeModules` that binds a
    // sibling CU's reference, so which object holds the seat cannot change what
    // the image contains. ⓘ It is READ HERE and the REST are passed as PATHS —
    // one read per object, never two.
    //
    // ⓘ The STATIC-ARCHIVE route above already answered this case correctly on
    // its own (zero CU-derived members, the objects packaged as members), which
    // is why this arm is below it rather than in front of it.
    if (cuMirs.empty()) {
        // Fail loud rather than write an empty image. Unreachable through the
        // shipped driver (`compileFiles`/`compileUnits` refuse an empty file
        // list, and a gathered file that is not an object stays a source), so
        // this is the net under a future gathering route, not a live arm.
        if (perCuOpts.objectInputs.empty()) {
            emitDriver(reporter, DiagnosticCode::D_EmptyInput,
                       "nothing to compile or link for target '" + targetSpecStr
                       + "': the build produced no compilation unit and named "
                         "no pre-assembled object input.");
            return std::nullopt;
        }
        std::span<std::filesystem::path const> const objectPaths{
            perCuOpts.objectInputs};

        // ── WHICH OBJECT HOLDS THE PROGRAM ENTRY, AND WHY IT MUST BE ASKED BY
        //    NAME ───────────────────────────────────────────────────────────
        //
        // ★★★ A RELOCATABLE OBJECT CANNOT RECORD `userEntrySymbol`, AND THAT IS
        // A FACT ABOUT THE FORMAT RATHER THAN A GAP IN THE READER. That field
        // is a per-CU `SymbolId` — an arena-local integer minted by the compile
        // that produced the module — and no object format has a place to put
        // one. So a module read back from an object arrives with it unset,
        // ✔MEASURED: before this arm `--compile u1.obj u2.obj --target
        // …-exec` reached the entry trampoline with two functions, no
        // `userEntrySymbol` and an empty format `entryPoint`, and failed loud
        // with `K_SymbolUndefined`. Every ordinary build sidesteps this because
        // its client is a CU the pipeline stamped.
        //
        // ★★ THE NAME IS THE FORMAT-LEVEL IDENTITY, and it is the one the merge
        // ALREADY uses — `ModuleSymbol::name` is the cross-module match key
        // precisely because `SymbolId` is not portable across units. Resolving
        // the entry the same way is reuse, not a parallel mechanism.
        //
        // ★ THE CANDIDATE SET IS THE DECLARED INTERSECTION, not a `"main"`
        // literal: the LANGUAGE's `entryFunctions` rows supply the names, the
        // FORMAT's `entryVerbs` set says which of those verbs it can realize,
        // and the format's own C decoration spells the on-binary form. That is
        // the same two-owner intersection `entry_shape.hpp` documents and the
        // same `applyCMangling` the merge keys on (`_main` on Mach-O, identity
        // on ELF/PE) — so `wmain` is a candidate on a Windows image and on
        // nothing else, with no format identity tested anywhere here.
        //
        // ⚠ NAME ONLY, NEVER THE SIGNATURE. The CU route intersects the
        // signature too (`collectEntryCandidates` reads each SemanticModel);
        // an object carries no prototype, so that half is simply not knowable
        // here. The name+verb intersection is what the format itself can see.
        std::vector<std::string> entryNames;
        {
            CSymbolDecorationScheme const scheme =
                (*formatR)->cSymbolDecoration().scheme;
            for (auto const& decl : grammar.semantics().declarations) {
                for (auto const& row : decl.entryFunctions) {
                    if (!(*formatR)->realizesEntryVerb(row.verb)) continue;
                    std::string mangled =
                        dss::ffi::applyCMangling(row.name, scheme);
                    if (std::find(entryNames.begin(), entryNames.end(), mangled)
                        == entryNames.end()) {
                        entryNames.push_back(std::move(mangled));
                    }
                }
            }
        }
        // Does this module DEFINE one of the candidates as a function? A
        // `ModuleSymbol` row alone is not enough — the readers mint one for
        // data and for section symbols too — so the row's `SymbolId` has to
        // name an actual `AssembledFunction`, which is what the trampoline will
        // call.
        auto definedEntryOf =
            [&entryNames](AssembledModule const& m) -> std::optional<SymbolId> {
            for (auto const& ms : m.symbols) {
                if (std::find(entryNames.begin(), entryNames.end(), ms.name)
                    == entryNames.end()) continue;
                for (auto const& fn : m.functions) {
                    if (fn.symbol.v == ms.symbol.v) return ms.symbol;
                }
            }
            return std::nullopt;
        };

        // ★ THE CLIENT IS THE OBJECT THAT DEFINES THE ENTRY, and the election
        // is what makes the choice mean something. `linkAndWriteWithStaticArchives`
        // composes `[client, objects…, pulled…]` and the client is the module
        // whose `userEntrySymbol` survives the merge, so electing the FIRST
        // object unconditionally would have made `--compile helper.o main.o`
        // and `--compile main.o helper.o` two different programs.
        //
        // ⓘ ONE READ PER OBJECT. The probe stops at the winner and keeps its
        // module, so the common case (`main.o` named first) reads one object
        // here and the rest exactly once more inside the link. Object 0 is
        // retained on the way past so the no-entry fallback costs no re-read.
        std::optional<AssembledModule> client;
        std::optional<AssembledModule> firstObject;
        std::optional<SymbolId>        entrySymbol;
        std::size_t                    clientIndex = 0;
        for (std::size_t oi = 0; oi < objectPaths.size(); ++oi) {
            auto one = readObjectInputModules(objectPaths.subspan(oi, 1),
                                              **targetR, **formatR, reporter);
            if (!one) return std::nullopt;  // fail-loud already reported
            if (auto sym = definedEntryOf((*one)[0])) {
                entrySymbol = sym;
                clientIndex = oi;
                client      = std::move((*one)[0]);
                break;
            }
            if (oi == 0) firstObject = std::move((*one)[0]);
        }
        if (!client) {
            // ★ NO OBJECT DEFINES AN ENTRY THE FORMAT CAN REALIZE — and when
            // the language DID name some, that is a refusal this arm owes the
            // operator IN ITS OWN WORDS. Falling through would reach the
            // trampoline, whose message talks about a synthesized `sym_<id>`
            // convention and a SymbolId encoded in the format's `entryPoint`
            // — true of the mechanism, and about nothing the operator can act
            // on: they named objects, and none of them has a `main`. gcc,
            // clang and MSVC all refuse the same link naming the same fact, so
            // this is the union's answer and not an invented strictness.
            //
            // ⓘ GATED ON A NON-EMPTY CANDIDATE SET. With no `--language` the
            // resolved grammar is the target's assembly dialect, which
            // declares no entry rows at all — there is then no claim to make
            // about a missing `main`, and the trampoline's own single-function
            // fallback stays the answer, unchanged.
            if (!entryNames.empty() && (*formatR)->isImageFlavor()) {
                std::string names;
                for (auto const& n : entryNames) {
                    if (!names.empty()) names += ", ";
                    names += '\'' + n + '\'';
                }
                std::string objs;
                for (auto const& p : objectPaths) {
                    if (!objs.empty()) objs += ", ";
                    objs += core::genericSpelling(p);
                }
                emitDriver(reporter, DiagnosticCode::K_SymbolUndefined,
                           "no program entry: none of the object inputs ("
                           + objs + ") defines a function named "
                           + names + " — the entry name(s) language '"
                           + std::string{grammar.configName()}
                           + "' declares that format '"
                           + std::string{(*formatR)->name()}
                           + "' can enter through. An object records no entry "
                             "of its own, so the link has nothing to call.");
                return std::nullopt;
            }
            // The FIRST object is elected and left unstamped, which preserves
            // the trampoline's own single-function fallback and, for anything
            // else, its existing loud refusal — this arm neither invents an
            // entry nor swallows the refusal.
            client      = std::move(firstObject);
            clientIndex = 0;
        }
        if (entrySymbol.has_value()) client->userEntrySymbol = entrySymbol;

        // Everything except the elected client, IN THE OPERATOR'S ORDER.
        std::vector<std::filesystem::path> remaining;
        remaining.reserve(objectPaths.size() - 1);
        for (std::size_t oi = 0; oi < objectPaths.size(); ++oi) {
            if (oi != clientIndex) remaining.push_back(objectPaths[oi]);
        }
        return reported(linkAndWriteWithStaticArchives(
            std::move(*client),
            std::span<std::filesystem::path const>{remaining},
            std::span<std::filesystem::path const>{staticArchives},
            std::span<ResolveLibrarySpec const>{perCuOpts.resolveLibraries},
            **targetR, **formatR, outPath, reporter, imageRequest));
    }

    // N==1 (the CU5 multi-file-single-CU case): lower the sole CU + link it. UNCHANGED
    // from cycle 24 in its LOWERING — routing N==1 through the merge would re-intern
    // CU0's types into a fresh host (a no-op for correctness, but extra work + a
    // different code path). P10 adds the PROGRAM-stage optimize the topology requires:
    // the sole CU's module IS a final module (nothing downstream merges it), so it gets
    // the link-time schedule here exactly as the N>1 merged module does above — no
    // driver `if` on stage content, the site exists and the config decides what runs.
    if (cuMirs.size() == 1) {
        if (!optimizeModule(cuMirs[0].mir, **targetR,
                            cuMirs[0].model.lattice().interner(), compileOpts,
                            PipelineStage::Program, reporter,
                            // this CU's extern table; lowerCuMirToAssembly moves
                            // it into MIR→LIR only after this call returns.
                            cuMirs[0].externImports)) {
            return std::nullopt;  // optimize-stage failure already reported
        }
        auto mod = lowerCuMirToAssembly(
            cuMirs[0], (*formatR)->processArgs(), (*formatR)->entryVerbs(),
            (*formatR)->sehPersonality(), (*formatR)->name(),
            cuMirs[0].target->wideFloatSoftcallLibrary((*formatR)->kind()),
            reporter);
        if (!mod) {              // back-half tier failure already reported via `reporter`
            emitNullNoDiagnostic("back-half lower (lowerCuMirToAssembly)");
            return std::nullopt;
        }
        // c165 (D-LK-STATIC-LINK): link against any `ar` static archives named on
        // `--resolve-library` (pull the referenced members + merge them in). With
        // no static archives this is `linkAndWrite({mod})`, unchanged.
        // D-LK-ARCHIVE-MEMBER-EXTERN-CANNOT-BIND-A-RESOLVE-LIBRARY: the DYNAMIC
        // half rides along so a pulled member's extern can rebind to a library
        // the operator named (`perCuOpts` — archives already partitioned out).
        return reported(linkAndWriteWithStaticArchives(
            std::move(*mod),
            std::span<std::filesystem::path const>{perCuOpts.objectInputs},
            std::span<std::filesystem::path const>{staticArchives},
            std::span<ResolveLibrarySpec const>{perCuOpts.resolveLibraries},
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

    // ── THE THIN-LTO PER-TU IMPORT STAGE (D-OPT11-LAZY-IMPORT-EDGE) ─────────
    //
    // This is the ONE point in the process where all N independent per-TU
    // modules co-exist, each still owning its own lattice, with nothing
    // collapsed — which is exactly what the summary index was shaped to consume.
    // OFF unless `--lto=thin`, and STRICTLY ADDITIVE when on: the whole-program
    // merge below still runs, so the stage can only move cross-CU inlining
    // earlier and into parallel.
    if (compileOpts.ltoMode == CompileOptions::LtoMode::Thin) {
        // The same executor discipline the two existing CU batches use: the
        // injected one when a caller supplied it (tests, a shared pool),
        // otherwise a local pool sized by the SAME `resolveCuPoolWidth` — so
        // `--jobs` means one thing across every batch in this build.
        std::optional<substrate::ThreadPool> thinPool;
        substrate::IExecutor* thinExec = injectedExecutor;
        if (thinExec == nullptr) {
            thinPool.emplace(resolveCuPoolWidth(cuMirs.size(), jobsOverride));
            thinExec = &*thinPool;
        }
        if (!runThinLtoImportStage(
                std::span<CuMirModule>{cuMirs.data(), cuMirs.size()}, **targetR,
                cSymDecor, targetSpecStr, compileOpts, thinExec, reporter)) {
            return std::nullopt;   // already reported
        }
    }

    std::vector<MergeCuInput> mergeInputs;
    mergeInputs.reserve(cuMirs.size());
    for (auto& cuMir : cuMirs) {
        MergeCuInput in;
        in.mir = &cuMir.mir;
        // ⚠ AFTER A THIN IMPORT THIS CU'S TYPES LIVE IN A DIFFERENT LATTICE.
        // Reading `model.lattice()` here would hand the merge an interner that
        // does not own the module's TypeIds — a reintern against the wrong
        // lattice, which is a silently retyped module rather than an error.
        in.interner = cuMir.usesImportedLattice
                          ? &cuMir.importedHost->interner()
                          : &cuMir.model.lattice().interner();
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
            // D-OPT11-LAZY-IMPORT-EDGE: after a thin import the SemanticModel's
            // symbol ids no longer describe this module — the import merge
            // renumbered them — so the imported table is authoritative and is
            // consulted FIRST. It was built from this very lambda's rule, so the
            // key is the same string either way.
            if (cuMirP->usesImportedLattice) {
                auto const it = cuMirP->importedSymbolNames.find(s.v);
                return it == cuMirP->importedSymbolNames.end() ? std::string{}
                                                               : it->second;
            }
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
    // language (cosmetic — the registry's sourceLanguage tags extension types; c
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
    if (!entryOk) return std::nullopt;   // undefined / ambiguous entry — already reported.

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
    if (!merged) return std::nullopt;  // merge failure (conflict / verify) already reported.

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
        return std::nullopt;  // unusable mechanism — fail-loud already reported.
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
    // runtime witness, `examples/c/shipped_sprintf_ucrt_crosscu` (exit 42, release
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
                return std::nullopt;
            }
            switch (*family) {
            case dss::ffi::ShimFamily::Threads: mergedThreadsRecipes.emplace(symV, bare); break;
            case dss::ffi::ShimFamily::Stdio:   mergedStdioRecipes.emplace(symV, bare);   break;
            }
        }
        if (!synthesizeThreadsShim(merged->mir, merged->host.interner(),
                                   mergedThreadsRecipes, (*formatR)->librarySynthesis(),
                                   cSymDecor, merged->externImports, reporter)) {
            return std::nullopt;  // internal invariant breach (vocab/switch drift) — reported.
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
            return std::nullopt;  // recipe/helper-import/va-strategy mismatch — reported.
        }
    }

    // Cycle 26 (D-OPT7-1): optimize the WHOLE-PROGRAM merged module with the configured
    // pipeline. The merge made every cross-CU call an intra-module DIRECT call, so the
    // inliner's `symToFunc` now resolves the callee — a cross-CU call becomes inline-
    // eligible exactly like an intra-CU one. `merged->host.interner()` is the type space
    // the merged TypeIds index into (the same interner `lowerMergedToAssembly` uses). The
    // optimizer runs MirVerifier after every pass (the merged-module safety net).
    //
    // P10 (D-OPT7-CROSSCU-LTO-SINGLE-OPTIMIZE, CLOSED): this is the PROGRAM stage — the
    // link-time schedule of the two-stage topology. The per-CU UNIT stage already ran in
    // `buildCuMir` (its schedule is the document's `unitPipeline`), so "double-opt" is
    // now TWO DIFFERENT pipelines by design, exactly like a per-TU -O2 compile followed
    // by a link-time pipeline — never the same 9×4 list twice. The examples-runner's
    // `["Inlining"]` override still flows here via `compileOpts.pipelineOverride`
    // (an override runs at every site).
    if (!optimizeModule(merged->mir, **targetR, merged->host.interner(),
                        compileOpts, PipelineStage::Program, reporter,
                        // D-CSUBSET-INLINE-FUNCTION-NO-EXTERNAL-DEFINITION-EMITTED:
                        // the MERGED module's extern table.
                        //
                        // ★★ THIS CALL IS A GUARANTEED NO-OP, AND KNOWING WHY IS
                        // WHAT MAKES IT SAFE TO MAKE. The strip drops a function
                        // whose SymbolId is also an extern's — so the question
                        // that matters here is whether a merged module can pair a
                        // GENUINE cross-CU definition with a sibling CU's `extern`
                        // declaration of it. If it could, this call would delete a
                        // real definition. It cannot: `mergeCuMirs` step 6 skips
                        // every extern row whose `mangledName` is in
                        // `plan.definedNames` ("→ direct, strip"), having already
                        // rewired those calls to direct in step 4. So by the time
                        // the merged module exists, an extern row and a defining
                        // function of the same symbol never co-exist. ✔MEASURED
                        // besides: `extern int f(void);` in cu_a with `int f(void)
                        // {…}` in cu_b links and runs correctly at BOTH configs.
                        //
                        // ⇒ It is passed anyway, deliberately. The per-CU optimize
                        // is what actually strips these bodies, and "the other one
                        // already did it" is exactly the assumption that ships a
                        // body when it did not. A no-op that costs one empty-set
                        // scan is the right price for not having to re-derive that
                        // argument the next time the merge changes.
                        merged->externImports)) {
        return std::nullopt;  // optimize / verify failure already reported via `reporter`
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
        return std::nullopt;  // unsupported SEH shape (c116b frontier) — fail-loud reported.
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
                                     effectiveUnnamedBitFieldAlignment(**targetR,
                                                                       **formatR),
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
    if (!mod) return std::nullopt;  // back-half tier failure already reported via `reporter`
    // c165 (D-LK-STATIC-LINK): the merged whole-program client module links
    // against any `ar` static archives named on `--resolve-library` the same way
    // the single-CU path does (pull referenced members + merge). No archives =>
    // `linkAndWrite({mod})`, unchanged.
    // D-LK-ARCHIVE-MEMBER-EXTERN-CANNOT-BIND-A-RESOLVE-LIBRARY: the DYNAMIC half
    // rides along here TOO. This is the THIRD of three routes to this call, and
    // all three must answer — a route that kept the old argument list would
    // silently keep the gap for whichever builds happen to take it.
    return reported(linkAndWriteWithStaticArchives(
        std::move(*mod),
        std::span<std::filesystem::path const>{perCuOpts.objectInputs},
        std::span<std::filesystem::path const>{staticArchives},
        std::span<ResolveLibrarySpec const>{perCuOpts.resolveLibraries},
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
    // ⚠⚠ THE **CONFIG** NAME (`GrammarSchema::configName()`, the `.lang.json`
    // stem), NOT `name()`. This member decides whether two targets SHARE ONE CU
    // BUILD, so two grammars that compare equal here are handed the same parsed
    // source. The stem is unique BY CONSTRUCTION — two documents in one
    // `sources/` directory cannot share a filename — whereas `language.name` is
    // a declared field under no uniqueness rule at all: two documents declaring
    // the same name would silently MERGE two builds, and a `.s` parsed under one
    // dialect would be compiled for the other CPU. That is the same silent
    // miscompile TF-C74 widened this key to prevent, reachable through a
    // one-word config edit. ✔MEASURED 2026-08-25 that the swap changes NO
    // grouping on today's corpus: both columns are injective over the six
    // shipped documents, so "same declared name" and "same stem" partition them
    // identically — pinned, so it cannot quietly stop being true, by
    // `ShippedCorpusStemsAndDeclaredNames*` in `tests/core/test_grammar_schema`.
    //
    // ⓘ AND THE ONE WAY THE TWO CAN STILL DISAGREE ERRS SAFELY. On a
    // case-insensitive host, two targets declaring the SAME document under two
    // spellings (`asm-x86_64-att` / `Asm-X86_64-Att`) resolve to one file but
    // two stems, so this key SPLITS what `name()` would have merged: one extra
    // identical CU build, never a shared one. Over-splitting costs time;
    // over-merging hands one target another's parsed source.
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
//      uses. A caller who says `--language c` and hands over a `.s` has
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

// ══ D-DRIVER-SHIPPED-SOURCE-RESOLUTION-COMPILES-EVERY-SHIPPED-GRAMMAR ════════
//
// ★★★ THE QUESTION IS ONE FIELD WIDE, SO THE READ IS ONE FIELD WIDE.
// `resolveShippedSourceGrammar` asks every shipped language document a single
// yes/no question — "does your `language.fileExtensions` contain this
// extension?" — and it used to answer it by CONSTRUCTING each document's entire
// grammar: tokens, shapes, FIRST/FOLLOW, ambiguity analysis, semantics and HIR
// lowering, for every shipped language, on the way to compiling one `.c` file.
//
// ✔MEASURED at 5085664a (Windows x86_64, Debug), compiling
// `int main(void){return 0;}` for pe64: THIRTEEN `GrammarSchema::loadShipped`
// calls, 2,068,338 bytes of JSON, 211 ms inside `buildSchemaFromJsonText` — of
// which exactly ONE construction was ever used to parse anything. The whole
// corpus was built TWICE because the resolver runs once per realized shipped
// unit and this build realizes two. The file READ across all thirteen was 1 ms
// and path discovery 2 ms, so the cost was never I/O: it was five grammars
// nobody asked for, built twice, thrown away.
//
// ⚠ A CACHE IS NOT THE FIX AND DOES NOT SUBSTITUTE FOR IT. Memoizing a load
// makes the SECOND pointless construction cheap; the first one still happens,
// in every process, for every shipped language. The two compose — this seam
// stops asking, a loader memo makes any remaining ask cheap — and neither makes
// the other redundant. ✔MEASURED in this build tree WITH the document memo
// present: `--time` reports `build-config` runs 9 -> 3 and `load-config` runs
// 18 -> 7 across this change alone.

// ── THE ONE FIELD, READ WITHOUT BUILDING A GRAMMAR ───────────────────────────
//
// ★★ WHY A SHALLOW READ IS FAITHFUL AND NOT AN APPROXIMATION. In
// `buildSchemaFromJsonText`, `GrammarSchemaData::fileExtensions` is filled from
// the HOST document's own `language` block, and it is filled UPSTREAM of the
// `languageReferences` merge — which folds only `shapes`, `semantics`,
// `hirLowering` and `pipelineEntry` from a referenced document. No referenced
// document can add, remove or rewrite an extension, and no later pass touches
// the vector. So the array in the file IS the answer a constructed schema would
// have given.
//
// ★ A SAX READER, NOT A DOM PARSE, AND THE DIFFERENCE IS THE POINT: it ABORTS
// the instant the `language` block closes, so it reads the first few hundred
// bytes of a 510 KB document instead of lexing all of it and building a DOM to
// throw away. `language` is the first or second key of every shipped document;
// one that buries it deeper still gets the RIGHT answer, just later.
//
// AGNOSTIC: the two strings below are SCHEMA VOCABULARY — the block and field
// names `grammar_schema_json.cpp` reads — never a language NAME. No language
// name is spelled anywhere in this file.
constexpr std::string_view kLanguageBlockKey  = "language";
constexpr std::string_view kFileExtensionsKey = "fileExtensions";

class LanguageBlockExtensionReader {
  public:
    using number_integer_t  = nlohmann::json::number_integer_t;
    using number_unsigned_t = nlohmann::json::number_unsigned_t;
    using number_float_t    = nlohmann::json::number_float_t;
    using string_t          = nlohmann::json::string_t;
    using binary_t          = nlohmann::json::binary_t;

    [[nodiscard]] std::vector<std::string> const& extensions() const noexcept {
        return extensions_;
    }

    bool null()                             { return true; }
    bool boolean(bool)                      { return true; }
    bool number_integer(number_integer_t)   { return true; }
    bool number_unsigned(number_unsigned_t) { return true; }
    bool number_float(number_float_t, string_t const&) { return true; }
    bool binary(binary_t&)                  { return true; }

    // Only DIRECT string elements of the array are taken, which is exactly what
    // the full loader does (`if (ext.is_string())`) — a nested array or an
    // object inside `fileExtensions` contributes nothing on either path.
    bool string(string_t& value) {
        if (collecting_ && depth_ == collectDepth_) extensions_.push_back(value);
        return true;
    }

    bool key(string_t& k) { lastKey_ = k; return true; }

    bool start_object(std::size_t) {
        ++depth_;
        if (depth_ == 2 && lastKey_ == kLanguageBlockKey) inLanguage_ = true;
        lastKey_.clear();
        return true;
    }

    // ★ THE ABORT. Returning false from any SAX callback stops the parse where
    // it stands; nlohmann reports that by returning false from `sax_parse`,
    // which is indistinguishable from a parse error at the call site and does
    // not need to be distinguished — see `declaredFileExtensionsOf`.
    bool end_object() {
        if (depth_ == 2 && inLanguage_) return false;
        --depth_;
        lastKey_.clear();
        return true;
    }

    bool start_array(std::size_t) {
        ++depth_;
        if (!collecting_ && inLanguage_ && depth_ == 3
            && lastKey_ == kFileExtensionsKey) {
            collecting_   = true;
            collectDepth_ = depth_;
        }
        lastKey_.clear();
        return true;
    }

    bool end_array() {
        if (collecting_ && depth_ == collectDepth_) collecting_ = false;
        --depth_;
        lastKey_.clear();
        return true;
    }

    bool parse_error(std::size_t, std::string const&,
                     nlohmann::detail::exception const&) {
        return false;
    }

  private:
    std::vector<std::string> extensions_;
    std::string              lastKey_;
    int                      depth_        = 0;
    int                      collectDepth_ = 0;
    bool                     inLanguage_   = false;
    bool                     collecting_   = false;
};

// `doc`'s own `language.fileExtensions`, ASCII-lowercased, or an empty vector.
//
// ⚠ DIAGNOSTIC-FREE, exactly like `readShippedSourcesForFormat`, and for the
// same reason: a document's HEALTH is the loader's own business, and this reads
// documents it has no intention of loading. A file that is missing, unreadable
// or malformed BEFORE its `language` block simply declares nothing here — which
// is the same answer the old code reached by loading it and discarding the
// failure, so a broken sibling document never became a claimant then and does
// not now.
[[nodiscard]] std::vector<std::string> declaredFileExtensionsOf(fs::path const& doc) {
    std::ifstream in{doc, std::ios::binary};
    if (!in) return {};
    LanguageBlockExtensionReader reader;
    // The return value is DELIBERATELY ignored: `false` means either "we aborted
    // on purpose at the end of the `language` block" (the fast path, every
    // well-formed document) or "the document is malformed". Both are answered by
    // what the reader collected before stopping.
    (void)nlohmann::json::sax_parse(in, &reader, nlohmann::json::input_format_t::json,
                                    /*strict=*/false);
    std::vector<std::string> out;
    out.reserve(reader.extensions().size());
    for (auto const& e : reader.extensions()) out.push_back(asciiLowerCopy(e));
    return out;
}

// The per-invocation memo for the shipped-source units' extension⇒language
// resolution. TWO fields, and they answer different questions:
//
//   * `claimantsByExtension` — WHICH language documents declare each extension.
//     Read off the documents ONCE per invocation (the whole `sources/` corpus
//     in one directory walk), because it is a property of the config tree, not
//     of the file being resolved. Every additional realized unit is then a map
//     lookup.
//   * `grammarByName` — the grammars actually CONSTRUCTED, i.e. only the ones a
//     realized unit is genuinely compiled under. This is the field the old
//     signature had; it was written to after every load and never read before
//     one, so it memoized nothing.
//
// `indexedDir` is the tree the index was read from, not a bool: an empty path
// means "not built", and a DIFFERENT path rebuilds. Assuming discovery is
// stable within one invocation would be an unstated assumption, and this is
// cheaper than the assumption.
struct ShippedSourceLanguageCache {
    std::map<std::string, std::vector<std::string>>             claimantsByExtension;
    std::map<std::string, std::shared_ptr<GrammarSchema const>> grammarByName;
    fs::path                                                    indexedDir;
};

// Which shipped LANGUAGE claims `path`'s extension — the extension⇒language
// resolution the shipped-source units need, and the ONE place it lives.
//
// ★ THIS IS WHY THERE IS NO LANGUAGE SEGMENT IN THE RUNTIME TREE AND NO UNIT
// MANIFEST. A language document declares its own `"fileExtensions"`, and `.c`
// is claimed by exactly one shipped language; restating "these files are C" in
// a path segment or a JSON key would be a second owner of a fact the language
// configs already hold, free to drift from them the moment either side is
// edited. The same rule is why the index above is read from the DOCUMENTS on
// every run and never written to a side file: a generated index is a second
// owner too, and this repository treats staleness as a defect class.
//
// ⚠ ZERO claimants and TWO claimants both fail LOUD, and the second is not
// hypothetical: `.s`/`.S` is claimed by BOTH shipped asm dialects, so a
// hand-written assembly runtime unit — soft-float helpers, setjmp/longjmp
// bodies, the classic contents of this tier — is genuinely ambiguous by
// extension. It would need the ARCH to disambiguate, not the language, and the
// realization key is FORMAT-keyed, so it is a different shape than anything
// here. Refusing is correct today; building the arch axis now would be the
// speculative structure the bar rules out.
//
// ⚠ ONE BEHAVIOUR MOVED, DELIBERATELY. "Health is the loader's own business"
// still holds for every document this resolution does NOT choose — they are no
// longer constructed at all, so their health cannot affect anything. It no
// longer holds for the ONE document it DOES choose: a sole claimant that fails
// to load now fails LOUD with the loader's own reason. The old code dropped it
// silently and then reported "no shipped language claims the extension '.c'",
// which is not what happened — the same misattribution class as reporting an
// I/O failure as a missing file, and this file already refuses that one a few
// hundred lines below.
//
// AGNOSTIC: no language NAME is compared against a literal — the config
// directory supplies the candidates and the match is set membership over
// config-declared strings.
// ★★★ BOTH HALVES ARE RETURNED, AND THE SECOND ONE IS THE BUG FIX.
// A language has TWO names: the one its document DECLARES (`language.name`,
// "C") and the one the config tree is INDEXED BY (the file stem, "c"). They are
// not required to agree and today they do not.
//
// ⚠⚠ AND THEY DIVERGE BY MORE THAN CASE — reading this as a case problem
// understates the class by half. ✔MEASURED over the shipped corpus 2026-08-25:
// of the six language documents five are nameable (the sixth, `asm`, is
// embedded-only), and of those five, `c`→"C" and `toy`→"Toy" differ only in
// case while `asm-arm64-gas`→"AsmArm64Gas", `asm-x86_64-att`→"AsmX86_64Att" and
// `tsql-subset`→"TsqlSubset" differ STRUCTURALLY — the stem's hyphens are
// absent from the declared name. Passing the declared name where a stem was
// meant fails on EVERY host for those three: `sources/AsmArm64Gas.lang.json`
// does not exist on NTFS either.
//
// ⚠ THIS RESOLVER USED TO RETURN ONLY THE GRAMMAR, so its caller had to recover
// a name — and the only name reachable from a `GrammarSchema` is the DECLARED
// one. The nested runtime build was therefore invoked as `--language C` and
// looked up `sources/C.lang.json`. That is the CASE-ONLY pair, which is why it
// reached production at all: the same file on NTFS, NO FILE on ext4. The
// Windows gate was 1656/1656 green while the WSL leg lost 527 tests to
// `error[C_InvalidLanguageName]`. A host-blind failure would have been caught
// by the first gate that ran.
// ⇒ the stem this function ALREADY derived and ALREADY loaded by is now part of
//   its answer, so no caller can re-derive it wrongly.
struct ShippedSourceLanguage {
    // The name the CONFIG TREE knows this language by — the `.lang.json` stem.
    // This is what `--language` takes and what a document path is built from.
    std::string                          configName;
    std::shared_ptr<GrammarSchema const> grammar;

    [[nodiscard]] explicit operator bool() const { return grammar != nullptr; }
};

[[nodiscard]] ShippedSourceLanguage resolveShippedSourceGrammar(
    fs::path const&             path,
    ShippedSourceLanguageCache& cache,
    DiagnosticReporter&         rep) {
    std::string const ext = asciiLowerCopy(path.extension().generic_string());

    auto const sourcesDir = findShippedConfigDir("sources");
    if (!sourcesDir) {
        emitDriver(rep, DiagnosticCode::D_SchemaLoadFailed,
                   "shipped-source realization: the shipped language directory "
                   "(src/dss-config/sources) could not be located, so '"
                   + core::genericSpelling(path) + "' has no front end to compile it");
        return {};
    }

    if (cache.indexedDir != *sourcesDir) {
        cache.claimantsByExtension.clear();
        std::error_code ec;
        for (fs::directory_iterator it{*sourcesDir, ec}, end; it != end;
             it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            std::string const leaf = it->path().filename().generic_string();
            auto const        dot  = leaf.find(".lang.json");
            if (dot == std::string::npos) continue;
            std::string const name = leaf.substr(0, dot);
            for (auto const& declared : declaredFileExtensionsOf(it->path())) {
                auto& claimants = cache.claimantsByExtension[declared];
                // A document that declares one extension twice is ONE claimant.
                if (std::find(claimants.begin(), claimants.end(), name)
                    == claimants.end())
                    claimants.push_back(name);
            }
        }
        // Sorted so the claimant set is a property of the CORPUS and not of the
        // host filesystem's iteration order (sorted on NTFS, hash-ordered on
        // ext4).
        for (auto& entry : cache.claimantsByExtension)
            std::sort(entry.second.begin(), entry.second.end());
        cache.indexedDir = *sourcesDir;
    }

    static std::vector<std::string> const kNoClaimants;
    auto const  found = cache.claimantsByExtension.find(ext);
    auto const& claimants =
        found == cache.claimantsByExtension.end() ? kNoClaimants : found->second;

    if (claimants.size() == 1) {
        std::string const& name = claimants.front();
        auto               got  = cache.grammarByName.find(name);
        if (got == cache.grammarByName.end()) {
            auto loaded = GrammarSchema::loadShipped(name);
            if (!loaded.has_value()) {
                forwardConfigDiagnostics(loaded.error(), rep);
                emitDriver(rep, DiagnosticCode::D_SchemaLoadFailed,
                           "shipped-source realization: shipped language '" + name
                           + "' is the only one that claims the extension '" + ext
                           + "' of '" + core::genericSpelling(path)
                           + "', but it could not be loaded — the reason is in the "
                             "configuration diagnostic(s) above (config: "
                             "src/dss-config/sources/" + name + ".lang.json)");
                // Not memoized: a load failure is fatal for this run anyway, and
                // caching it would make a future retry-after-fix answer from a
                // stale miss — the rule `resolveGrammarForTarget` already states.
                return {};
            }
            // ⓘ `loadDiagnostics()` are NOT forwarded on success here, and that
            // is the same one-emission-per-document-per-run rule
            // `resolveGrammarForTarget` documents: it forwards them for every
            // language a TARGET resolves, and a shipped unit compiled under that
            // language would otherwise report each warning a second time.
            got = cache.grammarByName.emplace(name, *loaded).first;
        }
        // `name` is the STEM the index was built from and the grammar was loaded
        // by — never `got->second->name()`, which is the declared name.
        return {name, got->second};
    }

    emitDriver(rep, DiagnosticCode::D_UnknownFileExtension,
               claimants.empty()
                   ? "shipped-source realization: no shipped language claims the "
                     "extension '" + ext + "' of '" + core::genericSpelling(path)
                         + "', so there is no front end to compile it"
                   : "shipped-source realization: " + std::to_string(claimants.size())
                         + " shipped languages claim the extension '" + ext
                         + "' of '" + core::genericSpelling(path)
                         + "', so the extension alone cannot name one — refusing "
                           "rather than guessing which front end owns this file");
    return {};
}

// ══ D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF — THE SHIPPED RUNTIME, ════════
//    COMPILED ONCE AND THEN READ OUT OF A CONTENT-ADDRESSED CACHE ════════════
//
// DSS SHIPS THE SOURCE. A descriptor's per-format `realization` map may state
// that a symbol's body is PROVIDED by a file the compiler ships rather than
// imported from a platform image — `opendir` on Windows, which no image
// exports because Windows has no POSIX directory API. This is the seam that
// turns that declaration into a build-graph edge.
//
// ★★★ THE UNIT USED TO BE AN EXTRA TRANSLATION UNIT OF EVERY BUILD, AND THAT IS
// WHAT THIS SECTION REPLACES. ✔MEASURED 2026-08-25 on this host, Release, one
// `int main(void){return 0;}`, the SAME binary and source with only `--target`
// changing: pe64 took 190 ms against 79 ms for elf64 and macho64, and the whole
// difference is the two units `realization.pe` names — PREPROCESSED AND PARSED
// on every single invocation (`preprocess`/`parse` `runs` = 3 on pe64, 1
// elsewhere) and then, for any program that does not reach them, DISCARDED. For
// scale, gcc 13.2.0 compiles the same file in 95.1 ms on this host and never
// compiles libc at all: it links a PREBUILT one. `runtime_object_cache.hpp` is
// DSS's equivalent of that prebuilt libc, and this is where it is wired.
//
// ★★★ HIT AND MISS MUST PRODUCE THE SAME IMAGE, AND THAT IS THE PROPERTY THE
// WHOLE SHAPE IS CHOSEN FOR. The route below is taken UNCONDITIONALLY: the
// runtime reaches the link as a STATIC ARCHIVE whether the archive was found in
// the cache or built one line earlier. The alternative — merge the units as CUs
// on a miss, pull archive members on a hit — would make a cold build and a warm
// build emit DIFFERENT code from identical inputs, so a green test could go red
// purely by being run twice. A cache whose hit rate changes the artifact is not
// a cache.
//
// ⚠ WHAT THIS CHANGES ABOUT THE EMITTED CODE, STATED RATHER THAN LEFT TO BE
// DISCOVERED. The units are no longer folded into the whole-program MIR merge
// alongside the user's CUs; they are lowered against the format's ARCHIVE
// SIBLING and pulled back at link time by `pullStaticArchiveMembers`. So there
// is no cross-module MIR inlining between a user function and `opendir`'s body,
// and member selection is now by SYMBOL REFERENCE (the armap worklist) rather
// than by "did this build resolve that descriptor". Symbol-level selection is
// strictly the finer of the two, and it is the mechanism the cache header
// already named: *COMPILE-ALWAYS IS NOT LINK-ALWAYS* — what is COMPILED is a
// pure function of (target, config), what is LINKED stays demand-driven.
//
// ★ THE ENGINE NEVER BRANCHES ON FORMAT. `allShippedSourcesForFormat` is a map
// lookup keyed by the active format's declared KIND NAME; there is no
// `if (format == "pe")` on this path. A build for a format no descriptor
// realizes from source gets an EMPTY list and never resolves an archive
// sibling, a cache root or a key.
//
// ★ THE RUNTIME COMPILE IS HERMETIC. The nested build below is a DEFAULT
// `Program` carrying only the build configuration — deliberately WITHOUT the
// user's `-I` dirs, `--define`s, optimizer overrides and `--resolve-library`
// list. DSS's runtime must mean the same thing in every program that links it;
// a user `-DNDEBUG` or a stray `-I` that shadowed a shipped header would
// silently compile someone else's runtime into their binary. It is also what
// makes the cache key HONEST: the key covers (compiler, target, format,
// sibling, config, unit bytes, descriptor bytes, loaded config documents), so
// anything else that could change the output must not be able to reach it.

// ★★★ THE RE-ENTRANCY GUARD, AND IT IS LOAD-BEARING RATHER THAN DEFENSIVE.
// The runtime archive is produced by a NESTED `Program` build aimed at the
// archive-writing SIBLING of this format — and the sibling has the SAME format
// KIND, so `allShippedSourcesForFormat` answers that nested build with the very
// same unit list. Without this flag the nested build would resolve its own
// runtime archives, which would nest again, forever. With it, the nested build
// compiles exactly ONE translation unit into exactly ONE archive member, which
// is also what makes the stored artifact the single-member archive
// `runtime_object_cache.hpp` documents.
//
// ⓘ `thread_local`, not global: the per-CU pool builds CUs concurrently, and a
// shared flag would let one thread's nested build silence another thread's
// legitimate resolution. It is set and cleared by `ShippedRuntimeBuildGuard`
// below, never by hand — an early `return` or a thrown exception between the
// two would otherwise leave the driver permanently unable to see its runtime.
//
// ⓘ Forward-declared beside `reportArtifactWritten`, whose suppression arm is
// its second reader. The DEFINITION is here, with the flag it belongs to.
thread_local bool gCompilingShippedRuntimeUnit = false;

struct ShippedRuntimeBuildGuard {
    ShippedRuntimeBuildGuard() { gCompilingShippedRuntimeUnit = true; }
    ~ShippedRuntimeBuildGuard() { gCompilingShippedRuntimeUnit = false; }
    ShippedRuntimeBuildGuard(ShippedRuntimeBuildGuard const&)            = delete;
    ShippedRuntimeBuildGuard& operator=(ShippedRuntimeBuildGuard const&) = delete;
};

// One realized shipped runtime unit and EVERY descriptor that declares it.
struct ShippedRuntimeClaim {
    std::string              source;       // config-root-relative
    std::vector<std::string> descriptors;  // config-root-relative, ≥1
};

// ★★★ THE DECLARING DESCRIPTORS ARE PART OF THE CACHE KEY, SO THEY MUST BE
// ATTRIBUTED RATHER THAN ASSUMED. The unit `#include`s its descriptor's
// declarations, so editing `struct dirent`'s layout must move the key or a
// build links an archive compiled against the OLD layout — links clean, returns
// silently wrong bytes, the `environ` copy-relocation class. `ffi` exposes the
// unit list (`allShippedSourcesForFormat`) and the per-descriptor read
// (`readShippedSourcesForFormat`) but no attribution between them, so the pairing
// is composed here from those two exported readers.
//
// ⓘ THE SECOND WALK IS NOT A SECOND READ. `cachedDescriptorJson` memoizes each
// descriptor's parsed JSON per thread, so this pass costs one directory
// iteration and a map lookup per file — the parse was already paid by
// `allShippedSourcesForFormat`.
//
// ⚠ A UNIT WITH NO ATTRIBUTED DESCRIPTOR IS A REFUSAL. It means the corpus
// reader that produced the unit and the walk that attributes it disagree, and
// the key would then be computed without the term the mechanism exists for.
// `computeRuntimeObjectKey` refuses an empty set for the same reason; this
// refusal fires FIRST because it can name the walk.
[[nodiscard]] std::vector<ShippedRuntimeClaim>
attributeShippedRuntimeUnits(fs::path const&     configRoot,
                             fs::path const&     descriptorDir,
                             std::string const&  formatKey,
                             DiagnosticReporter& rep) {
    std::vector<ShippedRuntimeClaim> claims;
    auto const sources =
        dss::ffi::allShippedSourcesForFormat(descriptorDir, formatKey);
    if (sources.empty()) return claims;   // this format realizes nothing

    std::map<std::string, std::vector<std::string>> declarers;
    std::error_code ec;
    fs::recursive_directory_iterator it{descriptorDir, ec};
    if (ec) {
        emitDriver(rep, DiagnosticCode::D_DirectoryScanFailed,
                   "shipped-source realization: the descriptor corpus at '"
                   + core::genericSpelling(descriptorDir)
                   + "' could not be walked (" + ec.message()
                   + "), so the descriptor(s) declaring each shipped runtime "
                     "unit cannot be identified — and the runtime object "
                     "cache keys on their CONTENT (the unit includes their "
                     "declarations). Refusing rather than keying without them.");
        return claims;
    }
    for (fs::recursive_directory_iterator const end;
         it != end;
         it.increment(ec)) {
        if (ec) break;
        std::error_code typeEc;
        if (!it->is_regular_file(typeEc) || typeEc) continue;
        if (asciiLowerCopy(it->path().extension().generic_string()) != ".json")
            continue;
        auto const declared =
            dss::ffi::readShippedSourcesForFormat(it->path(), formatKey);
        if (declared.empty()) continue;
        std::error_code relEc;
        fs::path const rel = fs::relative(it->path(), configRoot, relEc);
        // A descriptor outside the config root cannot be spelled
        // config-root-relatively, and the key document's whole point is that it
        // is portable between an installed tree and a source tree — so the
        // absolute spelling is used and it is the ROOT that differs, which the
        // key would then refuse to find. Recording the absolute path is the
        // honest answer; `computeRuntimeObjectKey` resolves it against the root
        // and reports the miss where it is real.
        std::string const spelling =
            // The RELATIVE arm has no root to lose; the ABSOLUTE fallback does.
            (relEc || rel.empty()) ? core::genericSpelling(it->path())
                                   : rel.generic_string();
        for (auto const& source : declared)
            declarers[source].push_back(spelling);
    }

    claims.reserve(sources.size());
    for (auto const& source : sources) {
        auto const found = declarers.find(source);
        if (found == declarers.end() || found->second.empty()) {
            emitDriver(rep, DiagnosticCode::D_SchemaLoadFailed,
                       "shipped-source realization: object format '" + formatKey
                       + "' realizes '" + source
                       + "', but no descriptor under '"
                       + core::genericSpelling(descriptorDir)
                       + "' was found to declare it. The corpus reader and the "
                         "descriptor walk disagree, so the runtime object "
                         "cache key would omit the declaring descriptor's "
                         "content — an edit to those declarations would then be "
                         "served a stale archive. Refusing.");
            continue;
        }
        claims.push_back(ShippedRuntimeClaim{source, found->second});
    }
    return claims;
}

// Read a whole file as bytes. Binary on every host — the cache stores exactly
// what the writer produced, and a text-mode CR translation would make the
// stored artifact host-dependent under a key that says it is not.
[[nodiscard]] std::optional<std::vector<std::uint8_t>>
readWholeBinaryFile(fs::path const& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>()};
    if (in.bad()) return std::nullopt;
    return bytes;
}

// A dependency-artifact cache miss or refusal is a NOTE on stderr and never a
// diagnostic, for the identical reason `reportRuntimeCacheNote` is: it belongs
// to no source position and no band, and routing it through the reporter would
// let `--warnings-as-errors` turn a full disk into a compile error. LOUD on
// every affected build is the other half of "a mechanism that silently does
// nothing is worse than one that fails".
void reportDependencyCacheNote(std::string_view text) {
    std::cerr << "dsscp: note: " << text << '\n';
}

std::expected<bool, std::string>
serveArtifactFromCache(ArtifactCacheTicket const& ticket,
                       fs::path const&            outPath) {
    auto const hit = dss::runtime::lookupRuntimeObject(ticket.key);
    // ⛔ AN UNVERIFIABLE ENTRY IS A REFUSAL AND IT PROPAGATES. Treating it as a
    // miss would restore the un-verified 80-bit behaviour by the back door —
    // the store's rule is `destination already exists ⇒ same key ⇒ same bytes`,
    // which is exactly the inference a sidecar mismatch has broken.
    if (!hit.has_value()) return std::unexpected(hit.error());
    if (!hit->has_value()) return false;   // an ordinary miss

    // ★★ THE CACHED BYTES ARE COPIED TO THE PATH **THIS** BUILD RESOLVED, never
    // linked or reported from the cache root. A warm build must leave the same
    // tree a cold one does — the operator asked for an artifact at a location,
    // and a mechanism that silently stopped producing it there once a cache
    // warmed would be a build whose OUTPUT depends on cache state.
    //
    // ⛔ THE DESTINATION IS UNLINKED FIRST, AND `copy_options::none` IS THE
    // POINT — `overwrite_existing` DOES NOT OVERWRITE HERE. ✔MEASURED
    // 2026-08-31 on Windows/MinGW: with a previous artifact still at `outPath`,
    // `fs::copy_file(src, outPath, copy_options::overwrite_existing)` failed
    // with *"File exists"*. That is the state EVERY real rebuild into a dirty
    // output tree is in, so the flag-only form served exactly once per clean
    // tree and refused forever after. It was found by a REMOVE-direction mutant
    // that turned an unrelated pin's second build into a hit, not by the warm
    // pin — which had deleted the destination and could not see it.
    // `copy_options::none` after the unlink then makes a surviving destination
    // a LOUD failure rather than a silent skip.
    //
    // ⓘ NOT ATOMIC, deliberately and consistently: the link step's own write to
    // this same path is not atomic either, so a temp-and-rename here would give
    // the cached path a stronger guarantee than the compiled path it must be
    // indistinguishable from. A crash between the two leaves no artifact, which
    // is the state a crashed link leaves too.
    std::error_code ec;
    fs::remove(outPath, ec);
    ec.clear();
    fs::copy_file(**hit, outPath, fs::copy_options::none, ec);
    if (ec) {
        // ⚠ A HARD FAILURE, NOT A FALL-THROUGH TO COMPILING. The verified entry
        // IS this build's artifact; failing to place it is a failure to produce
        // the artifact, and compiling instead would hide an undeletable or
        // unwritable output path behind a slow build that sometimes works.
        return std::unexpected(std::format(
            "dependency artifact cache: a VERIFIED entry for this build was "
            "found at '{}' but could not be placed at '{}': {}. The entry is "
            "this build's artifact, so this is a failure to produce it rather "
            "than a reason to compile again. Anchored: "
            "D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION.",
            core::genericSpelling(**hit), core::genericSpelling(outPath),
            ec.message()));
    }
    return true;
}

void storeBuiltArtifactInCache(ArtifactCacheTicket const& ticket,
                               fs::path const&            artifactPath) {
    // The artifact is already on disk and already correct, so nothing below can
    // change WHAT this build produced — only whether the NEXT build repeats it.
    // That is what keeps the cache an optimization: an unwritable root, a full
    // disk or an environment with no HOME costs a line on stderr, never a
    // build. What it must not be is INVISIBLE, hence the note.
    auto const bytes = readWholeBinaryFile(artifactPath);
    if (!bytes.has_value()) {
        reportDependencyCacheNote(
            "the artifact '" + core::genericSpelling(artifactPath)
            + "' could not be read back, so it was not added to the dependency "
              "artifact cache; the next build will compile it again.");
        return;
    }
    auto const stored = dss::runtime::storeRuntimeObject(
        ticket.key, std::span<std::uint8_t const>{bytes->data(), bytes->size()},
        ticket.eviction);
    if (!stored.has_value()) reportDependencyCacheNote(stored.error());
}

// ── THE STAGING AREA ────────────────────────────────────────────────────────
//
// A cache MISS compiles the unit somewhere before it can be stored, because
// `storeRuntimeObject` takes BYTES and owns its own write-temp-then-rename (the
// sidecar-first ordering is a crash-safety property, so the store cannot be
// handed a directory to compile into). This is that somewhere.
//
// ★ IT IS ALSO THE FALLBACK THE BUILD LINKS WHEN THE STORE CANNOT WRITE. That
// is what keeps the cache an OPTIMIZATION rather than a correctness dependency:
// an unwritable cache root costs a note and a recompile next time, never a
// failed build. The bytes are right either way — they were just produced.
class RuntimeArchiveStaging {
public:
    // EMPTY until first use. A build that hits in the cache for every unit —
    // the steady state — never creates a directory at all.
    [[nodiscard]] fs::path const& dir() {
        if (!dir_.empty()) return dir_;
        std::error_code    ec;
        fs::path const     base = fs::temp_directory_path(ec);
        if (ec) return dir_;   // still empty ⇒ the caller reports the failure
        static std::atomic<unsigned> counter{0u};
        for (unsigned attempt = 0; attempt < 64u; ++attempt) {
            fs::path candidate =
                base / std::format("dsscp-runtime-{:016x}-{}",
                                   static_cast<std::uint64_t>(
                                       std::chrono::steady_clock::now()
                                           .time_since_epoch().count()),
                                   counter.fetch_add(1u));
            std::error_code mkEc;
            // `create_directory` (not `create_directories`) returns FALSE for a
            // path that already existed, which is exactly the uniqueness probe
            // this loop needs — `create_directories` would happily adopt
            // somebody else's directory and two builds would share a staging
            // area.
            if (fs::create_directory(candidate, mkEc) && !mkEc) {
                dir_ = std::move(candidate);
                return dir_;
            }
        }
        return dir_;   // empty
    }

    ~RuntimeArchiveStaging() {
        if (dir_.empty()) return;
        std::error_code ec;
        fs::remove_all(dir_, ec);   // best-effort: a leaked temp dir is not a
                                    // build failure, and on Windows a file the
                                    // linker still has open cannot be unlinked.
    }

    RuntimeArchiveStaging()                                        = default;
    RuntimeArchiveStaging(RuntimeArchiveStaging const&)            = delete;
    RuntimeArchiveStaging& operator=(RuntimeArchiveStaging const&) = delete;

private:
    fs::path dir_;
};

// ★★ THE OPERATIONAL NOTE CHANNEL — `std::cerr`, beside `dsscp: artifact …`,
// and NOT a `D_*` diagnostic. A cache that could not be written is a statement
// about this MACHINE (an unwritable root, a full disk, a scrubbed environment),
// not about the program being compiled: it has no source position, no buffer
// and no band, and routing it through the diagnostic reporter would let
// `--warnings-as-errors` turn a full disk into a compile error. It is still
// LOUD — `runtime_object_cache.hpp`'s standing rule is that a mechanism which
// silently does nothing is worse than one that fails, and every later build
// re-emitting this line is the intended behaviour, not noise to be suppressed.
void reportRuntimeCacheNote(std::string_view text) {
    std::cerr << "dsscp: note: " << text << '\n';
}

// Append the documents a loaded schema is a function of: the document itself,
// plus every OTHER document its loader folded in (`languageReferences` and
// friends). Asked of the LOADER, never hand-listed — the header's rule, and the
// first hand-written list in this mechanism's history was already wrong.
void appendSchemaDocuments(
    std::vector<dss::runtime::LoadedConfigDocument>& out,
    std::string_view                                 label,
    std::string                                      path,
    std::string_view                                 digest,
    std::span<detail::ConfigDocumentDependency const> referenced,
    fs::path const&                                  configRoot) {
    out.push_back(dss::runtime::LoadedConfigDocument{
        std::string{label}, std::move(path), std::string{digest}});
    for (auto const& dep : referenced) {
        std::error_code relEc;
        fs::path const  rel = fs::relative(fs::path{dep.path}, configRoot, relEc);
        out.push_back(dss::runtime::LoadedConfigDocument{
            std::string{label} + "-ref",
            (relEc || rel.empty()) ? dep.path : rel.generic_string(),
            dep.digest});
    }
}

// ══ D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION (C) ════════
//
// The cross-build DEPENDENCY ARTIFACT cache's driver half: the input-closure
// union, the key, the note channel, and the ticket a target's build carries.
// The store itself is `runtime_object_cache.{hpp,cpp}` — reused, never
// re-spelled.

// ★★★ THE UNION OF EVERY LIVE CU'S `inputDigest()`, AS ONE SHA-256 — the term
// that makes this cache safe at all.
//
// ⚠ EMPTY IS RETURNED FOR EVERY STATE IN WHICH THE CLOSURE IS NOT FULLY KNOWN,
// and the caller treats empty as a REFUSAL:
//   * NO units — the all-object-link shape. The inputs are then the object
//     FILES, which no front end read and no digest covers. (They ARE covered
//     as link inputs, but a build with nothing parsed has no closure to speak
//     of, and inventing an "empty closure" digest would make every all-object
//     link share one key.)
//   * ANY unit reporting an EMPTY digest — a CU built through a path that
//     never computed one. `inputDigest()`'s own contract says empty means NOT
//     COMPUTED and that a key builder must refuse it.
//
// ★ THE UNITS ARE RENDERED IN BUILD ORDER, NOT SORTED, and that is deliberate
// and opposite to how config documents are treated. This order is the order the
// operator's `sources[]` produced; it decides link order and therefore which
// definition wins, so two orders are two programs. Sorting would merge them
// onto one key — the under-invalidating direction.
[[nodiscard]] std::string unionInputDigest(std::span<CompilationUnit const> cus) {
    if (cus.empty()) return {};
    std::string document = "dss-cu-input-closure/1\n";
    document += "units=" + std::to_string(cus.size()) + "\n";
    for (CompilationUnit const& cu : cus) {
        std::string_view const digest = cu.inputDigest();
        if (digest.empty()) return {};
        document += "unit=";
        document += digest;
        document += '\n';
    }
    return dss::crypto::sha256Hex(document);
}

// One link input, digested. EMPTY digest ⇒ the key builder REFUSES, which is
// why an unreadable library is not silently skipped: it is an input whose bytes
// decide the artifact.
[[nodiscard]] dss::runtime::LoadedConfigDocument
digestLinkInput(std::string_view label, fs::path const& path) {
    auto const bytes = readWholeBinaryFile(path);
    return dss::runtime::LoadedConfigDocument{
        std::string{label}, core::genericSpelling(path),
        bytes.has_value()
            ? dss::crypto::sha256Hex(std::string_view{
                  reinterpret_cast<char const*>(bytes->data()), bytes->size()})
            : std::string{}};
}

// Build ONE target's dependency-artifact cache key, or say why it cannot be
// built. Called at the `buildCus` boundary in `runCusToTargets`.
//
// ⚠ A REFUSAL HERE IS NOT A BUILD FAILURE. It means the optimization is
// unavailable for this target, which the caller reports as a note and then
// compiles normally — the same bargain `storeRuntimeObject`'s unwritable-miss
// refusal already carries. The arm that DOES stop a build is an unverifiable
// ENTRY, and that one lives at the lookup.
[[nodiscard]] std::expected<dss::runtime::RuntimeObjectKey, std::string>
buildDependencyArtifactKey(
    DependencyArtifactCacheConfig const& policy,
    std::string const&                   targetSpec,
    TargetSchema const&                  target,
    ObjectFormatSchema const&            buildFormat,
    GrammarSchema const&                 buildGrammar,
    std::span<CompilationUnit const>     cus,
    std::string const&                   artifactBaseName,
    std::string_view                     artifactSuffix,
    CompileOptions const&                compileOpts,
    ImageRequest const&                  imageRequest) {
    static constexpr std::string_view kAnchor =
        "D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION";

    // ⛔ AN INJECTED OPTIMIZER PIPELINE IS A REFUSAL, NOT A TERM. It is a raw
    // pointer to an in-memory pass list with no content identity to digest, and
    // it CHANGES THE EMITTED BYTES. The only two honest answers are "refuse to
    // cache" and "serve an artifact built by a different pipeline"; this is the
    // first. It is reachable only from the examples runner's differential arm
    // and from MIR tests, neither of which is a project build.
    if (compileOpts.pipelineOverride != nullptr) {
        return std::unexpected(std::format(
            "dependency artifact cache: this build injects an optimizer "
            "pipeline directly, which changes the emitted bytes and carries no "
            "content identity that could enter a cache key. Not cached. "
            "Anchored: {}.",
            kAnchor));
    }

    std::string const closure = unionInputDigest(cus);
    if (closure.empty()) {
        return std::unexpected(std::format(
            "dependency artifact cache: target '{}' has no usable input closure "
            "— it builds {} translation unit(s), and a closure is available "
            "only when there is at least one and every one of them reports "
            "`CompilationUnit::inputDigest()`. An all-object link has nothing "
            "parsed to digest. Not cached. Anchored: {}.",
            targetSpec, cus.size(), kAnchor));
    }

    // The CONFIG ROOT — the cache anchor, and the directory the archive-sibling
    // lookup scans. Derived from ONE walk, exactly as
    // `resolveShippedRuntimeArchives` derives it: two walks could answer with
    // two different trees on a host with more than one checkout.
    auto const objectFormatsDir = findShippedConfigDir("object-formats");
    if (!objectFormatsDir) {
        return std::unexpected(std::format(
            "dependency artifact cache: the shipped object-format directory "
            "(src/dss-config/object-formats) could not be located, so neither "
            "the cache root nor the archive-writing sibling can be resolved. "
            "Not cached. Anchored: {}.",
            kAnchor));
    }
    fs::path const configRoot = objectFormatsDir->parent_path();

    // The archive sibling is a real input: it is the writer that produced the
    // runtime archives this artifact links. Its own requester label and anchor,
    // never the runtime cache's — a refusal saying "runtime object cache" to
    // somebody whose DEPENDENCY did not cache names the wrong mechanism.
    static constexpr dss::runtime::ArchiveSiblingRequester kRequester{
        "dependency artifact cache",
        "D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION"};
    auto const siblingName = dss::runtime::resolveArchiveSiblingFormat(
        buildFormat, target, *objectFormatsDir, kRequester);
    if (!siblingName.has_value()) return std::unexpected(siblingName.error());
    auto const siblingSchema = ObjectFormatSchema::loadShipped(*siblingName);
    if (!siblingSchema.has_value()) {
        return std::unexpected(std::format(
            "dependency artifact cache: the archive-writing sibling format '{}' "
            "could not be loaded, so its content digest cannot enter the cache "
            "key. Not cached. Anchored: {}.",
            *siblingName, kAnchor));
    }

    auto const parsedSpec = TargetSpec::parse(targetSpec);
    if (!parsedSpec.has_value()) {
        return std::unexpected(std::format(
            "dependency artifact cache: the target spec '{}' does not parse, so "
            "the key's target term cannot be composed. Not cached. Anchored: "
            "{}.",
            targetSpec, kAnchor));
    }

    dss::runtime::DependencyArtifactRequest request;
    request.configRoot          = configRoot;
    request.overrideVariable    = policy.rootOverrideVariable;
    request.inputClosureDigest  = closure;
    request.artifactStem        = artifactBaseName;
    request.artifactSuffix      = std::string{artifactSuffix};
    request.targetSpec          = targetSpec;
    request.buildFormatName     = std::string{buildFormat.name()};
    request.siblingFormatName   = *siblingName;
    request.configName          = std::string{compileConfigName(compileOpts.config)};
    // Spelled from the closed enum rather than from a number, so the document
    // stays readable and a new topology cannot silently reuse an old key.
    request.ltoModeName =
        compileOpts.ltoMode == CompileOptions::LtoMode::Thin ? "thin" : "full";
    request.stackReserveBytes = imageRequest.stackReserveBytes;

    // The config documents this build ALREADY LOADED, asked of the loaders that
    // own them — never hand-listed. Identical set and identical construction to
    // `resolveShippedRuntimeArchives`'s, through the SAME helper.
    appendSchemaDocuments(request.loadedDocuments, "language",
                          buildGrammar.configDocumentPath(),
                          buildGrammar.contentDigest(),
                          buildGrammar.referencedDocuments(), configRoot);
    appendSchemaDocuments(request.loadedDocuments, "target",
                          "targets/" + parsedSpec->targetName + ".target.json",
                          target.contentDigest(), {}, configRoot);
    appendSchemaDocuments(request.loadedDocuments, "format",
                          "object-formats/" + std::string{buildFormat.name()}
                              + ".format.json",
                          buildFormat.contentDigest(), {}, configRoot);
    appendSchemaDocuments(request.loadedDocuments, "sibling-format",
                          "object-formats/" + *siblingName + ".format.json",
                          (*siblingSchema)->contentDigest(), {}, configRoot);

    // ⓘ THE SAME TWO LISTS `compileOneTarget` WILL HAND THE LINK, IN THE SAME
    // ORDER, read from the SAME `CompileOptions` — not a second gathering of
    // "what this build probably links". By this point the runtime archives have
    // already been appended by the caller, so the list is complete.
    request.linkInputs.reserve(compileOpts.resolveLibraries.size()
                               + compileOpts.objectInputs.size());
    for (ResolveLibrarySpec const& lib : compileOpts.resolveLibraries) {
        auto entry = digestLinkInput("resolve-library", fs::path{lib.path});
        // ⚠ THE DECLARED IMPORT NAME RIDES WITH THE PATH. It outranks the
        // binary's own embedded soname and is therefore recorded INTO the
        // artifact — two builds differing only in it produce different bytes.
        if (!lib.declaredImportName.empty()) {
            entry.path += "=" + lib.declaredImportName;
        }
        request.linkInputs.push_back(std::move(entry));
    }
    for (fs::path const& object : compileOpts.objectInputs) {
        request.linkInputs.push_back(digestLinkInput("object-input", object));
    }

    return dss::runtime::computeDependencyArtifactKey(request);
}

// ══ THE ONE SEAM ═════════════════════════════════════════════════════════════
//
// Every shipped runtime archive this (target, format, config) needs, as paths a
// static link can pull members from. Cached entries are returned as they lie;
// misses are compiled, stored, and returned from the staging area.
//
// Returns an EMPTY vector for every build that realizes nothing — which is
// every target of every format kind no descriptor declares a `realization` for,
// and is byte-identical to the pre-cache driver.
[[nodiscard]] std::vector<fs::path> resolveShippedRuntimeArchives(
    std::string const&          targetSpec,
    TargetSchema const&         target,
    ObjectFormatSchema const&   buildFormat,
    GrammarSchema const&        buildGrammar,
    ShippedSourceLanguageCache& grammarCache,
    CompileConfig               config,
    RuntimeArchiveStaging&      staging,
    DiagnosticReporter&         rep) {
    std::vector<fs::path> archives;
    // The nested build is aimed at THIS format's archive sibling, which shares
    // its KIND — so without this it would resolve the same unit list and nest
    // forever. See `gCompilingShippedRuntimeUnit`.
    if (gCompilingShippedRuntimeUnit) return archives;

    auto const descriptorDir = findShippedConfigDir("shippedLibs");
    if (!descriptorDir) return archives;   // no corpus ⇒ nothing realized

    // `findShippedConfigDir` returns `<configRoot>/<sub>`, so the root is its
    // parent. Derived rather than re-walked: two walks could answer with two
    // different trees on a host that has more than one checkout, which is the
    // D-PROGRAM-CONFIG-DIR-WALK-RESOLVES-A-FOREIGN-TREE shape.
    fs::path const    configRoot = descriptorDir->parent_path();
    // D-PROGRAM-TIER-RETAINS-FORMAT-IDENTITY-BRANCHES (the residual half): the
    // kind is READ OFF THE FORMAT HANDLE this function already holds, rather
    // than accepted as a second parameter beside it. The old signature took
    // BOTH `buildFormat` and its own `kind()`, so a caller could pass a kind
    // that disagreed with the schema and nothing would notice — the two are
    // now one fact with one owner, and the only spelling of the enum's TYPE
    // that this function needed is gone with the parameter.
    std::string const formatKey{objectFormatKindName(buildFormat.kind())};

    auto const claims = attributeShippedRuntimeUnits(configRoot, *descriptorDir,
                                                     formatKey, rep);
    if (claims.empty()) return archives;

    // ── THE ARCHIVE SIBLING: the format that WRITES the bytes ────────────────
    // Reached through the production lookup, which scans every candidate and
    // refuses on 0 or >1 rather than taking a first match — `directory_iterator`
    // is sorted on NTFS and hash-ordered on ext4, so first-match would let the
    // filesystem decide which format compiled the runtime.
    auto const objectFormatsDir = findShippedConfigDir("object-formats");
    if (!objectFormatsDir) {
        emitDriver(rep, DiagnosticCode::D_SchemaLoadFailed,
                   "shipped-source realization: object format '" + formatKey
                   + "' realizes " + std::to_string(claims.size())
                   + " shipped runtime unit(s), but the shipped object-format "
                     "directory (src/dss-config/object-formats) could not be "
                     "located, so the archive-writing sibling that compiles "
                     "them cannot be resolved.");
        return archives;
    }
    auto const siblingName = dss::runtime::resolveArchiveSiblingFormat(
        buildFormat, target, *objectFormatsDir,
        dss::runtime::kRuntimeCacheSiblingRequester);
    if (!siblingName.has_value()) {
        emitDriver(rep, DiagnosticCode::D_SchemaLoadFailed, siblingName.error());
        return archives;
    }
    auto const siblingSchema = ObjectFormatSchema::loadShipped(*siblingName);
    if (!siblingSchema.has_value()) {
        emitObjectFormatSchemaLoadFailed(rep, siblingSchema.error(),
                                         *siblingName);
        return archives;
    }

    auto const parsedSpec = TargetSpec::parse(targetSpec);
    if (!parsedSpec.has_value()) {
        // Unreachable from the driver — every spec parsed in the pre-flight
        // above — but stated rather than assumed: this function composes a NEW
        // spec from the target half, and composing one out of an unparsed
        // string is how a nested build silently aims somewhere else.
        emitDriver(rep, DiagnosticCode::D_InvalidTargetSpec,
                   "shipped-source realization: the target spec '" + targetSpec
                   + "' does not parse, so the archive-sibling spec the runtime "
                     "units compile under cannot be composed.");
        return archives;
    }
    std::string const siblingSpec = parsedSpec->targetName + ":" + *siblingName;

    // ── THE CONFIG DOCUMENTS THIS BUILD ALREADY LOADED ──────────────────────
    // Asked of the loaders, which retain a digest of the bytes they read, so
    // this term costs the hash alone and never a re-walk of src/dss-config
    // (✔MEASURED at ~165 ms per invocation, I/O-dominated).
    //
    // ⓘ THE TARGET AND FORMAT ROWS PASS AN EMPTY DEPENDENCY SPAN BECAUSE
    // NEITHER LOADER HAS ONE — ✔MEASURED 2026-08-25, `referencedDocuments()`
    // exists on `GrammarSchema` alone, which is the only loader that folds other
    // documents in (`languageReferences`). This is a statement about those
    // loaders and not a decision taken here: the day a target or format document
    // grows a reference, its loader grows the accessor, and the argument in
    // `LoadedConfigDocument`'s docblock — *the set must be ASKED of the loaders,
    // never hand-listed* — makes wiring it up the obvious edit. A hand-written
    // list of what a target document might include would be the drift this whole
    // term exists to avoid.
    // ⚠ `configDocumentPath()`, NOT a path composed from `name()` — the stem the
    // config tree is INDEXED by, never the name the document DECLARES. Three of
    // the six shipped languages declare a name that is not a file stem at all
    // (`asm-arm64-gas` → "AsmArm64Gas"), and one more differs only in case
    // (`c` → "C") — that last is the pair a case-insensitive host cannot see.
    // See `GrammarSchema::configName`.
    //
    // ⓘ THERE IS NO EMPTY-NAME REFUSAL HERE, AND ITS DELETION IS DELIBERATE
    // (cycle P36). One stood here and could not fire: `buildGrammar` reaches
    // this function only from `resolveGrammarForTarget`, whose every arm is
    // `GrammarSchema::loadShipped` → `loadFromFile` → `loadFromText(text,
    // "<…>/<stem>.lang.json")`, and ✔MEASURED over `src/` there is no
    // production caller of `GrammarSchema::loadFromText` at all — only tests,
    // which cannot reach this internal function. It also guarded the wrong
    // thing: this term is the LABEL half of the key line
    // `doc=<label>:<path>:<digest>`, so a wrong label cannot serve a stale
    // object, while the refusal failed the whole build. The property it wanted
    // now lives where it can be exercised — `configDocumentPath()` is total,
    // and BOTH its arms are driven directly in `tests/core/test_grammar_schema`.
    std::vector<dss::runtime::LoadedConfigDocument> baseDocuments;
    appendSchemaDocuments(baseDocuments, "language",
                          buildGrammar.configDocumentPath(),
                          buildGrammar.contentDigest(),
                          buildGrammar.referencedDocuments(), configRoot);
    appendSchemaDocuments(baseDocuments, "target",
                          "targets/" + parsedSpec->targetName + ".target.json",
                          target.contentDigest(), {}, configRoot);
    appendSchemaDocuments(baseDocuments, "format",
                          "object-formats/" + std::string{buildFormat.name()}
                              + ".format.json",
                          buildFormat.contentDigest(), {}, configRoot);
    appendSchemaDocuments(baseDocuments, "sibling-format",
                          "object-formats/" + *siblingName + ".format.json",
                          (*siblingSchema)->contentDigest(), {}, configRoot);

    for (ShippedRuntimeClaim const& claim : claims) {
        auto const resolved = dss::ffi::resolveShippedSource(claim.source);
        if (!resolved.resolved()) {
            // R1's descriptor-read-time refusal owns the diagnostic and has
            // already fired by the time we get here. This arm exists so a
            // discovery failure cannot silently drop a body: it says so, in the
            // driver's own voice.
            // ★ THE CODE FOLLOWS THE FINDING, because `D_FileNotFound` is itself
            // a claim. An I/O failure is not a missing file, and reporting one
            // as the other is what sent a reader hunting for `unistd.c` while it
            // sat in place (✔MEASURED 2026-08-25 under concurrent gate load).
            emitDriver(rep,
                       dss::ffi::diagnosticCodeForShippedSourceLookup(resolved),
                       "shipped-source realization: a descriptor this build "
                       "resolved names '" + claim.source
                       + "' for object format '" + formatKey + "', but "
                       + dss::ffi::describeShippedSourceLookup(resolved,
                                                               claim.source)
                       + " — the program would link against a symbol with no "
                         "body");
            continue;
        }
        // ★ THE LANGUAGE COMES FROM THE EXTENSION, through the SAME mechanism
        // every ordinary compile uses. `.c` is claimed by exactly one shipped
        // language, so no manifest, no path segment and no driver-side literal
        // states it a second time. An extension NO language claims, or one that
        // TWO claim (`.s`/`.S`, claimed by both asm dialects), fails LOUD rather
        // than picking.
        //
        // ⚠ RESOLVED ON THE HIT PATH TOO, AND THAT IS NOT WASTE. The grammar
        // that compiles the unit is an INPUT to the archive, so its digest
        // belongs in the key: skip it on a hit and an edit to `c.lang.json`
        // would be served an archive built by the previous grammar whenever the
        // user's own file is not also C. The loads are memoized in-process and
        // the claim index is memoized in `grammarCache`, so the common case —
        // a `.c` user file — costs a map lookup.
        auto const unitGrammar =
            resolveShippedSourceGrammar(resolved.path, grammarCache, rep);
        if (!unitGrammar) continue;   // already diagnosed

        dss::runtime::RuntimeObjectRequest request;
        request.configRoot        = configRoot;
        request.descriptorPaths   = claim.descriptors;
        request.sourcePath        = claim.source;
        request.targetSpec        = targetSpec;
        request.buildFormatName   = std::string{buildFormat.name()};
        request.siblingFormatName = *siblingName;
        request.configName        = std::string{compileConfigName(config)};
        request.loadedDocuments   = baseDocuments;
        // ⚠ THE GRAMMAR ANSWERS FOR ITS OWN DOCUMENT PATH — never a second
        // composition from `unitGrammar.configName` here and a third somewhere
        // else. The path must be the one that EXISTS (`sources/c.lang.json`),
        // and the declared name is "C". `resolveShippedSourceGrammar` loaded
        // this grammar BY that stem, so the two agree by construction; asking
        // the schema is what keeps them agreeing. See `ShippedSourceLanguage`.
        appendSchemaDocuments(request.loadedDocuments, "unit-language",
                              unitGrammar.grammar->configDocumentPath(),
                              unitGrammar.grammar->contentDigest(),
                              unitGrammar.grammar->referencedDocuments(),
                              configRoot);

        auto const key = dss::runtime::computeRuntimeObjectKey(request);
        if (!key.has_value()) {
            // NOT degraded to "compile it uncached". A key that cannot be
            // computed means an INPUT could not be read or a loader reported a
            // digest this mechanism does not recognise — the cache would be
            // serving keys it cannot verify, and the same unreadable input is
            // about to be handed to the front end anyway.
            emitDriver(rep, DiagnosticCode::D_SchemaLoadFailed, key.error());
            continue;
        }

        auto const hit = dss::runtime::lookupRuntimeObject(*key);
        if (!hit.has_value()) {
            // ⛔ AN UNVERIFIABLE ENTRY IS A REFUSAL, NOT A MISS, and this is the
            // one place the wiring deliberately does NOT degrade. An artifact
            // whose `.key` sidecar is absent, unreadable or different is an
            // entry that cannot be shown to be this key's — the exact state the
            // 16-character path index would otherwise make un-detectable — and
            // treating it as a miss would restore the un-verified behaviour by
            // the back door. The message names the file and the remedy.
            //
            // ⓘ `D_FileReadFailed` is the CLOSEST code the driver band carries,
            // not an exact one: it fits "the sidecar could not be read" and is
            // stretched over "it is absent" and "it differs". Nothing is lost to
            // a READER — the message carries the path, the 16-character index,
            // the full identity digest, the remedy and the anchor. What an exact
            // code would buy is FILTERABILITY, and minting one is an enumerator
            // in `core/types/parse_diagnostic.hpp`.
            emitDriver(rep, DiagnosticCode::D_FileReadFailed, hit.error());
            continue;
        }
        if (hit->has_value()) {
            archives.push_back(**hit);
            continue;
        }

        // ── A MISS: compile the unit into its own single-member archive ──────
        fs::path const& stagingRoot = staging.dir();
        if (stagingRoot.empty()) {
            emitDriver(rep, DiagnosticCode::D_OutputDirCreateFailed,
                       "shipped-source realization: no staging directory could "
                       "be created under the system temporary directory, so "
                       "the shipped runtime unit '" + claim.source
                       + "' cannot be compiled for target '" + siblingSpec
                       + "'. The build stops here rather than continuing "
                         "without its body — a missing runtime body links clean "
                         "and dies at run time.");
            continue;
        }
        // A FRESH directory per unit. The artifact assertion below then means
        // "THIS compile wrote it" rather than "a file with that name exists":
        // a leftover from an earlier unit cannot stand in for one that was
        // never produced.
        fs::path const unitDir =
            stagingRoot / fs::path{claim.source}.stem();
        std::error_code mkEc;
        fs::create_directories(unitDir, mkEc);
        if (mkEc && !fs::is_directory(unitDir)) {
            emitDriver(rep, DiagnosticCode::D_OutputDirCreateFailed,
                       "shipped-source realization: could not create the "
                       "staging directory '" + core::genericSpelling(unitDir)
                       + "' for shipped runtime unit '" + claim.source
                       + "': " + mkEc.message());
            continue;
        }

        int rc = 1;
        {
            // ★ THE NESTED BUILD IS THE PRODUCTION WRITER, NOT A SECOND ONE.
            // It is the same `Program` → `compileOneTarget` → static-archive
            // arm every `--target …-staticlib` build takes, which is why the
            // shipped-runtime compile GATE
            // (`tests/program/test_shipped_runtime_compiles`) is a real control
            // over this path: it drives the identical call for every declared
            // unit × machine × config.
            ShippedRuntimeBuildGuard const guard;
            Program                        runtimeProgram;
            runtimeProgram.setOutputDir(unitDir);
            runtimeProgram.setCompileConfig(config);
            // POLICY inherited (suppress / overrides / warnings-as-errors), CAP
            // and DEDUP relaxed — the same split the per-target scratch
            // reporter uses, and for the same reason: the run-wide limits are
            // enforced once at the destination.
            auto nestedCfg           = rep.config();
            nestedCfg.maxDiagnostics = std::numeric_limits<std::size_t>::max();
            nestedCfg.maxPerCode     = std::numeric_limits<std::size_t>::max();
            nestedCfg.dedupWindow    = 0;
            DiagnosticReporter nested{nestedCfg};
            // ⚠ `configName`, NOT `grammar->name()`. This string is a
            // `--language` argument, so it must be the name the CONFIG TREE is
            // indexed by. For C the declared name resolved
            // `sources/C.lang.json` — the same file on a case-insensitive host
            // and none at all on Linux — and for the three shipped languages
            // whose declared name is not a stem at all (`AsmArm64Gas`,
            // `AsmX86_64Att`, `TsqlSubset`) it resolves to nothing ANYWHERE.
            // See `ShippedSourceLanguage`.
            rc = runtimeProgram.compileFiles(
                std::vector<std::string>{resolved.path.string()},
                unitGrammar.configName,
                std::vector<std::string>{siblingSpec}, nested);
            // ★★★ A RUNTIME UNIT THAT DOES NOT COMPILE FAILS THE BUILD *HERE*,
            // NAMING THE UNIT, THE FORMAT AND THE TARGET — it is never skipped.
            // Skipping would produce a GREEN BUILD THAT DIES IN THE USER'S
            // HANDS: [[D-LINK-EXEC-UNDEFINED-SYMBOL-FAIL-LOUD]] is OPEN, so an
            // undefined EXEC symbol currently yields rc=0 at link and a runtime
            // exit-127 rather than a link error.
            if (rc != 0 || nested.hasErrors()) {
                emitDriver(rep, DiagnosticCode::D_SchemaLoadFailed,
                           "shipped-source realization: the shipped runtime "
                           "unit '" + claim.source
                           + "' (declared by a shipped-lib descriptor's "
                             "'realization." + formatKey + "') FAILED TO "
                             "COMPILE for target '" + siblingSpec
                           + "' (the archive-writing sibling of '"
                           + std::string{buildFormat.name()}
                           + "'). The build stops here rather than continuing "
                             "without its body — a missing runtime body links "
                             "clean and dies at run time. The reason is in the "
                             "diagnostic(s) above.");
                rc = 1;
            }
            if (rc == 0) {
                // The producer answers where it wrote — never a second copy of
                // the `<outputDir>/<stem><ext>` formula, which is exactly the
                // drift `Program::artifactPaths()` exists to remove.
                auto const& written = runtimeProgram.artifactPaths();
                if (written.size() != 1u || !written.front().has_value()) {
                    emitDriver(rep, DiagnosticCode::D_CompileUnitNullNoDiagnostic,
                               "internal: the shipped runtime unit '"
                               + claim.source
                               + "' compiled with rc=0 for '" + siblingSpec
                               + "' but reported no artifact path — a success "
                                 "that produced nothing.");
                    rc = 1;
                } else {
                    archives.push_back(*written.front());
                }
            }
        }
        if (rc != 0) continue;

        // ── STORE IT, AND A FAILURE TO STORE IS A NOTE ──────────────────────
        // The archive the link will pull from is already on disk in the staging
        // area and it is already in `archives`, so nothing below can change
        // WHAT this build produces — only whether the NEXT build has to repeat
        // the compile. That is what makes the cache an optimization: an
        // unwritable root, a full disk or an environment with no HOME costs a
        // line on stderr, never a build.
        auto const bytes = readWholeBinaryFile(archives.back());
        if (!bytes.has_value()) {
            reportRuntimeCacheNote(
                "the shipped runtime archive '"
                + core::genericSpelling(archives.back())
                + "' could not be read back, so it was not added to the runtime "
                  "object cache; this build links it from the staging area and "
                  "the next build will compile it again.");
            continue;
        }
        // ⓘ `PruneSuperseded` STATED rather than defaulted, and it is this
        // site's pre-existing behaviour written down: ONE shipped unit has ONE
        // current object per (target, config), so an entry a rebuild supersedes
        // is dead weight. The policy is a parameter because the OTHER subject
        // class — a project's dependency artifacts — legitimately wants the
        // opposite; see `CacheEviction`.
        auto const stored = dss::runtime::storeRuntimeObject(
            *key, std::span<std::uint8_t const>{bytes->data(), bytes->size()},
            dss::runtime::CacheEviction::PruneSuperseded);
        if (!stored.has_value()) {
            reportRuntimeCacheNote(stored.error());
            continue;
        }
        // ★ THE STORED COPY REPLACES THE STAGING COPY IN THE LINK INPUTS, so a
        // hit and a miss link the same FILE and not merely the same bytes. The
        // staging area is torn down at the end of the build; the cache entry is
        // not, and pointing the link at the durable copy removes any question
        // about which of the two the artifact was actually built from.
        archives.back() = *stored;
    }
    return archives;
}

int runCusToTargets(
    // ★★ D-LK-OBJECT-INPUT-COMPILE-FLAG-SURFACE: THE FIRST PARAMETER IS THE
    // FILE LIST TO BUILD, AND IT IS A PARAMETER RATHER THAN A CAPTURE FOR ONE
    // REASON. This closure used to be BOUND to `sourceFiles` by whichever
    // entry point created it (`compileFiles` / `compileUnits`), so by the time
    // this function could ask a FORMAT whether a gathered file is a
    // pre-assembled object, the decision of what to parse had already been
    // made and could not be revisited without a second list travelling beside
    // the first. Passing the list makes THIS function the single owner of
    // "which gathered files become compilation units" — which is where the
    // partition below already lives — and removes the parallel-list shape
    // entirely rather than threading one through.
    std::function<std::vector<CompilationUnit>(
        CuBuildKey const&, std::span<PredefinedMacroDef const>,
        std::span<PredefinedMacroDef const>,
        std::shared_ptr<GrammarSchema const> const&,
        std::span<std::string const>)> buildCus,
    std::shared_ptr<GrammarSchema const> const& explicitGrammar,
    // ⚠ EVERY gathered file, sources AND objects. The partition below splits
    // it; `sourceStem` is still derived from `front()` by the caller, which is
    // right — an all-object `--compile a.o` names its image after `a`.
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
    // D-OPT11-LAZY-IMPORT-EDGE: the link-time-optimization TOPOLOGY, threaded
    // beside `config` because it is the same kind of fact — a property of the
    // SHAPE of this build, decided by the driver, not of a pass schedule.
    LtoModeArg                                  ltoMode,
    ::dss::opt::OptPipeline const*              pipelineOverride,
    std::vector<ResolveLibrarySpec> const&      resolveLibraries,
    // AP6: the PER-TARGET ADDITIONS channel — extra `ResolveLibrarySpec`s for
    // ONE target spec, MERGED with (never replacing) the program-wide list
    // above. Keyed by the `<targetName>:<formatName>` string, EMPTY ⇒ nothing
    // added anywhere, which is byte-identical to the pre-AP6 broadcast. See
    // `Program::setResolveLibraryAdditionsByTarget` for why the channel is
    // keyed rather than index-parallel, and why it is internal-only.
    std::map<std::string, std::vector<ResolveLibrarySpec>> const&
                                                resolveLibraryAdditionsByTarget,
    // AP6: index-parallel to `targets` — entry i is the artifact
    // `compileOneTarget` wrote for `targets[i]`, `nullopt` if that target
    // failed. CLEARED and re-sized here, so a caller reading it after a run
    // reads THIS run. Left EMPTY when the function returns before the
    // per-target loop (a bad spec, an unloadable schema, a parse failure) —
    // i.e. "no target was reached", which is distinct from "a target was
    // reached and produced nothing".
    std::vector<std::optional<std::filesystem::path>>& artifactsOut,
    // D-PERF-4-CU-PARALLELISM: the per-CU build executor (nullptr ⇒ internal
    // pool) + the `--jobs` override, threaded verbatim to `compileOneTarget`.
    substrate::IExecutor*                       executor,
    unsigned                                    jobsOverride,
    // D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: the per-PROGRAM image knobs
    // (stack reserve), threaded verbatim to `compileOneTarget` — which
    // hands them to the link step, where the format's DECLARED capability
    // gates them. Passed PER TARGET, not resolved once: two targets of one
    // build can differ in whether their format can carry the request.
    ImageRequest const&                         imageRequest,
    // D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION (C): the
    // ROOT manifest's cross-build artifact cache policy, threaded verbatim from
    // the `Program` knob. nullopt (every CLI build, every root project build,
    // every dependency under a manifest that declares none) ⇒ no consult, no
    // store, byte-identical behaviour.
    std::optional<DependencyArtifactCacheConfig> const& artifactCachePolicy) {
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
        // D-PROGRAM-PROJECT-WIDE-PARSE-GATE-MASKS-CENSUS, the FIRST gate in the
        // chain. This one genuinely CANNOT continue and is not the row's
        // defect: with no target or format schema there is no pair to compile
        // AGAINST, so there is nothing to run the later tiers with. What it
        // owed the reader is the SCOPE of its silence, which it now states.
        // `sourceFiles`, not `sourcesToBuild`: the source/object partition has
        // not happened yet at this altitude, and the count the reader needs is
        // "what you named on the command line", which is exactly this list.
        emitStoppedBeforeCuBuild(rep, "target/object-format resolution",
                                 sourceFiles.size());
        drainDiagnosticsToStderr(rep);
        return 1;
    }

    // ── D-LK-OBJECT-INPUT-COMPILE-FLAG-SURFACE: PARTITION THE GATHERED FILES
    //    INTO SOURCES AND PRE-ASSEMBLED OBJECTS, BEFORE ANY OF THEM IS PARSED ─
    //
    // ★★★ THE STANDING RULING IS THAT `--project`, `--directory` AND
    // `--compile` ARE DIFFERENT WAYS TO GATHER THE FILES TO BE COMPILED AND
    // NEVER DIFFERENT COMPILERS. A relocatable object was nameable on
    // `--resolve-library` and on NO gathering route, which is precisely that
    // asymmetry: ✔MEASURED on the shipped CLI before this pass, `--compile u1.c
    // objs/u2.obj` produced a cascade of `P000E illegal character 0x01` from
    // the TOKENIZER — the object had been handed to the front end — while
    // `--compile u1.c --resolve-library objs/u2.obj` built an exe that RAN and
    // returned 42.
    //
    // ★★ IT LIVES HERE, AND THAT IS WHY IT SERVES ALL THREE ROUTES AT ONCE.
    // Every gathering route funnels into this function: the CLI's
    // `compileFiles`/`compileUnits`, a `.dss-project.json` build (whose
    // expanded+deduped source list reaches the same two entries), and
    // `compileDirectory`. Partitioning at ANY of the entry points would have
    // been one route learning a trick the others do not know — a second engine,
    // which is the thing the ruling forbids. There is one partition and every
    // route is behind it.
    //
    // ★ ASKED OF THE FORMAT, BY MAGIC BYTES, NEVER BY EXTENSION. The predicate
    // is `isRelocatableObjectFile(path, format)` — the SAME third arm the
    // `--resolve-library` dispatch already uses, reused rather than
    // reimplemented, so `.o` / `.obj` / `.lo` / no extension at all are decided
    // by what the file IS. It reads a 64-byte header prefix and never the file.
    //
    // ⚠ THE UNION OVER THIS BUILD'S FORMATS, NOT ONE FORMAT'S ANSWER. A build
    // may name several targets and a file can be an object for one format and
    // noise to another (an ELF `.o` under an additional `pe64` target). The CU
    // set is SHARED across targets — `CuBuildKey` groups the builds — so the
    // partition has to produce ONE answer for the whole run. Taking the union
    // is the fail-loud choice: the disagreeing target routes the file to
    // `readObjectInputModules`, which refuses it BY NAME as an object it cannot
    // read, instead of the tokenizer reporting `illegal character 0x7f` about a
    // file that was never text. The other direction — intersection — would send
    // a real object back to the front end, which is the defect this closes.
    //
    // ⓘ A file that is an object for NO format in this build stays a SOURCE and
    // reaches the front end exactly as before, so a build that names no object
    // is byte-identical: `objectInputsFromSources` is empty and
    // `sourcesToBuild` is `sourceFiles` itself.
    std::vector<std::string>           sourcesToBuild;
    std::vector<std::filesystem::path> objectInputsFromSources;
    {
        for (auto const& file : sourceFiles) {
            fs::path const path{file};
            bool isObject = false;
            for (auto const& [_, format] : formatByName) {
                if (isRelocatableObjectFile(path, *format)) { isObject = true; break; }
            }
            if (isObject) objectInputsFromSources.push_back(path);
            else          sourcesToBuild.push_back(file);
        }
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
            // D-LK-OBJECT-INPUT-COMPILE-FLAG-SURFACE: the SOURCES, not every
            // gathered file. This resolver reads the list's EXTENSIONS to pick
            // a dialect, and a pre-assembled object has no source language to
            // contribute — asking it for one is how an all-object link would
            // have been refused for naming a file no grammar claims.
            grammarPerTarget.push_back(resolveGrammarForTarget(
                explicitGrammar, targetSchema, sourcesToBuild, grammarByName, rep));
        }
    }
    if (rep.hasErrors()) {
        // Second gate in the chain, same class as the first and equally
        // unavoidable: with no grammar there is no front end to parse WITH.
        emitStoppedBeforeCuBuild(rep, "source-language resolution",
                                 sourcesToBuild.size());
        drainDiagnosticsToStderr(rep);
        return 1;
    }

    // ── AP6: the PER-TARGET `--resolve-library` ADDITIONS, resolved into the
    //    THIRD index-parallel vector ─────────────────────────────────────────
    //
    // `resolveLibraries` is ONE program-wide list broadcast to every target
    // (below, at `compileOpts.resolveLibraries`). That is right for the CLI and
    // the manifest, which are both program-wide statements. It is NOT right for
    // AP6: a dependency is built ONCE PER CONSUMER TARGET, so the artifact that
    // must be linked into `x86_64:elf64-…` is a DIFFERENT FILE from the one for
    // `arm64:macho64-…`. Broadcasting the union would hand every target every
    // other target's binaries — the wrong-format cross-feed that binds SILENTLY
    // today (registry `D-FFI-RESOLVE-LIBRARY-WRONG-FORMAT-GUARD-IS-INCIDENTAL`).
    //
    // MERGE, NEVER REPLACE: program-wide entries first, this target's additions
    // after. An empty map contributes nothing to anyone, so the vector below is
    // a per-target copy of `resolveLibraries` and the build is byte-identical to
    // the pre-AP6 broadcast.
    //
    // ★ KEYED BY SPEC STRING, INDEX-PARALLEL ONLY IN HERE. An index-parallel
    // channel across the API boundary would add an invariant the caller can
    // break — `additions.size() == targets.size()` — whose breach is a
    // MIS-ALIGNMENT: target 0 silently linking target 1's libraries. Keying on
    // the spec the producer already derives against removes that invariant
    // instead of diagnosing it, and the derived per-index vector below (the
    // third sibling of `grammarPerTarget` / `keyPerTarget`) is built HERE, so it
    // cannot be the wrong length. The one mistake the key form still admits —
    // naming a target this build does not have — is caught right below rather
    // than dropped, because a dependency silently NOT linked is a link error
    // several tiers from its cause.
    std::vector<std::vector<ResolveLibrarySpec>> resolveLibsPerTarget;
    resolveLibsPerTarget.reserve(targets.size());
    for (auto const& spec : targets) {
        std::vector<ResolveLibrarySpec> libs = resolveLibraries;
        auto const it = resolveLibraryAdditionsByTarget.find(spec);
        if (it != resolveLibraryAdditionsByTarget.end()) {
            libs.reserve(libs.size() + it->second.size());
            libs.insert(libs.end(), it->second.begin(), it->second.end());
        }
        resolveLibsPerTarget.push_back(std::move(libs));
    }
    // FAIL LOUD on an addition keyed to a target this build never compiles: its
    // libraries would reach no link at all, and the resulting undefined symbol
    // would name neither the key nor the target. Reported for EVERY unmatched
    // key in one pass (the same all-reasons-in-one-run discipline the target /
    // format / pair pre-flight above follows), and `std::map`'s ordering makes
    // the report deterministic. `D_InvalidTargetSpec` is the driver-band code
    // for "the caller's invocation names a target that cannot be honoured" —
    // the same code, and the same class of complaint, as this file's existing
    // "targets list is empty" rejects; it is a target-spec problem, not a
    // library problem, so it must not route an operator to the library.
    {
        std::set<std::string_view> const built{targets.begin(), targets.end()};
        bool unmatched = false;
        for (auto const& [spec, libs] : resolveLibraryAdditionsByTarget) {
            if (built.count(spec) != 0) continue;
            unmatched = true;
            emitDriver(rep, DiagnosticCode::D_InvalidTargetSpec,
                       "per-target resolve-library additions name target spec '"
                       + spec + "', which is not among this build's "
                       + std::to_string(targets.size())
                       + " target(s) — its " + std::to_string(libs.size())
                       + " librar(y/ies) would reach no link. Use one of the "
                         "build's own '<targetName>:<formatName>' specs.");
        }
        if (unmatched) {
            // Third gate in the chain. It rejects the INVOCATION, before any
            // source is read, so it deletes the same census the two above do
            // and says so in the same words.
            emitStoppedBeforeCuBuild(rep, "per-target --resolve-library "
                                          "validation",
                                     sourcesToBuild.size());
            drainDiagnosticsToStderr(rep);
            return 1;
        }
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
    // D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF: carries the shipped-source
    // units' extension->language resolution across keys, so a 5-target build
    // reads the language corpus ONCE rather than five times.
    // D-DRIVER-SHIPPED-SOURCE-RESOLUTION-COMPILES-EVERY-SHIPPED-GRAMMAR: it
    // used to be the grammar map ALONE, written after every load and never read
    // before one, so it memoized nothing at all — every realized unit re-built
    // every shipped grammar. It now carries the claim INDEX too, which is the
    // half that stops the loads happening.
    ShippedSourceLanguageCache shippedSourceGrammars;
    std::vector<CuBuildKey> keyPerTarget;
    keyPerTarget.reserve(targets.size());
    for (std::size_t ti = 0; ti < targets.size(); ++ti) {
        std::string const& spec = targets[ti];
        CuBuildKey key;
        std::span<PredefinedMacroDef const> targetPredefines;
        std::span<PredefinedMacroDef const> formatPredefines;
        // ★★ KEYED UNCONDITIONALLY — see `CuBuildKey::languageName` for why
        // this must NOT join the `ppEnabled` block below, and for why the
        // CONFIG NAME is the identity rather than the declared one.
        auto const& resolvedGrammar = grammarPerTarget[ti];
        key.languageName = std::string{resolvedGrammar->configName()};
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
            // D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF: DSS's own runtime
            // units NO LONGER JOIN THIS SET. They used to be appended here as
            // extra translation units of every build, which meant preprocessing
            // and parsing them on EVERY invocation and then discarding them for
            // every program that did not reach them — ✔MEASURED at 111 ms of a
            // 190 ms pe64 compile of `int main(void){return 0;}`. They are now
            // resolved per TARGET, below, as cached static archives; see
            // `resolveShippedRuntimeArchives`.
            cuByKey.emplace(key, buildCus(key, targetPredefines,
                                          formatPredefines, resolvedGrammar,
                                          // D-LK-OBJECT-INPUT-COMPILE-FLAG-SURFACE:
                                          // the SOURCES, never every gathered
                                          // file — the objects are already out.
                                          std::span<std::string const>{
                                              sourcesToBuild}));
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
    // ── D-PROGRAM-PROJECT-WIDE-PARSE-GATE-MASKS-CENSUS: THE GATE IS PER-UNIT ──
    //
    // ★★★ WHAT USED TO BE HERE, AND WHY IT WAS THE LARGEST DEFECT OF ITS CLASS.
    // The drain below fed EVERY CU of EVERY build key into one run-wide
    // reporter, and the next statement was a bare `if (rep.hasErrors())` ->
    // drain -> `return 1`, commented *"the per-target loop would only produce
    // derivative noise"*. `compileOneTarget` is the site that runs semantic
    // analysis, HIR, MIR, LIR, assembly and link — so ONE parse error in ONE
    // translation unit DELETED the semantic-and-later census for ALL of them.
    // Not degraded: deleted. The `S_*` / `I_*` / `H_*` / `M_*` families could
    // not appear at all, and their absence was indistinguishable from their
    // non-existence. Two registry rows carried a false blocking relationship
    // because of that inference, and a cycle was ordered around it.
    //
    // ★★★ THE SCOPE DECISION, AND IT IS THE REFERENCES OWN VERDICT. ✔MEASURED
    // 2026-08-27 on a two-TU program (TU-1 a parse error, TU-2 an undeclared
    // identifier): `gcc 13.3.0 -std=c2x` and `clang 18.1.3 -std=c23` BOTH report
    // the parse error AND the semantic error, on `-c` and on a real LINK job
    // alike, and neither writes an artifact. `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO
    // C` therefore does not merely PERMIT continuing — it REQUIRES it. DSS
    // reported only TU-1 and was silent about TU-2.
    //
    // ★★ SO THE GATE'S SCOPE MOVES FROM THE RUN TO THE UNIT, AND ITS REFUSAL IS
    // KEPT WHOLE. A unit that failed to parse is EXCLUDED — its broken tree
    // never reaches a later tier, which is the derivative noise the original
    // comment was right to avoid. Every unit that DID parse is analysed. And no
    // artifact is written by any target, because a program missing a
    // translation unit is not a program: `analysisOnly` stops each target after
    // the per-CU front half, BEFORE `mergeCuMirs` — the first cross-unit step,
    // and therefore the first one that could invent a complaint (a missing
    // entry point, an unresolved cross-unit symbol) out of the EXCLUSION rather
    // than out of the source. That is exactly where gcc stops: it compiles each
    // unit and declines to link.
    //
    // ⚠ `L_*` AND `K_*` STAY UNMEASURED WHEN A UNIT FAILS, DELIBERATELY, AND
    // THE NOTICE SAYS SO. Those tiers consume a MERGED program; running them
    // over a knowingly-incomplete one is how a clean refusal becomes a cascade.
    // The honest answer is to NAME them as not-run, not to guess at them.
    std::size_t                       totalCus  = 0;
    std::size_t                       failedCus = 0;
    std::map<CuBuildKey, std::size_t> liveCusPerKey;
    for (auto& kv : cuByKey) {
        std::vector<CompilationUnit>& keyCus = kv.second;
        std::vector<char>             cuFailed(keyCus.size(), 0);
        for (std::size_t ci = 0; ci < keyCus.size(); ++ci) {
            CompilationUnit const& cu = keyCus[ci];
            // ★ THE DELTA *AND* THE UNIT'S OWN VERDICT, BOTH, BECAUSE EACH SEES
            // WHAT THE OTHER CANNOT. `rep.errorCount()` moves when a WARNING is
            // promoted by `--warnings-as-errors` (the unit's own reporter,
            // which never saw that policy, still calls itself clean); the
            // unit's own `hasErrors()` still fires when `rep`'s dedup window
            // collapses the only error a unit had against an identical earlier
            // one. The UNION is conservative in the safe direction: a unit that
            // errored is excluded.
            auto const errorsBefore = rep.errorCount();
            bool       failed       = cu.driverDiagnostics().hasErrors();
            copyDiagnostics(cu.driverDiagnostics(), rep);
            for (auto const& tree : cu.trees()) {
                if (tree.diagnostics().hasErrors()) failed = true;
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
            if (rep.errorCount() != errorsBefore) failed = true;
            cuFailed[ci] = failed ? 1 : 0;
            ++totalCus;
            if (failed) ++failedCus;
        }
        // Compact the survivors to the FRONT, order preserved, so a target can
        // be handed `span{data(), live}` without a second container. Safe only
        // BECAUSE the loop above already copied every unit's diagnostics into
        // `rep` and every unit's buffers into `bufs` (which owns them by
        // `shared_ptr`) — the husks left in the tail are never read again.
        std::size_t live = 0;
        for (std::size_t ci = 0; ci < keyCus.size(); ++ci) {
            if (cuFailed[ci] != 0) continue;
            if (ci != live) keyCus[live] = std::move(keyCus[ci]);
            ++live;
        }
        liveCusPerKey.emplace(kv.first, live);
    }

    if (failedCus != 0) {
        emitPhasesNotRun(
            rep,
            std::to_string(failedCus) + " of " + std::to_string(totalCus)
                + " translation unit(s) failed to parse and were EXCLUDED from "
                  "the later tiers; semantic analysis, HIR and MIR DID run for "
                  "the other "
                + std::to_string(totalCus - failedCus)
                + ". The cross-unit tiers -- module merge, LIR, assembly "
                  "and link -- ran for NOTHING, and NO ARTIFACT WAS WRITTEN, "
                  "because a program missing a translation unit is not a "
                  "program."
                + kNotMeasuredNotAbsent);
    } else if (rep.hasErrors()) {
        // A driver-tier error with no failing unit behind it. There is nothing
        // to exclude, so there is nothing to continue WITH — the run stops, and
        // says so rather than leaving the reader to infer it.
        emitStoppedBeforeCuBuild(rep, "the translation-unit build",
                                 sourcesToBuild.size());
        drainDiagnosticsToStderr(rep, bufs);
        return 1;
    }

    // A failed unit fails the BUILD, exactly as before this change: the only
    // thing that moved is how much of the program gets MEASURED on the way to
    // that verdict.
    bool const analysisOnly = failedCus != 0;
    int        exitCode     = analysisOnly ? 1 : 0;
    // D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF: where a cache MISS compiles
    // its archive before it can be stored, and where the build links it from
    // when the store cannot write. Declared HERE so its lifetime covers every
    // target's link — a staging area torn down between the compile and the pull
    // would take the archive with it. Creates nothing until a miss.
    RuntimeArchiveStaging runtimeStaging;
    // Memoized on the SPEC STRING, which is what the cache key's `target=` term
    // carries verbatim — not on `CuBuildKey`, whose target/format halves are
    // populated only when the language preprocesses. Two targets naming one
    // spec resolve their archives once.
    // ⚠ `nullopt` = THIS SPEC'S RUNTIME COULD NOT BE RESOLVED, and it is a
    // distinct state from "resolved to nothing" (an empty vector, which is every
    // format that realizes no unit). It is memoized because two targets naming
    // one spec must reach the SAME verdict: re-running the resolver for the
    // second would emit the refusal twice and, worse, could answer differently.
    std::map<std::string, std::optional<std::vector<fs::path>>>
        runtimeArchivesBySpec;
    // AP6: sized ONCE, here — every entry starts disengaged, so a target whose
    // build fails leaves `nullopt` rather than a stale or invented path, and the
    // vector stays index-parallel to `targets` for the ones that succeeded.
    artifactsOut.assign(targets.size(), std::nullopt);
    for (std::size_t i = 0; i < targets.size(); ++i) {
        std::string const& spec = targets[i];
        // Route this target to the CUs built for its object-format-kind (c9).
        std::vector<CompilationUnit> const& cus = cuByKey.at(keyPerTarget[i]);
        // D-PROGRAM-PROJECT-WIDE-PARSE-GATE-MASKS-CENSUS: the units that PARSED,
        // compacted to the front above.
        //
        // ⚠ `!cus.empty() && liveCus == 0`, NEVER `liveCus == 0` ALONE, AND THE
        // DIFFERENCE IS A REGRESSION THIS CAUGHT. Zero units is ALSO the normal,
        // successful shape of an ALL-OBJECT LINK (`--compile a.obj b.obj`,
        // D-LK-OBJECT-INPUT-COMPILE-FLAG-SURFACE): nothing is parsed because
        // there is no source, and `compileOneTarget` must still run to link the
        // pre-assembled objects. The bare test conflated "every unit failed"
        // with "there were never any units" and silently stopped linking them —
        // ✔MEASURED as four reds in `program/test_static_link`
        // (`ObjectInputCompileSurface.*`), including one that turned a
        // fail-loud refusal into rc=0.
        //
        // With units present and none surviving there is genuinely nothing to
        // analyse, and skipping is not a silent drop: `exitCode` is already 1
        // and every excluded unit is already named in the stream by its own
        // parse errors.
        std::size_t const liveCus = liveCusPerKey.at(keyPerTarget[i]);
        if (!cus.empty() && liveCus == 0) continue;
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
        compileOpts.ltoMode = ltoMode == LtoModeArg::Thin
                                  ? CompileOptions::LtoMode::Thin
                                  : CompileOptions::LtoMode::Full;
        // c162 (D-FF1-READER-CONSUMER) + AP6: THIS target's list — the
        // program-wide entries plus whatever was added for this spec, resolved
        // in the pass above. Identical to `resolveLibraries` when no additions
        // were supplied, which is every caller outside AP6.
        compileOpts.resolveLibraries = resolveLibsPerTarget[i];
        // D-LK-OBJECT-INPUT-COMPILE-FLAG-SURFACE: the objects the partition
        // took out of the gathered file list, on the SAME channel a
        // `--resolve-library`-named object lands in — `compileOneTarget`'s own
        // partition APPENDS to this vector, so an operator may spell some
        // objects one way and some the other and the link sees one list. The
        // gathered ones lead, because they are the ones the operator named as
        // INPUTS and the first of them is what an all-object link elects as its
        // client module.
        compileOpts.objectInputs     = objectInputsFromSources;

        // ── D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF: THIS TARGET'S SHIPPED
        //    RUNTIME, AS CACHED STATIC ARCHIVES ────────────────────────────
        //
        // Resolved HERE rather than in the CU-build loop above because a cache
        // HIT must not require a front end at all: the whole saving is in not
        // preprocessing and parsing a unit whose object already exists.
        //
        // ⚠ A FAILURE HERE FAILS THIS TARGET. `scratch` is the per-target
        // reporter and `scratch.hasErrors()` already decides the exit code
        // below, so a unit that cannot be resolved, keyed, verified or compiled
        // stops the build exactly where the old in-graph CU build did.
        // ⓘ `formatByName`/`targetByName` are keyed off the SAME parse the
        // pre-flight used, and every surviving spec is in both.
        {
            auto const it = runtimeArchivesBySpec.find(spec);
            if (it == runtimeArchivesBySpec.end()) {
                std::vector<fs::path> resolvedArchives;
                auto const parsedForRuntime = TargetSpec::parse(spec);
                // ★ THE VERDICT IS THE ERROR COUNT ACROSS THE CALL, NOT A
                // RETURN VALUE. The resolver reports per UNIT and keeps going,
                // so a corpus with two broken units names both instead of
                // stopping at the first — the same all-reasons-in-one-run
                // discipline the target/format pre-flight follows. Reading
                // `scratch` before and after is what turns those per-unit
                // reports into one per-target verdict.
                std::size_t const errorsBefore = scratch.errorCount();
                // A language with no preprocess pass has no active format on
                // its CU key; the realization map is FORMAT-keyed, so such a
                // build realizes nothing — exactly as it did before the cache.
                if (parsedForRuntime.has_value()
                    && keyPerTarget[i].format.has_value()) {
                    resolvedArchives = resolveShippedRuntimeArchives(
                        spec, *targetByName.at(parsedForRuntime->targetName),
                        *formatByName.at(parsedForRuntime->formatName),
                        *grammarPerTarget[i],
                        shippedSourceGrammars, config, runtimeStaging, scratch);
                }
                if (scratch.errorCount() != errorsBefore) {
                    runtimeArchivesBySpec.emplace(spec, std::nullopt);
                } else {
                    runtimeArchivesBySpec.emplace(spec,
                                                  std::move(resolvedArchives));
                }
            }
        }
        // ★★★ A RUNTIME THAT COULD NOT BE RESOLVED STOPS THIS TARGET *BEFORE*
        // ANYTHING IS WRITTEN, and that ordering is the whole point rather than
        // tidiness. ✔MEASURED before this arm existed: breaking `dirent.c` in a
        // staged tree produced the correct refusal, exit code 1 — AND a
        // `hello.exe` on disk plus a `dsscp: artifact …` line announcing it. A
        // build that failed must not leave a working-looking binary behind: the
        // image would be missing a runtime body, and with
        // [[D-LINK-EXEC-UNDEFINED-SYMBOL-FAIL-LOUD]] still OPEN it would link
        // clean and die at run time — which is the exact failure this whole
        // mechanism exists to prevent, reintroduced one tier further down.
        //
        // ⓘ `artifactsOut[i]` is left DISENGAGED (it was never assigned), so a
        // consumer reading this build's artifacts sees "this target produced
        // nothing" rather than a path to a file that must not be linked.
        if (!runtimeArchivesBySpec.at(spec).has_value()) {
            mergeWithTargetContext(scratch, spec, rep);
            exitCode = 1;
            continue;
        }
        // ★★★ COMPILE-ALWAYS IS NOT LINK-ALWAYS, AND THIS IS THE LINK HALF.
        // The resolution above ran for EVERY build of a format that realizes
        // units — that is the ruling, and it is what keeps a broken runtime unit
        // caught by every build of its target rather than by the one program
        // that still uses it. What reaches an ARTIFACT is a separate question,
        // and a STATIC ARCHIVE answers it differently from an image:
        //
        // ⚠ A LIBRARY BUILD CARRIES EVERY MEMBER OF EVERY INPUT ARCHIVE, never
        // the referenced subset — `extractStaticArchiveMembers`, deliberately,
        // because dropping an unreferenced member would silently ship an
        // incomplete library. ✔MEASURED: handing the runtime archives to a
        // library build turned a one-CU `-staticlib` into a THREE-member `.lib`
        // and a fat archive built from it into a SIX-member one. Nothing is
        // lost by withholding them: a relocatable artifact's whole contract is
        // that it DEFERS "who owns this name" to a later link
        // (`allowsUndefinedImports()`), and that later link is an IMAGE build —
        // which supplies the runtime right here.
        // ⚠ READ OFF THE FORMAT'S DECLARED `container`, never off its name or
        // kind: the same predicate the driver's own static-archive arm is
        // dispatched on.
        //
        // ★★ APPENDED AFTER THE OPERATOR'S OWN `--resolve-library` ARCHIVES,
        // BECAUSE THE OPERATOR OUTRANKS THE PLATFORM DEFAULT. That is the same
        // precedence `resolveOperatorNamedLibraryImports` already encodes on the
        // rebinding side, reused rather than re-decided: an operator who
        // statically links their own `opendir` gets theirs, and a build that
        // names none gets DSS's shipped body. Order is load-bearing — the armap
        // is FIRST-WINS across archives in list order.
        // ⓘ They ride the SAME `--resolve-library` channel and are therefore
        // dispatched by MAGIC BYTES like every other input: the archive goes to
        // the static merge, and the demand-driven member pull is what keeps a
        // `hello.c` free of directory-walking code it never calls.
        if (!formatByName.at(TargetSpec::parse(spec)->formatName)
                 ->isStaticArchive()) {
            for (auto const& archive : *runtimeArchivesBySpec.at(spec))
                compileOpts.resolveLibraries.push_back(
                    ResolveLibrarySpec{archive});
        }
        // ── D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION (C):
        //    THE `buildCus` BOUNDARY, WHICH IS WHERE THE KEY IS COMPUTABLE ───
        //
        // ★★★ EVERY CU OF EVERY KEY IS FRONT-ENDED BY NOW (the `cuByKey` loop
        // above ran to completion) AND THIS TARGET HAS NOT BEEN COMPILED, so
        // `∪ cu.inputDigest()` over `cus` exists and nothing downstream of it
        // has happened yet. That is the ONLY point in the driver where both are
        // true. `compile_pipeline.cpp`'s per-CU `analyze()` is NOT it — it sits
        // inside the per-unit half and cannot see the union, so a key built
        // there would key a WHOLE-ARTIFACT cache from inside a per-unit loop.
        //
        // ⚠ IT IS BUILT AFTER `compileOpts` IS COMPLETE, INCLUDING THE RUNTIME
        // ARCHIVES APPENDED JUST ABOVE. Those archives are link inputs and
        // therefore key terms; building the key any earlier would omit them.
        //
        // ⚠ AND NOT WHEN `analysisOnly` IS SET: that program is knowingly
        // incomplete and writes no artifact by design, so there is nothing to
        // serve and nothing to store.
        std::optional<ArtifactCacheTicket> cacheTicket;
        if (artifactCachePolicy.has_value() && artifactCachePolicy->enabled
            && !analysisOnly) {
            auto const parsedForCache = TargetSpec::parse(spec);
            auto key = parsedForCache.has_value()
                         ? buildDependencyArtifactKey(
                               *artifactCachePolicy, spec,
                               *targetByName.at(parsedForCache->targetName),
                               *formatByName.at(parsedForCache->formatName),
                               *grammarPerTarget[i],
                               std::span<CompilationUnit const>{cus.data(),
                                                                liveCus},
                               artifactName.value_or(sourceStem),
                               outputExtensionFor(
                                   *formatByName.at(parsedForCache->formatName)),
                               compileOpts, imageRequest)
                         : std::unexpected(std::string{
                               "dependency artifact cache: the target spec does "
                               "not parse, so no key can be composed."});
            if (key.has_value()) {
                cacheTicket = ArtifactCacheTicket{
                    std::move(*key),
                    storeEvictionFor(artifactCachePolicy->eviction)};
            } else {
                // ⚠ LOUD, AND NOT FATAL. A key that cannot be COMPUTED means
                // the optimization is unavailable for this target — not that
                // any bytes are suspect — so the build compiles normally and
                // says why on every affected run. The arm that DOES stop a
                // build is an unverifiable ENTRY, which lives at the lookup
                // inside `compileOneTarget`.
                reportDependencyCacheNote(key.error());
            }
        }
        // D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET: the grammar THIS target's
        // CU was parsed under — never the invocation's, which no longer
        // exists as a single value.
        auto artifact = compileOneTarget(
            std::span<CompilationUnit const>{cus.data(), liveCus},
            *grammarPerTarget[i], sourceStem, spec, scratch,
            outputDir, /*multiTargetBuild*/ targets.size() > 1u,
            artifactName, perFormatOutputSubdir, compileOpts,
            executor, jobsOverride, imageRequest, analysisOnly, cacheTicket);
        mergeWithTargetContext(scratch, spec, rep);
        // AP6: a target counts as having produced an artifact only if it BOTH
        // wrote one and reported no error — the same conjunction the exit code
        // uses. A scratch error alongside a written file means the build is
        // failing, and handing a consumer that file would let a failed
        // dependency be linked into something that then "succeeds".
        if (!artifact || scratch.hasErrors()) exitCode = 1;
        else                                  artifactsOut[i] = std::move(artifact);
    }

    drainDiagnosticsToStderr(rep, bufs);
    return exitCode;
}

// ══ THE `--time` REPORT ══════════════════════════════════════════════════════
//
// ★★★ TWO CLOCKS, TWO COLUMNS, AND A REMAINDER THAT CANNOT LIE.
//
// The old report printed ONE time column per phase and defined its trailing
// `[other]` row as `processWall - Σ(phase times)`. Both halves of that were
// wrong the moment the driver grew a thread pool. The per-phase number is
// THREAD-time summed over every worker, so on the SQLite corpus the attributed
// sum came to 226.7 s against a 212.2 s wall — the subtraction went 14.5 s
// NEGATIVE and a `std::max`-style clamp printed `[other] 0ms`. A row that reads
// "nothing is unaccounted for" while the accounting is 14.5 s inconsistent is
// not a rounding artifact; it is the report telling the operator a fact that is
// false, and it is why nobody noticed the front half was still serial.
//
// So the table now prints, per phase:
//   cpu   — Σ self-time over every thread. Answers "what did this cost".
//   wall  — the UNION of that phase's self-intervals on the timeline. Answers
//           "how much of the build's duration did this occupy". <= cpu ALWAYS.
//   peak  — high-water mark of simultaneously-active scopes. 1 ⇒ the phase
//           never overlapped itself, so cpu and wall are the same number and
//           the reader can compare them directly.
//   runs  — unchanged; disambiguates multi-CU / multi-target accumulation.
//
// and, below it, `[other] = processWall - pipelineBusyWall`, where the
// subtrahend is the union over ALL phases — a set of intervals measured on the
// process's own steady clock, hence a subset of the process's lifetime. That
// subtraction cannot underflow. If it ever does, the substrate is broken, and
// this function says so LOUDLY and FAILS THE RUN rather than clamping.
//
// ★ WHY AN INVARIANT VIOLATION FAILS THE RUN. `--time` is an explicit request
// for a MEASUREMENT. Printing a violation banner and still exiting 0 would let
// a CI job scroll past it exactly the way the clamped `0ms` was scrolled past
// for the whole life of this flag. The compiled OUTPUT is unaffected — the
// violation says the instrumentation disagrees with itself, never that the
// artifact is wrong — but a run whose requested measurement is self-
// contradictory has not delivered what was asked for. Runs WITHOUT `--time`
// cannot be affected: this function is not called.
//
// EVERY phase prints (zero-run phases included) so the report's shape stays
// deterministic and pin-able. Phase names are pipeline verbs (see
// `compilePhaseName`) — lang/target/format-neutral, like everything here.
[[nodiscard]] int emitPhaseTimeReport(
    std::chrono::steady_clock::time_point start, int dispatchRc,
    std::ostream& os) {
    auto const totalNs = static_cast<std::uint64_t>(
        std::max<long long>(0, std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - start).count()));
    auto const ms = [](std::uint64_t ns) {
        return formatWallTime(static_cast<long long>(ns / 1'000'000u));
    };
    constexpr char const* kPfx = "dsscp:   ";

    os << "dsscp: compile time " << ms(totalNs) << "  (process wall)\n";
    os << kPfx << std::format("{:<21}{:>12}{:>12}{:>7}{:>8}", "phase", "cpu",
                              "wall", "peak", "runs")
       << "\n";

    // Collected first, printed second: the violation check below needs every
    // row, and a report that printed half a table and then announced the table
    // was invalid would be worse than either.
    std::vector<substrate::PhaseTimers::Row> rows(substrate::kCompilePhaseCount);
    std::uint64_t attributedCpuNs = 0;
    std::uint64_t sumPhaseWallNs  = 0;
    for (std::size_t i = 0; i < substrate::kCompilePhaseCount; ++i) {
        auto const p = static_cast<substrate::CompilePhase>(i);
        rows[i] = substrate::PhaseTimers::read(p);
        attributedCpuNs += rows[i].cpuNanoseconds;
        sumPhaseWallNs  += rows[i].wallNanoseconds;
        os << kPfx
           << std::format("{:<21}{:>12}{:>12}{:>7}{:>8}",
                          substrate::compilePhaseName(p),
                          ms(rows[i].cpuNanoseconds), ms(rows[i].wallNanoseconds),
                          rows[i].peakConcurrency, rows[i].runs)
           << "\n";
    }

    auto const busyNs = substrate::PhaseTimers::busyWallNanoseconds();
    auto const live   = substrate::PhaseTimers::liveScopeCount();

    // ── The invariants. Each is impossible under correct accounting. ──────────
    std::vector<std::string> violations;
    if (live != 0) {
        violations.push_back(std::format(
            "{} phase scope(s) are still LIVE at report time — a worker "
            "outlived the measurement window, so the wall-clock union below is "
            "missing an interval that is still open",
            live));
    }
    if (busyNs > totalNs) {
        violations.push_back(std::format(
            "pipeline busy wall ({}) EXCEEDS process wall ({}) by {} — a union "
            "of intervals measured on this process's own steady clock cannot "
            "outlast the process",
            ms(busyNs), ms(totalNs), ms(busyNs - totalNs)));
    }
    if (sumPhaseWallNs < busyNs) {
        violations.push_back(std::format(
            "the sum of the per-phase wall columns ({}) is LESS than the "
            "all-phase union ({}) — a union over the whole set cannot exceed "
            "the sum of its parts",
            ms(sumPhaseWallNs), ms(busyNs)));
    }
    for (std::size_t i = 0; i < substrate::kCompilePhaseCount; ++i) {
        if (rows[i].wallNanoseconds > rows[i].cpuNanoseconds) {
            violations.push_back(std::format(
                "phase '{}': wall ({}) exceeds cpu ({}) — a phase's occupancy "
                "of the timeline cannot exceed the thread-time spent in it",
                substrate::compilePhaseName(static_cast<substrate::CompilePhase>(i)),
                ms(rows[i].wallNanoseconds), ms(rows[i].cpuNanoseconds)));
        }
    }

    os << kPfx << std::format("{:<21}{:>12}{:>12}", "attributed", ms(attributedCpuNs),
                              ms(busyNs))
       << "   (cpu = Σ threads; wall = all-phase union)\n";
    if (violations.empty()) {
        os << kPfx
           << std::format("{:<21}{:>24}", "[other]", ms(totalNs - busyNs))
           << "   (process wall not inside any phase)\n";
        // Achieved parallelism, printed only when it is meaningful. `busy` is
        // the wall time the pipeline was doing instrumented work, so cpu/busy
        // is the average number of threads that work occupied.
        if (busyNs > 0) {
            os << kPfx
               << std::format("{:<21}{:>24.2f}x", "parallelism",
                              static_cast<double>(attributedCpuNs)
                                  / static_cast<double>(busyNs))
               << "   (attributed cpu / busy wall)\n";
        }
    } else {
        // ★ NEVER CLAMPED. The remainder is not printed at all when it cannot
        // be computed honestly — a `0ms` in its place is precisely the defect
        // being removed.
        os << "dsscp: *** --time INVARIANT VIOLATION — the phase "
              "accounting is inconsistent with itself, so the unattributed "
              "remainder is NOT reported ***\n";
        for (auto const& v : violations) {
            os << "dsscp:   ! " << v << "\n";
        }
    }

    // ── The per-FILE pre-scan memo, because `preprocess-splice` is the phase a
    //    reader of this report will land on and the NEXT question is always
    //    "how much of that was work we had already done?" ──────────────────────
    //
    // ★ A TIME WITHOUT ITS WORK COUNT CANNOT BE ACTED ON. ✔MEASURED 2026-08-28
    // on the WSL x86_64 leg: `preprocess-splice` was the largest phase in a
    // `--jobs 1` build at 64.25 s CPU against 21.91 s for the same work at
    // `--jobs 4`, and the report could not say whether that meant "read many
    // distinct headers" or "re-read the same ones" — which are different
    // defects with different fixes. These two counters answer it in the same
    // breath as the phase that raises the question.
    // ⓘ `builds` counts files actually read + spliced + tokenized; `hits` counts
    // requests the memo served instead. See `PreScanMemoCounters`.
    {
        auto const memo = PreScanMemoCounters::read();
        if (memo.builds != 0 || memo.hits != 0) {
            std::uint64_t const asked = memo.builds + memo.hits;
            os << kPfx
               << std::format("{:<21}{:>12}{:>12}", "pre-scan memo",
                              std::to_string(memo.builds) + " built",
                              std::to_string(memo.hits) + " hit")
               << std::format("   ({} include request(s), {:.1f}% served)\n",
                              asked,
                              asked ? 100.0 * static_cast<double>(memo.hits)
                                          / static_cast<double>(asked)
                                    : 0.0);
        }
    }

    // ── Per-job wall timing for the CU batches (see `JobBatchRecord`). ───────
    for (auto const& b : readJobBatches()) {
        std::uint64_t sumJobNs = 0;
        std::uint64_t maxJobNs = 0;
        for (auto const n : b.jobWallNs) {
            sumJobNs += n;
            maxJobNs = std::max(maxJobNs, n);
        }
        os << kPfx << std::format("{:<21}", b.stage)
           << std::format("width {:<4} jobs {:<5} batch {:>10}  Σjobs {:>10}",
                          b.width, b.jobWallNs.size(), ms(b.batchWallNs),
                          ms(sumJobNs))
           << std::format("  conc {:>5.2f}x  slowest {:>10}",
                          b.batchWallNs > 0
                              ? static_cast<double>(sumJobNs)
                                    / static_cast<double>(b.batchWallNs)
                              : 0.0,
                          ms(maxJobNs))
           << "\n";
    }

    if (violations.empty()) return dispatchRc;
    return dispatchRc != 0 ? dispatchRc : 1;
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
    // D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED (the ACCEPTANCE half):
    // an unusable `-I` is named HERE, at the point the driver accepts it,
    // rather than surfacing later as a missing-header error pointing at the
    // `#include`. It WARNS and never refuses — see
    // `InputResolver::checkSearchDirectoriesUsable` for the gcc/MSVC
    // measurement that settles which of the two it has to be.
    //
    // ★ THIS SITE COVERS EVERY MODE, because it sits before the dispatch fork
    // beside the other CLI stamps. The manifest's `includes` are checked at
    // their own acceptance point in `compileProject`, where they are merged —
    // one check per SURFACE, so neither is silent and neither doubles up.
    //
    // ⓘ The reporter is local and buffer-less, matching the other pre-parse
    // driver-tier sites: these diagnostics carry no source location because
    // their subject is a command-line argument, not a token.
    {
        DiagnosticReporter argRep{cfg};
        (void)InputResolver::checkSearchDirectoriesUsable(
            args.includeDirs, "-I", argRep);
        drainDiagnosticsToStderr(argRep);
        // Only `--warnings-as-errors` can make this fatal; without it the
        // count stays zero and the run continues, exactly as gcc and MSVC do.
        if (argRep.errorCount() > 0u) return 1;
    }
    // D-OPT1-PIPELINE-CONFIG-FROM-COMPILECONFIG: thread the CLI's
    // `--config=<debug|release>` into the kernel so the right
    // shipped pipeline gets loaded at compile_pipeline step 3.5.
    setCompileConfig(args.config);
    // D-OPT11-LAZY-IMPORT-EDGE: `--lto <full|thin>` — the link-time-optimization
    // topology. Stamped beside the build configuration because it is the same
    // kind of fact: a property of the SHAPE of this build, not of a pass
    // schedule.
    setLtoMode(args.lto);
    setJobs(args.jobs);  // D-PERF-4-CU-PARALLELISM: --jobs N per-CU build pool width
    // AP6 / B.4: `--force-git-cache` — bypass the `.dss-deps` cache-hit
    // short-circuit. Stamped before the dispatch fork like every other global;
    // only `compileProject` has a `dependsOn` list to apply it to, which is
    // what the parser's mode gate already enforces.
    setForceGitCache(args.forceGitCache);
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
    // `--time`: report the compilation's timings to stderr when this run
    // returns — covers EVERY compile-producing mode (project / transpile /
    // directory / compile) through ONE report site, no per-mode duplication.
    // A zero-arg run never reaches here with time==true (parseCliArgs rejects
    // options without a mode → NoModeSelected), so the report only ever
    // describes a real compile. Universal driver concern — lang/target/
    // format-neutral.
    //
    // ★ THE DISPATCH FORK IS AN IIFE SO THE REPORT CAN CHANGE THE EXIT CODE.
    // This used to be an RAII guard whose DESTRUCTOR printed, which covered
    // every `return` below but ran strictly after the return value was fixed —
    // so a report that detects its own numbers are inconsistent had no way to
    // say so in the process's status. `emitPhaseTimeReport` returns the exit
    // code, and an invariant violation turns a clean compile into a failed run
    // (see there for why that is the right trade). Coverage of every return
    // path is preserved by wrapping the fork rather than by the destructor.
    // Nothing is lost on the exceptional path: `main` installs no handler, so
    // an escaping exception was never going to unwind through here anyway.
    auto const timedRunStart = std::chrono::steady_clock::now();
    int const  dispatchRc    = [&]() -> int {
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
    // back-compat path for `dsscp` with zero arguments. The
    // parseCliArgs `NoModeSelected` guard already rejects the case
    // where the user supplied options without a mode flag, so we
    // know all CliArgs are at their defaults here.
    std::cout << "DSS Code Prime compiler ready.\n"
              << "Run `dsscp --help` for usage.\n";
    return 0;
    }();
    if (!args.time) return dispatchRc;
    return emitPhaseTimeReport(timedRunStart, dispatchRc, std::cerr);
}

namespace {

// The SHIPPED object formats that serve `profile`, by format name — the
// measurement the AP3 gate's reject arm needs to tell "you picked the wrong
// format" from "no format implements this anywhere" (closing
// D-AP3-UNSERVED-PROFILE-MISREPORTS-AS-A-FORMAT-MISMATCH). Sorted, because it
// is printed and a diagnostic must read the same on every host rather than
// inheriting the filesystem's enumeration order.
//
// `nullopt` ⇒ the shipped object-format DIRECTORY could not be located, which
// is NOT the same answer as "no format serves it" and must never collapse into
// it: an empty vector is the input that selects the "no backend exists"
// message, so returning one here would manufacture the exact confident
// falsehood this split exists to remove. The caller reports the load failure
// instead.
//
// It is called ONLY on the reject path — a build whose profile IS served pays
// nothing for it. `dependency_resolver.cpp` reads the same inventory for a
// different question (WHICH format to build a dependency with) and memoizes it
// per resolve; this one runs at most once per build, immediately before the
// driver returns 1.
[[nodiscard]] std::optional<std::vector<std::string>>
shippedFormatsServingProfile(std::string_view profile) {
    auto const dir = findShippedConfigDir("object-formats");
    if (!dir) return std::nullopt;

    std::error_code ec;
    std::vector<std::string> names;
    for (fs::directory_iterator it{*dir, ec}, end; !ec && it != end;
         it.increment(ec)) {
        std::string const file = it->path().filename().string();
        constexpr std::string_view kSuffix = ".format.json";
        if (file.size() <= kSuffix.size()) continue;
        if (!std::string_view{file}.ends_with(kSuffix)) continue;
        names.push_back(file.substr(0, file.size() - kSuffix.size()));
    }
    std::sort(names.begin(), names.end());

    std::vector<std::string> serving;
    for (auto const& n : names) {
        auto loaded = ObjectFormatSchema::loadShipped(n);
        // A shipped document that will not LOAD serves nothing by definition,
        // and reporting its load failure here would put an unrelated format's
        // error in front of a profile diagnostic. The format the user actually
        // named was loaded by the caller and reported there.
        if (!loaded.has_value()) continue;
        // The SAME generic membership predicate the gate itself uses — no
        // per-profile-name and no format-identity branch.
        if (artifactProfileSupported((*loaded)->artifactProfiles(), profile)) {
            serving.push_back(n);
        }
    }
    return serving;
}

} // namespace

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

    // ── `dependsOn` IS NOW RESOLVED, NOT REFUSED (AP6) ──────────────────────
    //
    // Until the resolver landed this seam held a `D_PlanNotLanded` reject,
    // placed FIRST so a build that could not happen had no side effects on the
    // way to saying so. The resolver replaces the reject, and the placement
    // argument SURVIVES INVERTED: resolution is itself the most side-effecting
    // thing this function does — it clones remotes, runs each dependency's
    // pre-build hooks in that dependency's own tree, and BUILDS artifacts — so
    // it must sit BELOW the cheap gates that can refuse this manifest outright
    // (a bad profile, an unloadable grammar) and ABOVE the ROOT's own pre-build
    // hooks, which are the first thing that writes into the AUTHOR's tree. A
    // resolution that fails therefore still leaves the root's tree untouched,
    // which is the property `NonEmptyDependsOnFailsLoud…`'s successor pins.
    // See the call site further down, after the gates and the flag merges.

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
        // THE TWO REJECT ARMS. The membership question is asked FIRST and
        // separately so the inventory scan is paid only by a build that is
        // already being rejected — every accepted target reads exactly the one
        // format it named, as before.
        if (artifactProfileSupported((*fmtR)->artifactProfiles(),
                                     pc.artifactProfile)) {
            continue;
        }
        auto serving = shippedFormatsServingProfile(pc.artifactProfile);
        if (!serving.has_value()) {
            // The inventory could not be read, so WHICH reject is true is
            // unknown — and an empty list would silently assert the stronger,
            // possibly false one. Report what actually went wrong instead.
            emitDriver(rep, DiagnosticCode::D_SchemaLoadFailed,
                       "the shipped object-format directory "
                       "('src/dss-config/object-formats') could not be located, "
                       "so the artifact profile '" + pc.artifactProfile
                       + "' could not be checked against the formats that serve "
                         "it. Set DSS_CONFIG_ROOT to the directory that contains "
                         "'src/dss-config', or run from inside the compiler's "
                         "source tree.");
            drainDiagnosticsToStderr(rep);
            return 1;
        }
        // The verdict is already known (the predicate above is the SAME one
        // this call re-asks), so the return value carries no new information
        // and is deliberately discarded — the call's remaining job is to write
        // whichever of the two rejects `serving` selects. Reading it into an
        // `if` would imply a path that cannot exist.
        (void) enforceArtifactProfileFormat((*fmtR)->artifactProfiles(),
                                            pc.artifactProfile,
                                            parsed->formatName, *serving, rep);
        drainDiagnosticsToStderr(rep);
        return 1;
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
        // D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED (the ACCEPTANCE half),
        // the MANIFEST surface. Checked BEFORE the merge and over `pc.includes`
        // ALONE, not over the merged list: the CLI's own `-I` entries were
        // already checked at their acceptance point in `Program::run`, and
        // re-checking them here would report each of them TWICE for every
        // project build — the "one check per surface" rule the CLI site states.
        // `rep` is this call's reporter, so a manifest warning renders with the
        // project's diagnostics rather than through a second channel.
        (void)InputResolver::checkSearchDirectoriesUsable(
            pc.includes, "the project manifest's `includes`", rep);

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

    // ── `dependsOn` RESOLUTION (AP6) ────────────────────────────────────────
    //
    // ★ THE PLACEMENT IS THREE STATEMENTS, AND EACH IS LOAD-BEARING.
    //
    // (1) BELOW the two profile gates and the manifest merges. Resolution is
    // the most side-effecting thing this function does — it clones remotes,
    // spawns every dependency's pre-build hooks in that dependency's own tree,
    // and BUILDS artifacts — so a manifest this driver is going to refuse
    // outright (a profile its language does not declare, a target whose format
    // does not serve it) must be refused BEFORE any of that happens. This is
    // the same argument that placed the reject it replaces FIRST, applied to a
    // seam whose cost went from zero to "the whole dependency graph".
    //
    // (2) ABOVE the ROOT's own pre-build hooks. Those are the first thing that
    // writes into the AUTHOR's tree, and a build that cannot resolve its
    // prerequisites must not have run the author's codegen step on the way to
    // saying so. It is also the only correct order for the other direction: a
    // dependency is a PREREQUISITE, so a root hook that consumes one has it.
    //
    // (3) ABOVE the root's `sources[]` expansion, because the merged list this
    // produces is joined to the root's own — and M4(b) makes that JOIN's order
    // load-bearing (see the join below).
    //
    // The git seam is INJECTED when a caller supplied one and a real
    // `SystemGitRunner` otherwise. It is constructed unconditionally because it
    // is inert until used — no PATH scan happens until an operation asks — so
    // a manifest with no git dependency pays nothing for its existence, and
    // `resolveProjectDependencies` returns immediately on an empty `dependsOn`
    // without touching it at all.
    SystemGitRunner systemGit;
    DependencyResolveRequest depRequest;
    // The ROOT manifest's canonical directory is where the ONE `.dss-deps` for
    // the whole graph lives (M2) and is the first cycle-detection key.
    // `ProjectConfig` carries no path of its own, which is why the resolver
    // takes the manifest path rather than deriving anything.
    depRequest.rootManifestPath = fs::path{projectFilePath};
    depRequest.targets          = pc.targets;
    // U-9's base, stated rather than left implicit: `--output` when given, and
    // `<cwd>/target` when not — the same rule `resolveArtifactOutputDir`
    // applies to this build's own artifact, so a dependency's artifacts land
    // beside the consumer's rather than in a second convention.
    depRequest.artifactOutputBase =
        outputDir().has_value() ? *outputDir() : (fs::current_path() / "target");
    depRequest.compileConfig  = compileConfig();
    depRequest.jobs           = jobs();
    depRequest.executor       = executor();
    // D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION (C): the
    // ROOT manifest's policy, read HERE — the one place a root manifest is in
    // hand — and carried to `Resolver::buildNode_`, which stamps it onto every
    // DEPENDENCY's fresh `Program`.
    //
    // ★★ IT IS DELIBERATELY **NOT** STAMPED ONTO **THIS** `Program`, so the
    // ROOT's own artifact is never served from the cache, and the member's name
    // is the statement of that scope rather than an accident of wiring. A root
    // build is the thing the operator is running: they expect it to happen, its
    // artifact is the deliverable they will inspect, and its sources are the
    // ones being edited — so it is the build where a cache buys least and where
    // a wrong hit would be hardest to notice. A dependency is a PREREQUISITE
    // nobody is editing, which is exactly the shape a content-addressed cache
    // serves well. Widening this is a decision to take deliberately, with its
    // own row; it is not a wiring change.
    depRequest.dependencyArtifactCache = pc.dependencyArtifactCache;
    depRequest.forceGitCache  = forceGitCache();
    auto resolved = resolveProjectDependencies(
        pc, depRequest, gitRunner() != nullptr ? *gitRunner() : systemGit, rep);
    if (!resolved) {
        drainDiagnosticsToStderr(rep);
        return 1;
    }
    // The per-target ADDITIONS channel — the reason
    // `setResolveLibraryAdditionsByTarget` exists. An empty map (the
    // no-`ArtifactLink`-dependency case) adds nothing to anyone, so every
    // existing build stays byte-identical.
    setResolveLibraryAdditionsByTarget(std::move(resolved->libraryAdditionsByTarget));

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

    // D-AP2-SOURCES-GLOB + AP6 M4: `sources[]` entries → the concrete, deduped
    // file list, resolved by `expandAndDedupProjectSources`
    // (`program/project_sources.hpp`, which carries the whole policy docblock:
    // expansion, the zero-match / I-O fail-loud rules, the `weakly_canonical`
    // dedup key, and why ORDER is load-bearing). It runs BEFORE the
    // multi-vs-single-CU routing count is taken, so a `"src/**/*.c"` entry routes
    // EXACTLY as if its matches had been listed literally.
    //
    // ★ THE BASE IS EMPTY HERE, AND THAT IS A STATEMENT, NOT AN OMISSION. The
    // ROOT manifest's entries resolve against the PROCESS working directory —
    // the same base its `preBuildScripts` run in (see the hook call above) and
    // the same base every relative CLI input uses — so a hook that writes
    // `generated/main.c` and a manifest that reads `generated/*.c` mean the same
    // directory. Passing the manifest's OWN directory here would be a silent
    // behaviour change for every existing project whose cwd is not its manifest's
    // directory. The non-empty base is for a DEPENDENCY manifest, which declares
    // its sources relative to itself and has no other way to mean them.
    //
    // The helper reports its own fail-loud diagnostic and leaves draining to us,
    // matching the gate sites above; the delegate below drains the rest
    // (runCusToTargets).
    auto expandedSourcesOpt =
        expandAndDedupProjectSources(pc.sources, fs::path{}, rep);
    if (!expandedSourcesOpt) {
        drainDiagnosticsToStderr(rep);
        return 1;
    }
    std::vector<std::string> expandedSources = *std::move(expandedSourcesOpt);

    // ── M4(b): THE ROOT'S OWN SOURCES COME FIRST, AND THAT IS NOT COSMETIC ──
    //
    // ✔MEASURED: `sourceStem = fs::path{sourceFiles.front()}.stem()` NAMES THE
    // ARTIFACT whenever the manifest states no `artifactName`, and it names the
    // members of a static archive too. So appending a `module` dependency's
    // sources AHEAD of the root's would silently RENAME the emitted binary —
    // the build stays green, every diagnostic count stays zero, and the file
    // the operator was looking for is simply not there under that name.
    //
    // The de-dup key is `weakly_canonical`, matching
    // `expandAndDedupProjectSources`'s own, and it has to: the two lists come
    // from two manifests resolved against two different bases, so ONE file
    // spelled relatively by the root and absolutely by a dependency is the
    // NORMAL case here rather than an exotic edge — and its consequence is a
    // duplicate CU and a duplicate-symbol link error that names no manifest.
    // FIRST occurrence wins, keeping its own spelling, exactly as within a
    // single manifest.
    if (!resolved->mergedSources.empty()) {
        std::set<core::PathIdentity> seen;
        auto const key = [](std::string const& s) {
            return core::PathIdentity::of(fs::path{s});
        };
        for (auto const& s : expandedSources) seen.insert(key(s));
        expandedSources.reserve(expandedSources.size()
                                + resolved->mergedSources.size());
        for (auto const& s : resolved->mergedSources) {
            if (seen.insert(key(s)).second) expandedSources.push_back(s);
        }
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

// ✅ `resolveSystemDirs` / `applySystemDirs` WERE DEFINED HERE AND HAVE MOVED --
// D-LSP-HAS-NO-SYSTEM-INCLUDE-DIRS-AND-DROPS-THE-CU-DRIVER-DIAGNOSTICS.
//
// The FF11 walk that turned a language's `semantics.shippedLibDirs` into
// absolute system-include dirs lived at `dss` namespace scope in THIS file,
// published by nothing but a forward declaration at the top of it. Two other
// channels needed the same answer and could not have it: the LSP built every
// open document's `UnitBuilder` with NO system dirs at all, and
// `dump_predefined_macros.cpp` re-walked `shippedLibDirs` with a private loop.
// One fact, three sites, and the site with no copy is the one a user sat in.
//
//   * the WALK          -> `dss::resolveSystemDirs(GrammarSchema const&)`,
//                          `core/types/config_path_walk.hpp`, beside the
//                          `findShippedConfigDir` it wraps;
//   * the BUILDER BIND  -> `dss::applySystemDirs(UnitBuilder&, GrammarSchema const&)`,
//                          `analysis/compilation_unit/compilation_unit.hpp`,
//                          beside `UnitBuilder::addSystemDir`.
//
// Both are already reachable from here: this file includes both headers. Do NOT
// re-open a local copy -- the whole history of this seam is copies drifting
// ([[D-PROGRAM-APPLY-SYSTEM-DIRS-IGNORED-DSS-CONFIG-ROOT]], where the driver's
// private walk never read `$DSS_CONFIG_ROOT` and the shipped CLI could not
// resolve an angle include from any cwd outside its own source tree).

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
        // [[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]]: NOT bare
        // `fs::absolute`. ✔MEASURED — it re-roots `-I //host/share/inc` onto the
        // local drive with no error, so the search dir named a directory that
        // cannot exist and the header missed 0/30 for BOTH slash spellings while
        // the acceptance check, which saw the ORIGINAL string, stayed silent.
        fs::path const abs = core::absoluteKeepingRoot(fs::path{d}, ec);
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
    // AP6: `artifactPaths()` describes THE LAST RUN. Clear before anything can
    // return, or a rejected second call would still be answering with the first
    // call's artifacts — a consumer would then link a binary this invocation
    // never built.
    artifactPaths_.clear();
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
    // (`parser.maxExpressionDepth`, c = 1024) admits would overflow the
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
                        std::shared_ptr<GrammarSchema const> const& keyGrammar,
                        // D-LK-OBJECT-INPUT-COMPILE-FLAG-SURFACE: the files
                        // `runCusToTargets` decided are SOURCES. Not the
                        // enclosing `sourceFiles` — that list may also name
                        // pre-assembled objects, which have no front end.
                        std::span<std::string const> sources)
        -> std::vector<CompilationUnit> {
        // ⓘ Every source turned out to be an object ⇒ this build has no
        // translation unit at all, and an empty CU vector is the honest answer.
        // Returning a CU built from ZERO files would mint an empty unit the
        // link then has to recognise as nothing.
        if (sources.empty()) return {};
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
                for (auto const& path : sources) {
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
        ltoMode_,
        optimizerPipelineOverride_.has_value() ? &*optimizerPipelineOverride_ : nullptr,
        resolveLibraries_, resolveLibraryAdditionsByTarget_, artifactPaths_,
        executor_, jobs_,
        // D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: the CLI/manifest stack-reserve
        // request (nullopt = the format default stands).
        ImageRequest{stackReserveBytes_},
        // D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION (C):
        // the cross-build artifact cache policy. nullopt on every CLI build and
        // on every ROOT project build; engaged only on a DEPENDENCY sub-build,
        // where `Resolver::buildNode_` stamped the ROOT manifest's policy on.
        dependencyArtifactCache_);
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
    // AP6: same last-run contract as `compileFiles` — see there.
    artifactPaths_.clear();
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
    //
    // ══ D-PERF-4-CU-PARALLELISM, THE FRONT HALF ══════════════════════════════
    //
    // ★★★ THE WHOLE FRONT END WAS SERIAL ON EVERY CORE OF THE MACHINE.
    // This loop is preprocess + splice + tokenize + expand + parse +
    // resolve-imports for every translation unit, and it ran one file at a
    // time: ✔MEASURED 39.6 s of a 212 s SQLite build (103 TUs), at 1.0 CPU-
    // second per wall-second throughout. `callOnLargeStack` made that easy to
    // miss — it DOES spawn a thread, but it spawns it and immediately JOINS
    // it, so it buys stack depth, never concurrency.
    //
    // The batch below is the SAME SHAPE the back half has used since D-PERF-4
    // (see `compileOneTarget`), deliberately reused rather than reinvented:
    //   • write BY INDEX into a pre-sized slot vector — no shared container is
    //     mutated, so there is nothing to lock and nothing to order;
    //   • a `std::latch` with an RAII count-down guard, so a job that throws
    //     (the pool logs and swallows it) can never DEADLOCK `done.wait()` —
    //     its slot simply stays disengaged and fails the build loudly below;
    //   • collect the results in CU (index) ORDER after the join, NEVER in
    //     completion order.
    //
    // ★★ WHY THE OUTPUT IS BYTE-IDENTICAL AND THE DIAGNOSTICS KEEP THEIR ORDER.
    // The vector this returns is assembled by walking `i = 0..n-1` after the
    // join, so it is the same sequence of CUs the serial loop produced, for any
    // schedule. Diagnostics need no merge step at all here — unlike the back
    // half, a front-half job writes NOTHING into `rep`: it only reads
    // `rep.config()` for the diagnostic budget, and every diagnostic it
    // produces lands inside the `CompilationUnit` it is building
    // (`driverDiagnostics()` and each `Tree`'s own). `runCusToTargets` then
    // drains those by walking the same index order. So diagnostic ORDER and
    // COUNT are a pure function of CU index, and CU index is a pure function of
    // the `sources` span — neither can observe the schedule.
    //
    // ⚠ WHAT IS *NOT* PARALLELIZED HERE, AND WHY. DSS's own shipped runtime
    // units are not in this batch at all — `runCusToTargets` resolves them
    // per TARGET as cached static archives (`resolveShippedRuntimeArchives`),
    // and that path stays SERIAL: it is dominated by two `stat`s and a digest
    // per unit on the steady-state HIT, and its MISS arm runs a whole nested
    // `Program` build whose own per-CU pool would then be nested inside this
    // one. Refusing to parallelize it is not an oversight.
    auto buildCus = [&](CuBuildKey const&                   key,
                        std::span<PredefinedMacroDef const> targetPredefines,
                        std::span<PredefinedMacroDef const> formatPredefines,
                        // D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET: this key's
                        // resolved grammar (see the `compileFiles` twin).
                        std::shared_ptr<GrammarSchema const> const& keyGrammar,
                        // D-LK-OBJECT-INPUT-COMPILE-FLAG-SURFACE: see the
                        // `compileFiles` twin. ONE CU PER SOURCE — so the CU
                        // index is a pure function of THIS span, and every
                        // downstream index-parallel claim (the diagnostic
                        // drain's order, `cuMirs[i]` ↔ `cus[i]`) is stated
                        // against the list that actually produced the units.
                        std::span<std::string const> sources)
        -> std::vector<CompilationUnit> {
        std::size_t const n = sources.size();
        // Resolved ONCE for the whole batch rather than once per file: the
        // answer is fixed for this key, and it is a filesystem walk (see
        // `resolveSystemDirs`).
        std::vector<fs::path> const systemDirs = resolveSystemDirs(*keyGrammar);

        std::vector<std::optional<CompilationUnit>> slots(n);
        auto buildOne = [&](std::size_t i) {
            // D-PARSE-DEEP-FRONTEND-STACK: build each CU on the 64 MiB worker
            // stack (see compileFiles for the rationale) so a deeply-nested
            // expression parses without overflowing the host main stack. Under
            // the pool this nests a large-stack thread inside a worker — the
            // same nesting the back half already runs, and the reason
            // `large_stack_call.hpp` documents the primitive as concurrent.
            slots[i] = substrate::callOnLargeStack(
                substrate::kDeepRecursionStackBytes, [&] {
                    // Same budget hop as `compileFiles` above
                    // (D-DIAG-VOLUME-CAP-ENFORCED-AT-SIX-STAGES-NOT-ONCE).
                    // ⚠ `rep` is READ here and NEVER WRITTEN — see the batch
                    // note above; writing it from a job would reintroduce
                    // exactly the completion-order dependence this shape
                    // exists to prevent.
                    UnitBuilder builder{keyGrammar, DiagnosticBudget{rep.config()}};
                    for (auto const& d : systemDirs) builder.addSystemDir(d);
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
                    builder.addFile(fs::path{sources[i]});
                    return std::move(builder).finish();
                });
        };

        if (n <= 1) {
            // One TU has no batch to form; keep it off the pool entirely so the
            // single-file path spawns nothing it did not spawn before.
            if (n == 1) buildOne(0);
        } else {
            std::optional<substrate::ThreadPool> localPool;
            substrate::IExecutor* exec  = executor_;
            std::size_t const     width = resolveCuPoolWidth(n, jobs_);
            if (exec == nullptr) {
                localPool.emplace(width);
                exec = &*localPool;
            }
            std::vector<std::uint64_t> jobWallNs(n, 0);
            auto const                 batchStart = std::chrono::steady_clock::now();
            std::latch                 done{static_cast<std::ptrdiff_t>(n)};
            for (std::size_t i = 0; i < n; ++i) {
                exec->submit([&, i] {
                    struct CountDownGuard {
                        std::latch& latch;
                        ~CountDownGuard() { latch.count_down(); }
                    } const guard{done};
                    auto const jobStart = std::chrono::steady_clock::now();
                    buildOne(i);
                    jobWallNs[i] = static_cast<std::uint64_t>(
                        std::max<long long>(0, std::chrono::duration_cast<
                            std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - jobStart).count()));
                });
            }
            done.wait();
            recordJobBatch(JobBatchRecord{
                "front-half cu build",
                // An injected executor's width belongs to the injector; report
                // 0 rather than a width this driver computed but did not use.
                executor_ != nullptr ? std::size_t{0} : width,
                static_cast<std::uint64_t>(std::max<long long>(
                    0, std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - batchStart).count())),
                std::move(jobWallNs)});
        }

        // Collect IN INDEX ORDER. A disengaged slot means that job threw and
        // the pool swallowed the exception after logging it — the CU does not
        // exist, so it FAILS THE BUILD naming the file rather than returning a
        // short vector that would silently compile a program missing a
        // translation unit. Every other CU is still returned so the run
        // surfaces all of their diagnostics too; `runCusToTargets` stops on
        // `rep.hasErrors()` immediately after its drain.
        std::vector<CompilationUnit> cus;
        cus.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            if (!slots[i].has_value()) {
                emitDriver(rep, DiagnosticCode::D_CompileUnitNullNoDiagnostic,
                           "internal: the front-end build of translation unit '"
                           + std::string{sources[i]}
                           + "' produced no compilation unit — its build job "
                             "terminated abnormally (the executor logs the "
                             "throw). The build stops rather than linking a "
                             "program that is silently missing this unit.");
                continue;
            }
            cus.push_back(std::move(*slots[i]));
        }
        return cus;
    };

    std::string const sourceStem = fs::path{sourceFiles.front()}.stem().string();
    return runCusToTargets(
        buildCus, grammar, sourceFiles, sourceStem,
        targets, rep, outputDir_, artifactName_, perFormatOutputSubdir_,
        compileConfig_, ltoMode_,
        optimizerPipelineOverride_.has_value() ? &*optimizerPipelineOverride_ : nullptr,
        resolveLibraries_, resolveLibraryAdditionsByTarget_, artifactPaths_,
        executor_, jobs_,
        // D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: the CLI/manifest stack-reserve
        // request (nullopt = the format default stands).
        ImageRequest{stackReserveBytes_},
        // D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION (C):
        // the cross-build artifact cache policy — see the `compileFiles` twin.
        dependencyArtifactCache_);
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

    // ★★★ ROUTE THROUGH THE SHARED PREDICATE, EXACTLY AS `--compile` AND
    // `--project` DO. A directory scan is a way of GATHERING files; it is not a
    // different compilation model, and the mode must not decide the translation
    // -unit semantics.
    //
    // ⚠ THIS CALLED `compileFiles` UNCONDITIONALLY, WHICH IS THE UNITY-BUILD
    // PATH — the one the dispatcher's own comment calls "deliberately NOT a CLI
    // surface", on the stated grounds that no language's file model
    // concatenates translation units. So `--directory` reached a surface the
    // code says no CLI should reach, and every C file after the first was
    // dropped. ✔MEASURED 2026-08-25 on a 257-source directory: **exit 0, zero
    // diagnostics, an artifact written, and `nm` finds NONE of the 256
    // functions in it** — a wrong image reported as success, which is the one
    // failure mode this project refuses outright. With three files it instead
    // fails at link with `undefined symbol`, so the damage is loud or silent
    // depending only on whether the dropped code happened to be referenced.
    //
    // ★ AND THE COMMENT ON `routesToMultiUnit` NAMED THE CAUSE BEFORE THE BUG
    // EXISTED: it calls itself "the single source of truth for the threshold,
    // shared with `compileProject` so the two dispatch sites never drift."
    // There are THREE dispatch sites. The one that did not use it is the one
    // that drifted.
    return routesToMultiUnit(sourceFiles.size())
        ? compileUnits(sourceFiles, languageName, targets, reporterConfig)
        : compileFiles(sourceFiles, languageName, targets, reporterConfig);
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
