#include "program/compile_pipeline.hpp"

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "asm/asm.hpp"
#include "core/substrate/large_stack_call.hpp"  // D-PARSE-DEEP-FRONTEND-STACK: BUILD half on a large stack
#include "core/substrate/phase_timers.hpp"      // c97: per-phase --time accumulation
#include "core/types/parse_diagnostic.hpp"
#include "ffi/ingest.hpp"
#include "hir/attributes/ffi_metadata.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "link/linker.hpp"
#include "link/writer.hpp"
#include "lir/lir_2addr_legalize.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_regalloc.hpp"
#include "lir/lir_rewrite.hpp"
#include "lir/lir_wide_call_args.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/lowering/hir_to_mir.hpp"
#include "mir/merge/mir_merge.hpp"  // MergedMirModule (lowerMergedToAssembly consumes it)
#include "mir/merge/synth_pe_startup.hpp"  // synthesizePeStartup (c111 D-RUNTIME-PE-MAIN-ARGS)
#include "opt/optimizer.hpp"
#include "opt/passes/prune_unreachable.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// Plan 14 LK10 cycle 2 — driver pipeline kernel.

namespace dss {

void copyDiagnostics(DiagnosticReporter const& src,
                     DiagnosticReporter&       dst) {
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
                    DiagnosticReporter&   reporter) {
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
        effectivePipeline = &loadedPipeline;
    }
    auto const optResult = ::dss::opt::optimize(
        mir, target, interner, *effectivePipeline, reporter);
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
// config-driven cap, c-subset = 1024) and as defense-in-depth for any not-yet-
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
    std::optional<AggregateLayoutParams> analyzeLayout;
    if (target.aggregateLayoutLoaded()) {
        analyzeLayout = target.aggregateLayout();
        analyzeLayout->bitFieldStrategy = effectiveBfStrategy;
    }
    // FC12b (D-FC12B-WIN64-VARIADIC-CALLEE, BLOCKER-2): thread the RESOLVED CC's
    // va_list lowering strategy so the semantic `va_list`-type injection sizes the
    // `ap` local per ABI (SysV __va_list_tag[1]=24B vs Win64 char*=8B). Read from
    // the SAME resolved CC the MirLoweringConfig reads its `vaListLayout` from
    // (below); `nullopt` when the CC declares no variadic-callee ABI ⇒ the
    // SysV-family default (a CC with no vaListLayout has no variadic-callee surface
    // anyway, so the injected type is inert).
    std::optional<VaListStrategy> analyzeVaStrategy;
    if (auto const* cc = target.callingConvention(callingConventionIndex);
        cc != nullptr && cc->vaListLayout.has_value()) {
        analyzeVaStrategy = cc->vaListLayout->strategy;
    }
    // c97: sequential per-phase scoping via optional emplace — emplace
    // destroys the prior Scope (closing its accumulation window) BEFORE
    // opening the next, and any early return closes the live one.
    std::optional<substrate::PhaseTimers::Scope> phase;
    phase.emplace(substrate::CompilePhase::Semantic);
    auto model = analyze(
        std::move(borrowed), format.dataModel(), analyzeLayout, analyzeVaStrategy,
        format.kind(),       // c8: the active object-format → per-target availability gate
        target.name());      // plan 25: the active arch → per-target shipped-struct variant selector
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

