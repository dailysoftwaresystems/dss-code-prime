// ML2 HIR→MIR lowering tests: end-to-end (parse c-subset → semantic → HIR
// lowering → MIR lowering) over the minimal cycle-1 surface — a straight-
// line function with params + literals + integer arithmetic + return.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "mir/lowering/hir_to_mir.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

using namespace dss;

namespace {

// Drive: c-subset source → CompilationUnit → SemanticModel → HIR → MIR.
// Each layer's diagnostics are surfaced to the test; ML2's are separated
// so the caller can opt-in or opt-out of `expected-clean`.
struct Lowered {
    SemanticModel                    model;
    std::unique_ptr<CstToHirResult>  hir;
    DiagnosticReporter               hirReporter;
    HirToMirResult                   mir;
    DiagnosticReporter               mirReporter;
};

[[nodiscard]] Lowered lowerCSubset(std::string src) {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    if (!loaded) { ADD_FAILURE() << "loadShipped(c-subset) failed"; std::abort(); }
    UnitBuilder builder{*loaded};
    builder.addInMemory(std::move(src), "<mem>");
    auto cu    = std::make_shared<CompilationUnit>(std::move(builder).finish());
    auto model = analyze(cu);
    DiagnosticReporter hirReporter;
    auto hir = lowerToHir(model, hirReporter);
    DiagnosticReporter mirReporter;
    HirToMirResult mir = lowerToMir(hir->hir, hir->literalPool,
                                    model.lattice().interner(), mirReporter);
    return Lowered{
        .model       = std::move(model),
        .hir         = std::move(hir),
        .hirReporter = std::move(hirReporter),
        .mir         = std::move(mir),
        .mirReporter = std::move(mirReporter),
    };
}

} // namespace

// ML2 cycle 1: the minimal vertical slice.
// `int add(int a, int b) { return a + b; }` lowers to ONE MIR function,
// ONE block, FOUR instructions: Arg(0:i32), Arg(1:i32), Add(%0, %1), Return(%2).
TEST(MirLoweringCSubset, StraightLineAddFunction) {
    auto L = lowerCSubset("int add(int a, int b) { return a + b; }");
    ASSERT_FALSE(L.model.hasErrors())
        << "semantic phase: " << (L.model.diagnostics().all().empty()
            ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok)
        << "HIR lowering: " << (L.hirReporter.all().empty()
            ? "" : L.hirReporter.all()[0].actual);
    ASSERT_TRUE(L.mir.ok)
        << "MIR lowering: " << (L.mirReporter.all().empty()
            ? "" : L.mirReporter.all()[0].actual);

    Mir const& m = L.mir.mir;
    ASSERT_EQ(m.moduleFuncCount(), 1u);
    MirFuncId const fn = m.funcAt(0);
    EXPECT_EQ(m.funcBlockCount(fn), 1u);
    MirBlockId const entry = m.funcEntry(fn);
    EXPECT_EQ(m.blockMarker(entry), StructCfMarker::EntryBlock);

    // Four instructions: Arg(0), Arg(1), Add, Return.
    ASSERT_EQ(m.blockInstCount(entry), 4u);
    MirInstId const arg0   = m.blockInstAt(entry, 0);
    MirInstId const arg1   = m.blockInstAt(entry, 1);
    MirInstId const sum    = m.blockInstAt(entry, 2);
    MirInstId const ret    = m.blockInstAt(entry, 3);

    EXPECT_EQ(m.instOpcode(arg0), MirOpcode::Arg);
    EXPECT_EQ(m.argIndex(arg0), 0u);
    EXPECT_EQ(m.instOpcode(arg1), MirOpcode::Arg);
    EXPECT_EQ(m.argIndex(arg1), 1u);

    EXPECT_EQ(m.instOpcode(sum), MirOpcode::Add);
    auto sumOps = m.instOperands(sum);
    ASSERT_EQ(sumOps.size(), 2u);
    EXPECT_EQ(sumOps[0], arg0);
    EXPECT_EQ(sumOps[1], arg1);

    EXPECT_EQ(m.instOpcode(ret), MirOpcode::Return);
    auto retOps = m.instOperands(ret);
    ASSERT_EQ(retOps.size(), 1u);
    EXPECT_EQ(retOps[0], sum);
}

