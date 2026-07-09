// MIR-tier Loop-Invariant Code Motion unit tests.
//
// Scope (c1): hoist provably-invariant pure insts whose operands
// are all defined OUTSIDE the loop body. Trap-eligible ops (SDiv /
// UDiv / SMod / UMod), Load (alias-unsafe), Volatile, Phi, and
// side-effecting opcodes are all NOT hoisted.
//
// Pins:
//   * Invariant Add hoisted from loop body to preheader
//     (instructionsHoisted == 1).
//   * Non-invariant inst (operand defined inside loop) NOT hoisted.
//   * SDiv (trap-eligible) NOT hoisted (anchored
//     D-OPT6-LICM-TRAP-SAFE-HOIST).
//   * Load NOT hoisted (alias-unsafe defer).
//   * Volatile-flagged inst NOT hoisted.
//   * Loop with multiple external predecessors NOT hoisted (preheader
//     insertion deferred to D-OPT6-LICM-PREHEADER-INSERTION).
//   * Function with no loops → no-op (instructionsHoisted == 0).
//   * Multi-function: per-function counter accumulation.
//   * Runtime-init carve-out parity.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "diagnostic_count.hpp"
#include "opt/passes/licm.hpp"

#include <gtest/gtest.h>

#include <cstdint>

using namespace dss;

// Invariant Add(a, b) where a and b are entry-block Consts → hoisted.
TEST(Licm, InvariantAddHoistedFromLoopBody) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirLiteralValue v4; v4.value = std::int64_t{4}; v4.core = TypeKind::I32;
    MirInstId const a = mb.addConst(v3, i32);
    MirInstId const b = mb.addConst(v4, i32);
    mb.addBr(header);
    mb.beginBlock(header);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
    // The hoistable inst: Add(a, b). Both operands are in entry → invariant.
    MirInstId const ops[] = {a, b};
    (void)mb.addInst(MirOpcode::Add, ops, i32);
    mb.addBr(header);
    mb.beginBlock(exitB);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 1u);
}

// D-OPT6-LICM-CHAINED-INVARIANTS closure (cycle 10j, 2026-06-04):
// a second-order invariant `y = x*c` whose operand `x = a+b` is
// itself a first-order invariant (all of a, b, c defined in
// entry) gets hoisted in the SAME analyze() call via the per-
// loop fixed-point iteration. Pre-10j only the first-order `x`
// would hoist (instructionsHoisted == 1); post-10j both hoist
// (instructionsHoisted == 2).
//
// Without this, a release pipeline `[..., Licm, ..., Licm, ...]`
// would have to run LICM twice to surface the chained case —
// burning a pipeline iteration for what's structurally a
// single per-loop fixed point. The fixed point also catches
// arbitrary-depth chains in one pass (z = y*d → w = z*e → ...
// each yielding to the prior).
TEST(Licm, ChainedInvariantsHoistedInFixedPoint) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirLiteralValue v4; v4.value = std::int64_t{4}; v4.core = TypeKind::I32;
    MirLiteralValue v5; v5.value = std::int64_t{5}; v5.core = TypeKind::I32;
    MirInstId const a = mb.addConst(v3, i32);
    MirInstId const b = mb.addConst(v4, i32);
    MirInstId const c = mb.addConst(v5, i32);
    mb.addBr(header);
    mb.beginBlock(header);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
    // x = a + b (first-order invariant)
    MirInstId const addOps[] = {a, b};
    MirInstId const x = mb.addInst(MirOpcode::Add, addOps, i32);
    // y = x * c (second-order — chained via x)
    MirInstId const mulOps[] = {x, c};
    (void)mb.addInst(MirOpcode::Mul, mulOps, i32);
    mb.addBr(header);
    mb.beginBlock(exitB);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 2u)
        << "chained-invariant fixed point must hoist BOTH x = a+b "
           "(first-order) AND y = x*c (second-order, via the "
           "round-2 admit of x into the hoistedInThisLoop set). "
           "A regression that left the analyze() at single-pass "
           "would yield 1 (only x hoisted).";
}

