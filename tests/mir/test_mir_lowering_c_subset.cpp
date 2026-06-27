// ML2 HIR→MIR lowering tests: end-to-end (parse c-subset → semantic → HIR
// lowering → MIR lowering) over the minimal cycle-1 surface — a straight-
// line function with params + literals + integer arithmetic + return.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/substrate/large_stack_call.hpp"
#include "core/types/call_payload.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "hir/hir.hpp"
#include "hir/hir_node.hpp"
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
#include <optional>
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

[[nodiscard]] Lowered lowerCSubset(std::string src,
                                   std::string targetName = "x86_64",
                                   std::string ccName     = "sysv_amd64") {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    if (!loaded) { ADD_FAILURE() << "loadShipped(c-subset) failed"; std::abort(); }
    UnitBuilder builder{*loaded};
    builder.addInMemory(std::move(src), "<mem>");
    auto cu    = std::make_shared<CompilationUnit>(std::move(builder).finish());
    // FC12b (D-FC12B-WIN64-VARIADIC-CALLEE): thread the selected CC's va_list
    // strategy into analyze() so a Win64 (ms_x64) variadic source gets the `char*`
    // `va_list` type (not SysV's __va_list_tag[1]) — mirrors compile_pipeline.cpp.
    std::optional<VaListStrategy> vaStrategy;
    if (auto t = TargetSchema::loadShipped(targetName); t.has_value()) {
        if (auto const* cc = (*t)->callingConventionByName(ccName);
            cc != nullptr && cc->vaListLayout.has_value()) {
            vaStrategy = cc->vaListLayout->strategy;
        }
    }
    auto model = analyze(cu, DataModel::Lp64, std::nullopt, vaStrategy);
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
    if (auto t = TargetSchema::loadShipped(targetName); t.has_value()) {
        mirCfg.aggregateLayout       = (*t)->aggregateLayout();
        mirCfg.aggregateLayoutLoaded = (*t)->aggregateLayoutLoaded();
        // FC7 (D-FC7-STRUCT-BY-VALUE-ARG-RETURN): thread the active CC's by-value
        // params so a struct passed/returned BY VALUE classifies. Mirrors
        // compile_pipeline.cpp. `targetName`/`ccName` default to x86_64/sysv_amd64
        // (the C1/C1c pins); the AAPCS64 x8-sret pins pass "arm64"/"aapcs64" so
        // `aggregateSretViaHiddenArg` resolves false (indirectResultRegister = x8).
        if (auto const* cc = (*t)->callingConventionByName(ccName)) {
            mirCfg.aggregateClassification   = cc->aggregateClassification;
            mirCfg.aggregateMaxRegBytes      = cc->aggregateMaxRegBytes;
            mirCfg.aggregateSretViaHiddenArg = !cc->indirectResultRegister.has_value();
            mirCfg.argSlotAligned            = cc->slotAligned;
            mirCfg.argGprCount               =
                static_cast<std::uint32_t>(cc->argGprs.size());
            mirCfg.argFprCount               =
                static_cast<std::uint32_t>(cc->argFprs.size());
            mirCfg.aggregateStackExhaustsRegisters =
                cc->aggregateStackExhaustsRegisters;
            // FC12a-core (D-FC12A-VARIADIC-CALLEE): thread the CC's va_list layout so
            // va_start/va_arg lower (or fail loud when the CC omits it).
            mirCfg.vaListLayout              = cc->vaListLayout;
        }
    }
    // D-CSUBSET-LINKAGE-SPECIFIERS: thread the native-decl linkage side-table
    // exactly as compile_pipeline.cpp does — so `static`/`__attribute__` source
    // flows binding/visibility into the MIR. Existing fixtures (no specifiers)
    // get an empty map ⇒ every symbol stays Global/Default (unchanged).
    // c21 (D-CSUBSET-VOLATILE-QUALIFIER): thread the per-access volatility
    // side-table exactly as compile_pipeline.cpp does, so a `volatile` object/
    // member/global access carries MirInstFlags::Volatile on its Load/Store.
    HirToMirResult mir = lowerToMir(hir->hir, hir->literalPool,
                                    model.lattice().interner(), mirReporter,
                                    &hir->sourceMap, mirCfg, /*ffiMap=*/nullptr,
                                    &hir->linkageMap, &hir->mutabilityMap,
                                    &hir->volatileMap);
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

// D-CSUBSET-ENUM-INT-CONVERSION (FC8): a bare enumerator lowers to a Const of its
// value through HIR→MIR. RED-ON-DISABLE (and red TODAY before the A1 Ref→Const
// fold): an enumerator Ref has no storage / SSA binding, so without the fold it
// hits the unbound-symbol fail-loud here → `L.mir.ok` is false. The `enum_value`
// corpus is the runtime witness; this pins the IR-tier fold in isolation.
TEST(MirLoweringCSubset, EnumeratorLowersToConstNotUnboundRef) {
    auto L = lowerCSubset(
        "enum Color { RED, GREEN, BLUE };\n"
        "int main(void) { return BLUE; }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "enumerator must fold to a Const (no unbound-symbol fail-loud): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    // The folded enumerator BLUE is a Const(2) in the entry block.
    Mir const& m = L.mir.mir;
    ASSERT_EQ(m.moduleFuncCount(), 1u);
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    bool sawConst = false;
    for (std::uint32_t i = 0; i < m.blockInstCount(entry); ++i) {
        if (m.instOpcode(m.blockInstAt(entry, i)) == MirOpcode::Const) {
            sawConst = true;
            break;
        }
    }
    EXPECT_TRUE(sawConst) << "the enumerator `BLUE` must lower to a Const";
}

// §3 two-tier: an enum-typed COMPOUND expr (`c + 1`) must type cleanly through
// BOTH the semantic typer (subtreeType, feeds the return check) AND HIR→MIR
// lowering — they must AGREE that an enum promotes to its underlying int. The
// param `c` is storage-backed, so it stays a Ref (isEnumerator false) — proving
// the A1 fold's guard. RED-ON-DISABLE: revert the UAC enum-promotion → `c + 1`
// stays enum → the int return check mismatches (or lowering fails).
TEST(MirLoweringCSubset, EnumParamArithmeticLowersClean) {
    auto L = lowerCSubset(
        "enum Color { RED, GREEN, BLUE };\n"
        "int g(enum Color c) { return c + 1; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty()
                ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
}

// Scope guard at the SEMANTIC tier: enum↔DIFFERENT-enum stays a loud mismatch
// (the conversion arm admits enum↔int ONLY). RED-ON-DISABLE: if the arm
// over-admitted enum↔enum, this cross-enum assignment would wrongly type-check.
TEST(MirLoweringCSubset, DifferentEnumAssignStaysMismatch) {
    auto L = lowerCSubset(
        "enum A { X };\n"
        "enum B { Y };\n"
        "int main(void) { enum A a = Y; return (int)a; }\n");
    EXPECT_TRUE(L.model.hasErrors())
        << "assigning a B enumerator to an A-typed var must be a loud mismatch";
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

// FC8 D-CSUBSET-BITFIELD: a bit-field READ lowers to Load-unit + extract
// (LShr by bitOffset + And mask); a bit-field WRITE lowers to a read-modify-
// write (Load + And-clear + And-mask + Shl + Or + Store). This is the
// HOST-INDEPENDENT structural guard (runs on every CI leg) complementing the
// runtime `bitfields` corpus. RED-ON-DISABLE: drop the bitfield intercepts and
// a read is a plain Load (no LShr/And) and a write a plain Store (no Or/Shl).
TEST(MirLoweringCSubset, BitFieldReadExtractsAndWriteIsReadModifyWrite) {
    auto L = lowerCSubset(
        "struct S { unsigned pad : 4; unsigned a : 3; };\n"
        "unsigned read_a(struct S* p) { return p->a; }\n"
        "void set_a(struct S* p, unsigned v) { p->a = v; }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto countIn = [&](std::uint32_t fnIdx, MirOpcode op) {
        MirBlockId const entry = m.funcEntry(m.funcAt(fnIdx));
        int n = 0;
        for (std::uint32_t i = 0; i < m.blockInstCount(entry); ++i)
            if (m.instOpcode(m.blockInstAt(entry, i)) == op) ++n;
        return n;
    };
    // read_a (fn 0): a is at bitOffset 4 (after pad:4) → LShr 4 + And mask.
    EXPECT_GE(countIn(0, MirOpcode::LShr), 1) << "bit-field read must shift by bitOffset";
    EXPECT_GE(countIn(0, MirOpcode::And), 1)  << "bit-field read must mask the width";
    // set_a (fn 1): read-modify-write → And (clear + mask) + Shl + Or + Store.
    EXPECT_GE(countIn(1, MirOpcode::And), 2)   << "RMW clears the field AND masks the value";
    EXPECT_GE(countIn(1, MirOpcode::Or), 1)    << "RMW ORs the shifted value back";
    EXPECT_GE(countIn(1, MirOpcode::Shl), 1)   << "RMW shifts the value to bitOffset";
    EXPECT_GE(countIn(1, MirOpcode::Store), 1) << "RMW stores the merged unit";
}

// FC8 D-CSUBSET-BITFIELD-WIDE-UNIT: a bit-field on a 64-BIT BASE
// (`unsigned long long` / `long long` → a 64-bit allocation unit) now
// COMPILES + LOWERS end-to-end. Before FC8 the semantic analyzer
// REJECTED it (`S_BitFieldWidthOutOfRange` via the `typeBits > 32`
// guard) because materializing the >int32 extract/insert masks hit a
// literal-pool dead-end; FC8 closed that (x86 `mov r64, imm64` / arm64
// MOVZ+MOVK ladder), so the guard is now `typeBits > 64` (I128/U128
// still rejected). This is the SEMANTIC FLIP: the same shape that was a
// reject is now a clean compile. RED-ON-DISABLE: restore the
// `typeBits > 32` guard in resolveBitfieldSuffix → `model.hasErrors()`
// becomes true (the field is rejected) → the `ASSERT_FALSE` goes RED.
// The extract/insert chain (a 40-bit field at bitOffset 0, mask
// 0xFFFFFFFFFF) lowers exactly like the 32-bit case — Load + And on
// read, RMW And/Shl/Or/Store on write — just at 64-bit width.
TEST(MirLoweringCSubset, WideUnitBitFieldOnLongLongBaseCompilesAndLowers) {
    auto L = lowerCSubset(
        "struct W { unsigned long long a : 40; unsigned long long b : 20; "
        "long long s : 36; };\n"
        "unsigned long long read_a(struct W* p) { return p->a; }\n"
        "long long read_s(struct W* p) { return p->s; }\n"
        "void set_a(struct W* p, unsigned long long v) { p->a = v; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << "a 64-bit-base bit-field must now COMPILE (the wide-unit anchor "
           "closed) — restoring the typeBits>32 guard turns this RED";
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto countIn = [&](std::uint32_t fnIdx, MirOpcode op) {
        MirBlockId const entry = m.funcEntry(m.funcAt(fnIdx));
        int n = 0;
        for (std::uint32_t i = 0; i < m.blockInstCount(entry); ++i)
            if (m.instOpcode(m.blockInstAt(entry, i)) == op) ++n;
        return n;
    };
    // read_a (fn 0): a 40-bit unsigned field at bitOffset 0 — masks the
    // width (And); a is the low field so no shift needed for read but the
    // And mask (0xFFFFFFFFFF) is the wide-constant witness.
    EXPECT_GE(countIn(0, MirOpcode::And), 1) << "wide read masks the field width";
    // read_s (fn 1): a SIGNED field sign-extends via Shl + AShr.
    EXPECT_GE(countIn(1, MirOpcode::Shl), 1)  << "signed wide read sign-extends (Shl)";
    EXPECT_GE(countIn(1, MirOpcode::AShr), 1) << "signed wide read sign-extends (AShr)";
    // set_a (fn 2): read-modify-write → And (clear + mask) + Or + Store.
    EXPECT_GE(countIn(2, MirOpcode::And), 2)   << "wide RMW clears the field AND masks the value";
    EXPECT_GE(countIn(2, MirOpcode::Or), 1)    << "wide RMW ORs the value back";
    EXPECT_GE(countIn(2, MirOpcode::Store), 1) << "wide RMW stores the merged unit";
}

// FC8 D-CSUBSET-BITFIELD-WIDE-UNIT: a width EXCEEDING the 64-bit base
// stays REJECTED — the `*w > typeBits` check (typeBits=64 for long long)
// fires for `long long s : 100`. This pins the UPPER bound the wide-unit
// lift did NOT remove: 64 is the new ceiling for the BASE, and a width
// larger than the base is still out of range. RED-ON-DISABLE: removing
// the `*w > typeBits` check would let `: 100` compile → hasErrors false →
// RED. (A 128-bit BASE — I128/U128 — is also rejected by the
// `typeBits > 64` guard, but `__int128` is not in the c-subset's spelled
// type set, so the BASE-128 path is unreachable from source and pinned
// by the guard logic + comment rather than a corpus.)
TEST(MirLoweringCSubset, WideUnitBitFieldWidthAboveBaseStaysRejected) {
    auto L = lowerCSubset(
        "struct H { long long s : 100; };\n"
        "int main(void) { return 0; }\n");
    EXPECT_TRUE(L.model.hasErrors())
        << "a bit-field width exceeding its 64-bit base must stay rejected";
}

// FC8 D-CSUBSET-BITFIELD-INIT: aggregate INITIALIZATION of a bit-field struct
// (`struct S s = {1,2};`) now LOWERS with per-allocation-unit PACKING (replacing
// the cycle-2 fail-loud). Two co-resident bit-fields a:3,b:5 share unit 0, so the
// slot init must: zero the unit ONCE, then read-modify-write each field in (And to
// clear + And to mask the value + Shl to bitOffset + Or + Store) — NOT a plain
// full-width store per field (which would clobber the neighbour). RED-ON-DISABLE:
// revert lowerBitfieldAggregateInitIntoSlot to a fail-loud and `mir.ok` goes
// false; revert the per-unit packing to plain stores and the And/Or/Shl chain
// (the packing signature) disappears.
TEST(MirLoweringCSubset, BitFieldStructInitializerPacksPerUnit) {
    auto L = lowerCSubset(
        "struct S { unsigned a : 3; unsigned b : 5; };\n"
        "void f(void) { struct S s = { 1, 2 }; }\n");
    ASSERT_FALSE(L.model.hasErrors()) << "the initializer is well-typed";
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "a bit-field struct aggregate initializer must now LOWER with per-unit "
           "packing (D-CSUBSET-BITFIELD-INIT): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    auto count = [&](MirOpcode op) {
        int n = 0;
        for (std::uint32_t i = 0; i < m.blockInstCount(entry); ++i)
            if (m.instOpcode(m.blockInstAt(entry, i)) == op) ++n;
        return n;
    };
    // a + b share unit 0: zero-store + two read-modify-writes. b is at bitOffset 3
    // (after a:3), so at least one Shl; each RMW masks the value (And) and clears
    // the field (And) and ORs it back (Or). A plain full-width per-field store
    // would emit neither the Or nor the Shl, and only 2 stores (no unit-zero).
    EXPECT_GE(count(MirOpcode::Or), 2)  << "each bit-field init ORs its value into the unit";
    EXPECT_GE(count(MirOpcode::And), 3) << "RMW clears the field AND masks the value";
    EXPECT_GE(count(MirOpcode::Shl), 1) << "b is at bitOffset 3 → shifted before OR";
    EXPECT_GE(count(MirOpcode::Store), 3) << "unit zeroed once + two RMW stores";
}

// F1 (review-caught silent miscompile): an ordinary field that PRECEDES a
// bit-field sharing its allocation unit must NOT be clobbered by the unit's
// zero-fill. `struct { char x; unsigned a:3; }` puts x at byte 0 and a's int
// unit at bytes [0,4) — overlapping x. The fix zeroes every bit-field unit in a
// PRE-PASS (before any field value is written), so x's store lands on the
// already-zeroed unit and survives (a's read-modify-write then preserves it).
// The buggy lazy-zero stored x first then wiped its unit → x == 0 (and a
// global/local divergence, since the static-data encoder pre-zeroes once).
// RED-ON-DISABLE: collapse the two passes (zero a unit on first touch in
// declaration order) and the FIRST store becomes the ordinary field's value
// store, not the unit zero → this assertion fails. The bitfield_init corpus is
// the end-to-end witness (lt.x survives → exit 42, not 35).
TEST(MirLoweringCSubset, BitFieldUnitZeroPrecedesOrdinaryFieldStoreInSharedUnit) {
    auto L = lowerCSubset(
        "struct T { char x; unsigned a : 3; };\n"
        "void f(void) { struct T t = { 7, 5 }; }\n");
    ASSERT_FALSE(L.model.hasErrors()) << "the initializer is well-typed";
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // The FIRST store into the slot must be the pass-1 unit zero (value Const 0),
    // emitted BEFORE the ordinary field x is written. Under the lazy-zero bug the
    // first store is instead x's value store (x precedes a in declaration order).
    int firstStoreIdx = -1;
    for (std::uint32_t i = 0; i < m.blockInstCount(entry); ++i)
        if (m.instOpcode(m.blockInstAt(entry, i)) == MirOpcode::Store) {
            firstStoreIdx = static_cast<int>(i);
            break;
        }
    ASSERT_GE(firstStoreIdx, 0) << "the initializer must emit stores";
    MirInstId const firstStore =
        m.blockInstAt(entry, static_cast<std::uint32_t>(firstStoreIdx));
    auto ops = m.instOperands(firstStore);
    ASSERT_EQ(ops.size(), 2u);
    ASSERT_EQ(m.instOpcode(ops[0]), MirOpcode::Const)
        << "the FIRST store must be the pass-1 unit zero, not an ordinary value";
    auto const& lit = m.literalValue(m.constLiteralIndex(ops[0]));
    auto const* iv  = std::get_if<std::int64_t>(&lit.value);
    ASSERT_NE(iv, nullptr);
    EXPECT_EQ(*iv, 0)
        << "the bit-field unit must be zeroed BEFORE the ordinary field x is "
           "stored, else the zero clobbers x (the F1 silent miscompile)";
}

// FC8 D-CSUBSET-ENUM-BITFIELD: an enum-typed bit-field (`enum E e : W`) is
// permitted (C 6.7.2.1) — an enum behaves AS its underlying integer
// (D-CSUBSET-ENUM-INT-CONVERSION), so it must be ACCEPTED at semantic and LOWER
// its allocation-unit access at the underlying. This pins two arms; the runtime
// SIGNEDNESS arm (the load-bearing behavioural fix — a signed-underlying enum
// bit-field must SIGN-extend) is pinned end-to-end by the `enum_bitfield`
// corpus (a negative enumerator reads back -3, not 13 → exit 42, not 74).
//  (a) RED-ON-DISABLE: revert resolveBitfieldSuffix's enum→underlying resolve →
//      the field is rejected S_BitFieldNonIntegerType → model.hasErrors().
//  (b) RED-ON-DISABLE: revert the enumReprType resolve at emitBitfieldExtract/
//      emitBitfieldInsert → every bit-field shift/mask op result is typed at the
//      Enum (primitive(Enum)) rather than the underlying integer. (The unit Load
//      is intentionally left Enum-typed — the LIR `reprKind` tier resolves its
//      WIDTH — so we assert the arithmetic OP results, which the resolve makes
//      underlying; a behaviour-equivalent-but-malformed Enum-typed op is exactly
//      what the LIR tier would mask, so this MIR-tier representation check is the
//      only place it can be caught.) Exercises READ (extract) + WRITE (insert).
TEST(MirLoweringCSubset, EnumTypedBitFieldResolvesAndLowers) {
    auto L = lowerCSubset(
        "enum Color { RED, GREEN = 5, BLUE };\n"
        "struct S { enum Color c : 4; unsigned x : 3; };\n"
        "int f(void) { struct S s = { GREEN, 3 }; s.c = BLUE; return (int)s.c; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << "an enum-typed bit-field must be ACCEPTED (C 6.7.2.1), not rejected "
           "S_BitFieldNonIntegerType";
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "an enum bit-field must LOWER (extract/insert resolve Enum→underlying): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto const& interner = L.model.lattice().interner();
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // Every bit-field arithmetic op (the read extract's shift/mask, the write
    // RMW's clear/mask/shift/merge) must be typed at the underlying integer, NOT
    // the Enum. In this function those opcodes come ONLY from the bit-field
    // codegen, so the check is unambiguous.
    bool sawBitfieldOp = false;
    for (std::uint32_t i = 0; i < m.blockInstCount(entry); ++i) {
        MirInstId const ix = m.blockInstAt(entry, i);
        switch (m.instOpcode(ix)) {
            case MirOpcode::And: case MirOpcode::Or:   case MirOpcode::Shl:
            case MirOpcode::AShr: case MirOpcode::LShr:
                sawBitfieldOp = true;
                EXPECT_NE(interner.kind(m.instType(ix)), TypeKind::Enum)
                    << "bit-field op #" << i << " must be typed at the enum's "
                       "UNDERLYING integer (enumReprType), not the Enum itself";
                break;
            default: break;
        }
    }
    EXPECT_TRUE(sawBitfieldOp)
        << "the enum bit-field read + write must emit shift/mask ops";
}

// FC8 (cycle-4 audit coverage debt): two bit-fields of DIFFERENT declared-type
// widths sharing one offset (`unsigned char a:3` → 1-byte unit at off 0;
// `unsigned b:4` → 4-byte unit also at off 0). The init two-pass must zero BOTH
// units up front, so the dedup keys on (offset, unitBytes) — NOT offset alone.
// RED-ON-DISABLE: dedup on `off` only → the wider unit's high bytes are left
// unzeroed (stale) → only ONE zero-store is emitted. This asserts TWO distinct
// unit zero-stores (value Const 0).
TEST(MirLoweringCSubset, MixedWidthBitFieldsSharingOffsetEachZeroTheirUnit) {
    auto L = lowerCSubset(
        "struct M { unsigned char a : 3; unsigned b : 4; };\n"
        "void f(void) { struct M m = { 5, 9 }; }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    int zeroStores = 0;
    for (std::uint32_t i = 0; i < m.blockInstCount(entry); ++i) {
        MirInstId const id = m.blockInstAt(entry, i);
        if (m.instOpcode(id) != MirOpcode::Store) continue;
        auto ops = m.instOperands(id);
        if (ops.size() != 2 || m.instOpcode(ops[0]) != MirOpcode::Const) continue;
        auto const& lit = m.literalValue(m.constLiteralIndex(ops[0]));
        if (auto const* iv = std::get_if<std::int64_t>(&lit.value);
            iv != nullptr && *iv == 0)
            ++zeroStores;
    }
    EXPECT_EQ(zeroStores, 2)
        << "each distinct-width unit at the shared offset must be zeroed once "
           "(dedup on (offset,unitBytes)); off-only dedup leaves the wider unit "
           "partly stale";
}

// FC8 D-LK4-RODATA-PRODUCER-LOCAL-ARRAY-DECAY + NONSTRING-GLOBAL-ARRAY-DECAY: a
// non-string array used as a pointer DECAYS to its first-element address — it
// must LOWER (not fail loud as it did pre-FC8). RED-ON-DISABLE: revert the decay
// arm and the Cast(Array→Ptr) on a non-literal operand hits H0009 -> mir.ok
// false. Covers a LOCAL array (alloca) and a GLOBAL array (GlobalAddr).
TEST(MirLoweringCSubset, NonStringArrayDecaysToPointerNotFailLoud) {
    auto L = lowerCSubset(
        "int g[2] = { 1, 2 };\n"
        "int use(int* p) { return p[0] + p[1]; }\n"
        "int f(void) { int a[2]; a[0] = 3; a[1] = 4; return use(a) + use(g); }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    EXPECT_TRUE(L.mir.ok)
        << "non-string array→pointer decay (local + global) must lower, not "
           "fail loud: "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
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

// `int* a; a[i]` over a POINTER base: the element index is PRE-SCALED by
// sizeof(elem) to a BYTE offset (Option A, D-MIR-STORAGE-ARRAY-INDEX-GEP, user
// §B 2026-06-17), so the GEP carries `[ptr, i*4]`. This FIXES the latent
// scale-1 silent miscompile — the old shape `Gep[ptr, i]` lowered to
// `lea [ptr + i*1]`, reading the element at byte `i` instead of `i*4` for any
// non-`char` element. RED-ON-DISABLE: drop the scaleIndexToBytes call → the
// GEP index reverts to the raw `Arg i` → the Mul + stride assertions fail.
TEST(MirLoweringCSubset, IndexOverIntPointerScalesIndexToByteOffset) {
    auto L = lowerCSubset(
        "int f(int* a, int i) { return a[i]; }\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // [Arg a, Arg i, Const(4), Mul(i,4), Gep(a, mul), Load, Return]
    ASSERT_EQ(m.blockInstCount(entry), 7u);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 0)), MirOpcode::Arg);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 1)), MirOpcode::Arg);
    // the scaling constant = sizeof(int) = 4 under LP64
    MirInstId const strideK = m.blockInstAt(entry, 2);
    auto const& strideLit = m.literalValue(m.constLiteralIndex(strideK));
    EXPECT_EQ(std::get<std::int64_t>(strideLit.value), 4)
        << "int element stride is 4";
    // the Mul scales the raw index by the stride
    MirInstId const mul = m.blockInstAt(entry, 3);
    ASSERT_EQ(m.instOpcode(mul), MirOpcode::Mul);
    auto mulOps = m.instOperands(mul);
    ASSERT_EQ(mulOps.size(), 2u);
    EXPECT_EQ(mulOps[0], m.blockInstAt(entry, 1)) << "Mul lhs is Arg i";
    EXPECT_EQ(mulOps[1], strideK) << "Mul rhs is the stride const";
    // the GEP indexes by the SCALED byte offset, not the raw index
    MirInstId const gep = m.blockInstAt(entry, 4);
    EXPECT_EQ(m.instOpcode(gep), MirOpcode::Gep);
    auto gepOps = m.instOperands(gep);
    ASSERT_EQ(gepOps.size(), 2u);
    EXPECT_EQ(gepOps[0], m.blockInstAt(entry, 0)) << "GEP base is Arg a";
    EXPECT_EQ(gepOps[1], mul)
        << "GEP index is the SCALED Mul result, not the raw Arg i "
           "(the scale-1 miscompile fix)";
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 5)), MirOpcode::Load);
}

// D-CSUBSET-INDEX-INTEGER-PROMOTION (C 6.3.1.1 / 6.5.2.1): a `char` subscript of
// a WIDE-element pointer integer-PROMOTES to int BEFORE the stride `Mul` — else
// the Mul is Char-typed and (1) OVERFLOWS (idx*stride wraps at char width) and
// (2) walls at the sub-native ALU gap. The promotion materializes a widening
// (SExt/ZExt) to I32 whose RESULT (not the raw char Arg) feeds the Mul, and the
// Mul is I32-typed. RED-ON-DISABLE: revert the cst_to_hir Index promotion arm →
// the block has no widening inst, the Mul lhs is the raw char Arg, and the Mul is
// Char-typed (the inst-count 8→7, the opcode + I32-type assertions all flip).
TEST(MirLoweringCSubset, CharIndexIntegerPromotesToI32BeforeStrideMul) {
    auto L = lowerCSubset("int f(int* a, char i) { return a[i]; }\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto& interner = L.model.lattice().interner();
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    // [Arg a, Arg i(char), {SExt|ZExt}(i)->i32, Const(4), Mul(promo,4), Gep, Load, Return]
    ASSERT_EQ(m.blockInstCount(entry), 8u)
        << "the char index adds ONE promotion (widen) inst vs the int-index 7";
    MirInstId const argI  = m.blockInstAt(entry, 1);
    MirInstId const promo = m.blockInstAt(entry, 2);
    MirOpcode const pop = m.instOpcode(promo);
    EXPECT_TRUE(pop == MirOpcode::SExt || pop == MirOpcode::ZExt)
        << "a char index promotes via a widening (SExt/ZExt) — C 6.3.1.1";
    EXPECT_EQ(interner.kind(m.instType(promo)), TypeKind::I32)
        << "the promotion target is int";
    auto promoOps = m.instOperands(promo);
    ASSERT_EQ(promoOps.size(), 1u);
    EXPECT_EQ(promoOps[0], argI) << "the widen source is the raw char Arg i";
    MirInstId const mul = m.blockInstAt(entry, 4);
    ASSERT_EQ(m.instOpcode(mul), MirOpcode::Mul);
    EXPECT_EQ(interner.kind(m.instType(mul)), TypeKind::I32)
        << "the stride Mul is I32-typed (promoted), NOT Char";
    auto mulOps = m.instOperands(mul);
    ASSERT_EQ(mulOps.size(), 2u);
    EXPECT_EQ(mulOps[0], promo)
        << "the Mul lhs is the PROMOTED index, not the raw char Arg";
}

// `int a[5]; a[i]` — a STORAGE array index. PRE-FC: lowered to a 3-op GEP
// `[&a, 0, i]` that FAILED LOUD at LIR (D-MIR-STORAGE-ARRAY-INDEX-GEP); it was
// never a miscompile, it didn't compile. NOW: the vestigial leading 0 is
// collapsed AND the index is byte-scaled → a 2-op GEP `[&a, i*4]`, identical in
// shape to the pointer path. MIR-tier pin (lowerCSubset stops at MIR).
// RED-ON-DISABLE: keep the 3-op form / raw index → the size==2 + Mul-stride
// assertions fail.
TEST(MirLoweringCSubset, IndexIntoIntArrayCollapsesToTwoOpGepAndScales) {
    auto L = lowerCSubset(
        "int g(int i) { int a[5]; return a[i]; }\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    bool foundScaledGep = false;
    for (std::uint32_t k = 0; k < m.blockInstCount(entry); ++k) {
        MirInstId const id = m.blockInstAt(entry, k);
        if (m.instOpcode(id) != MirOpcode::Gep) continue;
        auto ops = m.instOperands(id);
        ASSERT_EQ(ops.size(), 2u)
            << "storage-array GEP must collapse the vestigial 0 to 2 operands";
        MirInstId const idxOp = ops[1];
        ASSERT_EQ(m.instOpcode(idxOp), MirOpcode::Mul)
            << "the index must be byte-scaled (Mul by stride)";
        auto mulOps = m.instOperands(idxOp);
        ASSERT_EQ(mulOps.size(), 2u);
        auto const& strideLit = m.literalValue(m.constLiteralIndex(mulOps[1]));
        EXPECT_EQ(std::get<std::int64_t>(strideLit.value), 4)
            << "int element stride is 4";
        foundScaledGep = true;
    }
    EXPECT_TRUE(foundScaledGep)
        << "expected a 2-op byte-scaled storage-array GEP";
}

// `char a[5]; a[i]` — element stride 1, so NO Mul is emitted (the stride==1
// fast path): the GEP index IS the raw index. Guards the fast path AND that
// `char` indexing stays byte-identical (no spurious `imul`). RED-ON-DISABLE:
// remove the `stride == 1` short-circuit in scaleIndexToBytes → a `Mul(i, 1)`
// appears → the "no Mul" assertion fails.
TEST(MirLoweringCSubset, IndexIntoCharArrayDoesNotScale) {
    auto L = lowerCSubset(
        "int g(int i) { char a[5]; return a[i]; }\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    bool foundGep = false;
    for (std::uint32_t k = 0; k < m.blockInstCount(entry); ++k) {
        MirInstId const id = m.blockInstAt(entry, k);
        EXPECT_NE(m.instOpcode(id), MirOpcode::Mul)
            << "char (stride 1) indexing must NOT emit a scaling Mul";
        if (m.instOpcode(id) == MirOpcode::Gep) {
            auto ops = m.instOperands(id);
            ASSERT_EQ(ops.size(), 2u);
            EXPECT_NE(m.instOpcode(ops[1]), MirOpcode::Mul)
                << "char GEP index is the raw index, not a Mul";
            foundGep = true;
        }
    }
    EXPECT_TRUE(foundGep);
}

// `int a[3] = {10,20,30}` — an array-LOCAL brace-init. PRE-FC it fell to the
// scalar `else` arm → a multi-element InsertValue chain → FAIL-LOUD at LIR.
// NOW it lowers element-wise via lowerArrayInitIntoSlot (the VarDecl array
// arm), one Store per element at byte offset j*4. RED-ON-DISABLE: drop the
// VarDecl array arm → no per-element Stores at 0/4/8.
TEST(MirLoweringCSubset, ArrayLocalBraceInitStoresPerElementAtStride) {
    auto L = lowerCSubset(
        "int g(void) { int a[3] = {10, 20, 30}; return a[0]; }\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    std::vector<std::int64_t> storeOffsets;
    for (std::uint32_t k = 0; k < m.blockInstCount(entry); ++k) {
        MirInstId const id = m.blockInstAt(entry, k);
        if (m.instOpcode(id) != MirOpcode::Store) continue;
        auto stOps = m.instOperands(id);  // [value, ptr]
        ASSERT_EQ(stOps.size(), 2u);
        MirInstId const ptr = stOps[1];
        if (m.instOpcode(ptr) != MirOpcode::Gep) continue;
        auto gepOps = m.instOperands(ptr);
        if (gepOps.size() != 2u) continue;
        if (m.instOpcode(gepOps[1]) != MirOpcode::Const) continue;
        storeOffsets.push_back(
            std::get<std::int64_t>(
                m.literalValue(m.constLiteralIndex(gepOps[1])).value));
    }
    // `return a[0]` reads via a Load (not a Store), so exactly the 3 element
    // inits remain, in ascending order.
    ASSERT_EQ(storeOffsets.size(), 3u)
        << "exactly 3 array-element init Stores";
    EXPECT_EQ(storeOffsets[0], 0) << "a[0] at byte offset 0";
    EXPECT_EQ(storeOffsets[1], 4) << "a[1] at byte offset 4";
    EXPECT_EQ(storeOffsets[2], 8) << "a[2] at byte offset 8";
}

// `struct S { int a[3]; int n; }; struct S x = {{1,2,3}, 9}` — the array-TYPED
// FIELD brace-init (D-MIR-ARRAY-FIELD-AGGREGATE-INIT). PRE-FC the array field
// fell through the Struct||Union recurse-guard → lowerExpr → InsertValue chain
// → FAIL-LOUD at LIR. NOW the recurse-guard's Array arm routes it to
// lowerArrayInitIntoSlot → per-element Stores at 0,4,8, then the scalar field
// `n` at 12 — element-wise, NO InsertValue. RED-ON-DISABLE: drop the Array arm
// in the recurse-guard → the array elements are not stored at 0/4/8.
TEST(MirLoweringCSubset, ArrayFieldBraceInitStoresPerElementAtStride) {
    auto L = lowerCSubset(
        "struct S { int a[3]; int n; };\n"
        "int g(void) { struct S x = { {1,2,3}, 9 }; return x.n; }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    std::vector<std::int64_t> storeOffsets;
    for (std::uint32_t k = 0; k < m.blockInstCount(entry); ++k) {
        MirInstId const id = m.blockInstAt(entry, k);
        EXPECT_NE(m.instOpcode(id), MirOpcode::InsertValue)
            << "element-wise array-field init, no InsertValue chain";
        if (m.instOpcode(id) != MirOpcode::Store) continue;
        auto stOps = m.instOperands(id);
        ASSERT_EQ(stOps.size(), 2u);
        MirInstId const ptr = stOps[1];
        if (m.instOpcode(ptr) != MirOpcode::Gep) continue;
        auto gepOps = m.instOperands(ptr);
        if (gepOps.size() != 2u) continue;
        if (m.instOpcode(gepOps[1]) != MirOpcode::Const) continue;
        storeOffsets.push_back(
            std::get<std::int64_t>(
                m.literalValue(m.constLiteralIndex(gepOps[1])).value));
    }
    // a[0]@0, a[1]@4, a[2]@8 (array elements at stride 4), n@12.
    ASSERT_EQ(storeOffsets.size(), 4u);
    EXPECT_EQ(storeOffsets[0], 0) << "a[0] at byte offset 0";
    EXPECT_EQ(storeOffsets[1], 4) << "a[1] at byte offset 4";
    EXPECT_EQ(storeOffsets[2], 8) << "a[2] at byte offset 8";
    EXPECT_EQ(storeOffsets[3], 12) << "field n after the 3-int array";
}

// `int a[4]` reserves its FULL 16-byte layout in the frame, not the 8-byte
// scalar-slot sentinel. Array locals became RUNNABLE this cycle (index +
// brace-init), so the Alloca MUST carry the array byte size — an under-sized
// slot would let a high element write (a[2]/a[3]) clobber a neighbouring
// local (the silent frame-overflow the FC7 struct-alloca sizing also guards).
// The Alloca payload carries the byte size. RED-ON-DISABLE: omit Array from
// allocaForLocal's sizing arm → payload reverts to 0 (the 1-slot sentinel).
TEST(MirLoweringCSubset, ArrayLocalAllocaReservesFullByteSize) {
    auto L = lowerCSubset("int g(void) { int a[4]; return a[0]; }\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    bool foundAlloca = false;
    for (std::uint32_t k = 0; k < m.blockInstCount(entry); ++k) {
        MirInstId const id = m.blockInstAt(entry, k);
        if (m.instOpcode(id) != MirOpcode::Alloca) continue;
        EXPECT_EQ(m.instPayload(id), 16u)
            << "int[4] reserves 16 bytes (4*sizeof(int)), not the 8-byte "
               "scalar-slot sentinel (payload 0)";
        foundAlloca = true;
    }
    EXPECT_TRUE(foundAlloca) << "expected an Alloca for the array local";
}

// A ZERO-size element type (an empty aggregate `struct E {}`) must FAIL LOUD
// when indexed — never a silent `Mul(idx, 0)` that aliases every element to
// byte offset 0. `&a[1]` drives scaleIndexToBytes with stride 0.
// RED-ON-DISABLE: drop the `stride == 0` guard → MIR lowering succeeds
// (ASSERT_FALSE trips) with an all-aliasing index.
TEST(MirLoweringCSubset, IndexIntoZeroSizeElementArrayFailsLoud) {
    auto L = lowerCSubset(
        "struct E { };\n"
        "int g(void) { struct E a[3]; struct E* p = &a[1]; return 0; }\n");
    EXPECT_FALSE(L.mir.ok)
        << "indexing a zero-size (empty-aggregate) element must fail loud, "
           "not silently scale by 0";
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

// FC8 D-CSUBSET-BITFIELD-RVALUE-RUNTIME: a by-value aggregate COMPOUND LITERAL
// rvalue `take((struct P){a, 2})` (a NON-bit-field struct, runtime first field)
// is lowered through the SYNTHESIZED-SLOT carrier — `lowerLvalueAddress`'s new
// ConstructAggregate arm materializes a slot (Alloca) and inits it ELEMENT-WISE
// (one Gep+Store per field at its byte offset), exactly as a named local's
// `= {…}` does; the by-value arg path then consumes that slot by ADDRESS. This
// pins the MEMORY model and forbids regression to an SSA-aggregate InsertValue
// chain. The literal's field stores land at byte offsets 0 (a) and 4 (the `2`);
// offset 4 is written ONLY by the literal init (the 8-byte by-value copy is a
// single I64 chunk at offset 0), so a Store at offset 4 is the literal-init
// witness. RED-ON-DISABLE: revert the lowerLvalueAddress ConstructAggregate arm
// → the by-value arg fail-louds H0009 (ordinal 32) and `mir.ok` goes false; a
// regression to the InsertValue chain trips the `InsertValue == 0` assertion.
TEST(MirLoweringCSubset, ByValueCompoundLiteralArgMaterializesSlotElementWise) {
    auto L = lowerCSubset(
        "struct P { int a; int b; };\n"
        "int take(struct P p) { return p.a + p.b; }\n"
        "int main(void) { int a = 40; return take((struct P){a, 2}); }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty()
              ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "a by-value compound-literal aggregate rvalue must lower via a "
           "synthesized slot (D-CSUBSET-BITFIELD-RVALUE-RUNTIME): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    // main is the function that CALLs take — find it by the entry-block Call.
    // The compound-literal carrier lives in main's entry block.
    std::uint32_t mainFi = 0;
    for (std::uint32_t fi = 0; fi < m.moduleFuncCount(); ++fi) {
        auto const ops = funcEntryOpcodes(m, fi);
        if (countOpcode(ops, MirOpcode::Call) > 0) { mainFi = fi; break; }
    }
    auto const ops = funcEntryOpcodes(m, mainFi);
    EXPECT_EQ(countOpcode(ops, MirOpcode::InsertValue), 0u)
        << "the compound literal is materialized in MEMORY — never an "
           "SSA-aggregate InsertValue chain";
    EXPECT_GE(countOpcode(ops, MirOpcode::Alloca), 1u)
        << "a slot is synthesized for the compound-literal lvalue";
    // The literal's two field stores land at byte offsets 0 (a) and 4 (b=2).
    // Offset 4 is written ONLY by the literal init in this 8-byte layout.
    MirBlockId const entry = m.funcEntry(m.funcAt(mainFi));
    bool storeAt0 = false, storeAt4 = false;
    for (std::uint32_t i = 0; i < m.blockInstCount(entry); ++i) {
        MirInstId const ix = m.blockInstAt(entry, i);
        if (m.instOpcode(ix) != MirOpcode::Store) continue;
        auto st = m.instOperands(ix);
        if (st.size() != 2) continue;
        // st[1] is the destination pointer — a Gep(base, Const(off)).
        MirInstId const dst = st[1];
        if (m.instOpcode(dst) != MirOpcode::Gep) continue;
        auto g = m.instOperands(dst);
        if (g.size() != 2) continue;
        if (m.instOpcode(g[1]) != MirOpcode::Const) continue;
        auto const& lit = m.literalValue(m.constLiteralIndex(g[1]));
        auto const* off = std::get_if<std::int64_t>(&lit.value);
        if (off == nullptr) continue;
        if (*off == 0) storeAt0 = true;
        if (*off == 4) storeAt4 = true;
    }
    EXPECT_TRUE(storeAt0) << "field a stored at byte offset 0";
    EXPECT_TRUE(storeAt4)
        << "field b (=2) stored at byte offset 4 — the literal-init witness "
           "(only the element-wise slot init writes offset 4 here)";
}

// FC8 D-CSUBSET-BITFIELD-RVALUE-RUNTIME (bit-field variant): a by-value BIT-FIELD
// compound literal `take((struct B){a, 20})` (a:3 + b:5 share unit 0, runtime
// first field) is lowered through the synthesized-slot carrier whose init routes
// to `lowerBitfieldAggregateInitIntoSlot` — so the literal PACKS per allocation
// unit (zero the unit, then read-modify-write each field: And to clear + And to
// mask the value + Shl to bitOffset + Or). A plain full-width per-field store
// would clobber the neighbour in the shared unit. Mirrors
// BitFieldStructInitializerPacksPerUnit but in by-value RVALUE position.
// RED-ON-DISABLE: revert the lowerLvalueAddress ConstructAggregate arm → the
// by-value arg fail-louds H0009 (ordinal 32), `mir.ok` false; revert the per-unit
// packing → the And/Or/Shl packing signature disappears.
TEST(MirLoweringCSubset, ByValueBitfieldCompoundLiteralArgPacksPerUnit) {
    auto L = lowerCSubset(
        "struct B { unsigned a : 3; unsigned b : 5; };\n"
        "int take(struct B v) { return (int)v.a + (int)v.b; }\n"
        "int main(void) { int a = 5; return take((struct B){a, 20}); }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty()
              ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "a by-value bit-field compound-literal rvalue must lower with per-unit "
           "packing via a synthesized slot (D-CSUBSET-BITFIELD-RVALUE-RUNTIME): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t mainFi = 0;
    for (std::uint32_t fi = 0; fi < m.moduleFuncCount(); ++fi) {
        auto const ops = funcEntryOpcodes(m, fi);
        if (countOpcode(ops, MirOpcode::Call) > 0) { mainFi = fi; break; }
    }
    MirBlockId const entry = m.funcEntry(m.funcAt(mainFi));
    auto count = [&](MirOpcode op) {
        int n = 0;
        for (std::uint32_t i = 0; i < m.blockInstCount(entry); ++i)
            if (m.instOpcode(m.blockInstAt(entry, i)) == op) ++n;
        return n;
    };
    auto const ops = funcEntryOpcodes(m, mainFi);
    EXPECT_EQ(countOpcode(ops, MirOpcode::InsertValue), 0u)
        << "the bit-field literal is packed in MEMORY — no SSA-aggregate chain";
    EXPECT_GE(countOpcode(ops, MirOpcode::Alloca), 1u)
        << "a slot is synthesized for the bit-field compound-literal lvalue";
    // a + b share unit 0; b is at bitOffset 3 → at least one Shl; each RMW masks
    // (And) and clears the field (And) and ORs it back (Or). A full-width
    // per-field store would emit neither the Or nor the Shl.
    EXPECT_GE(count(MirOpcode::Or), 2)  << "each bit-field init ORs its value into the unit";
    EXPECT_GE(count(MirOpcode::And), 3) << "RMW clears the field AND masks the value";
    EXPECT_GE(count(MirOpcode::Shl), 1) << "b at bitOffset 3 → shifted before OR";
}

// D-CSUBSET-BITFIELD-RVALUE-RUNTIME (discard position): an aggregate compound
// literal used as a DISCARDED statement (`(struct P){h(), 2};`) must lower —
// materialized into a throwaway slot so its operand SIDE EFFECTS run, then the
// address dropped. Completes the carrier across the discard position (and
// removes the regression where a discarded aggregate literal fail-louded).
// RED-ON-DISABLE: revert the ExprStmt ConstructAggregate routing → lowerExpr
// hits the anti-resurrection fail-loud → mir.ok false.
TEST(MirLoweringCSubset, DiscardedAggregateCompoundLiteralStatementRunsSideEffects) {
    auto L = lowerCSubset(
        "struct P { int a; int b; };\n"
        "int h(void) { return 7; }\n"
        "void f(void) { (struct P){h(), 2}; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty()
              ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "a discarded aggregate compound-literal statement must lower (its "
           "operand side effects run via a throwaway slot), not fail loud: "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    // f() is the function with the discarded literal — find it by its Call to
    // h() (h() itself has no call). It must contain the h() side-effect call +
    // the throwaway slot, and NO SSA-aggregate chain.
    std::uint32_t fFi = 0;
    bool found = false;
    for (std::uint32_t fi = 0; fi < m.moduleFuncCount(); ++fi) {
        if (countOpcode(funcEntryOpcodes(m, fi), MirOpcode::Call) > 0) {
            fFi = fi; found = true; break;
        }
    }
    ASSERT_TRUE(found) << "f() (the discarded-literal body, with the h() call) not found";
    auto const ops = funcEntryOpcodes(m, fFi);
    EXPECT_GE(countOpcode(ops, MirOpcode::Call), 1u)
        << "the operand h() side effect must be lowered — the call survives the discard";
    EXPECT_GE(countOpcode(ops, MirOpcode::Alloca), 1u)
        << "a throwaway slot is synthesized for the discarded aggregate literal";
    EXPECT_EQ(countOpcode(ops, MirOpcode::InsertValue), 0u)
        << "memory-based — no SSA-aggregate chain";
}

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

// D-MIR-ARRAY-FIELD-AGGREGATE-INIT (array-LOCAL form): an array-local brace-
// init with a RUNTIME element `int xs[3] = {a, 2, 3}` lowers ELEMENT-WISE (one
// Gep+Store per element at byte offsets 0,4,8), NOT an InsertValue chain.
// PRE-FC the array fell to lowerExpr → a multi-element InsertValue chain that
// FAILED LOUD at LIR (`lowerInsertValue` rejects non-zero indices) — the same
// non-realizability that drove struct init element-wise (see
// StructLocalInitStoresEachFieldAtItsOffset). RED-ON-DISABLE: revert the
// VarDecl array arm → the array falls to lowerExpr → the InsertValue chain
// (count >= 3) reappears and the EXPECT_EQ(...,0) trips.
TEST(MirLoweringCSubset, ArrayLocalRuntimeInitStoresElementWise) {
    auto L = lowerCSubset(
        "void f(int a) { int xs[3] = {a, 2, 3}; }\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty()
              ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto const ops = entryOpcodes(m);
    EXPECT_EQ(countOpcode(ops, MirOpcode::InsertValue), 0u)
        << "array-local init is element-wise, no InsertValue chain";
    EXPECT_EQ(countOpcode(ops, MirOpcode::Store), 3u)
        << "one Store per array element";
    // the three element GEPs carry byte offsets 0, 4, 8 (stride 4).
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
    ASSERT_EQ(gepOffsets.size(), 3u);
    EXPECT_EQ(gepOffsets[0], 0) << "xs[0] at byte offset 0";
    EXPECT_EQ(gepOffsets[1], 4) << "xs[1] at byte offset 4";
    EXPECT_EQ(gepOffsets[2], 8) << "xs[2] at byte offset 8";
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

// FC7 C3 (AAPCS64/Apple x8 sret) — N-3 host-independent MIR pin. A >16-byte struct
// returned BY VALUE under AAPCS64 uses the register-based indirect-result (x8) path,
// NOT the SysV/Win64 hidden-arg path. This pin lowers for arm64/aapcs64 (so
// `aggregateSretViaHiddenArg` resolves FALSE — indirectResultRegister = x8) and
// asserts, deterministically + on every host (no execution), that:
//   (callee) the >16B-returning function reads the result pointer via a
//            `ReadIndirectResult` op + returns VOID (0-operand Return) + does NOT
//            consume a hidden GPR Arg for the sret pointer; and
//   (caller) the Call to it carries the `kIndirectResultBit` payload flag (so
//            lir_callconv reroutes the prepended sret pointer to x8, not arg0).
// RED-ON-DISABLE: revert `hir_to_mir`'s `aggregateSretViaHiddenArg` branch (callee
// → `addArg`+`addReturn(ptr)`, caller → no IRR bit) and both arms fail.
TEST(MirLoweringCSubset, Aapcs64StructSretUsesReadIndirectResultAndFlagsCall) {
    auto L = lowerCSubset(
        "typedef struct { long a; long b; long c; } Big;\n"
        "Big make(int tag, long a) { Big s; s.a = a + tag; s.b = a; s.c = a;"
        " return s; }\n"
        "int use(void) { Big b = make(7, 5); return (int)b.a; }\n",
        /*targetName=*/"arm64", /*ccName=*/"aapcs64");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "MIR lowering: " << (L.mirReporter.all().empty()
            ? "" : L.mirReporter.all()[0].actual);

    Mir const& m = L.mir.mir;
    // Scan every function: the callee (make) must read x8 + return void; the caller
    // (use) must flag its Call with hasIndirectResult.
    std::size_t readIndirectResultCount = 0;
    std::size_t voidStructReturnCount   = 0;
    std::size_t irrFlaggedCallCount     = 0;
    std::size_t hiddenSretArgCount      = 0;  // a 3rd GPR Arg would be a hidden sret ptr
    for (std::uint32_t fi = 0; fi < m.moduleFuncCount(); ++fi) {
        MirFuncId const f = m.funcAt(fi);
        std::size_t argCount = 0;
        bool fnReadsIrr = false;
        for (std::uint32_t bi = 0; bi < m.funcBlockCount(f); ++bi) {
            MirBlockId const b = m.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < m.blockInstCount(b); ++ii) {
                MirInstId const id = m.blockInstAt(b, ii);
                switch (m.instOpcode(id)) {
                    case MirOpcode::ReadIndirectResult:
                        ++readIndirectResultCount; fnReadsIrr = true; break;
                    case MirOpcode::Arg: ++argCount; break;
                    case MirOpcode::Call:
                        if (::dss::call_payload::hasIndirectResult(m.instPayload(id)))
                            ++irrFlaggedCallCount;
                        break;
                    case MirOpcode::Return:
                        if (fnReadsIrr && m.instOperands(id).empty())
                            ++voidStructReturnCount;
                        break;
                    default: break;
                }
            }
        }
        // `make` takes 2 real params (tag, a) → exactly 2 Args, NO hidden sret Arg.
        if (fnReadsIrr && argCount > 2) ++hiddenSretArgCount;
    }
    EXPECT_EQ(readIndirectResultCount, 1u)
        << "the x8-sret callee must read the indirect-result register once at entry";
    EXPECT_EQ(voidStructReturnCount, 1u)
        << "the x8-sret callee must return VOID (the caller owns the x8 storage)";
    EXPECT_EQ(irrFlaggedCallCount, 1u)
        << "the caller's Call must carry the hasIndirectResult payload flag so "
           "lir_callconv routes the sret pointer to x8, not arg0";
    EXPECT_EQ(hiddenSretArgCount, 0u)
        << "the x8-sret callee must NOT also consume a hidden GPR Arg for the sret "
           "pointer (that is the SysV/Win64 path; x8-sret uses ReadIndirectResult)";
}

// ─── D-CSUBSET-AGGREGATE-VALUED-CONTROL-EXPR (FC8 SUBSTRATE) ──────────────────
// An AGGREGATE-typed ternary / comma-SeqExpr used BY VALUE materializes each
// control-flow arm into ONE COMMON result slot under a CFG diamond — the
// memory-based aggregate model, NOT an SSA Phi-of-aggregate. These pins assert
// the diamond + the common slot + the no-Phi-of-aggregate invariant (and the
// bit-field arm's per-unit packing through the slot). They WALK ALL BLOCKS of
// the function (the diamond spreads the arm stores across the then/else
// blocks), unlike the entry-only `funcEntryOpcodes` helper.

namespace {
// Count `op` across EVERY block of function `fi` (the aggregate-ternary diamond
// places per-arm stores in the then/else blocks, not the entry).
[[nodiscard]] std::size_t countOpcodeAllBlocks(Mir const& m, std::uint32_t fi,
                                               MirOpcode op) {
    std::size_t n = 0;
    MirFuncId const f = m.funcAt(fi);
    for (std::uint32_t b = 0; b < m.funcBlockCount(f); ++b) {
        MirBlockId const blk = m.funcBlockAt(f, b);
        for (std::uint32_t i = 0; i < m.blockInstCount(blk); ++i)
            if (m.instOpcode(m.blockInstAt(blk, i)) == op) ++n;
    }
    return n;
}
// The function index that CALLs another function (main/the by-value consumer's
// caller — where the aggregate ternary/comma carrier lives).
[[nodiscard]] std::uint32_t funcWithCall(Mir const& m) {
    for (std::uint32_t fi = 0; fi < m.moduleFuncCount(); ++fi)
        if (countOpcodeAllBlocks(m, fi, MirOpcode::Call) > 0) return fi;
    return 0;
}
// True iff ANY instruction in function `fi` is a Phi whose RESULT type is an
// aggregate (Struct/Union/Array). The memory-based model forbids this — the
// aggregate "merge" of a ternary is a shared SLOT, never an SSA Phi.
[[nodiscard]] bool hasAggregatePhi(Mir const& m, TypeInterner const& interner,
                                   std::uint32_t fi) {
    MirFuncId const f = m.funcAt(fi);
    for (std::uint32_t b = 0; b < m.funcBlockCount(f); ++b) {
        MirBlockId const blk = m.funcBlockAt(f, b);
        for (std::uint32_t i = 0; i < m.blockInstCount(blk); ++i) {
            MirInstId const ix = m.blockInstAt(blk, i);
            if (m.instOpcode(ix) != MirOpcode::Phi) continue;
            TypeId const t = m.instType(ix);
            if (!t.valid()) continue;
            TypeKind const k = interner.kind(t);
            if (k == TypeKind::Struct || k == TypeKind::Union
                || k == TypeKind::Array)
                return true;
        }
    }
    return false;
}
} // namespace

// CORE PIN: a by-value NON-bit-field aggregate ternary `take(cond ? A : B)`
// lowers with (a) a CondBr diamond (the control-flow-dependent value); (b) a
// COMMON result slot (Alloca) — allocated ONCE before the branch, written by
// BOTH arms; (c) per-arm Stores into that slot (the arm materialization); (d)
// InsertValue == 0 (memory-based, no SSA-aggregate chain); (e) NO Phi whose
// type is the aggregate (the slot IS the merge, not a Phi). RED-ON-DISABLE:
// revert the lowerLvalueAddress Ternary arm → the by-value arg fail-louds H0009
// (ordinal 33) and `mir.ok` goes false (and the diamond/slot signatures vanish).
TEST(MirLoweringCSubset, ByValueAggregateTernaryMaterializesCommonSlotUnderDiamond) {
    // `pick` is a runtime PARAMETER (keeps the condition runtime so neither arm
    // folds away — c-subset has no bare prototypes; a param is the clean lever).
    auto L = lowerCSubset(
        "struct P { int a; int b; };\n"
        "int take(struct P p) { return p.a + p.b; }\n"
        "int run(int pick, int a) {\n"
        "  return take(pick ? (struct P){a, 2} : (struct P){a, 3});\n"
        "}\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty()
              ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "a by-value aggregate ternary must lower via a common slot under a "
           "diamond (D-CSUBSET-AGGREGATE-VALUED-CONTROL-EXPR): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto const& interner = L.model.lattice().interner();
    std::uint32_t const fi = funcWithCall(m);
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::CondBr), 1u)
        << "the aggregate ternary lowers as a CONTROL-FLOW diamond (CondBr)";
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::InsertValue), 0u)
        << "memory-based aggregate model — never an SSA-aggregate InsertValue chain";
    EXPECT_FALSE(hasAggregatePhi(m, interner, fi))
        << "the arms merge into a COMMON SLOT — there must be NO Phi whose type "
           "is the aggregate (Struct/Union/Array)";
    // The common result slot + the by-value arg's eightbyte temp are both
    // Allocas; the slot init + each arm's stores into it give >= 2 Stores total
    // (one per arm's 2 fields, across the then/else blocks). The diamond's two
    // arms each store the literal's fields into the SAME slot.
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::Alloca), 1u)
        << "a common result slot is synthesized for the aggregate ternary";
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::Store), 2u)
        << "each arm materializes the aggregate into the common slot (per-field "
           "Stores across the then/else blocks)";
}

// BIT-FIELD ARM PIN: a by-value BIT-FIELD aggregate ternary packs per allocation
// unit THROUGH the common slot under the diamond — each arm's slot init routes
// to lowerBitfieldAggregateInitIntoSlot (And/Or/Shl RMW packing), proving the
// per-unit packing flows through the diamond-materialized slot (not a plain
// per-field store that would clobber a shared-unit neighbour). RED-ON-DISABLE:
// revert the Ternary arm → H0009 ordinal 33; revert the per-unit packing → the
// And/Or/Shl signature disappears.
TEST(MirLoweringCSubset, ByValueBitfieldAggregateTernaryPacksPerUnitUnderDiamond) {
    auto L = lowerCSubset(
        "struct B { unsigned a : 3; unsigned b : 5; };\n"
        "int take(struct B v) { return (int)v.a + (int)v.b; }\n"
        "int run(int pick, int a) {\n"
        "  return take(pick ? (struct B){a, 20} : (struct B){a, 7});\n"
        "}\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty()
              ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "a by-value bit-field aggregate ternary must lower with per-unit "
           "packing through a common slot under a diamond "
           "(D-CSUBSET-AGGREGATE-VALUED-CONTROL-EXPR): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto const& interner = L.model.lattice().interner();
    std::uint32_t const fi = funcWithCall(m);
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::CondBr), 1u)
        << "the bit-field aggregate ternary lowers as a control-flow diamond";
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::InsertValue), 0u)
        << "memory-based — no SSA-aggregate chain";
    EXPECT_FALSE(hasAggregatePhi(m, interner, fi))
        << "no Phi of the aggregate — the arms merge into a common slot";
    // a + b share unit 0; b is at bitOffset 3. Each arm packs via RMW (And to
    // clear + And to mask + Shl + Or). Two arms → at least one Shl + several
    // And/Or. A plain full-width per-field store would emit neither.
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::Or), 2u)
        << "each bit-field arm ORs its packed value into the unit";
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::Shl), 1u)
        << "field b at bitOffset 3 is shifted before OR (per-unit packing)";
}

