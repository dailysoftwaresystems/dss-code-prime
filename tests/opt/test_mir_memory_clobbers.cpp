// D-OPT-MEMORYSSA-CLOBBER-WALK — `MirMemoryClobbers` (the pass-wide memory-
// clobber index + memoized reachability that replaced the per-Load-query
// region/loop walks in CSE + LICM).
//
// The proof strategy is DIFFERENTIAL: the reference walkers in mir_alias.hpp
// (`mirRegionBetween` + `mirAnyMayAliasingStoreInRegion` /
// `mirAnyMayAliasingStoreInLoop` — kept as the oracle) DEFINE the query
// semantics; every test asserts the index answers EQUAL the oracle's over
// exhaustive (block, block) / (block, range) sweeps × the full
// StrictTbaa × charTypesAliasAll flag matrix, on curated shapes AND a
// seeded randomized-CFG sweep. Plus the named FrontierStopsAtNonAliasingStore
// regression shape: the design-audit's concrete counterexample against the
// REJECTED "nearest-clobber frontier" design (a nearer non-aliasing Store must
// NOT mask a farther aliasing one) — pinned so that design can never sneak back.
//
// D-OPT-CSE-CLOBBER-COVER-CHOKEPOINT adds a SECOND proof strategy for Q5
// (`anyClobberBetweenPoints`), because equality against a same-shaped oracle
// cannot catch a hole that BOTH sides share. `mirAnyClobberOnPathBetweenPoints`
// is not a re-composition of the same slices: it walks the (block, index)
// PROGRAM-POINT graph exhaustively, so it is total by construction, and
// `assertPointCoverIsSound` asserts `spec ⇒ production` over EVERY ordered pair
// of program points × the flag matrix on every shape in this file. Production
// may over-approximate; it may never MISS. Deleting any one of the four slices
// reds this file, and a non-vacuity counter refuses a silent specification.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_dom.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/analysis/mir_alias.hpp"
#include "opt/analysis/mir_escape.hpp"
#include "opt/analysis/mir_memory_clobbers.hpp"
#include "opt/passes/cse.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <vector>

using namespace dss;
using dss::opt::analysis::MirMemoryClobbers;
using dss::opt::analysis::MirPointerEscape;
using dss::opt::analysis::StrictTbaa;

namespace {

// The reference in-block range scan — exactly the pre-index `storesClobber`
// lambda shape (cse.cpp): every instruction in [lo, hi), the ONE predicate.
bool oracleBlockRange(Mir const& mir, TypeInterner const& interner,
                      MirInstId loadPtr, MirBlockId blk,
                      std::uint32_t lo, std::uint32_t hi,
                      StrictTbaa st, bool ca,
                      dss::opt::analysis::MirPointerEscape const* esc = nullptr) {
    for (std::uint32_t j = lo; j < hi; ++j) {
        if (dss::opt::analysis::mirInstClobbersLoadPtr(
                mir, interner, loadPtr, mir.blockInstAt(blk, j), st, ca, esc)) {
            return true;
        }
    }
    return false;
}

// The reference between-region query — exactly the pre-index CSE slice (b).
bool oracleBetween(Mir const& mir, TypeInterner const& interner,
                   MirInstId loadPtr, MirBlockId a, MirBlockId b,
                   std::vector<std::vector<MirBlockId>> const& preds,
                   StrictTbaa st, bool ca,
                   dss::opt::analysis::MirPointerEscape const* esc = nullptr) {
    auto const region = dss::opt::analysis::mirRegionBetween(mir, a, b, preds);
    return dss::opt::analysis::mirAnyMayAliasingStoreInRegion(
        mir, interner, loadPtr, region, st, ca, esc);
}

// Every block of every function in the module.
std::vector<MirBlockId> allBlocks(Mir const& mir) {
    std::vector<MirBlockId> out;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            out.push_back(mir.funcBlockAt(f, bi));
        }
    }
    return out;
}

// The exhaustive differential sweep: for every flag combination, every given
// load pointer, every (block, block) pair (Q2 vs the region oracle), every
// (block, lo, hi) range (Q1 vs the range oracle), and every whole-module
// block list prefix (Q3 vs the loop oracle) — index == oracle, bit for bit.
void assertDifferentialEquality(Mir const& mir, TypeInterner const& interner,
                                std::vector<MirInstId> const& loadPtrs) {
    auto const preds = mirBuildPredecessors(mir);
    MirMemoryClobbers const idx{mir, preds};
    // ★ The oracle MUST reason at the production query PRECISION. The index
    // builds a MirPointerEscape for its module and threads it into every
    // probe (Rule 3b); an oracle left at the default nullptr would answer a
    // DIFFERENT question and every equality below would be noise.
    auto const* esc = &idx.escape();
    auto const blocks = allBlocks(mir);

    struct FlagCase { StrictTbaa st; bool ca; };
    FlagCase const flagMatrix[] = {
        {StrictTbaa::No,  true}, {StrictTbaa::No,  false},
        {StrictTbaa::Yes, true}, {StrictTbaa::Yes, false},
    };
    for (auto const [st, ca] : flagMatrix) {
        for (MirInstId const lp : loadPtrs) {
            for (MirBlockId const a : blocks) {
                for (MirBlockId const b : blocks) {
                    EXPECT_EQ(idx.anyClobberBetween(interner, lp, a, b, st, ca),
                              oracleBetween(mir, interner, lp, a, b, preds, st, ca, esc))
                        << "Q2 diverges from the region oracle: between #"
                        << a.v << " and #" << b.v << " for loadPtr v=" << lp.v
                        << " st=" << (st == StrictTbaa::Yes) << " ca=" << ca;
                }
                std::uint32_t const n = mir.blockInstCount(a);
                for (std::uint32_t lo = 0; lo <= n; ++lo) {
                    for (std::uint32_t hi = lo; hi <= n; ++hi) {
                        EXPECT_EQ(idx.anyClobberInBlockRange(interner, lp, a,
                                                             lo, hi, st, ca),
                                  oracleBlockRange(mir, interner, lp, a,
                                                   lo, hi, st, ca, esc))
                            << "Q1 diverges from the range oracle: block #"
                            << a.v << " [" << lo << ", " << hi << ") loadPtr v="
                            << lp.v;
                    }
                }
            }
            // Q3 vs the loop oracle over every block-list prefix (covers the
            // empty list, single blocks, and the whole "body").
            for (std::size_t take = 0; take <= blocks.size(); ++take) {
                std::vector<MirBlockId> body(blocks.begin(),
                                             blocks.begin()
                                                 + static_cast<std::ptrdiff_t>(take));
                EXPECT_EQ(idx.anyClobberInBlocks(interner, lp, body, st, ca),
                          dss::opt::analysis::mirAnyMayAliasingStoreInLoop(
                              mir, interner, lp, body, st, ca, esc))
                    << "Q3 diverges from the loop oracle: prefix size " << take
                    << " loadPtr v=" << lp.v;
            }
        }
    }
}

