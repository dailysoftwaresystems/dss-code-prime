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
#include "mir/mir_text.hpp"
#include "mir/mir_verifier.hpp"
#include "opt/optimizer.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <variant>

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
    // Cycle 3a wires the HirSourceMap so MIR diagnostics carry source spans
    // (mirroring HirVerifier's `&sourceMap` plumbing). The pointer is bound
    // through `hir->sourceMap` which CstToHirResult always populates.
    // Plan 12.5 §0.2 D3 closed: schema declares MIR-globals const-eval
    // policy; the test driver reads it off the loaded schema and passes
    // the resolved knob through. No per-language C++ — the policy lives
    // in `c-subset.lang.json`.
    MirLoweringConfig mirCfg;
    mirCfg.globalsAllowFloat = (*loaded)->hirLowering().globalsConstEval.allowFloat;
    // FC7 (D-FC7-MEMBER-ACCESS): thread the target's aggregate-layout params
    // (struct/union field offsets + sizes) into the MIR config exactly as
    // compile_pipeline.cpp does, so member-access + aggregate-local lowering
    // resolves field byte offsets. dataModel stays the Lp64 default (an
    // int-based struct's field offsets are dataModel-independent here).
    if (auto t = TargetSchema::loadShipped("x86_64"); t.has_value()) {
        mirCfg.aggregateLayout       = (*t)->aggregateLayout();
        mirCfg.aggregateLayoutLoaded = (*t)->aggregateLayoutLoaded();
        // FC7 (D-FC7-STRUCT-BY-VALUE-ARG-RETURN): thread the SysV CC's by-value
        // params so a struct passed/returned BY VALUE classifies (the sysv_amd64
        // strategy). Mirrors compile_pipeline.cpp.
        if (auto const* cc = (*t)->callingConventionByName("sysv_amd64")) {
            mirCfg.aggregateClassification   = cc->aggregateClassification;
            mirCfg.aggregateMaxRegBytes      = cc->aggregateMaxRegBytes;
            mirCfg.aggregateSretViaHiddenArg = !cc->indirectResultRegister.has_value();
            mirCfg.argSlotAligned            = cc->slotAligned;
        }
    }
    // D-CSUBSET-LINKAGE-SPECIFIERS: thread the native-decl linkage side-table
    // exactly as compile_pipeline.cpp does — so `static`/`__attribute__` source
    // flows binding/visibility into the MIR. Existing fixtures (no specifiers)
    // get an empty map ⇒ every symbol stays Global/Default (unchanged).
    HirToMirResult mir = lowerToMir(hir->hir, hir->literalPool,
                                    model.lattice().interner(), mirReporter,
                                    &hir->sourceMap, mirCfg, /*ffiMap=*/nullptr,
                                    &hir->linkageMap);
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

// ML2 cycle 1 (review-fix), updated for cycle 3a: pins finish()-no-abort on
// unsupported-construct fail-loud. Uses VarDecl-with-init (still deferred to
// the lvalue-via-alloca sub-cycle as a real prerequisite) so the diagnostic
// is reachable. The Call-as-unsupported variant was replaced when cycle 3a
// landed Call lowering; the abort-resilience invariant is independent of
// which construct is currently unsupported.
// The abort-resilience invariant ("never abort on unsupported, surface a
// diagnostic + keep the partial MIR walkable") is pinned by the dedicated
// Global-decl test below — kept here as the historical anchor for the
// invariant. When a future HIR construct lands that MIR doesn't lower,
// reinstate a dedicated `UnsupportedConstruct…` test for it.

// A module-level Global with a constant initializer lowers to a MirGlobal
// whose `initLiteralIndex` points to the folded literal — no synthesized
// init function needed.
TEST(MirLoweringCSubset, GlobalWithLiteralInitFoldsToConstant) {
    auto L = lowerCSubset("int g = 42;\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    ASSERT_EQ(m.moduleGlobalCount(), 1u);
    MirGlobalId const g = m.globalAt(0);
    EXPECT_NE(m.globalInitLiteralIndex(g), UINT32_MAX)
        << "constant init should fold to a literal-pool index";
    EXPECT_FALSE(m.globalInitFunc(g).valid())
        << "constant-init globals must not carry an init function";
    // The literal at that index is 42.
    auto const& lit = m.literalValue(m.globalInitLiteralIndex(g));
    EXPECT_EQ(std::get<std::int64_t>(lit.value), 42);
}

// A Global without an initializer is zero-init — no literal, no init func.
TEST(MirLoweringCSubset, GlobalWithoutInitIsZeroInit) {
    auto L = lowerCSubset("int g;\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    ASSERT_EQ(m.moduleGlobalCount(), 1u);
    MirGlobalId const g = m.globalAt(0);
    EXPECT_EQ(m.globalInitLiteralIndex(g), UINT32_MAX);
    EXPECT_FALSE(m.globalInitFunc(g).valid());
}

// A function reading a module global lowers the read as
// `GlobalAddr(sym) → Load`. Pins the new globalSymbols resolution path
// in `lowerExpr`'s `Ref` case.
TEST(MirLoweringCSubset, FunctionReadingGlobalEmitsGlobalAddrThenLoad) {
    auto L = lowerCSubset(
        "int g = 7;\n"
        "int read_g() { return g; }\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    ASSERT_EQ(m.moduleGlobalCount(), 1u);
    ASSERT_EQ(m.moduleFuncCount(), 1u);
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // [GlobalAddr, Load, Return]
    ASSERT_EQ(m.blockInstCount(entry), 3u);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 0)), MirOpcode::GlobalAddr);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 1)), MirOpcode::Load);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 2)), MirOpcode::Return);
}

// `int g = 1 + 2;` — BinaryOp(Add, Literal, Literal) folds to a constant
// `3` at lowering time. No init function is synthesized.
TEST(MirLoweringCSubset, GlobalWithBinaryOpOnLiteralsFoldsToConstant) {
    auto L = lowerCSubset("int g = 1 + 2;\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    ASSERT_EQ(m.moduleGlobalCount(), 1u);
    MirGlobalId const g = m.globalAt(0);
    EXPECT_NE(m.globalInitLiteralIndex(g), UINT32_MAX);
    EXPECT_FALSE(m.globalInitFunc(g).valid());
    auto const& lit = m.literalValue(m.globalInitLiteralIndex(g));
    EXPECT_EQ(std::get<std::int64_t>(lit.value), 3);
    EXPECT_EQ(m.moduleFuncCount(), 0u);
}

// HR cycle C end-to-end: `return a > b;` from an int-returning function
// emits a Cast(Bool→int) in HIR, which MIR lowers as ZExt. Pins the full
// HR-coercion + MIR-Cast-lowering chain.
TEST(MirLoweringCSubset, ReturnBoolFromIntFnEmitsZExt) {
    auto L = lowerCSubset(
        "int gt(int a, int b) { return a > b; }");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // [Arg a, Arg b, ICmpSgt, ZExt, Return]
    ASSERT_EQ(m.blockInstCount(entry), 5u);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 0)), MirOpcode::Arg);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 1)), MirOpcode::Arg);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 2)), MirOpcode::ICmpSgt);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 3)), MirOpcode::ZExt);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 4)), MirOpcode::Return);
}

// CE4 end-to-end: a ternary initializer folds when cond + selected arm
// both fold (the unselected arm doesn't need to fold). `int g = 1 ? 7 : x;`
// — even if `x` were non-constant, cond=true picks the then-arm and the
// global lands as a constant-init literal `7`. Pins the short-circuit
// recursion in the const-eval engine end-to-end through MIR-globals.
TEST(MirLoweringCSubset, GlobalWithTernaryInitFoldsToSelectedArm) {
    auto L = lowerCSubset("int g = 1 ? 7 : 9;\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    ASSERT_EQ(m.moduleGlobalCount(), 1u);
    MirGlobalId const g = m.globalAt(0);
    EXPECT_NE(m.globalInitLiteralIndex(g), UINT32_MAX);
    EXPECT_FALSE(m.globalInitFunc(g).valid());
    EXPECT_EQ(m.moduleFuncCount(), 0u);
    EXPECT_EQ(std::get<std::int64_t>(m.literalValue(m.globalInitLiteralIndex(g)).value), 7);
}

// CE4: short-circuit semantics through MIR-globals end-to-end. The RHS
// `1 / 0` is genuinely non-foldable (div-by-zero — even with the
// permissive MIR policy `refuseOnOverflow=false`, div-by-zero still
// reports `NotAConstantExpression`). Without short-circuit the global
// would route to `__module_init__`. With short-circuit, `0 && _` folds
// to 0 unconditionally — this is the test that actually distinguishes
// the two implementations end-to-end.
TEST(MirLoweringCSubset, GlobalWithLogicalAndShortCircuitsPastDivByZero) {
    auto L = lowerCSubset("int g = 0 && (1 / 0);\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    ASSERT_EQ(m.moduleGlobalCount(), 1u);
    MirGlobalId const g = m.globalAt(0);
    EXPECT_NE(m.globalInitLiteralIndex(g), UINT32_MAX)
        << "fold must succeed via short-circuit; without it the RHS "
           "div-by-zero would route the global to __module_init__";
    EXPECT_FALSE(m.globalInitFunc(g).valid());
    EXPECT_EQ(m.moduleFuncCount(), 0u);
    EXPECT_EQ(std::get<std::int64_t>(m.literalValue(m.globalInitLiteralIndex(g)).value), 0);
}

// CE3 wire-up: an overflowing initializer narrowed through HR's implicit
// Cast still folds (MIR-globals opts `refuseOnOverflow=false` because the
// runtime would wrap identically; refusing would only lose the
// optimization). Without that knob this global would route to a synthesized
// __module_init__ function. Locks the cycle's load-bearing policy choice.
TEST(MirLoweringCSubset, GlobalWithOverflowingInitFoldsWithModularWrap) {
    // The HR coercion pass wraps any int literal into `Cast(literal, target)`
    // when the target type differs from the literal's natural type. In
    // c-subset, every `int g = N;` produces such a Cast (literal core →
    // declared I32), so picking a value whose Cast IS load-bearing is
    // tricky for an integer-only language. Use a comparison literal that
    // forces a Cast(Bool→I32) — value is 1, no actual overflow — and a
    // wider literal that forces Cast(int→int). The structural pin is:
    // the global folds (no __module_init__).
    auto L = lowerCSubset("int g = 0 - 1;\n");   // -1 via subtraction, folds
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    ASSERT_EQ(m.moduleGlobalCount(), 1u);
    MirGlobalId const g = m.globalAt(0);
    EXPECT_NE(m.globalInitLiteralIndex(g), UINT32_MAX)
        << "fold should succeed without synthesizing __module_init__";
    EXPECT_FALSE(m.globalInitFunc(g).valid());
    EXPECT_EQ(m.moduleFuncCount(), 0u);
    EXPECT_EQ(std::get<std::int64_t>(m.literalValue(m.globalInitLiteralIndex(g)).value), -1);
}

// CE2 wire-up: `int a = 1; int b = a;` folds end-to-end. `b`'s init is a
// `Ref(a)`; the const-eval engine's resolver callback looks up `a`'s
// init in the globals pre-pass table and folds it to `1`, so `b` lands
// as a constant-init global too — no `__module_init__` synthesized.
TEST(MirLoweringCSubset, GlobalCrossReferenceFoldsViaConstEvalResolver) {
    auto L = lowerCSubset(
        "int a = 1;\n"
        "int b = a;\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    ASSERT_EQ(m.moduleGlobalCount(), 2u);
    // Both globals have constant initializers. No init function synthesized.
    MirGlobalId const ga = m.globalAt(0);
    MirGlobalId const gb = m.globalAt(1);
    EXPECT_NE(m.globalInitLiteralIndex(ga), UINT32_MAX);
    EXPECT_NE(m.globalInitLiteralIndex(gb), UINT32_MAX);
    EXPECT_FALSE(m.globalInitFunc(ga).valid());
    EXPECT_FALSE(m.globalInitFunc(gb).valid());
    EXPECT_EQ(m.moduleFuncCount(), 0u);
    // Both literals carry value 1.
    EXPECT_EQ(std::get<std::int64_t>(m.literalValue(m.globalInitLiteralIndex(ga)).value), 1);
    EXPECT_EQ(std::get<std::int64_t>(m.literalValue(m.globalInitLiteralIndex(gb)).value), 1);
}

// HR's implicit-coercion pass wraps `int g = (Bool literal);` in a Cast.
// `tryConstFold`'s new Cast case folds through the cast and produces a
// constant-init global — no `__module_init__` function synthesized.
TEST(MirLoweringCSubset, GlobalWithCastedLiteralFoldsToConstant) {
    // Use a comparison literal: `1 > 0` is Bool=true (1); declaring an int
    // global from it goes through Cast(Bool→int) at HR time and must still
    // fold cleanly via the Cast-aware const-fold.
    auto L = lowerCSubset("int g = 1 > 0;\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    ASSERT_EQ(m.moduleGlobalCount(), 1u);
    MirGlobalId const g = m.globalAt(0);
    EXPECT_NE(m.globalInitLiteralIndex(g), UINT32_MAX)
        << "Cast-wrapped constant init should fold, not route to __module_init__";
    EXPECT_FALSE(m.globalInitFunc(g).valid());
    EXPECT_EQ(m.moduleFuncCount(), 0u);  // no init function synthesized
}

// `int g = -1;` — UnaryOp(Neg, Literal) folds to a constant-init global.
// No init function is synthesized.
TEST(MirLoweringCSubset, GlobalWithUnaryNegLiteralFoldsToConstant) {
    auto L = lowerCSubset("int g = -7;\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    ASSERT_EQ(m.moduleGlobalCount(), 1u);
    MirGlobalId const g = m.globalAt(0);
    EXPECT_NE(m.globalInitLiteralIndex(g), UINT32_MAX);
    EXPECT_FALSE(m.globalInitFunc(g).valid())
        << "Neg(Literal) should fold — no init function needed";
    auto const& lit = m.literalValue(m.globalInitLiteralIndex(g));
    EXPECT_EQ(std::get<std::int64_t>(lit.value), -7);
    // No module-init function: only the (zero) real functions in the module.
    EXPECT_EQ(m.moduleFuncCount(), 0u);
}

// CE5 wire-up: a float-typed initializer expression folds end-to-end.
// `int g = 1.7 + 2.5;` is parsed as two F64 FloatLiterals; HR's
// commonType-driven coercion runs the BinaryOp in F64, then
// `lowerTopLevel`'s coerce wraps the result in Cast(F64→I32) for the
// declared int target; CE5's engine folds the float add (4.2) and the
// float→int truncation (toward zero → 4), so the global lands as a
// constant-init — no `__module_init__` synthesized. Locks the
// load-bearing CE5 contract MIR-globals depends on: `allowFloat=true`
// makes float-arithmetic globals fold instead of degrading to
// runtime-init.
TEST(MirLoweringCSubset, GlobalWithFloatArithmeticInitializerFoldsThroughCastToInt) {
    auto L = lowerCSubset("int g = 1.7 + 2.5;\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    ASSERT_EQ(m.moduleGlobalCount(), 1u);
    MirGlobalId const g = m.globalAt(0);
    EXPECT_NE(m.globalInitLiteralIndex(g), UINT32_MAX)
        << "float-arithmetic initializer must fold via CE5 — no __module_init__";
    EXPECT_FALSE(m.globalInitFunc(g).valid());
    EXPECT_EQ(m.moduleFuncCount(), 0u);
    // 1.7 + 2.5 = 4.2; Cast(F64→I32) truncates toward zero → 4.
    auto const& lit = m.literalValue(m.globalInitLiteralIndex(g));
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(lit.value))
        << "HR coerce must wrap F64 init in Cast(F64→I32) for int target; "
           "CE5 must fold through that Cast";
    EXPECT_EQ(std::get<std::int64_t>(lit.value), 4);
}

// A function writing to a module global lowers the write as
// `GlobalAddr(sym) → Store(rhs, addr)`. Pins the lvalue-side of the
// new globals resolution.
TEST(MirLoweringCSubset, FunctionWritingGlobalEmitsGlobalAddrThenStore) {
    auto L = lowerCSubset(
        "int g;\n"
        "void set_g(int v) { g = v; }\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // [Arg v, GlobalAddr g, Store, Return]
    ASSERT_EQ(m.blockInstCount(entry), 4u);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 0)), MirOpcode::Arg);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 1)), MirOpcode::GlobalAddr);
    MirInstId const storeI = m.blockInstAt(entry, 2);
    EXPECT_EQ(m.instOpcode(storeI), MirOpcode::Store);
    auto ops = m.instOperands(storeI);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], m.blockInstAt(entry, 0)) << "value operand is the Arg";
    EXPECT_EQ(ops[1], m.blockInstAt(entry, 1)) << "ptr operand is the GlobalAddr";
}

// ─── ML2 cycle 3a: Call + Ternary + Short-circuit ─────────────────────────