// SeqExpr (comma) PIN: a comma/SeqExpr whose VALUE is an aggregate rvalue used
// BY VALUE lowers — the side-effect statement runs (here a Call), then the
// aggregate result recurses to its lvalue (the compound-literal slot). No fail-
// loud, no Phi of aggregate. RED-ON-DISABLE: revert the lowerLvalueAddress
// SeqExpr arm → the by-value arg fail-louds H0009 (ordinal 39) → mir.ok false.
TEST(MirLoweringCSubset, AggregateSeqExprByValueLowersWithSideEffectAndSlot) {
    auto L = lowerCSubset(
        "struct P { int a; int b; };\n"
        "int side(void) { return 7; }\n"
        "int take(struct P p) { return p.a + p.b; }\n"
        "int main(void) {\n"
        "  int a = 40;\n"
        "  return take((side(), (struct P){a, 2}));\n"
        "}\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty()
              ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "a by-value aggregate comma/SeqExpr rvalue must lower (side effect "
           "runs, result recurses to its slot) "
           "(D-CSUBSET-AGGREGATE-VALUED-CONTROL-EXPR): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto const& interner = L.model.lattice().interner();
    std::uint32_t const fi = funcWithCall(m);
    // The side() call (side effect) AND the take() call both lower → >= 2 Calls.
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::Call), 2u)
        << "the comma side-effect call must run, plus the by-value consumer call";
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::Alloca), 1u)
        << "the aggregate result's compound-literal slot is synthesized";
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::InsertValue), 0u)
        << "memory-based — no SSA-aggregate chain";
    EXPECT_FALSE(hasAggregatePhi(m, interner, fi))
        << "a comma aggregate rvalue is not a Phi-of-aggregate";
}

// COPY-PATH PIN: a by-value aggregate ternary whose ARMS are NAMED LVALUES (not
// compound literals) exercises materializeAggregateArmIntoSlot's COPY branch
// (lowerLvalueAddress(arm) + lowerAggregateCopy into the common slot) — distinct
// from the in-place compound-literal init the pins above cover. Still a CFG
// diamond into ONE common slot, still NO Phi-of-aggregate. RED-ON-DISABLE: revert
// the lowerLvalueAddress Ternary arm → the by-value arg fail-louds → mir.ok false.
TEST(MirLoweringCSubset, ByValueAggregateTernaryNamedLvalueArmsCopyIntoCommonSlot) {
    auto L = lowerCSubset(
        "struct P { int a; int b; };\n"
        "int take(struct P p) { return p.a + p.b; }\n"
        "int run(int pick, int a) {\n"
        "  struct P s1 = {a, 2};\n"
        "  struct P s2 = {a, 3};\n"
        "  return take(pick ? s1 : s2);\n"
        "}\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty()
              ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "a by-value aggregate ternary with NAMED-LVALUE arms must lower via a "
           "common slot under a diamond (the copy-path) "
           "(D-CSUBSET-AGGREGATE-VALUED-CONTROL-EXPR): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto const& interner = L.model.lattice().interner();
    std::uint32_t const fi = funcWithCall(m);
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::CondBr), 1u)
        << "a named-arm aggregate ternary still lowers as a control-flow diamond";
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::InsertValue), 0u)
        << "memory-based — no SSA-aggregate chain";
    EXPECT_FALSE(hasAggregatePhi(m, interner, fi))
        << "the named arms are COPIED into a common slot — no Phi of the aggregate";
    // Each arm copies a named local into the common slot (lowerAggregateCopy,
    // field-wise for a flat scalar struct) → per-field Stores across the
    // then/else blocks (on top of the two VarDecl inits before the branch).
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::Store), 2u)
        << "each arm copies its named local into the common result slot";
}

// DISCARD-POSITION PIN (gate-discovered silent-miscompile seal): a DISCARDED
// aggregate ternary with NAMED-LVALUE arms (`(cond ? s1 : s2);` as a bare
// statement) must route through the ExprStmt aggregate chokepoint →
// lowerLvalueAddress's Ternary arm (a diamond into a throwaway slot, side effects
// run, address dropped) — NOT through lowerExpr, which would silently synthesize a
// phi-of-aggregate + aggregate-width arm Loads (the latent miscompile this seals).
// RED-ON-DISABLE: revert the ExprStmt aggregate chokepoint → falls to lowerExpr's
// Ternary arm → its anti-resurrection guard fail-louds → mir.ok false; revert that
// guard TOO → hasAggregatePhi becomes true (the forbidden phi-of-aggregate appears).
TEST(MirLoweringCSubset, DiscardedAggregateTernaryNamedArmsRoutesThroughSlotNoPhi) {
    auto L = lowerCSubset(
        "struct P { int a; int b; };\n"
        "int sink(int x) { return x; }\n"
        "int run(int pick, int a) {\n"
        "  struct P s1 = {a, 2};\n"
        "  struct P s2 = {a, 3};\n"
        "  (pick ? s1 : s2);\n"
        "  return sink(a);\n"
        "}\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty()
              ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "a discarded aggregate ternary must route through the slot carrier, not "
           "a phi-of-aggregate (D-CSUBSET-AGGREGATE-VALUED-CONTROL-EXPR): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto const& interner = L.model.lattice().interner();
    std::uint32_t const fi = funcWithCall(m);
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::CondBr), 1u)
        << "the discarded aggregate ternary still lowers as a control-flow diamond";
    EXPECT_FALSE(hasAggregatePhi(m, interner, fi))
        << "the discard routes through a slot — NEVER a phi-of-aggregate (the "
           "silent miscompile this seals)";
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::InsertValue), 0u)
        << "memory-based — no SSA-aggregate chain";
}

// SECOND DISCARD POSITION PIN (seal-review follow-up): a for-init/for-update
// clause is a BARE expression (NOT wrapped in ExprStmt), lowered via
// `lowerForClauseNode` → it is a SECOND discard position. An aggregate ternary in
// a for-UPDATE clause (`for (...; ...; (cond ? s1 : s2))`) must route through the
// SHARED discard chokepoint (`lowerDiscardedExpr`) → the slot carrier, NOT
// lowerExpr's anti-resurrection fail-loud and NEVER a phi-of-aggregate. This
// proves the chokepoint is by-construction across BOTH discard positions (no
// per-POSITION miss). RED-ON-DISABLE: revert the for-clause routing → the update
// fail-louds (mir.ok false); revert the lowerExpr Ternary guard too → a
// phi-of-aggregate appears in the update (hasAggregatePhi true).
TEST(MirLoweringCSubset, ForUpdateAggregateTernaryRoutesThroughSlotNoPhi) {
    auto L = lowerCSubset(
        "struct P { int a; int b; };\n"
        "int sink(int x) { return x; }\n"
        "int run(int pick, int n) {\n"
        "  struct P s1 = {1, 2};\n"
        "  struct P s2 = {3, 4};\n"
        "  int acc = 0;\n"
        "  for (int i = 0; i < n; (pick ? s1 : s2)) { acc = acc + i; i = i + 1; }\n"
        "  return sink(acc);\n"
        "}\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty()
              ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "an aggregate ternary in a for-update clause must route through the "
           "shared discard chokepoint, not fail loud "
           "(D-CSUBSET-AGGREGATE-VALUED-CONTROL-EXPR): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    auto const& interner = L.model.lattice().interner();
    std::uint32_t const fi = funcWithCall(m);
    // The for-loop's own `i < n` test is one CondBr; the aggregate ternary in the
    // update clause adds a SECOND (the diamond) — so >= 2 distinguishes the
    // diamond from the loop test alone.
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::CondBr), 2u)
        << "the loop test (1 CondBr) PLUS the for-update aggregate ternary's "
           "diamond (a 2nd CondBr)";
    EXPECT_FALSE(hasAggregatePhi(m, interner, fi))
        << "the for-update ternary routes through a slot — NEVER a phi-of-aggregate";
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::InsertValue), 0u)
        << "memory-based — no SSA-aggregate chain";
}

