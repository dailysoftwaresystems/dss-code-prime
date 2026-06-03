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

#include <cstdint>

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
