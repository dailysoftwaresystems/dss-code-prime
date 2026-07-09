// MIR-tier SimplifyCFG unit tests.
//
// Scope: branch-folding (CondBr(Const) → Br) + empty-block jump-
// threading (trampoline block elision when successor has no Phis).
//
// Pins:
//   * CondBr(true)  → Br(then-arm); branchesFolded == 1
//   * CondBr(false) → Br(else-arm); branchesFolded == 1
//   * CondBr(non-const) → unchanged; branchesFolded == 0
//   * Trampoline block with successor-has-no-Phis → elided;
//     blocksJumpThreaded == 1
//   * Trampoline block with successor-has-Phis → preserved
//     (conservative gate avoids phi-incoming fan-out)
//   * Entry-block trampoline → preserved (entry cannot be elided)
//   * Trampoline chain B1 → B2 → B3 → S → all elided; preds skip
//     directly to S (path-compressed jumpThreadMap)
//   * Multi-function: per-function counter reset
//   * Runtime-init carve-out parity

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_verifier.hpp"
#include "opt/optimizer.hpp"
#include "opt/passes/simplify_cfg.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace dss;

namespace {

std::size_t totalBlockCount(Mir const& mir) {
    std::size_t n = 0;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        n += mir.funcBlockCount(mir.funcAt(i));
    }
    return n;
}

} // namespace

// CondBr(Const(true), T, F) folds to Br(T).
TEST(SimplifyCfg, CondBrTrueFoldsToThenArm) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tArm  = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const fArm  = mb.createBlock(StructCfMarker::IfElse);
    mb.beginBlock(entry);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const c = mb.addConst(tru, boolT);
    mb.addCondBr(c, tArm, fArm);
    mb.beginBlock(tArm);
    MirLiteralValue v1; v1.value = std::int64_t{42}; v1.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v1, i32));
    mb.beginBlock(fArm);
    MirLiteralValue v2; v2.value = std::int64_t{99}; v2.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v2, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.branchesFolded, 1u);
    EXPECT_EQ(r.blocksJumpThreaded, 0u);
}

// CondBr(Const(false), T, F) folds to Br(F).
TEST(SimplifyCfg, CondBrFalseFoldsToElseArm) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tArm  = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const fArm  = mb.createBlock(StructCfMarker::IfElse);
    mb.beginBlock(entry);
    MirLiteralValue fals; fals.value = std::int64_t{0}; fals.core = TypeKind::Bool;
    MirInstId const c = mb.addConst(fals, boolT);
    mb.addCondBr(c, tArm, fArm);
    mb.beginBlock(tArm);
    MirLiteralValue v1; v1.value = std::int64_t{42}; v1.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v1, i32));
    mb.beginBlock(fArm);
    MirLiteralValue v2; v2.value = std::int64_t{99}; v2.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v2, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.branchesFolded, 1u);
}

// CondBr with non-Const condition → unchanged.
TEST(SimplifyCfg, CondBrNonConstantPreserved) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tArm  = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const fArm  = mb.createBlock(StructCfMarker::IfElse);
    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    mb.addCondBr(cond, tArm, fArm);
    mb.beginBlock(tArm);
    MirLiteralValue v1; v1.value = std::int64_t{42}; v1.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v1, i32));
    mb.beginBlock(fArm);
    MirLiteralValue v2; v2.value = std::int64_t{99}; v2.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v2, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.branchesFolded, 0u);
}

// Trampoline block (only `Br(S)` + S has no Phis) is elided + its
// predecessor's Br redirects to S.
TEST(SimplifyCfg, EmptyBlockJumpThreaded) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tramp = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const dst   = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry);
    mb.addBr(tramp);
    mb.beginBlock(tramp);
    mb.addBr(dst);  // empty block, just a Br
    mb.beginBlock(dst);
    MirLiteralValue v; v.value = std::int64_t{42}; v.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v, i32));
    Mir mir = std::move(mb).finish();

    auto const blocksBefore = totalBlockCount(mir);
    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.blocksJumpThreaded, 1u);
    EXPECT_LT(totalBlockCount(mir), blocksBefore)
        << "trampoline block must be physically removed by the rebuild";
}

// Trampoline block whose successor has a Phi: SimplifyCFG must NOT
// elide it (would require phi-incoming fan-out — out of scope for c2).
TEST(SimplifyCfg, TrampolineToSuccessorWithPhiNotThreaded) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
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
    MirLiteralValue one; one.value = std::int64_t{1}; one.core = TypeKind::I32;
    MirLiteralValue two; two.value = std::int64_t{2}; two.core = TypeKind::I32;
    MirInstId const c1 = mb.addConst(one, i32);
    MirInstId const c2 = mb.addConst(two, i32);
    mb.addCondBr(cond, tArm, fArm);
    mb.beginBlock(tArm);
    mb.addBr(join);  // trampoline-shape, but join has phis
    mb.beginBlock(fArm);
    mb.addBr(join);
    mb.beginBlock(join);
    MirPhiIncoming const incs[] = {{c1, tArm}, {c2, fArm}};
    MirInstId const phi = mb.addPhi(i32, incs);
    mb.addReturn(phi);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.blocksJumpThreaded, 0u)
        << "trampoline whose successor has Phis must not be elided "
           "(phi-incoming fan-out is out of scope for c2)";
}

// Entry block trampoline → preserved (cannot elide the function entry).
TEST(SimplifyCfg, EntryBlockTrampolineNotElided) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const dst   = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry);
    mb.addBr(dst);  // entry is a "trampoline" but cannot be elided
    mb.beginBlock(dst);
    MirLiteralValue v; v.value = std::int64_t{42}; v.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.blocksJumpThreaded, 0u)
        << "entry block must never be elided even if it's a trampoline";
}