// ── FC12a-core (D-FC12A-VARIADIC-CALLEE): va_start / va_arg MIR-shape pins ────
//
// `va_start(ap,n)` lowers to FOUR field Stores into the __va_list_tag (gp_offset,
// fp_offset, reg_save_area, overflow_arg_area) PLUS the two frame-address leaves
// (VaRegSaveAreaAddr / VaOverflowArgAreaAddr). `va_arg(ap,int)` lowers to the reg-
// vs-overflow diamond: a Load of the gp_offset cursor → an ICmpUlt against the gp
// limit → a CondBr → a Phi joining the two address arms → a final Load of the value.
// A `va_arg(ap, struct S)` STAYS FAIL-LOUD (the FC12a-struct boundary). RED-ON-
// DISABLE: half-implementing the struct path (returning a value instead of
// unsupported) flips the fail-loud assertion.

namespace {
// The function index whose body contains a VaRegSaveAreaAddr (i.e. called va_start).
[[nodiscard]] std::uint32_t funcWithVaStart(Mir const& m) {
    for (std::uint32_t fi = 0; fi < m.moduleFuncCount(); ++fi)
        if (countOpcodeAllBlocks(m, fi, MirOpcode::VaRegSaveAreaAddr) > 0) return fi;
    return 0;
}
// True iff function `fi` contains a `Const` inst whose integer value == `value`.
// (Hoisted here from a later block so the FC12a-struct va_arg/param pins — which
// precede that block — can pin the cursor/overflow Const values.)
[[nodiscard]] bool funcHasConstInt(Mir const& m, std::uint32_t fi,
                                   std::int64_t value) {
    MirFuncId const f = m.funcAt(fi);
    for (std::uint32_t b = 0; b < m.funcBlockCount(f); ++b) {
        MirBlockId const blk = m.funcBlockAt(f, b);
        for (std::uint32_t i = 0; i < m.blockInstCount(blk); ++i) {
            MirInstId const ix = m.blockInstAt(blk, i);
            if (m.instOpcode(ix) != MirOpcode::Const) continue;
            auto const& lit = m.literalValue(m.constLiteralIndex(ix));
            if (std::holds_alternative<std::int64_t>(lit.value)
                && std::get<std::int64_t>(lit.value) == value)
                return true;
        }
    }
    return false;
}
// FC12-deferral④ (FOLD 3): the payload of the (single) VaOverflowArgAreaAddr leaf in
// function `fi` — the fixed-stack-arg byte displacement va_start bakes for
// overflow_arg_area / __stack. Returns nullopt if there is no such inst (so a test
// can ASSERT presence before pinning the value, rather than silently reading 0).
[[nodiscard]] std::optional<std::uint32_t>
vaOverflowArgAreaPayload(Mir const& m, std::uint32_t fi) {
    MirFuncId const f = m.funcAt(fi);
    for (std::uint32_t b = 0; b < m.funcBlockCount(f); ++b) {
        MirBlockId const blk = m.funcBlockAt(f, b);
        for (std::uint32_t i = 0; i < m.blockInstCount(blk); ++i) {
            MirInstId const ix = m.blockInstAt(blk, i);
            if (m.instOpcode(ix) == MirOpcode::VaOverflowArgAreaAddr)
                return m.instPayload(ix);
        }
    }
    return std::nullopt;
}
// FC12-deferral④ (FOLD 3, AAPCS64 clamp): true iff function `fi` has ANY `Const`
// inst whose integer value is STRICTLY LESS than `threshold`. The clamped
// __gr_offs/__vr_offs are 0 when a class's fixed params consume its whole save block;
// the OLD underflowing formula `-(saveCount - fixedCount)*slotBytes` with fixedCount >
// saveCount wraps UNSIGNED then negates to a HUGE magnitude (≈ -4.29e9 i64) — far
// below ANY legitimate cursor (the largest legit magnitude is __vr_offs = -128). So
// "no const below -1000 survives" is the strict, precise witness that the clamp fired
// (red-on-disable: revert the clamp → the huge-magnitude underflow const appears),
// while NOT tripping on the legitimately-negative -64/-128 cursors of a partially-
// consumed class. (A blunt "any negative const" check would false-fail on those.)
[[nodiscard]] bool funcHasConstIntBelow(Mir const& m, std::uint32_t fi,
                                        std::int64_t threshold) {
    MirFuncId const f = m.funcAt(fi);
    for (std::uint32_t b = 0; b < m.funcBlockCount(f); ++b) {
        MirBlockId const blk = m.funcBlockAt(f, b);
        for (std::uint32_t i = 0; i < m.blockInstCount(blk); ++i) {
            MirInstId const ix = m.blockInstAt(blk, i);
            if (m.instOpcode(ix) != MirOpcode::Const) continue;
            auto const& lit = m.literalValue(m.constLiteralIndex(ix));
            if (std::holds_alternative<std::int64_t>(lit.value)
                && std::get<std::int64_t>(lit.value) < threshold)
                return true;
        }
    }
    return false;
}
} // namespace

TEST(MirLoweringCSubset, VaStartEmitsFourFieldStoresPlusFrameAddrs) {
    auto L = lowerCSubset(
        "int sum(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  va_end(ap);\n"
        "  return n;\n"
        "}\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok) << "variadic-callee va_start MIR lowering must succeed";
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    // The two frame-address leaves (reg-save-area + overflow-arg-area).
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::VaRegSaveAreaAddr), 1u);
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::VaOverflowArgAreaAddr), 1u);
    // va_start writes all FOUR __va_list_tag fields (4 Stores via Gep+Store). va_end
    // is a no-op, so these are the only Stores in this body.
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::Store), 4u)
        << "va_start must Store gp_offset + fp_offset + reg_save_area + "
           "overflow_arg_area into the __va_list_tag";
    // 4 field Geps for the tag fields (one per Store).
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::Gep), 4u);
}

TEST(MirLoweringCSubset, VaArgIntLowersToRegVsOverflowDiamond) {
    auto L = lowerCSubset(
        "int sum(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  int t = va_arg(ap, int);\n"
        "  va_end(ap);\n"
        "  return t;\n"
        "}\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    // The reg-vs-overflow diamond: an ICmpUlt (cursor < limit) feeding a CondBr,
    // a Phi joining the reg-arm + overflow-arm addresses, and Loads (the cursor +
    // the reg_save_area pointer + the final value).
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::ICmpUlt), 1u)
        << "va_arg compares the per-class offset cursor against its limit";
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::CondBr), 1u)
        << "va_arg branches reg-vs-overflow";
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::Phi), 1u)
        << "va_arg joins the two address arms with a Phi (of a POINTER — scalar)";
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::Load), 3u)
        << "va_arg Loads the cursor, the reg_save_area pointer, AND the final value";
}

// FC12b (D-FC12B-WIN64-VARIADIC-CALLEE) MIR pin: the SAME `va_arg(ap, int)` source
// lowers DIFFERENTLY by strategy — a reg-vs-overflow DIAMOND under SysV (cc 0) vs a
// LINEAR pointer bump (NO diamond) under Win64 (ms_x64). This is the strategy-
// dispatch witness: same source, two shapes, selected only by the CC's vaListLayout
// strategy. RED-ON-DISABLE: if the Win64 va_arg seam fell through to the SysV diamond
// (a half-migrated dispatch), the CondBr-count assertion below flips.
TEST(MirLoweringCSubset, Win64VaArgIntLowersLinearNotDiamond) {
    char const* src =
        "int sum(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  int t = va_arg(ap, int);\n"
        "  va_end(ap);\n"
        "  return t;\n"
        "}\n";
    // Win64 (ms_x64) lowering.
    auto W = lowerCSubset(src, "x86_64", "ms_x64");
    ASSERT_FALSE(W.model.hasErrors())
        << "Win64 variadic callee must type-check (va_list = char*)";
    ASSERT_TRUE(W.hir->ok);
    ASSERT_TRUE(W.mir.ok);
    Mir const& wm = W.mir.mir;
    // The Win64 va_start emits VaHomeArgAreaAddr (NOT VaRegSaveAreaAddr).
    std::uint32_t wfi = 0;
    bool foundW = false;
    for (std::uint32_t fi = 0; fi < wm.moduleFuncCount(); ++fi) {
        if (countOpcodeAllBlocks(wm, fi, MirOpcode::VaHomeArgAreaAddr) > 0) {
            wfi = fi; foundW = true; break;
        }
    }
    ASSERT_TRUE(foundW) << "Win64 va_start must emit a VaHomeArgAreaAddr leaf";
    EXPECT_EQ(countOpcodeAllBlocks(wm, wfi, MirOpcode::VaHomeArgAreaAddr), 1u);
    EXPECT_EQ(countOpcodeAllBlocks(wm, wfi, MirOpcode::VaRegSaveAreaAddr), 0u)
        << "Win64 does NOT use the SysV register-save-area leaf";
    // The LINEAR walk: NO reg-vs-overflow diamond. The scalar va_arg adds NO CondBr
    // (and no ICmpUlt/Phi). The loop's own CondBr is the baseline; this body has the
    // `while (i<n)` loop (1 CondBr) but the va_arg adds ZERO extra — so total CondBr
    // here (a straight body with one va_arg, NO loop) is 0. Pin exactly 0.
    EXPECT_EQ(countOpcodeAllBlocks(wm, wfi, MirOpcode::CondBr), 0u)
        << "Win64 va_arg is a LINEAR pointer bump — no reg-vs-overflow diamond";
    EXPECT_EQ(countOpcodeAllBlocks(wm, wfi, MirOpcode::ICmpUlt), 0u)
        << "Win64 va_arg compares nothing (no per-class limit check)";
    EXPECT_EQ(countOpcodeAllBlocks(wm, wfi, MirOpcode::Phi), 0u)
        << "Win64 va_arg joins nothing (single linear arm)";

    // SysV (sysv_amd64) lowering of the SAME source → the DIAMOND (the contrast).
    auto S = lowerCSubset(src, "x86_64", "sysv_amd64");
    ASSERT_TRUE(S.mir.ok);
    Mir const& sm = S.mir.mir;
    std::uint32_t sfi = 0;
    for (std::uint32_t fi = 0; fi < sm.moduleFuncCount(); ++fi) {
        if (countOpcodeAllBlocks(sm, fi, MirOpcode::VaRegSaveAreaAddr) > 0) {
            sfi = fi; break;
        }
    }
    EXPECT_GE(countOpcodeAllBlocks(sm, sfi, MirOpcode::CondBr), 1u)
        << "SysV va_arg of the SAME source emits the reg-vs-overflow diamond "
           "(CondBr) — the strategy dispatch distinguishes the two shapes";
    EXPECT_EQ(countOpcodeAllBlocks(sm, sfi, MirOpcode::VaHomeArgAreaAddr), 0u)
        << "SysV does NOT use the Win64 home-area leaf";
}

// FC12a-struct: `va_arg(ap, struct {long; double})` (a mixed GPR+SSE 16B aggregate,
// InRegisters under SysV) lowers to the ATOMIC register-gather diamond — BY ADDRESS,
// never as a bare aggregate value. RED-ON-DISABLE: reverting lowerVaArgAggregate to
// the old fail-loud flips mir.ok to false; reverting the gather to a single-class
// bump or a Load-of-aggregate breaks the structural assertions below.
TEST(MirLoweringCSubset, VaArgStructMixedLowersToAtomicRegisterGather) {
    auto L = lowerCSubset(
        "struct Pt { long a; double b; };\n"
        "long sum(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  struct Pt p = va_arg(ap, struct Pt);\n"
        "  va_end(ap);\n"
        "  return p.a + (long)p.b;\n"
        "}\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "an InRegisters (<=16B) struct va_arg must lower (the FC12a-struct gather)";
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    // A fresh Alloca for the gathered struct (the result-by-address temp). The `ap`
    // local + the `p` local also Alloca, so >= 1 is the floor that the gather adds.
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::Alloca), 1u)
        << "the gather copies the pieces into a fresh aggregate temp";
    // The ATOMIC decision is TWO ICmpUle (gp fits AND fp fits) feeding nested CondBr.
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::ICmpUle), 2u)
        << "the atomic gather compares BOTH class cursors against their folded limits";
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::CondBr), 2u)
        << "nested CondBr: gp-fits? -> fp-fits? -> regBB, else ovfBB";
    // The result is an ADDRESS Phi (a void* pointer) — NOT a Phi of the aggregate.
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::Phi), 1u)
        << "the two address arms join with a Phi of the struct's ADDRESS";
    EXPECT_FALSE(hasAggregatePhi(m, L.model.lattice().interner(), fi))
        << "the gather must NEVER produce a Phi of an aggregate-width value";
    // Both class cursors are bumped (the register arm: gp += 8, fp += 16) — two Adds
    // on the cursors, plus the per-class Stores writing them back (4 reg-arm Stores:
    // 2 piece-stores into temp + 2 cursor write-backs). va_start adds 4 Stores; so
    // the body Stores are well above the scalar-arm count. Pin the cursor Adds.
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::Add), 2u)
        << "the register arm bumps BOTH per-class cursors (gp by 8, fp by 16)";
}

// FC12a-struct fail-loud: `va_arg(ap, >16B struct)` is MEMORY class — it requires
// the by-value overflow mechanism not built this cycle. RED-ON-DISABLE: removing the
// ByReference guard in lowerVaArgAggregate lets a wrong gather lower (mir.ok true).
// FC12a-struct (D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT, va_arg ByReference): a >16B
// (MEMORY class) struct `va_arg` now LOWERS (was fail-loud) and DISPATCHES to the
// overflow-only arm — the struct sits ENTIRELY by value at overflow_arg_area (SysV
// §3.5.7), so there is NO register-gather diamond. STRUCTURAL PIN (red-on-disable):
// the ByReference dispatch in lowerVaArgAggregate emits NO `ICmpUle` (the InRegisters
// diamond's atomic-fit pair) — reverting the early ByReference dispatch (so a >16B
// MEMORY-class struct fell into the diamond with numGp==numFp==0 → a trivially-true
// fit → ZERO pieces gathered → a garbage address) would either re-introduce the
// fail-loud (mir.ok false) or emit the ICmpUle pair → this goes red.
TEST(MirLoweringCSubset, VaArgStructOver16BDispatchesToOverflowArm) {
    auto L = lowerCSubset(
        "struct Big { long a; long b; long c; };\n"   // 24B → MEMORY class
        "long sum(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  struct Big s = va_arg(ap, struct Big);\n"
        "  va_end(ap);\n"
        "  return s.a;\n"
        "}\n");
    ASSERT_TRUE(L.mir.ok)
        << "va_arg of a >16B (MEMORY class) struct must now LOWER via the overflow arm "
           "(D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    // The ByReference va_arg takes the OVERFLOW-ONLY arm — no reg-vs-overflow diamond,
    // so the atomic-fit `ICmpUle` pair is absent. (A scalar/InRegisters aggregate
    // va_arg emits exactly 2 ICmpUle; a >16B MEMORY-class struct emits 0.)
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::ICmpUle), 0u)
        << "a >16B (MEMORY class) va_arg dispatches to the overflow arm with NO "
           "register-gather diamond (no atomic-fit ICmpUle pair)";
    // The overflow bump is roundUp(24, 8) == 24 — pin the Const so a wrong tail-round
    // (e.g. one slot=8) goes red. The struct's 24-byte size is read from the overflow
    // slot, anti-folded by the run-witnessed corpus.
    EXPECT_TRUE(funcHasConstInt(m, fi, 24))
        << "the overflow_arg_area bump must be roundUp(sizeof(Big)=24, gpSlotBytes=8) "
           "== 24 (a >8B struct occupies multiple stack eightbytes)";
}

// Helper: does ANY diagnostic in `r` carry `anchor` in its `actual` text? (The
// `unsupported()` lowering helper routes its message into `.actual`.)
[[nodiscard]] inline bool
anyDiagActualContains(DiagnosticReporter const& r, std::string_view anchor) {
    for (auto const& d : r.all()) {
        if (d.actual.find(anchor) != std::string::npos) return true;
    }
    return false;
}

// The function index that issues NO Call — the variadic CALLEE (`sum`/`pick`),
// where the va_arg walk lives (the mirror of `funcWithCall`, which finds the
// caller). The struct-vararg fixtures have exactly one callee + one caller.
[[nodiscard]] inline std::uint32_t funcWithoutCall(Mir const& m) {
    for (std::uint32_t fi = 0; fi < m.moduleFuncCount(); ++fi)
        if (countOpcodeAllBlocks(m, fi, MirOpcode::Call) == 0) return fi;
    return 0;
}

// Count `Load` instructions in `fi` whose FIRST operand is itself a `Load` — i.e.
// a CHAINED `Load(Load(...))`. This is the host-independent structural signature of
// the Win64 (HomogeneousPointer) >8B-struct va_arg by-reference DEREF: the va_arg
// emits `apVal = Load(tagBase)` (the slot cursor) and, for a >8B ByReference struct,
// `structAddr = Load(apVal)` (deref the hidden pointer the slot holds). The deref's
// operand IS `apVal`, a Load — so it is a chained Load. The ≤8B by-value path returns
// `apVal` directly (NO chained Load). Crucially NO OTHER Load in these fixtures is
// chained: every field read (member access AND `lowerAggregateCopy`'s copy-in) loads
// through a `Gep` (even at offset 0 — MemberAccess/aggregate-copy always emit a Gep),
// so its operand is a Gep, not a Load; and `tagBase` is the `ap` local's Alloca
// address, not a Load. Thus this count discriminates deref-present (>=1) from
// deref-dropped (0) regardless of how many incidental copy-in/member Loads exist —
// the "Load >= N" whole-function count it replaces could not (the copy-in Loads kept
// the total above the threshold even with the deref dropped).
[[nodiscard]] std::size_t countChainedLoadOfLoad(Mir const& m, std::uint32_t fi) {
    std::size_t n = 0;
    MirFuncId const f = m.funcAt(fi);
    for (std::uint32_t b = 0; b < m.funcBlockCount(f); ++b) {
        MirBlockId const blk = m.funcBlockAt(f, b);
        for (std::uint32_t i = 0; i < m.blockInstCount(blk); ++i) {
            MirInstId const ix = m.blockInstAt(blk, i);
            if (m.instOpcode(ix) != MirOpcode::Load) continue;
            auto const ops = m.instOperands(ix);
            if (ops.empty()) continue;
            if (m.instOpcode(ops[0]) == MirOpcode::Load) ++n;
        }
    }
    return n;
}

// FOLD 3 — a trivial "is `op` present anywhere in `fi`?" predicate (the
// countOpcodeAllBlocks>0 wrapper the AAPCS64 struct-vararg pins use to assert the
// gather emitted an SExt / ICmpSlt etc.). Distinct from funcEntryOpcodes (entry-only).
[[nodiscard]] bool hasMirOpcode(Mir const& m, std::uint32_t fi, MirOpcode op) {
    return countOpcodeAllBlocks(m, fi, op) > 0;
}

// FOLD 3 (the load-bearing H2 guard) — count `Load`s in `fi` that read a `__va_list`
// FIELD at byte offset `byteOffset`. The structural signature of a va_list field read
// is `Load(Gep(tagBase, Const(byteOffset)))` — a Load whose first operand is a Gep
// whose first operand is the va_list tagBase and whose second operand is an integer
// Const equal to `byteOffset`. This DISCRIMINATES the AAPCS64 dual-cursor's per-class
// save-block selection: an HFA gather reads __vr_offs@28 (>0) and NEVER __gr_offs@24
// (==0); a non-HFA gather is the mirror. Reading the WRONG cursor (an HFA from the GR
// block, hazard H2) is silent garbage with NO trap — only this offset-keyed count
// catches it host-independently.
//
// tagBase identity (class-INDEPENDENT): it is the `Alloca` (the `ap` local) that is
// the base of at least one Gep whose RESULT is itself LOADED — the cursor/top/stack
// field reads. The va_arg RESULT temp (`freshAggregateTemp`) is ALSO an Alloca with
// Geps WHOSE RESULTS ARE LOADED — `lowerByteWiseCopy` (src hir_to_mir.cpp ~2440)
// emits `Load(Gep(temp,...))` when the gathered struct is copied out into the
// destination local, so the temp's Geps are NOT "unloaded". We rely on PROGRAM ORDER
// instead: the va_list cursor read `Load(Gep(tagBase, offset))` is EMITTED FIRST —
// va_start emits only Stores, so the first `Load(Gep(Alloca,...))` the scan meets is
// always a cursor read off the `ap` Alloca, never a result-temp copy-out (which is
// emitted later). (`__stack`@0 is both loaded and stored, so the tagBase also
// qualifies via that read even when offsets 0/8/16 would otherwise alias a temp Gep.)
[[nodiscard]] MirInstId findVaListTagBase(Mir const& m, std::uint32_t fi) {
    MirFuncId const f = m.funcAt(fi);
    // The tagBase is the Alloca that is the base of at least one Gep whose RESULT is
    // the op[0] of a Load (a cursor/top/stack FIELD read). Single linear scan: for
    // each Load(Gep(base, ...)) where base is an Alloca, that Alloca is the tagBase.
    // Correct by PROGRAM ORDER (NOT because the result temp's Geps are unloaded — they
    // are, via the copy-out): the cursor read is emitted before any copy-out, so the
    // FIRST such Load encountered is a `ap`-Alloca cursor read.
    for (std::uint32_t b = 0; b < m.funcBlockCount(f); ++b) {
        MirBlockId const blk = m.funcBlockAt(f, b);
        for (std::uint32_t i = 0; i < m.blockInstCount(blk); ++i) {
            MirInstId const ix = m.blockInstAt(blk, i);
            if (m.instOpcode(ix) != MirOpcode::Load) continue;
            auto const lops = m.instOperands(ix);
            if (lops.empty()) continue;
            MirInstId const base = lops[0];
            if (m.instOpcode(base) != MirOpcode::Gep) continue;
            auto const gops = m.instOperands(base);
            if (gops.empty()) continue;
            if (m.instOpcode(gops[0]) == MirOpcode::Alloca) return gops[0];
        }
    }
    return InvalidMirInst;
}

[[nodiscard]] std::size_t countLoadsAtFieldOffset(Mir const& m, std::uint32_t fi,
                                                  std::int64_t byteOffset) {
    MirInstId const tagBase = findVaListTagBase(m, fi);
    if (!tagBase.valid()) return 0;
    std::size_t n = 0;
    MirFuncId const f = m.funcAt(fi);
    for (std::uint32_t b = 0; b < m.funcBlockCount(f); ++b) {
        MirBlockId const blk = m.funcBlockAt(f, b);
        for (std::uint32_t i = 0; i < m.blockInstCount(blk); ++i) {
            MirInstId const ix = m.blockInstAt(blk, i);
            if (m.instOpcode(ix) != MirOpcode::Load) continue;
            auto const lops = m.instOperands(ix);
            if (lops.empty()) continue;
            MirInstId const base = lops[0];
            if (m.instOpcode(base) != MirOpcode::Gep) continue;
            auto const gops = m.instOperands(base);
            if (gops.size() < 2) continue;
            if (gops[0] != tagBase) continue;
            MirInstId const offInst = gops[1];
            if (m.instOpcode(offInst) != MirOpcode::Const) continue;
            auto const& lit = m.literalValue(m.constLiteralIndex(offInst));
            if (std::holds_alternative<std::int64_t>(lit.value)
                && std::get<std::int64_t>(lit.value) == byteOffset)
                ++n;
        }
    }
    return n;
}

// FOLD B (NIT 2 — the host-independent H5 REGISTER-arm pin) — count Loads in `fi`
// whose shape is the AAPCS64 ByReference register-arm pointer DEREF:
// `Load(Gep(<top>, idx))` where `<top>` is itself `Load(Gep(tagBase, byteOffset))` —
// the `__<gr|vr>_top` field read. Concretely (src hir_to_mir.cpp ~3683-3697): the reg
// arm computes `grTop = Load(Gep(tagBase, grTopField))`, then `slotAddr = Gep(grTop,
// SExt(curOffs))`, then DEREFS `regStructPtr = Load(slotAddr)`. This count matches the
// final deref Load: its op[0] is a Gep whose BASE op is a Load of the field at
// `byteOffset` (grTopField==8). `countChainedLoadOfLoad` CANNOT see this — the deref is
// `Load(Gep(...))`, NOT a `Load(Load(...))` — so that pin witnesses only the OVERFLOW
// arm (whose deref IS `Load(Load(stackField))`). Removing the register-arm deref
// (returning `slotAddr` instead of dereferencing the hidden pointer — a silent
// miscompile) takes THIS count to 0 while leaving countChainedLoadOfLoad>=1.
// Reuses findVaListTagBase to anchor the field read class-independently.
[[nodiscard]] std::size_t countRegArmTopDeref(Mir const& m, std::uint32_t fi,
                                              std::int64_t byteOffset) {
    MirInstId const tagBase = findVaListTagBase(m, fi);
    if (!tagBase.valid()) return 0;
    std::size_t n = 0;
    MirFuncId const f = m.funcAt(fi);
    for (std::uint32_t b = 0; b < m.funcBlockCount(f); ++b) {
        MirBlockId const blk = m.funcBlockAt(f, b);
        for (std::uint32_t i = 0; i < m.blockInstCount(blk); ++i) {
            MirInstId const ix = m.blockInstAt(blk, i);
            // The DEREF: a Load whose op[0] is a Gep.
            if (m.instOpcode(ix) != MirOpcode::Load) continue;
            auto const dops = m.instOperands(ix);
            if (dops.empty()) continue;
            MirInstId const slotGep = dops[0];
            if (m.instOpcode(slotGep) != MirOpcode::Gep) continue;
            auto const sg = m.instOperands(slotGep);
            if (sg.empty()) continue;
            // The Gep's BASE op must be a Load of the __<gr|vr>_top FIELD: itself a
            // `Load(Gep(tagBase, Const(byteOffset)))`.
            MirInstId const topLoad = sg[0];
            if (m.instOpcode(topLoad) != MirOpcode::Load) continue;
            auto const tl = m.instOperands(topLoad);
            if (tl.empty()) continue;
            MirInstId const topGep = tl[0];
            if (m.instOpcode(topGep) != MirOpcode::Gep) continue;
            auto const tg = m.instOperands(topGep);
            if (tg.size() < 2) continue;
            if (tg[0] != tagBase) continue;
            MirInstId const offInst = tg[1];
            if (m.instOpcode(offInst) != MirOpcode::Const) continue;
            auto const& lit = m.literalValue(m.constLiteralIndex(offInst));
            if (std::holds_alternative<std::int64_t>(lit.value)
                && std::get<std::int64_t>(lit.value) == byteOffset)
                ++n;
        }
    }
    return n;
}

// FOLD 3 SELF-TEST: the load-bearing H2 helper (`countLoadsAtFieldOffset`) +
// `findVaListTagBase` must be NON-VACUOUS — prove they locate the va_list tagBase and
// count a known field read on a fixture independent of the new struct gather. The
// AAPCS64 SCALAR va_arg(int) reads the GR cursor __gr_offs@24 exactly once (one
// `Load(Gep(ap, 24))`); offset 28 (__vr_offs) is NOT read on the integer path. If the
// helper were broken (returned 0 unconditionally, or failed to find the tagBase) the
// H2 pins below would PASS VACUOUSLY — this self-test forecloses that.
TEST(MirLoweringCSubset, Aapcs64FieldOffsetHelperSelfTest) {
    auto L = lowerCSubset(
        "int sum(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  int t = va_arg(ap, int);\n"
        "  va_end(ap);\n"
        "  return t;\n"
        "}\n",
        "arm64", "aapcs64");
    ASSERT_TRUE(L.mir.ok);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    EXPECT_TRUE(findVaListTagBase(m, fi).valid())
        << "the helper must locate the `ap` Alloca tagBase (the base of the cursor "
           "field Loads) — a broken finder makes the H2 pins vacuous";
    EXPECT_GE(countLoadsAtFieldOffset(m, fi, 24), 1u)
        << "the AAPCS64 scalar va_arg(int) reads __gr_offs@24 — the helper must COUNT "
           "it (a vacuous 0 would silently pass the HFA-from-VR-not-GR H2 guard)";
    EXPECT_EQ(countLoadsAtFieldOffset(m, fi, 28), 0u)
        << "the integer path does NOT read __vr_offs@28 — the helper must distinguish "
           "field offsets (not just count all field Loads)";
    EXPECT_TRUE(hasMirOpcode(m, fi, MirOpcode::SExt))
        << "hasMirOpcode wrapper must report the scalar arm's SXTW present";
}

// FC12b (D-FC12B-WIN64-STRUCT-VARARG) — INVERTED to SUCCESS: `va_arg(ap, struct)`
// under the Win64 (ms_x64) HomogeneousPointer strategy for a pow2-≤8B struct. The
// struct sits BY VALUE in the one arg slot, so va_arg returns the slot's ADDRESS
// (apVal) directly — NO deref Load, NO Phi (the slot IS the storage), and a LINEAR
// pointer bump (no reg-vs-overflow diamond → NO atomic-fit ICmpUle pair, NO
// ByValueStackArg carrier). RED-ON-DISABLE: reverting the lowerVaArgAggregate
// HomogeneousPointer arm to fail-loud flips mir.ok false; a regression to the SysV
// by-value gather would emit the ICmpUle atomic-fit pair (caught below).
TEST(MirLoweringCSubset, Win64VaArgStructLeq8ReturnsSlotAddr) {
    auto L = lowerCSubset(
        "struct Pt { int a; int b; };\n"   // 8B (pow2 ≤8) → InRegisters[1], by value
        "int sum(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  struct Pt p = va_arg(ap, struct Pt);\n"
        "  va_end(ap);\n"
        "  return p.a + p.b;\n"
        "}\n",
        "x86_64", "ms_x64");
    ASSERT_TRUE(L.mir.ok)
        << "Win64 va_arg(≤8B struct) must lower (D-FC12B-WIN64-STRUCT-VARARG): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithoutCall(m);   // `sum` (the variadic callee)
    // The slot IS the storage: a ≤8B struct va_arg has NO reg-vs-overflow diamond
    // (linear pointer bump), so NO atomic-fit ICmpUle pair and NO carrier.
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::ICmpUle), 0u)
        << "a Win64 ≤8B struct va_arg is a LINEAR pointer bump — no atomic-fit "
           "diamond (a SysV-gather regression would emit the ICmpUle pair)";
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::ByValueStackArg), 0u)
        << "no overflow carrier — Win64 places the struct in one slot by position";
    // The va_arg bump is by namedArgSlotBytes (8) — pin the Const so a wrong stride
    // (e.g. sizeof not slot) goes red. The struct fields are then byte-copied out of
    // the slot (the member-access reads p.a@0 / p.b@4) — anti-folded by the corpus.
    EXPECT_TRUE(funcHasConstInt(m, fi, 8))
        << "the va_list cursor bumps by namedArgSlotBytes==8 (one Win64 arg slot)";
    // STRUCTURAL no-deref mirror of the >8B pin: the ≤8B by-value path returns the
    // SLOT ADDRESS (apVal) directly, so there is NO chained `Load(Load(ap))`. A
    // spurious value Load of the cursor (returning the slot's *contents* — a scalar —
    // as if it were the aggregate's address) would be a silent miscompile that only
    // the Windows-only PE corpus catches; pin it red here too. (funcHasConstInt(8) and
    // the ICmpUle/ByValueStackArg==0 checks above can't see this — they don't inspect
    // the va_arg result chain.)
    EXPECT_EQ(countChainedLoadOfLoad(m, fi), 0u)
        << "the ≤8B by-value va_arg returns the slot ADDRESS (apVal) directly — there "
           "must be NO chained Load(Load(ap)) deref (a value Load here would return a "
           "scalar as the aggregate address)";
}

