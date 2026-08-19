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
#include "mir/mir_cfg.hpp"
#include "mir/mir_dom.hpp"
#include "mir/mir_struct_markers.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <span>
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

// ── the two REUSED substrates behind the derivation ─────────────────────────
//
// `deriveStructCfMarkers` costs O(function), not O(module), because two of its
// inputs stopped being recomputed from scratch per function:
//   - the natural-loop forest, whose back-edge sweep is now SCOPED to the
//     function's own candidate set (mir_dom.hpp `mirNaturalLoops` overload);
//   - the post-dominator tree, now built through a reusable `MirPostDomScratch`
//     (D-OPT-POSTDOM-SCRATCH-REUSE).
// Both are PERFORMANCE changes over a compiler's output, so each one is pinned
// DIFFERENTIALLY against the fresh/whole-module computation it replaced, over
// an adversarial module, after EVERY call. A performance change that alters a
// marker is a miscompile of the verifier's contract, not a slow build.

namespace {

// Adversarial module. The shapes are chosen so that consecutive per-function
// walks have DIFFERENT and SHRINKING write sets (a big loop nest followed by a
// one-block function is the case a partial-reset bug survives), and so that
// every ipdom arm is exercised: a real join, the virtual exit, and INVALID
// (reverse-unreachable, from the infinite loop). Two functions own SELF-LOOPING
// blocks — the shape that makes a back-edge sweep reach across function
// boundaries (see ForeignSelfLoopPseudoLoopClaimIsPreserved).
struct AdversarialModule {
    Mir                    m;
    std::vector<MirFuncId> funcs;
    MirFuncId              selfLoopFunc{};   // owns gSelf/gTail
    MirBlockId             gSelf{};
    MirBlockId             gTail{};
    MirFuncId              trivialFunc{};    // one block, derived FIRST
};

AdversarialModule buildAdversarialModule(TypeInterner& interner) {
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    AdversarialModule out;

    // f0 — TRIVIAL: one block. Derived FIRST, so the module-wide sweep's
    // reach into LATER functions is observable in its own derived vector.
    MirFuncId const f0 = mb.addFunction(fnSig, SymbolId{1});
    {
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e); mb.addReturn(mb.addConst(i32Lit(0), i32));
    }

    // f1 — BIG: a loop whose body carries a diamond, plus a switch after it.
    MirFuncId const f1 = mb.addFunction(fnSig, SymbolId{2});
    {
        MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const header = mb.createBlock();
        MirBlockId const body   = mb.createBlock();
        MirBlockId const innerT = mb.createBlock();
        MirBlockId const innerJ = mb.createBlock();
        MirBlockId const exit   = mb.createBlock();
        MirBlockId const c1     = mb.createBlock();
        MirBlockId const c2     = mb.createBlock();
        MirBlockId const sjoin  = mb.createBlock();
        mb.beginBlock(entry);
        MirInstId const cond = mb.addArg(0, boolT);
        mb.addBr(header);
        mb.beginBlock(header); mb.addCondBr(cond, body, exit);
        mb.beginBlock(body);   mb.addCondBr(cond, innerT, innerJ);
        mb.beginBlock(innerT); mb.addBr(innerJ);
        mb.beginBlock(innerJ); mb.addBr(header);
        mb.beginBlock(exit);
        MirInstId const disc = mb.addConst(i32Lit(1), i32);
        MirInstId const k1   = mb.addConst(i32Lit(1), i32);
        MirInstId const k2   = mb.addConst(i32Lit(2), i32);
        std::pair<MirInstId, MirBlockId> const cases[] = {{k1, c1}, {k2, c2}};
        mb.addSwitch(disc, cases, sjoin);
        mb.beginBlock(c1);    mb.addBr(sjoin);
        mb.beginBlock(c2);    mb.addBr(sjoin);
        mb.beginBlock(sjoin); mb.addReturn(mb.addConst(i32Lit(3), i32));
    }

    // f2 — INFINITE: reverse-unreachable blocks (ipdom INVALID) and a
    // REACHABLE self-loop.
    MirFuncId const f2 = mb.addFunction(fnSig, SymbolId{3});
    {
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const spin  = mb.createBlock();
        mb.beginBlock(entry); mb.addBr(spin);
        mb.beginBlock(spin);  mb.addBr(spin);   // self-loop, never exits
    }