// ── D-OPT-CSE-CLOBBER-COVER-CHOKEPOINT ───────────────────────────────
// `anyClobberBetweenPoints` (Q5) is the ONE point-to-point clobber cover, and
// its completeness is CHECKABLE instead of argued. `mirAnyClobberOnPathBetweenPoints`
// (mir_alias.hpp) answers the same question by exhaustively walking the
// (block, instruction-index) PROGRAM-POINT graph — total by construction, so no
// slice can go missing from it — and this sweep asserts `spec ⇒ production`
// over EVERY ordered pair of program points in the module × the full flag
// matrix. Production is allowed to OVER-approximate (its reachability is
// block-granular, and a cross-function pair trivially over-reports); it is
// never allowed to MISS. Each of the four slices has curated shapes below that
// red when it is deleted:
//   (a) producer's block TAIL   — LinearChain / Diamond
//   (b) STRICTLY-BETWEEN blocks — Diamond / LinearChain
//   (c) consumer's block HEAD   — LinearChain / SelfLoop
//   (d) WRAP-AROUND tail        — SelfLoop / MultiBlockLoop  (this is TF-C58)
void assertPointCoverIsSound(Mir const& mir, TypeInterner const& interner,
                             std::vector<MirInstId> const& loadPtrs) {
    auto const preds = mirBuildPredecessors(mir);
    MirMemoryClobbers const idx{mir, preds};
    // ★ The oracle MUST reason at the production query PRECISION. The index
    // builds a MirPointerEscape for its module and threads it into every
    // probe (Rule 3b); an oracle left at the default nullptr would answer a
    // DIFFERENT question and every equality below would be noise.
    auto const* esc = &idx.escape();
    auto const blocks = allBlocks(mir);

    struct FlagCase { StrictTbaa st; bool ca; };
    FlagCase const flagMatrix[] = {
        {StrictTbaa::No,  true}, {StrictTbaa::No,  false},
        {StrictTbaa::Yes, true}, {StrictTbaa::Yes, false},
    };
    // A specification that never fires would make every EXPECT below
    // unreachable and the sweep a green no-op — count the firings and assert
    // the instrument was not blind.
    std::size_t specFired = 0;
    for (auto const [st, ca] : flagMatrix) {
        for (MirInstId const lp : loadPtrs) {
            for (MirBlockId const a : blocks) {
                std::uint32_t const na = mir.blockInstCount(a);
                for (std::uint32_t ai = 0; ai < na; ++ai) {
                    for (MirBlockId const b : blocks) {
                        std::uint32_t const nb = mir.blockInstCount(b);
                        for (std::uint32_t bi = 0; bi < nb; ++bi) {
                            // Same block ⇒ the producer must precede its reuse;
                            // an unordered pair is a contract violation both
                            // sides fail loud on, not a question with an answer.
                            if (a.v == b.v && ai >= bi) continue;
                            if (!dss::opt::analysis::mirAnyClobberOnPathBetweenPoints(
                                    mir, interner, lp, a, ai, b, bi, preds, st, ca,
                                    esc)) {
                                continue;
                            }
                            ++specFired;
                            EXPECT_TRUE(idx.anyClobberBetweenPoints(
                                            interner, lp, a, ai, b, bi, st, ca))
                                << "the point-to-point cover MISSED a clobber the "
                                   "program-point specification found: from #"
                                << a.v << "[" << ai << "] to #" << b.v << "["
                                << bi << "] for loadPtr v=" << lp.v
                                << " st=" << (st == StrictTbaa::Yes) << " ca=" << ca
                                << " — a missing slice is a silent stale-Load "
                                   "miscompile (D-OPT-CSE-LOAD-BACKEDGE-TAIL)";
                        }
                    }
                }
            }
        }
    }
    EXPECT_GT(specFired, 0u)
        << "the program-point specification never once reported a clobber on "
           "this shape — every assertion above was unreachable, so this sweep "
           "proved nothing (the blindness of the instrument)";
}

// Locate an instruction by opcode + ordinal within a block (never a line
// number, and robust to a shape gaining an instruction).
std::uint32_t idxOfNthOpcode(Mir const& mir, MirBlockId b, MirOpcode op,
                             std::uint32_t nth) {
    std::uint32_t const n = mir.blockInstCount(b);
    std::uint32_t seen = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (mir.instOpcode(mir.blockInstAt(b, i)) != op) continue;
        if (seen == nth) return i;
        ++seen;
    }
    return n;   // caller ASSERTs on this
}

} // namespace

// Linear chain, multiple clobbers of two distinct allocas — the bread-and-
// butter shape (also exercises multi-clobber blocks + non-aliasing stores).
TEST(MirMemoryClobbers, LinearChainDifferentialEquality) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const b1 = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const b2 = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const b3 = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const b4 = mb.createBlock(StructCfMarker::Linear);

    mb.beginBlock(b1);
    MirInstId const p = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirInstId const q = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(v0, i32);
    MirInstId const sp0[] = {c0, p};
    (void)mb.addInst(MirOpcode::Store, sp0, InvalidType);
    MirInstId const sq0[] = {c0, q};
    (void)mb.addInst(MirOpcode::Store, sq0, InvalidType);
    MirInstId const lops[] = {p};
    (void)mb.addInst(MirOpcode::Load, lops, i32);
    mb.addBr(b2);

    mb.beginBlock(b2);
    MirLiteralValue v1; v1.value = std::int64_t{1}; v1.core = TypeKind::I32;
    MirInstId const c1 = mb.addConst(v1, i32);
    MirInstId const sp1[] = {c1, p};
    (void)mb.addInst(MirOpcode::Store, sp1, InvalidType);
    mb.addBr(b3);

    mb.beginBlock(b3);
    MirInstId const sq1[] = {c1, q};
    (void)mb.addInst(MirOpcode::Store, sq1, InvalidType);
    mb.addBr(b4);

    mb.beginBlock(b4);
    MirInstId const ld = mb.addInst(MirOpcode::Load, lops, i32);
    mb.addReturn(ld);
    Mir mir = std::move(mb).finish();

    assertDifferentialEquality(mir, interner, {p, q});
    assertPointCoverIsSound(mir, interner, {p, q});
}

// The design-audit's REJECTED-frontier counterexample, pinned forever:
//   B1: Load P → B2: Store P → B3: Store Q → B4: Load P   (P, Q distinct)
// A "nearest clobber-bearing block backward from B4" walk stops at B3, alias-
// tests only the non-aliasing Store Q, and ADMITS the CSE — a silent stale-load
// miscompile (B2 overwrote *P). The complete region enumeration must REFUSE.
TEST(MirMemoryClobbers, FrontierStopsAtNonAliasingStoreMustStillRefuse) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const b1 = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const b2 = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const b3 = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const b4 = mb.createBlock(StructCfMarker::Linear);

    mb.beginBlock(b1);
    MirInstId const p = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirInstId const q = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(v0, i32);
    MirInstId const sp0[] = {c0, p};
    (void)mb.addInst(MirOpcode::Store, sp0, InvalidType);
    MirInstId const lops[] = {p};
    MirInstId const ld1 = mb.addInst(MirOpcode::Load, lops, i32);
    mb.addBr(b2);

    mb.beginBlock(b2);   // the FARTHER, ALIASING store (in-region)
    MirLiteralValue v7; v7.value = std::int64_t{7}; v7.core = TypeKind::I32;
    MirInstId const c7 = mb.addConst(v7, i32);
    MirInstId const sp[] = {c7, p};
    (void)mb.addInst(MirOpcode::Store, sp, InvalidType);
    mb.addBr(b3);

    mb.beginBlock(b3);   // the NEARER, NON-aliasing store (distinct alloca)
    MirInstId const sq[] = {c7, q};
    (void)mb.addInst(MirOpcode::Store, sq, InvalidType);
    mb.addBr(b4);

    mb.beginBlock(b4);
    MirInstId const ld2 = mb.addInst(MirOpcode::Load, lops, i32);
    MirInstId const sum[] = {ld1, ld2};
    MirInstId const r = mb.addInst(MirOpcode::Add, sum, i32);
    mb.addReturn(r);
    Mir mir = std::move(mb).finish();

    // Index level: the between-query MUST see B2's aliasing store.
    auto const preds = mirBuildPredecessors(mir);
    MirMemoryClobbers const idx{mir, preds};
    EXPECT_TRUE(idx.anyClobberBetween(interner, p, b1, b4,
                                      StrictTbaa::No, true))
        << "the farther aliasing Store P (B2) must clobber — a nearer "
           "non-aliasing Store Q (B3) can never mask it";
    EXPECT_TRUE(oracleBetween(mir, interner, p, b1, b4, preds,
                              StrictTbaa::No, true));

    // End-to-end: real CSE must REFUSE the Load (0 CSEs).
    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.instructionsCsed, 0u)
        << "CSE admitted a Load across an aliasing Store — the frontier "
           "under-reporting miscompile";
}

