// MIR-tier Dead Code Elimination unit tests.
//
// Pins:
//   * Side-effect-free instructions with unused result get elided.
//   * Side-effecting instructions (Store / Call) survive even with
//     no live consumer.
//   * Volatile-flagged loads survive regardless of result usage.
//   * Unreachable blocks (no CFG path from entry) get elided.
//   * The dce_negative_pin shape — an unconditional `Store(100, &x)`
//     followed by a conditional `Store(7, &x)` inside an `if` — both
//     side-effecting + both in CFG-reachable blocks — both survive.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/passes/dce.hpp"

#include <gtest/gtest.h>

#include <cstdint>

using namespace dss;

namespace {

// Builds a single-function MIR with one entry block, returning the
// constant `c` after performing an unused `Add(c, 1)` computation.
// The Add should be DCE-elided.
struct TrivialModule {
    Mir    mir;
    TypeId i32;
    TypeId fnSig;
};

TrivialModule buildAddUnused(TypeInterner& interner, std::int64_t c) {
    TrivialModule m;
    m.i32   = interner.primitive(TypeKind::I32);
    m.fnSig = interner.fnSig({}, m.i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(m.fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirLiteralValue v; v.value = c;  v.core = TypeKind::I32;
    MirLiteralValue one; one.value = std::int64_t{1}; one.core = TypeKind::I32;
    MirInstId const ca = mb.addConst(v, m.i32);
    MirInstId const cb = mb.addConst(one, m.i32);
    MirInstId const ops[] = {ca, cb};
    // The Add is dead — its result feeds no live consumer.
    (void)mb.addInst(MirOpcode::Add, ops, m.i32);
    mb.addReturn(ca);
    m.mir = std::move(mb).finish();
    return m;
}

// Counts instructions across all functions of the module — sum of
// every reachable block's instCount.
std::size_t totalInstCount(Mir const& mir) {
    std::size_t total = 0;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            total += mir.blockInstCount(mir.funcBlockAt(f, bi));
        }
    }
    return total;
}

} // namespace

// Foldable: an Add(Const, Const) whose result is unused MUST be
// elided. The function has 4 source instructions (2 Const + 1 Add +
// 1 Return); post-DCE, only 3 survive (the unused Add gone).
TEST(Dce, ElidesUnusedAddInstruction) {
    TypeInterner interner{CompilationUnitId{1}};
    auto m = buildAddUnused(interner, 42);

    auto const beforeCount = totalInstCount(m.mir);

    DiagnosticReporter rep;
    auto const r = opt::passes::runDce(m.mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_GE(r.instructionsEliminated, 1u);

    auto const afterCount = totalInstCount(m.mir);
    EXPECT_LT(afterCount, beforeCount)
        << "DCE must remove the unused Add; "
        << "before=" << beforeCount << " after=" << afterCount;
}

// Side-effecting: a Store-equivalent expressed via an `Alloca + Store`
// to local memory MUST survive even though no Load reads it. Store
// always has `hasSideEffects=true`.
TEST(Dce, PreservesStoreEvenWhenResultUnused) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32  = interner.primitive(TypeKind::I32);
    TypeId const ptr  = interner.pointer(i32);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v; v.value = std::int64_t{42}; v.core = TypeKind::I32;
    MirInstId const c = mb.addConst(v, i32);
    MirInstId const storeOps[] = {c, slot};
    (void)mb.addInst(MirOpcode::Store, storeOps, InvalidType);
    mb.addReturn();
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runDce(mir, interner, rep);
    EXPECT_TRUE(r.ok);

    // Walk the rebuilt module + assert Alloca + Store are still
    // present.
    bool sawAlloca = false, sawStore = false;
    MirFuncId const fn = mir.funcAt(0);
    MirBlockId const e = mir.funcEntry(fn);
    std::uint32_t const n = mir.blockInstCount(e);
    for (std::uint32_t i = 0; i < n; ++i) {
        MirOpcode const op = mir.instOpcode(mir.blockInstAt(e, i));
        if (op == MirOpcode::Alloca) sawAlloca = true;
        if (op == MirOpcode::Store)  sawStore = true;
    }
    EXPECT_TRUE(sawAlloca) << "Alloca is side-effecting + must survive DCE";
    EXPECT_TRUE(sawStore)  << "Store is side-effecting + must survive DCE";
}