// Trampoline chain B1 → B2 → S. Path-compression collapses both
// (B1, B2 map directly to S). Entry's Br ends up pointing at S.
TEST(SimplifyCfg, TrampolineChainPathCompressed) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const b1    = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const b2    = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const dst   = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry); mb.addBr(b1);
    mb.beginBlock(b1);    mb.addBr(b2);
    mb.beginBlock(b2);    mb.addBr(dst);
    mb.beginBlock(dst);
    MirLiteralValue v; v.value = std::int64_t{42}; v.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.blocksJumpThreaded, 2u)
        << "both b1 and b2 must be elided (chain length 2)";
    // After elision: entry + dst survive. 2 blocks total.
    EXPECT_EQ(totalBlockCount(mir), 2u);
}

// Multi-function: per-function reset.
TEST(SimplifyCfg, MultiFunctionEachFoldedIndependently) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    for (std::uint32_t fnIdx = 0; fnIdx < 2; ++fnIdx) {
        mb.addFunction(fnSig, SymbolId{100u + fnIdx});
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const tArm  = mb.createBlock(StructCfMarker::IfThen);
        MirBlockId const fArm  = mb.createBlock(StructCfMarker::IfElse);
        mb.beginBlock(entry);
        MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
        MirInstId const c = mb.addConst(tru, boolT);
        mb.addCondBr(c, tArm, fArm);
        mb.beginBlock(tArm);
        MirLiteralValue v1; v1.value = std::int64_t{1 + fnIdx}; v1.core = TypeKind::I32;
        mb.addReturn(mb.addConst(v1, i32));
        mb.beginBlock(fArm);
        MirLiteralValue v2; v2.value = std::int64_t{99}; v2.core = TypeKind::I32;
        mb.addReturn(mb.addConst(v2, i32));
    }
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.branchesFolded, 2u)
        << "each function's CondBr-on-const must fold — counter accumulates";
}

// D-OPT5-BLOCK-MERGE — linear chain (P, B) where both are Linear:
// P+B merge into one block; B's terminator becomes the merged block's
// terminator; B is dropped.
TEST(SimplifyCfg, LinearChainBlockMerged) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const mid   = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry);
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirLiteralValue v4; v4.value = std::int64_t{4}; v4.core = TypeKind::I32;
    MirInstId const a = mb.addConst(v3, i32);
    MirInstId const b = mb.addConst(v4, i32);
    // Entry has real content (Add(a,b)) AND terminator Br(mid).
    MirInstId const addOps[] = {a, b};
    (void)mb.addInst(MirOpcode::Add, addOps, i32);
    mb.addBr(mid);
    mb.beginBlock(mid);
    // Mid has real content (Const + Return).
    MirLiteralValue v7; v7.value = std::int64_t{7}; v7.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v7, i32));
    Mir mir = std::move(mb).finish();

    auto const blocksBefore = totalBlockCount(mir);
    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.blocksMerged, 1u)
        << "linear chain (entry,mid) merges into one block";
    EXPECT_LT(totalBlockCount(mir), blocksBefore);
}

// Preds-hoist decision-identity pin (the SimplifyCfg analog of CSE's
// CrossBlockLoadCseDecidedPerFunctionInMultiFunctionModule): `runSimplifyCfg`
// now computes the whole-module preds ONCE and threads it into every
// function's analyze — this pins that the MERGE decision (the
// `preds[B.v].size() == 1` single-predecessor gate) stays PER FUNCTION in a
// multi-function module: fn0 carries a mergeable linear (entry, mid) pair;
// fn1's mid has TWO predecessors (a diamond) → refused. Exactly one merge.
TEST(SimplifyCfg, BlockMergeDecidedPerFunctionInMultiFunctionModule) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;

    // fn0 — mergeable (entry, mid): mid single-pred with real content.
    mb.addFunction(fnSig, SymbolId{100});
    {
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const mid   = mb.createBlock(StructCfMarker::Linear);
        mb.beginBlock(entry);
        MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
        MirInstId const a = mb.addConst(v3, i32);
        MirInstId const addOps[] = {a, a};
        (void)mb.addInst(MirOpcode::Add, addOps, i32);
        mb.addBr(mid);
        mb.beginBlock(mid);
        MirLiteralValue v7; v7.value = std::int64_t{7}; v7.core = TypeKind::I32;
        MirInstId const c7 = mb.addConst(v7, i32);
        MirInstId const add2[] = {c7, c7};
        MirInstId const r2 = mb.addInst(MirOpcode::Add, add2, i32);
        mb.addReturn(r2);
    }

    // fn1 — a diamond whose join has TWO preds: never merged.
    mb.addFunction(fnSig, SymbolId{101});
    {
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const tArm  = mb.createBlock(StructCfMarker::IfThen);
        MirBlockId const fArm  = mb.createBlock(StructCfMarker::IfElse);
        MirBlockId const join  = mb.createBlock(StructCfMarker::IfJoin);
        mb.beginBlock(entry);
        MirInstId const cond = mb.addArg(0, boolT);
        mb.addCondBr(cond, tArm, fArm);
        mb.beginBlock(tArm);
        MirLiteralValue v1; v1.value = std::int64_t{1}; v1.core = TypeKind::I32;
        MirInstId const c1 = mb.addConst(v1, i32);
        MirInstId const add1[] = {c1, c1};
        (void)mb.addInst(MirOpcode::Add, add1, i32);
        mb.addBr(join);
        mb.beginBlock(fArm);
        MirLiteralValue v2; v2.value = std::int64_t{2}; v2.core = TypeKind::I32;
        MirInstId const c2 = mb.addConst(v2, i32);
        MirInstId const add2[] = {c2, c2};
        (void)mb.addInst(MirOpcode::Add, add2, i32);
        mb.addBr(join);
        mb.beginBlock(join);
        MirLiteralValue v9; v9.value = std::int64_t{9}; v9.core = TypeKind::I32;
        MirInstId const c9 = mb.addConst(v9, i32);
        MirInstId const add3[] = {c9, c9};
        MirInstId const r3 = mb.addInst(MirOpcode::Add, add3, i32);
        mb.addReturn(r3);
    }
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.blocksMerged, 1u)
        << "block merge decided per function: fn0's linear pair merges, fn1's "
           "two-pred join refuses — the pass-wide preds must not leak across "
           "functions";
}

