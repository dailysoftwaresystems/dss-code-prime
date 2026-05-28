// ML1 — frozen Mir + MirBuilder + MirAttribute side-tables.

#include "core/types/strong_ids.hpp"
#include "mir/mir.hpp"

#include <gtest/gtest.h>

#include <array>
#include <type_traits>
#include <utility>
#include <vector>

using namespace dss;

// Move-only, like Hir.
static_assert(!std::is_copy_constructible_v<Mir>);
static_assert(!std::is_copy_assignable_v<Mir>);
static_assert(std::is_move_constructible_v<Mir>);
static_assert(!std::is_copy_constructible_v<MirBuilder>);

namespace {

// Untagged TypeId literals stand in for a CU type interner (arenaTag 0 passes the
// substrate guards — ML1 carries TypeIds passively, ML2 sources them for real).
constexpr TypeId kFnSig{1};
constexpr TypeId kI32{2};
constexpr TypeId kBool{3};

MirLiteralValue intLit(std::int64_t v) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = TypeKind::I32;
    return lit;
}

} // namespace

TEST(Mir, DefaultModuleIsEmpty) {
    Mir m;
    EXPECT_TRUE(m.empty());
    EXPECT_EQ(m.instCount(), 0u);
    EXPECT_EQ(m.moduleFuncCount(), 0u);
    EXPECT_EQ(m.moduleGlobalCount(), 0u);
    EXPECT_FALSE(m.id().valid());
}

// Globals arena round-trip: three globals (constant-init, function-init, and
// zero-init) survive build-once-freeze with the right accessor values.
TEST(Mir, BuildsAndReadsModuleGlobals) {
    MirBuilder b;
    // This substrate test exercises function-init + zero-init globals, which
    // is enough to pin the addGlobal builder shape; constant-init via the
    // literal pool is exercised by the ML2-globals lowering tests in the
    // mir_lowering_c_subset binary.
    // First, build a synthetic init function whose body just returns.
    MirFuncId const init = b.addFunction(kFnSig, SymbolId{100});
    MirBlockId const ie  = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(ie);
    b.addReturn();
    // Function-init global referencing `init`.
    MirGlobalId const g1 = b.addGlobal(kI32, SymbolId{10}, UINT32_MAX, init);
    // Zero-init global (no literal, no init func).
    MirGlobalId const g2 = b.addGlobal(kI32, SymbolId{11});
    EXPECT_NE(g1, g2);

    Mir m = std::move(b).finish();
    ASSERT_EQ(m.moduleGlobalCount(), 2u);
    EXPECT_EQ(m.globalAt(0), g1);
    EXPECT_EQ(m.globalAt(1), g2);
    EXPECT_EQ(m.globalType(g1).v, kI32.v);
    EXPECT_EQ(m.globalSymbol(g1).v, 10u);
    EXPECT_EQ(m.globalInitLiteralIndex(g1), UINT32_MAX);
    EXPECT_EQ(m.globalInitFunc(g1), init);
    EXPECT_EQ(m.globalType(g2).v, kI32.v);
    EXPECT_EQ(m.globalSymbol(g2).v, 11u);
    EXPECT_EQ(m.globalInitLiteralIndex(g2), UINT32_MAX);
    EXPECT_FALSE(m.globalInitFunc(g2).valid());
}

// Aborts: mutually-exclusive init shapes for a single global.
TEST(MirDeathTest, AddGlobalRejectsBothInitShapes) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder b;
    MirFuncId const init = b.addFunction(kFnSig, SymbolId{1});
    MirBlockId const ie  = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(ie);
    b.addReturn();
    EXPECT_DEATH({ (void)b.addGlobal(kI32, SymbolId{2}, /*lit=*/0, /*func=*/init); },
                 "mutually exclusive");
}

// Aborts: invalid symbol on a global (globals must have a stable name for
// codegen — anonymous module storage has no anchor).
TEST(MirDeathTest, AddGlobalRejectsInvalidSymbol) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder b;
    EXPECT_DEATH({ (void)b.addGlobal(kI32, SymbolId{}); }, "symbol must be valid");
}

