// MirFunctionRebuilder substrate round-trip tests
// (D-OPT-REBUILDER-IDENTITY-POLICY-TEST, cycle 10g).
//
// Verifies the shared rebuild substrate is bit-identical on the
// identity path: a hand-built MIR module driven through
// `MirFunctionRebuilder` with a no-op `IdentityPolicy` produces a
// rebuilt MIR with the same instCount / blockCount / opcodes.
//
// The existing pass tests (ConstFold / DCE / Mem2Reg / CopyProp / CSE /
// SimplifyCFG / LICM) exercise the substrate transitively; this test
// localizes regressions to the rebuilder vs. any policy by holding the
// policy at identity. A 3-phase-rebuild bug (rewrite-map drop, phi
// flush miswire, terminator emit shape) that the pass tests would
// blame on the consumer pass surfaces here as the rebuilder's own
// breakage.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_cfg.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/passes/mir_rebuild_helper.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace dss;
using namespace dss::opt::passes;

namespace {

// The minimum-effort `MirRebuildPolicy`: every hook keeps its base-class
// default. The single override is `selectBlocks`, which is pure virtual.
// All other hooks land their default-arm behavior, so a rebuild through
// this policy is a pure functional copy of the source function.
class IdentityPolicy final : public MirRebuildPolicy {
public:
    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        return mirReversePostOrder(src, src.funcEntry(fn));
    }
};

// Count instructions of an opcode across the entire module.
std::size_t countOp(Mir const& mir, MirOpcode op) {
    std::size_t n = 0;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t ii = 0; ii < ni; ++ii) {
                if (mir.instOpcode(mir.blockInstAt(b, ii)) == op) ++n;
            }
        }
    }
    return n;
}

// Drive a module through MirFunctionRebuilder + IdentityPolicy. The
// callers pin the input + output opcode counts; this helper is the
// rebuild plumbing they share. `cloneGlobalsOrCarveOut` is called
// first to match the production rebuild prelude.
//
// Cross-compiler portability + fail-loud discipline (cycle 10h
// post-fold, 2026-06-04): the helper returns `[[nodiscard]] bool` +
// writes to an output parameter, NOT a `Mir` return. Two reasons:
//   * `Mir` return is incompatible with internal `ASSERT_*` macros
//     — they expand to `return;` (void), which GCC rejects from a
//     `Mir`-returning function. MSVC accepted the prior shape;
//     cycle 10g CI on Linux/GCC was the catch.
//   * `[[nodiscard]]` (vs plain void) gives compile-time
//     enforcement that callers check the result. A caller that
//     wrote `identityRebuild(src, dst);` without `ASSERT_TRUE(...)`
//     would receive an unused-result warning AND silently proceed
//     past a carve-out failure with a default-constructed `dst`.
//     The nodiscard keeps the silent-failure trap closed.
// Callers: `ASSERT_TRUE(identityRebuild(src, dst));` — the
// out-param is populated only on success.
[[nodiscard]] bool identityRebuild(Mir const& src, Mir& out) {
    MirBuilder dst;
    DiagnosticReporter rep;
    auto const carveOut = cloneGlobalsOrCarveOut(src, dst, rep,
                                                  "IdentityRoundTrip");
    // Fail-loud at the test boundary: caller's ASSERT_TRUE on our
    // return value catches this. Returning false (not asserting
    // internally) keeps the helper compiler-portable + lets the
    // [[nodiscard]] warning surface forgotten-check call sites.
    if (carveOut != GlobalClonePrelude::Cloned) {
        ADD_FAILURE() << "test modules don't use runtime-init globals; "
                         "the carve-out branch shouldn't fire — "
                         "otherwise the IdentityPolicy test is "
                         "silently no-ops";
        return false;
    }
    IdentityPolicy policy;
    std::size_t const nf = src.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = src.funcAt(i);
        MirFunctionRebuilder rb{src, dst, policy};
        rb.rebuildFunction(f);
    }
    out = std::move(dst).finish();
    return true;
}

} // namespace