// D-OPT4-1-NON-LINEAR-MARKER-MERGE — CLOSED. Non-Linear markers on
// BOTH sides of a (P, B) pair no longer block the merge: admission is
// pure CFG-legality, and the post-rebuild canonical re-derivation
// (`rederiveStructCfMarkers`) stamps the merged block's ACTUAL role.
// This is the closing pin — the predecessor test
// (`BothNonLinearMarkersNotMerged`) asserted blocksMerged == 0 for
// exactly this fixture.
// RED-on-disable: re-add the old marker condition to
// `isCandidateForMerge` ("both non-Linear → return false") → the
// merges below are refused → the blocksMerged expectation goes red.
TEST(SimplifyCfg, BothNonLinearMarkersNowMerge) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    // entry → tArm → join, both non-Linear stamps; tArm.terminator =
    // Br(join); join has 1 pred (tArm). Both pairs are CFG-legal, so
    // the whole straight-line chain collapses into one block.
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tArm  = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const join  = mb.createBlock(StructCfMarker::IfJoin);
    mb.beginBlock(entry); mb.addBr(tArm);
    mb.beginBlock(tArm);
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirInstId const a = mb.addConst(v3, i32);
    MirInstId const addOps[] = {a, a};
    (void)mb.addInst(MirOpcode::Add, addOps, i32);
    mb.addBr(join);
    mb.beginBlock(join);
    MirLiteralValue v7; v7.value = std::int64_t{7}; v7.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v7, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.blocksMerged, 2u)
        << "BOTH pairs - (entry, tArm) and (tArm, join) - now merge; "
           "markers are not an admission gate "
           "(D-OPT4-1-NON-LINEAR-MARKER-MERGE closed)";
    EXPECT_EQ(r.blocksJumpThreaded, 0u);
    // The single surviving block derives EntryBlock (it IS the entry),
    // and the module satisfies the verifier's stored==derived equality.
    MirFuncId const f = mir.funcAt(0);
    ASSERT_EQ(mir.funcBlockCount(f), 1u)
        << "the whole straight-line chain collapses into the entry";
    EXPECT_EQ(mir.blockMarker(mir.funcEntry(f)), StructCfMarker::EntryBlock);
    DiagnosticReporter vrep;
    MirVerifier v{mir, &interner};
    EXPECT_TRUE(v.verify(vrep))
        << (vrep.all().empty() ? "" : vrep.all()[0].actual);
}

// Derivation-era successor of `MergeSurvivorInheritsNonLinearMarker`
// (which pinned the deleted "non-Linear wins" absorb-chain walk over a
// CFG-INCONSISTENT fixture — LoopExit stamped on a loop-free CFG).
// Now: the merged block's marker is the CANONICAL DERIVATION of the
// post-merge CFG — the stale LoopExit creation stamp is corrected, not
// propagated. Shape: entry CondBr(tArm, mid); tArm returns; mid → exit
// merge into one returning block. Both arms return → ipdom(entry) =
// virtual → tArm derives IfThen, the MERGED block derives IfElse.
TEST(SimplifyCfg, MergeSurvivorTakesDerivedMarker) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    // entry → CondBr(cond, tArm, mid).  mid(Linear) → exit(LoopExit).
    // (mid, exit) is the merge candidate we're testing. The entry
    // can't merge with anyone (its terminator is CondBr, not Br).
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tArm  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const mid   = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const exitB = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    mb.addCondBr(cond, tArm, mid);
    mb.beginBlock(tArm);
    MirLiteralValue v9; v9.value = std::int64_t{9}; v9.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v9, i32));
    mb.beginBlock(mid);
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirInstId const a = mb.addConst(v3, i32);
    MirInstId const addOps[] = {a, a};
    (void)mb.addInst(MirOpcode::Add, addOps, i32);
    mb.addBr(exitB);
    mb.beginBlock(exitB);
    MirLiteralValue v7; v7.value = std::int64_t{7}; v7.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v7, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.blocksMerged, 1u)
        << "exactly one merge fires — (mid, exit). Entry's terminator "
           "is CondBr, so it can't merge with anyone.";
    // The merged block is identified by its instruction count: mid's
    // 2 non-terminator insts + exitB's 2 insts (incl. its Return) = 4;
    // the unmerged tArm has 2. Its marker must be the DERIVED role of
    // the post-merge CFG — IfElse (entry's false arm; both arms return
    // so there is no real join) — NOT the stale LoopExit creation
    // stamp the fixture planted (the derivation corrects it).
    std::size_t ifElseCount = 0;
    bool mergedBlockIsIfElse = false;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        std::uint32_t const nb = mir.funcBlockCount(f);
        ASSERT_EQ(nb, 3u) << "after one merge, 3 blocks survive";
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const blk = mir.funcBlockAt(f, bi);
            StructCfMarker const m = mir.blockMarker(blk);
            if (m == StructCfMarker::LoopExit) {
                ADD_FAILURE() << "no block may keep the stale LoopExit "
                                 "stamp - the post-merge re-derivation "
                                 "must correct it";
            }
            if (m == StructCfMarker::IfElse) {
                ++ifElseCount;
                if (mir.blockInstCount(blk) > 2) {
                    mergedBlockIsIfElse = true;
                }
            }
        }
    }
    EXPECT_EQ(ifElseCount, 1u)
        << "exactly one block derives IfElse post-merge";
    EXPECT_TRUE(mergedBlockIsIfElse)
        << "the derived IfElse lands on the MERGED block (instCount > 2 "
           "- contains both mid's and exitB's content) - D-OPT4-1";
    // And the module satisfies verifier equality by construction.
    DiagnosticReporter vrep;
    MirVerifier v{mir, &interner};
    EXPECT_TRUE(v.verify(vrep))
        << (vrep.all().empty() ? "" : vrep.all()[0].actual);
}

// Block-merge skipped: B has a Phi node (degenerate single-incoming
// phi, but defensive — block-merge defers to CopyProp's Phi-collapse).
TEST(SimplifyCfg, TargetWithPhiNotMerged) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const mid   = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry);
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirInstId const c = mb.addConst(v3, i32);
    mb.addBr(mid);
    mb.beginBlock(mid);
    MirPhiIncoming const incs[] = {{c, entry}};
    MirInstId const phi = mb.addPhi(i32, incs);
    mb.addReturn(phi);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.blocksMerged, 0u)
        << "B has a Phi — block-merge defers; CopyProp's Phi-collapse "
           "handles the degenerate single-incoming case";
}

