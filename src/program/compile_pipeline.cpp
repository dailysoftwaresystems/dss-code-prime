#include "program/compile_pipeline.hpp"

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/substrate/path_identity.hpp"  // genericSpelling
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "asm/asm.hpp"
// The ONE call-frame join, shared by the calling-convention producer and the
// assembly-text producer (D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED).
#include "asm/asm_cfi.hpp"
#include "asm/asm_text_to_lir.hpp"
#include "core/substrate/large_stack_call.hpp"  // D-PARSE-DEEP-FRONTEND-STACK: BUILD half on a large stack
#include "core/substrate/mint_monotonic_id.hpp"  // c165: fresh per-member CompilationUnitId (static pull)
#include "core/types/config_path_walk.hpp"  // findShippedConfigDir -- the ONE shipped-config discovery precedence
#include "core/substrate/phase_timers.hpp"      // c97: per-phase --time accumulation
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_lattice.hpp"  // encode-tier extern binder's scratch lattice
#include "ffi/abi/abi_catalog.hpp"  // resolveAbi (encode-tier va_list shape, per active ABI)
#include "ffi/binary_reader.hpp"  // readImports (encode-tier --resolve-library binder)
#include "ffi/binary_readers/ar_reader.hpp"  // c165: readArArchive (static-pull member index)
#include "ffi/ingest.hpp"
#include "ffi/mangling/c_mangle.hpp"  // D-LK-OBJECT-EXTERN-SYMBOL-NAMES: applyCMangling
#include "ffi/shipped_lib_descriptor.hpp"  // c162: collectShippedExternSymbolNames
#include "core/types/symbol_attrs.hpp"  // isExternallyVisible (armap export filter, c163)
#include "hir/attributes/ffi_metadata.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "link/format/ar.hpp"  // writeArArchive (D-LK-STATIC-ARCHIVE-WRITER, c163)
#include "link/format/coff_object_reader.hpp"  // c170: COFF .obj member reader (static-pull dispatch)
#include "link/format/elf_object_reader.hpp"  // c165: readRelocatableObject (static-pull member parse)
#include "link/format/macho_object_reader.hpp"  // c168: Mach-O MH_OBJECT member reader (static-pull dispatch)
#include "core/substrate/thread_pool.hpp"      // D-OPT11-LAZY-IMPORT-EDGE: the thin stage's pool
#include "link/linker.hpp"
#include "link/writer.hpp"
#include "mir/summary/lazy_import_optimize.hpp"  // D-OPT11-LAZY-IMPORT-EDGE
#include "mir/summary/mir_summary.hpp"
#include "mir/summary/summary_index.hpp"
#include "lir/lir_2addr_legalize.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_peephole.hpp"
#include "lir/lir_regalloc.hpp"
#include "lir/lir_rewrite.hpp"
#include "lir/lir_verifier.hpp"           // LirVerifier — the LIR tier's own invariant checks
#include "lir/lir_wide_call_args.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/lowering/hir_to_mir.hpp"
#include "mir/merge/mir_merge.hpp"  // MergedMirModule (lowerMergedToAssembly consumes it)
#include "mir/mir_verifier.hpp"           // MirVerifier (UCRT-P4: verify the POST-SYNTHESIS module)
#include "mir/merge/synth_pe_startup.hpp"  // realizeEntryShape (UCRT-P4: the argv spine)
#include "mir/merge/synth_stdio_shim.hpp"  // synthesizeStdioShim (D-FFI-PE-CRT-UCRT-MIGRATION Phase 3)
#include "mir/merge/synth_threads_shim.hpp"  // synthesizeThreadsShim (FC17.9a D-CSUBSET-C11-THREADS-HEADER)
#include "opt/optimizer.hpp"
// D-LK-ARCHIVE-MEMBER-READ-USES-THE-IMAGE-FORMAT-NOT-THE-OBJECT-FORMAT:
// `resolveArchiveSiblingFormat` -- the declared-property lookup that answers
// "which object format wrote this archive's members", reused rather than respelt.
#include "program/runtime_object_cache.hpp"
#include "opt/passes/prune_unreachable.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <latch>   // D-OPT11-LAZY-IMPORT-EDGE: the thin stage batch join
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Plan 14 LK10 cycle 2 — driver pipeline kernel.

namespace dss {

void copyDiagnostics(DiagnosticReporter const& src,
                     DiagnosticReporter&       dst) {
    // The retained source buffers move WITH the diagnostics — same rule, same
    // reason, as `mergeWithTargetContext`: a diagnostic naming a buffer only
    // `src` holds renders `--> <unknown-buffer:N>` at `dst`. Any tier that
    // parses text the compilation units do not own (an embedded assembly
    // template today) registers its fragment buffer on the reporter it was
    // handed, so every reporter→reporter copy owes the pairing.
    dst.sourceBuffers().addAll(src.sourceBuffers());
    for (auto const& d : src.all()) dst.report(d);
}

BitFieldStrategy
effectiveBitFieldStrategy(TargetSchema const&       target,
                          ObjectFormatSchema const& format) noexcept {
    // FORMAT wins (the strategy is OS/format-determined); fall back to the
    // target's declared value when the format declared none. Selects on the
    // config-declared enum only — no target/format identity branch.
    if (format.bitFieldStrategy() != BitFieldStrategy::None) {
        return format.bitFieldStrategy();
    }
    return target.aggregateLayout().bitFieldStrategy;
}

LongDoubleFormat
effectiveLongDoubleFormat([[maybe_unused]] TargetSchema const& target,
                          ObjectFormatSchema const&            format) noexcept {
    // FC17.9(e) (D-CSUBSET-LONG-DOUBLE): FORMAT-only — no target-side field
    // exists to fall back to (see the header docblock). `None` propagates as
    // the honest undeclared state; the semantic bind fails loud on it.
    return format.longDoubleFormat();
}

UnnamedBitFieldAlignment
effectiveUnnamedBitFieldAlignment([[maybe_unused]] TargetSchema const& target,
                                  ObjectFormatSchema const&            format) noexcept {
    // D-CSUBSET-ZERO-WIDTH-BITFIELD-ALIGNMENT: FORMAT-only — no target-side field
    // exists to fall back to (see the header docblock). `None` propagates as the
    // honest undeclared state; the layout engine fails loud on it, and only when an
    // unnamed bit-field actually needs the rule.
    return format.unnamedBitFieldAlignment();
}

// ── NOT HERE: `effectiveCharIsUnsigned` (D-TARGET-CHAR-SIGNEDNESS-PER-PLATFORM)
// ──────────────────────────────────────────────────────────────
// There is deliberately no third member of the `effective*` family for
// bare-`char` signedness. This family exists because those axes have
// contributions from BOTH schemas that must be RECONCILED — a genuine
// two-sided negotiation. Char signedness has exactly ONE contributor: the
// target declares the whole (processor × platform) fact in its single
// `charIsUnsigned` key. An `effectiveCharIsUnsigned(target, format)` wrapper
// would advertise a negotiation that no longer happens, and would be a second
// place the fact is "about" — the exact duplication this reshape removed.
// The call site asks the owner directly: `target.charIsUnsigned(format.kind())`.

namespace {

// Snapshot-vs-current `errorCount` gate. Each tier shares `reporter`,
// so we cannot read `errorCount() == 0` as a tier-pass signal —
// upstream errors stay accumulated. Instead, every tier checkpoints
// against the count it saw at entry. Mirrors the linker's
// `errorsAtEntry` snapshot discipline in `linker.cpp::link`.
[[nodiscard]] bool tierClean(DiagnosticReporter const& reporter,
                              std::size_t entryCount) noexcept {
    return reporter.errorCount() == entryCount;
}

} // namespace

// MIR optimizer driver (Cycle 26 extraction). Resolves the pipeline — explicit
// `opts.pipelineOverride` (examples_runner differential-verify arm + unit tests) else
// the shipped JSON named by `resolvePipelineName(opts.config)` — then runs
// `opt::optimize` over `mir` in place, returning `ok && tierClean`. Fails loud (false)
// on an out-of-range CompileConfig ordinal or a pipeline load failure. The verifier
// runs after every pass (D-OPT1-VERIFY-AFTER-EVERY-PASS), so this is the safety net
// for the merged module too.
//
// `buildCuMir` calls this with the per-CU lattice's interner; the N>1 merged path
// (`Program::compileOneTarget`) calls it with the merged host lattice's interner so
// cross-CU calls — made intra-module DIRECT by the cycle-25 merge — get inlined
// (D-OPT7-1). Agnostic: no language/target/format branch — the pipeline is
// config-driven and `optimize` is target-blind at the MIR tier.
bool optimizeModule(Mir&                  mir,
                    TargetSchema const&   target,
                    TypeInterner const&   interner,
                    CompileOptions const& opts,
                    PipelineStage         stage,
                    DiagnosticReporter&   reporter,
                    // D-CSUBSET-INLINE-FUNCTION-NO-EXTERNAL-DEFINITION-EMITTED:
                    // the module's extern table, read by `opt::optimize`'s
                    // unconditional strip epilogue to identify the C99 6.7.4p7
                    // inline definitions it must remove before codegen. Passed by
                    // BOTH call sites (per-CU here, whole-program in program.cpp)
                    // because a body that survives either one is emitted.
                    std::span<ExternImport const> externImports) {
    // c97: one optimize phase covering pipeline resolution + every pass +
    // the mandatory prune-normalize — both the per-CU and merged call sites.
    substrate::PhaseTimers::Scope optimizePhase{
        substrate::CompilePhase::Optimize};
    auto const optEntry = reporter.errorCount();
    // MANDATORY post-lowering normalize: drop verifier-rejected unreachable
    // continuation blocks the frontend creates eagerly (D-MIR-UNREACHABLE-PRUNE-NORMALIZE).
    // Runs before opt::optimize's verify-after-every-pass, on EVERY CU and the merged
    // module — the universal chokepoint. NOT a PassId (a pipeline config must not omit it).
    if (!::dss::opt::passes::runPruneUnreachableBlocks(mir, interner, reporter).ok
        || !tierClean(reporter, optEntry)) {
        return false;
    }
    ::dss::opt::OptPipeline loadedPipeline;
    ::dss::opt::OptPipeline const* effectivePipeline = opts.pipelineOverride;
    if (effectivePipeline == nullptr) {
        auto const name = resolvePipelineName(opts.config);
        if (!name.has_value()) {
            // Out-of-range CompileConfig ordinal — fail loud rather
            // than silently degrade to "debug" (which would let a
            // buggy CLI parser silently demote a release build).
            ParseDiagnostic d;
            d.code     = DiagnosticCode::X_PipelineNameResolutionFailed;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "compile_pipeline: CompileConfig ordinal {} out of range "
                "(kCompileConfigCount = {}) — substrate-shape violation "
                "(D-OPT1-PIPELINE-CONFIG-FROM-COMPILECONFIG).",
                static_cast<int>(opts.config), kCompileConfigCount);
            reporter.report(std::move(d));
            return false;
        }
        auto loaded = ::dss::opt::loadShippedPipeline(*name);
        if (!loaded.has_value()) {
            // The pipeline file ships with the repo; a load failure
            // here is a deploy/install bug. Drain config diagnostics
            // so the user sees the JSON-path context.
            forwardConfigDiagnostics(loaded.error(), reporter);
            return false;
        }
        loadedPipeline = std::move(loaded).value();
        // P10 stage routing: at the UNIT stage, a document that declares a
        // top-level "unitPipeline" name runs THAT pipeline instead (the
        // per-TU schedule of a two-stage LTO topology). The Program stage
        // always runs the config's own document — the link-time schedule.
        // An override (above) already bypassed this whole block, so an
        // explicit pipelineOverride runs at every site unchanged.
        if (stage == PipelineStage::Unit
            && !loadedPipeline.unitPipelineName.empty()) {
            auto unitLoaded =
                ::dss::opt::loadShippedPipeline(loadedPipeline.unitPipelineName);
            if (!unitLoaded.has_value()) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::X_PipelineNameResolutionFailed;
                d.severity = DiagnosticSeverity::Error;
                d.actual   = std::format(
                    "compile_pipeline: pipeline '{}' declares unitPipeline "
                    "'{}' which failed to load — a two-stage topology must "
                    "name a shipped pipeline document (D-OPT7-CROSSCU-LTO-"
                    "SINGLE-OPTIMIZE).",
                    *name, loadedPipeline.unitPipelineName);
                reporter.report(std::move(d));
                forwardConfigDiagnostics(unitLoaded.error(), reporter);
                return false;
            }
            // The unit stage is ONE hop deep. A resolved unit document that
            // itself declares `unitPipeline` would have the key silently
            // ignored (nothing re-inspects it below) — refuse it loud: the
            // topology is two stages, and a chain is a config error, not a
            // third stage.
            if (!unitLoaded->unitPipelineName.empty()) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::X_PipelineNameResolutionFailed;
                d.severity = DiagnosticSeverity::Error;
                d.actual   = std::format(
                    "compile_pipeline: unit pipeline '{}' itself declares "
                    "unitPipeline '{}' — the unit stage resolves exactly ONE "
                    "hop; chaining is refused rather than silently ignored.",
                    loadedPipeline.unitPipelineName,
                    unitLoaded->unitPipelineName);
                reporter.report(std::move(d));
                return false;
            }
            loadedPipeline = std::move(unitLoaded).value();
        }
        effectivePipeline = &loadedPipeline;
        // Stage attribution under the SAME env gate as the per-pass trace
        // (DSS_OPT_TRACE) — the A/B instrument keys on these lines.
        if (std::getenv("DSS_OPT_TRACE") != nullptr) {
            std::fprintf(stderr, "opt: stage=%s pipeline=%s\n",
                         stage == PipelineStage::Unit ? "unit" : "program",
                         loadedPipeline.name.c_str());
            std::fflush(stderr);
        }
    }
    auto const optResult = ::dss::opt::optimize(
        mir, target, interner, *effectivePipeline, reporter, externImports);
    return optResult.ok && tierClean(reporter, optEntry);
}

// BUILD half (Cycle 24): semantic analysis → HIR → FFI synthesis → MIR → optimize for
// ONE CompilationUnit, returning the `CuMirModule` the LOWER half consumes. The
// `SemanticModel` is MOVED into the result so its `TypeLattice` interner stays alive
// past this call — `lowerCuMirToAssembly` re-opens it for MIR→LIR + the symbol-table
// populate. Returns nullopt on any front-half tier failure (diagnostics via `reporter`).
// Forward decl of the deep-frontend BUILD body; the public `buildCuMir`
// below runs it on a large worker stack.
static std::optional<CuMirModule> buildCuMirImpl(
    CompilationUnit const& cu, GrammarSchema const& grammar,
    TargetSchema const& target, ObjectFormatSchema const& format,
    std::uint16_t callingConventionIndex, DiagnosticReporter& reporter,
    CompileOptions const& opts);

// D-PARSE-DEEP-FRONTEND-STACK: the per-CU BUILD half runs the frontend stages
// that traverse the expression tree — semantic `analyze`, CST→HIR
// (`lowerToHir`), and HIR→MIR (`lowerToMir`). plan-24 flattened these onto
// explicit work-stacks (O(1) host-stack per level), so their OWN recursion no
// longer drives stack depth; the worker is RETAINED (BC-1) because the parser's
// residual paren/postfix arm can still build a deep tree (bounded by the
// config-driven cap, c = 1024) and as defense-in-depth for any not-yet-
// proven-flat recursion these stages reach. HIR/MIR run inline on the caller's
// thread AFTER `analyze`'s own worker has joined, so the WHOLE BUILD half runs
// on a 64 MiB worker stack (synchronous join — no concurrency). NOTE: `analyze`
// ALSO self-wraps (it has direct callers, e.g. the diagnostic-corpus test);
// reached through here it is a benign NESTED worker — only one stack is ever
// live-deep at a time. The LOWER half (MIR→LIR→codegen,
// `lowerMirModuleToAssembly`) iterates a flat SSA arena, not a tree, so it
// needs no wrap.
std::optional<CuMirModule> buildCuMir(CompilationUnit const&        cu,
                                      GrammarSchema const&          grammar,
                                      TargetSchema const&           target,
                                      ObjectFormatSchema const&     format,
                                      std::uint16_t                 callingConventionIndex,
                                      DiagnosticReporter&           reporter,
                                      CompileOptions const&         opts) {
    return substrate::callOnLargeStack(
        substrate::kDeepRecursionStackBytes, [&] {
            return buildCuMirImpl(cu, grammar, target, format,
                                  callingConventionIndex, reporter, opts);
        });
}

