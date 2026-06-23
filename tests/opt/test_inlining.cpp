// MIR-tier function INLINING unit tests (OPT7 cycles 1-2).
//
// Scope (LOCKED): inline a DIRECT call to a LEAF callee — SINGLE-BLOCK
// (cycle 1, LINEAR splice at the call site) OR MULTI-BLOCK (cycle 2,
// CFG-clone + call-site-block split + return-merge Phi). Governed by
// the §2.9 legality gate. The gate's CORRECTNESS rule is: NEVER inline
// a Weak callee (a strong def of the same name may replace it at link →
// inlining the weak body bakes in the wrong one = silent miscompile).
//
// Pins (each RED-on-disable for the gate / machinery it exercises):
//   * Weak callee → call NOT inlined (Call survives; count unchanged).
//     [THE correctness pin — D-OPT7-WEAK-INLINE-NEGATIVE-PIN, MIR tier]
//   * Global callee (leaf) → call IS inlined (Call gone; callee body
//     present in the caller). [NON-VACUOUS — proves the inliner works]
//   * Argument-passing callee (Arg substitution) → inlined; the actual
//     argument flows into the spliced body.
//   * Self-recursive call → NOT inlined.
//   * Address-taken callee → NOT inlined (escape via function pointer).
//   * MULTI-BLOCK LEAF callee (two returns → diamond) → IS inlined
//     (cycle 2): Call gone, callee blocks cloned into the caller
//     (block count rises), a return-merge Phi joins the two return
//     paths, MirVerifier clean. [THE cycle-2 deliverable]
//   * Multi-block NON-leaf callee (a cloned block contains a Call) →
//     IS inlined (OPT7 cycle 3 lifts the leaf restriction; the inner
//     Call clones correctly + recursion is caught by the SCC gate).
//   * Non-leaf single-block callee (body contains a Call) → IS inlined
//     (OPT7 cycle 3): both levels of a g←f←main chain flatten.
//   * MUTUAL recursion (f→g→f) → NOT inlined (the call-graph SCC gate
//     refuses every call within a recursive cycle — generalizing the
//     self-recursion refusal); an ACYCLIC non-leaf control IS inlined
//     (proving the refusal is recursion-specific, not refuse-all).
//   * IntrinsicCall-bearing callee → INLINED (OPT7 cycle 6): the
//     intrinsic clones SSA-correctly via the generic arm (payload-carried
//     id copied verbatim) — proven for the value-threaded single-block,
//     the multi-block (non-entry block), and the void (result-unused)
//     forms. Frame-sensitive intrinsics stay trigger-gated
//     (D-OPT7-INLINE-FRAME-SENSITIVE-INTRINSIC).
//   * VOID single-block leaf callee → inlined; the invalid-result-id
//     splice path threads cleanly + the module verifies.
//   * callsInlined counter accuracy + verifier-clean rebuild.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_verifier.hpp"
#include "opt/optimizer.hpp"
#include "opt/passes/inlining.hpp"
#include "diagnostic_count.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

using dss::test_support::countCode;

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

// Total basic-block count across every function in the module — used to
// prove the multi-block splice CLONED the callee's blocks into the
// caller (the count rises by the cloned-block + continuation count).
std::size_t blockCountInModule(Mir const& mir) {
    std::size_t n = 0;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        n += mir.funcBlockCount(mir.funcAt(i));
    }
    return n;
}

// Block count of a single function identified by its symbol id — used
// to assert the CALLER (`main`) specifically grew its block count.
std::size_t funcBlockCountBySymbol(Mir const& mir, std::uint32_t sym) {
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        if (mir.funcSymbol(f).v == sym) return mir.funcBlockCount(f);
    }
    return 0;
}

// Count of a given opcode WITHIN one function identified by its symbol
// id. Used by the mutual-recursion pin to assert the f↔g recursive
// edges survive INSIDE f and INSIDE g specifically — stable regardless
// of whether an unrelated (non-recursive) caller inlined f's body
// elsewhere.
std::size_t countOpInFuncBySymbol(Mir const& mir, std::uint32_t sym,
                                  MirOpcode want) {
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        if (mir.funcSymbol(f).v != sym) continue;
        std::size_t n = 0;
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t i2 = 0; i2 < ni; ++i2) {
                if (mir.instOpcode(mir.blockInstAt(b, i2)) == want) ++n;
            }
        }
        return n;
    }
    return 0;
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
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
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
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
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

// ── FC7 C1c/C3: frame/ABI-sensitive callees are REFUSED at the gate ──
// (D-FC7-INLINE-MULTI-PIECE-RETURN — closed via the legality-gate refusal.)
// A callee that returns a struct IN REGISTERS lowers to a MULTI-piece
// `Return` (N>1 operands). The single-block splice paths clone a Return
// taking only operand 0 — truncating the struct to its first field. The
// legality gate MUST refuse (the Call survives). RED-ON-DISABLE: drop the
// `instOperands(cid).size() > 1` gate refusal in inlineLegalityGate and this
// callee inlines (callsInlined == 1) → the splice silently drops fields
// 1..N-1. Reachable under the shipped release.pipeline.json (runs Inlining).
TEST(Inlining, MultiPieceStructReturningCalleeIsNotInlined) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i64    = interner.primitive(TypeKind::I64);
    std::array<TypeId, 2> fields{i64, i64};
    TypeId const pairTy = interner.structType("Pair", fields);
    TypeId const fnSig  = interner.fnSig({}, pairTy, CallConv::CcSysV);
    MirBuilder mb;
    // callee f (SymbolId 50): single-block leaf returning {11, 22} as TWO pieces.
    mb.addFunction(fnSig, SymbolId{50});
    MirBlockId const fEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(fEntry);
    MirInstId const p0 = mb.addConst(i32Lit(11), i64);
    MirInstId const p1 = mb.addConst(i32Lit(22), i64);
    std::array<MirInstId, 2> pieces{p0, p1};
    mb.addReturnMulti(pieces);
    // main (SymbolId 100): calls f.
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    MirInstId const calleeAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
    std::array<MirInstId, 1> callOps{calleeAddr};
    MirInstId const call = mb.addInst(MirOpcode::Call, callOps, pairTy);
    mb.addReturnMulti(std::array<MirInstId, 1>{call});
    Mir mir = std::move(mb).finish();

    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);
    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 0u)
        << "a callee returning a struct IN REGISTERS (a multi-piece Return) MUST "
           "NOT be inlined — the splice would truncate the Return to field 0";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 1u)
        << "the Call must survive (refused, not spliced)";
}

// An x8-sret callee (>16B struct return under AAPCS64) reads its incoming
// result pointer via `ReadIndirectResult` at ENTRY — a frame-sensitive op
// that reads the CALLEE's indirect-result register. Inlining would splice it
// into the CALLER's frame, reading the caller's (uninitialized) x8. The gate
// MUST refuse. RED-ON-DISABLE: drop the `ReadIndirectResult` gate refusal and
// this callee inlines → the spliced read binds to the wrong frame.
TEST(Inlining, X8SretCalleeWithReadIndirectResultIsNotInlined) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, ptr, CallConv::CcSysV);
    MirBuilder mb;
    // callee f (SymbolId 50): reads the indirect-result register at entry,
    // returns it (the x8-sret callee shape, modeled with a ptr result here).
    mb.addFunction(fnSig, SymbolId{50});
    MirBlockId const fEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(fEntry);
    MirInstId const irr = mb.addReadIndirectResult(ptr);
    mb.addReturn(irr);
    // main (SymbolId 100): calls f.
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    MirInstId const calleeAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
    std::array<MirInstId, 1> callOps{calleeAddr};
    MirInstId const call = mb.addInst(MirOpcode::Call, callOps, ptr);
    mb.addReturn(call);
    Mir mir = std::move(mb).finish();

    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);
    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 0u)
        << "an x8-sret callee (ReadIndirectResult at entry) MUST NOT be inlined — "
           "the spliced read would bind to the caller's frame, not the callee's";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);
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
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
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
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
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
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 0u)
        << "f's address escapes (stored to a slot) — its direct call MUST "
           "NOT be inlined (an indirect call could reach f)";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), callsBefore);
}

// ── THE cycle-2 deliverable: a MULTI-BLOCK LEAF callee IS inlined ──
// pick() { if (cond) return 7; else return 9; } — entry CondBr to two
// return blocks (a diamond, NO callee Phi: each arm returns directly).
// main() { return pick(); }. The cycle-2 machinery splices pick:
//   * main's call-site block is SPLIT (Call removed);
//   * pick's 3 blocks are CLONED into main (fresh block ids);
//   * each callee Return becomes a Br to a CONTINUATION block;
//   * a RETURN-MERGE PHI in the continuation joins (7, then-clone) +
//     (9, else-clone); that Phi is the Call's result → main returns it.
// Asserts: Call gone; callsInlined == 1; main's block count ROSE
// (clone happened — red-on-disable for "blocks cloned"); a Phi now
// exists (the merge — red-on-disable for "return-merge wired"); both
// return-Const values were spliced; MirVerifier clean.
TEST(Inlining, MultiBlockLeafCalleeIsInlined) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    // pick (SymbolId 50): entry → CondBr to two blocks that each return.
    // Markers are derivation-consistent (both arms return → ipdom is the
    // virtual exit → IfThen/IfElse, no join) so the un-inlined `pick`
    // is verifier-valid; the post-inline re-derivation re-stamps every
    // block from the spliced CFG regardless.
    mb.addFunction(fnSig, SymbolId{50});
    MirBlockId const fEntry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const fThen  = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const fElse  = mb.createBlock(StructCfMarker::IfElse);
    mb.beginBlock(fEntry);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, fThen, fElse);
    mb.beginBlock(fThen);
    mb.addReturn(mb.addConst(i32Lit(7), i32));
    mb.beginBlock(fElse);
    mb.addReturn(mb.addConst(i32Lit(9), i32));

    // main (SymbolId 100): return pick();
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    MirInstId const fAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
    MirInstId const callOps[] = {fAddr};
    MirInstId const call = mb.addInst(MirOpcode::Call, callOps, i32);
    mb.addReturn(call);
    Mir mir = std::move(mb).finish();

    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);
    ASSERT_EQ(countOpInModule(mir, MirOpcode::Phi), 0u)
        << "before: no Phi anywhere (pick has none; main has none)";
    auto const mainBlocksBefore = funcBlockCountBySymbol(mir, 100);
    ASSERT_EQ(mainBlocksBefore, 1u) << "before: main is a single block";
    auto const totalBlocksBefore = blockCountInModule(mir);

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 1u)
        << "a multi-block LEAF callee MUST be inlined (cycle 2)";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 0u)
        << "main's Call must be replaced by the spliced multi-block body";

    // (b) The callee's blocks were CLONED into main: main's block count
    // rose from 1 to (entry-remainder + 3 clones + 1 continuation) = 5,
    // and the module total rose by the same 4. Red-on-disable for "the
    // CFG was cloned" (a no-op-that-only-deletes would leave it at 1).
    EXPECT_EQ(funcBlockCountBySymbol(mir, 100), mainBlocksBefore + 4)
        << "pick's 3 blocks + 1 continuation must be cloned into main";
    EXPECT_EQ(blockCountInModule(mir), totalBlocksBefore + 4)
        << "module-wide block count rises by the cloned + continuation blocks";

    // (c) A return-merge Phi now exists (joins the two return paths).
    // Red-on-disable for "the merge Phi was wired".
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Phi), 1u)
        << "the two return paths must merge via exactly one Phi";

    // (c2) The merge Phi's incomings are CORRECTLY value↔pred paired —
    // each return-value operand flows from the SAME cloned block whose
    // Return produced it. `Const 7` came from pick's then-arm, `Const 9`
    // from pick's else-arm; after the splice each lives in its own cloned
    // predecessor block, and the rewritten Return→Br makes that block the
    // Phi incoming's `pred`. The load-bearing invariant is therefore
    // `instBlock(incoming.value) == incoming.pred` for BOTH edges: the
    // value an edge carries must be defined in that edge's source block.
    // A swapped mis-merge (7 attributed to the else-clone, 9 to the
    // then-clone) breaks this pairing → RED. This catches a swap the MIR
    // tier would otherwise miss (pick's cond is constant → still verifies
    // even if the two incomings are transposed).
    {
        MirFuncId mainFn{};
        std::size_t const nf = mir.moduleFuncCount();
        for (std::uint32_t i = 0; i < nf; ++i) {
            if (mir.funcSymbol(mir.funcAt(i)).v == 100u) mainFn = mir.funcAt(i);
        }
        ASSERT_TRUE(mainFn.valid()) << "main (sym 100) must exist post-splice";

        MirInstId mergePhi{};
        std::uint32_t const nb = mir.funcBlockCount(mainFn);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(mainFn, bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t i = 0; i < ni; ++i) {
                MirInstId const id = mir.blockInstAt(b, i);
                if (mir.instOpcode(id) == MirOpcode::Phi) mergePhi = id;
            }
        }
        ASSERT_TRUE(mergePhi.valid()) << "the return-merge Phi must be in main";

        auto const incs = mir.phiIncomings(mergePhi);
        ASSERT_EQ(incs.size(), 2u) << "the two return paths → two incomings";

        std::int64_t valFromThen = 0;  // value on the edge whose value==7
        std::int64_t valFromElse = 0;  // value on the edge whose value==9
        bool sawSeven = false, sawNine = false;
        for (MirPhiIncoming const& inc : incs) {
            // Each incoming value must be one of pick's two return Consts.
            ASSERT_EQ(mir.instOpcode(inc.value), MirOpcode::Const)
                << "each merge-Phi incoming value must be a spliced Const";
            std::int64_t const v = std::get<std::int64_t>(
                mir.literalValue(mir.constLiteralIndex(inc.value)).value);
            // THE PAIRING INVARIANT: the value is defined in its own pred.
            EXPECT_EQ(mir.instBlock(inc.value).v, inc.pred.v)
                << "merge-Phi incoming value " << v << " must be defined in "
                   "its OWN predecessor block (a swapped incoming breaks "
                   "this) — value↔pred mis-pairing";
            if (v == 7) { sawSeven = true; valFromThen = v; }
            else if (v == 9) { sawNine = true; valFromElse = v; }
            else ADD_FAILURE() << "unexpected merge-Phi incoming value " << v;
        }
        EXPECT_TRUE(sawSeven) << "the then-arm's Const 7 must be a Phi incoming";
        EXPECT_TRUE(sawNine) << "the else-arm's Const 9 must be a Phi incoming";
        // The two incomings carry DISTINCT predecessor blocks (the two
        // cloned return arms), never a single block feeding both values.
        EXPECT_NE(incs[0].pred.v, incs[1].pred.v)
            << "the two return paths must arrive from distinct cloned blocks";
        EXPECT_EQ(valFromThen, 7);
        EXPECT_EQ(valFromElse, 9);
    }

    // Both return-Const values (7 + 9) were spliced into main; pick's
    // own copies still exist (not deleted — DCE's job). Module-wide
    // Const count: pick had {true-cond, 7, 9} = 3; main had 0. After
    // splice main gains {true-cond, 7, 9} = 3 more → 6.
    // (We assert the two distinct return values are present by checking
    // the Const count rose by pick's whole const set.)
    // pick's consts: cond(bool) + 7 + 9 = 3.
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Const), 6u)
        << "pick's body consts (cond, 7, 9) must be cloned into main";

    // (d) MirVerifier clean — SSA dominance + Phi-incoming completeness
    // + valid CFG edges + marker parity all hold on the spliced result.
    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep))
        << "the multi-block-spliced module must pass the MIR verifier";
}