// Block-merge skipped: B has multiple predecessors. The merge requires
// exactly 1 pred (= P) so the surviving block has no fan-in to handle.
TEST(SimplifyCfg, TargetWithMultiplePredsNotMerged) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tArm  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const fArm  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const join  = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    mb.addCondBr(cond, tArm, fArm);
    mb.beginBlock(tArm);
    MirLiteralValue v1; v1.value = std::int64_t{1}; v1.core = TypeKind::I32;
    MirInstId const c1 = mb.addConst(v1, i32);
    MirInstId const aops[] = {c1, c1};
    (void)mb.addInst(MirOpcode::Add, aops, i32);
    mb.addBr(join);
    mb.beginBlock(fArm);
    MirLiteralValue v2; v2.value = std::int64_t{2}; v2.core = TypeKind::I32;
    MirInstId const c2 = mb.addConst(v2, i32);
    MirInstId const aops2[] = {c2, c2};
    (void)mb.addInst(MirOpcode::Sub, aops2, i32);
    mb.addBr(join);
    mb.beginBlock(join);
    MirLiteralValue v7; v7.value = std::int64_t{7}; v7.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v7, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.blocksMerged, 0u)
        << "join has 2 preds (tArm, fArm) — merging into one would "
           "require Phi-incoming fan-out (deferred via single-pred gate)";
}

// Multi-step chain: P → B → C, all Linear, all with real content,
// each gate fires → all three merge into one block.
TEST(SimplifyCfg, MultiStepChainMerged) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const mid1  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const mid2  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const dst   = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry);
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirInstId const a = mb.addConst(v3, i32);
    MirInstId const aops[] = {a, a};
    (void)mb.addInst(MirOpcode::Add, aops, i32);
    mb.addBr(mid1);
    mb.beginBlock(mid1);
    MirLiteralValue v4; v4.value = std::int64_t{4}; v4.core = TypeKind::I32;
    MirInstId const b = mb.addConst(v4, i32);
    MirInstId const bops[] = {b, b};
    (void)mb.addInst(MirOpcode::Sub, bops, i32);
    mb.addBr(mid2);
    mb.beginBlock(mid2);
    MirLiteralValue v5; v5.value = std::int64_t{5}; v5.core = TypeKind::I32;
    MirInstId const c = mb.addConst(v5, i32);
    MirInstId const cops[] = {c, c};
    (void)mb.addInst(MirOpcode::Mul, cops, i32);
    mb.addBr(dst);
    mb.beginBlock(dst);
    MirLiteralValue v7; v7.value = std::int64_t{7}; v7.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v7, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.blocksMerged, 3u)
        << "all three merge pairs fire — entry+mid1, mid1+mid2, "
           "mid2+dst collapse into ONE block";
    EXPECT_EQ(totalBlockCount(mir), 1u);
}

// Phi-incoming redirection across merge: downstream block has a Phi
// with incoming-from-absorbed-B; that incoming must redirect to the
// absorb head P. This pins the phase-3 phi-incoming redirect via
// redirectBlockTarget.
TEST(SimplifyCfg, PhiIncomingRedirectedAcrossMerge) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    // entry → tHead(Linear) → tMerged(Linear) → join(Linear);
    //  entry → fArm(Linear) → join.
    // tHead+tMerged should merge (both Linear, single-pred chain).
    // join has Phi with incomings (vT, tMerged) + (vF, fArm).
    // Post-merge: phi's tMerged-incoming must redirect to tHead.
    MirBlockId const entry    = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tHead    = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const tMerged  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const fArm     = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const join     = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    MirLiteralValue v1; v1.value = std::int64_t{1}; v1.core = TypeKind::I32;
    MirLiteralValue v2; v2.value = std::int64_t{2}; v2.core = TypeKind::I32;
    MirInstId const vT = mb.addConst(v1, i32);
    MirInstId const vF = mb.addConst(v2, i32);
    mb.addCondBr(cond, tHead, fArm);
    mb.beginBlock(tHead);
    MirInstId const aops[] = {vT, vT};
    (void)mb.addInst(MirOpcode::Add, aops, i32);
    mb.addBr(tMerged);
    mb.beginBlock(tMerged);
    MirInstId const sops[] = {vT, vT};
    (void)mb.addInst(MirOpcode::Sub, sops, i32);
    mb.addBr(join);
    mb.beginBlock(fArm);
    MirInstId const mops[] = {vF, vF};
    (void)mb.addInst(MirOpcode::Mul, mops, i32);
    mb.addBr(join);
    mb.beginBlock(join);
    MirPhiIncoming const incs[] = {{vT, tMerged}, {vF, fArm}};
    MirInstId const phi = mb.addPhi(i32, incs);
    mb.addReturn(phi);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.blocksMerged, 1u)
        << "tHead+tMerged merge (both Linear, single-pred chain)";
    // Stronger pin per simplifier review #8: assert NOT just "phi
    // has 2 incomings" but that one incoming's pred IS the absorb
    // head's surviving block id. Track the function's blocks in
    // creation order: entry, tHead, fArm, join (tMerged absorbed
    // into tHead). The phi's incomings should be {(vT, tHead-new),
    // (vF, fArm-new)} — not {(vT, fArm), (vF, fArm)} which would
    // pass the count-only assertion but be semantically wrong.
    bool phiIncomingsCorrectlyRedirected = false;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        std::uint32_t const nb = mir.funcBlockCount(f);
        // The tHead-new block is the SECOND block created in source
        // order (after entry). tMerged was absorbed into tHead.
        // Walk the funcBlockAt sequence; surviving blocks are
        // entry, tHead-new, fArm-new, join-new (tMerged elided).
        ASSERT_EQ(nb, 4u) << "tMerged should be elided; 4 blocks survive";
        MirBlockId const newEntry  = mir.funcBlockAt(f, 0);
        MirBlockId const newTHead  = mir.funcBlockAt(f, 1);
        MirBlockId const newFArm   = mir.funcBlockAt(f, 2);
        MirBlockId const newJoin   = mir.funcBlockAt(f, 3);
        (void)newEntry;
        std::uint32_t const ni = mir.blockInstCount(newJoin);
        for (std::uint32_t i2 = 0; i2 < ni; ++i2) {
            MirInstId const id = mir.blockInstAt(newJoin, i2);
            if (mir.instOpcode(id) != MirOpcode::Phi) continue;
            auto const phiIncs = mir.phiIncomings(id);
            ASSERT_EQ(phiIncs.size(), 2u);
            // Pin: one incoming pred == newTHead (absorb head),
            // other == newFArm. Order may vary but the SET must match.
            std::vector<std::uint32_t> predIds;
            for (auto const& inc : phiIncs) predIds.push_back(inc.pred.v);
            std::sort(predIds.begin(), predIds.end());
            std::vector<std::uint32_t> expected = {newTHead.v, newFArm.v};
            std::sort(expected.begin(), expected.end());
            if (predIds == expected) phiIncomingsCorrectlyRedirected = true;
        }
    }
    EXPECT_TRUE(phiIncomingsCorrectlyRedirected)
        << "phi incoming preds must be exactly {newTHead, newFArm} — "
           "tMerged's absorbed-pred slot redirected to its absorb "
           "head tHead, NOT silently dropped or aliased to fArm";
}

