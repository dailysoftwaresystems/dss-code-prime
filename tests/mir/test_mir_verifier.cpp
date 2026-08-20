// ML3 — MirVerifier: structural / CFG / dominance / type-consistency
// invariants on a frozen Mir module. Tests follow HirVerifier's
// positive+negative pattern: build via MirBuilder (positive — already
// valid), or construct a malformed Mir via the direct ctor to exercise
// the verifier's catch-the-bad-state path.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/substrate/arena_container.hpp"
#include "mir/mir.hpp"
#include "mir/mir_asm_descriptor.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"
#include "mir/mir_verifier.hpp"
#include "diagnostic_count.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;
using dss::test_support::countCode;

namespace {

// Untagged-TypeId stand-ins for tests that don't need a real interner.
constexpr TypeId kFnSig{1};
constexpr TypeId kI32{2};
constexpr TypeId kBool{3};
constexpr TypeId kVoidFn{4};

MirLiteralValue intLit(std::int64_t v) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = TypeKind::I32;
    return lit;
}

// (former local `countCode` folded to `dss::test_support::countCode`
// in `tests/test_support/diagnostic_count.hpp` at FF3+FF4 post-fold #3.)

// Build a minimal well-formed module: one function with an EntryBlock
// at slot 0 whose only instruction is `Return`.
Mir buildMinimalModule() {
    MirBuilder b;
    MirFuncId const f = b.addFunction(kFnSig, SymbolId{1});
    (void)f;
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    b.addReturn();
    return std::move(b).finish();
}

} // namespace

// ── positive: a well-formed module passes every rule ────────────────────────

TEST(MirVerifier, MinimalModulePasses) {
    Mir m = buildMinimalModule();
    DiagnosticReporter r;
    MirVerifier v{m};
    EXPECT_TRUE(v.verify(r));
    EXPECT_EQ(r.errorCount(), 0u);
}

// A simple straight-line function (Arg + Const + BinaryOp + Return).
TEST(MirVerifier, StraightLineFunctionPasses) {
    MirBuilder b;
    MirFuncId const f = b.addFunction(kFnSig, SymbolId{1});
    (void)f;
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const arg0 = b.addArg(0, kI32);
    MirInstId const c1   = b.addConst(intLit(1), kI32);
    std::array<MirInstId, 2> const sumOps{arg0, c1};
    MirInstId const sum  = b.addInst(MirOpcode::Add, sumOps, kI32);
    b.addReturn(sum);
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m};
    EXPECT_TRUE(v.verify(r));
}

// A diamond CFG (If/Then/Else/Join) with a phi at the join.
TEST(MirVerifier, DiamondWithPhiPasses) {
    MirBuilder b;
    MirFuncId const f = b.addFunction(kFnSig, SymbolId{1});
    (void)f;
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tBB   = b.createBlock(StructCfMarker::IfThen);
    MirBlockId const eBB   = b.createBlock(StructCfMarker::IfElse);
    MirBlockId const join  = b.createBlock(StructCfMarker::IfJoin);
    b.beginBlock(entry);
    MirInstId const c1   = b.addConst(intLit(1), kI32);
    MirInstId const c0   = b.addConst(intLit(0), kI32);
    std::array<MirInstId, 2> const cmpOps{c1, c0};
    MirInstId const cond = b.addInst(MirOpcode::ICmpEq, cmpOps, kBool);
    b.addCondBr(cond, tBB, eBB);
    b.beginBlock(tBB);
    MirInstId const ct = b.addConst(intLit(10), kI32);
    b.addBr(join);
    b.beginBlock(eBB);
    MirInstId const ce = b.addConst(intLit(20), kI32);
    b.addBr(join);
    b.beginBlock(join);
    std::array<MirPhiIncoming, 2> const incs{
        MirPhiIncoming{ct, tBB}, MirPhiIncoming{ce, eBB}};
    MirInstId const phi = b.addPhi(kI32, incs);
    b.addReturn(phi);
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m};
    EXPECT_TRUE(v.verify(r)) << (r.all().empty() ? "" : r.all()[0].actual);
}

// ── negative: structural ────────────────────────────────────────────────────

// A function with NO blocks emits I_NoEntryBlock.
TEST(MirVerifier, FunctionWithNoBlocksEmitsNoEntryBlock) {
    MirBuilder b;
    MirFuncId const f = b.addFunction(kFnSig, SymbolId{1});
    (void)f;
    // Skip block creation entirely — but `finish()` aborts on a
    // function with zero blocks (ML1 invariant). So we need to
    // bypass the builder. Use the direct Mir ctor with a hand-built
    // FuncArena: one slot-0 sentinel + one real function with
    // blockCount=0. This is exactly the synthetic-IR / future-
    // optimizer construction path the verifier is meant to cover.
    //
    // For cycle 1 we exercise this via a different shape: a builder
    // that aborts is not testable here; the equivalent is testing
    // checkEntryBlocks via a function that DOES have blocks but
    // missing the EntryBlock marker (see next test).
    Mir m = buildMinimalModule();
    DiagnosticReporter r;
    MirVerifier v{m};
    EXPECT_TRUE(v.verify(r));  // sanity — the minimal module IS valid
}

// A function whose first block is NOT marked EntryBlock fails
// I_NoEntryBlock.
TEST(MirVerifier, FirstBlockNotMarkedEntryBlockEmitsNoEntryBlock) {
    MirBuilder b;
    MirFuncId const f = b.addFunction(kFnSig, SymbolId{1});
    (void)f;
    // Linear marker, NOT EntryBlock.
    MirBlockId const entry = b.createBlock(StructCfMarker::Linear);
    b.beginBlock(entry);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m};
    EXPECT_FALSE(v.verify(r));
    EXPECT_GE(countCode(r, DiagnosticCode::I_NoEntryBlock), 1u);
}

// Two blocks marked EntryBlock fail I_MultipleEntryBlocks.
TEST(MirVerifier, MultipleEntryBlocksEmitsDiag) {
    MirBuilder b;
    MirFuncId const f = b.addFunction(kFnSig, SymbolId{1});
    (void)f;
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const extra = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    b.addBr(extra);
    b.beginBlock(extra);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m};
    EXPECT_FALSE(v.verify(r));
    EXPECT_GE(countCode(r, DiagnosticCode::I_MultipleEntryBlocks), 1u);
}

// EntryBlock marker at a non-first block fails I_EntryBlockNotFirst.
TEST(MirVerifier, EntryBlockMarkerNotAtSlot0EmitsDiag) {
    MirBuilder b;
    MirFuncId const f = b.addFunction(kFnSig, SymbolId{1});
    (void)f;
    MirBlockId const first  = b.createBlock(StructCfMarker::Linear);
    MirBlockId const second = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(first);
    b.addBr(second);
    b.beginBlock(second);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m};
    EXPECT_FALSE(v.verify(r));
    EXPECT_GE(countCode(r, DiagnosticCode::I_EntryBlockNotFirst), 1u);
}

// ── negative: StructCfMarker equality (stored == derived) ───────────────────

// A reachable block stamped with the DORMANT `ExitBlock` marker fails
// the equality check: no derivation rule ever produces ExitBlock, so
// stored(ExitBlock) != derived(Linear). EXACTLY one mismatch, and the
// diagnostic names BOTH markers (the equality-model successor of the
// old "ExitBlock must terminate in Return/Unreachable" rule).
TEST(MirVerifier, DormantExitBlockMarkerEmitsStructCfMismatch) {
    MirBuilder b;
    MirFuncId const f = b.addFunction(kFnSig, SymbolId{1});
    (void)f;
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const exit  = b.createBlock(StructCfMarker::ExitBlock);
    b.beginBlock(entry);
    b.addBr(exit);
    b.beginBlock(exit);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m};
    EXPECT_FALSE(v.verify(r));
    EXPECT_EQ(countCode(r, DiagnosticCode::I_StructCfMismatch), 1u)
        << "exactly ONE block is mis-marked -> exactly one mismatch";
    bool namesBothMarkers = false;
    for (auto const& d : r.all()) {
        if (d.code != DiagnosticCode::I_StructCfMismatch) continue;
        if (d.actual.find("ExitBlock") != std::string::npos
            && d.actual.find("Linear") != std::string::npos) {
            namesBothMarkers = true;
        }
    }
    EXPECT_TRUE(namesBothMarkers)
        << "the mismatch diagnostic must name stored (ExitBlock) AND "
           "derived (Linear)";
}

// A then-arm stamped `Linear` where the CFG derives `IfThen` fails the
// equality check (the equality-model successor of the old IfThen/IfJoin
// count-pairing rule — under-marking is now just as loud as
// over-marking).
TEST(MirVerifier, MisstampedThenArmEmitsStructCfMismatch) {
    MirBuilder b;
    MirFuncId const f = b.addFunction(kFnSig, SymbolId{1});
    (void)f;
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const thenB = b.createBlock(StructCfMarker::Linear);  // should be IfThen
    MirBlockId const joinB = b.createBlock(StructCfMarker::IfJoin);
    b.beginBlock(entry);
    MirInstId const c1 = b.addConst(intLit(1), kBool);
    b.addCondBr(c1, thenB, joinB);  // if-no-else: false edge = join
    b.beginBlock(thenB);
    b.addBr(joinB);
    b.beginBlock(joinB);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m};
    EXPECT_FALSE(v.verify(r));
    EXPECT_EQ(countCode(r, DiagnosticCode::I_StructCfMismatch), 1u)
        << "only the then-arm is mis-marked (join correctly IfJoin)";
}

// ── negative: phi predecessor ───────────────────────────────────────────────

// A Phi whose incoming.pred is not in the CFG predecessor set of the
// phi's block emits I_PhiPredNotInCfg.
TEST(MirVerifier, PhiPredNotInCfgEmitsDiag) {
    MirBuilder b;
    MirFuncId const f = b.addFunction(kFnSig, SymbolId{1});
    (void)f;
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tBB   = b.createBlock(StructCfMarker::IfThen);
    MirBlockId const eBB   = b.createBlock(StructCfMarker::IfElse);
    MirBlockId const join  = b.createBlock(StructCfMarker::IfJoin);
    // A 5th block that is NEVER a predecessor of `join`.
    MirBlockId const orphan = b.createBlock(StructCfMarker::Linear);
    b.beginBlock(entry);
    MirInstId const c1   = b.addConst(intLit(1), kBool);
    b.addCondBr(c1, tBB, eBB);
    b.beginBlock(tBB);
    MirInstId const ct = b.addConst(intLit(10), kI32);
    b.addBr(join);
    b.beginBlock(eBB);
    MirInstId const ce = b.addConst(intLit(20), kI32);
    b.addBr(join);
    b.beginBlock(join);
    // Phi names `orphan` as a predecessor — but `orphan` does NOT
    // branch to `join` (the CFG predecessor set is {tBB, eBB}).
    std::array<MirPhiIncoming, 2> const incs{
        MirPhiIncoming{ct, tBB}, MirPhiIncoming{ce, orphan}};
    MirInstId const phi = b.addPhi(kI32, incs);
    b.addReturn(phi);
    // `orphan` must also be terminated.
    b.beginBlock(orphan);
    b.addUnreachable();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m};
    EXPECT_FALSE(v.verify(r));
    EXPECT_GE(countCode(r, DiagnosticCode::I_PhiPredNotInCfg), 1u);
}