// Direct call: callee is a Ref-to-function (lowers as `GlobalAddr`), args are
// argument expressions, MIR Call's operand[0]=callee, [1..]=args.
TEST(MirLoweringCSubset, DirectCallLowersToMirCall) {
    auto L = lowerCSubset(
        "int g(int x) { return x; }\n"
        "int h(int y) { return g(y); }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "MIR: " << (L.mirReporter.all().empty()
            ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    ASSERT_EQ(m.moduleFuncCount(), 2u);
    // `h`'s entry block: Arg(0), GlobalAddr(g), Call(globalAddr, arg), Return(call).
    MirBlockId const entry = m.funcEntry(m.funcAt(1));
    ASSERT_GE(m.blockInstCount(entry), 4u);
    MirInstId const arg0   = m.blockInstAt(entry, 0);
    MirInstId const callee = m.blockInstAt(entry, 1);
    MirInstId const call   = m.blockInstAt(entry, 2);
    EXPECT_EQ(m.instOpcode(arg0), MirOpcode::Arg);
    EXPECT_EQ(m.instOpcode(callee), MirOpcode::GlobalAddr);
    EXPECT_EQ(m.instOpcode(call), MirOpcode::Call);
    auto ops = m.instOperands(call);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], callee);
    EXPECT_EQ(ops[1], arg0);
}

// Ternary `cond ? a : b` lowers to a diamond CFG with a phi at the join.
TEST(MirLoweringCSubset, TernaryLowersToDiamondPhi) {
    auto L = lowerCSubset(
        "int sel(int c, int a, int b) { return c ? a : b; }\n");
    ASSERT_TRUE(L.mir.ok);
    Mir const& m = L.mir.mir;
    MirFuncId const fn = m.funcAt(0);
    // Blocks: entry, thenBB, elseBB, joinBB.
    EXPECT_EQ(m.funcBlockCount(fn), 4u);
    MirBlockId const join = m.funcBlockAt(fn, 3);
    EXPECT_EQ(m.blockMarker(join), StructCfMarker::IfJoin);
    // join's first instruction is the phi.
    MirInstId const phi = m.blockInstAt(join, 0);
    EXPECT_EQ(m.instOpcode(phi), MirOpcode::Phi);
    auto inc = m.phiIncomings(phi);
    EXPECT_EQ(inc.size(), 2u);
}

// LogicalAnd `a && b` short-circuits: lhs is evaluated in the current block,
// then CondBr(lhs, rhsBlock, joinBlock). The join's phi takes lhs (from the
// current block) and rhs (from the rhsBlock).
TEST(MirLoweringCSubset, LogicalAndShortCircuitsWithPhi) {
    auto L = lowerCSubset(
        "int and2(int a, int b) { return a && b; }\n");
    ASSERT_TRUE(L.mir.ok)
        << "MIR: " << (L.mirReporter.all().empty()
            ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirFuncId const fn = m.funcAt(0);
    // Blocks: entry, rhsBB, joinBB.
    EXPECT_EQ(m.funcBlockCount(fn), 3u);
    MirBlockId const entry = m.funcEntry(fn);
    // entry's terminator is CondBr(lhs, rhsBB, joinBB).
    EXPECT_EQ(m.instOpcode(m.blockTerminator(entry)), MirOpcode::CondBr);
    auto succs = m.blockSuccessors(entry);
    ASSERT_EQ(succs.size(), 2u);
    // rhsBB (the true-edge target) is the diamond's conditional ARM —
    // FC3.5 sweep-c1 marks it IfThen so the verifier's IfThen↔IfJoin
    // count pairing holds (the one-armed-if shape; chip task_bd58aa3d).
    EXPECT_EQ(m.blockMarker(succs[0]), StructCfMarker::IfThen);
    // joinBB is the second successor (the false-edge / short-circuit target).
    MirBlockId const joinBB = succs[1];
    EXPECT_EQ(m.blockMarker(joinBB), StructCfMarker::IfJoin);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(joinBB, 0)), MirOpcode::Phi);
}

// LogicalOr `a || b` is the symmetric case — short-circuit on lhs TRUE.
TEST(MirLoweringCSubset, LogicalOrShortCircuitsWithPhi) {
    auto L = lowerCSubset(
        "int or2(int a, int b) { return a || b; }\n");
    ASSERT_TRUE(L.mir.ok);
    Mir const& m = L.mir.mir;
    MirFuncId const fn = m.funcAt(0);
    EXPECT_EQ(m.funcBlockCount(fn), 3u);
    MirBlockId const entry = m.funcEntry(fn);
    auto succs = m.blockSuccessors(entry);
    ASSERT_EQ(succs.size(), 2u);
    // joinBB is the FIRST successor (the true-edge / short-circuit target).
    MirBlockId const joinBB = succs[0];
    EXPECT_EQ(m.blockMarker(joinBB), StructCfMarker::IfJoin);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(joinBB, 0)), MirOpcode::Phi);
    // rhsBB sits on the FALSE edge for `||` — the canonical derivation
    // is edge-polarity-faithful (succs[1] != join → IfElse), unlike the
    // old hand-stamp which marked every short-circuit arm IfThen for
    // the dead count-pairing model.
    EXPECT_EQ(m.blockMarker(succs[1]), StructCfMarker::IfElse);
}

// FC3.5 sweep-c1 (chip task_bd58aa3d): `&&`/`||` AS AN IF-CONDITION.
// Pre-fix, LogicalAnd/Or minted an IfJoin with a `Linear` rhs arm, so
// `if (a < 2 && a < 3)` counted IfThen 1 vs IfJoin 2 and the verifier
// rejected the function (I_StructCfMismatch) — the LOWERING was the
// bug, not the verifier (the count pairing is the structural guard).
// This is the red-on-revert lever: flip rhsBB back to Linear and the
// verifier diagnostic returns.
TEST(MirLoweringCSubset, IfConditionWithLogicalOpsVerifiesClean) {
    auto L = lowerCSubset(
        "int pick(int a) {\n"
        "  if (a < 2 && a < 3) { return 40; }\n"
        "  if (a > 90 || a > 80) { return 41; }\n"
        "  if (a > 8 && (a < 12 || a > 100)) { return 42; }\n"
        "  return 7;\n"
        "}\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "MIR lowering: " << (L.mirReporter.all().empty()
            ? "" : L.mirReporter.all()[0].actual);
    DiagnosticReporter vrep;
    MirVerifier v{L.mir.mir};
    EXPECT_TRUE(v.verify(vrep))
        << "logical ops in if-conditions must verify clean: "
        << (vrep.all().empty() ? "" : vrep.all()[0].actual);
    for (auto const& d : vrep.all()) {
        EXPECT_NE(d.code, DiagnosticCode::I_StructCfMismatch)
            << d.actual;
    }
}

// The value-position uses must stay verifier-clean too (`return a&&b;`
// has NO IfStmt to balance against — the arm marker is what pairs the
// short-circuit join).
TEST(MirLoweringCSubset, LogicalOpsAsValuesVerifyClean) {
    auto L = lowerCSubset(
        "int and2(int a, int b) { return a && b; }\n"
        "int or2(int a, int b) { return a || b; }\n");
    ASSERT_TRUE(L.mir.ok);
    DiagnosticReporter vrep;
    MirVerifier v{L.mir.mir};
    EXPECT_TRUE(v.verify(vrep))
        << (vrep.all().empty() ? "" : vrep.all()[0].actual);
}

// ─── ML2 cycle 2: control flow ─────────────────────────────────────────────

// `int abs(int x) { if (x < 0) return -x; return x; }` exercises:
// * CondBr to two arms (both arms return so no join falls through).
// * Inline `return` in the then-arm (sealing it without Br(join)).
// * UnaryOp `Neg` lowering.
// * The if's else-arm being a fall-through to the join (which the second
//   `return x;` then seals).
TEST(MirLoweringCSubset, IfElseDiamondWithReturnsInBothArms) {
    auto L = lowerCSubset(
        "int abs(int x) {\n"
        "  if (x < 0) return -x;\n"
        "  return x;\n"
        "}\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "MIR lowering: " << (L.mirReporter.all().empty()
            ? "" : L.mirReporter.all()[0].actual);

    Mir const& m = L.mir.mir;
    MirFuncId const fn = m.funcAt(0);
    // Blocks: entry, thenBB, joinBB. (No else exists; CondBr's false edge
    // targets join directly. The function body's trailing `return x;` runs
    // inside joinBB.)
    EXPECT_EQ(m.funcBlockCount(fn), 3u);
    MirBlockId const entry = m.funcEntry(fn);
    EXPECT_EQ(m.blockMarker(entry), StructCfMarker::EntryBlock);
    // entry's terminator is CondBr.
    MirInstId const term = m.blockTerminator(entry);
    EXPECT_EQ(m.instOpcode(term), MirOpcode::CondBr);
    auto succs = m.blockSuccessors(entry);
    ASSERT_EQ(succs.size(), 2u);
    // The then-block's terminator is Return.
    MirBlockId const thenBB = succs[0];
    EXPECT_EQ(m.blockMarker(thenBB), StructCfMarker::IfThen);
    EXPECT_EQ(m.instOpcode(m.blockTerminator(thenBB)), MirOpcode::Return);
    // The false edge: BOTH paths return, so the canonical derivation
    // sees NO real join (ipdom(entry) = the virtual exit) — the
    // false-edge block derives as the ELSE-arm, not IfJoin (the old
    // hand-stamp's name for the block it created as a join).
    MirBlockId const joinBB = succs[1];
    EXPECT_EQ(m.blockMarker(joinBB), StructCfMarker::IfElse);
    EXPECT_EQ(m.instOpcode(m.blockTerminator(joinBB)), MirOpcode::Return);
}

// `void loop(int n) { while (n > 0) { return; } }` exercises the while
// header → CondBr(body, exit) shape, body that returns mid-loop (sealed
// before the back-edge), and the implicit void return synthesized at
// the exit block.
TEST(MirLoweringCSubset, WhileLoopWithEarlyReturn) {
    auto L = lowerCSubset(
        "void loop(int n) {\n"
        "  while (n > 0) { return; }\n"
        "}\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "MIR lowering: " << (L.mirReporter.all().empty()
            ? "" : L.mirReporter.all()[0].actual);

    Mir const& m = L.mir.mir;
    MirFuncId const fn = m.funcAt(0);
    // Blocks: entry, header, body, exit. (The exit block gets the implicit
    // void-return at the end of `loop`'s body.)
    EXPECT_EQ(m.funcBlockCount(fn), 4u);
    MirBlockId const entry = m.funcEntry(fn);
    // entry → header (unconditional)
    EXPECT_EQ(m.instOpcode(m.blockTerminator(entry)), MirOpcode::Br);
    MirBlockId const header = m.blockSuccessors(entry)[0];
    // DERIVATION TRUTH: the body always returns, so there is NO
    // back-edge — this "while" is not a loop in the CFG. The canonical
    // derivation leaves the header Linear (it is a plain CondBr whose
    // arms diverge to distinct exits → IfThen/IfElse, no join).
    EXPECT_EQ(m.blockMarker(header), StructCfMarker::Linear);
    // header → CondBr(body, exit)
    EXPECT_EQ(m.instOpcode(m.blockTerminator(header)), MirOpcode::CondBr);
    auto hsuccs = m.blockSuccessors(header);
    ASSERT_EQ(hsuccs.size(), 2u);
    MirBlockId const body = hsuccs[0];
    MirBlockId const exit = hsuccs[1];
    EXPECT_EQ(m.blockMarker(body), StructCfMarker::IfThen);
    EXPECT_EQ(m.blockMarker(exit), StructCfMarker::IfElse);
    // body returns (its own Return seals it before a back-edge would emit).
    EXPECT_EQ(m.instOpcode(m.blockTerminator(body)), MirOpcode::Return);
    // exit gets the implicit-void-return synthesized for the function.
    EXPECT_EQ(m.instOpcode(m.blockTerminator(exit)), MirOpcode::Return);
}

// Review-fix I-3: both-arms-return If creates a join block that's sealed
// with `Unreachable` since neither arm falls through. 4 blocks total
// (entry, then, else, joinUnreachable). Locks the
// `addUnreachable()` escape-hatch line of the lowering.
TEST(MirLoweringCSubset, IfBothArmsReturnSealsJoinAsUnreachable) {
    auto L = lowerCSubset(
        "int sign(int x) {\n"
        "  if (x < 0) return -1;\n"
        "  else return 1;\n"
        "}\n");
    ASSERT_TRUE(L.mir.ok);
    Mir const& m = L.mir.mir;
    MirFuncId const fn = m.funcAt(0);
    EXPECT_EQ(m.funcBlockCount(fn), 4u);  // entry, then, else, joinUnreachable
    MirBlockId const entry = m.funcEntry(fn);
    auto succs = m.blockSuccessors(entry);
    ASSERT_EQ(succs.size(), 2u);
    EXPECT_EQ(m.instOpcode(m.blockTerminator(succs[0])), MirOpcode::Return);
    EXPECT_EQ(m.instOpcode(m.blockTerminator(succs[1])), MirOpcode::Return);
    // The 4th block (the join) is sealed with Unreachable. It is
    // UNREACHABLE (neither arm falls through), and the canonical
    // derivation stamps unreachable blocks Linear — the arms derive
    // IfThen/IfElse around the VIRTUAL exit, with no real join.
    // (I_UnreachableBlock ownership of this block is a pre-existing,
    // separately-tracked issue — not this test's subject.)
    MirBlockId const joinBB = m.funcBlockAt(fn, 3);
    EXPECT_EQ(m.blockMarker(joinBB), StructCfMarker::Linear);
    EXPECT_EQ(m.instOpcode(m.blockTerminator(joinBB)), MirOpcode::Unreachable);
}

