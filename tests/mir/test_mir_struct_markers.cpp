// Pins for the canonical StructCfMarker derivation (D-OPT4-1 closure)
// and its post-dominator substrate:
//   - `computeMirPostDomTree` / `mirPostDominatesBlock` (mir_dom.hpp):
//     the reverse-graph Cooper-Harvey-Kennedy tree over a VIRTUAL exit,
//     with the THREE-valued ipdom (real / virtual / INVALID).
//   - `deriveStructCfMarkers` (mir_struct_markers.hpp): EXACT per-block
//     marker vectors over hand-built CFG shapes — including shapes NO
//     current frontend emits (multi-exit loop, multi-back-edge loop) so
//     the derivation is pinned as a CFG property, not a lowering echo.
//   - `rederiveStructCfMarkers`: the applier corrects arbitrary stale
//     stamps on a frozen Mir.
//
// THE SPEC lives in mir_struct_markers.hpp (priority order, first claim
// wins, dormant values). Each fixture's comment states the reasoning so
// a derivation change is debuggable from the test alone.

#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_dom.hpp"
#include "mir/mir_struct_markers.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

using namespace dss;

namespace {

MirLiteralValue i32Lit(std::int64_t v) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = TypeKind::I32;
    return lit;
}

// Collect the derived markers of `f`'s blocks, in function block order,
// as ints (gtest prints int vectors readably on failure).
std::vector<int> derivedVectorOf(Mir const& m, MirFuncId f) {
    auto const derived = deriveStructCfMarkers(m, f);
    std::vector<int> out;
    std::uint32_t const nb = m.funcBlockCount(f);
    for (std::uint32_t bi = 0; bi < nb; ++bi) {
        out.push_back(static_cast<int>(derived[m.funcBlockAt(f, bi).v]));
    }
    return out;
}

// Collect the STORED markers (for applier tests).
std::vector<int> storedVectorOf(Mir const& m, MirFuncId f) {
    std::vector<int> out;
    std::uint32_t const nb = m.funcBlockCount(f);
    for (std::uint32_t bi = 0; bi < nb; ++bi) {
        out.push_back(static_cast<int>(m.blockMarker(m.funcBlockAt(f, bi))));
    }
    return out;
}

std::vector<int> ints(std::initializer_list<StructCfMarker> ms) {
    std::vector<int> out;
    for (StructCfMarker const m : ms) out.push_back(static_cast<int>(m));
    return out;
}

} // namespace

// ── post-dominator units (computeMirPostDomTree) ────────────────────────────

// Diamond: ipdom(entry) = join (every path to exit passes the join);
// ipdom(arm) = join; ipdom(join) = the VIRTUAL exit (join Returns).
TEST(MirPostDom, DiamondIpdomIsJoin) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    MirFuncId const f = mb.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tArm  = mb.createBlock();
    MirBlockId const fArm  = mb.createBlock();
    MirBlockId const join  = mb.createBlock();
    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    mb.addCondBr(cond, tArm, fArm);
    mb.beginBlock(tArm); mb.addBr(join);
    mb.beginBlock(fArm); mb.addBr(join);
    mb.beginBlock(join);
    mb.addReturn(mb.addConst(i32Lit(0), i32));
    Mir m = std::move(mb).finish();

    auto const pd = computeMirPostDomTree(m, f);
    EXPECT_EQ(pd.virtualExitSlot(), static_cast<std::uint32_t>(m.blockCount()));
    EXPECT_EQ(pd.ipdom[entry.v].v, join.v);
    EXPECT_EQ(pd.ipdom[tArm.v].v,  join.v);
    EXPECT_EQ(pd.ipdom[fArm.v].v,  join.v);
    EXPECT_TRUE(pd.isVirtualExit(pd.ipdom[join.v]))
        << "the Returning join's ipdom is the virtual exit";
    // Tri-state walk: join post-dominates everything; an arm does not
    // post-dominate the entry (the other arm bypasses it).
    EXPECT_EQ(mirPostDominatesBlock(join, entry, pd), MirDomResult::Dominates);
    EXPECT_EQ(mirPostDominatesBlock(tArm, entry, pd), MirDomResult::DoesNot);
    EXPECT_EQ(mirPostDominatesBlock(join, join, pd),  MirDomResult::Dominates);
    // The virtual exit id post-dominates every reverse-reachable block.
    MirBlockId const virtualId{pd.virtualExitSlot(), m.id().v};
    EXPECT_EQ(mirPostDominatesBlock(virtualId, entry, pd), MirDomResult::Dominates);
}