static std::optional<CuMirModule> buildCuMirImpl(
                                      CompilationUnit const&        cu,
                                      GrammarSchema const&          grammar,
                                      TargetSchema const&           target,
                                      ObjectFormatSchema const&     format,
                                      std::uint16_t                 callingConventionIndex,
                                      DiagnosticReporter&           reporter,
                                      CompileOptions const&         opts) {
    // Take a CU pointer matching `analyze()`'s shared_ptr signature.
    // The CU is borrowed (caller owns); we re-wrap as a shared_ptr
    // with a null deleter so `analyze`'s ref-counting contract is
    // satisfied without taking ownership of the caller's CU.
    // `analyze` only reads from the CU; the temporary shared_ptr
    // owns nothing beyond the call.
    auto borrowed = std::shared_ptr<CompilationUnit const>(
        &cu, [](CompilationUnit const*) noexcept {});

    // 1. Semantic analysis. `analyze` accumulates into the model's
    //    OWN reporter; drain into the caller's so operator-visible
    //    stderr sees the S_* family. Without this drain, a semantic
    //    error (e.g. S_UndeclaredIdentifier) silently aborts the
    //    pipeline with no diagnostic surfacing. (code-reviewer F1
    //    fold + post-fold-1 architect: routed through the hoisted
    //    `copyDiagnostics` helper to eliminate the inline-drain
    //    duplicate.)
    auto const semEntry = reporter.errorCount();
    // FC3 c1: thread the FORMAT's declared data model (its REQUIRED
    // `dataModel` field) into the per-(CU × target) analysis — the
    // single source for every width-dependent resolution downstream
    // (builtinTypes/typeSpecifiers `coreByDataModel`, the integer-
    // literal ladder, descriptor `signatureByDataModel`). The HIR
    // lowering reads the SAME value back off the SemanticModel.
    // FC6 deferral-close: also thread the target's aggregate-layout params so a
    // `sizeof` in an array-dimension const-expression (`int a[sizeof(T)]`) folds
    // through the same `computeLayout` engine MIR uses — `nullopt` when the
    // target declared no block (the fold then fails loud, never a wrong size).
    // D-CSUBSET-BITFIELD-ABI-EXACT: overlay the FORMAT-resolved bit-field strategy
    // onto the target's params (the strategy is OS/format-determined; the target
    // supplies only the alignment rule). A `sizeof` over a bit-field struct in an
    // array dimension then folds with the byte-ABI-exact layout.
    auto const effectiveBfStrategy = effectiveBitFieldStrategy(target, format);
    // D-CSUBSET-ZERO-WIDTH-BITFIELD-ALIGNMENT: resolved beside the strategy and
    // overlaid at the SAME three consumer sites, because the two axes are read by one
    // packer and a site that got one without the other would lay out to a mixture of
    // two ABIs.
    auto const effectiveUnnamedBfAlign =
        effectiveUnnamedBitFieldAlignment(target, format);
    std::optional<AggregateLayoutParams> analyzeLayout;
    if (target.aggregateLayoutLoaded()) {
        analyzeLayout = target.aggregateLayout();
        analyzeLayout->bitFieldStrategy = effectiveBfStrategy;
        analyzeLayout->unnamedBitFieldAlignment = effectiveUnnamedBfAlign;
    }
    // FC12b (D-FC12B-WIN64-VARIADIC-CALLEE, BLOCKER-2): capture the RESOLVED CC's
    // WHOLE `vaListLayout` block. Read from the SAME resolved CC the MirLoweringConfig
    // reads its `vaListLayout` from (below); `nullopt` when the CC declares no
    // variadic-callee ABI.
    //
    // TWO consumers, each taking the part it needs from this ONE lookup:
    //   * the semantic `va_list`-type injection wants only `.strategy`, to size the `ap`
    //     local per ABI (SysV __va_list_tag[1]=24B vs Win64 char*=8B). `nullopt` there ⇒
    //     the SysV-family default, which is inert (a CC with no vaListLayout has no
    //     variadic-callee surface at all).
    //   * D-FFI-PE-CRT-UCRT-MIGRATION (Phase 3): `synthesizeStdioShim`, across the MIR/LIR
    //     seam, needs the WHOLE block — `.variadicUsesOverflowBase` is what selects its
    //     va leaf, and reading only `.strategy` there was a latent silent miscompile (see
    //     `CuMirModule::vaListLayout`). Same resolved CC, resolved ONCE.
    std::optional<VaListLayout> analyzeVaLayout;
    if (auto const* cc = target.callingConvention(callingConventionIndex);
        cc != nullptr && cc->vaListLayout.has_value()) {
        analyzeVaLayout = *cc->vaListLayout;
    }
    std::optional<VaListStrategy> const analyzeVaStrategy =
        analyzeVaLayout.has_value() ? std::optional<VaListStrategy>{analyzeVaLayout->strategy}
                                    : std::nullopt;
    // c97: sequential per-phase scoping via optional emplace — emplace
    // destroys the prior Scope (closing its accumulation window) BEFORE
    // opening the next, and any early return closes the live one.
    std::optional<substrate::PhaseTimers::Scope> phase;
    phase.emplace(substrate::CompilePhase::Semantic);
    auto model = analyze(
        // D-DIAG-VOLUME-CAP-ENFORCED-AT-SIX-STAGES-NOT-ONCE: the operator's
        // budget, carried on `opts` from `rep` -- NOT `reporter.config()`, which
        // is the relaxed per-target scratch.
        std::move(borrowed), opts.diagBudget,
        format.dataModel(), analyzeLayout, analyzeVaStrategy,
        format.kind(),       // c8: the active object-format → per-target availability gate
        target.name(),       // plan 25: the active arch → per-target shipped-struct variant selector
        // FC17.9(e) (D-CSUBSET-LONG-DOUBLE): the format-resolved `long double`
        // axis — drives the coreByLongDoubleFormat row overrides; None (wasm/
        // spirv) leaves `long double` rows unrealized (loud on use).
        effectiveLongDoubleFormat(target, format),
        // ★ Inline-asm P5 (D-CSUBSET-INLINE-ASM-OPERANDS): THE ACTIVE TARGET.
        // Without it `analyze` runs with `target == nullptr`, and its two
        // target-dependent asm checks — `S_InlineAsmConstraintLetterUndeclared`
        // (0xE065) and `S_InlineAsmClobberUnknown` (0xE068) — correctly decline
        // to guess and DO NOT RUN. ✔MEASURED before this argument existed: a
        // `"=Zq"` constraint and a `"notaregister"` clobber BOTH compiled to a
        // clean `.o` at rc=0 through this very pipeline. A diagnostic that fires
        // only in a unit test that passes its own schema is not a shipped
        // diagnostic. `target` is the driver's own long-lived schema and
        // outlives `model`, which is the lifetime the parameter requires.
        &target);
    phase.reset();
    copyDiagnostics(model.diagnostics(), reporter);
    if (model.hasErrors() || !tierClean(reporter, semEntry)) {
        return std::nullopt;
    }

    // 2. CST → HIR.
    auto const hirEntry = reporter.errorCount();
    phase.emplace(substrate::CompilePhase::LowerHir);
    auto hir = lowerToHir(model, reporter);
    phase.reset();
    if (!hir || !hir->ok || !tierClean(reporter, hirEntry)) {
        return std::nullopt;
    }

    // 2.5-pre. c162 (D-FF1-READER-CONSUMER) EAGER path validation: a
    //      `--resolve-library <path>` names a binary the build is pointed
    //      at, so a MISSING / UNREADABLE path is a hard error the operator
    //      must see -- UNCONDITIONALLY, even when this TU has no externs (or
    //      only explicitly-bound ones) and nothing routes to `ingest()`
    //      below. Without this eager open-probe the bad path would be
    //      SILENTLY IGNORED (the reader is reached only through `ingest()`,
    //      which the partition may skip). Fail loud `F_FileOpenFailed`,
    //      honoring the documented contract ("opened + read at compile time,
    //      fails loud on a missing/unreadable file"). A readable file's
    //      STRUCTURAL validity (sections, offsets, symbol tables) is still
    //      checked later by the reader when a governed extern actually routes
    //      to it -- that needs the whole file.
    //
    //      ★ THE PROBE ALSO ANSWERS "IS THIS THE RIGHT FORMAT AT ALL", on the
    //      SAME unconditional argument
    //      (D-FFI-RESOLVE-LIBRARY-WRONG-FORMAT-GUARD-IS-INCIDENTAL).
    //      A library whose object format is not this
    //      target's can never bind correctly, the fact is knowable from the
    //      first 8 bytes without reading it, and the alternative is a check
    //      that only fires when this particular TU happens to reference a
    //      binary-governed extern -- i.e. the same luck-of-the-TU silence the
    //      open-probe exists to remove. The COMPARISON is not restated here:
    //      `ffi::checkLibraryMatchesTargetFormat` is the one author of it and
    //      of its message, shared with the read chokepoint both binders use.
    if (!opts.resolveLibraries.empty()) {
        auto const probeEntry = reporter.errorCount();
        for (auto const& lib : opts.resolveLibraries) {
            std::ifstream probe(lib.path, std::ios::binary);
            if (!probe) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::F_FileOpenFailed;
                d.severity = DiagnosticSeverity::Error;
                d.actual   = std::format(
                    "--resolve-library: failed to open '{}' for reading "
                    "(the resolve-library binary must exist + be readable at "
                    "compile time). Check the path.",
                    core::genericSpelling(lib.path));
                reporter.report(std::move(d));
                continue;  // unopenable: nothing to classify, one message is enough
            }
            probe.close();
            // Reports F_UnsupportedBinaryFormat itself when it objects; the
            // discarded return is the structured twin, which this site (a
            // reporter-driven probe) has no use for.
            (void)ffi::checkLibraryMatchesTargetFormat(lib.path, format,
                                                       reporter);
        }
        if (!tierClean(reporter, probeEntry)) return std::nullopt;
    }

    // 2.5. FFI metadata synthesis for source-declared externs
    //      (FF6 Slice 2, 2026-06-02). Every extern the
    //      HIR lowerer collected gets a FfiMetadata row written to
    //      the per-CU `HirFfiMap`. HIR→MIR (step 3) consumes the
    //      map to materialize each `ExternFunction` /
    //      `ExternGlobal` HIR node as a MIR `ExternImport`.
    //
    //      No extern collected (every existing pre-FF6 module) ⇒
    //      skip the synthesis call entirely; the empty
    //      `HirFfiMap` flows to step 3 as the FfiMap-pointer arg.
    //      lowerToMir's extern-walker iterates HIR nodes whose
    //      kind is ExternFunction / ExternGlobal — modules with
    //      no such nodes never query the map, so an empty map
    //      with no `set()` calls is observationally identical to
    //      passing `nullptr` for callers of empty-extern modules.
    //
    //      ★★ UCRT-P4 (Decision 1): THERE IS NO PER-LANGUAGE
    //      FORMAT-DEFAULT LIBRARY ANY MORE. Every row's import
    //      library comes from the ROW — the PLATFORM's
    //      shipped-descriptor realization, or a source
    //      `extern "otherlib.dll" int foo();`
    //      (D-CSUBSET-EXTERN-LIBRARY-SYNTAX) — and a row with
    //      neither is UNBOUND, resolved at the LINK tier per C23
    //      5.1.1.2 phase 8. The former `externLibraryByFormat` map
    //      was a per-LANGUAGE GUESS standing in for the corpus and a
    //      SECOND OWNER of a fact the corpus owns per SYMBOL, and it
    //      is what made a hand-written
    //      `extern int printf(const char*, ...);` bind a different C
    //      runtime than the same program's `#include`d stdio surface.
    //      Agnostic over CPU + format: the only format-keyed step
    //      left is the FOLD below, which selects the active format's
    //      entry out of each row's own per-format map.
    HirFfiMap ffiMap{hir->hir};
    if (!hir->externDecls.empty()) {
        std::string const formatKey{
            objectFormatKindName(format.kind())};

        // Build the temporary ExternDeclRef span from the lowerer's
        // owning records. The views are valid for the duration of
        // this call only (the underlying strings live on
        // `hir->externDecls` and the `resolvedLibs` backing store below).
        //
        // Model 3 (2026-06-09): `HirExternRecord.libraryOverride` is a
        // per-OBJECT-FORMAT MAP (a shipped descriptor routes a different image
        // per format; a source `"libname"` override is the same string under
        // every format key). This is the ONE site where the active target's
        // object format is in scope, so this is where the map is FOLDED to the
        // single string `ExternDeclRef.libraryOverride` carries. The fold keys
        // on `formatKey` (= objectFormatKindName(format.kind())) — no
        // `if(format)`. A key present ⇒ that image; a key ABSENT ⇒ empty
        // override, which (UCRT-P4, Decision 1) now means UNBOUND — there is no
        // format-level default left to fall back to, so the reference resolves
        // at the LINK tier (C23 5.1.1.2 phase 8).
        std::vector<std::string> resolvedLibs;
        resolvedLibs.reserve(hir->externDecls.size());
        for (auto const& r : hir->externDecls) {
            if (auto it = r.libraryOverride.find(formatKey);
                it != r.libraryOverride.end()) {
                resolvedLibs.push_back(it->second);
            } else {
                resolvedLibs.emplace_back();  // empty ⇒ inherit format default
            }
        }
        std::vector<ffi::ExternDeclRef> refs;
        refs.reserve(hir->externDecls.size());
        for (std::size_t i = 0; i < hir->externDecls.size(); ++i) {
            auto const& r = hir->externDecls[i];
            // c86 (D-CSUBSET-BARE-PROTO-EXTERN-SYNTHESIS): thread the
            // no-library marker — FF5 then leaves the row's importLibrary
            // EMPTY (no format-default fallback) so the reference resolves
            // at the link tier (sibling-TU definition, or the LOUD
            // undefined-symbol reject).
            // D-LINK-EXTERN-IMPORT-REFERENCE-GATE: carry the eager marker so a
            // shipped-descriptor import (producer C) reaches the linker's
            // reference gate as eager (kept even when unreferenced). Non-eager
            // source/bare-proto externs leave it false → dropped if unreferenced.
            // TF-C88 (D-CSUBSET-ASM-LABEL-SYMBOL-RENAME): carry the per-declarator
            // assembler
            // name so FF5/FF1 name the import with it VERBATIM instead of the
            // C-mangled identifier. Empty for every extern without a label ⇒ the
            // downstream naming is byte-identical there.
            // TF-C121 (D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME): carry the
            // descriptor's per-target link BASE name so FF5/FF1 decorate IT with
            // the format's rule instead of the canonical identifier. Already
            // resolved per (arch, format) at descriptor-read time — a plain
            // string like `version`, not a per-format map needing the fold above.
            refs.push_back({r.node, r.canonicalName, resolvedLibs[i],
                            r.noLibraryBinding,
                            r.version,   // D-LK-ELF-SYMBOL-VERSIONING (c156)
                            r.isEagerImport,
                            r.asmName,
                            r.linkName});
        }

        auto const ffiEntry = reporter.errorCount();
        phase.emplace(substrate::CompilePhase::SynthesizeFfi);

        // c162 (D-FF1-READER-CONSUMER): the `--resolve-library` driver
        // surface makes the FF5 `ingest()` binary-reader consumer LIVE.
        //
        // PRECEDENCE for a source-declared extern's import-library binding
        // (highest wins):
        //   1. `noLibraryBinding`  -- empty library; resolves at the link
        //      tier (a sibling-TU definition or a loud undefined-symbol).
        //   2. explicit `libraryOverride` -- a source `extern "lib" ...` OR a
        //      shipped-library descriptor (its per-format `library`). The
        //      binding is EXPLICIT; the binary read never overrides it.
        //   3. `--resolve-library` binary MATCH -- the extern IS exported by
        //      a named binary: bind to it (the true DT_NEEDED / import
        //      descriptor for a DSS-built library that has no descriptor).
        //   4. a governed extern ABSENT from every named binary:
        //      * KNOWN system symbol AND declared for the ACTIVE OBJECT FORMAT
        //        (in some shipped descriptor, e.g. a bare `extern int puts;`
        //        the user did not #include) → fall through, the gcc implicit-libc
        //        semantics -- NOT a fail-loud. ⓘ UCRT-P4 (Decision 1): such a row
        //        now already CARRIES the platform's realization (the semantic
        //        realization pass gave it the descriptor's own per-format library
        //        before this stage ran), so "fall through" no longer means
        //        "inherit a language-level default" — that default is gone. The
        //        partition below sees it as explicitly bound, which is the honest
        //        classification: it IS bound, by the corpus.
        //      * KNOWN system symbol but declared ONLY FOR OTHER FORMATS (the
        //        elf-only `fdatasync` referenced by a macho build) → FAIL LOUD
        //        F_ShippedSymbolUnavailableForTarget. Falling through here would
        //        bind it to a library the config says has no such export: the
        //        image links CLEAN and dies at LOAD with no diagnostic at all
        //        (MEASURED exit 255). See the oracle call site below --
        //        D-FFI-SHIPPED-SYMBOL-ORACLE-IGNORES-OBJECT-FORMATS;
        //      * anything else → route to the UNBOUND channel (precedence 1's
        //        empty-library shape) and let the LINK tier decide. TF-C66
        //        (D-FFI-SHIPPED-LIBS-OS-ONLY testfixture): this stage is
        //        PER-CU, so it cannot know the symbol is defined by a SIBLING
        //        TU of the same multi-TU build (sqlite's own `sqlite3_version`
        //        etc. -- the 185-TU testfixture declared 2770 such externs and
        //        a compile-time fail-loud here false-positived on every one).
        //        The link tier has the whole merged picture and already
        //        implements the exact right policy (rejectOrDropUnreferenced-
        //        Externs, c143/c150): a sibling-TU definition RESOLVES the
        //        reference; an unreferenced declaration is DROPPED; a
        //        referenced-but-undefined symbol (the genuine typo, e.g.
        //        `dss_lib_answr`) REJECTS LOUD with K_SymbolUndefined on any
        //        exec-flavor image. The typo protection is preserved -- it
        //        moved to the tier that can judge it soundly (the same tier
        //        every C toolchain reports undefined symbols from).
        //        F_FfiResolveLibrarySymbolAbsent is retired from this path
        //        (kept in the enum for diagnostic-name stability).
        //
        // The binary read is NON-DUPLICATIVE of the JSON/shipped path
        // precisely because a DSS-BUILT library has no shipped descriptor --
        // reading its real export table is the only way to discover its true
        // library binding. The extern's TYPE still comes from the inline
        // declaration; the reader supplies existence + binding only.
        //
        // When no `--resolve-library` is given (every build before c162),
        // this collapses to the single `synthesizeFfiFromSourceDecls` call
        // over ALL externs -- byte-identical output.
        if (opts.resolveLibraries.empty()) {
            auto const ffiResult = ffi::synthesizeFfiFromSourceDecls(
                refs, target, format, ffiMap, reporter);
            (void)ffiResult;  // shape inspected via reporter.errorCount()
        } else {
            // PARTITION: an extern that already CARRIES A LIBRARY is explicitly
            // bound and the binary read never overrides it (precedence 2); every
            // other extern is "binary-governed" -- exactly the class
            // `--resolve-library` exists to resolve. `refs` and `binaryGoverned`
            // are parallel-indexed to `hir->externDecls` so an unmatched governed
            // extern can be recovered post-ingest.
            //
            // ★★ UCRT-P4 (Decision 1) — THE TEST IS "HAS A LIBRARY", NOT "CARRIES
            // THE no-library FLAG", AND THE DIFFERENCE IS LOAD-BEARING.
            // It used to be `!library.empty() || noLibraryBinding`. That worked only
            // while `noLibraryBinding` meant "a BARE PROTOTYPE deliberately opted
            // out"; it now also means "the platform realizes nothing for this name",
            // which is the state of EVERY extern to a user's own library. Keeping the
            // flag in this test therefore routed `extern int dss_lib_answer(void);`
            // into `explicitlyBound`, hid it from the reader, and made
            // `--resolve-library` unable to bind the very symbols it was pointed at
            // (MEASURED: K_SymbolUndefined on every round-trip test).
            //
            // ⇒ It also CLOSES A PRE-EXISTING ASYMMETRY, which is why this is a fix
            // and not a patch: the bare-prototype producer has always set
            // `noLibraryBinding=true` for an unknown name, so a bare
            // `int dss_lib_answer(void);` was ALREADY invisible to the reader while
            // the `extern`-keyword spelling of the same declaration was governed by
            // it. C23 6.2.2p5 makes those the same declaration. Keying on the LIBRARY
            // makes both governed, and an unmatched governed extern still routes
            // unbound below — so the deliberate opt-out loses nothing it actually
            // protected: it protected the ABSENCE of a guessed library, and the guess
            // is gone.
            std::vector<ffi::ExternDeclRef> binaryGoverned;
            std::vector<ffi::ExternDeclRef> explicitlyBound;
            for (std::size_t i = 0; i < refs.size(); ++i) {
                bool const hasExplicit = !resolvedLibs[i].empty();
                (hasExplicit ? explicitlyBound : binaryGoverned)
                    .push_back(refs[i]);
            }

            // The resolve-library binaries become `ingest()` sources. Three
            // levels decide the identity recorded in the import (DT_NEEDED /
            // LC_LOAD_DYLIB / PE import descriptor), ranked in `ingest()` and
            // documented on `ffi::BinaryLibrarySource`:
            //   1. `declaredImportName` -- what the CLI/manifest STATED for
            //      this entry (D-FFI-DECLARED-IMPORT-NAME; empty = unstated);
            //   2. the binary's own embedded soname, read by FF1;
            //   3. the file BASENAME supplied here as the last-resort fallback.
            // All three are plain strings: no arm of this is language-,
            // target-, or object-format-keyed.
            std::vector<ffi::IngestionSource> binarySources;
            binarySources.reserve(opts.resolveLibraries.size());
            for (auto const& lib : opts.resolveLibraries) {
                binarySources.push_back(ffi::BinaryLibrarySource{
                    lib.path, lib.path.filename().string(),
                    lib.declaredImportName});
            }

            // (i) `ingest()` BINDS every governed extern the named binaries
            // export (writes FfiMetadata to `ffiMap`); it SILENTLY SKIPS the
            // rest (it is a mechanism -- the policy for the unmatched is ours).
            if (!binaryGoverned.empty()) {
                auto const r = ffi::ingest(binarySources, binaryGoverned,
                                           target, format, ffiMap, reporter);
                (void)r;
            }

            // (ii) The governed externs `ingest()` did NOT bind (absent from
            // ffiMap) split by the shipped-descriptor oracle: a KNOWN system
            // symbol falls through to `synthesize` (format-default library);
            // everything else routes UNBOUND (empty library -- precedence 1's
            // channel) so the LINK tier resolves a sibling-TU definition,
            // drops an unreferenced declaration, or rejects a referenced-
            // undefined symbol LOUD (K_SymbolUndefined) -- see the precedence
            // comment above (TF-C66).
            // D-FFI-SHIPPED-SYMBOL-ORACLE-IGNORES-OBJECT-FORMATS: the oracle is
            // FORMAT-AWARE. It answers name -> the UNION (across every declaring
            // row, in every descriptor) of the object formats that name is
            // available on; an EMPTY set means "every format" (the
            // `objectFormatInAvailabilitySet` encoding). Three outcomes below,
            // and the middle one is the whole point of the anchor: a name that
            // is REAL but NOT ON THIS FORMAT used to be judged "known" and bound
            // to the format-default library, which links clean and then dies at
            // LOAD with no diagnostic at all (MEASURED: elf-only `fdatasync` on a
            // macho build -> the loader cannot resolve it in libSystem -> exit
            // 255). The UNION is load-bearing: `call_once`/`thrd_create`/
            // `mtx_lock` (+~20 more) are declared by THREE separate rows gated
            // ["elf"] / ["macho"] / ["pe"], and `sprintf` by an ["elf","macho"]
            // row plus a ["pe"] row -- a per-row test would turn the whole C11
            // threads surface red on every format.
            auto const shippedFormats = ffi::collectShippedExternSymbolFormats();
            std::vector<ffi::ExternDeclRef> fallThrough = explicitlyBound;
            for (auto const& g : binaryGoverned) {
                if (ffiMap.tryGet(g.node) != nullptr) continue;  // bound to a binary
                // `shippedFormats == nullopt` (config discovery failed) => treat
                // every symbol as possibly-known and fall through -- never a
                // false-positive fail-loud just because DSS_CONFIG_ROOT was unset.
                std::vector<std::string> const* declaredFormats = nullptr;
                bool inDescriptors = false;
                if (shippedFormats.has_value()) {
                    if (auto it = shippedFormats->find(
                            std::string{g.canonicalName});
                        it != shippedFormats->end()) {
                        inDescriptors   = true;
                        declaredFormats = &it->second;
                    }
                } else {
                    inDescriptors = true;   // discovery failed -> assume known
                }
                // Known ON THIS FORMAT? `declaredFormats == nullptr` is the
                // discovery-failed arm (assume yes). Otherwise ask the ONE shared
                // membership predicate the #include / __has_include / semantic-
                // injection gates all use, so the oracle can never drift from
                // them (and no `if (format == ...)` appears anywhere here).
                bool const availableHere =
                    declaredFormats == nullptr
                    || ffi::objectFormatInAvailabilitySet(*declaredFormats,
                                                          format.kind());
                // `declaredFormats != nullptr` (rather than `inDescriptors`)
                // makes the deref below locally provable: availableHere is
                // unconditionally true when the pointer is null, so the two
                // spellings select the same set of externs.
                if (declaredFormats != nullptr && !availableHere) {
                    // REAL SYMBOL, WRONG FORMAT. Binding it to the format
                    // default is provably wrong -- the config states it does not
                    // exist there. Fail LOUD naming the symbol, the active
                    // format, and the formats it IS declared for, so the user can
                    // tell this apart from "never heard of this symbol" (which
                    // routes unbound below and surfaces as K_SymbolUndefined at
                    // link).
                    std::string declared;
                    for (auto const& f : *declaredFormats) {
                        if (!declared.empty()) declared += ", ";
                        declared += f;
                    }
                    ParseDiagnostic d;
                    d.code     = DiagnosticCode::F_ShippedSymbolUnavailableForTarget;
                    d.severity = DiagnosticSeverity::Error;
                    d.actual   = std::format(
                        "shipped system symbol '{}' is NOT available on object "
                        "format '{}' -- the shipped descriptors declare it only "
                        "for: {}. Under --resolve-library it matched no named "
                        "binary's export table, and binding it to this format's "
                        "default library would link clean and then FAIL AT LOAD "
                        "(that library has no such export). Guard the reference "
                        "per platform, use this format's own spelling of the "
                        "facility, or declare the symbol for '{}' in its shipped "
                        "descriptor if it really exists there.",
                        g.canonicalName,
                        objectFormatKindName(format.kind()),
                        declared,
                        objectFormatKindName(format.kind()));
                    reporter.report(std::move(d));
                    continue;   // never binds, never routes
                }
                if (inDescriptors) {
                    fallThrough.push_back(g);  // system symbol -> format-default
                } else {
                    // Unknown to the named binaries AND the descriptors:
                    // hand it to the link tier with NO library binding. The
                    // c86 no-library marker makes FF5 leave the import row's
                    // library EMPTY, which is exactly the shape the link
                    // gate governs (resolve / drop / loud-undefined).
                    ffi::ExternDeclRef unbound = g;
                    unbound.libraryOverride = {};
                    unbound.noLibraryBinding = true;
                    fallThrough.push_back(std::move(unbound));
                }
            }

            // (iii) `synthesize` binds the fall-through set (explicitly-bound
            // + system-symbol governed) to their libraries. Skipped if the
            // typo fail-loud above already dirtied the tier (keeps the
            // diagnostic set focused).
            if (tierClean(reporter, ffiEntry) && !fallThrough.empty()) {
                auto const r = ffi::synthesizeFfiFromSourceDecls(
                    fallThrough, target, format, ffiMap,
                    reporter);
                (void)r;
            }
        }
        phase.reset();
        if (!tierClean(reporter, ffiEntry)) {
            return std::nullopt;
        }
    }

    // 3. HIR → MIR. Plug the language schema's globals const-eval
    //    policy into the lowering config (same shape as the
    //    lowered_lir_fixture used by ML6 / AS pipeline tests).
    auto const mirEntry = reporter.errorCount();
    MirLoweringConfig mirCfg;
    mirCfg.globalsAllowFloat =
        grammar.hirLowering().globalsConstEval.allowFloat;
    // D-OPT-LOAD-ALIAS-ANALYSIS-STRICT-TBAA-WIRING (cycle 10d): thread
    // the source-language strict-aliasing opt-in from the SemanticConfig
    // through to the HIR→MIR lowering, which stamps it onto the Mir
    // for CSE/LICM Load admission. Multi-language CUs will eventually
    // AND each schema's knob; today's single-language-per-CU shape
    // reads directly.
    mirCfg.strictAliasingOnDistinctTypes =
        grammar.semantics().pointerAliasing.strictAliasingOnDistinctTypes;
    mirCfg.charTypesAliasAll =
        grammar.semantics().pointerAliasing.charTypesAliasAll;
    // D-CSUBSET-VOID-POINTER-ARITHMETIC-REFUSED: the SAME shape, and threaded
    // here for the same reason — `void`/function operand sizes are a per-LANGUAGE
    // fact (GNU C says 1, ISO C says none) read at TWO tiers, so the schema states
    // it once and both the semantic const-fold and HIR→MIR read that one
    // declaration. Absent ⇒ the strict-ISO refusal, unchanged.
    mirCfg.nonObjectTypeSizes = grammar.semantics().nonObjectTypeSizes;
    // FC6: thread the active target's aggregate-layout params + the format's data
    // model so HIR→MIR can fold `sizeof(T)` to T's byte size via the type_layout
    // engine. The target supplies the alignment rule, the format the pointer width.
    // D-CSUBSET-BITFIELD-ABI-EXACT: the bit-field strategy is FORMAT-determined —
    // overlay the resolved value so bit-field member-access/init lowers byte-ABI-
    // exact for the active object format (PE → msvc_straddle, ELF/Mach-O →
    // gnu_packed), not just whatever the target declared.
    mirCfg.aggregateLayout       = target.aggregateLayout();
    mirCfg.aggregateLayout.bitFieldStrategy = effectiveBfStrategy;
    mirCfg.aggregateLayout.unnamedBitFieldAlignment = effectiveUnnamedBfAlign;
    mirCfg.aggregateLayoutLoaded = target.aggregateLayoutLoaded();
    mirCfg.dataModel             = format.dataModel();
    // TF-C56 (D-CSUBSET-BARE-CHAR-SIGNEDNESS-PER-TARGET) + TF-C75
    // (D-TARGET-CHAR-SIGNEDNESS-PER-PLATFORM): thread the RESOLVED bare-`char` signedness
    // so HIR→MIR picks ZExt vs SExt for the char→int promotion. The axis is
    // (processor × PLATFORM), not per-processor — the same arm64 CPU is
    // UNSIGNED under GNU/Linux and SIGNED under Darwin — and the TARGET
    // declares BOTH halves in its one `charIsUnsigned` key, so this asks the
    // one owner and passes it the active format KIND. No format schema
    // contributes; no arch or platform name is compared.
    mirCfg.charIsUnsigned        = target.charIsUnsigned(format.kind());
    // c86 (D-MIR-SYNTHETIC-GLOBAL-SYMBOL-ALIAS): lift the synthetic-global
    // SymbolId seed clear of the WHOLE semantic symbol table — the LK11
    // merge maps MIR symbols to names through `model.recordFor`, so a
    // synthetic literal global whose id aliased a typedef/tag/field/constant
    // record would enter the merge as a NAMED strong definition (bogus
    // cross-CU redefinitions; potential silent mis-merge onto a literal).
    mirCfg.syntheticSymbolFloor =
        static_cast<std::uint32_t>(model.symbols().size());
    // FC7 (D-FC7-STRUCT-BY-VALUE-ARG-RETURN): thread the RESOLVED calling
    // convention's by-value aggregate strategy into HIR→MIR (the §B-locked
    // boundary). A struct arg/return is classified + synthesized at HIR→MIR; the
    // sret mechanism follows the CC's indirect-result register (absent ⇒ hidden
    // first INTEGER arg, SysV/Win64; present ⇒ x8, AAPCS64 — C3).
    if (auto const* cc = target.callingConvention(callingConventionIndex)) {
        mirCfg.aggregateClassification  = cc->aggregateClassification;
        mirCfg.aggregateMaxRegBytes     = cc->aggregateMaxRegBytes;
        mirCfg.aggregateSretViaHiddenArg = !cc->indirectResultRegister.has_value();
        mirCfg.argSlotAligned           = cc->slotAligned;
        // D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS: the arg-register
        // pool counts (the agnostic source for the all-or-nothing fit check on
        // every call) + the stack-exhaust policy (SysV backfill vs AAPCS64 clamp).
        mirCfg.argGprCount              =
            static_cast<std::uint32_t>(cc->argGprs.size());
        mirCfg.argFprCount              =
            static_cast<std::uint32_t>(cc->argFprs.size());
        mirCfg.aggregateStackExhaustsRegisters =
            cc->aggregateStackExhaustsRegisters;
        // D-CODEGEN-APPLE-ARM64-STACK-ARGS-NOT-NATURALLY-PACKED: the stacked-arg
        // packing rules — HIR→MIR needs them for `va_start`'s overflow base, which
        // is the byte span of the named params that overflowed onto the incoming
        // stack and therefore depends on how those params are packed.
        mirCfg.stackArgPacking          = cc->stackArgPacking;
        // FC12a-core (D-FC12A-VARIADIC-CALLEE): thread the active CC's va_list layout
        // so HIR→MIR can lower va_start/va_arg (or fail loud when the CC omits it).
        mirCfg.vaListLayout             = cc->vaListLayout;
    }
    phase.emplace(substrate::CompilePhase::LowerMir);
    auto mir = lowerToMir(hir->hir, hir->literalPool,
                          model.lattice().interner(), reporter,
                          &hir->sourceMap, mirCfg, &ffiMap,
                          &hir->linkageMap, &hir->mutabilityMap,
                          &hir->volatileMap, &hir->alignmentMap,
                          &hir->threadLocalMap,   // TLS C1
                          &hir->vlaSizeExprBySymbol,   // VLA C1a (D-CSUBSET-VLA)
                          &hir->sizeofVlaSymbol,   // VLA C2 (D-CSUBSET-VLA)
                          &hir->typedefVlaOriginBySymbol,   // VLA C4b (D-CSUBSET-VLA)
                          &hir->synthRecipeBySymbol,   // FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER)
                          &hir->returnsTwiceMap,   // FC17.9(c) (D-CSUBSET-SETJMP)
                          &hir->noInlineMap,   // TF-C78 (D-CSUBSET-NOINLINE)
                          &hir->alwaysInlineMap,   // TF-C81 (D-CSUBSET-ALWAYSINLINE)
                          &hir->noOptimizeMap,   // TF-C85 (#pragma optimize region)
                          &hir->noSanitizeThreadMap,   // TF-C92 (no_sanitize_thread)
                          // ★ Inline-asm P5 (D-CSUBSET-INLINE-ASM-OPERANDS): the
                          // descriptor pool an `InlineAsm` node's payload HANDLE
                          // names. Without it a descriptor-carrying asm statement
                          // cannot be lowered at all and fails loud — which is the
                          // point: this is the argument whose absence used to make
                          // the whole statement lower to a bare barrier, dropping
                          // the template, the operands and the clobber list.
                          &hir->inlineAsmPool,
                          // D-CSUBSET-INLINE-FUNCTION-NO-EXTERNAL-DEFINITION-EMITTED
                          // (C99 6.7.4p7): which lowered bodies are INLINE
                          // DEFINITIONS. Without it HIR→MIR rejects the
                          // function-plus-extern SymbolId pair CST→HIR now emits
                          // for such a definition — loudly, which is the safe
                          // direction, but the feature is dead.
                          &hir->inlineDefinitionMap,
                          // D-C-LABEL-ADDRESS-IN-A-STATIC-INITIALIZER-REFUSED: which
                          // function body each block-scope `static` came out of.
                          // Without it a `&&label` in a static initializer is refused
                          // (loudly — a per-function label ordinal is unresolvable
                          // without its function), and nothing else changes.
                          &hir->enclosingFunctionMap);
    phase.reset();
    if (!mir.ok || !tierClean(reporter, mirEntry)) {
        return std::nullopt;
    }

    // 3.5. MIR optimizer (plan 22). Pipeline resolution + optimize + tier-clean gate
    //      extracted to the shared `optimizeModule` (Cycle 26) so the N>1 whole-program
    //      path can run the SAME pipeline over the MERGED module. Pure code-motion —
    //      same arguments as the former inline block, so the per-CU output is identical.
    if (!optimizeModule(mir.mir, target, model.lattice().interner(), opts,
                        // P10: the per-CU build is the UNIT stage — the site
                        // whose schedule the document's `unitPipeline` key
                        // selects (D-OPT7-CROSSCU-LTO-SINGLE-OPTIMIZE).
                        PipelineStage::Unit, reporter,
                        // D-CSUBSET-INLINE-FUNCTION-NO-EXTERNAL-DEFINITION-EMITTED:
                        // this CU's extern table, still owned by `mir` here (the
                        // LOWER half moves it into MIR→LIR later).
                        mir.externImports)) {
        return std::nullopt;
    }

    // BUILD half complete — hand the optimized MIR + the SemanticModel (interner
    // owner) + extern imports + the schema refs across the MIR/LIR seam. The model
    // is MOVED in so the interner survives for `lowerCuMirToAssembly`. Loop 1 of the
    // multi-CU driver collects these; loop 2 lowers each (Cycle 24 re-sequence).
    CuMirModule cuMir{
        std::move(mir.mir),
        std::move(model),
        std::move(mir.externImports),
        cu.id(),
        &grammar,
        &target,
        callingConventionIndex,
        // D-FFI-EXTERN-CALL-DISPATCH: capture the active format's extern-call
        // shape now (the LOWER half sees only this struct, not the format).
        format.externCallDispatch(),
        // D-LK-EXTERN-DATA-IMPORT (c117): capture the format's extern-DATA
        // binding model now, for the same reason (the LOWER half's MIR→LIR
        // GlobalAddr lowering selects got-indirect deref vs a direct lea).
        format.dataImportBinding(),
        // D-LK-ARM64-EXTERN-DATA-ADDR-PIE-GOT (TF-C52): capture the format's
        // extern-ADDRESS binding now, for the same reason (the LOWER half's
        // MIR→LIR GlobalAddr value-form arm routes an `&extern` value
        // through the arm64 GOT-address macro under `got`).
        format.externAddrBinding(),
        // TLS C1 (D-CSUBSET-THREAD-LOCAL): capture the format's thread-local
        // access block now, for the same reason (the LOWER half's MIR→LIR
        // GlobalAddr lowering selects the TLS access sequence; nullopt =
        // thread-local accesses fail loud on this leg).
        format.tlsAccess(),
        // D-LK4-RODATA-PRODUCER-AGGREGATE-GLOBAL: capture the format's data
        // model now, for the same reason — the aggregate-global rodata encoder
        // (in the LOWER half) needs the pointer width to compute byte layout.
        format.dataModel(),
        // D-CSUBSET-BITFIELD-ABI-EXACT: capture the FORMAT-resolved bit-field
        // strategy so the LOWER half lays out bit-field globals byte-ABI-exact.
        effectiveBfStrategy,
    };
    // FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER) + D-FFI-PE-CRT-UCRT-MIGRATION (Phase 3):
    // carry the shim recipe table — BOTH families, pe64 <threads.h> and pe64 <stdio.h> —
    // across the MIR/LIR seam so the LOWER half can define each shim. `hir` (the
    // CstToHirResult) is still alive here — lowerToMir read its maps by pointer, it was not
    // consumed. Empty for the overwhelming majority.
    cuMir.libraryShimRecipes = hir->synthRecipeBySymbol;
    // D-CSUBSET-C11-THREADS-MACHO: capture the format's synth vehicle (win32/pthread +
    // import library) the SAME post-construction way as libraryShimRecipes, so the LOWER
    // half's `synthesizeThreadsShim` picks the right primitive family. nullopt on elf.
    // D-CSUBSET-ZERO-WIDTH-BITFIELD-ALIGNMENT: set post-construction (the
    // `librarySynthesis` idiom) rather than positionally, so adding this axis cannot
    // silently shift a neighbouring member of the aggregate initializer above.
    cuMir.unnamedBitFieldAlignment = effectiveUnnamedBfAlign;
    cuMir.librarySynthesis = format.librarySynthesis();
    // D-FFI-CMANGLING-RULE-NOT-CONFIG-DRIVEN (C4): the DECLARED rule, not the identity.
    cuMir.cSymbolDecoration = format.cSymbolDecoration().scheme;
    // D-FFI-PE-CRT-UCRT-MIGRATION (Phase 3): capture the RESOLVED CC's WHOLE `vaListLayout`
    // so the LOWER half's `synthesizeStdioShim` knows the target's variadic-forwarding
    // model — strategy AND `variadicUsesOverflowBase`, which is what picks the va leaf.
    // Reuses `analyzeVaLayout` (resolved above from the SAME CC that also feeds the
    // semantic `va_list`-type injection) rather than re-resolving the CC a second time. The
    // optional is assigned THROUGH — its EMPTINESS is part of the signal, not noise to be
    // defaulted away: a CC that declares no `vaListLayout` must reach the synth pass as
    // "nothing declared", which that pass refuses loudly, rather than as a real layout it
    // could not tell from a genuine declaration. Consulted only if a stdio recipe appears.
    cuMir.vaListLayout = analyzeVaLayout;
    return cuMir;
}

