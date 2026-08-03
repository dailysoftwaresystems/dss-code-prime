// MIR-tier CSE / GVN unit tests.
//
// Pins:
//   * SAME-BLOCK CSE: two `Add(a, b)` in straight-line code → second
//     redirected to first (instructionsCsed == 1; both Adds survive
//     until DCE, but later uses point at the first).
//   * COMMUTATIVE CANONICALIZATION: `Add(a, b)` and `Add(b, a)`
//     merge — operands sorted by id before key.
//   * NON-COMMUTATIVE DISTINCTION: `Sub(a, b)` and `Sub(b, a)` do
//     NOT merge — D-OPT1-CSE-NONCOMMUTATIVE-PIN.
//   * SIDE-EFFECT EXCLUSION: two identical Stores (same value, same
//     addr) do NOT merge — both must execute (Store is side-effecting).
//   * LOAD ADMISSION (cycle 10b): two Loads from the same Alloca with
//     no may-aliasing Store between them DO merge. An intervening
//     aliasing Store (same Alloca) blocks; a non-aliasing Store
//     (distinct Alloca) does not.
//   * VOLATILE EXCLUSION: a Volatile-flagged binary op does NOT
//     merge with a non-volatile equivalent.
//   * DOMINANCE SCOPING: an expression defined only in one arm of
//     a diamond is NOT available at the join (the arm-scoped entry
//     is rolled back at Leave). Without this rollback, CSE'ing a
//     join-block expression that "matches" a then-arm entry would
//     reference a value that doesn't dominate the join — invalid SSA.
//   * MULTI-FUNCTION: per-function reset.
//   * RUNTIME-INIT CARVE-OUT: parity with the other passes.
//
// The dead duplicates remain in the rebuild (DCE sweeps next).

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/passes/cse.hpp"

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

// Same block: two Add(c, c) → CSE the second; only one survives
// effective use; both physically remain (DCE sweeps next).
TEST(Cse, SameBlockArithmeticCsed) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirLiteralValue v; v.value = std::int64_t{5}; v.core = TypeKind::I32;
    MirInstId const c = mb.addConst(v, i32);
    MirInstId const ops1[] = {c, c};
    MirInstId const add1 = mb.addInst(MirOpcode::Add, ops1, i32);
    MirInstId const ops2[] = {c, c};
    MirInstId const add2 = mb.addInst(MirOpcode::Add, ops2, i32);
    // Use both add results so SSA def-use is well-formed.
    MirInstId const opsSum[] = {add1, add2};
    MirInstId const sum = mb.addInst(MirOpcode::Add, opsSum, i32);
    mb.addReturn(sum);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    // Add(c,c) appears twice → second CSE'd. Add(add1, add2) is
    // distinct (different operands) and stays.
    EXPECT_EQ(r.instructionsCsed, 1u);
}

// Commutative-canonicalization: Add(a,b) and Add(b,a) → merge.
TEST(Cse, CommutativeOperandsCanonicalized) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const params[] = {i32, i32};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const a = mb.addArg(0, i32);
    MirInstId const b = mb.addArg(1, i32);
    MirInstId const ops_ab[] = {a, b};
    MirInstId const add_ab = mb.addInst(MirOpcode::Add, ops_ab, i32);
    MirInstId const ops_ba[] = {b, a};  // swapped — still commutative-equal
    MirInstId const add_ba = mb.addInst(MirOpcode::Add, ops_ba, i32);
    MirInstId const sum[] = {add_ab, add_ba};
    MirInstId const result = mb.addInst(MirOpcode::Add, sum, i32);
    mb.addReturn(result);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsCsed, 1u)
        << "Add(a,b) and Add(b,a) must canonicalize to the same key";
}

// D-OPT1-CSE-NONCOMMUTATIVE-PIN — the load-bearing rigor pin.
// Sub(a, b) and Sub(b, a) MUST NOT merge.
TEST(Cse, NonCommutativeSubtractionNotMerged) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const params[] = {i32, i32};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const a = mb.addArg(0, i32);
    MirInstId const b = mb.addArg(1, i32);
    MirInstId const ops_ab[] = {a, b};
    MirInstId const sub_ab = mb.addInst(MirOpcode::Sub, ops_ab, i32);
    MirInstId const ops_ba[] = {b, a};
    MirInstId const sub_ba = mb.addInst(MirOpcode::Sub, ops_ba, i32);
    MirInstId const sum[] = {sub_ab, sub_ba};
    MirInstId const result = mb.addInst(MirOpcode::Add, sum, i32);
    mb.addReturn(result);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsCsed, 0u)
        << "Sub(a,b) and Sub(b,a) are distinct expressions — must NOT "
           "merge (D-OPT1-CSE-NONCOMMUTATIVE-PIN)";
}

// Side-effect exclusion: two identical Stores must not merge.
TEST(Cse, SideEffectingStoreNotCsed) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v; v.value = std::int64_t{42}; v.core = TypeKind::I32;
    MirInstId const c = mb.addConst(v, i32);
    MirInstId const s1[] = {c, slot};
    (void)mb.addInst(MirOpcode::Store, s1, InvalidType);
    MirInstId const s2[] = {c, slot};
    (void)mb.addInst(MirOpcode::Store, s2, InvalidType);
    mb.addReturn();
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsCsed, 0u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Store), 2u);
}