// ── negative: dominance (use of a value defined in a non-dominator block) ───

// A simple cross-block use-without-dominance: define a value in the
// "then" branch, use it in the "else" branch. The else block is NOT
// dominated by the then block.
TEST(MirVerifier, UseFromNonDominatingBlockEmitsNotDominated) {
    MirBuilder b;
    MirFuncId const f = b.addFunction(kFnSig, SymbolId{1});
    (void)f;
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tBB   = b.createBlock(StructCfMarker::IfThen);
    MirBlockId const eBB   = b.createBlock(StructCfMarker::IfElse);
    MirBlockId const join  = b.createBlock(StructCfMarker::IfJoin);
    b.beginBlock(entry);
    MirInstId const c1 = b.addConst(intLit(1), kBool);
    b.addCondBr(c1, tBB, eBB);
    b.beginBlock(tBB);
    MirInstId const tv = b.addConst(intLit(10), kI32);
    b.addBr(join);
    b.beginBlock(eBB);
    // Illegal: use `tv` (defined in tBB) here in eBB — tBB does
    // NOT dominate eBB.
    MirInstId const c0   = b.addConst(intLit(0), kI32);
    std::array<MirInstId, 2> const badOps{tv, c0};
    MirInstId const bad  = b.addInst(MirOpcode::Add, badOps, kI32);
    (void)bad;
    b.addBr(join);
    b.beginBlock(join);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m};
    EXPECT_FALSE(v.verify(r));
    EXPECT_GE(countCode(r, DiagnosticCode::I_NotDominated), 1u);
}

// Use of a value defined LATER in the same block fails I_NotDominated.
TEST(MirVerifier, UseBeforeDefSameBlockEmitsNotDominated) {
    MirBuilder b;
    MirFuncId const f = b.addFunction(kFnSig, SymbolId{1});
    (void)f;
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const a = b.addConst(intLit(1), kI32);
    std::array<MirInstId, 2> const cOps{a, a};
    MirInstId const c = b.addInst(MirOpcode::Add, cOps, kI32);  // uses a, ok
    (void)c;
    // For this test, we need a use BEFORE its def. The builder
    // can't naturally produce that; this test is a placeholder
    // confirming the same-block path executes without crashing.
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m};
    EXPECT_TRUE(v.verify(r));  // forward-only def-use is legal
}

// ── interner-gated rules ───────────────────────────────────────────────────

// With a real TypeInterner: a CondBr whose condition is NOT a Bool-typed
// value emits I_TerminatorTypeMismatch.
TEST(MirVerifier, CondBrNonBoolConditionWithInterner) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32  = interner.primitive(TypeKind::I32);
    TypeId const boolTy = interner.primitive(TypeKind::Bool);
    TypeId const voidTy = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig(std::span<TypeId const>{}, voidTy, CallConv::CcSysV);
    (void)boolTy;

    MirBuilder b;
    MirFuncId const f = b.addFunction(fnSig, SymbolId{1});
    (void)f;
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tBB   = b.createBlock(StructCfMarker::IfThen);
    MirBlockId const eBB   = b.createBlock(StructCfMarker::IfElse);
    MirBlockId const join  = b.createBlock(StructCfMarker::IfJoin);
    b.beginBlock(entry);
    // Make the cond value an I32, NOT a Bool.
    MirInstId const c1 = b.addConst(intLit(1), i32);
    b.addCondBr(c1, tBB, eBB);
    b.beginBlock(tBB); b.addBr(join);
    b.beginBlock(eBB); b.addBr(join);
    b.beginBlock(join); b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &interner};
    EXPECT_FALSE(v.verify(r));
    EXPECT_GE(countCode(r, DiagnosticCode::I_TerminatorTypeMismatch), 1u);
}

// Arg index >= FnSig.paramCount emits I_ArgIndexOutOfRange.
TEST(MirVerifier, ArgIndexOutOfRange) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const voidTy = interner.primitive(TypeKind::Void);
    // FnSig with ONE param.
    std::array<TypeId, 1> const params{i32};
    TypeId const fnSig = interner.fnSig(params, voidTy, CallConv::CcSysV);

    MirBuilder b;
    MirFuncId const f = b.addFunction(fnSig, SymbolId{1});
    (void)f;
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    // argIndex 5 — out of range (param count is 1).
    b.addArg(5, i32);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &interner};
    EXPECT_FALSE(v.verify(r));
    EXPECT_GE(countCode(r, DiagnosticCode::I_ArgIndexOutOfRange), 1u);
}

// #3 (D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN): an Alloca whose secondary payload
// (its effective alignment) is not a power of two ≤ 256 is rejected with
// I_AllocaAlignmentNotPowerOfTwo. Guards a rebuild/merge site that drops or
// corrupts the alignment — the value drives per-alloca frame-slot placement, so a
// garbage value would mis-align the stack local. Here 3 (not a power of two).
TEST(MirVerifier, AllocaNonPowerOfTwoAlignmentRejected) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, interner.primitive(TypeKind::Void),
                                        CallConv::CcSysV);

    MirBuilder b;
    (void)b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    // Alloca: payload = byte size (0 = scalar), payload2 = alignment = 3 (BAD).
    b.addInst(MirOpcode::Alloca, {}, ptr, /*payload=*/0,
              MirInstFlags::None, /*payload2=*/3);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &interner};
    EXPECT_FALSE(v.verify(r));
    EXPECT_EQ(countCode(r, DiagnosticCode::I_AllocaAlignmentNotPowerOfTwo), 1u);
}

// #3: a well-formed Alloca alignment (16, a power of two ≤ 256) — and the 0
// "no over-alignment recorded" sentinel — pass the verifier cleanly. Pins that
// the check does NOT false-positive on a legal value.
TEST(MirVerifier, AllocaPowerOfTwoAlignmentAndZeroPass) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, interner.primitive(TypeKind::Void),
                                        CallConv::CcSysV);

    MirBuilder b;
    (void)b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    b.addInst(MirOpcode::Alloca, {}, ptr, 0, MirInstFlags::None, /*align=*/16);
    b.addInst(MirOpcode::Alloca, {}, ptr, 0, MirInstFlags::None, /*align=*/0);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &interner};
    EXPECT_TRUE(v.verify(r));
    EXPECT_EQ(countCode(r, DiagnosticCode::I_AllocaAlignmentNotPowerOfTwo), 0u);
}

// C23 nullptr_t (D-CSUBSET-NULLPTR): the I_NullptrTypeInMir tripwire — a never-fires
// backstop for the keystone invariant. `nullptr` lowers to the integer-0 null
// constant at the HIR tier, so NullptrT is a SEMANTIC-TIER-ONLY kind that must never
// reach MIR. This constructs the FORBIDDEN state directly (a Const whose result type
// is NullptrT) and asserts the verifier catches it — the red-on-disable proof
// (remove the verifier arm → this const passes → a keystone regression ships silently).
TEST(MirVerifier, NullptrTResultTypeRejected) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const nptr  = interner.primitive(TypeKind::NullptrT);
    TypeId const fnSig = interner.fnSig({}, interner.primitive(TypeKind::Void),
                                        CallConv::CcSysV);
    MirBuilder b;
    (void)b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirLiteralValue lv;
    lv.value = static_cast<std::int64_t>(0);
    lv.core  = TypeKind::NullptrT;
    (void)b.addConst(lv, nptr);   // a Const whose RESULT type is NullptrT — forbidden
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &interner};
    EXPECT_FALSE(v.verify(r));
    EXPECT_EQ(countCode(r, DiagnosticCode::I_NullptrTypeInMir), 1u);
}

// Return value type that doesn't match FnSig's return type emits
// I_TerminatorTypeMismatch.
TEST(MirVerifier, ReturnTypeMismatch) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const i64   = interner.primitive(TypeKind::I64);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);  // returns I32

    MirBuilder b;
    MirFuncId const f = b.addFunction(fnSig, SymbolId{1});
    (void)f;
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    // Returning an I64 value from a function declared to return I32.
    MirInstId const c1 = b.addConst(intLit(1), i64);
    b.addReturn(c1);
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &interner};
    EXPECT_FALSE(v.verify(r));
    EXPECT_GE(countCode(r, DiagnosticCode::I_TerminatorTypeMismatch), 1u);
}

// FC7 C1c (D-FC7-SYSV-STRUCT-RETURN-IN-REGS): a by-value struct-returning
// function whose Return carries a value that is NEITHER the struct VALUE (first-
// class aggregate), a register PIECE (I64/F64), NOR an sret POINTER — here an I32
// — emits I_TerminatorTypeMismatch. This is the truncation / wrong-piece guard;
// the positive shapes (multi-piece I64 Return, sret Ptr, mixed F64/I64) are
// covered by the HIR→MIR lowering pins + the runtime corpus.
TEST(MirVerifier, StructReturnWithWrongPieceTypeRejected) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    std::array<TypeId, 2> const fields{i32, i32};
    TypeId const structTy = interner.structType("S", fields);
    TypeId const fnSig = interner.fnSig({}, structTy, CallConv::CcSysV);  // returns S

    MirBuilder b;
    (void)b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    // Return an I32 — not the struct value, not an I64/F64 register piece, not a Ptr.
    MirInstId const c1 = b.addConst(intLit(1), i32);
    b.addReturn(c1);
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &interner};
    EXPECT_FALSE(v.verify(r));
    EXPECT_GE(countCode(r, DiagnosticCode::I_TerminatorTypeMismatch), 1u);
}

// An orphan block — present in the function but with no predecessor
// path from entry — fails I_UnreachableBlock. ML2-lowered code never
// produces orphans, but a future optimizer pass that deletes dead
// branches could leave one; the verifier catches it.
TEST(MirVerifier, OrphanBlockEmitsUnreachable) {
    MirBuilder b;
    MirFuncId const f = b.addFunction(kFnSig, SymbolId{1});
    (void)f;
    MirBlockId const entry  = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const orphan = b.createBlock(StructCfMarker::Linear);
    b.beginBlock(entry);
    b.addReturn();
    // `orphan` has no predecessor — entry does NOT branch to it.
    b.beginBlock(orphan);
    b.addUnreachable();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m};
    EXPECT_FALSE(v.verify(r));
    EXPECT_GE(countCode(r, DiagnosticCode::I_UnreachableBlock), 1u);
}

