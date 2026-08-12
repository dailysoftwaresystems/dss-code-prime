// MIR-tier Loop-Invariant Code Motion unit tests.
//
// Scope: hoist provably-invariant pure insts whose operands are all
// defined OUTSIDE the loop body. Volatile, Phi and side-effecting
// opcodes are never hoisted. MAY-FAULT opcodes — Load and the
// divisions SDiv / UDiv / SMod / UMod — are hoisted only out of a
// block that is GUARANTEED TO EXECUTE when the loop is entered
// (D-OPT6-LICM-SPECULATIVE-LOAD-HOIST).
//
// ⚠ FIXTURE DISCIPLINE, learned by breaking it: a Load or a division
// placed in a conditionally-executed body block is refused by the
// speculation gate BEFORE any alias / mode / char-exception question is
// reached, so a fixture shaped that way pins NOTHING about aliasing. The
// Load-family fixtures below therefore put their Load in the loop HEADER
// (which runs on every loop entry). Moving one back into `body` turns its
// test green-but-vacuous.
//
// Pins:
//   * Invariant Add hoisted from loop body to preheader
//     (instructionsHoisted == 1).
//   * Non-invariant inst (operand defined inside loop) NOT hoisted.
//   * SDiv / SMod NOT hoisted from a conditionally-executed block, and
//     SDiv DOES hoist from a guaranteed-to-execute one (the pair is what
//     makes either pin non-vacuous; D-OPT6-LICM-TRAP-SAFE-HOIST).
//   * The may-fault speculation battery: conditional block, guaranteed
//     non-header block, in-loop early return, exit-less loop, a
//     preheader that can skip the loop, and a loop containing a Call.
//   * Volatile-flagged inst NOT hoisted.
//   * Loop with multiple external predecessors NOT hoisted (preheader
//     insertion deferred to D-OPT6-LICM-PREHEADER-INSERTION).
//   * Function with no loops → no-op (instructionsHoisted == 0).
//   * Multi-function: per-function counter accumulation.
//   * Runtime-init carve-out parity.

#include "core/types/arg_payload.hpp"
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

// SDiv (may-fault) NOT hoisted out of a CONDITIONALLY-EXECUTED block even
// though its operands are invariant. `body` is reached only through the
// header's CondBr, so at a 0 trip count it never runs; hoisting would run
// the division in the preheader, which runs unconditionally.
// The refusal now comes from the SHARED may-fault gate
// (`mayFaultWhenSpeculated` + guaranteed-to-execute) rather than from a
// blanket opcode veto — see TrapEligibleSDivHoistedWhenGuaranteedToExecute
// for the other polarity, which is what makes this pin non-vacuous.
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

// FC1 (V2-4.X, 2026-06-10): SMod may fault exactly like SDiv (x86 lowers
// `%` through the SAME idiv instruction — `x % 0` traps).
// `mayFaultWhenSpeculated` lists SMod/UMod; this pin makes the listing
// load-bearing — removing SMod from that opcode-enumerated list goes RED
// here, not silently hoist-and-trap.
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

// ═══════════════════════════════════════════════════════════════════════
// D-OPT6-LICM-SPECULATIVE-LOAD-HOIST — the may-fault speculation gate.
//
// Witnessed live before the gate existed: a release build of
//     for (i = 0; i < n; i++) { if (q) x = *p; }        /* q==0, p==NULL */
// exited 0xC0000005 (ACCESS VIOLATION) while the debug build exited 0,
// because LICM moved the guarded `*p` into the preheader. The gate below
// is the fix: a may-fault op may only leave a block that runs on EVERY
// entry into the loop.
//
// Each pin below isolates ONE of the gate's four conditions and pairs it
// with a hoist that MUST still happen, so no pin can pass by the pass
// simply refusing everything.
// ═══════════════════════════════════════════════════════════════════════

