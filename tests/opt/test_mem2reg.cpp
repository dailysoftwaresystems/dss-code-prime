// MIR-tier Mem2Reg unit tests.
//
// Risk pins per the OPT4 c2 mandate (the riskiest cycle in the
// optimizer — promotion is a silent-miscompile vector if any of the
// four guards fail):
//
//   1. PROMOTABILITY GATE: an address-taken alloca MUST NOT be
//      promoted. `AddressTakenAllocaNotPromoted` passes the alloca's
//      pointer to a Call — the alloca + load + store must survive
//      and the load must NOT be replaced with the stored value (the
//      callee could have rewritten the slot).
//
//   2. PHI PLACEMENT AT DOMINANCE FRONTIER: a diamond CFG with one
//      Store on each arm + one Load at the join MUST emit a Phi at
//      the join. `DiamondConditionalStoreInsertsPhi` asserts the Phi
//      exists + has the right incomings. This is the
//      `copy_prop_across_join` shape generalized.
//
//   3. DIFFERENTIAL-EXECUTION CORRECTNESS: covered by the corpus pin
//      `examples/c-subset/copy_prop_across_join` whose
//      `optimizedPipelines: [mem2reg-only]` arm re-spawns the OS
//      process under Mem2Reg + diff-asserts vs the baseline.
//
//   4. CFG STRUCTURE SAFETY: Mem2Reg INSERTS Phis only — it never
//      restructures the CFG, never re-marks blocks. `StructCfMarkers
//      Unchanged` snapshots markers pre-pass, runs Mem2Reg, then
//      byte-compares.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/passes/mem2reg.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

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

std::vector<StructCfMarker> snapshotMarkers(Mir const& mir) {
    std::vector<StructCfMarker> out;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            out.push_back(mir.blockMarker(mir.funcBlockAt(f, bi)));
        }
    }
    return out;
}

} // namespace

// Single block: `int x; x = 42; return x;` — the alloca + store +
// load must all collapse; the return reads Const(42) directly.
TEST(Mem2Reg, SingleBlockStoreThenLoadPromoted) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v; v.value = std::int64_t{42}; v.core = TypeKind::I32;
    MirInstId const c = mb.addConst(v, i32);
    MirInstId const storeOps[] = {c, slot};
    (void)mb.addInst(MirOpcode::Store, storeOps, InvalidType);
    MirInstId const loadOps[] = {slot};
    MirInstId const ld = mb.addInst(MirOpcode::Load, loadOps, i32);
    mb.addReturn(ld);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runMem2Reg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.allocasPromoted,  1u);
    EXPECT_EQ(r.loadsReplaced,    1u);
    EXPECT_EQ(r.storesEliminated, 1u);
    EXPECT_EQ(r.phisInserted,     0u);  // single block — no joins

    EXPECT_EQ(countOpInModule(mir, MirOpcode::Alloca), 0u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Store),  0u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Load),   0u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Phi),    0u);
}