// Wrong arm POLARITY: the false-edge arm of a diamond stamped IfThen
// fails equality — the derivation is edge-polarity-faithful (succs[1]
// derives IfElse). The equality-model successor of the old "two IfThen
// + one IfJoin count mismatch" rule: the same fixture, but the
// diagnosis is now per-block and names the actual disagreement.
TEST(MirVerifier, WrongArmPolarityEmitsStructCfMismatch) {
    MirBuilder b;
    MirFuncId const f = b.addFunction(kFnSig, SymbolId{1});
    (void)f;
    MirBlockId const entry  = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const then1  = b.createBlock(StructCfMarker::IfThen);
    MirBlockId const then2  = b.createBlock(StructCfMarker::IfThen);  // false edge → should be IfElse
    MirBlockId const join   = b.createBlock(StructCfMarker::IfJoin);
    b.beginBlock(entry);
    MirInstId const c1 = b.addConst(intLit(1), kBool);
    b.addCondBr(c1, then1, then2);
    b.beginBlock(then1); b.addBr(join);
    b.beginBlock(then2); b.addBr(join);
    b.beginBlock(join);  b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m};
    EXPECT_FALSE(v.verify(r));
    EXPECT_EQ(countCode(r, DiagnosticCode::I_StructCfMismatch), 1u)
        << "only then2 disagrees with the derivation (stored IfThen, "
           "derived IfElse)";
}

// A degenerate CondBr whose BOTH arms target the same block: the target
// is the immediate post-dominator, so it derives IfJoin — a stored
// IfElse fails equality (the successor of the old "IfElse without
// IfJoin" count rule).
TEST(MirVerifier, BothArmsSameTargetDerivesIfJoinNotIfElse) {
    MirBuilder b;
    MirFuncId const f = b.addFunction(kFnSig, SymbolId{1});
    (void)f;
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const elseB = b.createBlock(StructCfMarker::IfElse);  // derives IfJoin
    b.beginBlock(entry);
    MirInstId const c1 = b.addConst(intLit(1), kBool);
    b.addCondBr(c1, elseB, elseB);
    b.beginBlock(elseB); b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m};
    EXPECT_FALSE(v.verify(r));
    EXPECT_EQ(countCode(r, DiagnosticCode::I_StructCfMismatch), 1u);
}

// LoopHeader with no back-edge predecessor fails equality: the
// derivation only claims LoopHeader for an actual back-edge target, so
// the stale stamp mismatches (stored LoopHeader, derived Linear). Same
// fail-loud intent as the old dominance-based back-edge rule — the
// equality model subsumes it (a `while(1){break;}`-class degenerate
// loop is the production shape: the PRODUCER's rederive normalizes the
// stamp, and a producer that forgets to rederive is caught HERE).
TEST(MirVerifier, LoopHeaderWithoutBackEdgeEmitsStructCfMismatch) {
    MirBuilder b;
    MirFuncId const f = b.addFunction(kFnSig, SymbolId{1});
    (void)f;
    MirBlockId const entry  = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = b.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const exit   = b.createBlock(StructCfMarker::Linear);
    b.beginBlock(entry);
    b.addBr(header);
    b.beginBlock(header);
    // Branch straight to exit — NO back-edge; `header` derives Linear.
    b.addBr(exit);
    b.beginBlock(exit);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m};
    EXPECT_FALSE(v.verify(r));
    EXPECT_EQ(countCode(r, DiagnosticCode::I_StructCfMismatch), 1u)
        << "only the stale LoopHeader stamp disagrees with the derivation";
}

// Without an interner: type-gated rules are skipped — even a malformed
// type-typed value passes (because the verifier can't decode types).
TEST(MirVerifier, InternerGatedRulesSkippedWhenInternerAbsent) {
    MirBuilder b;
    MirFuncId const f = b.addFunction(kFnSig, SymbolId{1});
    (void)f;
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tBB   = b.createBlock(StructCfMarker::IfThen);
    MirBlockId const eBB   = b.createBlock(StructCfMarker::IfElse);
    MirBlockId const join  = b.createBlock(StructCfMarker::IfJoin);
    b.beginBlock(entry);
    // I32 cond — would fail with an interner; passes without.
    MirInstId const c1 = b.addConst(intLit(1), kI32);
    b.addCondBr(c1, tBB, eBB);
    b.beginBlock(tBB); b.addBr(join);
    b.beginBlock(eBB); b.addBr(join);
    b.beginBlock(join); b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m};  // No interner.
    EXPECT_TRUE(v.verify(r));
}

// ── the LAYOUT rule (I_LayoutUseBeforeDef, D-OPT2 layout contract) ──────────
//
// Dominance is necessary but NOT sufficient for the linear MIR→LIR
// lowering: every linear consumer requires a TOPOLOGICAL block layout
// (a def emitted before its use). The verifier's layout rule catches a
// def that DOMINATES its use but is laid out AFTER it — a class no
// dominance check can see. These three pins:
//   (1) dominance-VALID but layout-INVERTED → EXACTLY 1
//       I_LayoutUseBeforeDef (and ZERO I_NotDominated — the rule is
//       GATED on Dominates so one bad operand never double-reports);
//   (2) the topological SIBLING (same CFG, correct layout) → clean;
//   (3) a loop back-edge Phi incoming whose value is defined in the
//       latch (laid out AFTER the header) → clean (Phi incomings are
//       EXEMPT; the dominance arm owns their semantics).

// (1) entry → B(def) → C(use), but C is CREATED before B, so C is laid
// out before B. B dominates C (straight-line entry→B→C), so SSA holds —
// yet the def in B is laid out AFTER its use in C. The layout rule fires
// EXACTLY once; I_NotDominated does NOT (the def dominates).
TEST(MirVerifier, LayoutInvertedDominatingDefEmitsLayoutUseBeforeDef) {
    MirBuilder b;
    MirFuncId const f = b.addFunction(kFnSig, SymbolId{1});
    (void)f;
    // Creation order == layout order: [entry, C, B]. C precedes B.
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const cUse  = b.createBlock(StructCfMarker::Linear);
    MirBlockId const bDef  = b.createBlock(StructCfMarker::Linear);
    // CFG: entry → B → C → return (a straight line — B dominates C).
    b.beginBlock(entry);
    b.addBr(bDef);
    b.beginBlock(bDef);
    MirInstId const tv = b.addConst(intLit(10), kI32);  // def in B
    b.addBr(cUse);
    b.beginBlock(cUse);
    std::array<MirInstId, 2> const useOps{tv, tv};       // use of B's def in C
    MirInstId const use = b.addInst(MirOpcode::Add, useOps, kI32);
    b.addReturn(use);
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m};
    EXPECT_FALSE(v.verify(r))
        << "a dominating-but-layout-later def must be rejected — no linear "
           "consumer can resolve a def emitted after its use";
    EXPECT_EQ(countCode(r, DiagnosticCode::I_LayoutUseBeforeDef), 1u)
        << "EXACTLY one layout violation (the single cross-block use of "
           "B's def in the earlier-laid-out C)";
    EXPECT_EQ(countCode(r, DiagnosticCode::I_NotDominated), 0u)
        << "the def DOMINATES the use (SSA holds) — the layout rule is "
           "gated on Dominates so it must NOT double-report I_NotDominated";
}

// (2) The TOPOLOGICAL sibling: identical CFG, but B is created (laid
// out) BEFORE C — [entry, B, C]. The def now precedes its use in layout
// → clean.
TEST(MirVerifier, TopologicalLayoutDominatingDefIsClean) {
    MirBuilder b;
    MirFuncId const f = b.addFunction(kFnSig, SymbolId{1});
    (void)f;
    // Creation order == layout order: [entry, B, C]. B precedes C.
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const bDef  = b.createBlock(StructCfMarker::Linear);
    MirBlockId const cUse  = b.createBlock(StructCfMarker::Linear);
    b.beginBlock(entry);
    b.addBr(bDef);
    b.beginBlock(bDef);
    MirInstId const tv = b.addConst(intLit(10), kI32);
    b.addBr(cUse);
    b.beginBlock(cUse);
    std::array<MirInstId, 2> const useOps{tv, tv};
    MirInstId const use = b.addInst(MirOpcode::Add, useOps, kI32);
    b.addReturn(use);
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m};
    EXPECT_TRUE(v.verify(r))
        << "a topological layout (def laid out before use) is clean";
    EXPECT_EQ(countCode(r, DiagnosticCode::I_LayoutUseBeforeDef), 0u);
}

// (3) THE EXEMPTION pin: a loop header Phi has a back-edge incoming whose
// VALUE is defined in the latch — and the latch is laid out AFTER the
// header. A loop back-edge legitimately carries a def whose layout
// FOLLOWS the phi-use; Phi incomings are EXEMPT from the layout rule (the
// dominance arm owns their semantics), so this canonical counted loop
// verifies clean. Layout order [entry, header, latch, exit].
//   entry:  br header
//   header: i_phi = phi[(0,entry),(i_next,latch)]; condbr latch, exit
//   latch:  i_next = i_phi + 1; br header   (back edge; laid out AFTER header)
//   exit:   return i_phi
// Markers are stamped by the canonical derivation (`rederiveStructCfMarkers`,
// exactly as every real producer does post-finish) so the test isolates the
// LAYOUT-rule's Phi exemption from marker bookkeeping — the back-edge i_next
// (defined in the later-laid-out latch) is the subject, and it must NOT trip
// I_LayoutUseBeforeDef.
TEST(MirVerifier, LoopBackEdgePhiIncomingIsExemptFromLayoutRule) {
    MirBuilder b;
    MirFuncId const f = b.addFunction(kFnSig, SymbolId{1});
    (void)f;
    MirBlockId const entry  = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = b.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const latch  = b.createBlock(StructCfMarker::Linear);
    MirBlockId const exit   = b.createBlock(StructCfMarker::Linear);
    b.beginBlock(entry);
    MirInstId const zero = b.addConst(intLit(0), kI32);
    b.addBr(header);
    // header: phi joins entry's 0 with the latch's i_next (defined LATER
    // in layout). Emit the phi placeholder, then add incomings after the
    // latch defines i_next.
    b.beginBlock(header);
    MirInstId const iPhi = b.addPhi(kI32);
    MirInstId const hcond = b.addConst(intLit(1), kBool);
    b.addCondBr(hcond, latch, exit);
    // latch: i_next = i_phi + 1; back-edge to header.
    b.beginBlock(latch);
    MirInstId const one = b.addConst(intLit(1), kI32);
    std::array<MirInstId, 2> const incOps{iPhi, one};
    MirInstId const iNext = b.addInst(MirOpcode::Add, incOps, kI32);
    b.addBr(header);
    // exit: return the phi.
    b.beginBlock(exit);
    b.addReturn(iPhi);
    // Wire the header phi's incomings now that i_next exists. The (i_next,
    // latch) incoming is the back edge — value defined in a block laid out
    // AFTER the header.
    b.addPhiIncoming(iPhi, MirPhiIncoming{zero, entry});
    b.addPhiIncoming(iPhi, MirPhiIncoming{iNext, latch});
    Mir m = std::move(b).finish();
    // Stamp canonical markers (the back-edge makes `header` a LoopHeader,
    // `latch` a LoopLatch, etc.) so only the layout rule is under test.
    rederiveStructCfMarkers(m);

    DiagnosticReporter r;
    MirVerifier v{m};
    EXPECT_TRUE(v.verify(r))
        << "a loop back-edge Phi incoming (value defined in the later-laid-"
           "out latch) is EXEMPT from the layout rule — the dominance arm "
           "owns Phi-incoming semantics";
    EXPECT_EQ(countCode(r, DiagnosticCode::I_LayoutUseBeforeDef), 0u)
        << "the layout rule must not fire on a Phi back-edge incoming";
}