namespace {

// Post-pass MIR probe: count Loads sitting in blocks carrying `marker`
// whose POINTER OPERAND is the parameter with ordinal `argOrdinal`.
//
// A bare "one Load moved" count cannot express the claim these pins make.
// The conditional-block fixture holds TWO Loads and expects exactly one of
// them — a specific one — to be hoisted; a count of 1 is equally satisfied
// by hoisting the WRONG one, which is precisely the miscompile. Pinning
// the operand chain names the instruction.
//
// The ordinal MUST come through `arg_payload::ordinal()`: an Arg's payload
// is an ENCODED (ordinal, position) pair, so comparing the raw payload to
// an ordinal matches only argument 0 — which made an earlier draft of the
// ordinal-1 assertion below pass VACUOUSLY (caught by its sibling
// "*pB must still be physically present" assertion going red).
[[nodiscard]] std::size_t countLoadsOfArgIn(Mir const& mir,
                                            StructCfMarker marker,
                                            std::uint32_t argOrdinal) {
    std::size_t n = 0;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const blk = mir.funcBlockAt(f, bi);
            if (mir.blockMarker(blk) != marker) continue;
            std::uint32_t const ni = mir.blockInstCount(blk);
            for (std::uint32_t ii = 0; ii < ni; ++ii) {
                MirInstId const id = mir.blockInstAt(blk, ii);
                if (mir.instOpcode(id) != MirOpcode::Load) continue;
                auto const ops = mir.instOperands(id);
                if (ops.empty()) continue;
                if (mir.instOpcode(ops[0]) != MirOpcode::Arg) continue;
                if (arg_payload::ordinal(mir.instPayload(ops[0]))
                    != argOrdinal) continue;
                ++n;
            }
        }
    }
    return n;
}

// Sibling for the non-Load polarities (the Add / SDiv that must still move).
[[nodiscard]] std::size_t countOpIn(Mir const& mir, StructCfMarker marker,
                                    MirOpcode op) {
    std::size_t n = 0;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const blk = mir.funcBlockAt(f, bi);
            if (mir.blockMarker(blk) != marker) continue;
            std::uint32_t const ni = mir.blockInstCount(blk);
            for (std::uint32_t ii = 0; ii < ni; ++ii) {
                if (mir.instOpcode(mir.blockInstAt(blk, ii)) == op) ++n;
            }
        }
    }
    return n;
}

} // namespace

// ★ THE MISCOMPILE PIN. Two invariant, alias-clean Loads in one loop:
// `*pA` in the HEADER (runs on every loop entry) and `*pB` in a block
// reached only through a second CondBr (the `if (q)` arm). Exactly ONE
// may move, and WHICH one is the whole claim.
//
// RED-ON-DISABLE (executed): delete the `mayFaultWhenSpeculated(op) &&
// !blockRunsOnEveryLoopEntry(b)` gate in licm.cpp's candidate scan →
// instructionsHoisted becomes 2 and `countLoadsOfArgIn(EntryBlock, 1)`
// becomes 1 — the guarded dereference lands in the preheader, which is
// the 0xC0000005 above.
TEST(Licm, MayFaultLoadNotHoistedFromConditionallyExecutedBlock) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const boolT  = interner.primitive(TypeKind::Bool);
    TypeId const ptrI32 = interner.pointer(i32);
    TypeId const params[] = {ptrI32, ptrI32, boolT, boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    // entry → header → {guard → {deref, latch}} → latch → header (back)
    // header → exitB is the ONLY way out of the loop.
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const guard  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const deref  = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const latch  = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirInstId const pA     = mb.addArg(0, ptrI32);
    MirInstId const pB     = mb.addArg(1, ptrI32);
    MirInstId const cTrip  = mb.addArg(2, boolT);
    MirInstId const cGuard = mb.addArg(3, boolT);
    mb.addBr(header);
    mb.beginBlock(header);
    // `*pA` — the header runs whenever the loop is entered, so this one
    // is guaranteed to execute and MUST hoist.
    MirInstId const loadA[] = {pA};
    (void)mb.addInst(MirOpcode::Load, loadA, i32);
    mb.addCondBr(cTrip, guard, exitB);
    mb.beginBlock(guard);
    mb.addCondBr(cGuard, deref, latch);
    mb.beginBlock(deref);
    // `*pB` — the `if (q)` arm. Invariant and alias-clean exactly like
    // `*pA`; the ONLY difference is that `deref` does not dominate the
    // loop's exiting block, so it may run ZERO times.
    MirInstId const loadB[] = {pB};
    (void)mb.addInst(MirOpcode::Load, loadB, i32);
    mb.addBr(latch);
    mb.beginBlock(latch);
    mb.addBr(header);
    mb.beginBlock(exitB);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 1u)
        << "exactly one of the two Loads is guaranteed to execute; the "
           "conditionally-executed one must stay in the loop";
    EXPECT_EQ(countLoadsOfArgIn(mir, StructCfMarker::EntryBlock, 0u), 1u)
        << "the HEADER Load (*pA) must be hoisted into the preheader — "
           "the gate must not simply disable Load hoisting";
    EXPECT_EQ(countLoadsOfArgIn(mir, StructCfMarker::EntryBlock, 1u), 0u)
        << "the GUARDED Load (*pB) must NOT reach the preheader: with a "
           "null pB and a false guard the source never dereferences, so a "
           "hoist here is a fault the program never had "
           "(D-OPT6-LICM-SPECULATIVE-LOAD-HOIST)";
    EXPECT_EQ(countLoadsOfArgIn(mir, StructCfMarker::IfThen, 1u), 1u)
        << "*pB must still be physically present in the guarded block";
}