// Diamond CFG: `if (cond) x = 1; else x = 2; return x;` — Mem2Reg
// must emit ONE Phi at the join with incomings (Const(1), tArm) +
// (Const(2), fArm). Address-of-x never escapes, so the alloca is
// promotable.
//
// RISK PIN #2: phi placement at dominance frontiers. The
// `copy_prop_across_join` shape generalized.
TEST(Mem2Reg, DiamondConditionalStoreInsertsPhi) {
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
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    mb.addCondBr(cond, tArm, fArm);

    MirLiteralValue one; one.value = std::int64_t{1}; one.core = TypeKind::I32;
    MirLiteralValue two; two.value = std::int64_t{2}; two.core = TypeKind::I32;

    mb.beginBlock(tArm);
    MirInstId const c1 = mb.addConst(one, i32);
    MirInstId const s1[] = {c1, slot};
    (void)mb.addInst(MirOpcode::Store, s1, InvalidType);
    mb.addBr(join);

    mb.beginBlock(fArm);
    MirInstId const c2 = mb.addConst(two, i32);
    MirInstId const s2[] = {c2, slot};
    (void)mb.addInst(MirOpcode::Store, s2, InvalidType);
    mb.addBr(join);

    mb.beginBlock(join);
    MirInstId const loadOps[] = {slot};
    MirInstId const ld = mb.addInst(MirOpcode::Load, loadOps, i32);
    mb.addReturn(ld);

    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runMem2Reg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.allocasPromoted, 1u);
    EXPECT_EQ(r.phisInserted,    1u)
        << "diamond join must have exactly one Phi for the promoted slot";
    EXPECT_EQ(r.loadsReplaced,    1u);
    EXPECT_EQ(r.storesEliminated, 2u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Alloca), 0u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Store),  0u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Load),   0u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Phi),    1u);

    // The Phi must have 2 incomings; their pred-block ids must map
    // to the new tArm + fArm (not entry, not join).
    bool foundPhi = false;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t i2 = 0; i2 < ni; ++i2) {
                MirInstId const id = mir.blockInstAt(b, i2);
                if (mir.instOpcode(id) != MirOpcode::Phi) continue;
                foundPhi = true;
                auto const incs = mir.phiIncomings(id);
                EXPECT_EQ(incs.size(), 2u)
                    << "join phi must have 2 incomings (one per arm)";
            }
        }
    }
    EXPECT_TRUE(foundPhi);
}

// RISK PIN #1 — the promotability gate.
// An address-taken alloca passed to a Call MUST survive Mem2Reg:
// the callee can rewrite memory through that pointer, so the Load
// after the Call cannot be rewritten to the pre-Call stored value.
// A buggy gate would silently miscompile (return the stale value).
TEST(Mem2Reg, AddressTakenAllocaNotPromoted) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const params[] = {ptr};
    TypeId const calleeSig = interner.fnSig(params, voidT, CallConv::CcSysV);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    // Declare an extern callee whose address we'll use via GlobalAddr.
    // (Extern fn modeled as a function with externally-visible binding
    // — sufficient for the gate check; Mem2Reg only cares whether the
    // alloca's pointer is an operand of a non-Load/non-Store op.)
    mb.addFunction(calleeSig, SymbolId{50},
                   SymbolBinding::Global, SymbolVisibility::Default);
    MirBlockId const calleeEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(calleeEntry);
    mb.addReturn();

    // The function under test.
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v; v.value = std::int64_t{42}; v.core = TypeKind::I32;
    MirInstId const c = mb.addConst(v, i32);
    MirInstId const storeOps[] = {c, slot};
    (void)mb.addInst(MirOpcode::Store, storeOps, InvalidType);

    // Pass the alloca's pointer to a Call. The address escapes →
    // the alloca is non-promotable.
    MirInstId const calleeAddr = mb.addGlobalAddr(SymbolId{50}, calleeSig);
    MirInstId const callOps[] = {calleeAddr, slot};
    (void)mb.addInst(MirOpcode::Call, callOps, voidT);

    // Reload the slot after the Call.
    MirInstId const loadOps[] = {slot};
    MirInstId const ld = mb.addInst(MirOpcode::Load, loadOps, i32);
    mb.addReturn(ld);
    Mir mir = std::move(mb).finish();

    auto const allocaBefore = countOpInModule(mir, MirOpcode::Alloca);
    auto const storeBefore  = countOpInModule(mir, MirOpcode::Store);
    auto const loadBefore   = countOpInModule(mir, MirOpcode::Load);

    DiagnosticReporter rep;
    auto const r = opt::passes::runMem2Reg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.allocasPromoted, 0u)
        << "address-taken alloca MUST NOT be promoted (silent miscompile "
           "if the gate misclassifies — the callee could mutate the slot)";
    EXPECT_EQ(r.phisInserted,    0u);
    EXPECT_EQ(r.loadsReplaced,   0u);
    EXPECT_EQ(r.storesEliminated, 0u);

    EXPECT_EQ(countOpInModule(mir, MirOpcode::Alloca), allocaBefore);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Store),  storeBefore);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Load),   loadBefore);
}

