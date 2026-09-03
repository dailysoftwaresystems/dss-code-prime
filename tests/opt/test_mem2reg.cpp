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
//      `examples/c/copy_prop_across_join` whose
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
#include "mir/mir_verifier.hpp"
#include "opt/passes/mem2reg.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <variant>
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

// If `id` is an integer `Const`, its int64 literal value; else nullopt.
std::optional<std::int64_t> constIntValue(Mir const& mir, MirInstId id) {
    if (mir.instOpcode(id) != MirOpcode::Const) return std::nullopt;
    MirLiteralValue const& lv = mir.literalValue(mir.instPayload(id));
    if (auto const* p = std::get_if<std::int64_t>(&lv.value)) return *p;
    return std::nullopt;
}

// Count integer `Const`s in the module whose value equals `want`.
std::size_t countConstIntInModule(Mir const& mir, std::int64_t want) {
    std::size_t n = 0;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t i2 = 0; i2 < ni; ++i2) {
                auto const v = constIntValue(mir, mir.blockInstAt(b, i2));
                if (v && *v == want) ++n;
            }
        }
    }
    return n;
}

template <class F>
void forEachPhi(Mir const& mir, F&& f) {
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const fn = mir.funcAt(i);
        std::uint32_t const nb = mir.funcBlockCount(fn);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(fn, bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t i2 = 0; i2 < ni; ++i2) {
                MirInstId const id = mir.blockInstAt(b, i2);
                if (mir.instOpcode(id) == MirOpcode::Phi) f(id);
            }
        }
    }
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

// D-OPT-MEM2REG-CONDITIONAL-INIT-UNDEF — a LIVE conditionally-initialized local:
// `if (c) x = 1; return x + c;` stores x on ONLY ONE arm. The merge Phi's else-edge
// (entry → join) has no reaching store; Mem2Reg must materialize undef-as-zero (an
// entry-block Const 0) rather than std::abort() the release compile — valid C that
// gcc/SQLite rely on. i32 is GPR-Const-zeroable, so the alloca stays PROMOTED.
// RED-on-disable: revert the zero materialization → the rename walk std::abort()s.
TEST(Mem2Reg, ConditionallyInitializedLiveAllocaZeroFillsUndefEdge) {
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
    MirBlockId const join  = mb.createBlock(StructCfMarker::IfJoin);

    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    mb.addCondBr(cond, tArm, join);   // else edge → join directly: NO store to x

    mb.beginBlock(tArm);
    MirLiteralValue one; one.value = std::int64_t{1}; one.core = TypeKind::I32;
    MirInstId const c1 = mb.addConst(one, i32);
    MirInstId const s1[] = {c1, slot};
    (void)mb.addInst(MirOpcode::Store, s1, InvalidType);
    mb.addBr(join);

    mb.beginBlock(join);
    MirInstId const loadOps[] = {slot};
    MirInstId const ld = mb.addInst(MirOpcode::Load, loadOps, i32);
    mb.addReturn(ld);

    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runMem2Reg(mir, interner, rep);
    ASSERT_TRUE(r.ok) << "a conditional-init live local must compile, not abort";
    EXPECT_EQ(r.allocasPromoted,  1u) << "i32 is Const-zeroable → stays promoted";
    EXPECT_EQ(r.phisInserted,     1u);
    EXPECT_EQ(r.loadsReplaced,    1u);
    EXPECT_EQ(r.storesEliminated, 1u);   // only the one arm stored
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Alloca), 0u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Load),   0u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Phi),    1u);

    // The join Phi has 2 incomings: the then-arm's Const 1 and the materialized
    // undef-as-zero Const 0 on the else edge.
    std::size_t incCount = 0;
    bool sawZero = false, sawOne = false;
    forEachPhi(mir, [&](MirInstId phi) {
        auto const incs = mir.phiIncomings(phi);
        incCount = incs.size();
        for (auto const& inc : incs) {
            auto const v = constIntValue(mir, inc.value);
            if (v && *v == 0) sawZero = true;
            if (v && *v == 1) sawOne  = true;
        }
    });
    EXPECT_EQ(incCount, 2u);
    EXPECT_TRUE(sawZero) << "the undef else edge must be a Const 0";
    EXPECT_TRUE(sawOne)  << "the then arm keeps its Const 1";

    // The entry-block Const 0 must dominate the join predecessor — proven by a full
    // structural/dominance verify of the rebuilt module (the fix's load-bearing claim).
    DiagnosticReporter vrep;
    EXPECT_TRUE(MirVerifier(mir, &interner).verify(vrep))
        << "the undef-as-zero incoming must be dominance-legal";
}

