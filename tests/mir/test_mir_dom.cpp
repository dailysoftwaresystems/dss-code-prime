// Direct pins for `mir_dom.hpp` — Cooper-Harvey-Kennedy idom +
// tri-state dominates + Cytron-Ferrante dominance frontier + iterated
// dominance frontier + dom-tree children. Consumers: the MIR verifier
// (use-dom-def check), Mem2Reg (Cytron-Ferrante Phi placement), and
// future LICM / CSE (D-OPT-DOMTREE-EXTRACTION).

#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_cfg.hpp"
#include "mir/mir_dom.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>

using namespace dss;

namespace {

// Helper: build a diamond CFG (entry → tArm + fArm → join → return).
// Returns the Mir + the 4 block ids in source order.
struct DiamondMir {
    Mir         mir;
    MirBlockId  entry;
    MirBlockId  tArm;
    MirBlockId  fArm;
    MirBlockId  merge;
};

DiamondMir buildDiamond(TypeInterner& interner) {
    DiamondMir d;
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    d.entry = mb.createBlock(StructCfMarker::EntryBlock);
    d.tArm  = mb.createBlock(StructCfMarker::Linear);
    d.fArm  = mb.createBlock(StructCfMarker::Linear);
    d.merge = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(d.entry);
    MirInstId const cond = mb.addArg(0, boolT);
    mb.addCondBr(cond, d.tArm, d.fArm);
    mb.beginBlock(d.tArm);
    mb.addBr(d.merge);
    mb.beginBlock(d.fArm);
    mb.addBr(d.merge);
    mb.beginBlock(d.merge);
    MirLiteralValue v; v.value = std::int64_t{0}; v.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v, i32));
    d.mir = std::move(mb).finish();
    return d;
}

} // namespace

// Diamond CFG: entry dominates every block; tArm + fArm dominate only
// themselves; merge is dominated by entry (not by either arm — they
// diverge at the CondBr).
TEST(MirDom, DiamondDominanceRelations) {
    TypeInterner interner{CompilationUnitId{1}};
    auto d = buildDiamond(interner);

    auto const rpo = mirReversePostOrder(d.mir, d.entry);
    auto const preds = mirBuildPredecessors(d.mir);
    auto const dom = computeMirDomTree(d.mir, d.entry, rpo, preds);

    // Entry dominates all 4 blocks.
    EXPECT_EQ(mirDominatesBlock(d.entry, d.entry, dom),  MirDomResult::Dominates);
    EXPECT_EQ(mirDominatesBlock(d.entry, d.tArm,  dom),  MirDomResult::Dominates);
    EXPECT_EQ(mirDominatesBlock(d.entry, d.fArm,  dom),  MirDomResult::Dominates);
    EXPECT_EQ(mirDominatesBlock(d.entry, d.merge, dom),  MirDomResult::Dominates);
    // tArm does NOT dominate fArm OR merge (diverging paths).
    EXPECT_EQ(mirDominatesBlock(d.tArm, d.fArm,  dom),   MirDomResult::DoesNot);
    EXPECT_EQ(mirDominatesBlock(d.tArm, d.merge, dom),   MirDomResult::DoesNot);
    EXPECT_EQ(mirDominatesBlock(d.fArm, d.merge, dom),   MirDomResult::DoesNot);
    // Each block dominates itself.
    EXPECT_EQ(mirDominatesBlock(d.tArm, d.tArm, dom),    MirDomResult::Dominates);
    EXPECT_EQ(mirDominatesBlock(d.merge, d.merge, dom),  MirDomResult::Dominates);
}

// Diamond CFG dominance frontier: merge is in the frontier of both
// tArm and fArm (those blocks dominate a predecessor of merge but
// do NOT dominate merge itself). Entry's frontier is empty (it
// dominates everyone).
TEST(MirDom, DiamondDominanceFrontier) {
    TypeInterner interner{CompilationUnitId{1}};
    auto d = buildDiamond(interner);

    auto const rpo = mirReversePostOrder(d.mir, d.entry);
    auto const preds = mirBuildPredecessors(d.mir);
    auto const dom = computeMirDomTree(d.mir, d.entry, rpo, preds);
    auto const df  = mirDominanceFrontier(d.mir, dom, preds);

    auto contains = [](std::vector<MirBlockId> const& v, MirBlockId b) {
        return std::any_of(v.begin(), v.end(),
            [&](MirBlockId const& x) { return x.v == b.v; });
    };

    // entry's frontier is empty (dominates everyone — no merge points
    // where it loses control).
    EXPECT_TRUE(df[d.entry.v].empty())
        << "entry block dominates the whole function; its frontier "
           "must be empty";

    // tArm's frontier contains merge (tArm dominates its own
    // predecessor of merge — itself — but doesn't dominate merge).
    EXPECT_TRUE(contains(df[d.tArm.v], d.merge))
        << "tArm dominates a predecessor of merge but not merge itself "
           "— merge must be in tArm's dominance frontier (Mem2Reg "
           "would insert Phis here)";

    // fArm's frontier also contains merge (symmetric).
    EXPECT_TRUE(contains(df[d.fArm.v], d.merge))
        << "fArm's frontier must also contain merge";

    // merge's frontier is empty (it doesn't dominate any further blocks
    // — its only predecessor is the Return-terminated block itself).
    EXPECT_TRUE(df[d.merge.v].empty());
}

