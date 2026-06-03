// MIR-tier Loop-Invariant Code Motion unit tests.
//
// Scope (c1): hoist provably-invariant pure insts whose operands
// are all defined OUTSIDE the loop body. Trap-eligible ops (SDiv /
// UDiv / SMod / UMod), Load (alias-unsafe), Volatile, Phi, and
// side-effecting opcodes are all NOT hoisted.
//
// Pins:
//   * Invariant Add hoisted from loop body to preheader
//     (instructionsHoisted == 1).
//   * Non-invariant inst (operand defined inside loop) NOT hoisted.
//   * SDiv (trap-eligible) NOT hoisted (anchored
//     D-OPT6-LICM-TRAP-SAFE-HOIST).
//   * Load NOT hoisted (alias-unsafe defer).
//   * Volatile-flagged inst NOT hoisted.
//   * Loop with multiple external predecessors NOT hoisted (preheader
//     insertion deferred to D-OPT6-LICM-PREHEADER-INSERTION).
//   * Function with no loops → no-op (instructionsHoisted == 0).
//   * Multi-function: per-function counter accumulation.
//   * Runtime-init carve-out parity.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/passes/licm.hpp"

#include <gtest/gtest.h>

#include <cstdint>

using namespace dss;

// Invariant Add(a, b) where a and b are entry-block Consts → hoisted.
TEST(Licm, InvariantAddHoistedFromLoopBody) {
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
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirLiteralValue v4; v4.value = std::int64_t{4}; v4.core = TypeKind::I32;
    MirInstId const a = mb.addConst(v3, i32);
    MirInstId const b = mb.addConst(v4, i32);
    mb.addBr(header);
    mb.beginBlock(header);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
    // The hoistable inst: Add(a, b). Both operands are in entry → invariant.
    MirInstId const ops[] = {a, b};
    (void)mb.addInst(MirOpcode::Add, ops, i32);
    mb.addBr(header);
    mb.beginBlock(exitB);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 1u);
}

// Non-invariant inst (depends on a loop-body Phi) → NOT hoisted.
TEST(Licm, NonInvariantNotHoisted) {
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
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    MirInstId const initI = mb.addConst(v0, i32);
    mb.addBr(header);
    mb.beginBlock(header);
    // Loop-induction Phi: i = phi(0 from entry, i+1 from body).
    MirInstId const phi = mb.addPhi(i32);
    MirLiteralValue v1; v1.value = std::int64_t{1}; v1.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(v1, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
    MirLiteralValue vone; vone.value = std::int64_t{1}; vone.core = TypeKind::I32;
    MirInstId const one = mb.addConst(vone, i32);
    // Add(phi, 1) — operand `phi` is the LOOP HEADER's phi (in the
    // loop body); NOT invariant.
    MirInstId const ops[] = {phi, one};
    MirInstId const next = mb.addInst(MirOpcode::Add, ops, i32);
    mb.addBr(header);
    // Wire up the phi's incomings.
    mb.addPhiIncoming(phi, {initI, entry});
    mb.addPhiIncoming(phi, {next, body});
    mb.beginBlock(exitB);
    mb.addReturn(phi);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 0u)
        << "Add(phi, 1) depends on the loop-body Phi → not invariant";
}

// SDiv (trap-eligible) NOT hoisted even if operands are invariant.
TEST(Licm, TrapEligibleSDivNotHoisted) {
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
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirLiteralValue v4; v4.value = std::int64_t{4}; v4.core = TypeKind::I32;
    MirInstId const a = mb.addConst(v3, i32);
    MirInstId const b = mb.addConst(v4, i32);
    mb.addBr(header);
    mb.beginBlock(header);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
    MirInstId const ops[] = {a, b};
    (void)mb.addInst(MirOpcode::SDiv, ops, i32);
    mb.addBr(header);
    mb.beginBlock(exitB);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 0u)
        << "SDiv could trap; hoisting it out of a 0-trip-count loop "
           "would change observable behavior (D-OPT6-LICM-TRAP-SAFE-HOIST)";
}

// Load NOT hoisted (alias-unsafe defer).
TEST(Licm, LoadNotHoisted) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v42; v42.value = std::int64_t{42}; v42.core = TypeKind::I32;
    MirInstId const c = mb.addConst(v42, i32);
    MirInstId const s[] = {c, slot};
    (void)mb.addInst(MirOpcode::Store, s, InvalidType);
    mb.addBr(header);
    mb.beginBlock(header);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
    MirInstId const lops[] = {slot};
    (void)mb.addInst(MirOpcode::Load, lops, i32);
    mb.addBr(header);
    mb.beginBlock(exitB);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 0u)
        << "Load is alias-unsafe (no alias analysis substrate) — not hoisted";
}

// Volatile-flagged inst NOT hoisted even if otherwise invariant.
TEST(Licm, VolatileBinaryOpNotHoisted) {
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
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirLiteralValue v4; v4.value = std::int64_t{4}; v4.core = TypeKind::I32;
    MirInstId const a = mb.addConst(v3, i32);
    MirInstId const b = mb.addConst(v4, i32);
    mb.addBr(header);
    mb.beginBlock(header);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
    MirInstId const ops[] = {a, b};
    (void)mb.addInst(MirOpcode::Add, ops, i32, /*payload*/0,
                     MirInstFlags::Volatile);
    mb.addBr(header);
    mb.beginBlock(exitB);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 0u);
}