// ── Multi-block NON-leaf callee IS inlined (OPT7 c3) ───────────────
// h() { return 11; }  g() { if (c) return h(); else return 5; }
// main() { return g(); }. g is MULTI-BLOCK and NON-LEAF (a cloned block
// contains a Call to h). OPT7 cycle 3 LIFTS the leaf restriction across
// ALL blocks, so main's call to the multi-block g is now ADMITTED and
// spliced via the CFG-clone machinery. A SINGLE pass inlines BOTH h→g
// AND g→main (callsInlined == 2); main inlines g's SOURCE body, so its
// cloned then-arm holds a residual Call-to-h until the next iteration.
// A SECOND iteration (the real engine, maxIterations≥2) inlines that →
// module-wide Call == 0.
// RED-on-disable: re-adding the all-blocks `Call`-leaf refusal in
// inlineLegalityGate refuses g → main's Call to g is NOT spliced
// (callsInlined drops to 1, and main's Call survives) → the assertions
// fail.
TEST(Inlining, MultiBlockNonLeafCalleeIsInlined) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& target = **targetR;

    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    // `Mir` is move-only, so build a FRESH module per observation stage.
    auto buildModule = [&] {
        MirBuilder mb;
        // h (SymbolId 30): single-block leaf returning 11.
        mb.addFunction(fnSig, SymbolId{30});
        MirBlockId const hEntry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(hEntry);
        mb.addReturn(mb.addConst(i32Lit(11), i32));
        // g (SymbolId 50): multi-block; then-arm CALLS h → NON-LEAF (now
        // admitted). Derivation-consistent markers (both arms return →
        // IfThen/IfElse around the virtual exit) so g stays verifier-
        // valid whether or not h is inlined.
        mb.addFunction(fnSig, SymbolId{50});
        MirBlockId const gEntry = mb.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const gThen  = mb.createBlock(StructCfMarker::IfThen);
        MirBlockId const gElse  = mb.createBlock(StructCfMarker::IfElse);
        mb.beginBlock(gEntry);
        MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
        MirInstId const gcond = mb.addConst(tru, boolT);
        mb.addCondBr(gcond, gThen, gElse);
        mb.beginBlock(gThen);
        MirInstId const hAddr = mb.addGlobalAddr(SymbolId{30}, fnSig);
        MirInstId const hCallOps[] = {hAddr};
        (void)mb.addInst(MirOpcode::Call, hCallOps, i32);
        mb.addReturn(mb.addConst(i32Lit(11), i32));
        mb.beginBlock(gElse);
        mb.addReturn(mb.addConst(i32Lit(5), i32));
        // main (SymbolId 100): return g();
        mb.addFunction(fnSig, SymbolId{100});
        MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(mEntry);
        MirInstId const gAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
        MirInstId const gCallOps[] = {gAddr};
        MirInstId const gCall = mb.addInst(MirOpcode::Call, gCallOps, i32);
        mb.addReturn(gCall);
        return std::move(mb).finish();
    };

    // (1) ONE pass inlines BOTH h→g AND the NON-LEAF multi-block g→main.
    {
        Mir mir = buildModule();
        ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 2u);  // g→h + main→g
        DiagnosticReporter rep;
        auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
        EXPECT_TRUE(r.ok);
        EXPECT_EQ(r.callsInlined, 2u)
            << "OPT7 c3: BOTH h→g AND the multi-block NON-leaf g→main inline "
               "(the old gate refused g→main, giving 1)";
        // main no longer calls g; it holds the residual cloned call-to-h
        // (spliced in from g's then-arm) until the next iteration.
        EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 1u)
            << "after one pass main holds the residual cloned call-to-h "
               "(g itself is fully inlined into main)";
        MirVerifier verifier{mir, &interner};
        EXPECT_TRUE(verifier.verify(rep))
            << "the multi-block NON-leaf splice must verify clean";
    }

    // (2) Two iterations flatten g→main→h entirely → Call == 0.
    {
        Mir mir = buildModule();
        DiagnosticReporter rep;
        opt::OptPipeline inlining{"inlining", {opt::PassId::Inlining}, 2};
        auto const result = opt::optimize(mir, target, interner, inlining, rep);
        EXPECT_TRUE(result.ok);
        EXPECT_EQ(rep.errorCount(), 0u);
        EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 0u)
            << "two iterations flatten the multi-block non-leaf chain — "
               "no Call remains";
        MirVerifier verifier{mir, &interner};
        EXPECT_TRUE(verifier.verify(rep));
    }
}

// ── Non-RETURNING-path leaf callee is NOT inlined (robustness gate) ─
// loopForever() { entry: Br L; L: Unreachable; }  — a MULTI-BLOCK leaf
// (no Call/IntrinsicCall/Phi) whose EVERY path ends in Unreachable: it
// has NO `Return` in any block. The multi-block splice would route each
// callee Return to a fresh continuation block; with NO Return, that
// continuation has ZERO predecessors → the MirVerifier (run after every
// pass) flags the unreachable continuation → the whole module is
// REJECTED → an otherwise-valid program becomes a build error UNDER
// inlining. inlineLegalityGate REFUSES such a callee (the Call stays;
// the program compiles).
// RED-on-disable: deleting the `if (!hasReturn) return std::nullopt;`
// gate admits loopForever → the splice emits a predecessor-less
// continuation → verifier.verify(rep) returns false (OR callsInlined
// becomes 1) and the assertions below fail.
TEST(Inlining, NonReturningLeafCalleeIsNotInlined) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    // loopForever (SymbolId 50): entry block branches to L; L ends in
    // Unreachable. Two blocks, leaf, NO Return on any path. Markers
    // are derivation-consistent (EntryBlock + Linear — a straight line;
    // ExitBlock is a dormant marker the equality verifier would reject
    // on a reachable block), so the un-inlined callee module is itself
    // verifier-valid.
    mb.addFunction(fnSig, SymbolId{50});
    MirBlockId const fEntry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const fLoop  = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(fEntry);
    mb.addBr(fLoop);
    mb.beginBlock(fLoop);
    mb.addUnreachable();

    // main (SymbolId 100): return loopForever();
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    MirInstId const fAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
    MirInstId const callOps[] = {fAddr};
    MirInstId const call = mb.addInst(MirOpcode::Call, callOps, i32);
    mb.addReturn(call);
    Mir mir = std::move(mb).finish();

    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);
    // The input module is itself verifier-valid before the pass.
    {
        DiagnosticReporter pre;
        MirVerifier preV{mir, &interner};
        ASSERT_TRUE(preV.verify(pre))
            << "the non-returning callee module must be valid pre-inlining";
    }

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 0u)
        << "a callee with NO returning path (every path ends in Unreachable) "
           "MUST NOT be inlined — the splice's continuation would be "
           "predecessor-less and the module would fail to compile";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 1u)
        << "main's Call to the non-returning loopForever must survive";

    // The refusal keeps the program compilable: the module still verifies.
    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep))
        << "refusing the non-returning callee leaves a valid module";
}

// ── Non-leaf callee (its body contains a Call) IS inlined (OPT7 c3) ─
// g() { return 9; }  f() { return g(); }  main() { return f(); }
// f is single-block but NON-LEAF (it calls g). OPT7 cycle 3 LIFTS the
// leaf restriction: BOTH g→f AND f→main are now legal. A SINGLE
// `runInlining` pass (reads the immutable source, writes a fresh
// builder) inlines BOTH levels at once — g splices into f's rebuild AND
// f splices into main's rebuild — so `callsInlined == 2`. The KEY fact
// the OLD test DENIED (asserting callsInlined==1, f's call refused) is
// that the NON-LEAF f IS inlined into main; `callsInlined == 2` proves
// it. Because main inlines f's SOURCE body (which still calls g), main
// gains a residual Call-to-g after one pass; a SECOND iteration (the
// real `optimize` engine, maxIterations≥2) flattens that → module-wide
// `Call == 0`.
// RED-on-disable: re-adding the line-209 `Call`-leaf refusal in
// inlineLegalityGate refuses the non-leaf f → main's Call to f is NOT
// spliced (callsInlined drops to 1, and main's Call survives every
// iteration) → both assertions below fail.
TEST(Inlining, NonLeafCalleeIsInlined) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& target = **targetR;

    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    // `Mir` is move-only, so build a FRESH module per observation stage.
    auto buildModule = [&] {
        MirBuilder mb;
        // g (SymbolId 40): leaf returning 9.
        mb.addFunction(fnSig, SymbolId{40});
        MirBlockId const gEntry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(gEntry);
        mb.addReturn(mb.addConst(i32Lit(9), i32));
        // f (SymbolId 50): single block, calls g → NON-LEAF (now admitted).
        mb.addFunction(fnSig, SymbolId{50});
        MirBlockId const fEntry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(fEntry);
        MirInstId const gAddr = mb.addGlobalAddr(SymbolId{40}, fnSig);
        MirInstId const gCallOps[] = {gAddr};
        (void)mb.addInst(MirOpcode::Call, gCallOps, i32);
        mb.addReturn(mb.addConst(i32Lit(9), i32));
        // main (SymbolId 100): return f();
        mb.addFunction(fnSig, SymbolId{100});
        MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(mEntry);
        MirInstId const fAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
        MirInstId const fCallOps[] = {fAddr};
        MirInstId const fCall = mb.addInst(MirOpcode::Call, fCallOps, i32);
        mb.addReturn(fCall);
        return std::move(mb).finish();
    };

    // (1) ONE pass inlines BOTH levels → callsInlined == 2. This is the
    // direct proof the NON-LEAF f is inlined into main (the old refusal
    // gave 1). main's Call to f vanishes from main; f's call-to-g is
    // cloned into main, so a residual Call remains until the next
    // iteration.
    {
        Mir mir = buildModule();
        ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 2u);  // f→g + main→f
        DiagnosticReporter rep;
        auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
        EXPECT_TRUE(r.ok);
        EXPECT_EQ(r.callsInlined, 2u)
            << "OPT7 c3: BOTH g→f AND the NON-LEAF f→main inline in one pass "
               "(the old gate refused f→main, giving 1)";
        // The non-leaf f is no longer called from main: main's only Call
        // is now the cloned call-to-g spliced in from f's body.
        EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 1u)
            << "after one pass main holds the residual cloned call-to-g "
               "(f itself is fully inlined into main)";
        MirVerifier verifier{mir, &interner};
        EXPECT_TRUE(verifier.verify(rep));
    }

    // (2) The full engine with maxIterations≥2 flattens the whole chain:
    // the second iteration inlines the residual call-to-g → module-wide
    // Call == 0. RED-on-disable (re-adding the Call-leaf refusal): f→main
    // never fires, main's Call to f survives every iteration → != 0.
    {
        Mir mir = buildModule();
        DiagnosticReporter rep;
        opt::OptPipeline inlining{"inlining", {opt::PassId::Inlining}, 2};
        auto const result = opt::optimize(mir, target, interner, inlining, rep);
        EXPECT_TRUE(result.ok);
        EXPECT_EQ(rep.errorCount(), 0u);
        EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 0u)
            << "two iterations flatten g→f→main entirely — no Call remains";
        MirVerifier verifier{mir, &interner};
        EXPECT_TRUE(verifier.verify(rep));
    }
}

// ── MUTUAL recursion is REFUSED via the SCC gate (OPT7 c3) ─────────
// f() { return g(); }  g() { return f(); }  — f and g form a 2-member
// call-graph SCC (f→g→f), so the §2.9 rule-3 SCC refusal declines to
// inline BOTH the f→g AND g→f calls — generalizing the cycle-1/2 SELF-
// recursion refusal to MUTUAL recursion. The cycle is the ONLY inline
// candidate in module 1, so the refusal is exact: `callsInlined == 0`
// and BOTH recursive Calls survive byte-for-byte.
//
// To prove the refusal is NON-VACUOUS (not a pass that refuses ALL non-
// leaf inlining), module 2 inlines an ACYCLIC non-leaf control chain
// A→B→C: A and B are non-leaf, C is a leaf, the chain is acyclic → every
// call IS inlined. If the SCC gate were over-broad (refusing all non-
// leaf), this control would fail.
//
// RED-on-disable: removing the rule-3 SCC refusal in inlineLegalityGate
// ADMITS the f↔g calls → `callsInlined` jumps to 2 (each recursive call
// "inlines", splicing the cyclic partner's body — an unroll attempt) →
// the `callsInlined == 0` assertion fails.
TEST(Inlining, MutualRecursiveCallIsNotInlined) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    // ---- Module 1: the mutual-recursion cycle f↔g (the ONLY candidates).
    {
        MirBuilder mb;
        // f (SymbolId 60): return g();
        mb.addFunction(fnSig, SymbolId{60});
        MirBlockId const fEntry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(fEntry);
        MirInstId const gAddr = mb.addGlobalAddr(SymbolId{61}, fnSig);
        MirInstId const gOps[] = {gAddr};
        MirInstId const gCall = mb.addInst(MirOpcode::Call, gOps, i32);
        mb.addReturn(gCall);
        // g (SymbolId 61): return f();
        mb.addFunction(fnSig, SymbolId{61});
        MirBlockId const gEntry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(gEntry);
        MirInstId const fAddr = mb.addGlobalAddr(SymbolId{60}, fnSig);
        MirInstId const fOps[] = {fAddr};
        MirInstId const fCall = mb.addInst(MirOpcode::Call, fOps, i32);
        mb.addReturn(fCall);
        Mir mir = std::move(mb).finish();

        ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 2u)
            << "before: f→g and g→f (the cyclic pair) are the only Calls";

        DiagnosticReporter rep;
        auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
        EXPECT_TRUE(r.ok);
        // THE discriminating assertion: NOTHING inlines — both f→g and g→f
        // are refused by the SCC gate. (With the gate OFF, each would
        // "inline" its cyclic partner → callsInlined == 2.)
        EXPECT_EQ(r.callsInlined, 0u)
            << "mutual recursion f↔g MUST NOT inline — the call-graph SCC "
               "gate refuses every call within a recursive cycle";
        // Both recursive Calls survive byte-for-byte (no rewrite happened).
        EXPECT_EQ(countOpInFuncBySymbol(mir, 60, MirOpcode::Call), 1u)
            << "f's recursive call to g MUST survive";
        EXPECT_EQ(countOpInFuncBySymbol(mir, 61, MirOpcode::Call), 1u)
            << "g's recursive call to f MUST survive";
        MirVerifier verifier{mir, &interner};
        EXPECT_TRUE(verifier.verify(rep));
    }

    // ---- Module 2 (CONTROL): acyclic non-leaf chain A→B→C IS inlined.
    // A (sym 70) calls B (sym 71) calls C (sym 72, leaf). Acyclic → each
    // is its own SCC → every call is admitted. Proves the SCC refusal is
    // not a blanket "refuse all non-leaf" gate.
    {
        MirBuilder mb;
        // C (SymbolId 72): leaf returning 3.
        mb.addFunction(fnSig, SymbolId{72});
        MirBlockId const cEntry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(cEntry);
        mb.addReturn(mb.addConst(i32Lit(3), i32));
        // B (SymbolId 71): NON-leaf, return C().
        mb.addFunction(fnSig, SymbolId{71});
        MirBlockId const bEntry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(bEntry);
        MirInstId const cAddr = mb.addGlobalAddr(SymbolId{72}, fnSig);
        MirInstId const cOps[] = {cAddr};
        MirInstId const cCall = mb.addInst(MirOpcode::Call, cOps, i32);
        mb.addReturn(cCall);
        // A (SymbolId 70): NON-leaf, return B().
        mb.addFunction(fnSig, SymbolId{70});
        MirBlockId const aEntry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(aEntry);
        MirInstId const bAddr = mb.addGlobalAddr(SymbolId{71}, fnSig);
        MirInstId const bOps[] = {bAddr};
        MirInstId const bCall = mb.addInst(MirOpcode::Call, bOps, i32);
        mb.addReturn(bCall);
        Mir mir = std::move(mb).finish();

        ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 2u);  // A→B + B→C

        DiagnosticReporter rep;
        auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
        EXPECT_TRUE(r.ok);
        // Both acyclic non-leaf calls inline in one pass (B→C and A→B).
        EXPECT_EQ(r.callsInlined, 2u)
            << "an ACYCLIC non-leaf chain A→B→C MUST inline — proves the SCC "
               "refusal is recursion-specific, not refuse-all-non-leaf";
        MirVerifier verifier{mir, &interner};
        EXPECT_TRUE(verifier.verify(rep));
    }
}