// Chained-invariant NEGATIVE — `y = x * d` where `d` is a loop-
// variant value (e.g., the loop's induction Phi). Even though `x`
// is hoisted, `y` MUST stay in the loop because `d` is not
// invariant. Regression guard against a fixed-point bug that
// blanket-admits ALL loop-body operands of an already-hoisted
// inst as "hoisted-too".
TEST(Licm, ChainedInvariantsRejectedWhenSiblingOperandIsLoopVariant) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirLiteralValue v4; v4.value = std::int64_t{4}; v4.core = TypeKind::I32;
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    MirInstId const a     = mb.addConst(v3, i32);
    MirInstId const b     = mb.addConst(v4, i32);
    MirInstId const initI = mb.addConst(v0, i32);
    mb.addBr(header);
    mb.beginBlock(header);
    MirInstId const phi = mb.addPhi(i32);  // loop induction var
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
    // x = a + b   — first-order invariant; hoist OK
    MirInstId const addOps[] = {a, b};
    MirInstId const x = mb.addInst(MirOpcode::Add, addOps, i32);
    // y = x * phi — sibling operand `phi` is loop-variant; y NOT hoist
    MirInstId const mulOps[] = {x, phi};
    MirInstId const next = mb.addInst(MirOpcode::Mul, mulOps, i32);
    mb.addBr(header);
    mb.addPhiIncoming(phi, {initI, entry});
    mb.addPhiIncoming(phi, {next, body});
    mb.beginBlock(exitB);
    mb.addReturn(phi);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 1u)
        << "only x must hoist; y depends on the loop-induction phi "
           "and is loop-variant. A regression that blanket-accepts "
           "all operands once one is hoisted would silently move y "
           "out of the loop → silent miscompile (y would compute "
           "using whatever phi value happens to be at preheader "
           "entry, not per-iteration).";
}

// Non-invariant inst (depends on a loop-body Phi) → NOT hoisted.
TEST(Licm, NonInvariantNotHoisted) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    MirInstId const initI = mb.addConst(v0, i32);
    mb.addBr(header);
    mb.beginBlock(header);
    // Loop-induction Phi: i = phi(0 from entry, i+1 from body).
    MirInstId const phi = mb.addPhi(i32);
    MirLiteralValue v1; v1.value = std::int64_t{1}; v1.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(v1, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
    MirLiteralValue vone; vone.value = std::int64_t{1}; vone.core = TypeKind::I32;
    MirInstId const one = mb.addConst(vone, i32);
    // Add(phi, 1) — operand `phi` is the LOOP HEADER's phi (in the
    // loop body); NOT invariant.
    MirInstId const ops[] = {phi, one};
    MirInstId const next = mb.addInst(MirOpcode::Add, ops, i32);
    mb.addBr(header);
    // Wire up the phi's incomings.
    mb.addPhiIncoming(phi, {initI, entry});
    mb.addPhiIncoming(phi, {next, body});
    mb.beginBlock(exitB);
    mb.addReturn(phi);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 0u)
        << "Add(phi, 1) depends on the loop-body Phi → not invariant";
}

// SDiv (trap-eligible) NOT hoisted even if operands are invariant.
TEST(Licm, TrapEligibleSDivNotHoisted) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirLiteralValue v4; v4.value = std::int64_t{4}; v4.core = TypeKind::I32;
    MirInstId const a = mb.addConst(v3, i32);
    MirInstId const b = mb.addConst(v4, i32);
    mb.addBr(header);
    mb.beginBlock(header);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
    MirInstId const ops[] = {a, b};
    (void)mb.addInst(MirOpcode::SDiv, ops, i32);
    mb.addBr(header);
    mb.beginBlock(exitB);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 0u)
        << "SDiv could trap; hoisting it out of a 0-trip-count loop "
           "would change observable behavior (D-OPT6-LICM-TRAP-SAFE-HOIST)";
}