// D-OPT-VARIADIC-RELEASE-ARGINDEX: an `Arg` is the SSA definition of a function
// PARAMETER — part of the fixed ABI signature, an ABI ROOT, never dead code even
// when its value is unused. `f(a,b,c)` returning `a + c` leaves `b` (Arg ordinal 1)
// unused. Before the fix DCE elided b's `Arg`, leaving the NON-CONTIGUOUS surviving
// set {Arg(0), Arg(2)} — whose COUNT (2) is at/below the highest surviving ordinal
// (2). The MirVerifier bounds an `Arg`'s physical ordinal by `count(Arg)` (FC7
// D-FC7-SYSV-STRUCT-ARG-MULTIREG), valid ONLY while every `Arg` survives; the
// stranded high-ordinal `Arg(2)` then tripped a SPURIOUS `I_ArgIndexOutOfRange`
// (release-only — the verifier runs between optimizer passes). Keeping every `Arg`
// holds `count(Arg)` == the physical-arg count. This is GENERAL (any fn with an
// unused param before a used high-ordinal one — a SysV variadic callee with 7 fixed
// ints, `varargs_overflow_fixed_stack`, was just the first corpus case). RED-ON-
// DISABLE: drop the `op == MirOpcode::Arg` root in isSideEffectRoot → b's `Arg` is
// elided and only 2 of the 3 Args survive.
TEST(Dce, UnusedParameterArgIsPreservedAsAbiRoot) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    std::array<TypeId, 3> const paramTys{i32, i32, i32};
    TypeId const fnSig = interner.fnSig(paramTys, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const a = mb.addArg(0, i32);
    (void)mb.addArg(1, i32);                  // b — the UNUSED parameter
    MirInstId const c = mb.addArg(2, i32);    // the high-ordinal LIVE Arg
    MirInstId const addOps[] = {a, c};
    MirInstId const sum = mb.addInst(MirOpcode::Add, addOps, i32);
    mb.addReturn(sum);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runDce(mir, interner, rep);
    EXPECT_TRUE(r.ok);

    std::uint32_t argCount = 0;
    MirFuncId const fn = mir.funcAt(0);
    MirBlockId const e = mir.funcEntry(fn);
    std::uint32_t const n = mir.blockInstCount(e);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (mir.instOpcode(mir.blockInstAt(e, i)) == MirOpcode::Arg) ++argCount;
    }
    EXPECT_EQ(argCount, 3u)
        << "an unused parameter's Arg must survive DCE (Args are ABI-parameter "
           "roots); else the surviving high-ordinal Arg strands the verifier's "
           "count-based ordinal bound";
}

// Volatile Load: `Load` has hasSideEffects=false, but the Volatile
// flag MUST keep it alive even when its result feeds no consumer.
TEST(Dce, PreservesVolatileLoadEvenWhenResultUnused) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32  = interner.primitive(TypeKind::I32);
    TypeId const ptr  = interner.pointer(i32);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirInstId const loadOps[] = {slot};
    (void)mb.addInst(MirOpcode::Load, loadOps, i32, /*payload*/0,
                     MirInstFlags::Volatile);
    mb.addReturn();
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runDce(mir, interner, rep);
    EXPECT_TRUE(r.ok);

    bool sawVolatileLoad = false;
    MirFuncId const fn = mir.funcAt(0);
    MirBlockId const e = mir.funcEntry(fn);
    std::uint32_t const n = mir.blockInstCount(e);
    for (std::uint32_t i = 0; i < n; ++i) {
        MirInstId const id = mir.blockInstAt(e, i);
        if (mir.instOpcode(id) == MirOpcode::Load &&
            has(mir.instFlags(id), MirInstFlags::Volatile)) {
            sawVolatileLoad = true;
        }
    }
    EXPECT_TRUE(sawVolatileLoad)
        << "Volatile Load must survive DCE regardless of result usage";
}

// Negative-pin replication: build the dce_negative_pin MIR shape
// directly + assert that the conditional Store(7, &x) survives along
// with the unconditional Store(100, &x). A buggy DCE that "last-store-
// wins" would delete the 100 store — both stores are side-effecting
// and CFG-reachable; both must survive.
TEST(Dce, NegativePinShape_BothStoresSurvive) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32  = interner.primitive(TypeKind::I32);
    TypeId const ptr  = interner.pointer(i32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const ifThen = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const ifJoin = mb.createBlock(StructCfMarker::Linear);

    mb.beginBlock(entry);
    MirInstId const slotA = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirInstId const slotX = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue vNeg1;   vNeg1.value = std::int64_t{-1};  vNeg1.core = TypeKind::I32;
    MirLiteralValue v100;    v100.value  = std::int64_t{100}; v100.core  = TypeKind::I32;
    MirLiteralValue v0;      v0.value    = std::int64_t{0};   v0.core    = TypeKind::I32;
    MirInstId const cN1 = mb.addConst(vNeg1, i32);
    MirInstId const c100 = mb.addConst(v100, i32);
    MirInstId const c0   = mb.addConst(v0, i32);
    MirInstId const sA[] = {cN1, slotA};
    (void)mb.addInst(MirOpcode::Store, sA, InvalidType);
    MirInstId const sX[] = {c100, slotX};
    (void)mb.addInst(MirOpcode::Store, sX, InvalidType);   // store(100, &x)
    MirInstId const ldOps[] = {slotA};
    MirInstId const ldA = mb.addInst(MirOpcode::Load, ldOps, i32);
    MirInstId const cmpOps[] = {ldA, c0};
    MirInstId const cmp = mb.addInst(MirOpcode::ICmpSgt, cmpOps, boolT);
    mb.addCondBr(cmp, ifThen, ifJoin);

    mb.beginBlock(ifThen);
    MirLiteralValue v7; v7.value = std::int64_t{7}; v7.core = TypeKind::I32;
    MirInstId const c7 = mb.addConst(v7, i32);
    MirInstId const sX2[] = {c7, slotX};
    (void)mb.addInst(MirOpcode::Store, sX2, InvalidType);   // store(7, &x)
    mb.addBr(ifJoin);

    mb.beginBlock(ifJoin);
    MirInstId const ldX[] = {slotX};
    MirInstId const ldXR = mb.addInst(MirOpcode::Load, ldX, i32);
    mb.addReturn(ldXR);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runDce(mir, interner, rep);
    EXPECT_TRUE(r.ok);

    // Count Store instructions across all blocks in the rebuilt MIR.
    int storeCount = 0;
    MirFuncId const fn = mir.funcAt(0);
    std::uint32_t const nb = mir.funcBlockCount(fn);
    for (std::uint32_t bi = 0; bi < nb; ++bi) {
        MirBlockId const b = mir.funcBlockAt(fn, bi);
        std::uint32_t const n = mir.blockInstCount(b);
        for (std::uint32_t i = 0; i < n; ++i) {
            if (mir.instOpcode(mir.blockInstAt(b, i)) == MirOpcode::Store) {
                ++storeCount;
            }
        }
    }
    EXPECT_EQ(storeCount, 3)
        << "All 3 source-level stores (a=-1, x=100, x=7) must survive — "
           "they are all side-effecting AND in CFG-reachable blocks. "
           "Pre-fold a 'last-store-wins' DCE would mis-delete one.";

    // Confirm the 3-block CFG is preserved (entry + ifThen + ifJoin
    // are all reachable from entry).
    EXPECT_EQ(mir.funcBlockCount(fn), 3u);
}

