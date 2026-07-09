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

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_dom.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/analysis/mir_alias.hpp"
#include "opt/analysis/mir_memory_clobbers.hpp"
#include "opt/passes/cse.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <vector>

using namespace dss;
using dss::opt::analysis::MirMemoryClobbers;
using dss::opt::analysis::StrictTbaa;

namespace {

// The reference in-block range scan — exactly the pre-index `storesClobber`
// lambda shape (cse.cpp): every instruction in [lo, hi), the ONE predicate.
bool oracleBlockRange(Mir const& mir, TypeInterner const& interner,
                      MirInstId loadPtr, MirBlockId blk,
                      std::uint32_t lo, std::uint32_t hi,
                      StrictTbaa st, bool ca) {
    for (std::uint32_t j = lo; j < hi; ++j) {
        if (dss::opt::analysis::mirInstClobbersLoadPtr(
                mir, interner, loadPtr, mir.blockInstAt(blk, j), st, ca)) {
            return true;
        }
    }
    return false;
}

// The reference between-region query — exactly the pre-index CSE slice (b).
bool oracleBetween(Mir const& mir, TypeInterner const& interner,
                   MirInstId loadPtr, MirBlockId a, MirBlockId b,
                   std::vector<std::vector<MirBlockId>> const& preds,
                   StrictTbaa st, bool ca) {
    auto const region = dss::opt::analysis::mirRegionBetween(mir, a, b, preds);
    return dss::opt::analysis::mirAnyMayAliasingStoreInRegion(
        mir, interner, loadPtr, region, st, ca);
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
                              oracleBetween(mir, interner, lp, a, b, preds, st, ca))
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
                                                   lo, hi, st, ca))
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
                              mir, interner, lp, body, st, ca))
                    << "Q3 diverges from the loop oracle: prefix size " << take
                    << " loadPtr v=" << lp.v;
            }
        }
    }
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
}

// Every opaque-clobber opcode mints a def: the non-Store `opcodeClobbersMemory`
// members that are BUILDABLE as ordinary insts (SehTryEnd, CompilerBarrier,
// AtomicCas) sit between two Loads; the range queries must see each. (The
// clobbering TERMINATORS — SehTryBegin / SehFilterReturn — get their own pin
// below; Call/IntrinsicCall ride the same positive-list arm by construction.)
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
    MirLiteralValue v1; v1.value = std::int64_t{1}; v1.core = TypeKind::I32;
    MirInstId const c1 = mb.addConst(v1, i32);
    MirInstId const casOps[] = {p, c0, c1};
    (void)mb.addInst(MirOpcode::AtomicCas, casOps, i32);
    MirInstId const ld = mb.addInst(MirOpcode::Load, lops, i32);
    mb.addReturn(ld);
    Mir mir = std::move(mb).finish();

    assertDifferentialEquality(mir, interner, {p});
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
    }
}