// FC1 (V2-4.X, 2026-06-10): SMod is trap-eligible exactly like SDiv
// (x86 lowers `%` through the SAME idiv instruction — `x % 0` traps).
// `isTrapEligible` already listed SMod/UMod; this pin makes the
// listing load-bearing — removing SMod from that opcode-enumerated
// list goes RED here, not silently hoist-and-trap.
TEST(Licm, TrapEligibleSModNotHoisted) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirLiteralValue v4; v4.value = std::int64_t{4}; v4.core = TypeKind::I32;
    MirInstId const a = mb.addConst(v3, i32);
    MirInstId const b = mb.addConst(v4, i32);
    mb.addBr(header);
    mb.beginBlock(header);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
    MirInstId const ops[] = {a, b};
    (void)mb.addInst(MirOpcode::SMod, ops, i32);
    mb.addBr(header);
    mb.beginBlock(exitB);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 0u)
        << "SMod could trap (x86 `%` goes through idiv); hoisting it "
           "out of a 0-trip-count loop would change observable "
           "behavior (D-OPT6-LICM-TRAP-SAFE-HOIST discipline).";
}

// Cycle 10b: Load IS a hoist candidate now. A loop-invariant Load
// (pointer defined outside loop AND no may-aliasing Store in body)
// hoists to the preheader.
TEST(Licm, InvariantLoadHoisted) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v42; v42.value = std::int64_t{42}; v42.core = TypeKind::I32;
    MirInstId const c = mb.addConst(v42, i32);
    MirInstId const s[] = {c, slot};
    (void)mb.addInst(MirOpcode::Store, s, InvalidType);
    mb.addBr(header);
    mb.beginBlock(header);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
    MirInstId const lops[] = {slot};
    (void)mb.addInst(MirOpcode::Load, lops, i32);
    mb.addBr(header);
    mb.beginBlock(exitB);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 1u)
        << "alias-clean Load must hoist now that alias substrate is wired";
}

// Multi-function preds-hoist + clobber-index decision-identity pin
// (D-OPT-MEMORYSSA-CLOBBER-WALK; the LICM analog of CSE's
// CrossBlockLoadCseDecidedPerFunctionInMultiFunctionModule): `runLicm` now
// computes the whole-module preds + the clobber index ONCE and threads them
// into every function's analyze — this pins that the hoist decision stays
// PER FUNCTION: fn0's loop body carries an aliasing Store (Load hoist
// refused), fn1's identical loop is clean (Load hoists) → exactly one hoist.
// A leaked clobber/preds scope across functions would mis-count.
TEST(Licm, LoopLoadHoistDecidedPerFunctionInMultiFunctionModule) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    for (std::uint32_t fnIdx = 0; fnIdx < 2; ++fnIdx) {
        mb.addFunction(fnSig, SymbolId{100u + fnIdx});
        MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
        MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
        MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
        mb.beginBlock(entry);
        MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
        MirLiteralValue v42; v42.value = std::int64_t{42}; v42.core = TypeKind::I32;
        MirInstId const c = mb.addConst(v42, i32);
        MirInstId const s[] = {c, slot};
        (void)mb.addInst(MirOpcode::Store, s, InvalidType);
        mb.addBr(header);
        mb.beginBlock(header);
        MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
        MirInstId const cond = mb.addConst(tru, boolT);
        mb.addCondBr(cond, body, exitB);
        mb.beginBlock(body);
        MirInstId const lops[] = {slot};
        (void)mb.addInst(MirOpcode::Load, lops, i32);
        if (fnIdx == 0) {   // aliasing Store in fn0's body ONLY — refuses its hoist
            MirLiteralValue v99; v99.value = std::int64_t{99}; v99.core = TypeKind::I32;
            MirInstId const c99 = mb.addConst(v99, i32);
            MirInstId const s99[] = {c99, slot};
            (void)mb.addInst(MirOpcode::Store, s99, InvalidType);
        }
        mb.addBr(header);
        mb.beginBlock(exitB);
        MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
        mb.addReturn(mb.addConst(v0, i32));
    }
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 1u)
        << "loop-Load hoist decided per function: fn0's body Store refuses, "
           "fn1 (clean body) hoists — the pass-wide preds + clobber index "
           "must not leak across functions";
}