// Both arms RETURN: paths diverge to DISTINCT exits → ipdom(entry) is
// the VIRTUAL exit (there is no real join).
TEST(MirPostDom, BothArmsReturnIpdomIsVirtual) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    MirFuncId const f = mb.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tArm  = mb.createBlock();
    MirBlockId const fArm  = mb.createBlock();
    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    mb.addCondBr(cond, tArm, fArm);
    mb.beginBlock(tArm); mb.addReturn(mb.addConst(i32Lit(7), i32));
    mb.beginBlock(fArm); mb.addReturn(mb.addConst(i32Lit(9), i32));
    Mir m = std::move(mb).finish();

    auto const pd = computeMirPostDomTree(m, f);
    ASSERT_TRUE(pd.ipdom[entry.v].valid());
    EXPECT_TRUE(pd.isVirtualExit(pd.ipdom[entry.v]))
        << "no real block joins the two returning arms";
    EXPECT_TRUE(pd.isVirtualExit(pd.ipdom[tArm.v]));
    EXPECT_TRUE(pd.isVirtualExit(pd.ipdom[fArm.v]));
}

// While loop: every path from the header to function exit passes the
// loop exit → ipdom(header) = exit.
TEST(MirPostDom, LoopHeaderIpdomIsLoopExit) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    MirFuncId const f = mb.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock();
    MirBlockId const body   = mb.createBlock();
    MirBlockId const exit   = mb.createBlock();
    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    mb.addBr(header);
    mb.beginBlock(header); mb.addCondBr(cond, body, exit);
    mb.beginBlock(body);   mb.addBr(header);
    mb.beginBlock(exit);   mb.addReturn(mb.addConst(i32Lit(0), i32));
    Mir m = std::move(mb).finish();

    auto const pd = computeMirPostDomTree(m, f);
    EXPECT_EQ(pd.ipdom[header.v].v, exit.v);
    EXPECT_EQ(pd.ipdom[body.v].v, header.v)
        << "the body's every path re-enters the header first";
    EXPECT_EQ(mirPostDominatesBlock(exit, entry, pd), MirDomResult::Dominates);
}

// Multi-exit loop: the two exits diverge → ipdom(header) is VIRTUAL.
TEST(MirPostDom, MultiExitLoopHeaderIpdomIsVirtual) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    MirFuncId const f = mb.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock();
    MirBlockId const body   = mb.createBlock();
    MirBlockId const exit1  = mb.createBlock();
    MirBlockId const exit2  = mb.createBlock();
    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    mb.addBr(header);
    mb.beginBlock(header); mb.addCondBr(cond, body, exit1);
    mb.beginBlock(body);   mb.addCondBr(cond, header, exit2);  // back-edge + 2nd exit
    mb.beginBlock(exit1);  mb.addReturn(mb.addConst(i32Lit(1), i32));
    mb.beginBlock(exit2);  mb.addReturn(mb.addConst(i32Lit(2), i32));
    Mir m = std::move(mb).finish();

    auto const pd = computeMirPostDomTree(m, f);
    EXPECT_TRUE(pd.isVirtualExit(pd.ipdom[header.v]))
        << "paths leave through exit1 OR exit2 — no real join";
    EXPECT_TRUE(pd.isVirtualExit(pd.ipdom[body.v]));
}

// Straight line: each block's ipdom is its unique successor; the last
// (Returning) block's ipdom is the virtual exit.
TEST(MirPostDom, StraightLineChainsToVirtual) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    MirFuncId const f = mb.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const b1    = mb.createBlock();
    MirBlockId const b2    = mb.createBlock();
    mb.beginBlock(entry); mb.addBr(b1);
    mb.beginBlock(b1);    mb.addBr(b2);
    mb.beginBlock(b2);    mb.addReturn(mb.addConst(i32Lit(0), i32));
    Mir m = std::move(mb).finish();

    auto const pd = computeMirPostDomTree(m, f);
    EXPECT_EQ(pd.ipdom[entry.v].v, b1.v);
    EXPECT_EQ(pd.ipdom[b1.v].v,    b2.v);
    EXPECT_TRUE(pd.isVirtualExit(pd.ipdom[b2.v]));
}