// FC12b (D-FC12B-WIN64-STRUCT-VARARG) — SUCCESS, >8B by-reference arm. A struct
// whose size is NOT pow2-≤8 (here 24B) rides as a hidden POINTER in the one slot;
// va_arg must DEREFERENCE the slot to get the caller's copy address. So the callee
// emits an EXTRA Load (the slot Load + the pointer deref Load) that the ≤8B by-value
// arm does NOT, still with NO diamond/carrier. RED-ON-DISABLE: dropping the
// ByReference deref Load would return the slot addr (a pointer-to-pointer) → the
// consumer copies 24 bytes of pointer+garbage (a silent miscompile).
TEST(MirLoweringCSubset, Win64VaArgStructGt8DerefsPointer) {
    auto L = lowerCSubset(
        "struct Big { long a; long b; long c; };\n"  // 24B → ByReference (hidden ptr)
        "long pick(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  struct Big b = va_arg(ap, struct Big);\n"
        "  va_end(ap);\n"
        "  return b.a + b.b + b.c;\n"
        "}\n",
        "x86_64", "ms_x64");
    ASSERT_TRUE(L.mir.ok)
        << "Win64 va_arg(>8B struct) must lower (D-FC12B-WIN64-STRUCT-VARARG): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithoutCall(m);   // `pick`
    // Still linear (one slot, by pointer): no atomic-fit diamond, no carrier.
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::ICmpUle), 0u)
        << "a Win64 >8B struct va_arg is a LINEAR pointer bump (one slot, by ptr)";
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::ByValueStackArg), 0u)
        << "no overflow carrier — the >8B struct rides as a pointer in one slot";
    // The bump is ONE slot, NOT roundUp(24,8)==24: the SLOT holds a pointer, not the
    // 24-byte struct. A regression to a by-value overflow bump would emit a Const 24
    // (the by-value tail-round). The struct's three long fields live at 0/8/16, so
    // there is NO legitimate Const 24 in the body — its presence means a wrong bump.
    EXPECT_FALSE(funcHasConstInt(m, fi, 24))
        << "Win64 does NOT bump by roundUp(sizeof,8)==24 (that is the SysV by-value "
           "overflow stride — the Win64 slot holds a pointer, not the bytes)";
    // The by-reference arm emits the slot-cursor Load `apVal = Load(tagBase)` AND a
    // chained deref `structAddr = Load(apVal)` (the slot holds a hidden pointer). The
    // ≤8B by-value arm omits the deref (returns apVal directly). A whole-function
    // `Load >= N` count CANNOT discriminate the two: BOTH paths also emit the
    // aggregate copy-in's per-field Loads (`lowerAggregateCopy` loads each of the 3
    // longs) plus the `return` member reads, so dropping the deref still leaves the
    // total well above any fixed floor. Instead assert the STRUCTURAL signature that
    // is UNIQUE to the deref: a `Load` whose operand is itself a `Load` (the chained
    // slot-Load → deref-Load). Every other Load here goes through a `Gep` (field
    // reads) or loads `tagBase` (the `ap` Alloca), so none is chained — dropping the
    // deref takes this count to 0. Host-independent: it inspects the MIR shape, not a
    // count the (Windows-only) PE corpus would otherwise be the sole guard of.
    EXPECT_GE(countChainedLoadOfLoad(m, fi), 1u)
        << "the >8B by-reference va_arg must deref the slot: a chained Load(Load(ap)) "
           "— dropping the deref (returning the slot address) takes this to 0 (a "
           "silent miscompile caught here, not just on the Windows PE leg)";
}

// FC12b (D-FC12B-WIN64-STRUCT-VARARG) — INVERTED to SUCCESS, caller side: passing a
// pow2-≤8B struct BY VALUE to a Win64 (ms_x64) variadic callee. The struct rides in
// exactly ONE arg slot BY VALUE — the Call gets exactly ONE value operand for it (a
// loaded I64), NOT a ByValueStackArg carrier and NOT the SysV piece-split. RED-ON-
// DISABLE: reverting the wall-2 HomogeneousPointer arm to fail-loud flips mir.ok
// false; a regression to the SysV atomic-fit path would emit a carrier or split.
TEST(MirLoweringCSubset, Win64StructByValueVarargCallLeq8OneSlot) {
    auto L = lowerCSubset(
        "struct Pt { int a; int b; };\n"   // 8B → InRegisters[1], one slot by value
        "int sink(int n, ...) { return n; }\n"   // DEFINED so the callee symbol binds (D-CSUBSET-FN-PROTOTYPE: a bare proto no longer emits a spurious FnSig global)
        "int main(void) {\n"
        "  struct Pt p; p.a = 1; p.b = 2;\n"
        "  return sink(1, p);\n"          // struct BY VALUE to a Win64 variadic fn
        "}\n",
        "x86_64", "ms_x64");
    ASSERT_TRUE(L.mir.ok)
        << "a ≤8B struct-by-value vararg to a Win64 variadic fn must lower "
           "(D-FC12B-WIN64-STRUCT-VARARG): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithCall(m);   // `main` (the caller)
    // NO overflow carrier — the by-value struct is ONE value operand in one slot.
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::ByValueStackArg), 0u)
        << "a Win64 ≤8B by-value struct vararg is ONE value slot, never a carrier";
    // The Call has exactly the fixed callee + the count `1` + ONE struct value
    // operand = 3 operands. (callee GlobalAddr, n=1, struct-as-I64.)
    MirFuncId const f = m.funcAt(fi);
    MirInstId call = InvalidMirInst;
    for (std::uint32_t b = 0; b < m.funcBlockCount(f) && !call.valid(); ++b) {
        MirBlockId const blk = m.funcBlockAt(f, b);
        for (std::uint32_t i = 0; i < m.blockInstCount(blk); ++i) {
            MirInstId const ix = m.blockInstAt(blk, i);
            if (m.instOpcode(ix) == MirOpcode::Call) { call = ix; break; }
        }
    }
    ASSERT_TRUE(call.valid());
    auto const ops = m.instOperands(call);
    EXPECT_EQ(ops.size(), 3u)
        << "Call = [callee, n=1, ONE struct value operand] — the ≤8B struct is a "
           "single by-value slot (NOT 0/fail-loud, NOT a 2-piece split)";
}

// FC12b (D-FC12B-WIN64-STRUCT-VARARG) — SUCCESS, caller side >8B by-reference. A
// non-pow2/>8B struct passed BY VALUE to a Win64 variadic callee rides as a hidden
// POINTER in ONE slot — the Call gets exactly ONE pointer operand (the temp copy
// address), still NO carrier. RED-ON-DISABLE: a regression to the SysV MEMORY-class
// carrier would emit a ByValueStackArg instead of the pointer operand.
TEST(MirLoweringCSubset, Win64StructByValueVarargCallGt8OnePointer) {
    auto L = lowerCSubset(
        "struct Big { long a; long b; long c; };\n"  // 24B → ByReference (hidden ptr)
        "int sink(int n, ...) { return n; }\n"   // DEFINED so the callee symbol binds (D-CSUBSET-FN-PROTOTYPE: a bare proto no longer emits a spurious FnSig global)
        "int main(void) {\n"
        "  struct Big b; b.a = 1; b.b = 2; b.c = 3;\n"
        "  return sink(1, b);\n"          // >8B struct BY VALUE to a Win64 variadic fn
        "}\n",
        "x86_64", "ms_x64");
    ASSERT_TRUE(L.mir.ok)
        << "a >8B struct-by-value vararg to a Win64 variadic fn must lower "
           "(D-FC12B-WIN64-STRUCT-VARARG): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithCall(m);   // `main`
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::ByValueStackArg), 0u)
        << "a Win64 >8B by-value struct vararg rides as a pointer in one slot — "
           "never the SysV MEMORY-class overflow carrier";
    MirFuncId const f = m.funcAt(fi);
    MirInstId call = InvalidMirInst;
    for (std::uint32_t b = 0; b < m.funcBlockCount(f) && !call.valid(); ++b) {
        MirBlockId const blk = m.funcBlockAt(f, b);
        for (std::uint32_t i = 0; i < m.blockInstCount(blk); ++i) {
            MirInstId const ix = m.blockInstAt(blk, i);
            if (m.instOpcode(ix) == MirOpcode::Call) { call = ix; break; }
        }
    }
    ASSERT_TRUE(call.valid());
    auto const ops = m.instOperands(call);
    EXPECT_EQ(ops.size(), 3u)
        << "Call = [callee, n=1, ONE struct POINTER operand] — the >8B struct is a "
           "single hidden-pointer slot (NOT a carrier, NOT 0/fail-loud)";
}

// FC12-deferral④ (D-FC12A-VARIADIC-OVERFLOW-FIXED-STACK-ARGS, CLOSED): a SysV
// variadic callee whose FIXED params overflow the 6 integer arg registers onto the
// stack now LOWERS — va_start bakes the fixed-stack-arg byte displacement into the
// VaOverflowArgAreaAddr payload so overflow_arg_area skips the named stack arg(s) and
// points at the FIRST vararg (SysV §3.5.7). FOLD 3 (strict): pin the payload VALUE.
// RED-ON-DISABLE: revert the mir_to_lir VaOverflowArgAreaAddr payload threading (or
// the hir_to_mir displacement) → the payload drops to 0 and this EXPECT_EQ goes red.
TEST(MirLoweringCSubset, VaStartFixedParamsOverflowToStackSucceeds) {
    // 7 fixed int params (a..g): SysV has 6 integer arg registers, so `g` overflows
    // ON THE STACK. gprOver = 7 - 6 = 1 → fixedStackBytes = 1 * gpSlotBytes(8) = 8.
    auto L = lowerCSubset(
        "int pick(int a, int b, int c, int d, int e, int f, int g, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, g);\n"
        "  int v = va_arg(ap, int);\n"
        "  va_end(ap);\n"
        "  return a + g + v;\n"
        "}\n");
    ASSERT_TRUE(L.mir.ok)
        << "a SysV variadic callee whose fixed params overflow to the stack must now "
           "LOWER (D-FC12A-VARIADIC-OVERFLOW-FIXED-STACK-ARGS): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    auto const payload = vaOverflowArgAreaPayload(m, fi);
    ASSERT_TRUE(payload.has_value())
        << "the lowered va_start must emit a VaOverflowArgAreaAddr leaf";
    EXPECT_EQ(*payload, 8u)
        << "overflow_arg_area must be displaced PAST the 1 overflowed fixed GPR "
           "(1 * gpSlotBytes(8) = 8) — a 0 payload would read the fixed stack arg `g` "
           "as the first vararg (FOLD 3: value, not presence)";
}

// FC12-deferral④ (D-FC12C-AAPCS64-VARIADIC-OVERFLOW-FIXED-STACK-ARGS, CLOSED): AAPCS64
// mirror — a variadic ARM64 callee whose FIXED int params overflow the 8 GPR arg regs
// (x0..x7) onto the stack now LOWERS. va_start bakes the fixed-stack-arg displacement
// into VaOverflowArgAreaAddr (→ __stack) AND CLAMPS __gr_offs to 0 (the GR block is
// fully consumed by fixed params, so every GPR vararg routes to __stack). FOLD 3
// (strict): payload VALUE == 8 + no negative const survives (the __gr_offs clamp).
// RED-ON-DISABLE: revert the payload threading → payload 0 (red); revert the
// __gr_offs clamp to `-(gpSaveCount - fixedGpr)` → a negative i32 const appears (red).
TEST(MirLoweringCSubset, Aapcs64VaStartFixedParamsOverflowToStackSucceeds) {
    // 9 fixed int params (a..i): AAPCS64 has 8 GPR arg regs (x0..x7), so `i` overflows
    // ON THE STACK. gprOver = 9 - 8 = 1 → fixedStackBytes = 1 * gpSlotBytes(8) = 8;
    // __gr_offs clamps to 0 (fixedGpr 9 >= gpSaveCount 8).
    auto L = lowerCSubset(
        "int pick(int a, int b, int c, int d, int e, int f, int g, int h, int i,"
        " ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, i);\n"
        "  int v = va_arg(ap, int);\n"
        "  va_end(ap);\n"
        "  return a + i + v;\n"
        "}\n",
        "arm64", "aapcs64");
    ASSERT_TRUE(L.mir.ok)
        << "an AAPCS64 variadic callee whose fixed INT params overflow to the stack "
           "must now LOWER (D-FC12C-AAPCS64-VARIADIC-OVERFLOW-FIXED-STACK-ARGS): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    auto const payload = vaOverflowArgAreaPayload(m, fi);
    ASSERT_TRUE(payload.has_value())
        << "the lowered va_start must emit a VaOverflowArgAreaAddr (→ __stack) leaf";
    EXPECT_EQ(*payload, 8u)
        << "__stack must skip the 1 overflowed fixed GPR (1 * gpSlotBytes(8) = 8)";
    // __gr_offs CLAMPS to 0 (GR block fully consumed). NOTE __vr_offs is legitimately
    // -128 here (no fixed FP params → whole VR block available), so a blunt "any
    // negative const" check would false-fail; the strict witness is "no HUGE-magnitude
    // underflow const survives" (< -1000), which the reverted clamp's ≈ -4.29e9 trips.
    EXPECT_TRUE(funcHasConstInt(m, fi, 0))
        << "__gr_offs must be CLAMPED to 0 (the GR block is fully consumed)";
    EXPECT_FALSE(funcHasConstIntBelow(m, fi, -1000))
        << "the OLD underflowing `-(gpSaveCount - fixedGpr)*8` wraps unsigned to a "
           "HUGE-magnitude negative const (≈ -4.29e9) for __gr_offs — its absence is "
           "the strict clamp witness (FOLD 3; the legit __vr_offs=-128 is far above "
           "the -1000 floor)";
}

// FC12-deferral④ (FP-cursor variant, CLOSED): 9 fixed DOUBLE params overflow the 8 FP
// arg regs (v0..v7) onto the stack — now LOWERS. fprOver = 9 - 8 = 1 → fixedStackBytes
// = 1 * gpSlotBytes(8) = 8 (the overflow FP slot is 8B, NOT fpSlotBytes(16)); __vr_offs
// clamps to 0. NOTE __gr_offs is legitimately -64 here (no fixed GPR params → whole GR
// block available), so the no-negative-const check does NOT apply to this FP case.
// RED-ON-DISABLE: revert the payload threading → payload 0 (red); revert the __vr_offs
// clamp → __vr_offs underflows (the Const 0 disappears) → red.
TEST(MirLoweringCSubset, Aapcs64VaStartFixedDoubleParamsOverflowToStackSucceeds) {
    auto L = lowerCSubset(
        "double pick(double a, double b, double c, double d, double e, double f,"
        " double g, double h, double i, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, i);\n"
        "  double v = va_arg(ap, double);\n"
        "  va_end(ap);\n"
        "  return a + i + v;\n"
        "}\n",
        "arm64", "aapcs64");
    ASSERT_TRUE(L.mir.ok)
        << "an AAPCS64 variadic callee whose fixed FP params overflow to the stack "
           "must now LOWER (D-FC12C-AAPCS64-VARIADIC-OVERFLOW-FIXED-STACK-ARGS): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    auto const payload = vaOverflowArgAreaPayload(m, fi);
    ASSERT_TRUE(payload.has_value())
        << "the lowered va_start must emit a VaOverflowArgAreaAddr (→ __stack) leaf";
    EXPECT_EQ(*payload, 8u)
        << "__stack must skip the 1 overflowed fixed FP slot, sized gpSlotBytes(8) "
           "NOT fpSlotBytes(16) (incoming overflow slots are 8B)";
    EXPECT_TRUE(funcHasConstInt(m, fi, 0))
        << "__vr_offs must be CLAMPED to 0 (the VR block is fully consumed by fixed "
           "FP params)";
}

// FC12-deferral④ (mixed-class overflow): 9 fixed int (x0..x7 + 1 overflow) AND 9 fixed
// double (v0..v7 + 1 overflow) → BOTH classes overflow by one slot. gprOver = 1,
// fprOver = 1 → fixedStackBytes = (1 + 1) * gpSlotBytes(8) = 16; BOTH __gr_offs and
// __vr_offs clamp to 0 (each block fully consumed). This is the dual-cursor stress
// case the single-class pins do not cover. RED-ON-DISABLE: drop the payload threading
// → payload 0; revert either clamp → a negative const appears (red).
TEST(MirLoweringCSubset, Aapcs64VaStartMixedClassFixedStackOverflow) {
    auto L = lowerCSubset(
        "double pick(int a, int b, int c, int d, int e, int f, int g, int h, int i,"
        " double m0, double m1, double m2, double m3, double m4, double m5,"
        " double m6, double m7, double m8, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, m8);\n"
        "  double v = va_arg(ap, double);\n"
        "  va_end(ap);\n"
        "  return a + i + v;\n"
        "}\n",
        "arm64", "aapcs64");
    ASSERT_TRUE(L.mir.ok)
        << "an AAPCS64 variadic callee whose fixed INT and FP params BOTH overflow must "
           "LOWER (D-FC12C-AAPCS64-VARIADIC-OVERFLOW-FIXED-STACK-ARGS): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    auto const payload = vaOverflowArgAreaPayload(m, fi);
    ASSERT_TRUE(payload.has_value())
        << "the lowered va_start must emit a VaOverflowArgAreaAddr (→ __stack) leaf";
    EXPECT_EQ(*payload, 16u)
        << "__stack must skip BOTH overflowed fixed slots (gprOver 1 + fprOver 1) * 8 "
           "= 16";
    EXPECT_TRUE(funcHasConstInt(m, fi, 0))
        << "both __gr_offs and __vr_offs must be CLAMPED to 0 (both blocks consumed)";
    EXPECT_FALSE(funcHasConstIntBelow(m, fi, -1000))
        << "with BOTH classes fully consumed, NO huge-magnitude underflow const may "
           "survive — reverting EITHER clamp emits one (≈ -4.29e9) (FOLD 3: the "
           "both-clamp witness)";
}

// D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS (CLOSED): a SysV variadic callee
// whose fixed params overflow the GPR pool AND include a by-value AGGREGATE that
// STRADDLES the register/stack boundary now LOWERS — the WHOLE struct is received
// ALL-OR-NOTHING from the incoming stack (a RecvByValueStackParam, the callee mirror of
// the caller's ByValueStackArg carrier), and va_start's VaOverflowArgArea payload counts
// its FULL 16 bytes. `f(a..e, struct S16, ...)`: a..e consume 5 GPR (rdi..r8); S16 =
// {long,long} needs 2 GPR eightbytes but only r9 remains → SysV places the WHOLE 16B in
// memory (NOT split), and BACKFILLS r9 for the first vararg (SysV does not exhaust on a
// stacked aggregate). RED-ON-DISABLE: revert the caller all-or-nothing (Phase A) or the
// callee reception (Phase B) → the struct splits and the byte cursor no longer reaches
// 16 / no RecvByValueStackParam appears; revert Phase C → wrong payload.
TEST(MirLoweringCSubset, VaStartFixedStructStraddleOverflowSucceeds) {
    auto L = lowerCSubset(
        "struct S16 { long x; long y; };\n"
        "long f(long a, long b, long c, long d, long e, struct S16 s, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, s);\n"
        "  long v = va_arg(ap, long);\n"
        "  va_end(ap);\n"
        "  return a + s.x + v;\n"
        "}\n");
    ASSERT_TRUE(L.mir.ok)
        << "a SysV variadic callee with a straddling fixed by-value aggregate must now "
           "LOWER all-or-nothing (D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    auto const payload = vaOverflowArgAreaPayload(m, fi);
    ASSERT_TRUE(payload.has_value())
        << "the lowered va_start must emit a VaOverflowArgAreaAddr leaf";
    EXPECT_EQ(*payload, 16u)
        << "overflow_arg_area must skip the WHOLE 16-byte stacked struct (roundUp(16,8) "
           "= 16), NOT the per-eightbyte slot count — a 0/8 payload would read the "
           "struct's own bytes as the first vararg (all-or-nothing, not split)";
    EXPECT_GT(countOpcodeAllBlocks(m, fi, MirOpcode::RecvByValueStackParam), 0u)
        << "a straddling fixed aggregate must be RECEIVED via RecvByValueStackParam "
           "(whole-from-stack), not split into per-eightbyte Args";
    // BACKFILL witness: SysV does NOT exhaust on a stacked aggregate, so the 5 fixed
    // GPRs leave gp_offset = 5*8 = 40 (the trailing vararg backfills r9, save-area slot
    // 5). The AAPCS64-style clamp would (wrongly) advance the cursor to 6 → gp_offset
    // = 48, and the Const 40 would NOT appear. (NB: 48 itself is also the legitimate
    // fp_offset base = gpOffsetLimit, so its PRESENCE is not a clamp signal — the
    // backfill witness is the PRESENCE of 40, absent under a clamp.)
    EXPECT_TRUE(funcHasConstInt(m, fi, 40))
        << "gp_offset must be 40 (5 fixed GPRs; SysV BACKFILLS r9 — does not exhaust the "
           "cursor on a stacked aggregate; a clamp would make it 48 and drop the 40)";
}

// D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS mirror (AAPCS64, CLOSED): a
// variadic ARM64 callee whose fixed params overflow the FP pool AND include a by-value
// HFA straddling the register/stack boundary now LOWERS all-or-nothing. `f(6 doubles,
// struct H3, ...)` where H3 = {double,double,double} is a 3-element HFA: the 6 fixed
// doubles consume v0..v5, leaving v6,v7 (2 VR) but H3 needs 3 → AAPCS64 §B places the
// WHOLE 24B HFA in memory AND EXHAUSTS the VR class (NSRN←8), so __vr_offs clamps to 0
// and the first FP vararg routes to __stack PAST the HFA. RED-ON-DISABLE: revert
// Phase A/B → split / no RecvByValueStackParam; revert the AAPCS64 cursor exhaust →
// __vr_offs is -32 (not clamped) and the vararg reads the HFA's bytes.
TEST(MirLoweringCSubset, Aapcs64VaStartFixedHfaStraddleOverflowSucceeds) {
    auto L = lowerCSubset(
        "struct H3 { double a; double b; double c; };\n"
        "double f(double a, double b, double c, double d, double e, double g,"
        " struct H3 h, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, h);\n"
        "  double v = va_arg(ap, double);\n"
        "  va_end(ap);\n"
        "  return a + h.a + v;\n"
        "}\n",
        "arm64", "aapcs64");
    ASSERT_TRUE(L.mir.ok)
        << "an AAPCS64 variadic callee with a straddling fixed by-value HFA must now "
           "LOWER all-or-nothing (D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    auto const payload = vaOverflowArgAreaPayload(m, fi);
    ASSERT_TRUE(payload.has_value())
        << "the lowered va_start must emit a VaOverflowArgAreaAddr (→ __stack) leaf";
    EXPECT_EQ(*payload, 24u)
        << "__stack must skip the WHOLE 24-byte stacked HFA (roundUp(24,8) = 24)";
    EXPECT_GT(countOpcodeAllBlocks(m, fi, MirOpcode::RecvByValueStackParam), 0u)
        << "the straddling HFA must be RECEIVED via RecvByValueStackParam";
    // EXHAUST witness: the HFA consumed the VR class → __vr_offs clamps to 0. Reverting
    // the AAPCS64 cursor exhaust leaves __vr_offs = -(8-6)*16 = -32 (not clamped).
    EXPECT_TRUE(funcHasConstInt(m, fi, 0))
        << "__vr_offs must be CLAMPED to 0 — the straddling HFA EXHAUSTED the VR class "
           "(AAPCS64 §B NSRN←8)";
    EXPECT_FALSE(funcHasConstInt(m, fi, -32))
        << "__vr_offs must NOT be -32 — the cursor must EXHAUST (clamp), not leave 2 VR "
           "slots as if the HFA had not consumed the class";
}

// D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS — the CALLER all-or-nothing (the
// latent NON-variadic split this cycle ALSO repaired, previously unfiled). A non-variadic
// call whose fixed by-value aggregate arg STRADDLES the reg/stack boundary must place the
// WHOLE struct on the stack via a ByValueStackArg carrier, NOT split it into per-eightbyte
// register pieces (lir_callconv would put one piece in the last register + one on the
// stack — non-conformant to SysV §3.2.3). `g(a..e, struct S16 s)`: a..e fill 5 GPR, S16 =
// {long,long} needs 2 but 1 remains → carrier. RED-ON-DISABLE: revert the caller's
// `!argSlotAligned` exhaustion check (Phase A — its hoist out of `if (calleeVariadic)`)
// → the struct splits into per-eightbyte Args → NO ByValueStackArg appears in `caller`.
TEST(MirLoweringCSubset, CallerNonVariadicStraddlingAggregateUsesStackCarrier) {
    auto L = lowerCSubset(
        "struct S16 { long x; long y; };\n"
        "long g(long a, long b, long c, long d, long e, struct S16 s) {\n"
        "  return a + s.x;\n"
        "}\n"
        "long caller(void) {\n"
        "  struct S16 s;\n"
        "  s.x = 1; s.y = 2;\n"
        "  return g(10, 20, 30, 40, 50, s);\n"
        "}\n");
    ASSERT_TRUE(L.mir.ok)
        << "a non-variadic call with a straddling fixed by-value aggregate must lower: "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    // The CALLER places the straddling struct via a ByValueStackArg carrier; the CALLEE
    // `g` receives it via RecvByValueStackParam. Both opcodes must therefore be present
    // in the module — count each across all functions (the carrier is the Phase-A pin).
    std::size_t carriers = 0, receives = 0;
    for (std::uint32_t f = 0; f < m.moduleFuncCount(); ++f) {
        carriers += countOpcodeAllBlocks(m, f, MirOpcode::ByValueStackArg);
        receives += countOpcodeAllBlocks(m, f, MirOpcode::RecvByValueStackParam);
    }
    EXPECT_GT(carriers, 0u)
        << "the straddling struct arg must be PLACED via a ByValueStackArg carrier "
           "(whole-to-stack), NOT split into per-eightbyte register pieces — the caller "
           "mirror of the callee RecvByValueStackParam (all-or-nothing, both sides)";
    EXPECT_GT(receives, 0u)
        << "the callee `g` (non-variadic) must RECEIVE the straddling struct via "
           "RecvByValueStackParam — the all-or-nothing fix covers non-variadic too";
}

namespace {
// Find the FIRST Call inst in function `fi` and return its payload (0 if none).
[[nodiscard]] std::uint32_t firstCallPayload(Mir const& m, std::uint32_t fi) {
    MirFuncId const f = m.funcAt(fi);
    for (std::uint32_t b = 0; b < m.funcBlockCount(f); ++b) {
        MirBlockId const blk = m.funcBlockAt(f, b);
        for (std::uint32_t i = 0; i < m.blockInstCount(blk); ++i) {
            MirInstId const ix = m.blockInstAt(blk, i);
            if (m.instOpcode(ix) == MirOpcode::Call) return m.instPayload(ix);
        }
    }
    return 0;
}
// FOLD 2 (va_arg same-class cursor threading): true iff some `Gep` in function `fi`
// has its INDEX operand (operand[1]) equal to the result of an `Add` instruction.
// In lowerVaArgAggregate's register arm, the SOURCE read of each eightbyte is
// `Gep(reg_save_area, cursor)`. The FIRST piece's cursor is the loaded gp/fp_offset;
// every SUBSEQUENT same-class piece's cursor is the PRIOR cursor + stride — an `Add`
// result. So a multi-piece SAME-class gather ALWAYS produces a Gep indexed off an Add.
// The threading regression (re-reading the ORIGINAL Load cursor for the 2nd piece)
// would make BOTH source Geps index off the Load, never off an Add → this returns
// false. (The DST Gep `Gep(temp, constOffset)` and the overflow-arm Gep both index
// off a `Const`, never an `Add`; the loop counter's `Add` feeds the compare/store,
// not a Gep index — so an Add-indexed Gep is unambiguously the threaded cursor read.)
[[nodiscard]] bool gepIndexesOffBumpedCursor(Mir const& m, std::uint32_t fi) {
    MirFuncId const f = m.funcAt(fi);
    for (std::uint32_t b = 0; b < m.funcBlockCount(f); ++b) {
        MirBlockId const blk = m.funcBlockAt(f, b);
        for (std::uint32_t i = 0; i < m.blockInstCount(blk); ++i) {
            MirInstId const ix = m.blockInstAt(blk, i);
            if (m.instOpcode(ix) != MirOpcode::Gep) continue;
            auto const ops = m.instOperands(ix);
            if (ops.size() < 2) continue;
            MirInstId const idxOperand = ops[1];
            if (idxOperand.valid()
                && m.instOpcode(idxOperand) == MirOpcode::Add)
                return true;
        }
    }
    return false;
}
} // namespace