// Load admission positive pin: two identical Loads with no may-aliasing
// Store between them collapse (D-OPT-LOAD-ALIAS-ANALYSIS).
TEST(Cse, LoadCsedAcrossNoStore) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v; v.value = std::int64_t{0}; v.core = TypeKind::I32;
    MirInstId const c = mb.addConst(v, i32);
    MirInstId const s[] = {c, slot};
    (void)mb.addInst(MirOpcode::Store, s, InvalidType);
    MirInstId const lops[] = {slot};
    MirInstId const ld1 = mb.addInst(MirOpcode::Load, lops, i32);
    MirInstId const ld2 = mb.addInst(MirOpcode::Load, lops, i32);
    MirInstId const sum[] = {ld1, ld2};
    MirInstId const r = mb.addInst(MirOpcode::Add, sum, i32);
    mb.addReturn(r);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    // CSE marks ld2 → ld1; the dead duplicate stays in the rebuild
    // (DCE sweeps next). instructionsCsed is the load-bearing signal.
    EXPECT_EQ(res.instructionsCsed, 1u);
}

// ── TF-C58 (D-OPT-CSE-LOAD-BACKEDGE-TAIL) ────────────────────────────
// PRIMARY red-on-disable pin. A loop body's Load must NOT be CSE'd against a
// PREHEADER Load when the body STORES to the same location AFTER that Load:
// execution wraps through the back edge, so the tail Store clobbers the NEXT
// iteration's Load. The three original slices (canonical tail / strictly-between
// / use head) never scan the use block's TAIL, which is exactly this store.
// Deliberately a SELF-LOOP so the strictly-between region is EMPTY and ONLY the
// wrap-around slice can refuse the CSE.
// RED-ON-DISABLE: delete slice (d) in cse.cpp → instructionsCsed becomes 1.
// This is the shape that silently miscompiled sqlite's balance_nonroot copy loop
// (the loop-carried pointer never advanced → page overrun → select1.test SEGV).
TEST(Cse, LoadNotCsedFromPreheaderIntoLoopBodyTailStore) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const body  = mb.createBlock();
    MirBlockId const exit  = mb.createBlock();

    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue z; z.value = std::int64_t{0}; z.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(z, i32);
    MirInstId const st0[] = {c0, slot};
    (void)mb.addInst(MirOpcode::Store, st0, InvalidType);
    MirInstId const lops[] = {slot};
    MirInstId const ldPre = mb.addInst(MirOpcode::Load, lops, i32);   // canonical
    mb.addBr(body);

    mb.beginBlock(body);
    MirInstId const ldBody = mb.addInst(MirOpcode::Load, lops, i32);  // candidate
    MirLiteralValue one; one.value = std::int64_t{1}; one.core = TypeKind::I32;
    MirInstId const c1 = mb.addConst(one, i32);
    MirInstId const inc[] = {ldBody, c1};
    MirInstId const next = mb.addInst(MirOpcode::Add, inc, i32);
    MirInstId const stTail[] = {next, slot};
    (void)mb.addInst(MirOpcode::Store, stTail, InvalidType);          // TAIL store
    MirInstId const cmp[] = {next, ldPre};
    MirInstId const cond = mb.addInst(MirOpcode::ICmpSlt, cmp, boolT);
    mb.addCondBr(cond, body, exit);                                   // SELF-LOOP

    mb.beginBlock(exit);
    mb.addReturn(ldPre);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.instructionsCsed, 0u)
        << "the body Load must NOT be CSE'd against the preheader Load — the "
           "body's TAIL Store clobbers it on the next iteration via the back edge";
}

// TF-C58 PRECISION pin (counterpart to the red-on-disable above). When the
// canonical AND the candidate sit in the SAME self-looping block with the Store
// AFTER both, the CSE is SOUND and must STILL happen: a block is entered only at
// index 0, so any wrap re-executes the canonical first and refreshes the value.
// This guards against "fixing" the bug by widening the same-block scan to the
// whole block, which would kill a provably-sound class.
TEST(Cse, LoadCsedSameBlockInLoopDespiteTailStore) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const body  = mb.createBlock();
    MirBlockId const exit  = mb.createBlock();

    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue z; z.value = std::int64_t{0}; z.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(z, i32);
    MirInstId const st0[] = {c0, slot};
    (void)mb.addInst(MirOpcode::Store, st0, InvalidType);
    mb.addBr(body);

    mb.beginBlock(body);
    MirInstId const lops[] = {slot};
    MirInstId const ld1 = mb.addInst(MirOpcode::Load, lops, i32);   // canonical
    MirInstId const ld2 = mb.addInst(MirOpcode::Load, lops, i32);   // candidate
    MirInstId const sum[] = {ld1, ld2};
    MirInstId const acc = mb.addInst(MirOpcode::Add, sum, i32);
    MirInstId const stTail[] = {acc, slot};
    (void)mb.addInst(MirOpcode::Store, stTail, InvalidType);        // store AFTER both
    MirLiteralValue lim; lim.value = std::int64_t{9}; lim.core = TypeKind::I32;
    MirInstId const c9 = mb.addConst(lim, i32);
    MirInstId const cmp[] = {acc, c9};
    MirInstId const cond = mb.addInst(MirOpcode::ICmpSlt, cmp, boolT);
    mb.addCondBr(cond, body, exit);                                 // SELF-LOOP

    mb.beginBlock(exit);
    mb.addReturn(c0);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.instructionsCsed, 1u)
        << "same-block canonical+candidate with the Store after BOTH is sound: a "
           "wrap re-executes the canonical first, so this CSE must still happen";
}