// Diamond with the clobber on ONE arm (join-precision) + full sweep.
TEST(MirMemoryClobbers, DiamondOneArmClobberDifferentialEquality) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tArm  = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const fArm  = mb.createBlock(StructCfMarker::IfElse);
    MirBlockId const join  = mb.createBlock(StructCfMarker::IfJoin);

    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    MirInstId const p = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(v0, i32);
    MirInstId const s0[] = {c0, p};
    (void)mb.addInst(MirOpcode::Store, s0, InvalidType);
    MirInstId const lops[] = {p};
    (void)mb.addInst(MirOpcode::Load, lops, i32);
    mb.addCondBr(cond, tArm, fArm);

    mb.beginBlock(tArm);   // the clobbering arm
    MirLiteralValue v1; v1.value = std::int64_t{1}; v1.core = TypeKind::I32;
    MirInstId const c1 = mb.addConst(v1, i32);
    MirInstId const s1[] = {c1, p};
    (void)mb.addInst(MirOpcode::Store, s1, InvalidType);
    mb.addBr(join);

    mb.beginBlock(fArm);   // the clean arm
    mb.addBr(join);

    mb.beginBlock(join);
    MirInstId const ld = mb.addInst(MirOpcode::Load, lops, i32);
    mb.addReturn(ld);
    Mir mir = std::move(mb).finish();

    assertDifferentialEquality(mir, interner, {p});
    assertPointCoverIsSound(mir, interner, {p});
}

// Loop with the clobber in the body: the back-edge region case (a block
// bwd-reachable from the use ONLY via the back edge is still in the region —
// reachability semantics, not path semantics) + full sweep.
TEST(MirMemoryClobbers, LoopBackEdgeRegionDifferentialEquality) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);

    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    MirInstId const p = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(v0, i32);
    MirInstId const s0[] = {c0, p};
    (void)mb.addInst(MirOpcode::Store, s0, InvalidType);
    MirInstId const lops[] = {p};
    (void)mb.addInst(MirOpcode::Load, lops, i32);
    mb.addBr(header);

    mb.beginBlock(header);
    (void)mb.addInst(MirOpcode::Load, lops, i32);
    mb.addCondBr(cond, body, exitB);

    mb.beginBlock(body);   // clobber reached from header only via the back edge
    MirLiteralValue v9; v9.value = std::int64_t{9}; v9.core = TypeKind::I32;
    MirInstId const c9 = mb.addConst(v9, i32);
    MirInstId const s9[] = {c9, p};
    (void)mb.addInst(MirOpcode::Store, s9, InvalidType);
    mb.addBr(header);

    mb.beginBlock(exitB);
    MirInstId const ld = mb.addInst(MirOpcode::Load, lops, i32);
    mb.addReturn(ld);
    Mir mir = std::move(mb).finish();

    // The loop body is in region(entry → header): bwd-reachable from header
    // via the back edge, fwd-reachable from entry. Both must see the clobber.
    auto const preds = mirBuildPredecessors(mir);
    MirMemoryClobbers const idx{mir, preds};
    EXPECT_TRUE(idx.anyClobberBetween(interner, p, entry, header,
                                      StrictTbaa::No, true))
        << "the loop-body Store is region-reachable only via the back edge";

    assertDifferentialEquality(mir, interner, {p});
    assertPointCoverIsSound(mir, interner, {p});
}

// Every opaque-clobber opcode mints a def: the non-Store `opcodeClobbersMemory`
// members that are BUILDABLE as ordinary insts (SehTryEnd, CompilerBarrier,
// AtomicFence, AtomicCas) sit between two Loads; the range queries must see each.
// (The clobbering TERMINATORS — SehTryBegin / SehFilterReturn — get their own pin
// below; Call/IntrinsicCall ride the same positive-list arm by construction.)
// D-CSUBSET-ATOMIC-FENCE joined this set as the 4th buildable non-Store clobber:
// the INDEX is a different chokepoint from the region walk pinned in
// test_mir_alias — this one memoizes clobber defs per block, and today answers
// correctly only because it queries `opcodeClobbersMemory` generically. The
// AtomicFence slot pin below is what reds if that generic query is ever traded
// for a hand-rolled opcode allowlist that forgets the fence.
TEST(MirMemoryClobbers, OpaqueClobberOpsMintDefsDifferentialEquality) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const p = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(v0, i32);
    MirInstId const s0[] = {c0, p};
    (void)mb.addInst(MirOpcode::Store, s0, InvalidType);
    MirInstId const lops[] = {p};
    (void)mb.addInst(MirOpcode::Load, lops, i32);
    (void)mb.addInst(MirOpcode::SehTryEnd, {}, InvalidType, /*payload=*/0);
    (void)mb.addInst(MirOpcode::Load, lops, i32);
    (void)mb.addInst(MirOpcode::CompilerBarrier, {}, InvalidType);
    (void)mb.addInst(MirOpcode::Load, lops, i32);
    // D-CSUBSET-ATOMIC-FENCE: the REAL-instruction sibling of CompilerBarrier above
    // — 0 operands, no result (R::None ⇒ InvalidType), the C11 order in `payload`
    // (seq_cst=5, __sync_synchronize's sole shipped bake).
    (void)mb.addInst(MirOpcode::AtomicFence, {}, InvalidType, /*payload=*/5);
    (void)mb.addInst(MirOpcode::Load, lops, i32);
    MirLiteralValue v1; v1.value = std::int64_t{1}; v1.core = TypeKind::I32;
    MirInstId const c1 = mb.addConst(v1, i32);
    MirInstId const casOps[] = {p, c0, c1};
    (void)mb.addInst(MirOpcode::AtomicCas, casOps, i32);
    MirInstId const ld = mb.addInst(MirOpcode::Load, lops, i32);
    mb.addReturn(ld);
    Mir mir = std::move(mb).finish();

    // The AtomicFence slot ALONE must mint a clobber def in the INDEX. The
    // differential sweep below compares index-vs-oracle and BOTH consult
    // `opcodeClobbersMemory`, so it cannot red on a dropped membership — this
    // direct one-slot pin is what reds if the memoized index ever trades the
    // generic predicate for a hand-rolled opcode allowlist that omits the fence.
    auto const preds = mirBuildPredecessors(mir);
    MirMemoryClobbers const idx{mir, preds};
    std::uint32_t const n = mir.blockInstCount(entry);
    std::uint32_t fenceIdx = n;   // located by opcode — the shape above may grow
    for (std::uint32_t i = 0; i < n; ++i) {
        if (mir.instOpcode(mir.blockInstAt(entry, i)) == MirOpcode::AtomicFence) {
            fenceIdx = i;
            break;
        }
    }
    ASSERT_LT(fenceIdx, n) << "the AtomicFence must survive into the finished Mir";
    EXPECT_TRUE(idx.anyClobberInBlockRange(interner, p, entry, fenceIdx,
                                           fenceIdx + 1u, StrictTbaa::No, true))
        << "the standalone CPU fence (AtomicFence) must mint a clobber def — no "
           "Load may be forwarded across it (D-CSUBSET-ATOMIC-FENCE)";

    assertDifferentialEquality(mir, interner, {p});
    assertPointCoverIsSound(mir, interner, {p});
}