// Precision pin for the DOMINANCE half of the gate — the header
// fast path is not enough. A bottom-tested loop (`do { … } while (c)`):
// `mid` is NOT the header, yet it dominates the loop's only exiting block
// (`latch`), so it runs on every entry and the Load MUST hoist.
//
// RED-ON-DISABLE (executed): replace the dominance walk with a bare
// `b.v == loop.header.v` test → instructionsHoisted drops to 0 here while
// every negative pin above still passes. That mutant is the "fix by
// switching the optimization off" failure this pin exists to catch.
TEST(Licm, MayFaultLoadHoistedFromGuaranteedNonHeaderBlock) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const boolT  = interner.primitive(TypeKind::Bool);
    TypeId const ptrI32 = interner.pointer(i32);
    TypeId const params[] = {ptrI32, boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    // entry → header → mid → latch → {header (back), exitB}
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const mid    = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const latch  = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirInstId const p = mb.addArg(0, ptrI32);
    MirInstId const c = mb.addArg(1, boolT);
    mb.addBr(header);
    mb.beginBlock(header);
    mb.addBr(mid);
    mb.beginBlock(mid);
    MirInstId const lops[] = {p};
    (void)mb.addInst(MirOpcode::Load, lops, i32);
    mb.addBr(latch);
    mb.beginBlock(latch);
    mb.addCondBr(c, header, exitB);
    mb.beginBlock(exitB);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 1u)
        << "`mid` is not the header but dominates the loop's only exiting "
           "block, so the Load runs on every loop entry and must hoist — "
           "the gate must keep the legitimate hoists it had";
    EXPECT_EQ(countLoadsOfArgIn(mir, StructCfMarker::EntryBlock, 0u), 1u)
        << "the hoisted Load must land in the preheader";
    EXPECT_EQ(countLoadsOfArgIn(mir, StructCfMarker::Linear, 0u), 0u)
        << "and must no longer be in `mid`";
}