namespace {

// Helper for the freeze-boundary death tests below — the {comma-in-init-list}
// inside the macro body trips EXPECT_DEATH's two-arg parser.
[[noreturn]] void buildAndFinishWithDanglingInitFunc() {
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId const e = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(e);
    b.addReturn();
    MirFuncId const dangling{99, b.id().v};   // slot 99 — past the arena
    (void)b.addGlobal(kI32, SymbolId{5}, UINT32_MAX, dangling);
    (void)std::move(b).finish();
    std::abort();   // unreached — finish() aborts first
}

[[noreturn]] void buildAndFinishWithDanglingLiteralIndex() {
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId const e = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(e);
    b.addReturn();
    (void)b.addGlobal(kI32, SymbolId{5}, /*lit=*/12345, /*func=*/{});
    (void)std::move(b).finish();
    std::abort();
}

} // namespace

// Aborts at finish(): a global whose initFunc references a non-existent
// function slot. Pins the freeze-boundary sweep over the globals arena
// (cross-arena dangling references are the load-bearing invariant the
// existing operand/phi/succ sweeps document; globals now share that gate).
TEST(MirDeathTest, FinishRejectsDanglingGlobalInitFunc) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(buildAndFinishWithDanglingInitFunc(),
                 "initFunc references non-existent function");
}

// Aborts at finish(): a global whose initLiteralIndex points past the
// literal pool. Pins the second half of the freeze-boundary sweep.
TEST(MirDeathTest, FinishRejectsDanglingGlobalInitLiteralIndex) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(buildAndFinishWithDanglingLiteralIndex(),
                 "initLiteralIndex");
}

TEST(Mir, ModuleIdsAreMonotonicAndValid) {
    MirModuleId const a = MirBuilder::nextModuleId();
    MirModuleId const b = MirBuilder::nextModuleId();
    EXPECT_TRUE(a.valid());
    EXPECT_TRUE(b.valid());
    EXPECT_LT(a.v, b.v);
}

TEST(Mir, MovedFromModuleIsObservablyEmpty) {
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    b.addReturn();
    Mir m = std::move(b).finish();
    ASSERT_FALSE(m.empty());

    Mir moved = std::move(m);
    EXPECT_TRUE(m.empty());            // NOLINT(bugprone-use-after-move) — checking reset
    EXPECT_FALSE(m.id().valid());
    EXPECT_FALSE(moved.empty());
}

// Build a straight-line function and read every tier back.
TEST(Mir, BuildsAndReadsAStraightLineFunction) {
    MirBuilder b;
    MirFuncId const f = b.addFunction(kFnSig, SymbolId{7});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const arg = b.addArg(0, kI32);
    MirInstId const c   = b.addConst(intLit(5), kI32);
    std::array<MirInstId, 2> operands{arg, c};
    MirInstId const sum = b.addInst(MirOpcode::Add, operands, kI32);
    b.addReturn(sum);
    Mir m = std::move(b).finish();

    // module / function tier
    EXPECT_EQ(m.moduleFuncCount(), 1u);
    EXPECT_EQ(m.funcAt(0), f);
    EXPECT_EQ(m.funcSignature(f), kFnSig);
    EXPECT_EQ(m.funcSymbol(f), SymbolId{7});
    EXPECT_EQ(m.funcBlockCount(f), 1u);
    EXPECT_EQ(m.funcEntry(f), entry);
    EXPECT_EQ(m.funcBlockAt(f, 0), entry);

    // block tier
    EXPECT_EQ(m.blockMarker(entry), StructCfMarker::EntryBlock);
    EXPECT_EQ(m.blockFunc(entry), f);
    EXPECT_EQ(m.blockInstCount(entry), 4u);          // arg, const, add, return
    EXPECT_EQ(m.blockInstAt(entry, 0), arg);
    EXPECT_EQ(m.blockInstAt(entry, 3), m.blockTerminator(entry));
    EXPECT_EQ(m.instOpcode(m.blockTerminator(entry)), MirOpcode::Return);
    EXPECT_TRUE(m.blockSuccessors(entry).empty());   // return has no successors

    // instruction tier — the fused value model
    EXPECT_EQ(m.instOpcode(arg), MirOpcode::Arg);
    EXPECT_EQ(m.argIndex(arg), 0u);                  // typed payload accessor
    EXPECT_EQ(m.instOpcode(c), MirOpcode::Const);
    EXPECT_EQ(m.instType(sum), kI32);
    // reverse lookup: every instruction maps back to its block
    EXPECT_EQ(m.instBlock(arg), entry);
    EXPECT_EQ(m.instBlock(sum), entry);
    auto const sumOps = m.instOperands(sum);
    ASSERT_EQ(sumOps.size(), 2u);
    EXPECT_EQ(sumOps[0], arg);
    EXPECT_EQ(sumOps[1], c);

    // literal pool — the const carries its decoded value (via typed accessor)
    auto const& lit = m.literalValue(m.constLiteralIndex(c));
    EXPECT_EQ(lit.core, TypeKind::I32);
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(lit.value));
    EXPECT_EQ(std::get<std::int64_t>(lit.value), 5);
}

