// MIR-tier SSA copy-prop (Phi-collapse) unit tests.
//
// Pins:
//   * Diamond with same value on both arms: Phi(V, V) collapses to V;
//     uses of the Phi redirect to V; dead Phi survives for DCE sweep.
//   * Diamond with DIFFERENT values: Phi(V1, V2) does NOT collapse;
//     module is unchanged (idempotency on non-collapsible Phis).
//   * Transitive chain: Phi1(Const) → Phi2(Phi1) → use(Phi2) — both
//     collapse; path-compression resolves Phi2's substitute directly
//     to Const, not via the intermediate Phi1 (single map lookup).
//   * Loop-header Phi with self-reference + one Const incoming
//     collapses to the Const (self-reference is an SSA artifact of
//     the back-edge, not a distinct value).
//   * Loop-header Phi with self-reference + two DIFFERENT non-self
//     incomings does NOT collapse.
//   * Phi-incoming substitution: if Phi P2's incoming is collapsed
//     Phi P1, P2's incoming should resolve through P1's target at
//     phase-3 fill time.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/passes/copy_prop.hpp"

#include <gtest/gtest.h>

#include <cstdint>

using namespace dss;

namespace {

std::size_t countOpInModule(Mir const& mir, MirOpcode want) {
    std::size_t n = 0;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t i2 = 0; i2 < ni; ++i2) {
                if (mir.instOpcode(mir.blockInstAt(b, i2)) == want) ++n;
            }
        }
    }
    return n;
}

} // namespace

// Diamond with the SAME Const on both arms: Phi(C, C) → C.
TEST(CopyProp, DiamondSameValueCollapsesPhi) {
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
    MirLiteralValue v; v.value = std::int64_t{42}; v.core = TypeKind::I32;
    MirInstId const c42 = mb.addConst(v, i32);
    mb.addCondBr(cond, tArm, fArm);

    mb.beginBlock(tArm); mb.addBr(join);
    mb.beginBlock(fArm); mb.addBr(join);

    mb.beginBlock(join);
    MirPhiIncoming const incs[] = {{c42, tArm}, {c42, fArm}};
    MirInstId const phi = mb.addPhi(i32, incs);
    mb.addReturn(phi);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runCopyProp(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.phisCollapsed, 1u);

    // The dead Phi survives (DCE sweeps in the next stage); count
    // stays at 1, but the Return's operand is now Const(42), not
    // the Phi result.
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Phi), 1u);

    // Walk to the Return; assert its operand is a Const, not a Phi.
    std::size_t const nf = mir.moduleFuncCount();
    bool returnPointsAtConst = false;
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            MirInstId const term = mir.blockTerminator(b);
            if (mir.instOpcode(term) != MirOpcode::Return) continue;
            auto const ops = mir.instOperands(term);
            if (ops.empty()) continue;
            if (mir.instOpcode(ops[0]) == MirOpcode::Const) {
                returnPointsAtConst = true;
            }
        }
    }
    EXPECT_TRUE(returnPointsAtConst)
        << "Return's operand must be the collapsed Const, not the dead Phi";
}

// Diamond with DIFFERENT incoming values: Phi(C1, C2) does NOT collapse.
TEST(CopyProp, DiamondDifferentValuesPhiPreserved) {
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
    mb.beginBlock(tArm); mb.addBr(join);
    mb.beginBlock(fArm); mb.addBr(join);
    mb.beginBlock(join);
    MirPhiIncoming const incs[] = {{c1, tArm}, {c2, fArm}};
    MirInstId const phi = mb.addPhi(i32, incs);
    mb.addReturn(phi);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runCopyProp(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.phisCollapsed, 0u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Phi), 1u);
}