// Infinite loop (no path to ANY exit): the region is reverse-
// unreachable → ipdom is INVALID (the third value of the tri-state).
TEST(MirPostDom, InfiniteLoopRegionIpdomIsInvalid) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    MirFuncId const f = mb.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock();
    mb.beginBlock(entry);  mb.addBr(header);
    mb.beginBlock(header); mb.addBr(header);  // self-loop, no exit edge
    Mir m = std::move(mb).finish();

    auto const pd = computeMirPostDomTree(m, f);
    EXPECT_FALSE(pd.ipdom[entry.v].valid())
        << "no path from entry reaches any exit";
    EXPECT_FALSE(pd.ipdom[header.v].valid());
    // The walk treats reverse-unreachable as DoesNot, never aborts.
    MirBlockId const virtualId{pd.virtualExitSlot(), m.id().v};
    EXPECT_EQ(mirPostDominatesBlock(virtualId, header, pd), MirDomResult::DoesNot);
}

// Tri-state GaveUp arm: a malformed ipdom CYCLE (only constructible by
// direct struct assembly — the computation never emits one) trips the
// step cap instead of hanging. Mirrors MirDom's GaveUp pin.
TEST(MirPostDom, MalformedIpdomCycleGivesUpNotHangs) {
    MirPostDomTree pd;
    pd.virtualExit = 3;
    pd.ipdom.resize(4);
    pd.gaveUp.assign(4, 0);
    pd.ipdom[1] = MirBlockId{2, 1};
    pd.ipdom[2] = MirBlockId{1, 1};  // 1 ↔ 2 cycle
    EXPECT_EQ(mirPostDominatesBlock(MirBlockId{3, 1}, MirBlockId{1, 1}, pd),
              MirDomResult::GaveUp);
}

// ── derivation pins (exact per-block vectors) ───────────────────────────────

TEST(StructCfDerivation, StraightLine) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    MirFuncId const f = mb.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const b1    = mb.createBlock();
    mb.beginBlock(entry); mb.addBr(b1);
    mb.beginBlock(b1);    mb.addReturn(mb.addConst(i32Lit(0), i32));
    Mir m = std::move(mb).finish();

    EXPECT_EQ(derivedVectorOf(m, f),
              ints({StructCfMarker::EntryBlock, StructCfMarker::Linear}));
}

TEST(StructCfDerivation, IfNoElse) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    MirFuncId const f = mb.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const thenB = mb.createBlock();
    MirBlockId const join  = mb.createBlock();
    mb.beginBlock(entry);
    mb.addCondBr(mb.addArg(0, boolT), thenB, join);
    mb.beginBlock(thenB); mb.addBr(join);
    mb.beginBlock(join);  mb.addReturn(mb.addConst(i32Lit(0), i32));
    Mir m = std::move(mb).finish();

    // succs[1] == ipdom(entry) == join → no IfElse; join claims IfJoin.
    EXPECT_EQ(derivedVectorOf(m, f),
              ints({StructCfMarker::EntryBlock, StructCfMarker::IfThen,
                    StructCfMarker::IfJoin}));
}

TEST(StructCfDerivation, IfElseDiamond) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    MirFuncId const f = mb.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tArm  = mb.createBlock();
    MirBlockId const fArm  = mb.createBlock();
    MirBlockId const join  = mb.createBlock();
    mb.beginBlock(entry);
    mb.addCondBr(mb.addArg(0, boolT), tArm, fArm);
    mb.beginBlock(tArm); mb.addBr(join);
    mb.beginBlock(fArm); mb.addBr(join);
    mb.beginBlock(join); mb.addReturn(mb.addConst(i32Lit(0), i32));
    Mir m = std::move(mb).finish();

    EXPECT_EQ(derivedVectorOf(m, f),
              ints({StructCfMarker::EntryBlock, StructCfMarker::IfThen,
                    StructCfMarker::IfElse, StructCfMarker::IfJoin}));
}

// Both arms return → ipdom(entry) = virtual → arms marked, NO IfJoin
// anywhere in the function.
TEST(StructCfDerivation, BothArmsReturnIfHasNoJoin) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    MirFuncId const f = mb.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tArm  = mb.createBlock();
    MirBlockId const fArm  = mb.createBlock();
    mb.beginBlock(entry);
    mb.addCondBr(mb.addArg(0, boolT), tArm, fArm);
    mb.beginBlock(tArm); mb.addReturn(mb.addConst(i32Lit(7), i32));
    mb.beginBlock(fArm); mb.addReturn(mb.addConst(i32Lit(9), i32));
    Mir m = std::move(mb).finish();

    auto const v = derivedVectorOf(m, f);
    EXPECT_EQ(v, ints({StructCfMarker::EntryBlock, StructCfMarker::IfThen,
                       StructCfMarker::IfElse}));
    for (int const x : v) {
        EXPECT_NE(x, static_cast<int>(StructCfMarker::IfJoin));
    }
}

