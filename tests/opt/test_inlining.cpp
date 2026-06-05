// MIR-tier function INLINING unit tests (OPT7 cycle 1).
//
// Scope (LOCKED): inline a DIRECT call to a SINGLE-BLOCK LEAF callee
// that splices LINEARLY at the call site, governed by the §2.9
// legality gate. The gate's CORRECTNESS rule is: NEVER inline a Weak
// callee (a strong def of the same name may replace it at link →
// inlining the weak body bakes in the wrong one = silent miscompile).
//
// Pins (each RED-on-disable for the gate it exercises):
//   * Weak callee → call NOT inlined (Call survives; count unchanged).
//     [THE correctness pin — D-OPT7-WEAK-INLINE-NEGATIVE-PIN, MIR tier]
//   * Global callee (leaf) → call IS inlined (Call gone; callee body
//     present in the caller). [NON-VACUOUS — proves the inliner works]
//   * Argument-passing callee (Arg substitution) → inlined; the actual
//     argument flows into the spliced body.
//   * Self-recursive call → NOT inlined.
//   * Address-taken callee → NOT inlined (escape via function pointer).
//   * Multi-block callee → NOT inlined (scope boundary).
//   * Non-leaf callee (callee body contains a Call) → NOT inlined.
//   * IntrinsicCall-bearing single-block callee → NOT inlined ("leaf"
//     means NO call-like op of any kind; deferred relaxation —
//     D-OPT7-INLINE-LEGALITY-GATE).
//   * VOID single-block leaf callee → inlined; the invalid-result-id
//     splice path threads cleanly + the module verifies.
//   * callsInlined counter accuracy + verifier-clean rebuild.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_verifier.hpp"
#include "opt/passes/inlining.hpp"

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

MirLiteralValue i32Lit(std::int64_t v) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = TypeKind::I32;
    return lit;
}

// Build a 2-function module: a nullary leaf callee `f` that returns the
// constant `retVal`, and `main` that calls `f` and returns its result.
// `calleeBinding` controls the §2.9 Weak-vs-Global gate.
Mir buildCallerCalleeModule(TypeInterner& interner, SymbolBinding calleeBinding,
                            std::int64_t retVal = 7) {
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    // callee f (SymbolId 50): single-block leaf returning a constant.
    mb.addFunction(fnSig, SymbolId{50}, calleeBinding, SymbolVisibility::Default);
    MirBlockId const fEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(fEntry);
    mb.addReturn(mb.addConst(i32Lit(retVal), i32));

    // main (SymbolId 100): calls f, returns the call result.
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    MirInstId const calleeAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
    MirInstId const callOps[] = {calleeAddr};
    MirInstId const call = mb.addInst(MirOpcode::Call, callOps, i32);
    mb.addReturn(call);
    return std::move(mb).finish();
}

} // namespace

// ── THE correctness pin: a Weak callee is NOT inlined ──────────────
// cu_a-style: weak f() exists in the module AND main calls it intra-CU.
// The gate must REFUSE (the Call survives). A broken gate would splice
// `return 7` into main — the exact silent miscompile the cross-CU
// corpus catches at exit-code tier (D-OPT7-WEAK-INLINE-NEGATIVE-PIN).
TEST(Inlining, WeakCalleeIsNotInlined) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildCallerCalleeModule(interner, SymbolBinding::Weak, 7);

    auto const callsBefore = countOpInModule(mir, MirOpcode::Call);
    ASSERT_EQ(callsBefore, 1u);

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 0u)
        << "a Weak callee MUST NOT be inlined — a strong def of the same "
           "name may replace it at link (silent miscompile if inlined)";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), callsBefore)
        << "the Call opcode must survive in main";
}

// ── NON-VACUOUS pin: a Global leaf callee IS inlined ───────────────
// Proves the inliner actually inlines, so "refused for weak" is
// meaningful (not a no-op pass that never inlines anything).
TEST(Inlining, GlobalLeafCalleeIsInlined) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildCallerCalleeModule(interner, SymbolBinding::Global, 7);

    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);
    // Before inlining: only f has the `Const 7` (main has zero Const).
    auto const constsBefore = countOpInModule(mir, MirOpcode::Const);
    ASSERT_EQ(constsBefore, 1u) << "before: only f holds the Const 7";

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 1u)
        << "a Global single-block leaf callee MUST be inlined";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 0u)
        << "main's Call must be replaced by the spliced callee body";

    // BODY-PRESENT pin (red-on-disable for the actual splice): f's own
    // body still exists (not deleted — DCE's job) AND a COPY of its
    // `Const 7` was spliced into main → the module-wide Const count
    // INCREASED by exactly one (from 1 to 2). A no-op-that-only-deletes-
    // the-Call would leave the count at 1 and fail here.
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Const), constsBefore + 1)
        << "the callee's Const 7 must be SPLICED into main (body present), "
           "not merely have the Call deleted";

    // The Return-threaded value means main returns the spliced const.
    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep))
        << "the inlined module must pass the MIR verifier";
}