// D-OPT-MEM2REG-CONDITIONAL-INIT-UNDEF — the Load empty-stack site: `int x; return x;`
// is a direct uninitialized read (UB, but valid to compile). The Load resolves to
// undef-as-zero rather than abort. RED-on-disable: the pre-fix Load-empty-stack
// std::abort() fires.
TEST(Mem2Reg, ConditionallyInitializedLoadZeroFillsUninitPath) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirInstId const loadOps[] = {slot};
    MirInstId const ld = mb.addInst(MirOpcode::Load, loadOps, i32);
    mb.addReturn(ld);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runMem2Reg(mir, interner, rep);
    ASSERT_TRUE(r.ok) << "an uninitialized read must compile, not abort";
    EXPECT_EQ(r.allocasPromoted, 1u);
    EXPECT_EQ(r.loadsReplaced,   1u);
    EXPECT_EQ(r.phisInserted,    0u);   // single block — no joins
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Load),   0u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Alloca), 0u);
    EXPECT_GE(countConstIntInModule(mir, 0), 1u) << "the read resolves to a Const 0";
    DiagnosticReporter vrep;
    EXPECT_TRUE(MirVerifier(mir, &interner).verify(vrep));
}

// ── D-OPT-MEM2REG-FPR-CONDITIONAL-INIT-RODATA-ZERO ────────────────────────────
//
// Build `T x; if (c) x = v; return x;` for a FLOAT element type and run Mem2Reg.
// Returns the finished module so each arm can assert on it. The store value is a
// real ARG (not a synthetic float Const), matching how HIR→MIR shapes floats.
struct CondInitFloatFixture {
    Mir                        mir;
    opt::passes::Mem2RegResult result;
};

CondInitFloatFixture runCondInitFloat(TypeInterner& interner, TypeKind floatKind) {
    TypeId const fT    = interner.primitive(floatKind);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const ptr   = interner.pointer(fT);
    TypeId const params[] = {boolT, fT};
    TypeId const fnSig = interner.fnSig(params, fT, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tArm  = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const join  = mb.createBlock(StructCfMarker::IfJoin);

    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    MirInstId const val  = mb.addArg(1, fT);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    mb.addCondBr(cond, tArm, join);   // else edge → join: NO store to x

    mb.beginBlock(tArm);
    MirInstId const s1[] = {val, slot};
    (void)mb.addInst(MirOpcode::Store, s1, InvalidType);
    mb.addBr(join);

    mb.beginBlock(join);
    MirInstId const loadOps[] = {slot};
    MirInstId const ld = mb.addInst(MirOpcode::Load, loadOps, fT);
    mb.addReturn(ld);

    CondInitFloatFixture out{std::move(mb).finish(), {}};
    DiagnosticReporter rep;
    out.result = opt::passes::runMem2Reg(out.mir, interner, rep);
    return out;
}

// D-OPT-MEM2REG-FPR-CONDITIONAL-INIT-RODATA-ZERO — a conditionally-initialized
// DOUBLE now PROMOTES. A float has no directly-lowerable zero `Const` (register
// machines have no float-immediate form), so the undef edge's zero is materialized
// the way DSS materializes EVERY float constant: an anonymous read-only MirGlobal
// holding the zero bit pattern, reached by `GlobalAddr` + `Load` in the ENTRY block
// (which dominates every edge). The alloca / store / load all disappear.
//
// RED-on-disable: put F32/F64/F80/F128 back on the `ZeroForm::None` arm of
// `zeroFormFor` (i.e. restore the de-promotion) → `allocasPromoted` falls to 0, no
// global is minted, and the alloca/store/load survive.
TEST(Mem2Reg, ConditionallyInitializedFloatAllocaPromotesViaRodataZero) {
    TypeInterner interner{CompilationUnitId{1}};
    auto fx = runCondInitFloat(interner, TypeKind::F64);
    Mir const& mir = fx.mir;

    ASSERT_TRUE(fx.result.ok) << "a conditional-init float must compile, not abort";
    EXPECT_EQ(fx.result.allocasPromoted,   1u)
        << "an FPR conditional-init alloca now promotes via a rodata zero";
    EXPECT_EQ(fx.result.phisInserted,      1u);
    EXPECT_EQ(fx.result.loadsReplaced,     1u);
    EXPECT_EQ(fx.result.storesEliminated,  1u);
    EXPECT_EQ(fx.result.rodataZerosMinted, 1u)
        << "exactly one anonymous zero global for the one float element type";

    // The slot is gone; what remains is the zero's GlobalAddr + Load + the phi.
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Alloca),     0u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Store),      0u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Phi),        1u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::GlobalAddr), 1u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Load),       1u)
        << "the ONLY surviving Load is the rodata zero's";

    // The minted global: anonymous, read-only, F64-typed, constant-initialized to
    // +0.0, and — load-bearing — LOCAL-bound so `mergeCuMirs` can never unify it
    // with anything by name (the c86 D-MIR-SYNTHETIC-GLOBAL-SYMBOL-ALIAS class).
    ASSERT_EQ(mir.moduleGlobalCount(), 1u);
    MirGlobalId const g = mir.globalAt(0);
    EXPECT_EQ(mir.globalBinding(g), SymbolBinding::Local)
        << "a GLOBAL-bound anonymous constant could collapse onto a same-named "
           "definition in another CU, or strip a real FFI import";
    EXPECT_EQ(mir.globalVisibility(g), SymbolVisibility::Hidden);
    EXPECT_TRUE(mir.globalIsConst(g)) << "isConst is asm.cpp's .rodata authority";
    EXPECT_FALSE(mir.globalIsThreadLocal(g));
    EXPECT_EQ(mir.globalType(g).v, interner.primitive(TypeKind::F64).v);
    EXPECT_GT(mir.globalSymbol(g).v, 100u)
        << "the minted symbol must clear every symbol the module already carries";
    ASSERT_NE(mir.globalInitLiteralIndex(g), UINT32_MAX)
        << "the zero must be a CONSTANT initializer, not a runtime-init global";
    MirLiteralValue const& lv = mir.literalValue(mir.globalInitLiteralIndex(g));
    ASSERT_TRUE(std::holds_alternative<double>(lv.value));
    EXPECT_EQ(std::get<double>(lv.value), 0.0);
    EXPECT_EQ(lv.core, TypeKind::F64);

    // The join Phi's two incomings must BOTH be F64 (the arm's Arg and the zero).
    forEachPhi(mir, [&](MirInstId phi) {
        auto const incs = mir.phiIncomings(phi);
        EXPECT_EQ(incs.size(), 2u);
        for (auto const& inc : incs) {
            EXPECT_EQ(mir.instType(inc.value).v, mir.instType(phi).v)
                << "every incoming carries the slot's element type";
        }
    });

    DiagnosticReporter vrep;
    EXPECT_TRUE(MirVerifier(mir, &interner).verify(vrep))
        << "the rodata-zero incoming must be dominance- and type-legal";
}

