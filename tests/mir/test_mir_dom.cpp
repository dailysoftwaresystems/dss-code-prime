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
#include <random>

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

// ─── D-OPT-DOMTREE-SCRATCH-REUSE — the scratch-vs-fresh differential pins ───
//
// The scratch overloads MUST produce byte-identical results to the fresh-
// allocation path on every call of any call sequence. The comparisons below
// deliberately compare `v` AND `arenaTag` explicitly — MirBlockId::operator==
// compares `v` only, so an equality-based check would PASS a broken reset
// that leaves a stale-tagged zero behind. Full module-sized arrays are
// compared after EVERY call (the bleed class: one call's leftovers surviving
// into the next call's untouched slots).

namespace {

void expectDomTreesIdenticalFull(MirDomTree const& fresh,
                                 MirDomTree const& scratch,
                                 char const* what) {
    ASSERT_EQ(fresh.idom.size(), scratch.idom.size()) << what;
    ASSERT_EQ(fresh.gaveUp.size(), scratch.gaveUp.size()) << what;
    for (std::size_t i = 0; i < fresh.idom.size(); ++i) {
        EXPECT_EQ(fresh.idom[i].v, scratch.idom[i].v)
            << what << ": idom slot " << i << " v mismatch";
        EXPECT_EQ(fresh.idom[i].arenaTag, scratch.idom[i].arenaTag)
            << what << ": idom slot " << i << " arenaTag mismatch (a reset "
               "writing a stale-tagged zero passes operator== — this pin "
               "compares the tag explicitly)";
    }
    for (std::size_t i = 0; i < fresh.gaveUp.size(); ++i) {
        EXPECT_EQ(static_cast<int>(fresh.gaveUp[i]),
                  static_cast<int>(scratch.gaveUp[i]))
            << what << ": gaveUp slot " << i;
    }
}

void expectChildrenIdenticalFull(
    std::vector<std::vector<MirBlockId>> const& fresh,
    std::vector<std::vector<MirBlockId>> const& scratch,
    char const* what) {
    ASSERT_EQ(fresh.size(), scratch.size()) << what;
    for (std::size_t i = 0; i < fresh.size(); ++i) {
        ASSERT_EQ(fresh[i].size(), scratch[i].size())
            << what << ": children[" << i << "] size";
        for (std::size_t j = 0; j < fresh[i].size(); ++j) {
            EXPECT_EQ(fresh[i][j].v, scratch[i][j].v)
                << what << ": children[" << i << "][" << j << "] v (inner "
                   "ORDER is load-bearing — the CSE dom-DFS depends on it)";
            EXPECT_EQ(fresh[i][j].arenaTag, scratch[i][j].arenaTag)
                << what << ": children[" << i << "][" << j << "] arenaTag";
        }
    }
}

// Run the full fresh-vs-scratch comparison for one function against a shared
// scratch (called in sequence to exercise the reset between calls).
void compareFreshVsScratch(Mir const& mir, MirFuncId f,
                           std::vector<std::vector<MirBlockId>> const& preds,
                           MirDomScratch& scratch, char const* what) {
    MirBlockId const entry = mir.funcEntry(f);
    auto const rpo = mirReversePostOrder(mir, entry);
    auto const freshDom = computeMirDomTree(mir, entry, rpo, preds);
    auto const freshChildren = mirDomTreeChildren(mir, freshDom);
    auto const& scratchDom =
        computeMirDomTree(mir, entry, rpo, preds, scratch);
    auto const& scratchChildren = mirDomTreeChildren(mir, scratchDom, scratch);
    expectDomTreesIdenticalFull(freshDom, scratchDom, what);
    expectChildrenIdenticalFull(freshChildren, scratchChildren, what);
}

} // namespace

// The adversarial multi-function sequence: big → small → big (stale-slot
// bleed), a function with an UNREACHABLE block that branches INTO a reachable
// one (the preds-outside-order read in the CHK core), the same function twice
// with one scratch (reset idempotence), then big again. Full-array
// comparisons after every call.
TEST(MirDomScratch, AdversarialSequenceMatchesFreshOnEveryCall) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    // fn0 — "big": a 3-level diamond cascade (10 blocks).
    mb.addFunction(fnSig, SymbolId{100});
    {
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        MirBlockId prev = entry;
        MirInstId cond{};
        for (int lvl = 0; lvl < 3; ++lvl) {
            MirBlockId const t = mb.createBlock(StructCfMarker::Linear);
            MirBlockId const e = mb.createBlock(StructCfMarker::Linear);
            MirBlockId const j = mb.createBlock(StructCfMarker::Linear);
            mb.beginBlock(prev);
            if (lvl == 0) cond = mb.addArg(0, boolT);
            mb.addCondBr(cond, t, e);
            mb.beginBlock(t);
            mb.addBr(j);
            mb.beginBlock(e);
            mb.addBr(j);
            prev = j;
        }
        mb.beginBlock(prev);
        MirLiteralValue v; v.value = std::int64_t{0}; v.core = TypeKind::I32;
        mb.addReturn(mb.addConst(v, i32));
    }
    // fn1 — "small": entry → ret (1 block).
    mb.addFunction(fnSig, SymbolId{101});
    {
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(entry);
        MirLiteralValue v; v.value = std::int64_t{1}; v.core = TypeKind::I32;
        mb.addReturn(mb.addConst(v, i32));
    }
    // fn2 — an UNREACHABLE block branching INTO the reachable join: the CHK
    // core reads preds[join] = {entry, dead} and must skip `dead` via the
    // rpoIndex gate — on the SECOND+ scratch call this exercises the reset
    // (a stale rpoIndex entry for `dead` from a prior call would flip it).
    mb.addFunction(fnSig, SymbolId{102});
    {
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const dead  = mb.createBlock(StructCfMarker::Linear);
        MirBlockId const join  = mb.createBlock(StructCfMarker::Linear);
        mb.beginBlock(entry);
        mb.addBr(join);
        mb.beginBlock(dead);   // never branched to — unreachable
        mb.addBr(join);
        mb.beginBlock(join);
        MirLiteralValue v; v.value = std::int64_t{2}; v.core = TypeKind::I32;
        mb.addReturn(mb.addConst(v, i32));
    }
    Mir mir = std::move(mb).finish();
    auto const preds = mirBuildPredecessors(mir);

    MirDomScratch scratch;
    MirFuncId const f0 = mir.funcAt(0);
    MirFuncId const f1 = mir.funcAt(1);
    MirFuncId const f2 = mir.funcAt(2);
    compareFreshVsScratch(mir, f0, preds, scratch, "call1 big");
    compareFreshVsScratch(mir, f1, preds, scratch, "call2 small-after-big");
    compareFreshVsScratch(mir, f2, preds, scratch,
                          "call3 unreachable-into-reachable");
    compareFreshVsScratch(mir, f2, preds, scratch, "call4 same-fn-twice");
    compareFreshVsScratch(mir, f0, preds, scratch, "call5 big-again");
    compareFreshVsScratch(mir, f1, preds, scratch, "call6 small-last");
}