// Off-by-one boundary pins on Q1's [lo, hi) filter + the TERMINATOR-slot
// clobber: a SehTryBegin block TERMINATOR (the region-opening clobber, c115)
// must be seen by a tail range that includes the terminator index — the
// builder walks blockInstCount() INCLUSIVE of the terminator.
TEST(MirMemoryClobbers, BoundaryAndSehTryBeginTerminatorPins) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry   = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tryB    = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const filterB = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const handler = mb.createBlock(StructCfMarker::Linear);

    mb.beginBlock(entry);
    MirInstId const p = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(v0, i32);
    MirInstId const s0[] = {c0, p};
    MirInstId const st = mb.addInst(MirOpcode::Store, s0, InvalidType);
    (void)st;
    MirInstId const lops[] = {p};
    (void)mb.addInst(MirOpcode::Load, lops, i32);
    (void)mb.addSehTryBegin(tryB, filterB, /*regionId=*/0);

    mb.beginBlock(tryB);
    MirInstId const ld = mb.addInst(MirOpcode::Load, lops, i32);
    mb.addReturn(ld);

    mb.beginBlock(filterB);
    MirLiteralValue one; one.value = std::int64_t{1}; one.core = TypeKind::I32;
    MirInstId const fv = mb.addConst(one, i32);
    (void)mb.addSehFilterReturn(fv, handler, /*regionId=*/0);

    mb.beginBlock(handler);
    mb.addReturn(mb.addConst(one, i32));
    Mir mir = std::move(mb).finish();

    auto const preds = mirBuildPredecessors(mir);
    MirMemoryClobbers const idx{mir, preds};

    // entry's layout: [0]=Alloca [1]=Const [2]=Store [3]=Load [4]=SehTryBegin.
    std::uint32_t const n = mir.blockInstCount(entry);
    ASSERT_EQ(n, 5u);
    auto const F = StrictTbaa::No;
    // The Store at idx 2: excluded when lo starts past it / hi stops at it…
    EXPECT_FALSE(idx.anyClobberInBlockRange(interner, p, entry, 3u, 4u, F, true))
        << "[3,4) holds only the Load — no clobber";
    EXPECT_TRUE(idx.anyClobberInBlockRange(interner, p, entry, 2u, 3u, F, true))
        << "[2,3) holds exactly the Store";
    EXPECT_FALSE(idx.anyClobberInBlockRange(interner, p, entry, 0u, 2u, F, true))
        << "[0,2) is Alloca+Const — no clobber";
    // …and the TERMINATOR slot: [4,5) is exactly the SehTryBegin — an opaque
    // clobber the tail range MUST see (the c115 SEH-region soundness surface).
    EXPECT_TRUE(idx.anyClobberInBlockRange(interner, p, entry, 4u, 5u, F, true))
        << "the SehTryBegin block terminator must mint a clobber def";
    EXPECT_TRUE(idx.anyClobberInBlockRange(interner, p, entry, 3u, n, F, true))
        << "the canonical-tail range including the terminator must clobber";

    assertDifferentialEquality(mir, interner, {p});
    assertPointCoverIsSound(mir, interner, {p});
}

// Multi-function module: the ledger is whole-module, the queries per-function —
// clobbers in one function must not leak into another's region answers (block
// slots are module-global; reachability confines them). Function 0 carries a
// clobber between its Loads; function 1 is clean.
TEST(MirMemoryClobbers, MultiFunctionIsolationDifferentialEquality) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    std::vector<MirInstId> ptrs;
    std::vector<MirBlockId> fn1Blocks;
    for (std::uint32_t fnIdx = 0; fnIdx < 2; ++fnIdx) {
        mb.addFunction(fnSig, SymbolId{100u + fnIdx});
        MirBlockId const b1 = mb.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const b2 = mb.createBlock(StructCfMarker::Linear);
        if (fnIdx == 1) { fn1Blocks = {b1, b2}; }
        mb.beginBlock(b1);
        MirInstId const p = mb.addInst(MirOpcode::Alloca, {}, ptr);
        ptrs.push_back(p);
        MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
        MirInstId const c0 = mb.addConst(v0, i32);
        MirInstId const s0[] = {c0, p};
        (void)mb.addInst(MirOpcode::Store, s0, InvalidType);
        MirInstId const lops[] = {p};
        (void)mb.addInst(MirOpcode::Load, lops, i32);
        if (fnIdx == 0) {   // in-between clobber in function 0 ONLY
            MirLiteralValue v5; v5.value = std::int64_t{5}; v5.core = TypeKind::I32;
            MirInstId const c5 = mb.addConst(v5, i32);
            MirInstId const s5[] = {c5, p};
            (void)mb.addInst(MirOpcode::Store, s5, InvalidType);
        }
        mb.addBr(b2);
        mb.beginBlock(b2);
        MirInstId const ld = mb.addInst(MirOpcode::Load, lops, i32);
        mb.addReturn(ld);
    }
    Mir mir = std::move(mb).finish();

    // Function 1's between-region (its entry → its b2) must be clean even
    // though function 0's blocks carry clobbers in the same module ledger.
    auto const preds = mirBuildPredecessors(mir);
    MirMemoryClobbers const idx{mir, preds};
    EXPECT_FALSE(idx.anyClobberBetween(interner, ptrs[1],
                                       fn1Blocks[0], fn1Blocks[1],
                                       StrictTbaa::No, true))
        << "function 0's clobbers must not leak into function 1's region";

    assertDifferentialEquality(mir, interner, ptrs);
    assertPointCoverIsSound(mir, interner, ptrs);
}

