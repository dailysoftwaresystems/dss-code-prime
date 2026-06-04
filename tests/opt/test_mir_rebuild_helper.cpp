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
Mir identityRebuild(Mir const& src) {
    MirBuilder dst;
    DiagnosticReporter rep;
    auto const carveOut = cloneGlobalsOrCarveOut(src, dst, rep,
                                                  "IdentityRoundTrip");
    // Fail-loud (ASSERT not EXPECT) so a future module mutation that
    // accidentally adds a runtime-init global halts here with an
    // attribution-clear failure rather than degrading the rebuild to
    // a silent no-op.
    ASSERT_EQ(carveOut, GlobalClonePrelude::Cloned)
        << "test modules don't use runtime-init globals; the carve-out "
           "branch shouldn't fire — otherwise the IdentityPolicy test "
           "is silently no-ops";
    IdentityPolicy policy;
    std::size_t const nf = src.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = src.funcAt(i);
        MirFunctionRebuilder rb{src, dst, policy};
        rb.rebuildFunction(f);
    }
    return std::move(dst).finish();
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

    Mir dst = identityRebuild(src);

    EXPECT_EQ(dst.instCount(),  srcInstCount);
    EXPECT_EQ(dst.blockCount(), srcBlockCount);
    EXPECT_EQ(countOp(dst, MirOpcode::Const),  srcConstCount);
    EXPECT_EQ(countOp(dst, MirOpcode::Phi),    srcPhiCount);
    EXPECT_EQ(countOp(dst, MirOpcode::CondBr), srcCondBrCount);
    EXPECT_EQ(countOp(dst, MirOpcode::Br),     srcBrCount);
    EXPECT_EQ(countOp(dst, MirOpcode::Return), srcReturnCount);
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

    Mir dst = identityRebuild(src);

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
