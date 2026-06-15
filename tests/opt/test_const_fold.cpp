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
#include <limits>

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

// Left-associative chain shape: `10 - 3 + 1` lowers to
// Add(Sub(10, 3), 1) — both ops must fold through to Const(8).
// (The right-recursive parser mis-shape Sub(10, Add(3, 1)) would fold
// to 6; pinning 8 here keeps the MIR tier honest about the chain
// shape it receives.)
TEST(ConstFold, FoldsLeftAssociativeChainToEight) {
    TypeInterner interner{CompilationUnitId{1}};
    BuildResult br;
    br.i32   = interner.primitive(TypeKind::I32);
    br.fnSig = interner.fnSig({}, br.i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(br.fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirLiteralValue v10; v10.value = std::int64_t{10}; v10.core = TypeKind::I32;
    MirLiteralValue v3;  v3.value  = std::int64_t{3};  v3.core  = TypeKind::I32;
    MirLiteralValue v1;  v1.value  = std::int64_t{1};  v1.core  = TypeKind::I32;
    MirInstId const subOps[] = {mb.addConst(v10, br.i32), mb.addConst(v3, br.i32)};
    MirInstId const sub = mb.addInst(MirOpcode::Sub, subOps, br.i32);
    MirInstId const addOps[] = {sub, mb.addConst(v1, br.i32)};
    mb.addReturn(mb.addInst(MirOpcode::Add, addOps, br.i32));
    br.mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runConstFold(br.mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsFolded, 2u);
    auto const ret = inspectReturnOperand(br.mir);
    EXPECT_TRUE(ret.isConst);
    EXPECT_EQ(ret.constValue, 8) << "(10 - 3) + 1 must fold to 8";
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

// ─── FC1 (V2-4.X, 2026-06-10): SMod folding + the INT64_MIN/-1 guard ───────

namespace {

// Build `fn() -> i32/i64 { return OP(Const a, Const b); }` for the
// SMod/SDiv fold tests below.
[[nodiscard]] Mir buildConstBinOp(TypeInterner& interner, MirOpcode op,
                                  TypeKind core, std::int64_t a,
                                  std::int64_t b) {
    TypeId const ty    = interner.primitive(core);
    TypeId const fnSig = interner.fnSig({}, ty, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirLiteralValue va; va.value = a; va.core = core;
    MirLiteralValue vb; vb.value = b; vb.core = core;
    MirInstId const ops[] = {mb.addConst(va, ty), mb.addConst(vb, ty)};
    mb.addReturn(mb.addInst(op, ops, ty));
    return std::move(mb).finish();
}

}  // namespace

// SMod folds with C TRUNCATED-remainder semantics (C23 6.5.5: the
// sign follows the DIVIDEND; (a/b)*b + a%b == a with / truncating
// toward zero). A floored-division remainder ((-7)%2 == +1) here
// would be a silent cross-semantics miscompile.
TEST(ConstFold, SModFoldsTruncatedRemainderSemantics) {
    {
        TypeInterner interner{CompilationUnitId{1}};
        Mir mir = buildConstBinOp(interner, MirOpcode::SMod,
                                  TypeKind::I32, -7, 2);
        DiagnosticReporter rep;
        auto const r = opt::passes::runConstFold(mir, interner, rep);
        EXPECT_TRUE(r.ok);
        EXPECT_EQ(r.instructionsFolded, 1u);
        auto const ret = inspectReturnOperand(mir);
        ASSERT_TRUE(ret.isConst);
        EXPECT_EQ(ret.constValue, -1)
            << "(-7) % 2 must be -1 (truncated; floored gives +1).";
    }
    {
        TypeInterner interner{CompilationUnitId{1}};
        Mir mir = buildConstBinOp(interner, MirOpcode::SMod,
                                  TypeKind::I32, 7, -2);
        DiagnosticReporter rep;
        auto const r = opt::passes::runConstFold(mir, interner, rep);
        EXPECT_TRUE(r.ok);
        EXPECT_EQ(r.instructionsFolded, 1u);
        auto const ret = inspectReturnOperand(mir);
        ASSERT_TRUE(ret.isConst);
        EXPECT_EQ(ret.constValue, 1)
            << "7 % (-2) must be +1 (truncated; floored gives -1).";
    }
}

// SMod-by-zero defers to runtime exactly like SDiv-by-zero (folding
// would erase the trap the unoptimized path has).
TEST(ConstFold, SModByZeroDefersToRuntime) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildConstBinOp(interner, MirOpcode::SMod,
                              TypeKind::I32, 7, 0);
    DiagnosticReporter rep;
    auto const r = opt::passes::runConstFold(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsFolded, 0u);
    auto const ret = inspectReturnOperand(mir);
    EXPECT_FALSE(ret.isConst);
    EXPECT_EQ(ret.op, MirOpcode::SMod);
}

// INT64_MIN % -1 must NOT fold (FC1, the new const_eval_arith guard):
// it is C UB (6.5.5p6 — a/b is unrepresentable so a%b is undefined)
// AND, without the guard, the COMPILER ITSELF would execute
// `INT64_MIN % -1` on the host — signed-overflow UB that the
// linux-clang UBSan CI leg traps. The op stays live; the TARGET
// defines the runtime outcome (x86 idiv #DE; arm64 sdiv wraps).
TEST(ConstFold, SModIntMinByMinusOneNotFolded) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildConstBinOp(interner, MirOpcode::SMod, TypeKind::I64,
                              std::numeric_limits<std::int64_t>::min(), -1);
    DiagnosticReporter rep;
    auto const r = opt::passes::runConstFold(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsFolded, 0u)
        << "folding INT64_MIN % -1 is host UB (UBSan-visible) and "
           "erases the target-defined runtime behavior.";
    auto const ret = inspectReturnOperand(mir);
    EXPECT_FALSE(ret.isConst);
    EXPECT_EQ(ret.op, MirOpcode::SMod);
}

// The SDiv twin — the guard fixes a PRE-EXISTING latent host-UB bug
// in the Div arm (it folded `av / bv` with only a bv==0 check;
// INT64_MIN / -1 overflows int64 inside the compiler).
TEST(ConstFold, SDivIntMinByMinusOneNotFolded) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildConstBinOp(interner, MirOpcode::SDiv, TypeKind::I64,
                              std::numeric_limits<std::int64_t>::min(), -1);
    DiagnosticReporter rep;
    auto const r = opt::passes::runConstFold(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsFolded, 0u);
    auto const ret = inspectReturnOperand(mir);
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

TEST(ConstFold, WrapsU32OnAddOverflowToZero) {
    // FC3 c2 (D-CSUBSET-32BIT-ALU-FORMS): the U32 sibling of the I32
    // wrap pin — 0xFFFFFFFFu + 1u is C-DEFINED to be 0 (modulo-2^32).
    // This is the FOLDED shape of the u32_wraparound corpus' witness;
    // a 64-wide fold would produce 0x100000000 and the optimized arm
    // would diverge from the runtime 32-bit add. (The corpus example
    // itself carries no constfold arm — the folded 0xFFFFFFFF constant
    // hits arm64's pre-existing MOVZ-imm16 wide-immediate wall — so
    // this unit pin carries the fold-width contract.)
    TypeInterner interner{CompilationUnitId{1}};
    BuildResult br;
    auto const u32 = interner.primitive(TypeKind::U32);
    br.fnSig = interner.fnSig({}, u32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(br.fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirLiteralValue va; va.value = std::int64_t{0xFFFFFFFF}; va.core = TypeKind::U32;
    MirLiteralValue vb; vb.value = std::int64_t{1};          vb.core = TypeKind::U32;
    MirInstId const ops[] = {mb.addConst(va, u32), mb.addConst(vb, u32)};
    mb.addReturn(mb.addInst(MirOpcode::Add, ops, u32));
    br.mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runConstFold(br.mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsFolded, 1u);
    auto const ret = inspectReturnOperand(br.mir);
    ASSERT_TRUE(ret.isConst);
    EXPECT_EQ(ret.constValue, std::int64_t{0})
        << "u32 wrap: 0xFFFFFFFF + 1 must wrap to 0 (defined "
           "unsigned wraparound)";
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

// D-OPT-CONST-FOLD-PHI-TEST: 3-phase Phi rebuild preservation. A
// regression in deferredPhi handling, rewrite_.clear() ordering, or
// oldPhi/newPhi confusion would silently miscompile any phi-using
// function. This test constructs a 3-block CFG with a Phi at the
// merge, runs ConstFold, and asserts every incoming (value, pred)
// pair in the source's Phi appears verbatim (semantically) in the
// rebuilt Phi.
TEST(ConstFold, PreservesPhiIncomingsAtMergeBlock) {
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
    MirBlockId const merge = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    mb.addCondBr(cond, tArm, fArm);
    mb.beginBlock(tArm);
    MirLiteralValue v1; v1.value = std::int64_t{1}; v1.core = TypeKind::I32;
    MirInstId const c1 = mb.addConst(v1, br.i32);
    mb.addBr(merge);
    mb.beginBlock(fArm);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(v0, br.i32);
    mb.addBr(merge);
    mb.beginBlock(merge);
    MirPhiIncoming const incomings[] = {{c1, tArm}, {c0, fArm}};
    MirInstId const phi = mb.addPhi(br.i32, incomings);
    mb.addReturn(phi);
    br.mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runConstFold(br.mir, interner, rep);
    EXPECT_TRUE(r.ok);

    // Find the rebuilt Phi + assert it has 2 incomings whose values
    // are Consts (1 and 0) AND whose predecessors are valid distinct
    // blocks. A bug in phase 3 (rewrite map missing back-edge values
    // OR predecessor block-id remap drifting) would surface here.
    MirFuncId const fn = br.mir.funcAt(0);
    ASSERT_EQ(br.mir.funcBlockCount(fn), 4u)
        << "All 4 blocks (entry/tArm/fArm/merge) reachable + preserved";

    bool foundPhi = false;
    for (std::uint32_t bi = 0; bi < 4u; ++bi) {
        MirBlockId const b = br.mir.funcBlockAt(fn, bi);
        std::uint32_t const ni = br.mir.blockInstCount(b);
        for (std::uint32_t i = 0; i < ni; ++i) {
            MirInstId const id = br.mir.blockInstAt(b, i);
            if (br.mir.instOpcode(id) != MirOpcode::Phi) continue;
            foundPhi = true;
            auto const inc = br.mir.phiIncomings(id);
            ASSERT_EQ(inc.size(), 2u);
            // Each incoming.value MUST resolve to a Const inst in
            // the rebuilt module.
            for (auto const& edge : inc) {
                EXPECT_EQ(br.mir.instOpcode(edge.value), MirOpcode::Const)
                    << "phi incoming value must be the preserved Const";
                EXPECT_TRUE(edge.pred.valid())
                    << "phi predecessor block must be valid post-rebuild";
            }
            // Both predecessors must be distinct blocks.
            EXPECT_NE(inc[0].pred.v, inc[1].pred.v);
        }
    }
    EXPECT_TRUE(foundPhi) << "Phi inst missing from rebuilt module";
}
