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
    // Cycle 3a wires the HirSourceMap so MIR diagnostics carry source spans
    // (mirroring HirVerifier's `&sourceMap` plumbing). The pointer is bound
    // through `hir->sourceMap` which CstToHirResult always populates.
    HirToMirResult mir = lowerToMir(hir->hir, hir->literalPool,
                                    model.lattice().interner(), mirReporter,
                                    &hir->sourceMap);
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
    // The false edge goes to the join block.
    MirBlockId const joinBB = succs[1];
    EXPECT_EQ(m.blockMarker(joinBB), StructCfMarker::IfJoin);
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
    EXPECT_EQ(m.blockMarker(header), StructCfMarker::LoopHeader);
    // header → CondBr(body, exit)
    EXPECT_EQ(m.instOpcode(m.blockTerminator(header)), MirOpcode::CondBr);
    auto hsuccs = m.blockSuccessors(header);
    ASSERT_EQ(hsuccs.size(), 2u);
    MirBlockId const body = hsuccs[0];
    MirBlockId const exit = hsuccs[1];
    EXPECT_EQ(m.blockMarker(exit), StructCfMarker::LoopExit);
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
    // The 4th block (the join) is sealed with Unreachable.
    MirBlockId const joinBB = m.funcBlockAt(fn, 3);
    EXPECT_EQ(m.blockMarker(joinBB), StructCfMarker::IfJoin);
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
    EXPECT_EQ(m.blockMarker(body), StructCfMarker::LoopHeader);
    EXPECT_EQ(m.instOpcode(m.blockTerminator(body)), MirOpcode::Return);
    // The continueBB exists but is sealed as Unreachable — the cond was
    // NOT lowered (so no ICmp/Const instructions other than possibly the
    // exit's implicit return). Locate the LoopLatch block; its terminator
    // must be Unreachable, NOT CondBr.
    bool sawLatch = false;
    auto const blockCount = m.funcBlockCount(fn);
    for (std::uint32_t i = 0; i < blockCount; ++i) {
        MirBlockId const b = m.funcBlockAt(fn, i);
        if (m.blockMarker(b) == StructCfMarker::LoopLatch) {
            sawLatch = true;
            EXPECT_EQ(m.instOpcode(m.blockTerminator(b)),
                      MirOpcode::Unreachable);
            break;
        }
    }
    EXPECT_TRUE(sawLatch);
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
    bool sawLatchWithCondBr = false;
    auto const blockCount = m.funcBlockCount(fn);
    for (std::uint32_t i = 0; i < blockCount; ++i) {
        MirBlockId const b = m.funcBlockAt(fn, i);
        if (m.blockMarker(b) == StructCfMarker::LoopLatch) {
            if (m.instOpcode(m.blockTerminator(b)) == MirOpcode::CondBr) {
                sawLatchWithCondBr = true;
            }
            break;
        }
    }
    EXPECT_TRUE(sawLatchWithCondBr);
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
    EXPECT_EQ(m.blockMarker(header), StructCfMarker::LoopHeader);
    EXPECT_EQ(m.instOpcode(m.blockTerminator(header)), MirOpcode::CondBr);
    MirBlockId const body = m.blockSuccessors(header)[0];
    EXPECT_EQ(m.instOpcode(m.blockTerminator(body)), MirOpcode::Return);
    // Update block is marked LoopLatch.
    MirBlockId const update = m.funcBlockAt(fn, 3);
    EXPECT_EQ(m.blockMarker(update), StructCfMarker::LoopLatch);
    EXPECT_EQ(m.instOpcode(m.blockTerminator(update)), MirOpcode::Br);
    EXPECT_EQ(m.blockSuccessors(update)[0], header);
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
    // [Arg p, Const 0, Const 0(field), Gep, Load, Return]
    ASSERT_EQ(m.blockInstCount(entry), 6u);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 0)), MirOpcode::Arg);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 1)), MirOpcode::Const);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 2)), MirOpcode::Const);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 3)), MirOpcode::Gep);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 4)), MirOpcode::Load);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 5)), MirOpcode::Return);
    // The GEP's operand[0] IS the Arg p (no Load — Deref's lvalue-address
    // returns the pointer rvalue directly).
    auto gepOps = m.instOperands(m.blockInstAt(entry, 3));
    ASSERT_EQ(gepOps.size(), 3u);
    EXPECT_EQ(gepOps[0], m.blockInstAt(entry, 0));
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
    // [Arg p, Arg v, Const 0, Const 1(field=y), Gep, Store, Return]
    ASSERT_EQ(m.blockInstCount(entry), 7u);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 4)), MirOpcode::Gep);
    MirInstId const storeI = m.blockInstAt(entry, 5);
    EXPECT_EQ(m.instOpcode(storeI), MirOpcode::Store);
    auto ops = m.instOperands(storeI);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], m.blockInstAt(entry, 1)) << "Store value should be Arg v";
    EXPECT_EQ(ops[1], m.blockInstAt(entry, 4)) << "Store ptr should be Gep";
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
    // [Arg p, Const 0, Const 0(field), Gep, Return(gep)]   — NO Load.
    ASSERT_EQ(m.blockInstCount(entry), 5u);
    EXPECT_EQ(m.instOpcode(m.blockInstAt(entry, 3)), MirOpcode::Gep);
    MirInstId const ret = m.blockInstAt(entry, 4);
    EXPECT_EQ(m.instOpcode(ret), MirOpcode::Return);
    auto retOps = m.instOperands(ret);
    ASSERT_EQ(retOps.size(), 1u);
    EXPECT_EQ(retOps[0], m.blockInstAt(entry, 3))
        << "Return value should be the Gep result, not a Load";
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
    EXPECT_EQ(m.blockMarker(exit), StructCfMarker::LoopExit);
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
    EXPECT_EQ(m.blockMarker(cont), StructCfMarker::LoopLatch);
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