// ── Argument substitution: a callee taking a parameter is inlined,
// and the actual argument flows into the spliced body. ─────────────
// Callee g(int x) { return x + 1; }; main() { return g(41); } → after
// inlining, main computes Const(41) + Const(1) inline (= 42).
TEST(Inlining, ArgumentIsSubstitutedIntoSplicedBody) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32      = interner.primitive(TypeKind::I32);
    TypeId const params[] = {i32};
    TypeId const gSig     = interner.fnSig(params, i32, CallConv::CcSysV);
    TypeId const mainSig  = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    // g(int x): return x + 1
    mb.addFunction(gSig, SymbolId{60});
    MirBlockId const gEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(gEntry);
    MirInstId const x   = mb.addArg(0, i32);
    MirInstId const one = mb.addConst(i32Lit(1), i32);
    MirInstId const addOps[] = {x, one};
    MirInstId const sum = mb.addInst(MirOpcode::Add, addOps, i32);
    mb.addReturn(sum);

    // main(): return g(41)
    mb.addFunction(mainSig, SymbolId{100});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    MirInstId const gAddr = mb.addGlobalAddr(SymbolId{60}, gSig);
    MirInstId const arg41 = mb.addConst(i32Lit(41), i32);
    MirInstId const callOps[] = {gAddr, arg41};
    MirInstId const call = mb.addInst(MirOpcode::Call, callOps, i32);
    mb.addReturn(call);
    Mir mir = std::move(mb).finish();

    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);
    ASSERT_EQ(countOpInModule(mir, MirOpcode::Arg), 1u)
        << "before: only g has the Arg";
    // Before inlining: only g holds the Add (main has none).
    auto const addsBefore = countOpInModule(mir, MirOpcode::Add);
    ASSERT_EQ(addsBefore, 1u) << "before: only g has the Add";

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 1u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 0u);
    // The spliced Add in main references the ACTUAL argument Const(41),
    // NOT a copied Arg — the inliner must NOT re-emit the callee's Arg.
    // g's own Arg still exists in g's (un-deleted) body, so module-wide
    // Arg count is unchanged at 1 (the splice added no new Arg).
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Arg), 1u)
        << "the Arg must be substituted by the actual argument, not "
           "copied into the caller";
    // BODY-PRESENT pin (red-on-disable for the actual splice): g's body
    // computes `x + 1` via an Add; that Add must be SPLICED into main.
    // g's own Add still exists (not deleted — DCE's job) + a copy lands
    // in main → module-wide Add count INCREASED by exactly one (1 → 2).
    // A no-op-that-only-deletes-the-Call would leave it at 1 and fail.
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Add), addsBefore + 1)
        << "the callee's Add (x + 1) must be SPLICED into main (body "
           "present), not merely have the Call deleted";
    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep));
}

// ── Recursion is REFUSED (this cycle only refuses self-recursion) ──
// f() { return f(); } — a self-call. The gate must refuse (callee ==
// caller). Inlining a recursive call without a depth policy would loop.
TEST(Inlining, SelfRecursiveCallIsNotInlined) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    mb.addFunction(fnSig, SymbolId{70});
    MirBlockId const fEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(fEntry);
    MirInstId const selfAddr = mb.addGlobalAddr(SymbolId{70}, fnSig);
    MirInstId const callOps[] = {selfAddr};
    MirInstId const call = mb.addInst(MirOpcode::Call, callOps, i32);
    mb.addReturn(call);
    Mir mir = std::move(mb).finish();

    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 0u)
        << "a self-recursive call MUST NOT be inlined this cycle";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);
}