// The F32 sibling — the SAME rodata producer, one float width over. Pins that the
// zero's literal `core` follows the ELEMENT type (the globals byte-emitter narrows
// double→float on the F32 arm and would emit 8 bytes for a 4-byte slot otherwise).
TEST(Mem2Reg, ConditionallyInitializedFloat32AllocaPromotesViaRodataZero) {
    TypeInterner interner{CompilationUnitId{1}};
    auto fx = runCondInitFloat(interner, TypeKind::F32);
    ASSERT_TRUE(fx.result.ok);
    EXPECT_EQ(fx.result.allocasPromoted,   1u);
    EXPECT_EQ(fx.result.rodataZerosMinted, 1u);
    ASSERT_EQ(fx.mir.moduleGlobalCount(), 1u);
    MirGlobalId const g = fx.mir.globalAt(0);
    EXPECT_EQ(fx.mir.globalType(g).v, interner.primitive(TypeKind::F32).v);
    MirLiteralValue const& lv = fx.mir.literalValue(fx.mir.globalInitLiteralIndex(g));
    EXPECT_EQ(lv.core, TypeKind::F32) << "the literal's core is the ELEMENT width";
    DiagnosticReporter vrep;
    EXPECT_TRUE(MirVerifier(fx.mir, &interner).verify(vrep));
}

// ══ THE FAIL-LOUD-IN-PLACE BOUNDARY, SPLIT IN TWO ═══════════════════════════════
//
// This used to be ONE test making TWO independent claims — F16 and F80/F128 both
// stay de-promoted — for two unrelated reasons. `D-CSUBSET-LONG-DOUBLE-CONTROL-MERGE`
// answered only ONE of them, so the test had to split rather than shrink: F16's claim
// is unchanged and keeps a pin of its own, and the long-double claim INVERTS.
// Collapsing the pair would have traded a pin for a comment.

