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

bool compileSingleUnit(CompilationUnit const&        cu,
                       GrammarSchema const&          grammar,
                       TargetSchema const&           target,
                       ObjectFormatSchema const&     format,
                       std::uint16_t                 callingConventionIndex,
                       std::filesystem::path const&  outPath,
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
        return false;
    }

    // 2. CST → HIR.
    auto const hirEntry = reporter.errorCount();
    auto hir = lowerToHir(model, reporter);
    if (!hir || !hir->ok || !tierClean(reporter, hirEntry)) {
        return false;
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
            return false;
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
    auto mir = lowerToMir(hir->hir, hir->literalPool,
                          model.lattice().interner(), reporter,
                          &hir->sourceMap, mirCfg, &ffiMap);
    if (!mir.ok || !tierClean(reporter, mirEntry)) {
        return false;
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
        mir.mir, target, model.lattice().interner(),
        *effectivePipeline, reporter);
    if (!optResult.ok || !tierClean(reporter, optEntry)) {
        return false;
    }

    // 4. MIR → LIR (vreg-based). Extern imports propagate through.
    auto const lirEntry = reporter.errorCount();
    auto lir = lowerToLir(mir.mir, target,
                          model.lattice().interner(), reporter,
                          std::move(mir.externImports));
    if (!lir.ok || !tierClean(reporter, lirEntry)) {
        return false;
    }

    // 5. Liveness analysis (input to regalloc).
    auto const liveness = analyzeLiveness(lir.lir);

    // 6. Register allocation.
    auto const allocEntry = reporter.errorCount();
    auto const alloc = allocateRegisters(lir.lir, target, liveness,
                                          callingConventionIndex, reporter);
    if (!alloc.ok() || !tierClean(reporter, allocEntry)) {
        return false;
    }

    // 7. Rewrite vregs → physical registers.
    auto const rewriteEntry = reporter.errorCount();
    auto rewritten = rewriteWithAllocation(lir.lir, target, alloc, reporter);
    if (!rewritten.ok || !tierClean(reporter, rewriteEntry)) {
        return false;
    }

    // 8. Two-address legalize (post-regalloc).
    auto const legalEntry = reporter.errorCount();
    auto legal = legalizeTwoAddress(rewritten.lir, target, reporter);
    if (!legal.ok() || !tierClean(reporter, legalEntry)) {
        return false;
    }

    // 9. Calling-convention materialization (prologue/epilogue,
    //    frame_load/frame_store; `arg` virtual-op rewrite is the
    //    ML7 cycle 2 gap — anchored D-LK10-2 for caller awareness).
    auto const ccEntry = reporter.errorCount();
    auto cc = materializeCallingConvention(legal.lir, target, alloc, reporter);
    if (!cc.ok() || !tierClean(reporter, ccEntry)) {
        return false;
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
        return false;
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
                return false;
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
        mir.mir, model.lattice().interner(), reporter);
    if (!tierClean(reporter, asmEntry)) {
        // Any per-global encoding error already raised a loud
        // diagnostic via the function's internal `emit`.
        return false;
    }
    assembled.dataItems = std::move(dataItems);

    // 11. Link.
    auto const linkEntry = reporter.errorCount();
    auto image = linker::link(assembled, target, format, reporter);
    if (!image.ok() || !tierClean(reporter, linkEntry)) {
        return false;
    }

    // 12. Commit to disk.
    return linker::writeImage(image, outPath, reporter);
}

} // namespace dss