// LOWER half body (Cycle 25, Stage C): MIR → LIR → liveness → regalloc → rewrite →
// legalize → callconv → assemble → the LK11a symbol-table populate → the user-entry
// resolution, producing the `AssembledModule` (NO link, NO write). PARAMETERIZED on
// the seam state so BOTH the single-CU path (`lowerCuMirToAssembly`) and the merged
// whole-program path (`lowerMergedToAssembly`) share one body:
//   * `mir`               — the module to lower (per-CU optimized OR whole-program merged).
//   * `interner`          — the type interner the module's TypeIds index into (the per-CU
//                           lattice's interner OR the merged host lattice's interner).
//   * `nameOf`            — merged/declared symbol-id → declared name; powers the LK11a
//                           symbol-table populate (replaces the per-CU `model.recordFor`).
//   * `externImports`     — the module's surviving real-FFI imports (MOVED into MIR→LIR).
//   * `userEntrySymbol`   — the caller's pre-resolved user-entry symbol (its CALLER ran
//                           the entry-name scan: the single-CU path against the
//                           SemanticModel, the merged path inside `mergeCuMirs`). When
//                           set it is stamped onto `AssembledModule.userEntrySymbol`;
//                           nullopt leaves it nullopt (no entry found — pre-Cycle-25 shape).
//   * `target`            — the MIR→LIR + assemble target.
//   * `cuId`              — stamped onto the AssembledModule so the linker keys symbols.
// Returns nullopt on any back-half tier failure (diagnostics already emitted via `reporter`).
//
// ★★★ BIND ONE INTERIOR-BLOCK SYMBOL TO ITS BYTE OFFSET — THE ONE STEP TWO
// SOURCE LANGUAGES SHARE. `assemble()` binds a block symbol automatically when
// an INSTRUCTION named the block (the block-address `lea`'s trailing `BlockRef`
// becomes a `BlockSymPatch`). A block named only from DATA emits no such
// instruction, so the binding has to happen here, from the function's published
// `blockByteOffsets`. Two producers reach this:
//   * a C dense `switch` — `JumpTableDescriptor` (D-OPT-SWITCH-JUMP-TABLE);
//   * a hand-written `.s` jump table — `AsmBlockSymbolBinding`
//     (D-ASM-INTERIOR-LABELS-NOT-ADDRESSABLE-AT-AN-OFFSET).
// They differ in what ELSE they carry (the C path also has to synthesize the
// table's bytes; a `.s` already wrote them), so only the binding is shared —
// but it is shared as CODE, not as a copied pair of lines, because a second
// copy is how the two would start disagreeing about `alreadyBound`.
//
// Returns false when the function published no byte offset for `lirBlockV` —
// malformed IR, which each caller reports with its own subject.
[[nodiscard]] static bool
bindBlockSymbol(AssembledFunction& outFn, std::uint32_t lirBlockV,
                SymbolId blockSymbol,
                std::unordered_set<std::uint32_t>& alreadyBound) {
    // Already bound (a block that is ALSO a computed-goto `&&label`, or a
    // duplicate/gap slot reusing one SymbolId) — binding twice would give the
    // linker two VAs for one symbol.
    if (!alreadyBound.insert(blockSymbol.v).second) return true;
    auto const offIt = outFn.blockByteOffsets.find(lirBlockV);
    if (offIt == outFn.blockByteOffsets.end()) return false;
    outFn.blockSymbols.push_back(
        SyntheticBlockSymbol{blockSymbol, offIt->second});
    return true;
}

// The grammar's entry-name list is intentionally NOT a parameter — the entry-name SCAN
// needs to ENUMERATE symbols + names (which `nameOf` cannot do) plus the ambiguity
// fail-loud, so each caller runs it and hands the resolved id here. Keeping a dead
// `grammar` param just to mirror the old monolith would be a smell.
static std::optional<AssembledModule>
lowerMirModuleToAssembly(Mir&                                        mir,
                         TypeInterner const&                         interner,
                         std::function<std::string(SymbolId)> const& nameOf,
                         std::vector<ExternImport>                    externImports,
                         std::optional<SymbolId>                     userEntrySymbol,
                         TargetSchema const&                         target,
                         DataModel                                   dataModel,
                         BitFieldStrategy                            bitFieldStrategy,
                         UnnamedBitFieldAlignment                    unnamedBitFieldAlignment,
                         std::uint16_t                               callingConventionIndex,
                         CompilationUnitId                           cuId,
                         std::optional<ExternCallDispatch>           externCallDispatch,
                         std::optional<DataImportBinding>            dataImportBinding,
                         // D-LK-ARM64-EXTERN-DATA-ADDR-PIE-GOT (TF-C52): the
                         // format's extern-ADDRESS binding, threaded into
                         // MIR→LIR exactly like dataImportBinding (nullopt =
                         // this leg has no GOT-address model; an `&extern`
                         // value takes the ordinary lea).
                         std::optional<ExternAddrBinding>            externAddrBinding,
                         // TLS C1 (D-CSUBSET-THREAD-LOCAL): the format's
                         // thread-local access block, threaded into MIR→LIR
                         // exactly like dataImportBinding (nullopt = this leg
                         // has no TLS machinery; thread-local accesses fail
                         // loud K_FormatLacksThreadLocalSupport).
                         std::optional<TlsAccessInfo>                tlsAccess,
                         // c116 (D-WIN64-SEH-FUNCLETS): the SEH scope records the
                         // funclet-synthesis pass produced (empty for a non-SEH
                         // module). Threaded into MIR→LIR, which emits the
                         // SehScopeDescriptors this body then binds post-assemble.
                         std::vector<MirSehScope>                    sehScopes,
                         // D-CSUBSET-LONG-DOUBLE-IEEE128-ARITH (LD-2): the active
                         // format's F128 softfloat-helper runtime library
                         // (libgcc_s.so.1 on elf-arm64), threaded into MIR→LIR
                         // exactly like the extern-dispatch/TLS blocks (nullopt =
                         // this leg has no F128 softcall binding; an F128 softcall
                         // then fails loud). Resolved per-leg by each wrapper.
                         std::optional<std::string>                  wideFloatSoftcallLibrary,
                         DiagnosticReporter&                         reporter) {
    // 4. MIR → LIR (vreg-based). Extern imports propagate through.
    // D-FFI-EXTERN-CALL-DISPATCH: the active format's extern-call shape
    // selects the call-site opcode (indirect-slot → call_indirect_via_extern;
    // direct-plt → plain call). Threaded from the format at the driver.
    // c97: sequential per-phase scoping (see buildCuMirImpl) — lower-lir
    // covers 4+4b, regalloc covers 5-9, encode covers 10 + the data items.
    std::optional<substrate::PhaseTimers::Scope> phase;
    phase.emplace(substrate::CompilePhase::LowerLir);
    auto const lirEntry = reporter.errorCount();
    auto lir = lowerToLir(mir, target,
                          interner, reporter,
                          std::move(externImports),
                          externCallDispatch,
                          dataImportBinding,
                          tlsAccess,
                          sehScopes,
                          std::move(wideFloatSoftcallLibrary),
                          // D-LK-ARM64-EXTERN-DATA-ADDR-PIE-GOT (TF-C52):
                          // trailing param on lowerToLir (positional-safe).
                          externAddrBinding);
    if (!lir.ok || !tierClean(reporter, lirEntry)) {
        return std::nullopt;
    }
    // 4a. THE WHOLE-MODULE STRUCTURAL CHECK, on the module MIR→LIR just
    // produced and BEFORE any pass rebuilds it — so a violation is
    // attributed to the lowering that minted it rather than to whichever
    // later pass happened to copy it forward.
    //
    // This is the ONE verifier entrypoint that gets the MIR cross-reference
    // (`lir.lirToMir`), so it is the only place the type-agreement rules can
    // run at all: a `store` whose value operand's register class disagrees
    // with the MIR pointee type, a result vreg whose class disagrees with its
    // MIR result type, an `intrinsic_call` whose result presence disagrees
    // with its MIR Void-ness. Each of those is a silent wrong-register-file
    // encode downstream, not a crash. The three later checkpoints
    // (`verifyLirRebuild` ×4, `verifyLirPostRegalloc` ×2) are rebuild- and
    // allocation-shaped and cannot see any of it.
    //
    // ⚠ IT COULD NOT BE CALLED HERE UNTIL 2026-08-15, and the reason is worth
    // keeping: Rule 1 (`checkMemOperandPairing`) demanded that every
    // `load`/`store`/`lea` operand list end with a `MemBase`+`MemOffset`
    // pair, while the shipped lowering also emits a symbol-addressed form
    // (`lea r, [@sym]` for every `&global`, the TLS address blocks, a
    // jump-table base) carrying no base/offset pair at all — so enabling the
    // call reddened the examples corpus while the COMPILER was correct and
    // the VERIFIER was wrong (D-LIR-VERIFY-MEM-OPERAND-PAIRING-RULE-IS-FALSE,
    // closed by teaching the rule both addressing modes and asserting them
    // disjoint).
    if (!verifyLir(lir.lir, mir, interner, target, lir.lirToMir, reporter).ok) {
        return std::nullopt;
    }

    // 4b. Wide-call arg materialization (D-AS-REGALLOC-WIDE-CALL-OPERAND-COUNT,
    //     option E): BEFORE regalloc, split each Call's scalar arguments beyond
    //     the active cc's register-passed count into `store_outgoing_arg`
    //     carriers, so no Call holds more register-operands than the machine
    //     passes in registers (the func-2088 wide-call blocker). Config-driven
    //     from the cc descriptor (argGprs/argFprs/slotAligned). This is the
    //     earliest tier that both knows the active cc AND holds the LIR.
    auto const wideEntry = reporter.errorCount();
    auto wideLir = lowerWideCallArgs(lir.lir, target, callingConventionIndex,
                                     reporter);
    if (!wideLir.ok || !tierClean(reporter, wideEntry)) {
        return std::nullopt;
    }
    // ★★ THE PAIRED REBUILD CHECK, PASS 1 OF 4
    // (D-LIR-PER-INST-REG-CONSTRAINTS). Each of the four passes below
    // rebuilds the module into a FRESH `LirBuilder`, and both side structures
    // — the wide-literal pool and the per-instruction register-constraint
    // pool — are referenced BY INDEX from the instruction stream that the
    // rebuild re-creates. A dropped reference is structurally invisible
    // (nothing dangles, no pool shrank, the handle reads as the perfectly
    // legal "no constraints") and its consequence is a SILENT MISCOMPILE: the
    // clobbers vanish and the allocator reuses a register the instruction
    // destroys. The before/after pair is the only place the loss shows up, so
    // it is checked HERE, per pass, rather than once at the end — a drop in
    // pass 1 restored by luck in pass 4 reads as green.
    if (!verifyLirRebuild(lir.lir, wideLir.lir, "wide-call-args", reporter)) {
        return std::nullopt;
    }

    // 5. Liveness analysis (input to regalloc).
    phase.emplace(substrate::CompilePhase::Regalloc);
    auto const liveness = analyzeLiveness(wideLir.lir);

    // 6. Register allocation.
    auto const allocEntry = reporter.errorCount();
    auto const alloc = allocateRegisters(wideLir.lir, target, liveness,
                                          callingConventionIndex, reporter);
    if (!alloc.ok() || !tierClean(reporter, allocEntry)) {
        return std::nullopt;
    }

    // 7. Rewrite vregs → physical registers.
    auto const rewriteEntry = reporter.errorCount();
    auto rewritten = rewriteWithAllocation(wideLir.lir, target, alloc, reporter);
    if (!rewritten.ok || !tierClean(reporter, rewriteEntry)) {
        return std::nullopt;
    }
    // Paired rebuild check, pass 2 of 4 — plus the POST-REGALLOC rules
    // (`lir_rewrite.hpp` has told its callers to run `verifyLirPostRegalloc`
    // since ML6 and no caller did): no virtual register may survive anywhere,
    // and every `frame_load`/`frame_store` must carry a non-zero spill-slot
    // payload. A virtual register reaching the assembler encodes as whatever
    // its vreg id happens to alias — a physical register nobody allocated.
    if (!verifyLirRebuild(wideLir.lir, rewritten.lir, "rewrite", reporter)
        || !verifyLirPostRegalloc(rewritten.lir, target, reporter)) {
        return std::nullopt;
    }

    // 8. Two-address legalize (post-regalloc).
    auto const legalEntry = reporter.errorCount();
    auto legal = legalizeTwoAddress(rewritten.lir, target, reporter);
    if (!legal.ok() || !tierClean(reporter, legalEntry)) {
        return std::nullopt;
    }
    // Paired rebuild check, pass 3 of 4.
    if (!verifyLirRebuild(rewritten.lir, legal.lir, "two-address-legalize",
                          reporter)) {
        return std::nullopt;
    }
    // TF-C58 bisect (env-gated, zero-cost when unset): the loop-carried-update check
    // runs at EACH post-regalloc stage so the pass that drops a back-edge update is
    // identified by which stage first reports.
    checkLoopCarriedSpills(legal.lir, target, "post-legalize");

    // 8b. LIR PEEPHOLE (plan 22 OPT8) -- delete the register-to-register
    //     copies the allocator left redundant. See `lir_peephole.hpp` for
    //     why it runs HERE and not after callconv: callconv mints ZERO
    //     additional identity copies (MEASURED 5575 at both stages over
    //     `examples/c/**`) and its `perFuncCfi` is keyed BY `LirInstId`,
    //     so a rebuild downstream of it would renumber every CFI row's
    //     subject -- an unwind table that loads clean and walks into the
    //     wrong frame.
    auto const peepEntry = reporter.errorCount();
    auto peeped = runLirPeephole(legal.lir, target, reporter);
    if (!peeped.ok() || !tierClean(reporter, peepEntry)) {
        return std::nullopt;
    }
    // Paired rebuild check -- the same side-structure census every other
    // rebuilding pass is held to, plus the post-regalloc rules (this pass
    // is the only one that DELETES, so it is the only one that can orphan
    // a pool entry).
    if (!verifyLirRebuild(legal.lir, peeped.lir, "lir-peephole", reporter)
        || !verifyLirPostRegalloc(peeped.lir, target, reporter)) {
        return std::nullopt;
    }

    // 9. Calling-convention materialization (prologue/epilogue,
    //    frame_load/frame_store; `arg` virtual-op rewrite is the
    //    ML7 cycle 2 gap — anchored D-LK10-2 for caller awareness).
    // c116 H1 (D-WIN64-SEH-FUNCLETS): thread the funclet→parent bindings so each
    // funclet's `recover_parent_frame_slot` ops resolve their slot offsets against
    // the PARENT's finalized FrameLayout (the funclet is materialized after its
    // parent, so the parent layout is already computed). Empty for a non-SEH module.
    std::vector<SehFuncletParent> sehFuncletParents;
    sehFuncletParents.reserve(sehScopes.size());
    for (auto const& s : sehScopes) {
        sehFuncletParents.push_back(
            SehFuncletParent{s.filterFuncletSymbol, s.parentFuncSymbol});
    }
    auto const ccEntry = reporter.errorCount();
    // D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN: thread each function's max local
    // alignment (SymbolId-keyed, from MIR→LIR — it survived the LIR rebuilds)
    // so `computeFrameLayout` can align the local area for an over-aligned local
    // and fail loud on one exceeding the stack-slot bound. Translated MIR→LIR's
    // descriptor to the callconv's (SAME shape, decoupled headers — mirrors the
    // sehFuncletParents projection above). Empty when no function has an
    // over-aligned local (the common case).
    std::vector<LirFuncLocalAlignment> funcLocalAligns;
    funcLocalAligns.reserve(lir.funcLocalAlignments.size());
    for (auto const& a : lir.funcLocalAlignments) {
        funcLocalAligns.push_back(
            LirFuncLocalAlignment{a.funcSymbol, a.maxLocalAlignBytes,
                                  a.perAllocaAlignBytes});
    }
    auto cc = materializeCallingConvention(peeped.lir, target, alloc, reporter,
                                           sehFuncletParents,
                                           funcLocalAligns);
    if (!cc.ok() || !tierClean(reporter, ccEntry)) {
        return std::nullopt;
    }
    // Paired rebuild check, pass 4 of 4 — the LAST checkpoint before the
    // module turns into bytes. `verifyLirPostRegalloc` runs again here for the
    // same reason: callconv materializes the frame ops and the arg/return
    // moves, so it is the pass with the most opportunities to mint a register
    // operand, and after `assemble()` there is no LIR left to check.
    if (!verifyLirRebuild(peeped.lir, cc.lir, "callconv", reporter)
        || !verifyLirPostRegalloc(cc.lir, target, reporter)) {
        return std::nullopt;
    }

    // 10. Assemble. `lirToMir` is all-invalid at this stage — the
    //     post-legalize, post-callconv LIR's instruction arena has
    //     diverged from the original MIR's instruction set, so a
    //     fidelity-preserving map would require legalize + callconv
    //     to thread their own translation tables (anchored at
    //     plan 12 D-ML3-2.1 MirSourceMap IOU). Cycle 2 acceptance
    //     pins SHAPE + BYTES, not source-map fidelity.
    auto const asmEntry = reporter.errorCount();
    phase.emplace(substrate::CompilePhase::Encode);
    std::vector<MirInstId> lirToMir(cc.lir.instCount(), InvalidMirInst);
    checkLoopCarriedSpills(cc.lir, target, "post-callconv");
    dumpLirFuncs(cc.lir, target, "post-callconv");
    auto assembled = assemble(cc.lir, target, lirToMir, reporter,
                              lir.externImports);
    if (!assembled.ok() || !tierClean(reporter, asmEntry)) {
        return std::nullopt;
    }

    // Cycle 25 Stage C — stamp the user-entry symbol the CALLER pre-resolved
    // (D-CSUBSET-MULTI-FN-WIN64-CC). The entry-name SCAN lives in each caller because
    // it ENUMERATES symbols + names (which `nameOf` cannot do) plus the ambiguity
    // fail-loud:
    //   * single-CU (`lowerCuMirToAssembly`) scans the SemanticModel's symbol records
    //     — verbatim the scan this body ran pre-Cycle-25, so the result is identical;
    //   * merged whole-program (`lowerMergedToAssembly`) reads the id `mergeCuMirs`
    //     already computed against the merged functions.
    // `assembled.userEntrySymbol` is nullopt out of `assemble()`, so a caller passing
    // nullopt (no entry found) leaves it nullopt — exactly the pre-Cycle-25 shape (the
    // trampoline injector then falls through to its own default).
    if (userEntrySymbol.has_value()) {
        assembled.userEntrySymbol = userEntrySymbol;
    }

    // ── CALL FRAME INFORMATION (plan 15 CFI slice) ──
    //
    // Join the two halves of the unwind description that no single tier holds:
    //   * `cc.perFuncCfi[fi]` -- WHICH instructions change the frame and WHAT
    //     each one means. Emitted by `emitPrologue` / `emitEpilogue` / the VLA
    //     frame-pointer capture, at their own emit sites, keyed by `LirInstId`.
    //   * `assembled.functions[fi].sourceMap` -- WHERE each of those
    //     instructions landed in the byte stream. Recorded by the assembler.
    //
    // * NOTHING HERE ASSUMES AN ENCODING. The predecessor of this block built a
    //   `FrameUnwindInfo` -- a frame SHAPE with no PC dimension -- and left its
    //   consumer to reconstruct prologue byte offsets from hardcoded instruction
    //   lengths (`sub rsp,imm32` is 7; a GPR spill store is 8; an xmm8-15 spill
    //   is 9). Those numbers were right only because one encoder happened to
    //   pick one form; a `MemDisp8` selection pass would have silently shifted
    //   every unwind CodeOffset with nothing to catch it.
    //
    // Positional, mirroring the dataItems/userEntrySymbol post-`assemble()`
    // splices: `cc.perFunc`, `cc.perFuncCfi` and `assembled.functions` are ALL
    // guaranteed size == moduleFuncCount() (`cc.ok()` + `assembled.ok()`) and
    // enumerated identically.
    if (cc.perFuncCfi.size() == assembled.functions.size()) {
        // The entry state, straight off the calling convention -- no new config.
        // `callPushBytes` is defined as the SP delta at call entry, which IS the
        // CFA offset at function entry; `linkRegister` states whether the return
        // address is on the stack or in a register. Reading them here keeps the
        // frame-alignment rule and the unwind rule on ONE fact.
        auto const* ccDesc = target.callingConvention(callingConventionIndex);
        if (ccDesc == nullptr || !ccDesc->stackPointer.has_value()) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::K_UnwindRuleUnrepresentable;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "calling convention index {} declares no stack-pointer register, "
                "so no canonical-frame-address rule can be stated for this "
                "module's functions; unwind information would have to be guessed "
                "(plan 15 CFI)", callingConventionIndex);
            reporter.report(std::move(d));
            return std::nullopt;
        }
        CfiInitialState initial;
        initial.cfaRegister = ccDesc->stackPointer->ordinal;
        initial.cfaOffset   = static_cast<std::int64_t>(ccDesc->callPushBytes);
        if (ccDesc->callPushBytes > 0) {
            // The CALL pushed the return address, so it sits immediately below
            // the CFA -- at CFA minus exactly what was pushed.
            initial.returnAddressAtCfaOffset =
                -static_cast<std::int64_t>(ccDesc->callPushBytes);
        } else if (ccDesc->linkRegister.has_value()) {
            initial.returnAddressRegister = ccDesc->linkRegister->ordinal;
        }
        // NOTE the deliberate absence of an `else` fail-loud: a convention with
        // neither a pushed return address nor a link register is a machine model
        // with no return address to describe at all (an operand-stack VM). Its
        // functions get a CFA rule and no RA rule, which is the truth.

        // ★★★ THE JOIN ITSELF LIVES IN `asm/asm_cfi.hpp`, AND IT IS SHARED WITH
        // THE ASSEMBLY PRODUCER (D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED). It
        // used to be written out here, and that was fine while there was ONE
        // producer; a hand-written `.s`'s `.cfi_*` directives are a second one,
        // keyed identically (`LirCfiOp::inst`), and `core/types/cfi.hpp` opens
        // by naming the failure two descriptions of one frame produce. So the
        // resolution moved to the tier that owns BOTH halves it joins --
        // `AssembledFunction`'s byte offsets and `LirFuncCfi`'s anchors -- and
        // both callers reach it through one function.
        if (!attachCallconvCfi(assembled, cc.perFuncCfi, initial, reporter)) {
            return std::nullopt;
        }
    }

    // D-LK4-RODATA-PRODUCER (2026-06-02): materialize MIR globals
    // into AssembledData items the linker emits as .rodata. The
    // MIR globals model (MirBuilder::addGlobal) was already wired
    // by HIR→MIR (e.g. `int g = 42;` at file scope produces a
    // MirGlobal with constant-init literal pool entry); previously
    // these globals were declared in MIR but DROPPED at assemble()
    // since the assembler had no globals-bytes path. The new pass
    // closes the producer thread end-to-end.
    // D-LK4-RODATA-PRODUCER-AGGREGATE-GLOBAL: pass the target's per-ABI
    // aggregate-layout params (nullopt if the target declared no block — an
    // aggregate global then fails loud, never a guessed layout) + the format's
    // data model (the pointer width) so a const-init aggregate global
    // (`struct P { int x; int y; } v = { 20, 22 };`) reaches `.rodata`
    // byte-exact via the shared `type_layout` engine.
    // D-CSUBSET-BITFIELD-ABI-EXACT: overlay the FORMAT-resolved bit-field strategy
    // (threaded in as `bitFieldStrategy`) so a const-init BIT-FIELD global packs
    // byte-ABI-exact for the active object format.
    std::optional<AggregateLayoutParams> globalsLayout;
    if (target.aggregateLayoutLoaded()) {
        globalsLayout = target.aggregateLayout();
        globalsLayout->bitFieldStrategy = bitFieldStrategy;
        globalsLayout->unnamedBitFieldAlignment = unnamedBitFieldAlignment;
    }
    // F5 (D-CSUBSET-SYMBOL-ADDRESS-GLOBAL): find the target's ABSOLUTE-64 pointer
    // relocation kind by FORMULA (widthBytes==8 && !pcRelative), never by name —
    // agnosticism (the same scan the linker uses for cross-CU thunk slots). A
    // symbol-address global (`char* g="..."`, `int* p=&x`) emits this reloc; if
    // the target declares none, the assembler fails loud.
    std::optional<RelocationKind> absPtrRelocKind;
    for (auto const& r : target.relocations()) {
        if (r.widthBytes == 8 && !r.pcRelative) { absPtrRelocKind = r.kind; break; }
    }
    auto dataItems = lowerMirGlobalsToDataItems(
        mir, interner, globalsLayout, dataModel, reporter, absPtrRelocKind);
    if (!tierClean(reporter, asmEntry)) {
        // Any per-global encoding error already raised a loud
        // diagnostic via the function's internal `emit`.
        return std::nullopt;
    }
    assembled.dataItems = std::move(dataItems);

    // D-OPT-SWITCH-JUMP-TABLE (c70): materialize each dense switch's `.data`
    // address table from the descriptors the LIR lowerer emitted. Runs AFTER
    // assemble() because it reads each owning AssembledFunction's blockByteOffsets
    // (populated by the assembler) to bind the synthetic per-block symbols the
    // table's slots relocate against — those blocks have no live block-address
    // `lea`, so the assembler's BlockSymPatch loop never bound them. Each table
    // is one `AssembledData{Data, span*8 bytes, abs64 reloc per slot}` — the same
    // proven shape as a c67 symbol-address global (writable-at-load `.data` so
    // Mach-O dyld can PIE-rebase it; ELF ET_EXEC / PE `.reloc` handle the abs64
    // in-place / via base-relocations). `absPtrRelocKind` is the target's abs64
    // pointer reloc (found by the widthBytes==8 && !pcRelative formula above); if
    // the target declares none, a jump table cannot be emitted → fail loud.
    for (auto const& desc : lir.jumpTableDescriptors) {
        if (!absPtrRelocKind.has_value()) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::K_NoMatchingObjectFormat;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "jump-table (SymbolId={{ {} }}) requires an absolute-64 pointer "
                "relocation but target '{}' declares none (D-OPT-SWITCH-JUMP-"
                "TABLE) — the dense-switch address table cannot be emitted",
                desc.tableSymbol.v, target.name());
            reporter.report(std::move(d));
            return std::nullopt;
        }
        if (desc.funcIndex >= assembled.functions.size()) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::K_NoMatchingObjectFormat;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "jump-table descriptor names function index {} but the assembled "
                "module has {} function(s) (D-OPT-SWITCH-JUMP-TABLE)",
                desc.funcIndex, assembled.functions.size());
            reporter.report(std::move(d));
            return std::nullopt;
        }
        AssembledFunction& outFn = assembled.functions[desc.funcIndex];

        // Byte offsets already bound into blockSymbols (e.g. a target block that
        // is ALSO a computed-goto `&&label`) — don't double-bind those.
        std::unordered_set<std::uint32_t> alreadyBound;
        for (auto const& bs : outFn.blockSymbols) alreadyBound.insert(bs.symbol.v);

        AssembledData table;
        table.symbol    = desc.tableSymbol;
        table.section   = DataSectionKind::Data;
        table.alignment = Alignment::ofRuntimePow2(8);
        table.bytes.assign(desc.slotCount * 8u, std::uint8_t{0});
        table.relocations.reserve(desc.slotBindings.size());

        bool tableOk = true;
        for (auto const& [lirBlockV, slotIdx] : desc.slotBindings) {
            auto symIt = desc.blockSymbols.find(lirBlockV);
            if (symIt == desc.blockSymbols.end()) { tableOk = false; break; }
            SymbolId const blkSym = symIt->second;
            // Bind the block symbol from the function's byte-offset map (once per
            // distinct symbol; a gap/duplicate reuses the same SymbolId).
            if (!bindBlockSymbol(outFn, lirBlockV, blkSym, alreadyBound)) {
                tableOk = false;
                break;
            }
            // abs64 reloc at slot byte offset (slotIdx * 8) → the block symbol.
            table.relocations.push_back(Relocation{
                static_cast<std::uint32_t>(slotIdx * 8u),
                blkSym, *absPtrRelocKind, /*addend=*/0});
        }
        if (!tableOk) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::K_NoMatchingObjectFormat;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "jump-table (SymbolId={{ {} }}) references a target block with no "
                "byte offset or symbol — malformed descriptor (D-OPT-SWITCH-JUMP-"
                "TABLE)", desc.tableSymbol.v);
            reporter.report(std::move(d));
            return std::nullopt;
        }
        assembled.dataItems.push_back(std::move(table));
    }

    // D-C-LABEL-ADDRESS-IN-A-STATIC-INITIALIZER-REFUSED: bind each block whose
    // address a STATIC-STORAGE initializer took and that no instruction names.
    // `static void *tbl[] = {&&L0, &&L1}; goto *tbl[i];` reads the addresses out of
    // the table rather than materializing them, so the encoder's `BlockSymPatch`
    // channel never fires for those blocks. THE BYTES ARE NOT OURS TO EMIT — they
    // are the C object's own initializer, already emitted by
    // `lowerMirGlobalsToDataItems` with one abs64 relocation per slot; only the
    // binding is missing, which is the third producer this comment block's own
    // header (at `bindBlockSymbol`) describes.
    for (auto const& b : lir.blockSymbolBindings) {
        if (b.funcIndex >= assembled.functions.size()) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::K_NoMatchingObjectFormat;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "a label-address block binding names function index {} but the "
                "assembled module has {} function(s) "
                "(D-C-LABEL-ADDRESS-IN-A-STATIC-INITIALIZER-REFUSED)",
                b.funcIndex, assembled.functions.size());
            reporter.report(std::move(d));
            return std::nullopt;
        }
        AssembledFunction& outFn = assembled.functions[b.funcIndex];
        // Seeded per function from what `assemble()` already bound (the same block
        // may ALSO carry a computed-goto `lea`), so one symbol never gets two VAs.
        std::unordered_set<std::uint32_t> alreadyBound;
        for (auto const& bs : outFn.blockSymbols) alreadyBound.insert(bs.symbol.v);
        if (!bindBlockSymbol(outFn, b.lirBlockV, b.symbol, alreadyBound)) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::K_NoMatchingObjectFormat;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "a static initializer takes the address of block {} in fn '{}', but "
                "the assembler published no byte offset for it — the block was "
                "elided after its address was taken "
                "(D-C-LABEL-ADDRESS-IN-A-STATIC-INITIALIZER-REFUSED)",
                b.lirBlockV, outFn.symbol.v);
            reporter.report(std::move(d));
            return std::nullopt;
        }
    }

    // c78 (D-CSUBSET-FLOAT-NEG-ENCODING): materialize each x86-style float-negate
    // sign-mask the LIR lowerer recorded. Each is a 16-byte, 16-byte-aligned
    // `.rodata` item whose low bytes carry the sign bit (bit 63 for F64 / bit 31
    // for F32) and whose high bytes are zero — bit-identical to gcc's `.LC0`
    // (F64: 00 00 00 00 00 00 00 80  00×8) / `.LC1` (F32: 00 00 00 80  00×12).
    // The `xorpd/xorps xmm, [rip+mask]` memory operand MUST be 16-byte aligned at
    // runtime; the 16-byte `Alignment` + the section-alignment layout (ELF
    // sh_addralign; PE 4 KiB sectionAlignment) guarantee it. NO relocations (a
    // pure constant) — CONST → `.rodata` (read-only; a store would never occur).
    for (auto const& mask : lir.signMaskConstants) {
        AssembledData m;
        m.symbol    = mask.symbol;
        m.section   = DataSectionKind::Rodata;
        m.alignment = Alignment::ofRuntimePow2(16);
        m.bytes.assign(16u, std::uint8_t{0});
        if (mask.isF64) {
            m.bytes[7] = 0x80u;   // low qword = 0x8000000000000000 (bit 63)
        } else {
            m.bytes[3] = 0x80u;   // low dword = 0x80000000 (bit 31)
        }
        assembled.dataItems.push_back(std::move(m));
    }

    // c116 (D-WIN64-SEH-FUNCLETS): bind each SEH scope descriptor to its owning
    // function's `sehScopes`. Runs AFTER the CFI production (whose
    // `prologueEndPc` the pe writer needs to host a scope table) AND after
    // assemble() (which populated each
    // function's `blockByteOffsets`) — the same ordering the c70 jump-table binding
    // relies on. Translates the descriptor's LIR block ids to byte offsets within
    // the parent function; the pe writer resolves the funclet + personality symbols
    // to image-RVAs and emits the __C_specific_handler scope table + EHANDLER.
    for (auto const& desc : lir.sehScopeDescriptors) {
        if (desc.funcIndex >= assembled.functions.size()) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::K_NoMatchingObjectFormat;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "SEH scope descriptor names function index {} but the assembled "
                "module has {} function(s) (D-WIN64-SEH-FUNCLETS)",
                desc.funcIndex, assembled.functions.size());
            reporter.report(std::move(d));
            return std::nullopt;
        }
        AssembledFunction& outFn = assembled.functions[desc.funcIndex];
        if (!outFn.cfi.has_value()) {
            // A SEH-guarding function ALWAYS has a frame, so it always produced
            // CFI. A missing one means the production was skipped — fail loud,
            // never emit a dangling scope table with no UNWIND_INFO to host it.
            ParseDiagnostic d;
            d.code     = DiagnosticCode::K_NoMatchingObjectFormat;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "SEH scope on function index {} has no call-frame information to "
                "host its scope table (D-WIN64-SEH-FUNCLETS)", desc.funcIndex);
            reporter.report(std::move(d));
            return std::nullopt;
        }
        auto beginIt = outFn.blockByteOffsets.find(desc.beginLirBlockV);
        auto endIt   = outFn.blockByteOffsets.find(desc.endLirBlockV);
        auto handIt  = outFn.blockByteOffsets.find(desc.handlerLirBlockV);
        if (beginIt == outFn.blockByteOffsets.end()
            || endIt == outFn.blockByteOffsets.end()
            || handIt == outFn.blockByteOffsets.end()) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::K_NoMatchingObjectFormat;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "SEH scope on function index {} references a block with no byte "
                "offset (begin={}, end={}, handler={}) — malformed descriptor "
                "(D-WIN64-SEH-FUNCLETS)", desc.funcIndex, desc.beginLirBlockV,
                desc.endLirBlockV, desc.handlerLirBlockV);
            reporter.report(std::move(d));
            return std::nullopt;
        }
        // The guarded PC range's END = one-past the guarded body's LAST block =
        // the byte offset of whatever block is laid out immediately AFTER it. Block
        // LAYOUT order is NOT MIR creation order (the optimizer's mandatory prune
        // reorders by RPO), so compute it as the smallest block offset STRICTLY
        // GREATER than the guarded body's last-block offset — or the function's
        // total byte size if that block is laid out last. (c116a: the guarded body
        // is a single block, so `endLirBlockV == beginLirBlockV`.)
        std::uint32_t const lastBlockOff = endIt->second;
        std::uint32_t endByteOffset =
            static_cast<std::uint32_t>(outFn.bytes.size());
        for (auto const& [blkV, off] : outFn.blockByteOffsets) {
            (void)blkV;
            if (off > lastBlockOff && off < endByteOffset) endByteOffset = off;
        }
        SehScopeEntry e;
        e.beginByteOffset      = beginIt->second;
        e.endByteOffset        = endByteOffset;
        e.jumpTargetByteOffset = handIt->second;
        e.filterFuncletSymbol  = desc.filterFuncletSymbol;
        e.personalitySymbol    = desc.personalitySymbol;
        outFn.sehScopes.push_back(e);
    }

    // D-LK4-3: stamp the owning CompilationUnit's id so the linker keys this
    // module's symbols by `(cuId, SymbolId)`. Single-CU build → one cuId; a merged
    // whole-program image carries CU0's id (cosmetic — the merge already collapsed
    // every CU into one symbol space, so the linker receives a single module).
    assembled.cuId = cuId;

    // LK11a: build the per-module symbol table the linker matches by NAME across
    // CUs (cross-CU resolution + weak-vs-strong). One entry per DEFINED function /
    // global — extern imports are references, not definitions, and are carried
    // separately in `externImports`. The name comes from `nameOf` (raw declared
    // identifier, no mangling — the SemanticModel name for a CU, the merged symbol
    // name for a whole-program module); binding/visibility from MIR. IRs stay
    // numeric — the name is resolved here via `nameOf`, not threaded through MIR/LIR.
    // (Source/target/format-agnostic: reads `nameOf` + MIR linkage, no language/CPU/
    // format branch.)
    //
    // Cycle 25 Stage C — `nameOf` returns "" for a symbol with NO declared name. That
    // covers two cases, both module-private and SKIPPED here (no symbol-table entry):
    //   * a compiler-SYNTHESIZED symbol — e.g. a string-literal rodata global (minted
    //     ABOVE the semantic range per D-LK4-RODATA-PRODUCER-STRING) or a synthesized
    //     init thunk. Never referenced across CUs by name; resolved intra-module by id.
    //   * (single-CU) a SymbolId with no SemanticModel record at all — `nameOf`'s
    //     `recordFor(s) ? name : ""` returns "" exactly as the old `rec == nullptr`
    //     skip did. Byte-identical: every REAL c func/global has a non-empty
    //     declared name, so only synthesized symbols hit the "" skip in the corpus.
    // (The pre-Cycle-25 monolith ALSO had an `empty-name && non-Local` fail-loud arm;
    // it required a symbol with a record but an empty name — a state the semantic
    // analyzer never produces, and indistinguishable from a synthesized symbol once
    // names flow through `nameOf`. The merged module legitimately carries empty-named
    // externally-visible synthesized globals, so an empty name is no longer a breach.
    // The merge's own `MirVerifier` + `mergedSymbolOf` fail-louds guard merged-module
    // integrity in that arm's place.)
    {
        auto appendSym = [&](SymbolId sym, SymbolBinding bind,
                             SymbolVisibility vis) {
            std::string name = nameOf(sym);
            if (name.empty()) return;  // module-private (synthesized / no record)
            assembled.symbols.push_back(
                ModuleSymbol{sym, std::move(name), bind, vis});
        };
        for (std::uint32_t i = 0; i < mir.moduleFuncCount(); ++i) {
            MirFuncId const fid = mir.funcAt(i);
            appendSym(mir.funcSymbol(fid), mir.funcBinding(fid),
                      mir.funcVisibility(fid));
        }
        for (std::uint32_t i = 0; i < mir.moduleGlobalCount(); ++i) {
            MirGlobalId const gid = mir.globalAt(i);
            appendSym(mir.globalSymbol(gid), mir.globalBinding(gid),
                      mir.globalVisibility(gid));
        }
    }

    // Assembly complete — return the per-CU module; linking + writing is the shared
    // `linkAndWrite` phase below, so N CUs can each assemble before one merged link.
    return assembled;
}