// ── Address-taken callee is REFUSED ────────────────────────────────
// f() { return 7; }  main() { int(*p)() = f; return f(); }  — f's
// address is ALSO taken (stored to a slot), so an indirect call could
// reach it. The gate refuses to inline EVEN the direct call (the body
// must stay out-of-line, and we keep it conservative). The escape is a
// Store of &f (NOT a Call operand) so no second call confuses the count.
TEST(Inlining, AddressTakenCalleeIsNotInlined) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    TypeId const ptr   = interner.pointer(fnSig);
    MirBuilder mb;

    // f (SymbolId 50): leaf returning 7.
    mb.addFunction(fnSig, SymbolId{50});
    MirBlockId const fEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(fEntry);
    mb.addReturn(mb.addConst(i32Lit(7), i32));

    // main (SymbolId 100): int(**slot)() = &f; return f();
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    // f's address ESCAPES: stored into a stack slot (Store operand[0]).
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirInstId const fAddrEscape = mb.addGlobalAddr(SymbolId{50}, ptr);
    MirInstId const storeOps[] = {fAddrEscape, slot};
    (void)mb.addInst(MirOpcode::Store, storeOps, InvalidType);
    // The direct call we'd otherwise inline.
    MirInstId const fAddrCall = mb.addGlobalAddr(SymbolId{50}, fnSig);
    MirInstId const callOps[] = {fAddrCall};
    MirInstId const call = mb.addInst(MirOpcode::Call, callOps, i32);
    mb.addReturn(call);
    Mir mir = std::move(mb).finish();

    auto const callsBefore = countOpInModule(mir, MirOpcode::Call);
    ASSERT_EQ(callsBefore, 1u);  // only f()

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 0u)
        << "f's address escapes (stored to a slot) — its direct call MUST "
           "NOT be inlined (an indirect call could reach f)";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), callsBefore);
}

// ── Multi-block callee is NOT inlined (scope boundary) ─────────────
// f() with two blocks (a CondBr diamond) is beyond the single-block
// minimal splice. The gate refuses; general multi-block splice is
// deferred (D-OPT7-MULTIBLOCK-SPLICE).
TEST(Inlining, MultiBlockCalleeIsNotInlined) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    // f (SymbolId 50): entry → CondBr to two blocks that each return.
    mb.addFunction(fnSig, SymbolId{50});
    MirBlockId const fEntry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const fThen  = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const fElse  = mb.createBlock(StructCfMarker::IfElse);
    mb.beginBlock(fEntry);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, fThen, fElse);
    mb.beginBlock(fThen);
    mb.addReturn(mb.addConst(i32Lit(1), i32));
    mb.beginBlock(fElse);
    mb.addReturn(mb.addConst(i32Lit(2), i32));

    // main (SymbolId 100): return f();
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    MirInstId const fAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
    MirInstId const callOps[] = {fAddr};
    MirInstId const call = mb.addInst(MirOpcode::Call, callOps, i32);
    mb.addReturn(call);
    Mir mir = std::move(mb).finish();

    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 0u)
        << "a multi-block callee is beyond the single-block splice scope";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);
}

// ── Non-leaf callee (its body contains a Call) is NOT inlined ──────
// g() { return 9; }  f() { return g(); }  main() { return f(); }
// f is single-block but NOT a leaf (it calls g) → f's call from main is
// refused. (g's call from f IS a leaf call and gets inlined into f,
// which is fine; the pin asserts main's Call to f survives.)
TEST(Inlining, NonLeafCalleeIsNotInlined) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    // g (SymbolId 40): leaf returning 9.
    mb.addFunction(fnSig, SymbolId{40});
    MirBlockId const gEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(gEntry);
    mb.addReturn(mb.addConst(i32Lit(9), i32));

    // f (SymbolId 50): single block, calls g → NOT a leaf.
    mb.addFunction(fnSig, SymbolId{50});
    MirBlockId const fEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(fEntry);
    MirInstId const gAddr = mb.addGlobalAddr(SymbolId{40}, fnSig);
    MirInstId const gCallOps[] = {gAddr};
    MirInstId const gCall = mb.addInst(MirOpcode::Call, gCallOps, i32);
    mb.addReturn(gCall);

    // main (SymbolId 100): return f();
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    MirInstId const fAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
    MirInstId const fCallOps[] = {fAddr};
    MirInstId const fCall = mb.addInst(MirOpcode::Call, fCallOps, i32);
    mb.addReturn(fCall);
    Mir mir = std::move(mb).finish();

    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 2u);  // f→g + main→f

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    // g's call inside f IS a leaf call → inlined (callsInlined == 1).
    // main's call to f is NOT a leaf → refused. So exactly ONE Call
    // remains module-wide (main→f), and it is main's.
    EXPECT_EQ(r.callsInlined, 1u)
        << "g's leaf call in f is inlined; main's call to non-leaf f is not";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 1u)
        << "main's Call to the non-leaf f must survive";

    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep));
}