// Transitive chain Phi1(C) → Phi2(Phi1) → use(Phi2). Both collapse;
// path compression maps Phi2 → C directly (not via Phi1).
TEST(CopyProp, TransitiveChainCollapses) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    // entry -> {tA, fA} -> join1 -> {tB, fB} -> join2 -> return
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tA = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const fA = mb.createBlock(StructCfMarker::IfElse);
    MirBlockId const join1 = mb.createBlock(StructCfMarker::IfJoin);
    MirBlockId const tB = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const fB = mb.createBlock(StructCfMarker::IfElse);
    MirBlockId const join2 = mb.createBlock(StructCfMarker::IfJoin);

    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    MirLiteralValue v; v.value = std::int64_t{77}; v.core = TypeKind::I32;
    MirInstId const c77 = mb.addConst(v, i32);
    mb.addCondBr(cond, tA, fA);
    mb.beginBlock(tA); mb.addBr(join1);
    mb.beginBlock(fA); mb.addBr(join1);
    mb.beginBlock(join1);
    MirPhiIncoming const incs1[] = {{c77, tA}, {c77, fA}};
    MirInstId const phi1 = mb.addPhi(i32, incs1);
    mb.addCondBr(cond, tB, fB);
    mb.beginBlock(tB); mb.addBr(join2);
    mb.beginBlock(fB); mb.addBr(join2);
    mb.beginBlock(join2);
    MirPhiIncoming const incs2[] = {{phi1, tB}, {phi1, fB}};
    MirInstId const phi2 = mb.addPhi(i32, incs2);
    mb.addReturn(phi2);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runCopyProp(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.phisCollapsed, 2u)
        << "both Phis in the transitive chain must collapse";

    // The Return's operand must now point directly at Const(77),
    // not at Phi1 (which would be a one-hop, not path-compressed).
    std::size_t const nf = mir.moduleFuncCount();
    bool returnPointsAtConst = false;
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            MirInstId const term = mir.blockTerminator(b);
            if (mir.instOpcode(term) != MirOpcode::Return) continue;
            auto const ops = mir.instOperands(term);
            if (!ops.empty() && mir.instOpcode(ops[0]) == MirOpcode::Const) {
                returnPointsAtConst = true;
            }
        }
    }
    EXPECT_TRUE(returnPointsAtConst)
        << "transitive collapse must path-compress Phi2 → Const directly";
}

// Loop-header Phi with self-reference + one Const incoming. The
// SSA back-edge contributes a self-reference; it should be filtered,
// leaving one distinct non-self incoming → Phi collapses to that
// value.
TEST(CopyProp, LoopHeaderPhiWithSelfReferenceCollapses) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB = mb.createBlock(StructCfMarker::LoopExit);

    mb.beginBlock(entry);
    MirLiteralValue v; v.value = std::int64_t{55}; v.core = TypeKind::I32;
    MirInstId const c55 = mb.addConst(v, i32);
    mb.addBr(header);
    mb.beginBlock(header);
    // Loop-header Phi with two incomings:
    //   (c55, entry)  — the initial value
    //   (self, body)  — the back-edge self-reference (trivial loop)
    MirInstId const phi = mb.addPhi(i32);
    mb.addPhiIncoming(phi, {c55, entry});
    mb.addPhiIncoming(phi, {phi, body});
    MirLiteralValue tru; tru.value = std::int64_t{0}; tru.core = TypeKind::Bool;
    MirInstId const falseConst = mb.addConst(tru, boolT);
    mb.addCondBr(falseConst, body, exitB);
    mb.beginBlock(body); mb.addBr(header);
    mb.beginBlock(exitB); mb.addReturn(phi);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runCopyProp(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.phisCollapsed, 1u)
        << "self-reference must be filtered; one distinct non-self "
           "incoming → Phi collapses";
}

// Loop-header Phi with TWO distinct incomings (entry-init + body-
// back-edge value) does NOT collapse — it carries real merge
// information. The negative pin for over-eager filtering: a buggy
// "filter every back-edge incoming" rule would wrongly classify
// this as collapsible.
TEST(CopyProp, LoopHeaderPhiTwoDistinctIncomingsNotCollapsed) {
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
    MirLiteralValue v1; v1.value = std::int64_t{1}; v1.core = TypeKind::I32;
    MirLiteralValue v2; v2.value = std::int64_t{2}; v2.core = TypeKind::I32;
    MirInstId const c1 = mb.addConst(v1, i32);
    MirInstId const c2 = mb.addConst(v2, i32);
    mb.addBr(header);
    mb.beginBlock(header);
    MirInstId const phi = mb.addPhi(i32);
    mb.addPhiIncoming(phi, {c1, entry});
    mb.addPhiIncoming(phi, {c2, body});
    MirLiteralValue fals; fals.value = std::int64_t{0}; fals.core = TypeKind::Bool;
    MirInstId const falseConst = mb.addConst(fals, boolT);
    mb.addCondBr(falseConst, body, exitB);
    mb.beginBlock(body); mb.addBr(header);
    mb.beginBlock(exitB); mb.addReturn(phi);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runCopyProp(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.phisCollapsed, 0u)
        << "Phi with two distinct non-self incomings carries real "
           "merge info — must not collapse";
}