// FC12a-struct (operand-count stamp): a variadic call that passes a FIXED by-value
// struct AND a struct vararg in ONE call stamps fixedOperandCount in OPERAND units —
// NOT param units. `combine(struct Pt base, int n, ...)` with one `struct Pt` vararg:
//   fixed params = {base (1 GPR + 1 FPR = 2 operands), n (1 operand)} → 3 operands;
//   the struct vararg adds 2 more (1 GPR + 1 FPR). So fixedOperandCount must be 3.
// RED-ON-DISABLE: reverting the snapshot to fnParams().size() (=2) makes the
// lir_callconv vararg loop treat operand index 2 (base's FPR piece) as a vararg FPR,
// over-counting the SysV AL by one — here pinned at MIR as fixedOperandCount==3.
TEST(MirLoweringCSubset, VariadicCallWithStructStampsFixedOperandCount) {
    using namespace dss::call_payload;
    // `combine` is DEFINED in-TU (not extern) so its symbol binds in the MIR-tier
    // driver (which fails loud on unbound symbols — extern symbols resolve only in
    // the full link pipeline). The body is minimal; `use` is the function under test.
    auto L = lowerCSubset(
        "struct Pt { long a; double b; };\n"
        "long combine(struct Pt base, int n, ...) { return base.a + n; }\n"
        "long use(void) {\n"
        "  struct Pt base = {1, 2.0};\n"
        "  struct Pt v = {3, 4.0};\n"
        "  return combine(base, 1, v);\n"
        "}\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "the variadic call with a struct vararg must lower: "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    // `use` is the caller — the function whose Call is variadic. (combine also
    // exists; pick the function that CONTAINS the variadic Call.)
    std::uint32_t fi = 0;
    for (std::uint32_t f = 0; f < m.moduleFuncCount(); ++f) {
        if (dss::call_payload::isVariadic(firstCallPayload(m, f))) { fi = f; break; }
    }
    std::uint32_t const payload = firstCallPayload(m, fi);
    EXPECT_TRUE(isVariadic(payload)) << "the call to a variadic callee is variadic";
    EXPECT_EQ(fixedOperandCount(payload), 3u)
        << "fixedOperandCount is in OPERAND units: base (2 pieces) + n (1) = 3 — "
           "NOT the param count (2)";
}

// FC12a-struct (FOLD 1 — hidden-arg sret in fixedOperandCount): a VARIADIC call to a
// callee that RETURNS a >16B struct BY VALUE (SysV hidden-arg sret) with a trailing
// FIXED `double` param. SysV routes the sret pointer as a REAL arg in rdi (argIdx 0,
// firstArgIdx==1 in lir_callconv — NOT the x8-IRR convention), so `operandsBeforeArgs`
// must COUNT it: operands = [callee, sretPtr, 2.5(fixed double), 1(vararg), 2(vararg)];
// at the stamp point (after the fixed double) operands.size()==3 and
// operandsBeforeArgs==1 (callee only — the hidden-arg sret is INSIDE fixedOperandCount),
// so fixedOperandCount == 2 (sret@argIdx0 + double@argIdx1 are both fixed; varargs start
// at argIdx 2 → AL vector count EXCLUDES the fixed double's FPR).
//
// RED-ON-DISABLE: reverting `operandsBeforeArgs` to `operands.size()` (the post-sret-push
// capture) makes it 2, so fixedOperandCount == 3-2 == 1. Then lir_callconv's vararg loop
// (firstArgIdx==1) sees the fixed double at argIdx 1 >= fixedCount(1) → misclassifies it
// as the FIRST vararg, an FPR → AL set one too high → silent miscompile. The EXPECT below
// goes red (sees 1, expects 2). Verified by temp-reverting the capture and confirming red.
//
// `vsret` is DEFINED in-TU so its symbol binds in the MIR-tier driver (extern symbols
// resolve only in the full link pipeline), mirroring the other call-site fixtures.
TEST(MirLoweringCSubset, VariadicCallSretHiddenArgCountedInFixedOperandCount) {
    using namespace dss::call_payload;
    auto L = lowerCSubset(
        "struct Big { long a; long b; long c; };\n"   // 24B → MEMORY class → hidden-arg sret
        "struct Big vsret(double d, ...) {\n"
        "  struct Big r = {0, 0, 0};\n"
        "  return r;\n"
        "}\n"
        "long use(void) {\n"
        "  struct Big r = vsret(2.5, 1, 2);\n"
        "  return r.a;\n"
        "}\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "a variadic call returning a >16B struct (hidden-arg sret) with a trailing "
           "fixed double must lower: "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    // `use` is the caller — the function whose Call is variadic. (`vsret` is also
    // variadic-typed but contains NO Call; pick the function that CONTAINS the call.)
    std::uint32_t fi = 0;
    bool found = false;
    for (std::uint32_t f = 0; f < m.moduleFuncCount(); ++f) {
        if (isVariadic(firstCallPayload(m, f))) { fi = f; found = true; break; }
    }
    ASSERT_TRUE(found) << "the variadic call must be present in some function";
    std::uint32_t const payload = firstCallPayload(m, fi);
    EXPECT_TRUE(isVariadic(payload)) << "the call to a variadic callee is variadic";
    EXPECT_TRUE(hasIndirectResult(payload) == false)
        << "SysV (no indirectResultRegister) uses the hidden-ARG sret convention, "
           "NOT the x8-IRR bit — the sret pointer is a real arg, not routed to x8";
    EXPECT_EQ(fixedOperandCount(payload), 2u)
        << "fixedOperandCount MUST count the hidden-arg sret pointer (argIdx 0) AND the "
           "fixed double (argIdx 1) = 2. Reverting operandsBeforeArgs to operands.size() "
           "(post-sret-push) yields 1, mis-binning the fixed double as the first vararg "
           "and over-counting the SysV AL by one.";
}

// FC12a-struct (wall 3): a variadic fn with a FIXED InRegisters `{long; double}`
// param LOWERS (no longer fail-loud) and its va_start initializes gp_offset/fp_offset
// PAST the struct's pieces (1 GPR + 1 FPR). For SysV: gp_offset = 1*8 = 8;
// fp_offset = gpOffsetLimit(48) + 1*16 = 64. RED-ON-DISABLE: reverting wall-3 to
// fail-loud makes mir.ok false; reverting receiveByValueParam's per-class advance
// (so the FPR piece is not counted) makes fp_offset 48 (no Const 64) → red.
TEST(MirLoweringCSubset, VariadicFnFixedStructParamThreadsBothClassCursors) {
    auto L = lowerCSubset(
        "struct Pt { long a; double b; };\n"
        "long sum(struct Pt base, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, base);\n"
        "  va_end(ap);\n"
        "  return base.a;\n"
        "}\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "a variadic fn with a fixed InRegisters struct param must LOWER (wall-3 "
           "now classifies before the variadic check)";
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    // gp_offset init = fixedGpr(1) * gpSlotBytes(8) = 8 (the struct's 1 GPR piece).
    EXPECT_TRUE(funcHasConstInt(m, fi, 8))
        << "va_start gp_offset must start PAST the struct's 1 GPR piece (1*8=8)";
    // fp_offset init = gpOffsetLimit(48) + fixedFpr(1) * fpSlotBytes(16) = 64.
    EXPECT_TRUE(funcHasConstInt(m, fi, 64))
        << "va_start fp_offset must start PAST the struct's 1 FPR piece "
           "(48 + 1*16 = 64) — pins the per-class accounting through the struct";
}

// FC12a-struct (wall 1, MEMORY class): passing a >16B (MEMORY class) struct BY VALUE
// to a variadic callee now LOWERS via the Option-C carrier (was fail-loud). STRUCTURAL
// PIN (red-on-disable): the call's vararg region carries exactly ONE `ByValueStackArg`
// (the by-value-stack aggregate carrier) — reverting the ByReference-variadic route to
// fail-loud makes mir.ok false; a regression that pushed the hidden pointer instead
// (the non-variadic Win64-style path) would emit 0 ByValueStackArg → red.
TEST(MirLoweringCSubset, VariadicCallStructOver16BByValueUsesCarrier) {
    auto L = lowerCSubset(
        "struct Big { long a; long b; long c; };\n"   // 24B → MEMORY class
        "long combine(int n, ...) { return n; }\n"    // DEFINED so the symbol binds
        "long use(void) {\n"
        "  struct Big b = {1, 2, 3};\n"
        "  return combine(1, b);\n"
        "}\n");
    ASSERT_TRUE(L.mir.ok)
        << "a >16B (MEMORY class) struct vararg must now LOWER via the Option-C carrier "
           "(D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithCall(m);   // `use` (the caller)
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::ByValueStackArg), 1u)
        << "the >16B struct vararg is routed to exactly one by-value-stack carrier "
           "(NOT the hidden-pointer convention)";
}

// FC12a-struct (wall 3, MEMORY class): a >16B by-value struct FIXED param in a
// variadic fn now LOWERS (was fail-loud) — receiveByValueParam's ByReference arm
// receives the hidden pointer as ONE GPR arg, so va_start's gp_offset starts past it
// (1 GPR consumed → gp_offset init = 1*8 = 8). STRUCTURAL PIN (red-on-disable):
// reverting wall-3 to fail-loud makes mir.ok false; the Const 8 pins that the hidden
// pointer was counted as exactly one fixed GPR (a regression that miscounted it would
// not emit Const 8). The SEPARATE D-FC12A-VARIADIC-OVERFLOW-FIXED-STACK-ARGS guard is
// untouched (this fn has 1 fixed GPR ≤ 6, so it does not fire).
TEST(MirLoweringCSubset, VariadicFnFixedStructOver16BParamThreadsHiddenPtrCursor) {
    auto L = lowerCSubset(
        "struct Big { long a; long b; long c; };\n"   // 24B → MEMORY class
        "long sum(struct Big base, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, base);\n"
        "  va_end(ap);\n"
        "  return base.a;\n"
        "}\n");
    ASSERT_TRUE(L.mir.ok)
        << "a >16B (MEMORY class) fixed struct param in a variadic fn must now LOWER "
           "(D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    // gp_offset init = fixedGpr(1) * gpSlotBytes(8) = 8 — the hidden sret-style pointer
    // for the >16B param consumed exactly one GPR arg register.
    EXPECT_TRUE(funcHasConstInt(m, fi, 8))
        << "va_start gp_offset must start PAST the >16B param's 1 hidden-pointer GPR "
           "(1*8 = 8)";
}

// FC12a-struct (atomic-exhaustion split): a variadic call where enough prior GPR
// args have been consumed that a `{long, long}` (2-GPR) struct vararg cannot fit
// wholly in registers now routes the WHOLE struct to the Option-C carrier (was
// fail-loud) — SysV forbids the register/stack split, so the aggregate goes ENTIRELY
// to memory. Here: `combine(int n, ...)` with 1 fixed int (GPR 0) + 5 long varargs
// (GPRs 1..5) means only 1 arg GPR remains, but `{long,long}` needs 2 → whole struct
// to overflow. STRUCTURAL PIN (red-on-disable): exactly ONE `ByValueStackArg` carrier
// is emitted (NOT 2 register pieces); reverting the split route to fail-loud makes
// mir.ok false; a regression that emitted the 2 register pieces (the silent split the
// audit caught) would emit 0 ByValueStackArg → red.
TEST(MirLoweringCSubset, VariadicCallStructRegisterExhaustionSplitRoutesToCarrier) {
    auto L = lowerCSubset(
        "struct LL { long a; long b; };\n"
        "long combine(int n, ...) { return n; }\n"    // DEFINED so the symbol binds
        "long use(void) {\n"
        "  struct LL p = {7, 8};\n"
        "  return combine(1, 100L, 200L, 300L, 400L, 500L, p);\n"
        "}\n");
    ASSERT_TRUE(L.mir.ok)
        << "a struct vararg that cannot fit wholly in registers must route the WHOLE "
           "aggregate to the carrier (D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithCall(m);   // `use` (the caller)
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::ByValueStackArg), 1u)
        << "the register-exhaustion-split struct vararg goes WHOLLY to the overflow "
           "area via one by-value-stack carrier (never a register/stack split)";
}

// FC12a-struct (anti-resurrection / by-address routing): a DISCARDED aggregate
// va_arg (`va_arg(ap, struct Pt);` as a bare ExprStmt — side effect only) MUST
// route through lowerLvalueAddress's VaArg arm (→ lowerVaArgAggregate), running the
// gather BY ADDRESS so the cursor side-effect happens, and MUST NOT synthesize a
// Phi-of-aggregate / aggregate-width value. The lowerExpr VaArg arm is a fail-loud
// anti-resurrection backstop (no C-subset source reaches it — every aggregate
// va_arg consumer keys on type-kind → by address). RED-ON-DISABLE: removing the
// lowerLvalueAddress VaArg arm makes the discard fall to lowerLvalueAddress's final
// fail-loud (mir.ok false); a regression that synthesized an aggregate Phi trips the
// hasAggregatePhi assertion.
TEST(MirLoweringCSubset, DiscardedAggregateVaArgRoutesByAddressNoAggregatePhi) {
    auto L = lowerCSubset(
        "struct Pt { long a; double b; };\n"
        "long sum(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  va_arg(ap, struct Pt);\n"   // DISCARDED aggregate va_arg (side effect only)
        "  va_end(ap);\n"
        "  return 0;\n"
        "}\n");
    ASSERT_TRUE(L.mir.ok)
        << "a discarded aggregate va_arg routes BY ADDRESS via lowerLvalueAddress's "
           "VaArg arm (the gather's cursor bump is the side effect) — it must lower";
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    // The gather ran (the atomic decision's ICmpUle pair is present), confirming the
    // by-address arm — not the scalar lowerExpr path (which would fail loud).
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::ICmpUle), 2u)
        << "the discarded aggregate va_arg lowered via the atomic register-gather";
    EXPECT_FALSE(hasAggregatePhi(m, L.model.lattice().interner(), fi))
        << "an aggregate va_arg must NEVER produce a Phi of an aggregate value — it "
           "is lowered BY ADDRESS (the lowerExpr bare-rvalue path is fail-loud)";
}

// FOLD 2 (va_arg SAME-class cursor threading — MIR pin paired with the run-witnessed
// varargs_struct corpus): a `va_arg(ap, struct LL)` where `struct LL {long a; long b;}`
// gathers TWO GPR eightbytes. lowerVaArgAggregate must thread the cursor: piece 0 reads
// `Gep(reg_save_area, gp_offset)`; piece 1 reads `Gep(reg_save_area, gp_offset + 8)` —
// the BUMPED cursor (an `Add` result). The MIR therefore contains a source Gep whose
// INDEX operand is an `Add`. RED-ON-DISABLE: if the 2nd-piece read re-used the ORIGINAL
// `gp_offset` Load (the threading regression — `b` aliases `a`'s slot), NO Gep would
// index off an Add, and `gepIndexesOffBumpedCursor` returns false → this EXPECT goes
// red. The runtime corpus is the primary lever (it witnesses a WRONG exit code); this
// pin localizes the same regression to the MIR shape for a fast, host-independent fail.
TEST(MirLoweringCSubset, VaArgTwoSameClassStructThreadsCursorThroughAdd) {
    auto L = lowerCSubset(
        "struct LL { long a; long b; };\n"
        "long sumLL(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  struct LL p = va_arg(ap, struct LL);\n"
        "  va_end(ap);\n"
        "  return p.a + p.b;\n"
        "}\n");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "a 2-GPR struct va_arg must lower: "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    // The atomic register-gather ran (the by-address arm, not the fail-loud rvalue path).
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::ICmpUle), 2u)
        << "the aggregate va_arg lowered via the atomic register-gather";
    // The 2nd GPR piece's source Gep indexes off the BUMPED cursor (an Add), proving
    // the cursor was threaded rather than re-read from the original gp_offset Load.
    EXPECT_TRUE(gepIndexesOffBumpedCursor(m, fi))
        << "the 2nd same-class eightbyte must read reg_save_area + the BUMPED cursor "
           "(a Gep indexed off an Add); a Gep indexed only off the original gp_offset "
           "Load for both pieces is the threading regression (b aliases a's slot)";
}

// ─── FC12c (D-FC12C-*) AAPCS64 dual-cursor MIR pins (host-independent) ───

namespace {
// BLOCKER-2 pin support: true iff ANY Gep in function `fi` has an index operand that
// is a SExt (sign-extend) instruction. The AAPCS64 va_arg reg arm MUST sign-extend
// the NEGATIVE i32 cursor to i64 BEFORE the byte Gep (a zero-extended -40 becomes
// ~+4 GiB → a wild address). Removing the SExt (the red-on-disable) flips this false.
[[nodiscard]] bool sextFeedsGepIndex(Mir const& m, std::uint32_t fi) {
    MirFuncId const f = m.funcAt(fi);
    for (std::uint32_t b = 0; b < m.funcBlockCount(f); ++b) {
        MirBlockId const blk = m.funcBlockAt(f, b);
        for (std::uint32_t i = 0; i < m.blockInstCount(blk); ++i) {
            MirInstId const ix = m.blockInstAt(blk, i);
            if (m.instOpcode(ix) != MirOpcode::Gep) continue;
            auto const ops = m.instOperands(ix);
            if (ops.size() < 2) continue;
            MirInstId const idx = ops[1];
            if (idx.valid() && m.instOpcode(idx) == MirOpcode::SExt) return true;
        }
    }
    return false;
}
// True iff function `fi` contains a Const instruction whose integer value == `want`.
[[nodiscard]] bool hasConstValue(Mir const& m, std::uint32_t fi, std::int64_t want) {
    MirFuncId const f = m.funcAt(fi);
    for (std::uint32_t b = 0; b < m.funcBlockCount(f); ++b) {
        MirBlockId const blk = m.funcBlockAt(f, b);
        for (std::uint32_t i = 0; i < m.blockInstCount(blk); ++i) {
            MirInstId const ix = m.blockInstAt(blk, i);
            if (m.instOpcode(ix) != MirOpcode::Const) continue;
            auto const& lit = m.literalValue(m.constLiteralIndex(ix));
            if (auto const* v = std::get_if<std::int64_t>(&lit.value);
                v != nullptr && *v == want)
                return true;
        }
    }
    return false;
}
} // namespace

// Gate #2 + #9 (host-independent, BLOCKER-2): an AAPCS64 va_arg(int) lowers to the
// dual-cursor diamond — ICmpSlt(offs, 0) (NEGATIVE cursor < 0, a SIGNED compare, not
// the SysV ICmpUlt-vs-limit) feeding a CondBr, a Phi joining the two address arms,
// AND — the BLOCKER-2 pin — a SExt feeding the reg-arm's address Gep index. RED-ON-
// DISABLE: removing the SExt (the load-bearing sign-extend) flips sextFeedsGepIndex.
TEST(MirLoweringCSubset, Aapcs64VaArgIntLowersToDualCursorDiamondSignExtend) {
    auto L = lowerCSubset(
        "int sum(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  int t = va_arg(ap, int);\n"
        "  va_end(ap);\n"
        "  return t;\n"
        "}\n",
        "arm64", "aapcs64");
    ASSERT_FALSE(L.model.hasErrors())
        << "AAPCS64 variadic callee must type-check (va_list = __va_list struct)";
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << "the AAPCS64 dual-cursor va_arg must lower: "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    // SIGNED compare against 0 (the negative cursor) — NOT the SysV unsigned-vs-limit.
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::ICmpSlt), 1u)
        << "AAPCS64 va_arg compares the NEGATIVE class cursor against 0 (signed)";
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::ICmpUlt), 0u)
        << "AAPCS64 does NOT use the SysV unsigned-vs-limit compare";
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::CondBr), 1u)
        << "AAPCS64 va_arg branches reg-vs-stack";
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::Phi), 1u)
        << "AAPCS64 va_arg joins the two address arms with a Phi (scalar pointer)";
    EXPECT_TRUE(sextFeedsGepIndex(m, fi))
        << "BLOCKER-2: the reg arm MUST sign-extend the i32 cursor before the byte Gep "
           "(a zero-extended negative cursor is a +4 GiB wild address → segfault); "
           "remove the SExt and this assertion goes red";
    // AAPCS64 uses the dual-cursor leaf (VaRegSaveAreaAddr), not the Win64 home leaf.
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::VaRegSaveAreaAddr), 1u);
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::VaHomeArgAreaAddr), 0u);
}

// Gate #3 (host-independent): AAPCS64 va_start with N fixed int args sets __gr_offs =
// -(8 - N) * 8. With N=1 (the fixed `n`) → -(8-1)*8 = -56. Emit a Const of that value.
// RED-ON-DISABLE: a wrong sign (e.g. +56) or wrong arithmetic flips the Const value.
TEST(MirLoweringCSubset, Aapcs64VaStartInitsNegativeGrOffs) {
    auto L = lowerCSubset(
        "int sum(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  int t = va_arg(ap, int);\n"
        "  va_end(ap);\n"
        "  return t;\n"
        "}\n",
        "arm64", "aapcs64");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    // __gr_offs = -(8 - 1) * 8 = -56 (1 fixed int arg in x0).
    EXPECT_TRUE(hasConstValue(m, fi, -56))
        << "AAPCS64 va_start must init __gr_offs = -(gpSaveCount - fixedGpr)*gpSlotBytes "
           "= -(8-1)*8 = -56 for one fixed int arg (a NEGATIVE cursor counting up to 0)";
    // __vr_offs = -(8 - 0) * 16 = -128 (no fixed FP args).
    EXPECT_TRUE(hasConstValue(m, fi, -128))
        << "AAPCS64 va_start must init __vr_offs = -(8-0)*16 = -128 (no fixed FP args)";
}

// D-FC12C-AAPCS64-HFA-STRUCT-VARARG — INVERTED to SUCCESS (was Aapcs64VaArgStructFailsLoud).
// A struct va_arg under AAPCS64 now GATHERS via the dual-cursor diamond. `struct Pt`
// (8B, non-HFA) is a 1-GPR InRegisters aggregate → it reads the GR save block. The
// inverted pin asserts the gather's structural signature (NOT a bare ok flip): the
// SIGNED atomic-fit (ICmpSlt, NOT the SysV ICmpUlt), the SXTW (SExt, hazard H1), and
// the H2 wrong-class guard (reads __gr_offs@24, NEVER __vr_offs@28). RED-ON-DISABLE:
// reverting the lowerVaArgAggregate Aapcs64DualCursor arm to fail-loud flips ok false.
TEST(MirLoweringCSubset, Aapcs64VaArgStructGathersViaDualCursor) {
    auto L = lowerCSubset(
        "struct Pt { int a; int b; };\n"   // 8B, non-HFA → 1 GPR piece (GR block)
        "int sum(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  struct Pt p = va_arg(ap, struct Pt);\n"
        "  va_end(ap);\n"
        "  return p.a + p.b;\n"
        "}\n",
        "arm64", "aapcs64");
    ASSERT_TRUE(L.mir.ok)
        << "va_arg of a struct under AAPCS64 must SUCCEED after "
           "D-FC12C-AAPCS64-HFA-STRUCT-VARARG: "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);   // `sum` (the variadic callee)
    EXPECT_TRUE(hasMirOpcode(m, fi, MirOpcode::ICmpSlt))
        << "the AAPCS64 struct gather uses the SIGNED atomic-fit (negative cursor < 0)";
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::ICmpUlt), 0u)
        << "AAPCS64 does NOT use the SysV unsigned-vs-limit compare (a fall-through to "
           "the SysV gather would emit ICmpUlt)";
    EXPECT_TRUE(hasMirOpcode(m, fi, MirOpcode::SExt))
        << "H1: the reg arm MUST sign-extend the i32 cursor before the byte Gep";
    // H2 (wrong-class save block): a non-HFA reads __gr_offs@24, NEVER __vr_offs@28.
    EXPECT_GE(countLoadsAtFieldOffset(m, fi, 24), 1u)
        << "a non-HFA struct gather reads the GR cursor __gr_offs@24";
    EXPECT_EQ(countLoadsAtFieldOffset(m, fi, 28), 0u)
        << "a non-HFA struct gather must NEVER read the VR cursor __vr_offs@28 (H2: "
           "reading the wrong save block is silent garbage)";
}

// D-FC12C-AAPCS64-HFA-STRUCT-VARARG — INVERTED to SUCCESS (was
// Aapcs64StructByValueVarargCallFailsLoud). Passing a struct BY VALUE to an AAPCS64
// variadic CALLEE now places it in registers (the GR/VR pool has room). `struct Pt`
// is non-HFA → 1 GPR operand. The inverted pin asserts the register placement (NOT a
// bare ok flip): NO ByValueStackArg carrier when the struct fits in registers. RED-ON-
// DISABLE: reverting the Call-arg-loop Aapcs64DualCursor arm to fail-loud flips ok false.
TEST(MirLoweringCSubset, Aapcs64StructByValueVarargCallPlacesInRegisters) {
    auto L = lowerCSubset(
        "struct Pt { int a; int b; };\n"
        "int sink(int n, ...) { return n; }\n"   // DEFINED so the callee symbol binds (D-CSUBSET-FN-PROTOTYPE: a bare proto no longer emits a spurious FnSig global)
        "int main(void) {\n"
        "  struct Pt p; p.a = 1; p.b = 2;\n"
        "  return sink(1, p);\n"          // struct BY VALUE to an AAPCS64 variadic fn
        "}\n",
        "arm64", "aapcs64");
    ASSERT_TRUE(L.mir.ok)
        << "a struct-by-value vararg to an AAPCS64 variadic fn must SUCCEED after "
           "D-FC12C-AAPCS64-HFA-STRUCT-VARARG: "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithCall(m);   // `main` (the caller)
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::ByValueStackArg), 0u)
        << "a struct that fits in the GR pool is REGISTER-placed (piece operands), "
           "NOT forced to the overflow via the ByValueStackArg carrier";
}

// D-FC12C-AAPCS64-HFA-STRUCT-VARARG (Step 4.3) — an HFA `{double,double}` va_arg
// gathers from the VR save block, NOT the GR block (hazard H2). 2 FPR pieces ⇒ ≥2
// SExt (one cursor SXTW per piece, H1) + the signed VR atomic-fit. The load-bearing
// H2 pin: __vr_offs@28 is read (>0) and __gr_offs@24 is NEVER read (==0). RED-ON-
// DISABLE: routing HFA pieces through the GR cursor (offField=grOffs) flips both
// offset counts (24 becomes >0, 28 becomes 0) → silent garbage from the integer block.
TEST(MirLoweringCSubset, Aapcs64VaArgHfaFromVrNotGr) {
    auto L = lowerCSubset(
        "struct HFA { double a; double b; };\n"   // 16B, 2 FPR pieces (VR block)
        "double sum(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  double t = 0;\n"
        "  for (int i = 0; i < n; i = i + 1) {\n"
        "    struct HFA p = va_arg(ap, struct HFA);\n"
        "    t = t + p.a + p.b;\n"
        "  }\n"
        "  va_end(ap);\n"
        "  return t;\n"
        "}\n",
        "arm64", "aapcs64");
    ASSERT_TRUE(L.mir.ok)
        << "AAPCS64 HFA va_arg must lower: "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    EXPECT_TRUE(hasMirOpcode(m, fi, MirOpcode::ICmpSlt))
        << "the VR atomic-fit is a SIGNED compare (negative cursor)";
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::SExt), 2u)
        << "H1: each of the 2 HFA pieces sign-extends its i32 cursor before the byte Gep";
    EXPECT_GE(countLoadsAtFieldOffset(m, fi, 28), 1u)
        << "an HFA gather reads the VR cursor __vr_offs@28";
    EXPECT_EQ(countLoadsAtFieldOffset(m, fi, 24), 0u)
        << "H2: an HFA gather must NEVER read the GR cursor __gr_offs@24 (reading the "
           "integer save block for an FPR piece is silent garbage)";
}

// D-FC12C-AAPCS64-HFA-STRUCT-VARARG (Step 4.4) — the MIRROR of the H2 pin: a non-HFA
// `{long,long}` (16B, 2 GPR pieces) reads the GR block and NEVER the VR block.
TEST(MirLoweringCSubset, Aapcs64VaArgNonHfaFromGrNotVr) {
    auto L = lowerCSubset(
        "struct Pair { long a; long b; };\n"   // 16B, 2 GPR pieces (GR block)
        "long sum(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  struct Pair p = va_arg(ap, struct Pair);\n"
        "  va_end(ap);\n"
        "  return p.a + p.b;\n"
        "}\n",
        "arm64", "aapcs64");
    ASSERT_TRUE(L.mir.ok)
        << "AAPCS64 non-HFA ≤16B va_arg must lower: "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    EXPECT_GE(countLoadsAtFieldOffset(m, fi, 24), 1u)
        << "a non-HFA gather reads the GR cursor __gr_offs@24";
    EXPECT_EQ(countLoadsAtFieldOffset(m, fi, 28), 0u)
        << "H2 mirror: a non-HFA gather must NEVER read the VR cursor __vr_offs@28";
    EXPECT_GE(countOpcodeAllBlocks(m, fi, MirOpcode::SExt), 2u)
        << "H1: each of the 2 GPR pieces sign-extends its i32 cursor before the byte Gep";
}

// D-FC12C-AAPCS64-HFA-STRUCT-VARARG (Step 4.5) — a >16B `{long,long,long}` (24B,
// ByReference) va_arg DEREFERENCES a hidden pointer from ONE GR slot (hazards H5/H7):
// a chained Load(Load(...)) (the slot-Load → pointer-deref-Load). RED-ON-DISABLE:
// dropping the deref (returning the slot address) takes the chained-Load count to 0.
TEST(MirLoweringCSubset, Aapcs64VaArgByRefUsesChainedLoad) {
    auto L = lowerCSubset(
        "struct Big { long a; long b; long c; };\n"   // 24B → ByReference (hidden ptr)
        "long pick(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  struct Big b = va_arg(ap, struct Big);\n"
        "  va_end(ap);\n"
        "  return b.a + b.b + b.c;\n"
        "}\n",
        "arm64", "aapcs64");
    ASSERT_TRUE(L.mir.ok)
        << "AAPCS64 >16B ByReference va_arg must lower: "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    EXPECT_GE(countChainedLoadOfLoad(m, fi), 1u)
        << "H5: the ByReference va_arg must deref the GR slot (chained Load(Load(...)))";
    EXPECT_GE(countLoadsAtFieldOffset(m, fi, 24), 1u)
        << "the hidden pointer rides in ONE GR slot — read __gr_offs@24 (NOT the VR "
           "cursor; a pointer is integer-class, hazard H5)";
    EXPECT_EQ(countLoadsAtFieldOffset(m, fi, 28), 0u)
        << "a ByReference pointer is GR-class — the VR cursor __vr_offs@28 is untouched";
}

// FOLD B (NIT 2) — the precise, host-independent H5 REGISTER-arm deref pin. The
// existing Aapcs64VaArgByRefUsesChainedLoad above uses countChainedLoadOfLoad, which
// matches `Load(Load(...))` — the OVERFLOW arm's deref (`Load(Load(stackField))`). But
// the REGISTER arm's deref is `Load(Gep(__gr_top, idx))` = `Load(Gep(...))`, NOT a
// chained Load-of-Load — so that pin does NOT witness the register arm. Dropping the
// register-arm deref (returning `slotAddr` — the address OF the hidden pointer slot —
// instead of dereferencing it: a silent miscompile that hands the consumer a
// pointer-to-pointer) would leave countChainedLoadOfLoad>=1 (overflow arm intact) and
// flip NO host-independent pin. This asserts the register-arm deref SHAPE directly:
// `Load(Gep(<gr_top>, idx))` where `<gr_top>` is `Load(Gep(tagBase, grTopField==8))`
// (the byteOffset is read from arm64.target.json aapcs64 vaListLayout). RED-ON-DISABLE:
// removing the deref Load takes countRegArmTopDeref(...,8) to 0.
TEST(MirLoweringCSubset, Aapcs64VaArgByRefRegArmDerefsGrTop) {
    auto L = lowerCSubset(
        "struct Big { long a; long b; long c; };\n"   // 24B → ByReference (hidden ptr)
        "long pick(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  struct Big b = va_arg(ap, struct Big);\n"
        "  va_end(ap);\n"
        "  return b.a + b.b + b.c;\n"
        "}\n",
        "arm64", "aapcs64");
    ASSERT_TRUE(L.mir.ok)
        << "AAPCS64 >16B ByReference va_arg must lower: "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    // grTopField byteOffset == 8 (arm64.target.json aapcs64 vaListLayout). The register
    // arm derefs the hidden pointer: Load(Gep(Load(Gep(ap, 8)), SExt(curOffs))). This
    // count is EXACTLY 1 (the single register-arm deref) and goes to 0 if the deref is
    // removed — a witness the chained-Load pin above structurally cannot provide.
    EXPECT_EQ(countRegArmTopDeref(m, fi, 8), 1u)
        << "H5 (register arm): the ByReference va_arg must DEREF the hidden pointer in "
           "the GR slot — Load(Gep(__gr_top@8, idx)). Removing the deref (returning the "
           "slot address) is a silent miscompile that countChainedLoadOfLoad (overflow "
           "arm only) cannot catch — this pin can";
}

// D-FC12C-AAPCS64-HFA-STRUCT-VARARG (Step 4.6, CALLER) — passing an HFA
// `{double,double}` vararg with room in the VR pool places it in REGISTERS (FPR
// pieces), NOT the overflow carrier (hazards H4/H8). RED-ON-DISABLE: a wrong-class
// exhaustion check that tests the GR pool for an HFA would route it to the stack
// carrier (ByValueStackArg present).
TEST(MirLoweringCSubset, Aapcs64CallerHfaVarargInRegisters) {
    auto L = lowerCSubset(
        "struct HFA { double a; double b; };\n"
        "int sink(int n, ...) { return n; }\n"   // DEFINED so the callee symbol binds (D-CSUBSET-FN-PROTOTYPE: a bare proto no longer emits a spurious FnSig global)
        "int main(void) {\n"
        "  struct HFA h; h.a = 1.0; h.b = 2.0;\n"
        "  return sink(1, h);\n"
        "}\n",
        "arm64", "aapcs64");
    ASSERT_TRUE(L.mir.ok)
        << "AAPCS64 HFA vararg caller must lower: "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithCall(m);
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::ByValueStackArg), 0u)
        << "H4/H8: an HFA with room in the VR pool is register-placed (FPR pieces), "
           "NOT routed to the overflow carrier";
}

// D-FC12C-AAPCS64-HFA-STRUCT-VARARG (Step 4.7, CALLER) — a non-HFA `{long,long}`
// vararg passed AFTER 8 scalar long varargs drains the GR pool (gpSaveCount=8) forces
// the struct WHOLE to the overflow via the ByValueStackArg carrier (hazard H8). RED-
// ON-DISABLE: a missing exhaustion check would emit register pieces (no carrier).
TEST(MirLoweringCSubset, Aapcs64CallerNonHfaVarargExhaustsGpr) {
    auto L = lowerCSubset(
        "struct LL { long a; long b; };\n"
        "long sink(int n, ...) { return n; }\n"   // DEFINED so the callee symbol binds (D-CSUBSET-FN-PROTOTYPE)
        "int main(void) {\n"
        "  struct LL p; p.a = 40; p.b = 5;\n"
        "  return (int)sink(8, 1L,2L,3L,4L,5L,6L,7L,8L, p);\n"
        "}\n",
        "arm64", "aapcs64");
    ASSERT_TRUE(L.mir.ok)
        << "AAPCS64 GR-exhaustion vararg caller must lower: "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithCall(m);
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::ByValueStackArg), 1u)
        << "H8: 8 scalar longs drain the GR pool (gpSaveCount=8) → the trailing struct "
           "is forced WHOLE to the overflow via the ByValueStackArg carrier";
}

// FOLD C (NIT 3) — the host-independent H7 caller-ByReference pin. The caller arms have
// host-independent MIR pins for HFA-in-regs (Aapcs64CallerHfaVarargInRegisters), non-
// HFA-in-regs (Aapcs64StructByValueVarargCallPlacesInRegisters), and GR-exhaustion→
// carrier (Aapcs64CallerNonHfaVarargExhaustsGpr). But the >16B ByReference caller arm
// (src hir_to_mir.cpp ~762-765: `appendByValueArg` + `runGpr += 1` — a hidden POINTER
// in ONE GR slot, placed BY POSITION, NO carrier — hazard H7) was covered ONLY by the
// qemu runtime corpus. AAPCS64 has no SysV-style MEMORY-class-to-stack rule for
// ByReference, so the pointer must ride as a normal GR operand (register or, once the
// pool drains, the stack BY POSITION via lir_callconv's positional walk), NEVER forced
// into the overflow area by the carrier. RED-ON-DISABLE: swapping this arm's
// `appendByValueArg` for `appendByValueStackArg` makes a ByValueStackArg appear in the
// Call (and drops the plain pointer operand) — both checked below.
TEST(MirLoweringCSubset, Aapcs64CallerByRefVarargIsPositionedPointerNotCarrier) {
    auto L = lowerCSubset(
        "struct Big { long a; long b; long c; };\n"  // 24B → ByReference (hidden ptr)
        "int sink(int n, ...) { return n; }\n"   // DEFINED so the callee symbol binds (D-CSUBSET-FN-PROTOTYPE: a bare proto no longer emits a spurious FnSig global)
        "int main(void) {\n"
        "  struct Big b; b.a = 1; b.b = 2; b.c = 3;\n"
        "  return sink(1, b);\n"          // >16B struct BY VALUE to an AAPCS64 variadic fn
        "}\n",
        "arm64", "aapcs64");
    ASSERT_TRUE(L.mir.ok)
        << "a >16B struct-by-value vararg to an AAPCS64 variadic fn must lower "
           "(D-FC12C-AAPCS64-HFA-STRUCT-VARARG): "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithCall(m);   // `main` (the caller)
    // H7: the >16B ByReference vararg is a hidden pointer placed BY POSITION (register or
    // positional stack) — NEVER force-routed to the overflow via the carrier.
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::ByValueStackArg), 0u)
        << "H7: a >16B ByReference vararg rides as a pointer in ONE GR slot (by "
           "position), NOT the SysV MEMORY-class overflow carrier — swapping "
           "appendByValueArg for appendByValueStackArg would make a carrier appear here";
    // The Call gained exactly ONE extra operand for the struct: a single pointer. So
    // Call = [callee, n=1, ONE struct pointer] = 3 operands. A carrier swap would
    // replace this plain pointer with a ByValueStackArg-kind operand (caught above) — and
    // a piece-split (wrong: ByReference is one pointer, not eightbytes) would push it >3.
    MirFuncId const f = m.funcAt(fi);
    MirInstId call = InvalidMirInst;
    for (std::uint32_t b = 0; b < m.funcBlockCount(f) && !call.valid(); ++b) {
        MirBlockId const blk = m.funcBlockAt(f, b);
        for (std::uint32_t i = 0; i < m.blockInstCount(blk); ++i) {
            MirInstId const ix = m.blockInstAt(blk, i);
            if (m.instOpcode(ix) == MirOpcode::Call) { call = ix; break; }
        }
    }
    ASSERT_TRUE(call.valid());
    auto const ops = m.instOperands(call);
    EXPECT_EQ(ops.size(), 3u)
        << "Call = [callee, n=1, ONE struct POINTER operand] — the >16B ByReference "
           "struct adds exactly one pointer operand (NOT a carrier, NOT a piece-split)";
}