// ── IntrinsicCall-bearing callee IS inlined (OPT7 cycle 6 relaxation) ─
// f() { return some_intrinsic(5); }  main() { return f(); }
// f is single-block and contains an `IntrinsicCall` (a distinct call-like
// opcode). OPT7 cycle 6 LIFTS the IntrinsicCall refusal: the intrinsic
// clones SSA-correctly via the splice's generic arm — its id lives in the
// inst PAYLOAD (a module-stable integer copied verbatim) and its operands
// (the args) remap through the `local` map. So f is inlined: main's Call
// vanishes, the IntrinsicCall is CLONED into main (count 1 → 2 — f's
// original body is not deleted; that is DCE's job), and main's Return
// reads the spliced intrinsic.
// RED-on-disable: re-adding the `IntrinsicCall` arm in inlineLegalityGate
// refuses f → callsInlined stays 0 and main's Call survives → every
// assertion below flips. (Frame-sensitivity caveat — a frame-sensitive
// intrinsic must NOT be inlined — is trigger-gated to the first such
// intrinsic: D-OPT7-INLINE-FRAME-SENSITIVE-INTRINSIC.)
TEST(Inlining, IntrinsicCalleeIsInlined) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    // f (SymbolId 55): single block, body computes an IntrinsicCall and
    // returns it. Built exactly as HIR→MIR lowers an intrinsic call:
    // operands are the args, the intrinsic id lives in the payload (here
    // an arbitrary id — the MIR verifier does not validate it).
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
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 1u)
        << "OPT7 cycle 6: a single-block callee containing an IntrinsicCall "
           "MUST now be inlined";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 0u)
        << "main's Call to the IntrinsicCall-bearing f must be spliced away";
    // BODY-PRESENT: the intrinsic is CLONED into main; f's original stays
    // (the inliner never deletes a callee body — that is DCE's job), so the
    // module-wide IntrinsicCall count rises 1 → 2.
    EXPECT_EQ(countOpInModule(mir, MirOpcode::IntrinsicCall), 2u)
        << "the IntrinsicCall must be CLONED into main (count 1 → 2; f's "
           "original body is not deleted by the inliner)";

    // VALUE-THREADING: main's Return now reads the spliced intrinsic — the
    // intrinsic's result became the inlined call's result. A broken
    // payload/operand clone would either fail to verify or thread a wrong
    // value; this pins the result actually flows to the caller's use.
    {
        MirFuncId mainFn{};
        std::size_t const nf = mir.moduleFuncCount();
        for (std::uint32_t i = 0; i < nf; ++i) {
            if (mir.funcSymbol(mir.funcAt(i)).v == 100u) mainFn = mir.funcAt(i);
        }
        ASSERT_TRUE(mainFn.valid());
        bool foundReturn = false;
        std::uint32_t const nb = mir.funcBlockCount(mainFn);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(mainFn, bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t i = 0; i < ni; ++i) {
                MirInstId const id = mir.blockInstAt(b, i);
                if (mir.instOpcode(id) != MirOpcode::Return) continue;
                foundReturn = true;
                auto const rops = mir.instOperands(id);
                ASSERT_EQ(rops.size(), 1u);
                EXPECT_EQ(mir.instOpcode(rops[0]), MirOpcode::IntrinsicCall)
                    << "main's Return must read the spliced IntrinsicCall "
                       "(value-threading through the inlined call result)";
            }
        }
        EXPECT_TRUE(foundReturn) << "main must have a Return";
    }

    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep));
}

// ── MULTI-BLOCK IntrinsicCall callee IS inlined (intrinsic in a NON-
// entry block) ──────────────────────────────────────────────────────
// f() { entry: Br B1; B1: t = some_intrinsic(5); return t; }
// main() { return f(); }
// f is 2-block (routes through MultiBlockInliner, NOT the single-block
// linear arm) and carries the IntrinsicCall in its NON-entry block B1.
// Proves the cycle-6 relaxation holds on the multi-block CFG-clone path
// too: the IntrinsicCall is cloned via the same generic arm into main
// (count 1 → 2 — body-present), no merge Phi is introduced (single
// return), and the module verifies. RED-on-disable: re-adding the
// IntrinsicCall gate arm refuses f → callsInlined stays 0, Call survives.
TEST(Inlining, MultiBlockIntrinsicCalleeIsInlined) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    // f (SymbolId 55): entry → Br B1; B1 computes an IntrinsicCall and
    // returns it. Derivation-consistent markers (a straight line —
    // EntryBlock + Linear) so the un-inlined f is verifier-valid; the
    // post-inline re-derivation re-stamps the spliced result anyway.
    mb.addFunction(fnSig, SymbolId{55});
    MirBlockId const fEntry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const fB1    = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(fEntry);
    mb.addBr(fB1);
    mb.beginBlock(fB1);
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

    ASSERT_EQ(funcBlockCountBySymbol(mir, 55), 2u)
        << "f must be a 2-block callee so it routes through spliceMultiBlock";
    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);
    ASSERT_EQ(countOpInModule(mir, MirOpcode::IntrinsicCall), 1u);

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 1u)
        << "a multi-block callee with an IntrinsicCall in a non-entry block "
           "MUST be inlined (cycle-6 relaxation on the multi-block path)";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 0u)
        << "main's Call must be replaced by the spliced multi-block body";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::IntrinsicCall), 2u)
        << "the IntrinsicCall must be CLONED into main (count 1 → 2; f's "
           "original body is not deleted by the inliner)";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Phi), 0u)
        << "a single-return multi-block callee elides the merge Phi — the "
           "intrinsic relaxation must not introduce one";

    // VALUE-THREADING through the multi-block return-merge: main's Return
    // reads the spliced intrinsic (the single return edge's value, Phi
    // elided). This pins the value path DISTINCT from the single-block
    // linear splice — here it flows through the return-edge merge.
    {
        MirFuncId mainFn{};
        std::size_t const nf = mir.moduleFuncCount();
        for (std::uint32_t i = 0; i < nf; ++i) {
            if (mir.funcSymbol(mir.funcAt(i)).v == 100u) mainFn = mir.funcAt(i);
        }
        ASSERT_TRUE(mainFn.valid());
        bool foundReturn = false;
        std::uint32_t const nb = mir.funcBlockCount(mainFn);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(mainFn, bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t i = 0; i < ni; ++i) {
                MirInstId const id = mir.blockInstAt(b, i);
                if (mir.instOpcode(id) != MirOpcode::Return) continue;
                foundReturn = true;
                auto const rops = mir.instOperands(id);
                ASSERT_EQ(rops.size(), 1u);
                EXPECT_EQ(mir.instOpcode(rops[0]), MirOpcode::IntrinsicCall)
                    << "main's Return must read the spliced IntrinsicCall "
                       "(value-threading through the multi-block return merge)";
            }
        }
        EXPECT_TRUE(foundReturn) << "main must have a Return";
    }

    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep));
}

// ── VOID callee with an IntrinsicCall whose result is UNUSED IS inlined
// ───────────────────────────────────────────────────────────────────
// g() { some_intrinsic(5); return; }  main() { g(); return 0; }
// The intrinsic is a side-effecting statement (its value is discarded)
// and g is void. Exercises the intrinsic relocation INDEPENDENT of
// return-value threading (the void invalid-result-id splice path): the
// IntrinsicCall must still be spliced into main even though nothing reads
// it, and the module must verify. RED-on-disable: re-adding the
// IntrinsicCall gate arm refuses g.
TEST(Inlining, VoidIntrinsicCalleeIsInlined) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32     = interner.primitive(TypeKind::I32);
    TypeId const voidSig = interner.fnSig({}, InvalidType, CallConv::CcSysV);
    TypeId const mainSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    // g (SymbolId 80): VOID single-block. An IntrinsicCall (result
    // discarded) then a bare void Return.
    mb.addFunction(voidSig, SymbolId{80}, SymbolBinding::Global,
                   SymbolVisibility::Default);
    MirBlockId const gEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(gEntry);
    MirInstId const five = mb.addConst(i32Lit(5), i32);
    MirInstId const intrinOps[] = {five};
    constexpr std::uint32_t kIntrinsicId = 1;  // arbitrary, non-registry
    (void)mb.addInst(MirOpcode::IntrinsicCall, intrinOps, i32, kIntrinsicId);
    mb.addReturn();  // bare void Return — no value

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
    ASSERT_EQ(countOpInModule(mir, MirOpcode::IntrinsicCall), 1u);

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 1u)
        << "a void callee containing an IntrinsicCall MUST be inlined";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 0u)
        << "main's void Call to g must be replaced by the spliced body";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::IntrinsicCall), 2u)
        << "the IntrinsicCall must be CLONED into main (count 1 → 2; g's "
           "original body is not deleted by the inliner)";

    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep))
        << "the void-intrinsic splice (invalid result id) must verify clean";
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
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
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

// ── MIXED: one caller inlines BOTH a single-block leaf AND a multi-
// block leaf (exercises the multi-block rebuild's single-block arm) ──
// s() { return 3; }  (single-block leaf)
// m() { if (c) return 7; return 9; }  (multi-block leaf)
// main() { return s() + m(); }
// main's plan has a MULTI-block target (m) → the whole function is
// rebuilt by MultiBlockInliner, which must ALSO splice the single-block
// s() linearly (its `spliceSingleBlock` arm). Both Calls vanish; the
// merge Phi for m exists; the module verifies. RED-on-disable for the
// multi-block path's single-block handling: if `spliceSingleBlock` mis-
// threaded s()'s return, main's `s() + m()` sum would be wrong + the
// verifier would reject a dangling operand.
TEST(Inlining, MixedSingleAndMultiBlockLeavesBothInlined) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    // s (SymbolId 40): single-block leaf returning 3.
    mb.addFunction(fnSig, SymbolId{40});
    MirBlockId const sEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(sEntry);
    mb.addReturn(mb.addConst(i32Lit(3), i32));

    // m (SymbolId 50): multi-block leaf, two return paths (early-return
    // shape; derivation-consistent markers — both arms return →
    // IfThen/IfElse — so the un-inlined m is verifier-valid).
    mb.addFunction(fnSig, SymbolId{50});
    MirBlockId const mEntryB = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const mThen   = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const mAfter  = mb.createBlock(StructCfMarker::IfElse);
    mb.beginBlock(mEntryB);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const mcond = mb.addConst(tru, boolT);
    mb.addCondBr(mcond, mThen, mAfter);
    mb.beginBlock(mThen);
    mb.addReturn(mb.addConst(i32Lit(7), i32));
    mb.beginBlock(mAfter);
    mb.addReturn(mb.addConst(i32Lit(9), i32));

    // main (SymbolId 100): return s() + m();
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const mainEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mainEntry);
    MirInstId const sAddr = mb.addGlobalAddr(SymbolId{40}, fnSig);
    MirInstId const sCallOps[] = {sAddr};
    MirInstId const sCall = mb.addInst(MirOpcode::Call, sCallOps, i32);
    MirInstId const mAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
    MirInstId const mCallOps[] = {mAddr};
    MirInstId const mCall = mb.addInst(MirOpcode::Call, mCallOps, i32);
    MirInstId const addOps[] = {sCall, mCall};
    MirInstId const sum = mb.addInst(MirOpcode::Add, addOps, i32);
    mb.addReturn(sum);
    Mir mir = std::move(mb).finish();

    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 2u);

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 2u)
        << "both the single-block s() and the multi-block m() are inlined";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 0u)
        << "both Calls in main must be spliced away";
    // m's two returns → one merge Phi; s's single return → no Phi
    // (linear splice). So exactly one Phi module-wide.
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Phi), 1u)
        << "only m's two-return merge produces a Phi; s splices linearly";

    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep))
        << "the mixed single+multi-block splice must verify clean";
}

// ── MULTI-BLOCK leaf with EXACTLY ONE return → the merge Phi is ELIDED
// to the single value (the second of spliceMultiBlock's three return-
// handling forms; the first — 2+ returns → Phi — is pinned above by
// MultiBlockLeafCalleeIsInlined). ───────────────────────────────────
// callee f() { entry: Br B1; B1: return 7; }  — MULTI-block (2 blocks,
// so it routes through MultiBlockInliner::spliceMultiBlock, NOT the
// single-block linear path) + LEAF (no Call/IntrinsicCall) + NO Phi +
// exactly ONE Return. main() { return f(); }. The splice clones f's two
// blocks into main, rewrites B1's `Return 7` to a `Br` into a fresh
// continuation block, and — because there is exactly ONE return edge —
// takes the `returnEdges.size() == 1` branch: `result = returnEdges[0]
// .value` (the cloned `Const 7`), ELIDING a degenerate 1-incoming Phi.
//
// THE load-bearing assertion is `Phi count == 0`: it goes RED if a
// future change to spliceMultiBlock drops the size()==1 special case
// and instead emits a 1-incoming merge Phi for the single-return form.
// We ALSO prove value correctness: main's Return resolves to the cloned
// `Const 7` (not a Phi, not f's original const) — the elided value
// actually flows to the caller's use of the call result.
TEST(Inlining, MultiBlockSingleReturnLeafElidesMergePhi) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    // f (SymbolId 50): entry branches to B1; B1 returns 7. Two blocks,
    // leaf, NO Phi, exactly ONE Return. Derivation-consistent markers
    // (a straight line — EntryBlock + Linear) so the un-inlined f is
    // itself verifier-valid; the post-inline re-derivation re-stamps
    // the spliced result.
    mb.addFunction(fnSig, SymbolId{50});
    MirBlockId const fEntry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const fB1    = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(fEntry);
    mb.addBr(fB1);
    mb.beginBlock(fB1);
    mb.addReturn(mb.addConst(i32Lit(7), i32));

    // main (SymbolId 100): return f();
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    MirInstId const fAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
    MirInstId const callOps[] = {fAddr};
    MirInstId const call = mb.addInst(MirOpcode::Call, callOps, i32);
    mb.addReturn(call);
    Mir mir = std::move(mb).finish();

    // ROUTING pin: f is MULTI-block (2 blocks) → planHasMultiBlock is
    // true → main is rebuilt by MultiBlockInliner and the call hits
    // spliceMultiBlock (NOT the single-block linear arm). RED-on-disable
    // if the builder ever collapsed the trivial entry→B1 chain to one
    // block (it does not — MirBuilder is a faithful recorder).
    ASSERT_EQ(funcBlockCountBySymbol(mir, 50), 2u)
        << "f must be a 2-block callee so it routes through spliceMultiBlock";
    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);
    ASSERT_EQ(countOpInModule(mir, MirOpcode::Phi), 0u)
        << "before: no Phi anywhere (f has none; main has none)";
    auto const mainBlocksBefore = funcBlockCountBySymbol(mir, 100);
    ASSERT_EQ(mainBlocksBefore, 1u) << "before: main is a single block";
    auto const totalBlocksBefore = blockCountInModule(mir);

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 1u)
        << "a multi-block LEAF callee with one return MUST be inlined";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 0u)
        << "main's Call must be replaced by the spliced multi-block body";

    // The callee's 2 blocks were CLONED + a continuation added: main's
    // block count rose from 1 to (entry + 2 clones + 1 continuation) = 4,
    // and the module total rose by the same 3. Red-on-disable for "the
    // CFG was cloned".
    EXPECT_EQ(funcBlockCountBySymbol(mir, 100), mainBlocksBefore + 3)
        << "f's 2 blocks + 1 continuation must be cloned into main";
    EXPECT_EQ(blockCountInModule(mir), totalBlocksBefore + 3)
        << "module-wide block count rises by the cloned + continuation blocks";

    // ★ THE load-bearing assertion: the single-return merge Phi was
    // ELIDED. With exactly one return edge, spliceMultiBlock takes
    // `result = returnEdges[0].value` and emits NO Phi. A regression that
    // wrongly built a 1-incoming Phi for this form would make this == 1.
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Phi), 0u)
        << "a single-return multi-block callee MUST elide the merge Phi "
           "(returnEdges.size()==1 → result = the value directly), NOT "
           "create a degenerate 1-incoming Phi";

    // VALUE correctness: the elided value flows to main's use of the call
    // result. main's Return operand must be the CLONED `Const 7` (a
    // Const, value 7) — never a Phi, never f's original const. f's own
    // `Const 7` still exists (not deleted — DCE's job), so module-wide
    // Const count rose by exactly one (1 → 2).
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Const), 2u)
        << "f's Const 7 must be cloned into main (body present)";
    {
        MirFuncId mainFn{};
        std::size_t const nf = mir.moduleFuncCount();
        for (std::uint32_t i = 0; i < nf; ++i) {
            if (mir.funcSymbol(mir.funcAt(i)).v == 100u) mainFn = mir.funcAt(i);
        }
        ASSERT_TRUE(mainFn.valid()) << "main (sym 100) must exist post-splice";

        // Find main's Return and inspect the value it carries.
        MirInstId retVal{};
        bool foundReturn = false;
        std::uint32_t const nb = mir.funcBlockCount(mainFn);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(mainFn, bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t i = 0; i < ni; ++i) {
                MirInstId const id = mir.blockInstAt(b, i);
                if (mir.instOpcode(id) != MirOpcode::Return) continue;
                foundReturn = true;
                auto const rops = mir.instOperands(id);
                ASSERT_EQ(rops.size(), 1u) << "main returns a value";
                retVal = rops[0];
            }
        }
        ASSERT_TRUE(foundReturn) << "main must still terminate in a Return";
        ASSERT_TRUE(retVal.valid());
        // The returned value is the cloned Const 7 — the ELIDED return
        // value, threaded straight to main's Return (no Phi indirection).
        EXPECT_EQ(mir.instOpcode(retVal), MirOpcode::Const)
            << "main must return the elided cloned Const, NOT a merge Phi";
        EXPECT_EQ(std::get<std::int64_t>(
                      mir.literalValue(mir.constLiteralIndex(retVal)).value),
                  7)
            << "the elided single-return value (7) must reach main's Return";
    }

    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep))
        << "the single-return multi-block splice (Phi elided) must verify clean";
}