    // 2.5. FFI metadata synthesis for source-declared externs
    //      (FF6 Slice 2, 2026-06-02). When the language schema's
    //      `externDecl` rule declares an `externLibraryByFormat`
    //      entry for the active object format, every extern the
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
    //      Agnostic over CPU + format: the per-format library
    //      identity comes from `grammar.semantics().declarations`'s
    //      `externLibraryByFormat` map keyed on
    //      `objectFormatKindName(format.kind())`. ELF / Mach-O
    //      hosts thread through the same call with their own
    //      library identities; a future grammar extension to allow
    //      `extern "otherlib.dll" int foo();` (anchored
    //      D-CSUBSET-EXTERN-LIBRARY-SYNTAX) layers a per-extern
    //      override on top of this map without touching the
    //      synthesis kernel.
    HirFfiMap ffiMap{hir->hir};
    if (!hir->externDecls.empty()) {
        // Find the active language's `externLibraryByFormat`
        // entry for this object format. Lives at SemanticConfig
        // scope (post-fold #1 2026-06-02): one map per language,
        // keyed on `objectFormatKindName(format.kind())`. Empty
        // string ⇒ no entry; synthesize() fails loud with
        // F_FfiNoImportLibraryForFormat upstream of the linker.
        auto const& libMap =
            grammar.semantics().externLibraryByFormat;
        std::string const formatKey{
            objectFormatKindName(format.kind())};
        std::string importLibrary;
        if (auto it = libMap.find(formatKey); it != libMap.end()) {
            importLibrary = it->second;
        }

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
        // override, which (per the existing ExternDeclRef contract) makes the
        // FFI synthesize stage fall back to the format-level default
        // `importLibrary` (externLibraryByFormat[format]).
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
            refs.push_back({r.node, r.canonicalName, resolvedLibs[i],
                            r.noLibraryBinding});
        }