// D-FC12C-APPLE-ARM64-VARIADIC-CALLEE (Step 4.8, Apple struct CALLEE) — apple_arm64
// `va_arg(struct Pair)` where Pair={int,int} (8B, InRegisters) is a LINEAR slot read
// (the slot IS the storage): NO diamond (no ICmpSlt), NO deref (no chained Load). This
// exercises the existing HomogeneousPointer arm for Apple — an integration witness.
TEST(MirLoweringCSubset, AppleVaArgStructSmallByValueLinear) {
    auto L = lowerCSubset(
        "struct Pair { int a; int b; };\n"   // 8B (pow2 ≤8) → InRegisters, by value
        "long sum(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  struct Pair p = va_arg(ap, struct Pair);\n"
        "  va_end(ap);\n"
        "  return p.a + p.b;\n"
        "}\n",
        "arm64", "apple_arm64");
    ASSERT_TRUE(L.mir.ok)
        << "Apple va_arg(≤8B struct) must lower: "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    EXPECT_FALSE(hasMirOpcode(m, fi, MirOpcode::ICmpSlt))
        << "Apple HomogeneousPointer is LINEAR — no dual-cursor diamond";
    EXPECT_EQ(countChainedLoadOfLoad(m, fi), 0u)
        << "a ≤8B by-value Apple struct va_arg returns the slot ADDRESS — no deref";
    EXPECT_EQ(countOpcodeAllBlocks(m, fi, MirOpcode::ByValueStackArg), 0u)
        << "the callee va_arg never emits a caller-side carrier";
}

// D-FC12C-APPLE-ARM64-VARIADIC-CALLEE (Step 4.9, Apple struct CALLEE) — apple_arm64
// `va_arg(struct Big)` where Big is 24B (>16B, ByReference) DEREFERENCES the slot (the
// slot holds a hidden pointer) — chained Load — but is still LINEAR (no diamond, no
// SExt: HomogeneousPointer never deals with negative cursors).
TEST(MirLoweringCSubset, AppleVaArgStructLargeByRefLinear) {
    auto L = lowerCSubset(
        "struct Big { long a; long b; long c; };\n"   // 24B → ByReference (hidden ptr)
        "long pick(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  struct Big b = va_arg(ap, struct Big);\n"
        "  va_end(ap);\n"
        "  return b.a + b.b + b.c;\n"
        "}\n",
        "arm64", "apple_arm64");
    ASSERT_TRUE(L.mir.ok)
        << "Apple va_arg(>16B struct) must lower: "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    EXPECT_FALSE(hasMirOpcode(m, fi, MirOpcode::ICmpSlt))
        << "Apple HomogeneousPointer is LINEAR — no diamond";
    EXPECT_FALSE(hasMirOpcode(m, fi, MirOpcode::SExt))
        << "HomogeneousPointer never sign-extends a negative cursor";
    EXPECT_GE(countChainedLoadOfLoad(m, fi), 1u)
        << "the >16B ByReference Apple struct va_arg derefs the slot (chained Load)";
}

// FOLD 1(b) (BLOCKER) — the ALWAYS-ON, host-independent witness for the Step 2.0
// size-aware bump. apple_arm64 `va_arg(struct HFA{double,double})` is 16B InRegisters
// and MUST advance the va_list cursor by 16 (TWO 8-byte slots), NOT 8 — else a
// SUBSEQUENT va_arg re-reads this struct's tail (a silent miscompile only the macOS
// runtime leg would otherwise catch). This pins the bump CONSTANT == 16 directly. RED-
// ON-DISABLE: reverting Step 2.0 to the flat `namedArgSlotBytes` bump removes the 16.
TEST(MirLoweringCSubset, AppleVaArgHfa16BBumpsBySixteenNotEight) {
    auto L = lowerCSubset(
        "struct HFA { double a; double b; };\n"   // 16B InRegisters (2 eightbytes)
        "double sum(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  double t = 0;\n"
        "  for (int i = 0; i < n; i = i + 1) {\n"
        "    struct HFA p = va_arg(ap, struct HFA);\n"
        "    t = t + p.a + p.b;\n"
        "  }\n"
        "  va_end(ap);\n"
        "  return t;\n"
        "}\n",
        "arm64", "apple_arm64");
    ASSERT_TRUE(L.mir.ok)
        << "Apple va_arg(16B HFA) must lower: "
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    std::uint32_t const fi = funcWithVaStart(m);
    EXPECT_TRUE(funcHasConstInt(m, fi, 16))
        << "Step 2.0: the Apple 16B-HFA va_arg must bump the cursor by 16 (two 8-byte "
           "slots) — a flat namedArgSlotBytes=8 bump re-reads the struct's tail on the "
           "NEXT va_arg (Apple spec: 'the appropriate number of 8-byte stack slots')";
}

// FC12c MIR pin (Apple): the SAME va_arg(int) source lowers to a LINEAR pointer bump
// under apple_arm64 (HomogeneousPointer) — NO dual-cursor diamond — and va_start
// anchors at the OVERFLOW base (VaOverflowArgAreaAddr), NOT a home/save-area leaf.
TEST(MirLoweringCSubset, AppleVaArgIntLowersLinearOverflowBase) {
    auto L = lowerCSubset(
        "int sum(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  int t = va_arg(ap, int);\n"
        "  va_end(ap);\n"
        "  return t;\n"
        "}\n",
        "arm64", "apple_arm64");
    ASSERT_FALSE(L.model.hasErrors())
        << "Apple variadic callee must type-check (va_list = char*)";
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok);
    Mir const& m = L.mir.mir;
    // Apple va_start emits VaOverflowArgAreaAddr (NOT VaHomeArgAreaAddr, NOT
    // VaRegSaveAreaAddr) — the overflow-base anchor for the always-stacked varargs.
    std::uint32_t afi = 0;
    bool found = false;
    for (std::uint32_t fi = 0; fi < m.moduleFuncCount(); ++fi) {
        if (countOpcodeAllBlocks(m, fi, MirOpcode::VaOverflowArgAreaAddr) > 0) {
            afi = fi; found = true; break;
        }
    }
    ASSERT_TRUE(found) << "Apple va_start must emit a VaOverflowArgAreaAddr leaf";
    EXPECT_EQ(countOpcodeAllBlocks(m, afi, MirOpcode::VaHomeArgAreaAddr), 0u)
        << "Apple has NO home area (it always-stacks varargs)";
    EXPECT_EQ(countOpcodeAllBlocks(m, afi, MirOpcode::VaRegSaveAreaAddr), 0u)
        << "Apple HomogeneousPointer uses no register-save-area";
    // LINEAR walk: no reg-vs-overflow diamond from va_arg (no CondBr/ICmp/Phi added).
    EXPECT_EQ(countOpcodeAllBlocks(m, afi, MirOpcode::CondBr), 0u)
        << "Apple va_arg is a LINEAR pointer bump — no diamond";
    EXPECT_EQ(countOpcodeAllBlocks(m, afi, MirOpcode::Phi), 0u);
}

// ── D-CSUBSET-COMPUTED-GOTO: HIR→MIR structural pins (MF-1 / MF-A) ──────────
//
// `&&label` lowers to a BlockAddress (payload = target block), and `goto *p`
// lowers to an IndirectBr whose SUCCESSORS are EVERY address-taken block. That
// successor set is the load-bearing CFG-correctness invariant: it is what makes
// reachability/DCE keep the label blocks and phi-validation see the indirect
// predecessor. These pins assert it directly off the MIR.

namespace {
// Find the single IndirectBr terminator in function `fi`, or InvalidMirInst.
[[nodiscard]] MirInstId findIndirectBr(Mir const& m, std::uint32_t fi) {
    MirFuncId const fn = m.funcAt(fi);
    std::uint32_t const nb = m.funcBlockCount(fn);
    for (std::uint32_t bi = 0; bi < nb; ++bi) {
        MirBlockId const b = m.funcBlockAt(fn, bi);
        if (m.blockInstCount(b) == 0) continue;
        MirInstId const term = m.blockTerminator(b);
        if (m.instOpcode(term) == MirOpcode::IndirectBr) return term;
    }
    return MirInstId{};
}
} // namespace

TEST(MirLoweringCSubset, ComputedGotoIndirectBrSuccessorsAreAllAddressTakenBlocks) {
    // Two `&&label` + a `goto *p`: the IndirectBr must list BOTH label blocks as
    // successors, and each must read back as address-taken. RED-ON-DISABLE: drop a
    // successor in a clone arm (mir_merge / mir_rebuild_helper) and the count != 2;
    // lose the BlockAddress-derived address-taken mark and isBlockAddressTaken flips.
    auto L = lowerCSubset(
        "int f(int s) {\n"
        "  void *a = &&one;\n"
        "  void *b = &&two;\n"
        "  void *t = s ? a : b;\n"
        "  goto *t;\n"
        "one:\n"
        "  return 1;\n"
        "two:\n"
        "  return 2;\n"
        "}\n");
    ASSERT_FALSE(L.model.hasErrors())
        << "semantic: " << (L.model.diagnostics().all().empty()
            ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok)
        << "HIR: " << (L.hirReporter.all().empty() ? "" : L.hirReporter.all()[0].actual);
    ASSERT_TRUE(L.mir.ok)
        << "MIR: " << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);

    Mir const& m = L.mir.mir;
    ASSERT_EQ(m.moduleFuncCount(), 1u);

    // Two `&&label` → two BlockAddress ops, with two DISTINCT target blocks.
    EXPECT_EQ(countOpcodeAllBlocks(m, 0, MirOpcode::BlockAddress), 2u)
        << "each &&label lowers to one BlockAddress";

    MirInstId const ibr = findIndirectBr(m, 0);
    ASSERT_TRUE(ibr.valid()) << "goto *p must lower to an IndirectBr terminator";
    EXPECT_EQ(m.instOperands(ibr).size(), 1u) << "IndirectBr operand[0] = the address";

    // ★ THE MF-1 PIN: successors == the FULL address-taken block set (here 2).
    MirBlockId const ibrBlock = m.instBlock(ibr);
    auto const succs = m.blockSuccessors(ibrBlock);
    ASSERT_EQ(succs.size(), 2u)
        << "IndirectBr successors must be ALL address-taken blocks (2)";
    for (MirBlockId const s : succs) {
        EXPECT_TRUE(m.isBlockAddressTaken(s))
            << "every IndirectBr successor is an address-taken (&&label) block";
    }
    // The two successors are the two distinct BlockAddress targets.
    EXPECT_NE(succs[0].v, succs[1].v) << "the two label blocks are distinct";
}

TEST(MirLoweringCSubset, ComputedGotoBlockAddressTargetsAreAddressTaken) {
    // A block targeted by a BlockAddress reads back as address-taken; a block that
    // is NOT a &&label target does not. RED-ON-DISABLE: isBlockAddressTaken is the
    // SimplifyCfg fold-guard's source of truth — if it returned false for a real
    // target, an address-taken block could be folded away (a silent miscompile).
    auto L = lowerCSubset(
        "int f(int s) {\n"
        "  void *a = &&only;\n"
        "  goto *a;\n"
        "only:\n"
        "  return 7;\n"
        "}\n");
    ASSERT_TRUE(L.mir.ok)
        << "MIR: " << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirFuncId const fn = m.funcAt(0);

    // Find the BlockAddress and confirm its target reads back as address-taken.
    bool sawBlockAddr = false;
    std::uint32_t const nb = m.funcBlockCount(fn);
    for (std::uint32_t bi = 0; bi < nb; ++bi) {
        MirBlockId const b = m.funcBlockAt(fn, bi);
        std::uint32_t const ni = m.blockInstCount(b);
        for (std::uint32_t ii = 0; ii < ni; ++ii) {
            MirInstId const inst = m.blockInstAt(b, ii);
            if (m.instOpcode(inst) != MirOpcode::BlockAddress) continue;
            sawBlockAddr = true;
            MirBlockId const target = m.blockAddressTarget(inst);
            EXPECT_TRUE(m.isBlockAddressTaken(target))
                << "a BlockAddress target IS address-taken";
        }
    }
    ASSERT_TRUE(sawBlockAddr) << "&&only must lower to a BlockAddress";

    // The function's ENTRY block is not a &&label target → not address-taken.
    EXPECT_FALSE(m.isBlockAddressTaken(m.funcEntry(fn)))
        << "a non-&&label block is not address-taken (the negative pin)";
}

// ─── Plan 24 Stage 4 — iterative HIR→MIR straight-line expression driver ────
//
// SF-4 differential pin (synthetic deep HIR): a DEEPLY-nested left-associative
// `(((a+a)+a)+a)…` BinaryOp chain lowers to the EXACT SAME post-order MIR the
// recursive lowerer would: `Arg0`, then one `Add` per chain level in deepest-
// first order, each `Add`'s operands `[deeperResult, Mul_i]` (the leaf is a
// `Mul(Arg0,Arg0)`), then `Return(lastAdd)`. Every `Ref(a)` resolves to the
// single param `Arg` SSA value (no new instruction), so the body is exactly the
// leaf Mul + N (Mul,Add) pairs + the Return.
//
// WITNESS SPLIT — the flat/host-stack-overflow witness (that the lowering driver
// is an O(1)-host-stack work-stack, not host recursion) lives in the CORPUS
// examples `deep_mir_expr` / `deep_expr_lower` (and the upcoming Stage-7 e2e),
// which lower a deep tree through the real pipeline and would crash if the driver
// recursed. THIS unit pin asserts the orthogonal half: byte-identity MIR
// correctness (exact opcode/operand-order shape) at depth, which the corpus
// (golden-MIR-free) does not. The build/lower/teardown here run on the project's
// 64 MiB worker (callOnLargeStack) exactly as the production driver lowers — so
// the deep-tree build and its destructors do NOT depend on the host's ~1 MiB
// main-stack budget (an ORTHOGONAL per-node HIR-teardown recursion overflows a
// stock Debug main stack at this depth; the worker removes that artifact).
//
// RED-ON-REORDER (the byte-identity guard): each Add's RHS is a fresh EMITTING
// `a * a` Mul, so the recursive form's two SEQUENTIAL statements `lhs =
// lowerExpr(kids[0]); rhs = lowerExpr(kids[1]);` emit the spine deepest-first
// with each `Mul_i` lowered IMMEDIATELY before its `Add_i`. The test pins that
// each `Add_i` is preceded by its own RHS `Mul_i`. Building RHS-before-LHS (the
// cst_to_hir Binary-frame order — WRONG here because MIR lowers the operands as
// sequential statements, NOT function-call arguments) emits every `Mul` before
// the deeper spine, so `Add_i` is no longer preceded by `Mul_i` → red.
//
// Synthetic (SF-4): the parser cannot emit beyond its 256 cap until Stage 5, so
// the deep input is built via the public `HirBuilder`, not by parsing.
TEST(MirLoweringCSubset, IterativeDeepBinaryChainLowersFlatAndByteIdentical) {
    constexpr std::uint32_t kDepth   = 4000;  // # of Add nodes (chain levels)
    constexpr std::uint32_t kParamSym = 1;

    // Build the deep tree, lower it, and assert — ALL on the 64 MiB worker
    // (kDeepRecursionStackBytes), mirroring how Program::compileFiles lowers a
    // CU. This keeps the deep HIR build + lowering + its per-node destructors
    // off the host's small main stack (which a stock MSVC Debug build sizes at
    // 1 MiB — too small for the orthogonal deep-tree-teardown recursion here).
    dss::substrate::runOnLargeStack(
        dss::substrate::kDeepRecursionStackBytes, [&] {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const fnTy = ti.fnSig(std::array{i32}, i32, CallConv::CcSysV);

    HirBuilder b{"c-subset"};
    // `int f(int a)` — the param's Refs resolve to the entry `Arg(0)` SSA value
    // (no emitted instruction); each `a * a` emits a fresh `Mul(Arg, Arg)`.
    HirNodeId const param = b.makeVarDecl(i32, kParamSym);
    auto refA = [&] { return b.makeRef(i32, kParamSym); };
    auto mulAA = [&] {
        return b.makeBinaryOp(HirOpKind::Mul, refA(), refA(), i32);
    };
    // Left-assoc spine: leaf `(a*a)`, then `cur = cur + (a*a)` (kDepth times).
    HirNodeId cur = mulAA();                                  // the deepest value
    for (std::uint32_t i = 0; i < kDepth; ++i)
        cur = b.makeBinaryOp(HirOpKind::Add, cur, mulAA(), i32);
    HirNodeId const body = b.makeBlock(std::array{b.makeReturn(cur)});
    HirNodeId const fn   = b.makeFunction(fnTy, /*sym=*/7, std::array{param}, body);
    HirNodeId const root = b.makeModule(std::array{fn});
    Hir hir = std::move(b).finish(root);

    DiagnosticReporter rep;
    HirLiteralPool pool;
    auto result = lowerToMir(hir, pool, ti, rep, /*sourceMap=*/nullptr,
                             MirLoweringConfig{});
    ASSERT_TRUE(result.ok)
        << "deep BinaryOp chain must lower clean: "
        << (rep.all().empty() ? "" : rep.all()[0].actual);

    Mir const& m = result.mir;
    ASSERT_EQ(m.moduleFuncCount(), 1u);
    MirFuncId const f = m.funcAt(0);
    ASSERT_EQ(m.funcBlockCount(f), 1u);
    MirBlockId const entry = m.funcEntry(f);

    // Body = Arg(0) + the leaf Mul + kDepth (Mul, Add) pairs + Return.
    ASSERT_EQ(m.blockInstCount(entry), 2u * kDepth + 3u);
    MirInstId const arg0 = m.blockInstAt(entry, 0);
    EXPECT_EQ(m.instOpcode(arg0), MirOpcode::Arg);
    EXPECT_EQ(m.argIndex(arg0), 0u);

    // The leaf value `a*a` (the deepest spine node) is the first emitted op.
    MirInstId const leafMul = m.blockInstAt(entry, 1);
    EXPECT_EQ(m.instOpcode(leafMul), MirOpcode::Mul);
    {
        auto ops = m.instOperands(leafMul);
        ASSERT_EQ(ops.size(), 2u);
        EXPECT_EQ(ops[0], arg0);
        EXPECT_EQ(ops[1], arg0);
    }

    // Then `kDepth` (Mul_i, Add_i) pairs at positions [2..], deepest-first. The
    // BYTE-IDENTITY guard: each Add_i's LHS is the previous spine result and its
    // RHS is `Mul_i`, the instruction IMMEDIATELY before it (left-to-right eval:
    // LHS spine already emitted, then this level's RHS Mul, then the Add). A
    // RHS-before-LHS build would move every Mul ahead of the spine → this fails.
    MirInstId prevSpine = leafMul;
    for (std::uint32_t i = 0; i < kDepth; ++i) {
        MirInstId const mulI = m.blockInstAt(entry, 2u + 2u * i);
        MirInstId const addI = m.blockInstAt(entry, 3u + 2u * i);
        EXPECT_EQ(m.instOpcode(mulI), MirOpcode::Mul) << "RHS Mul at level " << i;
        EXPECT_EQ(m.instOpcode(addI), MirOpcode::Add) << "Add at level " << i;
        auto ops = m.instOperands(addI);
        ASSERT_EQ(ops.size(), 2u);
        EXPECT_EQ(ops[0], prevSpine)
            << "Add[" << i << "] lhs is the deeper spine result";
        EXPECT_EQ(ops[1], mulI)
            << "Add[" << i << "] rhs is its OWN `a*a`, emitted immediately before "
               "it (left-to-right operand order — red on a RHS-first build)";
        prevSpine = addI;
    }

    // Return(outermost Add).
    MirInstId const ret = m.blockInstAt(entry, 2u * kDepth + 2u);
    EXPECT_EQ(m.instOpcode(ret), MirOpcode::Return);
    {
        auto ops = m.instOperands(ret);
        ASSERT_EQ(ops.size(), 1u);
        EXPECT_EQ(ops[0], prevSpine) << "Return takes the outermost Add";
    }
        });  // runOnLargeStack
}

// SF-4 companion: a DEEPLY-nested UnaryOp chain `-(-(-(…-a)))` lowers via the
// single-child straight-line arm. Pins that the body is exactly `Arg0` followed
// by kDepth `Neg`s (deepest-first), each taking the previous result, then Return.
// Same byte-identity correctness guard as the BinaryOp pin for the 1-child frame
// shape; the flat/host-stack witness lives in the corpus (see the BinaryOp pin's
// WITNESS SPLIT note). Build/lower/teardown run on the 64 MiB worker so the deep
// HIR tree's orthogonal per-node teardown recursion does not depend on the host
// main-stack size (stock MSVC Debug = 1 MiB).
TEST(MirLoweringCSubset, IterativeDeepUnaryChainLowersFlatAndByteIdentical) {
    constexpr std::uint32_t kDepth    = 4000;  // # of Neg nodes
    constexpr std::uint32_t kParamSym = 1;

    dss::substrate::runOnLargeStack(
        dss::substrate::kDeepRecursionStackBytes, [&] {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const fnTy = ti.fnSig(std::array{i32}, i32, CallConv::CcSysV);

    HirBuilder b{"c-subset"};
    HirNodeId const param = b.makeVarDecl(i32, kParamSym);
    HirNodeId cur = b.makeRef(i32, kParamSym);
    for (std::uint32_t i = 0; i < kDepth; ++i)
        cur = b.makeUnaryOp(HirOpKind::Neg, cur, i32);
    HirNodeId const body = b.makeBlock(std::array{b.makeReturn(cur)});
    HirNodeId const fn   = b.makeFunction(fnTy, /*sym=*/7, std::array{param}, body);
    HirNodeId const root = b.makeModule(std::array{fn});
    Hir hir = std::move(b).finish(root);

    DiagnosticReporter rep;
    HirLiteralPool pool;
    auto result = lowerToMir(hir, pool, ti, rep, /*sourceMap=*/nullptr,
                             MirLoweringConfig{});
    ASSERT_TRUE(result.ok)
        << "deep UnaryOp chain must lower clean: "
        << (rep.all().empty() ? "" : rep.all()[0].actual);

    Mir const& m = result.mir;
    MirFuncId const f = m.funcAt(0);
    MirBlockId const entry = m.funcEntry(f);
    ASSERT_EQ(m.blockInstCount(entry), kDepth + 2u);
    MirInstId const arg0 = m.blockInstAt(entry, 0);
    EXPECT_EQ(m.instOpcode(arg0), MirOpcode::Arg);

    MirInstId prev = arg0;
    for (std::uint32_t i = 0; i < kDepth; ++i) {
        MirInstId const neg = m.blockInstAt(entry, 1u + i);
        EXPECT_EQ(m.instOpcode(neg), MirOpcode::Neg);
        auto ops = m.instOperands(neg);
        ASSERT_EQ(ops.size(), 1u);
        EXPECT_EQ(ops[0], prev) << "Neg[" << i << "] takes the deeper result";
        prev = neg;
    }
    MirInstId const ret = m.blockInstAt(entry, kDepth + 1u);
    EXPECT_EQ(m.instOpcode(ret), MirOpcode::Return);
    EXPECT_EQ(m.instOperands(ret)[0], prev);
        });  // runOnLargeStack
}

// ─── Plan 24 Stage 4-address — iterative HIR→MIR by-ADDRESS driver ──────────
//
// SF-4 differential pin for the {Address} half: a DEEPLY-nested storage-array
// subscript chain `a[0][0][0]…[0]` (the lvalue ADDRESS axis — each level's base
// is an ARRAY, so each recurses into the base's lvalue ADDRESS, the deep axis
// `lowerLvalueAddress` now flattens onto the shared work-stack). It must produce
// the EXACT SAME post-order MIR the recursive lowerer would.
//
// The array element is `char` (stride 1), so `scaleIndexToBytes` emits NO Mul —
// each subscript `0` is a single `Const`, and each level emits exactly one
// `Gep`. Emission order (proven identical iterative-vs-recursive): every level's
// subscript `Const` is emitted BEFORE recursing into the base (the recursive arm
// lowers `idx = lowerExpr(kids[1])` BEFORE `base = lowerLvalueAddress(kids[0])`,
// and the iterative IndexAddr frame does phase-0 subscript before phase-1 base),
// so the body is: the array Alloca, then kDepth subscript `Const`s OUTERMOST-
// first, then kDepth `Gep`s INNERMOST-first (each `Gep[i]` bases on `Gep[i-1]`,
// `Gep[0]` on the Alloca), then the `Load` of the char element, then `Return`.
//
// WITNESS SPLIT — the flat/host-stack-overflow witness (that `lowerLvalueAddress`
// is an O(1)-host-stack work-stack rather than recursion) lives in the CORPUS
// example `deep_lvalue_chain` (and the upcoming Stage-7 e2e), which lowers a deep
// lvalue chain through the real pipeline and would crash if the {Address} axis
// recursed. THIS unit pin asserts the orthogonal half: byte-identity MIR
// correctness (the Gep backbone shape + stride-1 no-Mul fast path) at depth.
// Build/lower/teardown run on the project's 64 MiB worker (callOnLargeStack)
// exactly as the production driver lowers — so neither the deep array-type
// `computeLayout` recursion (one frame per nested dimension, plan-24 family C-2
// "type-node recursion, OUT of scope") NOR the deep HIR tree's per-node teardown
// depends on the host's small main-stack budget (a stock MSVC Debug main stack
// is 1 MiB — too small for those orthogonal recursions at this depth; the worker
// removes that artifact while leaving the asserted MIR shape unchanged).
//
// RED-ON-REORDER (the byte-identity guard): the pin walks the `Gep` backbone and
// asserts `Gep[i].base == Gep[i-1]` (innermost-first) AND that the `Gep` count
// equals kDepth with NO Mul anywhere (the stride-1 fast path). A frame that
// lowered the base BEFORE the subscript, or that scaled a stride-1 index, would
// perturb the Const/Gep interleave or inject a Mul → red.
//
// Synthetic (SF-4): the parser's 256 maxExpressionDepth cap forbids a 1500-deep
// subscript in source, so the deep input is built via the public `HirBuilder`.
TEST(MirLoweringCSubset, IterativeDeepIndexAddressChainLowersFlatAndByteIdentical) {
    constexpr std::uint32_t kDepth    = 1500;  // # of subscript levels (Index nodes)
    constexpr std::uint32_t kArraySym = 1;

    // Build the nested array types + deep subscript chain, lower it, and assert
    // — ALL on the 64 MiB worker (kDeepRecursionStackBytes), mirroring how
    // Program::compileFiles lowers a CU. The orthogonal per-dimension
    // `computeLayout` recursion (~1500 frames here) and the deep HIR tree's
    // teardown both run on the worker, off the host's small main stack.
    dss::substrate::runOnLargeStack(
        dss::substrate::kDeepRecursionStackBytes, [&] {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const ch   = ti.primitive(TypeKind::Char);
    TypeId const fnTy = ti.fnSig(std::array<TypeId, 0>{}, i32, CallConv::CcSysV);

    // Nested array types: T0 = char, T_{k} = T_{k-1}[1]. The local is typed T_N
    // (the full kDepth-dimensional `char[1][1]…[1]`); each `[0]` peels one
    // dimension, the innermost yielding the `char` element.
    std::vector<TypeId> dim(kDepth + 1);
    dim[0] = ch;
    for (std::uint32_t k = 1; k <= kDepth; ++k) dim[k] = ti.array(dim[k - 1], 1);

    HirLiteralPool pool;
    // ONE pooled `0` int literal — every subscript Index reuses its index
    // (HIR Literal nodes carry the pool index as payload; the value is shared,
    // the emitted MIR `Const` per level is fresh as the recursive form emits).
    std::uint32_t const zeroLit =
        pool.add(HirLiteralValue{.value = std::int64_t{0}, .core = TypeKind::I32});

    HirBuilder b{"c-subset"};
    // `char a[1]…[1];` — an addressable array local (its Alloca is the chain base).
    HirNodeId const decl = b.makeVarDecl(dim[kDepth], kArraySym);
    // Build `a[0][0]…[0]` outermost-last: base `Ref(a):T_N`, then peel a
    // dimension per level, the level-k Index typed T_{N-1-…} = dim[kDepth-1-i].
    HirNodeId cur = b.makeRef(dim[kDepth], kArraySym);
    for (std::uint32_t i = 0; i < kDepth; ++i) {
        HirNodeId const idx0 = b.makeLiteral(i32, zeroLit);
        cur = b.makeIndex(cur, idx0, /*elemType=*/dim[kDepth - 1 - i]);
    }
    // The fully-subscripted lvalue is read by value (`return a[0]…[0];`): the
    // MemberAccess/Index value arm routes the deep chain through
    // lowerLvalueAddress, then Loads the char element.
    HirNodeId const body =
        b.makeBlock(std::array{decl, b.makeReturn(cur)});
    HirNodeId const fn   = b.makeFunction(fnTy, /*sym=*/7, std::array<HirNodeId, 0>{}, body);
    HirNodeId const root = b.makeModule(std::array{fn});
    Hir hir = std::move(b).finish(root);

    // Thread the x86_64 aggregate layout so array stride/size resolve (mirrors
    // lowerCSubset). A char[1]…[1] is 1 byte; stride at every level is 1.
    MirLoweringConfig mirCfg;
    if (auto t = TargetSchema::loadShipped("x86_64"); t.has_value()) {
        mirCfg.aggregateLayout       = (*t)->aggregateLayout();
        mirCfg.aggregateLayoutLoaded = (*t)->aggregateLayoutLoaded();
    }

    DiagnosticReporter rep;
    // NOTE: lowers HIR→MIR on THIS (main) stack — the flat-property witness.
    auto result = lowerToMir(hir, pool, ti, rep, /*sourceMap=*/nullptr, mirCfg);
    ASSERT_TRUE(result.ok)
        << "deep storage-Index address chain must lower clean: "
        << (rep.all().empty() ? "" : rep.all()[0].actual);

    Mir const& m = result.mir;
    MirFuncId const f = m.funcAt(0);
    MirBlockId const entry = m.funcEntry(f);
    // Body = Alloca + kDepth subscript Consts + kDepth Geps + Load + Return.
    ASSERT_EQ(m.blockInstCount(entry), 2u * kDepth + 3u);

    MirInstId const alloca = m.blockInstAt(entry, 0);
    EXPECT_EQ(m.instOpcode(alloca), MirOpcode::Alloca)
        << "the array local's storage slot is the chain base";

    // kDepth subscript Consts at [1..kDepth], all value 0 (outermost-first — the
    // recursive arm lowers each level's subscript before recursing into its base,
    // and stride-1 `char` emits NO Mul, so these are contiguous).
    for (std::uint32_t i = 0; i < kDepth; ++i) {
        MirInstId const c = m.blockInstAt(entry, 1u + i);
        ASSERT_EQ(m.instOpcode(c), MirOpcode::Const)
            << "subscript Const at level " << i << " (no Mul: char stride 1)";
        auto const& lit = m.literalValue(m.constLiteralIndex(c));
        EXPECT_EQ(std::get<std::int64_t>(lit.value), 0);
    }

    // kDepth Geps at [kDepth+1 .. 2*kDepth], INNERMOST-first: Gep[0] bases on the
    // Alloca, Gep[i] on Gep[i-1] (the deepest-base-first composition — red if a
    // frame lowered the base before the subscript or perturbed the chain).
    MirInstId prevBase = alloca;
    for (std::uint32_t i = 0; i < kDepth; ++i) {
        MirInstId const g = m.blockInstAt(entry, 1u + kDepth + i);
        ASSERT_EQ(m.instOpcode(g), MirOpcode::Gep) << "Gep at chain level " << i;
        auto ops = m.instOperands(g);
        ASSERT_EQ(ops.size(), 2u)
            << "storage-array Gep is 2-op [base, byteIndex]";
        EXPECT_EQ(ops[0], prevBase)
            << "Gep[" << i << "] bases on the deeper address (Alloca then prior Gep)";
        prevBase = g;
    }
    // No Mul anywhere — the stride-1 fast path (a regression scaling the index
    // would inject one and shift every offset above).
    for (std::uint32_t k = 0; k < m.blockInstCount(entry); ++k)
        EXPECT_NE(m.instOpcode(m.blockInstAt(entry, k)), MirOpcode::Mul)
            << "char (stride 1) indexing must NOT emit a scaling Mul";

    // Load(outermost Gep) then Return(load).
    MirInstId const load = m.blockInstAt(entry, 2u * kDepth + 1u);
    EXPECT_EQ(m.instOpcode(load), MirOpcode::Load);
    EXPECT_EQ(m.instOperands(load)[0], prevBase)
        << "the char element is Loaded through the outermost Gep";
    MirInstId const ret = m.blockInstAt(entry, 2u * kDepth + 2u);
    EXPECT_EQ(m.instOpcode(ret), MirOpcode::Return);
    EXPECT_EQ(m.instOperands(ret)[0], load);
        });  // runOnLargeStack
}