// TF-C58 MULTI-BLOCK red-on-disable (the shape that ACTUALLY miscompiled sqlite;
// found missing by the independent code-audit). The self-loop test above returns
// from `blockReachesItselfAvoiding` on the FIRST successor, so the worklist, the
// `seen` marking and the step-cap are never executed there — gutting the walk's
// loop to `return false` leaves that test GREEN while the real bug returns. Here
// the loop is entry → body → latch → body, so reaching `body` from `body`
// REQUIRES popping `latch` off the worklist.
// RED-ON-DISABLE: gut the `while (!work.empty())` walk → instructionsCsed 0 → 1.
TEST(Cse, LoadNotCsedFromPreheaderIntoMultiBlockLoopTailStore) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const body  = mb.createBlock();
    MirBlockId const latch = mb.createBlock();
    MirBlockId const exit  = mb.createBlock();

    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue z; z.value = std::int64_t{0}; z.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(z, i32);
    MirInstId const st0[] = {c0, slot};
    (void)mb.addInst(MirOpcode::Store, st0, InvalidType);
    MirInstId const lops[] = {slot};
    MirInstId const ldPre = mb.addInst(MirOpcode::Load, lops, i32);   // canonical
    mb.addBr(body);

    mb.beginBlock(body);
    MirInstId const ldBody = mb.addInst(MirOpcode::Load, lops, i32);  // candidate
    MirLiteralValue one; one.value = std::int64_t{1}; one.core = TypeKind::I32;
    MirInstId const c1 = mb.addConst(one, i32);
    MirInstId const inc[] = {ldBody, c1};
    MirInstId const next = mb.addInst(MirOpcode::Add, inc, i32);
    MirInstId const stTail[] = {next, slot};
    (void)mb.addInst(MirOpcode::Store, stTail, InvalidType);          // TAIL store
    mb.addBr(latch);

    mb.beginBlock(latch);                                             // no clobber
    MirInstId const cmp[] = {next, ldPre};
    MirInstId const cond = mb.addInst(MirOpcode::ICmpSlt, cmp, boolT);
    mb.addCondBr(cond, body, exit);                                   // BACK EDGE

    mb.beginBlock(exit);
    mb.addReturn(ldPre);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.instructionsCsed, 0u)
        << "the body Load must NOT be CSE'd against the preheader Load — the wrap "
           "body→latch→body carries the tail Store into the next iteration";
}

// TF-C58 PRECISION pin for the `avoid` parameter (found missing by the code-audit).
// Canonical in the loop HEADER, candidate + tail Store in the body: every wrap from
// the body back to itself passes through the HEADER, which RE-EXECUTES the canonical
// and refreshes the value — so this CSE is SOUND and must be admitted. That is
// exactly what `avoid = canonicalBlock` encodes.
// RED-ON-DISABLE for the precision half: delete the `avoid` skip in
// `blockReachesItselfAvoiding` → the walk finds body→header→body → refuses → 1 → 0.
// Without this pin, "simplifying" the primitive to blockReachesItself(B) would
// silently degrade Load CSE to "never across any later clobber", invisibly.
TEST(Cse, LoadCsedFromLoopHeaderIntoBodyDespiteTailStore) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry  = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = mb.createBlock();
    MirBlockId const body   = mb.createBlock();
    MirBlockId const exit   = mb.createBlock();

    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue z; z.value = std::int64_t{0}; z.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(z, i32);
    MirInstId const st0[] = {c0, slot};
    (void)mb.addInst(MirOpcode::Store, st0, InvalidType);
    mb.addBr(header);

    mb.beginBlock(header);
    MirInstId const lops[] = {slot};
    MirInstId const ldH = mb.addInst(MirOpcode::Load, lops, i32);     // canonical
    mb.addBr(body);

    mb.beginBlock(body);
    MirInstId const ldB = mb.addInst(MirOpcode::Load, lops, i32);     // candidate
    MirLiteralValue one; one.value = std::int64_t{1}; one.core = TypeKind::I32;
    MirInstId const c1 = mb.addConst(one, i32);
    MirInstId const inc[] = {ldB, c1};
    MirInstId const next = mb.addInst(MirOpcode::Add, inc, i32);
    MirInstId const stTail[] = {next, slot};
    (void)mb.addInst(MirOpcode::Store, stTail, InvalidType);          // TAIL store
    MirInstId const cmp[] = {next, ldH};
    MirInstId const cond = mb.addInst(MirOpcode::ICmpSlt, cmp, boolT);
    mb.addCondBr(cond, header, exit);                        // wraps VIA the header

    mb.beginBlock(exit);
    mb.addReturn(ldH);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.instructionsCsed, 1u)
        << "every wrap re-enters the canonical's own block (the header), which "
           "re-executes the canonical Load — this CSE is sound and must be kept";
}