// Seeded randomized-CFG sweep: 25 modules × full differential equality. The
// strongest breadth pin — any enumeration/reachability divergence between the
// index and the reference walkers on ANY (pair, range, flags) combination
// fails. Deterministic (fixed seed; no wall-clock/randomness dependence).
TEST(MirMemoryClobbers, RandomizedCfgDifferentialSweep) {
    std::mt19937 rng{0xD55C0DEu};
    for (std::uint32_t moduleIdx = 0; moduleIdx < 25; ++moduleIdx) {
        TypeInterner interner{CompilationUnitId{1}};
        TypeId const i32   = interner.primitive(TypeKind::I32);
        TypeId const boolT = interner.primitive(TypeKind::Bool);
        TypeId const ptr   = interner.pointer(i32);
        TypeId const params[] = {boolT};
        TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

        MirBuilder mb;
        mb.addFunction(fnSig, SymbolId{100});
        std::uint32_t const nb = 3u + rng() % 5u;   // 3..7 blocks
        std::vector<MirBlockId> blocks;
        blocks.reserve(nb);
        for (std::uint32_t i = 0; i < nb; ++i) {
            blocks.push_back(mb.createBlock(
                i == 0 ? StructCfMarker::EntryBlock : StructCfMarker::Linear));
        }

        MirInstId cond{};
        std::vector<MirInstId> allocas;
        for (std::uint32_t i = 0; i < nb; ++i) {
            mb.beginBlock(blocks[i]);
            if (i == 0) {
                cond = mb.addArg(0, boolT);
                std::uint32_t const na = 1u + rng() % 2u;   // 1..2 allocas
                for (std::uint32_t a = 0; a < na; ++a) {
                    allocas.push_back(mb.addInst(MirOpcode::Alloca, {}, ptr));
                }
                // Every alloca gets a defining Store (keeps any later pass
                // usage well-defined; irrelevant to the pure index queries).
                for (MirInstId const a : allocas) {
                    MirLiteralValue z; z.value = std::int64_t{0};
                    z.core = TypeKind::I32;
                    MirInstId const cz = mb.addConst(z, i32);
                    MirInstId const sz[] = {cz, a};
                    (void)mb.addInst(MirOpcode::Store, sz, InvalidType);
                }
            }
            // Random body: loads / stores through random allocas / barriers.
            std::uint32_t const bodyOps = rng() % 4u;
            for (std::uint32_t k = 0; k < bodyOps; ++k) {
                std::uint32_t const pick = rng() % 4u;
                MirInstId const a = allocas[rng() % allocas.size()];
                if (pick == 0) {
                    MirInstId const lops[] = {a};
                    (void)mb.addInst(MirOpcode::Load, lops, i32);
                } else if (pick == 1) {
                    MirLiteralValue lv; lv.value = static_cast<std::int64_t>(rng() % 100);
                    lv.core = TypeKind::I32;
                    MirInstId const cv = mb.addConst(lv, i32);
                    MirInstId const sv[] = {cv, a};
                    (void)mb.addInst(MirOpcode::Store, sv, InvalidType);
                } else if (pick == 2) {
                    (void)mb.addInst(MirOpcode::CompilerBarrier, {},
                                     InvalidType);
                } else {
                    MirLiteralValue lv; lv.value = std::int64_t{2};
                    lv.core = TypeKind::I32;
                    MirInstId const cv = mb.addConst(lv, i32);
                    (void)mb.addInst(MirOpcode::Add,
                                     std::array<MirInstId, 2>{cv, cv}, i32);
                }
            }
            // Terminator: last block returns; others branch (30% CondBr with a
            // random extra target — forward or BACKWARD, minting loops).
            if (i + 1 == nb) {
                MirLiteralValue z; z.value = std::int64_t{0};
                z.core = TypeKind::I32;
                mb.addReturn(mb.addConst(z, i32));
            } else if (rng() % 10u < 3u) {
                MirBlockId const other = blocks[rng() % nb];
                mb.addCondBr(cond, other, blocks[i + 1]);
            } else {
                mb.addBr(blocks[i + 1]);
            }
        }
        Mir mir = std::move(mb).finish();
        assertDifferentialEquality(mir, interner, allocas);
        assertPointCoverIsSound(mir, interner, allocas);
    }
}

// D-OPT-CSE-CLOBBER-COVER-CHOKEPOINT — the TIGHT verdicts. The soundness sweeps
// above prove the cover never MISSES; they cannot prove it stays PRECISE, and a
// cover that answered "clobber" unconditionally would pass every one of them
// while silently disabling Load CSE. These three shapes pin the exact boolean
// on the pair that decides TF-C58, plus the two RE-EXECUTION-LEMMA classes that
// must stay ADMITTED. One builder, three CFGs, so the shapes cannot drift apart:
//
//   SelfLoop        entry[ …, Ld ] → body[ Ld, …, Store, condbr → body ]
//                   wrap does NOT re-execute entry's Ld  ⇒ slice (d) REFUSES
//   MultiBlockLoop  same, but the back edge runs body → latch → body, so the
//                   self-reach walk must POP the worklist to see it
//   ViaHeader       canonical lives in the loop HEADER, so every wrap
//                   RE-EXECUTES it and refreshes the value ⇒ must ADMIT
//
// RED-ON-DISABLE (precision half): make `blockReachesItselfAvoiding` ignore its
// `avoid` argument and the ViaHeader expectation flips false → true.
// RED-ON-DISABLE (soundness half): delete slice (d) and the two loop shapes
// flip true → false.
TEST(MirMemoryClobbers, PointCoverVerdictsOnTheBackEdgeTailShapes) {
    enum class Shape { SelfLoop, MultiBlockLoop, ViaHeader };
    struct Built {
        Mir           mir;
        MirInstId     ptr;
        MirBlockId    canonicalBlock;
        MirBlockId    useBlock;
    };
    auto const build = [](TypeInterner& interner, Shape shape) {
        TypeId const i32   = interner.primitive(TypeKind::I32);
        TypeId const ptrT  = interner.pointer(i32);
        TypeId const boolT = interner.primitive(TypeKind::Bool);
        TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

        bool const viaHeader = (shape == Shape::ViaHeader);
        bool const viaLatch  = (shape == Shape::MultiBlockLoop);

        MirBuilder mb;
        mb.addFunction(fnSig, SymbolId{100});
        MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const header = viaHeader
            ? mb.createBlock(StructCfMarker::LoopHeader) : MirBlockId{};
        MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
        MirBlockId const latch  = viaLatch
            ? mb.createBlock(StructCfMarker::LoopLatch) : MirBlockId{};
        MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);

        mb.beginBlock(entry);
        MirInstId const p = mb.addInst(MirOpcode::Alloca, {}, ptrT);
        MirLiteralValue z; z.value = std::int64_t{0}; z.core = TypeKind::I32;
        MirInstId const c0 = mb.addConst(z, i32);
        MirInstId const st0[] = {c0, p};
        (void)mb.addInst(MirOpcode::Store, st0, InvalidType);
        MirInstId const lops[] = {p};
        // The canonical lives in `entry` for the two loop shapes and in the
        // loop HEADER for the re-execution shape — that ONE difference is what
        // slice (d)'s `avoid` argument is about.
        if (!viaHeader) (void)mb.addInst(MirOpcode::Load, lops, i32);
        mb.addBr(viaHeader ? header : body);

        if (viaHeader) {
            mb.beginBlock(header);
            (void)mb.addInst(MirOpcode::Load, lops, i32);   // the canonical
            mb.addBr(body);
        }

        mb.beginBlock(body);
        MirInstId const ldBody = mb.addInst(MirOpcode::Load, lops, i32);
        MirLiteralValue one; one.value = std::int64_t{1}; one.core = TypeKind::I32;
        MirInstId const c1 = mb.addConst(one, i32);
        MirInstId const inc[] = {ldBody, c1};
        MirInstId const next = mb.addInst(MirOpcode::Add, inc, i32);
        MirInstId const stTail[] = {next, p};
        (void)mb.addInst(MirOpcode::Store, stTail, InvalidType);   // the TAIL store
        MirInstId const cmp[] = {next, c0};
        if (viaLatch) {
            mb.addBr(latch);
            mb.beginBlock(latch);                    // clobber-free back edge
            MirInstId const condL = mb.addInst(MirOpcode::ICmpSlt, cmp, boolT);
            mb.addCondBr(condL, body, exitB);
        } else {
            MirInstId const cond = mb.addInst(MirOpcode::ICmpSlt, cmp, boolT);
            mb.addCondBr(cond, viaHeader ? header : body, exitB);
        }

        mb.beginBlock(exitB);
        mb.addReturn(c0);

        return Built{std::move(mb).finish(), p,
                     viaHeader ? header : entry, body};
    };

    struct Case { Shape shape; bool expectClobber; char const* why; };
    Case const cases[] = {
        {Shape::SelfLoop, true,
         "the body's own back edge carries its TAIL store into the next "
         "execution of the use — slice (d) must REFUSE"},
        {Shape::MultiBlockLoop, true,
         "the wrap body→latch→body carries the TAIL store into the next "
         "execution of the use — slice (d)'s walk must POP the worklist"},
        {Shape::ViaHeader, false,
         "every wrap re-enters the canonical's own block, which RE-EXECUTES "
         "the canonical Load and refreshes the value — must ADMIT"},
    };
    for (auto const& c : cases) {
        TypeInterner interner{CompilationUnitId{1}};
        auto const b = build(interner, c.shape);
        auto const preds = mirBuildPredecessors(b.mir);
        MirMemoryClobbers const idx{b.mir, preds};

        std::uint32_t const ci =
            idxOfNthOpcode(b.mir, b.canonicalBlock, MirOpcode::Load, 0);
        std::uint32_t const ui =
            idxOfNthOpcode(b.mir, b.useBlock, MirOpcode::Load, 0);
        ASSERT_LT(ci, b.mir.blockInstCount(b.canonicalBlock));
        ASSERT_LT(ui, b.mir.blockInstCount(b.useBlock));

        EXPECT_EQ(idx.anyClobberBetweenPoints(interner, b.ptr,
                                              b.canonicalBlock, ci,
                                              b.useBlock, ui,
                                              StrictTbaa::No, true),
                  c.expectClobber) << c.why;
        // The specification must agree EXACTLY here — these shapes are inside
        // the class where the block-granular cover is not an approximation.
        EXPECT_EQ(dss::opt::analysis::mirAnyClobberOnPathBetweenPoints(
                      b.mir, interner, b.ptr, b.canonicalBlock, ci,
                      b.useBlock, ui, preds, StrictTbaa::No, true),
                  c.expectClobber)
            << "the program-point specification itself disagrees: " << c.why;
    }
}