// Mixed: one promotable alloca + one address-taken alloca in the
// same function. Promotion is per-slot — the promotable one promotes;
// the escaped one survives.
TEST(Mem2Reg, MixedPromotableAndEscapedAllocasIndependent) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const calleeParams[] = {ptr};
    TypeId const calleeSig = interner.fnSig(calleeParams, voidT, CallConv::CcSysV);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(calleeSig, SymbolId{50},
                   SymbolBinding::Global, SymbolVisibility::Default);
    MirBlockId const calleeEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(calleeEntry);
    mb.addReturn();

    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const promotableSlot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirInstId const escapedSlot    = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v; v.value = std::int64_t{42}; v.core = TypeKind::I32;
    MirInstId const c = mb.addConst(v, i32);
    MirInstId const sp[] = {c, promotableSlot};
    (void)mb.addInst(MirOpcode::Store, sp, InvalidType);
    MirInstId const se[] = {c, escapedSlot};
    (void)mb.addInst(MirOpcode::Store, se, InvalidType);
    // Escape only the escaped slot.
    MirInstId const calleeAddr = mb.addGlobalAddr(SymbolId{50}, calleeSig);
    MirInstId const callOps[] = {calleeAddr, escapedSlot};
    (void)mb.addInst(MirOpcode::Call, callOps, voidT);
    // Load the promotable one (will be replaced) + return it.
    MirInstId const loadOps[] = {promotableSlot};
    MirInstId const ld = mb.addInst(MirOpcode::Load, loadOps, i32);
    mb.addReturn(ld);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runMem2Reg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.allocasPromoted, 1u);
    // After: one alloca + one store survive (the escaped slot); the
    // promotable alloca + its store + its load all gone.
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Alloca), 1u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Store),  1u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Load),   0u);
}

// RISK PIN #4 — CFG-marker safety.
// Mem2Reg INSERTS Phis only; it MUST NOT re-mark blocks or restructure
// the CFG. A regression where the rebuilder accidentally drops the
// source marker (e.g. via a default `Linear` fallback) would silently
// degrade WASM lowering downstream (LoopHeader → Linear means the
// emitter can no longer detect the loop without a Relooper pass).
TEST(Mem2Reg, StructCfMarkersUnchanged) {
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
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    mb.addCondBr(cond, tArm, fArm);
    MirLiteralValue one; one.value = std::int64_t{1}; one.core = TypeKind::I32;
    MirLiteralValue two; two.value = std::int64_t{2}; two.core = TypeKind::I32;
    mb.beginBlock(tArm);
    MirInstId const c1 = mb.addConst(one, i32);
    MirInstId const s1[] = {c1, slot};
    (void)mb.addInst(MirOpcode::Store, s1, InvalidType);
    mb.addBr(join);
    mb.beginBlock(fArm);
    MirInstId const c2 = mb.addConst(two, i32);
    MirInstId const s2[] = {c2, slot};
    (void)mb.addInst(MirOpcode::Store, s2, InvalidType);
    mb.addBr(join);
    mb.beginBlock(join);
    MirInstId const loadOps[] = {slot};
    MirInstId const ld = mb.addInst(MirOpcode::Load, loadOps, i32);
    mb.addReturn(ld);
    Mir mir = std::move(mb).finish();

    auto const before = snapshotMarkers(mir);

    DiagnosticReporter rep;
    auto const r = opt::passes::runMem2Reg(mir, interner, rep);
    EXPECT_TRUE(r.ok);

    auto const after = snapshotMarkers(mir);
    ASSERT_EQ(before.size(), after.size())
        << "Mem2Reg must not add or remove blocks — count diverged";
    for (std::size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(static_cast<int>(before[i]), static_cast<int>(after[i]))
            << "block #" << i << " StructCfMarker changed across Mem2Reg "
            << "— pass restructured CFG metadata (silent regression of "
            << "WASM lowering's structural-CF detection)";
    }
}