// ── MULTI-BLOCK leaf returning VOID → no return value, no merge Phi,
// no rewrite entry (the third of spliceMultiBlock's return forms). ───
// The existing VoidLeafCalleeIsInlined is SINGLE-block (routes through
// spliceSingleBlock); this one is MULTI-block so it exercises the VOID
// branch of spliceMultiBlock specifically (line: `result` stays invalid
// → no `rewrite_.emplace(oldCall.v, ...)`; the void Return contributes
// no return edge so `returnEdges` is empty → no Phi).
// callee g() { entry: Br B1; B1: return; }  (void, two blocks, leaf).
// main() { g(); return 0; }  (g's "result" is unused).
// Asserts: callsInlined==1; Call gone; main's block count rose (clone
// happened); Phi count == 0 (a void callee produces no merge Phi); the
// void body inst (a Const) is spliced in; main returns its OWN Const 0
// (the void call yields no value to thread); MirVerifier clean.
TEST(Inlining, MultiBlockVoidLeafIsInlined) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32     = interner.primitive(TypeKind::I32);
    TypeId const voidSig = interner.fnSig({}, InvalidType, CallConv::CcSysV);
    TypeId const mainSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    // g (SymbolId 80): VOID multi-block leaf. entry branches to B1; B1
    // holds a real (non-terminator) body inst — a Const the splice must
    // clone — then a bare void Return (no value). Derivation-consistent
    // markers (straight line — EntryBlock + Linear) → un-inlined g valid.
    mb.addFunction(voidSig, SymbolId{80}, SymbolBinding::Global,
                   SymbolVisibility::Default);
    MirBlockId const gEntry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const gB1    = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(gEntry);
    mb.addBr(gB1);
    mb.beginBlock(gB1);
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

    // ROUTING pin: g is MULTI-block (2 blocks) → the call hits
    // spliceMultiBlock (NOT spliceSingleBlock — that is the existing
    // VoidLeafCalleeIsInlined's path). This is what makes this test cover
    // the VOID branch of the MULTI-block splice specifically.
    ASSERT_EQ(funcBlockCountBySymbol(mir, 80), 2u)
        << "g must be a 2-block callee so it routes through spliceMultiBlock";
    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);
    ASSERT_EQ(countOpInModule(mir, MirOpcode::Phi), 0u)
        << "before: no Phi anywhere";
    // Before: g holds Const 99; main holds Const 0 → 2 Consts module-wide.
    auto const constsBefore = countOpInModule(mir, MirOpcode::Const);
    ASSERT_EQ(constsBefore, 2u);
    auto const mainBlocksBefore = funcBlockCountBySymbol(mir, 100);
    ASSERT_EQ(mainBlocksBefore, 1u) << "before: main is a single block";
    auto const totalBlocksBefore = blockCountInModule(mir);

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 1u)
        << "a multi-block VOID leaf callee MUST be inlined";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 0u)
        << "main's void Call to g must be replaced by the spliced body";

    // g's 2 blocks were CLONED + a continuation added → main rose by 3.
    EXPECT_EQ(funcBlockCountBySymbol(mir, 100), mainBlocksBefore + 3)
        << "g's 2 blocks + 1 continuation must be cloned into main";
    EXPECT_EQ(blockCountInModule(mir), totalBlocksBefore + 3)
        << "module-wide block count rises by the cloned + continuation blocks";

    // ★ A void callee produces NO return value → NO merge Phi. The void
    // Return contributes no return edge, so returnEdges is empty and the
    // continuation gets no Phi. RED if a future change emitted a spurious
    // Phi for the void form.
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Phi), 0u)
        << "a void callee must NOT produce a return-merge Phi";

    // BODY-PRESENT pin: g's `Const 99` is spliced into main (g's own copy
    // still exists) → module-wide Const count rose by exactly one (2 → 3).
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Const), constsBefore + 1)
        << "the void callee's body (Const 99) must be SPLICED into main";

    // No spurious result use: main returns its OWN Const 0 (the void call
    // yields no value to thread; an invalid result id is recorded into NO
    // rewrite entry). main's Return value must be a Const 0, never the
    // cloned Const 99 and never a Phi.
    {
        MirFuncId mainFn{};
        std::size_t const nf = mir.moduleFuncCount();
        for (std::uint32_t i = 0; i < nf; ++i) {
            if (mir.funcSymbol(mir.funcAt(i)).v == 100u) mainFn = mir.funcAt(i);
        }
        ASSERT_TRUE(mainFn.valid()) << "main (sym 100) must exist post-splice";
        MirInstId retVal{};
        bool foundReturn = false;
        std::uint32_t const nb = mir.funcBlockCount(mainFn);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(mainFn, bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t i = 0; i < ni; ++i) {
                MirInstId const id = mir.blockInstAt(b, i);
                if (mir.instOpcode(id) != MirOpcode::Return) continue;
                foundReturn = true;
                auto const rops = mir.instOperands(id);
                ASSERT_EQ(rops.size(), 1u) << "main returns its own value";
                retVal = rops[0];
            }
        }
        ASSERT_TRUE(foundReturn) << "main must still terminate in a Return";
        ASSERT_TRUE(retVal.valid());
        EXPECT_EQ(mir.instOpcode(retVal), MirOpcode::Const)
            << "main must return its own Const, not a spliced/void value";
        EXPECT_EQ(std::get<std::int64_t>(
                      mir.literalValue(mir.constLiteralIndex(retVal)).value),
                  0)
            << "main returns its own Const 0 (the void call yields no value)";
    }

    // The invalid-result-id (void Return) splice path must not break the
    // verifier — SSA dominance, Phi completeness, CFG edges all hold.
    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep))
        << "the void multi-block splice (invalid result id) must verify clean";
}

// ── G4 (Cycle 26, D-OPT7-1): a WEAK callee that SURVIVES the merge is REFUSED by the
// inliner running on the MERGED module ───────────────────────────────────────────────
//
// The cross-CU Weak corpus (`weak_inline_crosscu`) proves the MERGE drops a weak `f`
// when a STRONG sibling exists (strong-shadows-weak), so the merged inliner only ever
// sees the strong body. But a weak symbol with NO strong sibling SURVIVES the merge with
// its `SymbolBinding::Weak` preserved — and at final link a strong def from ANOTHER
// translation unit (or library) may still replace it. So the merged-module inliner must
// ITSELF refuse a surviving Weak callee; the merge's strong-drop is not the only line of
// defense. This pin makes that merged-tier Weak gate load-bearing.
//
// It runs the FULL `opt::optimize` engine (exactly what `optimizeModule` drives on the
// merged module) with an `[Inlining]` pipeline over a module where `f` is Weak (no
// strong sibling present) + `main` calls it. The §2.9 rule-2 Weak refusal must fire →
// main's Call SURVIVES. RED-on-disable: removing the Weak refusal in
// `inlineLegalityGate` would splice `return 7` into main and the Call would vanish
// (`countOpInModule(..., Call) == 0`), failing this pin — the same miscompile the
// cross-CU corpus catches at exit-code tier.
TEST(Inlining, WeakCalleeSurvivingMergeIsRefusedByMergedOptimize) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& target = **targetR;

    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildCallerCalleeModule(interner, SymbolBinding::Weak, 7);

    auto const callsBefore = countOpInModule(mir, MirOpcode::Call);
    ASSERT_EQ(callsBefore, 1u) << "before: main holds one direct call to the weak f";

    DiagnosticReporter rep;
    opt::OptPipeline inlining{"inlining", {opt::PassId::Inlining}};
    auto const result = opt::optimize(mir, target, interner, inlining, rep);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(result.mutationCount(opt::PassId::Inlining), 0u)
        << "the Inlining pass must record ZERO mutations — a surviving Weak callee is "
           "refused on the merged module (a strong def may still replace it at link)";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), callsBefore)
        << "the cross-CU/merged-module inliner MUST NOT inline a surviving Weak callee "
           "— the Call survives (D-OPT7-1 merged-tier Weak gate)";

    // The module the merged optimize produced still verifies (the refusal is a no-op,
    // not a malformed rewrite).
    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep));
}

// ── INLINE COST MODEL (OPT7 cycle 28) ───────────────────────────────
// The §2.9 legality gate refuses a callee whose instruction-count
// (summed across ALL blocks) exceeds `inlineThreshold`. These pins
// exercise the size gate: a real >threshold callee refused, a small
// one inlined, the EXACT-threshold/over-by-one boundary (proving `>`
// not `>=`), and the threshold-0 fail-safe (everything refused).

namespace {

// Build a SINGLE-BLOCK LEAF callee with EXACTLY `instCount` MIR
// instructions, returning an I32 value. The body is a Const seed
// followed by a chain of `Add`s, terminated by a `Return` — every one
// of which the gate's body scan counts. `instCount` must be >= 2 (a
// Const + a Return is the minimum returning leaf). Global binding,
// nullary, non-recursive, address-not-taken, no Call/IntrinsicCall/Phi
// → it passes EVERY other gate rule, so the ONLY thing that can refuse
// it is the size bound. Appends the callee to `mb` under `calleeSym`.
void addSizedLeafCallee(MirBuilder& mb, TypeInterner& interner,
                        SymbolId calleeSym, std::uint32_t instCount) {
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    mb.addFunction(fnSig, calleeSym, SymbolBinding::Global,
                   SymbolVisibility::Default);
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    // inst 1: the Const seed.
    MirInstId acc = mb.addConst(i32Lit(1), i32);
    // insts 2 .. (instCount - 1): a chain of Adds (instCount-2 of them).
    for (std::uint32_t k = 2; k < instCount; ++k) {
        MirInstId const addOps[] = {acc, acc};
        acc = mb.addInst(MirOpcode::Add, addOps, i32);
    }
    // inst instCount: the Return terminator.
    mb.addReturn(acc);
}

// Count the instructions in a single-block leaf identified by symbol —
// a self-check that `addSizedLeafCallee` produced exactly the intended
// size (so the boundary tests below pin the gate, not a miscounted
// fixture).
std::size_t leafInstCountBySymbol(Mir const& mir, std::uint32_t sym) {
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        if (mir.funcSymbol(f).v != sym) continue;
        std::size_t n = 0;
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            n += mir.blockInstCount(mir.funcBlockAt(f, bi));
        }
        return n;
    }
    return 0;
}

// Build a module: a SMALL leaf (`smallSize` insts, sym 50) + a LARGE
// leaf (`largeSize` insts, sym 60), and `main` (sym 100) that calls
// BOTH and returns their sum. Used by the discriminating cost-model
// pin: with a threshold BETWEEN the two sizes, only the small one
// inlines.
Mir buildSmallAndLargeCalleeModule(TypeInterner& interner,
                                   std::uint32_t smallSize,
                                   std::uint32_t largeSize) {
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    addSizedLeafCallee(mb, interner, SymbolId{50}, smallSize);
    addSizedLeafCallee(mb, interner, SymbolId{60}, largeSize);
    // main (SymbolId 100): return small() + large();
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    MirInstId const smallAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
    MirInstId const smallOps[] = {smallAddr};
    MirInstId const smallCall = mb.addInst(MirOpcode::Call, smallOps, i32);
    MirInstId const largeAddr = mb.addGlobalAddr(SymbolId{60}, fnSig);
    MirInstId const largeOps[] = {largeAddr};
    MirInstId const largeCall = mb.addInst(MirOpcode::Call, largeOps, i32);
    MirInstId const sumOps[] = {smallCall, largeCall};
    MirInstId const sum = mb.addInst(MirOpcode::Add, sumOps, i32);
    mb.addReturn(sum);
    return std::move(mb).finish();
}

// Whether `main` (sym 100) still calls the callee at `calleeSym` — i.e.
// a Call in main whose operand[0] is a GlobalAddr to `calleeSym`. Used
// to assert PRECISELY which call survived (vs. a module-wide Call count
// that a residual inner call could confuse).
bool mainStillCalls(Mir const& mir, std::uint32_t calleeSym) {
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        if (mir.funcSymbol(f).v != 100u) continue;
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t ii = 0; ii < ni; ++ii) {
                MirInstId const id = mir.blockInstAt(b, ii);
                if (mir.instOpcode(id) != MirOpcode::Call) continue;
                auto const ops = mir.instOperands(id);
                if (ops.empty()) continue;
                if (mir.instOpcode(ops[0]) != MirOpcode::GlobalAddr) continue;
                if (mir.globalAddrSymbol(ops[0]).v == calleeSym) return true;
            }
        }
    }
    return false;
}

} // namespace

// ── COST MODEL: a callee larger than the threshold is REFUSED; a
// smaller one is inlined. ────────────────────────────────────────────
// Module: small leaf (3 insts, sym 50) + large leaf (12 insts, sym 60),
// main calls BOTH. Run `runInlining` with threshold = 5 (BETWEEN 3 and
// 12): the small callee (3 <= 5) IS inlined; the large callee (12 > 5)
// is REFUSED. Asserts callsInlined == 1 (ONLY the small one), main no
// longer calls the small callee, AND main STILL calls the large one.
// RED-on-disable: removing the `if (instCount > inlineThreshold) return
// std::nullopt;` check in inlineLegalityGate admits the large callee →
// callsInlined becomes 2 and `mainStillCalls(large)` becomes false →
// both assertions fail.
TEST(Inlining, InlineCostModelRefusesLargeCallee) {
    TypeInterner interner{CompilationUnitId{1}};
    constexpr std::uint32_t kSmall = 3;
    constexpr std::uint32_t kLarge = 12;
    constexpr std::uint32_t kThreshold = 5;  // 3 <= 5 < 12
    Mir mir = buildSmallAndLargeCalleeModule(interner, kSmall, kLarge);

    // Fixture self-check: the two callees have EXACTLY the intended
    // sizes, so the assertions below pin the gate, not a miscount.
    ASSERT_EQ(leafInstCountBySymbol(mir, 50), kSmall);
    ASSERT_EQ(leafInstCountBySymbol(mir, 60), kLarge);
    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 2u)
        << "before: main calls both the small and large callee";

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep, kThreshold);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 1u)
        << "only the SMALL callee (<= threshold) inlines; the large one "
           "(> threshold) is refused by the cost model";
    EXPECT_FALSE(mainStillCalls(mir, 50))
        << "the small callee's call must be spliced away (inlined)";
    EXPECT_TRUE(mainStillCalls(mir, 60))
        << "the LARGE callee's call MUST survive — it exceeds the size "
           "threshold (RED if the instCount>threshold gate is removed)";

    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep))
        << "the cost-gated module must verify clean";
}