// Derivation-era successor of `ChainNonLinearMarkerRejectsSecondAdmission`
// (which pinned the deleted "≤1 non-Linear per chain" admission
// rejection). Markers no longer gate admission AT ALL: a straight-line
// chain carrying MULTIPLE non-Linear stamps collapses entirely, and the
// canonical re-derivation stamps the single survivor's actual role.
// This doubles as the EFFECTIVENESS pin: the exact shapes the old gate
// refused now merge (D-OPT4-1-NON-LINEAR-MARKER-MERGE closed).
TEST(SimplifyCfg, WholeChainMergesRegardlessOfMarkerMultiplicity) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    // entry → ifThen(IfThen) → linkage(Linear) → loopExit(LoopExit).
    // (entry, ifThen): entry=EntryBlock non-Linear, ifThen=IfThen
    //                  non-Linear → pair gate REJECTS (both non-Linear).
    // Adjust: use entry → linkage1(Linear) → ifThen(IfThen) →
    //          linkage2(Linear) → loopExit(LoopExit) to expose the
    //          chain-multi-non-Linear case.
    MirBlockId const entry    = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const linkage1 = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const ifThen   = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const linkage2 = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const loopExit = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry); mb.addBr(linkage1);
    mb.beginBlock(linkage1);
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirInstId const a = mb.addConst(v3, i32);
    MirInstId const aops1[] = {a, a};
    (void)mb.addInst(MirOpcode::Add, aops1, i32);
    mb.addBr(ifThen);
    mb.beginBlock(ifThen);
    MirLiteralValue v4; v4.value = std::int64_t{4}; v4.core = TypeKind::I32;
    MirInstId const b = mb.addConst(v4, i32);
    MirInstId const aops2[] = {b, b};
    (void)mb.addInst(MirOpcode::Sub, aops2, i32);
    mb.addBr(linkage2);
    mb.beginBlock(linkage2);
    MirLiteralValue v5; v5.value = std::int64_t{5}; v5.core = TypeKind::I32;
    MirInstId const c = mb.addConst(v5, i32);
    MirInstId const aops3[] = {c, c};
    (void)mb.addInst(MirOpcode::Mul, aops3, i32);
    mb.addBr(loopExit);
    mb.beginBlock(loopExit);
    MirLiteralValue v7; v7.value = std::int64_t{7}; v7.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v7, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    // EVERY straight-line pair admits — (entry, linkage1), (linkage1,
    // ifThen), (ifThen, linkage2), (linkage2, loopExit) — regardless
    // of the IfThen + LoopExit stamps in the chain. The old gate
    // refused the 4th admission (blocksMerged was 2); pure
    // CFG-legality admits all 4.
    EXPECT_EQ(r.blocksMerged, 4u)
        << "the whole 5-block straight line collapses into the entry; "
           "markers are not an admission gate";
    MirFuncId const f = mir.funcAt(0);
    ASSERT_EQ(mir.funcBlockCount(f), 1u);
    // The single survivor derives EntryBlock — the stale IfThen /
    // LoopExit stamps are corrected by the canonical re-derivation,
    // not "picked" by a chain heuristic.
    EXPECT_EQ(mir.blockMarker(mir.funcEntry(f)), StructCfMarker::EntryBlock);
    DiagnosticReporter vrep;
    MirVerifier v{mir, &interner};
    EXPECT_TRUE(v.verify(vrep))
        << (vrep.all().empty() ? "" : vrep.all()[0].actual);
}