// A diamond CFG with a forward conditional branch and a join phi — exercises the
// create-then-fill model and the succ/phi pools.
TEST(Mir, BuildsADiamondWithCondBrAndPhi) {
    MirBuilder b;
    MirFuncId const f = b.addFunction(kFnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const thenB = b.createBlock(StructCfMarker::IfThen);
    MirBlockId const elseB = b.createBlock(StructCfMarker::IfElse);
    MirBlockId const join  = b.createBlock(StructCfMarker::IfJoin);

    b.beginBlock(entry);
    MirInstId const cond = b.addConst([] { MirLiteralValue v; v.value = true; v.core = TypeKind::Bool; return v; }(), kBool);
    b.addCondBr(cond, thenB, elseB);

    b.beginBlock(thenB);
    MirInstId const x = b.addConst(intLit(1), kI32);
    b.addBr(join);

    b.beginBlock(elseB);
    MirInstId const y = b.addConst(intLit(2), kI32);
    b.addBr(join);

    b.beginBlock(join);
    std::array<MirPhiIncoming, 2> incomings{MirPhiIncoming{x, thenB}, MirPhiIncoming{y, elseB}};
    MirInstId const phi = b.addPhi(kI32, incomings);
    b.addReturn(phi);

    Mir m = std::move(b).finish();

    EXPECT_EQ(m.funcBlockCount(f), 4u);
    // entry → [then, else]
    auto const entrySuccs = m.blockSuccessors(entry);
    ASSERT_EQ(entrySuccs.size(), 2u);
    EXPECT_EQ(entrySuccs[0], thenB);
    EXPECT_EQ(entrySuccs[1], elseB);
    // then/else → [join]
    ASSERT_EQ(m.blockSuccessors(thenB).size(), 1u);
    EXPECT_EQ(m.blockSuccessors(thenB)[0], join);
    EXPECT_EQ(m.blockSuccessors(elseB)[0], join);
    // join phi has both incomings
    EXPECT_EQ(m.instOpcode(phi), MirOpcode::Phi);
    auto const inc = m.phiIncomings(phi);
    ASSERT_EQ(inc.size(), 2u);
    EXPECT_EQ(inc[0].value, x);
    EXPECT_EQ(inc[0].pred, thenB);
    EXPECT_EQ(inc[1].value, y);
    EXPECT_EQ(inc[1].pred, elseB);
}

// Phi incomings may be backpatched after the predecessor blocks are filled
// (the loop back-edge pattern).
TEST(Mir, PhiIncomingsCanBeBackpatched) {
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = b.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const exit = b.createBlock(StructCfMarker::LoopExit);

    b.beginBlock(entry);
    MirInstId const init = b.addConst(intLit(0), kI32);
    b.addBr(header);

    b.beginBlock(header);
    MirInstId const phi = b.addPhi(kI32);              // no incomings yet
    b.addPhiIncoming(phi, MirPhiIncoming{init, entry});
    MirInstId const next = b.addConst(intLit(1), kI32);
    b.addPhiIncoming(phi, MirPhiIncoming{next, header});  // back-edge value
    b.addBr(exit);

    b.beginBlock(exit);
    b.addReturn(phi);

    Mir m = std::move(b).finish();
    auto const inc = m.phiIncomings(phi);
    ASSERT_EQ(inc.size(), 2u);
    EXPECT_EQ(inc[0].value, init);
    EXPECT_EQ(inc[1].value, next);
}

// Switch: operands [discriminant, case constants…] pair positionally with
// successors [case targets…, default].
TEST(Mir, BuildsASwitchWithPositionalCasePairing) {
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::SwitchHead);
    MirBlockId const caseA = b.createBlock(StructCfMarker::SwitchCase);
    MirBlockId const caseB = b.createBlock(StructCfMarker::SwitchCase);
    MirBlockId const dflt  = b.createBlock(StructCfMarker::SwitchCase);

    b.beginBlock(entry);
    MirInstId const disc = b.addArg(0, kI32);
    MirInstId const k1   = b.addConst(intLit(10), kI32);
    MirInstId const k2   = b.addConst(intLit(20), kI32);
    std::array<std::pair<MirInstId, MirBlockId>, 2> cases{{{k1, caseA}, {k2, caseB}}};
    MirInstId const sw = b.addSwitch(disc, cases, dflt);
    for (MirBlockId arm : {caseA, caseB, dflt}) { b.beginBlock(arm); b.addReturn(); }
    Mir m = std::move(b).finish();

    EXPECT_EQ(m.blockTerminator(entry), sw);
    auto const ops = m.instOperands(sw);
    ASSERT_EQ(ops.size(), 3u);          // [disc, k1, k2]
    EXPECT_EQ(ops[0], disc);
    EXPECT_EQ(ops[1], k1);
    EXPECT_EQ(ops[2], k2);
    auto const succs = m.blockSuccessors(entry);
    ASSERT_EQ(succs.size(), 3u);        // [caseA, caseB, default]
    EXPECT_EQ(succs[0], caseA);
    EXPECT_EQ(succs[1], caseB);
    EXPECT_EQ(succs[2], dflt);
}