// Negative pin: a may-aliasing Store inside the loop body blocks
// Load hoist. The Store writes through the same Alloca the Load
// reads → Rule 1 (Yes) in body → admission refuses.
TEST(Licm, LoadNotHoistedAcrossAliasingStoreInLoop) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v42; v42.value = std::int64_t{42}; v42.core = TypeKind::I32;
    MirInstId const c = mb.addConst(v42, i32);
    MirInstId const s[] = {c, slot};
    (void)mb.addInst(MirOpcode::Store, s, InvalidType);
    mb.addBr(header);
    mb.beginBlock(header);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
    MirInstId const lops[] = {slot};
    (void)mb.addInst(MirOpcode::Load, lops, i32);
    // Aliasing Store inside the body — clobbers the Load every iteration.
    MirLiteralValue v99; v99.value = std::int64_t{99}; v99.core = TypeKind::I32;
    MirInstId const c99 = mb.addConst(v99, i32);
    MirInstId const sBody[] = {c99, slot};
    (void)mb.addInst(MirOpcode::Store, sBody, InvalidType);
    mb.addBr(header);
    mb.beginBlock(exitB);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 0u)
        << "aliasing Store in loop body must block Load hoist";
}

// Strict-TBAA precision pin: under MirAliasingMode::StrictTBAA, a Store
// through Ptr<I64> inside the loop body cannot alias a Load through
// Ptr<I32>; the Load hoists. Closes
// D-OPT-LOAD-ALIAS-ANALYSIS-STRICT-TBAA-WIRING on the LICM side.
TEST(Licm, LoadHoistedAcrossDistinctPrimitiveStoreInLoopUnderStrictTBAA) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32     = interner.primitive(TypeKind::I32);
    TypeId const i64     = interner.primitive(TypeKind::I64);
    TypeId const boolT   = interner.primitive(TypeKind::Bool);
    TypeId const ptrI32  = interner.pointer(i32);
    TypeId const ptrI64  = interner.pointer(i64);
    TypeId const params[] = {ptrI32, ptrI64};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.setAliasingMode(MirAliasingMode::StrictTBAA);
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirInstId const pI32 = mb.addArg(0, ptrI32);
    MirInstId const pI64 = mb.addArg(1, ptrI64);
    mb.addBr(header);
    mb.beginBlock(header);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
    MirInstId const lops[] = {pI32};
    (void)mb.addInst(MirOpcode::Load, lops, i32);
    // Store through Ptr<I64> — strict-TBAA: doesn't alias the I32 Load.
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I64;
    MirInstId const c0 = mb.addConst(v0, i64);
    MirInstId const sOps[] = {c0, pI64};
    (void)mb.addInst(MirOpcode::Store, sOps, InvalidType);
    mb.addBr(header);
    mb.beginBlock(exitB);
    MirLiteralValue v0r; v0r.value = std::int64_t{0}; v0r.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0r, i32));
    Mir mir = std::move(mb).finish();

    // Flag-state attribution pin (symmetric with CSE): if a future
    // MirBuilder regression drops `setAliasingMode`, attribute the
    // failure at the mode-threading, not the alias predicate.
    ASSERT_EQ(mir.aliasingMode(), MirAliasingMode::StrictTBAA);

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 1u)
        << "strict-TBAA: Store<I64> in body cannot alias Load<I32>; LICM hoists";
}

// Negative polarity for the above: same fixture under Permissive (the
// default) refuses the hoist. Paired with the strict-TBAA positive,
// proves the LICM consumer reads `mir.aliasingMode()` rather than
// hardcoding one polarity.
TEST(Licm, LoadNotHoistedAcrossDistinctPrimitiveStoreInLoopUnderPermissive) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32     = interner.primitive(TypeKind::I32);
    TypeId const i64     = interner.primitive(TypeKind::I64);
    TypeId const boolT   = interner.primitive(TypeKind::Bool);
    TypeId const ptrI32  = interner.pointer(i32);
    TypeId const ptrI64  = interner.pointer(i64);
    TypeId const params[] = {ptrI32, ptrI64};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    // Explicit (not relying on default) so a future flip of the default
    // can't silently make this test vacuous.
    mb.setAliasingMode(MirAliasingMode::Permissive);
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirInstId const pI32 = mb.addArg(0, ptrI32);
    MirInstId const pI64 = mb.addArg(1, ptrI64);
    mb.addBr(header);
    mb.beginBlock(header);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
    MirInstId const lops[] = {pI32};
    (void)mb.addInst(MirOpcode::Load, lops, i32);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I64;
    MirInstId const c0 = mb.addConst(v0, i64);
    MirInstId const sOps[] = {c0, pI64};
    (void)mb.addInst(MirOpcode::Store, sOps, InvalidType);
    mb.addBr(header);
    mb.beginBlock(exitB);
    MirLiteralValue v0r; v0r.value = std::int64_t{0}; v0r.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0r, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 0u)
        << "Permissive: distinct primitive pointees stay Maybe; LICM refuses";
}