// Three-transform composition (test-analyzer rating 6): a SINGLE
// `runSimplifyCfg` invocation should fire branch-fold + jump-thread
// + block-merge if the CFG has each opportunity. Catches ordering
// bugs (e.g. branch-fold creating a new merge opportunity that the
// same analysis pass misses, or jump-thread invalidating a merge
// candidate's pred-set without reseeding the analyzer).
TEST(SimplifyCfg, ThreeTransformsComposeInOnePass) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    // Combined CFG with all 3 shapes:
    //   entry: CondBr(Const(true), tArm, fArm) → branch-fold fires.
    //   tArm(Linear): real content, Br(tramp).
    //   tramp(Linear): only Br(dst) → jump-thread fires (dst has no phis).
    //   fArm(Linear): dead post-fold; DCE handles next iter.
    //   dst(Linear): real content + Br(final).
    //   final(Linear): Return.
    //   (tArm, dst) is a block-merge candidate via the jump-threaded
    //   target — but actually let's structure it more cleanly:
    //   entry → CondBr(true, A, B) → A → tramp → C → final.
    //   A and C are merge candidates (both Linear, single-pred chain).
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const A     = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const B     = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const tramp = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const C     = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const final_ = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const condT = mb.addConst(tru, boolT);
    mb.addCondBr(condT, A, B);  // CondBr(const(true)) → branch-fold
    mb.beginBlock(A);
    MirLiteralValue v1; v1.value = std::int64_t{1}; v1.core = TypeKind::I32;
    MirInstId const c1 = mb.addConst(v1, i32);
    MirInstId const aops[] = {c1, c1};
    (void)mb.addInst(MirOpcode::Add, aops, i32);
    mb.addBr(tramp);
    mb.beginBlock(B);  // unreachable post-fold; ConstFold/DCE clears next iter
    MirLiteralValue v9; v9.value = std::int64_t{9}; v9.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v9, i32));
    mb.beginBlock(tramp); mb.addBr(C);  // trampoline → jump-thread fires
    mb.beginBlock(C);
    MirLiteralValue v2; v2.value = std::int64_t{2}; v2.core = TypeKind::I32;
    MirInstId const c2 = mb.addConst(v2, i32);
    MirInstId const sops[] = {c2, c2};
    (void)mb.addInst(MirOpcode::Sub, sops, i32);
    mb.addBr(final_);
    mb.beginBlock(final_);
    MirLiteralValue v7; v7.value = std::int64_t{7}; v7.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v7, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_GE(r.branchesFolded,    1u) << "CondBr(true) folds to Br(A)";
    EXPECT_GE(r.blocksJumpThreaded, 1u) << "tramp(Br-only) jump-threaded";
    EXPECT_GE(r.blocksMerged,      1u)
        << "after jump-thread + branch-fold, C+final (or A+...) merge";
}

// Engine end-to-end (test-analyzer rating 7): block-merge must
// produce a verifier-accepted MIR. Direct `runSimplifyCfg` tests
// the pass; this test rounds through `optimize()` with
// `verify-after-pass` active. A regression that produces malformed
// SSA (dangling phi-incoming, broken pred-edge set, orphan
// terminator) fails the verifier rather than passing this test.
TEST(SimplifyCfg, BlockMergeProducesVerifierAcceptedMir) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const mid   = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry);
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirInstId const a = mb.addConst(v3, i32);
    MirInstId const aops[] = {a, a};
    (void)mb.addInst(MirOpcode::Add, aops, i32);
    mb.addBr(mid);
    mb.beginBlock(mid);
    MirLiteralValue v7; v7.value = std::int64_t{7}; v7.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v7, i32));
    Mir mir = std::move(mb).finish();

    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    DiagnosticReporter rep;
    opt::OptPipeline const pipeline{
        "simplify-cfg-only", {opt::PassId::SimplifyCfg}, /*maxIterations*/1};
    auto const result =
        opt::optimize(mir, **targetR, interner, pipeline, rep);
    EXPECT_TRUE(result.ok)
        << "block-merge'd MIR must round through verify-after-pass "
           "cleanly — a malformed CFG would set ok=false";
    // No verifier diagnostic codes should appear.
    std::size_t verifierFailureCount = 0;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::I_VerifierFailure) {
            ++verifierFailureCount;
        }
    }
    EXPECT_EQ(verifierFailureCount, 0u);
}