// The else-less-sealed-then shape (`if (c) return a; return b;`): the
// then seals, so the false-edge block is NOT a join (the then path
// never reaches it) — it derives IfElse around the virtual exit.
TEST(StructCfDerivation, ElseLessSealedThenDerivesIfElse) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    MirFuncId const f = mb.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const thenB = mb.createBlock();
    MirBlockId const rest  = mb.createBlock();
    mb.beginBlock(entry);
    mb.addCondBr(mb.addArg(0, boolT), thenB, rest);
    mb.beginBlock(thenB); mb.addReturn(mb.addConst(i32Lit(1), i32));
    mb.beginBlock(rest);  mb.addReturn(mb.addConst(i32Lit(2), i32));
    Mir m = std::move(mb).finish();

    EXPECT_EQ(derivedVectorOf(m, f),
              ints({StructCfMarker::EntryBlock, StructCfMarker::IfThen,
                    StructCfMarker::IfElse}));
}

TEST(StructCfDerivation, WhileShape) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    MirFuncId const f = mb.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock();
    MirBlockId const body   = mb.createBlock();
    MirBlockId const exit   = mb.createBlock();
    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    mb.addBr(header);
    mb.beginBlock(header); mb.addCondBr(cond, body, exit);
    mb.beginBlock(body);   mb.addBr(header);
    mb.beginBlock(exit);   mb.addReturn(mb.addConst(i32Lit(0), i32));
    Mir m = std::move(mb).finish();

    // The loop-condition CondBr is loop vocabulary: rule 2 claims the
    // header, rule 4 skips it, the body stays Linear, the exit is the
    // loop-exiting-edge target.
    EXPECT_EQ(derivedVectorOf(m, f),
              ints({StructCfMarker::EntryBlock, StructCfMarker::LoopHeader,
                    StructCfMarker::Linear, StructCfMarker::LoopExit}));
}

// Multi-exit loop — a shape NO current frontend emits (§A.5 multi-form
// rule): BOTH exit targets derive LoopExit; the in-loop CondBr's arms
// are claimed by earlier rules (header by rule 2, exit2 by rule 3), so
// rule 4 adds nothing.
TEST(StructCfDerivation, MultiExitLoop) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    MirFuncId const f = mb.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock();
    MirBlockId const body   = mb.createBlock();
    MirBlockId const exit1  = mb.createBlock();
    MirBlockId const exit2  = mb.createBlock();
    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    mb.addBr(header);
    mb.beginBlock(header); mb.addCondBr(cond, body, exit1);
    mb.beginBlock(body);   mb.addCondBr(cond, header, exit2);
    mb.beginBlock(exit1);  mb.addReturn(mb.addConst(i32Lit(1), i32));
    mb.beginBlock(exit2);  mb.addReturn(mb.addConst(i32Lit(2), i32));
    Mir m = std::move(mb).finish();

    EXPECT_EQ(derivedVectorOf(m, f),
              ints({StructCfMarker::EntryBlock, StructCfMarker::LoopHeader,
                    StructCfMarker::Linear, StructCfMarker::LoopExit,
                    StructCfMarker::LoopExit}));
}

// Multi-back-edge loop: two distinct back-edge sources, ONE header.
// PRIORITY-ORDER pin: b1's CondBr would claim its false arm (b2) as
// IfElse via rule 4 (ipdom(b1) = header, the real join of its arms) —
// and the header keeps LoopHeader (rule 2 claimed it first).
TEST(StructCfDerivation, MultiBackEdgeLoop) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    MirFuncId const f = mb.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock();
    MirBlockId const b1     = mb.createBlock();
    MirBlockId const b2     = mb.createBlock();
    MirBlockId const exit   = mb.createBlock();
    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    mb.addBr(header);
    mb.beginBlock(header); mb.addCondBr(cond, b1, exit);
    mb.beginBlock(b1);     mb.addCondBr(cond, header, b2);  // back-edge #1
    mb.beginBlock(b2);     mb.addBr(header);                // back-edge #2
    mb.beginBlock(exit);   mb.addReturn(mb.addConst(i32Lit(0), i32));
    Mir m = std::move(mb).finish();

    EXPECT_EQ(derivedVectorOf(m, f),
              ints({StructCfMarker::EntryBlock, StructCfMarker::LoopHeader,
                    StructCfMarker::Linear, StructCfMarker::IfElse,
                    StructCfMarker::LoopExit}));
}

