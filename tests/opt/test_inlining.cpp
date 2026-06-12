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
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_verifier.hpp"
#include "opt/optimizer.hpp"
#include "opt/passes/inlining.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <variant>

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