// The source shape `for (…) { if (r) return 0; … x = *p; }` — control can
// leave the function from inside the loop before the dereference is ever
// reached, so `*p` may run zero times and must stay put.
//
// ⚠ MEASURED, and it corrected a wrong hypothesis while this pin was being
// written: the Return block is NOT part of `loop.body`. `mirNaturalLoops`
// builds the body by walking PREDECESSORS back from the back-edge sources,
// and a zero-successor block is a predecessor of nothing on that walk. So
// what refuses the hoist is the ordinary CROSS-EDGE from `header` to the
// out-of-body `retBlk`: that makes `header` an exiting block, and `cont`
// does not dominate `header`. An earlier draft of licm.cpp carried a
// "zero-successor blocks are exits too" arm for this case; the arm could
// not be made to fire and was deleted rather than left as unpinnable code
// with a comment claiming it mattered.
//
// RED-ON-DISABLE (executed): remove the may-fault gate from licm.cpp's
// candidate scan → instructionsHoisted becomes 1 and the dereference lands
// in the preheader, running on the `r != 0` path that returns immediately.
TEST(Licm, MayFaultLoadNotHoistedWhenLoopCanReturnBeforeIt) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const boolT  = interner.primitive(TypeKind::Bool);
    TypeId const ptrI32 = interner.pointer(i32);
    TypeId const params[] = {ptrI32, boolT, boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    // entry → header → {retBlk (Return, IN the loop), cont} → latch →
    // {header (back), exitB}
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const retBlk = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const cont   = mb.createBlock(StructCfMarker::IfElse);
    MirBlockId const latch  = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirInstId const p  = mb.addArg(0, ptrI32);
    MirInstId const cR = mb.addArg(1, boolT);
    MirInstId const cT = mb.addArg(2, boolT);
    mb.addBr(header);
    mb.beginBlock(header);
    mb.addCondBr(cR, retBlk, cont);
    mb.beginBlock(retBlk);
    MirLiteralValue v7; v7.value = std::int64_t{7}; v7.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v7, i32));
    mb.beginBlock(cont);
    MirInstId const lops[] = {p};
    (void)mb.addInst(MirOpcode::Load, lops, i32);
    mb.addBr(latch);
    mb.beginBlock(latch);
    mb.addCondBr(cT, header, exitB);
    mb.beginBlock(exitB);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 0u)
        << "control can leave the loop through the in-loop Return before "
           "ever reaching `cont`; a zero-successor block is an exit too";
    EXPECT_EQ(countLoadsOfArgIn(mir, StructCfMarker::EntryBlock, 0u), 0u)
        << "no Load may reach the preheader";
    EXPECT_EQ(countLoadsOfArgIn(mir, StructCfMarker::IfElse, 0u), 1u)
        << "the Load must still be physically present in `cont`";
}

// A loop with NO exiting block at all — `for (;;) { if (q) x = *p; }` — can
// never leave, so "B dominates every exiting block" is VACUOUSLY true for
// every B and proves nothing. The guarded dereference still runs zero times
// when `q` is false, while the hoisted one would fault; the original program
// merely hangs. The empty-exit set must therefore refuse, not admit.
//
// RED-ON-DISABLE (executed): change `ok = !exits.empty();` to `ok = true;`
// in licm.cpp → the `for` over an empty `exits` is skipped, the block is
// declared guaranteed-to-execute, and instructionsHoisted becomes 1.
TEST(Licm, MayFaultLoadNotHoistedWhenLoopHasNoExit) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const boolT  = interner.primitive(TypeKind::Bool);
    TypeId const ptrI32 = interner.pointer(i32);
    TypeId const params[] = {ptrI32, boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    // entry → header → {deref, skip} → latch → header (back). NOTHING
    // leaves the loop: no block has a successor outside the body.
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const deref  = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const skip   = mb.createBlock(StructCfMarker::IfElse);
    MirBlockId const latch  = mb.createBlock(StructCfMarker::LoopLatch);
    mb.beginBlock(entry);
    MirInstId const p = mb.addArg(0, ptrI32);
    MirInstId const q = mb.addArg(1, boolT);
    mb.addBr(header);
    mb.beginBlock(header);
    mb.addCondBr(q, deref, skip);
    mb.beginBlock(deref);
    MirInstId const lops[] = {p};
    (void)mb.addInst(MirOpcode::Load, lops, i32);
    mb.addBr(latch);
    mb.beginBlock(skip);
    mb.addBr(latch);
    mb.beginBlock(latch);
    mb.addBr(header);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 0u)
        << "a loop with no exiting block can never leave, so `dominates "
           "every exit` is vacuous — an empty exit set must refuse";
    EXPECT_EQ(countLoadsOfArgIn(mir, StructCfMarker::EntryBlock, 0u), 0u)
        << "no Load may reach the preheader";
    EXPECT_EQ(countLoadsOfArgIn(mir, StructCfMarker::IfThen, 0u), 1u)
        << "the Load must still be physically present in the guarded block";
}

