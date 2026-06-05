#include "program/compile_pipeline.hpp"

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "asm/asm.hpp"
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
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/lowering/hir_to_mir.hpp"
#include "opt/optimizer.hpp"

#include <algorithm>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Plan 14 LK10 cycle 2 — driver pipeline kernel.

namespace dss {

void copyDiagnostics(DiagnosticReporter const& src,
                     DiagnosticReporter&       dst) {
    for (auto const& d : src.all()) dst.report(d);
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

// BUILD half (Cycle 24): semantic analysis → HIR → FFI synthesis → MIR → optimize for
// ONE CompilationUnit, returning the `CuMirModule` the LOWER half consumes. The
// `SemanticModel` is MOVED into the result so its `TypeLattice` interner stays alive
// past this call — `lowerCuMirToAssembly` re-opens it for MIR→LIR + the symbol-table
// populate. Returns nullopt on any front-half tier failure (diagnostics via `reporter`).
std::optional<CuMirModule> buildCuMir(CompilationUnit const&        cu,
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
    auto model = analyze(std::move(borrowed));
    copyDiagnostics(model.diagnostics(), reporter);
    if (model.hasErrors() || !tierClean(reporter, semEntry)) {
        return std::nullopt;
    }

    // 2. CST → HIR.
    auto const hirEntry = reporter.errorCount();
    auto hir = lowerToHir(model, reporter);
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
        // `hir->externDecls`).
        std::vector<ffi::ExternDeclRef> refs;
        refs.reserve(hir->externDecls.size());
        for (auto const& r : hir->externDecls) {
            // D-CSUBSET-EXTERN-LIBRARY-SYNTAX closure (step 13.3):
            // propagate the per-symbol library override decoded by
            // the lowerer from the optional trailing string literal.
            // Empty = use format-level default (existing behavior).
            refs.push_back({r.node, r.canonicalName, r.libraryOverride});
        }

        auto const ffiEntry = reporter.errorCount();
        auto const ffiResult = ffi::synthesizeFfiFromSourceDecls(
            refs, importLibrary, target, format, ffiMap, reporter);
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
    auto mir = lowerToMir(hir->hir, hir->literalPool,
                          model.lattice().interner(), reporter,
                          &hir->sourceMap, mirCfg, &ffiMap,
                          &hir->linkageMap);
    if (!mir.ok || !tierClean(reporter, mirEntry)) {
        return std::nullopt;
    }

    // 3.5. MIR optimizer (plan 22). Pipeline resolution:
    //   (a) explicit `opts.pipelineOverride` (examples_runner's
    //       differential-verify arm, unit tests) — bypasses the JSON
    //       registry.
    //   (b) shipped JSON via `resolvePipelineName(opts.config)`
    //       (Debug → "debug" / Release → "release").
    auto const optEntry = reporter.errorCount();
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
            return std::nullopt;
        }
        auto loaded = ::dss::opt::loadShippedPipeline(*name);
        if (!loaded.has_value()) {
            // The pipeline file ships with the repo; a load failure
            // here is a deploy/install bug. Drain config diagnostics
            // so the user sees the JSON-path context.
            forwardConfigDiagnostics(loaded.error(), reporter);
            return std::nullopt;
        }
        loadedPipeline = std::move(loaded).value();
        effectivePipeline = &loadedPipeline;
    }
    auto const optResult = ::dss::opt::optimize(
        mir.mir, target, model.lattice().interner(),
        *effectivePipeline, reporter);
    if (!optResult.ok || !tierClean(reporter, optEntry)) {
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
    };
    return cuMir;
}