// Switch with zero cases (default-only) — the minSuccessors=1 boundary. Locks
// the descriptor's "default always present" invariant against an off-by-one
// regression in either the bound or in addSwitch's unconditional-default push.
TEST(Mir, BuildsASwitchWithDefaultOnly) {
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::SwitchHead);
    MirBlockId const dflt  = b.createBlock(StructCfMarker::SwitchCase);
    b.beginBlock(entry);
    MirInstId const disc = b.addArg(0, kI32);
    MirInstId const sw   = b.addSwitch(disc, {}, dflt);  // zero cases
    b.beginBlock(dflt);
    b.addReturn();
    Mir m = std::move(b).finish();

    EXPECT_EQ(m.instOpcode(sw), MirOpcode::Switch);
    EXPECT_EQ(m.instOperands(sw).size(), 1u);  // discriminant only, no case constants
    auto const succs = m.blockSuccessors(entry);
    ASSERT_EQ(succs.size(), 1u);
    EXPECT_EQ(succs[0], dflt);                  // the default is the sole successor
}

// Multiple functions in one module: funcAt(i) maps to the right function and
// block/inst ranges don't bleed across functions.
TEST(Mir, BuildsMultipleFunctions) {
    MirBuilder b;
    MirFuncId const f0 = b.addFunction(kFnSig, SymbolId{11});
    MirBlockId const e0 = b.createBlock();
    b.beginBlock(e0);
    b.addReturn();
    MirFuncId const f1 = b.addFunction(kFnSig, SymbolId{22});
    MirBlockId const e1 = b.createBlock();
    b.beginBlock(e1);
    b.addReturn(b.addConst(intLit(3), kI32));
    Mir m = std::move(b).finish();

    ASSERT_EQ(m.moduleFuncCount(), 2u);
    EXPECT_EQ(m.funcAt(0), f0);
    EXPECT_EQ(m.funcAt(1), f1);
    EXPECT_EQ(m.funcSymbol(f0), SymbolId{11});
    EXPECT_EQ(m.funcSymbol(f1), SymbolId{22});
    EXPECT_EQ(m.funcEntry(f0), e0);
    EXPECT_EQ(m.funcEntry(f1), e1);
    EXPECT_EQ(m.blockFunc(e1), f1);
    EXPECT_EQ(m.funcBlockCount(f0), 1u);
}

// The Optional result rule: a Call may carry a result type (value callee) or not
// (void callee) — addInst must accept both.
TEST(Mir, VoidAndValueCallsBothBuild) {
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId const e = b.createBlock();
    b.beginBlock(e);
    MirInstId const callee = b.addGlobalAddr(SymbolId{5}, kI32);
    std::array<MirInstId, 1> ops{callee};
    MirInstId const voidCall  = b.addInst(MirOpcode::Call, ops, InvalidType);  // void callee
    MirInstId const valueCall = b.addInst(MirOpcode::Call, ops, kI32);          // value callee
    b.addReturn(valueCall);
    Mir m = std::move(b).finish();
    EXPECT_FALSE(m.instType(voidCall).valid());
    EXPECT_EQ(m.instType(valueCall), kI32);
}