// Negative pin: a may-aliasing Store between two identical Loads
// blocks CSE. The Store writes through the SAME Alloca pointer the
// Loads read — Rule 1 (same SSA) says Yes, so admission MUST refuse.
TEST(Cse, LoadNotCsedAcrossAliasingStore) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(v0, i32);
    MirInstId const s0[] = {c0, slot};
    (void)mb.addInst(MirOpcode::Store, s0, InvalidType);
    MirInstId const lops[] = {slot};
    MirInstId const ld1 = mb.addInst(MirOpcode::Load, lops, i32);
    MirLiteralValue v1; v1.value = std::int64_t{1}; v1.core = TypeKind::I32;
    MirInstId const c1 = mb.addConst(v1, i32);
    MirInstId const s1[] = {c1, slot};
    (void)mb.addInst(MirOpcode::Store, s1, InvalidType);
    MirInstId const ld2 = mb.addInst(MirOpcode::Load, lops, i32);
    MirInstId const sum[] = {ld1, ld2};
    MirInstId const r = mb.addInst(MirOpcode::Add, sum, i32);
    mb.addReturn(r);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.instructionsCsed, 0u)
        << "intervening aliasing Store must block Load CSE";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Load), 2u);
}

// c113 (D-CSUBSET-INTRINSIC-BARRIER, audit-F1): a CompilerBarrier between
// two identical Loads blocks CSE — the barrier is a full memory clobber
// (an MSVC _ReadWriteBarrier forbids ANY memory motion across it), keyed
// on the `opcodeClobbersMemory` positive list consumed by the shared
// `mirInstClobbersLoadPtr` predicate (this SAME-BLOCK shape exercises the
// CSE in-block slice, not the cross-block region walker — both funnel
// through the one predicate). RED-on-disable: drop CompilerBarrier from
// opcodeClobbersMemory (or bypass the predicate in the in-block slice) →
// the barrier is skipped (it is not a Store) → the second Load is
// CSE-reused ACROSS the fence → instructionsCsed becomes 1 and this fails.
TEST(Cse, LoadNotCsedAcrossCompilerBarrier) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v0; v0.value = std::int64_t{7}; v0.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(v0, i32);
    MirInstId const s0[] = {c0, slot};
    (void)mb.addInst(MirOpcode::Store, s0, InvalidType);
    MirInstId const lops[] = {slot};
    MirInstId const ld1 = mb.addInst(MirOpcode::Load, lops, i32);
    (void)mb.addInst(MirOpcode::CompilerBarrier, {}, InvalidType);
    MirInstId const ld2 = mb.addInst(MirOpcode::Load, lops, i32);
    MirInstId const sum[] = {ld1, ld2};
    MirInstId const r = mb.addInst(MirOpcode::Add, sum, i32);
    mb.addReturn(r);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.instructionsCsed, 0u)
        << "an intervening CompilerBarrier must block Load CSE — the "
           "fence is a full memory clobber";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Load), 2u);
    // The barrier itself must survive (side-effecting — never CSE'd).
    EXPECT_EQ(countOpInModule(mir, MirOpcode::CompilerBarrier), 1u);
}

// c115 SEH (D-WIN64-SEH-FUNCLETS): a SehTryEnd marker (a region boundary) is
// a full memory clobber — a Load may not be CSE-reused across it, since at c116
// the guarded body's memory state at the boundary is what the fault-time
// filter/handler observe. Same `opcodeClobbersMemory` chokepoint as the
// CompilerBarrier pin above (SehTryBegin/SehTryEnd/SehFilterReturn are all on
// the positive list). RED-on-disable: drop SehTryEnd from opcodeClobbersMemory
// → the second Load CSE-reuses across the boundary → instructionsCsed = 1.
TEST(Cse, LoadNotCsedAcrossSehTryEnd) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v0; v0.value = std::int64_t{7}; v0.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(v0, i32);
    MirInstId const s0[] = {c0, slot};
    (void)mb.addInst(MirOpcode::Store, s0, InvalidType);
    MirInstId const lops[] = {slot};
    MirInstId const ld1 = mb.addInst(MirOpcode::Load, lops, i32);
    (void)mb.addInst(MirOpcode::SehTryEnd, {}, InvalidType, /*payload=*/0);
    MirInstId const ld2 = mb.addInst(MirOpcode::Load, lops, i32);
    MirInstId const sum[] = {ld1, ld2};
    MirInstId const r = mb.addInst(MirOpcode::Add, sum, i32);
    mb.addReturn(r);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.instructionsCsed, 0u)
        << "a SehTryEnd region boundary must block Load CSE";
    EXPECT_EQ(countOpInModule(mir, MirOpcode::Load), 2u);
    EXPECT_EQ(countOpInModule(mir, MirOpcode::SehTryEnd), 1u);
}