// Diamond CFG: entry → {thenB, elseB} → joinB → return. CondBr at
// entry, plain Br at thenB and elseB. Phi at joinB merging two i32
// values. Exercises phase 1 (block pre-create), phase 2 (instruction
// emit + terminator emit), and phase 3 (Phi incoming flush via the
// completed rewrite map).
TEST(MirRebuildHelper, IdentityRoundTripPreservesDiamondCfg) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const thenB = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const elseB = mb.createBlock(StructCfMarker::IfElse);
    MirBlockId const joinB = mb.createBlock(StructCfMarker::IfJoin);

    mb.beginBlock(entry);
    MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(tru, boolT);
    mb.addCondBr(cond, thenB, elseB);

    mb.beginBlock(thenB);
    MirLiteralValue v1; v1.value = std::int64_t{1}; v1.core = TypeKind::I32;
    MirInstId const c1 = mb.addConst(v1, i32);
    mb.addBr(joinB);

    mb.beginBlock(elseB);
    MirLiteralValue v2; v2.value = std::int64_t{2}; v2.core = TypeKind::I32;
    MirInstId const c2 = mb.addConst(v2, i32);
    mb.addBr(joinB);

    mb.beginBlock(joinB);
    MirPhiIncoming inc[] = {{c1, thenB}, {c2, elseB}};
    MirInstId const phi = mb.addPhi(i32, inc);
    mb.addReturn(phi);
    Mir src = std::move(mb).finish();

    std::size_t const srcInstCount   = src.instCount();
    std::size_t const srcBlockCount  = src.blockCount();
    std::size_t const srcConstCount  = countOp(src, MirOpcode::Const);
    std::size_t const srcPhiCount    = countOp(src, MirOpcode::Phi);
    std::size_t const srcCondBrCount = countOp(src, MirOpcode::CondBr);
    std::size_t const srcBrCount     = countOp(src, MirOpcode::Br);
    std::size_t const srcReturnCount = countOp(src, MirOpcode::Return);

    Mir dst;
    ASSERT_TRUE(identityRebuild(src, dst));

    EXPECT_EQ(dst.instCount(),  srcInstCount);
    EXPECT_EQ(dst.blockCount(), srcBlockCount);
    EXPECT_EQ(countOp(dst, MirOpcode::Const),  srcConstCount);
    EXPECT_EQ(countOp(dst, MirOpcode::Phi),    srcPhiCount);
    EXPECT_EQ(countOp(dst, MirOpcode::CondBr), srcCondBrCount);
    EXPECT_EQ(countOp(dst, MirOpcode::Br),     srcBrCount);
    EXPECT_EQ(countOp(dst, MirOpcode::Return), srcReturnCount);
}