// ═════════════════════════════════════════════════════════════════════
// D-OPT-MEMSSA-WALK-PAST-PRECISION — the (def, location)-keyed walk-past
// layer, and the PROVENANCE precision that made it worth building.
//
// Two independent things are pinned here, because either alone would be a
// half-closure:
//   PRECISION — a Store through a PARAMETER no longer clobbers a Load from a
//     NON-ESCAPED local slot, through the REAL index the passes query. Without
//     it the walk-past would still almost never fire, which is the exact
//     reason the anchor was deferred in the first place.
//   THE SKIP — a def whose verdict for this location is already known is
//     passed over with NO alias reasoning, and the counters say so. The
//     anchor's other trigger was "the index's per-query alias-TEST count
//     dominating a pass", so that count is now a measured number.
//
// And the REJECTED design stays rejected: the memo must never let a nearer
// non-aliasing def mask a farther aliasing one. `FrontierStopsAtNonAliasing
// StoreMustStillRefuse` above pins that for the ORIGINAL frontier proposal;
// `WalkPastMemoNeverMasksAFartherAliasingClobber` below pins it for THIS
// layer, at the higher precision where a nearer def now really does answer No.
// ═════════════════════════════════════════════════════════════════════

namespace {

// One function, one block:
//
//   void probe(int *p) {
//       int keep[4];         // never escapes
//       int leak[4];         // escapes into sink()
//       sink(leak);
//       *p       = 0;        // 3 Stores through the PARAMETER — under Rule 3b
//       *p       = 0;        //   none of them clobbers a `keep` Load, so the
//       *p       = 0;        //   scan must walk PAST all three...
//       keep[0]  = 0;        // ...and still find THIS one.
//   }
struct SlotVsParamShape {
    Mir        mir;
    MirBlockId entry{};
    MirInstId  gepParam{};
    MirInstId  gepKeep{};
    MirInstId  gepLeak{};
    MirInstId  storeKeep{};      // the ONE aliasing clobber of `gepKeep`
    std::uint32_t storeKeepIdx = 0;
};

SlotVsParamShape buildSlotVsParamShape(TypeInterner& interner,
                                       std::uint32_t paramStores) {
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const ptrI32 = interner.pointer(i32);
    TypeId const voidT  = interner.primitive(TypeKind::Void);
    TypeId const params[] = {ptrI32};
    TypeId const fnSig  = interner.fnSig(params, voidT, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const param = mb.addArg(0, ptrI32);
    MirInstId const keep  = mb.addInst(MirOpcode::Alloca, {}, ptrI32);
    MirInstId const leak  = mb.addInst(MirOpcode::Alloca, {}, ptrI32);
    MirLiteralValue z; z.value = std::int64_t{0}; z.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(z, i32);

    SlotVsParamShape s{};
    MirInstId const gp[] = {param, c0};
    s.gepParam = mb.addInst(MirOpcode::Gep, gp, ptrI32);
    MirInstId const gk[] = {keep, c0};
    s.gepKeep  = mb.addInst(MirOpcode::Gep, gk, ptrI32);
    MirInstId const gl[] = {leak, c0};
    s.gepLeak  = mb.addInst(MirOpcode::Gep, gl, ptrI32);

    // `leak` escapes; `keep` never does.
    MirInstId const callee = mb.addGlobalAddr(SymbolId{200}, fnSig);
    MirInstId const callOps[] = {callee, s.gepLeak};
    (void)mb.addInst(MirOpcode::Call, callOps, InvalidType);

    for (std::uint32_t i = 0; i < paramStores; ++i) {
        MirInstId const st[] = {c0, s.gepParam};
        (void)mb.addInst(MirOpcode::Store, st, InvalidType);
    }
    MirInstId const stk[] = {c0, s.gepKeep};
    s.storeKeep = mb.addInst(MirOpcode::Store, stk, InvalidType);
    mb.addReturn();

    s.mir   = std::move(mb).finish();
    s.entry = entry;
    std::uint32_t const n = s.mir.blockInstCount(entry);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (s.mir.blockInstAt(entry, i).v == s.storeKeep.v) s.storeKeepIdx = i;
    }
    return s;
}

} // namespace