// Multi-function module: per-function collapse-map reset. Without
// `resetPerFunction`, function B's analysis would inherit function
// A's collapse map keys; if the arena IDs happened to alias (they
// shouldn't with the cross-arena tag, but the discipline is
// load-bearing) the second function's substitution would silently
// miscompile.
TEST(CopyProp, MultiFunctionModuleEachCollapsedIndependently) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    // Build TWO functions, each with one collapsible diamond Phi.
    for (std::uint32_t fnIdx = 0; fnIdx < 2; ++fnIdx) {
        mb.addFunction(fnSig, SymbolId{100u + fnIdx});
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const tArm  = mb.createBlock(StructCfMarker::IfThen);
        MirBlockId const fArm  = mb.createBlock(StructCfMarker::IfElse);
        MirBlockId const join  = mb.createBlock(StructCfMarker::IfJoin);
        mb.beginBlock(entry);
        MirInstId const cond = mb.addArg(0, boolT);
        MirLiteralValue v; v.value = std::int64_t{7 + fnIdx}; v.core = TypeKind::I32;
        MirInstId const c = mb.addConst(v, i32);
        mb.addCondBr(cond, tArm, fArm);
        mb.beginBlock(tArm); mb.addBr(join);
        mb.beginBlock(fArm); mb.addBr(join);
        mb.beginBlock(join);
        MirPhiIncoming const incs[] = {{c, tArm}, {c, fArm}};
        MirInstId const phi = mb.addPhi(i32, incs);
        mb.addReturn(phi);
    }
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runCopyProp(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.phisCollapsed, 2u)
        << "each function's Phi must collapse independently — counter "
           "accumulates across functions";
}

// Runtime-init globals carve-out: a module with `globalInitFunc.valid()`
// must emit X_OptPassSkipped Info + return ok=true with zero counters.
// Parity test with ConstFold/Dce/Mem2Reg's parallel carve-out.
TEST(CopyProp, RuntimeInitGlobalsModuleEmitsXOptPassSkippedInfo) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const initSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    // The init function (declared first so its MirFuncId is valid).
    MirFuncId const initFn = mb.addFunction(initSig, SymbolId{50});
    MirBlockId const initEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(initEntry);
    mb.addReturn();
    // The runtime-init global referencing initFn.
    mb.addGlobal(i32, SymbolId{200}, /*initLiteralIndex*/UINT32_MAX, initFn);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runCopyProp(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.phisCollapsed, 0u);

    std::size_t infoCount = 0;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::X_OptPassSkipped) ++infoCount;
    }
    EXPECT_EQ(infoCount, 1u)
        << "CopyProp must emit X_OptPassSkipped on a runtime-init "
           "globals module (parity with ConstFold/Dce/Mem2Reg)";
}

// Single-incoming Phi (e.g. an artifact of an unbalanced join)
// collapses trivially to its lone incoming.
TEST(CopyProp, SingleIncomingPhiCollapses) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const succ  = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry);
    MirLiteralValue v; v.value = std::int64_t{99}; v.core = TypeKind::I32;
    MirInstId const c99 = mb.addConst(v, i32);
    mb.addBr(succ);
    mb.beginBlock(succ);
    MirPhiIncoming const incs[] = {{c99, entry}};
    MirInstId const phi = mb.addPhi(i32, incs);
    mb.addReturn(phi);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runCopyProp(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.phisCollapsed, 1u);
}