// D-OPT-MIR-ALIAS-CHAR-EXCEPTION-OVERRIDE end-to-end pin (LICM side):
// proves the LicmPolicy ctor reads Mir.charTypesAliasAll() and threads
// it to mirAnyMayAliasingStoreInLoop. Fixture: Load through Ptr<Char>
// in loop body + Store through Ptr<I32> in same body. Under default
// (char-aliases-all=true) strict-TBAA: Rule 5 fires → Maybe → LICM
// refuses. Under char-aliases-all=false strict-TBAA: Rule 6
// distinguishes Char vs I32 → No → LICM hoists.
TEST(Licm, LoadHoistedAcrossDistinctPrimitiveStoreInLoopUnderStrictTBAANoCharException) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const charT  = interner.primitive(TypeKind::Char);
    TypeId const boolT  = interner.primitive(TypeKind::Bool);
    TypeId const ptrCh  = interner.pointer(charT);
    TypeId const ptrI32 = interner.pointer(i32);
    TypeId const params[] = {ptrCh, ptrI32};
    TypeId const fnSig = interner.fnSig(params, charT, CallConv::CcSysV);

    MirBuilder mb;
    mb.setAliasingMode(MirAliasingMode::StrictTBAA);
    mb.setCharTypesAliasAll(false);  // Rust-like / strict-typed DSL
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirInstId const pCh  = mb.addArg(0, ptrCh);
    MirInstId const pI32 = mb.addArg(1, ptrI32);
    mb.addBr(header);
    mb.beginBlock(header);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
    MirInstId const lops[] = {pCh};
    (void)mb.addInst(MirOpcode::Load, lops, charT);
    // Store through Ptr<I32> in loop body — under strict + char-
    // exception-disabled, cannot alias the Char Load.
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(v0, i32);
    MirInstId const sOps[] = {c0, pI32};
    (void)mb.addInst(MirOpcode::Store, sOps, InvalidType);
    mb.addBr(header);
    mb.beginBlock(exitB);
    MirLiteralValue v0r; v0r.value = std::int64_t{0}; v0r.core = TypeKind::Char;
    mb.addReturn(mb.addConst(v0r, charT));
    Mir mir = std::move(mb).finish();

    // Flag-state attribution pin (symmetric with CSE).
    ASSERT_EQ(mir.aliasingMode(), MirAliasingMode::StrictTBAA);
    ASSERT_FALSE(mir.charTypesAliasAll());

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 1u)
        << "strict + char-exception-disabled: Store<I32> cannot alias "
           "Load<Char>; LICM must hoist";
}

// Negative polarity: same fixture under default charTypesAliasAll=true.
TEST(Licm, LoadNotHoistedAcrossDistinctPrimitiveStoreInLoopUnderStrictTBAAWithCharException) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const charT  = interner.primitive(TypeKind::Char);
    TypeId const boolT  = interner.primitive(TypeKind::Bool);
    TypeId const ptrCh  = interner.pointer(charT);
    TypeId const ptrI32 = interner.pointer(i32);
    TypeId const params[] = {ptrCh, ptrI32};
    TypeId const fnSig = interner.fnSig(params, charT, CallConv::CcSysV);

    MirBuilder mb;
    mb.setAliasingMode(MirAliasingMode::StrictTBAA);
    mb.setCharTypesAliasAll(true);  // C/C++/ObjC default — explicit
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirInstId const pCh  = mb.addArg(0, ptrCh);
    MirInstId const pI32 = mb.addArg(1, ptrI32);
    mb.addBr(header);
    mb.beginBlock(header);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
    MirInstId const lops[] = {pCh};
    (void)mb.addInst(MirOpcode::Load, lops, charT);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(v0, i32);
    MirInstId const sOps[] = {c0, pI32};
    (void)mb.addInst(MirOpcode::Store, sOps, InvalidType);
    mb.addBr(header);
    mb.beginBlock(exitB);
    MirLiteralValue v0r; v0r.value = std::int64_t{0}; v0r.core = TypeKind::Char;
    mb.addReturn(mb.addConst(v0r, charT));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 0u)
        << "strict + char-exception-enabled: char* may alias int*; LICM refuses";
}