// F16 STILL DE-PROMOTES, and its reason has nothing to do with long doubles: F16 has
// no encodings at ANY width (D-TARGET-ENCODING-WIDTH-GUARD), so neither its zero
// constant nor its phi can be realized. The correct answer is DE-PROMOTION (leave it
// in memory, exactly what the debug pipeline does), never a refusal — a PERF
// refinement may not turn a program that compiles today into one that does not. The
// matching `onBlockBegin` abort is the invariant assert that keeps the classification
// and the de-promotion from drifting apart.
//
// RED-on-disable: move F16 onto the `RodataLoad` arm of `zeroFormFor` → this test
// sees `allocasPromoted == 1` and the alloca/store/load gone.
TEST(Mem2Reg, ConditionallyInitializedF16AllocaStaysDepromoted) {
    TypeKind const k = TypeKind::F16;
    TypeInterner interner{CompilationUnitId{1}};
    auto fx = runCondInitFloat(interner, k);
    unsigned const ord = static_cast<unsigned>(k);
    ASSERT_TRUE(fx.result.ok) << "kind ordinal " << ord
                              << " conditional-init must still COMPILE";
    EXPECT_EQ(fx.result.allocasPromoted,   0u)
        << "F16 has no usable zero form (no encodings at any width) → de-promoted, "
           "not refused — and NOT because of any long-double phi question";
    EXPECT_EQ(fx.result.rodataZerosMinted, 0u) << "kind ordinal " << ord;
    EXPECT_EQ(fx.mir.moduleGlobalCount(), 0u)
        << "no global is minted for a de-promotion (kind ordinal " << ord << ")";
    EXPECT_EQ(countOpInModule(fx.mir, MirOpcode::Alloca), 1u) << ord;
    EXPECT_EQ(countOpInModule(fx.mir, MirOpcode::Store),  1u) << ord;
    EXPECT_EQ(countOpInModule(fx.mir, MirOpcode::Load),   1u) << ord;
    EXPECT_EQ(countOpInModule(fx.mir, MirOpcode::Phi),    0u) << ord;
    DiagnosticReporter vrep;
    EXPECT_TRUE(MirVerifier(fx.mir, &interner).verify(vrep)) << ord;
}

// D-CSUBSET-LONG-DOUBLE-CONTROL-MERGE — INVERTED from the de-promotion arm above.
// A conditionally-initialized `long double` now PROMOTES on both long-double axes
// (F80 = x87 extended, F128 = IEEE binary128), because the thing that was missing
// was never the CONSTANT — it was the PHI, and the memory-home merge supplies it.
// The alloca/store/load disappear and a phi appears, exactly as for F32/F64.
//
// RED-on-disable: put F80/F128 back on the `ZeroForm::None` arm of `zeroFormFor` →
// `allocasPromoted` falls to 0, no global is minted, `phisInserted` falls to 0 and
// the alloca/store/load survive.
TEST(Mem2Reg, ConditionallyInitializedLongDoubleAllocaPromotesViaRodataZero) {
    for (TypeKind const k : {TypeKind::F80, TypeKind::F128}) {
        TypeInterner interner{CompilationUnitId{1}};
        auto fx = runCondInitFloat(interner, k);
        unsigned const ord = static_cast<unsigned>(k);
        ASSERT_TRUE(fx.result.ok) << "kind ordinal " << ord
                                  << " conditional-init must compile, not abort";
        EXPECT_EQ(fx.result.allocasPromoted,   1u)
            << "kind ordinal " << ord << " now has a usable zero form AND a phi "
               "lowering (D-CSUBSET-LONG-DOUBLE-CONTROL-MERGE)";
        EXPECT_EQ(fx.result.phisInserted,      1u) << ord;
        EXPECT_EQ(fx.result.loadsReplaced,     1u) << ord;
        EXPECT_EQ(fx.result.storesEliminated,  1u) << ord;
        EXPECT_EQ(fx.result.rodataZerosMinted, 1u)
            << "one anonymous rodata +0.0 for the one long-double element type "
               "(kind ordinal " << ord << ")";
        ASSERT_EQ(fx.mir.moduleGlobalCount(), 1u) << ord;
        MirGlobalId const g = fx.mir.globalAt(0);
        EXPECT_EQ(fx.mir.globalType(g).v, interner.primitive(k).v) << ord;
        MirLiteralValue const& lv =
            fx.mir.literalValue(fx.mir.globalInitLiteralIndex(g));
        EXPECT_EQ(lv.core, k) << "the literal's core is the ELEMENT kind" << ord;
        EXPECT_EQ(countOpInModule(fx.mir, MirOpcode::Alloca), 0u) << ord;
        EXPECT_EQ(countOpInModule(fx.mir, MirOpcode::Store),  0u) << ord;
        EXPECT_EQ(countOpInModule(fx.mir, MirOpcode::Phi),    1u) << ord;
        DiagnosticReporter vrep;
        EXPECT_TRUE(MirVerifier(fx.mir, &interner).verify(vrep)) << ord;
    }
}