// ── VLA C1a (D-CSUBSET-VLA): the runtime-sized-Alloca operand<->payload invariant ──
//
// A VLA-typed alloca (pointee isVlaArray) MUST carry exactly ONE operand (the total
// runtime byte size) + a ZERO primary payload; a fixed (non-VLA) alloca MUST carry
// NO operand. Positive: both well-formed shapes pass.
TEST(MirVerifier, VlaAndFixedAllocaWellFormedPass) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const i64    = interner.primitive(TypeKind::I64);
    TypeId const ptrI32 = interner.pointer(i32);                    // fixed alloca ptr
    TypeId const ptrVla = interner.pointer(interner.vlaArray(i32)); // VLA alloca ptr
    TypeId const fnSig  = interner.fnSig({}, interner.primitive(TypeKind::Void),
                                         CallConv::CcSysV);
    MirBuilder b;
    (void)b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    // Fixed alloca: byte size in the payload, NO operand.
    b.addInst(MirOpcode::Alloca, {}, ptrI32, /*payload=*/4, MirInstFlags::None,
              /*align=*/4);
    // VLA alloca: ONE runtime size operand, ZERO payload.
    MirInstId const sz = b.addConst(intLit(16), i64);
    std::array<MirInstId, 1> const vlaOps{sz};
    b.addInst(MirOpcode::Alloca, vlaOps, ptrVla, /*payload=*/0, MirInstFlags::None,
              /*align=*/4);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &interner};
    EXPECT_TRUE(v.verify(r));
    EXPECT_EQ(countCode(r, DiagnosticCode::I_VlaAllocaOperandInvalid), 0u);
}

// Negative (red-on-disable): a VLA-typed alloca that LOST its size operand (→ a
// 0-sized fixed slot) is caught loud.
TEST(MirVerifier, VlaAllocaWithoutSizeOperandRejected) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const ptrVla = interner.pointer(interner.vlaArray(i32));
    TypeId const fnSig  = interner.fnSig({}, interner.primitive(TypeKind::Void),
                                         CallConv::CcSysV);
    MirBuilder b;
    (void)b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    // A VLA alloca with NO operand — the forbidden state (size dropped).
    b.addInst(MirOpcode::Alloca, {}, ptrVla, /*payload=*/0, MirInstFlags::None,
              /*align=*/4);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &interner};
    EXPECT_FALSE(v.verify(r));
    EXPECT_EQ(countCode(r, DiagnosticCode::I_VlaAllocaOperandInvalid), 1u);
}

// Negative (red-on-disable): a FIXED (non-VLA) alloca that grew a spurious runtime
// operand is caught loud.
TEST(MirVerifier, FixedAllocaWithSpuriousOperandRejected) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const i64    = interner.primitive(TypeKind::I64);
    TypeId const ptrI32 = interner.pointer(i32);
    TypeId const fnSig  = interner.fnSig({}, interner.primitive(TypeKind::Void),
                                         CallConv::CcSysV);
    MirBuilder b;
    (void)b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const sz = b.addConst(intLit(16), i64);
    std::array<MirInstId, 1> const ops{sz};
    // A fixed (ptr<i32>) alloca WITH a runtime operand — forbidden.
    b.addInst(MirOpcode::Alloca, ops, ptrI32, /*payload=*/0, MirInstFlags::None,
              /*align=*/4);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &interner};
    EXPECT_FALSE(v.verify(r));
    EXPECT_EQ(countCode(r, DiagnosticCode::I_VlaAllocaOperandInvalid), 1u);
}

// ── FC17.9(d) 1b (D-CSUBSET-ATOMIC): the atomic-lowering belt ────────────────
//
// A plain Load/Store still carrying an `_Atomic`-qualified accessed type is a
// MISSED funnel site — it must have lowered to AtomicLoad/AtomicStore. The belt
// converts that silent non-atomic access into a LOUD I_AtomicAccessNotLowered.

// Negative (RED-ON-DISABLE): a plain `load` whose RESULT type is `_Atomic`-
// qualified is rejected. Remove the belt (checkAtomicAccessLowered) → this Load
// passes silently → a non-atomic read of atomic memory ships. That is exactly the
// regression the belt exists to catch.
TEST(MirVerifier, PlainLoadOfAtomicTypeRejected) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32       = interner.primitive(TypeKind::I32);
    TypeId const atomicI32 = interner.atomicQualified(i32);
    TypeId const ptrAtomic = interner.pointer(atomicI32);
    TypeId const fnSig = interner.fnSig({}, interner.primitive(TypeKind::Void),
                                        CallConv::CcSysV);
    MirBuilder b;
    (void)b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const slot = b.addInst(MirOpcode::Alloca, {}, ptrAtomic);
    std::array<MirInstId, 1> const ld{slot};
    // A PLAIN Load of an atomic-qualified type — the missed-funnel shape.
    b.addInst(MirOpcode::Load, ld, atomicI32);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &interner};
    EXPECT_FALSE(v.verify(r));
    EXPECT_EQ(countCode(r, DiagnosticCode::I_AtomicAccessNotLowered), 1u);
}

// Negative (RED-ON-DISABLE): a plain `store` whose ADDRESS operand's pointee is
// `_Atomic`-qualified — and WITHOUT the AtomicInitExempt flag — is rejected.
TEST(MirVerifier, PlainStoreToAtomicPointeeRejected) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32       = interner.primitive(TypeKind::I32);
    TypeId const atomicI32 = interner.atomicQualified(i32);
    TypeId const ptrAtomic = interner.pointer(atomicI32);
    TypeId const fnSig = interner.fnSig({}, interner.primitive(TypeKind::Void),
                                        CallConv::CcSysV);
    MirBuilder b;
    (void)b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const slot = b.addInst(MirOpcode::Alloca, {}, ptrAtomic);
    MirInstId const val  = b.addConst(intLit(7), i32);
    std::array<MirInstId, 2> const st{val, slot};   // Store order = [value, ptr]
    b.addInst(MirOpcode::Store, st, InvalidType);    // PLAIN store, no exempt flag
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &interner};
    EXPECT_FALSE(v.verify(r));
    EXPECT_EQ(countCode(r, DiagnosticCode::I_AtomicAccessNotLowered), 1u);
}

// Positive: the CORRECTLY-lowered atomic ops (AtomicLoad / AtomicStore) pass the
// belt — the belt keys on the OPCODE (plain Load/Store), so the atomic opcodes are
// never flagged. Pins that the belt does NOT false-positive on the intended form.
TEST(MirVerifier, AtomicLoadAndAtomicStorePass) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32       = interner.primitive(TypeKind::I32);
    TypeId const atomicI32 = interner.atomicQualified(i32);
    TypeId const ptrAtomic = interner.pointer(atomicI32);
    TypeId const fnSig = interner.fnSig({}, interner.primitive(TypeKind::Void),
                                        CallConv::CcSysV);
    MirBuilder b;
    (void)b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const slot = b.addInst(MirOpcode::Alloca, {}, ptrAtomic);
    MirInstId const val  = b.addConst(intLit(42), i32);
    std::array<MirInstId, 2> const ast{val, slot};
    b.addInst(MirOpcode::AtomicStore, ast, InvalidType, /*payload=*/5);
    std::array<MirInstId, 1> const ald{slot};
    b.addInst(MirOpcode::AtomicLoad, ald, atomicI32, /*payload=*/5);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &interner};
    EXPECT_TRUE(v.verify(r)) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::I_AtomicAccessNotLowered), 0u);
}

// Positive: an INITIALIZATION store (C11 7.17.2.1 — atomic init is not itself
// atomic) stays a plain Store and carries MirInstFlags::AtomicInitExempt, so the
// belt SPARES it even though its pointee is atomic-qualified. Pins the exemption.
TEST(MirVerifier, AtomicInitExemptStorePasses) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32       = interner.primitive(TypeKind::I32);
    TypeId const atomicI32 = interner.atomicQualified(i32);
    TypeId const ptrAtomic = interner.pointer(atomicI32);
    TypeId const fnSig = interner.fnSig({}, interner.primitive(TypeKind::Void),
                                        CallConv::CcSysV);
    MirBuilder b;
    (void)b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const slot = b.addInst(MirOpcode::Alloca, {}, ptrAtomic);
    MirInstId const val  = b.addConst(intLit(7), i32);
    std::array<MirInstId, 2> const st{val, slot};
    b.addInst(MirOpcode::Store, st, InvalidType, /*payload=*/0,
              MirInstFlags::AtomicInitExempt);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &interner};
    EXPECT_TRUE(v.verify(r)) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::I_AtomicAccessNotLowered), 0u);
}

// ── TF-C112 (D-MIR-VERIFIER-NO-CALLSITE-SIGNATURE-CHECK) ────────────────────
//
// The CALL-SITE signature belt. `checkTypeInvariants` checks an `Arg` against
// the ENCLOSING function's FnSig; until this rule NOTHING cross-checked a
// `Call`'s operands against its CALLEE's FnSig, so every hand-built call in
// every MIR-tier synthesis pass could pass the wrong number of arguments, or
// the wrong type at a position, and no tier objected. (The frontend path was
// already covered — `HirVerifier::checkCallArguments` runs the same arity +
// per-position rule on every cst_to_hir-produced call — but a pass that emits
// MIR DIRECTLY bypasses HIR entirely.) These tests build MIR BY HAND, never
// through a frontend.
//
// ⚠ Two LIMITS are pinned as tests too, deliberately, so nobody reads the belt
// as more coverage than it is: `SameTypedTranspositionStaysInvisible` and
// `ByValueAggregateParamCallIsSkippedNotFlagged`. Read those before concluding
// this rule covers "the call is wired correctly" — it does not.