// ── D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE: program-entry resolution ──────────────
//
// See `resolveProgramEntry`'s docblock in the header for the contract and for why
// ONE owner replaced the two disagreeing scans that were here before.

void collectEntryCandidates(SemanticModel const& model,
                            std::vector<EntryCandidate>& out,
                            std::vector<std::uint32_t>& outSymbolIndex) {
    // ★ BY REFERENCE. `symbols()` returns `std::vector<SymbolRecord> const&`, so
    // `auto const` (no `&`) silently COPIES every record — the measured dangling-view
    // defect described on `EntryCandidate::name`. The copy is also pure waste on a
    // hot path.
    auto const& recs = model.symbols();
    for (std::size_t i = 0; i < recs.size(); ++i) {
        auto const& rec = recs[i];
        if (rec.kind != DeclarationKind::Function) continue;
        // The semantic tier stamps `entryVerb` ONLY on a function DEFINITION whose
        // name the language declares as an entry spelling AND whose signature
        // matched one of that name's declared rows. So this one test replaces the
        // old name-list scan entirely — and it is strictly narrower in two ways
        // that both matter: a PROTOTYPE `int main(int, char**);` no longer makes a
        // TU look like it has an entry (gcc: `undefined reference to 'main'`), and
        // a definition with a bad signature was already refused at its declarator
        // instead of arriving here to be mis-selected.
        if (!rec.entryVerb.has_value()) continue;
        out.push_back(EntryCandidate{std::string{rec.name}, rec.entryVerb});
        outSymbolIndex.push_back(static_cast<std::uint32_t>(i));
    }
}

std::optional<ResolvedEntry>
resolveProgramEntry(std::span<EntryCandidate const>       candidates,
                    std::span<EntryMaterialization const> formatVerbs,
                    std::string_view                      formatName,
                    DiagnosticReporter&                   reporter,
                    bool&                                 ok) {
    ok = true;

    // ── THE "THIS FORMAT STARTS NO PROGRAM" ARM ────────────────────────────
    //
    // An EMPTY declared verb set is a DECLARED ANSWER, not a missing declaration:
    // `ObjectFormatData::validate()` pins `entryVerbs` non-empty ⟺ exec-flavored
    // in BOTH directions, so empty is exactly the relocatable / staticlib /
    // entry-less-library case. Such a build resolves its entry by NAME with no
    // verb requirement and demands nothing — `main` in a `.o` is resolved if
    // present (a later linker decides what starts) and is otherwise an ordinary
    // global function.
    //
    // ⚠ THIS IS NOT A FORMAT CHECK, and the distinction is load-bearing. The
    // predicate is the DECLARED SET being empty — never `isExecFlavor()`, never a
    // format name. MEASURED when the predecessor gate was built: without this
    // arm, `dss --target x86_64:elf64-x86_64-linux` (the RELOCATABLE format)
    // refused `int main(void){return 0;}`, i.e. every object-file build in the
    // tree.
    if (formatVerbs.empty()) {
        if (candidates.empty()) return std::nullopt;
        return ResolvedEntry{0, EntryMaterialization::None};
    }

    std::vector<std::size_t> realizable;
    std::vector<std::size_t> nearMiss;   // an entry name whose verb this format cannot realize
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!candidates[i].verb.has_value()) continue;
        bool found = false;
        for (auto const v : formatVerbs) {
            if (v == *candidates[i].verb) { found = true; break; }
        }
        (found ? realizable : nearMiss).push_back(i);
    }

    // ── EXACTLY ONE: the normal path ───────────────────────────────────────
    if (realizable.size() == 1) {
        auto const i = realizable[0];
        return ResolvedEntry{i, *candidates[i].verb};
    }

    // ── ZERO: no entry this format can start ───────────────────────────────
    if (realizable.empty()) {
        // ★ NAME THE NEAR-MISSES. This is the entire reason this diagnostic
        // exists rather than letting the link tier's `K_SymbolUndefined` speak.
        // MEASURED 2026-08-10 on HEAD `3e86a187`: a `wmain`-only source built for
        // `elf64-x86_64-linux-exec` was told that `wmain` WAS the Linux entry and
        // that the remedy was an ELF config row to make it one. The honest report
        // is that the program defines no entry this format can start, and WHY.
        std::string detail;
        for (auto const i : nearMiss) {
            if (!detail.empty()) detail += "; ";
            detail += std::format("'{}' is defined here but needs the '{}' "
                                  "materialization verb, which this format does "
                                  "not realize",
                                  candidates[i].name,
                                  entryMaterializationName(*candidates[i].verb));
        }
        std::string declared;
        for (auto const v : formatVerbs) {
            if (!declared.empty()) declared += ", ";
            declared += entryMaterializationName(v);
        }
        ParseDiagnostic d;
        d.code     = DiagnosticCode::K_ProgramEntryUndefined;
        d.severity = DiagnosticSeverity::Error;
        d.actual = std::format(
            "no program entry is defined. Object format '{}' STARTS A PROGRAM, so "
            "the build needs exactly one function that is both an entry spelling "
            "the source language declares and realizable by this format; it found "
            "none. This format realizes these materialization verbs: {}. {} The "
            "accepted entry set is the INTERSECTION of the language's "
            "`entryFunctions` mapping and this format's `entryVerbs`, so a "
            "definition can be a valid entry on one target and an ordinary "
            "function on another — that is by design, not a bug. "
            "(D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE.)",
            formatName, declared,
            detail.empty()
                ? std::string{"No entry-named function is DEFINED in this build "
                              "at all (a prototype without a body defines "
                              "nothing to run)."}
                : std::format("Near miss: {}.", detail));
        reporter.report(std::move(d));
        ok = false;
        return std::nullopt;
    }

    // ── MORE THAN ONE: refuse, never pick ──────────────────────────────────
    //
    // ★ THE PREDECESSORS OF THIS BRANCH WERE BOTH WRONG, IN DIFFERENT WAYS. The
    // single-CU scan refused with `K_SymbolUndefined` — a code about a symbol that
    // does not exist, for a condition where two do — citing
    // `D-CSUBSET-MULTI-FN-WIN64-CC`, an anchor about calling conventions. The
    // merged scan did not refuse at all: it took the first matching name it walked
    // past, so `main` in a.c and `wmain` in b.c silently picked one.
    std::string list;
    for (auto const i : realizable) {
        if (!list.empty()) list += ", ";
        list += std::format("'{}' [{}]", candidates[i].name,
                            entryMaterializationName(*candidates[i].verb));
    }
    ParseDiagnostic d;
    d.code     = DiagnosticCode::K_ProgramEntryAmbiguous;
    d.severity = DiagnosticSeverity::Error;
    d.actual = std::format(
        "ambiguous program entry: {} functions are BOTH entry spellings this "
        "source language declares AND realizable by object format '{}' ({}). A "
        "program has exactly one entry and this compiler will not choose for you "
        "— picking one silently is how a build runs the wrong code while "
        "reporting success. Define one, or build for a target that realizes only "
        "one of their verbs (the same source resolves cleanly wherever only one "
        "of these verbs is realized). (D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE.)",
        realizable.size(), formatName, list);
    reporter.report(std::move(d));
    ok = false;
    return std::nullopt;
}