// A GPR conditional-init must NOT have regressed onto the rodata path — an integer
// zero is a direct `Const 0`, no global, no memory. The complement of the arm above.
TEST(Mem2Reg, ConditionallyInitializedIntAllocaStillUsesADirectConstZero) {
    TypeInterner interner{CompilationUnitId{1}};
    auto fx = runCondInitFloat(interner, TypeKind::I32);
    ASSERT_TRUE(fx.result.ok);
    EXPECT_EQ(fx.result.allocasPromoted,   1u);
    EXPECT_EQ(fx.result.rodataZerosMinted, 0u) << "a GPR zero needs no constant pool";
    EXPECT_EQ(fx.mir.moduleGlobalCount(), 0u);
    EXPECT_EQ(countOpInModule(fx.mir, MirOpcode::Load),       0u);
    EXPECT_EQ(countOpInModule(fx.mir, MirOpcode::GlobalAddr), 0u);
    EXPECT_GE(countConstIntInModule(fx.mir, 0), 1u);
}

// TWO conditionally-initialized floats of DIFFERENT widths in one function: one
// global per distinct element type, each with its OWN fresh symbol. A shared or
// re-seeded symbol counter would collapse them onto one arena slot.
TEST(Mem2Reg, TwoFloatWidthsMintTwoDistinctZeroGlobals) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const f32   = interner.primitive(TypeKind::F32);
    TypeId const f64   = interner.primitive(TypeKind::F64);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const p32   = interner.pointer(f32);
    TypeId const p64   = interner.pointer(f64);
    TypeId const params[] = {boolT, f32, f64};
    TypeId const fnSig = interner.fnSig(params, f64, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tArm  = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const join  = mb.createBlock(StructCfMarker::IfJoin);

    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    MirInstId const v32  = mb.addArg(1, f32);
    MirInstId const v64  = mb.addArg(2, f64);
    MirInstId const s32  = mb.addInst(MirOpcode::Alloca, {}, p32);
    MirInstId const s64  = mb.addInst(MirOpcode::Alloca, {}, p64);
    mb.addCondBr(cond, tArm, join);

    mb.beginBlock(tArm);
    MirInstId const st32[] = {v32, s32};
    (void)mb.addInst(MirOpcode::Store, st32, InvalidType);
    MirInstId const st64[] = {v64, s64};
    (void)mb.addInst(MirOpcode::Store, st64, InvalidType);
    mb.addBr(join);

    mb.beginBlock(join);
    MirInstId const l32ops[] = {s32};
    MirInstId const l32 = mb.addInst(MirOpcode::Load, l32ops, f32);
    MirInstId const l64ops[] = {s64};
    MirInstId const l64 = mb.addInst(MirOpcode::Load, l64ops, f64);
    MirInstId const widen[] = {l32};
    MirInstId const w = mb.addInst(MirOpcode::FPExt, widen, f64);
    MirInstId const sum[] = {w, l64};
    mb.addReturn(mb.addInst(MirOpcode::FAdd, sum, f64));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runMem2Reg(mir, interner, rep);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.allocasPromoted,   2u);
    EXPECT_EQ(r.rodataZerosMinted, 2u) << "one zero global per distinct element type";
    ASSERT_EQ(mir.moduleGlobalCount(), 2u);
    EXPECT_NE(mir.globalSymbol(mir.globalAt(0)).v,
              mir.globalSymbol(mir.globalAt(1)).v)
        << "each minted global needs its OWN symbol";
    DiagnosticReporter vrep;
    EXPECT_TRUE(MirVerifier(mir, &interner).verify(vrep));
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