// ML2 cycle 1: literal + return.
// `int f() { return 42; }` lowers to one block with Const(42:i32), Return(%0).
TEST(MirLoweringCSubset, ReturnLiteralProducesConst) {
    auto L = lowerCSubset("int f() { return 42; }");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok);

    Mir const& m = L.mir.mir;
    ASSERT_EQ(m.moduleFuncCount(), 1u);
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    ASSERT_EQ(m.blockInstCount(entry), 2u);
    MirInstId const c   = m.blockInstAt(entry, 0);
    MirInstId const ret = m.blockInstAt(entry, 1);

    EXPECT_EQ(m.instOpcode(c), MirOpcode::Const);
    auto const& lit = m.literalValue(m.constLiteralIndex(c));
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(lit.value));
    EXPECT_EQ(std::get<std::int64_t>(lit.value), 42);

    EXPECT_EQ(m.instOpcode(ret), MirOpcode::Return);
    EXPECT_EQ(m.instOperands(ret)[0], c);
}

// ML2 cycle 1 (review-fix): an empty void-bodied function lowers to an
// implicit `return` at MIR. Previously this aborted finish() because the
// entry block had no terminator. Pins the implicit-void-return synthesis.
TEST(MirLoweringCSubset, VoidFunctionWithEmptyBodyGetsImplicitReturn) {
    auto L = lowerCSubset("void f() {}");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "MIR lowering: " << (L.mirReporter.all().empty()
            ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    ASSERT_EQ(m.blockInstCount(entry), 1u);
    MirInstId const term = m.blockInstAt(entry, 0);
    EXPECT_EQ(m.instOpcode(term), MirOpcode::Return);
    EXPECT_TRUE(m.instOperands(term).empty());  // void return — no value
}

// ML2 cycle 1 (review-fix): an unsupported construct mid-body emits a
// fail-loud diagnostic and the MIR builder still hands back a frozen module
// (the open block is sealed with Unreachable so finish() can complete).
// Previously this aborted the process. Uses a function-call (not yet
// supported in ML2 cycle 1) as the unsupported construct.
TEST(MirLoweringCSubset, UnsupportedConstructEmitsDiagnosticWithoutAbort) {
    auto L = lowerCSubset(
        "int g(int x) { return x; }\n"
        "int h(int y) { return g(y); }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    // ML2 cycle 1 doesn't handle Call yet → MIR lowering should fail loud.
    EXPECT_FALSE(L.mir.ok);
    bool sawUnsupported = false;
    for (auto const& d : L.mirReporter.all()) {
        if (d.code == DiagnosticCode::H_UnsupportedLoweringForKind) {
            sawUnsupported = true; break;
        }
    }
    EXPECT_TRUE(sawUnsupported) << "expected H_UnsupportedLoweringForKind";
    // Critical: finish() did NOT abort. The frozen module exists (even if
    // partial). This is the load-bearing review-fix.
    EXPECT_FALSE(L.mir.mir.empty());
}

// ML2 cycle 1 (review-fix): a Global declaration emits an unsupported
// diagnostic, not a silent skip. Previously silently skipped which would
// mask real gaps when later cycles add globals-with-initializers.
TEST(MirLoweringCSubset, GlobalDeclarationEmitsUnsupported) {
    auto L = lowerCSubset("int g = 42;\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    EXPECT_FALSE(L.mir.ok);
    bool sawUnsupported = false;
    for (auto const& d : L.mirReporter.all()) {
        if (d.code == DiagnosticCode::H_UnsupportedLoweringForKind
            && d.actual.find("Global") != std::string::npos) {
            sawUnsupported = true; break;
        }
    }
    EXPECT_TRUE(sawUnsupported) << "expected Global-unsupported diagnostic";
}

// ML2 cycle 1: unsigned integer signed-vs-unsigned opcode selection. With
// c-subset's current builtinTypes mapping `int → I32` (signed), all arith
// goes through the signed forms. This pins the type-driven opcode-pick
// path; cycle 2+ adds unsigned types and floats.
TEST(MirLoweringCSubset, SignedDivisionLowersToSDiv) {
    auto L = lowerCSubset("int q(int a, int b) { return a / b; }");
    ASSERT_TRUE(L.mir.ok);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // [arg0, arg1, SDiv, return]
    ASSERT_EQ(m.blockInstCount(entry), 4u);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 2)), MirOpcode::SDiv);
}