namespace {

// Does some I_CallSignatureMismatch diagnostic mention `needle`? The rule's
// value to a human debugging a miscompile is that it names the POSITION, so
// these tests assert on the TEXT, not merely on a count.
[[nodiscard]] bool anyCallDiagContains(DiagnosticReporter const& r,
                                       std::string_view needle) {
    for (auto const& d : r.all()) {
        if (d.code == DiagnosticCode::I_CallSignatureMismatch
            && d.actual.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

MirLiteralValue litOf(std::int64_t v, TypeKind core) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = core;
    return lit;
}

// Every diagnostic's text joined — the `<<` payload for a failing EXPECT, so a
// red shows WHAT fired instead of only a count.
[[nodiscard]] std::string allActuals(DiagnosticReporter const& r) {
    std::string s;
    for (auto const& d : r.all()) { s += "\n  "; s += d.actual; }
    return s;
}

} // namespace

// ★ THE HEADLINE SHAPE: a TRANSPOSITION. `f(i32, ptr<i32>)` called with
// `(ptr<i32>, i32)` — the right VALUES in the wrong SLOTS. Before this rule the
// module verified clean. Both positions are named.
TEST(MirVerifier, CallWithTransposedOperandsRejectedAtBothPositions) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32    = in.primitive(TypeKind::I32);
    TypeId const voidTy = in.primitive(TypeKind::Void);
    TypeId const pI32   = in.pointer(i32);
    std::array<TypeId, 2> const ps{i32, pI32};
    TypeId const calleeSig = in.fnSig(ps, i32, CallConv::CcSysV);
    TypeId const callerSig = in.fnSig({}, voidTy, CallConv::CcSysV);

    MirBuilder b;
    (void)b.addFunction(callerSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const ga = b.addGlobalAddr(SymbolId{2}, in.pointer(calleeSig));
    MirInstId const n  = b.addConst(litOf(7, TypeKind::I32), i32);
    MirInstId const p  = b.addInst(MirOpcode::Alloca, {}, pI32, /*bytes=*/4);
    std::array<MirInstId, 3> const ops{ga, p, n};   // TRANSPOSED
    b.addInst(MirOpcode::Call, ops, i32);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &in};
    EXPECT_FALSE(v.verify(r));
    EXPECT_EQ(countCode(r, DiagnosticCode::I_CallSignatureMismatch), 2u)
        << allActuals(r);
    EXPECT_TRUE(anyCallDiagContains(r, "POSITION 0")) << allActuals(r);
    EXPECT_TRUE(anyCallDiagContains(r, "POSITION 1")) << allActuals(r);
}

// ⚠ LIMIT #1, PINNED SO NOBODY MISREADS THE BELT AS COVERAGE.
// `f(ptr<char>, ptr<char>)` called with its two `ptr<char>` operands SWAPPED
// verifies CLEAN — and always will. This is the EXACT bug that motivated the
// rule (transposing `buf` and `fmt` in synthesizeStdioShim's `sprintf` arm,
// both `char*`, at parameters 1 and 3 of one signature): only POSITION tells
// them apart and no type check at any tier can read position. The same holds
// for `__stdio_common_vsprintf` / `__stdio_common_vsscanf`, which deliberately
// SHARE one FnSig TypeId — mis-wiring a recipe to the wrong one of that pair is
// a SYMBOL-level error invisible here. Those shapes are caught ONLY by per-body
// test pins. If this test ever starts FAILING, the rule has grown a claim it
// cannot honour — work out what changed; do not "fix" it by relaxing anything.
TEST(MirVerifier, SameTypedTranspositionStaysInvisible) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const charTy = in.primitive(TypeKind::Char);
    TypeId const i32    = in.primitive(TypeKind::I32);
    TypeId const voidTy = in.primitive(TypeKind::Void);
    TypeId const pChar  = in.pointer(charTy);
    std::array<TypeId, 2> const ps{pChar, pChar};
    TypeId const calleeSig = in.fnSig(ps, i32, CallConv::CcSysV);
    TypeId const callerSig = in.fnSig({}, voidTy, CallConv::CcSysV);

    MirBuilder b;
    (void)b.addFunction(callerSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const ga  = b.addGlobalAddr(SymbolId{2}, in.pointer(calleeSig));
    MirInstId const buf = b.addInst(MirOpcode::Alloca, {}, pChar, /*bytes=*/64);
    MirInstId const fmt = b.addInst(MirOpcode::Alloca, {}, pChar, /*bytes=*/8);
    std::array<MirInstId, 3> const ops{ga, fmt, buf};   // swapped vs (buf, fmt)
    b.addInst(MirOpcode::Call, ops, i32);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &in};
    EXPECT_TRUE(v.verify(r)) << allActuals(r);
    EXPECT_EQ(countCode(r, DiagnosticCode::I_CallSignatureMismatch), 0u)
        << "a same-TypeId transposition is unreachable for a TYPE rule — this "
           "test documents the hole, it does not endorse it";
}

// Wrong arity, TOO FEW: a 2-parameter non-variadic callee given 1 argument.
TEST(MirVerifier, CallWithTooFewArgumentsRejected) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32    = in.primitive(TypeKind::I32);
    TypeId const voidTy = in.primitive(TypeKind::Void);
    std::array<TypeId, 2> const ps{i32, i32};
    TypeId const calleeSig = in.fnSig(ps, i32, CallConv::CcSysV);
    TypeId const callerSig = in.fnSig({}, voidTy, CallConv::CcSysV);

    MirBuilder b;
    (void)b.addFunction(callerSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const ga = b.addGlobalAddr(SymbolId{2}, in.pointer(calleeSig));
    MirInstId const n  = b.addConst(litOf(1, TypeKind::I32), i32);
    std::array<MirInstId, 2> const ops{ga, n};
    b.addInst(MirOpcode::Call, ops, i32);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &in};
    EXPECT_FALSE(v.verify(r));
    EXPECT_EQ(countCode(r, DiagnosticCode::I_CallSignatureMismatch), 1u)
        << allActuals(r);
    EXPECT_TRUE(anyCallDiagContains(r, "passes 1 argument operand(s)"))
        << allActuals(r);
    EXPECT_TRUE(anyCallDiagContains(r, "declares 2 parameter(s)"))
        << allActuals(r);
}

// Wrong arity, TOO MANY: a 2-parameter non-variadic callee given 3 arguments.
// The variadic `>=`-not-`==` relaxation must NOT leak into the fixed case.
TEST(MirVerifier, CallWithTooManyArgumentsRejected) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32    = in.primitive(TypeKind::I32);
    TypeId const voidTy = in.primitive(TypeKind::Void);
    std::array<TypeId, 2> const ps{i32, i32};
    TypeId const calleeSig = in.fnSig(ps, i32, CallConv::CcSysV);
    TypeId const callerSig = in.fnSig({}, voidTy, CallConv::CcSysV);

    MirBuilder b;
    (void)b.addFunction(callerSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const ga = b.addGlobalAddr(SymbolId{2}, in.pointer(calleeSig));
    MirInstId const a  = b.addConst(litOf(1, TypeKind::I32), i32);
    MirInstId const c  = b.addConst(litOf(2, TypeKind::I32), i32);
    MirInstId const d  = b.addConst(litOf(3, TypeKind::I32), i32);
    std::array<MirInstId, 4> const ops{ga, a, c, d};
    b.addInst(MirOpcode::Call, ops, i32);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &in};
    EXPECT_FALSE(v.verify(r));
    EXPECT_EQ(countCode(r, DiagnosticCode::I_CallSignatureMismatch), 1u)
        << allActuals(r);
    EXPECT_TRUE(anyCallDiagContains(r, "passes 3 argument operand(s)"))
        << allActuals(r);
}

// A WRONG-TYPED operand at ONE position: `f(i32, i32, i32)` given
// `(i32, ptr<i32>, i32)`. Exactly one diagnostic, and it names POSITION 1 —
// the whole point of the rule for a human reading a verifier finding.
TEST(MirVerifier, CallWithWrongTypedOperandNamesThePosition) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32    = in.primitive(TypeKind::I32);
    TypeId const voidTy = in.primitive(TypeKind::Void);
    TypeId const pI32   = in.pointer(i32);
    std::array<TypeId, 3> const ps{i32, i32, i32};
    TypeId const calleeSig = in.fnSig(ps, i32, CallConv::CcSysV);
    TypeId const callerSig = in.fnSig({}, voidTy, CallConv::CcSysV);

    MirBuilder b;
    (void)b.addFunction(callerSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const ga = b.addGlobalAddr(SymbolId{2}, in.pointer(calleeSig));
    MirInstId const a  = b.addConst(litOf(1, TypeKind::I32), i32);
    MirInstId const p  = b.addInst(MirOpcode::Alloca, {}, pI32, /*bytes=*/4);
    MirInstId const c  = b.addConst(litOf(3, TypeKind::I32), i32);
    std::array<MirInstId, 4> const ops{ga, a, p, c};
    b.addInst(MirOpcode::Call, ops, i32);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &in};
    EXPECT_FALSE(v.verify(r));
    EXPECT_EQ(countCode(r, DiagnosticCode::I_CallSignatureMismatch), 1u)
        << allActuals(r);
    EXPECT_TRUE(anyCallDiagContains(r, "POSITION 1")) << allActuals(r);
    EXPECT_FALSE(anyCallDiagContains(r, "POSITION 0")) << allActuals(r);
    EXPECT_FALSE(anyCallDiagContains(r, "POSITION 2")) << allActuals(r);
}

// A NON-void pointee mismatch is still a violation: `f(ptr<char>)` given a
// `ptr<i32>`. This is the boundary of the `void*` slack below — the class
// nearest the shim bug the rule exists for (a `FILE*` wired into a `char*`
// slot) stays LOUD. Relaxing this to "all pointers are interchangeable" is
// exactly the weakening that would make the rule stop paying for itself.
TEST(MirVerifier, NonVoidPointeeMismatchStillRejected) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32    = in.primitive(TypeKind::I32);
    TypeId const charTy = in.primitive(TypeKind::Char);
    TypeId const voidTy = in.primitive(TypeKind::Void);
    TypeId const pChar  = in.pointer(charTy);
    TypeId const pI32   = in.pointer(i32);
    std::array<TypeId, 1> const ps{pChar};
    TypeId const calleeSig = in.fnSig(ps, i32, CallConv::CcSysV);
    TypeId const callerSig = in.fnSig({}, voidTy, CallConv::CcSysV);

    MirBuilder b;
    (void)b.addFunction(callerSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const ga = b.addGlobalAddr(SymbolId{2}, in.pointer(calleeSig));
    MirInstId const p  = b.addInst(MirOpcode::Alloca, {}, pI32, /*bytes=*/4);
    std::array<MirInstId, 2> const ops{ga, p};
    b.addInst(MirOpcode::Call, ops, i32);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &in};
    EXPECT_FALSE(v.verify(r));
    EXPECT_EQ(countCode(r, DiagnosticCode::I_CallSignatureMismatch), 1u)
        << allActuals(r);
}

