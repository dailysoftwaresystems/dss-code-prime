// ML5 cycle 3a — MIR→LIR isel vertical slice tests.
// Drives the full c-subset → CST → HIR → MIR → LIR pipeline on minimal
// straight-line functions (Arg/Const/Add/Sub/Return) and pins the
// per-opcode lowering shape against the shipped x86_64 target schema.
//
// Same harness style as `tests/mir/test_mir_lowering_c_subset.cpp`: one
// `lowerCSubsetToLir(src)` helper threads each phase's diagnostics so
// assertions can disambiguate which layer flagged a failure.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "lir/lir.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/lowering/hir_to_mir.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

using namespace dss;

namespace {

struct Lowered {
    SemanticModel                    model;
    std::unique_ptr<CstToHirResult>  hir;
    DiagnosticReporter               hirReporter;
    HirToMirResult                   mir;
    DiagnosticReporter               mirReporter;
    std::shared_ptr<TargetSchema>    target;
    DiagnosticReporter               lirReporter;
    MirToLirResult                   lir;
};

[[nodiscard]] Lowered lowerCSubsetToLir(std::string src) {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    if (!loaded) { ADD_FAILURE() << "loadShipped(c-subset) failed"; std::abort(); }
    UnitBuilder builder{*loaded};
    builder.addInMemory(std::move(src), "<mem>");
    auto cu    = std::make_shared<CompilationUnit>(std::move(builder).finish());
    auto model = analyze(cu);
    DiagnosticReporter hirReporter;
    auto hir = lowerToHir(model, hirReporter);
    DiagnosticReporter mirReporter;
    MirLoweringConfig mirCfg;
    mirCfg.globalsAllowFloat = (*loaded)->hirLowering().globalsConstEval.allowFloat;
    HirToMirResult mir = lowerToMir(hir->hir, hir->literalPool,
                                    model.lattice().interner(), mirReporter,
                                    &hir->sourceMap, mirCfg);
    auto target = TargetSchema::loadShipped("x86_64");
    if (!target) { ADD_FAILURE() << "loadShipped(x86_64) failed"; std::abort(); }
    DiagnosticReporter lirReporter;
    auto lir = lowerToLir(mir.mir, **target, lirReporter);
    return Lowered{
        .model       = std::move(model),
        .hir         = std::move(hir),
        .hirReporter = std::move(hirReporter),
        .mir         = std::move(mir),
        .mirReporter = std::move(mirReporter),
        .target      = std::move(*target),
        .lirReporter = std::move(lirReporter),
        .lir         = std::move(lir),
    };
}

// Test-helper: assert every prior phase succeeded so failure messages
// pinpoint the layer that broke. Used by every cycle-3a test below.
void assertUpstreamClean(Lowered const& L) {
    ASSERT_FALSE(L.model.hasErrors())
        << "semantic phase: " << (L.model.diagnostics().all().empty()
            ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok)
        << "HIR lowering: " << (L.hirReporter.all().empty()
            ? "" : L.hirReporter.all()[0].actual);
    ASSERT_TRUE(L.mir.ok)
        << "MIR lowering: " << (L.mirReporter.all().empty()
            ? "" : L.mirReporter.all()[0].actual);
}

}  // namespace

TEST(MirToLir, StraightLineAddLowersToLirAddSequence) {
    // The reference vertical slice. `int add(int a, int b) { return a+b; }`
    // → MIR { Arg(0), Arg(1), Add(%0,%1), Return(%2) }
    // → LIR { arg(payload=0), arg(payload=1), add(%0,%1), ret(%2) }.
    auto L = lowerCSubsetToLir("int add(int a, int b) { return a + b; }");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok)
        << "LIR lowering: " << (L.lirReporter.all().empty()
            ? "" : L.lirReporter.all()[0].actual);

    Lir const& lir = L.lir.lir;
    ASSERT_EQ(lir.moduleFuncCount(), 1u);
    LirFuncId const fn = lir.funcAt(0);
    EXPECT_EQ(lir.funcBlockCount(fn), 1u);

    LirBlockId const bb = lir.funcBlockAt(fn, 0);
    // The block carries: arg(0), arg(1), add, ret — 4 LIR instructions.
    EXPECT_EQ(lir.blockInstCount(bb), 4u);

    auto opOf = [&](std::uint32_t idx) {
        return lir.instOpcode(lir.blockInstAt(bb, idx));
    };
    auto const& sch = *L.target;
    EXPECT_EQ(opOf(0), *sch.opcodeByMnemonic("arg"));
    EXPECT_EQ(opOf(1), *sch.opcodeByMnemonic("arg"));
    EXPECT_EQ(opOf(2), *sch.opcodeByMnemonic("add"));
    EXPECT_EQ(opOf(3), *sch.opcodeByMnemonic("ret"));

    // The Return is the block terminator (per LirBuilder::addReturn).
    EXPECT_EQ(lir.blockTerminator(bb), lir.blockInstAt(bb, 3));

    // Argument-index payloads on the two `arg` insts must be 0 and 1.
    EXPECT_EQ(lir.instPayload(lir.blockInstAt(bb, 0)), 0u);
    EXPECT_EQ(lir.instPayload(lir.blockInstAt(bb, 1)), 1u);
}