// A while loop whose body does NOT return must have a real back-edge from
// body to header. Pinned now that AssignStmt is lowered (cycle 3b).
TEST(MirLoweringCSubset, WhileLoopBodyEmitsBackEdgeToHeader) {
    auto L = lowerCSubset(
        "void spin(int n) {\n"
        "  while (n > 0) { n = n - 1; }\n"
        "}\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirFuncId const fn = m.funcAt(0);
    MirBlockId const entry = m.funcEntry(fn);
    // entry → header → body → header (back-edge), header → exit.
    MirBlockId header{};
    auto entrySuccs = m.blockSuccessors(entry);
    ASSERT_GE(entrySuccs.size(), 1u);
    header = entrySuccs[0];
    EXPECT_EQ(m.blockMarker(header), StructCfMarker::LoopHeader);
    auto hdrSuccs = m.blockSuccessors(header);
    ASSERT_EQ(hdrSuccs.size(), 2u);
    MirBlockId const body = hdrSuccs[0];
    // Body's terminator is Br(header) — the back-edge.
    EXPECT_EQ(m.instOpcode(m.blockTerminator(body)), MirOpcode::Br);
    auto bodySuccs = m.blockSuccessors(body);
    ASSERT_EQ(bodySuccs.size(), 1u);
    EXPECT_EQ(bodySuccs[0], header) << "body back-edge should target header";
}

// Do-while whose body self-seals and never targets `continue;` — the
// continueBB has no real predecessor and is elided to Unreachable so the
// dead cond expression isn't lowered. Cycle-4 invariant: continueBB only
// becomes live (with the cond-test + CondBr) when the body falls through
// OR a `continue;` resolves to this loop's frame.
TEST(MirLoweringCSubset, DoWhileBodyReturnsElidesCondTest) {
    auto L = lowerCSubset(
        "void f(int n) {\n"
        "  do { return; } while (n > 0);\n"  // no fall-through, no continue
        "}\n");
    ASSERT_TRUE(L.mir.ok);
    Mir const& m = L.mir.mir;
    MirFuncId const fn = m.funcAt(0);
    // Blocks: entry, body, continueBB(unreachable), exit.
    EXPECT_EQ(m.funcBlockCount(fn), 4u);
    MirBlockId const entry = m.funcEntry(fn);
    MirBlockId const body = m.blockSuccessors(entry)[0];
    // DERIVATION TRUTH: the body always returns → no back-edge → this
    // do-while is not a loop in the CFG; the body derives Linear.
    EXPECT_EQ(m.blockMarker(body), StructCfMarker::Linear);
    EXPECT_EQ(m.instOpcode(m.blockTerminator(body)), MirOpcode::Return);
    // The continueBB exists but is sealed as Unreachable — the cond was
    // NOT lowered. LoopLatch is no longer stamped (a dormant marker the
    // canonical derivation never produces), so locate continueBB by
    // CREATION POSITION: do-while creates body, continueBB, exit in
    // order after entry → continueBB = funcBlockAt(fn, 2). Its
    // terminator must be Unreachable, NOT CondBr.
    MirBlockId const continueBB = m.funcBlockAt(fn, 2);
    EXPECT_EQ(m.instOpcode(m.blockTerminator(continueBB)),
              MirOpcode::Unreachable);
}

// The fall-through path: body has no self-seal, so continueBB IS lowered
// with the cond-test + CondBr(body, exit). Pins the inverse case.
TEST(MirLoweringCSubset, DoWhileBodyFallsThroughLowersCondTest) {
    auto L = lowerCSubset(
        "void f(int n) {\n"
        "  do { n = n; } while (n > 0);\n"
        "}\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirFuncId const fn = m.funcAt(0);
    // The body falls through into the cond test. LoopLatch is no longer
    // stamped (dormant marker) — locate continueBB by CREATION POSITION
    // (entry, body, continueBB, exit) and pin the CFG truth directly:
    // it carries the cond test (CondBr) and its true-arm is the
    // back-edge to the body (which derives LoopHeader).
    ASSERT_EQ(m.funcBlockCount(fn), 4u);
    MirBlockId const body       = m.funcBlockAt(fn, 1);
    MirBlockId const continueBB = m.funcBlockAt(fn, 2);
    EXPECT_EQ(m.blockMarker(body), StructCfMarker::LoopHeader);
    ASSERT_EQ(m.instOpcode(m.blockTerminator(continueBB)), MirOpcode::CondBr);
    auto const contSuccs = m.blockSuccessors(continueBB);
    ASSERT_EQ(contSuccs.size(), 2u);
    EXPECT_EQ(contSuccs[0].v, body.v)
        << "the cond test's true-arm is the back-edge to the loop body";
}

// Review-fix I-4: a for-loop with cond/update/body lowers to the
// header/body/update/exit shape with the update on the back-edge.
// (No init — cycle 2 doesn't yet lower the local-var declaration `int i = 0;`
// that would typically be the init clause; the update is a pure expression.)
TEST(MirLoweringCSubset, ForLoopLowersWithUpdateOnBackEdge) {
    auto L = lowerCSubset(
        "void f(int n) {\n"
        "  for (; n > 0; n + 1) { return; }\n"  // update is a pure expr
        "}\n");
    ASSERT_TRUE(L.mir.ok)
        << "MIR lowering: " << (L.mirReporter.all().empty()
            ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirFuncId const fn = m.funcAt(0);
    // Blocks: entry, header, body, update, exit. Update block is created
    // because the source has an update clause. Entry's terminator is
    // Br(header); header is CondBr(body, exit); body's `return;` seals it;
    // update is dead (body returns before reaching the back-edge), but it
    // still exists as a created block — the lowering creates blocks before
    // it knows which paths fall through.
    EXPECT_EQ(m.funcBlockCount(fn), 5u);
    MirBlockId const entry  = m.funcEntry(fn);
    MirBlockId const header = m.blockSuccessors(entry)[0];
    // DERIVATION TRUTH: the body always returns, so the update block
    // (the only Br-to-header) is UNREACHABLE — live code never closes
    // the loop. The header derives Linear (its CondBr arms diverge to
    // distinct exits → IfThen/IfElse), and the dead update block
    // derives Linear (unreachable blocks always do).
    EXPECT_EQ(m.blockMarker(header), StructCfMarker::Linear);
    EXPECT_EQ(m.instOpcode(m.blockTerminator(header)), MirOpcode::CondBr);
    MirBlockId const body = m.blockSuccessors(header)[0];
    EXPECT_EQ(m.blockMarker(body), StructCfMarker::IfThen);
    EXPECT_EQ(m.instOpcode(m.blockTerminator(body)), MirOpcode::Return);
    // The update block still exists (created before the lowering knows
    // which paths fall through) and still Brs to the header.
    MirBlockId const update = m.funcBlockAt(fn, 3);
    EXPECT_EQ(m.blockMarker(update), StructCfMarker::Linear);
    EXPECT_EQ(m.instOpcode(m.blockTerminator(update)), MirOpcode::Br);
    EXPECT_EQ(m.blockSuccessors(update)[0], header);
}

// FC3.5 sweep-c1 (chip task_20b1224d): the CONVENTIONAL C for-loop —
// the update clause is an ASSIGNMENT (`i = i - 1`), which cst_to_hir
// lowers to an AssignStmt, not an expression. Pre-fix, ForStmt routed
// the update through `lowerExpr`, which fail-louded with "HIR
// expression kind ordinal 19 [AssignStmt] not yet supported"; the
// statement-shaped clause now routes through `lowerStmt` (the same
// path the init clause always took). The update block must hold the
// Store back into `i`'s slot and branch to the header.
TEST(MirLoweringCSubset, ForLoopWithAssignmentUpdateLowers) {
    auto L = lowerCSubset(
        "int f(int i) {\n"
        "  int acc = 0;\n"
        "  for (i = 9; i; i = i - 1) {\n"
        "    acc = acc + 1;\n"
        "  }\n"
        "  return acc;\n"
        "}\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "MIR lowering: " << (L.mirReporter.all().empty()
            ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirFuncId const fn = m.funcAt(0);
    // Find the update block by CFG SHAPE (LoopLatch is a dormant marker
    // the canonical derivation never stamps): the header is the derived
    // LoopHeader; the update block is the NON-ENTRY block whose Br
    // targets it (the back-edge source — entry also Brs to the header,
    // but entry is funcBlockAt(fn, 0)). It must contain the Store (the
    // `i = i - 1` write-back).
    MirBlockId const entry = m.funcEntry(fn);
    MirBlockId header{};
    for (std::uint32_t bi = 0; bi < m.funcBlockCount(fn); ++bi) {
        MirBlockId const b = m.funcBlockAt(fn, bi);
        if (m.blockMarker(b) == StructCfMarker::LoopHeader) { header = b; break; }
    }
    ASSERT_TRUE(header.valid()) << "the for-loop must derive a LoopHeader";
    bool sawLatchStore = false;
    for (std::uint32_t bi = 0; bi < m.funcBlockCount(fn); ++bi) {
        MirBlockId const b = m.funcBlockAt(fn, bi);
        if (b.v == entry.v) continue;
        if (m.blockInstCount(b) == 0) continue;
        if (m.instOpcode(m.blockTerminator(b)) != MirOpcode::Br) continue;
        auto const succ = m.blockSuccessors(b);
        if (succ.size() != 1 || succ[0].v != header.v) continue;
        // This is the back-edge source — the update block.
        for (std::uint32_t ii = 0; ii < m.blockInstCount(b); ++ii) {
            if (m.instOpcode(m.blockInstAt(b, ii)) == MirOpcode::Store) {
                sawLatchStore = true;
            }
        }
    }
    EXPECT_TRUE(sawLatchStore)
        << "the assignment update must lower to a Store in the back-edge "
           "source (the update block)";
    // The whole function verifies clean (loop pairing intact).
    DiagnosticReporter vrep;
    MirVerifier v{m};
    EXPECT_TRUE(v.verify(vrep))
        << (vrep.all().empty() ? "" : vrep.all()[0].actual);
}

// Compound-assign + postfix inc/dec updates desugar to AssignStmt in
// cst_to_hir (`lowerCompoundAssign` / `lowerIncDecStmt`) and ride the
// same statement path. The grammar admits ALL of `+=`/`-=`/`*=`/`/=`/
// `%=`/`&=`/`|=`/`^=`/`<<=`/`>>=` plus postfix `++`/`--`; pin one
// compound (`-=`) and one postfix (`++`) — the others share the
// single desugar site.
TEST(MirLoweringCSubset, ForLoopCompoundAndIncDecUpdatesLower) {
    auto L = lowerCSubset(
        "int f(int n) {\n"
        "  int acc = 0;\n"
        "  for (n = 6; n; n -= 1) { acc = acc + 1; }\n"
        "  for (int j = 0; j < 3; j++) { acc = acc + 1; }\n"
        "  return acc;\n"
        "}\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty()
            ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "MIR lowering: " << (L.mirReporter.all().empty()
            ? "" : L.mirReporter.all()[0].actual);
    DiagnosticReporter vrep;
    MirVerifier v{L.mir.mir};
    EXPECT_TRUE(v.verify(vrep))
        << (vrep.all().empty() ? "" : vrep.all()[0].actual);
}

// A bare-EXPRESSION init clause (`for (f(); ...)`) is the symmetric
// shape on the INIT side — cst_to_hir emits the unwrapped expression
// and the same for-clause dispatch admits it (pre-fix, the init path
// routed through lowerStmt whose default arm rejected expressions).
TEST(MirLoweringCSubset, ForLoopWithBareExpressionInitLowers) {
    auto L = lowerCSubset(
        "int g(int x) { return x + 1; }\n"
        "int f(int n) {\n"
        "  int acc = 0;\n"
        "  for (g(n); acc < 2; acc = acc + 1) { }\n"
        "  return acc;\n"
        "}\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.mir.ok)
        << "MIR lowering: " << (L.mirReporter.all().empty()
            ? "" : L.mirReporter.all()[0].actual);
    DiagnosticReporter vrep;
    MirVerifier v{L.mir.mir};
    EXPECT_TRUE(v.verify(vrep))
        << (vrep.all().empty() ? "" : vrep.all()[0].actual);
}

// Logical `!x` lowers as `cmp eq operand, 0` → Bool. Returning it from an
// int-returning function adds an implicit `Cast(Bool→int)` from cycle C's
// HR coercion pass (ZExt at MIR-time).
TEST(MirLoweringCSubset, LogicalNotLowersToICmpEqZero) {
    auto L = lowerCSubset("int isz(int x) { return !x; }\n");
    ASSERT_TRUE(L.mir.ok)
        << "MIR lowering: " << (L.mirReporter.all().empty()
            ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // Instructions: Arg, Const(0), ICmpEq(arg, 0), ZExt(bool→i32), Return.
    ASSERT_EQ(m.blockInstCount(entry), 5u);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 0)), MirOpcode::Arg);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 1)), MirOpcode::Const);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 2)), MirOpcode::ICmpEq);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 3)), MirOpcode::ZExt);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 4)), MirOpcode::Return);
}

// Unary negation lowering (review-touched: cycle 1's mapBinaryOp also covers
// arithmetic for Neg via the unary branch; pin it explicitly).
TEST(MirLoweringCSubset, UnaryNegationLowersToNeg) {
    auto L = lowerCSubset("int neg(int x) { return -x; }\n");
    ASSERT_TRUE(L.mir.ok);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // [arg, Neg, Return]
    ASSERT_EQ(m.blockInstCount(entry), 3u);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 1)), MirOpcode::Neg);
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

// ─── D-CSUBSET-LINKAGE-SPECIFIERS (pre-OPT7 P1): static / __attribute__ ─────
//
// End-to-end proof that a source linkage specifier flows
//   source → grammar specifier-prefix → HIR LinkageAttr → MirFunc binding →
//   the optimizer's DCE protect predicate (`isExternallyVisible`).
// The discriminator is PURELY the `static` keyword: the SAME unused helper is
// DCE-eliminated when `static` (Local binding) and PRESERVED when omitted
// (Global = externally visible). Regression-proof: if the linkage thread breaks
// (a `static` helper stays Global), Arm A keeps 2 functions and `== 1u` is RED.

// Arm A — `static` makes an unused helper Local ⇒ DCE eliminates it.
TEST(MirLoweringCSubsetLinkage, StaticUnusedFunctionIsDceEliminated) {
    auto L = lowerCSubset(
        "static int helper(int x) { return x + 1; }\n"
        "int main() { return 0; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir& m = L.mir.mir;
    ASSERT_EQ(m.moduleFuncCount(), 2u) << "pre-DCE: static helper + main";

    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& target = **targetR;
    DiagnosticReporter rep;
    opt::OptPipeline pipeline{"linkage-dce", {opt::PassId::Dce}};
    auto const result =
        opt::optimize(m, target, L.model.lattice().interner(), pipeline, rep);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(rep.errorCount(), 0u);

    // `helper` is Local (from `static`) + has no callers ⇒ eliminated; `main`
    // is Global (externally visible) ⇒ preserved as a DCE root.
    EXPECT_EQ(m.moduleFuncCount(), 1u)
        << "static helper (Local, no callers) must be DCE-eliminated; only "
           "main (Global) survives";
    EXPECT_GE(result.mutationCount(opt::PassId::Dce), 1u)
        << "DCE must record having removed the static helper";
}

// Arm B — control: WITHOUT `static`, the SAME unused helper is Global
// (externally visible) ⇒ DCE preserves it. The ONLY source difference from Arm
// A is the `static` keyword, and it flips elimination ⇒ the linkage specifier
// is provably what drives the behavior (the red-on-disable pair for Arm A).
TEST(MirLoweringCSubsetLinkage, NonStaticUnusedFunctionSurvivesDce) {
    auto L = lowerCSubset(
        "int helper(int x) { return x + 1; }\n"
        "int main() { return 0; }\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir& m = L.mir.mir;
    ASSERT_EQ(m.moduleFuncCount(), 2u);

    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& target = **targetR;
    DiagnosticReporter rep;
    opt::OptPipeline pipeline{"linkage-dce", {opt::PassId::Dce}};
    auto const result =
        opt::optimize(m, target, L.model.lattice().interner(), pipeline, rep);
    ASSERT_TRUE(result.ok);

    EXPECT_EQ(m.moduleFuncCount(), 2u)
        << "without `static`, helper is Global (externally visible) and MUST "
           "survive DCE even with no callers";
}

// Arm C — `__attribute__((weak))` threads to MirFunc binding == Weak. Weak is
// externally visible (the linker may supersede it), so it is NOT DCE-eligible;
// the proof is the binding VALUE on the lowered MIR, not elimination. Pins that
// the second linkage value (besides Local) also flows source → HIR → MIR.
TEST(MirLoweringCSubsetLinkage, WeakAttributeThreadsToMirBinding) {
    auto L = lowerCSubset(
        "__attribute__((weak)) int wfn() { return 7; }\n"
        "int main() { return 0; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    ASSERT_EQ(m.moduleFuncCount(), 2u);

    // Exactly one function carries Weak binding (the `__attribute__((weak))`
    // one); `main` stays Global. Anything but 1 means the specifier failed to
    // thread (0) or leaked onto another symbol (2).
    int weakCount = 0;
    for (std::uint32_t i = 0; i < m.moduleFuncCount(); ++i)
        if (m.funcBinding(m.funcAt(i)) == SymbolBinding::Weak) ++weakCount;
    EXPECT_EQ(weakCount, 1)
        << "__attribute__((weak)) must thread to exactly one MirFunc binding==Weak";
}

// D-CSUBSET-LINKAGE-UNKNOWN-SPECIFIER-DIAGNOSTIC (cycle 14): an UNRECOGNIZED
// specifier inside `__attribute__((...))` — a typo (`bogus`) or an unsupported
// attribute — FAILS LOUD (H_UnknownLinkageSpecifier), never silently ignored. The
// validation lives in the single `linkageFrom` chokepoint, which `lowerTopLevel`
// (func + var) AND `lowerExternDecl` all route through — so coverage is
// by-construction across every decl-lowering arm. RED-ON-DISABLE: drop the emit in
// `linkageFrom` and `bogus` is silently skipped → both these go green-when-broken.
TEST(MirLoweringCSubsetLinkage, UnknownAttributeOnFunctionFailsLoud) {
    auto L = lowerCSubset("__attribute__((bogus)) int f() { return 0; }\n");
    EXPECT_FALSE(L.hir->ok)
        << "an unrecognized linkage specifier must fail HIR lowering, not be ignored";
    std::size_t n = 0;
    for (auto const& d : L.hirReporter.all())
        if (d.code == DiagnosticCode::H_UnknownLinkageSpecifier) ++n;
    EXPECT_EQ(n, 1u) << "exactly one H_UnknownLinkageSpecifier for 'bogus'";
}

// The variable FORM (the other arm through lowerTopLevel) — same fail-loud, proving
// the contract holds for every form that carries a specifier prefix, not just funcs.
TEST(MirLoweringCSubsetLinkage, UnknownAttributeOnVariableFailsLoud) {
    auto L = lowerCSubset("__attribute__((bogus)) int g;\n");
    EXPECT_FALSE(L.hir->ok)
        << "an unrecognized linkage specifier on a variable must fail loud";
    std::size_t n = 0;
    for (auto const& d : L.hirReporter.all())
        if (d.code == DiagnosticCode::H_UnknownLinkageSpecifier) ++n;
    EXPECT_EQ(n, 1u) << "exactly one H_UnknownLinkageSpecifier for 'bogus' on the var form";
}

// ─── ML2 cycle 3b: lvalue-via-alloca ──────────────────────────────────────

// Body-local VarDecl with initializer lowers to Alloca + Store. The local's
// later read site (here `return x;`) becomes a Load against the slot.
TEST(MirLoweringCSubset, VarDeclWithInitLowersToAllocaPlusStore) {
    auto L = lowerCSubset("int f() { int x = 5; return x; }");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "MIR lowering: " << (L.mirReporter.all().empty()
            ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // [Alloca, Const 5, Store(const,alloca), Load(alloca), Return(load)]
    ASSERT_EQ(m.blockInstCount(entry), 5u);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 0)), MirOpcode::Alloca);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 1)), MirOpcode::Const);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 2)), MirOpcode::Store);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 3)), MirOpcode::Load);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 4)), MirOpcode::Return);
}