// Loop CFG dominance frontier (D-OPT-DOMFRONTIER-LOOP-TEST):
//
//   entry → header → body → header   (back-edge)
//             header → exit           (loop-exit edge)
//
// The header has TWO predecessors (entry + body via back-edge) — it
// IS a join point, so dom-frontier semantics apply: body's frontier
// must contain header (body dominates a predecessor of header — itself
// — but doesn't dominate header itself; header's idom is entry, NOT
// body). exit has only header as a predecessor so it's NOT in any
// frontier. This is the exact shape Mem2Reg needs for promoting a
// loop-induction variable: a Phi at the loop header with incomings
// (initial value, entry) and (back-edge value, body).
TEST(MirDom, LoopCfgDominanceFrontier) {
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
    mb.addBr(header);
    mb.beginBlock(header);
    MirLiteralValue cv; cv.value = std::int64_t{1}; cv.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(cv, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
    mb.addBr(header);  // the back-edge that makes header a join
    mb.beginBlock(exitB);
    MirLiteralValue ev; ev.value = std::int64_t{0}; ev.core = TypeKind::I32;
    mb.addReturn(mb.addConst(ev, i32));
    Mir mir = std::move(mb).finish();

    auto const rpo = mirReversePostOrder(mir, entry);
    auto const preds = mirBuildPredecessors(mir);
    auto const dom = computeMirDomTree(mir, entry, rpo, preds);
    auto const df  = mirDominanceFrontier(mir, dom, preds);

    auto contains = [](std::vector<MirBlockId> const& v, MirBlockId b) {
        return std::any_of(v.begin(), v.end(),
            [&](MirBlockId const& x) { return x.v == b.v; });
    };

    // The body's frontier contains the header — back-edge contributes
    // a join, so Mem2Reg needs a Phi at the header for any alloca
    // stored to inside the loop body.
    EXPECT_TRUE(contains(df[body.v], header))
        << "loop back-edge: body's dominance frontier must contain header";

    // The entry's frontier is empty (it dominates the whole function).
    EXPECT_TRUE(df[entry.v].empty());
    // The exit's frontier is empty (no further joins).
    EXPECT_TRUE(df[exitB.v].empty());

    // IDF check: if `body` is the sole def-block, IDF must contain
    // header (a Phi is required there). This is the Cytron-Ferrante
    // gate Mem2Reg consults.
    std::vector<MirBlockId> const defs = {body};
    auto const idf = mirIteratedDominanceFrontier(defs, df);
    EXPECT_TRUE(contains(idf, header))
        << "IDF({body}) must contain header — Mem2Reg's Phi-insertion site";

    // Dom-tree children: entry's child is header; header's children
    // include body + exit (it dominates both); body has no children;
    // exit has no children.
    auto const dchild = mirDomTreeChildren(mir, dom);
    EXPECT_TRUE(contains(dchild[entry.v], header));
    EXPECT_TRUE(contains(dchild[header.v], body));
    EXPECT_TRUE(contains(dchild[header.v], exitB));
    EXPECT_TRUE(dchild[body.v].empty());
    EXPECT_TRUE(dchild[exitB.v].empty());
}

// Trivial single-block function: dominator tree is just {entry → entry};
// frontier is empty for every block.
TEST(MirDom, SingleBlockTrivialDominance) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    mb.addReturn();
    Mir mir = std::move(mb).finish();

    auto const rpo = mirReversePostOrder(mir, entry);
    auto const preds = mirBuildPredecessors(mir);
    auto const dom = computeMirDomTree(mir, entry, rpo, preds);
    auto const df  = mirDominanceFrontier(mir, dom, preds);

    EXPECT_EQ(mirDominatesBlock(entry, entry, dom), MirDomResult::Dominates);
    EXPECT_TRUE(df[entry.v].empty());
}