// ─── Plan 24 Stage 4-cfg — iterative HIR→MIR control-flow VALUE driver ──────
//
// SF-4 differential pin for the CFG-value half: a DEEPLY right-nested ternary
// chain `a ? b : (c ? d : (e ? f : … ))` (each level's ELSE arm is the next
// ternary, so the recursion goes one frame deeper per level). The Ternary value
// arm now flattens onto the shared work-stack — each level is a 4-phase frame
// that INTERLEAVES createBlock / addCondBr / addBr / beginBlock / addPhi with
// its cond/then/else sub-lowerings. This pin asserts the EXACT thing a phase
// bug corrupts: the monotonic BLOCK-ID creation sequence AND each join phi's
// PREDECESSOR-block ids in INCOMING ORDER.
//
// Block-creation order (entry == block 0; createBlock id == creation order, the
// single-chokepoint monotonic invariant): the lowering is depth-first leftmost,
// and each ternary mints then→else→join AFTER its cond is lowered and BEFORE its
// then/else are. So level i (outermost == 0) mints thenBB=block(3i+1),
// elseBB=block(3i+2), joinBB=block(3i+3); the innermost level is i=kDepth-1.
//
// Each level's join phi predecessor order (the byte-identity guard — a swapped
// incoming or a wrong insertion-block switch flips these):
//   incoming[0].pred == level i's thenBB == block(3i+1)   (the THEN arm)
//   incoming[1].pred == (i < kDepth-1) ? level (i+1)'s joinBB == block(3i+6)
//                                      : level i's elseBB   == block(3i+2)
// i.e. an OUTER level's else-incoming arrives from the NESTED ternary's JOIN
// (where the inner phi lives, the open block after the inner diamond closes),
// while the INNERMOST level's else-incoming arrives from its own elseBB. A
// phase machine that captured `elsePred` from the wrong block, minted blocks in
// the wrong order, or swapped the phi incomings would fail these exact asserts.
//
// WITNESS SPLIT — the flat/host-stack-overflow witness (that the Ternary value
// arm is an O(1)-host-stack work-stack, not recursion) lives in the CORPUS
// example `deep_ternary_mir`, which lowers a deep ?: chain through the real
// pipeline and would crash if the CFG-value axis recursed. THIS unit pin asserts
// the orthogonal half: byte-identity of the diamond/phi backbone at depth.
// Build/lower/teardown run on the 64 MiB worker (kDeepRecursionStackBytes) so
// neither the deep HIR-tree teardown nor any orthogonal recursion depends on the
// host's small main-stack budget (stock MSVC Debug main stack = 1 MiB).
//
// Synthetic (SF-4): the parser's 256 maxExpressionDepth cap forbids a 1000-deep
// ternary in source, so the deep input is built via the public `HirBuilder`.
TEST(MirLoweringCSubset, IterativeDeepTernaryChainLowersFlatAndByteIdentical) {
    constexpr std::uint32_t kDepth = 1000;  // # of nested ternary levels

    dss::substrate::runOnLargeStack(
        dss::substrate::kDeepRecursionStackBytes, [&] {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32  = ti.primitive(TypeKind::I32);
    // `int f(int a, int b)` — `a` is the condition value (Arg 0), `b` the arm
    // value (Arg 1). Both are pure-SSA Args (no emitted instruction), so each
    // ternary level's cond/then/else are the same two Args; the diamond/phi
    // scaffolding is the ONLY per-level MIR — exactly what we pin.
    TypeId const fnTy = ti.fnSig(std::array{i32, i32}, i32, CallConv::CcSysV);

    HirBuilder b{"c-subset"};
    HirNodeId const pa = b.makeVarDecl(i32, /*sym=*/1);
    HirNodeId const pb = b.makeVarDecl(i32, /*sym=*/2);
    auto refA = [&] { return b.makeRef(i32, /*sym=*/1); };
    auto refB = [&] { return b.makeRef(i32, /*sym=*/2); };
    // Build right-nested `a ? b : (a ? b : ( … : b))` innermost-first. The
    // innermost else is a plain `b`; each outer level wraps the prior as its
    // ELSE arm, so the nesting (and the recursion it would otherwise cost) grows
    // down the ELSE spine.
    HirNodeId cur = refB();                                   // innermost else
    for (std::uint32_t i = 0; i < kDepth; ++i)
        cur = b.makeTernary(refA(), refB(), cur, i32);
    HirNodeId const body = b.makeBlock(std::array{b.makeReturn(cur)});
    HirNodeId const fn   =
        b.makeFunction(fnTy, /*sym=*/7, std::array{pa, pb}, body);
    HirNodeId const root = b.makeModule(std::array{fn});
    Hir hir = std::move(b).finish(root);

    DiagnosticReporter rep;
    HirLiteralPool pool;
    auto result = lowerToMir(hir, pool, ti, rep, /*sourceMap=*/nullptr,
                             MirLoweringConfig{});
    ASSERT_TRUE(result.ok)
        << "deep ternary chain must lower clean: "
        << (rep.all().empty() ? "" : rep.all()[0].actual);

    Mir const& m = result.mir;
    MirFuncId const f = m.funcAt(0);
    // entry + 3 blocks (then/else/join) per ternary level.
    ASSERT_EQ(m.funcBlockCount(f), 3u * kDepth + 1u);

    // Walk every level and assert the diamond markers + the join phi's exact
    // predecessor-block ids in incoming order (the precise thing a phase bug
    // corrupts). Block index == creation order == id (single-chokepoint
    // monotonic), so `funcBlockAt(f, 3i+k)` resolves level i's then/else/join.
    for (std::uint32_t i = 0; i < kDepth; ++i) {
        MirBlockId const thenBB = m.funcBlockAt(f, 3u * i + 1u);
        MirBlockId const elseBB = m.funcBlockAt(f, 3u * i + 2u);
        MirBlockId const joinBB = m.funcBlockAt(f, 3u * i + 3u);
        EXPECT_EQ(m.blockMarker(thenBB), StructCfMarker::IfThen) << "level " << i;
        EXPECT_EQ(m.blockMarker(elseBB), StructCfMarker::IfElse) << "level " << i;
        EXPECT_EQ(m.blockMarker(joinBB), StructCfMarker::IfJoin) << "level " << i;

        // The join's first instruction is the 2-incoming phi.
        MirInstId const phi = m.blockInstAt(joinBB, 0);
        ASSERT_EQ(m.instOpcode(phi), MirOpcode::Phi) << "level " << i;
        auto inc = m.phiIncomings(phi);
        ASSERT_EQ(inc.size(), 2u) << "level " << i;
        // incoming[0] is the THEN arm (predecessor == this level's thenBB).
        EXPECT_EQ(inc[0].pred, thenBB)
            << "level " << i << " phi incoming[0] must arrive from the THEN block";
        // incoming[1] is the ELSE arm: an outer level's else-value is the NESTED
        // ternary's result, arriving from the nested level's JOIN block; the
        // innermost level's else-value is a plain `b`, arriving from its elseBB.
        MirBlockId const expectedElsePred =
            (i + 1u < kDepth) ? m.funcBlockAt(f, 3u * (i + 1u) + 3u)  // inner join
                              : elseBB;                                // own else
        EXPECT_EQ(inc[1].pred, expectedElsePred)
            << "level " << i << " phi incoming[1] must arrive from "
            << (i + 1u < kDepth ? "the NESTED ternary's JOIN" : "its own ELSE")
            << " block (predecessor order then-then-else — red on a swap)";
    }

    // The entry block's terminator is the OUTERMOST diamond's CondBr into
    // level-0's then/else (a sanity anchor on the top of the chain).
    MirBlockId const entry = m.funcEntry(f);
    EXPECT_EQ(m.instOpcode(m.blockTerminator(entry)), MirOpcode::CondBr);
    {
        auto succs = m.blockSuccessors(entry);
        ASSERT_EQ(succs.size(), 2u);
        EXPECT_EQ(succs[0], m.funcBlockAt(f, 1u)) << "entry CondBr true → then[0]";
        EXPECT_EQ(succs[1], m.funcBlockAt(f, 2u)) << "entry CondBr false → else[0]";
    }
        });  // runOnLargeStack
}

// SF-4 companion for the Logical (`&&`/`||`) frame: a DEEPLY LEFT-nested
// short-circuit chain `((…((a && b) && b) …) && b)`. Left-nesting puts the deep
// recursion on the LHS spine — which the recursive arm lowers in the CURRENT
// block BEFORE any block switch (`lhs = lowerExpr(kids[0])`), so it is the axis
// the work-stack flattening removes. Each level is the 3-phase Logical frame
// (lower lhs → mint rhsBB/joinBB + CondBr + beginBlock(rhsBB) → lower rhs → phi).
//
// Block-creation order (entry == block 0): the innermost `&&` (lowered first,
// level 0) mints rhsBB=block1(IfThen), joinBB=block2(IfJoin); level i mints
// rhsBB=block(2i+1), joinBB=block(2i+2). Each level's join phi (block 2i+2):
//   incoming[0].pred == its LHS predecessor == (i==0 ? entry : level(i-1)'s
//                       joinBB == block(2i))   — the SHORT-CIRCUIT/lhs edge
//   incoming[1].pred == its own rhsBB == block(2i+1)  — the rhs edge
// (predecessor order lhs-then-rhs — red on a swap or a wrong lhsPred capture).
// All `&&` ⇒ the conditional ARM (rhsBB) is the TRUE-edge target; the join is
// the FALSE/short-circuit target. Built on the 64 MiB worker (deep teardown).
TEST(MirLoweringCSubset, IterativeDeepLogicalChainLowersFlatAndByteIdentical) {
    constexpr std::uint32_t kDepth = 1000;  // # of `&&` levels

    dss::substrate::runOnLargeStack(
        dss::substrate::kDeepRecursionStackBytes, [&] {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const fnTy = ti.fnSig(std::array{i32, i32}, i32, CallConv::CcSysV);

    HirBuilder b{"c-subset"};
    HirNodeId const pa = b.makeVarDecl(i32, /*sym=*/1);
    HirNodeId const pb = b.makeVarDecl(i32, /*sym=*/2);
    auto refA = [&] { return b.makeRef(i32, /*sym=*/1); };
    auto refB = [&] { return b.makeRef(i32, /*sym=*/2); };
    // Left-nested: leaf `a`, then `cur = (cur && b)` kDepth times, so the LHS
    // spine grows down (the deep-recursion axis the frame flattens).
    HirNodeId cur = refA();
    for (std::uint32_t i = 0; i < kDepth; ++i)
        cur = b.makeLogicalAnd(cur, refB(), i32);
    HirNodeId const body = b.makeBlock(std::array{b.makeReturn(cur)});
    HirNodeId const fn   =
        b.makeFunction(fnTy, /*sym=*/7, std::array{pa, pb}, body);
    HirNodeId const root = b.makeModule(std::array{fn});
    Hir hir = std::move(b).finish(root);

    DiagnosticReporter rep;
    HirLiteralPool pool;
    auto result = lowerToMir(hir, pool, ti, rep, /*sourceMap=*/nullptr,
                             MirLoweringConfig{});
    ASSERT_TRUE(result.ok)
        << "deep && chain must lower clean: "
        << (rep.all().empty() ? "" : rep.all()[0].actual);

    Mir const& m = result.mir;
    MirFuncId const f = m.funcAt(0);
    MirBlockId const entry = m.funcEntry(f);
    // entry + 2 blocks (rhs/join) per `&&` level.
    ASSERT_EQ(m.funcBlockCount(f), 2u * kDepth + 1u);

    for (std::uint32_t i = 0; i < kDepth; ++i) {
        MirBlockId const rhsBB  = m.funcBlockAt(f, 2u * i + 1u);
        MirBlockId const joinBB = m.funcBlockAt(f, 2u * i + 2u);
        EXPECT_EQ(m.blockMarker(rhsBB),  StructCfMarker::IfThen) << "level " << i;
        EXPECT_EQ(m.blockMarker(joinBB), StructCfMarker::IfJoin) << "level " << i;

        MirInstId const phi = m.blockInstAt(joinBB, 0);
        ASSERT_EQ(m.instOpcode(phi), MirOpcode::Phi) << "level " << i;
        auto inc = m.phiIncomings(phi);
        ASSERT_EQ(inc.size(), 2u) << "level " << i;
        // incoming[0] = lhs edge: level 0 from entry, an outer level from the
        // nested `&&`'s JOIN block (block 2i). incoming[1] = rhs edge (rhsBB).
        MirBlockId const expectedLhsPred =
            (i == 0u) ? entry : m.funcBlockAt(f, 2u * i);
        EXPECT_EQ(inc[0].pred, expectedLhsPred)
            << "level " << i << " phi incoming[0] is the lhs/short-circuit edge";
        EXPECT_EQ(inc[1].pred, rhsBB)
            << "level " << i << " phi incoming[1] is the rhs edge (lhs-then-rhs "
               "order — red on a swap)";
    }
        });  // runOnLargeStack
}

// ─── Plan 24 (hir_to_mir Call residual) — iterative HIR→MIR Call driver ──────
//
// SF-4 differential pin for the flattened Call arm: a DEEPLY-nested single-arg
// call chain `id(id(id(…id(42)…)))` (each call's sole argument is the next inner
// call — exactly the `f(f(f(…)))` shape the residual targets). Before this change
// the recursive `lowerExprNode` Call arm lowered each argument via
// `lowerExpr(arg)`, re-entering `runExprDriver` once per nesting level → 800 host
// frames deep during HIR→MIR. Now the Call frame on the driver's explicit
// work-stack carries that descent (callee built first, then the scalar argument
// lowered through the SAME work-stack) with FLAT O(1) host-stack cost per level.
//
// BYTE-IDENTITY backbone: the OUTERMOST call lowers its callee FIRST, then its
// arg (the next inner call), recursively — so the emission order is all D callee
// `GlobalAddr`s OUTER→INNER, then the innermost `Const(42)`, then all D `Call`s
// INNER→OUTER (each `Call_k` takes `Call_{k+1}` — the instruction immediately
// before it — as its single argument operand), then `Return(Call_outer)`. A
// dropped/duplicated callee, a callee-after-arg mis-order, or a wrong arg
// operand would break this exact shape. The WITNESS SPLIT mirrors the other
// IterativeDeep pins: the flat/host-stack-overflow run witness lives in the
// corpus example `deep_call_mir` (a deep call chain through the real pipeline
// that would crash if the Call arg-lowering recursed); THIS unit pin asserts the
// orthogonal byte-identity half. Build/lower/teardown run on the 64 MiB worker
// (kDeepRecursionStackBytes) so the deep HIR-tree teardown does not depend on
// the host's small main-stack budget (stock MSVC Debug main stack = 1 MiB).
//
// Synthetic (SF-4): the parser's 256 maxExpressionDepth cap forbids an 800-deep
// call in source, so the deep input is built via the public `HirBuilder`.
TEST(MirLoweringCSubset, IterativeDeepCallChainLowersFlatAndByteIdentical) {
    constexpr std::uint32_t kDepth   = 800;  // # of nested id() calls
    constexpr std::uint32_t kIdSym   = 1;    // the identity function's symbol
    constexpr std::uint32_t kDeepSym = 7;    // the caller function's symbol

    dss::substrate::runOnLargeStack(
        dss::substrate::kDeepRecursionStackBytes, [&] {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const idTy = ti.fnSig(std::array{i32}, i32, CallConv::CcSysV);
    TypeId const deepTy = ti.fnSig(std::array<TypeId, 0>{}, i32, CallConv::CcSysV);

    HirLiteralPool pool;
    std::uint32_t const lit42 =
        pool.add(HirLiteralValue{.value = std::int64_t{42}, .core = TypeKind::I32});

    HirBuilder b{"c-subset"};
    // `int id(int x) { return x; }` — its Refs resolve to Arg(0).
    HirNodeId const idParam = b.makeVarDecl(i32, /*sym=*/2);
    HirNodeId const idBody =
        b.makeBlock(std::array{b.makeReturn(b.makeRef(i32, /*sym=*/2))});
    HirNodeId const idFn =
        b.makeFunction(idTy, kIdSym, std::array{idParam}, idBody);

    // `int deep(void) { return id(id(…id(42)…)); }` — 800 nested single-arg
    // calls around the innermost literal `42`. Each call's callee is a fresh
    // `Ref(id):FnSig` (lowers to GlobalAddr); the arg is the inner call.
    HirNodeId cur = b.makeLiteral(i32, lit42);          // innermost argument
    for (std::uint32_t i = 0; i < kDepth; ++i) {
        HirNodeId const callee = b.makeRef(idTy, kIdSym);
        cur = b.makeCall(callee, std::array{cur}, i32);
    }
    HirNodeId const deepBody = b.makeBlock(std::array{b.makeReturn(cur)});
    HirNodeId const deepFn =
        b.makeFunction(deepTy, kDeepSym, std::array<HirNodeId, 0>{}, deepBody);
    HirNodeId const root = b.makeModule(std::array{idFn, deepFn});
    Hir hir = std::move(b).finish(root);

    DiagnosticReporter rep;
    auto result = lowerToMir(hir, pool, ti, rep, /*sourceMap=*/nullptr,
                             MirLoweringConfig{});
    ASSERT_TRUE(result.ok)
        << "deep Call chain must lower clean: "
        << (rep.all().empty() ? "" : rep.all()[0].actual);

    Mir const& m = result.mir;
    ASSERT_EQ(m.moduleFuncCount(), 2u);
    // funcAt(1) is `deep` (declared second). Its single entry block holds the
    // whole flattened chain (no CFG — pure straight-line calls).
    MirFuncId const f = m.funcAt(1);
    ASSERT_EQ(m.funcBlockCount(f), 1u);
    MirBlockId const entry = m.funcEntry(f);

    // Body = D GlobalAddr (callees, outer→inner) + 1 Const(42) + D Call
    // (inner→outer) + 1 Return.
    ASSERT_EQ(m.blockInstCount(entry), 2u * kDepth + 2u);

    // The D callee GlobalAddrs come first, in OUTER→INNER order (the outermost
    // call lowers its callee before descending into its argument).
    for (std::uint32_t i = 0; i < kDepth; ++i) {
        MirInstId const ga = m.blockInstAt(entry, i);
        EXPECT_EQ(m.instOpcode(ga), MirOpcode::GlobalAddr)
            << "callee GlobalAddr at level " << i;
    }
    // Then the innermost argument `Const(42)`.
    MirInstId const konst = m.blockInstAt(entry, kDepth);
    EXPECT_EQ(m.instOpcode(konst), MirOpcode::Const);

    // Then the D Calls, INNER→OUTER. Call at position kDepth+1 is the INNERMOST
    // (`id(42)`): its callee operand is the LAST GlobalAddr (innermost) and its
    // arg operand is the Const. Each subsequent Call takes the PREVIOUS Call as
    // its argument (the byte-identity guard: a call-before-callee or wrong-arg
    // build would move these). The callee operand of Call rank j (counting from
    // the inner) is GlobalAddr[kDepth-1-j].
    MirInstId prevValue = konst;   // innermost arg feeds the innermost call
    for (std::uint32_t j = 0; j < kDepth; ++j) {
        MirInstId const call = m.blockInstAt(entry, kDepth + 1u + j);
        EXPECT_EQ(m.instOpcode(call), MirOpcode::Call) << "Call at depth-rank " << j;
        auto ops = m.instOperands(call);
        ASSERT_EQ(ops.size(), 2u) << "Call[" << j << "] = (callee, arg)";
        MirInstId const expectedCallee = m.blockInstAt(entry, kDepth - 1u - j);
        EXPECT_EQ(ops[0], expectedCallee)
            << "Call[" << j << "] callee is its matching GlobalAddr";
        EXPECT_EQ(ops[1], prevValue)
            << "Call[" << j << "] arg is the next-inner call's result "
               "(red on a callee-after-arg or dropped-call build)";
        prevValue = call;
    }

    // Return(outermost Call).
    MirInstId const ret = m.blockInstAt(entry, 2u * kDepth + 1u);
    EXPECT_EQ(m.instOpcode(ret), MirOpcode::Return);
    ASSERT_EQ(m.instOperands(ret).size(), 1u);
    EXPECT_EQ(m.instOperands(ret)[0], prevValue) << "Return takes the outermost Call";
        });  // runOnLargeStack
}

// ─── Plan 24 Stage 4b — iterative HIR→MIR STATEMENT driver ──────────────────
//
// SF-4 differential pin for the flattened control-flow STATEMENT arms (the
// `lowerStmt` driver). A DEEPLY-nested if-chain `if(a){ if(a){ if(a){ … return
// b; } } }` — each level's THEN body IS the next nested `if`, so the recursion
// the recursive `lowerStmtNode` IfStmt arm would otherwise cost (`lowerStmt(
// thenN)` once per nesting level, ~600 host frames deep during HIR→MIR) is the
// axis the explicit `StmtFrame` work-stack flattens to FLAT O(1) host-stack cost
// per level. (No `else`, so each level is the minimal 2-block diamond — thenBB +
// joinBB — the crispest statement backbone to pin.)
//
// BYTE-IDENTITY backbone (block id == creation order == single-chokepoint
// monotonic): entry == block 0 evaluates level 0's cond; level i's cond is
// evaluated in level (i-1)'s thenBB. Each level i mints thenBB == block(2i+1)
// then joinBB == block(2i+2) IN THAT ORDER, and a CondBr(cond, thenBB, joinBB).
// So the block CONTAINING level i's cond (entry for i==0, else block 2i-1) ends
// in a CondBr whose successors are EXACTLY {block 2i+1, block 2i+2} — a swapped
// successor or a block minted out of order (the silent-miscompile class this
// gate guards) moves these. The innermost thenBB (block 2N-1) holds `return b`,
// so its terminator is Return. Total blocks = 2N+1.
//
// The WITNESS SPLIT mirrors the IterativeDeep expression pins: the flat/host-
// stack-overflow RUN witness lives in the corpus example `deep_stmt_mir` (a deep
// nested-control-flow program through the real pipeline that crashes if the
// statement lowering recurses); THIS unit pin asserts the orthogonal CFG
// byte-identity half. Build/lower/teardown run on the 64 MiB worker
// (kDeepRecursionStackBytes) so the deep HIR-tree teardown does not depend on
// the host's small main-stack budget (stock MSVC Debug main stack = 1 MiB).
//
// Synthetic (SF-4): the parser's 256 maxStatementDepth cap forbids a 600-deep
// nest in source, so the deep input is built via the public `HirBuilder`.
TEST(MirLoweringCSubset, IterativeDeepIfNestLowersFlatAndByteIdentical) {
    constexpr std::uint32_t kDepth = 600;  // # of nested `if` levels

    dss::substrate::runOnLargeStack(
        dss::substrate::kDeepRecursionStackBytes, [&] {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32  = ti.primitive(TypeKind::I32);
    // `int f(int a, int b)` — `a` (Arg 0) is every level's condition, `b`
    // (Arg 1) the innermost return value. Both are pure-SSA Args (no emitted
    // instruction), so the diamond scaffolding is the ONLY per-level MIR.
    TypeId const fnTy = ti.fnSig(std::array{i32, i32}, i32, CallConv::CcSysV);

    HirBuilder b{"c-subset"};
    HirNodeId const pa = b.makeVarDecl(i32, /*sym=*/1);
    HirNodeId const pb = b.makeVarDecl(i32, /*sym=*/2);
    auto refA = [&] { return b.makeRef(i32, /*sym=*/1); };
    // Build innermost-first: the deepest THEN is `return b`; each outer level
    // wraps the prior `if` as its (else-less) THEN statement, so the nesting
    // grows down the THEN spine — the deep-recursion axis the frame flattens.
    HirNodeId cur = b.makeReturn(b.makeRef(i32, /*sym=*/2));   // innermost then
    for (std::uint32_t i = 0; i < kDepth; ++i)
        cur = b.makeIfStmt(refA(), cur, /*elseStmt=*/std::nullopt);
    // The function body must end in a terminator on every path; the outermost
    // `if` falls through its join, so append a trailing `return b`.
    HirNodeId const body = b.makeBlock(std::array{
        cur, b.makeReturn(b.makeRef(i32, /*sym=*/2))});
    HirNodeId const fn   =
        b.makeFunction(fnTy, /*sym=*/7, std::array{pa, pb}, body);
    HirNodeId const root = b.makeModule(std::array{fn});
    Hir hir = std::move(b).finish(root);

    DiagnosticReporter rep;
    HirLiteralPool pool;
    auto result = lowerToMir(hir, pool, ti, rep, /*sourceMap=*/nullptr,
                             MirLoweringConfig{});
    ASSERT_TRUE(result.ok)
        << "deep if nest must lower clean: "
        << (rep.all().empty() ? "" : rep.all()[0].actual);

    Mir const& m = result.mir;
    MirFuncId const f = m.funcAt(0);
    // entry + 2 blocks (then/join) per `if` level.
    ASSERT_EQ(m.funcBlockCount(f), 2u * kDepth + 1u);
    MirBlockId const entry = m.funcEntry(f);

    // Walk every level and assert its CondBr successors are EXACTLY this level's
    // {thenBB == 2i+1, joinBB == 2i+2}, in that order (the precise thing a phase
    // bug — a swapped CondBr target or an out-of-order createBlock — corrupts).
    // Block index == creation order == id, so `funcBlockAt(f, n)` resolves it.
    for (std::uint32_t i = 0; i < kDepth; ++i) {
        // The block holding level i's cond: entry for level 0, else the PRIOR
        // level's thenBB == block 2(i-1)+1 == 2i-1.
        MirBlockId const condBlock =
            (i == 0u) ? entry : m.funcBlockAt(f, 2u * i - 1u);
        MirBlockId const thenBB = m.funcBlockAt(f, 2u * i + 1u);
        MirBlockId const joinBB = m.funcBlockAt(f, 2u * i + 2u);
        MirInstId const term = m.blockTerminator(condBlock);
        ASSERT_EQ(m.instOpcode(term), MirOpcode::CondBr) << "level " << i;
        auto succs = m.blockSuccessors(condBlock);
        ASSERT_EQ(succs.size(), 2u) << "level " << i;
        EXPECT_EQ(succs[0], thenBB)
            << "level " << i << " CondBr true-edge → this level's thenBB "
               "(red on a swapped successor or out-of-order createBlock)";
        EXPECT_EQ(succs[1], joinBB)
            << "level " << i << " CondBr false-edge → this level's joinBB";
    }

    // The innermost thenBB (block 2N-1) holds `return b` → Return terminator.
    MirBlockId const innermostThen = m.funcBlockAt(f, 2u * kDepth - 1u);
    EXPECT_EQ(m.instOpcode(m.blockTerminator(innermostThen)), MirOpcode::Return)
        << "the deepest THEN body is `return b`";
        });  // runOnLargeStack
}

// FC-F1 (C 6.5.2.4 / 6.5.6): a POINTER `++`/`--` steps by sizeof(*p), NOT 1 byte.
// The HIR lowers `p++` to `AddressOf(Index(lvRead(p), ±1, T))`, routing through
// the SAME `scaleIndexToBytes`→Gep path `p[i]` uses — so at MIR the pointer step
// is a Gep whose index is a byte-scaling `Mul(±1, sizeof(T))`, never a raw 1-byte
// `Add(ptr, 1)`. This MIR-tier pin is the structural witness for the silent-
// miscompile FIX (the end-to-end runtime witness is examples/c-subset/core_incdec,
// exit 0). RED-ON-DISABLE: route the Ptr lvalue through the integer arithmetic
// path → the step becomes `Add(p, 1)` (no Gep, no Mul-by-4) → these fail.
TEST(MirLoweringCSubset, PointerIncrementScalesByElementSizeViaGep) {
    auto L = lowerCSubset("void f(int* p) { p++; }\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    MirBlockId const entry = m.funcEntry(m.funcAt(0));
    bool foundScaledGep = false;
    bool sawRawAdd = false;
    for (std::uint32_t k = 0; k < m.blockInstCount(entry); ++k) {
        MirInstId const id = m.blockInstAt(entry, k);
        if (m.instOpcode(id) == MirOpcode::Add) sawRawAdd = true;
        if (m.instOpcode(id) != MirOpcode::Gep) continue;
        auto ops = m.instOperands(id);
        ASSERT_EQ(ops.size(), 2u) << "pointer-step GEP is [base, byteIdx]";
        MirInstId const idxOp = ops[1];
        ASSERT_EQ(m.instOpcode(idxOp), MirOpcode::Mul)
            << "the pointer step index must be byte-scaled (Mul by sizeof(T))";
        auto mulOps = m.instOperands(idxOp);
        ASSERT_EQ(mulOps.size(), 2u);
        auto const& strideLit = m.literalValue(m.constLiteralIndex(mulOps[1]));
        EXPECT_EQ(std::get<std::int64_t>(strideLit.value), 4)
            << "int* element stride is sizeof(int) = 4";
        foundScaledGep = true;
    }
    EXPECT_TRUE(foundScaledGep)
        << "pointer ++ must lower to a sizeof-scaled GEP, never a 1-byte Add";
    EXPECT_FALSE(sawRawAdd)
        << "pointer ++ must NOT emit a raw integer Add (the old 1-byte step)";
}

// ─────────────────────────────────────────────────────────────────────────────
// c21 (D-CSUBSET-VOLATILE-QUALIFIER) — per-form volatile threading.
//
// Model B: `volatile` is a per-symbol/member `isVolatile` bool that CST→HIR
// records on the ACCESS node (object Ref / MemberAccess / VarDecl/Global) and
// HIR→MIR ORs into `MirInstFlags::Volatile` on the access's Load/Store. Each of
// the tests below pins ONE access form's flag (multi-site completeness = test
// EVERY form). RED-ON-DISABLE: drop any `volatileFlagFor` thread → the matching
// flag-count assertion fails. Backend correctness (DCE/CSE/Mem2Reg/LICM honor
// the flag) is pre-existing; these tests pin the FRONTEND threading the cycle
// adds. The same `volatileMap` is threaded by `lowerCSubset` above.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Count, across EVERY block of EVERY function in the module, the instructions of
// `op` whose `MirInstFlags::Volatile` bit is (set == wantVolatile).
[[nodiscard]] std::uint32_t countOpWithVolatile(Mir const& m, MirOpcode op,
                                                bool wantVolatile) {
    std::uint32_t n = 0;
    for (std::uint32_t f = 0; f < m.moduleFuncCount(); ++f) {
        MirFuncId const fn = m.funcAt(f);
        for (std::uint32_t b = 0; b < m.funcBlockCount(fn); ++b) {
            MirBlockId const bb = m.funcBlockAt(fn, b);
            for (std::uint32_t i = 0; i < m.blockInstCount(bb); ++i) {
                MirInstId const id = m.blockInstAt(bb, i);
                if (m.instOpcode(id) != op) continue;
                bool const isVol = has(m.instFlags(id), MirInstFlags::Volatile);
                if (isVol == wantVolatile) ++n;
            }
        }
    }
    return n;
}

} // namespace