// Volatile-flagged inst NOT hoisted even if otherwise invariant.
TEST(Licm, VolatileBinaryOpNotHoisted) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirLiteralValue v4; v4.value = std::int64_t{4}; v4.core = TypeKind::I32;
    MirInstId const a = mb.addConst(v3, i32);
    MirInstId const b = mb.addConst(v4, i32);
    mb.addBr(header);
    mb.beginBlock(header);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
    MirInstId const ops[] = {a, b};
    (void)mb.addInst(MirOpcode::Add, ops, i32, /*payload*/0,
                     MirInstFlags::Volatile);
    mb.addBr(header);
    mb.beginBlock(exitB);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 0u);
}

// Load-specific Volatile guard (licm.cpp:326). The sibling
// `VolatileBinaryOpNotHoisted` exercises only an `Add`, leaving the Load-
// admission path — which sits AFTER the Volatile `continue` — unpinned. This
// fixture is byte-identical to `InvariantLoadHoisted` (a clean pointer defined
// OUTSIDE the loop, empty body, NO aliasing Store → that test hoists the Load,
// instructionsHoisted == 1) EXCEPT the loop-body Load carries
// MirInstFlags::Volatile. The single-bit delta isolates the Volatile `continue`
// at licm.cpp:326, which runs BEFORE the Load alias-admission gate: with it,
// instructionsHoisted == 0 and the volatile Load stays physically in the body.
// RED-ON-DISABLE: neutralize the `if (has(...Volatile)) continue;` at
// licm.cpp:326 → the volatile Load hoists → instructionsHoisted == 1.
TEST(Licm, VolatileLoadInOtherwiseHoistableLoopNotHoisted) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v42; v42.value = std::int64_t{42}; v42.core = TypeKind::I32;
    MirInstId const c = mb.addConst(v42, i32);
    MirInstId const s[] = {c, slot};
    (void)mb.addInst(MirOpcode::Store, s, InvalidType);
    mb.addBr(header);
    mb.beginBlock(header);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
    // Identical to InvariantLoadHoisted's body Load EXCEPT the Volatile flag —
    // the pointer `slot` is loop-invariant and no aliasing Store sits in the
    // body, so ONLY the Volatile bit can stop the hoist.
    MirInstId const lops[] = {slot};
    (void)mb.addInst(MirOpcode::Load, lops, i32, /*payload*/0,
                     MirInstFlags::Volatile);
    mb.addBr(header);
    mb.beginBlock(exitB);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 0u)
        << "a Volatile Load must NOT be hoisted even though its pointer is loop-"
           "invariant and no aliasing Store sits in the body — licm.cpp:326's "
           "Volatile `continue` runs before the Load alias-admission gate";

    // The volatile Load must remain PHYSICALLY in the loop-body (LoopLatch)
    // block; NO Load may have been relocated into the preheader (EntryBlock).
    // `instructionsHoisted == 0` alone is a counter check; this walk proves the
    // instruction did not move.
    std::size_t volLoadsInBody   = 0;
    std::size_t loadsInPreheader = 0;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const blk = mir.funcBlockAt(f, bi);
            StructCfMarker const mrk = mir.blockMarker(blk);
            std::uint32_t const ni = mir.blockInstCount(blk);
            for (std::uint32_t i2 = 0; i2 < ni; ++i2) {
                MirInstId const id = mir.blockInstAt(blk, i2);
                if (mir.instOpcode(id) != MirOpcode::Load) continue;
                if (mrk == StructCfMarker::LoopLatch &&
                    has(mir.instFlags(id), MirInstFlags::Volatile)) {
                    ++volLoadsInBody;
                }
                if (mrk == StructCfMarker::EntryBlock) ++loadsInPreheader;
            }
        }
    }
    EXPECT_EQ(volLoadsInBody, 1u)
        << "the volatile Load must still live in the loop body (LoopLatch), "
           "not be moved to the preheader";
    EXPECT_EQ(loadsInPreheader, 0u)
        << "no Load may be hoisted into the preheader (EntryBlock)";
}

