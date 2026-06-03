// MIR-tier CSE / GVN unit tests.
//
// Pins:
//   * SAME-BLOCK CSE: two `Add(a, b)` in straight-line code → second
//     redirected to first (instructionsCsed == 1; both Adds survive
//     until DCE, but later uses point at the first).
//   * COMMUTATIVE CANONICALIZATION: `Add(a, b)` and `Add(b, a)`
//     merge — operands sorted by id before key.
//   * NON-COMMUTATIVE DISTINCTION: `Sub(a, b)` and `Sub(b, a)` do
//     NOT merge — D-OPT1-CSE-NONCOMMUTATIVE-PIN.
//   * SIDE-EFFECT EXCLUSION: two identical Stores (same value, same
//     addr) do NOT merge — both must execute (Store is side-effecting).
//   * LOAD EXCLUSION: two Loads from same address do NOT merge
//     (alias-unsafe defer).
//   * VOLATILE EXCLUSION: a Volatile-flagged binary op does NOT
//     merge with a non-volatile equivalent.
//   * DOMINANCE SCOPING: an expression defined only in one arm of
//     a diamond is NOT available at the join (the arm-scoped entry
//     is rolled back at Leave). Without this rollback, CSE'ing a
//     join-block expression that "matches" a then-arm entry would
//     reference a value that doesn't dominate the join — invalid SSA.
//   * MULTI-FUNCTION: per-function reset.
//   * RUNTIME-INIT CARVE-OUT: parity with the other passes.
//
// The dead duplicates remain in the rebuild (DCE sweeps next).

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/passes/cse.hpp"

#include <gtest/gtest.h>

#include <cstdint>

using namespace dss;

namespace {

std::size_t countOpInModule(Mir const& mir, MirOpcode want) {
    std::size_t n = 0;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t i2 = 0; i2 < ni; ++i2) {
                if (mir.instOpcode(mir.blockInstAt(b, i2)) == want) ++n;
            }
        }
    }
    return n;
}

} // namespace

// Same block: two Add(c, c) → CSE the second; only one survives
// effective use; both physically remain (DCE sweeps next).
TEST(Cse, SameBlockArithmeticCsed) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirLiteralValue v; v.value = std::int64_t{5}; v.core = TypeKind::I32;
    MirInstId const c = mb.addConst(v, i32);
    MirInstId const ops1[] = {c, c};
    MirInstId const add1 = mb.addInst(MirOpcode::Add, ops1, i32);
    MirInstId const ops2[] = {c, c};
    MirInstId const add2 = mb.addInst(MirOpcode::Add, ops2, i32);
    // Use both add results so SSA def-use is well-formed.
    MirInstId const opsSum[] = {add1, add2};
    MirInstId const sum = mb.addInst(MirOpcode::Add, opsSum, i32);
    mb.addReturn(sum);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    // Add(c,c) appears twice → second CSE'd. Add(add1, add2) is
    // distinct (different operands) and stays.
    EXPECT_EQ(r.instructionsCsed, 1u);
}

// Commutative-canonicalization: Add(a,b) and Add(b,a) → merge.
TEST(Cse, CommutativeOperandsCanonicalized) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const params[] = {i32, i32};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const a = mb.addArg(0, i32);
    MirInstId const b = mb.addArg(1, i32);
    MirInstId const ops_ab[] = {a, b};
    MirInstId const add_ab = mb.addInst(MirOpcode::Add, ops_ab, i32);
    MirInstId const ops_ba[] = {b, a};  // swapped — still commutative-equal
    MirInstId const add_ba = mb.addInst(MirOpcode::Add, ops_ba, i32);
    MirInstId const sum[] = {add_ab, add_ba};
    MirInstId const result = mb.addInst(MirOpcode::Add, sum, i32);
    mb.addReturn(result);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsCsed, 1u)
        << "Add(a,b) and Add(b,a) must canonicalize to the same key";
}

// D-OPT1-CSE-NONCOMMUTATIVE-PIN — the load-bearing rigor pin.
// Sub(a, b) and Sub(b, a) MUST NOT merge.
TEST(Cse, NonCommutativeSubtractionNotMerged) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const params[] = {i32, i32};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const a = mb.addArg(0, i32);
    MirInstId const b = mb.addArg(1, i32);
    MirInstId const ops_ab[] = {a, b};
    MirInstId const sub_ab = mb.addInst(MirOpcode::Sub, ops_ab, i32);
    MirInstId const ops_ba[] = {b, a};
    MirInstId const sub_ba = mb.addInst(MirOpcode::Sub, ops_ba, i32);
    MirInstId const sum[] = {sub_ab, sub_ba};
    MirInstId const result = mb.addInst(MirOpcode::Add, sum, i32);
    mb.addReturn(result);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsCsed, 0u)
        << "Sub(a,b) and Sub(b,a) are distinct expressions — must NOT "
           "merge (D-OPT1-CSE-NONCOMMUTATIVE-PIN)";
}