// ── COST MODEL BOUNDARY: EXACTLY threshold inlines, threshold+1 is
// refused (pins `>` not `>=`). ───────────────────────────────────────
// Two independent observations, each main()→callee():
//   (a) a callee of EXACTLY `kThreshold` insts, threshold = kThreshold
//       → INLINED (instCount == threshold is NOT > threshold).
//   (b) a callee of `kThreshold + 1` insts, threshold = kThreshold
//       → REFUSED (instCount == threshold+1 IS > threshold).
// RED-on-disable: changing the gate from `>` to `>=` flips (a) to
// refused (the at-threshold callee no longer inlines) → arm (a) fails.
TEST(Inlining, InlineCostModelBoundaryIsExclusiveUpper) {
    constexpr std::uint32_t kThreshold = 6;

    // (a) EXACTLY threshold insts → inlined.
    {
        TypeInterner interner{CompilationUnitId{1}};
        TypeId const i32   = interner.primitive(TypeKind::I32);
        TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
        MirBuilder mb;
        addSizedLeafCallee(mb, interner, SymbolId{50}, kThreshold);
        mb.addFunction(fnSig, SymbolId{100});
        MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(mEntry);
        MirInstId const cAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
        MirInstId const cOps[] = {cAddr};
        MirInstId const cCall = mb.addInst(MirOpcode::Call, cOps, i32);
        mb.addReturn(cCall);
        Mir mir = std::move(mb).finish();

        ASSERT_EQ(leafInstCountBySymbol(mir, 50), kThreshold)
            << "fixture: the callee has EXACTLY threshold instructions";

        DiagnosticReporter rep;
        auto const r = opt::passes::runInlining(mir, interner, rep, kThreshold);
        EXPECT_TRUE(r.ok);
        EXPECT_EQ(r.callsInlined, 1u)
            << "a callee of EXACTLY threshold instructions MUST inline "
               "(instCount == threshold is not > threshold; `>` not `>=`)";
        EXPECT_FALSE(mainStillCalls(mir, 50))
            << "the at-threshold callee's call must be spliced away";
        MirVerifier verifier{mir, &interner};
        EXPECT_TRUE(verifier.verify(rep));
    }

    // (b) threshold + 1 insts → refused.
    {
        TypeInterner interner{CompilationUnitId{1}};
        TypeId const i32   = interner.primitive(TypeKind::I32);
        TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
        MirBuilder mb;
        addSizedLeafCallee(mb, interner, SymbolId{50}, kThreshold + 1);
        mb.addFunction(fnSig, SymbolId{100});
        MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(mEntry);
        MirInstId const cAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
        MirInstId const cOps[] = {cAddr};
        MirInstId const cCall = mb.addInst(MirOpcode::Call, cOps, i32);
        mb.addReturn(cCall);
        Mir mir = std::move(mb).finish();

        ASSERT_EQ(leafInstCountBySymbol(mir, 50), kThreshold + 1)
            << "fixture: the callee has threshold+1 instructions";

        DiagnosticReporter rep;
        auto const r = opt::passes::runInlining(mir, interner, rep, kThreshold);
        EXPECT_TRUE(r.ok);
        EXPECT_EQ(r.callsInlined, 0u)
            << "a callee ONE instruction over the threshold MUST be refused";
        EXPECT_TRUE(mainStillCalls(mir, 50))
            << "the over-by-one callee's call must survive";
        MirVerifier verifier{mir, &interner};
        EXPECT_TRUE(verifier.verify(rep));
    }
}

// ── COST MODEL FAIL-SAFE: threshold 0 refuses EVERYTHING. ────────────
// A threshold of 0 (impossible from the loader, which rejects 0, but
// reachable via a programmatic OptPipeline construction) means NO callee
// inlines — even the smallest 2-instruction leaf has instCount (2) > 0.
// This is the conservative fail-safe: a threshold below the smallest
// callee refuses all inlining; nothing miscompiles. RED-on-disable:
// removing the size gate inlines the 2-inst callee even at threshold 0.
TEST(Inlining, InlineCostModelZeroThresholdRefusesAll) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    // The smallest possible returning leaf: Const + Return = 2 insts.
    addSizedLeafCallee(mb, interner, SymbolId{50}, 2);
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    MirInstId const cAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
    MirInstId const cOps[] = {cAddr};
    MirInstId const cCall = mb.addInst(MirOpcode::Call, cOps, i32);
    mb.addReturn(cCall);
    Mir mir = std::move(mb).finish();

    ASSERT_EQ(leafInstCountBySymbol(mir, 50), 2u);
    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep, /*threshold=*/0);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 0u)
        << "threshold 0 must refuse EVERY callee — even a 2-instruction "
           "leaf has instCount (2) > 0 (fail-safe: refuse-all below the "
           "smallest callee, never a miscompile)";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 1u)
        << "the smallest leaf's call must survive at threshold 0";

    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep));
}

// ── D15 cycle C (a): a LOOP-BEARING callee inlines, LAYOUT-clean ────
// f(k){ int acc=37; while(k){acc++; k--;} return acc; } in its PRE-Mem2Reg
// alloca form (the shape `runInlining` sees in release — Inlining runs
// BEFORE Mem2Reg, so the loop variable lives in an alloca, no callee Phi).
// The callee is MULTI-BLOCK with a real loop back-edge (body→header). The
// C1 by-construction topological pre-creation lays the cloned header
// before its body and the continuation after every clone, so the
// MirVerifier — WITH the new layout rule (I_LayoutUseBeforeDef) — sees
// ZERO diagnostics. This is the iter.c RC-B shape at the unit tier.
// RED-on-disable: reverting C1's creation order (continuation created
// before the clones) lays a clone-defined value's def AFTER the
// continuation that consumes it → I_LayoutUseBeforeDef fires →
// verifier.verify(rep) returns false (demonstrated in the cycle gate).
TEST(Inlining, LoopBearingCalleeInlinesLayoutClean) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const i32p  = interner.pointer(i32);
    TypeId const params[] = {i32};
    TypeId const fSig  = interner.fnSig(params, i32, CallConv::CcSysV);
    TypeId const mainSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    // f(int k): alloca-based counted loop (Phi-free — the gate forbids
    // callee Phis; this is the pre-Mem2Reg lowering).
    mb.addFunction(fSig, SymbolId{50});
    MirBlockId const fEntry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const fHeader = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const fBody   = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const fExit   = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(fEntry);
    MirInstId const kArg    = mb.addArg(0, i32);
    MirInstId const accSlot = mb.addInst(MirOpcode::Alloca, {}, i32p);
    MirInstId const kSlot   = mb.addInst(MirOpcode::Alloca, {}, i32p);
    {
        MirInstId const st0[] = {kArg, kSlot};
        (void)mb.addInst(MirOpcode::Store, st0, InvalidType);
        MirInstId const c37 = mb.addConst(i32Lit(37), i32);
        MirInstId const st1[] = {c37, accSlot};
        (void)mb.addInst(MirOpcode::Store, st1, InvalidType);
    }
    mb.addBr(fHeader);
    mb.beginBlock(fHeader);
    MirInstId const kv   = mb.addInst(MirOpcode::Load, std::array{kSlot}, i32);
    MirInstId const zero = mb.addConst(i32Lit(0), i32);
    MirInstId const ne   = mb.addInst(MirOpcode::ICmpNe, std::array{kv, zero}, boolT);
    mb.addCondBr(ne, fBody, fExit);
    mb.beginBlock(fBody);
    {
        MirInstId const a    = mb.addInst(MirOpcode::Load, std::array{accSlot}, i32);
        MirInstId const one  = mb.addConst(i32Lit(1), i32);
        MirInstId const a1   = mb.addInst(MirOpcode::Add, std::array{a, one}, i32);
        MirInstId const sta[] = {a1, accSlot};
        (void)mb.addInst(MirOpcode::Store, sta, InvalidType);
        MirInstId const kk   = mb.addInst(MirOpcode::Load, std::array{kSlot}, i32);
        MirInstId const one2 = mb.addConst(i32Lit(1), i32);
        MirInstId const kk1  = mb.addInst(MirOpcode::Sub, std::array{kk, one2}, i32);
        MirInstId const stk[] = {kk1, kSlot};
        (void)mb.addInst(MirOpcode::Store, stk, InvalidType);
    }
    mb.addBr(fHeader);  // back edge
    mb.beginBlock(fExit);
    MirInstId const rv = mb.addInst(MirOpcode::Load, std::array{accSlot}, i32);
    mb.addReturn(rv);

    // main(): return f(5)
    mb.addFunction(mainSig, SymbolId{100});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    MirInstId const fAddr = mb.addGlobalAddr(SymbolId{50}, fSig);
    MirInstId const arg5  = mb.addConst(i32Lit(5), i32);
    MirInstId const callOps[] = {fAddr, arg5};
    MirInstId const call = mb.addInst(MirOpcode::Call, callOps, i32);
    mb.addReturn(call);
    Mir mir = std::move(mb).finish();

    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 1u)
        << "the loop-bearing (alloca-form) callee MUST inline (one call)";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 0u)
        << "main's call to f must be spliced away";

    // THE pin: the spliced module — INCLUDING the new layout rule — is
    // verifier-clean. ZERO diagnostics (not just verify==true).
    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep))
        << "the loop-bearing splice must verify clean WITH the layout rule";
    EXPECT_EQ(countCode(rep, DiagnosticCode::I_LayoutUseBeforeDef), 0u)
        << "C1's by-construction topological layout must leave NO layout "
           "violation in the cloned loop body / continuation";
    EXPECT_EQ(rep.errorCount(), 0u) << "zero diagnostics of any kind";
}

// ── D15 cycle C (b): recursion × inlining — main→rec inlines, rec→rec
// survives (the cross-SCC edge). ────────────────────────────────────
// rec(k){ if(k){ return rec(k-1)+1; } return 37; }  main(){ return rec(5); }
// `rec` is self-recursive → its self-call is in rec's own SCC → the SCC
// gate (rule 3) REFUSES rec→rec (it stays out-of-line). But main→rec is
// a CROSS-SCC edge (main is not in rec's SCC) → it IS inlined. So exactly
// ONE call inlines (main→rec); the recursive self-call survives INSIDE
// the now-inlined-into-main body. Verifier-clean incl. the layout rule
// (rec is a multi-block diamond — the splice's continuation consumes the
// merged return value, which C1 lays out correctly).
TEST(Inlining, RecursionCrossSccInlinesCallerSelfCallSurvives) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {i32};
    TypeId const recSig = interner.fnSig(params, i32, CallConv::CcSysV);
    TypeId const mainSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    // rec(int k): if (k) return rec(k-1)+1; return 37;  (alloca-free,
    // Phi-free diamond — entry CondBr to a recursive-then arm + a base
    // else arm; each arm Returns, so no merge Phi).
    mb.addFunction(recSig, SymbolId{50});
    MirBlockId const rEntry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const rThen  = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const rElse  = mb.createBlock(StructCfMarker::IfElse);
    mb.beginBlock(rEntry);
    MirInstId const kArg = mb.addArg(0, i32);
    MirInstId const zero = mb.addConst(i32Lit(0), i32);
    MirInstId const ne   = mb.addInst(MirOpcode::ICmpNe, std::array{kArg, zero}, boolT);
    mb.addCondBr(ne, rThen, rElse);
    mb.beginBlock(rThen);
    {
        MirInstId const selfAddr = mb.addGlobalAddr(SymbolId{50}, recSig);
        MirInstId const one  = mb.addConst(i32Lit(1), i32);
        MirInstId const km1  = mb.addInst(MirOpcode::Sub, std::array{kArg, one}, i32);
        MirInstId const recOps[] = {selfAddr, km1};
        MirInstId const recCall = mb.addInst(MirOpcode::Call, recOps, i32);
        MirInstId const one2 = mb.addConst(i32Lit(1), i32);
        MirInstId const sum  = mb.addInst(MirOpcode::Add, std::array{recCall, one2}, i32);
        mb.addReturn(sum);
    }
    mb.beginBlock(rElse);
    mb.addReturn(mb.addConst(i32Lit(37), i32));

    // main(): return rec(5)
    mb.addFunction(mainSig, SymbolId{100});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    MirInstId const recAddr = mb.addGlobalAddr(SymbolId{50}, recSig);
    MirInstId const arg5    = mb.addConst(i32Lit(5), i32);
    MirInstId const callOps[] = {recAddr, arg5};
    MirInstId const call = mb.addInst(MirOpcode::Call, callOps, i32);
    mb.addReturn(call);
    Mir mir = std::move(mb).finish();

    // Before: 2 calls (main→rec, rec→rec self-call).
    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 2u);

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 1u)
        << "exactly ONE call inlines: the cross-SCC main→rec edge. The "
           "rec→rec self-call is in rec's own SCC and the gate refuses it.";
    // rec's own body still holds its self-call (refused, out-of-line).
    EXPECT_EQ(countOpInFuncBySymbol(mir, 50, MirOpcode::Call), 1u)
        << "rec's recursive self-call MUST survive (SCC gate refusal)";
    // main inlined rec's SOURCE body → it now carries the cloned self-call
    // (the residual recursive Call spliced in from rec's then-arm).
    EXPECT_EQ(countOpInFuncBySymbol(mir, 100, MirOpcode::Call), 1u)
        << "main holds the cloned residual self-call from rec's then-arm";

    // Verifier-clean incl. the layout rule (the diamond's continuation).
    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep))
        << "the cross-SCC recursion splice must verify clean (layout rule)";
    EXPECT_EQ(countCode(rep, DiagnosticCode::I_LayoutUseBeforeDef), 0u);
}

// ════════════════════════════════════════════════════════════════════
// D-OPT7-MULTIBLOCK-SPLICE-PHI: inline a callee that contains a `Phi`.
//
// The multi-block inliner now clones a callee `Phi` via a DEFERRED flush
// (placeholder in the clone loop; incomings remapped through the value
// (`local`) + block (`calleeBlockMap`) maps AFTER the loop) — the same
// discipline the caller's own phis use, sibling of the `returnEdges`
// flush, DISJOINT maps. The deferral is UNIFORM across phi shapes (join +
// loop): loop-phi support is the ABSENCE of an artificial restriction.
//
// THE PRIMARY GUARANTEE the MirVerifier provably CANNOT give is value↔
// pred pairing: when both incoming values dominate both preds (the
// constant-armed ternary), a transposition (right preds, wrong value↔
// pred pairing) still passes the verifier (the documented blind spot at
// MultiBlockLeafCalleeIsInlined's pairing block). So every Phi-clone test
// below carries a STRUCTURAL pairing assertion via the shared helper.
// ════════════════════════════════════════════════════════════════════

namespace {

// Find the function with symbol `sym` in `mir` (Invalid if absent).
MirFuncId funcBySymbol(Mir const& mir, std::uint32_t sym) {
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        if (mir.funcSymbol(f).v == sym) return f;
    }
    return MirFuncId{};
}

// Collect every Phi instruction in the function with symbol `sym`, in
// (block-layout, in-block) order.
std::vector<MirInstId> phisInFuncBySymbol(Mir const& mir, std::uint32_t sym) {
    std::vector<MirInstId> phis;
    MirFuncId const f = funcBySymbol(mir, sym);
    if (!f.valid()) return phis;
    std::uint32_t const nb = mir.funcBlockCount(f);
    for (std::uint32_t bi = 0; bi < nb; ++bi) {
        MirBlockId const b = mir.funcBlockAt(f, bi);
        std::uint32_t const ni = mir.blockInstCount(b);
        for (std::uint32_t i = 0; i < ni; ++i) {
            MirInstId const id = mir.blockInstAt(b, i);
            if (mir.instOpcode(id) == MirOpcode::Phi) phis.push_back(id);
        }
    }
    return phis;
}

// Capture the SORTED multiset of Const incoming values of a phi (read
// from the module the phi belongs to). Called on the SOURCE callee phi
// BEFORE inlining — `runInlining` produces a NEW Mir (the source phi's id
// becomes stale across the module boundary), so we capture plain data
// here and compare the CLONE against it after.
std::vector<std::int64_t> captureConstPhiValues(Mir const& m, MirInstId phi) {
    std::vector<std::int64_t> vs;
    for (MirPhiIncoming const& inc : m.phiIncomings(phi)) {
        if (m.instOpcode(inc.value) != MirOpcode::Const) continue;
        vs.push_back(std::get<std::int64_t>(
            m.literalValue(m.constLiteralIndex(inc.value)).value));
    }
    std::sort(vs.begin(), vs.end());
    return vs;
}

// THE MANDATORY STRUCTURAL PAIRING ASSERTION (mirrors the return-merge
// proof at MultiBlockLeafCalleeIsInlined). Given the CLONE callee phi (in
// the caller after inlining) and the SOURCE phi's captured Const-value
// multiset (`srcConstVals`, from `captureConstPhiValues` pre-inline),
// assert the clone's {(value, pred)} incoming set matches the source's
// under the value/block remap. The clone is a CFG isomorphism (callee
// blocks 1:1), so we check the load-bearing pairing invariant the verifier
// cannot see — for fixtures whose phi incomings carry DISTINCT Const
// values (one per arm):
//   (a) same incoming count (count == captured value count);
//   (b) the clone's Const-value multiset equals the source's captured one
//       (no value dropped or fabricated);
//   (c) THE PAIRING — each clone incoming's Const value is DEFINED IN ITS
//       OWN pred block (instBlock(value) == pred), EXACTLY as the source
//       phi paired them. A transposition (value of arm A attributed to arm
//       B's pred) makes instBlock(value) != pred for the swapped edges →
//       RED, while the verifier stays green.
void assertConstArmedPhiClonePairing(
    Mir const& cloned, MirInstId clonePhi,
    std::vector<std::int64_t> const& srcConstVals) {
    auto const cloneIncs = cloned.phiIncomings(clonePhi);
    ASSERT_EQ(cloneIncs.size(), srcConstVals.size())
        << "the cloned callee phi must have the SAME incoming count as the "
           "source callee phi";

    // (b) identical multiset of incoming Const values.
    auto const cloneVals = captureConstPhiValues(cloned, clonePhi);
    EXPECT_EQ(cloneVals, srcConstVals)
        << "the cloned phi's incoming Const values must match the source's "
           "(no value dropped or fabricated by the clone)";

    // (c) THE PAIRING INVARIANT — each clone incoming's value is defined in
    // its OWN pred block, EXACTLY as the source phi pairs them. This is the
    // ONLY thing that catches a value↔pred transposition the verifier can't.
    bool pairingHolds = true;
    for (MirPhiIncoming const& inc : cloneIncs) {
        if (cloned.instBlock(inc.value).v != inc.pred.v) pairingHolds = false;
    }
    EXPECT_TRUE(pairingHolds)
        << "STRUCTURAL PAIRING: each cloned-callee-phi incoming value must be "
           "defined in its OWN predecessor block — a value↔pred transposition "
           "(which the MirVerifier provably cannot catch when both values "
           "dominate both preds) breaks this and goes RED here";
}