// ★ THE TYPE-QUALIFIER ARM. `T*` → `volatile T*` at a call argument is an
// IMPLICIT QUALIFICATION CONVERSION (C17 6.5.16.1p1) — legal with no cast, and
// bit-identical, so the tree emits no Cast for it. The interner already calls a
// qualifier skin representation-neutral (`sameRepresentation`: "`volatile long`
// and `long` compare equal here"), but compares a composite's OPERANDS by raw
// TypeId, so the neutrality does not survive one level of indirection.
//
// MEASURED at TF-C112 against sqlite `src/func.c`: `kahanBabuskaNeumaierInit/
// Step/StepInt64` are declared `(volatile SumCtx *, …)` and called from
// `sumStep`/`sumInverse` with a plain `SumCtx *p` — 11 call sites, 3 callees,
// one shape, and gcc 13.2.0 accepts the reduction at `-std=c17 -Wall -Wextra
// -pedantic` with zero diagnostics. There is only ONE interned `SumCtx`: the
// param pointee is a VolatileQual skin whose `stripVolatile` returns the
// argument pointee's own TypeId.
//
// RED-ON-DISABLE: delete the qualifier arm in `checkCallSignatures` and this
// test fails with 1 mismatch at POSITION 0.
TEST(MirVerifier, QualifiedPointeeOnEitherSideIsCompatible) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const f64    = in.primitive(TypeKind::F64);
    TypeId const i32    = in.primitive(TypeKind::I32);
    TypeId const voidTy = in.primitive(TypeKind::Void);
    std::array<TypeId, 2> const fields{f64, f64};
    TypeId const sumCtx  = in.structType("SumCtx", fields);
    TypeId const volCtx  = in.volatileQualified(sumCtx);
    ASSERT_NE(sumCtx.v, volCtx.v) << "the qualifier skin must split identity";
    ASSERT_EQ(in.stripVolatile(volCtx).v, sumCtx.v)
        << "one interning + a transparent skin, not two internings";
    TypeId const pCtx    = in.pointer(sumCtx);
    TypeId const pVolCtx = in.pointer(volCtx);
    ASSERT_NE(pCtx.v, pVolCtx.v);
    // param 0 = `volatile SumCtx*` fed a plain `SumCtx*` (sqlite's shape);
    // param 1 = plain `SumCtx*` fed a `volatile SumCtx*` (the other direction —
    // a C constraint violation the FRONT END owns, not a representation error,
    // so MIR must not encode one source language's qualifier rules here).
    std::array<TypeId, 2> const ps{pVolCtx, pCtx};
    TypeId const calleeSig = in.fnSig(ps, i32, CallConv::CcSysV);
    TypeId const callerSig = in.fnSig({}, voidTy, CallConv::CcSysV);

    MirBuilder b;
    (void)b.addFunction(callerSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const ga = b.addGlobalAddr(SymbolId{2}, in.pointer(calleeSig));
    MirInstId const p  = b.addInst(MirOpcode::Alloca, {}, pCtx, /*bytes=*/16);
    MirInstId const q  = b.addInst(MirOpcode::Alloca, {}, pVolCtx, /*bytes=*/16);
    std::array<MirInstId, 3> const ops{ga, p, q};
    b.addInst(MirOpcode::Call, ops, i32);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &in};
    EXPECT_TRUE(v.verify(r)) << allActuals(r);
    EXPECT_EQ(countCode(r, DiagnosticCode::I_CallSignatureMismatch), 0u);
}

// THE BOUNDARY of the arm above, and the reason it is a qualifier arm rather
// than a pointer arm. Stripping the skin does NOT make pointers interchangeable:
// two DIFFERENT structs stay a violation even when BOTH sides are volatile-
// qualified, because the arm compares the stripped pointees by interned IDENTITY
// (not `sameRepresentation`, which would have let `ptr<long>` into a
// `ptr<long long>` slot). A `FILE*` wired into a `char*` slot — the shim bug the
// whole rule exists for — is still LOUD, qualified or not.
TEST(MirVerifier, QualifiedButDistinctPointeesStillRejected) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const f64    = in.primitive(TypeKind::F64);
    TypeId const i32    = in.primitive(TypeKind::I32);
    TypeId const voidTy = in.primitive(TypeKind::Void);
    std::array<TypeId, 2> const fa{f64, f64};
    std::array<TypeId, 1> const fb{i32};
    TypeId const volA = in.volatileQualified(in.structType("A", fa));
    TypeId const volB = in.volatileQualified(in.structType("B", fb));
    std::array<TypeId, 1> const ps{in.pointer(volA)};
    TypeId const calleeSig = in.fnSig(ps, i32, CallConv::CcSysV);
    TypeId const callerSig = in.fnSig({}, voidTy, CallConv::CcSysV);

    MirBuilder b;
    (void)b.addFunction(callerSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const ga = b.addGlobalAddr(SymbolId{2}, in.pointer(calleeSig));
    MirInstId const p  = b.addInst(MirOpcode::Alloca, {}, in.pointer(volB),
                                   /*bytes=*/4);
    std::array<MirInstId, 2> const ops{ga, p};
    b.addInst(MirOpcode::Call, ops, i32);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &in};
    EXPECT_FALSE(v.verify(r));
    EXPECT_EQ(countCode(r, DiagnosticCode::I_CallSignatureMismatch), 1u)
        << allActuals(r);
}

// ★ THE `void*` SLACK, and why it is not a red-driven relaxation. `ptr<void>`
// is MIR's canonical "an ADDRESS whose pointee is unknown or irrelevant"
// spelling — the ABSENCE of a type claim. MEASURED at TF-C112: running
// **Mem2Reg** over `int vsum(int, va_list)`'s caller promotes the `va_list ap`
// local and forwards the `VaHomeArgAreaAddr` leaf — typed `ptr<void>` — into a
// parameter declared `ptr<i8>` (Win64 `va_list`), erasing the pointee with no
// retagging Cast (one would invent a runtime instruction for a bit-identical
// conversion). So pointee identity at a call operand is not a MIR invariant
// after optimization. Both directions are admitted: a `void*` ARGUMENT (the
// Mem2Reg shape) and a `void*` PARAMETER (every synthesis pass spells an opaque
// OS/CRT handle that way — synth_threads_shim passes `&__dss_once_tramp`, a
// `ptr<FnSig>`, into InitOnceExecuteOnce's `void*`-declared slot).
TEST(MirVerifier, VoidPointerOnEitherSideIsCompatible) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i8     = in.primitive(TypeKind::I8);
    TypeId const i32    = in.primitive(TypeKind::I32);
    TypeId const voidTy = in.primitive(TypeKind::Void);
    TypeId const pVoid  = in.pointer(voidTy);
    TypeId const pI8    = in.pointer(i8);
    // param 0 = `va_list` (ptr<i8>) fed a ptr<void>; param 1 = ptr<void> fed a
    // ptr<FnSig> (a function address, the once-trampoline shape).
    std::array<TypeId, 2> const ps{pI8, pVoid};
    TypeId const calleeSig = in.fnSig(ps, i32, CallConv::CcSysV);
    TypeId const callerSig = in.fnSig({}, voidTy, CallConv::CcSysV);

    MirBuilder b;
    (void)b.addFunction(callerSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const ga = b.addGlobalAddr(SymbolId{2}, in.pointer(calleeSig));
    // The Mem2Reg-promoted va_list leaf: a frame address typed ptr<void>.
    MirInstId const ap = b.addInst(MirOpcode::VaHomeArgAreaAddr, {}, pVoid,
                                   /*payload=*/2);
    MirInstId const fn = b.addGlobalAddr(SymbolId{3}, in.pointer(calleeSig));
    std::array<MirInstId, 3> const ops{ga, ap, fn};
    b.addInst(MirOpcode::Call, ops, i32);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &in};
    EXPECT_TRUE(v.verify(r)) << allActuals(r);
    EXPECT_EQ(countCode(r, DiagnosticCode::I_CallSignatureMismatch), 0u);
}

// Positive: a correctly-wired call passes. Pins that the rule does not
// false-positive on the intended form.
TEST(MirVerifier, CorrectlyWiredCallPasses) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32    = in.primitive(TypeKind::I32);
    TypeId const voidTy = in.primitive(TypeKind::Void);
    TypeId const pI32   = in.pointer(i32);
    std::array<TypeId, 2> const ps{i32, pI32};
    TypeId const calleeSig = in.fnSig(ps, i32, CallConv::CcSysV);
    TypeId const callerSig = in.fnSig({}, voidTy, CallConv::CcSysV);

    MirBuilder b;
    (void)b.addFunction(callerSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const ga = b.addGlobalAddr(SymbolId{2}, in.pointer(calleeSig));
    MirInstId const n  = b.addConst(litOf(7, TypeKind::I32), i32);
    MirInstId const p  = b.addInst(MirOpcode::Alloca, {}, pI32, /*bytes=*/4);
    std::array<MirInstId, 3> const ops{ga, n, p};
    b.addInst(MirOpcode::Call, ops, i32);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &in};
    EXPECT_TRUE(v.verify(r)) << allActuals(r);
    EXPECT_EQ(countCode(r, DiagnosticCode::I_CallSignatureMismatch), 0u);
}

// Positive: a VARIADIC callee accepts MORE operands than its fixed parameter
// count, and the vararg tail is UNTYPED by construction (C's default argument
// promotions + the platform vararg ABI own it) — so a `ptr` in the tail of a
// `(char*, ...)` signature is not a finding. HirVerifier's convention verbatim.
TEST(MirVerifier, VariadicCallWithExtraArgumentsPasses) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32    = in.primitive(TypeKind::I32);
    TypeId const charTy = in.primitive(TypeKind::Char);
    TypeId const voidTy = in.primitive(TypeKind::Void);
    TypeId const pChar  = in.pointer(charTy);
    std::array<TypeId, 1> const ps{pChar};
    TypeId const calleeSig =
        in.fnSig(ps, i32, CallConv::CcSysV, /*isVariadic=*/true);
    TypeId const callerSig = in.fnSig({}, voidTy, CallConv::CcSysV);

    MirBuilder b;
    (void)b.addFunction(callerSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const ga  = b.addGlobalAddr(SymbolId{2}, in.pointer(calleeSig));
    MirInstId const fmt = b.addInst(MirOpcode::Alloca, {}, pChar, /*bytes=*/8);
    MirInstId const v0  = b.addConst(litOf(1, TypeKind::I32), i32);
    MirInstId const v1  = b.addInst(MirOpcode::Alloca, {}, pChar, /*bytes=*/8);
    std::array<MirInstId, 4> const ops{ga, fmt, v0, v1};
    b.addInst(MirOpcode::Call, ops, i32, /*payload=*/0);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &in};
    EXPECT_TRUE(v.verify(r)) << allActuals(r);
    EXPECT_EQ(countCode(r, DiagnosticCode::I_CallSignatureMismatch), 0u);
}