// Positive pin (SSA-derivable Rule 2): an intervening Store to a
// DISTINCT Alloca does NOT alias the Loaded pointer, so CSE proceeds.
TEST(Cse, LoadCsedAcrossDistinctAllocaStore) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const slotA = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirInstId const slotB = mb.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(v0, i32);
    MirInstId const sA0[] = {c0, slotA}; (void)mb.addInst(MirOpcode::Store, sA0, InvalidType);
    MirInstId const sB0[] = {c0, slotB}; (void)mb.addInst(MirOpcode::Store, sB0, InvalidType);
    MirInstId const lopsA[] = {slotA};
    MirInstId const ldA1 = mb.addInst(MirOpcode::Load, lopsA, i32);
    MirLiteralValue v1; v1.value = std::int64_t{1}; v1.core = TypeKind::I32;
    MirInstId const c1 = mb.addConst(v1, i32);
    MirInstId const sB1[] = {c1, slotB};
    (void)mb.addInst(MirOpcode::Store, sB1, InvalidType);
    MirInstId const ldA2 = mb.addInst(MirOpcode::Load, lopsA, i32);
    MirInstId const sum[] = {ldA1, ldA2};
    MirInstId const r = mb.addInst(MirOpcode::Add, sum, i32);
    mb.addReturn(r);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.instructionsCsed, 1u)
        << "Store to distinct Alloca must not block Load CSE on a different Alloca";
}

// Strict-TBAA precision pin: under MirAliasingMode::StrictTBAA, a Store
// through a Ptr<I64> cannot alias a Load through Ptr<I32> (Rule 5 in
// mirMayAlias). Pointers are Args (not Allocas), so Rule 2 doesn't
// preempt — this is the only path that exercises Rule 5 in CSE.
// Closes D-OPT-LOAD-ALIAS-ANALYSIS-STRICT-TBAA-WIRING.
TEST(Cse, LoadCsedAcrossDistinctPrimitiveStoreUnderStrictTBAA) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32     = interner.primitive(TypeKind::I32);
    TypeId const i64     = interner.primitive(TypeKind::I64);
    TypeId const ptrI32  = interner.pointer(i32);
    TypeId const ptrI64  = interner.pointer(i64);
    TypeId const params[] = {ptrI32, ptrI64};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.setAliasingMode(MirAliasingMode::StrictTBAA);
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const pI32 = mb.addArg(0, ptrI32);
    MirInstId const pI64 = mb.addArg(1, ptrI64);
    MirInstId const lops[] = {pI32};
    MirInstId const ld1 = mb.addInst(MirOpcode::Load, lops, i32);
    // Intervening Store through Ptr<I64> — strict-TBAA says this
    // cannot alias the I32 Load.
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I64;
    MirInstId const c0 = mb.addConst(v0, i64);
    MirInstId const sOps[] = {c0, pI64};
    (void)mb.addInst(MirOpcode::Store, sOps, InvalidType);
    MirInstId const ld2 = mb.addInst(MirOpcode::Load, lops, i32);
    MirInstId const sum[] = {ld1, ld2};
    MirInstId const r = mb.addInst(MirOpcode::Add, sum, i32);
    mb.addReturn(r);
    Mir mir = std::move(mb).finish();

    // Flag-state attribution pin: if a future MirBuilder regression
    // drops `setAliasingMode` (e.g. move-ctor reset bug), this
    // assertion fails BEFORE the CSE counter — making the diagnostic
    // path point at the mode-threading, not the alias predicate.
    ASSERT_EQ(mir.aliasingMode(), MirAliasingMode::StrictTBAA);

    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.instructionsCsed, 1u)
        << "strict-TBAA: Store<I64> cannot alias Load<I32>; CSE must admit";
}

// Negative polarity: same fixture under MirAliasingMode::Permissive
// (the default). Rule 5 doesn't fire, conservative Rule 6 returns
// Maybe, CSE refuses. Pins the strict-TBAA flag is actually being
// read at the consumer site (a regression that hardcodes
// StrictTbaa::No would PASS this negative but FAIL the positive
// above — together they make the wiring testable).
TEST(Cse, LoadNotCsedAcrossDistinctPrimitiveStoreUnderPermissive) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32     = interner.primitive(TypeKind::I32);
    TypeId const i64     = interner.primitive(TypeKind::I64);
    TypeId const ptrI32  = interner.pointer(i32);
    TypeId const ptrI64  = interner.pointer(i64);
    TypeId const params[] = {ptrI32, ptrI64};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    // Explicit Permissive (not relying on default) so a future flip
    // of the default can't silently make this test vacuous.
    mb.setAliasingMode(MirAliasingMode::Permissive);
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const pI32 = mb.addArg(0, ptrI32);
    MirInstId const pI64 = mb.addArg(1, ptrI64);
    MirInstId const lops[] = {pI32};
    MirInstId const ld1 = mb.addInst(MirOpcode::Load, lops, i32);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I64;
    MirInstId const c0 = mb.addConst(v0, i64);
    MirInstId const sOps[] = {c0, pI64};
    (void)mb.addInst(MirOpcode::Store, sOps, InvalidType);
    MirInstId const ld2 = mb.addInst(MirOpcode::Load, lops, i32);
    MirInstId const sum[] = {ld1, ld2};
    MirInstId const r = mb.addInst(MirOpcode::Add, sum, i32);
    mb.addReturn(r);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.instructionsCsed, 0u)
        << "Permissive: distinct primitive pointees stay Maybe; CSE refuses";
}