// LOWER half (single-CU): thin wrapper over the shared `lowerMirModuleToAssembly`.
// Binds the seam state from the `CuMirModule`: the per-CU type interner, a
// `nameOf` that reads the SemanticModel's symbol records, the CU's extern imports,
// and the entry symbol resolved by the CU-specific scan above. Produces output
// byte-identical to the pre-Cycle-25 monolith for any single-CU build.
std::optional<AssembledModule>
lowerCuMirToAssembly(CuMirModule&                       cuMir,
                     std::optional<ProcessArgs> const& processArgs,
                     std::span<EntryMaterialization const> entryVerbs,
                     std::optional<SehPersonality> const& sehPersonality,
                     std::string_view                  formatName,
                     std::string_view                  wideFloatSoftcallLibrary,
                     DiagnosticReporter&               reporter) {
    SemanticModel&       model   = cuMir.model;
    GrammarSchema const& grammar = *cuMir.grammar;

    // Resolve the program entry FIRST so a multi-entry or entry-less source halts
    // before lowering — same observable failure point as pre-Cycle-25. Non-const:
    // `realizeEntryShape` may retarget it.
    //
    // Both driver paths call the SAME `resolveProgramEntry`; see its docblock for
    // why that had to become single-owner.
    std::vector<EntryCandidate> cands;
    std::vector<std::uint32_t>  candSym;
    collectEntryCandidates(model, cands, candSym);
    bool entryOk = true;
    auto const resolved = resolveProgramEntry(cands, entryVerbs, formatName,
                                              reporter, entryOk);
    if (!entryOk) return std::nullopt;

    std::optional<SymbolId> userEntry;
    EntryMaterialization    entryVerb = EntryMaterialization::None;
    if (resolved.has_value()) {
        // `collectEntryCandidates` records each candidate's index in
        // `model.symbols()`, which IS its SymbolId on this path (the same identity
        // the pre-existing scan minted).
        userEntry = SymbolId{candSym[resolved->index]};
        entryVerb = resolved->verb;
    }

    // UCRT-P4 (D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE + D-FFI-PE-CRT-UCRT-MIGRATION):
    // single-CU counterpart of the merge-path call (program.cpp). MATERIALIZE the
    // resolved entry's arguments per its verb × the format's declared mechanism (on
    // the CRT-accessor route this appends the pre-main init and retargets
    // `userEntry` to it). The signature GATE is no longer here or anywhere in the
    // back half: the semantic tier owns it, with a source span. The CU is already
    // per-CU-optimized here, so an appended init skips the optimizer but is lowered
    // like any other function; the interner is the CU model's (the type space this
    // CU's TypeIds index into).
    if (!realizeEntryShape(cuMir.mir, model.lattice().interner(),
                           userEntry, cuMir.externImports,
                           entryVerb, processArgs, cuMir.cSymbolDecoration,
                           formatName, reporter)) {
        return std::nullopt;  // unusable mechanism — fail-loud already reported.
    }

    // D-FFI-PE-CRT-UCRT-MIGRATION (Phase 3): PARTITION `cuMir.libraryShimRecipes` (the
    // single combined synthesize-recipe map CST→HIR seeded, carrying BOTH families) by
    // `dss::ffi::shimFamilyOf` BEFORE calling EITHER synth
    // pass. Each pass fails loud on a recipe id it has no switch arm for (its own anti-
    // vocab-drift backstop), so handing the WHOLE map to one pass would make it reject the
    // other family's ids and abort a build that never should have failed. A `nullopt`
    // family here is an INTERNAL INVARIANT BREACH, not a user error: the descriptor loader
    // (`readShippedLibDescriptor`) already rejects an unknown `synthesize` id at READ time
    // via the same closed-vocab table `shimFamilyOf` reads, so a recipe reaching this point
    // with no family means the loader and this switch have drifted out of lockstep. It is
    // reported with the DRIVER-band internal-invariant code `D_SynthRecipeFamilyUnknown`
    // (the `D_CompileUnitNullNoDiagnostic` class) — never the linker's
    // `K_NoMatchingObjectFormat`, which would point an operator at the object-format config
    // for what is a recipe-table defect. The merged-module seam (program.cpp) emits the
    // SAME code for the SAME breach.
    std::unordered_map<std::uint32_t, std::string> threadsRecipes, stdioRecipes;
    for (auto const& [symV, recipe] : cuMir.libraryShimRecipes) {
        auto const family = dss::ffi::shimFamilyOf(recipe);
        if (!family.has_value()) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::D_SynthRecipeFamilyUnknown;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "synthesize recipe '{}' (symbol {{ {} }}) belongs to no known shim "
                "family (D-FFI-PE-CRT-UCRT-MIGRATION) — internal invariant breach: the "
                "descriptor loader should have rejected an unknown recipe id at read "
                "time (isKnownSynthesizeRecipe)",
                recipe, symV);
            reporter.report(std::move(d));
            return std::nullopt;
        }
        switch (*family) {
        case dss::ffi::ShimFamily::Threads: threadsRecipes.emplace(symV, recipe); break;
        case dss::ffi::ShimFamily::Stdio:   stdioRecipes.emplace(symV, recipe);   break;
        }
    }

    // FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER / D-CSUBSET-C11-THREADS-MACHO): single-CU
    // counterpart of the merge-path shim synth (program.cpp). Supply a definition for every
    // <threads.h> shim symbol the descriptor tagged (mtx_lock etc.) over the format's synth
    // vehicle (pe→kernel32, macho→pthread); a clean no-op when `threadsRecipes` is empty
    // (every elf + non-threads TU). Same seam as synthesizePeStartup (the CU is per-CU-
    // optimized; the appended shims are lowered like any other function). The interner is
    // the CU model's; the vehicle comes from `cuMir.librarySynthesis`.
    if (!synthesizeThreadsShim(cuMir.mir, model.lattice().interner(),
                               threadsRecipes, cuMir.librarySynthesis,
                               cuMir.cSymbolDecoration, cuMir.externImports,
                               reporter)) {
        return std::nullopt;  // internal invariant breach (vocab/switch drift) — reported.
    }

    // D-FFI-PE-CRT-UCRT-MIGRATION (Phase 3): the <stdio.h> printf-family shim sibling — see
    // `synth_stdio_shim.hpp` for the full contract. A clean no-op when `stdioRecipes` is
    // empty (every elf/macho build and every pe TU that includes no <stdio.h> printf
    // family). `cuMir.vaListLayout` is the RESOLVED CC's WHOLE va_list block (captured at
    // BUILD time in `buildCuMirImpl`, above) — the shim's variadic-forwarding arm reads
    // `.strategy` to take the HomogeneousPointer arm (or fail loud on a model it has no arm
    // for) and `.variadicUsesOverflowBase` to pick the va leaf WITHIN it.
    if (!synthesizeStdioShim(cuMir.mir, model.lattice().interner(),
                             stdioRecipes, cuMir.vaListLayout,
                             cuMir.externImports, reporter)) {
        return std::nullopt;  // recipe/helper-import/va-strategy mismatch — reported.
    }

    // ══ VERIFY THE POST-SYNTHESIS MODULE (UCRT-P4) ═══════════════════════════
    //
    // ★ THE HOLE THIS CLOSES, AND HOW IT WAS FOUND. TF-C112 advertised MIR
    // call-site signature checking as covering "wrong arity at every hand-built
    // call in every synthesis pass". MEASURED that it did not: a 3-parameter
    // `int main(int, char**, char**)` compiled rc=0 while the synthesized startup
    // called it with TWO arguments. The verifier's arity rule
    // (`I_CallSignatureMismatch`) is fully CAPABLE of catching that — it reads the
    // callee's FnSig straight off the `GlobalAddr`'s own type, needs no definition
    // and no symbol table — so the defect was pure COVERAGE: on the single-CU path
    // the LAST verify happens inside `optimizeModule` during the BUILD half, and
    // every synthesis pass runs afterwards in this LOWER half, unverified.
    //
    // ★ POSITION IS THE WHOLE DESIGN, and it MIRRORS THE MERGED PATH EXACTLY.
    // On the N>1 path `program.cpp` runs `realizeEntryShape` → threads → stdio and
    // THEN `optimizeModule`, whose verify covers all three; `synthesizeSehFunclets`
    // runs after it and is uncovered there too. Placing this verify at the same
    // point makes the two seams AGREE on what is verified instead of one silently
    // checking less than the other (`D-MIR-SYNTH-SHIM-SEAM-OPTIMIZE-PLACEMENT-ASYMMETRY`).
    //
    // ⚠ IT DELIBERATELY PRECEDES `synthesizeSehFunclets`, AND THAT RESIDUE IS
    // STATED, NOT HIDDEN. That pass RELAYOUTS parent blocks to make each `__try`
    // body PC-contiguous and does not re-derive the StructCf markers the verifier
    // compares (its three siblings all do, at their own sites). Verifying after it
    // would therefore red on the marker equality and the layout-position rules for
    // reasons that are the PASS's to fix, inside `src/mir/merge/`. Extending
    // coverage over the SEH pass is its own change; what must not happen is this
    // verify being dropped because that one is harder.
    {
        MirVerifier verifier{cuMir.mir, &model.lattice().interner()};
        if (!verifier.verify(reporter)) {
            // The verifier already reported the specific broken invariant (with the
            // offending instruction); this names the TIER so the reader knows a
            // SYNTHESIS pass produced it rather than the optimizer or the front end.
            ParseDiagnostic d;
            d.code     = DiagnosticCode::I_VerifierFailure;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = "the module failed MIR verification AFTER the synthesis "
                         "passes (entry realization / threads shim / stdio shim) — "
                         "a synthesized body broke a structural, SSA or call-"
                         "signature invariant. This is a compiler defect, never a "
                         "program error.";
            reporter.report(std::move(d));
            return std::nullopt;
        }
    }

    // c116 (D-WIN64-SEH-FUNCLETS): synthesize the SEH filter funclets + record the
    // scope ranges (post-optimize; the CU is already optimized here). Trigger =
    // presence of SehTryBegin — a no-op fast-return for the overwhelming majority
    // of TUs. Appends the __C_specific_handler personality import on demand.
    // UCRT-P4: the personality routine + its image are now the FORMAT's declared
    // `sehPersonality` block instead of two literals inside the pass. A format that
    // declares none fails loud HERE (only when a region actually resolved), which is
    // what an ELF/Mach-O build carrying `__try` should get instead of an msvcrt
    // import planted in a non-Windows image.
    std::vector<MirSehScope> sehScopes;
    if (!synthesizeSehFunclets(cuMir.mir, model.lattice().interner(),
                               cuMir.externImports, sehPersonality,
                               cuMir.cSymbolDecoration, formatName,
                               sehScopes, reporter)) {
        return std::nullopt;  // unsupported SEH shape (c116b frontier) / no declared
                              // personality — fail-loud.
    }

    // `nameOf`: SymbolId → the on-binary symbol name = the declared name run
    // through the FORMAT'S C mangling (`applyCMangling`: identity on ELF/PE, a
    // leading `_` on Mach-O). D-LK-OBJECT-EXTERN-SYMBOL-NAMES: this makes the
    // single-CU `ModuleSymbol.name` the SAME pre-mangled on-binary form the
    // merge path already stores (program.cpp, via D-LK-MACHO-CROSSCU-MANGLE-MERGE-KEY
    // / c118), so the object writers emit it VERBATIM with no per-
    // path divergence and no double-mangle. Identity on ELF/PE → byte-identical
    // to the pre-fix output; adds the `_` on Mach-O only. A SymbolId with no
    // record (synthesized / out-of-range) yields "" — the LK11a symbol-table
    // populate then skips it as module-private, exactly as before.
    // TF-C88 (D-CSUBSET-ASM-LABEL-SYMBOL-RENAME): routed through `linkNameFor`,
    // which returns an
    // explicit assembler name VERBATIM and falls back to `applyCMangling` when
    // there is none — so this lambda is byte-identical for every symbol without a
    // label (i.e. every symbol in every program before this cycle). This is the
    // DEFINITION rail; `program.cpp`'s merge-key lambda is its cross-CU twin and
    // MUST route through the same function (see linkNameFor's own comment for what
    // a divergence between the two silently produces).
    // TF-C121 (D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME): `linkName` is passed
    // EXPLICITLY (the parameter is required, not defaulted) so this rail and the
    // import rail hand `linkNameFor` the same four inputs. The semantic injector
    // writes the identical string onto BOTH the SymbolRecord read here and the
    // ShippedExternSymbol that becomes the import row.
    auto nameOf = [&](SymbolId s) -> std::string {
        SymbolRecord const* r = model.recordFor(s);
        return r ? dss::ffi::linkNameFor(r->name, r->asmName,
                                         cuMir.cSymbolDecoration, r->linkName)
                 : std::string{};
    };

    // D-CSUBSET-LONG-DOUBLE-IEEE128-ARITH (LD-2): the ACTIVE format's F128
    // softcall runtime library. Empty = the format declares none (nullopt → an
    // F128 softcall fails loud).
    //
    // ★ D-PROGRAM-TIER-RETAINS-FORMAT-IDENTITY-BRANCHES (the residual half):
    // THIS FUNCTION USED TO TAKE THE FORMAT'S *KIND* AND DO THE LOOKUP ITSELF.
    // It now takes the RESOLVED LIBRARY — the fact — which is the same
    // decomposition every other parameter here already follows: `processArgs`,
    // `entryVerbs` and `sehPersonality` are all facts READ OFF the format by
    // the caller rather than an identity this function re-interrogates. The
    // caller holds the schema handle and is the right place to ask it; the
    // lowering has no business knowing which FAMILY of object format it is
    // serving, only what that family declared.
    std::optional<std::string> wideFloatSoftcallLibraryOpt =
        wideFloatSoftcallLibrary.empty()
            ? std::nullopt
            : std::optional<std::string>(
                  std::string(wideFloatSoftcallLibrary));

    return lowerMirModuleToAssembly(
        cuMir.mir, model.lattice().interner(), nameOf,
        std::move(cuMir.externImports), userEntry, *cuMir.target,
        cuMir.dataModel, cuMir.bitFieldStrategy, cuMir.unnamedBitFieldAlignment,
        cuMir.callingConventionIndex, cuMir.cuId,
        cuMir.externCallDispatch, cuMir.dataImportBinding,
        cuMir.externAddrBinding,
        cuMir.tlsAccess,
        std::move(sehScopes), std::move(wideFloatSoftcallLibraryOpt), reporter);
}

// LOWER half (merged whole-program): thin wrapper over the shared
// `lowerMirModuleToAssembly` for the N>1 merge path. `mergeCuMirs` already unified
// the N CUs into ONE module over a host lattice, resolved cross-CU calls to DIRECT
// intra-module calls (stripping the resolved extern imports), and computed the
// user-entry symbol. This drives that single module through the same LOWER body,
// so the linker downstream receives exactly ONE AssembledModule (no assembled-tier
// cross-CU resolution — the linker's dispatch-keyed direct-bind / thunk-slot
// machinery, c154, only runs for N>1 pre-assembled modules).
//
// `merged` is taken by non-const ref because the shared body needs a mutable `Mir&`
// (MIR→LIR may intern lowered-expression types into the host) + the surviving
// externImports are MOVED into MIR→LIR. The host lattice's interner is the type
// space for the merged module's TypeIds. `cuId` is CU0's (the merge stamped CU0's
// symbol values preferentially; cosmetic, since the linker gets one module).
std::optional<AssembledModule>
lowerMergedToAssembly(MergedMirModule&    merged,
                      GrammarSchema const& /*grammar*/,
                      TargetSchema const& target,
                      DataModel           dataModel,
                      BitFieldStrategy    bitFieldStrategy,
                      UnnamedBitFieldAlignment unnamedBitFieldAlignment,
                      std::uint16_t       callingConventionIndex,
                      CompilationUnitId   cuId,
                      std::optional<ExternCallDispatch> externCallDispatch,
                      std::optional<DataImportBinding> dataImportBinding,
                      // D-LK-ARM64-EXTERN-DATA-ADDR-PIE-GOT (TF-C52): the
                      // format's extern-ADDRESS binding, pre-resolved one
                      // level up in program.cpp (same shape as
                      // externCallDispatch / dataImportBinding).
                      std::optional<ExternAddrBinding> externAddrBinding,
                      std::optional<TlsAccessInfo> tlsAccess,
                      std::vector<MirSehScope> sehScopes,
                      // D-CSUBSET-LONG-DOUBLE-IEEE128-ARITH (LD-2): the F128
                      // softcall runtime library, pre-resolved one level up in
                      // program.cpp (no ObjectFormatKind in scope here — the
                      // merge path resolves it near **formatR, exactly as
                      // externCallDispatch is pre-resolved there).
                      std::optional<std::string> wideFloatSoftcallLibrary,
                      DiagnosticReporter& reporter) {
    // `nameOf`: merged SymbolId → declared name from the merge's `symbolNames` map.
    // A synthesized / nameless merged symbol is absent from the map → "" → skipped
    // by the LK11a symbol-table populate (module-private), exactly as in the CU path.
    auto nameOf = [&](SymbolId s) -> std::string {
        auto const it = merged.symbolNames.find(s.v);
        return it != merged.symbolNames.end() ? it->second : std::string{};
    };

    return lowerMirModuleToAssembly(
        merged.mir, merged.host.interner(), nameOf,
        std::move(merged.externImports), merged.userEntrySymbol, target,
        dataModel, bitFieldStrategy, unnamedBitFieldAlignment,
        callingConventionIndex, cuId,
        externCallDispatch, dataImportBinding, externAddrBinding, tlsAccess,
        std::move(sehScopes), std::move(wideFloatSoftcallLibrary), reporter);
}

// Link N assembled CUs into one image + commit to disk. N==1 is the v1 single-CU
// path; N>1 the linker merges the CUs (LK11a resolution + LK11b byte emission)
// before the format walker emits. `outPath` is caller-owned.
bool linkAndWrite(std::span<AssembledModule const> modules,
                  TargetSchema const&              target,
                  ObjectFormatSchema const&        format,
                  std::filesystem::path const&     outPath,
                  DiagnosticReporter&              reporter,
                  ImageRequest const&              request) {
    // c97: link phase — resolution + byte emission + image write.
    substrate::PhaseTimers::Scope linkPhase{substrate::CompilePhase::Link};
    auto const linkEntry = reporter.errorCount();
    auto image = linker::link(modules, target, format, reporter, request);
    if (!image.ok() || !tierClean(reporter, linkEntry)) {
        return false;
    }
    // D-OUTPUT-EXEC-BIT: mark the file executable iff the active object
    // format is an exec/image flavor (config-driven via the schema predicate,
    // never an arch/format identity branch) so a produced binary runs
    // directly without a manual `chmod +x`.
    return linker::writeImage(image, outPath, reporter, format.isImageFlavor());
}

// -- c165 (D-LK-STATIC-LINK): STATIC linking against `ar` archives --------------

namespace {

// The 8-byte GNU/SysV `ar` global magic ("!<arch>\n"). The ar-vs-dynamic
// `--resolve-library` dispatch keys on THIS (magic bytes, agnostic -- never a
// `.a` extension), mirroring `ffi::guessFormat`'s Ar arm. Kept local: `ffi`'s
// `reader_common.hpp` is an internal header the program tier does not include,
// and re-stating 8 bytes is cheaper than a new cross-layer include/export.
constexpr std::uint8_t kArGlobalMagic[8] = {'!', '<', 'a', 'r', 'c', 'h', '>', 0x0Au};

// ── D-LK-ARCHIVE-MEMBER-EXTERN-LOSES-ITS-LIBRARY ─────────────────────────────
//
// ★★★ THE PLATFORM CORPUS, ASKED BY THE **ON-BINARY** SYMBOL NAME.
//
// `ffi::realizeShippedExternSymbols` is keyed on the CANONICAL C IDENTIFIER,
// because its first caller was the semantic tier, which is holding a C
// declaration. TWO producers in this file are not: an assembly unit writes the
// on-binary symbol itself (`call _puts`), and an ARCHIVE MEMBER is already-
// compiled object code whose symbol table records whatever the compiler emitted.
// Both therefore have to reach the same corpus rows through the same query, and
// this is that query — one owner of "what does the platform say about this
// on-binary name here", so the two producers cannot drift on the answer.
//
// ★★★ AND THE INVERSE OF THE DECORATION IS NOT ENOUGH ON ITS OWN — ✔MEASURED,
// AND IT IS THE HALF THE OBVIOUS FIX MISSES. Un-decorating the written name
// recovers the C identifier ONLY for a row the platform exports under its own
// name. A row carrying `linkName` does not: pe's `<time.h>` `time` is exported as
// `_time64`, `<io.h>`'s whole family as `_open`/`_close`/`_read`/`_write`/…,
// Darwin x86_64's `opendir`/`readdir` as `opendir$INODE64`/`readdir$INODE64`,
// `realpath` as `realpath$DARWIN_EXTSN`, and pe `_setjmp` as
// `__intrinsic_setjmp`. A DSS-compiled TU applies `linkNameFor` BEFORE it emits,
// so `#include <time.h>` + `time(0)` leaves `_time64` in the object's symbol
// table — and `_time64` is not a corpus key, so a canonical-name-only lookup
// misses EVERY one of those rows. ✔MEASURED (shipped CLI, pe64-x86_64, this
// host): the member's undefined symbol is `_time64` and the exec link rejected
// it with `K_SymbolUndefined`, i.e. an ordinary `#include <time.h>` in an
// archived TU did not link. So the lookup runs FORWARD first and falls back to a
// REVERSE index keyed by each row's REALIZED link name.
//
// ★★ FORWARD FIRST, AND THE ORDER IS POLICY-BEARING RATHER THAN AN OPTIMIZATION.
// A name found FORWARD is reported under the canonical identifier the caller
// un-decorated, so a caller that refuses a `linkName` row reached by its C
// spelling (the `.s` author who wrote `call time` where the platform exports
// `_time64`) still sees exactly the row it has always seen, and its refusal is
// unchanged. A name found only in REVERSE is reported under the ROW's own C
// identifier, so `linkNameFor` on it reproduces the written name by construction
// and the same refusal correctly stays silent. One lookup, both verdicts, no
// second policy.
//
// ★★ AMBIGUITY IS REFUSED, NEVER FIRST-MATCHED. The reverse index is built by
// walking a hash map, so a name two rows both realize to would otherwise be
// decided by hash order — the "decides the answer by filesystem" hazard this
// repo has already paid for. Two rows realizing to one on-binary name are
// recorded as AMBIGUOUS and yield NO answer, which routes the reference unbound
// to the link tier exactly as an unknown name does.
struct PlatformExternRealization {
    // The corpus row's own C identifier — what `linkNameFor` must be applied to
    // in order to reproduce the on-binary name. NOT necessarily the caller's
    // un-decorated spelling (see the FORWARD/REVERSE note above).
    std::string                   canonicalName;
    ffi::ShippedSymbolRealization row;
};

// Realize `onBinaryNames` against the shipped-descriptor corpus for the ACTIVE
// (target, format). Returns one entry per name the corpus answers for; a name
// with no row, no availability here, or an AMBIGUOUS reverse match is simply
// ABSENT (the caller routes it unbound and the LINK tier judges the reference —
// C23 5.1.1.2 phase 8). Returns nullopt IFF the corpus directory could not be
// located, which is a statement about the ENVIRONMENT and never about the user's
// program: the caller must then behave exactly as it did before this oracle
// existed. `latticeOwner` / `latticeLabel` name the throwaway lattice the
// descriptor decode needs; nothing reads the resulting TypeIds here.
//
// ── `reporter` — D-FFI-DUPLICATE-SYMBOL-ACROSS-DESCRIPTORS-SILENTLY-ORDER-RESOLVED
//
// ★★ IT IS THE REAL REPORTER, NOT A THROWAWAY, AND THE DISTINCTION IS THE FIX.
// Every OTHER descriptor fault this path meets belongs to a descriptor the user
// never asked for, so it is deliberately swallowed (see the skip rationale in the
// oracle's header). A cross-descriptor realization DISAGREEMENT is the opposite
// kind of fault: it is not "some unrelated file is malformed", it is "the corpus
// gives TWO answers for a name THIS BUILD IS BINDING and the tie-break is a path
// sort". Swallowing it is exactly the silence being deleted, so it travels on the
// build's own reporter.
//
// ⓘ THE CALLER MUST REFUSE — the conflict is NOT in the return value. `nullopt`
// keeps its single meaning ("the corpus directory could not be located", benign,
// route unbound); a conflict is signalled by `reporter.errorCount()` having
// moved, which is the `tierClean` idiom used throughout this file.
[[nodiscard]] std::optional<std::unordered_map<std::string, PlatformExternRealization>>
realizePlatformExternsByOnBinaryName(std::span<std::string const> onBinaryNames,
                                     TargetSchema const&          target,
                                     ObjectFormatSchema const&    format,
                                     CompilationUnitId            latticeOwner,
                                     std::string_view             latticeLabel,
                                     DiagnosticReporter&          reporter) {
    std::unordered_map<std::string, PlatformExternRealization> out;
    if (onBinaryNames.empty()) return out;

    auto const scheme = format.cSymbolDecoration().scheme;

    // The oracle interns each row's declared signature, so it needs a lattice.
    // Neither of this file's on-binary-name producers has a `SemanticModel` (the
    // `encode` tier's absence of one IS the tier; an archive member never had
    // one in this process at all), so one is built here, scoped to this call: a
    // throwaway interner keeps the descriptor read byte-for-byte the one the
    // `#include` path performs rather than inventing a second, signature-free
    // reader.
    TypeLattice lattice{latticeOwner, std::string{latticeLabel}};

    // ★★★ `va_list` IS A REQUIRED PART OF THE CORPUS'S TYPE VOCABULARY, AND
    // OMITTING IT SILENTLY LOSES A WHOLE DESCRIPTOR. ✔MEASURED: without this
    // binding, `stdio.json` fails to decode (`vfprintf` and friends spell
    // `va_list`, which no descriptor can declare — it is compiler-provided),
    // `realizeShippedExternSymbols` SKIPS the unreadable descriptor by design,
    // and every stdio name comes back absent ⇒ `Unknown` ⇒ unbound. So a `.s`
    // calling `abort` (stdlib.json, no va_list anywhere) linked while one
    // calling `putchar` did NOT — a per-descriptor split with no diagnostic
    // between them. The corpus-wide decode test binds it for exactly this
    // reason (`sysvVaListBinding`, tests/ffi).
    //
    // ★★ AND IT IS DERIVED FROM THE ACTIVE ABI, NEVER PICKED. The three shapes
    // are the three `VaListStrategy` arms the semantic tier injects from the
    // SAME `cc->vaListLayout->strategy`, so the descriptor read here sees the
    // identical type the `#include` path would give it on this (target, format)
    // — Win64's `char*` is 8 bytes and SysV's `__va_list_tag[1]` is 24, and a
    // corpus row that ever gained a `signatureByDataModel` arm keyed on the
    // difference would otherwise decode one way here and another way there.
    // Hard-coding one arm would be a platform GUESS in shared substrate; asking
    // the ABI is a fact lookup, with no `if (arch)` and no `if (format)`.
    // ⓘ The resulting TypeId is not read by either caller. It is threaded so the
    // READ succeeds and so that, when a future consumer does read it, it is
    // already the right one rather than a placeholder someone must remember.
    std::array<NamedTypeBinding, 1> namedTypeStorage{};
    std::span<NamedTypeBinding const> namedTypes{};
    {
        DiagnosticReporter abiScratch;   // an ABI miss is not this tier's error
        auto const abi = ffi::resolveAbi(target, format, abiScratch);
        std::optional<VaListStrategy> strat;
        if (abi.has_value() && abi->cc != nullptr
            && abi->cc->vaListLayout.has_value()) {
            strat = abi->cc->vaListLayout->strategy;
        }
        auto& in = lattice.interner();
        TypeId vaListTy;
        if (strat == VaListStrategy::HomogeneousPointer) {
            vaListTy = in.pointer(in.primitive(TypeKind::I8));   // Win64: char*
        } else if (strat == VaListStrategy::Aapcs64DualCursor) {
            TypeId const voidPtr = in.pointer(in.primitive(TypeKind::Void));
            std::array<TypeId, 5> f{voidPtr, voidPtr, voidPtr,
                                    in.primitive(TypeKind::I32),
                                    in.primitive(TypeKind::I32)};
            vaListTy = in.structType("__va_list", f);
        } else {
            // SysVRegisterSave, and the nullopt default: a cc with no
            // `vaListLayout` has no variadic-callee surface at all, so the
            // SysV-family shape is inert there — the identical fallback the
            // semantic injector takes.
            TypeId const voidPtr = in.pointer(in.primitive(TypeKind::Void));
            std::array<TypeId, 4> f{in.primitive(TypeKind::U32),
                                    in.primitive(TypeKind::U32), voidPtr,
                                    voidPtr};
            vaListTy = in.array(in.structType("__va_list_tag", f), 1);
        }
        namedTypeStorage[0] = NamedTypeBinding{"va_list", vaListTy};
        namedTypes = std::span<NamedTypeBinding const>{namedTypeStorage.data(),
                                                       1};
    }

    // ── FORWARD: the caller's spelling, un-decorated ─────────────────────────
    //
    // ⚠ THE INVERSE IS CHECKED BY RE-APPLYING IT, WHICH IS NOT PEDANTRY —
    // ✔MEASURED, THE CONSERVATIVE INVERSE ALONE TURNED A BUILD ERROR INTO A LOAD
    // ERROR. `unapplyCMangling` passes an undecorated name THROUGH unchanged (its
    // documented, deliberately lenient contract), so a Mach-O source writing
    // `call putchar` — without the `_` that format requires — recovered the
    // canonical `putchar`, matched the corpus, and got BOUND to libSystem under a
    // spelling libSystem does not export. A "fix" that replaces a build error
    // with a dyld failure at process start is a regression, not a feature. So a
    // name is carried FORWARD only when re-decorating the recovered identifier
    // reproduces BYTE-FOR-BYTE what the producer wrote; anything else is not this
    // platform's C symbol under this format's own declared rule. No `_` literal
    // and no format name appears here — the rule is applied in both directions.
    std::vector<std::string> canonicalOf(onBinaryNames.size());
    std::vector<std::string> forwardRequest;
    forwardRequest.reserve(onBinaryNames.size());
    for (std::size_t i = 0; i < onBinaryNames.size(); ++i) {
        if (onBinaryNames[i].empty()) continue;
        std::string canonical = ffi::unapplyCMangling(onBinaryNames[i], scheme);
        if (ffi::applyCMangling(canonical, scheme) != onBinaryNames[i]) continue;
        canonicalOf[i] = std::move(canonical);
        forwardRequest.push_back(canonicalOf[i]);
    }

    if (!forwardRequest.empty()) {
        auto const realized = ffi::realizeShippedExternSymbols(
            forwardRequest, lattice.interner(), lattice.registry(), reporter,
            format.dataModel(), std::optional<std::string_view>{target.name()},
            format.kind(), namedTypes);
        if (!realized.has_value()) return std::nullopt;   // corpus not located
        for (std::size_t i = 0; i < onBinaryNames.size(); ++i) {
            if (canonicalOf[i].empty()) continue;
            auto const row = realized->find(canonicalOf[i]);
            if (row == realized->end()) continue;
            // Only a fully realized row is an ANSWER. `Unknown`,
            // `UnavailableForFormat` and `NoLibraryForFormat` all mean "the
            // platform states no image for this name here" ⇒ absent, so the
            // caller routes it unbound. Enumerated by the status check, never
            // fallen through.
            if (row->second.status != ffi::ShippedRealizationStatus::Realized) {
                continue;
            }
            out.emplace(onBinaryNames[i],
                        PlatformExternRealization{canonicalOf[i], row->second});
        }
    }

    // ── REVERSE: the corpus's own realized link names ────────────────────────
    //
    // Built ONLY for the names FORWARD could not answer, and only when there are
    // any: realizing the whole corpus decodes every descriptor's signatures, and
    // the ordinary case (every unbound name is a plain library export, or a
    // sibling symbol the caller already filtered out) never reaches this.
    std::vector<std::string> missing;
    for (auto const& name : onBinaryNames) {
        if (!name.empty() && out.find(name) == out.end()) missing.push_back(name);
    }
    if (missing.empty()) return out;

    auto const allNames = ffi::collectShippedExternSymbolFormats();
    if (!allNames.has_value()) return std::nullopt;   // corpus not located
    std::vector<std::string> everyName;
    everyName.reserve(allNames->size());
    for (auto const& [name, formats] : *allNames) {
        (void)formats;   // availability is re-decided by the realization below
        everyName.push_back(name);
    }
    if (everyName.empty()) return out;
    // ★ THE AGREEMENT CHECK THIS CALL RUNS IS CORPUS-WIDE, BECAUSE THE QUESTION
    // IS. Every other caller asks about a handful of names and is held to those;
    // this arm genuinely asks "realize EVERY name", because the reverse index it
    // is building is keyed on realized link names and cannot be narrowed before
    // the realization exists. So a disagreement anywhere in the corpus is a
    // disagreement about an answer this call is computing, and it is reported.
    // ⓘ It is reached only when the FORWARD pass left a name unbound, so the
    // ordinary build never pays for it.
    auto const wholeCorpus = ffi::realizeShippedExternSymbols(
        everyName, lattice.interner(), lattice.registry(), reporter,
        format.dataModel(), std::optional<std::string_view>{target.name()},
        format.kind(), namedTypes);
    if (!wholeCorpus.has_value()) return std::nullopt;   // corpus not located

    // on-binary name -> the row realizing to it. A SECOND row claiming one name
    // marks the key AMBIGUOUS (see the ★★ note): it is erased and blacklisted,
    // so no answer is returned for it and the reference routes unbound.
    std::unordered_map<std::string, PlatformExternRealization> byLinkName;
    std::unordered_set<std::string>                            ambiguous;
    for (auto const& [cName, row] : *wholeCorpus) {
        if (row.status != ffi::ShippedRealizationStatus::Realized) continue;
        std::string onBinary =
            ffi::linkNameFor(cName, /*asmLabel=*/{}, scheme, row.linkName);
        if (onBinary.empty()) continue;
        if (ambiguous.count(onBinary) != 0) continue;
        auto const it = byLinkName.find(onBinary);
        if (it != byLinkName.end()) {
            if (it->second.canonicalName == cName) continue;   // the same row
            ambiguous.insert(onBinary);
            byLinkName.erase(it);
            continue;
        }
        byLinkName.emplace(std::move(onBinary),
                           PlatformExternRealization{cName, row});
    }
    for (auto const& name : missing) {
        auto const it = byLinkName.find(name);
        if (it == byLinkName.end()) continue;
        out.emplace(name, it->second);
    }
    return out;
}

// ── D-LK-ARCHIVE-MEMBER-EXTERN-CANNOT-BIND-A-RESOLVE-LIBRARY ─────────────────
//
// ★★★ THE `--resolve-library` BINDING QUERY: what the binaries THE OPERATOR
// NAMED say about a symbol, asked ONCE for every producer that needs it.
//
// The sibling of `realizePlatformExternsByOnBinaryName` above, and it exists for
// the identical reason. That helper owns "what does the PLATFORM say about this
// on-binary name here"; this one owns "what do the binaries the BUILD WAS
// POINTED AT say about it". Both questions have the same two askers — an
// assembly unit (`bindAsmExternImports`) and an archive member
// (`pullStaticArchiveMembers`) — and the platform half was already shared while
// this half was not, which is exactly why the archive member could bind a name
// the corpus knows and could NOT bind a library the operator named on the
// command line. A policy written twice is a policy that gets updated once.
//
// ★★ WHAT IS SHARED IS THE QUERY, NOT THE REFUSAL. Which of a producer's rows
// may accept an answer is worded in that producer's own vocabulary (an assembly
// unit has no notion of "a name another pulled member defines"; an archive
// member has no notion of a `synthesize` recipe being unreachable at the
// `encode` tier), so each caller keeps its own accept/skip loop and asks this
// only for the mapping.
//
// ── THE POLICY THIS OWNS ────────────────────────────────────────────────────
//   * FIRST-SOURCE-WINS across the named binaries, matching `ingest()`'s
//     documented rule for a symbol two libraries both export.
//   * The recorded runtime identity is `ffi::recordedImportIdentity`'s ranking
//     (a STATED `=<import-name>` beats the file's embedded soname beats the
//     reader's basename label) — the one owner of that ranking.
//   * A DEFAULT ELF symbol version is carried only when the identity we RECORD
//     is the identity the file claims for ITSELF. Re-requesting a `sym@COMPAT`
//     row we merely walked past would manufacture the misbinding
//     D-LK-ELF-SYMBOL-VERSIONING exists to prevent, and a declared stand-in name
//     means we never observed the real library's version set at all.
//     Format-blind: PE and Mach-O rows carry no `elfSymbolVersion`.
//
// ★ THE MATCH KEY IS THE ON-BINARY SPELLING ON BOTH SIDES — an extern's
// `mangledName` against the export table's own name, decorated-to-decorated. No
// mangling rule is consulted in either direction and there is no format arm.
//
// `nullopt` ⇒ a named binary could not be read. The chokepoint
// (`ffi::readImportsForTargetFormat`) has ALREADY reported the structural cause
// loud (a wrong format, an unreadable file, a corrupted section), so a caller
// propagates the failure WITHOUT a second diagnostic. What must never happen is
// continuing with the flag quietly ineffective — that is the anchor.
struct OperatorNamedImport {
    std::string library;   // the runtime identity to RECORD
    std::string version;   // ELF default version, or empty
};

[[nodiscard]] std::optional<std::unordered_map<std::string, OperatorNamedImport>>
resolveOperatorNamedLibraryImports(std::span<ResolveLibrarySpec const> libraries,
                                   ObjectFormatSchema const&           format,
                                   DiagnosticReporter&                 reporter) {
    std::unordered_map<std::string, OperatorNamedImport> bySymbol;
    if (libraries.empty()) return bySymbol;

    for (auto const& lib : libraries) {
        // ★ THE TARGET-AWARE READ, NOT THE BARE READER. This binder and
        // `ingest()`'s `readSource` are the paths a `--resolve-library` binary
        // reaches FF1 by, and a wrong-FORMAT library must be refused on all of
        // them — elf↔pe cross-feeding is caught by nothing else, since both
        // formats are undecorated and the export names match verbatim
        // (D-FFI-RESOLVE-LIBRARY-WRONG-FORMAT-GUARD-IS-INCIDENTAL).
        auto surface = ffi::readImportsForTargetFormat(lib.path, format, reporter);
        if (!surface.has_value()) return std::nullopt;   // reported loud there
        std::string const basename = lib.path.filename().string();
        for (auto const& row : *surface) {
            if (row.mangledName.empty()) continue;
            std::string identity = ffi::recordedImportIdentity(
                lib.declaredImportName, row.soname, basename);
            std::string version;
            if (row.elfSymbolVersion.has_value()
                && row.elfSymbolVersion->isDefaultVersion) {
                std::string const& observedIdentity =
                    row.soname.empty() ? row.libraryPath : row.soname;
                if (identity == observedIdentity) {
                    version = row.elfSymbolVersion->name;
                }
            }
            bySymbol.try_emplace(
                row.mangledName,
                OperatorNamedImport{std::move(identity), std::move(version)});
        }
    }
    return bySymbol;
}

}  // namespace

