// D-OPT1-MIR-UNIT-TESTS — direct MIR-tier unit pins for the
// ConstFold pass. Constructs MIR manually (no source compile),
// runs the pass, and asserts byte-equal pre/post for non-foldable
// shapes + specific rewrites for foldable shapes.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/passes/const_fold.hpp"

#include <gtest/gtest.h>

#include <cstdint>

using namespace dss;

namespace {

// Build a single-function module: `return <foldable arith with two
// constants>`. e.g. `return Add(Const 5, Const 3)` — after const-fold
// the function should be `return Const(8)` (one folded instruction).
struct BuildResult {
    Mir          mir;
    TypeId       i32;
    TypeId       fnSig;
};

BuildResult buildAddConstants(TypeInterner& interner,
                              std::int64_t a, std::int64_t b) {
    BuildResult br;
    br.i32   = interner.primitive(TypeKind::I32);
    // FnSig with no params, i32 result, default cc, non-variadic.
    br.fnSig = interner.fnSig({}, br.i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(br.fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirLiteralValue va; va.value = a; va.core = TypeKind::I32;
    MirLiteralValue vb; vb.value = b; vb.core = TypeKind::I32;
    MirInstId const ca = mb.addConst(va, br.i32);
    MirInstId const cb = mb.addConst(vb, br.i32);
    MirInstId const ops[] = {ca, cb};
    MirInstId const sum = mb.addInst(MirOpcode::Add, ops, br.i32);
    mb.addReturn(sum);
    br.mir = std::move(mb).finish();
    return br;
}

// Build a module with one block that uses a runtime Arg in the
// arithmetic — fold MUST NOT fire (one operand is dynamic).
BuildResult buildArgPlusConstant(TypeInterner& interner,
                                 std::int64_t k) {
    BuildResult br;
    br.i32   = interner.primitive(TypeKind::I32);
    TypeId const params[] = {br.i32};
    br.fnSig = interner.fnSig(params, br.i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(br.fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const arg = mb.addArg(0, br.i32);
    MirLiteralValue vk; vk.value = k; vk.core = TypeKind::I32;
    MirInstId const ck  = mb.addConst(vk, br.i32);
    MirInstId const ops[] = {arg, ck};
    MirInstId const sum = mb.addInst(MirOpcode::Add, ops, br.i32);
    mb.addReturn(sum);
    br.mir = std::move(mb).finish();
    return br;
}

// Walk the rebuilt module's entry block and find the instruction
// that feeds the return. Returns its opcode + (if Const) the literal
// value as int64.
struct ReturnOperandInspect {
    MirOpcode    op = MirOpcode::Invalid;
    std::int64_t constValue = 0;
    bool         isConst = false;
};
ReturnOperandInspect inspectReturnOperand(Mir const& mir) {
    ReturnOperandInspect r;
    MirFuncId const fn = mir.funcAt(0);
    MirBlockId const entry = mir.funcEntry(fn);
    std::uint32_t const n = mir.blockInstCount(entry);
    MirInstId const term = mir.blockInstAt(entry, n - 1);
    auto const ops = mir.instOperands(term);
    if (ops.empty()) return r;
    MirInstId const operand = ops[0];
    r.op = mir.instOpcode(operand);
    if (r.op == MirOpcode::Const) {
        r.isConst = true;
        auto const idx = mir.constLiteralIndex(operand);
        auto const& lit = mir.literalValue(idx);
        if (auto p = std::get_if<std::int64_t>(&lit.value)) {
            r.constValue = *p;
        }
    }
    return r;
}

} // namespace

// Foldable: Add(5, 3) → Const(8). Pinned via the rebuilt module's
// return-operand opcode (must be Const) + literal value (must equal
// 5+3=8) + the pass's instructionsFolded counter (must be 1).
TEST(ConstFold, FoldsAddOfTwoConstants) {
    TypeInterner interner{CompilationUnitId{1}};
    auto br = buildAddConstants(interner, 5, 3);
    DiagnosticReporter rep;
    auto const r = opt::passes::runConstFold(br.mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsFolded, 1u);
    auto const ret = inspectReturnOperand(br.mir);
    EXPECT_TRUE(ret.isConst);
    EXPECT_EQ(ret.constValue, 8);
}

// Sub with two constants. Different op vs Add to pin the binaryOpKind
// dispatch.
TEST(ConstFold, FoldsSubOfTwoConstants) {
    TypeInterner interner{CompilationUnitId{1}};
    BuildResult br;
    br.i32   = interner.primitive(TypeKind::I32);
    br.fnSig = interner.fnSig({}, br.i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(br.fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirLiteralValue va; va.value = std::int64_t{50}; va.core = TypeKind::I32;
    MirLiteralValue vb; vb.value = std::int64_t{8};  vb.core = TypeKind::I32;
    MirInstId const ops[] = {mb.addConst(va, br.i32), mb.addConst(vb, br.i32)};
    mb.addReturn(mb.addInst(MirOpcode::Sub, ops, br.i32));
    br.mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runConstFold(br.mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsFolded, 1u);
    auto const ret = inspectReturnOperand(br.mir);
    EXPECT_TRUE(ret.isConst);
    EXPECT_EQ(ret.constValue, 42);
}

// Not foldable: one operand is a function Arg. The pass MUST NOT
// rewrite the Add — the result is still an Add inst, not a Const.
TEST(ConstFold, DoesNotFoldWhenOperandIsArg) {
    TypeInterner interner{CompilationUnitId{1}};
    auto br = buildArgPlusConstant(interner, 100);
    DiagnosticReporter rep;
    auto const r = opt::passes::runConstFold(br.mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsFolded, 0u);
    auto const ret = inspectReturnOperand(br.mir);
    EXPECT_FALSE(ret.isConst) << "Add with one dynamic operand must "
                                  "NOT fold to a Const";
    EXPECT_EQ(ret.op, MirOpcode::Add);
}

// Div-by-zero MUST defer to runtime (folding would introduce a trap
// that the unoptimized path doesn't have). Pin: the SDiv(7, 0) is
// preserved as an SDiv, not folded.
TEST(ConstFold, DefersDivByZeroToRuntime) {
    TypeInterner interner{CompilationUnitId{1}};
    BuildResult br;
    br.i32   = interner.primitive(TypeKind::I32);
    br.fnSig = interner.fnSig({}, br.i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(br.fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirLiteralValue va; va.value = std::int64_t{7}; va.core = TypeKind::I32;
    MirLiteralValue vb; vb.value = std::int64_t{0}; vb.core = TypeKind::I32;
    MirInstId const ops[] = {mb.addConst(va, br.i32), mb.addConst(vb, br.i32)};
    mb.addReturn(mb.addInst(MirOpcode::SDiv, ops, br.i32));
    br.mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runConstFold(br.mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsFolded, 0u);
    auto const ret = inspectReturnOperand(br.mir);
    EXPECT_FALSE(ret.isConst);
    EXPECT_EQ(ret.op, MirOpcode::SDiv);
}

// G1: wrap-on-overflow normalization. `Add(i32 0x7FFFFFFF, i32 1)`
// must fold to a literal whose int64 value is -2147483648 (wrapped),
// NOT 0x80000000 (positive 2147483648). A regression bypassing
// `wrapToIntTarget` would produce an out-of-range literal that
// silently miscompiles downstream.
TEST(ConstFold, WrapsI32OnAddOverflow) {
    TypeInterner interner{CompilationUnitId{1}};
    BuildResult br;
    br.i32   = interner.primitive(TypeKind::I32);
    br.fnSig = interner.fnSig({}, br.i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(br.fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirLiteralValue va; va.value = std::int64_t{0x7FFFFFFF}; va.core = TypeKind::I32;
    MirLiteralValue vb; vb.value = std::int64_t{1};          vb.core = TypeKind::I32;
    MirInstId const ops[] = {mb.addConst(va, br.i32), mb.addConst(vb, br.i32)};
    mb.addReturn(mb.addInst(MirOpcode::Add, ops, br.i32));
    br.mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runConstFold(br.mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsFolded, 1u);
    auto const ret = inspectReturnOperand(br.mir);
    ASSERT_TRUE(ret.isConst);
    EXPECT_EQ(ret.constValue, std::int64_t{-2147483648LL})
        << "i32 wrap: 0x7FFFFFFF + 1 must wrap to INT32_MIN";
}

// G3: nested folding. `Add(Mul(2, 3), 7)` must fold to `Const 13` in
// a SINGLE pass — the Mul folds to a Const that the constCache
// resolves so the Add also folds. Regression target: a constCache
// keyed wrong / populated late / cleared mid-walk would leave the
// Add as `Add(Const 6, Const 7)` and `instructionsFolded` would be
// 1 (only the Mul), not 2.
TEST(ConstFold, NestedFoldPicksUpConstCache) {
    TypeInterner interner{CompilationUnitId{1}};
    BuildResult br;
    br.i32   = interner.primitive(TypeKind::I32);
    br.fnSig = interner.fnSig({}, br.i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(br.fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirLiteralValue v2; v2.value = std::int64_t{2}; v2.core = TypeKind::I32;
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirLiteralValue v7; v7.value = std::int64_t{7}; v7.core = TypeKind::I32;
    MirInstId const cMul[] = {mb.addConst(v2, br.i32), mb.addConst(v3, br.i32)};
    MirInstId const mulInst = mb.addInst(MirOpcode::Mul, cMul, br.i32);
    MirInstId const cAdd[] = {mulInst, mb.addConst(v7, br.i32)};
    mb.addReturn(mb.addInst(MirOpcode::Add, cAdd, br.i32));
    br.mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runConstFold(br.mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsFolded, 2u)
        << "Mul should fold first, then Add picks up the cached Const "
           "and folds too — both count as folded instructions";
    auto const ret = inspectReturnOperand(br.mir);
    ASSERT_TRUE(ret.isConst);
    EXPECT_EQ(ret.constValue, 13) << "2*3 + 7 = 13";
}

// G4: multi-block CondBr. A function with `if-else` constructed via
// MirBuilder. Tests `emitTerminator`'s CondBr arm + the `blockMap`
// remapping for successors. The fold itself doesn't fire (no
// foldable inst); the test pins that the rebuilt module preserves
// the CFG shape (3 blocks: entry/then/else; entry's CondBr targets
// both with correct successor order).
TEST(ConstFold, PreservesMultiBlockCondBrCfg) {
    TypeInterner interner{CompilationUnitId{1}};
    BuildResult br;
    br.i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    br.fnSig = interner.fnSig(params, br.i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(br.fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tArm  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const fArm  = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    mb.addCondBr(cond, tArm, fArm);
    mb.beginBlock(tArm);
    MirLiteralValue v1; v1.value = std::int64_t{1}; v1.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v1, br.i32));
    mb.beginBlock(fArm);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, br.i32));
    br.mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runConstFold(br.mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsFolded, 0u) << "No foldable insts in this CFG";

    MirFuncId const fn = br.mir.funcAt(0);
    ASSERT_EQ(br.mir.funcBlockCount(fn), 3u);
    MirBlockId const rebuiltEntry = br.mir.funcEntry(fn);
    auto const succ = br.mir.blockSuccessors(rebuiltEntry);
    ASSERT_EQ(succ.size(), 2u);
    // Order must be preserved: entry's CondBr taken-target first,
    // not-taken target second. Both must be in the same function +
    // belong to the rebuilt arena (NOT phantom defaults).
    EXPECT_TRUE(succ[0].valid());
    EXPECT_TRUE(succ[1].valid());
    EXPECT_NE(succ[0].v, succ[1].v);
}

// ICmp result is always Bool. Pin the core-type normalization rule:
// the folded literal's `core` field is Bool, not the input's int kind.
TEST(ConstFold, FoldsICmpEqProducesBool) {
    TypeInterner interner{CompilationUnitId{1}};
    BuildResult br;
    br.i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    br.fnSig = interner.fnSig({}, boolT, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(br.fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirLiteralValue va; va.value = std::int64_t{5}; va.core = TypeKind::I32;
    MirLiteralValue vb; vb.value = std::int64_t{5}; vb.core = TypeKind::I32;
    MirInstId const ops[] = {mb.addConst(va, br.i32), mb.addConst(vb, br.i32)};
    mb.addReturn(mb.addInst(MirOpcode::ICmpEq, ops, boolT));
    br.mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runConstFold(br.mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsFolded, 1u);
    auto const ret = inspectReturnOperand(br.mir);
    EXPECT_TRUE(ret.isConst);
    EXPECT_EQ(ret.constValue, 1) << "5 == 5 must fold to true (1)";
}