// D-OPT-MIR-ALIAS-CHAR-EXCEPTION-OVERRIDE end-to-end pin (consumer side):
// proves the CsePolicy ctor reads Mir.charTypesAliasAll() and threads
// it to mirMayAlias. Without this, a regression dropping the
// charTypesAliasAll_ cache member or passing the wrong field value
// would leave the predicate-level test green but the consumer broken.
//
// Fixture: Load through Ptr<Char>, Store through Ptr<I32>, second
// Load through Ptr<Char>. Under default (charTypesAliasAll=true)
// strict-TBAA — Rule 5 char-exception fires → Maybe → CSE refuses.
// Under charTypesAliasAll=false strict-TBAA — Rule 5 is bypassed,
// Rule 6 distinguishes Char vs I32 → No → CSE admits.
TEST(Cse, LoadCsedAcrossDistinctPrimitiveStoreUnderStrictTBAANoCharException) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const charT  = interner.primitive(TypeKind::Char);
    TypeId const ptrCh  = interner.pointer(charT);
    TypeId const ptrI32 = interner.pointer(i32);
    TypeId const params[] = {ptrCh, ptrI32};
    TypeId const fnSig = interner.fnSig(params, charT, CallConv::CcSysV);

    MirBuilder mb;
    mb.setAliasingMode(MirAliasingMode::StrictTBAA);
    mb.setCharTypesAliasAll(false);  // Rust-like / strict-typed DSL
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const pCh  = mb.addArg(0, ptrCh);
    MirInstId const pI32 = mb.addArg(1, ptrI32);
    MirInstId const lops[] = {pCh};
    MirInstId const ld1 = mb.addInst(MirOpcode::Load, lops, charT);
    // Intervening Store through Ptr<I32> — under StrictTBAA + char-
    // exception-disabled, cannot alias the Char Load.
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(v0, i32);
    MirInstId const sOps[] = {c0, pI32};
    (void)mb.addInst(MirOpcode::Store, sOps, InvalidType);
    MirInstId const ld2 = mb.addInst(MirOpcode::Load, lops, charT);
    mb.addReturn(ld2);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    // Flag-state attribution pin: both flags must round-trip through
    // finish() — a regression in either Mir field threading would
    // attribute the failure here, not in the alias predicate.
    ASSERT_EQ(mir.aliasingMode(), MirAliasingMode::StrictTBAA);
    ASSERT_FALSE(mir.charTypesAliasAll());

    EXPECT_EQ(res.instructionsCsed, 1u)
        << "strict + char-exception-disabled: Store<I32> cannot alias "
           "Load<Char>; CSE must admit";
}

// Negative polarity: same fixture under default charTypesAliasAll=true.
// Rule 5 char-exception fires → Maybe → CSE refuses. The pair (positive
// above + this negative) proves the CsePolicy reads + threads the flag.
TEST(Cse, LoadNotCsedAcrossDistinctPrimitiveStoreUnderStrictTBAAWithCharException) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const charT  = interner.primitive(TypeKind::Char);
    TypeId const ptrCh  = interner.pointer(charT);
    TypeId const ptrI32 = interner.pointer(i32);
    TypeId const params[] = {ptrCh, ptrI32};
    TypeId const fnSig = interner.fnSig(params, charT, CallConv::CcSysV);

    MirBuilder mb;
    mb.setAliasingMode(MirAliasingMode::StrictTBAA);
    mb.setCharTypesAliasAll(true);  // C/C++/ObjC default — explicit
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const pCh  = mb.addArg(0, ptrCh);
    MirInstId const pI32 = mb.addArg(1, ptrI32);
    MirInstId const lops[] = {pCh};
    MirInstId const ld1 = mb.addInst(MirOpcode::Load, lops, charT);
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(v0, i32);
    MirInstId const sOps[] = {c0, pI32};
    (void)mb.addInst(MirOpcode::Store, sOps, InvalidType);
    MirInstId const ld2 = mb.addInst(MirOpcode::Load, lops, charT);
    mb.addReturn(ld2);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.instructionsCsed, 0u)
        << "strict + char-exception-enabled: char* may alias int*; CSE refuses";
}

// Volatile exclusion: a Volatile-flagged binary op participates in
// neither side of a CSE merge.
TEST(Cse, VolatileBinaryOpNotCsed) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const params[] = {i32, i32};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const a = mb.addArg(0, i32);
    MirInstId const b = mb.addArg(1, i32);
    MirInstId const ops[] = {a, b};
    // First Add is plain.
    MirInstId const add1 = mb.addInst(MirOpcode::Add, ops, i32);
    // Second Add is volatile-flagged.
    MirInstId const add2 = mb.addInst(MirOpcode::Add, ops, i32,
                                       /*payload*/0, MirInstFlags::Volatile);
    MirInstId const sum[] = {add1, add2};
    MirInstId const r = mb.addInst(MirOpcode::Add, sum, i32);
    mb.addReturn(r);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.instructionsCsed, 0u)
        << "Volatile-flagged binary op must not participate in CSE";
}

