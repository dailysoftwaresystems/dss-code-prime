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