// Build a callee `f(int x)` that contains BOTH a join-Phi AND two returns
// — so a callee JOIN-PHI and the inserted RETURN-MERGE Phi coexist in one
// splice (proving they compose via disjoint maps). Shape (Phi-free of any
// loop; a value-producing ternary join then an early-return diamond):
//
//   entry:  br tjoin                       (unconditional, x is Arg 0)
//   tA:     (unreachable in practice; we build the join directly)
//
// We hand-build the canonical ternary-join then a CondBr diamond:
//   entry:   cbr x?  -> ta, tb
//   ta:      va = 7;  br join
//   tb:      vb = 9;  br join
//   join:    t = phi [va, ta], [vb, tb]      ← the CALLEE JOIN-PHI
//            cbr x?  -> r1, r2
//   r1:      return t                         ← return edge 1 (value t)
//   r2:      return 100                       ← return edge 2 (const 100)
//
// After inlining into `main` (which calls f(...)): the join-phi is
// recloned (deferred flush) AND a 2-incoming return-merge Phi joins
// (clone-of-t, r1-clone) + (100, r2-clone). DISJOINT: the join-phi's
// incomings are {7,9}; the return-merge's are {t, 100}.
Mir buildJoinPhiPlusTwoReturnsModule(TypeInterner& interner) {
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {i32};
    TypeId const fSig  = interner.fnSig(params, i32, CallConv::CcSysV);
    TypeId const mainSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    // f(int x): ternary-join then early-return diamond (markers
    // derivation-consistent: join is a real IfJoin reached by both arms;
    // the two return arms make their ipdom the virtual exit → IfThen/
    // IfElse). The post-inline re-derivation re-stamps regardless.
    mb.addFunction(fSig, SymbolId{50});
    MirBlockId const fEntry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const fTa    = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const fTb    = mb.createBlock(StructCfMarker::IfElse);
    MirBlockId const fJoin  = mb.createBlock(StructCfMarker::IfJoin);
    MirBlockId const fR1    = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const fR2    = mb.createBlock(StructCfMarker::IfElse);

    mb.beginBlock(fEntry);
    MirInstId const x    = mb.addArg(0, i32);
    MirInstId const zero = mb.addConst(i32Lit(0), i32);
    MirInstId const ne0  = mb.addInst(MirOpcode::ICmpNe, std::array{x, zero}, boolT);
    mb.addCondBr(ne0, fTa, fTb);

    mb.beginBlock(fTa);
    MirInstId const va = mb.addConst(i32Lit(7), i32);
    mb.addBr(fJoin);

    mb.beginBlock(fTb);
    MirInstId const vb = mb.addConst(i32Lit(9), i32);
    mb.addBr(fJoin);

    mb.beginBlock(fJoin);
    std::array<MirPhiIncoming, 2> tIncs{
        MirPhiIncoming{va, fTa},
        MirPhiIncoming{vb, fTb},
    };
    MirInstId const t = mb.addPhi(i32, tIncs);  // THE callee join-phi
    // second condition reuses x (still nonzero-test) → CondBr to r1/r2.
    MirInstId const zero2 = mb.addConst(i32Lit(0), i32);
    MirInstId const ne1   = mb.addInst(MirOpcode::ICmpNe, std::array{x, zero2}, boolT);
    mb.addCondBr(ne1, fR1, fR2);

    mb.beginBlock(fR1);
    mb.addReturn(t);                              // return edge 1: value = t

    mb.beginBlock(fR2);
    mb.addReturn(mb.addConst(i32Lit(100), i32));  // return edge 2: const 100

    // main(): return f(1)
    mb.addFunction(mainSig, SymbolId{100});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    MirInstId const fAddr = mb.addGlobalAddr(SymbolId{50}, fSig);
    MirInstId const arg1  = mb.addConst(i32Lit(1), i32);
    MirInstId const callOps[] = {fAddr, arg1};
    MirInstId const call = mb.addInst(MirOpcode::Call, callOps, i32);
    mb.addReturn(call);
    return std::move(mb).finish();
}

} // namespace

// ── MANDATORY SHAPE (1): a callee JOIN-PHI and the RETURN-MERGE Phi
// COEXIST in one splice and compose (disjoint maps). ─────────────────
// The callee has a value-producing ternary (a join-Phi `t` over {7,9})
// AND two returns (so the splice ALSO inserts a return-merge Phi over
// {t, 100}). After inlining: BOTH phis are present in main, the cloned
// JOIN-phi is correctly value↔pred paired (the structural assertion),
// and the module verifies. RED-on-disable: dropping the clone-loop Phi
// arm makes the join-phi hit emitCalleeInst's defensive abort; mis-wiring
// the join-phi incomings breaks the structural pairing assertion below.
TEST(Inlining, CalleePhiJoinAndReturnMergeCoexist) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildJoinPhiPlusTwoReturnsModule(interner);

    // Capture the SOURCE callee join-phi's Const incoming values (the only
    // Phi in f, sym 50) BEFORE inlining — runInlining makes a NEW Mir, so
    // the source phi id is stale across the boundary; we compare the clone
    // against this captured data.
    auto const srcPhis = phisInFuncBySymbol(mir, 50);
    ASSERT_EQ(srcPhis.size(), 1u) << "f must hold exactly one join-phi pre-inline";
    auto const srcJoinVals = captureConstPhiValues(mir, srcPhis[0]);
    ASSERT_EQ(srcJoinVals.size(), 2u) << "the join-phi has two Const arms {7,9}";
    // The un-inlined module is itself verifier-valid.
    {
        DiagnosticReporter pre;
        MirVerifier preV{mir, &interner};
        ASSERT_TRUE(preV.verify(pre))
            << "the join-phi callee module must be valid pre-inlining";
    }
    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);
    ASSERT_EQ(countOpInModule(mir, MirOpcode::Phi), 1u)
        << "before: only f's join-phi exists";

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 1u)
        << "the Phi-bearing multi-block callee MUST now inline";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 0u)
        << "main's Call must be replaced by the spliced Phi-bearing body";

    // BOTH phis coexist in main: the cloned join-phi AND the return-merge
    // Phi. f's own join-phi still exists (not deleted — DCE's job), so the
    // module-wide Phi count is 3 (f's original 1 + main's cloned 2).
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Phi), 3u)
        << "f's original join-phi (1) + main's {cloned join-phi, return-merge} "
           "(2) = 3 — the two deferred mechanisms compose via disjoint maps";
    auto const mainPhis = phisInFuncBySymbol(mir, 100);
    ASSERT_EQ(mainPhis.size(), 2u)
        << "main holds BOTH the cloned join-phi AND the return-merge phi";

    // Identify the CLONED join-phi in main (the one whose incomings are the
    // {7,9} arms — the return-merge's incomings are {t, 100}). The cloned
    // join-phi has BOTH incoming values being Const (7 and 9); the return-
    // merge has one Phi-typed incoming (the cloned t) + one Const (100).
    MirInstId clonedJoinPhi{};
    for (MirInstId const p : mainPhis) {
        auto const incs = mir.phiIncomings(p);
        bool allConst = true;
        for (MirPhiIncoming const& inc : incs) {
            if (mir.instOpcode(inc.value) != MirOpcode::Const) { allConst = false; break; }
        }
        // The cloned join-phi's incomings are Const {7,9}; the return-merge
        // has a non-Const incoming (the cloned phi value t). Distinguish by
        // the {7,9} value set.
        if (allConst && incs.size() == 2) {
            std::int64_t a = std::get<std::int64_t>(
                mir.literalValue(mir.constLiteralIndex(incs[0].value)).value);
            std::int64_t b = std::get<std::int64_t>(
                mir.literalValue(mir.constLiteralIndex(incs[1].value)).value);
            if ((a == 7 && b == 9) || (a == 9 && b == 7)) clonedJoinPhi = p;
        }
    }
    ASSERT_TRUE(clonedJoinPhi.valid())
        << "the cloned callee join-phi (incomings {7,9}) must be in main";

    // ★ THE STRUCTURAL PAIRING PIN (the ONLY thing catching a transposition):
    // the cloned join-phi's (value,pred) pairing equals the source's.
    assertConstArmedPhiClonePairing(mir, clonedJoinPhi, srcJoinVals);

    // The return-merge Phi is the OTHER phi; assert it carries the cloned-t
    // and the 100 (it joins the two return edges).
    MirInstId returnMerge{};
    for (MirInstId const p : mainPhis) if (p.v != clonedJoinPhi.v) returnMerge = p;
    ASSERT_TRUE(returnMerge.valid());
    {
        auto const incs = mir.phiIncomings(returnMerge);
        ASSERT_EQ(incs.size(), 2u) << "two return edges → two return-merge incomings";
        bool sawHundred = false, sawPhiVal = false;
        for (MirPhiIncoming const& inc : incs) {
            if (mir.instOpcode(inc.value) == MirOpcode::Const) {
                EXPECT_EQ(std::get<std::int64_t>(
                    mir.literalValue(mir.constLiteralIndex(inc.value)).value), 100)
                    << "the const return edge carries 100";
                // The const-100 was defined in r2 → its own pred (r2-clone).
                EXPECT_EQ(mir.instBlock(inc.value).v, inc.pred.v)
                    << "the const-100 return edge: value defined in its own pred";
                sawHundred = true;
            } else if (mir.instOpcode(inc.value) == MirOpcode::Phi) {
                EXPECT_EQ(inc.value.v, clonedJoinPhi.v)
                    << "the value return edge carries the cloned join-phi t";
                // `t` (the cloned join-phi) is defined in the cloned-join
                // block and flows out through r1-clone — it DOMINATES its
                // pred but is NOT defined IN it (the return-merge value is a
                // dominating def, not a same-block def — the disjoint-maps
                // composition, not a transposition).
                EXPECT_NE(mir.instBlock(inc.value).v, inc.pred.v)
                    << "the cloned-join-phi return edge: t is defined in the "
                       "cloned join block, dominating (not equal to) its pred";
                sawPhiVal = true;
            }
        }
        EXPECT_TRUE(sawHundred) << "return-merge must include the const-100 edge";
        EXPECT_TRUE(sawPhiVal)
            << "return-merge must include the cloned-join-phi (t) edge — the "
               "two deferred mechanisms compose";
        // The two return edges arrive from DISTINCT cloned predecessor blocks.
        EXPECT_NE(incs[0].pred.v, incs[1].pred.v)
            << "the two return paths arrive from distinct cloned blocks";
    }

    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep))
        << "the join-phi + return-merge splice must verify clean";
}

// ── RED-ON-DISABLE (value-transposition → PAIRING-red, verifier STAYS
// GREEN). ────────────────────────────────────────────────────────────
// This DEMONSTRATES the verifier's documented blind spot. `Mir` is
// immutable post-`finish` (no incoming-mutation API), so — matching the
// suite idiom (test_mir_verifier builds the bad module directly) — we
// hand-build TWO diamond modules with a join-phi over {7,9} where BOTH
// constants are defined in the ENTRY block (a common dominator of both
// arms):
//   * CORRECT:    phi [7, then], [9, else]   (canonical `cond ? 7 : 9`)
//   * TRANSPOSED: phi [9, then], [7, else]   (values swapped; preds intact)
// Because both 7 and 9 dominate BOTH preds (defined in the entry), the
// verifier's phi-incoming dominance rule passes for EITHER pairing → the
// MirVerifier passes BOTH. Only the value↔pred PAIRING (the then-arm must
// carry 7) distinguishes them. This is the proof that the structural
// pairing assertion is the ONLY guard against a value-transposition
// miscompile — exactly the wiring a mis-remap in the deferred flush would
// produce when the merged values share a dominator.
namespace {
// Build a single-function diamond with a join-phi over two values BOTH
// DEFINED IN THE ENTRY block (a common dominator of both arms), so the
// verifier's phi-incoming dominance rule (defBlock must dominate the
// incoming pred) is satisfied for EITHER value↔pred pairing — that is the
// precondition for the blind spot. `transpose` swaps which arm each value
// is attributed to (the preds stay {then,else}). Returns module + phi id.
// To distinguish the two arms STRUCTURALLY we tag each value with a
// distinct per-arm marker instruction in its arm and feed the marker into
// the phi value via an Add against the entry const — no: simpler — we make
// the phi VALUES be entry consts {7,9} (dominating) and recover the
// intended pairing from a per-arm WITNESS map the test captures. Here the
// pairing invariant we assert is the SOURCE-INTENT one: arm `then` is
// meant to carry 7, arm `else` 9 (the canonical `cond ? 7 : 9`).
Mir buildDiamondPhiModule(TypeInterner& interner, bool transpose,
                          MirInstId& phiOut, MirBlockId& thenOut,
                          MirBlockId& elseOut, std::int64_t& thenValOut) {
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {i32};
    TypeId const fSig  = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const thenB = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const elseB = mb.createBlock(StructCfMarker::IfElse);
    MirBlockId const join  = mb.createBlock(StructCfMarker::IfJoin);
    mb.beginBlock(entry);
    MirInstId const x     = mb.addArg(0, i32);
    MirInstId const zero  = mb.addConst(i32Lit(0), i32);
    // BOTH phi values are defined in the ENTRY (dominates both arms) → a
    // transposition keeps both dominating, so the verifier CANNOT catch it.
    MirInstId const seven = mb.addConst(i32Lit(7), i32);
    MirInstId const nine  = mb.addConst(i32Lit(9), i32);
    MirInstId const ne    = mb.addInst(MirOpcode::ICmpNe, std::array{x, zero}, boolT);
    mb.addCondBr(ne, thenB, elseB);
    mb.beginBlock(thenB);
    mb.addBr(join);
    mb.beginBlock(elseB);
    mb.addBr(join);
    mb.beginBlock(join);
    // The canonical intent: `then`→7, `else`→9. TRANSPOSED swaps them.
    std::array<MirPhiIncoming, 2> incs =
        transpose
            ? std::array<MirPhiIncoming, 2>{MirPhiIncoming{nine,  thenB},
                                            MirPhiIncoming{seven, elseB}}
            : std::array<MirPhiIncoming, 2>{MirPhiIncoming{seven, thenB},
                                            MirPhiIncoming{nine,  elseB}};
    phiOut = mb.addPhi(i32, incs);
    mb.addReturn(phiOut);
    thenOut = thenB;
    elseOut = elseB;
    thenValOut = transpose ? 9 : 7;  // the value the `then` arm carries
    return std::move(mb).finish();
}

// THE PAIRING the verifier cannot see: in the canonical `cond ? 7 : 9`,
// the `then`-arm pred must carry 7 (and `else` carries 9). Returns true
// iff the phi pairs `then`→7. A transposition flips this to `then`→9 while
// the verifier stays green (both consts dominate from the entry).
bool diamondPairingHolds(Mir const& m, MirInstId phi, MirBlockId thenB) {
    for (MirPhiIncoming const& inc : m.phiIncomings(phi)) {
        if (inc.pred.v != thenB.v) continue;
        std::int64_t const v = std::get<std::int64_t>(
            m.literalValue(m.constLiteralIndex(inc.value)).value);
        return v == 7;  // the `then` arm must carry 7 in the canonical intent
    }
    return false;
}
} // namespace