// Side-effect exclusion: two identical Stores must not merge.
TEST(Cse, SideEffectingStoreNotCsed) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v; v.value = std::int64_t{42}; v.core = TypeKind::I32;
    MirInstId const c = mb.addConst(v, i32);
    MirInstId const s1[] = {c, slot};
    (void)mb.addInst(MirOpcode::Store, s1, InvalidType);
    MirInstId const s2[] = {c, slot};
    (void)mb.addInst(MirOpcode::Store, s2, InvalidType);
    mb.addReturn();
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsCsed, 0u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Store), 2u);
}

// Load exclusion: two Loads from same address NOT merged (alias-
// unsafe defer until alias analysis lands).
TEST(Cse, LoadNotCsed) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v; v.value = std::int64_t{0}; v.core = TypeKind::I32;
    MirInstId const c = mb.addConst(v, i32);
    MirInstId const s[] = {c, slot};
    (void)mb.addInst(MirOpcode::Store, s, InvalidType);
    MirInstId const lops[] = {slot};
    MirInstId const ld1 = mb.addInst(MirOpcode::Load, lops, i32);
    MirInstId const ld2 = mb.addInst(MirOpcode::Load, lops, i32);
    MirInstId const sum[] = {ld1, ld2};
    MirInstId const r = mb.addInst(MirOpcode::Add, sum, i32);
    mb.addReturn(r);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.instructionsCsed, 0u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Load), 2u);
}

// Volatile exclusion: a Volatile-flagged binary op participates in
// neither side of a CSE merge.
TEST(Cse, VolatileBinaryOpNotCsed) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const params[] = {i32, i32};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const a = mb.addArg(0, i32);
    MirInstId const b = mb.addArg(1, i32);
    MirInstId const ops[] = {a, b};
    // First Add is plain.
    MirInstId const add1 = mb.addInst(MirOpcode::Add, ops, i32);
    // Second Add is volatile-flagged.
    MirInstId const add2 = mb.addInst(MirOpcode::Add, ops, i32,
                                       /*payload*/0, MirInstFlags::Volatile);
    MirInstId const sum[] = {add1, add2};
    MirInstId const r = mb.addInst(MirOpcode::Add, sum, i32);
    mb.addReturn(r);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.instructionsCsed, 0u)
        << "Volatile-flagged binary op must not participate in CSE";
}

// Dominance scoping: an expression defined only in the then-arm
// of a diamond must NOT be available at the join. A CSE bug that
// fails to roll back the then-arm's scope on Leave would attempt
// to substitute a join-block use with a value that doesn't
// dominate the join — invalid SSA + silent miscompile.
TEST(Cse, DominanceScopingDiamondNoCrossArmMerge) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT, i32, i32};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tArm  = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const fArm  = mb.createBlock(StructCfMarker::IfElse);
    MirBlockId const join  = mb.createBlock(StructCfMarker::IfJoin);

    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    MirInstId const a = mb.addArg(1, i32);
    MirInstId const b = mb.addArg(2, i32);
    mb.addCondBr(cond, tArm, fArm);

    // then-arm computes a+b; falls to join with phi-incoming.
    mb.beginBlock(tArm);
    MirInstId const ops_then[] = {a, b};
    MirInstId const add_then = mb.addInst(MirOpcode::Add, ops_then, i32);
    mb.addBr(join);

    // else-arm: just a constant.
    mb.beginBlock(fArm);
    MirLiteralValue v; v.value = std::int64_t{0}; v.core = TypeKind::I32;
    MirInstId const zero = mb.addConst(v, i32);
    mb.addBr(join);

    // Join computes a+b AGAIN. Even though syntactically identical to
    // the then-arm's add, that def doesn't dominate the join (the
    // else-arm bypasses it). CSE must NOT merge.
    mb.beginBlock(join);
    MirPhiIncoming const incs[] = {{add_then, tArm}, {zero, fArm}};
    MirInstId const phi = mb.addPhi(i32, incs);
    MirInstId const ops_join[] = {a, b};
    MirInstId const add_join = mb.addInst(MirOpcode::Add, ops_join, i32);
    MirInstId const sum[] = {phi, add_join};
    MirInstId const r = mb.addInst(MirOpcode::Add, sum, i32);
    mb.addReturn(r);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.instructionsCsed, 0u)
        << "then-arm's Add does not dominate the join — must NOT be "
           "available for CSE at join";
}