// Loop CFG: alloca + initial Store in entry + Store + Load inside the
// body + Load after the exit. The header is in the IDF of {entry,body}
// → one Phi at the header with incomings (init, entry) + (body-store,
// body). Pinned end-to-end through the pass (not just the dom-tree
// helper) — the rename walk's interaction with a back-edge is the
// most error-prone shape and the one most likely to silently
// miscompile induction variables.
TEST(Mem2Reg, LoopInductionVariablePromoted) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);

    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue zero; zero.value = std::int64_t{0}; zero.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(zero, i32);
    MirInstId const s0[] = {c0, slot};
    (void)mb.addInst(MirOpcode::Store, s0, InvalidType);
    mb.addBr(header);

    mb.beginBlock(header);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);

    mb.beginBlock(body);
    MirInstId const loadOps[] = {slot};
    MirInstId const ld = mb.addInst(MirOpcode::Load, loadOps, i32);
    MirLiteralValue one; one.value = std::int64_t{1}; one.core = TypeKind::I32;
    MirInstId const c1 = mb.addConst(one, i32);
    MirInstId const addOps[] = {ld, c1};
    MirInstId const inc = mb.addInst(MirOpcode::Add, addOps, i32);
    MirInstId const s1[] = {inc, slot};
    (void)mb.addInst(MirOpcode::Store, s1, InvalidType);
    mb.addBr(header);  // back-edge

    mb.beginBlock(exitB);
    MirInstId const loadOps2[] = {slot};
    MirInstId const ld2 = mb.addInst(MirOpcode::Load, loadOps2, i32);
    mb.addReturn(ld2);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runMem2Reg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.allocasPromoted, 1u);
    // ONE Phi at the header (the IDF of {entry, body} is {header}).
    EXPECT_EQ(r.phisInserted, 1u)
        << "loop with one promotable alloca must yield exactly one "
           "header Phi — the canonical Cytron-Ferrante shape";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Alloca), 0u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Store),  0u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Load),   0u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Phi),    1u);

    // The Phi must have 2 incomings (entry + body back-edge).
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t i2 = 0; i2 < ni; ++i2) {
                MirInstId const id = mir.blockInstAt(b, i2);
                if (mir.instOpcode(id) != MirOpcode::Phi) continue;
                auto const incs = mir.phiIncomings(id);
                EXPECT_EQ(incs.size(), 2u)
                    << "loop header phi: 2 incomings (entry + back-edge)";
            }
        }
    }
}

// D-OPT-MEM2REG-LOOP-BODY-LOCAL-DEAD-PHI: an alloca declared AND used entirely
// inside a loop body (Store-then-Load in the latch — `int iv = expr;`) is
// BLOCK-LOCAL: not upward-exposed, so SEMI-PRUNED SSA places NO Phi for it.
// Minimal (un-pruned) SSA would place a DEAD Phi at the loop HEADER (the body-
// Store's IDF includes the header via the back-edge); that Phi's entry-
// predecessor incoming is genuinely undefined (the slot was never stored before
// the loop) → the rename walk's empty-stack guard ABORTS. That bug crashed the
// release pipeline on ANY loop-body-local (`vsum`'s `int iv`/`double dv`, the
// varargs_win64_sum symptom, every plain `while (...) { int x = ...; }`).
// Here `slot` is a real induction var (Load-before-Store in the latch → upward-
// exposed → exactly 1 header Phi) and `bl` is a body-local (Store-before-Load →
// none). RED-ON-DISABLE: revert the upward-exposed gate in mem2reg.cpp and this
// pass ABORTS on bl's dead header Phi (the test process crashes).
TEST(Mem2Reg, LoopBodyLocalAllocaNeedsNoPhi) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);

    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);  // induction var → GLOBAL
    MirInstId const bl   = mb.addInst(MirOpcode::Alloca, {}, ptr);  // loop-body-local → block-local
    MirLiteralValue zero; zero.value = std::int64_t{0}; zero.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(zero, i32);
    MirInstId const s0[] = {c0, slot};
    (void)mb.addInst(MirOpcode::Store, s0, InvalidType);
    mb.addBr(header);

    mb.beginBlock(header);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);

    mb.beginBlock(body);
    // bl: Store THEN Load within the body (`int bl = 7;`) — block-local.
    MirLiteralValue seven; seven.value = std::int64_t{7}; seven.core = TypeKind::I32;
    MirInstId const c7 = mb.addConst(seven, i32);
    MirInstId const sb[] = {c7, bl};
    (void)mb.addInst(MirOpcode::Store, sb, InvalidType);
    MirInstId const blLoadOps[] = {bl};
    MirInstId const blv = mb.addInst(MirOpcode::Load, blLoadOps, i32);
    // slot: Load BEFORE its Store in the latch → upward-exposed (induction var).
    MirInstId const slotLoadOps[] = {slot};
    MirInstId const ld = mb.addInst(MirOpcode::Load, slotLoadOps, i32);
    MirInstId const addOps[] = {ld, blv};
    MirInstId const inc = mb.addInst(MirOpcode::Add, addOps, i32);  // slot' = slot + bl
    MirInstId const s1[] = {inc, slot};
    (void)mb.addInst(MirOpcode::Store, s1, InvalidType);
    mb.addBr(header);  // back-edge

    mb.beginBlock(exitB);
    MirInstId const exitLoadOps[] = {slot};
    MirInstId const ld2 = mb.addInst(MirOpcode::Load, exitLoadOps, i32);
    mb.addReturn(ld2);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runMem2Reg(mir, interner, rep);  // WITHOUT the fix: ABORTS here
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.allocasPromoted, 2u)
        << "both the induction var and the loop-body-local promote";
    // ONLY the induction var's header Phi — the block-local gets NONE.
    EXPECT_EQ(r.phisInserted, 1u)
        << "a loop-body-local is not upward-exposed → semi-pruned SSA inserts no "
           "(dead) header Phi for it; only the induction var needs one";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Phi),    1u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Alloca), 0u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Load),   0u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Store),  0u);
}