// Switch shape (hand-built): SwitchCase per non-join target, SwitchJoin
// for the real ipdom, and the DISCRIMINANT block is NOT SwitchHead — it
// falls to the lower rules (Linear here).
TEST(StructCfDerivation, SwitchShapeHeadStaysLinear) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    MirFuncId const f = mb.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const head  = mb.createBlock();
    MirBlockId const c1    = mb.createBlock();
    MirBlockId const c2    = mb.createBlock();
    MirBlockId const join  = mb.createBlock();
    mb.beginBlock(entry);
    mb.addBr(head);
    mb.beginBlock(head);
    MirInstId const disc = mb.addConst(i32Lit(1), i32);
    MirInstId const k1   = mb.addConst(i32Lit(1), i32);
    MirInstId const k2   = mb.addConst(i32Lit(2), i32);
    std::pair<MirInstId, MirBlockId> const cases[] = {{k1, c1}, {k2, c2}};
    mb.addSwitch(disc, cases, join);  // default falls to the join
    mb.beginBlock(c1);   mb.addBr(join);
    mb.beginBlock(c2);   mb.addBr(join);
    mb.beginBlock(join); mb.addReturn(mb.addConst(i32Lit(0), i32));
    Mir m = std::move(mb).finish();

    // ipdom(head) = join: the case arms derive SwitchCase; the default
    // target IS the join (== ipdom → not a case) and claims SwitchJoin.
    EXPECT_EQ(derivedVectorOf(m, f),
              ints({StructCfMarker::EntryBlock, StructCfMarker::Linear,
                    StructCfMarker::SwitchCase, StructCfMarker::SwitchCase,
                    StructCfMarker::SwitchJoin}));
}

// Nested if inside a loop: rules compose — loop family claims header +
// exit first; the inner diamond derives IfThen/IfJoin inside the body.
TEST(StructCfDerivation, NestedIfInLoop) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    MirFuncId const f = mb.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock();
    MirBlockId const body   = mb.createBlock();
    MirBlockId const innerT = mb.createBlock();
    MirBlockId const innerJ = mb.createBlock();
    MirBlockId const exit   = mb.createBlock();
    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    mb.addBr(header);
    mb.beginBlock(header); mb.addCondBr(cond, body, exit);
    mb.beginBlock(body);   mb.addCondBr(cond, innerT, innerJ);
    mb.beginBlock(innerT); mb.addBr(innerJ);
    mb.beginBlock(innerJ); mb.addBr(header);  // the back-edge
    mb.beginBlock(exit);   mb.addReturn(mb.addConst(i32Lit(0), i32));
    Mir m = std::move(mb).finish();

    EXPECT_EQ(derivedVectorOf(m, f),
              ints({StructCfMarker::EntryBlock, StructCfMarker::LoopHeader,
                    StructCfMarker::Linear, StructCfMarker::IfThen,
                    StructCfMarker::IfJoin, StructCfMarker::LoopExit}));
}

// Compound loop condition (`while (a && b)`) — THE ACCEPTED QUIRK: the
// &&-join carries the loop's CondBr; rule 4 sees a non-header CondBr
// whose ipdom is the loop exit, so the BODY-HEAD derives IfThen. The
// &&'s own rhs/join blocks derive Linear (the header is loop
// vocabulary; rule 4 skips it). Canonical is canonical.
TEST(StructCfDerivation, CompoundConditionLoopBodyHeadIsIfThen) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    MirFuncId const f = mb.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock();  // evaluates `a`
    MirBlockId const rhs    = mb.createBlock();  // evaluates `b`
    MirBlockId const cjoin  = mb.createBlock();  // the &&-join; loop CondBr
    MirBlockId const body   = mb.createBlock();
    MirBlockId const exit   = mb.createBlock();
    mb.beginBlock(entry);
    MirInstId const a = mb.addArg(0, boolT);
    mb.addBr(header);
    mb.beginBlock(header); mb.addCondBr(a, rhs, cjoin);
    mb.beginBlock(rhs);    mb.addBr(cjoin);
    mb.beginBlock(cjoin);  mb.addCondBr(a, body, exit);
    mb.beginBlock(body);   mb.addBr(header);  // the back-edge
    mb.beginBlock(exit);   mb.addReturn(mb.addConst(i32Lit(0), i32));
    Mir m = std::move(mb).finish();

    EXPECT_EQ(derivedVectorOf(m, f),
              ints({StructCfMarker::EntryBlock, StructCfMarker::LoopHeader,
                    StructCfMarker::Linear,     // rhs — header's CondBr is skipped
                    StructCfMarker::Linear,     // cjoin — claimed by nothing
                    StructCfMarker::IfThen,     // body-head — THE QUIRK
                    StructCfMarker::LoopExit}));
}