// The F2 bound: a synthetic out-of-range entry slot must neither write nor
// record out of bounds — and must produce the same all-default tree the
// fresh path returns for it (the core's own early-return tolerance).
TEST(MirDomScratch, OutOfRangeEntrySlotIsBoundedAndIdentical) {
    TypeInterner interner{CompilationUnitId{1}};
    auto d = buildDiamond(interner);
    auto const preds = mirBuildPredecessors(d.mir);

    // A fabricated entry far past blockCount, with an empty order (an RPO
    // from a nonsense entry is empty).
    MirBlockId const bogus{
        static_cast<std::uint32_t>(d.mir.blockCount()) + 41u, d.mir.id().v};
    std::vector<MirBlockId> const emptyOrder;
    auto const fresh = computeMirDomTree(d.mir, bogus, emptyOrder, preds);

    MirDomScratch scratch;
    auto const& viaScratch =
        computeMirDomTree(d.mir, bogus, emptyOrder, preds, scratch);
    expectDomTreesIdenticalFull(fresh, viaScratch, "oob entry");

    // And the scratch must still be healthy for a real call afterwards.
    compareFreshVsScratch(d.mir, d.mir.funcAt(0), preds, scratch,
                          "real call after oob entry");
}

// Seeded randomized-CFG sweep (mirrors test_mir_memory_clobbers.cpp's):
// 25 modules × every function in sequence through ONE scratch per module,
// full-array dom + children comparison after every call. Deterministic.
TEST(MirDomScratch, RandomizedCfgSweepMatchesFresh) {
    std::mt19937 rng{0xD0357EEDu};
    for (std::uint32_t moduleIdx = 0; moduleIdx < 25; ++moduleIdx) {
        TypeInterner interner{CompilationUnitId{1}};
        TypeId const i32   = interner.primitive(TypeKind::I32);
        TypeId const boolT = interner.primitive(TypeKind::Bool);
        TypeId const params[] = {boolT};
        TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

        MirBuilder mb;
        std::uint32_t const nfn = 2u + rng() % 3u;   // 2..4 functions
        for (std::uint32_t fi = 0; fi < nfn; ++fi) {
            mb.addFunction(fnSig, SymbolId{100u + fi});
            std::uint32_t const nb = 3u + rng() % 5u;   // 3..7 blocks
            std::vector<MirBlockId> blocks;
            blocks.reserve(nb);
            for (std::uint32_t i = 0; i < nb; ++i) {
                blocks.push_back(mb.createBlock(
                    i == 0 ? StructCfMarker::EntryBlock
                           : StructCfMarker::Linear));
            }
            MirInstId cond{};
            for (std::uint32_t i = 0; i < nb; ++i) {
                mb.beginBlock(blocks[i]);
                if (i == 0) cond = mb.addArg(0, boolT);
                if (i + 1 == nb) {
                    MirLiteralValue z; z.value = std::int64_t{0};
                    z.core = TypeKind::I32;
                    mb.addReturn(mb.addConst(z, i32));
                } else if (rng() % 10u < 4u) {
                    // Random extra edge — forward or BACKWARD (loops), or to
                    // a skipped block (leaves some blocks unreachable).
                    MirBlockId const other = blocks[rng() % nb];
                    mb.addCondBr(cond, other, blocks[i + 1]);
                } else {
                    mb.addBr(blocks[i + 1]);
                }
            }
        }
        Mir mir = std::move(mb).finish();
        auto const preds = mirBuildPredecessors(mir);

        MirDomScratch scratch;
        std::size_t const nf = mir.moduleFuncCount();
        for (std::uint32_t fi = 0; fi < nf; ++fi) {
            compareFreshVsScratch(mir, mir.funcAt(fi), preds, scratch,
                                  "randomized sweep");
        }
    }
}