// The preheader is only "the header's unique predecessor outside the
// loop" — it may still branch elsewhere. `if (c) for (…) x = *p;` puts the
// `if` block in that role: executing it does NOT mean entering the loop, so
// even a HEADER Load may not be hoisted into it. The invariant `Add` in the
// same header still hoists, which is what keeps this pin honest.
//
// RED-ON-DISABLE (executed): force `entryIsUnconditional = true` in
// licm.cpp → instructionsHoisted becomes 2 and a Load appears in the
// EntryBlock, i.e. `*p` executes on the `c == false` path.
TEST(Licm, MayFaultLoadNotHoistedWhenPreheaderCanSkipTheLoop) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const boolT  = interner.primitive(TypeKind::Bool);
    TypeId const ptrI32 = interner.pointer(i32);
    TypeId const params[] = {ptrI32, boolT, boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    // entry → {header, skip}; header → {body, exitB}; body → header (back).
    // `entry` IS the preheader and it can skip the loop entirely.
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const skip   = mb.createBlock(StructCfMarker::IfElse);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirInstId const p     = mb.addArg(0, ptrI32);
    MirInstId const cIf   = mb.addArg(1, boolT);
    MirInstId const cTrip = mb.addArg(2, boolT);
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirLiteralValue v4; v4.value = std::int64_t{4}; v4.core = TypeKind::I32;
    MirInstId const a = mb.addConst(v3, i32);
    MirInstId const b = mb.addConst(v4, i32);
    mb.addCondBr(cIf, header, skip);
    mb.beginBlock(header);
    MirInstId const lops[] = {p};
    (void)mb.addInst(MirOpcode::Load, lops, i32);
    // A cannot-fault invariant in the SAME block: speculating an Add into a
    // preheader that may skip the loop is harmless, so this one must still
    // move. Without it, "hoisted == 0" would also be satisfied by a pass
    // that gave up on the whole loop.
    MirInstId const addOps[] = {a, b};
    (void)mb.addInst(MirOpcode::Add, addOps, i32);
    mb.addCondBr(cTrip, body, exitB);
    mb.beginBlock(body);
    mb.addBr(header);
    mb.beginBlock(skip);
    mb.addBr(exitB);
    mb.beginBlock(exitB);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 1u)
        << "the cannot-fault Add hoists; the Load must not, because "
           "executing this preheader does not imply entering the loop";
    EXPECT_EQ(countOpIn(mir, StructCfMarker::EntryBlock, MirOpcode::Add), 1u)
        << "the Add must still be hoisted into the preheader";
    EXPECT_EQ(countLoadsOfArgIn(mir, StructCfMarker::EntryBlock, 0u), 0u)
        << "the Load must NOT be hoisted into a preheader that can branch "
           "past the loop — `if (c) for (…) x = *p;` with c == false";
    EXPECT_EQ(countLoadsOfArgIn(mir, StructCfMarker::LoopHeader, 0u), 1u)
        << "the Load must still be physically present in the header";
}