TEST(Inlining, CalleePhiValueTranspositionIsCaughtOnlyStructurally) {
    TypeInterner interner{CompilationUnitId{1}};

    MirInstId goodPhi{}, badPhi{};
    MirBlockId gThen{}, gElse{}, bThen{}, bElse{};
    std::int64_t gThenVal = 0, bThenVal = 0;
    Mir good = buildDiamondPhiModule(interner, /*transpose=*/false, goodPhi,
                                     gThen, gElse, gThenVal);
    Mir bad  = buildDiamondPhiModule(interner, /*transpose=*/true,  badPhi,
                                     bThen, bElse, bThenVal);

    // (1) BOTH modules verify GREEN — the verifier provably cannot tell the
    // transposition apart: both 7 and 9 are defined in the ENTRY (a common
    // dominator), so each dominates BOTH preds regardless of pairing, and
    // the pred set is identical. This is the documented blind spot.
    {
        DiagnosticReporter rg, rb;
        MirVerifier vg{good, &interner}, vb{bad, &interner};
        EXPECT_TRUE(vg.verify(rg)) << "the correct diamond verifies";
        EXPECT_TRUE(vb.verify(rb))
            << "the TRANSPOSED diamond ALSO verifies — a value-transposition "
               "(right preds, swapped values, both dominating from the entry) "
               "is the documented blind spot the structural assertion covers";
    }

    // (2) The intended PAIRING (then-arm carries 7) holds for the CORRECT
    // module and is BROKEN for the transposed one — the ONLY signal of a
    // value-transposition miscompile, structural, not verifier-visible.
    EXPECT_TRUE(diamondPairingHolds(good, goodPhi, gThen))
        << "the correct phi pairs the then-arm with 7 (canonical intent)";
    EXPECT_FALSE(diamondPairingHolds(bad, badPhi, bThen))
        << "the transposed phi BREAKS the pairing (then-arm now carries 9) "
           "— caught ONLY by the value↔pred pairing, never by the verifier";
}

// ── MANDATORY SHAPE (2): a real LOOP-PHI callee inlines (the ONLY shape
// that exercises the DEFERRAL). ──────────────────────────────────────
// A join-phi resolves even under an immediate flush (all incoming values
// precede the phi in RPO). A LOOP header-phi does NOT: its back-edge
// incoming VALUE is defined in the latch, LATER in RPO — so flushing the
// incomings inside the clone loop would hit mapCalleeOperand's def-before-
// use abort. The deferral (flush AFTER the clone loop, when `local` is
// complete) is what makes the loop-phi resolve. We hand-build the MIR
// (a loop-phi is NOT frontend-reachable in release — Inlining runs before
// Mem2Reg, so the loop var is still an alloca, not a phi):
//
//   entry:   acc0 = 0;  br header
//   header:  acc = phi [acc0, entry], [acc1, latch]    ← THE LOOP-PHI
//            i   = phi [0, entry], [i1, latch]
//            c   = i < N ? ...                          (ICmpSlt vs N)
//            cbr c -> latch, exit
//   latch:   acc1 = acc + i;  i1 = i + 1;  br header    ← back-edge VALUES
//   exit:    return acc
//
// The header-phi's back-edge incoming `acc1` is DEFINED IN THE LATCH,
// which RPO visits AFTER the header → the deferral is mandatory.
// Asserts: inlines, verifier-clean (incl. layout rule), and the loop-phi's
// back-edge incoming is correctly remapped (the cloned acc1 in the cloned
// latch, paired with the cloned latch as pred — the structural pairing).
TEST(Inlining, LoopPhiCalleeInlinesBackEdgeRemapped) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {i32};
    TypeId const fSig  = interner.fnSig(params, i32, CallConv::CcSysV);
    TypeId const mainSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    // f(int n): a Phi-form counted loop summing 0..n-1 (hand-built SSA).
    // Markers are derivation-consistent: the header is a LoopHeader (rule
    // 2 — the latch is a pred it dominates); the exit is the target of the
    // header's loop-EXITING edge → LoopExit (rule 3); the latch (loop body)
    // is Linear. Mis-stamping the exit as Linear would fire I_StructCfMismatch
    // PRE-inlining; the post-inline re-derivation re-stamps the clone anyway.
    mb.addFunction(fSig, SymbolId{50});
    MirBlockId const fEntry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const fHeader = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const fLatch  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const fExit   = mb.createBlock(StructCfMarker::LoopExit);

    mb.beginBlock(fEntry);
    MirInstId const n     = mb.addArg(0, i32);
    MirInstId const acc0  = mb.addConst(i32Lit(0), i32);
    MirInstId const i0    = mb.addConst(i32Lit(0), i32);
    mb.addBr(fHeader);

    // header: two phis. Back-edge incomings (acc1, i1) are defined LATER
    // (in the latch) → supplied as deferred incomings now; the inliner's
    // clone must reproduce this deferral.
    mb.beginBlock(fHeader);
    MirInstId const accPhi = mb.addPhi(i32);   // acc = phi [acc0, entry],[acc1, latch]
    MirInstId const iPhi   = mb.addPhi(i32);   // i   = phi [i0,   entry],[i1,   latch]
    MirInstId const cond   = mb.addInst(MirOpcode::ICmpSlt, std::array{iPhi, n}, boolT);
    mb.addCondBr(cond, fLatch, fExit);

    mb.beginBlock(fLatch);
    MirInstId const acc1 = mb.addInst(MirOpcode::Add, std::array{accPhi, iPhi}, i32);
    MirInstId const one  = mb.addConst(i32Lit(1), i32);
    MirInstId const i1   = mb.addInst(MirOpcode::Add, std::array{iPhi, one}, i32);
    mb.addBr(fHeader);  // back edge

    mb.beginBlock(fExit);
    mb.addReturn(accPhi);

    // Wire the header phis' incomings (entry forward + latch back-edge).
    mb.addPhiIncoming(accPhi, MirPhiIncoming{acc0, fEntry});
    mb.addPhiIncoming(accPhi, MirPhiIncoming{acc1, fLatch});   // back-edge VALUE
    mb.addPhiIncoming(iPhi,   MirPhiIncoming{i0,   fEntry});
    mb.addPhiIncoming(iPhi,   MirPhiIncoming{i1,   fLatch});   // back-edge VALUE

    // main(): return f(4)
    mb.addFunction(mainSig, SymbolId{100});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    MirInstId const fAddr = mb.addGlobalAddr(SymbolId{50}, fSig);
    MirInstId const arg4  = mb.addConst(i32Lit(4), i32);
    MirInstId const callOps[] = {fAddr, arg4};
    MirInstId const call = mb.addInst(MirOpcode::Call, callOps, i32);
    mb.addReturn(call);
    Mir mir = std::move(mb).finish();

    // The un-inlined module (with real loop phis) is itself verifier-valid.
    {
        DiagnosticReporter pre;
        MirVerifier preV{mir, &interner};
        ASSERT_TRUE(preV.verify(pre))
            << "the loop-phi callee module must be valid pre-inlining";
    }
    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);
    ASSERT_EQ(countOpInModule(mir, MirOpcode::Phi), 2u)
        << "before: f's two header phis";

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 1u)
        << "the LOOP-PHI callee MUST inline (the deferral handles the back-"
           "edge value defined later in RPO)";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 0u)
        << "main's Call to the loop-phi callee must be spliced away";

    // f's two phis stay (un-deleted) + main gains 2 cloned header phis = 4.
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Phi), 4u)
        << "f's 2 header phis + main's 2 cloned header phis = 4";

    // ★ THE DEFERRAL PIN: the cloned loop-phi's back-edge incoming is
    // correctly remapped. For each cloned header phi, find its back-edge
    // incoming (the one whose value is an `Add`, defined in the cloned
    // latch) and assert that value is DEFINED IN ITS OWN pred (the cloned
    // latch). A flush that resolved the back-edge value too early (inside
    // the clone loop) would have aborted; a flush that mis-paired it would
    // break instBlock(value) == pred here.
    {
        MirFuncId const mainFn = funcBySymbol(mir, 100);
        ASSERT_TRUE(mainFn.valid());
        auto const clonedPhis = phisInFuncBySymbol(mir, 100);
        ASSERT_EQ(clonedPhis.size(), 2u);
        std::size_t backEdgesChecked = 0;
        for (MirInstId const p : clonedPhis) {
            auto const incs = mir.phiIncomings(p);
            ASSERT_EQ(incs.size(), 2u)
                << "each cloned header phi has entry + back-edge incomings";
            for (MirPhiIncoming const& inc : incs) {
                // EVERY incoming (forward AND back-edge) must be defined in
                // its own predecessor block — the load-bearing pairing.
                EXPECT_EQ(mir.instBlock(inc.value).v, inc.pred.v)
                    << "cloned loop-phi incoming value must be defined in its "
                       "OWN pred (back-edge value lives in the cloned latch)";
                if (mir.instOpcode(inc.value) == MirOpcode::Add) {
                    // This is the back-edge incoming — its pred (the cloned
                    // latch) must be the block holding the Add.
                    ++backEdgesChecked;
                }
            }
        }
        EXPECT_EQ(backEdgesChecked, 2u)
            << "both cloned header phis must carry an Add-valued back-edge "
               "incoming (acc1, i1) — the deferral resolved them";
    }

    // Verifier-clean INCLUDING the layout rule — the cloned loop body is
    // laid out topologically (C1 by-construction) and the loop-phi back-edge
    // is the only layout-exempt cross-edge.
    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep))
        << "the loop-phi splice must verify clean WITH the layout rule";
    EXPECT_EQ(countCode(rep, DiagnosticCode::I_LayoutUseBeforeDef), 0u)
        << "the cloned loop body must leave NO layout violation";
    EXPECT_EQ(rep.errorCount(), 0u) << "zero diagnostics of any kind";
}

// ── RED-ON-DISABLE (pred mis-remap/drop → VERIFIER-red). ─────────────
// The SPLIT's other half: a pred mis-remap (the flush keys each incoming
// pred via `calleeBlockMap`; a WRONG mapping yields a pred that is not in
// the phi-block's CFG predecessor set) goes VERIFIER-red — distinct from a
// value-transposition (structural-only, proven above). `Mir` is immutable
// post-`finish`, so (suite idiom) we hand-build a loop module two ways:
//   * CORRECT: the header phi's back-edge pred is the latch (a real pred).
//   * MIS-REMAPPED: the back-edge pred is an ORPHAN block that does NOT
//     branch to the header — exactly what a wrong calleeBlockMap lookup
//     would produce. The verifier's phi-pred-in-CFG check (I_PhiPredNotInCfg)
//     must fire.
namespace {
// Build a loop module whose header phi's back-edge pred is the `latch`
// (a REAL predecessor of the header) if `correct`, else the `exit` block
// — a REACHABLE block that does NOT branch to the header, so naming it as
// the phi's back-edge pred is exactly a pred mis-remap (a wrong
// calleeBlockMap lookup in the flush). Using a reachable block (not an
// orphan) isolates the failure to I_PhiPredNotInCfg, NOT I_UnreachableBlock.
Mir buildLoopPhiModule(TypeInterner& interner, bool correct) {
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {i32};
    TypeId const fSig  = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const latch  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const exit   = mb.createBlock(StructCfMarker::LoopExit);

    mb.beginBlock(entry);
    MirInstId const n    = mb.addArg(0, i32);
    MirInstId const acc0 = mb.addConst(i32Lit(0), i32);
    MirInstId const i0   = mb.addConst(i32Lit(0), i32);
    mb.addBr(header);
    mb.beginBlock(header);
    MirInstId const accPhi = mb.addPhi(i32);
    MirInstId const iPhi   = mb.addPhi(i32);
    MirInstId const cond   = mb.addInst(MirOpcode::ICmpSlt, std::array{iPhi, n}, boolT);
    mb.addCondBr(cond, latch, exit);
    mb.beginBlock(latch);
    MirInstId const acc1 = mb.addInst(MirOpcode::Add, std::array{accPhi, iPhi}, i32);
    MirInstId const one  = mb.addConst(i32Lit(1), i32);
    MirInstId const i1   = mb.addInst(MirOpcode::Add, std::array{iPhi, one}, i32);
    mb.addBr(header);  // the only real back-edge into `header`
    mb.beginBlock(exit);
    mb.addReturn(accPhi);

    // CORRECT: back-edge pred = latch (a real predecessor). MIS-REMAP:
    // back-edge pred = exit (reachable, but NOT a predecessor of header).
    MirBlockId const backPred = correct ? latch : exit;
    mb.addPhiIncoming(accPhi, MirPhiIncoming{acc0, entry});
    mb.addPhiIncoming(accPhi, MirPhiIncoming{acc1, backPred});  // mis-remap if !correct
    mb.addPhiIncoming(iPhi,   MirPhiIncoming{i0,   entry});
    mb.addPhiIncoming(iPhi,   MirPhiIncoming{i1,   backPred});
    return std::move(mb).finish();
}
} // namespace

TEST(Inlining, CalleePhiPredMisremapGoesVerifierRed) {
    TypeInterner interner{CompilationUnitId{1}};

    Mir good = buildLoopPhiModule(interner, /*correct=*/true);
    Mir bad  = buildLoopPhiModule(interner, /*correct=*/false);

    // CORRECT back-edge pred (latch) verifies (control).
    {
        DiagnosticReporter v0;
        MirVerifier verifier{good, &interner};
        ASSERT_TRUE(verifier.verify(v0))
            << "the loop-phi with its real latch back-edge pred must verify";
    }

    // MIS-REMAPPED back-edge pred (orphan, not a real predecessor of the
    // header) must go RED — the verifier's phi-pred-in-CFG check fires.
    DiagnosticReporter vrep;
    MirVerifier verifier{bad, &interner};
    EXPECT_FALSE(verifier.verify(vrep))
        << "a phi back-edge pred mis-remap (pred = orphan, not a real "
           "predecessor) MUST be caught by the MirVerifier";
    EXPECT_GT(countCode(vrep, DiagnosticCode::I_PhiPredNotInCfg) +
                  countCode(vrep, DiagnosticCode::I_NotDominated),
              0u)
        << "the pred mis-remap goes VERIFIER-red (I_PhiPredNotInCfg / "
           "I_NotDominated) — distinct from the value-transposition path "
           "(structural-assert-red)";
}

// ── MANDATORY SHAPE (3, unit mirror of the runtime witness): an
// argument-keyed value-ternary callee inlines, Phi cloned + paired. ──
// pick(k){ return k ? 7 : 9; } — the EXACT corpus witness shape
// (examples/c-subset/phi_inline). Hand-built in its pre-Mem2Reg Phi form
// (a value-producing ternary lowers to a join-Phi). main calls pick(0).
// We assert the cloned Phi is correctly value↔pred paired (structural),
// so the post-inline fold over k==0 would read the `9` arm. The runtime
// exit (9) is pinned at the corpus tier; here we pin the WIRING.
TEST(Inlining, ArgKeyedTernaryPhiCalleeInlinesPaired) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {i32};
    TypeId const fSig  = interner.fnSig(params, i32, CallConv::CcSysV);
    TypeId const mainSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    // pick(int k): the ternary diamond `k ? 7 : 9`, exactly as hir_to_mir
    // lowers it (entry CondBr → then(7)/else(9) → join-phi → return phi).
    mb.addFunction(fSig, SymbolId{50});
    MirBlockId const pEntry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const pThen  = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const pElse  = mb.createBlock(StructCfMarker::IfElse);
    MirBlockId const pJoin  = mb.createBlock(StructCfMarker::IfJoin);
    mb.beginBlock(pEntry);
    MirInstId const k    = mb.addArg(0, i32);
    MirInstId const zero = mb.addConst(i32Lit(0), i32);
    MirInstId const ne   = mb.addInst(MirOpcode::ICmpNe, std::array{k, zero}, boolT);
    mb.addCondBr(ne, pThen, pElse);
    mb.beginBlock(pThen);
    MirInstId const seven = mb.addConst(i32Lit(7), i32);
    mb.addBr(pJoin);
    mb.beginBlock(pElse);
    MirInstId const nine = mb.addConst(i32Lit(9), i32);
    mb.addBr(pJoin);
    mb.beginBlock(pJoin);
    std::array<MirPhiIncoming, 2> incs{
        MirPhiIncoming{seven, pThen},
        MirPhiIncoming{nine,  pElse},
    };
    MirInstId const phi = mb.addPhi(i32, incs);
    mb.addReturn(phi);

    // main(): return pick(0)  (constant arg, but in the CALLEE not the
    // condition — the condition is the arg, so the callee Phi is real).
    mb.addFunction(mainSig, SymbolId{100});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    MirInstId const pAddr = mb.addGlobalAddr(SymbolId{50}, fSig);
    MirInstId const arg0  = mb.addConst(i32Lit(0), i32);
    MirInstId const callOps[] = {pAddr, arg0};
    MirInstId const call = mb.addInst(MirOpcode::Call, callOps, i32);
    mb.addReturn(call);
    Mir mir = std::move(mb).finish();

    auto const srcPhis = phisInFuncBySymbol(mir, 50);
    ASSERT_EQ(srcPhis.size(), 1u);
    auto const srcPhiVals = captureConstPhiValues(mir, srcPhis[0]);
    ASSERT_EQ(srcPhiVals.size(), 2u) << "pick's join-phi has Const arms {7,9}";
    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 1u)
        << "the arg-keyed value-ternary (Phi-bearing) callee MUST inline";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 0u);

    // The cloned ternary join-phi is the single return edge's value → the
    // return-merge Phi is ELIDED (1 return). So main holds exactly ONE phi:
    // the cloned join-phi. (Module-wide: pick's original + main's clone = 2.)
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Phi), 2u)
        << "pick's original join-phi + main's single cloned join-phi "
           "(return-merge elided — exactly one return) = 2";
    auto const mainPhis = phisInFuncBySymbol(mir, 100);
    ASSERT_EQ(mainPhis.size(), 1u)
        << "main holds exactly the cloned ternary join-phi (return-merge "
           "elided for the single-return callee)";

    // ★ STRUCTURAL PAIRING: the cloned join-phi's (value,pred) pairing
    // equals pick's source phi. This is what guarantees pick(0) folds to 9
    // (not 7) post-inline — the runtime witness's correctness, pinned at
    // the wiring tier.
    assertConstArmedPhiClonePairing(mir, mainPhis[0], srcPhiVals);

    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep))
        << "the arg-keyed ternary-phi splice must verify clean";
}