// addUnreachable seals the block with no successors; setBlockMarker overrides.
TEST(Mir, UnreachableTerminatorAndMarkerOverride) {
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId const e = b.createBlock(StructCfMarker::Linear);
    b.beginBlock(e);
    b.setBlockMarker(e, StructCfMarker::LoopHeader);
    MirInstId const u = b.addUnreachable();
    Mir m = std::move(b).finish();
    EXPECT_EQ(m.instOpcode(u), MirOpcode::Unreachable);
    EXPECT_TRUE(m.blockSuccessors(e).empty());
    EXPECT_EQ(m.blockMarker(e), StructCfMarker::LoopHeader);
}

// MirAttribute<T> binds to the instruction tier; the block/func sibling
// attributes bind to their arenas.
TEST(Mir, AttributesBindToEachTier) {
    MirBuilder b;
    MirFuncId const f = b.addFunction(kFnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const c = b.addConst(intLit(42), kI32);
    b.addReturn(c);
    Mir m = std::move(b).finish();

    MirAttribute<int> instTag{m};
    instTag.set(c, 99);
    EXPECT_TRUE(instTag.has(c));
    EXPECT_EQ(instTag.get(c), 99);

    MirBlockAttribute<bool> blockTag{m.blockArena()};
    blockTag.set(entry, true);
    EXPECT_TRUE(blockTag.get(entry));

    MirFuncAttribute<int> funcTag{m.funcArena()};
    funcTag.set(f, 7);
    EXPECT_EQ(funcTag.get(f), 7);
}

// ── fail-loud builder guards ──

TEST(MirDeathTest, AddInstWithTerminatorOpcodeAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId e = b.createBlock();
    b.beginBlock(e);
    EXPECT_DEATH({ (void)b.addInst(MirOpcode::Br, {}, InvalidType); }, "terminator");
}

TEST(MirDeathTest, AddInstAfterTerminatorAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId e = b.createBlock();
    b.beginBlock(e);
    b.addReturn();
    EXPECT_DEATH({ (void)b.addConst(intLit(1), kI32); }, "already terminated");
}

TEST(MirDeathTest, ValueOpcodeWithoutResultTypeAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId e = b.createBlock();
    b.beginBlock(e);
    MirInstId const c = b.addConst(intLit(1), kI32);
    std::array<MirInstId, 2> ops{c, c};
    EXPECT_DEATH({ (void)b.addInst(MirOpcode::Add, ops, InvalidType); }, "produces a value");
}

TEST(MirDeathTest, FinishWithUnterminatedBlockAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId e = b.createBlock();
    b.beginBlock(e);
    b.addConst(intLit(1), kI32);   // no terminator
    EXPECT_DEATH({ (void)std::move(b).finish(); }, "no terminator");
}

TEST(MirDeathTest, FinishWithUnfilledBlockAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId e = b.createBlock();
    b.createBlock();               // created but never begun/terminated
    b.beginBlock(e);
    b.addReturn();
    EXPECT_DEATH({ (void)std::move(b).finish(); }, "never filled");
}

TEST(MirDeathTest, FunctionWithInvalidSignatureAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder b;
    EXPECT_DEATH({ (void)b.addFunction(InvalidType, SymbolId{1}); }, "signature");
}

TEST(MirDeathTest, InstOperandsOnPhiAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId e = b.createBlock();
    b.beginBlock(e);
    MirInstId const phi = b.addPhi(kI32);
    b.addReturn(phi);
    Mir m = std::move(b).finish();
    EXPECT_DEATH({ (void)m.instOperands(phi); }, "Phi");
}

TEST(MirDeathTest, PhiIncomingsOnNonPhiAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId e = b.createBlock();
    b.beginBlock(e);
    MirInstId const c = b.addConst(intLit(1), kI32);
    b.addReturn(c);
    Mir m = std::move(b).finish();
    EXPECT_DEATH({ (void)m.phiIncomings(c); }, "not a Phi");
}

TEST(MirDeathTest, AddInstRejectsValueOriginOpcodes) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId e = b.createBlock();
    b.beginBlock(e);
    // Const has a dedicated builder (addConst) so its payload is always a real
    // literal-pool index; addInst must refuse to spell it.
    EXPECT_DEATH({ (void)b.addInst(MirOpcode::Const, {}, kI32, 0); }, "dedicated builder");
}