// ★ THE UNIFICATION PIN — the other polarity of TrapEligibleSDivNotHoisted.
// A division in the HEADER runs on the first iteration of any entered loop,
// so hoisting it moves a fault that was going to happen anyway (and C makes
// division by zero undefined either way). `mayFaultWhenSpeculated` therefore
// owns Load and the divisions with ONE rule; this pin is what proves the
// division arm goes through the guaranteed-to-execute gate rather than a
// second, parallel opcode veto.
//
// RED-ON-DISABLE (executed): restore the blanket
// `if (mayFaultWhenSpeculated(op)) return false;` inside
// isLicmCandidateOpcode → instructionsHoisted drops to 0 here (and
// MayFaultLoadHoistedFromGuaranteedNonHeaderBlock also goes red), proving
// the two families really do share one gate.
TEST(Licm, TrapEligibleSDivHoistedWhenGuaranteedToExecute) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {i32, boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirLiteralValue v100; v100.value = std::int64_t{100}; v100.core = TypeKind::I32;
    MirInstId const num   = mb.addConst(v100, i32);
    MirInstId const d     = mb.addArg(0, i32);
    MirInstId const cTrip = mb.addArg(1, boolT);
    mb.addBr(header);
    mb.beginBlock(header);
    MirInstId const divOps[] = {num, d};
    (void)mb.addInst(MirOpcode::SDiv, divOps, i32);
    mb.addCondBr(cTrip, body, exitB);
    mb.beginBlock(body);
    mb.addBr(header);
    mb.beginBlock(exitB);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 1u)
        << "a division in the loop header executes on the first iteration "
           "of any entered loop, so the hoist is behaviour-preserving — "
           "Load and the divisions share ONE may-fault gate";
    EXPECT_EQ(countOpIn(mir, StructCfMarker::EntryBlock, MirOpcode::SDiv), 1u)
        << "the hoisted SDiv must land in the preheader";
}