// Dominance scoping POSITIVE case: an expression in the entry block
// IS available everywhere it dominates. Two Adds with same operands
// at entry and at then-arm → merge (entry dominates then-arm).
TEST(Cse, DominanceScopingEntryDefAvailableInChild) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT, i32, i32};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tArm  = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const fArm  = mb.createBlock(StructCfMarker::IfElse);
    MirBlockId const join  = mb.createBlock(StructCfMarker::IfJoin);

    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    MirInstId const a = mb.addArg(1, i32);
    MirInstId const b = mb.addArg(2, i32);
    MirInstId const ops_entry[] = {a, b};
    MirInstId const add_entry = mb.addInst(MirOpcode::Add, ops_entry, i32);
    mb.addCondBr(cond, tArm, fArm);

    mb.beginBlock(tArm);
    MirInstId const ops_then[] = {a, b};  // identical key → CSE
    MirInstId const add_then = mb.addInst(MirOpcode::Add, ops_then, i32);
    mb.addBr(join);

    mb.beginBlock(fArm);
    mb.addBr(join);

    mb.beginBlock(join);
    MirPhiIncoming const incs[] = {{add_then, tArm}, {add_entry, fArm}};
    MirInstId const phi = mb.addPhi(i32, incs);
    mb.addReturn(phi);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.instructionsCsed, 1u)
        << "entry's Add dominates then-arm — the duplicate must CSE";
}

// Multi-function: per-function reset of the cseMap.
TEST(Cse, MultiFunctionModuleEachCsedIndependently) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const params[] = {i32, i32};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    for (std::uint32_t fnIdx = 0; fnIdx < 2; ++fnIdx) {
        mb.addFunction(fnSig, SymbolId{100u + fnIdx});
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(entry);
        MirInstId const a = mb.addArg(0, i32);
        MirInstId const b = mb.addArg(1, i32);
        MirInstId const ops[] = {a, b};
        MirInstId const add1 = mb.addInst(MirOpcode::Add, ops, i32);
        MirInstId const add2 = mb.addInst(MirOpcode::Add, ops, i32);
        MirInstId const sum[] = {add1, add2};
        MirInstId const r = mb.addInst(MirOpcode::Add, sum, i32);
        mb.addReturn(r);
    }
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.instructionsCsed, 2u)
        << "each function's duplicate Add must CSE independently — "
           "counter accumulates across functions";
}

// Type-discriminator: Add<I32>(a,b) and Add<I64>(a,b) must NOT merge.
// The `CseKey.type` field participates in equality + hash; a
// regression that dropped it would silently fuse different-width
// arithmetic into one (catastrophic miscompile).
TEST(Cse, ResultTypeDiscriminatesKey) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const i64   = interner.primitive(TypeKind::I64);
    TypeId const params[] = {i32, i32};
    TypeId const fnSig = interner.fnSig(params, i64, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const a = mb.addArg(0, i32);
    MirInstId const b = mb.addArg(1, i32);
    MirInstId const ops[] = {a, b};
    // Two Adds with same operands but different result types.
    MirInstId const add32 = mb.addInst(MirOpcode::Add, ops, i32);
    MirInstId const add64 = mb.addInst(MirOpcode::Add, ops, i64);
    (void)add32; (void)add64;
    // Return one of them (don't care which) — just need both alive.
    mb.addReturn(add64);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsCsed, 0u)
        << "different result types must not merge — the type field is "
           "load-bearing in the CSE key";
}

// Transitive CSE chain: Add(a,b) emitted three times. Second merges
// to first via the scope; third's operand-resolution walks
// `cseMap_` (via `resolveTransitive` in `buildKey`) → its key
// canonicalizes against the first def's id too → merges. This
// exercises `resolveTransitive` and path-compression.
TEST(Cse, TransitiveChainResolves) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const params[] = {i32, i32};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const a = mb.addArg(0, i32);
    MirInstId const b = mb.addArg(1, i32);
    MirInstId const ops[] = {a, b};
    MirInstId const add1 = mb.addInst(MirOpcode::Add, ops, i32);
    MirInstId const add2 = mb.addInst(MirOpcode::Add, ops, i32);
    MirInstId const add3 = mb.addInst(MirOpcode::Add, ops, i32);
    // Build a downstream consumer using add2 as an operand. After
    // CSE, add2 → add1; the consumer's operand resolves to add1.
    MirInstId const consumer_ops[] = {add2, add3};
    MirInstId const consumer = mb.addInst(MirOpcode::Add, consumer_ops, i32);
    mb.addReturn(consumer);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsCsed, 2u)
        << "add2 + add3 must both CSE to add1 (operands canonicalize "
           "through cseMap_ — path-compression invariant)";
}

// Runtime-init carve-out: module with `globalInitFunc.valid()` emits
// X_OptPassSkipped Info + returns ok=true + zero counters. Parity
// with ConstFold/Dce/Mem2Reg/CopyProp.
TEST(Cse, RuntimeInitGlobalsModuleEmitsXOptPassSkippedInfo) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const initSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    MirFuncId const initFn = mb.addFunction(initSig, SymbolId{50});
    MirBlockId const initEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(initEntry);
    mb.addReturn();
    mb.addGlobal(i32, SymbolId{200}, UINT32_MAX, initFn);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsCsed, 0u);
    std::size_t infoCount = 0;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::X_OptPassSkipped) ++infoCount;
    }
    EXPECT_EQ(infoCount, 1u);
}
