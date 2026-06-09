#include "program/program.hpp"

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_lattice.hpp"  // TypeLattice (fresh merge host)
#include "ffi/abi/abi_catalog.hpp"
#include "link/object_format_schema.hpp"
#include "mir/merge/mir_merge.hpp"  // MergeCuInput, mergeCuMirs (N>1 whole-program merge)
#include "lsp/lsp_server.hpp"
#include "lsp/schema_cache.hpp"
#include "lsp/thread_pool.hpp"
#include "lsp/transport.hpp"
#include "program/cli_args.hpp"
#include "program/compile_pipeline.hpp"
#include "program/cross_validate_target_format.hpp"
#include "program/input_resolver.hpp"
#include "program/project_config.hpp"
#include "program/target_spec.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <limits>
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
//
// eb2c6c7 audit-fold (2026-06-01): `d.contextPrefix` is rendered
// between the code-band and `d.actual` so multi-target runs route
// per-target context to the operator's terminal. Pre-fold the prefix
// was baked into `actual` so this print site saw it for free; post-
// fold the prefix lives in its own field (excluded from dedup hash)
// and every render path must spell out the inclusion. LSP
// `composeMessage` performs the symmetric prepend.
void drainDiagnosticsToStderr(DiagnosticReporter const& rep) {
    for (auto const& d : rep.all()) {
        std::cerr << severityName(d.severity)
                  << "[" << diagnosticCodeName(d.code) << "] "
                  << d.contextPrefix
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

// Compile one resolved (CU, target, format) triple to one artifact.
// Returns true on success; emits via `reporter` on failure.
//
// `outputDir` (D-LK10-ENTRY Slice C companion): when set, the
// emitted binary lands at `<outputDir>/<sourceStem><ext>` for
// single-target builds, or `<outputDir>/<formatName>/<sourceStem>
// <ext>` for multi-target builds (the multi-target qualifier
// disambiguates same-named outputs across formats). When unset,
// the legacy `<cwd>/target/<formatName>/<sourceStem><ext>`
// convention applies — keeps existing call sites unchanged.
[[nodiscard]] bool compileOneTarget(std::span<CompilationUnit const> cus,
                                    GrammarSchema const&   grammar,
                                    std::string const&     sourceStem,
                                    std::string const&     targetSpecStr,
                                    DiagnosticReporter&    reporter,
                                    std::optional<std::filesystem::path> const& outputDir,
                                    bool                   multiTargetBuild,
                                    CompileOptions const&  compileOpts) {
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
    // authoritative artifact-profile-driven scheme):
    //   default      : <cwd>/target/<formatName>/<sourceStem><ext>
    //   --output dir : <dir>/<sourceStem><ext>               (single target)
    //                  <dir>/<formatName>/<sourceStem><ext>  (multi target)
    // `formatName` already encodes machine+OS, so we don't add a
    // separate `<targetName>` subdir (redundant + bloats the path).
    auto const ext = parsed->outputExtension(**formatR);
    fs::path outDir;
    if (outputDir.has_value()) {
        outDir = multiTargetBuild
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
    auto const outPath = outDir / (std::string{sourceStem} + std::string{ext});

    // Cycle 24/25 build-then-lower sequence. LOOP 1: build EVERY CU's MIR up front
    // (`buildCuMir` — sem→HIR→FFI→MIR→optimize), holding each `CuMirModule` (which keeps
    // its SemanticModel — the interner owner — alive). Early-return on the FIRST CU's
    // build failure preserves the fail-fast contract (diagnostics already reported).
    std::vector<CuMirModule> cuMirs;
    cuMirs.reserve(cus.size());
    for (auto const& cu : cus) {
        auto cuMir = buildCuMir(cu, grammar, **targetR, **formatR,
                                ccIndex, reporter, compileOpts);
        if (!cuMir) return false;  // front-half tier failure already reported via `reporter`
        cuMirs.push_back(std::move(*cuMir));
    }

    // N==1 (the CU5 multi-file-single-CU case): lower the sole CU + link it. UNCHANGED
    // from cycle 24 — byte-identical single-CU output. Routing N==1 through the merge
    // would re-intern CU0's types into a fresh host (a no-op for correctness, but extra
    // work + a different code path); keep the proven single-CU lowering for byte-identity.
    if (cuMirs.size() == 1) {
        auto mod = lowerCuMirToAssembly(cuMirs[0], reporter);
        if (!mod) return false;  // back-half tier failure already reported via `reporter`
        return linkAndWrite(std::span<AssembledModule const>{&*mod, 1},
                            **targetR, **formatR, outPath, reporter);
    }

    // N>1 (CU6 multi-CU): WHOLE-PROGRAM MIR MERGE (Cycle 25 Stage C). Fold the N per-CU
    // modules into ONE module over a fresh host lattice, resolving cross-CU calls to
    // DIRECT intra-module calls (no cycle-19 assembled-tier thunk), then lower that single
    // module ONCE and link it (the linker takes its single-module path). The merge reads
    // each CU's `nameOf` (SemanticModel symbol names + extern mangledNames) while cloning,
    // so `cuMirs` must stay alive through `mergeCuMirs` — it does (function-local, no CU's
    // lattice is moved out: the host is FRESH, leaving every SemanticModel intact).
    std::vector<MergeCuInput> mergeInputs;
    mergeInputs.reserve(cuMirs.size());
    for (auto& cuMir : cuMirs) {
        MergeCuInput in;
        in.mir      = &cuMir.mir;
        in.interner = &cuMir.model.lattice().interner();
        // nameOf: symbol id → declared name. Covers DEFINITIONS (SemanticModel record)
        // AND extern IMPORTS (the import's mangledName, when the symbol has no record —
        // an extern reference's SymbolId is not in the semantic symbol table). The merge
        // keys cross-CU matching on this name exactly as the linker keys on the on-binary
        // symbol name. Capturing `&cuMir` is safe — `cuMirs` is done growing.
        in.nameOf = [cuMirP = &cuMir](SymbolId s) -> std::string {
            if (SymbolRecord const* r = cuMirP->model.recordFor(s)) return r->name;
            for (auto const& e : cuMirP->externImports) {
                if (e.symbol.v == s.v) return e.mangledName;
            }
            return std::string{};
        };
        in.externImports = cuMir.externImports;
        mergeInputs.push_back(std::move(in));
    }

    // Fresh host TypeLattice for the merged module: seeded with CU0's id + source
    // language (cosmetic — the registry's sourceLanguage tags extension types; c-subset
    // has none). The merge re-interns ALL CUs (incl CU0) into this fresh host, so no
    // SemanticModel's lattice is mutated or moved — still "re-intern at merge", just a
    // fresh host rather than CU0's in-place. Agnostic: id + language string, no branch.
    TypeLattice host{cuMirs[0].cuId,
                     std::string{cuMirs[0].model.lattice().registry().sourceLanguage()}};

    // entryNames: the grammar's entry-function name list (the same list the single-CU
    // user-entry scan reads). The merge uses it to compute the merged `userEntrySymbol`.
    std::vector<std::string> entryNames;
    for (auto const& decl : grammar.semantics().declarations) {
        for (auto const& n : decl.implicitReturnZeroForFunctionNames) {
            entryNames.push_back(n);
        }
    }

    auto merged = mergeCuMirs(
        std::span<MergeCuInput const>{mergeInputs.data(), mergeInputs.size()},
        std::move(host),
        std::span<std::string const>{entryNames.data(), entryNames.size()},
        reporter);
    if (!merged) return false;  // merge failure (conflict / verify) already reported.

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

    // D-FFI-EXTERN-CALL-DISPATCH: the merged module compiles to ONE
    // (target, format); pass that format's extern-call shape so MIR→LIR
    // selects the right call-site opcode for any surviving extern import.
    auto mod = lowerMergedToAssembly(*merged, grammar, **targetR,
                                     ccIndex, cuMirs[0].cuId,
                                     (*formatR)->externCallDispatch(),
                                     reporter);
    if (!mod) return false;  // back-half tier failure already reported via `reporter`
    return linkAndWrite(std::span<AssembledModule const>{&*mod, 1},
                        **targetR, **formatR, outPath, reporter);
}

// Drain N CUs' parse / driver-tier diagnostics into the run-wide reporter, then compile
// them to each target — the linker MERGES the N CUs into ONE image per target (LK11).
// Shared by `compileFiles` (one CU5 multi-file CU → 1-element span) and `compileUnits`
// (N single-file CUs). The only difference between those two entries is how the CUs are
// constructed; everything downstream (drain, per-target scratch-reporter merge, link) is
// identical and lives here. Returns 0 on success, 1 on any error.
int runCusToTargets(std::span<CompilationUnit const>            cus,
                    GrammarSchema const&                        grammar,
                    std::string const&                          sourceStem,
                    std::vector<std::string> const&             targets,
                    DiagnosticReporter&                         rep,
                    std::optional<std::filesystem::path> const& outputDir,
                    CompileConfig                               config,
                    ::dss::opt::OptPipeline const*              pipelineOverride) {
    // Drain each CU's driver-tier + per-Tree diagnostics (D_FileNotFound, parser/lexer
    // errors) into the run-wide reporter. Without this drain, a missing source file
    // produces rc=1 with ZERO stderr — the substrate silent-failure archetype.
    for (auto const& cu : cus) {
        copyDiagnostics(cu.driverDiagnostics(), rep);
        for (auto const& tree : cu.trees()) {
            copyDiagnostics(tree.diagnostics(), rep);
        }
    }
    // If parsing already failed, the per-target loop would only produce derivative noise.
    if (rep.hasErrors()) {
        drainDiagnosticsToStderr(rep);
        return 1;
    }

    int exitCode = 0;
    for (auto const& spec : targets) {
        // Per-target scratch reporter inheriting `rep`'s POLICY axes (suppress / overrides
        // / warningsAsErrors) but with the CAP/DEDUP axes RELAXED — those run-wide limits
        // are enforced once at `rep` during merge (silent-failure-hunter F9 / H1 fix; see
        // D-MERGE-POLICY-IDEMPOTENCY / D-MERGE-SCRATCH-FRESH / D-COMPILE-ONE-TARGET-NO-LEAK).
        auto scratchCfg = rep.config();
        scratchCfg.maxDiagnostics = std::numeric_limits<std::size_t>::max();
        scratchCfg.maxPerCode     = std::numeric_limits<std::size_t>::max();
        scratchCfg.dedupWindow    = 0;
        DiagnosticReporter scratch{scratchCfg};
        CompileOptions compileOpts;
        compileOpts.config           = config;
        compileOpts.pipelineOverride = pipelineOverride;
        bool const ok = compileOneTarget(
            cus, grammar, sourceStem, spec, scratch,
            outputDir, /*multiTargetBuild*/ targets.size() > 1u, compileOpts);
        mergeWithTargetContext(scratch, spec, rep);
        if (!ok || scratch.hasErrors()) exitCode = 1;
    }

    drainDiagnosticsToStderr(rep);
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
    // D-OPT1-PIPELINE-CONFIG-FROM-COMPILECONFIG: thread the CLI's
    // `--config=<debug|release>` into the kernel so the right
    // shipped pipeline gets loaded at compile_pipeline step 3.5.
    setCompileConfig(args.config);
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
    DiagnosticReporter rep{reporterConfig};
    return compileProject(projectFilePath, rep);
}

int Program::compileProject(
    const std::string& projectFilePath,
    DiagnosticReporter&             rep
) {
    // Plan 06 AP2: load the `.dss-project.json` project config, enforce
    // the requested `artifactProfile` against the language's declared
    // set (AP1's `GrammarSchema::artifactProfiles()`), then delegate to
    // the existing compile path. AP2 = validate + delegate; threading
    // the resolved profile to codegen / a CompilationContext is AP3/AP4
    // (D-AP2-COMPILATION-CONTEXT).
    auto pcOpt = loadProjectConfig(fs::path{projectFilePath}, rep);
    if (!pcOpt.has_value()) {
        // loadProjectConfig already emitted the structural diagnostic
        // (D_FileNotFound / C_MalformedJson / C_MissingField).
        drainDiagnosticsToStderr(rep);
        return 1;
    }
    ProjectConfig const& pc = *pcOpt;

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
                   + "' could not be loaded — check that "
                     "src/dss-config/sources/" + pc.language
                   + ".lang.json exists and parses cleanly.");
        drainDiagnosticsToStderr(rep);
        return 1;
    }
    auto grammar = *grammarR;

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

    // Route by source COUNT via the shared `routesToMultiUnit` threshold
    // (identical to the CLI dispatcher): >1 source ⇒ N independent CUs
    // the linker merges (`compileUnits`, `cc a.c b.c` semantics); ≤1 ⇒
    // the single-CU path (`compileFiles`). The delegate validates each
    // `<targetName>:<formatName>` spec (D_InvalidTargetSpec) and drains
    // `rep` at its end (runCusToTargets), so we do NOT drain here.
    return routesToMultiUnit(pc.sources.size())
        ? compileUnits(pc.sources, pc.language, pc.targets, rep)
        : compileFiles(pc.sources, pc.language, pc.targets, rep);
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

// FF11: declare the language's SYSTEM include dirs (its
// `semantics.shippedLibDirs`, the /usr/include analogue) on `builder`
// so the angle form `#include <h>` resolves against them. Each config
// string is a subdirectory under `src/dss-config/`; resolve it to an
// absolute dir by walking up from cwd (mirroring `findShippedConfig`'s
// 8-level walk, so it works from repo root, build/, or a nested ctest
// cwd). A dir not found in the ancestry is skipped — a header miss then
// hard-fails downstream with F_ShippedHeaderNotFound, which is the
// correct fail-loud surface (vs. silently swallowing here). NO language
// branch: the dirs come entirely from the schema's per-language config.
void applySystemDirs(UnitBuilder& builder, GrammarSchema const& grammar) {
    auto const& dirs = grammar.semantics().shippedLibDirs;
    if (dirs.empty()) return;
    std::error_code ec;
    for (std::string const& sub : dirs) {
        fs::path here = fs::current_path(ec);
        for (int i = 0; i < 8 && !here.empty(); ++i) {
            fs::path const candidate = here / "src" / "dss-config" / sub;
            if (fs::is_directory(candidate, ec)) {
                builder.addSystemDir(candidate);
                break;
            }
            fs::path const parent = here.parent_path();
            if (parent == here) break;   // hit filesystem root
            here = parent;
        }
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
    DiagnosticReporter rep{reporterConfig};
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

    // Build ONE CompilationUnit for ALL source files — the CU5 multi-file-single-CU shape
    // (cross-file references resolved WITHIN the CU; each file routes to the language's
    // schema by extension). For one CU per file (a multi-CU image the linker MERGES, LK11)
    // use `compileUnits` — this entry's single-CU semantics are unchanged.
    UnitBuilder builder{grammar};
    applySystemDirs(builder, *grammar);
    for (auto const& path : sourceFiles) {
        builder.addFile(fs::path{path});
    }
    auto cu = std::move(builder).finish();

    // Stem of the first source file names the artifact (one artifact per target). Plan 06
    // will eventually let artifact profiles override this.
    std::string const sourceStem = fs::path{sourceFiles.front()}.stem().string();
    return runCusToTargets(
        std::span<CompilationUnit const>{&cu, 1}, *grammar, sourceStem, targets, rep,
        outputDir_, compileConfig_,
        optimizerPipelineOverride_.has_value() ? &*optimizerPipelineOverride_ : nullptr);
}

int Program::compileUnits(
    const std::vector<std::string>& sourceFiles,
    const std::string& languageName,
    const std::vector<std::string>& targets,
    DiagnosticReporter::Config const& reporterConfig
) {
    DiagnosticReporter rep{reporterConfig};
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

    // Build ONE CompilationUnit PER source file — the multi-CU model the linker MERGES
    // into one image (CU6 + LK11). Distinct from `compileFiles` (one CU5 multi-file CU);
    // here cross-file references are resolved at LINK time (a sibling CU's definition or a
    // library import), not within a single CU.
    std::vector<CompilationUnit> cus;
    cus.reserve(sourceFiles.size());
    for (auto const& path : sourceFiles) {
        UnitBuilder builder{grammar};
        applySystemDirs(builder, *grammar);
        builder.addFile(fs::path{path});
        cus.push_back(std::move(builder).finish());
    }

    std::string const sourceStem = fs::path{sourceFiles.front()}.stem().string();
    return runCusToTargets(
        std::span<CompilationUnit const>{cus.data(), cus.size()}, *grammar, sourceStem,
        targets, rep, outputDir_, compileConfig_,
        optimizerPipelineOverride_.has_value() ? &*optimizerPipelineOverride_ : nullptr);
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