namespace {
// Build `f(int k, int a, int b) { return k ? a : b; }` where the ternary's
// phi VALUES are the callee's ARGS a,b (NOT arm-local consts), then main
// calls `f(0, 7, 9)`. After the arg-substituting splice, the cloned join-
// phi's incomings are main's consts {7,9}, BOTH defined in main's entry —
// a COMMON DOMINATOR of both spliced arms. That is the precondition for the
// verifier blind spot: a value↔pred transposition keeps each value
// dominating BOTH preds and leaves the pred set unchanged, so the
// MirVerifier's dominance/pred-in-CFG rules stay GREEN either way. (Also
// exercises mapCalleeOperand on an Arg-VALUED phi incoming — the one
// incoming kind the const-armed fixtures never present to it.)
Mir buildArgValuedTernaryModule(TypeInterner& interner) {
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {i32, i32, i32};
    TypeId const fSig    = interner.fnSig(params, i32, CallConv::CcSysV);
    TypeId const mainSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    // f(int k, int a, int b): entry CondBr(k!=0)->then/else (both empty,
    // br join); join phi [a, then],[b, else]; return phi. The phi VALUES
    // are the Args a,b — entry-dominating, so post-splice (a→7, b→9) the
    // cloned phi's values land in main's entry (common dominator).
    mb.addFunction(fSig, SymbolId{50});
    MirBlockId const fEntry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const fThen  = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const fElse  = mb.createBlock(StructCfMarker::IfElse);
    MirBlockId const fJoin  = mb.createBlock(StructCfMarker::IfJoin);
    mb.beginBlock(fEntry);
    MirInstId const k    = mb.addArg(0, i32);
    MirInstId const a    = mb.addArg(1, i32);   // then-arm intent value
    MirInstId const b    = mb.addArg(2, i32);   // else-arm intent value
    MirInstId const zero = mb.addConst(i32Lit(0), i32);
    MirInstId const ne   = mb.addInst(MirOpcode::ICmpNe, std::array{k, zero}, boolT);
    mb.addCondBr(ne, fThen, fElse);
    mb.beginBlock(fThen);
    mb.addBr(fJoin);
    mb.beginBlock(fElse);
    mb.addBr(fJoin);
    mb.beginBlock(fJoin);
    std::array<MirPhiIncoming, 2> incs{
        MirPhiIncoming{a, fThen},   // SOURCE INTENT: then-arm carries a
        MirPhiIncoming{b, fElse},   // SOURCE INTENT: else-arm carries b
    };
    MirInstId const phi = mb.addPhi(i32, incs);
    mb.addReturn(phi);

    // main(): return f(0, 7, 9). The actuals (consts in main's entry)
    // become the cloned phi's incoming VALUES → common-dominator armed.
    mb.addFunction(mainSig, SymbolId{100});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    MirInstId const fAddr = mb.addGlobalAddr(SymbolId{50}, fSig);
    MirInstId const a0 = mb.addConst(i32Lit(0), i32);  // k = 0
    MirInstId const a1 = mb.addConst(i32Lit(7), i32);  // a = 7 (then intent)
    MirInstId const a2 = mb.addConst(i32Lit(9), i32);  // b = 9 (else intent)
    MirInstId const callOps[] = {fAddr, a0, a1, a2};
    MirInstId const call = mb.addInst(MirOpcode::Call, callOps, i32);
    mb.addReturn(call);
    return std::move(mb).finish();
}

// The (single) CondBr's [ifTrue, ifFalse] successors in function `sym`.
// After inlining the spliced diamond retains exactly one CondBr; its true
// edge reaches the then-arm clone, its false edge the else-arm clone — the
// ONLY way to label two otherwise-identical empty arms once block ids are
// fresh and StructCfMarkers are re-derived. `found=false` if absent.
struct CondBrArms { MirBlockId ifTrue{}, ifFalse{}; bool found = false; };
CondBrArms condBrArmsInFunc(Mir const& mir, std::uint32_t sym) {
    MirFuncId const f = funcBySymbol(mir, sym);
    if (!f.valid()) return {};
    std::uint32_t const nb = mir.funcBlockCount(f);
    for (std::uint32_t bi = 0; bi < nb; ++bi) {
        MirBlockId const blk  = mir.funcBlockAt(f, bi);
        MirInstId const  term = mir.blockTerminator(blk);
        if (mir.instOpcode(term) != MirOpcode::CondBr) continue;
        auto const succ = mir.blockSuccessors(blk);
        if (succ.size() != 2) continue;
        return {succ[0], succ[1], true};
    }
    return {};
}
} // namespace

// ── MANDATORY SHAPE (4): the value↔pred pairing is the SOLE red-on-disable
// guard THROUGH the real inliner on a VERIFIER-BLIND shape. ───────────────
// The arm-local-const fixtures (ArgKeyedTernary, JoinPhiPlusTwoReturns) pin
// the pairing via assertConstArmedPhiClonePairing's instBlock(value)==pred
// proxy — but a transposition THERE is ALSO verifier-caught (each const
// lives in its own arm, so a swap breaks dominance), making the structural
// pin belt-and-suspenders. The standalone diamond
// (CalleePhiValueTranspositionIsCaughtOnlyStructurally) shows the assertion
// catches a verifier-INVISIBLE swap — but on a HAND-BUILT module, never
// through runInlining. THIS unifies them: an Arg-valued ternary whose cloned
// phi values land in a COMMON DOMINATOR (main's entry) after the real splice
// of f(0,7,9). A transposition here keeps both values dominating both preds
// → the verifier stays GREEN, so the source-intent pairing (then→7, else→9)
// asserted below — navigated via the spliced CondBr, since instBlock(value)
// ==pred cannot apply when values live in a common dominator — is the ONLY
// thing that would catch it. RED-on-disable: a swapped flush flips thenVal
// to 9 (EXPECT_EQ(thenVal,7) fails) while verify() stays true. (The
// mechanism reads value and pred from one `inc` so cannot transpose by
// construction; this proves that end-to-end on the exact shape where nothing
// else guards it.)
TEST(Inlining, CalleePhiCommonDominatorArmsPairedThroughInliner) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildArgValuedTernaryModule(interner);

    // The SOURCE phi's incoming VALUES are callee Args (entry-dominating) —
    // the blind-spot precondition + the Arg-valued-incoming coverage.
    auto const srcPhis = phisInFuncBySymbol(mir, 50);
    ASSERT_EQ(srcPhis.size(), 1u) << "f holds exactly one join-phi pre-inline";
    {
        auto const srcIncs = mir.phiIncomings(srcPhis[0]);
        ASSERT_EQ(srcIncs.size(), 2u);
        for (MirPhiIncoming const& inc : srcIncs)
            EXPECT_EQ(mir.instOpcode(inc.value), MirOpcode::Arg)
                << "the source phi's incoming values are callee Args";
    }
    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 1u)
        << "the Arg-VALUED ternary-phi callee MUST inline";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 0u);
    // f's original join-phi + main's single cloned join-phi (return-merge
    // elided — exactly one return) = 2.
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Phi), 2u);

    auto const mainPhis = phisInFuncBySymbol(mir, 100);
    ASSERT_EQ(mainPhis.size(), 1u)
        << "main holds exactly the cloned join-phi (return-merge elided)";
    auto const incs = mir.phiIncomings(mainPhis[0]);
    ASSERT_EQ(incs.size(), 2u);

    // VERIFIER-BLIND precondition: both cloned incoming values live in ONE
    // block (main's entry, a common dominator) → each dominates BOTH preds,
    // so a transposition is invisible to the dominance rule.
    EXPECT_EQ(mir.instBlock(incs[0].value).v, mir.instBlock(incs[1].value).v)
        << "both cloned phi values share one common-dominator block — the "
           "precondition that makes a transposition VERIFIER-INVISIBLE";
    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep))
        << "the common-dominator-armed splice verifies GREEN — the verifier "
           "is BLIND to which value pairs with which pred here";

    // ★ THE SOLE GUARD: label the arms via the spliced CondBr (ifTrue=then,
    // ifFalse=else), then assert the source intent (then→7, else→9) survived
    // the clone. A transposition flips these while verify() stays true.
    CondBrArms const arms = condBrArmsInFunc(mir, 100);
    ASSERT_TRUE(arms.found) << "the spliced diamond retains its CondBr";
    EXPECT_TRUE((arms.ifTrue.v == incs[0].pred.v && arms.ifFalse.v == incs[1].pred.v) ||
                (arms.ifTrue.v == incs[1].pred.v && arms.ifFalse.v == incs[0].pred.v))
        << "the CondBr's two edges are exactly the cloned phi's two preds";

    bool sawThen = false, sawElse = false;
    std::int64_t thenVal = 0, elseVal = 0;
    for (MirPhiIncoming const& inc : incs) {
        std::int64_t const v = std::get<std::int64_t>(
            mir.literalValue(mir.constLiteralIndex(inc.value)).value);
        if (inc.pred.v == arms.ifTrue.v)       { sawThen = true; thenVal = v; }
        else if (inc.pred.v == arms.ifFalse.v) { sawElse = true; elseVal = v; }
    }
    ASSERT_TRUE(sawThen && sawElse)
        << "each cloned phi incoming pred is one of the CondBr arms";
    EXPECT_EQ(thenVal, 7)
        << "SOURCE-INTENT PAIRING: the then-arm (CondBr ifTrue, clone of the "
           "arm carrying Arg a) must carry a's actual value 7 — a value↔pred "
           "transposition (verifier-GREEN here) flips this to 9";
    EXPECT_EQ(elseVal, 9)
        << "SOURCE-INTENT PAIRING: the else-arm (CondBr ifFalse, clone of the "
           "arm carrying Arg b) must carry b's actual value 9";
}

// ── D-CSUBSET-COMPUTED-GOTO inlining gate + routing (3 pins) ──────────────────

// CALLEE direction (MF-4): a callee that contains computed goto (BlockAddress +
// IndirectBr) is NEVER inlined — block renumbering would invalidate the &&label
// symbols + the IndirectBr successor set. RED-ON-DISABLE: drop the BlockAddress/
// IndirectBr arms from inlineLegalityGate.
TEST(Inlining, ComputedGotoCalleeWithIndirectBrIsNotInlined) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const vptr  = interner.pointer(interner.primitive(TypeKind::Void));
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    // callee f (SymbolId 50): a tiny computed-goto function.
    mb.addFunction(fnSig, SymbolId{50});
    MirBlockId const fEntry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const fTarget = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(fEntry);
    MirInstId const fba = mb.addBlockAddress(fTarget, vptr);
    std::array<MirBlockId, 1> fsuccs{fTarget};
    mb.addIndirectBr(fba, fsuccs);
    mb.beginBlock(fTarget);
    mb.addReturn(mb.addConst(i32Lit(7), i32));

    // main (SymbolId 100): calls f.
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const cmEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(cmEntry);
    MirInstId const cAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
    std::array<MirInstId, 1> cOps{cAddr};
    mb.addReturn(mb.addInst(MirOpcode::Call, cOps, i32));
    Mir mir = std::move(mb).finish();

    ASSERT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);
    ASSERT_EQ(countOpInModule(mir, MirOpcode::IndirectBr), 1u);
    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 0u)
        << "a computed-goto callee (BlockAddress + IndirectBr) MUST NOT be inlined";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 1u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::IndirectBr), 1u);
}

// CALLER direction (the pr-review's silent-miscompile catch): a HOST that itself
// contains computed goto AND calls a MULTI-BLOCK helper must route to the single-
// block rebuilder, NEVER the MultiBlockInliner (whose caller-host emit mis-copies
// the BlockAddress block-id payload + aborts on IndirectBr). RED-ON-DISABLE: drop
// the `functionHasComputedGoto` routing guard and runInlining aborts/corrupts.
TEST(Inlining, ComputedGotoHostWithMultiBlockCalleeRoutesSafely) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const vptr  = interner.pointer(interner.primitive(TypeKind::Void));
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    // MULTI-block inline-eligible helper (SymbolId 50): cond-branch to two returns.
    mb.addFunction(fnSig, SymbolId{50});
    MirBlockId const hEntry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const hThen  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const hElse  = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(hEntry);
    mb.addCondBr(mb.addConst(i32Lit(1), i32), hThen, hElse);
    mb.beginBlock(hThen);
    mb.addReturn(mb.addConst(i32Lit(3), i32));
    mb.beginBlock(hElse);
    mb.addReturn(mb.addConst(i32Lit(4), i32));

    // computed-goto HOST main (SymbolId 100): BlockAddress + IndirectBr + a call.
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const mmEntry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const mmTarget = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(mmEntry);
    MirInstId const mAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
    std::array<MirInstId, 1> mOps{mAddr};
    mb.addInst(MirOpcode::Call, mOps, i32);
    MirInstId const mba = mb.addBlockAddress(mmTarget, vptr);
    std::array<MirBlockId, 1> msuccs{mmTarget};
    mb.addIndirectBr(mba, msuccs);
    mb.beginBlock(mmTarget);
    mb.addReturn(mb.addConst(i32Lit(9), i32));
    Mir mir = std::move(mb).finish();

    ASSERT_EQ(countOpInModule(mir, MirOpcode::IndirectBr), 1u);
    ASSERT_EQ(countOpInModule(mir, MirOpcode::BlockAddress), 1u);
    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
    EXPECT_TRUE(r.ok)
        << "inlining a multi-block callee INTO a computed-goto host must not abort";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::IndirectBr), 1u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::BlockAddress), 1u);
    EXPECT_EQ(r.callsInlined, 0u)
        << "the multi-block helper is NOT inlined into the computed-goto host "
           "(follow-up D-CG-INLINE-MULTIBLOCK-INTO-COMPUTED-GOTO-HOST)";
}

// The single-block-callee-INTO-host case (self-audit gap pin): the host routes to
// the single-block rebuilder, which DOES inline a single-block-leaf callee
// (callsInlined==1) while keeping the host's BlockAddress + IndirectBr intact
// across the renumber.
TEST(Inlining, ComputedGotoHostInlinesSingleBlockCalleeAndKeepsGoto) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const vptr  = interner.pointer(interner.primitive(TypeKind::Void));
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    // SINGLE-block leaf helper (SymbolId 50): returns a constant (inline-eligible).
    mb.addFunction(fnSig, SymbolId{50});
    MirBlockId const sEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(sEntry);
    mb.addReturn(mb.addConst(i32Lit(5), i32));

    // computed-goto HOST main (SymbolId 100): BlockAddress + IndirectBr + a call.
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const smEntry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const smTarget = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(smEntry);
    MirInstId const sAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
    std::array<MirInstId, 1> sOps{sAddr};
    mb.addInst(MirOpcode::Call, sOps, i32);
    MirInstId const sba = mb.addBlockAddress(smTarget, vptr);
    std::array<MirBlockId, 1> ssuccs{smTarget};
    mb.addIndirectBr(sba, ssuccs);
    mb.beginBlock(smTarget);
    mb.addReturn(mb.addConst(i32Lit(9), i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runInlining(mir, interner, rep,
                                            opt::kMaxInlineThreshold);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 1u)
        << "a SINGLE-block leaf callee IS inlined into a computed-goto host";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Call), 0u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::IndirectBr), 1u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::BlockAddress), 1u);
}