        auto const ffiEntry = reporter.errorCount();
        phase.emplace(substrate::CompilePhase::SynthesizeFfi);
        auto const ffiResult = ffi::synthesizeFfiFromSourceDecls(
            refs, importLibrary, target, format, ffiMap, reporter);
        phase.reset();
        (void)ffiResult;  // shape inspected via reporter.errorCount()
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
    // FC6: thread the active target's aggregate-layout params + the format's data
    // model so HIR→MIR can fold `sizeof(T)` to T's byte size via the type_layout
    // engine. The target supplies the alignment rule, the format the pointer width.
    // D-CSUBSET-BITFIELD-ABI-EXACT: the bit-field strategy is FORMAT-determined —
    // overlay the resolved value so bit-field member-access/init lowers byte-ABI-
    // exact for the active object format (PE → msvc_straddle, ELF/Mach-O →
    // gnu_packed), not just whatever the target declared.
    mirCfg.aggregateLayout       = target.aggregateLayout();
    mirCfg.aggregateLayout.bitFieldStrategy = effectiveBfStrategy;
    mirCfg.aggregateLayoutLoaded = target.aggregateLayoutLoaded();
    mirCfg.dataModel             = format.dataModel();
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
                          &hir->sizeofVlaSymbol);   // VLA C2 (D-CSUBSET-VLA)
    phase.reset();
    if (!mir.ok || !tierClean(reporter, mirEntry)) {
        return std::nullopt;
    }

    // 3.5. MIR optimizer (plan 22). Pipeline resolution + optimize + tier-clean gate
    //      extracted to the shared `optimizeModule` (Cycle 26) so the N>1 whole-program
    //      path can run the SAME pipeline over the MERGED module. Pure code-motion —
    //      same arguments as the former inline block, so the per-CU output is identical.
    if (!optimizeModule(mir.mir, target, model.lattice().interner(), opts, reporter)) {
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
                         std::uint16_t                               callingConventionIndex,
                         CompilationUnitId                           cuId,
                         std::optional<ExternCallDispatch>           externCallDispatch,
                         std::optional<DataImportBinding>            dataImportBinding,
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
                          sehScopes);
    if (!lir.ok || !tierClean(reporter, lirEntry)) {
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

    // 8. Two-address legalize (post-regalloc).
    auto const legalEntry = reporter.errorCount();
    auto legal = legalizeTwoAddress(rewritten.lir, target, reporter);
    if (!legal.ok() || !tierClean(reporter, legalEntry)) {
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
    auto cc = materializeCallingConvention(legal.lir, target, alloc, reporter,
                                           sehFuncletParents,
                                           funcLocalAligns);
    if (!cc.ok() || !tierClean(reporter, ccEntry)) {
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

    // c114 (D-WIN64-PDATA-XDATA-UNWIND): project each function's frame
    // prologue (from the callconv pass's per-function FrameLayout) onto its
    // AssembledFunction, so a downstream unwind-table emitter (the pe64
    // writer's .pdata/.xdata builder) can describe the frame WITHOUT a lir/
    // dependency. Positional, mirroring the dataItems/userEntrySymbol
    // post-`assemble()` splices: cc.perFunc and assembled.functions are BOTH
    // guaranteed size == moduleFuncCount() (cc.ok() @561 + assembled.ok()
    // @577) and enumerated identically (asm.cpp populates via lir.funcAt(i),
    // the same order as perFunc's `1:1 with src.funcAt(i)`). The projection
    // is format-neutral frame data; only the pe64 writer reads it today.
    if (cc.perFunc.size() == assembled.functions.size()) {
        for (std::size_t fi = 0; fi < assembled.functions.size(); ++fi) {
            FrameLayout const& fl = cc.perFunc[fi];
            FrameUnwindInfo ui;
            ui.totalFrameSize      = fl.totalFrameSize;
            ui.stackProbePageBytes = fl.stackProbePageBytes;
            ui.usesStackProbe      = fl.stackProbePageBytes > 0
                                  && fl.totalFrameSize > fl.stackProbePageBytes;
            std::uint32_t const base = fl.savedRegAreaOffset();
            ui.savedRegs.reserve(fl.savedRegs.size());
            for (std::size_t i = 0; i < fl.savedRegs.size(); ++i) {
                LirReg const r = fl.savedRegs[i];
                FrameSavedReg sr;
                // The x64 unwind register number is the HARDWARE encoding
                // (rax=0..r15=15; xmm0=0..xmm15=15) — NOT the DSS physical
                // ORDINAL, which offsets FPRs past the 16 GPRs (xmm14 = ordinal
                // 30). GPR ordinal == hwEncoding so it was coincidentally right;
                // FPR needs the mapping. registerInfo(ordinal) is the source.
                auto const* ri = target.registerInfo(static_cast<std::uint16_t>(r.id));
                sr.regEncoding = ri != nullptr ? ri->hwEncoding
                                               : static_cast<std::uint16_t>(r.id);
                sr.isFpr       = r.regClass() != LirRegClass::GPR;
                sr.saveOffset  = base + static_cast<std::uint32_t>(i) * fl.slotSize;
                ui.savedRegs.push_back(sr);
            }
            assembled.functions[fi].unwind = std::move(ui);
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
            if (alreadyBound.insert(blkSym.v).second) {
                auto offIt = outFn.blockByteOffsets.find(lirBlockV);
                if (offIt == outFn.blockByteOffsets.end()) { tableOk = false; break; }
                outFn.blockSymbols.push_back(
                    SyntheticBlockSymbol{blkSym, offIt->second});
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
    // function's `FrameUnwindInfo.sehScopes`. Runs AFTER the unwind projection
    // (which created the FrameUnwindInfo) AND after assemble() (which populated each
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
        if (!outFn.unwind.has_value()) {
            // A SEH-guarding function ALWAYS has a frame (the unwind projection
            // attaches FrameUnwindInfo to every callconv'd function). A missing one
            // means the projection was skipped — fail loud, never emit a dangling
            // scope table with no UNWIND_INFO to host it.
            ParseDiagnostic d;
            d.code     = DiagnosticCode::K_NoMatchingObjectFormat;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "SEH scope on function index {} has no FrameUnwindInfo to host its "
                "scope table (D-WIN64-SEH-FUNCLETS)", desc.funcIndex);
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
        outFn.unwind->sehScopes.push_back(e);
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
    //     skip did. Byte-identical: every REAL c-subset func/global has a non-empty
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

namespace {

// Resolve the user-entry symbol for a SINGLE CU by scanning its SemanticModel's
// symbol records for a function symbol whose declared name appears in ANY decl
// rule's entry-name list (D-CSUBSET-MULTI-FN-WIN64-CC). Returns the matched
// SymbolId, or nullopt when no function matches. Fail-loud on MULTIPLE matches
// (sets `*ok=false` + reports K_SymbolUndefined) — the trampoline injector cannot
// silently pick one.
//
// This is the entry-name SCAN that used to live inline in the LOWER half (pre-
// Cycle-25); it stays CU-specific because it ENUMERATES the model's symbol records
// (which the merged path's symbol→name `nameOf` cannot express). The merged path's
// equivalent runs inside `mergeCuMirs` against the merged functions. Source-agnostic:
// the trigger is the grammar's `entryFunctionNames` (FC5 — falling back to the
// `implicitReturnZeroForFunctionNames` return-0 set when absent), never a hardcoded "main".
[[nodiscard]] std::optional<SymbolId>
resolveSingleCuUserEntry(SemanticModel const& model, GrammarSchema const& grammar,
                         DiagnosticReporter& reporter, bool& ok) {
    ok = true;
    std::vector<std::string_view> entryNames;
    for (auto const& decl : grammar.semantics().declarations) {
        auto const& names = decl.entryFunctionNames.empty()
                                ? decl.implicitReturnZeroForFunctionNames
                                : decl.entryFunctionNames;
        for (auto const& n : names) {
            entryNames.push_back(n);
        }
    }
    if (entryNames.empty()) return std::nullopt;

    // Silent-failure HIGH #1 (2026-06-03): walk EVERY function symbol matching the
    // entry-name list; fail-loud on multiple matches rather than silently
    // first-match-wins (a future `["main", "_start"]` both-defined, or a duplicate-
    // name config, would otherwise re-introduce the silent wrong-entry bug class).
    std::vector<SymbolId> matches;
    std::vector<std::string_view> matchNames;
    for (auto const& rec : model.symbols()) {
        if (rec.kind != DeclarationKind::Function) continue;
        bool const isEntry = std::any_of(
            entryNames.begin(), entryNames.end(),
            [&](std::string_view n){ return n == rec.name; });
        if (isEntry) {
            SymbolId const sym{static_cast<std::uint32_t>(
                &rec - model.symbols().data())};
            matches.push_back(sym);
            matchNames.push_back(rec.name);
        }
    }
    if (matches.size() > 1) {
        std::string list;
        for (std::size_t i = 0; i < matchNames.size(); ++i) {
            if (i) list += ", ";
            list += matchNames[i];
        }
        ParseDiagnostic d;
        d.code     = DiagnosticCode::K_SymbolUndefined;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::format(
            "compile_pipeline: ambiguous user-entry — {} "
            "function symbol(s) match the language's entry-"
            "name list (matched: {}). The trampoline injector "
            "cannot silently pick one. Declare distinct entry "
            "names per decl rule OR restrict the source to "
            "exactly one of these (D-CSUBSET-MULTI-FN-WIN64-CC "
            "ambiguity gate).", matches.size(), list);
        reporter.report(std::move(d));
        ok = false;
        return std::nullopt;
    }
    if (matches.size() == 1) return matches[0];
    return std::nullopt;
}

} // namespace

// LOWER half (single-CU): thin wrapper over the shared `lowerMirModuleToAssembly`.
// Binds the seam state from the `CuMirModule`: the per-CU type interner, a
// `nameOf` that reads the SemanticModel's symbol records, the CU's extern imports,
// and the entry symbol resolved by the CU-specific scan above. Produces output
// byte-identical to the pre-Cycle-25 monolith for any single-CU build.
std::optional<AssembledModule>
lowerCuMirToAssembly(CuMirModule&                       cuMir,
                     std::optional<ProcessArgs> const& processArgs,
                     DiagnosticReporter&               reporter) {
    SemanticModel&       model   = cuMir.model;
    GrammarSchema const& grammar = *cuMir.grammar;

    // Resolve the user-entry FIRST (fail-loud on ambiguity, exactly as the inline
    // scan did) so a multi-entry source halts before lowering — same observable
    // failure point as pre-Cycle-25. Non-const: `synthesizePeStartup` may retarget it.
    bool entryOk = true;
    std::optional<SymbolId> userEntry =
        resolveSingleCuUserEntry(model, grammar, reporter, entryOk);
    if (!entryOk) return std::nullopt;

    // c111 (D-RUNTIME-PE-MAIN-ARGS): single-CU counterpart of the merge-path synth
    // (program.cpp). When the target format fetches argc/argv via a CRT out-parameter
    // call (Windows), append the pre-main init that makes that call + forwards
    // (argc, argv) to the user entry, retargeting `userEntry` to it. A no-op for every
    // other mechanism / a no-arg entry. The CU is already per-CU-optimized here, so the
    // appended init skips the optimizer but is lowered like any other function; the
    // interner is the CU model's (the type space this CU's TypeIds index into).
    if (processArgs.has_value()) {
        if (!synthesizePeStartup(cuMir.mir, model.lattice().interner(),
                                 userEntry, cuMir.externImports,
                                 *processArgs, reporter)) {
            return std::nullopt;  // malformed argv parameter type — fail-loud reported.
        }
    }

    // c116 (D-WIN64-SEH-FUNCLETS): synthesize the SEH filter funclets + record the
    // scope ranges (post-optimize; the CU is already optimized here). Trigger =
    // presence of SehTryBegin — a no-op fast-return for the overwhelming majority
    // of TUs. Appends the __C_specific_handler personality import on demand.
    std::vector<MirSehScope> sehScopes;
    if (!synthesizeSehFunclets(cuMir.mir, model.lattice().interner(),
                               cuMir.externImports, sehScopes, reporter)) {
        return std::nullopt;  // unsupported SEH shape (c116b frontier) — fail-loud.
    }

    // `nameOf`: SymbolId → declared name. A SymbolId with no record (synthesized /
    // out-of-range) yields "" — the LK11a symbol-table populate then skips it as
    // module-private, exactly as the old `rec == nullptr` skip did.
    auto nameOf = [&](SymbolId s) -> std::string {
        SymbolRecord const* r = model.recordFor(s);
        return r ? r->name : std::string{};
    };

    return lowerMirModuleToAssembly(
        cuMir.mir, model.lattice().interner(), nameOf,
        std::move(cuMir.externImports), userEntry, *cuMir.target,
        cuMir.dataModel, cuMir.bitFieldStrategy,
        cuMir.callingConventionIndex, cuMir.cuId,
        cuMir.externCallDispatch, cuMir.dataImportBinding,
        cuMir.tlsAccess,
        std::move(sehScopes), reporter);
}

// LOWER half (merged whole-program): thin wrapper over the shared
// `lowerMirModuleToAssembly` for the N>1 merge path. `mergeCuMirs` already unified
// the N CUs into ONE module over a host lattice, resolved cross-CU calls to DIRECT
// intra-module calls (stripping the resolved extern imports), and computed the
// user-entry symbol. This drives that single module through the same LOWER body,
// so the linker downstream receives exactly ONE AssembledModule (no assembled-tier
// cross-CU thunk — the cycle-19 GOT-like rodata slot is never minted).
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
                      std::uint16_t       callingConventionIndex,
                      CompilationUnitId   cuId,
                      std::optional<ExternCallDispatch> externCallDispatch,
                      std::optional<DataImportBinding> dataImportBinding,
                      std::optional<TlsAccessInfo> tlsAccess,
                      std::vector<MirSehScope> sehScopes,
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
        dataModel, bitFieldStrategy, callingConventionIndex, cuId,
        externCallDispatch, dataImportBinding, tlsAccess,
        std::move(sehScopes), reporter);
}

// Link N assembled CUs into one image + commit to disk. N==1 is the v1 single-CU
// path; N>1 the linker merges the CUs (LK11a resolution + LK11b byte emission)
// before the format walker emits. `outPath` is caller-owned.
bool linkAndWrite(std::span<AssembledModule const> modules,
                  TargetSchema const&              target,
                  ObjectFormatSchema const&        format,
                  std::filesystem::path const&     outPath,
                  DiagnosticReporter&              reporter) {
    // c97: link phase — resolution + byte emission + image write.
    substrate::PhaseTimers::Scope linkPhase{substrate::CompilePhase::Link};
    auto const linkEntry = reporter.errorCount();
    auto image = linker::link(modules, target, format, reporter);
    if (!image.ok() || !tierClean(reporter, linkEntry)) {
        return false;
    }
    // D-OUTPUT-EXEC-BIT: mark the file executable iff the active object
    // format is an exec/image flavor (config-driven via the schema predicate,
    // never an arch/format identity branch) so a produced binary runs
    // directly without a manual `chmod +x`.
    return linker::writeImage(image, outPath, reporter, format.isImageFlavor());
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
    return lowerCuMirToAssembly(*cuMir, format.processArgs(), reporter);
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

} // namespace dss