bool isArArchiveFile(std::filesystem::path const& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;   // unreadable -> stays dynamic (eager probe fails loud)
    std::uint8_t buf[8] = {};
    in.read(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(sizeof(buf)));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(buf))) return false;
    for (std::size_t i = 0; i < sizeof(buf); ++i) {
        if (buf[i] != kArGlobalMagic[i]) return false;
    }
    return true;
}

bool isRelocatableObjectFile(std::filesystem::path const& path,
                             ObjectFormatSchema const&    format) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;   // unreadable -> stays dynamic (eager probe fails loud)

    // A HEADER PREFIX, never the file. Every field the backends inspect lives in
    // the first few dozen bytes (the ELF `e_type` at 16, the Mach-O `filetype`
    // at 12, the COFF `SizeOfOptionalHeader` at 16), so 64 bytes covers all
    // three with margin -- and this predicate runs over every path the operator
    // named, including large dynamic libraries it will answer `false` for.
    // ⚠ A SHORT READ IS NOT AN ERROR HERE: a file smaller than the prefix is
    // simply passed to the backend as the short span it is, and the backends'
    // bounds checks answer `false`. Rejecting the short read instead would make
    // a genuinely tiny object unclassifiable.
    std::uint8_t buf[64] = {};
    in.read(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(sizeof(buf)));
    auto const got = static_cast<std::size_t>(in.gcount());
    return format.looksLikeRelocatableObject(
        std::span<std::uint8_t const>{buf, got});
}

// ── D-LK-ARCHIVE-MEMBER-READ-USES-THE-IMAGE-FORMAT-NOT-THE-OBJECT-FORMAT ────
//
// THE FORMAT AN ARCHIVE MEMBER **IS**, WHICH IS NOT THE FORMAT THE LINK IS
// PRODUCING. A `.o` inside a `.a` is a relocatable object no matter what image
// it is being linked into, and the two vocabularies are genuinely different
// documents rather than duplicates of one:
//   ✔MEASURED (shipped CLI, baseline config, this host) reading a member with
//     the IMAGE format refuses two REAL programs -- `macho::readRelocatable-
//     Object: relocation nativeId 620756992 ... is not declared by Mach-O
//     format 'macho64-x86_64-darwin-exec'` and `elf::readRelocatableObject:
//     relocation type 4 in '.rela.text' is not declared by ELF format
//     'elf64-x86_64-linux-exec'` -- for the ordinary case of a library member
//     that CALLS another function.
//   ✔MEASURED the two vocabularies DISAGREE rather than one merely lacking a
//     key: `macho64-x86_64-darwin{,-staticlib}` declare relocation kind 1 as
//     620756992 while `-exec`/`-dylib` declare the same kind as 369098752. So
//     HOISTING the relocation table to one per-lineage declaration -- the other
//     half of this row's design fork -- cannot be right: no single table holds
//     two values for one kind. Reading with the object's own format is forced.
// ⓘ NOTHING DOWNSTREAM HAD TO CHANGE, and that is a property of the
// architecture rather than luck: every reader maps native -> universal
// `RelocationKind` on the way in and the writers map universal -> native on the
// way out, so a member read through the OBJECT vocabulary yields exactly the
// universal kinds the linker already consumes. There is no object->image
// relocation reconciliation step because there is nothing left to reconcile.
//
// ★★ THE ANSWER IS DERIVED FROM DECLARED PROPERTIES, NEVER FROM THE FORMAT'S
// NAME. `<base>-exec` -> `<base>` is a tempting string edit and it would be a
// second, silent owner of the format-identity relation -- the very shape
// [[D-CONFIG-FORMAT-DECLARES-NO-UNIFORM-ARCHITECTURE]] and the `kCManglingRules`
// lesson warn about. `resolveArchiveSiblingFormat` already answers this exact
// question by scanning every shipped document and filtering on the properties a
// format declares UNIFORMLY (`kind()`, `container() == Archive`, and agreement
// with the target delegated to `crossValidateTargetFormat`), refusing loud on 0
// or >1 rather than first-matching. Reusing it is also the only reading that is
// exactly INVERSE to the writer: the driver emits a static library's members
// through the `container: "archive"` schema, so that schema is the one that
// wrote the bytes being read back.
//
// ⓘ WHY A NON-IMAGE LINK FORMAT SHORT-CIRCUITS rather than resolving. On the
// fat-archive path the link format is itself a relocatable one (`-staticlib`,
// or the bare `.o` format), so its own `relocations()` IS an object-relocation
// table -- the identity case, not a fallback to something merely close. That
// path was already correct and stays free of the scan.
struct ArchiveMemberFormat {
    // The resolved answer, or null until first use. Points either at the link's
    // own schema (the identity case) or into `owned`.
    ObjectFormatSchema const*                 schema = nullptr;
    std::shared_ptr<ObjectFormatSchema const> owned;
    // Latched so a refusal is reported ONCE per link and not once per member:
    // the cause is the configuration, which every member shares.
    bool                                      refused = false;
};

// Who is asking, so `resolveArchiveSiblingFormat`'s refusal names the static
// link instead of the runtime object cache it was first written for.
constexpr ::dss::runtime::ArchiveSiblingRequester kArchiveMemberRequester{
    "static-link archive member read",
    "D-LK-ARCHIVE-MEMBER-READ-USES-THE-IMAGE-FORMAT-NOT-THE-OBJECT-FORMAT"};

// Resolve (once) the format that describes `archivePath`'s members. Returns
// null having ALREADY REPORTED, and never falls back to the image format --
// that fallback was the defect.
[[nodiscard]] static ObjectFormatSchema const*
archiveMemberFormat(ArchiveMemberFormat&          cache,
                    ObjectFormatSchema const&     linkFormat,
                    TargetSchema const&           target,
                    std::filesystem::path const&  archivePath,
                    std::string_view              memberName,
                    DiagnosticReporter&           reporter) {
    if (cache.schema != nullptr) return cache.schema;
    if (cache.refused)           return nullptr;

    // The refusal every arm below shares. It names the MEMBER and its ARCHIVE
    // (which member-read failed), the format the link is producing and the
    // object format it could not reach -- the four facts a triager needs, none
    // of which the resolver itself can know.
    auto const refuse = [&](std::string_view objectFormatName,
                            std::string_view detail) -> ObjectFormatSchema const* {
        cache.refused = true;
        ParseDiagnostic d;
        d.code     = DiagnosticCode::D_SchemaLoadFailed;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::format(
            "static-link: cannot determine the object format of archive member "
            "'{}' in '{}'. The link is producing '{}', whose relocation "
            "vocabulary describes an IMAGE and was never promised to describe a "
            "relocatable member; the member's own object format {}. {} "
            "Anchored: D-LK-ARCHIVE-MEMBER-READ-USES-THE-IMAGE-FORMAT-NOT-THE-"
            "OBJECT-FORMAT.",
            memberName, core::genericSpelling(archivePath), linkFormat.name(),
            objectFormatName.empty()
                ? std::string{"could not be resolved"}
                : std::format("resolved to '{}' but could not be loaded",
                              objectFormatName),
            detail);
        reporter.report(std::move(d));
        return nullptr;
    };

    // The identity case: a relocatable link format already speaks the object
    // vocabulary (see the note above).
    if (!linkFormat.isImageFlavor()) {
        cache.schema = &linkFormat;
        return cache.schema;
    }

    auto const formatsDir = findShippedConfigDir("object-formats");
    if (!formatsDir) {
        return refuse({},
            "The shipped `object-formats` directory could not be located, so "
            "no object-format document could be scanned (set DSS_CONFIG_ROOT, "
            "or run from inside the config tree).");
    }
    auto sibling = ::dss::runtime::resolveArchiveSiblingFormat(
        linkFormat, target, *formatsDir, kArchiveMemberRequester);
    if (!sibling) return refuse({}, sibling.error());

    auto loaded = ObjectFormatSchema::loadShipped(*sibling);
    if (!loaded) {
        std::string detail;
        for (auto const& diag : loaded.error()) {
            if (!detail.empty()) detail += "; ";
            detail += diag.message;
        }
        return refuse(*sibling, detail);
    }
    cache.owned  = std::move(loaded).value();
    cache.schema = cache.owned.get();
    return cache.schema;
}

// Parse ONE `ar` member's raw bytes back into a mergeable `AssembledModule`,
// dispatching to the per-FORMAT relocatable-object reader by the object-format
// KIND (the closed-enum agnostic axis, never a format-name branch). A fresh,
// process-unique CompilationUnitId is minted per member (the merge keys its
// symbol index by (cuId, SymbolId), so a member must never share a cuId with the
// client or another member -- the monotonic minter never repeats). A format whose
// kind has no reader arm fails loud rather than silently mis-parsing a member with
// the wrong reader. Consuming the reader's `optional` is the read-success signal
// -- NOT `module.ok()`, which is a tautology for reader output AND false for a
// data-only member (see elf_object_reader.hpp). THE single member-read chokepoint
// shared by BOTH the lazy reference-driven pull (`pullStaticArchiveMembers`, the
// exe/final-link path) AND the whole-archive extraction (`extractStaticArchive-
// Members`, the fat-static-library path) -- so a new object format lights up for
// both by construction (the §A.5 multi-site funnel).
//
// ★ IT IS ALSO THE ONE PLACE THE MEMBER'S OWN FORMAT IS RESOLVED, and that is
// deliberate: the funnel that guarantees a new object format lights up for both
// callers is the same funnel that guarantees neither can go on reading a member
// with the image's vocabulary. `linkFormat` is what the link is PRODUCING;
// everything below reads through `memberSchema`, which is what the member IS.
[[nodiscard]] static std::optional<AssembledModule>
readArchiveMemberModule(std::span<std::uint8_t const> memberBytes,
                        TargetSchema const&           target,
                        ObjectFormatSchema const&     linkFormat,
                        ArchiveMemberFormat&          memberFormatCache,
                        std::filesystem::path const&  archivePath,
                        std::string_view              memberName,
                        DiagnosticReporter&           reporter) {
    ObjectFormatSchema const* const resolved =
        archiveMemberFormat(memberFormatCache, linkFormat, target, archivePath,
                            memberName, reporter);
    if (resolved == nullptr) return std::nullopt;  // fail-loud already reported
    ObjectFormatSchema const& format = *resolved;

    CompilationUnitId const memberCu =
        substrate::mintMonotonicId<CompilationUnitId>();

    // ── D-PROGRAM-TIER-RETAINS-FORMAT-IDENTITY-BRANCHES ────────────────────
    //
    // A 3-arm `switch (format.kind())` STOOD HERE, plus a `default:` arm that
    // emitted the no-reader refusal. It is GONE: the member's own schema has
    // already resolved to a backend, and reading a relocatable object is that
    // backend's job — the exact counterpart of `encode`, which TF-C125 moved
    // onto this same interface for this same reason.
    //
    // ★ THE `default:` ARM MOVED RATHER THAN VANISHING, AND IT GOT STRONGER.
    // Its wording and F_ code are unchanged, but it now lives in the wasm and
    // spirv backends as an OVERRIDE of a PURE VIRTUAL. A `default:` silently
    // absorbs every format nobody thought about; a pure virtual makes a sixth
    // backend unable to COMPILE without answering the question.
    //
    // ⚠ A null backend cannot reach here — `archiveMemberFormat` above returns
    // a schema that LOADED, and a document with no resolvable `kind` is refused
    // at load. Defended anyway, because `ObjectFormatSchema{ObjectFormatData}`
    // is a public constructor and an in-memory producer bypasses that entirely.
    link::ObjectFormatBackend const* const backend = format.backend();
    if (backend == nullptr) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::F_UnsupportedBinaryFormat;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::format(
            "archive member reader: object format '{}' resolved no backend -- "
            "cannot read archive members for a schema with no format identity.",
            format.name());
        reporter.report(std::move(d));
        return std::nullopt;
    }
    return backend->readRelocatableObject(memberBytes, target, format,
                                          reporter, memberCu);
}