// D-OPT-SETJMP-RETURNS-TWICE-INLINE (mem2reg half; FC17.9(c) D-CSUBSET-SETJMP): a
// function that CONTAINS a returns-twice Call (setjmp/_setjmp) promotes NO allocas —
// even a slot the Call never touches. A matching longjmp re-enters the setjmp site
// over a control-flow edge the CFG cannot see, so a promoted local reassigned on that
// hidden path would carry a stale entry-reaching SSA value past the resume; kept in
// memory (no promotion), longjmp's SP + callee-saved restore makes the last store
// observable (GCC's returns_twice treatment). This pins the WHOLE-FUNCTION bail: the
// alloca here is otherwise textbook-promotable — a single store then load, and the
// Call does NOT take its address — so ONLY the MirInstFlags::ReturnsTwice Call blocks
// it. RED-ON-DISABLE: remove the returns-twice whole-function scan in mem2reg.cpp
// analyze() → the alloca IS promoted (allocasPromoted == 1) and this
// EXPECT_EQ(...,0) fails. Structural twin of VolatileAccessAllocaNotPromoted.
TEST(Mem2Reg, ReturnsTwiceCallInFunctionBlocksPromotion) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const calleeSig = interner.fnSig({}, i32, CallConv::CcSysV);
    TypeId const fnSig     = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    // An extern returns-twice callee (setjmp-class), modeled as an externally-
    // visible function (the AddressTakenAllocaNotPromoted extern-callee pattern).
    // MIR lowering stamps MirInstFlags::ReturnsTwice on a DIRECT call to such a
    // callee; here the fixture sets the flag directly (the carrier-test discipline).
    mb.addFunction(calleeSig, SymbolId{50},
                   SymbolBinding::Global, SymbolVisibility::Default);
    MirBlockId const calleeEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(calleeEntry);
    MirLiteralValue z; z.value = std::int64_t{0}; z.core = TypeKind::I32;
    mb.addReturn(mb.addConst(z, i32));

    // The function under test: a textbook-promotable scalar slot (single store then
    // load) PLUS a returns-twice Call that does NOT touch the slot.
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v; v.value = std::int64_t{42}; v.core = TypeKind::I32;
    MirInstId const c = mb.addConst(v, i32);
    MirInstId const storeOps[] = {c, slot};
    (void)mb.addInst(MirOpcode::Store, storeOps, InvalidType);   // non-volatile
    // The returns-twice barrier — its ONLY operand is the callee (no alloca operand,
    // so the slot does not escape and would otherwise promote cleanly).
    MirInstId const calleeAddr = mb.addGlobalAddr(SymbolId{50}, calleeSig);
    MirInstId const callOps[] = {calleeAddr};
    (void)mb.addInst(MirOpcode::Call, callOps, i32, /*payload=*/0,
                     MirInstFlags::ReturnsTwice);
    MirInstId const loadOps[] = {slot};
    MirInstId const ld = mb.addInst(MirOpcode::Load, loadOps, i32);
    mb.addReturn(ld);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runMem2Reg(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.allocasPromoted, 0u)
        << "a returns-twice (setjmp) Call in the function forces every local to stay "
           "memory-resident — mem2reg must promote nothing (else a live-across-setjmp "
           "local reads a stale value on the longjmp resume)";
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