// Runtime-init globals carve-out: a module with a global whose
// `initFunc` is valid trips the cycle-1 carve-out. DCE emits
// X_OptPassSkipped at Info severity + returns ok=true + zero counters.
// Closes the silent-skip parity gap with ConstFold.
TEST(Dce, RuntimeInitGlobalsModuleEmitsXOptPassSkippedInfo) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const initSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    // Function that initializes the global at module load.
    MirFuncId const initFn = mb.addFunction(initSig, SymbolId{200});
    MirBlockId const initEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(initEntry);
    mb.addReturn();
    // Global with runtime-init referencing the function above.
    mb.addGlobal(i32, SymbolId{201}, /*initLiteralIndex*/ UINT32_MAX, initFn);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runDce(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsEliminated, 0u);
    EXPECT_EQ(r.blocksEliminated, 0u);
    EXPECT_EQ(r.functionsEliminated, 0u);
    EXPECT_EQ(r.globalsEliminated, 0u);
    std::size_t infoCount = 0;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::X_OptPassSkipped) ++infoCount;
    }
    EXPECT_EQ(infoCount, 1u)
        << "DCE must emit X_OptPassSkipped Info when skipping a "
           "module with runtime-init globals (mirrors ConstFold's "
           "carve-out signal).";
}

// const-ness preservation across DCE's standalone global-clone loop
// (D-LK4-DATA-PRODUCER-MUTABLE-GLOBAL). DCE rebuilds the surviving globals
// (dce.cpp:303) — it MUST carry `MirGlobal.isConst`, or a const global silently
// degrades to a writable `.data` section after DCE (loss of read-only-memory
// protection). Both globals have external (Global/Default) linkage → liveness
// ROOTS, kept regardless of references, isolating the clone-loop field-carry.
// RED-ON-DISABLE: drop the `mir.globalIsConst(g)` arg at dce.cpp:306 → the const
// global comes back mutable and `constCount == 1` fails.
TEST(Dce, PreservesGlobalConstness) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32 = interner.primitive(TypeKind::I32);
    TypeId const sig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    MirLiteralValue v5; v5.value = std::int64_t{5}; v5.core = TypeKind::I32;
    MirLiteralValue v7; v7.value = std::int64_t{7}; v7.core = TypeKind::I32;
    // gc CONST (→ .rodata); gm MUTABLE (→ .data). Both external roots.
    mb.addGlobal(i32, SymbolId{300}, mb.literalPoolAdd(v5), MirFuncId{},
                 SymbolBinding::Global, SymbolVisibility::Default, /*isConst=*/true);
    mb.addGlobal(i32, SymbolId{301}, mb.literalPoolAdd(v7), MirFuncId{},
                 SymbolBinding::Global, SymbolVisibility::Default, /*isConst=*/false);
    // A trivial root function so DCE runs its full rebuild path (not a carve-out).
    mb.addFunction(sig, SymbolId{100});
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runDce(mir, interner, rep);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(mir.moduleGlobalCount(), 2u)
        << "both externally-visible globals are liveness roots — DCE keeps them";
    int constCount = 0, mutCount = 0;
    for (std::uint32_t i = 0; i < mir.moduleGlobalCount(); ++i) {
        if (mir.globalIsConst(mir.globalAt(i))) ++constCount; else ++mutCount;
    }
    EXPECT_EQ(constCount, 1)
        << "the CONST global must survive DCE as const (else it lands in a "
           "writable .data section)";
    EXPECT_EQ(mutCount, 1) << "the mutable global must survive DCE as mutable";
}