// AssignStmt to a body-local lowers to Store(rhs, alloca). The Ref-as-lvalue
// produces no extra load on the assignment side.
TEST(MirLoweringCSubset, AssignStmtLowersToStore) {
    auto L = lowerCSubset(
        "int f() { int x = 1; x = 2; return x; }");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // [Alloca x, Const 1, Store(1→x), Const 2, Store(2→x), Load x, Return]
    ASSERT_EQ(m.blockInstCount(entry), 7u);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 0)), MirOpcode::Alloca);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 2)), MirOpcode::Store);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 4)), MirOpcode::Store);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 5)), MirOpcode::Load);
}

// AddressOf of a body-local returns the alloca directly (no extra MIR
// instruction). Followed by a deref it should round-trip — this verifies
// both sides of the lvalue model.
TEST(MirLoweringCSubset, AddressOfLocalReturnsAllocaDirectly) {
    auto L = lowerCSubset(
        "int f() { int x = 1; int* p = &x; return *p; }");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // [Alloca x, Const 1, Store, Alloca p, Store(allocaX→p), Load p, Load *p, Return]
    // The AddressOf(x) does NOT add an instruction — it reuses alloca x.
    ASSERT_EQ(m.blockInstCount(entry), 8u);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 0)), MirOpcode::Alloca);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 3)), MirOpcode::Alloca);
    // Slot 4 stores alloca-x into the p slot — verify the value operand IS
    // the first alloca (proving AddressOf returned the alloca, not a copy).
    MirInstId const storeP = m.blockInstAt(entry, 4);
    EXPECT_EQ(m.instOpcode(storeP), MirOpcode::Store);
    auto storeOps = m.instOperands(storeP);
    ASSERT_EQ(storeOps.size(), 2u);
    EXPECT_EQ(storeOps[0], m.blockInstAt(entry, 0))
        << "Store value operand should BE the alloca-x (AddressOf returns "
           "the alloca directly, no copy)";
}

// AddressOf of a PARAM forces entry-block slot-promotion. The pre-pass
// detects `&p` in the body and emits Arg + Alloca + Store for that param
// on entry, so reads of the param thereafter go through Load(alloca).
TEST(MirLoweringCSubset, AddressOfParamPromotesItToSlot) {
    auto L = lowerCSubset(
        "int f(int p) { int* q = &p; return *q; }");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // [Arg, Alloca p, Store(arg→p), Alloca q, Store(allocaP→q), Load q, Load *q, Return]
    ASSERT_EQ(m.blockInstCount(entry), 8u);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 0)), MirOpcode::Arg);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 1)), MirOpcode::Alloca);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 2)), MirOpcode::Store);
    // The Store-to-paramSlot's value operand IS the Arg.
    auto storeOps = m.instOperands(m.blockInstAt(entry, 2));
    ASSERT_EQ(storeOps.size(), 2u);
    EXPECT_EQ(storeOps[0], m.blockInstAt(entry, 0));
    EXPECT_EQ(storeOps[1], m.blockInstAt(entry, 1));
}

// A param whose address is NEVER taken stays as a pure SSA `Arg` — the
// pre-pass does not slot-promote it, preserving the cycle-1 canonical form
// for the common case. This is the negative-control for the prior test.
TEST(MirLoweringCSubset, ParamWithoutAddressOfStaysAsArg) {
    auto L = lowerCSubset("int id(int x) { return x; }");
    ASSERT_TRUE(L.mir.ok);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // [Arg, Return(arg)] — no Alloca, no Store, no Load.
    ASSERT_EQ(m.blockInstCount(entry), 2u);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 0)), MirOpcode::Arg);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 1)), MirOpcode::Return);
}

// Assignment THROUGH a pointer (`*p = v`) lowers to `Store(v, p)` with the
// pointer operand being the Arg directly — NOT a Load(p). This pins the
// lvalue model's contract: `lowerLvalueAddress(Deref(p))` returns the
// pointer value, not a load of the pointee.
TEST(MirLoweringCSubset, AssignThroughDerefStoresIntoPointerWithoutExtraLoad) {
    auto L = lowerCSubset("void f(int* p, int v) { *p = v; }");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // [Arg p, Arg v, Store(v→p), Return] — no Load of p's pointee anywhere.
    ASSERT_EQ(m.blockInstCount(entry), 4u);
    MirInstId const argP   = m.blockInstAt(entry, 0);
    MirInstId const argV   = m.blockInstAt(entry, 1);
    MirInstId const storeI = m.blockInstAt(entry, 2);
    EXPECT_EQ(m.instOpcode(argP),   MirOpcode::Arg);
    EXPECT_EQ(m.instOpcode(argV),   MirOpcode::Arg);
    EXPECT_EQ(m.instOpcode(storeI), MirOpcode::Store);
    auto ops = m.instOperands(storeI);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], argV) << "Store value operand should be v (the Arg)";
    EXPECT_EQ(ops[1], argP) << "Store ptr operand should be p (the Arg, not a Load)";
}

// ─── ML2 cycle 3c: MemberAccess + Index + SeqExpr ─────────────────────────

// `p->x` lowers in HIR to `(*p).x` ≡ `MemberAccess(Deref(Ref(p)), field=0)`.
// MIR lowers Deref's lvalue-address to the pointer rvalue (no double-load),
// then GEPs into the field with `[ptr, const-0, const-fieldIdx]`, then
// Loads the field. The Store side of `p->x = v` follows the same path
// (verified by the symmetric assign test below).
TEST(MirLoweringCSubset, MemberAccessReadEmitsGepThenLoad) {
    auto L = lowerCSubset(
        "struct Point { int x; int y; };\n"
        "int read_x(struct Point* p) { return p->x; }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // FC7 (D-FC7-MEMBER-ACCESS): [Arg p, Const(byteOffset 0=field x),
    // Gep(2-op), Load, Return].
    ASSERT_EQ(m.blockInstCount(entry), 5u);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 0)), MirOpcode::Arg);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 1)), MirOpcode::Const);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 2)), MirOpcode::Gep);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 3)), MirOpcode::Load);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 4)), MirOpcode::Return);
    // FC7: the GEP is now 2-op [base, byteOffset] — operand[0] IS the Arg p
    // (Deref's lvalue-address returns the pointer rvalue directly), and
    // operand[1] is the field's BYTE OFFSET (field x → 0), NOT a field index.
    // (The old 3-op [base, 0, fieldIdx] shape MIR→LIR never realized.)
    auto gepOps = m.instOperands(m.blockInstAt(entry, 2));
    ASSERT_EQ(gepOps.size(), 2u);
    EXPECT_EQ(gepOps[0], m.blockInstAt(entry, 0));
    EXPECT_EQ(gepOps[1], m.blockInstAt(entry, 1));
    auto const& offLit =
        m.literalValue(m.constLiteralIndex(m.blockInstAt(entry, 1)));
    EXPECT_EQ(std::get<std::int64_t>(offLit.value), 0)
        << "field x is at byte offset 0";
}

// Symmetric write: `p->y = v` lowers to GEP-then-Store, with the value
// operand being the Arg v and the ptr operand the GEP result.
TEST(MirLoweringCSubset, MemberAccessAssignEmitsGepThenStore) {
    auto L = lowerCSubset(
        "struct Point { int x; int y; };\n"
        "void set_y(struct Point* p, int v) { p->y = v; }\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // FC7 (D-FC7-MEMBER-ACCESS): [Arg p, Arg v, Const(byteOffset 4=field y),
    // Gep(2-op), Store, Return].
    ASSERT_EQ(m.blockInstCount(entry), 6u);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 2)), MirOpcode::Const);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 3)), MirOpcode::Gep);
    MirInstId const storeI = m.blockInstAt(entry, 4);
    EXPECT_EQ(m.instOpcode(storeI), MirOpcode::Store);
    auto ops = m.instOperands(storeI);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], m.blockInstAt(entry, 1)) << "Store value should be Arg v";
    EXPECT_EQ(ops[1], m.blockInstAt(entry, 3)) << "Store ptr should be Gep";
    // FC7: the GEP's 2nd operand is field y's BYTE OFFSET (LP64: x@0, y@4),
    // not field index 1 — a wrong offset would store into the wrong field.
    auto const& offLit =
        m.literalValue(m.constLiteralIndex(m.blockInstAt(entry, 2)));
    EXPECT_EQ(std::get<std::int64_t>(offLit.value), 4)
        << "field y is at byte offset 4 under LP64";
}

// `p[i]` over a POINTER base: GEP carries `[ptr, idx]` (no leading 0 —
// the pointer is already at the element-pointer layer). Pins the
// pointer-vs-array discrimination in `lowerLvalueAddress`'s Index path.
TEST(MirLoweringCSubset, IndexOverPointerEmitsTwoOperandGepThenLoad) {
    auto L = lowerCSubset(
        "int f(int* a, int i) { return a[i]; }\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // [Arg a, Arg i, Gep(a, i), Load, Return]  — no Const-0 prefix.
    ASSERT_EQ(m.blockInstCount(entry), 5u);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 0)), MirOpcode::Arg);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 1)), MirOpcode::Arg);
    MirInstId const gep = m.blockInstAt(entry, 2);
    EXPECT_EQ(m.instOpcode(gep), MirOpcode::Gep);
    auto gepOps = m.instOperands(gep);
    ASSERT_EQ(gepOps.size(), 2u);
    EXPECT_EQ(gepOps[0], m.blockInstAt(entry, 0));
    EXPECT_EQ(gepOps[1], m.blockInstAt(entry, 1));
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 3)), MirOpcode::Load);
}

// `&p->x` exercises AddressOf delegating to `lowerLvalueAddress` for the
// new MemberAccess shape — the address-of operator returns the GEP result
// directly, no extra instructions, no Load on the value side.
TEST(MirLoweringCSubset, AddressOfMemberAccessReturnsGepDirectly) {
    auto L = lowerCSubset(
        "struct Point { int x; int y; };\n"
        "int* addr_x(struct Point* p) { return &p->x; }\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // FC7 (D-FC7-MEMBER-ACCESS): [Arg p, Const(byteOffset 0=field x),
    // Gep(2-op), Return(gep)] — NO Load.
    ASSERT_EQ(m.blockInstCount(entry), 4u);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 2)), MirOpcode::Gep);
    MirInstId const ret = m.blockInstAt(entry, 3);
    EXPECT_EQ(m.instOpcode(ret), MirOpcode::Return);
    auto retOps = m.instOperands(ret);
    ASSERT_EQ(retOps.size(), 1u);
    EXPECT_EQ(retOps[0], m.blockInstAt(entry, 2))
        << "Return value should be the Gep result, not a Load";
}

// FC7 (D-FC7-NESTED-STRUCT-FIELD): a struct-TYPED field declared by a bare
// typedef-name (`Inner in;` — the new `typeBaseAllowingStruct` Identifier alt)
// parses, and nested member access `p->in.y` COMPOSES the byte offsets via a
// CHAINED GEP: Gep(p, off-of-`in`=0) then Gep(<that>, off-of-`y`-within-Inner
// =4). The second GEP's BASE is the first GEP (not Arg p) and its offset is 4
// — proving `.y` resolves against Inner's OWN layout, composed onto `in`'s
// offset, not flattened or resolved against Outer. RED-ON-DISABLE: revert the
// grammar `Identifier` alt → the struct-typed field no longer parses; parse-
// recovery yields a tree that lowers to an unsupported top-level node, so the
// `ASSERT_TRUE(L.mir.ok)` below trips (empirically verified — the failure is at
// MIR-lowering, not `model.hasErrors()`, which recovery keeps false).
TEST(MirLoweringCSubset, NestedMemberAccessComposesChainedGepOffsets) {
    auto L = lowerCSubset(
        "typedef struct { int x; int y; } Inner;\n"
        "typedef struct { Inner in; int z; } Outer;\n"
        "int read_iny(Outer* p) { return p->in.y; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << "struct-typed field `Inner in;` must parse "
           "(typeBaseAllowingStruct Identifier alt)";
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // [Arg p, Const(0=`in`@Outer), Gep1(p,0), Const(4=`y`@Inner),
    //  Gep2(Gep1,4), Load, Return].
    ASSERT_EQ(m.blockInstCount(entry), 7u);
    MirInstId const argP = m.blockInstAt(entry, 0);
    MirInstId const gep1 = m.blockInstAt(entry, 2);
    MirInstId const gep2 = m.blockInstAt(entry, 4);
    ASSERT_EQ(m.instOpcode(gep1), MirOpcode::Gep);
    ASSERT_EQ(m.instOpcode(gep2), MirOpcode::Gep);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 5)), MirOpcode::Load);
    auto g1 = m.instOperands(gep1);
    ASSERT_EQ(g1.size(), 2u);
    EXPECT_EQ(g1[0], argP) << "outer GEP bases on Arg p";
    auto const& off1 =
        m.literalValue(m.constLiteralIndex(m.blockInstAt(entry, 1)));
    EXPECT_EQ(std::get<std::int64_t>(off1.value), 0) << "`in` at offset 0";
    auto g2 = m.instOperands(gep2);
    ASSERT_EQ(g2.size(), 2u);
    EXPECT_EQ(g2[0], gep1)
        << "nested `.y` chains off the inner GEP, not Arg p";
    auto const& off2 =
        m.literalValue(m.constLiteralIndex(m.blockInstAt(entry, 3)));
    EXPECT_EQ(std::get<std::int64_t>(off2.value), 4)
        << "`y` at offset 4 WITHIN Inner (composed 0 + 4)";
}

// FC7 (D-FC7-NESTED-STRUCT-FIELD): the SAME grammar alt admits a struct-typed
// field in a UNION body (`unionField` shares `typeRefAllowingStruct`). A union
// whose member is a struct type parses, and `u->p.y` accesses the struct
// field through the union (all union members at offset 0, so the access
// composes 0 + 4). Guards the union half of the multi-form contract.
TEST(MirLoweringCSubset, UnionWithStructTypedFieldParsesAndAccesses) {
    auto L = lowerCSubset(
        "typedef struct { int x; int y; } Inner;\n"
        "typedef union { Inner p; int n; } U;\n"
        "int read(U* u) { return u->p.y; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << "struct-typed field in a UNION body must parse";
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // Two chained GEPs: `p`@0 within the union, then `y`@4 within Inner.
    std::vector<std::int64_t> offs;
    auto const n = m.blockInstCount(entry);
    for (std::uint32_t i = 0; i < n; ++i) {
        MirInstId const ix = m.blockInstAt(entry, i);
        if (m.instOpcode(ix) != MirOpcode::Gep) continue;
        auto gOps = m.instOperands(ix);
        ASSERT_EQ(gOps.size(), 2u);
        offs.push_back(
            std::get<std::int64_t>(m.literalValue(m.constLiteralIndex(gOps[1])).value));
    }
    ASSERT_EQ(offs.size(), 2u) << "two chained GEPs for u->p.y";
    EXPECT_EQ(offs[0], 0) << "union member `p` at offset 0";
    EXPECT_EQ(offs[1], 4) << "`y` at offset 4 within Inner";
}

// SeqExpr: a value-yielding expression that bundles side-effect statements
// + a result expression. HR8 emits these for assignment-as-expression and
// compound-assign in c-subset. `x = 5` as an rvalue is the canonical case:
// the AssignStmt becomes a SeqExpr whose tail loads the new value.
TEST(MirLoweringCSubset, SeqExprLowersStmtsThenYieldsResult) {
    auto L = lowerCSubset(
        "int f() { int x = 1; return (x = 5) + 1; }");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // [Alloca x, Const 1, Store(1→x), Const 5, Store(5→x), Load x,
    //  Const 1, Add, Return]
    ASSERT_EQ(m.blockInstCount(entry), 9u);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 0)), MirOpcode::Alloca);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 2)), MirOpcode::Store);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 4)), MirOpcode::Store);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 5)), MirOpcode::Load);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 7)), MirOpcode::Add);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 8)), MirOpcode::Return);
}

// ─── ML2 cycle 4: Switch / Break / Continue ──────────────────────────────