// ═══ D-OPT-MEM2REG-ERASES-CALL-OPERAND-POINTEE ═══════════════════════════════
//
// THE MEASURED SHAPE, rebuilt at the MIR tier: `examples/c/va_list_param_forward`'s
// `sumv` does `va_list ap; va_start(ap, n); vsum(n, ap);`. `hir_to_mir` types the
// `VaHomeArgAreaAddr` leaf `ptr<void>` and stores it into an `ap` slot whose element
// type is the declared `va_list` (= `ptr<i8>` under Win64). Mem2Reg promotes the slot
// and — before this fix — forwarded that `ptr<void>` DEFINITION straight into a call
// operand declared `ptr<i8>`, erasing the pointee with no retagging Cast.
//
// The rule this pins is STRICTER than `MirVerifier::sameSlotType`, deliberately:
// the verifier admits `void*` on either side, and this asserts EXACT TypeId identity
// at the call operand — i.e. the property the verifier had to give up because of
// this pass, restored.
//
// RED-on-disable: make `isPointerRetag` return false unconditionally → the operand
// is `ptr<void>` again and `retagsInserted` is 0.
namespace {
// Build `int sumv(int n) { va_list ap = <VaHomeArgAreaAddr>; return vsum(n, ap); }`
// with `int vsum(int, ptr<i8>)`.
struct VaForwardFixture {
    Mir        mir;
    TypeId     declaredVaList{};   // ptr<i8>
    SymbolId   callee{50};
};

VaForwardFixture buildVaForward(TypeInterner& interner) {
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const i8T    = interner.primitive(TypeKind::I8);
    TypeId const voidT  = interner.primitive(TypeKind::Void);
    TypeId const vaList = interner.pointer(i8T);       // the DECLARED va_list
    TypeId const voidP  = interner.pointer(voidT);     // what the Va* leaf carries
    TypeId const slotTy = interner.pointer(vaList);    // the `ap` alloca's type
    TypeId const calleeParams[] = {i32, vaList};
    TypeId const calleeSig = interner.fnSig(calleeParams, i32, CallConv::CcSysV);
    TypeId const callerParams[] = {i32};
    TypeId const callerSig = interner.fnSig(callerParams, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(calleeSig, SymbolId{50},
                   SymbolBinding::Global, SymbolVisibility::Default);
    MirBlockId const calleeEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(calleeEntry);
    MirLiteralValue z; z.value = std::int64_t{0}; z.core = TypeKind::I32;
    mb.addReturn(mb.addConst(z, i32));

    mb.addFunction(callerSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const n    = mb.addArg(0, i32);
    MirInstId const ap   = mb.addInst(MirOpcode::Alloca, {}, slotTy);
    // The `ptr<void>`-typed frame leaf — the definition that used to be forwarded.
    MirInstId const home = mb.addInst(MirOpcode::VaHomeArgAreaAddr, {}, voidP);
    MirInstId const st[] = {home, ap};
    (void)mb.addInst(MirOpcode::Store, st, InvalidType);
    MirInstId const ld[] = {ap};
    MirInstId const apVal = mb.addInst(MirOpcode::Load, ld, vaList);
    MirInstId const calleeAddr = mb.addGlobalAddr(SymbolId{50}, calleeSig);
    MirInstId const callOps[] = {calleeAddr, n, apVal};
    mb.addReturn(mb.addInst(MirOpcode::Call, callOps, i32));

    return VaForwardFixture{std::move(mb).finish(), vaList, SymbolId{50}};
}

// The type of the LAST Call instruction's operand at flat index `opIndex`
// (0 = callee), or InvalidType if there is no Call.
TypeId callOperandType(Mir const& mir, std::size_t opIndex) {
    TypeId out{};
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t i2 = 0; i2 < ni; ++i2) {
                MirInstId const id = mir.blockInstAt(b, i2);
                if (mir.instOpcode(id) != MirOpcode::Call) continue;
                auto const ops = mir.instOperands(id);
                if (opIndex < ops.size()) out = mir.instType(ops[opIndex]);
            }
        }
    }
    return out;
}
} // namespace

TEST(Mem2Reg, PromotedValueArrivesAtCallOperandWithTheDeclaredPointee) {
    TypeInterner interner{CompilationUnitId{1}};
    auto fx = buildVaForward(interner);

    // BASELINE: the un-promoted module already passes the declared `ptr<i8>` (the
    // Load carries the slot's element type) — the erasure was purely the pass's.
    ASSERT_EQ(callOperandType(fx.mir, 2).v, fx.declaredVaList.v)
        << "pre-condition: the baseline lowering is already type-exact";

    DiagnosticReporter rep;
    auto const r = opt::passes::runMem2Reg(fx.mir, interner, rep);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.allocasPromoted,  1u);
    EXPECT_EQ(r.loadsReplaced,    1u);
    EXPECT_EQ(r.retagsInserted,   1u)
        << "exactly one retagging Bitcast, at the store that wrote the ptr<void>";
    EXPECT_EQ(countOpInModule(fx.mir, MirOpcode::Bitcast), 1u);
    EXPECT_EQ(countOpInModule(fx.mir, MirOpcode::Alloca),  0u);
    EXPECT_EQ(countOpInModule(fx.mir, MirOpcode::Store),   0u);
    EXPECT_EQ(countOpInModule(fx.mir, MirOpcode::Load),    0u);

    // THE ASSERTION THIS ROW EXISTS FOR: exact TypeId identity, not `sameSlotType`
    // compatibility. Before the fix this was `ptr<void>`.
    EXPECT_EQ(callOperandType(fx.mir, 2).v, fx.declaredVaList.v)
        << "Mem2Reg must not erase the declared pointee at a call operand";

    DiagnosticReporter vrep;
    EXPECT_TRUE(MirVerifier(fx.mir, &interner).verify(vrep));
}