// Test-analyzer Critical Gap 1: assert the hoisted inst actually
// LANDS IN THE PREHEADER. `instructionsHoisted == 1` is satisfied
// by the policy's counter regardless of which block the clone
// lands in. This walks the post-pass MIR to verify the Add ended
// up OUTSIDE the loop body (specifically in the preheader, which
// is the entry block in our fixture).
TEST(Licm, InvariantHoistLandsInPreheaderNotLoopBody) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirLiteralValue v4; v4.value = std::int64_t{4}; v4.core = TypeKind::I32;
    MirInstId const a = mb.addConst(v3, i32);
    MirInstId const b = mb.addConst(v4, i32);
    mb.addBr(header);
    mb.beginBlock(header);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
    MirInstId const ops[] = {a, b};
    (void)mb.addInst(MirOpcode::Add, ops, i32);
    mb.addBr(header);
    mb.beginBlock(exitB);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 1u);

    // Walk post-pass MIR. Find the Add. Its block's marker should
    // be EntryBlock (the preheader). The LoopLatch (body) should
    // contain NO Add.
    std::size_t addsInEntry = 0;
    std::size_t addsInBody  = 0;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const blk = mir.funcBlockAt(f, bi);
            StructCfMarker const m = mir.blockMarker(blk);
            std::uint32_t const ni = mir.blockInstCount(blk);
            for (std::uint32_t i2 = 0; i2 < ni; ++i2) {
                MirInstId const id = mir.blockInstAt(blk, i2);
                if (mir.instOpcode(id) != MirOpcode::Add) continue;
                if (m == StructCfMarker::EntryBlock) ++addsInEntry;
                if (m == StructCfMarker::LoopLatch)  ++addsInBody;
            }
        }
    }
    EXPECT_EQ(addsInEntry, 1u)
        << "the hoisted Add must land in the preheader (entry block)";
    EXPECT_EQ(addsInBody, 0u)
        << "the loop body must NO LONGER contain the Add — it was hoisted";
}

// Test-analyzer Important Gap 5: a loop with TWO non-back-edge
// predecessors (ambiguous preheader) is conservatively SKIPPED.
// The pass's `preheader.valid()` + `!ambiguous` gate refuses to
// hoist; the obvious invariant stays in the loop body.
TEST(Licm, MultiplePreheaderPredsSkipsLoop) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    // entry → {p1, p2} → header → body → header back-edge; header → exit.
    // header has TWO external preds (p1, p2) + one back-edge pred (body).
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const p1     = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const p2     = mb.createBlock(StructCfMarker::IfElse);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirLiteralValue v4; v4.value = std::int64_t{4}; v4.core = TypeKind::I32;
    MirInstId const a = mb.addConst(v3, i32);
    MirInstId const b = mb.addConst(v4, i32);
    mb.addCondBr(cond, p1, p2);
    mb.beginBlock(p1); mb.addBr(header);
    mb.beginBlock(p2); mb.addBr(header);
    mb.beginBlock(header);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const c = mb.addConst(tru, boolT);
    mb.addCondBr(c, body, exitB);
    mb.beginBlock(body);
    // Obvious invariant: Add(a, b) — but loop has 2 external preds,
    // so c1's preheader-singleton gate skips this loop.
    MirInstId const ops[] = {a, b};
    (void)mb.addInst(MirOpcode::Add, ops, i32);
    mb.addBr(header);
    mb.beginBlock(exitB);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 0u)
        << "loop with >1 external pred has no unique preheader → "
           "c1 conservatively skips (D-OPT6-LICM-PREHEADER-INSERTION)";
    // Cycle 10l closure: the skip is now observable via Info-severity
    // X_OptPassSkipped citing the deferred anchor. Pre-10l this was
    // a silent `continue` — developers couldn't tell why LICM didn't
    // fire on a hoist-eligible loop with multiple external preds.
    EXPECT_EQ(::dss::test_support::countCode(
                  rep, DiagnosticCode::X_OptPassSkipped), 1u)
        << "ambiguous-preheader skip must emit exactly one Info-severity "
           "X_OptPassSkipped diagnostic citing "
           "D-OPT6-LICM-PREHEADER-INSERTION — observable for developers";
}