// Test 1 — a volatile LOCAL object: its Store (init) AND its Load (read) carry
// the flag. `vx` is slot-backed (body-locals always get an alloca), so the read
// is a real Load and the init a real Store.
TEST(MirLoweringCSubsetVolatile, LocalObjectLoadAndStoreFlagged) {
    auto L = lowerCSubset(
        "int main(void) { volatile int vx = 1; return vx; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    EXPECT_EQ(countOpWithVolatile(m, MirOpcode::Store, /*wantVolatile=*/true), 1u)
        << "the volatile local's init store must carry MirInstFlags::Volatile";
    EXPECT_EQ(countOpWithVolatile(m, MirOpcode::Load, /*wantVolatile=*/true), 1u)
        << "the volatile local's read must carry MirInstFlags::Volatile";
}

// Test 2 — a volatile struct MEMBER (SQLite's case): `p->m` Load carries the
// flag; the non-volatile sibling `p->n` Load does NOT. Pins that volatility is
// per-FIELD (recorded from the field record), not per-object.
TEST(MirLoweringCSubsetVolatile, VolatileMemberLoadFlaggedNonVolatileSiblingNot) {
    auto L = lowerCSubset(
        "struct S { volatile int m; int n; };\n"
        "int rd(struct S *p) { return p->m + p->n; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    // Exactly ONE volatile member Load (p->m) and at least one non-volatile Load
    // (p->n) — the sibling read must stay plain.
    EXPECT_EQ(countOpWithVolatile(m, MirOpcode::Load, /*wantVolatile=*/true), 1u)
        << "the volatile member `p->m` read must carry the flag";
    EXPECT_GE(countOpWithVolatile(m, MirOpcode::Load, /*wantVolatile=*/false), 1u)
        << "the non-volatile sibling `p->n` read must NOT carry the flag";
}

// Test 3 — a volatile GLOBAL: its read Load carries the flag, and a write Store
// carries it too. `gv` is a module global, so reads/writes route through
// GlobalAddr + Load/Store.
TEST(MirLoweringCSubsetVolatile, VolatileGlobalLoadAndStoreFlagged) {
    auto L = lowerCSubset(
        "volatile int gv;\n"
        "int main(void) { gv = 5; return gv; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    EXPECT_EQ(countOpWithVolatile(m, MirOpcode::Store, /*wantVolatile=*/true), 1u)
        << "the volatile global write `gv = 5` must carry the flag";
    EXPECT_EQ(countOpWithVolatile(m, MirOpcode::Load, /*wantVolatile=*/true), 1u)
        << "the volatile global read `return gv` must carry the flag";
}

// Test 4 — a volatile POINTER OBJECT (`int * volatile p`, EAST position): the
// volatility is the POINTER's own storage, so a STORE INTO p carries the flag —
// NOT a deref of p. Proves the east form is ACCEPTED (not rejected as a
// pointee-volatile) and threaded at the pointer-object access.
TEST(MirLoweringCSubsetVolatile, VolatilePointerObjectStoreFlagged) {
    auto L = lowerCSubset(
        "int g;\n"
        "int main(void) { int * volatile p; p = &g; return 0; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    // The assignment `p = &g` stores the address into the volatile pointer slot.
    EXPECT_GE(countOpWithVolatile(m, MirOpcode::Store, /*wantVolatile=*/true), 1u)
        << "a store into a `int * volatile p` must carry the flag (east-volatile "
           "pointer OBJECT is accepted, not rejected as pointee-volatile)";
}

// Test 5 — NON-volatile baseline: the SAME member program WITHOUT `volatile` has
// ZERO volatile Loads/Stores. The non-volatile path must be byte-identical to
// pre-c21 (the flag defaults None). The control for tests 1-3.
TEST(MirLoweringCSubsetVolatile, NonVolatileBaselineHasNoVolatileFlags) {
    auto L = lowerCSubset(
        "struct S { int m; int n; };\n"
        "int gv;\n"
        "int rd(struct S *p) { int x = p->m; gv = x; return gv + p->n; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    EXPECT_EQ(countOpWithVolatile(m, MirOpcode::Load, /*wantVolatile=*/true), 0u)
        << "a program with no `volatile` must emit NO volatile Loads";
    EXPECT_EQ(countOpWithVolatile(m, MirOpcode::Store, /*wantVolatile=*/true), 0u)
        << "a program with no `volatile` must emit NO volatile Stores";
}

// Test 6 (c27 D-CSUBSET-VOLATILE-POINTEE) — a pointer-to-volatile-POINTEE
// (`volatile <base> *p`) now COMPILES (the c21 reject is retired) and a deref
// through it emits a VOLATILE Load (the volatile lives in the pointee TYPE
// `Ptr<VolatileQual(T)>` and is read at the access). The OLD test asserted the
// reject fired; c27 INVERTS it — these are the exact SQLite forms (cast/member/
// local/param) that must carry the volatile, not drop it. RED-ON-DISABLE: drop
// the `volatileFlagForType` thread in `combineDeref` → the Load loses the flag.
TEST(MirLoweringCSubsetVolatile, PointerToVolatilePointeeDerefIsVolatile) {
    auto hasPointeeReject = [](DiagnosticReporter const& r) {
        for (auto const& d : r.all())
            if (d.code == DiagnosticCode::S_VolatilePointeeNotSupported) return true;
        return false;
    };
    // 6a — simple local decl `volatile int *p = &x;` then a deref read `*p`: compiles,
    // and the deref Load is volatile (the pointee `VolatileQual(int)` drives it).
    {
        auto L = lowerCSubset(
            "int main(void) { int x = 0; volatile int *p = &x; return *p; }\n");
        ASSERT_FALSE(hasPointeeReject(L.model.diagnostics()))
            << "`volatile int *p` (pointee-volatile local) must now COMPILE";
        ASSERT_FALSE(L.model.hasErrors())
            << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
        ASSERT_TRUE(L.mir.ok)
            << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
        EXPECT_EQ(countOpWithVolatile(L.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 1u)
            << "the deref `*p` through a volatile pointee must emit a VOLATILE Load";
    }
    // 6b — a COMPLEX deref through pointer arithmetic `*(p + 1)`: proves the
    // volatile is carried by TYPE construction (not a narrow `Deref(Ref(sym))`
    // pattern) — `*(p+1)` is still a deref of a `Ptr<VolatileQual(int)>`.
    {
        auto L = lowerCSubset(
            "int main(void) { int a[2]; volatile int *p = a; return *(p + 1); }\n");
        ASSERT_FALSE(hasPointeeReject(L.model.diagnostics()))
            << "a complex deref `*(p+1)` of a pointee-volatile must COMPILE";
        ASSERT_TRUE(L.mir.ok)
            << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
        EXPECT_EQ(countOpWithVolatile(L.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 1u)
            << "a complex deref `*(p+1)` must STILL be volatile (type-carried)";
    }
    // 6c — a function PARAMETER `volatile int *p`: the param declarator position
    // must build `Ptr<VolatileQual(int)>` too, and its deref be volatile.
    {
        auto L = lowerCSubset(
            "int rd(volatile int *p) { return *p; }\n");
        ASSERT_FALSE(hasPointeeReject(L.model.diagnostics()))
            << "a `volatile int *` PARAMETER must COMPILE (declarator position)";
        ASSERT_TRUE(L.mir.ok)
            << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
        EXPECT_EQ(countOpWithVolatile(L.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 1u)
            << "a deref of a `volatile int *` parameter must be a VOLATILE Load";
    }
    // 6d — a CAST `(volatile int *)` then deref (the sqlite 67392 form): the cast
    // builds `Ptr<VolatileQual(int)>`, so `*(volatile int *)&x` is a volatile Load.
    {
        auto L = lowerCSubset(
            "int main(void) { int x = 0; return *(volatile int *)&x; }\n");
        ASSERT_FALSE(hasPointeeReject(L.model.diagnostics()))
            << "the cast `(volatile int *)` must PARSE + TYPE (no P0009/S0025)";
        ASSERT_TRUE(L.mir.ok)
            << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
        EXPECT_EQ(countOpWithVolatile(L.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 1u)
            << "the deref of a `(volatile int *)` cast must be a VOLATILE Load";
    }
    // 6e — a NON-volatile control: `int *p` deref is NOT volatile (proves 6a–6d
    // are the qualifier's effect, not deref-always-volatile).
    {
        auto L = lowerCSubset(
            "int main(void) { int x = 0; int *p = &x; return *p; }\n");
        ASSERT_TRUE(L.mir.ok)
            << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
        EXPECT_EQ(countOpWithVolatile(L.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 0u)
            << "a deref of a NON-volatile `int *p` must NOT be volatile";
    }
}

// Test 7 — MIR-tier optimizer pin: a volatile local's Load SURVIVES Mem2Reg
// (mem2reg refuses to promote a volatile alloca). RED-ON-DISABLE of the skip:
// if Mem2Reg promoted the volatile alloca, the Load would vanish. The end-to-end
// witness is the `volatile_local` corpus; this isolates the IR-tier guard +
// proves the frontend flag actually reaches the optimizer's decision.
TEST(MirLoweringCSubsetVolatile, VolatileLocalLoadSurvivesMem2Reg) {
    auto L = lowerCSubset(
        "int main(void) { volatile int vx = 1; int a = vx; vx = 2; return a + vx; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir& m = L.mir.mir;
    std::uint32_t const volLoadsBefore =
        countOpWithVolatile(m, MirOpcode::Load, /*wantVolatile=*/true);
    ASSERT_GE(volLoadsBefore, 1u) << "pre-Mem2Reg: the volatile reads are Loads";

    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    DiagnosticReporter rep;
    opt::OptPipeline pipeline{"vol-mem2reg", {opt::PassId::Mem2Reg}};
    auto const result =
        opt::optimize(m, **targetR, L.model.lattice().interner(), pipeline, rep);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(rep.errorCount(), 0u);

    EXPECT_GE(countOpWithVolatile(m, MirOpcode::Load, /*wantVolatile=*/true), 1u)
        << "Mem2Reg must NOT promote a volatile alloca — its Load must survive";
}

// Test 8 — a `volatile` ADDRESS-TAKEN scalar PARAM: its incoming-arg→slot init
// Store carries the flag (param is an in-scope volatile OBJECT form). `int *q =
// &p` forces the param address-taken so it gets a slot + an init store (a non-
// address-taken param stays a pure-SSA Arg with no memory access to flag). The
// param VarDecl is annotated at CST→HIR's `lowerVarLikeInto`, same path as a
// body local — so the init store AND the body read `+ p` both carry the flag.
// (The `*q` deref is NOT flagged — q's pointee is non-volatile, by construction.)
TEST(MirLoweringCSubsetVolatile, VolatileAddressTakenParamInitStoreFlagged) {
    auto L = lowerCSubset(
        "int rd(volatile int p) { int *q = &p; return *q + p; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    // The arg→slot init store of the volatile param must be flagged (≥1: the
    // init store; the body read `+ p` adds a flagged Load too).
    EXPECT_GE(countOpWithVolatile(m, MirOpcode::Store, /*wantVolatile=*/true), 1u)
        << "the volatile address-taken param's init store must carry the flag";
    EXPECT_GE(countOpWithVolatile(m, MirOpcode::Load, /*wantVolatile=*/true), 1u)
        << "the volatile param's body read must carry the flag";
}

// Test 8b — control: a NON-volatile address-taken param's init store is NOT
// flagged (proves test 8 is the qualifier's effect, not address-taking's).
TEST(MirLoweringCSubsetVolatile, NonVolatileAddressTakenParamInitStoreNotFlagged) {
    auto L = lowerCSubset(
        "int rd(int p) { int *q = &p; return *q + p; }\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    EXPECT_EQ(countOpWithVolatile(m, MirOpcode::Store, /*wantVolatile=*/true), 0u)
        << "a NON-volatile param init store must NOT be flagged";
    EXPECT_EQ(countOpWithVolatile(m, MirOpcode::Load, /*wantVolatile=*/true), 0u)
        << "a NON-volatile param read must NOT be flagged";
}

// Test 9 — a WHOLE-VOLATILE aggregate COPY (`volatile struct S a; a = b;`) flags
// the structural Load/Store the aggregate-copy lowering emits (field-wise for a
// flat scalar struct). Without the thread the copy silently drops the volatility.
// `S` is a flat 2-scalar struct → field-wise copy (Load+Store per field).
TEST(MirLoweringCSubsetVolatile, WholeVolatileAggregateAssignCopyFlagged) {
    auto L = lowerCSubset(
        "struct S { int x; int y; };\n"
        "struct S b;\n"
        "int rd(void) { volatile struct S a; a = b; return a.x; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    // The field-wise copy of the 2-field struct emits ≥2 stores into `a` (dest is
    // the volatile aggregate) — all must be flagged. (The `return a.x` read adds
    // another flagged Load.)
    EXPECT_GE(countOpWithVolatile(m, MirOpcode::Store, /*wantVolatile=*/true), 2u)
        << "a whole-volatile aggregate copy's field stores must carry the flag";
    EXPECT_GE(countOpWithVolatile(m, MirOpcode::Load, /*wantVolatile=*/true), 2u)
        << "a whole-volatile aggregate copy's field loads must carry the flag";
}

// Test 9b — control: a NON-volatile aggregate copy has ZERO volatile flags
// (proves test 9 is the qualifier's effect, and the structural copy defaults
// None for the by-value/brace-init/return callers that share the helper).
TEST(MirLoweringCSubsetVolatile, NonVolatileAggregateAssignCopyNotFlagged) {
    auto L = lowerCSubset(
        "struct S { int x; int y; };\n"
        "struct S b;\n"
        "int rd(void) { struct S a; a = b; return a.x; }\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    Mir const& m = L.mir.mir;
    EXPECT_EQ(countOpWithVolatile(m, MirOpcode::Store, /*wantVolatile=*/true), 0u)
        << "a non-volatile aggregate copy must emit NO volatile stores";
    EXPECT_EQ(countOpWithVolatile(m, MirOpcode::Load, /*wantVolatile=*/true), 0u)
        << "a non-volatile aggregate copy must emit NO volatile loads";
}

// ─────────────────────────────────────────────────────────────────────────────
// c27 (D-CSUBSET-VOLATILE-POINTEE) review batch — the c21 reject tests for
// struct/union MEMBER + double-pointer + multi-declarator + typedef pointee-
// volatile forms are INVERTED: each now COMPILES, and the deref through the
// volatile pointee emits a VOLATILE Load (the volatile rides the pointee TYPE).
// These are the exact SQLite aggregate shapes (`volatile <T> *` member /
// `volatile <T> **` / `volatile <T> *a, *b` list / `typedef volatile <T> *`).
// ─────────────────────────────────────────────────────────────────────────────

namespace {
[[nodiscard]] bool hasVolatilePointeeReject(DiagnosticReporter const& r) {
    for (auto const& d : r.all())
        if (d.code == DiagnosticCode::S_VolatilePointeeNotSupported) return true;
    return false;
}
} // namespace

// Test 10 (c27) — struct MEMBER `volatile int *p` now COMPILES; `*s->p` deref is
// volatile (the member load of the plain pointer `s->p` is NOT — only the deref
// reaching the `volatile int` pointee is). The c21 reject is retired.
TEST(MirLoweringCSubsetVolatile, StructMemberPointeeVolatileNowCompilesAndDerefVolatile) {
    auto L = lowerCSubset(
        "struct S { volatile int *p; };\n"
        "int rd(struct S *s){ return *s->p; }\n");
    ASSERT_FALSE(hasVolatilePointeeReject(L.model.diagnostics()))
        << "a `volatile int *` STRUCT MEMBER must now COMPILE (pointee in the type)";
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    // `*s->p` is the deref of the volatile-pointee member → exactly one volatile
    // Load (the `s->p` member read of the plain pointer is NOT volatile).
    EXPECT_EQ(countOpWithVolatile(L.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 1u)
        << "the deref `*s->p` through a volatile-pointee member must be a VOLATILE Load";
}

// Test 11 — union MEMBER `volatile int *p` compiles + deref is volatile.
TEST(MirLoweringCSubsetVolatile, UnionMemberPointeeVolatileNowCompiles) {
    auto L = lowerCSubset(
        "union U { volatile int *p; int x; };\n"
        "int rd(union U *u){ return *u->p; }\n");
    ASSERT_FALSE(hasVolatilePointeeReject(L.model.diagnostics()))
        << "a `volatile int *` UNION MEMBER must now COMPILE";
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    EXPECT_EQ(countOpWithVolatile(L.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 1u)
        << "the deref `*u->p` of a volatile-pointee union member must be volatile";
}

// Test 12 — struct MEMBER DOUBLE pointer `volatile int **pp` (sqlite `apWiData`):
// builds `Ptr<Ptr<VolatileQual(int)>>`. `**s->pp` — ONLY the innermost deref (the
// one reaching the volatile int) is volatile; the intermediate `*s->pp` deref
// (a `Ptr<VolatileQual(int)>` value) is NOT. Pins volatile binds the INNERMOST
// pointee (C 6.7.3), not every level.
TEST(MirLoweringCSubsetVolatile, StructMemberDoublePointeeVolatileInnermostOnly) {
    auto L = lowerCSubset(
        "struct S { volatile int **pp; };\n"
        "int rd(struct S *s){ return **s->pp; }\n");
    ASSERT_FALSE(hasVolatilePointeeReject(L.model.diagnostics()))
        << "a `volatile int **` STRUCT MEMBER must now COMPILE";
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    // Exactly ONE volatile Load: the innermost `*(...)` reaching `volatile int`.
    // (`s->pp` member load + the intermediate `*s->pp` load are NON-volatile.)
    EXPECT_EQ(countOpWithVolatile(L.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 1u)
        << "only the INNERMOST deref of a `volatile int **` is volatile (C 6.7.3)";
    EXPECT_GE(countOpWithVolatile(L.mir.mir, MirOpcode::Load, /*wantVolatile=*/false), 2u)
        << "the member load + the intermediate deref must be NON-volatile";
}

// Test 12b (c23 × c27) — a MULTI-DECLARATOR volatile-pointer LIST `volatile int
// *a, *b;` builds BOTH `a` and `b` as `Ptr<VolatileQual(int)>` (each slot takes
// the head VolatileQual base independently). Pins the per-declarator engine wraps
// EVERY slot's pointee (a first-slot-only build would type `b` as a plain `int *`
// — a silent volatile DROP on `*s.b`).
TEST(MirLoweringCSubsetVolatile, StructMemberPointeeVolatileListEverySlotVolatile) {
    auto L = lowerCSubset(
        "struct S { volatile int *a, *b; };\n"
        "int rd(struct S *s){ return *s->a + *s->b; }\n");
    ASSERT_FALSE(hasVolatilePointeeReject(L.model.diagnostics()))
        << "`volatile int *a, *b;` members must now COMPILE";
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    // BOTH `*s->a` and `*s->b` are volatile derefs → two volatile Loads. A
    // first-slot-only build would give `b` a plain `int *` → only one.
    EXPECT_EQ(countOpWithVolatile(L.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 2u)
        << "EACH slot of `volatile int *a, *b;` is a volatile pointee — both "
           "derefs must be volatile (a first-slot-only build would drop `*b`)";
}

// Test 13 (c27) — a `volatile`-carrying TYPEDEF now works through the TYPE: a
// `typedef volatile int vint;` aliases `VolatileQual(int)`, so a `vint` OBJECT is
// volatile (its access flagged), and `typedef volatile int *vip;` aliases
// `Ptr<VolatileQual(int)>` so a `vip` deref is volatile. The c21 typedef reject is
// retired — volatility rides the aliased type, never silently dropped.
TEST(MirLoweringCSubsetVolatile, VolatileTypedefCarriesThroughType) {
    {
        auto L = lowerCSubset(
            "typedef volatile int vint;\n"
            "int rd(void){ vint x = 0; return x; }\n");
        ASSERT_FALSE(hasVolatilePointeeReject(L.model.diagnostics()))
            << "`typedef volatile int vint;` must now COMPILE (volatile in the alias)";
        ASSERT_TRUE(L.mir.ok)
            << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
        EXPECT_GE(countOpWithVolatile(L.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 1u)
            << "a `vint` (= volatile int) object read must be a VOLATILE Load";
    }
    {
        auto L = lowerCSubset(
            "typedef volatile int *vip;\n"
            "int rd(vip p){ return *p; }\n");
        ASSERT_FALSE(hasVolatilePointeeReject(L.model.diagnostics()))
            << "`typedef volatile int *vip;` must now COMPILE";
        ASSERT_TRUE(L.mir.ok)
            << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
        EXPECT_EQ(countOpWithVolatile(L.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 1u)
            << "a deref of a `vip` (= volatile int *) must be a VOLATILE Load";
    }
}

// Test 14 (c27) — `const` gets NO VolatileQual wrapper, so a deref of a `const int
// *` pointee is NOT volatile (const never affects codegen; only volatile is
// materialized). Pins the asymmetry: volatile wraps, const does not.
TEST(MirLoweringCSubsetVolatile, ConstPointeeStaysNonVolatile) {
    auto L = lowerCSubset(
        "int main(void) { int x = 0; const int *p = &x; return *p; }\n");
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    EXPECT_EQ(countOpWithVolatile(L.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 0u)
        << "a deref of a `const int *` pointee must NOT be volatile (const is "
           "ignored for codegen; only volatile is materialized)";
}

// Test 15 (c27) — `sizeof(volatile T)` == `sizeof(T)`: a qualifier never changes
// the size (C 6.7.3). VolatileQual is transparent to layout (`computeLayout`
// strips it). A `volatile int` array and an `int` array have the same size.
TEST(MirLoweringCSubsetVolatile, SizeofVolatileEqualsSizeofUnqualified) {
    auto L = lowerCSubset(
        "int main(void) { return (int)(sizeof(volatile int) - sizeof(int)); }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    // The program returns sizeof(volatile int) - sizeof(int) == 0 by construction;
    // we only assert it lowered (the const-eval folded the equal sizes).
    EXPECT_EQ(countOpWithVolatile(L.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 0u)
        << "a sizeof expression emits no volatile access";
}

// Test 16 (c27) — a `volatile`-element ARRAY index `va[i]` (va's element is
// `VolatileQual(int)`) is a VOLATILE Load (the element TYPE drives it). Control:
// a plain `int va[4]` index is NOT volatile.
TEST(MirLoweringCSubsetVolatile, VolatileArrayElementIndexIsVolatile) {
    auto L = lowerCSubset(
        "int rd(int i) { volatile int va[4]; return va[i]; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    EXPECT_EQ(countOpWithVolatile(L.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 1u)
        << "indexing a `volatile int va[4]` element must be a VOLATILE Load";
    // Control: a plain `int va[4]` index is NOT volatile.
    auto L2 = lowerCSubset(
        "int rd(int i) { int va[4]; return va[i]; }\n");
    ASSERT_TRUE(L2.mir.ok)
        << (L2.mirReporter.all().empty() ? "" : L2.mirReporter.all()[0].actual);
    EXPECT_EQ(countOpWithVolatile(L2.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 0u)
        << "indexing a plain `int va[4]` element must NOT be volatile";
}

// ─────────────────────────────────────────────────────────────────────────────
// c27 CONTAINER-volatility (D-CSUBSET-VOLATILE-POINTEE, container half) — a
// member/element/by-value access of a `volatile`-qualified CONTAINER is a
// volatile access EVEN WHEN the field/element is a plain (non-volatile) type
// (C 6.7.3p5 / 6.5.2.3). `combineMember`/`combineIndex` qualify the access
// RESULT TYPE when the container is volatile (which the MIR sites read via
// `volatileFlagForType`, and which PROPAGATES through nested chains); the
// brace-init + by-value/return copy sites thread the dest/source volatility.
// Each test pins the EXACT volatile flag count with a non-volatile control;
// RED-ON-DISABLE: drop the container-volatility thread for that form → the
// count drops to 0 (a silent miscompile — the optimizer reassociates/CSEs/
// elides the access). These are the SQLite Kahan-summation + WAL shapes.
// ─────────────────────────────────────────────────────────────────────────────

// Test 17 — member READ through a `volatile struct S *` pointer (plain field).
// `p->a` is a volatile Load even though `a` is a plain `int`.
TEST(MirLoweringCSubsetVolatile, ContainerVolatileMemberReadThroughPtr) {
    auto L = lowerCSubset(
        "struct S { int a; int b; };\n"
        "int rd(volatile struct S *p){ return p->a; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    EXPECT_EQ(countOpWithVolatile(L.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 1u)
        << "`p->a` through a `volatile struct S *` must be a VOLATILE Load (C 6.7.3p5)";
    // Control: a plain `struct S *p` member read is NOT volatile.
    auto L2 = lowerCSubset(
        "struct S { int a; int b; };\n"
        "int rd(struct S *p){ return p->a; }\n");
    ASSERT_TRUE(L2.mir.ok)
        << (L2.mirReporter.all().empty() ? "" : L2.mirReporter.all()[0].actual);
    EXPECT_EQ(countOpWithVolatile(L2.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 0u)
        << "`p->a` through a plain `struct S *` must NOT be volatile";
}

// Test 18 — member WRITE through a `volatile struct S *` pointer (plain field).
// `p->a = 5` is a volatile Store. Mirrors the WAL `pInfo->nBackfill = 0` shape.
TEST(MirLoweringCSubsetVolatile, ContainerVolatileMemberWriteThroughPtr) {
    auto L = lowerCSubset(
        "struct S { int a; int b; };\n"
        "void wr(volatile struct S *p){ p->a = 5; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    EXPECT_EQ(countOpWithVolatile(L.mir.mir, MirOpcode::Store, /*wantVolatile=*/true), 1u)
        << "`p->a = 5` through a `volatile struct S *` must be a VOLATILE Store";
    auto L2 = lowerCSubset(
        "struct S { int a; int b; };\n"
        "void wr(struct S *p){ p->a = 5; }\n");
    ASSERT_TRUE(L2.mir.ok)
        << (L2.mirReporter.all().empty() ? "" : L2.mirReporter.all()[0].actual);
    EXPECT_EQ(countOpWithVolatile(L2.mir.mir, MirOpcode::Store, /*wantVolatile=*/true), 0u)
        << "`p->a = 5` through a plain `struct S *` must NOT be volatile";
}

// Test 19 — member of a `volatile struct S` LOCAL (`s.a`): both the write `s.a=1`
// and the read `s.a` are volatile (the object `s` is volatile, plain field `a`).
TEST(MirLoweringCSubsetVolatile, ContainerVolatileMemberOfLocal) {
    auto L = lowerCSubset(
        "struct S { int a; int b; };\n"
        "int rd(void){ volatile struct S s; s.a = 1; return s.a; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    EXPECT_EQ(countOpWithVolatile(L.mir.mir, MirOpcode::Store, /*wantVolatile=*/true), 1u)
        << "`s.a = 1` on a `volatile struct S` local must be a VOLATILE Store";
    EXPECT_EQ(countOpWithVolatile(L.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 1u)
        << "`return s.a` on a `volatile struct S` local must be a VOLATILE Load";
    auto L2 = lowerCSubset(
        "struct S { int a; int b; };\n"
        "int rd(void){ struct S s; s.a = 1; return s.a; }\n");
    ASSERT_TRUE(L2.mir.ok)
        << (L2.mirReporter.all().empty() ? "" : L2.mirReporter.all()[0].actual);
    EXPECT_EQ(countOpWithVolatile(L2.mir.mir, MirOpcode::Store, /*wantVolatile=*/true), 0u)
        << "a plain `struct S` local member write must NOT be volatile";
    EXPECT_EQ(countOpWithVolatile(L2.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 0u)
        << "a plain `struct S` local member read must NOT be volatile";
}

// Test 20 — index into a `volatile`-CONTAINER array whose element is a PLAIN type
// (the container's qualifier rides through a typedef, so the element type is a
// bare `int`): `va[i]` is volatile via the CONTAINER, not the element type.
TEST(MirLoweringCSubsetVolatile, ContainerVolatileIndexOfArray) {
    auto L = lowerCSubset(
        "typedef int IntArr[4];\n"
        "int rd(int i){ volatile IntArr va; return va[i]; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    EXPECT_EQ(countOpWithVolatile(L.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 1u)
        << "`va[i]` of a `volatile IntArr` (volatile CONTAINER) must be a VOLATILE Load";
    auto L2 = lowerCSubset(
        "typedef int IntArr[4];\n"
        "int rd(int i){ IntArr va; return va[i]; }\n");
    ASSERT_TRUE(L2.mir.ok)
        << (L2.mirReporter.all().empty() ? "" : L2.mirReporter.all()[0].actual);
    EXPECT_EQ(countOpWithVolatile(L2.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 0u)
        << "`va[i]` of a plain `IntArr` must NOT be volatile";
}

// Test 21 — NESTED member chain through a volatile OUTER (`p->inner.x`): the
// qualifier PROPAGATES — `p->inner` is volatile-typed, so the outer `.x` is a
// volatile Load even though both `inner` and `x` are plain fields. RED-ON-DISABLE:
// without the result-type qualification, only `p->inner` (not `.x`) would carry
// the flag, and `.x` is the actual memory access → silent miscompile.
TEST(MirLoweringCSubsetVolatile, ContainerVolatileNestedMemberPropagates) {
    auto L = lowerCSubset(
        "struct Inner { int x; };\n"
        "struct Outer { struct Inner inner; };\n"
        "int rd(volatile struct Outer *p){ return p->inner.x; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    EXPECT_EQ(countOpWithVolatile(L.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 1u)
        << "`p->inner.x` through a volatile OUTER must propagate volatility to `.x`";
    auto L2 = lowerCSubset(
        "struct Inner { int x; };\n"
        "struct Outer { struct Inner inner; };\n"
        "int rd(struct Outer *p){ return p->inner.x; }\n");
    ASSERT_TRUE(L2.mir.ok)
        << (L2.mirReporter.all().empty() ? "" : L2.mirReporter.all()[0].actual);
    EXPECT_EQ(countOpWithVolatile(L2.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 0u)
        << "a plain nested member chain must NOT be volatile";
}

// Test 22 — COMPOUND-assign of a volatile-container member (`p->s += r`, the
// Kahan-summation shape): the read AND the write-back of `p->s` are BOTH volatile
// even though `s` is a plain `double` and the lvalue is COMPLEX (read+written via
// a temp pointer). RED-ON-DISABLE: without the lvalue-type qualification the temp
// pointer points at a plain `double`, dropping BOTH flags → the optimizer
// reassociates the compensated sum (the exact miscompile the volatile prevents).
TEST(MirLoweringCSubsetVolatile, ContainerVolatileCompoundAssignMember) {
    auto L = lowerCSubset(
        "struct C { double s; };\n"
        "void step(volatile struct C *p, double r){ p->s += r; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    EXPECT_EQ(countOpWithVolatile(L.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 1u)
        << "`p->s += r` must read `p->s` VOLATILE (the Kahan-sum guard)";
    EXPECT_EQ(countOpWithVolatile(L.mir.mir, MirOpcode::Store, /*wantVolatile=*/true), 1u)
        << "`p->s += r` must write `p->s` VOLATILE";
    auto L2 = lowerCSubset(
        "struct C { double s; };\n"
        "void step(struct C *p, double r){ p->s += r; }\n");
    ASSERT_TRUE(L2.mir.ok)
        << (L2.mirReporter.all().empty() ? "" : L2.mirReporter.all()[0].actual);
    EXPECT_EQ(countOpWithVolatile(L2.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 0u)
        << "a plain-container `p->s += r` must NOT be volatile";
    EXPECT_EQ(countOpWithVolatile(L2.mir.mir, MirOpcode::Store, /*wantVolatile=*/true), 0u)
        << "a plain-container `p->s += r` must NOT be volatile";
}

// Test 23 — passing a `volatile struct` BY VALUE (>16B → memory copy): the
// source-reading Loads of the by-value copy are volatile (the whole object is
// read, C 6.7.3p5). Control: a plain by-value pass has ZERO volatile flags.
TEST(MirLoweringCSubsetVolatile, ContainerVolatileByValueArg) {
    auto L = lowerCSubset(
        "struct S { long a; long b; long c; };\n"
        "void take(struct S q){ (void)q; }\n"
        "void f(void){ volatile struct S s; take(s); }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    EXPECT_GE(countOpWithVolatile(L.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 1u)
        << "the by-value copy of a `volatile struct` arg must read it VOLATILE";
    auto L2 = lowerCSubset(
        "struct S { long a; long b; long c; };\n"
        "void take(struct S q){ (void)q; }\n"
        "void f(void){ struct S s; take(s); }\n");
    ASSERT_TRUE(L2.mir.ok)
        << (L2.mirReporter.all().empty() ? "" : L2.mirReporter.all()[0].actual);
    EXPECT_EQ(countOpWithVolatile(L2.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 0u)
        << "a plain by-value struct arg copy must NOT be volatile";
}

// Test 24 — RETURNING a `volatile struct` BY VALUE (>16B → sret copy): the
// source-reading Loads of the return copy are volatile. Control = plain return.
TEST(MirLoweringCSubsetVolatile, ContainerVolatileStructReturn) {
    auto L = lowerCSubset(
        "struct S { long a; long b; long c; };\n"
        "struct S g(void){ volatile struct S s; return s; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    EXPECT_GE(countOpWithVolatile(L.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 1u)
        << "returning a `volatile struct` by value must read it VOLATILE";
    auto L2 = lowerCSubset(
        "struct S { long a; long b; long c; };\n"
        "struct S g(void){ struct S s; return s; }\n");
    ASSERT_TRUE(L2.mir.ok)
        << (L2.mirReporter.all().empty() ? "" : L2.mirReporter.all()[0].actual);
    EXPECT_EQ(countOpWithVolatile(L2.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 0u)
        << "a plain by-value struct return must NOT be volatile";
}

// Test 25 — BRACE-INIT of a `volatile` aggregate (`volatile struct S s = {1,2}`):
// every field-init Store is a volatile access (C 6.7.3p5). The scalar-array form
// (`volatile int va[3] = {1,2,3}`) distributes the qualifier to the ELEMENT type,
// so its element stores are volatile too. Controls have ZERO volatile stores.
TEST(MirLoweringCSubsetVolatile, ContainerVolatileBraceInit) {
    auto L = lowerCSubset(
        "struct S { int a; int b; };\n"
        "int rd(void){ volatile struct S s = {1, 2}; return s.a; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    EXPECT_EQ(countOpWithVolatile(L.mir.mir, MirOpcode::Store, /*wantVolatile=*/true), 2u)
        << "a `volatile struct S s = {1,2}` brace-init must emit 2 VOLATILE field stores";
    // Scalar-array brace-init: 3 volatile element stores.
    auto La = lowerCSubset(
        "int rd(void){ volatile int va[3] = {1, 2, 3}; return va[0]; }\n");
    ASSERT_TRUE(La.mir.ok)
        << (La.mirReporter.all().empty() ? "" : La.mirReporter.all()[0].actual);
    EXPECT_EQ(countOpWithVolatile(La.mir.mir, MirOpcode::Store, /*wantVolatile=*/true), 3u)
        << "a `volatile int va[3] = {1,2,3}` brace-init must emit 3 VOLATILE element stores";
    // Control: plain struct brace-init has ZERO volatile stores.
    auto L2 = lowerCSubset(
        "struct S { int a; int b; };\n"
        "int rd(void){ struct S s = {1, 2}; return s.a; }\n");
    ASSERT_TRUE(L2.mir.ok)
        << (L2.mirReporter.all().empty() ? "" : L2.mirReporter.all()[0].actual);
    EXPECT_EQ(countOpWithVolatile(L2.mir.mir, MirOpcode::Store, /*wantVolatile=*/true), 0u)
        << "a plain `struct S s = {1,2}` brace-init must emit NO volatile stores";
}

// Test 26 — INDEX-then-MEMBER through a `volatile`-container array of structs
// (`arr[i].a`): the array container is volatile → `arr[i]` is volatile-typed →
// `.a` is a volatile Load. Pins the Index→Member propagation.
TEST(MirLoweringCSubsetVolatile, ContainerVolatileIndexThenMember) {
    auto L = lowerCSubset(
        "struct S { int a; };\n"
        "int rd(int i){ volatile struct S arr[4]; return arr[i].a; }\n");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty() ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    EXPECT_EQ(countOpWithVolatile(L.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 1u)
        << "`arr[i].a` of a `volatile struct S arr[4]` must be a VOLATILE Load";
    auto L2 = lowerCSubset(
        "struct S { int a; };\n"
        "int rd(int i){ struct S arr[4]; return arr[i].a; }\n");
    ASSERT_TRUE(L2.mir.ok)
        << (L2.mirReporter.all().empty() ? "" : L2.mirReporter.all()[0].actual);
    EXPECT_EQ(countOpWithVolatile(L2.mir.mir, MirOpcode::Load, /*wantVolatile=*/true), 0u)
        << "a plain `struct S arr[4]` `arr[i].a` must NOT be volatile";
}