// LOWER half (Cycle 24): MIR → LIR → liveness → regalloc → rewrite → legalize →
// callconv → assemble → the LK11a symbol-table populate → the user-entry scan,
// producing the `AssembledModule` (NO link, NO write). Consumes the `CuMirModule`
// the BUILD half handed across the MIR/LIR seam: its `externImports` are MOVED into
// MIR→LIR; its `mir` + `model` (interner) are read. Returns nullopt on any back-half
// tier failure (diagnostics already emitted via `reporter`).
std::optional<AssembledModule>
lowerCuMirToAssembly(CuMirModule& cuMir, DiagnosticReporter& reporter) {
    // Re-bind the seam state under the names the back-half body uses. `model` stays a
    // mutable reference — MIR→LIR + the optimizer-minted-type reads re-open its
    // interner (`lattice().interner()` is non-const). `grammar`/`target` are the
    // caller's schemas (live across both driver loops); the cuId is stamped below.
    auto&                     model                  = cuMir.model;
    GrammarSchema const&      grammar                = *cuMir.grammar;
    TargetSchema const&       target                 = *cuMir.target;
    std::uint16_t const       callingConventionIndex = cuMir.callingConventionIndex;

    // 4. MIR → LIR (vreg-based). Extern imports propagate through.
    auto const lirEntry = reporter.errorCount();
    auto lir = lowerToLir(cuMir.mir, target,
                          model.lattice().interner(), reporter,
                          std::move(cuMir.externImports));
    if (!lir.ok || !tierClean(reporter, lirEntry)) {
        return std::nullopt;
    }

    // 5. Liveness analysis (input to regalloc).
    auto const liveness = analyzeLiveness(lir.lir);

    // 6. Register allocation.
    auto const allocEntry = reporter.errorCount();
    auto const alloc = allocateRegisters(lir.lir, target, liveness,
                                          callingConventionIndex, reporter);
    if (!alloc.ok() || !tierClean(reporter, allocEntry)) {
        return std::nullopt;
    }

    // 7. Rewrite vregs → physical registers.
    auto const rewriteEntry = reporter.errorCount();
    auto rewritten = rewriteWithAllocation(lir.lir, target, alloc, reporter);
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
    auto const ccEntry = reporter.errorCount();
    auto cc = materializeCallingConvention(legal.lir, target, alloc, reporter);
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
    std::vector<MirInstId> lirToMir(cc.lir.instCount(), InvalidMirInst);
    auto assembled = assemble(cc.lir, target, lirToMir, reporter,
                              lir.externImports);
    if (!assembled.ok() || !tierClean(reporter, asmEntry)) {
        return std::nullopt;
    }

    // D-CSUBSET-MULTI-FN-WIN64-CC (step 13.5 cycle 2 post-fold,
    // 2026-06-03): plumb the source-language entry-function symbol
    // into AssembledModule.userEntrySymbol. The language config's
    // declaration rules carry an `implicitReturnZeroForFunctionNames`
    // list — c-subset names "main" there; toy / future languages
    // name their own entry. Scan the semantic model's symbol records
    // for a function symbol whose name appears in ANY decl rule's
    // entry-name list, and stamp it on the AssembledModule so the
    // trampoline injector's `resolveUserEntrySymbol` doesn't fall
    // through to the `functions[0]` default (which silently picked
    // the FIRST-declared function — wrong when the entry isn't
    // declared first in source order).
    //
    // Source-agnostic: the trigger comes from grammar config, not
    // a hardcoded "main" string. Multiple symbols matching the
    // entry-name list fail-loud (silent-failure post-fold HIGH #1
    // 2026-06-03 — never pick silently between candidates).
    if (!assembled.userEntrySymbol.has_value()) {
        std::vector<std::string_view> entryNames;
        for (auto const& decl : grammar.semantics().declarations) {
            for (auto const& n : decl.implicitReturnZeroForFunctionNames) {
                entryNames.push_back(n);
            }
        }
        if (!entryNames.empty()) {
            // Silent-failure HIGH #1 post-fold (2026-06-03): walk
            // EVERY function symbol matching the entry-name list;
            // fail-loud on multiple matches rather than silently
            // first-match-wins. A future language declaring
            // `["main", "_start"]` (both defined in the source)
            // OR a duplicate-name config (same name across two
            // decl rules) would otherwise re-introduce the silent
            // wrong-entry bug class this fix is supposed to close.
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
                return std::nullopt;
            }
            if (matches.size() == 1) {
                assembled.userEntrySymbol = matches[0];
            }
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
    auto dataItems = lowerMirGlobalsToDataItems(
        cuMir.mir, model.lattice().interner(), reporter);
    if (!tierClean(reporter, asmEntry)) {
        // Any per-global encoding error already raised a loud
        // diagnostic via the function's internal `emit`.
        return std::nullopt;
    }
    assembled.dataItems = std::move(dataItems);

    // D-LK4-3: stamp the owning CompilationUnit's id so the linker keys this
    // module's symbols by `(cuId, SymbolId)`. Single-CU build → one cuId; the
    // compound key only matters once LK11 links multiple CUs in one image.
    assembled.cuId = cuMir.cuId;

    // LK11a: build the per-module symbol table the linker matches by NAME across
    // CUs (cross-CU resolution + weak-vs-strong). One entry per DEFINED function /
    // global — extern imports are references, not definitions, and are carried
    // separately in `externImports`. The name comes from the semantic symbol table
    // (raw declared identifier, no mangling); binding/visibility from MIR. IRs stay
    // numeric — the name is assembled here, where `model` + `mir` are both in scope,
    // not threaded through MIR/LIR. A defined symbol with no semantic record, or a
    // Global/Weak with an empty name, is a producer-contract breach the linker could
    // only mis-resolve cross-CU — fail loud here rather than emit an unmatchable
    // symbol. (Source/target/format-agnostic: reads the symbol table + MIR linkage,
    // no language/CPU/format branch.)
    {
        auto appendSym = [&](SymbolId sym, SymbolBinding bind,
                             SymbolVisibility vis) -> bool {
            SymbolRecord const* rec = model.recordFor(sym);
            if (rec == nullptr) {
                // No semantic record → a compiler-SYNTHESIZED, module-internal symbol:
                // e.g. a string-literal rodata global (its SymbolId is minted ABOVE the
                // semantic range per D-LK4-RODATA-PRODUCER-STRING, so it lands out of
                // range here, never a wrong-record collision), or a synthesized init
                // thunk. Such a symbol is never referenced across CUs by name — exclude
                // it from the cross-CU symbol table; it stays module-private, resolved
                // intra-CU by its SymbolId. (A genuinely corrupted producer id — OOR for
                // a real source symbol — is indistinguishable from a synthetic one at
                // this layer, so it is likewise skipped rather than fail-loud; such
                // corruption surfaces downstream where the SymbolId is actually consumed.
                // That is the honest reason this is a skip, not a hard error.)
                return true;
            }
            if (rec->name.empty() && bind != SymbolBinding::Local) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::K_SymbolUndefined;
                d.severity = DiagnosticSeverity::Error;
                d.actual   = std::format(
                    "compile_pipeline: externally-visible symbol #{} has an empty "
                    "name — the linker cannot match it across CUs (LK11a).", sym.v);
                reporter.report(std::move(d));
                return false;
            }
            assembled.symbols.push_back(ModuleSymbol{sym, rec->name, bind, vis});
            return true;
        };
        for (std::uint32_t i = 0; i < cuMir.mir.moduleFuncCount(); ++i) {
            MirFuncId const fid = cuMir.mir.funcAt(i);
            if (!appendSym(cuMir.mir.funcSymbol(fid), cuMir.mir.funcBinding(fid),
                           cuMir.mir.funcVisibility(fid))) {
                return std::nullopt;
            }
        }
        for (std::uint32_t i = 0; i < cuMir.mir.moduleGlobalCount(); ++i) {
            MirGlobalId const gid = cuMir.mir.globalAt(i);
            if (!appendSym(cuMir.mir.globalSymbol(gid), cuMir.mir.globalBinding(gid),
                           cuMir.mir.globalVisibility(gid))) {
                return std::nullopt;
            }
        }
    }

    // Assembly complete — return the per-CU module; linking + writing is the shared
    // `linkAndWrite` phase below, so N CUs can each assemble before one merged link.
    return assembled;
}

// Link N assembled CUs into one image + commit to disk. N==1 is the v1 single-CU
// path; N>1 the linker merges the CUs (LK11a resolution + LK11b byte emission)
// before the format walker emits. `outPath` is caller-owned.
bool linkAndWrite(std::span<AssembledModule const> modules,
                  TargetSchema const&              target,
                  ObjectFormatSchema const&        format,
                  std::filesystem::path const&     outPath,
                  DiagnosticReporter&              reporter) {
    auto const linkEntry = reporter.errorCount();
    auto image = linker::link(modules, target, format, reporter);
    if (!image.ok() || !tierClean(reporter, linkEntry)) {
        return false;
    }
    return linker::writeImage(image, outPath, reporter);
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
    return lowerCuMirToAssembly(*cuMir, reporter);
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