TEST(MirDeathTest, BranchToBlockOfAnotherFunctionAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder b;
    // function 1 with a block
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId const foreign = b.createBlock();
    b.beginBlock(foreign);
    b.addReturn();
    // function 2 tries to branch to function 1's block
    b.addFunction(kFnSig, SymbolId{2});
    MirBlockId e2 = b.createBlock();
    b.beginBlock(e2);
    EXPECT_DEATH({ (void)b.addBr(foreign); }, "belongs to function");
}

TEST(MirDeathTest, TypedPayloadAccessorOnWrongOpcodeAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId e = b.createBlock();
    b.beginBlock(e);
    MirInstId const c = b.addConst(intLit(1), kI32);
    b.addReturn(c);
    Mir m = std::move(b).finish();
    EXPECT_DEATH({ (void)m.argIndex(c); }, "argIndex");  // c is a Const, not an Arg
}

TEST(MirDeathTest, CrossModuleOperandAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder a;
    a.addFunction(kFnSig, SymbolId{1});
    MirBlockId ea = a.createBlock();
    a.beginBlock(ea);
    MirInstId const fromA = a.addConst(intLit(1), kI32);  // tagged with module A

    MirBuilder bb;  // a different module
    bb.addFunction(kFnSig, SymbolId{1});
    MirBlockId eb = bb.createBlock();
    bb.beginBlock(eb);
    std::array<MirInstId, 2> ops{fromA, fromA};
    EXPECT_DEATH({ (void)bb.addInst(MirOpcode::Add, ops, kI32); }, "module");
}

// ── freeze-boundary sweep: dangling pooled references abort at finish ──
// An untagged out-of-range id passes checkSameModule_ (test-ergonomics bypass),
// so the finish() sweep is the only guard — it must fire.

TEST(MirDeathTest, FinishWithDanglingOperandAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId e = b.createBlock();
    b.beginBlock(e);
    MirInstId const c = b.addConst(intLit(1), kI32);
    std::array<MirInstId, 2> ops{c, MirInstId{9999}};  // 9999 untagged, nonexistent
    b.addInst(MirOpcode::Add, ops, kI32);
    b.addReturn();
    EXPECT_DEATH({ (void)std::move(b).finish(); }, "non-existent instruction");
}

TEST(MirDeathTest, FinishWithDanglingPhiValueAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId e = b.createBlock();
    b.beginBlock(e);
    std::array<MirPhiIncoming, 1> inc{MirPhiIncoming{MirInstId{9999}, e}};
    MirInstId const phi = b.addPhi(kI32, inc);
    b.addReturn(phi);
    EXPECT_DEATH({ (void)std::move(b).finish(); }, "phi value references non-existent");
}

TEST(MirDeathTest, FinishWithDanglingPhiPredAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId e = b.createBlock();
    b.beginBlock(e);
    MirInstId const c = b.addConst(intLit(1), kI32);
    std::array<MirPhiIncoming, 1> inc{MirPhiIncoming{c, MirBlockId{9999}}};
    MirInstId const phi = b.addPhi(kI32, inc);
    b.addReturn(phi);
    EXPECT_DEATH({ (void)std::move(b).finish(); }, "phi predecessor references non-existent");
}

// ── builder lifecycle guards ──

TEST(MirDeathTest, CreateBlockWithoutOpenFunctionAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder b;
    EXPECT_DEATH({ (void)b.createBlock(); }, "no open function");
}

TEST(MirDeathTest, AddInstWithNoOpenBlockAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    b.createBlock();  // created but never begun
    EXPECT_DEATH({ (void)b.addConst(intLit(1), kI32); }, "no open block");
}

TEST(MirDeathTest, BeginBlockTwiceAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId e = b.createBlock();
    b.beginBlock(e);
    EXPECT_DEATH({ b.beginBlock(e); }, "already opened");
}

TEST(MirDeathTest, AddPhiIncomingOnNonPhiAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId e = b.createBlock();
    b.beginBlock(e);
    MirInstId const c = b.addConst(intLit(1), kI32);
    EXPECT_DEATH({ b.addPhiIncoming(c, MirPhiIncoming{c, e}); }, "not a Phi");
}

TEST(MirDeathTest, AddInstTooFewOperandsAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId e = b.createBlock();
    b.beginBlock(e);
    MirInstId const c = b.addConst(intLit(1), kI32);
    std::array<MirInstId, 1> ops{c};  // Add needs 2
    EXPECT_DEATH({ (void)b.addInst(MirOpcode::Add, ops, kI32); }, "operands");
}