// Nested-loop invariant — surfaced by the OPT6 2nd-look review as
// CRITICAL #1. An inst (e.g. Add of two entry-block Consts) is
// invariant in BOTH the outer and inner loops. Pre-fold, both loop-
// iterations of `analyze()` would try to `recordHoist` the same
// id, triggering the `!inserted` substrate-violation abort. Post-
// fold, the second visit short-circuits via the
// `hoistedInsts_.count(id) != 0` gate. The candidate is hoisted to
// the OUTERMOST valid preheader (deepest hoist). This test would
// have aborted the test process pre-fold.
TEST(Licm, NestedLoopInvariantHoistedToOuterPreheader) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    // entry → outerH → outerBody=innerH → innerBody → innerH (back)
    // outerBody → outerH (back); outerH → exitB
    MirBlockId const entry      = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const outerH     = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const innerH     = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const innerBody  = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const outerLatch = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB      = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirLiteralValue v4; v4.value = std::int64_t{4}; v4.core = TypeKind::I32;
    MirInstId const a = mb.addConst(v3, i32);
    MirInstId const b = mb.addConst(v4, i32);
    mb.addBr(outerH);
    mb.beginBlock(outerH);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cOuter = mb.addConst(tru, boolT);
    mb.addCondBr(cOuter, innerH, exitB);
    mb.beginBlock(innerH);
    MirInstId const cInner = mb.addConst(tru, boolT);
    mb.addCondBr(cInner, innerBody, outerLatch);
    mb.beginBlock(innerBody);
    // The hoist candidate: Add(a, b). Operands defined in entry.
    // Invariant in BOTH the inner loop (body = {innerH, innerBody})
    // AND the outer loop (body = {outerH, innerH, innerBody, outerLatch}).
    MirInstId const ops[] = {a, b};
    (void)mb.addInst(MirOpcode::Add, ops, i32);
    mb.addBr(innerH);
    mb.beginBlock(outerLatch);
    mb.addBr(outerH);
    mb.beginBlock(exitB);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 1u)
        << "the entry-Const-operand Add is invariant in BOTH loops; "
           "the recordHoist dedup must register it exactly once";
}

// Multi-function: counter accumulates across functions. Each
// function has an independently-hoistable invariant.
TEST(Licm, MultiFunctionCounterAccumulates) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    for (std::uint32_t fnIdx = 0; fnIdx < 2; ++fnIdx) {
        mb.addFunction(fnSig, SymbolId{100u + fnIdx});
        MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
        MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
        MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
        mb.beginBlock(entry);
        MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
        MirLiteralValue v4; v4.value = std::int64_t{4}; v4.core = TypeKind::I32;
        MirInstId const a = mb.addConst(v3, i32);
        MirInstId const b = mb.addConst(v4, i32);
        mb.addBr(header);
        mb.beginBlock(header);
        MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
        MirInstId const cond = mb.addConst(tru, boolT);
        mb.addCondBr(cond, body, exitB);
        mb.beginBlock(body);
        MirInstId const ops[] = {a, b};
        (void)mb.addInst(MirOpcode::Add, ops, i32);
        mb.addBr(header);
        mb.beginBlock(exitB);
        MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
        mb.addReturn(mb.addConst(v0, i32));
    }
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 2u)
        << "each function's invariant Add hoists independently — "
           "counter accumulates across functions";
}

// Function with no loops: pass is a no-op.
TEST(Licm, NoLoopsNoOp) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirLiteralValue v; v.value = std::int64_t{42}; v.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 0u);
}

// Runtime-init carve-out parity.
TEST(Licm, RuntimeInitGlobalsModuleEmitsXOptPassSkippedInfo) {
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
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 0u);
    std::size_t infoCount = 0;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::X_OptPassSkipped) ++infoCount;
    }
    EXPECT_EQ(infoCount, 1u);
}