// A Call in the loop may never return (exit / abort / longjmp / a nested
// infinite loop). Control then never reaches the loop's exit, so the
// dominance argument — "control left through E and B dominates E, therefore
// B ran" — proves nothing, and even a HEADER may-fault op must stay.
// The pin uses a DIVISION deliberately: a Load would be refused anyway
// because Call sits in `opcodeClobbersMemory`, which would make this pin
// inert. The division is the only arm the condition actually decides.
//
// RED-ON-DISABLE (executed): remove `bodyHasCall` from
// `blockRunsOnEveryLoopEntry`'s conjunction → instructionsHoisted becomes 1
// and the SDiv is hoisted above a call that may never return.
TEST(Licm, MayFaultSDivNotHoistedWhenLoopBodyContainsACall) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const calleeSig = interner.fnSig({}, voidT, CallConv::CcSysV);
    TypeId const params[] = {i32, boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    // The callee — stands in for `exit()`: LICM cannot know it returns.
    mb.addFunction(calleeSig, SymbolId{50});
    MirBlockId const cEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(cEntry);
    mb.addReturn();

    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirLiteralValue v100; v100.value = std::int64_t{100}; v100.core = TypeKind::I32;
    MirInstId const num    = mb.addConst(v100, i32);
    MirInstId const d      = mb.addArg(0, i32);
    MirInstId const cTrip  = mb.addArg(1, boolT);
    MirInstId const callee = mb.addGlobalAddr(SymbolId{50}, calleeSig);
    mb.addBr(header);
    mb.beginBlock(header);
    MirInstId const callOps[] = {callee};
    (void)mb.addInst(MirOpcode::Call, callOps, voidT);
    MirInstId const divOps[] = {num, d};
    (void)mb.addInst(MirOpcode::SDiv, divOps, i32);
    mb.addCondBr(cTrip, body, exitB);
    mb.beginBlock(body);
    mb.addBr(header);
    mb.beginBlock(exitB);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 0u)
        << "a Call in the loop may never return, so nothing after it is "
           "guaranteed to execute — the division must stay in the loop";
    EXPECT_EQ(countOpIn(mir, StructCfMarker::EntryBlock, MirOpcode::SDiv), 0u)
        << "no SDiv may reach the preheader";
    EXPECT_EQ(countOpIn(mir, StructCfMarker::LoopHeader, MirOpcode::SDiv), 1u)
        << "the SDiv must still be physically present in the header";
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
    // ★ The Load lives in the HEADER, not in `body`. Entering a loop IS
    // executing its header, so the may-fault speculation gate
    // (D-OPT6-LICM-SPECULATIVE-LOAD-HOIST) is satisfied and the ALIAS
    // decision is the only live variable here. In the conditionally-
    // executed `body` block — where this fixture used to put it — the
    // speculation gate refuses FIRST and the test would decide nothing
    // about aliasing. Do not move it back.
    MirInstId const lops[] = {slot};
    (void)mb.addInst(MirOpcode::Load, lops, i32);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
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
        // Load in the HEADER (guaranteed to execute on loop entry) so the
        // may-fault speculation gate is satisfied in BOTH functions and
        // the per-function ALIAS decision is the only live variable —
        // see InvariantLoadHoisted for the full note.
        MirInstId const lops[] = {slot};
        (void)mb.addInst(MirOpcode::Load, lops, i32);
        MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
        MirInstId const cond = mb.addConst(tru, boolT);
        mb.addCondBr(cond, body, exitB);
        mb.beginBlock(body);
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
    // Load in the HEADER (guaranteed to execute on loop entry) so the
    // may-fault speculation gate is satisfied and the ALIASING STORE is
    // the only live variable — see InvariantLoadHoisted for the note.
    MirInstId const lops[] = {slot};
    (void)mb.addInst(MirOpcode::Load, lops, i32);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
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
    MirInstId const lops[] = {pI32};
    (void)mb.addInst(MirOpcode::Load, lops, i32);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
    // NOTE the Load is emitted into the HEADER above, not here — it is
    // guaranteed to execute on loop entry, so the may-fault speculation
    // gate is satisfied and strict-TBAA is the only live variable (see
    // InvariantLoadHoisted).
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
    // Load in the HEADER (guaranteed to execute on loop entry) so the
    // may-fault speculation gate is satisfied and the ALIASING MODE is
    // the only live variable — see InvariantLoadHoisted for the note.
    MirInstId const lops[] = {pI32};
    (void)mb.addInst(MirOpcode::Load, lops, i32);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
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
    // Load in the HEADER (guaranteed to execute on loop entry) so the
    // may-fault speculation gate is satisfied and the CHAR EXCEPTION is
    // the only live variable — see InvariantLoadHoisted for the note.
    MirInstId const lops[] = {pCh};
    (void)mb.addInst(MirOpcode::Load, lops, charT);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
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
    // Load in the HEADER (guaranteed to execute on loop entry) so the
    // may-fault speculation gate is satisfied and the CHAR EXCEPTION is
    // the only live variable — see InvariantLoadHoisted for the note.
    MirInstId const lops[] = {pCh};
    (void)mb.addInst(MirOpcode::Load, lops, charT);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
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
    // Identical to InvariantLoadHoisted's HEADER Load EXCEPT the Volatile
    // flag — same block (guaranteed to execute on loop entry, so the
    // may-fault speculation gate is satisfied), loop-invariant pointer
    // `slot`, no aliasing Store in the loop, so ONLY the Volatile bit can
    // stop the hoist.
    MirInstId const lops[] = {slot};
    (void)mb.addInst(MirOpcode::Load, lops, i32, /*payload*/0,
                     MirInstFlags::Volatile);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
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

    // The volatile Load must remain PHYSICALLY in the loop (LoopHeader)
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
                if (mrk == StructCfMarker::LoopHeader &&
                    has(mir.instFlags(id), MirInstFlags::Volatile)) {
                    ++volLoadsInBody;
                }
                if (mrk == StructCfMarker::EntryBlock) ++loadsInPreheader;
            }
        }
    }
    EXPECT_EQ(volLoadsInBody, 1u)
        << "the volatile Load must still live inside the loop (LoopHeader), "
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
    mb.addGlobal(i32, SymbolId{200}, UINT32_MAX, initFn,
                 SymbolBinding::Global, SymbolVisibility::Default,
                 /*isConst=*/false, MirThreadStorage::Shared);
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