TEST(MirToLir, ConstReturnLowersToMovRet) {
    // `int forty_two() { return 42; }`
    // → MIR { Const(42), Return(%0) }
    // → LIR { mov vN, 42 ; ret vN }.
    auto L = lowerCSubsetToLir("int forty_two() { return 42; }");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok);

    Lir const& lir = L.lir.lir;
    ASSERT_EQ(lir.moduleFuncCount(), 1u);
    LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    ASSERT_EQ(lir.blockInstCount(bb), 2u);

    auto const& sch = *L.target;
    LirInstId const movId = lir.blockInstAt(bb, 0);
    LirInstId const retId = lir.blockInstAt(bb, 1);
    EXPECT_EQ(lir.instOpcode(movId), *sch.opcodeByMnemonic("mov"));
    EXPECT_EQ(lir.instOpcode(retId), *sch.opcodeByMnemonic("ret"));

    // The mov's source operand is the immediate 42.
    auto const movOperands = lir.instOperands(movId);
    ASSERT_EQ(movOperands.size(), 1u);
    EXPECT_EQ(movOperands[0].kind, LirOperandKind::ImmInt);
    EXPECT_EQ(movOperands[0].immInt32, 42);

    // The ret's value operand references the mov's result register.
    auto const retOperands = lir.instOperands(retId);
    ASSERT_EQ(retOperands.size(), 1u);
    EXPECT_EQ(retOperands[0].kind, LirOperandKind::Reg);
    EXPECT_EQ(retOperands[0].reg, lir.instResult(movId));
}

TEST(MirToLir, SubReturnLowersThreeInstructions) {
    // `int s(int a, int b) { return a - b; }` → 4 LIR insts: arg, arg, sub, ret.
    auto L = lowerCSubsetToLir("int s(int a, int b) { return a - b; }");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok);

    auto const& sch = *L.target;
    Lir const& lir = L.lir.lir;
    LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    ASSERT_EQ(lir.blockInstCount(bb), 4u);

    EXPECT_EQ(lir.instOpcode(lir.blockInstAt(bb, 2)),
              *sch.opcodeByMnemonic("sub"));
}

TEST(MirToLir, ReturnVoidLowersToBareRet) {
    auto L = lowerCSubsetToLir("void noop() { return; }");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok);

    Lir const& lir = L.lir.lir;
    LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    ASSERT_EQ(lir.blockInstCount(bb), 1u);
    EXPECT_EQ(lir.instOpcode(lir.blockInstAt(bb, 0)),
              *L.target->opcodeByMnemonic("ret"));
    // Bare ret has no operands.
    EXPECT_EQ(lir.instOperands(lir.blockInstAt(bb, 0)).size(), 0u);
}

TEST(MirToLir, MultipleFunctionsEachIsolatedVRegSpace) {
    // Two functions must each restart at vreg 1; the per-function reset of
    // `valueToReg` + the builder's nextVReg counter prevents cross-pollution.
    auto L = lowerCSubsetToLir(
        "int a(int x) { return x; }\n"
        "int b(int y) { return y; }\n");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok);

    Lir const& lir = L.lir.lir;
    ASSERT_EQ(lir.moduleFuncCount(), 2u);
    // Each function defines its own argument register independently. The
    // first inst of each must be `arg`.
    for (std::uint32_t i = 0; i < 2; ++i) {
        LirFuncId const fn = lir.funcAt(i);
        LirBlockId const bb = lir.funcBlockAt(fn, 0);
        EXPECT_EQ(lir.instOpcode(lir.blockInstAt(bb, 0)),
                  *L.target->opcodeByMnemonic("arg"));
    }
}

TEST(MirToLir, UnsupportedMirOpcodeFailsLoud) {
    // c-subset's `if/while` lowers to MIR control-flow (Br/CondBr); cycle 3a
    // deliberately does NOT yet lower these. The lowerer must report
    // L_UnsupportedLoweringForOpcode rather than silently producing wrong code.
    auto L = lowerCSubsetToLir(
        "int abs(int x) { if (x < 0) return -x; return x; }");
    assertUpstreamClean(L);
    // The pass surfaces at least one L_UnsupportedLoweringForOpcode and `ok`
    // is false — same fail-loud-deferral discipline as ML2 cycle 1.
    EXPECT_FALSE(L.lir.ok);
    bool foundUnsupported = false;
    for (auto const& d : L.lirReporter.all()) {
        if (d.code == DiagnosticCode::L_UnsupportedLoweringForOpcode) {
            foundUnsupported = true;
            break;
        }
    }
    EXPECT_TRUE(foundUnsupported)
        << "cycle 3a must report L_UnsupportedLoweringForOpcode on CondBr "
           "(or similar non-3a opcode); silent acceptance is a regression";
}