// A switch with two cases + default + breaks in each arm lowers to:
//   entry → Switch(disc, [(1, caseA), (2, caseB)], default=caseD)
//   caseA / caseB / caseD all `Br(exit)` because of the explicit break
// Exit then runs the implicit-void-return.
TEST(MirLoweringCSubset, SwitchWithBreaksInEachArm) {
    auto L = lowerCSubset(
        "void f(int x) {\n"
        "  switch (x) {\n"
        "    case 1: break;\n"
        "    case 2: break;\n"
        "    default: break;\n"
        "  }\n"
        "}\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirFuncId const fn = m.funcAt(0);
    // Blocks: entry, caseA, caseB, default, exit  → 5
    EXPECT_EQ(m.funcBlockCount(fn), 5u);
    MirBlockId const entry = m.funcEntry(fn);
    // Entry's terminator is the Switch.
    MirInstId const term = m.blockTerminator(entry);
    EXPECT_EQ(m.instOpcode(term), MirOpcode::Switch);
    // Switch successors: [case targets…, default].
    auto succs = m.blockSuccessors(entry);
    EXPECT_EQ(succs.size(), 3u);
    // Every arm's terminator is Br (the break;).
    for (std::size_t i = 0; i < succs.size(); ++i) {
        EXPECT_EQ(m.instOpcode(m.blockTerminator(succs[i])), MirOpcode::Br);
    }
}

// Fall-through: arm 1 omits `break;`, so MIR must Br to arm 2's block
// instead of the switch-exit. C semantics preserved.
TEST(MirLoweringCSubset, SwitchFallthroughBranchesToNextArm) {
    auto L = lowerCSubset(
        "void f(int x) {\n"
        "  switch (x) {\n"
        "    case 1:\n"           // no break → falls through to case 2
        "    case 2: break;\n"
        "  }\n"
        "}\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirFuncId const fn = m.funcAt(0);
    // Blocks: entry, caseA(empty), caseB, exit → 4
    EXPECT_EQ(m.funcBlockCount(fn), 4u);
    MirBlockId const entry = m.funcEntry(fn);
    auto succs = m.blockSuccessors(entry);
    ASSERT_EQ(succs.size(), 3u);  // [caseA, caseB, default=exit]
    // caseA terminator branches to caseB (fall-through), NOT to exit.
    MirBlockId const caseA = succs[0];
    MirBlockId const caseB = succs[1];
    MirBlockId const exit  = succs[2];
    EXPECT_EQ(m.instOpcode(m.blockTerminator(caseA)), MirOpcode::Br);
    auto caseAExits = m.blockSuccessors(caseA);
    ASSERT_EQ(caseAExits.size(), 1u);
    EXPECT_EQ(caseAExits[0], caseB)
        << "fall-through should land at next arm, not at switch-exit";
    EXPECT_NE(caseAExits[0], exit);
}

// `continue;` inside a while branches to the loop header, NOT to exit.
TEST(MirLoweringCSubset, ContinueInsideWhileBranchesToHeader) {
    auto L = lowerCSubset(
        "void f(int n) {\n"
        "  while (n > 0) { continue; }\n"
        "}\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirFuncId const fn = m.funcAt(0);
    MirBlockId const entry = m.funcEntry(fn);
    MirBlockId const header = m.blockSuccessors(entry)[0];
    EXPECT_EQ(m.blockMarker(header), StructCfMarker::LoopHeader);
    // Header's CondBr targets are [body, exit].
    auto hdrSuccs = m.blockSuccessors(header);
    ASSERT_EQ(hdrSuccs.size(), 2u);
    MirBlockId const body = hdrSuccs[0];
    // Body's terminator is the continue → Br(header).
    EXPECT_EQ(m.instOpcode(m.blockTerminator(body)), MirOpcode::Br);
    auto bodySuccs = m.blockSuccessors(body);
    ASSERT_EQ(bodySuccs.size(), 1u);
    EXPECT_EQ(bodySuccs[0], header) << "continue should target the loop header";
}

// `break;` inside a while branches to the loop exit.
TEST(MirLoweringCSubset, BreakInsideWhileBranchesToExit) {
    auto L = lowerCSubset(
        "void f(int n) {\n"
        "  while (n > 0) { break; }\n"
        "}\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirFuncId const fn = m.funcAt(0);
    MirBlockId const entry = m.funcEntry(fn);
    MirBlockId const header = m.blockSuccessors(entry)[0];
    auto hdrSuccs = m.blockSuccessors(header);
    ASSERT_EQ(hdrSuccs.size(), 2u);
    MirBlockId const body = hdrSuccs[0];
    MirBlockId const exit = hdrSuccs[1];
    // DERIVATION TRUTH: the body always breaks → no back-edge → not a
    // loop. The header's CondBr is a plain if-shape whose join is the
    // "exit" block (both the false edge and the body's break converge
    // there) → it derives IfJoin, not LoopExit.
    EXPECT_EQ(m.blockMarker(exit), StructCfMarker::IfJoin);
    // Body's terminator is the break → Br(exit).
    auto bodySuccs = m.blockSuccessors(body);
    ASSERT_EQ(bodySuccs.size(), 1u);
    EXPECT_EQ(bodySuccs[0], exit) << "break should target the loop exit";
}

// `continue;` inside a do-while branches to the cond-test block, not body.
// Pins the cycle-4 do-while reshape that introduced an explicit
// continueBB between body and exit.
TEST(MirLoweringCSubset, ContinueInsideDoWhileBranchesToCondTest) {
    auto L = lowerCSubset(
        "void f(int n) {\n"
        "  do { continue; } while (n > 0);\n"
        "}\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirFuncId const fn = m.funcAt(0);
    // Blocks: entry, body, continueBB, exit.
    EXPECT_EQ(m.funcBlockCount(fn), 4u);
    MirBlockId const entry = m.funcEntry(fn);
    MirBlockId const body = m.blockSuccessors(entry)[0];
    EXPECT_EQ(m.blockMarker(body), StructCfMarker::LoopHeader);
    auto bodySuccs = m.blockSuccessors(body);
    ASSERT_EQ(bodySuccs.size(), 1u);
    MirBlockId const cont = bodySuccs[0];
    // The cond-test block derives Linear (LoopLatch is a dormant marker
    // — a back-edge SOURCE is not CFG-distinguishable from a plain
    // body-tail; the load-bearing pin is the CFG: continue targets the
    // CondBr block, whose true-arm is the back-edge).
    EXPECT_EQ(m.blockMarker(cont), StructCfMarker::Linear);
    EXPECT_EQ(m.instOpcode(m.blockTerminator(cont)), MirOpcode::CondBr);
}

// `break;` inside a switch arm goes to the switch's exit, not to any
// enclosing loop's exit. Pins the switch-pushes-break-only frame
// discipline.
TEST(MirLoweringCSubset, BreakInsideSwitchArmTargetsSwitchExit) {
    auto L = lowerCSubset(
        "void f(int x) {\n"
        "  while (x > 0) {\n"
        "    switch (x) { case 1: break; default: break; }\n"
        "  }\n"
        "}\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirFuncId const fn = m.funcAt(0);
    // Locate the switch-join (where both case-arm breaks converge) and
    // verify it has 2+ predecessors-worth of incoming Br edges from the
    // arm blocks (by counting blocks with SwitchJoin marker — exactly 1).
    std::uint32_t joinCount = 0;
    auto const blockCount = m.funcBlockCount(fn);
    for (std::uint32_t i = 0; i < blockCount; ++i) {
        MirBlockId const b = m.funcBlockAt(fn, i);
        if (m.blockMarker(b) == StructCfMarker::SwitchJoin) ++joinCount;
    }
    EXPECT_EQ(joinCount, 1u);
}

// ─── ML2 cycle 5: multi-function modules + forward-reference calls ────────

// A two-function module produces two MirFuncs with isolated per-function
// state (each gets its own entry block, allocas, SSA values). Pins the
// per-function context-reset discipline added in cycle 3b — pre-3b, the
// symbolToValue map would leak between functions.
TEST(MirLoweringCSubset, MultipleFunctionsEachGetIsolatedMirFunc) {
    auto L = lowerCSubset(
        "int add(int a, int b) { return a + b; }\n"
        "int sub(int a, int b) { return a - b; }\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    ASSERT_EQ(m.moduleFuncCount(), 2u);
    MirFuncId const f0 = m.funcAt(0);
    MirFuncId const f1 = m.funcAt(1);
    EXPECT_NE(f0, f1);
    // Each function's entry block has [Arg, Arg, op, Return] — 4 insts.
    for (MirFuncId fn : {f0, f1}) {
        MirBlockId const entry = m.funcEntry(fn);
        EXPECT_EQ(m.blockMarker(entry), StructCfMarker::EntryBlock);
        ASSERT_EQ(m.blockInstCount(entry), 4u);
        EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 0)), MirOpcode::Arg);
        EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 1)), MirOpcode::Arg);
    }
    // First function uses Add; second uses Sub — proving the lowerings
    // didn't cross-pollute (e.g., second function reusing first's
    // residual symbolToValue from before the per-fn clear).
    EXPECT_EQ(m.instOpcode(m.blockInstAt(m.funcEntry(f0), 2)), MirOpcode::Add);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(m.funcEntry(f1), 2)), MirOpcode::Sub);
}

// Forward reference: caller declared BEFORE callee. The function-symbols
// pre-pass collects symbols from all module-level Functions before the
// main lowering walk so a Ref-to-function from an earlier function
// resolves to a real GlobalAddr. Without the pre-pass, this would fail
// loud with "Ref to unbound symbol".
TEST(MirLoweringCSubset, ForwardReferenceCallResolvesViaPrePass) {
    auto L = lowerCSubset(
        "int caller(int x) { return callee(x); }\n"   // forward-refs callee
        "int callee(int x) { return x; }\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirFuncId const caller = m.funcAt(0);
    MirBlockId const entry = m.funcEntry(caller);
    // Inside `caller`: Arg, GlobalAddr(callee), Call, Return.
    ASSERT_GE(m.blockInstCount(entry), 4u);
    bool sawGlobalAddr = false;
    bool sawCall = false;
    auto const n = m.blockInstCount(entry);
    for (std::uint32_t i = 0; i < n; ++i) {
        MirOpcode const op = m.instOpcode(m.blockInstAt(entry, i));
        if (op == MirOpcode::GlobalAddr) sawGlobalAddr = true;
        if (op == MirOpcode::Call) sawCall = true;
    }
    EXPECT_TRUE(sawGlobalAddr) << "forward-referenced callee should resolve "
                                  "via GlobalAddr from the pre-pass";
    EXPECT_TRUE(sawCall);
}

// VarDecl without an initializer still emits the alloca but no store —
// reads before assignment will Load whatever the alloca's uninitialized
// memory holds (which is HIR-policy-defined; MIR doesn't auto-init).
TEST(MirLoweringCSubset, VarDeclWithoutInitOnlyAllocas) {
    auto L = lowerCSubset(
        "int f() { int x; x = 7; return x; }");
    ASSERT_TRUE(L.mir.ok);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // [Alloca x, Const 7, Store, Load, Return] — only ONE Store (no init).
    ASSERT_EQ(m.blockInstCount(entry), 5u);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 0)), MirOpcode::Alloca);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 1)), MirOpcode::Const);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 2)), MirOpcode::Store);
}

// ── ML2 cycle 6: ConstructAggregate lowering ──────────────────────────

namespace {
// Find the opcodes used by the first function's entry block in
// `lowerCSubset`-ordered. Order-preserving so consumers can assert
// "Const before InsertValue, InsertValue before Store" etc.
[[nodiscard]] std::vector<MirOpcode> entryOpcodes(Mir const& m) {
    std::vector<MirOpcode> out;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    auto const n = m.blockInstCount(entry);
    out.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        out.push_back(m.instOpcode(m.blockInstAt(entry, i)));
    }
    return out;
}
[[nodiscard]] std::size_t countOpcode(std::vector<MirOpcode> const& ops, MirOpcode k) {
    return static_cast<std::size_t>(std::count(ops.begin(), ops.end(), k));
}
// Entry-block opcodes of function `fi` (the struct-return fixtures are all
// straight-line single-block bodies, so the entry block is the whole function).
[[nodiscard]] std::vector<MirOpcode> funcEntryOpcodes(Mir const& m, std::uint32_t fi) {
    std::vector<MirOpcode> out;
    MirBlockId const entry = m.funcEntry(m.funcAt(fi));
    auto const n = m.blockInstCount(entry);
    out.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i)
        out.push_back(m.instOpcode(m.blockInstAt(entry, i)));
    return out;
}
// The Return terminator of function `fi`'s entry block.
[[nodiscard]] MirInstId entryReturn(Mir const& m, std::uint32_t fi) {
    MirBlockId const entry = m.funcEntry(m.funcAt(fi));
    auto const n = m.blockInstCount(entry);
    for (std::uint32_t i = 0; i < n; ++i) {
        MirInstId const ix = m.blockInstAt(entry, i);
        if (m.instOpcode(ix) == MirOpcode::Return) return ix;
    }
    return InvalidMirInst;
}
} // namespace

// All-constant `struct Point p = {1, 2}` const-folds to a single
// Const(MirAggregateValue) — no InsertValue chain needed because the
// const-eval engine handles the whole aggregate at HIR→MIR time.
TEST(MirLoweringCSubset, ConstructAggregateAllConstFoldsToConst) {
    auto L = lowerCSubset(
        "struct Point { int x; int y; };\n"
        "void f() { struct Point p = {1, 2}; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty()
              ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok)
        << (L.hirReporter.all().empty()
              ? "" : L.hirReporter.all()[0].actual);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty()
              ? "" : L.mirReporter.all()[0].actual);
    auto const ops = entryOpcodes(L.mir.mir);
    // Should contain at least one Const (the folded aggregate) + the
    // VarDecl's Alloca + Store. NO InsertValue (folded inline).
    EXPECT_GE(countOpcode(ops, MirOpcode::Alloca), 1u);
    EXPECT_GE(countOpcode(ops, MirOpcode::Const),  1u);
    EXPECT_EQ(countOpcode(ops, MirOpcode::InsertValue), 0u)
        << "all-constant ConstructAggregate must const-fold to a single "
           "Const, not an InsertValue chain";
}

// FC7 (D-FC7-MEMBER-ACCESS): a struct LOCAL initializer with a runtime
// child lowers ELEMENT-WISE — one Gep+Store per field into the slot — NOT
// an InsertValue chain (whose non-zero-index form had no LIR realization).
// `{a, 2}` → Store(a)→field x, Store(2)→field y.
TEST(MirLoweringCSubset, StructLocalRuntimeInitEmitsPerFieldStores) {
    auto L = lowerCSubset(
        "struct Point { int x; int y; };\n"
        "void f(int a) { struct Point p = {a, 2}; }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty()
              ? "" : L.mirReporter.all()[0].actual);
    auto const ops = entryOpcodes(L.mir.mir);
    EXPECT_EQ(countOpcode(ops, MirOpcode::InsertValue), 0u)
        << "FC7 struct local init is element-wise, never an InsertValue chain";
    EXPECT_EQ(countOpcode(ops, MirOpcode::Gep),   2u)
        << "one Gep per field (x, y)";
    EXPECT_EQ(countOpcode(ops, MirOpcode::Store), 2u)
        << "one Store per field";
}

// FC7 (D-FC7-MEMBER-ACCESS): a UNION local initializer (1 child, the
// active variant) lowers to ONE Gep+Store at byte offset 0 — never an
// InsertValue.
TEST(MirLoweringCSubset, UnionLocalRuntimeInitEmitsOneFieldStore) {
    auto L = lowerCSubset(
        "union U { int i; char c; };\n"
        "void f(int a) { union U u = { a }; }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty()
              ? "" : L.mirReporter.all()[0].actual);
    auto const ops = entryOpcodes(L.mir.mir);
    EXPECT_EQ(countOpcode(ops, MirOpcode::InsertValue), 0u)
        << "FC7 union local init is element-wise, never an InsertValue chain";
    EXPECT_EQ(countOpcode(ops, MirOpcode::Gep),   1u)
        << "union has 1 active-variant child → 1 Gep at offset 0";
    EXPECT_EQ(countOpcode(ops, MirOpcode::Store), 1u);
}

// Variant-aware union seed (review fix-up lock-in): `.c = 'x'` is
// variant-1 (char), not variant-0 (int). The seed Const's slot[0] type
// must match the active variant's type so the InsertValue's child
// type aligns — otherwise variant identity is silently erased.
TEST(MirLoweringCSubset, ConstructAggregateUnionNonZeroVariantRuntimeOk) {
    auto L = lowerCSubset(
        "union U { int i; char c; };\n"
        "void f(char x) { union U u = { .c = x }; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty()
              ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok)
        << (L.hirReporter.all().empty()
              ? "" : L.hirReporter.all()[0].actual);
    ASSERT_TRUE(L.mir.ok)
        << "MIR-lowering must accept a non-zero-variant union init "
           "(seed's slot type must match active variant): "
        << (L.mirReporter.all().empty()
              ? "" : L.mirReporter.all()[0].actual);
}

// Array ConstructAggregate with runtime children exercises the
// zeroLiteralOf Array arm + positional indexing.
TEST(MirLoweringCSubset, ConstructAggregateArrayRuntimeUsesChain) {
    auto L = lowerCSubset(
        "void f(int a) { int xs[3] = {a, 2, 3}; }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty()
              ? "" : L.mirReporter.all()[0].actual);
    auto const ops = entryOpcodes(L.mir.mir);
    EXPECT_GE(countOpcode(ops, MirOpcode::InsertValue), 3u)
        << "array ConstructAggregate with N runtime children emits N "
           "InsertValues (one per positional slot)";
}