std::optional<std::vector<AssembledModule>>
pullStaticArchiveMembers(std::span<AssembledModule const>       clientModules,
                         std::span<std::filesystem::path const> archivePaths,
                         std::span<ResolveLibrarySpec const>    dynamicLibraries,
                         TargetSchema const&                    target,
                         ObjectFormatSchema const&              format,
                         DiagnosticReporter&                    reporter) {
    std::vector<AssembledModule> pulled;
    if (archivePaths.empty()) return pulled;  // nothing to pull

    // Read every archive up front: its raw bytes (kept alive so member subspans
    // stay valid) + its parsed member list + armap (c161 reader).
    struct ParsedArchive {
        std::vector<std::uint8_t> bytes;
        ffi::ArArchive            archive;
    };
    std::vector<ParsedArchive> archives;
    archives.reserve(archivePaths.size());
    for (auto const& archivePath : archivePaths) {
        std::ifstream in(archivePath, std::ios::binary);
        if (!in) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::F_FileOpenFailed;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "static-link: failed to open archive '{}' for reading "
                "(D-LK-STATIC-LINK).",
                core::genericSpelling(archivePath));
            reporter.report(std::move(d));
            return std::nullopt;
        }
        std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in),
                                        std::istreambuf_iterator<char>()};
        auto arch = ffi::readArArchive(
            std::span<std::uint8_t const>{bytes.data(), bytes.size()},
            archivePath.filename().string(), reporter);
        if (!arch) return std::nullopt;   // corrupt archive -> reader fail-loud
        archives.push_back(ParsedArchive{std::move(bytes), std::move(*arch)});
    }

    // Global armap: symbol name -> (archiveIdx, memberIndex). First-wins across
    // archives (standard left-to-right link order) + first-wins within one
    // archive (an armap lists a symbol against its single defining member).
    std::unordered_map<std::string, std::pair<std::size_t, std::size_t>> armap;
    for (std::size_t ai = 0; ai < archives.size(); ++ai) {
        for (auto const& sym : archives[ai].archive.symbols) {
            armap.emplace(sym.name, std::pair{ai, sym.memberIndex});
        }
    }

    // Names already satisfied by a DEFINITION (every client module, then each
    // pulled member). A worklist name that is already defined is never pulled
    // again. Only externally-visible definitions can satisfy a cross-module
    // reference (the same filter the c163 armap writer applies), so Local defs
    // are excluded.
    // ⚠ EVERY module in `clientModules`, not merely the compiled one -- see the
    // header's plural note
    // (D-OPT7-CROSSCU-THUNK-RESERVED-FOR-SEPARATE-COMPILATION). A
    // pre-assembled
    // object input that DEFINES a name must suppress the pull of an archive
    // member defining it too, or the link merges two definitions of one symbol.
    std::unordered_set<std::string> definedNames;
    for (auto const& clientModule : clientModules) {
        for (auto const& ms : clientModule.symbols) {
            if (!ms.name.empty()
                && isExternallyVisible(ms.binding, ms.visibility)) {
                definedNames.insert(ms.name);
            }
        }
    }

    // Worklist: every client module's unresolved extern names (the references to
    // satisfy). An object input's externs belong here for the same reason the
    // compiled client's do -- they are references the archives may resolve.
    std::vector<std::string> worklist;
    for (auto const& clientModule : clientModules) {
        for (auto const& ext : clientModule.externImports) {
            if (!ext.mangledName.empty()) worklist.push_back(ext.mangledName);
        }
    }

    // The member format, resolved at most ONCE for the whole pull (see
    // `ArchiveMemberFormat`): it is a function of the LINK's format and target,
    // which no member varies, so per-member resolution would re-scan the
    // object-format tree for every symbol an archive satisfies.
    ArchiveMemberFormat memberFormat;

    // (archiveIdx << 32) | memberIndex of every member already pulled -- the LAZY
    // dedup so a member defining several referenced symbols is pulled once.
    std::unordered_set<std::uint64_t> pulledMembers;
    auto memberKey = [](std::size_t ai, std::size_t mi) -> std::uint64_t {
        return (static_cast<std::uint64_t>(ai) << 32)
             |  static_cast<std::uint64_t>(static_cast<std::uint32_t>(mi));
    };

    for (std::size_t cursor = 0; cursor < worklist.size(); ++cursor) {
        std::string const& name = worklist[cursor];
        if (definedNames.count(name) != 0) continue;   // already satisfied
        auto const it = armap.find(name);
        if (it == armap.end()) continue;   // no archive defines it -> a real FFI
                                           // import / an undefined the linker's
                                           // own gate handles (never pulled here)
        auto const [ai, mi] = it->second;
        if (!pulledMembers.insert(memberKey(ai, mi)).second) continue;  // lazy dedup

        ffi::ArMember const& member = archives[ai].archive.members[mi];
        std::span<std::uint8_t const> const memberBytes{
            archives[ai].bytes.data() + static_cast<std::size_t>(member.dataOffset),
            static_cast<std::size_t>(member.size)};

        // Parse the member back into a mergeable module via the shared
        // per-format reader chokepoint (fresh cuId minted inside; a format
        // with no reader arm fails loud there).
        auto member_mod =
            readArchiveMemberModule(memberBytes, target, format, memberFormat,
                                    archivePaths[ai], member.name, reporter);
        if (!member_mod) return std::nullopt;   // member-read fail-loud

        // A pulled member's externally-visible definitions satisfy later
        // worklist names; its OWN unresolved externs feed the next pass -- the
        // transitive lazy-pull (a member referencing another member).
        for (auto const& ms : member_mod->symbols) {
            if (!ms.name.empty() && isExternallyVisible(ms.binding, ms.visibility)) {
                definedNames.insert(ms.name);
            }
        }
        for (auto const& ext : member_mod->externImports) {
            if (!ext.mangledName.empty()) worklist.push_back(ext.mangledName);
        }
        pulled.push_back(std::move(*member_mod));
    }

    // ── D-LK-ARCHIVE-MEMBER-EXTERN-LOSES-ITS-LIBRARY ─────────────────────────
    //
    // ★★★ REBIND EVERY PULLED MEMBER'S LIBRARY IMPORTS, BECAUSE THE OBJECT FILE
    // COULD NOT CARRY THEM AND THAT IS NOT A DSS GAP.
    //
    // ✔MEASURED (shipped CLI, this host, baseline config) with a discriminating
    // pair over ONE pair of sources — a TU calling `puts` plus a `main` calling
    // it — compiled DIRECTLY as a multi-TU exec vs. routed through
    // `-staticlib` + `--resolve-library`: the direct build returned rc=0 on all
    // five legs, and the archive round trip returned rc=1 on all five with
    // `K_SymbolUndefined` naming `puts`. So the member read back CLEANLY and
    // then failed to LINK.
    //
    // WHY, AND WHY THE FIX IS NOT AT READ TIME. On the direct path the owning
    // image is decided at the SEMANTIC tier: `suppressedShippedSymbolFor` folds
    // the shipped descriptor's per-format `library` map into the extern's
    // `libraryPath` (UCRT-P4 Decision 1 — the corpus is the single owner of
    // "which image owns this name here"). An OBJECT FILE has nowhere to record
    // that: an undefined symbol in ELF/COFF/Mach-O is a NAME, and nothing else.
    // ✔MEASURED by dumping the emitted archives — the pe64 `.lib` and the elf64
    // `.a` contain the string `puts` and NO library string at all, no
    // `ucrtbase.dll` and no `libc.so.6`. Teaching a reader to recover it would
    // mean inventing a DSS-private extension to a format whose whole value is
    // that a foreign linker reads it, so the binding must be RE-DERIVED, from
    // the same authority that derived it the first time.
    //
    // ★★ AND IT MUST HAPPEN BEFORE THE MERGE, WHICH IS WHY IT IS HERE AND NOT IN
    // THE LINKER. `linker::link`'s dedup key is the FULL import identity
    // (mangledName, libraryPath, version), so a member's `puts` and the client's
    // `puts` fold into ONE import only if both are already bound to the same
    // image when the merge runs. Binding after the merge would leave two rows
    // that agree on the name and disagree on the library — two import
    // descriptors for one C runtime, which is precisely the split-CRT defect
    // UCRT-P4 exists to prevent. (The linker could not do this anyway: `link`
    // declares no dependency on `ffi`, and it never sees `CompileOptions`.)
    //
    // ★★ THE FAT-ARCHIVE PATH DELIBERATELY DOES **NOT** GET THIS, and the
    // asymmetry is the point rather than an omission.
    // `extractStaticArchiveMembers` repacks members into another RELOCATABLE
    // archive, and a relocatable object's whole contract is that it DEFERS "who
    // owns this name" to a later link — `allowsUndefinedImports()` keeps such a
    // row as a legal undefined symbol. Answering the question there would bind
    // an import the repacked member cannot even record. This path is the one
    // producing an IMAGE, so it is the one that must answer.
    //
    // ★ WHAT IS DELIBERATELY LEFT UNBOUND, so a genuinely undefined symbol still
    // fails LOUD with `K_SymbolUndefined` and its present message:
    //   * a name the client or another pulled member DEFINES — the merge binds
    //     that reference to the definition, and it is not a library import at
    //     all. Filtering it here also keeps the corpus query off the names that
    //     dominate an ordinary static link.
    //   * a name the corpus does not answer for on this (target, format) — a
    //     typo, a missing library, an ordinary program symbol. Absent ⇒ untouched
    //     ⇒ judged by the link tier exactly as today.
    //   * a `synthesize` RECIPE row (pe's bare `printf`). The body that realizes
    //     it is emitted by MIR synthesis, which already ran when the member was
    //     COMPILED — a member DSS built carries the shim and imports the UCRT
    //     cores, so this arm is reached only by a FOREIGN member that references
    //     `printf` directly. Binding it would request a symbol `ucrtbase.dll`
    //     does not export: the image would link clean and die at LOAD with
    //     0xC0000139. Leaving it unbound yields a build error instead, which is
    //     strictly the better failure.
    //   * a `linkName` row reached by its C spelling (a foreign member writing
    //     `time` where this platform exports `_time64`). DSS will not silently
    //     re-spell a reference the member's own relocations already name.
    // ⓘ `isEagerImport` is NOT set: an unreferenced row must still be DROPPED by
    // the reference gate, so binding a library here can never resurrect a dead
    // declaration as a real import.

    // ── (1) D-LK-ARCHIVE-MEMBER-EXTERN-CANNOT-BIND-A-RESOLVE-LIBRARY ─────────
    //
    // ★★★ THE OPERATOR-NAMED BINARIES FIRST, BECAUSE THE OPERATOR OUTRANKS THE
    // PLATFORM DEFAULT. Everything the arm below says about WHY the rebinding
    // must happen here applies verbatim; this arm answers the half of the
    // question the shipped-descriptor corpus structurally CANNOT. The corpus
    // knows the platform's own images (`ucrtbase.dll`, `libc.so.6`); it has
    // never heard of `dynsrc.dll`, a library the operator built five seconds ago
    // and named on the command line. An object file records a symbol NAME and
    // nothing else, so a member that referenced that library lost the binding at
    // archive-write time and only the build that names the library again can
    // restore it.
    //
    // ★★ ORDER IS PRECEDENCE, AND IT IS THE SAME ORDER `bindAsmExternImports`
    // USES. Both arms skip a row whose `libraryPath` is already set, so running
    // this one first IS the statement "an operator-named binary wins". Reversing
    // them would let a platform default silently claim a name the operator had
    // explicitly pointed the build at.
    //
    // ★★ AND — like the corpus arm — IT MUST HAPPEN BEFORE THE MERGE. See that
    // arm's note: `linker::link`'s dedup key is the FULL import identity
    // (mangledName, libraryPath, version), so a member's reference and the
    // client's reference to one symbol fold into ONE import only if both are
    // already bound to the same image when the merge runs.
    //
    // ⚠ `dynamicLibraries` is the DYNAMIC half of `--resolve-library` — the
    // driver has already partitioned the `ar` archives out of it (they are
    // `archivePaths` here, merged INTO the image, recording no import at all).
    // Handing the un-partitioned list over would feed an archive to the export
    // reader, which correctly refuses it and would fail an otherwise good build.
    //
    // The skip set is this producer's own, identical to the corpus arm's: a name
    // the client or another pulled member DEFINES is bound by the merge to that
    // definition and is not a library import at all.
    if (!pulled.empty() && !dynamicLibraries.empty()) {
        auto const bySymbol = resolveOperatorNamedLibraryImports(
            dynamicLibraries, format, reporter);
        // nullopt ⇒ a named binary could not be read; the chokepoint already
        // reported the structural cause loud. Refuse WITHOUT a second
        // diagnostic rather than continue with the flag quietly ineffective.
        if (!bySymbol.has_value()) return std::nullopt;
        for (auto& mod : pulled) {
            for (auto& ext : mod.externImports) {
                if (ext.mangledName.empty()) continue;
                if (!ext.libraryPath.empty()) continue;
                if (definedNames.count(ext.mangledName) != 0) continue;
                auto const it = bySymbol->find(ext.mangledName);
                if (it == bySymbol->end()) continue;
                ext.libraryPath = it->second.library;
                ext.version     = it->second.version;
            }
        }
    }

    // ── (2) THE PLATFORM REALIZATION ORACLE ─────────────────────────────────
    if (!pulled.empty()) {
        std::vector<std::string> names;
        for (auto const& mod : pulled) {
            for (auto const& ext : mod.externImports) {
                if (ext.mangledName.empty()) continue;
                if (!ext.libraryPath.empty()) continue;
                if (definedNames.count(ext.mangledName) != 0) continue;
                names.push_back(ext.mangledName);
            }
        }
        if (!names.empty()) {
            // The lattice label is the throwaway interner's, not a claim about
            // the member's source language — an archive member has none in this
            // process. `format.name()` is the one fact that is actually true of
            // every member being bound here.
            // D-FFI-DUPLICATE-SYMBOL-ACROSS-DESCRIPTORS-SILENTLY-ORDER-RESOLVED:
            // the oracle reports a cross-descriptor realization disagreement on
            // THIS reporter and still answers the name (omitting it would route
            // it unbound and file the config's fault against the user's program).
            // The refusal is therefore the caller's, off the errorCount snapshot
            // — and it must be spelled here rather than left to a later tier,
            // because the enclosing `entry` snapshot was taken before this
            // function ran and nothing between here and the link would compare.
            auto const corpusEntry = reporter.errorCount();
            auto const realized = realizePlatformExternsByOnBinaryName(
                names, target, format,
                substrate::mintMonotonicId<CompilationUnitId>(), format.name(),
                reporter);
            if (!tierClean(reporter, corpusEntry)) return std::nullopt;
            // nullopt ⇒ the shippedLibs directory could not be located: a
            // statement about the ENVIRONMENT, never about the user's program.
            // Every name stays unbound and the link tier judges the reference,
            // exactly as it did before this binder existed.
            if (realized.has_value()) {
                std::string const formatKey{objectFormatKindName(format.kind())};
                for (auto& mod : pulled) {
                    for (auto& ext : mod.externImports) {
                        if (ext.mangledName.empty()) continue;
                        if (!ext.libraryPath.empty()) continue;
                        if (definedNames.count(ext.mangledName) != 0) continue;
                        auto const found = realized->find(ext.mangledName);
                        if (found == realized->end()) continue;
                        auto const& row = found->second.row;
                        if (!row.recipeId.empty()) continue;   // see ★ above
                        if (!row.linkName.empty()
                            && ffi::linkNameFor(found->second.canonicalName,
                                                /*asmLabel=*/{},
                                                format.cSymbolDecoration().scheme,
                                                row.linkName)
                                   != ext.mangledName) {
                            continue;   // see ★ above
                        }
                        // Fold the row's per-object-format `library` map on the
                        // active format — the identical fold `buildCuMir`
                        // performs for the C path and the asm binder performs
                        // for a `.s`, keyed on the same `objectFormatKindName`.
                        // A key ABSENT means the platform states no image for
                        // this name on this format: leave it unbound.
                        auto const lib = row.library.find(formatKey);
                        if (lib == row.library.end() || lib->second.empty()) {
                            continue;
                        }
                        ext.libraryPath = lib->second;
                        ext.version     = row.version;
                    }
                }
            }
        }
    }
    return pulled;
}

std::optional<ExtractedArchiveMembers>
extractStaticArchiveMembers(std::span<std::filesystem::path const> archivePaths,
                            TargetSchema const&                    target,
                            ObjectFormatSchema const&              format,
                            DiagnosticReporter&                    reporter) {
    ExtractedArchiveMembers out;
    // Resolved at most once for the whole extraction -- same reasoning as the
    // lazy pull's. On this path `format` is a `container: "archive"` schema, so
    // the resolution is the identity case and costs nothing.
    ArchiveMemberFormat memberFormat;
    for (auto const& archivePath : archivePaths) {
        std::ifstream in(archivePath, std::ios::binary);
        if (!in) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::F_FileOpenFailed;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "fat-archive: failed to open input static archive '{}' for "
                "reading (D-FF1-STATICLIB-FAT-ARCHIVE).",
                core::genericSpelling(archivePath));
            reporter.report(std::move(d));
            return std::nullopt;
        }
        std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in),
                                        std::istreambuf_iterator<char>()};
        auto arch = ffi::readArArchive(
            std::span<std::uint8_t const>{bytes.data(), bytes.size()},
            archivePath.filename().string(), reporter);
        if (!arch) return std::nullopt;   // corrupt archive -> reader fail-loud

        // EVERY member, in archive order -- a static LIBRARY packages all of its
        // objects (unlike the LAZY referenced-subset the exe/final-link path
        // pulls): a downstream link against this library must be able to pull
        // ANY member, so dropping an unreferenced one would silently ship an
        // incomplete library. The member name (long-name-expanded by the reader)
        // is carried verbatim -- cosmetic (the armap selects members by index),
        // and `ar` permits duplicate member names; a rare empty name (never from
        // a well-formed archive) is synthesized so the writer's name belt holds.
        for (auto const& member : arch->members) {
            std::span<std::uint8_t const> const memberBytes{
                bytes.data() + static_cast<std::size_t>(member.dataOffset),
                static_cast<std::size_t>(member.size)};
            auto member_mod =
                readArchiveMemberModule(memberBytes, target, format,
                                        memberFormat, archivePath, member.name,
                                        reporter);
            if (!member_mod) return std::nullopt;   // member-read fail-loud
            out.modules.push_back(std::move(*member_mod));
            out.names.push_back(
                member.name.empty()
                    ? ("member_" + std::to_string(out.names.size()) + ".o")
                    : member.name);
        }
    }
    return out;
}

// D-OPT7-CROSSCU-THUNK-RESERVED-FOR-SEPARATE-COMPILATION -- the EAGER half of
// the link's input set. See
// the header docblock for why "eager" is the whole distinction from an archive.
std::optional<std::vector<AssembledModule>>
readObjectInputModules(std::span<std::filesystem::path const> objectPaths,
                       TargetSchema const&                    target,
                       ObjectFormatSchema const&              linkFormat,
                       DiagnosticReporter&                    reporter) {
    std::vector<AssembledModule> objects;
    if (objectPaths.empty()) return objects;   // valid no-op: no object inputs
    objects.reserve(objectPaths.size());

    // Resolved at most ONCE for the whole input set, exactly as both archive
    // paths do it: the member format is a function of the LINK's format and
    // target, which no input varies, so per-file resolution would re-scan the
    // object-format tree for every object named.
    ArchiveMemberFormat memberFormat;
    for (auto const& objectPath : objectPaths) {
        std::ifstream in(objectPath, std::ios::binary);
        if (!in) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::F_FileOpenFailed;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "object input: failed to open '{}' for reading. It was named as "
                "a link input, so it cannot be skipped -- an omitted object "
                "would link into a smaller image that may still run "
                "(D-OPT7-CROSSCU-THUNK-RESERVED-FOR-SEPARATE-COMPILATION).",
                core::genericSpelling(objectPath));
            reporter.report(std::move(d));
            return std::nullopt;
        }
        std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in),
                                        std::istreambuf_iterator<char>()};

        // THE SAME CHOKEPOINT AN ARCHIVE MEMBER TAKES. A bare object file is a
        // member of a one-member container: the bytes are the whole file rather
        // than a subspan, and nothing else about reading it differs. Funnelling
        // here is what makes a newly-supported object format light up for object
        // inputs and archive members together, by construction.
        auto object_mod = readArchiveMemberModule(
            std::span<std::uint8_t const>{bytes.data(), bytes.size()}, target,
            linkFormat, memberFormat, objectPath,
            objectPath.filename().string(), reporter);
        if (!object_mod) return std::nullopt;   // read fail-loud already reported
        objects.push_back(std::move(*object_mod));
    }
    return objects;
}

bool linkAndWriteWithStaticArchives(AssembledModule                        clientModule,
                                    std::span<std::filesystem::path const> objectInputs,
                                    std::span<std::filesystem::path const> staticArchives,
                                    std::span<ResolveLibrarySpec const>    dynamicLibraries,
                                    TargetSchema const&                    target,
                                    ObjectFormatSchema const&              format,
                                    std::filesystem::path const&           outPath,
                                    DiagnosticReporter&                    reporter,
                                    ImageRequest const&                    request) {
    if (objectInputs.empty() && staticArchives.empty()) {
        return linkAndWrite(std::span<AssembledModule const>{&clientModule, 1},
                            target, format, outPath, reporter, request);
    }

    // EAGER FIRST -- see the header's ordering note. The objects join the seed
    // the lazy archive pull resolves against, so an archive member an object
    // input needs is pulled rather than silently left behind.
    auto objects = readObjectInputModules(objectInputs, target, format, reporter);
    if (!objects) return false;   // read fail-loud already reported

    std::vector<AssembledModule> combined;
    combined.reserve(1 + objects->size());
    combined.push_back(std::move(clientModule));
    for (auto& object_mod : *objects) combined.push_back(std::move(object_mod));

    auto pulled = pullStaticArchiveMembers(
        std::span<AssembledModule const>{combined.data(), combined.size()},
        staticArchives, dynamicLibraries, target, format, reporter);
    if (!pulled) return false;   // pull fail-loud already reported

    // Link the COMBINED span [client, objects..., pulled...]. >1 element triggers
    // the c154 cross-CU merge in `linker::link`, whose `mergeModules` binds each
    // cross-module reference to its definition (stripping the extern import)
    // exactly as it resolves a sibling-CU reference. With no objects and nothing
    // pulled the span is the client alone -- the single-CU path, unchanged.
    combined.reserve(combined.size() + pulled->size());
    for (auto& member_mod : *pulled) combined.push_back(std::move(member_mod));
    return linkAndWrite(std::span<AssembledModule const>{combined.data(), combined.size()},
                        target, format, outPath, reporter, request);
}

// c163 (D-LK-STATIC-ARCHIVE-WRITER): link N assembled CUs into N relocatable
// object members + bundle them into ONE `.a` static archive. See the header
// docblock for the contract.
bool linkAndWriteStaticArchive(std::span<AssembledModule const> modules,
                               std::span<std::string const>     memberNames,
                               TargetSchema const&              target,
                               ObjectFormatSchema const&        format,
                               std::filesystem::path const&     outPath,
                               DiagnosticReporter&              reporter,
                               ImageRequest const&              request) {
    substrate::PhaseTimers::Scope linkPhase{substrate::CompilePhase::Link};
    auto const entry = reporter.errorCount();

    // An archive bundles RELOCATABLE objects a foreign linker later pulls +
    // merges -- an image-flavor format (.so/.exe/.dylib) is not poolable this
    // way. Reject loud rather than emit an archive of non-relocatable members.
    if (format.isImageFlavor()) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::K_NoMatchingObjectFormat;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::format(
            "linkAndWriteStaticArchive: object format '{}' is an image flavor "
            "(.so/.dll/.exe/.dylib); a static archive bundles RELOCATABLE "
            "objects (ELF ET_REL / Mach-O MH_OBJECT) a foreign linker pulls + "
            "merges -- select a relocatable object format "
            "(D-LK-STATIC-ARCHIVE-WRITER).",
            format.name());
        reporter.report(std::move(d));
        return false;
    }
    if (modules.size() != memberNames.size()) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::K_NoMatchingObjectFormat;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::format(
            "linkAndWriteStaticArchive: {} module(s) but {} member name(s) -- "
            "the spans must be parallel (one archived file name per member) "
            "(D-LK-STATIC-ARCHIVE-WRITER).",
            modules.size(), memberNames.size());
        reporter.report(std::move(d));
        return false;
    }

    // Link each module INDEPENDENTLY to its own `.o` bytes (a 1-element link,
    // never the cross-CU merge) + collect its DEFINED externally-visible
    // symbols for the armap (the same on-binary names the object writer put in
    // the member's symbol table).
    std::vector<link::format::ArMemberInput> members;
    members.reserve(modules.size());
    for (std::size_t i = 0; i < modules.size(); ++i) {
        // D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: `request` rides each member link
        // so the capability gate fires on the archive path too. No archive
        // format declares a stack-reserve capability (an `ar` member carries no
        // image headers), so a request here is REFUSED on the first member —
        // never silently swallowed by a build that reports success.
        auto image = linker::link(std::span<AssembledModule const>{&modules[i], 1},
                                  target, format, reporter, request);
        if (!image.ok() || !tierClean(reporter, entry)) {
            return false;
        }
        std::vector<std::string> exported;
        for (ModuleSymbol const& ms : modules[i].symbols) {
            if (isExternallyVisible(ms.binding, ms.visibility) && !ms.name.empty()) {
                exported.push_back(ms.name);
            }
        }
        members.push_back(link::format::ArMemberInput{
            memberNames[i], std::move(image.bytes), std::move(exported)});
    }

    // c169/c171 (D-FF1-AR-COFF-WRITER + D-FF1-AR-STATICLIB-DRIVER-WIRING):
    // the archive FLAVOR is the format ecosystem's `ar` variant — Microsoft
    // COFF (`.lib`: adds the little-endian 2nd linker member a link.exe
    // consumer requires) for PE, GNU/System V (`.a`) for ELF + Mach-O.
    // Derived from `format.kind()` (the closed enum, the existing agnostic
    // dispatch axis), never a format-name branch. Without this a PE static
    // library would ship SysV-only and MS link.exe could not resolve its
    // members (the c169 default was SysV; c171 threads the real flavor).
    // D-PROGRAM-TIER-RETAINS-FORMAT-IDENTITY-BRANCHES: this was
    // `(format.kind() == ObjectFormatKind::Pe) ? Coff : SysV`. ⚠ THE COMMENT
    // ABOVE USED TO DEFEND IT AS "the closed enum, the existing agnostic
    // dispatch axis, never a format-name branch" — and that defence is exactly
    // the rationalization the veto rejects: a closed enum keyed on IDENTITY is
    // an identity branch whether it is spelled as an `if`, a `switch` or a
    // table. The flavor is now DECLARED by the format that writes the archive.
    // The switch below is over a declared VERB, not an identity, and the
    // `-Werror=switch` backstop makes a new flavor a build error here rather
    // than a silent SysV fallback.
    link::format::ArArchiveFlavor flavor = link::format::ArArchiveFlavor::SysV;
    switch (format.archiveFlavor()) {
        case ArchiveFlavor::Coff: flavor = link::format::ArArchiveFlavor::Coff; break;
        case ArchiveFlavor::SysV: flavor = link::format::ArArchiveFlavor::SysV; break;
        case ArchiveFlavor::Unspecified:
            // Unreachable through the loader (`validate()` requires the key on
            // every `container: archive` format) and unreachable here anyway —
            // this function only runs for an archive. Fail LOUD rather than
            // write a guessed layout: a wrong `ar` variant produces a `.lib`
            // whose members `link.exe` silently cannot resolve.
            report(reporter, DiagnosticCode::C_MissingField,
                   DiagnosticSeverity::Error,
                   std::format("object format '{}' writes a static archive but "
                               "declares no `archiveFlavor` — refusing to guess "
                               "an archive layout", format.name()));
            return false;
    }
    auto const archive = link::format::writeArArchive(members, reporter, flavor);
    if (!tierClean(reporter, entry)) {
        return false;   // a writer fail-loud belt fired (name/size/offset).
    }
    return linker::writeBytes(archive, outPath, reporter);
}

// Assemble ONE CompilationUnit to its AssembledModule (no link/write). Returns
// nullopt on any tier failure (diagnostics already emitted via `reporter`). The
// multi-CU driver calls this per CU, collects the modules, then `linkAndWrite`s once.
//
// = `buildCuMir(...)` composed with `lowerCuMirToAssembly(...)`. Single-CU callers
// (`compileSingleUnit`, `compileFiles`) get byte-identical output to the former
// monolithic `buildAssembledModule` — the two halves run back-to-back with no
// state held between them other than the `CuMirModule` that carried the MIR/LIR
// seam state inline before the split.
std::optional<AssembledModule>
assembleUnit(CompilationUnit const&        cu,
             GrammarSchema const&          grammar,
             TargetSchema const&           target,
             ObjectFormatSchema const&     format,
             std::uint16_t                 callingConventionIndex,
             DiagnosticReporter&           reporter,
             CompileOptions const&         opts) {
    auto cuMir = buildCuMir(cu, grammar, target, format,
                            callingConventionIndex, reporter, opts);
    if (!cuMir) return std::nullopt;
    return lowerCuMirToAssembly(
        *cuMir, format.processArgs(), format.entryVerbs(),
        format.sehPersonality(), format.name(),
        target.wideFloatSoftcallLibrary(format.kind()), reporter);
}