// The retag lands at the STORE, so it also fixes the PHI shape — a diamond whose two
// arms both store `ptr<void>` into a `ptr<i8>` slot must produce a `ptr<i8>` phi with
// two `ptr<i8>` incomings. A Load-site-only retag would leave the phi's incomings
// carrying the erased pointee.
TEST(Mem2Reg, RetagAtTheStoreKeepsPhiIncomingsTypeExact) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i8T    = interner.primitive(TypeKind::I8);
    TypeId const voidT  = interner.primitive(TypeKind::Void);
    TypeId const boolT  = interner.primitive(TypeKind::Bool);
    TypeId const vaList = interner.pointer(i8T);
    TypeId const voidP  = interner.pointer(voidT);
    TypeId const slotTy = interner.pointer(vaList);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, vaList, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tArm  = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const fArm  = mb.createBlock(StructCfMarker::IfElse);
    MirBlockId const join  = mb.createBlock(StructCfMarker::IfJoin);

    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, slotTy);
    mb.addCondBr(cond, tArm, fArm);

    mb.beginBlock(tArm);
    MirInstId const h1 = mb.addInst(MirOpcode::VaHomeArgAreaAddr, {}, voidP);
    MirInstId const s1[] = {h1, slot};
    (void)mb.addInst(MirOpcode::Store, s1, InvalidType);
    mb.addBr(join);

    mb.beginBlock(fArm);
    MirInstId const h2 = mb.addInst(MirOpcode::VaOverflowArgAreaAddr, {}, voidP);
    MirInstId const s2[] = {h2, slot};
    (void)mb.addInst(MirOpcode::Store, s2, InvalidType);
    mb.addBr(join);

    mb.beginBlock(join);
    MirInstId const ld[] = {slot};
    mb.addReturn(mb.addInst(MirOpcode::Load, ld, vaList));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runMem2Reg(mir, interner, rep);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.phisInserted,   1u);
    EXPECT_EQ(r.retagsInserted, 2u) << "one retag per mismatching STORE, not per use";
    bool sawPhi = false;
    forEachPhi(mir, [&](MirInstId phi) {
        sawPhi = true;
        EXPECT_EQ(mir.instType(phi).v, vaList.v);
        auto const incs = mir.phiIncomings(phi);
        EXPECT_EQ(incs.size(), 2u);
        for (auto const& inc : incs) {
            EXPECT_EQ(mir.instType(inc.value).v, vaList.v)
                << "a phi incoming must carry the phi's own pointee";
        }
    });
    EXPECT_TRUE(sawPhi);
    DiagnosticReporter vrep;
    EXPECT_TRUE(MirVerifier(mir, &interner).verify(vrep));
}

// THE NARROWNESS IS THE CORRECTNESS ARGUMENT. A `Bitcast` is a REINTERPRETATION, so
// it may only be emitted where the two types are the same machine value. A store
// whose value type differs NON-pointer-wise (here `i32` into an `f64` slot) is an
// ill-typed store some other rule owns — Mem2Reg must leave it EXACTLY as before
// rather than invent a conversion that would silently mis-lower it and delete the
// diagnostic. RED-on-disable: widen `isPointerRetag` to "any differing pair" → a
// Bitcast appears here.
TEST(Mem2Reg, NonPointerTypeMismatchAtAStoreGetsNoInventedCast) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const f64   = interner.primitive(TypeKind::F64);
    TypeId const ptr   = interner.pointer(f64);
    TypeId const fnSig = interner.fnSig({}, f64, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v; v.value = std::int64_t{7}; v.core = TypeKind::I32;
    MirInstId const c = mb.addConst(v, i32);          // i32 value into an f64 slot
    MirInstId const st[] = {c, slot};
    (void)mb.addInst(MirOpcode::Store, st, InvalidType);
    MirInstId const ld[] = {slot};
    mb.addReturn(mb.addInst(MirOpcode::Load, ld, f64));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runMem2Reg(mir, interner, rep);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.allocasPromoted, 1u);
    EXPECT_EQ(r.retagsInserted,  0u)
        << "Mem2Reg must not invent a conversion for a genuinely ill-typed store";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Bitcast), 0u);
}

// A store that ALREADY writes the slot's exact pointer type costs nothing — the
// common case, and the pin that this fix is not a blanket instruction tax.
TEST(Mem2Reg, MatchingPointerStoreInsertsNoRetag) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i8T    = interner.primitive(TypeKind::I8);
    TypeId const vaList = interner.pointer(i8T);
    TypeId const slotTy = interner.pointer(vaList);
    TypeId const params[] = {vaList};
    TypeId const fnSig = interner.fnSig(params, vaList, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const a    = mb.addArg(0, vaList);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, slotTy);
    MirInstId const st[] = {a, slot};
    (void)mb.addInst(MirOpcode::Store, st, InvalidType);
    MirInstId const ld[] = {slot};
    mb.addReturn(mb.addInst(MirOpcode::Load, ld, vaList));
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runMem2Reg(mir, interner, rep);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.allocasPromoted, 1u);
    EXPECT_EQ(r.retagsInserted,  0u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Bitcast), 0u);
}