    // f3 — DIAMOND: both arms return (ipdom == the VIRTUAL exit).
    MirFuncId const f3 = mb.addFunction(fnSig, SymbolId{4});
    {
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const tArm  = mb.createBlock();
        MirBlockId const fArm  = mb.createBlock();
        mb.beginBlock(entry);
        mb.addCondBr(mb.addArg(0, boolT), tArm, fArm);
        mb.beginBlock(tArm); mb.addReturn(mb.addConst(i32Lit(7), i32));
        mb.beginBlock(fArm); mb.addReturn(mb.addConst(i32Lit(9), i32));
    }

    // f4 — an UNREACHABLE self-looping block whose OTHER successor is a
    // further unreachable block. This is the shape the whole-module back-edge
    // sweep turns into a single-block pseudo-loop in EVERY function's
    // derivation, claiming LoopExit on `gTail` across function boundaries.
    MirFuncId const f4 = mb.addFunction(fnSig, SymbolId{5});
    MirBlockId gSelf{}, gTail{};
    {
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        gSelf = mb.createBlock();
        gTail = mb.createBlock();
        mb.beginBlock(entry);
        MirInstId const c = mb.addArg(0, boolT);
        mb.addReturn(mb.addConst(i32Lit(1), i32));
        mb.beginBlock(gSelf); mb.addCondBr(c, gSelf, gTail);
        mb.beginBlock(gTail); mb.addReturn(mb.addConst(i32Lit(2), i32));
    }

    out.m = std::move(mb).finish();
    out.funcs = {f0, f1, f2, f3, f4};
    out.trivialFunc  = f0;
    out.selfLoopFunc = f4;
    out.gSelf = gSelf;
    out.gTail = gTail;
    return out;
}

// FULL-array equality over the module-sized post-dominator arrays, comparing
// `v` AND `arenaTag` — MirBlockId's `operator==` compares `.v` only, so a
// provenance divergence would slip past a plain `EXPECT_EQ`.
::testing::AssertionResult postDomEqual(MirPostDomTree const& fresh,
                                        MirPostDomTree const& reused) {
    if (fresh.virtualExit != reused.virtualExit) {
        return ::testing::AssertionFailure()
            << "virtualExit " << fresh.virtualExit << " != " << reused.virtualExit;
    }
    if (fresh.ipdom.size() != reused.ipdom.size()) {
        return ::testing::AssertionFailure()
            << "ipdom size " << fresh.ipdom.size() << " != " << reused.ipdom.size();
    }
    if (fresh.gaveUp.size() != reused.gaveUp.size()) {
        return ::testing::AssertionFailure()
            << "gaveUp size " << fresh.gaveUp.size() << " != " << reused.gaveUp.size();
    }
    for (std::size_t i = 0; i < fresh.ipdom.size(); ++i) {
        if (fresh.ipdom[i].v != reused.ipdom[i].v
         || fresh.ipdom[i].arenaTag != reused.ipdom[i].arenaTag) {
            return ::testing::AssertionFailure()
                << "ipdom[" << i << "] fresh {v=" << fresh.ipdom[i].v
                << ", tag=" << fresh.ipdom[i].arenaTag << "} != reused {v="
                << reused.ipdom[i].v << ", tag=" << reused.ipdom[i].arenaTag << "}";
        }
        if (fresh.gaveUp[i] != reused.gaveUp[i]) {
            return ::testing::AssertionFailure()
                << "gaveUp[" << i << "] " << int{fresh.gaveUp[i]} << " != "
                << int{reused.gaveUp[i]};
        }
    }
    return ::testing::AssertionSuccess();
}

::testing::AssertionResult loopForestEqual(std::vector<MirNaturalLoop> const& a,
                                           std::vector<MirNaturalLoop> const& b) {
    if (a.size() != b.size()) {
        return ::testing::AssertionFailure()
            << "loop count " << a.size() << " != " << b.size();
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].header.v != b[i].header.v
         || a[i].header.arenaTag != b[i].header.arenaTag) {
            return ::testing::AssertionFailure()
                << "loop[" << i << "] header " << a[i].header.v << " != " << b[i].header.v;
        }
        auto ids = [](std::vector<MirBlockId> const& v) {
            std::vector<std::uint32_t> o;
            for (MirBlockId const x : v) { o.push_back(x.v); o.push_back(x.arenaTag); }
            return o;
        };
        if (ids(a[i].body) != ids(b[i].body)) {
            return ::testing::AssertionFailure()
                << "loop[" << i << "] (header " << a[i].header.v << ") body differs";
        }
        if (ids(a[i].backEdgeSources) != ids(b[i].backEdgeSources)) {
            return ::testing::AssertionFailure()
                << "loop[" << i << "] (header " << a[i].header.v
                << ") backEdgeSources differ";
        }
    }
    return ::testing::AssertionSuccess();
}