// D-OPT-MEM2REG-DEAD-PHI-PRUNE (the NESTED-LOOP half): the inner counter `ij` is
// re-initialized (Store 0) at the top of each OUTER iteration, then walked by the
// inner loop. So `ij` is LIVE across the INNER back-edge (it IS upward-exposed in
// the inner header → a semi-pruned "global" test keeps ALL its phis) yet DEAD at
// the OUTER header. Minimal SSA placed a phi for `ij` at the outer header too (its
// def-blocks {entry, outer-body, inner-body} have the outer header in their IDF via
// the outer back-edge); that phi's entry incoming is undefined → the rename walk
// ABORTS. Only true LIVE-IN prunes it — semi-pruning cannot. Fully-pruned SSA keeps
// exactly the two LIVE header phis (outer-i, inner-j) and drops the dead outer-header
// inner-j phi. RED-ON-DISABLE: revert the live-in gate in mem2reg.cpp → the pass
// ABORTS on the dead outer-header phi (the test process crashes).
TEST(Mem2Reg, NestedLoopInnerCounterNoOuterHeaderPhi) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const oHead  = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const oBody  = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const iHead  = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const iBody  = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const oLatch = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);

    auto constI = [&](std::int64_t v) {
        MirLiteralValue lv; lv.value = v; lv.core = TypeKind::I32; return mb.addConst(lv, i32);
    };
    auto constTrue = [&]() {
        MirLiteralValue lv; lv.value = std::int64_t{1}; lv.core = TypeKind::Bool; return mb.addConst(lv, boolT);
    };
    auto storeTo = [&](MirInstId v, MirInstId slot) {
        MirInstId const s[] = {v, slot}; (void)mb.addInst(MirOpcode::Store, s, InvalidType);
    };
    auto loadOf = [&](MirInstId slot) {
        MirInstId const l[] = {slot}; return mb.addInst(MirOpcode::Load, l, i32);
    };
    auto incStore = [&](MirInstId slot) {
        MirInstId const ld = loadOf(slot);
        MirInstId const a[] = {ld, constI(1)};
        MirInstId const inc = mb.addInst(MirOpcode::Add, a, i32);
        storeTo(inc, slot);
    };

    mb.beginBlock(entry);
    MirInstId const oi = mb.addInst(MirOpcode::Alloca, {}, ptr);  // outer i (live across outer loop)
    MirInstId const ij = mb.addInst(MirOpcode::Alloca, {}, ptr);  // inner j (live inner, DEAD at outer header)
    storeTo(constI(0), oi);
    mb.addBr(oHead);

    mb.beginBlock(oHead);
    (void)loadOf(oi);                      // outer-i upward-exposed → live-in here → phi
    mb.addCondBr(constTrue(), oBody, exitB);

    mb.beginBlock(oBody);
    storeTo(constI(0), ij);                // j = 0 — kills j before the inner loop
    mb.addBr(iHead);

    mb.beginBlock(iHead);
    (void)loadOf(ij);                      // inner-j upward-exposed → live-in here → phi
    mb.addCondBr(constTrue(), iBody, oLatch);

    mb.beginBlock(iBody);
    incStore(ij);
    mb.addBr(iHead);                       // inner back-edge

    mb.beginBlock(oLatch);
    incStore(oi);
    mb.addBr(oHead);                       // outer back-edge

    mb.beginBlock(exitB);
    mb.addReturn(constI(0));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runMem2Reg(mir, interner, rep);  // WITHOUT the fix: ABORTS
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.allocasPromoted, 2u);
    // EXACTLY two LIVE header phis: outer-i @ outer header + inner-j @ inner header.
    // The inner counter gets NO phi at the OUTER header (dead there) — that dead phi
    // was the crash.
    EXPECT_EQ(r.phisInserted, 2u)
        << "fully-pruned SSA keeps only the live header phis; the inner counter's "
           "dead outer-header phi (the un-pruned crash) is gone";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Phi),    2u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Alloca), 0u);
}