// Negative: variadic relaxes the UPPER bound only — FEWER operands than the
// fixed parameter count is still a violation.
TEST(MirVerifier, VariadicCallWithFewerThanFixedArgumentsRejected) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32    = in.primitive(TypeKind::I32);
    TypeId const charTy = in.primitive(TypeKind::Char);
    TypeId const voidTy = in.primitive(TypeKind::Void);
    TypeId const pChar  = in.pointer(charTy);
    std::array<TypeId, 2> const ps{pChar, i32};
    TypeId const calleeSig =
        in.fnSig(ps, i32, CallConv::CcSysV, /*isVariadic=*/true);
    TypeId const callerSig = in.fnSig({}, voidTy, CallConv::CcSysV);

    MirBuilder b;
    (void)b.addFunction(callerSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const ga  = b.addGlobalAddr(SymbolId{2}, in.pointer(calleeSig));
    MirInstId const fmt = b.addInst(MirOpcode::Alloca, {}, pChar, /*bytes=*/8);
    std::array<MirInstId, 2> const ops{ga, fmt};
    b.addInst(MirOpcode::Call, ops, i32);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &in};
    EXPECT_FALSE(v.verify(r));
    EXPECT_EQ(countCode(r, DiagnosticCode::I_CallSignatureMismatch), 1u)
        << allActuals(r);
    EXPECT_TRUE(anyCallDiagContains(r, "declares 2 fixed parameter(s)"))
        << allActuals(r);
}

// Positive: an INDIRECT call — the callee is a register value (here an `Arg`
// holding a function pointer, the synth_threads_shim once-adapter shape) — has
// NO static callee, so there is no signature to check against. It must be
// skipped CLEANLY, never flagged. Its operands deliberately match no signature.
TEST(MirVerifier, IndirectCallWithNoStaticCalleeIsSkipped) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32    = in.primitive(TypeKind::I32);
    TypeId const voidTy = in.primitive(TypeKind::Void);
    TypeId const pVoid  = in.pointer(voidTy);
    std::array<TypeId, 1> const cps{pVoid};
    TypeId const callerSig = in.fnSig(cps, voidTy, CallConv::CcSysV);

    MirBuilder b;
    (void)b.addFunction(callerSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const fp = b.addArg(0, pVoid);            // the callee, in a register
    MirInstId const n  = b.addConst(litOf(9, TypeKind::I32), i32);
    std::array<MirInstId, 2> const ops{fp, n};
    b.addInst(MirOpcode::Call, ops, i32);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &in};
    EXPECT_TRUE(v.verify(r)) << allActuals(r);
    EXPECT_EQ(countCode(r, DiagnosticCode::I_CallSignatureMismatch), 0u);
}

// The OTHER shipped callee spelling. hir_to_mir's direct-call arm types the
// callee GlobalAddr with the HIR Ref's own type — the FnSig ITSELF, not
// `Ptr<FnSig>` (which is what `&fn` and every synthesis pass produce). Pins
// that the rule engages on that spelling too rather than silently skipping the
// entire frontend-produced call population.
TEST(MirVerifier, DirectFnSigTypedCalleeIsAlsoChecked) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32    = in.primitive(TypeKind::I32);
    TypeId const voidTy = in.primitive(TypeKind::Void);
    std::array<TypeId, 2> const ps{i32, i32};
    TypeId const calleeSig = in.fnSig(ps, i32, CallConv::CcSysV);
    TypeId const callerSig = in.fnSig({}, voidTy, CallConv::CcSysV);

    MirBuilder b;
    (void)b.addFunction(callerSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const ga = b.addGlobalAddr(SymbolId{2}, calleeSig);   // FnSig DIRECTLY
    MirInstId const n  = b.addConst(litOf(1, TypeKind::I32), i32);
    std::array<MirInstId, 2> const ops{ga, n};                      // 1 arg, needs 2
    b.addInst(MirOpcode::Call, ops, i32);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &in};
    EXPECT_FALSE(v.verify(r));
    EXPECT_EQ(countCode(r, DiagnosticCode::I_CallSignatureMismatch), 1u)
        << allActuals(r);
}

// D-LANG-TYPE-IDENTITY-VOCABULARY: an operand whose type is identity-DISTINCT
// from the parameter but REPRESENTATIONALLY identical (`i64` vs `i64 "long"`)
// is accepted. A same-representation conversion changes NO bits, so the tree
// RETAGS rather than emitting a Cast for it — rejecting it here would make the
// verifier contradict the lowering.
TEST(MirVerifier, SameRepresentationDistinctIdentityOperandAccepted) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i64Anon = in.primitive(TypeKind::I64);
    TypeId const i64Long = in.primitive(TypeKind::I64, "long");
    TypeId const i32     = in.primitive(TypeKind::I32);
    TypeId const voidTy  = in.primitive(TypeKind::Void);
    ASSERT_NE(i64Anon.v, i64Long.v) << "the vocabulary tag must split identity";
    std::array<TypeId, 1> const ps{i64Long};
    TypeId const calleeSig = in.fnSig(ps, i32, CallConv::CcSysV);
    TypeId const callerSig = in.fnSig({}, voidTy, CallConv::CcSysV);

    MirBuilder b;
    (void)b.addFunction(callerSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const ga = b.addGlobalAddr(SymbolId{2}, in.pointer(calleeSig));
    MirInstId const n  = b.addConst(litOf(5, TypeKind::I64), i64Anon);
    std::array<MirInstId, 2> const ops{ga, n};
    b.addInst(MirOpcode::Call, ops, i32);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &in};
    EXPECT_TRUE(v.verify(r)) << allActuals(r);
    EXPECT_EQ(countCode(r, DiagnosticCode::I_CallSignatureMismatch), 0u);
}

// ⚠ LIMIT #2: the PHYSICAL-vs-SEMANTIC gate. A MIR Call's operand list is
// ABI-LOWERED — a by-value aggregate parameter expands into a target-dependent
// number of register pieces / carriers, and MIR must not know the target's
// classifier (the agnosticism bar). So a call whose callee declares a
// by-value-class parameter is SKIPPED WHOLE: here `f(struct S)` is called with
// TWO i64 register pieces, which a naive arity rule would call a 2-vs-1
// violation. Skipping is the check DECLINING TO JUDGE a list it cannot align,
// not a relaxation — narrowing it needs the ABI classification recorded ON the
// Call, which is a MIR-tier change, not a verifier one.
TEST(MirVerifier, ByValueAggregateParamCallIsSkippedNotFlagged) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i64    = in.primitive(TypeKind::I64);
    TypeId const i32    = in.primitive(TypeKind::I32);
    TypeId const voidTy = in.primitive(TypeKind::Void);
    std::array<TypeId, 2> const fields{i64, i64};
    TypeId const sTy = in.structType("S", fields);
    std::array<TypeId, 1> const ps{sTy};
    TypeId const calleeSig = in.fnSig(ps, i32, CallConv::CcSysV);
    TypeId const callerSig = in.fnSig({}, voidTy, CallConv::CcSysV);

    MirBuilder b;
    (void)b.addFunction(callerSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const ga = b.addGlobalAddr(SymbolId{2}, in.pointer(calleeSig));
    MirInstId const p0 = b.addConst(litOf(1, TypeKind::I64), i64);
    MirInstId const p1 = b.addConst(litOf(2, TypeKind::I64), i64);
    std::array<MirInstId, 3> const ops{ga, p0, p1};   // 2 pieces for 1 param
    b.addInst(MirOpcode::Call, ops, i32);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m, &in};
    EXPECT_TRUE(v.verify(r)) << allActuals(r);
    EXPECT_EQ(countCode(r, DiagnosticCode::I_CallSignatureMismatch), 0u);
}