// D-OPT4-1 marker stability: when NO transform fires, the pass's
// post-rebuild re-derivation must reproduce the same markers the
// (derivation-consistent) input carried — the fixture is a clean
// diamond whose stamps EQUAL their derivation (IfThen/IfElse/IfJoin
// around a Returning join; ipdom(entry) = join). CONTINGENCY NOTE:
// this stays green under the equality model precisely because the
// join RETURNS and ExitBlock is NOT a derived marker — if a future
// rule ever derived ExitBlock, this fixture's expectations would
// need revisiting.
TEST(SimplifyCfg, StructCfMarkersUnchangedWhenNoTransformsFire) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    // Build a CFG with diverse markers + no eligible transforms.
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tArm   = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const fArm   = mb.createBlock(StructCfMarker::IfElse);
    MirBlockId const join   = mb.createBlock(StructCfMarker::IfJoin);
    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);  // non-const → not foldable
    MirLiteralValue v1; v1.value = std::int64_t{1}; v1.core = TypeKind::I32;
    MirLiteralValue v2; v2.value = std::int64_t{2}; v2.core = TypeKind::I32;
    MirInstId const c1 = mb.addConst(v1, i32);
    MirInstId const c2 = mb.addConst(v2, i32);
    mb.addCondBr(cond, tArm, fArm);
    mb.beginBlock(tArm);
    // tArm has a real inst → not a trampoline.
    MirInstId const aops[] = {c1, c1};
    (void)mb.addInst(MirOpcode::Add, aops, i32);
    mb.addBr(join);
    mb.beginBlock(fArm);
    // fArm has a real inst → not a trampoline.
    MirInstId const sops[] = {c2, c2};
    (void)mb.addInst(MirOpcode::Sub, sops, i32);
    mb.addBr(join);
    mb.beginBlock(join);
    MirPhiIncoming const incs[] = {{c1, tArm}, {c2, fArm}};
    MirInstId const phi = mb.addPhi(i32, incs);
    mb.addReturn(phi);
    Mir mir = std::move(mb).finish();

    // Snapshot markers before the pass. SimplifyCFG's `selectBlocks`
    // returns RPO order — which reorders a diamond's tArm/fArm
    // relative to source-creation order — so we compare the MULTISET
    // of markers, not positional. The invariant is "every surviving
    // block KEEPS its marker"; block-position-in-arena is an arena
    // detail of the rebuild, not part of the marker contract.
    auto collectMarkersSorted = [&](Mir const& m) {
        std::vector<int> out;
        std::size_t const nf = m.moduleFuncCount();
        for (std::uint32_t i = 0; i < nf; ++i) {
            MirFuncId const f = m.funcAt(i);
            std::uint32_t const nb = m.funcBlockCount(f);
            for (std::uint32_t bi = 0; bi < nb; ++bi) {
                out.push_back(
                    static_cast<int>(m.blockMarker(m.funcBlockAt(f, bi))));
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    auto const before = collectMarkersSorted(mir);

    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    // Pre-condition for the marker pin: no transform should fire.
    ASSERT_EQ(r.branchesFolded, 0u);
    ASSERT_EQ(r.blocksJumpThreaded, 0u);

    auto const after = collectMarkersSorted(mir);
    ASSERT_EQ(before, after)
        << "SimplifyCFG with no transforms must preserve the marker "
           "MULTISET — every surviving block KEEPS its structural role "
           "(D-OPT4-1). Position-in-arena may differ due to RPO; the "
           "load-bearing invariant is per-block marker preservation, "
           "which a sorted-multiset compare pins.";
}

// Branch-fold PHI HYGIENE: folding `CondBr(b, T, F)` → `Br(taken)`
// removes the b → abandoned edge while b stays LIVE — a phi in the
// abandoned target naming b would go stale (I_PhiPredNotInCfg; the
// unreachable-pred cleanup never fires for a live pred). The fold must
// drop exactly that incoming. This shape was UNREACHABLE before the
// canonical marker derivation (a constant loop condition tripped the
// old LoopHeader-back-edge rejection first), so the latent bug had no
// witness until the degenerate-loop class started compiling.
// RED-on-disable: remove the `abandonedPhiEdges_` recording in
// analyze() → the stale incoming survives → the verifier emits
// I_PhiPredNotInCfg and the phi keeps 2 incomings → both pins red.
TEST(SimplifyCfg, BranchFoldDropsAbandonedTargetPhiIncoming) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    // entry CondBr(true, t, j); t: real inst + Br(j); j: phi{(v1,entry),
    // (v2,t)} + Return. The fold rewires entry → t; j stays reachable
    // via t but the entry → j edge is GONE.
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const t     = mb.createBlock();
    MirBlockId const j     = mb.createBlock();
    mb.beginBlock(entry);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    MirLiteralValue v1; v1.value = std::int64_t{3}; v1.core = TypeKind::I32;
    MirInstId const c1 = mb.addConst(v1, i32);
    mb.addCondBr(cond, t, j);
    mb.beginBlock(t);
    MirLiteralValue v2; v2.value = std::int64_t{4}; v2.core = TypeKind::I32;
    MirInstId const c2 = mb.addConst(v2, i32);
    MirInstId const aops[] = {c2, c2};
    (void)mb.addInst(MirOpcode::Add, aops, i32);
    mb.addBr(j);
    mb.beginBlock(j);
    MirPhiIncoming const incs[] = {{c1, entry}, {c2, t}};
    MirInstId const phi = mb.addPhi(i32, incs);
    mb.addReturn(phi);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    ASSERT_EQ(r.branchesFolded, 1u);
    // The phi keeps EXACTLY the surviving edge's incoming (pred = t).
    bool sawPhi = false;
    MirFuncId const f = mir.funcAt(0);
    for (std::uint32_t bi = 0; bi < mir.funcBlockCount(f); ++bi) {
        MirBlockId const b = mir.funcBlockAt(f, bi);
        for (std::uint32_t ii = 0; ii < mir.blockInstCount(b); ++ii) {
            MirInstId const id = mir.blockInstAt(b, ii);
            if (mir.instOpcode(id) != MirOpcode::Phi) continue;
            sawPhi = true;
            EXPECT_EQ(mir.phiIncomings(id).size(), 1u)
                << "the abandoned-edge incoming (pred = the folded "
                   "CondBr's block) must be dropped";
        }
    }
    EXPECT_TRUE(sawPhi);
    DiagnosticReporter vrep;
    MirVerifier v{mir, &interner};
    EXPECT_TRUE(v.verify(vrep))
        << (vrep.all().empty() ? "" : vrep.all()[0].actual);
}

// Derivation-era successor of `StructCfMarkersPreservedAfterTrampoline-
// Elision` (whose fixture stamped LoopExit on a LOOP-FREE straight line
// — CFG-inconsistent under the equality model). Markers are no longer
// "preserved" through elision; they are RE-DERIVED from the post-
// elision CFG. Fixture (CFG-consistent): a diamond whose else-arm is a
// trampoline — entry CondBr(tArm, tramp); tArm: Add + Br(join); tramp:
// Br(join); join: Return. After the trampoline elides, entry's false
// edge goes STRAIGHT to join; the destination derives its actual role
// (IfJoin) and the module satisfies verifier equality.
TEST(SimplifyCfg, TrampolineElisionOutputSatisfiesDerivedMarkers) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tArm  = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const tramp = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const join  = mb.createBlock(StructCfMarker::IfJoin);
    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    mb.addCondBr(cond, tArm, tramp);
    mb.beginBlock(tArm);
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirInstId const a = mb.addConst(v3, i32);
    MirInstId const addOps[] = {a, a};
    (void)mb.addInst(MirOpcode::Add, addOps, i32);
    mb.addBr(join);
    mb.beginBlock(tramp); mb.addBr(join);  // 1-inst trampoline → elided
    mb.beginBlock(join);
    MirLiteralValue v7; v7.value = std::int64_t{7}; v7.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v7, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    ASSERT_EQ(r.blocksJumpThreaded, 1u);

    // Post-pass: 3 blocks (tramp gone). The destination of the elided
    // trampoline derives IfJoin (it post-dominates the CondBr); tArm
    // derives IfThen; everything equals the canonical derivation.
    MirFuncId const f = mir.funcAt(0);
    ASSERT_EQ(mir.funcBlockCount(f), 3u);
    MirBlockId const newEntry = mir.funcEntry(f);
    auto const succs = mir.blockSuccessors(newEntry);
    ASSERT_EQ(succs.size(), 2u);
    EXPECT_EQ(mir.blockMarker(succs[0]), StructCfMarker::IfThen);
    EXPECT_EQ(mir.blockMarker(succs[1]), StructCfMarker::IfJoin)
        << "the elided trampoline's destination derives its actual "
           "role (the if's join) after elision";
    DiagnosticReporter vrep;
    MirVerifier v{mir, &interner};
    EXPECT_TRUE(v.verify(vrep))
        << "elision output must satisfy verifier marker equality: "
        << (vrep.all().empty() ? "" : vrep.all()[0].actual);
}

