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
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
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

// Non-Linear marker on BOTH sides → block-merge skipped (conservative
// scope; full marker re-derivation is anchored as
// D-OPT4-1-NON-LINEAR-MARKER-MERGE for a future cycle).
TEST(SimplifyCfg, BothNonLinearMarkersNotMerged) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    // entry → tArm → join, both non-Linear; tArm.terminator = Br(join);
    // join has 1 pred (tArm). Would be a merge candidate EXCEPT both
    // markers are non-Linear → conservative gate refuses.
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
    // tArm IS trampoline-shaped? No — it has real content (Add).
    // Block-merge gate would fire EXCEPT both markers non-Linear.
    EXPECT_EQ(r.blocksMerged, 0u)
        << "IfThen + IfJoin both non-Linear — conservative c3 gate "
           "refuses (D-OPT4-1-NON-LINEAR-MARKER-MERGE).";
    EXPECT_EQ(r.blocksJumpThreaded, 0u);
}

// Marker re-derivation: P=Linear, B=LoopExit → merged block becomes
// LoopExit (the surviving non-Linear marker). Pin the survivor's
// marker post-merge. The fixture uses entry → CondBr split → only
// the false-arm reaches the merge chain; this prevents the entry's
// own merge from collapsing the chain into entry (which would
// inherit EntryBlock and shadow the marker-re-derivation test).
TEST(SimplifyCfg, MergeSurvivorInheritsNonLinearMarker) {
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
    bool foundLoopExitMarker = false;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const blk = mir.funcBlockAt(f, bi);
            if (mir.blockMarker(blk) == StructCfMarker::LoopExit) {
                foundLoopExitMarker = true;
            }
        }
    }
    EXPECT_TRUE(foundLoopExitMarker)
        << "after merge, LoopExit marker must survive (non-Linear "
           "marker wins over Linear head marker — D-OPT4-1)";
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

// Chain non-Linear marker invariant: a chain like H(Linear) →
// B1(IfThen) → B2(LoopExit) would violate the "≤1 non-Linear marker
// per chain" rule that c3's marker re-derivation depends on.
// analyze() rejects the second admission to keep the chain
// single-non-Linear. Without this guard, the marker re-derivation
// would silently pick one and drop the other.
TEST(SimplifyCfg, ChainNonLinearMarkerRejectsSecondAdmission) {
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
    // Pairs by-the-gate: (linkage1, ifThen) admits (head=linkage1,
    // chain non-Linear=IfThen). (ifThen, linkage2) — head is still
    // linkage1; chain has IfThen + linkage2 is Linear → admit.
    // (linkage2, loopExit) — head is linkage1; chain has IfThen
    // non-Linear AND loopExit is non-Linear → REJECT (would push to
    // 2 non-Linear). So 2 admissions, not 3.
    EXPECT_EQ(r.blocksMerged, 2u)
        << "the third admission (linkage2 → loopExit) is rejected "
           "to keep the chain at ≤1 non-Linear marker; loopExit "
           "survives as its own block";
    // Surviving non-Linear markers: EntryBlock (entry), IfThen
    // (linkage1's chain inherits via override), LoopExit (loopExit
    // standalone). The chain's surviving marker should be IfThen.
    std::vector<int> survivors;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            survivors.push_back(static_cast<int>(
                mir.blockMarker(mir.funcBlockAt(f, bi))));
        }
    }
    std::sort(survivors.begin(), survivors.end());
    std::vector<int> expected = {
        static_cast<int>(StructCfMarker::EntryBlock),
        static_cast<int>(StructCfMarker::IfThen),
        static_cast<int>(StructCfMarker::LoopExit),
    };
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(survivors, expected)
        << "after chain-non-Linear rejection, surviving markers are "
           "{EntryBlock, IfThen (from chain via override), LoopExit}";
}

// D-OPT4-1 marker preservation (RULE: c2's transforms — branch-fold +
// empty-block elision — NEVER mutate a surviving block's structural
// role). Build a multi-marker CFG with NO trampolines (so no blocks
// elided) + NO constant-condition CondBrs (so no folding). Snapshot
// markers pre-pass, run the pass, snapshot post-pass, byte-compare.
// A regression where the rebuilder accidentally defaulted markers to
// `Linear` (e.g. via a refactor of `createBlock`) would flip them
// silently — WASM/SPIR-V lowering depends on the markers to detect
// structured-CF regions without a Relooper recovery pass.
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

// Marker SURVIVAL under elision: when SimplifyCFG elides one or more
// trampoline blocks, the surviving blocks must retain their original
// markers. Build a CFG where exactly one trampoline gets elided;
// assert the post-pass marker SEQUENCE for surviving blocks matches
// the pre-pass marker sequence MINUS the elided block's slot.
TEST(SimplifyCfg, StructCfMarkersPreservedAfterTrampolineElision) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    // entry (EntryBlock) → tramp (Linear, will elide) → dst (LoopExit).
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tramp = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const dst   = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry); mb.addBr(tramp);
    mb.beginBlock(tramp); mb.addBr(dst);
    mb.beginBlock(dst);
    MirLiteralValue v; v.value = std::int64_t{42}; v.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runSimplifyCfg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    ASSERT_EQ(r.blocksJumpThreaded, 1u);

    // Post-pass: tramp is gone. The surviving sequence in RPO order
    // is [EntryBlock, LoopExit] — entry's marker preserved, dst's
    // marker preserved, the Linear trampoline elided. A regression
    // that re-marked dst as `Linear` (e.g. by collapsing markers
    // during block-elision) would change the visible sequence.
    std::vector<StructCfMarker> after;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            after.push_back(mir.blockMarker(mir.funcBlockAt(f, bi)));
        }
    }
    ASSERT_EQ(after.size(), 2u);
    EXPECT_EQ(static_cast<int>(after[0]),
              static_cast<int>(StructCfMarker::EntryBlock))
        << "entry's marker must survive trampoline elision";
    EXPECT_EQ(static_cast<int>(after[1]),
              static_cast<int>(StructCfMarker::LoopExit))
        << "destination's marker must survive trampoline elision "
           "unchanged (NOT re-marked to Linear)";
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