// ★ THE PRECISION, through the real index. The `Call` is an opaque clobber of
// everything, so the range queried below deliberately starts AFTER it: what is
// under test is the three parameter Stores, which Rule 3b separates from a
// non-escaped slot and cannot separate from an escaped one.
TEST(MirMemoryClobbers, ParameterStoresDoNotClobberANonEscapedLocalSlot) {
    TypeInterner interner{CompilationUnitId{1}};
    auto s = buildSlotVsParamShape(interner, /*paramStores=*/3);
    auto const preds = mirBuildPredecessors(s.mir);
    MirMemoryClobbers const idx{s.mir, preds};

    ASSERT_GT(s.storeKeepIdx, 0u);
    std::uint32_t const firstParamStore =
        idxOfNthOpcode(s.mir, s.entry, MirOpcode::Store, 0);
    ASSERT_LT(firstParamStore, s.storeKeepIdx);

    EXPECT_FALSE(idx.anyClobberInBlockRange(interner, s.gepKeep, s.entry,
                                            firstParamStore, s.storeKeepIdx,
                                            StrictTbaa::No, true))
        << "three Stores through a PARAMETER cannot reach a local slot whose "
           "address never left the function — this is the precision "
           "D-OPT-MEMSSA-WALK-PAST-PRECISION was waiting on";
    EXPECT_TRUE(idx.anyClobberInBlockRange(interner, s.gepLeak, s.entry,
                                           firstParamStore, s.storeKeepIdx,
                                           StrictTbaa::No, true))
        << "the ESCAPED slot's twin must still be clobbered by the very same "
           "Stores — otherwise the No above is unconditional, not earned";
    EXPECT_TRUE(idx.anyClobberInBlockRange(interner, s.gepKeep, s.entry,
                                           firstParamStore, s.storeKeepIdx + 1u,
                                           StrictTbaa::No, true))
        << "the slot's OWN Store, one index further on, must still clobber it";
    // The whole-module differential + the exhaustive program-point cover, at
    // this precision, on this shape.
    assertDifferentialEquality(s.mir, interner, {s.gepKeep, s.gepLeak, s.gepParam});
    assertPointCoverIsSound(s.mir, interner, {s.gepKeep, s.gepLeak, s.gepParam});
}

// ★ THE SKIP, MEASURED. Run the identical query twice and read the index's own
// counters: the second run must perform ZERO new alias tests and skip exactly
// as many defs as the first run tested. Non-vacuity is asserted first — a run
// that tested nothing would make the second half trivially true.
//
// RED-ON-DISABLE: delete the `verdicts_` lookup in `defClobbers_` (so every
// visit re-runs the predicate) and `aliasTestCount()` doubles while
// `aliasTestSkipCount()` stays 0 — both EXPECTs below fail.
TEST(MirMemoryClobbers, WalkPastMemoSkipsEveryRepeatedAliasTest) {
    TypeInterner interner{CompilationUnitId{1}};
    auto s = buildSlotVsParamShape(interner, /*paramStores=*/3);
    auto const preds = mirBuildPredecessors(s.mir);
    MirMemoryClobbers const idx{s.mir, preds};

    // The range starts AFTER the escaping slot's Call on purpose: an opaque
    // clobber sits at
    // the head of this block and would short-circuit the scan at its FIRST def,
    // leaving one alias test to skip and nothing to measure. Starting at the
    // parameter Stores makes the scan walk past three provably-non-aliasing
    // defs before reaching the one that clobbers.
    std::uint32_t const firstParamStore =
        idxOfNthOpcode(s.mir, s.entry, MirOpcode::Store, 0);
    ASSERT_LT(firstParamStore, s.storeKeepIdx);
    EXPECT_TRUE(idx.anyClobberInBlockRange(interner, s.gepKeep, s.entry,
                                           firstParamStore, s.storeKeepIdx + 1u,
                                           StrictTbaa::No, true));
    std::size_t const testsAfterFirst = idx.aliasTestCount();
    std::size_t const skipsAfterFirst = idx.aliasTestSkipCount();
    EXPECT_GT(testsAfterFirst, 1u)
        << "the first sweep must genuinely alias-test several defs, or the "
           "repeat below proves nothing (the blindness of the instrument)";
    EXPECT_EQ(skipsAfterFirst, 0u)
        << "nothing can be skipped before anything has been recorded";

    EXPECT_TRUE(idx.anyClobberInBlockRange(interner, s.gepKeep, s.entry,
                                           firstParamStore, s.storeKeepIdx + 1u,
                                           StrictTbaa::No, true));
    EXPECT_EQ(idx.aliasTestCount(), testsAfterFirst)
        << "the repeated query must run the alias predicate ZERO more times — "
           "every def it visits has a recorded (def, location) verdict";
    EXPECT_EQ(idx.aliasTestSkipCount(), testsAfterFirst)
        << "and it must have PASSED OVER exactly the defs the first run "
           "tested — same enumeration, no alias reasoning";

    // A DIFFERENT location shares no verdicts: the key carries the location,
    // so the skip says nothing about any other pointer.
    EXPECT_TRUE(idx.anyClobberInBlockRange(interner, s.gepLeak, s.entry,
                                           firstParamStore, s.storeKeepIdx + 1u,
                                           StrictTbaa::No, true));
    EXPECT_GT(idx.aliasTestCount(), testsAfterFirst)
        << "a second LOCATION must be tested afresh — a cache that answered it "
           "from the first location's verdicts would be the rejected "
           "location-independent design";
}

// ★ THE REJECTED DESIGN, re-refuted against THIS layer. A nearer def that
// answers No must never stop the scan reaching a farther def that answers Yes.
// At Rule 3b precision this is no longer hypothetical: the parameter Stores
// really do answer No now, which is exactly the condition under which a
// frontier/stop cache would have started under-reporting.
//
// The multi-block arm is the one that matters: the non-aliasing Stores sit in
// an EARLIER block than the aliasing one, so a per-block stop would record
// "block clean" and never look further.
TEST(MirMemoryClobbers, WalkPastMemoNeverMasksAFartherAliasingClobber) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const ptrI32 = interner.pointer(i32);
    TypeId const voidT  = interner.primitive(TypeKind::Void);
    TypeId const params[] = {ptrI32};
    TypeId const fnSig  = interner.fnSig(params, voidT, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const mid   = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const tail  = mb.createBlock(StructCfMarker::ExitBlock);
    mb.beginBlock(entry);
    MirInstId const param = mb.addArg(0, ptrI32);
    MirInstId const keep  = mb.addInst(MirOpcode::Alloca, {}, ptrI32);
    MirLiteralValue z; z.value = std::int64_t{0}; z.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(z, i32);
    MirInstId const gp[] = {param, c0};
    MirInstId const gepParam = mb.addInst(MirOpcode::Gep, gp, ptrI32);
    MirInstId const gk[] = {keep, c0};
    MirInstId const gepKeep  = mb.addInst(MirOpcode::Gep, gk, ptrI32);
    MirInstId const ld[] = {gepKeep};
    (void)mb.addInst(MirOpcode::Load, ld, i32);      // the value being reused
    (void)mb.addBr(mid);
    // The NEARER block: two Stores that Rule 3b proves cannot reach `keep`.
    mb.beginBlock(mid);
    for (int i = 0; i < 2; ++i) {
        MirInstId const st[] = {c0, gepParam};
        (void)mb.addInst(MirOpcode::Store, st, InvalidType);
    }
    (void)mb.addBr(tail);
    // The FARTHER block: the Store that really does clobber it.
    mb.beginBlock(tail);
    MirInstId const stk[] = {c0, gepKeep};
    (void)mb.addInst(MirOpcode::Store, stk, InvalidType);
    MirInstId const ld2[] = {gepKeep};
    (void)mb.addInst(MirOpcode::Load, ld2, i32);
    mb.addReturn();
    Mir mir = std::move(mb).finish();

    auto const preds = mirBuildPredecessors(mir);
    MirMemoryClobbers const idx{mir, preds};

    // Q2, the strictly-between region: `mid` is clean for `keep` and must not
    // be allowed to stand for the whole region.
    EXPECT_FALSE(idx.anyClobberBetween(interner, gepKeep, entry, tail,
                                       StrictTbaa::No, true))
        << "`mid` holds only parameter Stores — strictly between entry and "
           "tail there is nothing that reaches `keep`";
    // Warm the memo on the clean block, THEN ask the question that must see
    // past it. A stop-cache would answer this one wrong.
    std::uint32_t const tailN = mir.blockInstCount(tail);
    EXPECT_TRUE(idx.anyClobberBetweenPoints(
                    interner, gepKeep, entry,
                    idxOfNthOpcode(mir, entry, MirOpcode::Load, 0),
                    tail, tailN - 1u, StrictTbaa::No, true))
        << "the FARTHER Store in `tail` clobbers the Load's pointer; a nearer "
           "clean block must never mask it — that is the rejected per-block "
           "stop cache, and it ships stale Loads";
    assertDifferentialEquality(mir, interner, {gepKeep, gepParam});
    assertPointCoverIsSound(mir, interner, {gepKeep, gepParam});
}