// The candidate set a caller must supply for the scoped sweep to answer what
// the whole-module sweep answers. Recomputed HERE independently of the
// production builder (mir_struct_markers.cpp) — a shared helper would make the
// differential tautological.
std::vector<std::uint32_t> candidateSetFor(Mir const& m, MirFuncId f,
                                           std::vector<MirBlockId> const& rpo) {
    std::vector<std::uint32_t> out;
    std::uint32_t const bc = static_cast<std::uint32_t>(m.blockCount());
    std::uint32_t const nb = m.funcBlockCount(f);
    for (std::uint32_t bi = 0; bi < nb; ++bi) out.push_back(m.funcBlockAt(f, bi).v);
    for (MirBlockId const b : rpo) out.push_back(b.v);
    for (std::uint32_t i = 1; i < bc; ++i) {          // the module self-loop index
        MirBlockId const b{i, m.id().v};
        for (MirBlockId const s : m.blockSuccessors(b)) {
            if (s.valid() && s.v == i) { out.push_back(i); break; }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace

// D-OPT-POSTDOM-SCRATCH-REUSE. One scratch, an adversarial SEQUENCE of
// functions (big → tiny → infinite → …, then reversed, then a repeat), and a
// FULL module-sized array comparison against the fresh tree after EVERY call.
// A partial reset that misses a slot leaves the PREVIOUS function's ipdom
// there; the big-then-tiny order is what makes that stale slot survive.
TEST(MirPostDomScratchReuse, MatchesFreshOverAdversarialSequence) {
    TypeInterner interner{CompilationUnitId{1}};
    AdversarialModule a = buildAdversarialModule(interner);

    std::vector<MirFuncId> sequence = a.funcs;
    for (auto it = a.funcs.rbegin(); it != a.funcs.rend(); ++it) {
        sequence.push_back(*it);
    }
    sequence.push_back(a.funcs.front());   // repeat: idempotence on one scratch
    sequence.push_back(a.funcs.front());

    MirPostDomScratch scratch;
    for (std::size_t step = 0; step < sequence.size(); ++step) {
        MirFuncId const f = sequence[step];
        MirPostDomTree const  fresh  = computeMirPostDomTree(a.m, f);
        MirPostDomTree const& reused = computeMirPostDomTree(a.m, f, scratch);
        EXPECT_TRUE(postDomEqual(fresh, reused))
            << "step " << step << ", func #" << f.v;
    }
}

// The OTHER half of the scratch contract, and the half a value-differential
// CANNOT see: each call must see ONLY its own function. A reset that misses
// the reverse-ADJACENCY leaves the previous function's exit blocks hanging off
// the shared virtual-exit node, so the next call's reverse-RPO walks them too.
// Their idom still resolves to "unset" (their own reverse-preds WERE cleared),
// so every ipdom value stays correct and the differential above passes — while
// the traversal quietly grows by every exit block in the module and the whole
// O(function) property, which is the only reason the scratch exists, is gone.
//
// So this pins the SHAPE of the scratch after each call: nothing outside the
// function's own block range (plus the virtual exit) may be dirty.
TEST(MirPostDomScratchReuse, EachCallSeesOnlyItsOwnFunction) {
    TypeInterner interner{CompilationUnitId{1}};
    AdversarialModule a = buildAdversarialModule(interner);
    std::uint32_t const virtualSlot = static_cast<std::uint32_t>(a.m.blockCount());

    std::vector<MirFuncId> sequence = a.funcs;
    for (auto it = a.funcs.rbegin(); it != a.funcs.rend(); ++it) {
        sequence.push_back(*it);
    }

    MirPostDomScratch scratch;
    for (std::size_t step = 0; step < sequence.size(); ++step) {
        MirFuncId const f = sequence[step];
        (void)computeMirPostDomTree(a.m, f, scratch);

        std::uint32_t const nb    = a.m.funcBlockCount(f);
        std::uint32_t const first = a.m.funcBlockAt(f, 0).v;
        std::uint32_t const lastEx = first + nb;
        for (std::uint32_t s = 0; s <= virtualSlot; ++s) {
            if (s >= first && s < lastEx) continue;   // this function's own
            if (s == virtualSlot) continue;           // checked exactly below
            EXPECT_TRUE(scratch.revPreds[s].empty())
                << "step " << step << ", func #" << f.v << ": revPreds[" << s
                << "] carries " << scratch.revPreds[s].size()
                << " stale edge(s) from an earlier function";
            EXPECT_TRUE(scratch.revSuccs[s].empty())
                << "step " << step << ", func #" << f.v << ": revSuccs[" << s
                << "] carries " << scratch.revSuccs[s].size()
                << " stale edge(s) from an earlier function";
            EXPECT_EQ(scratch.visited[s], 0)
                << "step " << step << ", func #" << f.v << ": visited[" << s
                << "] still set from an earlier function";
        }
        // The virtual exit is the ONE shared node: its reverse-successors are
        // exactly THIS function's reachable Return/Unreachable blocks.
        // Recomputed here from the Mir, independently of the production walk.
        std::unordered_set<std::uint32_t> reachable;
        for (MirBlockId const b : mirReversePostOrder(a.m, a.m.funcEntry(f))) {
            reachable.insert(b.v);
        }
        std::size_t exits = 0;
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = a.m.funcBlockAt(f, bi);
            if (!reachable.contains(b.v)) continue;
            std::uint32_t const ni = a.m.blockInstCount(b);
            if (ni == 0) continue;
            MirOpcode const term = a.m.instOpcode(a.m.blockInstAt(b, ni - 1));
            if (term == MirOpcode::Return || term == MirOpcode::Unreachable) ++exits;
        }
        EXPECT_EQ(scratch.revSuccs[virtualSlot].size(), exits)
            << "step " << step << ", func #" << f.v
            << ": the virtual exit accumulated other functions' exit blocks";
        EXPECT_EQ(scratch.order.size(), scratch.touchedNodes.size())
            << "step " << step << ": the recorded write set must BE the order";
    }
}

// The scratch is bound to ONE module — a stale scratch carried across a
// rebuild must fail LOUD, never silently mix two modules' slots.
TEST(MirPostDomScratchReuseDeathTest, StaleScratchAcrossModulesAborts) {
    TypeInterner interner{CompilationUnitId{1}};
    AdversarialModule a = buildAdversarialModule(interner);
    AdversarialModule b = buildAdversarialModule(interner);
    MirPostDomScratch scratch;
    (void)computeMirPostDomTree(a.m, a.funcs.front(), scratch);
    EXPECT_DEATH((void)computeMirPostDomTree(b.m, b.funcs.front(), scratch),
                 "stale scratch across a rebuild");
}

// D-OPT-NATURAL-LOOPS-MODULE-WIDE-SCAN. The scoped back-edge sweep must answer
// EXACTLY what the whole-module sweep answers — same loops, same bodies, same
// back-edge-source ORDER — for every function of the adversarial module.
TEST(MirNaturalLoopsScopedSweep, MatchesWholeModuleSweepForEveryFunction) {
    TypeInterner interner{CompilationUnitId{1}};
    AdversarialModule a = buildAdversarialModule(interner);
    auto const preds = mirBuildPredecessors(a.m);
    for (MirFuncId const f : a.funcs) {
        MirBlockId const entry = a.m.funcEntry(f);
        auto const rpo = mirReversePostOrder(a.m, entry);
        MirDomTree const dom = computeMirDomTree(a.m, entry, rpo, preds);
        auto const cands = candidateSetFor(a.m, f, rpo);
        auto const wholeModule = mirNaturalLoops(a.m, dom, preds);
        auto const scoped = mirNaturalLoops(
            a.m, dom, preds, std::span<std::uint32_t const>(cands.data(), cands.size()));
        EXPECT_TRUE(loopForestEqual(wholeModule, scoped)) << "func #" << f.v;
    }
}

// The scoped overload's ascending-unique-in-range contract is what fixes
// `backEdgeSources` order; a violation is a caller bug and fails LOUD.
TEST(MirNaturalLoopsScopedSweepDeathTest, UnsortedCandidatesAbort) {
    TypeInterner interner{CompilationUnitId{1}};
    AdversarialModule a = buildAdversarialModule(interner);
    auto const preds = mirBuildPredecessors(a.m);
    MirFuncId const f = a.funcs[1];
    MirBlockId const entry = a.m.funcEntry(f);
    auto const rpo = mirReversePostOrder(a.m, entry);
    MirDomTree const dom = computeMirDomTree(a.m, entry, rpo, preds);
    std::vector<std::uint32_t> const descending = {3u, 2u};
    EXPECT_DEATH((void)mirNaturalLoops(a.m, dom, preds,
                     std::span<std::uint32_t const>(descending.data(), descending.size())),
                 "not strictly ascending");
    std::vector<std::uint32_t> const outOfRange = {
        static_cast<std::uint32_t>(a.m.blockCount())};
    EXPECT_DEATH((void)mirNaturalLoops(a.m, dom, preds,
                     std::span<std::uint32_t const>(outOfRange.data(), outOfRange.size())),
                 "outside \\[1, blockCount");
}

// ── the derivation's CROSS-FUNCTION reach (behaviour, not aspiration) ───────

// `mirDominatesBlock(s, u, dom)` short-circuits to Dominates when s.v == u.v,
// BEFORE consulting the tree — so a SELF-LOOPING block registers as a
// back-edge source even in a function whose dominator tree knows nothing about
// it. The whole-module sweep therefore manufactures a single-block pseudo-loop
// for every self-looping block in the MODULE, in EVERY function's derivation,
// and rule 3 claims LoopExit on its non-self successors.
//
// This pins that behaviour EXACTLY, because the scoped sweep must reproduce
// it: RED if the module self-loop index is dropped from the candidate set.
//
// It is also the pin for a documented-spec divergence: mir_struct_markers.hpp
// says unreachable blocks stay Linear, and `gTail` here is unreachable and
// derives LoopExit. The behaviour is canon until the derivation rules change
// deliberately — this test is what makes that change a DECISION rather than an
// accident (D-MIR-STRUCTCF-UNREACHABLE-BLOCK-CLAIMED).
TEST(StructCfDerivation, ForeignSelfLoopPseudoLoopClaimIsPreserved) {
    TypeInterner interner{CompilationUnitId{1}};
    AdversarialModule a = buildAdversarialModule(interner);

    // Deriving the TRIVIAL one-block function claims a slot that belongs to a
    // DIFFERENT function, five functions later.
    auto const derivedForTrivial = deriveStructCfMarkers(a.m, a.trivialFunc);
    ASSERT_LT(a.gTail.v, derivedForTrivial.size());
    EXPECT_EQ(derivedForTrivial[a.gTail.v], StructCfMarker::LoopExit)
        << "the module-wide back-edge sweep reaches out of the function being "
           "derived; the scoped sweep must reproduce it";
    EXPECT_EQ(derivedForTrivial[a.gSelf.v], StructCfMarker::Linear)
        << "the pseudo-loop's own block is IN its body, so it is not an exit";

    // And the owning function derives the same thing for those slots.
    auto const derivedForOwner = deriveStructCfMarkers(a.m, a.selfLoopFunc);
    EXPECT_EQ(derivedForOwner[a.gTail.v], StructCfMarker::LoopExit);
    EXPECT_EQ(derivedForOwner[a.gSelf.v], StructCfMarker::Linear);

    // …so the module-wide applier STAMPS an unreachable block LoopExit.
    rederiveStructCfMarkers(a.m);
    EXPECT_EQ(a.m.blockMarker(a.gTail), StructCfMarker::LoopExit);
    EXPECT_EQ(a.m.blockMarker(a.gSelf), StructCfMarker::Linear);
}

// The module-wide applier and the one-function applier must agree block for
// block. They share a derivation core but reach it differently — the module
// path hoists the self-loop index and REUSES one post-dominator scratch across
// every function, the per-function path builds both fresh. This is the
// end-to-end differential over both.
TEST(StructCfDerivation, ModuleWideAndPerFunctionAppliersAgree) {
    TypeInterner interner{CompilationUnitId{1}};
    AdversarialModule wide = buildAdversarialModule(interner);
    AdversarialModule one  = buildAdversarialModule(interner);

    rederiveStructCfMarkers(wide.m);
    for (MirFuncId const f : one.funcs) rederiveStructCfMarkers(one.m, f);

    ASSERT_EQ(wide.funcs.size(), one.funcs.size());
    for (std::size_t i = 0; i < wide.funcs.size(); ++i) {
        EXPECT_EQ(storedVectorOf(wide.m, wide.funcs[i]),
                  storedVectorOf(one.m, one.funcs[i]))
            << "func index " << i;
    }
}
