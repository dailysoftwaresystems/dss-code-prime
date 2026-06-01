#include "program/compile_pipeline.hpp"

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "asm/asm.hpp"
#include "core/types/parse_diagnostic.hpp"
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

#include <memory>

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
                       DiagnosticReporter&           reporter) {
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

    // 3. HIR → MIR. Plug the language schema's globals const-eval
    //    policy into the lowering config (same shape as the
    //    lowered_lir_fixture used by ML6 / AS pipeline tests).
    auto const mirEntry = reporter.errorCount();
    MirLoweringConfig mirCfg;
    mirCfg.globalsAllowFloat =
        grammar.hirLowering().globalsConstEval.allowFloat;
    auto mir = lowerToMir(hir->hir, hir->literalPool,
                          model.lattice().interner(), reporter,
                          &hir->sourceMap, mirCfg);
    if (!mir.ok || !tierClean(reporter, mirEntry)) {
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