// ★ THE PRECISION REACHING ITS CONSUMER, at the PASS tier.
//
// Every test above measures the index. This one measures what the index is
// FOR: whether `runCse` — the real pass, unmodified, reading `Mir` exactly as
// the shipped pipelines do — actually reuses a Load across a Store it could
// not previously reason about. A substrate whose precision never reaches a
// consumer is a claim, not a closure.
//
// The two shapes differ by ONE instruction, and it is the escape:
//
//   entry:                              entry:
//     p    = Arg 0                        p    = Arg 0
//     keep = Alloca                       keep = Alloca
//     gk1  = Gep(keep, 0)                 gk1  = Gep(keep, 0)
//                                         Store(gk1, GlobalAddr)  <-- THE ESCAPE
//     l1   = Load(gk1)                    l1   = Load(gk1)
//     Store(0, p)   <-- the parameter     Store(0, p)
//     gk2  = Gep(keep, 0)                 gk2  = Gep(keep, 0)
//     l2   = Load(gk2)                    l2   = Load(gk2)
//     Return(l1 + l2)                     Return(l1 + l2)
//
// The escape is placed BEFORE `l1` on purpose: putting it between the two
// Loads would clobber them for a completely different reason (a Store through
// a global is an ordinary may-alias), and the test would then pass whatever
// the escape analysis said. Where it sits, the ONLY thing between `l1` and
// `l2` is the parameter Store, so the two arms differ in exactly one fact:
// whether `keep`'s address ever left the function.
namespace {

struct ReloadPair {
    Mir       mir;
    MirInstId keep{};
};

// `published` adds the escape; everything else is identical.
ReloadPair buildReloadPair(TypeInterner& interner, bool published) {
    TypeId const i32     = interner.primitive(TypeKind::I32);
    TypeId const ptrI32  = interner.pointer(i32);
    TypeId const ptrPtr  = interner.pointer(ptrI32);
    TypeId const params[] = {ptrI32};
    TypeId const fnSig   = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const p    = mb.addArg(0, ptrI32);
    MirInstId const keep = mb.addInst(MirOpcode::Alloca, {}, ptrI32);
    MirLiteralValue z; z.value = std::int64_t{0}; z.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(z, i32);

    MirInstId const g1[] = {keep, c0};
    MirInstId const gk1  = mb.addInst(MirOpcode::Gep, g1, ptrI32);
    if (published) {
        MirInstId const sym = mb.addGlobalAddr(SymbolId{300}, ptrPtr);
        MirInstId const pub[] = {gk1, sym};
        (void)mb.addInst(MirOpcode::Store, pub, InvalidType);
    }
    MirInstId const ld1[] = {gk1};
    MirInstId const l1 = mb.addInst(MirOpcode::Load, ld1, i32);
    MirInstId const stp[] = {c0, p};
    (void)mb.addInst(MirOpcode::Store, stp, InvalidType);
    MirInstId const g2[] = {keep, c0};
    MirInstId const gk2  = mb.addInst(MirOpcode::Gep, g2, ptrI32);
    MirInstId const ld2[] = {gk2};
    MirInstId const l2 = mb.addInst(MirOpcode::Load, ld2, i32);
    MirInstId const sum[] = {l1, l2};
    mb.addReturn(mb.addInst(MirOpcode::Add, sum, i32));

    return ReloadPair{std::move(mb).finish(), keep};
}

std::size_t countOpcode(Mir const& mir, MirOpcode op) {
    std::size_t n = 0;
    for (MirBlockId const b : allBlocks(mir)) {
        std::uint32_t const c = mir.blockInstCount(b);
        for (std::uint32_t i = 0; i < c; ++i) {
            if (mir.instOpcode(mir.blockInstAt(b, i)) == op) ++n;
        }
    }
    return n;
}

} // namespace

// RED-ON-DISABLE, three independent arms, each already demonstrated:
//   * remove Rule 3b from `mirMayAlias`      -> the non-escaped arm stops
//     CSEing and its Load count stays 2;
//   * disable the forward escape scan        -> the PUBLISHED arm starts
//     CSEing and its Load count drops to 1, which is a STALE LOAD;
//   * break provenance (stop walking Geps)   -> the non-escaped arm stops
//     CSEing, same as arm one.
TEST(MirMemoryClobbers, CseReusesALoadAcrossAParameterStoreOnlyWhenTheSlotStayedHome) {
    TypeInterner interner{CompilationUnitId{1}};

    auto home = buildReloadPair(interner, /*published=*/false);
    ASSERT_EQ(countOpcode(home.mir, MirOpcode::Load), 2u)
        << "the shape must START with two Loads or there is nothing to reuse";
    // NOTE ON THE METRIC: MEASURED — the Cse pass REDIRECTS a redundant
    // instruction and leaves the now-dead original in place for Dce to
    // collect, so an opcode COUNT taken after the pass is UNCHANGED and would
    // make this test assert nothing. This was caught by running it: a first
    // draft asserted the Load count and went red on a module the pass had in
    // fact optimized correctly. `CseResult::instructionsCsed` is the pass's own
    // count of redirections and is the honest measure of what it decided.
    {
        MirPointerEscape const esc{home.mir};
        ASSERT_FALSE(esc.slotEscapes(home.keep))
            << "the control arm's slot must genuinely stay home";
    }
    DiagnosticReporter repHome;
    auto const rHome = dss::opt::passes::runCse(home.mir, interner, repHome);
    EXPECT_TRUE(rHome.ok);
    EXPECT_EQ(rHome.instructionsCsed, 2u)
        << "BOTH the second Gep and the second Load must be REUSED (the Gep is "
           "a pure computation and needs no gate; the Load is the one under "
           "test). The only thing between the two Loads is "
           "a Store through a PARAMETER, which cannot reach a local slot whose "
           "address never left the function. Before Rule 3b this pair was "
           "Maybe and the reuse was refused — that refusal is what "
           "D-OPT-MEMSSA-WALK-PAST-PRECISION was waiting on";

    auto pub = buildReloadPair(interner, /*published=*/true);
    ASSERT_EQ(countOpcode(pub.mir, MirOpcode::Load), 2u);
    {
        MirPointerEscape const esc{pub.mir};
        ASSERT_TRUE(esc.slotEscapes(pub.keep))
            << "the published arm's slot must genuinely escape, or the two "
               "arms below differ in nothing and the sweep is vacuous";
    }
    DiagnosticReporter repPub;
    auto const rPub = dss::opt::passes::runCse(pub.mir, interner, repPub);
    EXPECT_TRUE(rPub.ok);
    EXPECT_EQ(rPub.instructionsCsed, 1u)
        << "only the Gep may be reused here — the Load may NOT. "
           "Once `keep`'s address is published to a global, the parameter MAY "
           "name it — reusing the second Load here would forward a stale value, "
           "which is a silent miscompile and not an optimization";
}