namespace {

// ── D-ASM-EXTERN-CALL-CANNOT-BIND-A-LIBRARY + ────────────────────────────────
// ── D-ASM-RESOLVE-LIBRARY-SILENTLY-IGNORED-ON-ENCODE-TIER ────────────────────
//
// Bind the `encode` tier's unbound extern rows to an owning image, by the SAME
// two oracles the C path consults for a HAND-DECLARED name, in the same order.
//
// ★★★ WHY THE `.s` PATH NEEDS ITS OWN BINDER RATHER THAN `ffi::ingest()`.
// `ingest()` matches a CANONICAL C IDENTIFIER: it runs `linkNameFor` to compute
// the on-binary spelling, then un-mangles it back to key the export table, and
// it writes its answers into an `HirFfiMap` keyed by `HirNodeId`. A `.s` has
// neither end of that. It writes the ON-BINARY SYMBOL ITSELF — a Mach-O source
// spells `_puts`, exactly as gas requires (`asm_text_to_lir.cpp`: "THE NAME IS
// TAKEN VERBATIM, NOT MANGLED") — so there is no canonical identifier to mangle
// and no HIR node to key. Matching the export table by the written name is
// therefore not a shortcut, it is the ONLY exact key; and running the name
// through un-mangle→re-mangle to reach `ingest()` would push it through a lossy
// inverse to arrive back where it started. What the two binders MUST share is
// the recorded-import IDENTITY ranking, and they do — `ffi::recordedImportIdentity`
// is one function with two callers, not a copied conditional.
//
// ★★ AGNOSTIC. Nothing here asks which language, CPU or object format this is.
// The library map is folded by `objectFormatKindName(format.kind())` — the same
// key `buildCuMir` folds `HirExternRecord.libraryOverride` on, and the ONE
// format-keyed step the C path has too. Every other input is data off a
// descriptor row or an export table.
//
// Returns false iff a diagnostic was reported and the build must stop.
[[nodiscard]] bool bindAsmExternImports(std::vector<ExternImport>& externs,
                                        CompilationUnit const&     cu,
                                        GrammarSchema const&       grammar,
                                        TargetSchema const&        target,
                                        ObjectFormatSchema const&  format,
                                        CompileOptions const&      opts,
                                        DiagnosticReporter&        reporter) {
    if (externs.empty()) return true;

    // ── (1) `--resolve-library` — the binaries the build was POINTED AT ──────
    //
    // Highest precedence for the same reason it is on the C path: the operator
    // named this file, so its export table outranks any platform default.
    //
    // ★★★ THE QUERY ITSELF NOW LIVES IN `resolveOperatorNamedLibraryImports`,
    // WHICH THIS FILE'S OTHER EXTERN PRODUCER — THE ARCHIVE MEMBER — ALSO ASKS
    // (D-LK-ARCHIVE-MEMBER-EXTERN-CANNOT-BIND-A-RESOLVE-LIBRARY). It used to be
    // spelled out HERE, inline, which is why the member could not bind an
    // operator-named library at all: there was nothing to call. Read that
    // helper's docblock for the identity ranking, the first-source-wins rule and
    // the ELF default-version clause. What stays here is the ACCEPT policy —
    // which of THIS producer's rows may take an answer — because that is worded
    // in this producer's vocabulary and nobody else's.
    if (!opts.resolveLibraries.empty()) {
        auto const bySymbol = resolveOperatorNamedLibraryImports(
            opts.resolveLibraries, format, reporter);
        // The chokepoint reports its own failure LOUD (it names the path and
        // the structural cause), so this refuses WITHOUT a second diagnostic —
        // the same contract `ingest()`'s source loop honours.
        if (!bySymbol.has_value()) return false;
        for (auto& e : externs) {
            if (!e.libraryPath.empty()) continue;
            auto const it = bySymbol->find(e.mangledName);
            if (it == bySymbol->end()) continue;
            e.libraryPath = it->second.library;
            e.version     = it->second.version;
        }
    }

    // ── (2) THE PLATFORM REALIZATION ORACLE ─────────────────────────────────
    //
    // The same `ffi::realizeShippedExternSymbols` the semantic tier asks about a
    // hand-written C prototype. A `.s` that writes `call putchar` and a `.c`
    // that writes `extern int putchar(int);` are making the SAME claim about the
    // platform, so they must reach the same descriptor row and produce a
    // byte-identical import — the corpus is the single owner of "which image
    // owns this name here" (UCRT-P4 Decision 1).
    // ★★ THE CORPUS IS KEYED ON THE CANONICAL C IDENTIFIER; A `.s` WROTE THE
    // DECORATED ONE. ✔MEASURED: a Mach-O source spells `call _putchar` (gas
    // requires it, and `asm_text_to_lir` takes the name VERBATIM by design), so
    // looking the written name up in the descriptor index missed on every
    // decorating format while elf and pe — where the decoration is empty — bound
    // fine. That is the worst shape of gap: a feature that works on two of three
    // formats with no diagnostic on the third.
    // ⚠ AND IT IS UN-DECORATED BY ASKING THE FORMAT, never by stripping a `_`:
    // `cSymbolDecoration().scheme` is the format's DECLARED rule and
    // `unapplyCMangling` is the one shared inverse (the same call `ingest()`
    // makes on binary-reader rows), so there is no `if (format == macho)` here
    // and a format that declares no decoration is a byte-identical no-op.
    //
    // ★★★ THE QUERY ITSELF NOW LIVES IN `realizePlatformExternsByOnBinaryName`,
    // WHICH THIS FILE'S OTHER ON-BINARY-NAME PRODUCER — THE ARCHIVE MEMBER — ALSO
    // ASKS (D-LK-ARCHIVE-MEMBER-EXTERN-LOSES-ITS-LIBRARY). Read its docblock for
    // the un-decoration round trip that used to be spelled here, and for the
    // REVERSE (realized-link-name) arm that was missing from it: a `.s` writing
    // `call _time64` names a symbol ucrtbase genuinely exports, yet the
    // canonical-name-only lookup could not find its row, because the corpus keys
    // that row under `time`. The POLICY below — which rows may bind, and the two
    // refusals — stays HERE, with the producer whose vocabulary it is worded in.
    auto const scheme  = format.cSymbolDecoration().scheme;
    std::vector<std::string> writtenNames;
    writtenNames.reserve(externs.size());
    for (auto const& e : externs) {
        writtenNames.push_back(e.libraryPath.empty() ? e.mangledName
                                                     : std::string{});
    }
    // D-FFI-DUPLICATE-SYMBOL-ACROSS-DESCRIPTORS-SILENTLY-ORDER-RESOLVED: see the
    // archive member's twin of this snapshot. The oracle reports a
    // cross-descriptor realization disagreement on `reporter` and still answers
    // the name; refusing is this caller's job, and it has to happen HERE —
    // `compileAsmUnit` takes its `asmEntry` snapshot AFTER this function returns,
    // so an error raised inside it would be carried past the tier gate.
    auto const corpusEntry = reporter.errorCount();
    auto const realized = realizePlatformExternsByOnBinaryName(
        writtenNames, target, format, cu.id(), grammar.name(), reporter);
    if (!tierClean(reporter, corpusEntry)) return false;
    // nullopt ⇒ the shippedLibs directory could not be located. A statement
    // about the ENVIRONMENT, never about the user's program: every name stays
    // unbound and the link tier judges the reference, exactly as before this
    // binder existed.
    if (!realized.has_value()) return true;

    std::string const formatKey{objectFormatKindName(format.kind())};
    for (std::size_t i = 0; i < externs.size(); ++i) {
        ExternImport& e = externs[i];
        if (!e.libraryPath.empty()) continue;
        auto const found = realized->find(e.mangledName);
        if (found == realized->end()) continue;
        // The corpus row's OWN C identifier — not necessarily the un-decorated
        // spelling this file wrote (see the helper's FORWARD/REVERSE note), and
        // it is what the two refusals below must reason about.
        std::string const& canonical = found->second.canonicalName;
        auto const row = &found->second;
        // ★★★ A `synthesize` ROW IS REFUSED HERE, LOUDLY, AND THAT REFUSAL IS
        // THE POINT RATHER THAN A GAP. A recipe row is realized as a
        // COMPILER-EMITTED BODY (pe `printf` is a shim over
        // `__stdio_common_vfprintf`), and the tier that emits those bodies is
        // MIR synthesis — which the `encode` tier does not run, by construction.
        // Binding the row as a plain import would request a symbol the image
        // genuinely does not export: ucrtbase exports no bare `printf`, so the
        // artifact would LINK CLEAN and die at LOAD with 0xC0000139 and no
        // diagnostic anywhere. That is the exact failure mode the eager-import
        // law exists to prevent, and it is worth a compile error.
        if (!row->row.recipeId.empty()) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::A_AsmTextUnsupported;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "assembly unit references '{}', which this platform realizes as "
                "a COMPILER-SYNTHESIZED body (shipped-descriptor recipe '{}' for "
                "object format '{}') rather than as a library export. The "
                "'encode' pipeline tier emits no such bodies — it runs no MIR "
                "synthesis, which is what the tier IS — and importing the name "
                "directly would produce a binary that links clean and then fails "
                "to LOAD, because '{}' exports no such symbol. Call the "
                "descriptor's underlying core symbol from this file, or route "
                "this reference through a compiled translation unit.",
                e.mangledName, row->row.recipeId, formatKey,
                [&] {
                    auto const lib = row->row.library.find(formatKey);
                    return lib != row->row.library.end() ? lib->second
                                                            : formatKey;
                }());
            reporter.report(std::move(d));
            return false;
        }
        // Fold the row's per-object-format `library` map on the active format —
        // the identical fold `buildCuMir` performs for the C path, keyed on the
        // same `objectFormatKindName`. A key ABSENT means the platform states no
        // image for this name on this format: leave it unbound (there is no
        // format-level default left to fall back to since UCRT-P4).
        auto const lib = row->row.library.find(formatKey);
        if (lib == row->row.library.end() || lib->second.empty()) continue;
        e.libraryPath = lib->second;
        e.version     = row->row.version;
        // ⚠ `mangledName` IS NOT REWRITTEN, and a `linkName` row is REFUSED
        // rather than silently re-spelled. `ShippedSymbolRealization::linkName`
        // replaces the C identifier a C declaration would have been decorated
        // from (Darwin's `fstat` → `fstat$INODE64`); a `.s` did not write a C
        // identifier, it wrote the on-binary symbol, and its own relocations
        // already name that symbol. Rewriting the import's name here would leave
        // the file's `call fstat` pointing at an import row named
        // `_fstat$INODE64` — a mismatch resolved at LOAD, not at build.
        // The comparison goes through `linkNameFor`, so the row's UNDECORATED
        // link base is decorated by the FORMAT's rule before it is measured
        // against what the file wrote — a source that already spelled
        // `_fstat$INODE64` is correct and must not be refused.
        if (!row->row.linkName.empty()
            && ffi::linkNameFor(canonical, /*asmLabel=*/{}, scheme,
                                row->row.linkName) != e.mangledName) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::A_AsmTextUnsupported;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "assembly unit references '{}', but on this target the platform "
                "exports that facility under the link name '{}' (shipped-"
                "descriptor per-target linkName). A '.s' names the ON-BINARY "
                "symbol directly, so DSS will not silently re-spell the "
                "reference: write '{}' in the source if that is the symbol you "
                "mean.",
                e.mangledName, row->row.linkName,
                ffi::linkNameFor(canonical, /*asmLabel=*/{}, scheme,
                                 row->row.linkName));
            reporter.report(std::move(d));
            return false;
        }
    }
    return true;
}

}  // namespace

std::optional<AssembledModule>
assembleAsmUnit(CompilationUnit const&     cu,
                GrammarSchema const&       grammar,
                TargetSchema const&        target,
                ObjectFormatSchema const&  format,
                DiagnosticReporter&        reporter,
                CompileOptions const&      opts) {
    std::span<EntryMaterialization const> const formatVerbs = format.entryVerbs();
    // ★★★ THE `encode` TIER'S WHOLE BODY. Compare it with `assembleUnit` above:
    // no semantic analysis, no CST→HIR, no HIR→MIR, no optimizer, no MIR→LIR,
    // no liveness, no register allocation, no two-address legalization, no
    // calling-convention materialization. That absence IS the tier — every one
    // of those passes would rewrite something the programmer wrote by hand, and
    // `pipeline_entry_config.hpp` names the three that would do it silently.
    // ⚠ AND IT IS AN ABSENCE BY CONSTRUCTION, NOT BY A FLAG: there is no MIR
    // module here for a MIR pass to run over, which is exactly why the facet has
    // no `optimize: false` sibling key. Two sources of truth for "is this
    // optimized" is how they drift apart.
    if (cu.trees().empty()) {
        // An empty unit is a valid empty object, matching every other language.
        AssembledModule empty;
        empty.cuId = cu.id();
        return empty;
    }
    // ★★★ THE ENTRY NAMES ARE **NOT** INTERSECTED WITH THE FORMAT'S VERBS, AND
    // THIS COMMENT USED TO CLAIM THEY WERE. The claim sat above a `(void)
    // formatVerbs;` for the whole life of the function — fifteen lines
    // describing an intersection that no line performed. Corrected rather than
    // implemented, because the intersection is not merely unwritten: it has no
    // subject here.
    //
    // ✔MEASURED in `resolveProgramEntry` (this file): the intersection is
    // `EntryCandidate::verb ∈ formatVerbs`, and `verb` comes from
    // `SemanticModel`'s `SymbolRecord::entryVerb` — the SEMANTIC tier's record
    // of which declared entry SHAPE a definition's signature matched. The
    // `encode` tier runs no semantic analysis by construction (there is no MIR
    // module, which is the whole point of the tier), and a `.s` label carries
    // no signature to match a shape against anyway. So an assembly label has no
    // materialization verb, and "intersect the labels' verbs with the format's"
    // is not a thing that can be computed — not a TODO.
    //
    // ⚠ WHAT `formatVerbs` CAN HONESTLY ANSWER HERE, AND NOW DOES: whether this
    // format STARTS A PROGRAM at all. A non-empty declared verb set means it
    // does (`ObjectFormatData::validate()` pins non-empty ⟺ exec-flavored, in
    // both directions), and a dialect that declares no `entryLabels` can then
    // never resolve one — every function would come back unelected and the
    // trampoline would call whatever landed at functions[0]. That is the
    // silently-wrong-entry outcome the previous comment named as the thing that
    // must not happen, while the code it introduced did nothing to prevent it.
    std::span<std::string const> const entryNames{
        grammar.assembly().entryLabels};
    if (!formatVerbs.empty() && entryNames.empty()) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::K_ProgramEntryUndefined;
        d.severity = DiagnosticSeverity::Error;
        d.actual = std::format(
            "assembly dialect '{}' declares no 'assembly.entryLabels', so no "
            "label in a '{}' file can be this program's entry — but the object "
            "format STARTS A PROGRAM (it declares {} entry-materialization "
            "verb(s)), so it needs one. Without an elected entry the emitted "
            "trampoline would call whichever function happened to land first. "
            "Add the entry spelling(s) to the dialect's 'assembly.entryLabels', "
            "or build this unit for a relocatable format.",
            grammar.name(), grammar.name(), formatVerbs.size());
        reporter.report(std::move(d));
        return std::nullopt;
    }

    AssembledModule merged;
    merged.cuId = cu.id();
    for (auto const& tree : cu.trees()) {
        auto lowered = lowerAsmTextToLir(tree, grammar, target, entryNames,
                                         reporter);
        if (!lowered) return std::nullopt;

        // D-ASM-EXTERN-CALL-CANNOT-BIND-A-LIBRARY +
        // D-ASM-RESOLVE-LIBRARY-SILENTLY-IGNORED-ON-ENCODE-TIER: the rows
        // arrive here UNBOUND by construction (a `.s` states no owning image
        // and gas has no extern directive), and BEFORE this call nothing on
        // this route ever asked the platform — so `call putchar` reached the
        // EXEC link with an empty `libraryPath` and was refused as an undefined
        // symbol, on every format. The binder runs between the lowering and the
        // assembler because `assemble()` copies `externImports` verbatim: a row
        // bound after that call would be bound too late.
        if (!bindAsmExternImports(lowered->externImports, cu, grammar, target,
                                  format, opts, reporter)) {
            return std::nullopt;
        }

        auto const asmEntry = reporter.errorCount();
        std::vector<MirInstId> lirToMir(lowered->lir.instCount(), InvalidMirInst);
        // D-ASM-EXTERNAL-CALL-UNREPRESENTABLE: the extern rows ride the SAME
        // channel the ordinary pipeline uses — `assemble()`'s `externs` span,
        // which `lowerCuMirToAssembly` fills from `MirToLirResult::
        // externImports`. Assigning to `assembled.externImports` afterwards
        // would have worked and would have been a SECOND convention for one
        // fact; the assembler already owns the copy (`asm.cpp`'s
        // `result.externImports.assign(...)`).
        auto assembled = assemble(lowered->lir, target, lirToMir, reporter,
                                  lowered->externImports);
        if (!assembled.ok() || !tierClean(reporter, asmEntry)) {
            return std::nullopt;
        }
        assembled.cuId            = cu.id();
        assembled.symbols         = std::move(lowered->symbols);
        assembled.userEntrySymbol = lowered->userEntrySymbol;
        // D-ASM-DATA-ITEMS-NOT-WIRED-INTO-THE-DRIVER: without this line the
        // data-defining directives' bytes were COMPUTED AND DROPPED. `assemble()`
        // builds its module from the LIR, and LIR carries no data — so the only
        // copy of `.data`/`.byte`/`.quad`/`.zero`'s output is the one the text
        // walker returns, and nothing else was reading it.
        //
        // ★ THE ONE-LINE TWIN OF THE C PATH'S `assembled.dataItems =
        // std::move(dataItems)` (this file, `assembleUnit`), and deliberately the
        // SAME statement rather than a merge: both arms produce the complete
        // item set for their unit, and an `insert` here would silently tolerate
        // an `assemble()` that had started emitting its own.
        assembled.dataItems       = std::move(lowered->dataItems);
        // ★★★ D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED: attach the CALL FRAME
        // INFORMATION this file's `.cfi_*` directives stated. Runs AFTER
        // `assemble()` for the identical reason the C path's own attach does —
        // the byte offsets a rule resolves against are what `assemble()`
        // computes — and through the SAME `asm/asm_cfi.hpp` resolver, so a
        // hand-written frame and a compiled frame become an `.eh_frame` FDE by
        // one code path. Without this line the directives are parsed,
        // validated, folded and DROPPED, which is the defect itself.
        // ⚠ `cfiInitial` is engaged only when the file described a frame; a
        // `.s` with no `.cfi_*` at all carries no entry state and needs no
        // attach (its `perFuncCfi` slots are all `nullopt`).
        if (lowered->cfiInitial.has_value()
            && !attachAssemblyCfi(assembled, lowered->perFuncCfi,
                                  *lowered->cfiInitial, reporter)) {
            return std::nullopt;
        }
        // D-ASM-INTERIOR-LABELS-NOT-ADDRESSABLE-AT-AN-OFFSET: bind the block
        // symbols that only a DATA slot names (a hand-written jump table). This
        // runs AFTER `assemble()` for the same reason the C jump-table arm does
        // — `blockByteOffsets` is what `assemble()` computes — and through the
        // SAME `bindBlockSymbol` helper, so "symbol S is block B of function F"
        // has one implementation for both source languages.
        //
        // ⚠ `alreadyBound` IS SEEDED FROM WHAT THE ENCODER ALREADY BOUND. A
        // label that is BOTH `lea`'d and listed in a table (the ordinary
        // computed-goto-plus-jump-table shape) is one symbol reached twice, and
        // binding it twice would hand the linker two VAs for it.
        for (auto const& b : lowered->blockSymbolBindings) {
            if (b.funcIndex >= assembled.functions.size()) {
                report(reporter, DiagnosticCode::A_AsmTextUnsupported,
                       DiagnosticSeverity::Error,
                       std::format(
                           "a data slot takes the address of a label in "
                           "function #{}, but this unit assembled {} "
                           "function(s) — the assembly lowering and the "
                           "assembler disagree about this file's function list "
                           "(D-ASM-INTERIOR-LABELS-NOT-ADDRESSABLE-AT-AN-"
                           "OFFSET)",
                           b.funcIndex, assembled.functions.size()));
                return std::nullopt;
            }
            AssembledFunction& outFn = assembled.functions[b.funcIndex];
            std::unordered_set<std::uint32_t> alreadyBound;
            for (auto const& bs : outFn.blockSymbols) {
                alreadyBound.insert(bs.symbol.v);
            }
            if (!bindBlockSymbol(outFn, b.lirBlockV, b.symbol, alreadyBound)) {
                report(reporter, DiagnosticCode::A_AsmTextUnsupported,
                       DiagnosticSeverity::Error,
                       std::format(
                           "a data slot takes the address of a label whose "
                           "basic block (id {}) the assembler published no byte "
                           "offset for — the relocation would name a symbol "
                           "with no address (D-ASM-INTERIOR-LABELS-NOT-"
                           "ADDRESSABLE-AT-AN-OFFSET)",
                           b.lirBlockV));
                return std::nullopt;
            }
        }
        // ⚠ ONE TREE PER UNIT TODAY. A multi-file `.s` build would need each
        // file's SymbolIds namespaced before the merge; refusing is the honest
        // answer while that is unbuilt, because silently keeping the LAST tree
        // would drop every function in the others.
        if (merged.expectedFuncCount != 0 || !merged.functions.empty()) {
            report(reporter, DiagnosticCode::A_AsmTextUnsupported,
                   DiagnosticSeverity::Error,
                   "a compilation unit with more than one assembly source is "
                   "not yet lowered — each file's symbol space would have to be "
                   "namespaced before the modules merge, and keeping only one "
                   "of them would silently drop the others' functions");
            return std::nullopt;
        }
        merged = std::move(assembled);
    }
    return merged;
}

bool compileSingleUnit(CompilationUnit const&        cu,
                       GrammarSchema const&          grammar,
                       TargetSchema const&           target,
                       ObjectFormatSchema const&     format,
                       std::uint16_t                 callingConventionIndex,
                       std::filesystem::path const&  outPath,
                       DiagnosticReporter&           reporter,
                       CompileOptions const&         opts) {
    auto mod = assembleUnit(cu, grammar, target, format,
                            callingConventionIndex, reporter, opts);
    if (!mod) return false;
    return linkAndWrite(std::span<AssembledModule const>{&*mod, 1},
                        target, format, outPath, reporter);
}

// ═══════════════════════════════════════════════════════════════════════════
// THE THIN-LTO PER-TU IMPORT STAGE (D-OPT11-LAZY-IMPORT-EDGE, plan 22 §0.2)
// ═══════════════════════════════════════════════════════════════════════════
//
// ★★ STRICTLY ADDITIVE, AND THAT IS THE WHOLE SAFETY ARGUMENT. The whole-program
// merge and its single optimize still run afterwards, unchanged. This stage can
// therefore only move cross-CU inlining EARLIER and into PARALLEL; it cannot
// remove a splice the merged module would have made, and a TU it has nothing to
// offer is left byte-identical.
//
// Reached only from `--lto=thin`. Every build that does not ask for it takes the
// identical path it took before this function existed — including the
// `PhaseTimers::read(Optimize).runs` count three driver-supply tests pin.
bool runThinLtoImportStage(std::span<CuMirModule>  cuMirs,
                           TargetSchema const&     target,
                           CSymbolDecorationScheme cSymDecor,
                           std::string_view        targetIdentity,
                           CompileOptions const&   opts,
                           substrate::IExecutor*   executor,
                           DiagnosticReporter&     reporter) {
    if (cuMirs.size() < 2) return true;   // nothing crosses a boundary

    // ── (1) THE POLICY, 100% FROM THE PIPELINE DOCUMENT ─────────────────────
    // `inlineThreshold` is the SAME cost bound the gate applies, so the
    // candidate filter cannot admit a callee the gate refuses on size; the
    // prefetch batch size is the Inlining fixpoint's own `max`, because that is
    // how many levels the in-module inliner collapses in one run. Nothing here
    // is a new invented constant, and nothing branches on a language, a target
    // or an object format.
    ::dss::opt::OptPipeline        loaded;
    ::dss::opt::OptPipeline const* effective = opts.pipelineOverride;
    if (effective == nullptr) {
        auto const name = resolvePipelineName(opts.config);
        if (!name.has_value()) return false;   // already refused upstream
        auto r = ::dss::opt::loadShippedPipeline(*name);
        if (!r.has_value()) {
            forwardConfigDiagnostics(r.error(), reporter);
            return false;
        }
        loaded    = std::move(r).value();
        effective = &loaded;
    }
    ::dss::mirsum::SummaryIndexPolicy policy;
    policy.inlineThreshold = effective->inlineThreshold;
    policy.maxImportDepth =
        effective->schedule.count == 0 ? 1u : effective->schedule.count;

    // ── (2) PER-TU SYMBOL NAME TABLES ───────────────────────────────────────
    //
    // ⚠ A FLAT TABLE, BUILT SERIALLY, NOT A CALLBACK INTO `SemanticModel`. N
    // importers run at once and each reads every OTHER TU's names; a callback
    // would put N threads inside one CU's symbol table. The KEY is the same
    // string `MergeCuInput::nameOf` produces — `linkNameFor` for a definition,
    // the import row's `mangledName` for a reference — because the import and
    // the whole-program merge must agree about what one symbol is.
    auto nameTableFor = [&](CuMirModule const& cu) {
        auto nameOf = [&](SymbolId sym) -> std::string {
            if (SymbolRecord const* r = cu.model.recordFor(sym)) {
                return dss::ffi::linkNameFor(r->name, r->asmName, cSymDecor,
                                             r->linkName);
            }
            for (ExternImport const& e : cu.externImports) {
                if (e.symbol.v == sym.v) return e.mangledName;
            }
            return std::string{};
        };
        std::vector<std::uint32_t> ids;
        Mir const&                 m = cu.mir;
        for (std::uint32_t i = 0; i < m.moduleFuncCount(); ++i)
            ids.push_back(m.funcSymbol(m.funcAt(i)).v);
        for (std::uint32_t i = 0; i < m.moduleGlobalCount(); ++i)
            ids.push_back(m.globalSymbol(m.globalAt(i)).v);
        for (ExternImport const& e : cu.externImports) ids.push_back(e.symbol.v);
        for (auto const& [v, recipe] : cu.libraryShimRecipes) ids.push_back(v);
        for (std::uint32_t fi = 0; fi < m.moduleFuncCount(); ++fi) {
            MirFuncId const f = m.funcAt(fi);
            for (std::uint32_t bi = 0; bi < m.funcBlockCount(f); ++bi) {
                MirBlockId const b = m.funcBlockAt(f, bi);
                for (std::uint32_t ii = 0; ii < m.blockInstCount(b); ++ii) {
                    MirInstId const inst = m.blockInstAt(b, ii);
                    if (m.instOpcode(inst) == MirOpcode::GlobalAddr)
                        ids.push_back(m.globalAddrSymbol(inst).v);
                    else if (m.instOpcode(inst) == MirOpcode::BlockAddressExport)
                        ids.push_back(m.blockAddressExportSymbol(inst).v);
                }
            }
        }
        std::uint32_t maxV = 0;
        for (std::uint32_t v : ids) maxV = std::max(maxV, v);
        std::vector<std::string> table(static_cast<std::size_t>(maxV) + 1);
        for (std::uint32_t v : ids) {
            if (table[v].empty()) table[v] = nameOf(SymbolId{v});
        }
        return table;
    };

    std::vector<std::vector<std::string>>      nameTables;
    std::vector<::dss::mirsum::ModuleSummary>  summaries;
    nameTables.reserve(cuMirs.size());
    summaries.reserve(cuMirs.size());
    for (CuMirModule const& cu : cuMirs) nameTables.push_back(nameTableFor(cu));
    for (std::size_t i = 0; i < cuMirs.size(); ++i) {
        ::dss::mirsum::SummaryCuInput in;
        in.mir    = &cuMirs[i].mir;
        in.nameOf = [&tbl = nameTables[i]](SymbolId s) {
            return s.v < tbl.size() ? tbl[s.v] : std::string{};
        };
        in.externImports = cuMirs[i].externImports;
        // ⚠ NO MODULE DIGEST, AND EMPTY IS THE HONEST VALUE. The digest exists
        // to key a CACHED post-import object, and this stage caches nothing — it
        // runs in one process over modules that are all in memory. Filling it
        // with something unique-LOOKING (a CU index, a source path) would be
        // exactly the P36 `CuBuildKey::languageName` mistake: a value that is
        // not unique BY CONSTRUCTION wearing a key's face. A Tier-2 consumer
        // must refuse an empty digest rather than treat it as one.
        in.moduleDigest   = std::string{};
        in.targetIdentity = std::string{targetIdentity};
        summaries.push_back(::dss::mirsum::buildModuleSummary(in));
    }

    std::vector<::dss::mirsum::LazyImportCu> views;
    views.reserve(cuMirs.size());
    for (std::size_t i = 0; i < cuMirs.size(); ++i) {
        views.push_back(::dss::mirsum::LazyImportCu{
            &cuMirs[i].mir, &cuMirs[i].model.lattice().interner(),
            &nameTables[i], cuMirs[i].externImports,
            &cuMirs[i].libraryShimRecipes});
    }

    auto index = ::dss::mirsum::buildSummaryIndex(summaries, policy, reporter);
    if (!index.has_value()) return false;
    if (!::dss::mirsum::summariesDescribeModules(views, summaries, reporter))
        return false;

    // ── (3) THE PER-TU STAGE, IN PARALLEL ───────────────────────────────────
    //
    // Every input is const, so the jobs share nothing but the index and the
    // summaries — both read-only. Each writes its own outcome slot and its own
    // scratch reporter, and the scratches drain in CU-index order after the
    // join, exactly as the two existing CU batches do.
    std::string const srcLanguage{
        cuMirs[0].model.lattice().registry().sourceLanguage()};
    DiagnosticReporter::Config scratchCfg = reporter.config();
    scratchCfg.maxDiagnostics = std::numeric_limits<std::size_t>::max();
    scratchCfg.maxPerCode     = std::numeric_limits<std::size_t>::max();
    scratchCfg.dedupWindow    = 0;
    std::vector<DiagnosticReporter> scratch;
    scratch.reserve(cuMirs.size());
    for (std::size_t i = 0; i < cuMirs.size(); ++i)
        scratch.emplace_back(scratchCfg);

    std::vector<::dss::mirsum::LazyImportOutcome> outcomes(cuMirs.size());
    auto runOne = [&](std::size_t i) {
        outcomes[i] = ::dss::mirsum::lazyImportOptimize(
            static_cast<std::uint32_t>(i), views, summaries, *index, policy,
            srcLanguage,
            [&](Mir& m, TypeInterner const& in,
                std::span<ExternImport const> ex) {
                return optimizeModule(m, target, in, opts,
                                      PipelineStage::Program, scratch[i], ex);
            },
            scratch[i]);
    };
    if (executor == nullptr) {
        for (std::size_t i = 0; i < cuMirs.size(); ++i) runOne(i);
    } else {
        std::latch done{static_cast<std::ptrdiff_t>(cuMirs.size())};
        for (std::size_t i = 0; i < cuMirs.size(); ++i) {
            executor->submit([&, i] {
                struct CountDownGuard {
                    std::latch& latch;
                    ~CountDownGuard() { latch.count_down(); }
                } const guard{done};
                runOne(i);
            });
        }
        done.wait();
    }

    // ── (4) INSTALL, SERIALLY, IN CU ORDER ──────────────────────────────────
    // Nothing is installed while a job could still be reading it: a rewritten
    // module is also a potential import SOURCE for every other importer, which
    // is exactly why `lazyImportOptimize` returns its module instead of writing
    // one through a reference.
    bool ok = true;
    for (std::size_t i = 0; i < cuMirs.size(); ++i) {
        copyDiagnostics(scratch[i], reporter);
        if (!outcomes[i].ok) { ok = false; continue; }
        if (!outcomes[i].mir.has_value()) continue;   // imported nothing
        cuMirs[i].mir                 = std::move(*outcomes[i].mir);
        cuMirs[i].importedHost        = std::move(outcomes[i].host);
        cuMirs[i].importedSymbolNames = std::move(outcomes[i].symbolNames);
        cuMirs[i].externImports       = std::move(outcomes[i].externImports);
        cuMirs[i].libraryShimRecipes  = std::move(outcomes[i].synthRecipes);
        cuMirs[i].usesImportedLattice = true;
    }
    return ok;
}

} // namespace dss