// Dominance scoping: an expression defined only in the then-arm
// of a diamond must NOT be available at the join. A CSE bug that
// fails to roll back the then-arm's scope on Leave would attempt
// to substitute a join-block use with a value that doesn't
// dominate the join — invalid SSA + silent miscompile.
TEST(Cse, DominanceScopingDiamondNoCrossArmMerge) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT, i32, i32};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tArm  = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const fArm  = mb.createBlock(StructCfMarker::IfElse);
    MirBlockId const join  = mb.createBlock(StructCfMarker::IfJoin);

    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    MirInstId const a = mb.addArg(1, i32);
    MirInstId const b = mb.addArg(2, i32);
    mb.addCondBr(cond, tArm, fArm);

    // then-arm computes a+b; falls to join with phi-incoming.
    mb.beginBlock(tArm);
    MirInstId const ops_then[] = {a, b};
    MirInstId const add_then = mb.addInst(MirOpcode::Add, ops_then, i32);
    mb.addBr(join);

    // else-arm: just a constant.
    mb.beginBlock(fArm);
    MirLiteralValue v; v.value = std::int64_t{0}; v.core = TypeKind::I32;
    MirInstId const zero = mb.addConst(v, i32);
    mb.addBr(join);

    // Join computes a+b AGAIN. Even though syntactically identical to
    // the then-arm's add, that def doesn't dominate the join (the
    // else-arm bypasses it). CSE must NOT merge.
    mb.beginBlock(join);
    MirPhiIncoming const incs[] = {{add_then, tArm}, {zero, fArm}};
    MirInstId const phi = mb.addPhi(i32, incs);
    MirInstId const ops_join[] = {a, b};
    MirInstId const add_join = mb.addInst(MirOpcode::Add, ops_join, i32);
    MirInstId const sum[] = {phi, add_join};
    MirInstId const r = mb.addInst(MirOpcode::Add, sum, i32);
    mb.addReturn(r);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.instructionsCsed, 0u)
        << "then-arm's Add does not dominate the join — must NOT be "
           "available for CSE at join";
}

// Dominance scoping POSITIVE case: an expression in the entry block
// IS available everywhere it dominates. Two Adds with same operands
// at entry and at then-arm → merge (entry dominates then-arm).
TEST(Cse, DominanceScopingEntryDefAvailableInChild) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT, i32, i32};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tArm  = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const fArm  = mb.createBlock(StructCfMarker::IfElse);
    MirBlockId const join  = mb.createBlock(StructCfMarker::IfJoin);

    mb.beginBlock(entry);
    MirInstId const cond = mb.addArg(0, boolT);
    MirInstId const a = mb.addArg(1, i32);
    MirInstId const b = mb.addArg(2, i32);
    MirInstId const ops_entry[] = {a, b};
    MirInstId const add_entry = mb.addInst(MirOpcode::Add, ops_entry, i32);
    mb.addCondBr(cond, tArm, fArm);

    mb.beginBlock(tArm);
    MirInstId const ops_then[] = {a, b};  // identical key → CSE
    MirInstId const add_then = mb.addInst(MirOpcode::Add, ops_then, i32);
    mb.addBr(join);

    mb.beginBlock(fArm);
    mb.addBr(join);

    mb.beginBlock(join);
    MirPhiIncoming const incs[] = {{add_then, tArm}, {add_entry, fArm}};
    MirInstId const phi = mb.addPhi(i32, incs);
    mb.addReturn(phi);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.instructionsCsed, 1u)
        << "entry's Add dominates then-arm — the duplicate must CSE";
}

// Perf-hoist decision-identity pin (the CSE analysis-recomputation fix): the
// cross-block Load-CSE region walker `mirRegionBetween` now receives the caller's
// ONCE-computed whole-module predecessor map and scopes its block-intersection to
// the function-local reachable set (was a per-query whole-module rebuild + scan).
// This pins that the ADMIT/REFUSE decision stays byte-identical PER FUNCTION in a
// MULTI-function module: fn0 has an in-region Store clobber (CSE refused), fn1 has
// none (CSE admitted) → exactly one CSE. A broken scope (leaking another function's
// blocks/preds, or mis-sizing the whole-module preds) would mis-count. Both Loads
// are cross-block (canonical in entry, candidate in join), so both functions
// exercise the rewritten region walker.
TEST(Cse, CrossBlockLoadCseDecidedPerFunctionInMultiFunctionModule) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const params[] = {boolT};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    for (std::uint32_t fnIdx = 0; fnIdx < 2; ++fnIdx) {
        mb.addFunction(fnSig, SymbolId{100u + fnIdx});
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const tArm  = mb.createBlock(StructCfMarker::IfThen);
        MirBlockId const fArm  = mb.createBlock(StructCfMarker::IfElse);
        MirBlockId const join  = mb.createBlock(StructCfMarker::IfJoin);

        mb.beginBlock(entry);
        MirInstId const cond = mb.addArg(0, boolT);
        MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
        MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
        MirInstId const c0 = mb.addConst(v0, i32);
        MirInstId const st0[] = {c0, slot};
        (void)mb.addInst(MirOpcode::Store, st0, InvalidType);
        MirInstId const lops[] = {slot};
        MirInstId const ld1 = mb.addInst(MirOpcode::Load, lops, i32);
        mb.addCondBr(cond, tArm, fArm);

        mb.beginBlock(tArm);
        if (fnIdx == 0) {   // in-region aliasing clobber — function 0 only
            MirLiteralValue v1; v1.value = std::int64_t{1}; v1.core = TypeKind::I32;
            MirInstId const c1 = mb.addConst(v1, i32);
            MirInstId const st1[] = {c1, slot};
            (void)mb.addInst(MirOpcode::Store, st1, InvalidType);
        }
        mb.addBr(join);

        mb.beginBlock(fArm);
        mb.addBr(join);

        mb.beginBlock(join);
        MirInstId const ld2 = mb.addInst(MirOpcode::Load, lops, i32);
        MirInstId const sum[] = {ld1, ld2};
        MirInstId const r = mb.addInst(MirOpcode::Add, sum, i32);
        mb.addReturn(r);
    }
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.instructionsCsed, 1u)
        << "cross-block Load-CSE decided per function: fn0's in-region clobber "
           "refuses, fn1 (clean region) admits — the hoisted whole-module preds "
           "+ function-scoped region walker must not leak across functions";
}