// FC7 C1c — the rebuild substrate's `Return` clone must preserve EVERY operand.
// A by-value struct returned IN REGISTERS lowers to a MULTI-operand `Return` (one
// per eightbyte / HFA piece); the clone previously mapped only `oldOps[0]`,
// silently DROPPING pieces 1..N-1. That truncation was a HIGH silent miscompile,
// masked on x86_64 (the dropped piece's value often still aliased its arg register
// at the return register — a 3rd field passed in rdx == returnGprs[1]) and exposed
// only on AAPCS64's distinct arg/return mapping. RED-ON-DISABLE: revert the helper's
// Return clone to `addReturn(mapOperand(oldOps[0]))` and the rebuilt Return drops
// from 2 operands to 1 here (and `IdentityRoundTripPreservesDiamondCfg`'s scalar
// Return — 1 operand — stays green, so this is the isolated multi-piece lever).
TEST(MirRebuildHelper, IdentityRoundTripPreservesMultiPieceReturnAllOperands) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i64 = interner.primitive(TypeKind::I64);
    // A 2-eightbyte (SysV) / 2-GPR (AAPCS64) struct return lowers to a Return
    // carrying TWO I64 register pieces. The rebuild substrate runs no verifier, so
    // the fnSig return type is metadata here — the operand-count round-trip is what
    // this pins (the verifier's own multi-piece acceptance is covered by the
    // struct-return corpus + Return's maxOperands=N descriptor).
    TypeId const fnSig = interner.fnSig({}, i64, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{200});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirLiteralValue p0; p0.value = std::int64_t{11}; p0.core = TypeKind::I64;
    MirLiteralValue p1; p1.value = std::int64_t{22}; p1.core = TypeKind::I64;
    MirInstId const v0 = mb.addConst(p0, i64);
    MirInstId const v1 = mb.addConst(p1, i64);
    MirInstId const pieces[] = {v0, v1};
    mb.addReturnMulti(pieces);
    Mir src = std::move(mb).finish();

    // Sanity: the source Return carries BOTH pieces.
    auto findReturnOperandCount = [](Mir const& m) -> std::size_t {
        std::size_t const nf = m.moduleFuncCount();
        for (std::uint32_t fi = 0; fi < nf; ++fi) {
            MirFuncId const f = m.funcAt(fi);
            std::uint32_t const nb = m.funcBlockCount(f);
            for (std::uint32_t bi = 0; bi < nb; ++bi) {
                MirBlockId const b = m.funcBlockAt(f, bi);
                std::uint32_t const ni = m.blockInstCount(b);
                for (std::uint32_t ii = 0; ii < ni; ++ii) {
                    MirInstId const id = m.blockInstAt(b, ii);
                    if (m.instOpcode(id) == MirOpcode::Return)
                        return m.instOperands(id).size();
                }
            }
        }
        return 0;
    };
    ASSERT_EQ(findReturnOperandCount(src), 2u);

    Mir dst;
    ASSERT_TRUE(identityRebuild(src, dst));

    // The crux: the rebuilt Return MUST still carry BOTH pieces. A single-operand
    // clone would drop piece 1 → 1 here (the silent miscompile).
    EXPECT_EQ(findReturnOperandCount(dst), 2u)
        << "the rebuild substrate dropped a return-register piece — multi-piece "
           "struct returns would silently lose every field past the first";
    EXPECT_EQ(countOp(dst, MirOpcode::Return), 1u);
}

// Multi-block straight-line with GlobalAddr + Load + Store + Add +
// Return. Exercises terminator dispatch on Br (no-operand) and Return
// (one-operand), plus GlobalAddr's payload threading + the operand-
// pool round-trip on the binary-op + Store shape.
TEST(MirRebuildHelper, IdentityRoundTripPreservesGlobalAddrLoadStoreReturn) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
    std::uint32_t const initIdx = mb.literalPoolAdd(v0);
    (void)mb.addGlobal(i32, SymbolId{200}, initIdx);

    mb.addFunction(fnSig, SymbolId{201});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const body  = mb.createBlock(StructCfMarker::Linear);

    mb.beginBlock(entry);
    MirInstId const gAddr = mb.addGlobalAddr(SymbolId{200}, ptr);
    mb.addBr(body);

    mb.beginBlock(body);
    MirInstId const lops[] = {gAddr};
    MirInstId const ld = mb.addInst(MirOpcode::Load, lops, i32);
    MirLiteralValue v7; v7.value = std::int64_t{7}; v7.core = TypeKind::I32;
    MirInstId const c7 = mb.addConst(v7, i32);
    MirInstId const addOps[] = {ld, c7};
    MirInstId const sum = mb.addInst(MirOpcode::Add, addOps, i32);
    MirInstId const storeOps[] = {sum, gAddr};
    (void)mb.addInst(MirOpcode::Store, storeOps, InvalidType);
    mb.addReturn(sum);
    Mir src = std::move(mb).finish();

    std::size_t const srcInstCount        = src.instCount();
    std::size_t const srcBlockCount       = src.blockCount();
    std::size_t const srcGlobalCount      = src.moduleGlobalCount();
    std::size_t const srcGlobalAddrCount  = countOp(src, MirOpcode::GlobalAddr);
    std::size_t const srcLoadCount        = countOp(src, MirOpcode::Load);
    std::size_t const srcStoreCount       = countOp(src, MirOpcode::Store);
    std::size_t const srcAddCount         = countOp(src, MirOpcode::Add);
    std::size_t const srcBrCount          = countOp(src, MirOpcode::Br);
    std::size_t const srcReturnCount      = countOp(src, MirOpcode::Return);

    Mir dst;
    ASSERT_TRUE(identityRebuild(src, dst));

    EXPECT_EQ(dst.instCount(),         srcInstCount);
    EXPECT_EQ(dst.blockCount(),        srcBlockCount);
    EXPECT_EQ(dst.moduleGlobalCount(), srcGlobalCount);
    EXPECT_EQ(countOp(dst, MirOpcode::GlobalAddr), srcGlobalAddrCount);
    EXPECT_EQ(countOp(dst, MirOpcode::Load),       srcLoadCount);
    EXPECT_EQ(countOp(dst, MirOpcode::Store),      srcStoreCount);
    EXPECT_EQ(countOp(dst, MirOpcode::Add),        srcAddCount);
    EXPECT_EQ(countOp(dst, MirOpcode::Br),         srcBrCount);
    EXPECT_EQ(countOp(dst, MirOpcode::Return),     srcReturnCount);
}