// Runtime-init carve-out parity.
TEST(SimplifyCfg, RuntimeInitGlobalsModuleEmitsXOptPassSkippedInfo) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const initSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    MirFuncId const initFn = mb.addFunction(initSig, SymbolId{50});
    MirBlockId const initEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(initEntry);
    mb.addReturn();
    mb.addGlobal(i32, SymbolId{200}, UINT32_MAX, initFn);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.branchesFolded, 0u);
    EXPECT_EQ(r.blocksJumpThreaded, 0u);
    std::size_t infoCount = 0;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::X_OptPassSkipped) ++infoCount;
    }
    EXPECT_EQ(infoCount, 1u);
}

// ── D15 cycle C (c): constant-CondBr fold PRUNES the dead arm in the
// SAME rebuild → verifier-clean, EXACT post-fold block count. ───────
// entry: CondBr(Const(true), then, else); then: return 42; else: return 99.
// The fold rewrites entry's terminator to Br(then) AND (C2 post-fold
// reachability) drops the now-unreachable `else` arm in the SAME rebuild.
// So the rebuilt function has EXACTLY 2 blocks (entry + then), the dead
// `else` is GONE, branchesFolded == 1, and the MirVerifier — whose per-
// pass I_UnreachableBlock check would otherwise fire on an emitted-but-
// orphan else — passes clean.
// RED-on-disable: dropping the `postFoldReachable_` filter in
// `selectBlocks` re-emits the dead `else` arm → it becomes an orphan
// island unreachable from entry → MirVerifier fires I_UnreachableBlock →
// verifier.verify(rep) returns false (demonstrated in the cycle gate),
// and the post-fold block count is 3 (the dead arm survives), failing the
// EXACT-count assertion.
TEST(SimplifyCfg, ConstantCondBrFoldPrunesDeadArmVerifierClean) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tArm  = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const fArm  = mb.createBlock(StructCfMarker::IfElse);
    mb.beginBlock(entry);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const c = mb.addConst(tru, boolT);
    mb.addCondBr(c, tArm, fArm);
    mb.beginBlock(tArm);
    MirLiteralValue v1; v1.value = std::int64_t{42}; v1.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v1, i32));
    mb.beginBlock(fArm);
    MirLiteralValue v2; v2.value = std::int64_t{99}; v2.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v2, i32));
    Mir mir = std::move(mb).finish();

    ASSERT_EQ(totalBlockCount(mir), 3u) << "before: entry + then + else";

    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.branchesFolded, 1u) << "the constant CondBr folds once";

    // THE post-fold count pin: the dead `else` arm is GONE (pruned in the
    // SAME rebuild). EXACTLY 2 blocks survive.
    EXPECT_EQ(totalBlockCount(mir), 2u)
        << "the dead else arm must be pruned in the fold's own rebuild "
           "(entry + surviving then-arm only)";

    // THE verifier pin: no orphan island → no I_UnreachableBlock → clean.
    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep))
        << "post-fold reachability leaves no unreachable block for the "
           "per-pass verifier to reject";
    EXPECT_EQ(rep.errorCount(), 0u) << "zero error-severity diagnostics";
}

// ── D-CSUBSET-COMPUTED-GOTO (MF-B): SimplifyCfg must NOT trampoline-remove an
// ADDRESS-TAKEN target block. Same shape as EmptyBlockJumpThreaded — an empty
// Br-only `tramp` block — but `tramp` is the target of a BlockAddress (and an
// IndirectBr can jump to it), so eliding it would leave the synthetic block
// symbol pointing at deleted/moved code. The guard reads isBlockAddressTaken.
// RED-ON-DISABLE: remove the `if (src_.isBlockAddressTaken(b)) continue;` guard
// in simplify_cfg.cpp and `tramp` is jump-threaded away (blockCount drops). ─────
TEST(SimplifyCfg, AddressTakenTrampolineNotElided) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const vptr  = interner.pointer(interner.primitive(TypeKind::Void));
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const disp  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const tramp = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const dst   = mb.createBlock(StructCfMarker::Linear);

    // entry: ba = BlockAddress(tramp); br disp   (ba makes `tramp` address-taken)
    mb.beginBlock(entry);
    MirInstId const ba = mb.addBlockAddress(tramp, vptr);
    mb.addBr(disp);
    // disp: indirectbr ba -> [tramp]   (the computed-goto edge into the trampoline)
    mb.beginBlock(disp);
    std::array<MirBlockId, 1> succs{tramp};
    mb.addIndirectBr(ba, succs);
    // tramp: EMPTY Br-only block (the trampoline shape) -> dst
    mb.beginBlock(tramp);
    mb.addBr(dst);
    // dst: return 42
    mb.beginBlock(dst);
    MirLiteralValue v; v.value = std::int64_t{42}; v.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    // ★ THE MF-B PIN: the address-taken trampoline is NOT jump-thread-ELIDED. (A
    // safe single-pred merge of a NON-address-taken successor INTO it may still
    // run — that preserves the trampoline's entry address — so we assert the
    // jump-thread count is 0 and the address-taken block still exists, not the raw
    // block count.) RED-ON-DISABLE: drop the isBlockAddressTaken guard in
    // simplify_cfg.cpp's trampoline loop and `tramp` IS jump-threaded
    // (blocksJumpThreaded == 1) → its synthetic symbol would dangle.
    EXPECT_EQ(r.blocksJumpThreaded, 0u)
        << "an address-taken (&&label-targeted) trampoline must NOT be jump-threaded";
    // An address-taken block (the BlockAddress target) survives the rebuild.
    bool anyAddressTaken = false;
    for (std::uint32_t fi = 0; fi < mir.moduleFuncCount(); ++fi) {
        MirFuncId const fn = mir.funcAt(fi);
        std::uint32_t const nb = mir.funcBlockCount(fn);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            if (mir.isBlockAddressTaken(mir.funcBlockAt(fn, bi))) anyAddressTaken = true;
        }
    }
    EXPECT_TRUE(anyAddressTaken)
        << "the &&label target block survives SimplifyCfg (MF-B guard held)";
}