// ── IntrinsicCall-bearing callee is NOT inlined ("leaf" = no call-like
// op of ANY kind) ──────────────────────────────────────────────────
// f() { return some_intrinsic(5); }  main() { return f(); }
// f is single-block, has NO `Call`, but DOES contain an `IntrinsicCall`
// (a distinct side-effecting call-like opcode). For the minimal
// correctness-first OPT7 cycle, "leaf" means NO call-like op of any
// kind, so the gate REFUSES f → main's Call to f survives. (Inlining it
// would be SSA-correct — the intrinsic id is module-stable + correctly
// remapped — so this is a conservative scope-narrowing whose relaxation
// is deferred: D-OPT7-INLINE-LEGALITY-GATE.)
// RED-on-disable: removing the `IntrinsicCall` arm in inlineLegalityGate
// admits f → main's Call is spliced away → callsInlined becomes 1 and
// the surviving-Call assert fails.
TEST(Inlining, IntrinsicCalleeIsNotInlined) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    // f (SymbolId 55): single block, body computes an IntrinsicCall.
    // Built exactly as HIR→MIR lowers an intrinsic call: operands are
    // the args, the intrinsic id lives in the payload (here an arbitrary
    // id — the MIR verifier does not validate it against any registry).
    mb.addFunction(fnSig, SymbolId{55});
    MirBlockId const fEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(fEntry);
    MirInstId const five = mb.addConst(i32Lit(5), i32);
    MirInstId const intrinOps[] = {five};
    constexpr std::uint32_t kIntrinsicId = 1;  // arbitrary, non-registry
    MirInstId const intrin =
        mb.addInst(MirOpcode::IntrinsicCall, intrinOps, i32, kIntrinsicId);
    mb.addReturn(intrin);

    // main (SymbolId 100): return f();
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    MirInstId const fAddr = mb.addGlobalAddr(SymbolId{55}, fnSig);
    MirInstId const callOps[] = {fAddr};
    MirInstId const call = mb.addInst(MirOpcode::Call, callOps, i32);
    mb.addReturn(call);
    Mir mir = std::move(mb).finish();

    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);          // main→f
    ASSERT_EQ(countOpInModule(mir, MirOpcode::IntrinsicCall), 1u);  // in f

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 0u)
        << "a single-block callee containing an IntrinsicCall is NOT a leaf "
           "this cycle — its call MUST NOT be inlined";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 1u)
        << "main's Call to the IntrinsicCall-bearing f must survive";

    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep));
}

// ── VOID single-block leaf callee IS inlined (invalid-result-id path) ─
// g() { volatile-free body; return; }  (void, no return value)
// main() { g(); return 0; }  (g's "result" is unused)
// Exercises the VOID-callee splice path: the callee's bare `Return` has
// no value, so spliceCallee threads an INVALID MirInstId as the Call's
// result. That invalid id must not break the rebuild or the verifier,
// and the callee's real body instruction must still be spliced in.
TEST(Inlining, VoidLeafCalleeIsInlined) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32     = interner.primitive(TypeKind::I32);
    TypeId const voidSig = interner.fnSig({}, InvalidType, CallConv::CcSysV);
    TypeId const mainSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    // g (SymbolId 80): VOID single-block leaf. Real (non-terminator)
    // body instruction — a Const — that the splice must copy, then a
    // bare void Return (no value).
    mb.addFunction(voidSig, SymbolId{80}, SymbolBinding::Global,
                   SymbolVisibility::Default);
    MirBlockId const gEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(gEntry);
    (void)mb.addConst(i32Lit(99), i32);  // real body inst to splice
    mb.addReturn();                       // bare void Return — no value

    // main (SymbolId 100): g(); return 0;  (the void Call's result is
    // unused — it is NOT fed to main's Return).
    mb.addFunction(mainSig, SymbolId{100});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    MirInstId const gAddr = mb.addGlobalAddr(SymbolId{80}, voidSig);
    MirInstId const callOps[] = {gAddr};
    (void)mb.addInst(MirOpcode::Call, callOps, InvalidType);  // void call
    mb.addReturn(mb.addConst(i32Lit(0), i32));
    Mir mir = std::move(mb).finish();

    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);
    // Before: g holds Const 99; main holds Const 0 → 2 Consts module-wide.
    auto const constsBefore = countOpInModule(mir, MirOpcode::Const);
    ASSERT_EQ(constsBefore, 2u);

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 1u)
        << "a void single-block leaf callee MUST be inlined";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 0u)
        << "main's void Call to g must be replaced by the spliced body";
    // BODY-PRESENT pin: g's `Const 99` is spliced into main (g's own copy
    // still exists) → module-wide Const count INCREASED by exactly one.
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Const), constsBefore + 1)
        << "the void callee's body (Const 99) must be SPLICED into main";

    // The invalid-result-id (void Return) splice path must not break the
    // verifier — main returns its own Const 0, never the void call result.
    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep))
        << "the void-callee splice (invalid result id) must verify clean";
}