// Test-analyzer Critical Gap 1: assert the hoisted inst actually
// LANDS IN THE PREHEADER. `instructionsHoisted == 1` is satisfied
// by the policy's counter regardless of which block the clone
// lands in. This walks the post-pass MIR to verify the Add ended
// up OUTSIDE the loop body (specifically in the preheader, which
// is the entry block in our fixture).
TEST(Licm, InvariantHoistLandsInPreheaderNotLoopBody) {
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
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirLiteralValue v4; v4.value = std::int64_t{4}; v4.core = TypeKind::I32;
    MirInstId const a = mb.addConst(v3, i32);
    MirInstId const b = mb.addConst(v4, i32);
    mb.addBr(header);
    mb.beginBlock(header);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, body, exitB);
    mb.beginBlock(body);
    MirInstId const ops[] = {a, b};
    (void)mb.addInst(MirOpcode::Add, ops, i32);
    mb.addBr(header);
    mb.beginBlock(exitB);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 1u);

    // Walk post-pass MIR. Find the Add. Its block's marker should
    // be EntryBlock (the preheader). The LoopLatch (body) should
    // contain NO Add.
    std::size_t addsInEntry = 0;
    std::size_t addsInBody  = 0;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const blk = mir.funcBlockAt(f, bi);
            StructCfMarker const m = mir.blockMarker(blk);
            std::uint32_t const ni = mir.blockInstCount(blk);
            for (std::uint32_t i2 = 0; i2 < ni; ++i2) {
                MirInstId const id = mir.blockInstAt(blk, i2);
                if (mir.instOpcode(id) != MirOpcode::Add) continue;
                if (m == StructCfMarker::EntryBlock) ++addsInEntry;
                if (m == StructCfMarker::LoopLatch)  ++addsInBody;
            }
        }
    }
    EXPECT_EQ(addsInEntry, 1u)
        << "the hoisted Add must land in the preheader (entry block)";
    EXPECT_EQ(addsInBody, 0u)
        << "the loop body must NO LONGER contain the Add — it was hoisted";
}

// Test-analyzer Important Gap 5: a loop with TWO non-back-edge
// predecessors (ambiguous preheader) is conservatively SKIPPED.
// The pass's `preheader.valid()` + `!ambiguous` gate refuses to
// hoist; the obvious invariant stays in the loop body.
TEST(Licm, MultiplePreheaderPredsSkipsLoop) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    // entry → {p1, p2} → header → body → header back-edge; header → exit.
    // header has TWO external preds (p1, p2) + one back-edge pred (body).
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const p1     = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const p2     = mb.createBlock(StructCfMarker::IfElse);
    MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
    MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
    MirLiteralValue v4; v4.value = std::int64_t{4}; v4.core = TypeKind::I32;
    MirInstId const a = mb.addConst(v3, i32);
    MirInstId const b = mb.addConst(v4, i32);
    mb.addCondBr(cond, p1, p2);
    mb.beginBlock(p1); mb.addBr(header);
    mb.beginBlock(p2); mb.addBr(header);
    mb.beginBlock(header);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const c = mb.addConst(tru, boolT);
    mb.addCondBr(c, body, exitB);
    mb.beginBlock(body);
    // Obvious invariant: Add(a, b) — but loop has 2 external preds,
    // so c1's preheader-singleton gate skips this loop.
    MirInstId const ops[] = {a, b};
    (void)mb.addInst(MirOpcode::Add, ops, i32);
    mb.addBr(header);
    mb.beginBlock(exitB);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 0u)
        << "loop with >1 external pred has no unique preheader → "
           "c1 conservatively skips (D-OPT6-LICM-PREHEADER-INSERTION)";
}

// Multi-function: counter accumulates across functions. Each
// function has an independently-hoistable invariant.
TEST(Licm, MultiFunctionCounterAccumulates) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    for (std::uint32_t fnIdx = 0; fnIdx < 2; ++fnIdx) {
        mb.addFunction(fnSig, SymbolId{100u + fnIdx});
        MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const header = mb.createBlock(StructCfMarker::LoopHeader);
        MirBlockId const body   = mb.createBlock(StructCfMarker::LoopLatch);
        MirBlockId const exitB  = mb.createBlock(StructCfMarker::LoopExit);
        mb.beginBlock(entry);
        MirLiteralValue v3; v3.value = std::int64_t{3}; v3.core = TypeKind::I32;
        MirLiteralValue v4; v4.value = std::int64_t{4}; v4.core = TypeKind::I32;
        MirInstId const a = mb.addConst(v3, i32);
        MirInstId const b = mb.addConst(v4, i32);
        mb.addBr(header);
        mb.beginBlock(header);
        MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
        MirInstId const cond = mb.addConst(tru, boolT);
        mb.addCondBr(cond, body, exitB);
        mb.beginBlock(body);
        MirInstId const ops[] = {a, b};
        (void)mb.addInst(MirOpcode::Add, ops, i32);
        mb.addBr(header);
        mb.beginBlock(exitB);
        MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
        mb.addReturn(mb.addConst(v0, i32));
    }
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 2u)
        << "each function's invariant Add hoists independently — "
           "counter accumulates across functions";
}

// Function with no loops: pass is a no-op.
TEST(Licm, NoLoopsNoOp) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirLiteralValue v; v.value = std::int64_t{42}; v.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 0u);
}

// Runtime-init carve-out parity.
TEST(Licm, RuntimeInitGlobalsModuleEmitsXOptPassSkippedInfo) {
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
    auto const r = opt::passes::runLicm(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsHoisted, 0u);
    std::size_t infoCount = 0;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::X_OptPassSkipped) ++infoCount;
    }
    EXPECT_EQ(infoCount, 1u);
}