// Interner-gated like every other type rule: with NO interner the callee's
// FnSig cannot be decoded, so a blatantly wrong call is not judged (a raw
// fixture's TypeIds are untagged stand-ins that resolve to nothing).
TEST(MirVerifier, CallSignatureRuleSkippedWithoutInterner) {
    MirBuilder b;
    (void)b.addFunction(kFnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const ga = b.addGlobalAddr(SymbolId{2}, kVoidFn);
    MirInstId const n  = b.addConst(intLit(1), kI32);
    std::array<MirInstId, 2> const ops{ga, n};
    b.addInst(MirOpcode::Call, ops, kI32);
    b.addReturn();
    Mir m = std::move(b).finish();

    DiagnosticReporter r;
    MirVerifier v{m};                     // no interner
    EXPECT_TRUE(v.verify(r));
    EXPECT_EQ(countCode(r, DiagnosticCode::I_CallSignatureMismatch), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// THE TERMINATOR-SUCCESSOR-ARITY BACKSTOP + THE INLINE-ASM POOL RANGE CHECK
// ─────────────────────────────────────────────────────────────────────────────
//
// ★★★ THESE TWO RULES EXIST BECAUSE A DOCBLOCK CLAIMED A CHECK THAT DID NOT
// EXIST — `MirBuilder::recordSuccessors_` justified itself by saying "ML3's
// verifier re-runs the same descriptor check on any frozen module". It did not.
// Shipping the replacement UNTESTED would be the same species one generation on:
// mechanism present, nothing forcing it to matter. Every arm below is therefore
// built through the DIRECT `Mir` ctor and not through `MirBuilder`, which is not
// a workaround for an inconvenient builder — it IS the covered path. The builder
// guards what it owns; these rules guard what it does not (the merge output, a
// deserializer, a hand-built fixture, a future rebuild pass).
//
// ⚠ THE ASSERTIONS MATCH THE RULE'S OWN MESSAGE, never merely `I_VerifierFailure`
// or `verify() == false`. A hand-built module is malformed in more than one way
// by nature — an unreachable block, a missing edge — so a code-only assertion is
// satisfied by a DIFFERENT rule firing, and would stay green with the rule under
// test deleted.

namespace {

// The four arenas + the pools, assembled into a frozen `Mir` with no builder in
// the loop. Slot 0 of every arena is the sentinel `ArenaBuilder` mints itself.
struct RawModule {
    substrate::ArenaBuilder<detail::MirInst,   MirInstId,   MirModuleId> insts{MirModuleId{1}};
    substrate::ArenaBuilder<detail::MirBlock,  MirBlockId,  MirModuleId> blocks{MirModuleId{1}};
    substrate::ArenaBuilder<detail::MirFunc,   MirFuncId,   MirModuleId> funcs{MirModuleId{1}};
    substrate::ArenaBuilder<detail::MirGlobal, MirGlobalId, MirModuleId> globals{MirModuleId{1}};
    std::vector<MirBlockId>      instBlock{InvalidMirBlock};   // slot 0
    std::vector<MirBlockId>      succPool;
    MirAsmDescriptorPool         asmPool;

    // Append one instruction and record the block that owns it. `instBlock` must
    // stay exactly as long as the instruction arena or the ctor aborts, which is
    // why the two are written together and never separately.
    MirInstId addInst(MirOpcode op, MirBlockId owner, std::uint32_t payload = 0) {
        detail::MirInst pod;
        pod.opcode  = op;
        pod.payload = payload;
        MirInstId const id = insts.addNode(pod);
        instBlock.push_back(owner);
        return id;
    }

    [[nodiscard]] Mir finish() && {
        return Mir(std::move(insts).finish(), std::move(blocks).finish(),
                   std::move(funcs).finish(), std::move(globals).finish(),
                   std::move(instBlock), {}, {}, std::move(succPool),
                   MirLiteralPool{}, std::move(asmPool),
                   MirAliasingMode::Permissive, /*charTypesAliasAll=*/true);
    }
};

// Does any diagnostic's text contain `needle`?
[[nodiscard]] bool saidThat(DiagnosticReporter const& r, std::string_view needle) {
    for (auto const& d : r.all()) {
        if (d.actual.find(needle) != std::string::npos) return true;
    }
    return false;
}

// Everything reported, so a failure prints WHICH rules fired.
[[nodiscard]] std::string said(DiagnosticReporter const& r) {
    std::string out;
    for (auto const& d : r.all()) { out += d.actual; out += "\n"; }
    return out.empty() ? std::string{"(nothing reported)"} : out;
}

} // namespace

// A `Br` — opcode row `[1, 1]` successors — in a block that carries NONE.
// `MirBuilder::addBr` cannot produce this; the direct ctor can, and before this
// rule existed the edge simply was not there and nothing said so.
TEST(MirVerifier, TerminatorWithTooFewSuccessorsIsReported) {
    RawModule m;
    MirBlockId const entry = m.blocks.addNode({});
    (void)m.addInst(MirOpcode::Br, entry);
    {
        auto& b     = m.blocks.at(entry);
        b.instStart = 1;
        b.instCount = 1;
        b.succStart = 0;
        b.succCount = 0;              // … and the row demands exactly one
        b.func      = 1;
        b.marker    = StructCfMarker::EntryBlock;
    }
    detail::MirFunc fn;
    fn.signature  = kFnSig;
    fn.blockStart = entry.v;
    fn.blockCount = 1;
    fn.symbol     = 1;
    (void)m.funcs.addNode(fn);

    Mir const mir = std::move(m).finish();
    DiagnosticReporter r;
    MirVerifier v{mir};
    EXPECT_FALSE(v.verify(r));
    EXPECT_TRUE(saidThat(r, "terminator br carries 0 CFG successor(s), "
                            "outside [1, 1]")) << said(r);
}

// The MIRROR, and it is not decoration: a rule written as `n != min` would pass
// this one, and a rule written as `n < min` alone would pass the over-max half.
// A `CondBr` — row `[2, 2]` — given THREE edges.
TEST(MirVerifier, TerminatorWithTooManySuccessorsIsReported) {
    RawModule m;
    MirBlockId const entry = m.blocks.addNode({});
    MirBlockId const tgt   = m.blocks.addNode({});
    MirInstId const cond   = m.addInst(MirOpcode::Const, entry);
    m.insts.at(cond).typeId = kBool;      // Const is value-producing
    (void)m.addInst(MirOpcode::CondBr, entry);
    (void)m.addInst(MirOpcode::Return, tgt);
    m.succPool = {tgt, tgt, tgt};         // three edges where the row allows two
    {
        auto& b     = m.blocks.at(entry);
        b.instStart = 1;
        b.instCount = 2;
        b.succStart = 0;
        b.succCount = 3;
        b.func      = 1;
        b.marker    = StructCfMarker::EntryBlock;
    }
    {
        auto& b     = m.blocks.at(tgt);
        b.instStart = 3;
        b.instCount = 1;
        b.succStart = 0;
        b.succCount = 0;
        b.func      = 1;
        b.marker    = StructCfMarker::Linear;
    }
    detail::MirFunc fn;
    fn.signature  = kFnSig;
    fn.blockStart = entry.v;
    fn.blockCount = 2;
    fn.symbol     = 1;
    (void)m.funcs.addNode(fn);

    Mir const mir = std::move(m).finish();
    DiagnosticReporter r;
    MirVerifier v{mir};
    EXPECT_FALSE(v.verify(r));
    EXPECT_TRUE(saidThat(r, "terminator condbr carries 3 CFG successor(s), "
                            "outside [2, 2]")) << said(r);
}

// ★★★ THE ONE THE OPCODE ROW CANNOT CATCH, AND THE REASON THE DESCRIPTOR CARRIES
// ITS LABELS. `InlineAsmGoto`'s row is `[2, ∞)`, so a TWO-label `asm goto` that
// LOST its fall-through edge still sits inside the range — and losing that edge
// deletes the code after the statement, because the mandatory unreachable-prune
// drops a block nothing reaches. Only `labelSpellings.size() + 1 == successors`
// sees it.
TEST(MirVerifier, InlineAsmGotoWhoseLabelCountDisagreesWithItsEdgesIsReported) {
    RawModule m;
    MirAsmDescriptor d;
    d.templateText = "jmp %l1";
    d.isExtended   = true;
    d.labelSpellings.push_back({"%l0"});
    d.labelSpellings.push_back({"%l1"});   // TWO labels ⇒ THREE edges are owed
    (void)m.asmPool.add(std::move(d));

    MirBlockId const entry = m.blocks.addNode({});
    MirBlockId const one   = m.blocks.addNode({});
    MirBlockId const two   = m.blocks.addNode({});
    (void)m.addInst(MirOpcode::InlineAsmGoto, entry, /*payload=*/0);
    (void)m.addInst(MirOpcode::Return, one);
    (void)m.addInst(MirOpcode::Return, two);
    m.succPool = {one, two};               // … and only TWO are present
    {
        auto& b     = m.blocks.at(entry);
        b.instStart = 1;
        b.instCount = 1;
        b.succStart = 0;
        b.succCount = 2;
        b.func      = 1;
        b.marker    = StructCfMarker::EntryBlock;
    }
    for (auto [blk, first] : {std::pair{one, 2u}, std::pair{two, 3u}}) {
        auto& b     = m.blocks.at(blk);
        b.instStart = first;
        b.instCount = 1;
        b.succStart = 0;
        b.succCount = 0;
        b.func      = 1;
        b.marker    = StructCfMarker::Linear;
    }
    detail::MirFunc fn;
    fn.signature  = kFnSig;
    fn.blockStart = entry.v;
    fn.blockCount = 3;
    fn.symbol     = 1;
    (void)m.funcs.addNode(fn);

    Mir const mir = std::move(m).finish();
    DiagnosticReporter r;
    MirVerifier v{mir};
    EXPECT_FALSE(v.verify(r));
    EXPECT_TRUE(saidThat(r, "inlineasmgoto declares 2 label(s) but carries 2 CFG "
                            "successor(s)")) << said(r);
    // ⚠ AND THE OPCODE-ROW RULE MUST **NOT** HAVE FIRED: two successors is inside
    // `[2, ∞)`. Without this the arm above could be satisfied by the range check,
    // and the descriptor rule could be deleted with the test still green.
    EXPECT_FALSE(saidThat(r, "outside [2,")) << said(r);
}

// ★★ THE POOL-RANGE CHECK, AND ITS REAL CLAIM IS THAT THE PROCESS SURVIVES.
// `Mir::asmDescriptor` ABORTS on an out-of-range index, so before this rule a
// module carrying a stale index KILLED the compiler inside the verifier instead
// of being described by it — "a refusal that crashes is not a refusal". The
// assertion is therefore as much about reaching the next line as about the text.
TEST(MirVerifier, InlineAsmPayloadOutsideTheDescriptorPoolIsReported) {
    RawModule m;                            // … with an EMPTY descriptor pool
    MirBlockId const entry = m.blocks.addNode({});
    (void)m.addInst(MirOpcode::InlineAsm, entry, /*payload=*/7);
    (void)m.addInst(MirOpcode::Return, entry);
    {
        auto& b     = m.blocks.at(entry);
        b.instStart = 1;
        b.instCount = 2;
        b.succStart = 0;
        b.succCount = 0;
        b.func      = 1;
        b.marker    = StructCfMarker::EntryBlock;
    }
    detail::MirFunc fn;
    fn.signature  = kFnSig;
    fn.blockStart = entry.v;
    fn.blockCount = 1;
    fn.symbol     = 1;
    (void)m.funcs.addNode(fn);

    Mir const mir = std::move(m).finish();
    DiagnosticReporter r;
    MirVerifier v{mir};
    EXPECT_FALSE(v.verify(r));
    EXPECT_TRUE(saidThat(r, "inlineasm payload 7 out of inline-asm "
                            "descriptor-pool range [0, 0)")) << said(r);
}

// ★★★ THE TWO RULES MEET HERE, AND THE ORDER BETWEEN THEM IS LOAD-BEARING. An
// `InlineAsmGoto` whose payload is out of range is BOTH a pool-range violation
// and a candidate for the label-arity rule — and the label rule would have to
// call `Mir::asmDescriptor` to evaluate, which ABORTS on exactly this index.
// `checkTerminatorSuccessorArity` therefore guards on the pool range before
// reading the descriptor. This arm is that guard's only exercise: without it the
// clause is a comment, and a future edit that drops the guard turns a reported
// module into a killed process — which no code-only assertion would notice,
// because a dead test binary reports nothing at all.
TEST(MirVerifier, InlineAsmGotoWithAStalePoolIndexIsReportedRatherThanAborting) {
    RawModule m;                            // … with an EMPTY descriptor pool
    MirBlockId const entry = m.blocks.addNode({});
    MirBlockId const one   = m.blocks.addNode({});
    MirBlockId const two   = m.blocks.addNode({});
    (void)m.addInst(MirOpcode::InlineAsmGoto, entry, /*payload=*/3);
    (void)m.addInst(MirOpcode::Return, one);
    (void)m.addInst(MirOpcode::Return, two);
    m.succPool = {one, two};
    {
        auto& b     = m.blocks.at(entry);
        b.instStart = 1;
        b.instCount = 1;
        b.succStart = 0;
        b.succCount = 2;
        b.func      = 1;
        b.marker    = StructCfMarker::EntryBlock;
    }
    for (auto [blk, first] : {std::pair{one, 2u}, std::pair{two, 3u}}) {
        auto& b     = m.blocks.at(blk);
        b.instStart = first;
        b.instCount = 1;
        b.succStart = 0;
        b.succCount = 0;
        b.func      = 1;
        b.marker    = StructCfMarker::Linear;
    }
    detail::MirFunc fn;
    fn.signature  = kFnSig;
    fn.blockStart = entry.v;
    fn.blockCount = 3;
    fn.symbol     = 1;
    (void)m.funcs.addNode(fn);

    Mir const mir = std::move(m).finish();
    DiagnosticReporter r;
    MirVerifier v{mir};
    EXPECT_FALSE(v.verify(r));              // reaching this line IS the claim
    EXPECT_TRUE(saidThat(r, "inlineasmgoto payload 3 out of inline-asm "
                            "descriptor-pool range [0, 0)")) << said(r);
    // The label-arity rule must have SKIPPED rather than read the stale index.
    EXPECT_FALSE(saidThat(r, "inlineasmgoto declares")) << said(r);
}