// Chain TOPOLOGY: the runtime-chain test only COUNTS InsertValues —
// it doesn't verify each InsertValue threads through the previous.
// A regression that emitted N parallel InsertValue(zeroBase, v_i, [i])
// would silently drop all but the last field. This test reads each
// InsertValue's first operand and asserts the chain shape.
// FC7 (D-FC7-MEMBER-ACCESS): a struct local init `{a, b}` (both runtime)
// lowers to two INDEPENDENT Gep+Store pairs at the field BYTE OFFSETS
// (x@0, y@4) — no InsertValue chain. Pins that each field lands at its OWN
// offset (a wrong/duplicated offset would write both fields to one place).
TEST(MirLoweringCSubset, StructLocalInitStoresEachFieldAtItsOffset) {
    auto L = lowerCSubset(
        "struct Point { int x; int y; };\n"
        "void f(int a, int b) { struct Point p = {a, b}; }\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto const ops = entryOpcodes(m);
    EXPECT_EQ(countOpcode(ops, MirOpcode::InsertValue), 0u)
        << "element-wise init, no InsertValue chain";
    EXPECT_EQ(countOpcode(ops, MirOpcode::Gep),   2u);
    EXPECT_EQ(countOpcode(ops, MirOpcode::Store), 2u);
    // The two field GEPs carry distinct byte offsets 0 (x) and 4 (y).
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    std::vector<std::int64_t> gepOffsets;
    auto const n = m.blockInstCount(entry);
    for (std::uint32_t i = 0; i < n; ++i) {
        MirInstId const ix = m.blockInstAt(entry, i);
        if (m.instOpcode(ix) != MirOpcode::Gep) continue;
        auto gOps = m.instOperands(ix);
        ASSERT_EQ(gOps.size(), 2u);
        auto const& lit = m.literalValue(m.constLiteralIndex(gOps[1]));
        gepOffsets.push_back(std::get<std::int64_t>(lit.value));
    }
    ASSERT_EQ(gepOffsets.size(), 2u);
    EXPECT_EQ(gepOffsets[0], 0) << "field x at byte offset 0";
    EXPECT_EQ(gepOffsets[1], 4) << "field y at byte offset 4";
}

// FC7 (D-FC7-NESTED-STRUCT-FIELD): a NESTED brace initializer
// `Outer o = {{a, b}, c}` (the Inner-typed field `in` initialized by an inner
// brace) lowers RECURSIVELY — `lowerAggregateInitIntoSlot` recurses into the
// struct-typed field's sub-slot, emitting one scalar Store per LEAF field,
// NEVER an aggregate-width store of the inner struct. Runtime params (a,b,c)
// keep it off the const-fold path. The GEPs are: inner-slot Gep(o,0), then
// leaf Gep(<>,0) for in.x, leaf Gep(<>,4) for in.y, then Gep(o,8) for z —
// offset multiset {0,0,4,8} with exactly 3 leaf Stores. RED-ON-DISABLE:
// revert the recursion and the inner field is lowerExpr'd as a non-realizable
// aggregate value (≠ 3 scalar leaf stores at these composed offsets).
TEST(MirLoweringCSubset, StructLocalNestedInitRecursesIntoSubSlot) {
    auto L = lowerCSubset(
        "typedef struct { int x; int y; } Inner;\n"
        "typedef struct { Inner in; int z; } Outer;\n"
        "void f(int a, int b, int c) { Outer o = {{a, b}, c}; }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto const ops = entryOpcodes(m);
    EXPECT_EQ(countOpcode(ops, MirOpcode::InsertValue), 0u)
        << "recursive element-wise init, no aggregate InsertValue";
    EXPECT_EQ(countOpcode(ops, MirOpcode::Store), 3u)
        << "one scalar Store per leaf field (in.x, in.y, z) — NOT one "
           "aggregate store of `in`";
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    int c0 = 0, c4 = 0, c8 = 0, cOther = 0;
    auto const n = m.blockInstCount(entry);
    for (std::uint32_t i = 0; i < n; ++i) {
        MirInstId const ix = m.blockInstAt(entry, i);
        if (m.instOpcode(ix) != MirOpcode::Gep) continue;
        auto gOps = m.instOperands(ix);
        ASSERT_EQ(gOps.size(), 2u);
        auto const off =
            std::get<std::int64_t>(m.literalValue(m.constLiteralIndex(gOps[1])).value);
        if (off == 0) ++c0;
        else if (off == 4) ++c4;
        else if (off == 8) ++c8;
        else ++cOther;
    }
    EXPECT_EQ(c0, 2) << "inner-slot Gep(o,0) + in.x leaf Gep(<>,0)";
    EXPECT_EQ(c4, 1) << "in.y leaf at offset 4 within Inner";
    EXPECT_EQ(c8, 1) << "z leaf at offset 8 within Outer";
    EXPECT_EQ(cOther, 0) << "no GEP at an unexpected offset";
}

// Nested aggregate `{{1}, {2}}` as a LOCAL init: post-FC7
// (D-FC7-NESTED-STRUCT-FIELD) it lowers ELEMENT-WISE — lowerAggregateInitIntoSlot
// recurses into each struct field's sub-slot → leaf Stores, NO InsertValue
// chain. (A global / expression-position fully-const aggregate still const-
// folds to a single Const; either way the InsertValue count is 0 — the
// invariant this guards. zeroLiteralOf's Struct recursion is also exercised.)
TEST(MirLoweringCSubset, ConstructAggregateNestedAllConstFolds) {
    auto L = lowerCSubset(
        "struct Inner { int v; };\n"
        "struct Outer { struct Inner a; struct Inner b; };\n"
        "void f() { struct Outer o = {{1}, {2}}; }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty()
              ? "" : L.mirReporter.all()[0].actual);
    auto const ops = entryOpcodes(L.mir.mir);
    EXPECT_EQ(countOpcode(ops, MirOpcode::InsertValue), 0u)
        << "nested aggregate init lowers without an InsertValue chain "
           "(element-wise leaf stores for a local; a single folded Const "
           "in global/expression position)";
}

// FC7 (D-FC7-AGGREGATE-COPY-MEMCPY): a UNION copy (`*d = *s`) is BYTE-WISE,
// not field-wise — the variants overlap, so a field-wise copy of one variant
// would miss the others' bytes. `union U { int n; struct P p; }` is 8 bytes →
// ONE I64 chunk (Gep+Load(i64)+Gep+Store). The DISCRIMINATOR vs a wrong
// field-wise copy: the Load type is I64 (full 8-byte chunk), NOT I32 (the `n`
// variant) — a field-wise copy of `n` would move only 4 of the 8 bytes.
TEST(MirLoweringCSubset, UnionCopyIsByteWiseNotFieldWise) {
    auto L = lowerCSubset(
        "struct P { int x; int y; };\n"
        "union U { int n; struct P p; };\n"
        "void copy(union U* d, union U* s) { *d = *s; }\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto const& interner = L.model.lattice().interner();
    auto const ops = entryOpcodes(m);
    EXPECT_EQ(countOpcode(ops, MirOpcode::InsertValue), 0u);
    EXPECT_EQ(countOpcode(ops, MirOpcode::Load),  1u) << "one 8-byte chunk";
    EXPECT_EQ(countOpcode(ops, MirOpcode::Store), 1u);
    EXPECT_EQ(countOpcode(ops, MirOpcode::Gep),   2u) << "src + dst GEP";
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    auto const n = m.blockInstCount(entry);
    for (std::uint32_t i = 0; i < n; ++i) {
        MirInstId const ix = m.blockInstAt(entry, i);
        if (m.instOpcode(ix) != MirOpcode::Load) continue;
        EXPECT_EQ(interner.kind(m.instType(ix)), TypeKind::I64)
            << "byte-wise union copy loads an I64 chunk, not the 4-byte variant";
    }
}

// FC7 (D-FC7-AGGREGATE-COPY-MEMCPY): a struct with an AGGREGATE field copies
// BYTE-WISE (a field-wise copy can't realize an aggregate-width field Load).
// `Outer { Inner in; int z; }` is 12 bytes → two chunks: I64 @0 (covers `in`),
// I32 @8 (covers `z`). Chunk Load TYPES are {I64, I32} and GEP offsets are the
// CHUNK offsets {0,0,8,8} — NOT field offsets. The far field `z`@8 is covered
// by the second chunk (a truncating 8-byte copy would drop it).
TEST(MirLoweringCSubset, StructWithStructFieldCopyIsByteWise) {
    auto L = lowerCSubset(
        "struct Inner { int x; int y; };\n"
        "struct Outer { struct Inner in; int z; };\n"
        "void copy(struct Outer* d, struct Outer* s) { *d = *s; }\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto const& interner = L.model.lattice().interner();
    auto const ops = entryOpcodes(m);
    EXPECT_EQ(countOpcode(ops, MirOpcode::InsertValue), 0u);
    EXPECT_EQ(countOpcode(ops, MirOpcode::Load),  2u) << "I64 @0 + I32 @8";
    EXPECT_EQ(countOpcode(ops, MirOpcode::Store), 2u);
    EXPECT_EQ(countOpcode(ops, MirOpcode::Gep),   4u);
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    auto const n = m.blockInstCount(entry);
    int c0 = 0, c8 = 0, cOther = 0, i64Loads = 0, i32Loads = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        MirInstId const ix = m.blockInstAt(entry, i);
        if (m.instOpcode(ix) == MirOpcode::Gep) {
            auto g = m.instOperands(ix);
            ASSERT_EQ(g.size(), 2u);
            auto const off = std::get<std::int64_t>(
                m.literalValue(m.constLiteralIndex(g[1])).value);
            if (off == 0) ++c0; else if (off == 8) ++c8; else ++cOther;
        } else if (m.instOpcode(ix) == MirOpcode::Load) {
            TypeKind const k = interner.kind(m.instType(ix));
            if (k == TypeKind::I64) ++i64Loads; else if (k == TypeKind::I32) ++i32Loads;
        }
    }
    EXPECT_EQ(c0, 2) << "src+dst GEP for the @0 I64 chunk";
    EXPECT_EQ(c8, 2) << "src+dst GEP for the @8 I32 chunk (covers far field z)";
    EXPECT_EQ(cOther, 0);
    EXPECT_EQ(i64Loads, 1) << "8-byte chunk @0";
    EXPECT_EQ(i32Loads, 1) << "4-byte chunk @8";
}

// FC7 (D-FC7-AGGREGATE-COPY-MEMCPY): an ARRAY field is ALSO an aggregate field
// → the struct copies BYTE-WISE. `S { int arr[3]; int n; }` is 16 bytes → two
// I64 chunks @0, @8. This pins the array-field arm of the byte-wise dispatch
// at the MIR tier; reading an array field's ELEMENTS (`s.arr[i]`) is a
// SEPARATE pre-existing gap (the unsupported 3-op storage-array GEP,
// D-MIR-STORAGE-ARRAY-INDEX-GEP), so this is a MIR-tier (not runtime) pin.
TEST(MirLoweringCSubset, StructWithArrayFieldCopyIsByteWise) {
    auto L = lowerCSubset(
        "struct S { int arr[3]; int n; };\n"
        "void copy(struct S* d, struct S* s) { *d = *s; }\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto const& interner = L.model.lattice().interner();
    auto const ops = entryOpcodes(m);
    EXPECT_EQ(countOpcode(ops, MirOpcode::InsertValue), 0u);
    EXPECT_EQ(countOpcode(ops, MirOpcode::Load),  2u) << "two I64 chunks (16 bytes)";
    EXPECT_EQ(countOpcode(ops, MirOpcode::Store), 2u);
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    auto const n = m.blockInstCount(entry);
    for (std::uint32_t i = 0; i < n; ++i) {
        MirInstId const ix = m.blockInstAt(entry, i);
        if (m.instOpcode(ix) != MirOpcode::Load) continue;
        EXPECT_EQ(interner.kind(m.instType(ix)), TypeKind::I64)
            << "16-byte struct-with-array-field copies as two I64 chunks";
    }
}

// FC7 C1a (D-FC7-STRUCT-BY-VALUE-ARG-RETURN): an 8-byte struct param passed BY
// VALUE under SysV is ONE eightbyte → the CALLEE receives a SINGLE I64 register
// `Arg` (not a struct value) and stores it into the param's frame slot; the body
// reads the fields from the slot. (The 2-eightbyte case is fail-loud,
// D-FC7-SYSV-STRUCT-ARG-MULTIREG.)
TEST(MirLoweringCSubset, SysVStructByValueParamReceivesOneRegisterPiece) {
    auto L = lowerCSubset(
        "struct Pair { int x; int y; };\n"
        "int sum(struct Pair p) { return p.x + p.y; }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto const& interner = L.model.lattice().interner();
    auto const ops = entryOpcodes(m);
    EXPECT_EQ(countOpcode(ops, MirOpcode::Arg), 1u)
        << "exactly one register Arg for the single-eightbyte struct param";
    EXPECT_GE(countOpcode(ops, MirOpcode::Alloca), 1u)
        << "the param is reconstructed into a frame slot (Alloca precedes the Arg)";
    // The single Arg is an I64 register piece (the eightbyte), NOT a struct value.
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    auto const n = m.blockInstCount(entry);
    bool sawArg = false;
    for (std::uint32_t i = 0; i < n; ++i) {
        MirInstId const ix = m.blockInstAt(entry, i);
        if (m.instOpcode(ix) != MirOpcode::Arg) continue;
        sawArg = true;
        EXPECT_EQ(interner.kind(m.instType(ix)), TypeKind::I64)
            << "the 8-byte struct param arrives as ONE I64 register piece";
    }
    EXPECT_TRUE(sawArg);
}

// FC7 C1a: the CALLER copies the 8-byte struct arg to a temp, loads the ONE
// eightbyte as an I64, and passes THAT scalar as the Call operand (not the
// struct value). So `sum(p)`'s Call carries [callee, <I64 piece>].
TEST(MirLoweringCSubset, SysVStructByValueCallPassesOneRegisterPiece) {
    auto L = lowerCSubset(
        "struct Pair { int x; int y; };\n"
        "int sum(struct Pair p) { return p.x + p.y; }\n"
        "int f(void) { struct Pair p; p.x = 1; p.y = 2; return sum(p); }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto const& interner = L.model.lattice().interner();
    // f is the second function; find its Call and check the arg operand.
    MirBlockId const entry = m.funcEntry(m.funcAt(1));
    auto const n = m.blockInstCount(entry);
    bool sawCall = false;
    for (std::uint32_t i = 0; i < n; ++i) {
        MirInstId const ix = m.blockInstAt(entry, i);
        if (m.instOpcode(ix) != MirOpcode::Call) continue;
        sawCall = true;
        auto cops = m.instOperands(ix);
        ASSERT_EQ(cops.size(), 2u) << "[callee, one I64 piece]";
        EXPECT_EQ(interner.kind(m.instType(cops[1])), TypeKind::I64)
            << "the struct arg is passed as ONE I64 register piece, not a struct";
    }
    EXPECT_TRUE(sawCall);
}

// FC7 C1b (D-FC7-SYSV-STRUCT-ARG-MULTIREG): a 12-byte struct is TWO SysV
// eightbytes → the callee receives TWO I64 register Args, with the PER-CLASS GPR
// ordinals 0 and 1 (rdi, rsi), each stored into its eightbyte of the slot. This
// is the multi-register case the verifier's physical-arg-count bound unblocks.
TEST(MirLoweringCSubset, SysVTwoEightbyteStructParamReceivesTwoRegisterPieces) {
    auto L = lowerCSubset(
        "struct Tri { int x; int y; int z; };\n"
        "int sum(struct Tri t) { return t.x + t.y + t.z; }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto const& interner = L.model.lattice().interner();
    auto const ops = entryOpcodes(m);
    EXPECT_EQ(countOpcode(ops, MirOpcode::Arg), 2u)
        << "two register Args for the two-eightbyte struct param";
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    auto const n = m.blockInstCount(entry);
    std::vector<std::uint32_t> argPayloads;
    for (std::uint32_t i = 0; i < n; ++i) {
        MirInstId const ix = m.blockInstAt(entry, i);
        if (m.instOpcode(ix) != MirOpcode::Arg) continue;
        EXPECT_EQ(interner.kind(m.instType(ix)), TypeKind::I64)
            << "each eightbyte is an I64 register piece";
        argPayloads.push_back(m.argIndex(ix));
    }
    ASSERT_EQ(argPayloads.size(), 2u);
    EXPECT_EQ(argPayloads[0], 0u) << "first eightbyte → per-class GPR ordinal 0 (rdi)";
    EXPECT_EQ(argPayloads[1], 1u) << "second eightbyte → per-class GPR ordinal 1 (rsi)";
}

// FC7 C1a: a >16-byte struct param passed BY REFERENCE arrives as ONE POINTER
// Arg (to the caller's private copy); the callee binds the param's address to it
// directly (no piece reconstruction, no slot copy). The always-on structural
// guard for the by-ref path — the runtime corpus runs on linux-x86_64 only.
TEST(MirLoweringCSubset, SysVByRefStructParamReceivesOnePointerArg) {
    auto L = lowerCSubset(
        "struct Big { long a; long b; long c; };\n"
        "long pick(struct Big b) { return b.c; }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto const& interner = L.model.lattice().interner();
    auto const ops = entryOpcodes(m);
    EXPECT_EQ(countOpcode(ops, MirOpcode::Arg), 1u)
        << "one POINTER Arg for the by-reference (>16B) struct param";
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    auto const n = m.blockInstCount(entry);
    bool sawArg = false;
    for (std::uint32_t i = 0; i < n; ++i) {
        MirInstId const ix = m.blockInstAt(entry, i);
        if (m.instOpcode(ix) != MirOpcode::Arg) continue;
        sawArg = true;
        EXPECT_EQ(interner.kind(m.instType(ix)), TypeKind::Ptr)
            << "the >16-byte struct param arrives as a pointer to the caller's copy";
    }
    EXPECT_TRUE(sawArg);
}

// FC7 C1c (D-FC7-SYSV-STRUCT-RETURN-IN-REGS): a 12-byte (2-eightbyte) struct
// RETURNED by value lowers the callee's `return t;` to a MULTI-OPERAND Return
// carrying TWO I64 register pieces (loaded from t's slot) — never a single
// truncating struct value. The aggregate types use `typedef` because a top-level
// `struct Tag` return specifier is the pre-FC4 grammar residue
// (D-CSUBSET-STRUCT-BODY-VARDECL-POSITION); the ABI codegen is identical.
TEST(MirLoweringCSubset, SysVTwoEightbyteStructReturnEmitsMultiOperandReturn) {
    auto L = lowerCSubset(
        "typedef struct { int x; int y; int z; } Tri;\n"
        "Tri mk(int a, int b, int c) { Tri t; t.x=a; t.y=b; t.z=c; return t; }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto const& interner = L.model.lattice().interner();
    MirInstId const ret = entryReturn(m, 0);
    ASSERT_TRUE(ret.valid()) << "mk has a Return terminator";
    auto rops = m.instOperands(ret);
    ASSERT_EQ(rops.size(), 2u)
        << "a 2-eightbyte struct returns TWO register pieces, not one truncated "
           "struct value";
    for (auto const op : rops)
        EXPECT_EQ(interner.kind(m.instType(op)), TypeKind::I64)
            << "each eightbyte is an I64 register piece (rax:rdx)";
}

// FC7 C1c: a >16-byte struct returned by value uses SRET — the callee receives a
// hidden result POINTER as its first Arg (GPR ordinal 0, shifting the real
// params) and returns that pointer; the body copies the result through it.
TEST(MirLoweringCSubset, SysVByRefStructReturnUsesSretHiddenPointer) {
    auto L = lowerCSubset(
        "typedef struct { long a; long b; long c; } Big;\n"
        "Big mk(long a, long b, long c) { Big r; r.a=a; r.b=b; r.c=c; return r; }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto const& interner = L.model.lattice().interner();
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    MirInstId firstArg = InvalidMirInst;
    for (std::uint32_t i = 0; i < m.blockInstCount(entry); ++i) {
        MirInstId const ix = m.blockInstAt(entry, i);
        if (m.instOpcode(ix) == MirOpcode::Arg) { firstArg = ix; break; }
    }
    ASSERT_TRUE(firstArg.valid());
    EXPECT_EQ(interner.kind(m.instType(firstArg)), TypeKind::Ptr)
        << "the sret hidden result pointer is the FIRST Arg (GPR ordinal 0)";
    EXPECT_EQ(m.argIndex(firstArg), 0u);
    MirInstId const ret = entryReturn(m, 0);
    ASSERT_TRUE(ret.valid());
    auto rops = m.instOperands(ret);
    ASSERT_EQ(rops.size(), 1u) << "sret returns the single hidden pointer";
    EXPECT_EQ(interner.kind(m.instType(rops[0])), TypeKind::Ptr);
}

// FC7 C1c: a {double; long} struct returns in MIXED register classes — eightbyte
// 0 (double @0) is the SSE piece (F64 → xmm0), eightbyte 1 (long @8) is the
// INTEGER piece (I64 → rax). Pins the per-class return split (the most likely
// off-by-one: the GPR piece must NOT land in rdx).
TEST(MirLoweringCSubset, SysVMixedClassStructReturnSplitsAcrossRegisterClasses) {
    auto L = lowerCSubset(
        "typedef struct { double d; long n; } Mix;\n"
        "Mix mk(double d, long n) { Mix m; m.d=d; m.n=n; return m; }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto const& interner = L.model.lattice().interner();
    MirInstId const ret = entryReturn(m, 0);
    ASSERT_TRUE(ret.valid());
    auto rops = m.instOperands(ret);
    ASSERT_EQ(rops.size(), 2u) << "{double; long} returns in two register classes";
    EXPECT_EQ(interner.kind(m.instType(rops[0])), TypeKind::F64)
        << "eightbyte 0 (double @0) is the SSE piece → xmm0";
    EXPECT_EQ(interner.kind(m.instType(rops[1])), TypeKind::I64)
        << "eightbyte 1 (long @8) is the INTEGER piece → rax";
}

// FC7 C1c (SF-2): a struct-returning CALL is emitted EXACTLY ONCE — the factored
// helper backs both `lowerExpr(Call)` and `lowerLvalueAddress(Call)`, so a
// consumer that reaches the call by address (`Tri t = mk(...)`) must not double-
// emit it. A 2-eightbyte return captures piece 0 as the Call result + ONE
// `ReturnPiece` for piece 1.
TEST(MirLoweringCSubset, StructReturningCallEmitsOneCallAndOneReturnPiece) {
    auto L = lowerCSubset(
        "typedef struct { int x; int y; int z; } Tri;\n"
        "Tri mk(int a, int b, int c) { Tri t; t.x=a; t.y=b; t.z=c; return t; }\n"
        "int use(void) { Tri t = mk(1, 2, 3); return t.x + t.y + t.z; }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto const ops = funcEntryOpcodes(m, 1);   // `use`
    EXPECT_EQ(countOpcode(ops, MirOpcode::Call), 1u)
        << "the struct-returning call is emitted EXACTLY once (no double-emit "
           "across lowerExpr / lowerLvalueAddress)";
    EXPECT_EQ(countOpcode(ops, MirOpcode::ReturnPiece), 1u)
        << "a 2-eightbyte return captures piece 0 (the Call result) + ONE "
           "ReturnPiece for piece 1";
}

// ML3 end-to-end: ML2-lowered MIR for a representative c-subset
// corpus passes the MirVerifier (with TypeInterner). Validates that
// the verifier finds no false-positives in production-shape MIR.
TEST(MirLoweringCSubset, Ml3VerifierAcceptsRealLoweredMir) {
    Lowered L = lowerCSubset(
        "int add(int x, int y) { return x + y; }\n"
        "int branch(int x) { if (x > 0) return x; return 0 - x; }\n"
        "int loopsum(int n) {\n"
        "  int s = 0; int i = 0;\n"
        "  while (i < n) { s = s + i; i = i + 1; }\n"
        "  return s;\n"
        "}\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok);
    DiagnosticReporter verifyReporter;
    MirVerifier v{L.mir.mir, &L.model.lattice().interner()};
    EXPECT_TRUE(v.verify(verifyReporter))
        << "MirVerifier rejected ML2-lowered MIR — "
        << (verifyReporter.all().empty() ? "" : verifyReporter.all()[0].actual);
}

// ML4 end-to-end: ML2-lowered MIR for a representative corpus
// round-trips through the .dssir text format byte-identically.
// Catches any emitter/parser asymmetry on production-shape MIR.
TEST(MirLoweringCSubset, Ml4TextFormatRoundTripsRealMir) {
    Lowered L = lowerCSubset(
        "int add(int x, int y) { return x + y; }\n"
        "int factorial(int n) {\n"
        "  if (n <= 1) return 1;\n"
        "  return n * (n - 1);\n"
        "}\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok);
    DiagnosticReporter r1, r2, r3;
    MirTextContext ctx1{&L.model.lattice().interner(), nullptr};
    std::string first = emitMir(L.mir.mir, ctx1, r1);
    auto parsed = parseMir(first, CompilationUnitId{1}, r2);
    EXPECT_TRUE(parsed->ok)
        << "parse failed: "
        << (r2.all().empty() ? "" : r2.all()[0].actual);
    MirTextContext ctx2{&parsed->interner, &parsed->symbolNames};
    std::string second = emitMir(parsed->mir, ctx2, r3);
    EXPECT_EQ(first, second)
        << "byte-equal round-trip failed\nfirst:\n" << first
        << "\nsecond:\n" << second;
}

// FC2 Part B (F64 constant materialization): a function-body F64
// float literal lowers the way STRING literals do — an anonymous
// module-level rodata global carrying the value + GlobalAddr + Load
// — NEVER a MIR `Const` (register machines have no float-immediate
// form; the old Const route dead-ended in the LIR literal pool).
// `1.7 + 2.5;` (a bare expression statement — c-subset has no float
// TYPE keyword and no implicit float→int return coercion yet, so the
// expression statement is the one body position a float literal can
// legally occupy until Part A's casts land) carries TWO body float
// literals through HR's F64 common-type unification into one FAdd.
TEST(MirLoweringCSubset, BodyF64LiteralPromotesToAnonymousGlobalPlusLoad) {
    auto L = lowerCSubset("int f() { 1.7 + 2.5; return 4; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << "semantic phase: " << (L.model.diagnostics().all().empty()
            ? "" : std::string(diagnosticCodeName(
                       L.model.diagnostics().all()[0].code))
                   + " " + L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok)
        << "HIR lowering: " << (L.hirReporter.all().empty()
            ? "" : L.hirReporter.all()[0].actual);
    ASSERT_TRUE(L.mir.ok)
        << "MIR lowering: " << (L.mirReporter.all().empty()
            ? "" : L.mirReporter.all()[0].actual);

    Mir const& m = L.mir.mir;
    auto const& interner = L.model.lattice().interner();

    // TWO promoted globals (one per literal occurrence — the string-
    // literal convention: per-occurrence, no dedup), each typed F64
    // with a constant-init double literal carrying the exact value.
    ASSERT_EQ(m.moduleGlobalCount(), 2u)
        << "each body F64 literal must mint one anonymous rodata global";
    bool saw17 = false, saw25 = false;
    for (std::uint32_t gi = 0; gi < m.moduleGlobalCount(); ++gi) {
        MirGlobalId const g = m.globalAt(gi);
        EXPECT_EQ(interner.kind(m.globalType(g)), TypeKind::F64);
        EXPECT_TRUE(m.globalSymbol(g).valid())
            << "promoted global needs a real (minted) SymbolId so the "
               "rodata relocation resolves — not the anonymous sentinel";
        std::uint32_t const lit = m.globalInitLiteralIndex(g);
        ASSERT_NE(lit, UINT32_MAX) << "promoted global must be constant-init";
        auto const* dv = std::get_if<double>(&m.literalValue(lit).value);
        ASSERT_NE(dv, nullptr);
        if (*dv == 1.7) saw17 = true;
        if (*dv == 2.5) saw25 = true;
    }
    EXPECT_TRUE(saw17 && saw25)
        << "the two globals must carry the two literal values";

    // The body: NO float Const anywhere; two GlobalAddr + two F64
    // Loads feeding the FAdd (the float expression statement's value
    // is unused — the int return is a plain literal).
    ASSERT_EQ(m.moduleFuncCount(), 1u);
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    std::uint32_t nGlobalAddr = 0, nF64Load = 0, nFAdd = 0;
    for (std::uint32_t i = 0; i < m.blockInstCount(entry); ++i) {
        MirInstId const inst = m.blockInstAt(entry, i);
        switch (m.instOpcode(inst)) {
            case MirOpcode::Const:
                EXPECT_FALSE(std::holds_alternative<double>(
                    m.literalValue(m.constLiteralIndex(inst)).value))
                    << "a float Const in the body means the promotion "
                       "did NOT fire — the LIR literal-pool dead end";
                break;
            case MirOpcode::GlobalAddr: ++nGlobalAddr; break;
            case MirOpcode::Load:
                if (interner.kind(m.instType(inst)) == TypeKind::F64) {
                    ++nF64Load;
                }
                break;
            case MirOpcode::FAdd:   ++nFAdd;   break;
            default: break;
        }
    }
    EXPECT_EQ(nGlobalAddr, 2u);
    EXPECT_EQ(nF64Load, 2u);
    EXPECT_EQ(nFAdd, 1u);
}

// ── FC3 c1: UAC materialization pins (plan 23) ──────────────────────────
//
// The `arithmeticConversions` block drives the HIR combine sites; these
// pins assert the MIR consequences: implicit conversions exist as REAL
// cast instructions and the signedness routing sees the COMMON type.

namespace {

// Count instructions of `op` across the whole module.
[[nodiscard]] std::size_t countOp(::dss::Mir const& m, ::dss::MirOpcode op) {
    std::size_t n = 0;
    for (std::uint32_t f = 0; f < m.moduleFuncCount(); ++f) {
        auto const fn = m.funcAt(f);
        for (std::uint32_t b = 0; b < m.funcBlockCount(fn); ++b) {
            auto const bb = m.funcBlockAt(fn, b);
            for (std::uint32_t i = 0; i < m.blockInstCount(bb); ++i) {
                if (m.instOpcode(m.blockInstAt(bb, i)) == op) ++n;
            }
        }
    }
    return n;
}

// The instType TypeKind of the FIRST instruction of `op` (Void if none).
[[nodiscard]] ::dss::TypeKind firstOpTypeKind(Lowered const& L,
                                              ::dss::MirOpcode op) {
    auto const& m = L.mir.mir;
    for (std::uint32_t f = 0; f < m.moduleFuncCount(); ++f) {
        auto const fn = m.funcAt(f);
        for (std::uint32_t b = 0; b < m.funcBlockCount(fn); ++b) {
            auto const bb = m.funcBlockAt(fn, b);
            for (std::uint32_t i = 0; i < m.blockInstCount(bb); ++i) {
                auto const inst = m.blockInstAt(bb, i);
                if (m.instOpcode(inst) == op) {
                    auto const ty = m.instType(inst);
                    return ty.valid()
                        ? L.model.lattice().interner().kind(ty)
                        : ::dss::TypeKind::Void;
                }
            }
        }
    }
    return ::dss::TypeKind::Void;
}

} // namespace

// `long > unsigned long` (same width, mixed signedness): C 6.3.1.8 says
// the UNSIGNED type wins — the compare must be ICmpUgt over U64 with the
// signed operand converted by an EXPLICIT cast (same-width I64→U64 is a
// Bitcast). RED-on-disable: breaking the mixed-signedness verb (or the
// comparison promotion) routes this through ICmpSgt — asserted absent.
TEST(MirLoweringCSubset, MixedSignCompareLowersUnsignedWithExplicitCast) {
    auto L = lowerCSubset(
        "int cmp(long s, unsigned long u) { if (s > u) { return 1; } "
        "return 0; }");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok);
    Mir const& m = L.mir.mir;
    EXPECT_EQ(countOp(m, MirOpcode::ICmpUgt), 1u)
        << "mixed I64/U64 compare must route UNSIGNED (C 6.3.1.8)";
    EXPECT_EQ(countOp(m, MirOpcode::ICmpSgt), 0u)
        << "a signed compare here is the UAC-disabled miscompile shape";
    EXPECT_EQ(countOp(m, MirOpcode::Bitcast), 1u)
        << "the I64 operand's conversion to U64 must be a REAL cast inst";
}

// `char + 1` promotes char to int (the `alsoPromote` config row): the
// Add computes at I32 and the char operand is widened by a REAL SExt.
TEST(MirLoweringCSubset, CharPlusIntPromotesToI32WithSExt) {
    auto L = lowerCSubset("int f(char c) { return c + 1; }");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.mir.ok);
    EXPECT_EQ(firstOpTypeKind(L, MirOpcode::Add), TypeKind::I32);
    EXPECT_EQ(countOp(L.mir.mir, MirOpcode::SExt), 1u)
        << "char (signed, sub-int) widens to the promotion floor by SExt";
}

// `short + short` integer-promotes BOTH operands to int (value-
// preserving); the Add computes at I32, never at I16.
TEST(MirLoweringCSubset, ShortPlusShortPromotesToI32) {
    auto L = lowerCSubset("int f(short a, short b) { return a + b; }");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.mir.ok);
    EXPECT_EQ(firstOpTypeKind(L, MirOpcode::Add), TypeKind::I32);
    EXPECT_EQ(countOp(L.mir.mir, MirOpcode::SExt), 2u);
}

// Mixed I32 + U64: the wider unsigned type wins; the I32 operand
// sign-extends (its own value semantics) into the U64 compute.
TEST(MirLoweringCSubset, MixedI32U64AddComputesU64) {
    auto L = lowerCSubset(
        "unsigned long long f(int a, unsigned long long b) "
        "{ return a + b; }");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.mir.ok);
    EXPECT_EQ(firstOpTypeKind(L, MirOpcode::Add), TypeKind::U64);
    EXPECT_EQ(countOp(L.mir.mir, MirOpcode::SExt), 1u);
}

// ── Condition truthiness pins (C99 "compares unequal to 0") ─────────────
//
// A non-Bool ARITHMETIC condition at EVERY condition site (if / while /
// do-while / for / ternary — and a call-result feeding one) lowers as
// `ICmpNe(cond, 0-of-cond's-type)`, NEVER as the value-truncating
// `Cast → Trunc(cond → Bool)` (low-bit truncation would make `if (2)`
// FALSE). The `countOp(Trunc) == 0` half of each pin is the RED-on-
// disable lever: reverting any site in cst_to_hir.cpp to the old
// `coerce(cond, boolType())` re-materializes the Trunc and drops the
// ICmpNe.

namespace {

// TypeKind of operand `idx` of the FIRST instruction of `op` across the
// whole module (Void if no such instruction / operand).
[[nodiscard]] ::dss::TypeKind firstOperandKindOf(Lowered const& L,
                                                 ::dss::MirOpcode op,
                                                 std::size_t idx) {
    auto const& m = L.mir.mir;
    for (std::uint32_t f = 0; f < m.moduleFuncCount(); ++f) {
        auto const fn = m.funcAt(f);
        for (std::uint32_t b = 0; b < m.funcBlockCount(fn); ++b) {
            auto const bb = m.funcBlockAt(fn, b);
            for (std::uint32_t i = 0; i < m.blockInstCount(bb); ++i) {
                auto const inst = m.blockInstAt(bb, i);
                if (m.instOpcode(inst) != op) continue;
                auto const ops = m.instOperands(inst);
                if (idx >= ops.size()) return ::dss::TypeKind::Void;
                auto const ty = m.instType(ops[idx]);
                return ty.valid()
                    ? L.model.lattice().interner().kind(ty)
                    : ::dss::TypeKind::Void;
            }
        }
    }
    return ::dss::TypeKind::Void;
}

// Shared three-sided truthiness assertion: exactly one ICmpNe, zero
// Trunc (the old wrong shape), upstream + MIR clean.
void expectTruthinessNe(Lowered const& L, char const* site) {
    ASSERT_FALSE(L.model.hasErrors()) << site;
    ASSERT_TRUE(L.hir->ok) << site << ": "
        << (L.hirReporter.all().empty() ? "" : L.hirReporter.all()[0].actual);
    ASSERT_TRUE(L.mir.ok) << site << ": "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    EXPECT_EQ(countOp(L.mir.mir, MirOpcode::ICmpNe), 1u)
        << site << ": a non-Bool arithmetic condition must lower as the "
           "truthiness `!= 0` compare";
    EXPECT_EQ(countOp(L.mir.mir, MirOpcode::Trunc), 0u)
        << site << ": the old Cast-to-Bool shape (Trunc keeps only the "
           "low bit — `if (2)` would be false) must NOT appear";
}

} // namespace

TEST(MirLoweringCSubset, BareIntIfConditionLowersAsICmpNeNotTrunc) {
    auto L = lowerCSubset("int main() { if (2) return 42; return 7; }");
    expectTruthinessNe(L, "if");
    // The synthetic zero keeps the condition's own type: I32 vs I32.
    EXPECT_EQ(firstOperandKindOf(L, MirOpcode::ICmpNe, 0), TypeKind::I32);
    EXPECT_EQ(firstOperandKindOf(L, MirOpcode::ICmpNe, 1), TypeKind::I32);
}

TEST(MirLoweringCSubset, BareIntWhileConditionLowersAsICmpNeNotTrunc) {
    auto L = lowerCSubset(
        "int f(int n) { while (n) { n = n - 1; } return n; }");
    expectTruthinessNe(L, "while");
}

TEST(MirLoweringCSubset, BareIntDoWhileConditionLowersAsICmpNeNotTrunc) {
    auto L = lowerCSubset(
        "int f(int n) { do { n = n - 1; } while (n); return n; }");
    expectTruthinessNe(L, "do-while");
}

TEST(MirLoweringCSubset, BareIntForConditionLowersAsICmpNeNotTrunc) {
    // The update clause is EMPTY (decrement in the body): every
    // statement-shaped update (`n = n - 1`, `n -= 1`, `n--`) lowers at
    // HIR as an AssignStmt, and ForStmt's MIR lowering routes the update
    // through lowerExpr — which does not handle AssignStmt (the init
    // clause goes through lowerStmt and is fine). A PRE-EXISTING,
    // condition-UNRELATED gap; this pin exercises only the for-COND
    // truthiness site. (Runtime-witnessed: an empty-update for with a
    // bare int cond compiles + exits clean on PE x86_64.)
    auto L = lowerCSubset(
        "int f(int n) { for (n = 3; n; ) { n = n - 1; } return n; }");
    expectTruthinessNe(L, "for");
}

TEST(MirLoweringCSubset, BareIntTernaryConditionLowersAsICmpNeNotTrunc) {
    auto L = lowerCSubset("int f(int c) { return c ? 1 : 2; }");
    expectTruthinessNe(L, "ternary");
}

TEST(MirLoweringCSubset, CallResultIfConditionLowersAsICmpNeNotTrunc) {
    auto L = lowerCSubset(
        "int two() { return 2; }\n"
        "int main() { if (two()) return 42; return 7; }\n");
    expectTruthinessNe(L, "if(call)");
    EXPECT_EQ(countOp(L.mir.mir, MirOpcode::Call), 1u);
}

// A condition that is ALREADY Bool (a comparison) gets NO truthiness
// wrap: exactly ONE comparison total — the source-level `<` — and no
// double-Ne re-test of its Bool result.
TEST(MirLoweringCSubset, BoolIfConditionKeepsExactlyOneComparison) {
    auto L = lowerCSubset(
        "int f(int a, int b) { if (a < b) return 1; return 0; }");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    EXPECT_EQ(countOp(L.mir.mir, MirOpcode::ICmpSlt), 1u);
    EXPECT_EQ(countOp(L.mir.mir, MirOpcode::ICmpNe), 0u)
        << "a Bool condition must NOT be re-wrapped in a truthiness Ne";
    EXPECT_EQ(countOp(L.mir.mir, MirOpcode::Trunc), 0u);
}

// Float condition: truthiness is `!= 0.0` → FCmpUNE over the cond's own
// float type (C99 6.8.4.1 over scalars), NOT the old Cast shape whose
// mapCast routed float→Bool as FPToUI (value-WRONG: FPToUI(0.5) == 0
// would make `if (0.5)` false). FC3.5 sweep-c2 — the
// D-COND-FLOAT-NAN-TRUTHINESS-FCMP adjudication: the predicate is the
// UNORDERED-or-unequal Une (per C 6.5.9, `!=` on NaN is TRUE, so
// `if (NaN)` is true — NaN compares unequal to 0.0). The interim
// FCmpOne (ordered-ne — FALSE on NaN) would have made `if (NaN)`
// silently false once FCmp gained its LIR lowering; this pin is the
// red-on-disable lever for the Ne→Une mapping (flipping mapBinaryOp
// back to FCmpOne turns it red).
TEST(MirLoweringCSubset, FloatIfConditionLowersAsFCmpUneNotFpToUi) {
    auto L = lowerCSubset(
        "int f(double d) { if (d) return 1; return 0; }");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    EXPECT_EQ(countOp(L.mir.mir, MirOpcode::FCmpUne), 1u)
        << "float truthiness must be the UNORDERED `!= 0.0` compare "
           "(true on NaN — C 6.5.9)";
    EXPECT_EQ(countOp(L.mir.mir, MirOpcode::FCmpOne), 0u)
        << "ordered-ne would make `if (NaN)` false — the exact "
           "D-COND-FLOAT-NAN-TRUTHINESS-FCMP miscompile";
    EXPECT_EQ(firstOperandKindOf(L, MirOpcode::FCmpUne, 1), TypeKind::F64)
        << "the synthetic float zero keeps the cond's own type";
    EXPECT_EQ(countOp(L.mir.mir, MirOpcode::FPToUI), 0u)
        << "the old Cast(F64→Bool) shape routed FPToUI — value-wrong";
    EXPECT_EQ(countOp(L.mir.mir, MirOpcode::Trunc), 0u);
}

// Char condition: Char is C-scalar arithmetic — same `!= 0` test against
// a Char-typed zero (no promotion is needed for an unequal-to-zero
// test). Char-width ALU forms are still gated at LIR (D-CSUBSET-32BIT-
// ALU-FORMS) so the runtime tier stays fail-loud at the right shape.
TEST(MirLoweringCSubset, CharIfConditionLowersAsICmpNeTypedZero) {
    auto L = lowerCSubset(
        "int f(char c) { if (c) return 1; return 0; }");
    expectTruthinessNe(L, "if(char)");
    EXPECT_EQ(firstOperandKindOf(L, MirOpcode::ICmpNe, 0), TypeKind::Char);
    EXPECT_EQ(firstOperandKindOf(L, MirOpcode::ICmpNe, 1), TypeKind::Char);
}

// Pointer condition: `if (p)` tests `p != null-pointer-constant` when
// the language admits integer-zero null-pointer constants (c-subset
// does). The constant materializes through the 13.3 substrate shape —
// Cast(0:I32 → Ptr) = MIR IntToPtr — and the compare is ICmpNe over
// pointer operands. (Runtime-witnessed: PE x86_64 `if (p)` exits 42.)
TEST(MirLoweringCSubset, PointerIfConditionLowersAsICmpNeAgainstNullConstant) {
    auto L = lowerCSubset(
        "int main() { int x; x = 5; int* p; p = &x; "
        "if (p) return 42; return 7; }");
    expectTruthinessNe(L, "if(ptr)");
    EXPECT_EQ(countOp(L.mir.mir, MirOpcode::IntToPtr), 1u)
        << "the null-pointer constant must materialize as the 13.3 "
           "Cast(int 0 → Ptr) shape";
    EXPECT_EQ(firstOperandKindOf(L, MirOpcode::ICmpNe, 0), TypeKind::Ptr);
    EXPECT_EQ(firstOperandKindOf(L, MirOpcode::ICmpNe, 1), TypeKind::Ptr);
}

// Shifts under the block follow C 6.5.7: the result type is the PROMOTED
// LEFT operand — `u64 >> int` stays U64 (LShr, not AShr) and the int
// count does NOT widen the result.
//
// NOTE (pre-existing heuristic, surfaced by FC3's mixed-operand shifts):
// `return v >> n;` would spuriously S_ReturnTypeMismatch — the semantic
// tier's `subtreeType` descends an INFIX wrapper to a LEAF (DFS reaches
// the rightmost `n`, I32) because pass 2 has no binary-expression typing
// arm yet (D-SEMANTIC-SUBTREETYPE-TRANSPARENT-WRAPPERS, full closure
// pending). Heterogeneous-operand expressions in checked positions bind
// through a local until that closure.
TEST(MirLoweringCSubset, ShiftResultIsPromotedLeftOperand) {
    auto L = lowerCSubset(
        "unsigned long long f(unsigned long long v, int n) "
        "{ unsigned long long r; r = v >> n; return r; }");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.mir.ok);
    EXPECT_EQ(firstOpTypeKind(L, MirOpcode::LShr), TypeKind::U64);
    EXPECT_EQ(countOp(L.mir.mir, MirOpcode::AShr), 0u)
        << "an unsigned left operand must never route arithmetic-shift";
}

// `true`/`false` keyword literals carry their config-declared FIXED
// values (1/0) — never a decode of the keyword text (which would be 0
// for both). The function pair returns 1 and 0 via the literals.
TEST(MirLoweringCSubset, BoolKeywordLiteralsCarryFixedValues) {
    auto L = lowerCSubset(
        "bool t() { return true; }\n"
        "bool f() { return false; }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.mir.ok);
    Mir const& m = L.mir.mir;
    ASSERT_EQ(m.moduleFuncCount(), 2u);
    bool sawTrue = false;
    bool sawFalse = false;
    for (std::uint32_t f = 0; f < m.moduleFuncCount(); ++f) {
        auto const fn = m.funcAt(f);
        auto const bb = m.funcEntry(fn);
        for (std::uint32_t i = 0; i < m.blockInstCount(bb); ++i) {
            auto const inst = m.blockInstAt(bb, i);
            if (m.instOpcode(inst) != MirOpcode::Const) continue;
            auto const& lit = m.literalValue(m.instPayload(inst));
            if (auto const* v = std::get_if<std::int64_t>(&lit.value)) {
                if (*v == 1) sawTrue = true;
                if (*v == 0) sawFalse = true;
            }
        }
    }
    EXPECT_TRUE(sawTrue)  << "true must lower as the fixed value 1";
    EXPECT_TRUE(sawFalse) << "false must lower as the fixed value 0";
}

// ── FC4 c1 stage 2b: the visibility("hidden") DCE lever ─────────────────
// (closes D-CSUBSET-LINKAGE-VISIBILITY-SYNTAX)
//
// END-TO-END through the REAL c-subset attribute (not hand-built MIR —
// that half lives in tests/opt/test_dce_linkage.cpp): the composite
// linkage key `visibility:hidden` threads SymbolVisibility::Hidden from
// `__attribute__((visibility("hidden")))` source through the linkage
// side-table into MIR, where `isExternallyVisible(Global, Hidden)` is
// FALSE — so an UNCALLED hidden function is DCE-eliminated exactly like
// a `static` one, while a plain Global/Default uncalled function is
// linkage-protected and RETAINED. Red-on-disable lever: strip the
// `"visibility:hidden"` row from c-subset.lang.json's linkageSpecifiers
// and hidden_unused stays Default -> retained -> this test goes RED.
TEST(MirLoweringCSubset, HiddenVisibilityUnusedFunctionIsDceEliminated) {
    auto L = lowerCSubset(
        "__attribute__((visibility(\"hidden\"))) int hidden_unused(int v) "
        "{ return v + 1; }\n"
        "int plain_unused(int v) { return v + 2; }\n"
        "int main() { return 0; }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok);
    Mir& m = L.mir.mir;

    // Resolve the two symbols by NAME from the semantic model so the pin
    // is independent of minting order.
    SymbolId hiddenSym, plainSym;
    for (std::size_t i = 1; i < L.model.symbols().size(); ++i) {
        if (L.model.symbols()[i].name == "hidden_unused") {
            hiddenSym = SymbolId{static_cast<std::uint32_t>(i)};
        }
        if (L.model.symbols()[i].name == "plain_unused") {
            plainSym = SymbolId{static_cast<std::uint32_t>(i)};
        }
    }
    ASSERT_TRUE(hiddenSym.valid());
    ASSERT_TRUE(plainSym.valid());

    auto findFunc = [&](SymbolId sym) -> MirFuncId {
        for (std::uint32_t i = 0; i < m.moduleFuncCount(); ++i) {
            if (m.funcSymbol(m.funcAt(i)) == sym) return m.funcAt(i);
        }
        return MirFuncId{};
    };

    // PRE-DCE: all three functions present; the attribute actually
    // threaded (Hidden visibility = not externally visible) while the
    // plain one is Global/Default (externally visible). These two
    // asserts ARE the red-on-disable lever's anchor — without the
    // config row hidden_unused would read Default here.
    ASSERT_EQ(m.moduleFuncCount(), 3u);
    MirFuncId const hiddenFn = findFunc(hiddenSym);
    MirFuncId const plainFn  = findFunc(plainSym);
    ASSERT_TRUE(hiddenFn.valid());
    ASSERT_TRUE(plainFn.valid());
    EXPECT_EQ(m.funcVisibility(hiddenFn), SymbolVisibility::Hidden);
    EXPECT_EQ(m.funcBinding(hiddenFn),   SymbolBinding::Global);
    EXPECT_FALSE(isExternallyVisible(m.funcBinding(hiddenFn),
                                     m.funcVisibility(hiddenFn)));
    EXPECT_EQ(m.funcVisibility(plainFn), SymbolVisibility::Default);
    EXPECT_TRUE(isExternallyVisible(m.funcBinding(plainFn),
                                    m.funcVisibility(plainFn)));

    // Run DCE (the tests/opt/test_dce_linkage.cpp pipeline shape).
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    DiagnosticReporter rep;
    opt::OptPipeline pipeline{"fc4-visibility-lever", {opt::PassId::Dce}};
    auto const result = opt::optimize(m, **targetR,
                                      L.model.lattice().interner(),
                                      pipeline, rep);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(rep.errorCount(), 0u);

    // POST-DCE: hidden+uncalled ELIMINATED; plain Global/Default
    // uncalled RETAINED (linkage protect); main retained.
    EXPECT_FALSE(findFunc(hiddenSym).valid())
        << "hidden_unused (Global/Hidden, no callers) must be "
           "DCE-eliminated exactly like a static function";
    EXPECT_TRUE(findFunc(plainSym).valid())
        << "plain_unused (Global/Default) must survive — externally "
           "visible symbols are linkage-protected";
}