// Multi-function: per-function reset of the cseMap.
TEST(Cse, MultiFunctionModuleEachCsedIndependently) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const params[] = {i32, i32};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    for (std::uint32_t fnIdx = 0; fnIdx < 2; ++fnIdx) {
        mb.addFunction(fnSig, SymbolId{100u + fnIdx});
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(entry);
        MirInstId const a = mb.addArg(0, i32);
        MirInstId const b = mb.addArg(1, i32);
        MirInstId const ops[] = {a, b};
        MirInstId const add1 = mb.addInst(MirOpcode::Add, ops, i32);
        MirInstId const add2 = mb.addInst(MirOpcode::Add, ops, i32);
        MirInstId const sum[] = {add1, add2};
        MirInstId const r = mb.addInst(MirOpcode::Add, sum, i32);
        mb.addReturn(r);
    }
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const res = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.instructionsCsed, 2u)
        << "each function's duplicate Add must CSE independently — "
           "counter accumulates across functions";
}

// Type-discriminator: Add<I32>(a,b) and Add<I64>(a,b) must NOT merge.
// The `CseKey.type` field participates in equality + hash; a
// regression that dropped it would silently fuse different-width
// arithmetic into one (catastrophic miscompile).
TEST(Cse, ResultTypeDiscriminatesKey) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const i64   = interner.primitive(TypeKind::I64);
    TypeId const params[] = {i32, i32};
    TypeId const fnSig = interner.fnSig(params, i64, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const a = mb.addArg(0, i32);
    MirInstId const b = mb.addArg(1, i32);
    MirInstId const ops[] = {a, b};
    // Two Adds with same operands but different result types.
    MirInstId const add32 = mb.addInst(MirOpcode::Add, ops, i32);
    MirInstId const add64 = mb.addInst(MirOpcode::Add, ops, i64);
    (void)add32; (void)add64;
    // Return one of them (don't care which) — just need both alive.
    mb.addReturn(add64);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsCsed, 0u)
        << "different result types must not merge — the type field is "
           "load-bearing in the CSE key";
}

// Transitive CSE chain: Add(a,b) emitted three times. Second merges
// to first via the scope; third's operand-resolution walks
// `cseMap_` (via `resolveTransitive` in `buildKey`) → its key
// canonicalizes against the first def's id too → merges. This
// exercises `resolveTransitive` and path-compression.
TEST(Cse, TransitiveChainResolves) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const params[] = {i32, i32};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const a = mb.addArg(0, i32);
    MirInstId const b = mb.addArg(1, i32);
    MirInstId const ops[] = {a, b};
    MirInstId const add1 = mb.addInst(MirOpcode::Add, ops, i32);
    MirInstId const add2 = mb.addInst(MirOpcode::Add, ops, i32);
    MirInstId const add3 = mb.addInst(MirOpcode::Add, ops, i32);
    // Build a downstream consumer using add2 as an operand. After
    // CSE, add2 → add1; the consumer's operand resolves to add1.
    MirInstId const consumer_ops[] = {add2, add3};
    MirInstId const consumer = mb.addInst(MirOpcode::Add, consumer_ops, i32);
    mb.addReturn(consumer);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsCsed, 2u)
        << "add2 + add3 must both CSE to add1 (operands canonicalize "
           "through cseMap_ — path-compression invariant)";
}

// Runtime-init carve-out: module with `globalInitFunc.valid()` emits
// X_OptPassSkipped Info + returns ok=true + zero counters. Parity
// with ConstFold/Dce/Mem2Reg/CopyProp.
TEST(Cse, RuntimeInitGlobalsModuleEmitsXOptPassSkippedInfo) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const initSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    MirFuncId const initFn = mb.addFunction(initSig, SymbolId{50});
    MirBlockId const initEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(initEntry);
    mb.addReturn();
    mb.addGlobal(i32, SymbolId{200}, UINT32_MAX, initFn,
                 SymbolBinding::Global, SymbolVisibility::Default,
                 /*isConst=*/false, MirThreadStorage::Shared);
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = opt::passes::runCse(mir, interner, rep);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.instructionsCsed, 0u);
    std::size_t infoCount = 0;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::X_OptPassSkipped) ++infoCount;
    }
    EXPECT_EQ(infoCount, 1u);
}