// J = INVALID (reverse-unreachable CondBr inside an infinite loop) is
// treated as "no real join" — the arms still derive IfThen/IfElse.
TEST(StructCfDerivation, InfiniteLoopCondBrTreatsInvalidIpdomAsVirtual) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    MirFuncId const f = mb.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock();
    MirBlockId const c      = mb.createBlock();
    MirBlockId const b1     = mb.createBlock();
    MirBlockId const b2     = mb.createBlock();
    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    mb.addBr(header);
    mb.beginBlock(header); mb.addBr(c);
    mb.beginBlock(c);      mb.addCondBr(cond, b1, b2);
    mb.beginBlock(b1);     mb.addBr(header);
    mb.beginBlock(b2);     mb.addBr(header);
    Mir m = std::move(mb).finish();

    EXPECT_EQ(derivedVectorOf(m, f),
              ints({StructCfMarker::EntryBlock, StructCfMarker::LoopHeader,
                    StructCfMarker::Linear, StructCfMarker::IfThen,
                    StructCfMarker::IfElse}));
}

// ── the applier (rederiveStructCfMarkers) ───────────────────────────────────

// Arbitrary stale stamps on a FROZEN Mir are corrected in place — the
// narrow metadata-only mutation surface (Mir::setBlockMarker) works
// post-finish, and unreachable blocks stamp Linear.
TEST(StructCfDerivation, RederiveCorrectsStaleStampsOnFrozenMir) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    MirFuncId const f = mb.addFunction(fnSig, SymbolId{1});
    // Deliberately WRONG stamps everywhere (incl. dormant values).
    MirBlockId const entry = mb.createBlock(StructCfMarker::SwitchHead);
    MirBlockId const tArm  = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const fArm  = mb.createBlock(StructCfMarker::ExitBlock);
    MirBlockId const join  = mb.createBlock(StructCfMarker::LoopHeader);
    mb.beginBlock(entry);
    mb.addCondBr(mb.addArg(0, boolT), tArm, fArm);
    mb.beginBlock(tArm); mb.addBr(join);
    mb.beginBlock(fArm); mb.addBr(join);
    mb.beginBlock(join); mb.addReturn(mb.addConst(i32Lit(0), i32));
    Mir m = std::move(mb).finish();

    rederiveStructCfMarkers(m, f);
    EXPECT_EQ(storedVectorOf(m, f),
              ints({StructCfMarker::EntryBlock, StructCfMarker::IfThen,
                    StructCfMarker::IfElse, StructCfMarker::IfJoin}));
}

// The module-wide overload covers every function in one call.
TEST(StructCfDerivation, ModuleWideRederiveCoversEveryFunction) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    MirFuncId const f1 = mb.addFunction(fnSig, SymbolId{1});
    MirBlockId const e1 = mb.createBlock(StructCfMarker::LoopExit);  // stale
    mb.beginBlock(e1); mb.addReturn(mb.addConst(i32Lit(1), i32));
    MirFuncId const f2 = mb.addFunction(fnSig, SymbolId{2});
    MirBlockId const e2 = mb.createBlock(StructCfMarker::IfJoin);    // stale
    mb.beginBlock(e2); mb.addReturn(mb.addConst(i32Lit(2), i32));
    Mir m = std::move(mb).finish();

    rederiveStructCfMarkers(m);
    EXPECT_EQ(m.blockMarker(m.funcEntry(f1)), StructCfMarker::EntryBlock);
    EXPECT_EQ(m.blockMarker(m.funcEntry(f2)), StructCfMarker::EntryBlock);
}