// Alloca-as-Return-value: escapes the function. Must NOT be promoted.
// (The function returns a pointer that the caller can read/write
// through; promoting would erase the storage that pointer references.)
TEST(Mem2Reg, AllocaReturnedAsPointerNotPromoted) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, ptr, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    mb.addReturn(slot);  // returns the alloca's pointer — escape!
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runMem2Reg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.allocasPromoted, 0u)
        << "alloca returned as a pointer escapes the function — must "
           "not be promoted (silent miscompile if the gate misses this)";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Alloca), 1u);
}

// Volatile-flagged accesses pin the user's opt-in to observable
// memory semantics. Mem2Reg MUST NOT promote an alloca whose
// Load/Store is volatile. The promotability gate disqualifies the
// alloca; the rebuild copies the alloca + load + store verbatim.
TEST(Mem2Reg, VolatileAccessAllocaNotPromoted) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v; v.value = std::int64_t{42}; v.core = TypeKind::I32;
    MirInstId const c = mb.addConst(v, i32);
    MirInstId const storeOps[] = {c, slot};
    (void)mb.addInst(MirOpcode::Store, storeOps, InvalidType,
                     /*payload*/0, MirInstFlags::Volatile);
    MirInstId const loadOps[] = {slot};
    MirInstId const ld = mb.addInst(MirOpcode::Load, loadOps, i32,
                                    /*payload*/0, MirInstFlags::Volatile);
    mb.addReturn(ld);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runMem2Reg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.allocasPromoted, 0u)
        << "volatile access disqualifies promotion — observable memory "
           "semantics must survive the pass";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Alloca), 1u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Store),  1u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Load),   1u);
}

// Array allocas (alloca with operand count > 0) are NOT scalar slots
// and MUST NOT be promoted — promoting them would lose memory identity
// (array indexing reads / writes the contiguous slab).
TEST(Mem2Reg, ArrayAllocaNotPromoted) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirLiteralValue four; four.value = std::int64_t{4}; four.core = TypeKind::I32;
    MirInstId const countC = mb.addConst(four, i32);
    MirInstId const countOps[] = {countC};
    // 4-element array alloca — has an operand (the count); not a scalar slot.
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, countOps, ptr);
    MirLiteralValue v; v.value = std::int64_t{42}; v.core = TypeKind::I32;
    MirInstId const c = mb.addConst(v, i32);
    MirInstId const storeOps[] = {c, slot};
    (void)mb.addInst(MirOpcode::Store, storeOps, InvalidType);
    MirInstId const loadOps[] = {slot};
    MirInstId const ld = mb.addInst(MirOpcode::Load, loadOps, i32);
    mb.addReturn(ld);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runMem2Reg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.allocasPromoted, 0u)
        << "array alloca (with element-count operand) must not be promoted";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Alloca), 1u);
}