TEST(MirDeathTest, AddInstRejectsPhiOpcode) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId e = b.createBlock();
    b.beginBlock(e);
    EXPECT_DEATH({ (void)b.addInst(MirOpcode::Phi, {}, kI32); }, "addPhi");
}

TEST(MirDeathTest, BranchToNonexistentBlockAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirBuilder b;
    b.addFunction(kFnSig, SymbolId{1});
    MirBlockId e = b.createBlock();
    b.beginBlock(e);
    EXPECT_DEATH({ (void)b.addBr(MirBlockId{9999}); }, "MirBlockId");  // untagged OOB target
}

// ── direct (non-builder) Mir ctor invariant guards ──

TEST(MirDeathTest, DirectCtorArenaTagMismatchAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    substrate::ArenaBuilder<detail::MirInst,   MirInstId,   MirModuleId> ib{MirModuleId{1}};
    substrate::ArenaBuilder<detail::MirBlock,  MirBlockId,  MirModuleId> bb{MirModuleId{2}};
    substrate::ArenaBuilder<detail::MirFunc,   MirFuncId,   MirModuleId> fb{MirModuleId{1}};
    substrate::ArenaBuilder<detail::MirGlobal, MirGlobalId, MirModuleId> gb{MirModuleId{1}};
    auto ia = std::move(ib).finish();
    auto ba = std::move(bb).finish();
    auto fa = std::move(fb).finish();
    auto ga = std::move(gb).finish();
    std::vector<MirBlockId> instBlock{InvalidMirBlock};  // size 1 == ia.nodeCount()
    // Function-call ctor form keeps every comma inside parens (so neither the
    // EXPECT_DEATH macro nor a braced-init-list splits the arguments).
    EXPECT_DEATH((void)Mir(std::move(ia), std::move(ba), std::move(fa), std::move(ga),
                           std::move(instBlock),
                           {}, {}, {}, MirLiteralPool{}),
                 "module-tag mismatch");
}

TEST(MirDeathTest, DirectCtorInstBlockSizeMismatchAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    substrate::ArenaBuilder<detail::MirInst,   MirInstId,   MirModuleId> ib{MirModuleId{1}};
    substrate::ArenaBuilder<detail::MirBlock,  MirBlockId,  MirModuleId> bb{MirModuleId{1}};
    substrate::ArenaBuilder<detail::MirFunc,   MirFuncId,   MirModuleId> fb{MirModuleId{1}};
    substrate::ArenaBuilder<detail::MirGlobal, MirGlobalId, MirModuleId> gb{MirModuleId{1}};
    auto ia = std::move(ib).finish();
    auto ba = std::move(bb).finish();
    auto fa = std::move(fb).finish();
    auto ga = std::move(gb).finish();
    std::vector<MirBlockId> instBlock{};  // size 0 ≠ ia.nodeCount() (1)
    EXPECT_DEATH((void)Mir(std::move(ia), std::move(ba), std::move(fa), std::move(ga),
                           std::move(instBlock),
                           {}, {}, {}, MirLiteralPool{}),
                 "size mismatch");
}

// New: a 4th-arena module-tag mismatch (globals arena) aborts identically.
TEST(MirDeathTest, DirectCtorGlobalArenaTagMismatchAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    substrate::ArenaBuilder<detail::MirInst,   MirInstId,   MirModuleId> ib{MirModuleId{1}};
    substrate::ArenaBuilder<detail::MirBlock,  MirBlockId,  MirModuleId> bb{MirModuleId{1}};
    substrate::ArenaBuilder<detail::MirFunc,   MirFuncId,   MirModuleId> fb{MirModuleId{1}};
    substrate::ArenaBuilder<detail::MirGlobal, MirGlobalId, MirModuleId> gb{MirModuleId{2}};
    auto ia = std::move(ib).finish();
    auto ba = std::move(bb).finish();
    auto fa = std::move(fb).finish();
    auto ga = std::move(gb).finish();
    std::vector<MirBlockId> instBlock{InvalidMirBlock};
    EXPECT_DEATH((void)Mir(std::move(ia), std::move(ba), std::move(fa), std::move(ga),
                           std::move(instBlock),
                           {}, {}, {}, MirLiteralPool{}),
                 "module-tag mismatch");
}