// const-ness preservation across the shared rebuild global-clone fns
// (D-LK4-DATA-PRODUCER-MUTABLE-GLOBAL). `MirGlobal.isConst` drives the emitted
// data section — a CONST global lands in read-only `.rodata`, a MUTABLE one in
// writable `.data` (asm.cpp). EVERY pass that rebuilds the module's globals must
// carry the bit, or a const global silently degrades to a writable section under
// optimization (loss of the read-only-memory protection). This pins the two SHARED
// rebuild clone fns (`cloneGlobalsVerbatim` — the prune/normalize chokepoint; and
// `cloneGlobalsOrCarveOut` — the rebuild-pass prelude used by the MirFunctionRebuilder
// substrate). The OTHER two copy sites have their own standalone loops, pinned
// separately: DCE (`DceConst.PreservesGlobalConstness`) + merge
// (`MirMerge.MergePreservesGlobalConstness`). RED-ON-DISABLE: drop the
// `…globalIsConst(g)` argument at mir_rebuild_helper.cpp:54 / :96 (let it default to
// false) → the const global's `isConst` flips to false and the `EXPECT_TRUE` fails.
TEST(MirRebuildHelper, CloneGlobalsPreservesConstness) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32 = interner.primitive(TypeKind::I32);

    MirBuilder mb;
    MirLiteralValue v; v.value = std::int64_t{5}; v.core = TypeKind::I32;
    std::uint32_t const lit = mb.literalPoolAdd(v);
    // global #0 CONST (→ .rodata); global #1 MUTABLE (→ .data).
    (void)mb.addGlobal(i32, SymbolId{1}, lit, MirFuncId{},
                       SymbolBinding::Global, SymbolVisibility::Default,
                       /*isConst=*/true);
    (void)mb.addGlobal(i32, SymbolId{2}, lit, MirFuncId{},
                       SymbolBinding::Global, SymbolVisibility::Default,
                       /*isConst=*/false);
    Mir src = std::move(mb).finish();
    ASSERT_EQ(src.moduleGlobalCount(), 2u);
    ASSERT_TRUE(src.globalIsConst(src.globalAt(0)));
    ASSERT_FALSE(src.globalIsConst(src.globalAt(1)));

    // (a) cloneGlobalsVerbatim — the rebuild-pass chokepoint.
    {
        MirBuilder dst;
        cloneGlobalsVerbatim(src, dst);
        Mir out = std::move(dst).finish();
        ASSERT_EQ(out.moduleGlobalCount(), 2u);
        EXPECT_TRUE(out.globalIsConst(out.globalAt(0)))
            << "cloneGlobalsVerbatim must preserve a CONST global's const-ness "
               "(else it degrades to a writable .data section under a rebuild pass)";
        EXPECT_FALSE(out.globalIsConst(out.globalAt(1)))
            << "a mutable global must stay mutable";
    }
    // (b) cloneGlobalsOrCarveOut — the DCE / rebuild prelude.
    {
        MirBuilder dst;
        DiagnosticReporter rep;
        auto const r = cloneGlobalsOrCarveOut(src, dst, rep, "ConstnessTest");
        ASSERT_EQ(r, GlobalClonePrelude::Cloned);
        Mir out = std::move(dst).finish();
        ASSERT_EQ(out.moduleGlobalCount(), 2u);
        EXPECT_TRUE(out.globalIsConst(out.globalAt(0)))
            << "cloneGlobalsOrCarveOut must preserve const-ness";
        EXPECT_FALSE(out.globalIsConst(out.globalAt(1)));
    }
}
