// FC2 Part A ⇄ Part B integration: the SOURCE-LEVEL C cast drives the
// SSE lowering end-to-end.
//
// `int main() { return (int)(1.7 + 2.5); }` — c-subset SOURCE — through
// the REAL pipeline: parse → semantic (the explicit cast is what makes
// the F64→I32 conversion legal; the implicit form is rejected) → CST→HIR
// (castExpr → HirKind::Cast, explicit flags) → HIR→MIR (mapCast F64→I32
// signed = FPToSI) → MIR→LIR → liveness → regalloc → rewrite → 2-addr
// legalize → callconv → assemble (the exact stage order of
// compile_pipeline.cpp's lowerMirModuleToAssembly, mirroring
// test_asm_x86_sse.cpp's runFullPipeline). Asserts ZERO diagnostics at
// every tier and that the encoded function contains the CVTTSD2SI
// (F2 [REX] 0F 2C) + ADDSD (F2 [REX] 0F 58) byte sequences — the
// integration point Part B was waiting on.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "lir/lir.hpp"
#include "lir/lir_2addr_legalize.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_regalloc.hpp"
#include "lir/lir_rewrite.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/lowering/hir_to_mir.hpp"
#include "mir/mir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace dss;

namespace {

// True iff `bytes` contains `prefix [REX] 0F op2` at any offset (the
// matcher from test_asm_x86_sse.cpp — REX may sit between the mandatory
// prefix and the opcode escape).
[[nodiscard]] bool containsSseOp(std::vector<std::uint8_t> const& bytes,
                                 std::uint8_t prefix, std::uint8_t op2) {
    for (std::size_t i = 0; i + 2 < bytes.size(); ++i) {
        if (bytes[i] != prefix) continue;
        std::size_t j = i + 1;
        if ((bytes[j] & 0xF0u) == 0x40u) ++j;  // optional REX
        if (j + 1 < bytes.size() && bytes[j] == 0x0F && bytes[j + 1] == op2) {
            return true;
        }
    }
    return false;
}

void dumpDiagnostics(DiagnosticReporter const& rep) {
    for (auto const& d : rep.all()) {
        ADD_FAILURE() << "diagnostic: " << diagnosticCodeName(d.code)
                      << " " << d.actual;
    }
}

} // namespace

TEST(CastIntegration, SourceLevelFloatToIntCastEncodesCvttsdsi) {
    // ── front half: source → semantic → HIR → MIR ──
    auto loaded = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(loaded.has_value());
    UnitBuilder builder{*loaded};
    builder.addInMemory("int main() { return (int)(1.7 + 2.5); }\n", "<cast-e2e>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    for (auto const& t : cu->trees()) {
        EXPECT_FALSE(t.diagnostics().hasErrors()) << "parse must be clean";
    }

    auto model = analyze(cu);
    ASSERT_FALSE(model.hasErrors())
        << "semantic must accept the EXPLICIT cast (the implicit form is "
           "rejected — see SemanticAnalyzerCSubset."
           "ExplicitFloatToIntCastAcceptedWhereImplicitRejected): "
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);

    DiagnosticReporter hirReporter;
    auto hir = lowerToHir(model, hirReporter);
    ASSERT_TRUE(hir->ok);
    EXPECT_EQ(hirReporter.errorCount(), 0u);
    if (hirReporter.errorCount() != 0u) dumpDiagnostics(hirReporter);

    DiagnosticReporter mirReporter;
    MirLoweringConfig mirCfg;
    mirCfg.globalsAllowFloat =
        (*loaded)->hirLowering().globalsConstEval.allowFloat;
    HirToMirResult mir = lowerToMir(hir->hir, hir->literalPool,
                                    model.lattice().interner(), mirReporter,
                                    &hir->sourceMap, mirCfg,
                                    /*ffiMap=*/nullptr, &hir->linkageMap);
    ASSERT_TRUE(mir.ok);
    EXPECT_EQ(mirReporter.errorCount(), 0u);
    if (mirReporter.errorCount() != 0u) dumpDiagnostics(mirReporter);

    // ── back half: MIR → LIR pipeline → x86_64 bytes ──
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    auto lir = lowerToLir(mir.mir, **target, model.lattice().interner(), rep);
    ASSERT_TRUE(lir.ok) << "MIR->LIR failed";
    auto const liveness = analyzeLiveness(lir.lir);
    auto const alloc = allocateRegisters(lir.lir, **target, liveness,
                                         /*callingConventionIndex=*/0, rep);
    ASSERT_TRUE(alloc.ok()) << "regalloc failed";
    auto rewritten = rewriteWithAllocation(lir.lir, **target, alloc, rep);
    ASSERT_TRUE(rewritten.ok) << "rewrite failed";
    auto legal = legalizeTwoAddress(rewritten.lir, **target, rep);
    ASSERT_TRUE(legal.ok()) << "2-addr legalize failed";
    auto cc = materializeCallingConvention(legal.lir, **target, alloc, rep);
    ASSERT_TRUE(cc.ok()) << "callconv failed";
    std::vector<MirInstId> lirToMir(cc.lir.instCount(), InvalidMirInst);
    auto assembled = assemble(cc.lir, **target, lirToMir, rep);
    ASSERT_TRUE(assembled.ok()) << "assemble failed";
    EXPECT_EQ(rep.errorCount(), 0u);
    if (rep.errorCount() != 0u) dumpDiagnostics(rep);
    ASSERT_EQ(assembled.functions.size(), 1u);
    auto const& bytes = assembled.functions[0].bytes;

    // The cast lowered FPToSI → CVTTSD2SI; the float add lowered ADDSD.
    EXPECT_TRUE(containsSseOp(bytes, 0xF2, 0x2C))
        << "encoded main() must contain CVTTSD2SI (F2 [REX] 0F 2C) — "
           "the source-level (int) cast's FPToSI";
    EXPECT_TRUE(containsSseOp(bytes, 0xF2, 0x58))
        << "encoded main() must contain ADDSD (F2 [REX] 0F 58)";
}
