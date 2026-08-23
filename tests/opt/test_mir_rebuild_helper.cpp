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
#include <functional>
#include <variant>
#include <unordered_map>
#include <optional>
#include <string>
#include <string_view>
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
    // Mandatory (pure virtual) — a policy that does not name itself does not
    // compile (D-OPT-MIR-REBUILDER-FATAL-CANNOT-NAME-THE-PASS).
    [[nodiscard]] std::string_view passName() const noexcept override {
        return "IdentityRoundTrip";
    }

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
    (void)mb.addGlobal(i32, SymbolId{200}, initIdx, MirFuncId{},
                       SymbolBinding::Global, SymbolVisibility::Default,
                       /*isConst=*/false, MirThreadStorage::Shared);

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
// `…globalIsConst(g)` argument at mir_rebuild_helper.cpp (let it default to
// false) → the const global's `isConst` flips to false and the `EXPECT_TRUE` fails.
// TF-C78 (D-CSUBSET-NOINLINE): ★ THE LOAD-BEARING PROPAGATION PIN.
//
// `rebuildFunction` is the shared substrate under EVERY optimizer pass
// (ConstFold, Mem2Reg, CopyProp, Cse, Licm, SimplifyCfg, Dce). The shipped
// `release` pipeline runs `Inlining` FIRST in each of its 4 iterations, so a
// `noInline` flag dropped by ANY of those rebuilds is gone by iteration 2 and
// the callee is spliced then — with the inliner's refusal rule fully present
// and correct.
//
// ★ THIS IS MEASURED, NOT REASONED. Removing ONLY the `src_.funcNoInline(oldFn)`
// argument from `mir_rebuild_helper.cpp` — leaving `inlining.cpp` rule 2b
// untouched — produced a release-pipeline binary IDENTICAL to deleting rule 2b
// outright: the `noinline` helper gone entirely and `main` const-folded to a
// single `mov x29, #0x2a`. A half-landed flag and no flag at all are
// indistinguishable in the output, which is exactly why the refusal pin cannot
// stand alone and this one has to exist.
//
// RED-ON-DISABLE: drop that argument (let the 5th parameter default to false)
// and the `EXPECT_TRUE` below fails while every other rebuild test stays green.
// The un-annotated sibling asserts the flag is not spuriously acquired, so the
// test cannot be satisfied by hardcoding true.
TEST(MirRebuildHelper, RebuildFunctionPreservesNoInline) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    // func #0: NOINLINE (and deliberately Global/Default so the flag is the
    // only non-default axis — a Local binding would let a binding-preserving
    // rebuild look correct while dropping this bit).
    mb.addFunction(fnSig, SymbolId{1}, SymbolBinding::Global,
                   SymbolVisibility::Default, /*noInline=*/true);
    MirBlockId const b0 = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(b0);
    MirLiteralValue v0; v0.value = std::int64_t{7}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    // func #1: plain — must NOT acquire the flag.
    mb.addFunction(fnSig, SymbolId{2});
    MirBlockId const b1 = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(b1);
    MirLiteralValue v1; v1.value = std::int64_t{8}; v1.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v1, i32));

    Mir src = std::move(mb).finish();
    ASSERT_EQ(src.moduleFuncCount(), 2u);
    ASSERT_TRUE(src.funcNoInline(src.funcAt(0)));
    ASSERT_FALSE(src.funcNoInline(src.funcAt(1)));

    Mir dst;
    ASSERT_TRUE(identityRebuild(src, dst));
    ASSERT_EQ(dst.moduleFuncCount(), 2u);

    EXPECT_TRUE(dst.funcNoInline(dst.funcAt(0)))
        << "rebuildFunction must preserve the noInline flag — this rebuilder "
           "runs under every optimizer pass, so dropping it here silently "
           "re-arms the inliner on the pipeline's next iteration";
    EXPECT_FALSE(dst.funcNoInline(dst.funcAt(1)))
        << "an un-annotated function must not acquire the flag";

    // The sibling per-function axes must survive the same rebuild — they are
    // carried by the same call and a regression to any of them has the same
    // shape (this rebuilder is where `binding`/`visibility` are already pinned
    // by test_dce_linkage, mirrored here so the three stay pinned together).
    EXPECT_EQ(dst.funcBinding(dst.funcAt(0)),    SymbolBinding::Global);
    EXPECT_EQ(dst.funcVisibility(dst.funcAt(0)), SymbolVisibility::Default);
}

// TF-C81 (D-CSUBSET-ALWAYSINLINE): ★ THE SECOND PROPAGATION PIN — and MEASUREMENT
// MADE IT MORE NECESSARY THAN ITS TF-C78 SIBLING, NOT LESS. THIS TEST IS THE ONLY
// THING THAT CATCHES THIS HOP.
//
// TF-C78's finding was that two disable states (delete the rule / drop the flag
// at one hop) produce BYTE-IDENTICAL breakage, so an end-to-end test can prove
// the chain is broken but not which hop broke it. For `alwaysInline` the
// situation is STRICTLY WORSE and it was MEASURED, not assumed: dropping
// `src_.funcAlwaysInline(oldFn)` below leaves the end-to-end pin
// `MirLoweringCSubsetLinkage.AlwaysInlineBypassesThresholdInShippedRelease`
// COMPLETELY GREEN. The end-to-end test is BLIND to this hop.
//
// Why the asymmetry: `noInline` must keep REFUSING on every iteration, so a
// cleared flag re-arms the inliner on iteration 2 and the output changes.
// `alwaysInline` only has to be present at the FIRST inlining opportunity —
// `Inlining` runs first in iteration 1, before any rebuild touches the module,
// so in the simple shape the splice is already done. The flag still matters
// wherever the first iteration does not finish the job (a callee that becomes
// inlinable only after an earlier pass simplifies its caller; a cross-CU module
// merged after a round of optimization), and those shapes are precisely the ones
// an end-to-end fixture does not happen to construct.
//
// So: without THIS test the hop could be deleted and the whole suite would stay
// green. That is the entire argument for a dedicated propagation pin, in its
// sharpest form yet.
//
// ★ THE FIXTURE IS DELIBERATELY ASYMMETRIC: func #0 sets ONLY `alwaysInline` and
// this test asserts its `noInline` is still CLEAR. The two flags are adjacent
// trailing bools at every `addFunction` copy site, so a transposed pair would
// compile silently and invert the directive; a fixture that set both could not
// detect that. Keep the asymmetry when extending this test.
//
// RED-ON-DISABLE (MEASURED against the final build): drop the
// `src_.funcAlwaysInline(oldFn)` argument in `mir_rebuild_helper.cpp` (let the
// 6th parameter default to false) and exactly TWO assertions in the whole suite
// fail — the first EXPECT_TRUE below, and the flag-survival check inside
// `Inlining.AlwaysInlineCalleeBypassesCostThreshold`. Every other test,
// INCLUDING the shipped-release end-to-end pin and the corpus example, stays
// green.
TEST(MirRebuildHelper, RebuildFunctionPreservesAlwaysInline) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    // func #0: ALWAYS_INLINE only — Global/Default binding and noInline CLEAR,
    // so this flag is the single non-default axis on the record.
    mb.addFunction(fnSig, SymbolId{1}, SymbolBinding::Global,
                   SymbolVisibility::Default, /*noInline=*/false,
                   /*alwaysInline=*/true);
    MirBlockId const b0 = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(b0);
    MirLiteralValue v0; v0.value = std::int64_t{7}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    // func #1: NOINLINE only — the mirror record. Its presence is what proves a
    // swapped argument pair cannot pass: a transposition would make func #0 read
    // noInline and func #1 read alwaysInline, failing both directions at once.
    mb.addFunction(fnSig, SymbolId{2}, SymbolBinding::Global,
                   SymbolVisibility::Default, /*noInline=*/true,
                   /*alwaysInline=*/false);
    MirBlockId const b1 = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(b1);
    MirLiteralValue v1; v1.value = std::int64_t{8}; v1.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v1, i32));
    // func #2: plain — must acquire NEITHER flag.
    mb.addFunction(fnSig, SymbolId{3});
    MirBlockId const b2 = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(b2);
    MirLiteralValue v2; v2.value = std::int64_t{9}; v2.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v2, i32));

    Mir src = std::move(mb).finish();
    ASSERT_EQ(src.moduleFuncCount(), 3u);
    ASSERT_TRUE(src.funcAlwaysInline(src.funcAt(0)));
    ASSERT_FALSE(src.funcNoInline(src.funcAt(0)));

    Mir dst;
    ASSERT_TRUE(identityRebuild(src, dst));
    ASSERT_EQ(dst.moduleFuncCount(), 3u);

    EXPECT_TRUE(dst.funcAlwaysInline(dst.funcAt(0)))
        << "rebuildFunction must preserve the alwaysInline flag — this "
           "rebuilder runs under every optimizer pass, so dropping it here "
           "silently re-applies the size threshold on the next iteration";
    EXPECT_FALSE(dst.funcNoInline(dst.funcAt(0)))
        << "and it must land in the RIGHT bit — the two flags are adjacent "
           "bools at the addFunction call, so a swap must fail here";
    EXPECT_TRUE(dst.funcNoInline(dst.funcAt(1)))
        << "the mirror record: noInline must survive independently";
    EXPECT_FALSE(dst.funcAlwaysInline(dst.funcAt(1)))
        << "and must not bleed into the alwaysInline bit";
    EXPECT_FALSE(dst.funcAlwaysInline(dst.funcAt(2)))
        << "an un-annotated function must not acquire the flag";
    EXPECT_FALSE(dst.funcNoInline(dst.funcAt(2)));
}

// ══ TF-C85: the `#pragma optimize("", off)` per-function opt-out ══════════════
namespace {
// A policy that WOULD mangle any function it is let near: it drops every
// non-terminator instruction (`shouldEmit` false) and keeps only the entry
// block. Under the neuter it must be consulted ZERO times, so a `noOptimize`
// function comes through with all of its blocks and instructions intact.
//
// ★ IT IS DELIBERATELY DESTRUCTIVE RATHER THAN MERELY DIFFERENT. A policy that
// made a subtle change could pass a sloppy assertion; this one makes the
// difference between "neutered" and "not neutered" impossible to miss, and it
// exercises BOTH the block-selection hook and a per-instruction hook.
// A policy that INJECTS one extra instruction at the head of every block it is
// let near — the `Mem2Reg` shape (its `onBlockBegin` inserts IDF phis), and the
// simplest transform that is both legal and trivially observable.
//
// ★ WHY A HOOK THAT ADDS RATHER THAN ONE THAT REMOVES. A policy that dropped
// instructions would trip the rebuilder's own `D-OPT2-REWRITE-MAP-COMPLETENESS`
// fail-loud the moment a surviving operand referenced a skipped instruction —
// MEASURED while writing this test. Injection is unconditionally safe (an
// unreferenced Const is legal MIR) and makes "the policy ran" a pure count.
//
// ★ NOTE `tryRewrite` IS NOT USABLE HERE: `emitValue` copies a `Const` verbatim
// BEFORE consulting the hook, so a Const-rewriting policy is silently a no-op.
// (MEASURED — the first draft of this test used exactly that and passed
// vacuously on one arm.)
class InjectingPolicy : public MirRebuildPolicy {
public:
    [[nodiscard]] std::string_view passName() const noexcept override {
        return "NoOptimizeNeuter";
    }

    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        ++selectBlocksCalls;
        std::vector<MirBlockId> out;
        for (std::uint32_t i = 0; i < src.funcBlockCount(fn); ++i)
            out.push_back(src.funcBlockAt(fn, i));
        return out;
    }
    void onBlockBegin(MirBlockId /*oldB*/, MirBlockId /*newB*/,
                      MirBuilder& dst,
                      std::unordered_map<std::uint32_t, MirInstId>& /*rewrite*/,
                      std::unordered_map<std::uint32_t, MirBlockId> const&
                          /*blockMap*/) override {
        ++onBlockBeginCalls;
        MirLiteralValue v;
        v.value = std::int64_t{999};
        v.core  = TypeKind::I32;
        (void)dst.addConst(v, i32Type);
    }
    TypeId      i32Type = InvalidType;
    std::size_t selectBlocksCalls  = 0;
    std::size_t onBlockBeginCalls  = 0;
};

// The same policy, but declaring itself a MANDATORY NORMALIZATION — the
// `PruneUnreachable` posture. It must run even on a `noOptimize` function.
class MandatoryInjectingPolicy final : public InjectingPolicy {
public:
    // Re-answered rather than inherited: a fatal that named the BASE policy
    // would point the reader at the neuter test while the mandatory-normalize
    // test is the one that died.
    [[nodiscard]] std::string_view passName() const noexcept override {
        return "MandatoryNormalization";
    }

    [[nodiscard]] bool mandatoryNormalization() const noexcept override {
        return true;
    }
};

// A policy recording whether the rebuilder told it a function was neutered —
// the `Mem2Reg` notification, isolated.
class NeuterNotedPolicy final : public MirRebuildPolicy {
public:
    [[nodiscard]] std::string_view passName() const noexcept override {
        return "NeuterNotice";
    }

    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        std::vector<MirBlockId> out;
        for (std::uint32_t i = 0; i < src.funcBlockCount(fn); ++i)
            out.push_back(src.funcBlockAt(fn, i));
        return out;
    }
    void onFunctionNeutered(MirFuncId oldFn) override {
        neutered.push_back(oldFn.v);
    }
    std::vector<std::uint32_t> neutered;
};

// Two single-block functions: #0 `noOptimize`, #1 plain. Each returns a const.
[[nodiscard]] Mir buildNoOptimizePair(TypeInterner& interner) {
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    // func #0: noOptimize ONLY — noInline and alwaysInline deliberately CLEAR so
    // this flag is the single non-default axis. The three are adjacent trailing
    // bools at every `addFunction` copy site, so a transposed argument would
    // compile silently; the asymmetry is what makes a swap fail here.
    mb.addFunction(fnSig, SymbolId{1}, SymbolBinding::Global,
                   SymbolVisibility::Default, /*noInline=*/false,
                   /*alwaysInline=*/false, /*noOptimize=*/true);
    MirBlockId const b0 = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(b0);
    MirLiteralValue v0; v0.value = std::int64_t{7}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    // func #1: plain.
    mb.addFunction(fnSig, SymbolId{2});
    MirBlockId const b1 = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(b1);
    MirLiteralValue v1; v1.value = std::int64_t{8}; v1.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v1, i32));
    return std::move(mb).finish();
}
} // namespace

// ★ THE THIRD PROPAGATION PIN. Same argument as its two neighbours: this
// rebuilder runs under every optimizer pass, so a flag dropped here means
// iteration 2 starts optimizing a function the source excluded.
//
// RED-ON-DISABLE (re-verified against the FINAL build): drop the
// `src_.funcNoOptimize(oldFn)` argument at `mir_rebuild_helper.cpp`'s
// `addFunction` (let the 7th parameter default to false) and the first
// EXPECT_TRUE below fails.
TEST(MirRebuildHelper, RebuildFunctionPreservesNoOptimize) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir src = buildNoOptimizePair(interner);
    ASSERT_EQ(src.moduleFuncCount(), 2u);
    ASSERT_TRUE(src.funcNoOptimize(src.funcAt(0)));
    ASSERT_FALSE(src.funcNoInline(src.funcAt(0)));
    ASSERT_FALSE(src.funcAlwaysInline(src.funcAt(0)));

    Mir dst;
    ASSERT_TRUE(identityRebuild(src, dst));
    ASSERT_EQ(dst.moduleFuncCount(), 2u);
    EXPECT_TRUE(dst.funcNoOptimize(dst.funcAt(0)))
        << "rebuildFunction must preserve the noOptimize flag";
    EXPECT_FALSE(dst.funcNoInline(dst.funcAt(0)))
        << "and it must land in the RIGHT bit — three adjacent bools at the "
           "addFunction call, so a swap must fail here";
    EXPECT_FALSE(dst.funcAlwaysInline(dst.funcAt(0)));
    EXPECT_FALSE(dst.funcNoOptimize(dst.funcAt(1)))
        << "an un-marked function must not acquire the flag";
}

// ★★ TF-C92 (D-CSUBSET-NO-SANITIZE-THREAD): THE FOURTH PROPAGATION PIN — AND THE
// ONLY ONE OF THE FOUR WITH **NO** BEHAVIOURAL BACKSTOP ANYWHERE IN THE SUITE.
//
// The argument, sharpened from TF-C81's: `noInline` must keep refusing on every
// iteration (so dropping it changes the end-to-end release output);
// `alwaysInline`'s hop is invisible to the end-to-end pin but a dedicated
// inliner-behaviour test still exists; `noOptimize` reaches the rebuilder's own
// policy swap, which a behaviour test observes. `noSanitizeThread` reaches NO PASS
// AT ALL — MEASURED, `grep -rni sanitiz src/` returns zero hits — so dropping the
// argument below changes NOTHING that any pass-behaviour test could see. The only
// detectors are this pin and the MIR-text assertions.
//
// ★ THE FIXTURE IS DELIBERATELY ASYMMETRIC: func #0 sets ONLY `noSanitizeThread`
// and this test asserts its other three flags are still CLEAR. Four adjacent
// trailing bools at every `addFunction` copy site means a transposed pair compiles
// silently; a fixture that set two flags could not detect it. Func #1 sets ONLY
// `noOptimize` — the IMMEDIATE neighbour in the argument list — so a one-position
// shift fails both records at once. Keep the asymmetry when extending this test.
//
// RED-ON-DISABLE (verified against the final build): drop the
// `src_.funcNoSanitizeThread(oldFn)` argument at `mir_rebuild_helper.cpp`'s
// `addFunction` (let the 8th parameter default to false) and the first EXPECT_TRUE
// below fails, together with the post-optimize MIR-text assertions in
// `MirLoweringCSubsetLinkage.NoSanitizeThreadSurvivesShippedReleasePipeline`.
// Nothing else in the suite moves.
TEST(MirRebuildHelper, RebuildFunctionPreservesNoSanitizeThread) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    // func #0: NO_SANITIZE_THREAD only — Global/Default binding and all three
    // sibling flags CLEAR, so this flag is the single non-default axis on the record.
    mb.addFunction(fnSig, SymbolId{1}, SymbolBinding::Global,
                   SymbolVisibility::Default, /*noInline=*/false,
                   /*alwaysInline=*/false, /*noOptimize=*/false,
                   /*noSanitizeThread=*/true);
    MirBlockId const b0 = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(b0);
    MirLiteralValue v0; v0.value = std::int64_t{11}; v0.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v0, i32));
    // func #1: NOOPTIMIZE only — the adjacent-argument mirror. Its presence is what
    // proves a shifted argument list cannot pass: a one-position shift would make
    // func #0 read noOptimize and func #1 read noSanitizeThread, failing both
    // directions at once.
    mb.addFunction(fnSig, SymbolId{2}, SymbolBinding::Global,
                   SymbolVisibility::Default, /*noInline=*/false,
                   /*alwaysInline=*/false, /*noOptimize=*/true,
                   /*noSanitizeThread=*/false);
    MirBlockId const b1 = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(b1);
    MirLiteralValue v1; v1.value = std::int64_t{12}; v1.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v1, i32));
    // func #2: plain — must acquire NEITHER flag.
    mb.addFunction(fnSig, SymbolId{3});
    MirBlockId const b2 = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(b2);
    MirLiteralValue v2; v2.value = std::int64_t{13}; v2.core = TypeKind::I32;
    mb.addReturn(mb.addConst(v2, i32));

    Mir src = std::move(mb).finish();
    ASSERT_EQ(src.moduleFuncCount(), 3u);
    ASSERT_TRUE(src.funcNoSanitizeThread(src.funcAt(0)));
    ASSERT_FALSE(src.funcNoOptimize(src.funcAt(0)));

    Mir dst;
    ASSERT_TRUE(identityRebuild(src, dst));
    ASSERT_EQ(dst.moduleFuncCount(), 3u);

    EXPECT_TRUE(dst.funcNoSanitizeThread(dst.funcAt(0)))
        << "rebuildFunction must preserve the noSanitizeThread flag — this "
           "rebuilder runs under every optimizer pass, and because NO pass reads "
           "the flag, dropping it here is invisible to every behavioural test in "
           "the suite; this assertion is the only guard";
    EXPECT_FALSE(dst.funcNoOptimize(dst.funcAt(0)))
        << "and it must land in the RIGHT bit — four adjacent bools at the "
           "addFunction call, so a shift must fail here";
    EXPECT_FALSE(dst.funcNoInline(dst.funcAt(0)));
    EXPECT_FALSE(dst.funcAlwaysInline(dst.funcAt(0)));
    EXPECT_TRUE(dst.funcNoOptimize(dst.funcAt(1)))
        << "the mirror record: noOptimize must survive independently";
    EXPECT_FALSE(dst.funcNoSanitizeThread(dst.funcAt(1)))
        << "and must not bleed into the noSanitizeThread bit";
    EXPECT_FALSE(dst.funcNoSanitizeThread(dst.funcAt(2)))
        << "an un-annotated function must not acquire the flag";
    EXPECT_FALSE(dst.funcNoOptimize(dst.funcAt(2)));
}

// ★★ THE SINK ITSELF. A `noOptimize` function is rebuilt VERBATIM under a policy
// that would otherwise gut it — and, critically, IT STILL EXISTS.
//
// ★ THE SURVIVES-AT-ALL ASSERTION IS THE POINT, NOT A FORMALITY. The obvious
// implementation of "skip optimizing this function" is to `return` early from
// `rebuildFunction`. That does not leave the function alone: `dst` is a FRESH
// module containing exactly what the rebuilder puts in it, so an early return
// DELETES the function and every caller is left referencing an undefined symbol.
// `moduleFuncCount() == 2` is what fails first under that mistake.
TEST(MirRebuildHelper, NoOptimizeFunctionIsRebuiltVerbatimUnderAnInjectingPolicy) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir src = buildNoOptimizePair(interner);
    std::uint32_t const srcInsts0 = src.blockInstCount(src.funcEntry(src.funcAt(0)));
    std::uint32_t const srcInsts1 = src.blockInstCount(src.funcEntry(src.funcAt(1)));

    MirBuilder dstB;
    DiagnosticReporter rep;
    ASSERT_EQ(cloneGlobalsOrCarveOut(src, dstB, rep, "NoOptimizeNeuter"),
              GlobalClonePrelude::Cloned);
    InjectingPolicy policy;
    policy.i32Type = interner.primitive(TypeKind::I32);
    for (std::uint32_t i = 0; i < src.moduleFuncCount(); ++i) {
        MirFunctionRebuilder rb{src, dstB, policy};
        rb.rebuildFunction(src.funcAt(i));
    }
    Mir dst = std::move(dstB).finish();

    ASSERT_EQ(dst.moduleFuncCount(), 2u)
        << "THE FUNCTION MUST STILL EXIST. Neutering means swapping the POLICY, "
           "never skipping the rebuild — skipping deletes the function";
    EXPECT_EQ(dst.blockInstCount(dst.funcEntry(dst.funcAt(0))), srcInsts0)
        << "func #0 is noOptimize: it comes through VERBATIM because the "
           "injecting policy was never consulted for it";
    EXPECT_EQ(policy.selectBlocksCalls, 1u)
        << "selectBlocks must be called for the PLAIN function only";
    EXPECT_EQ(policy.onBlockBeginCalls, 1u)
        << "…and so must the per-block hook";
    EXPECT_EQ(dst.blockInstCount(dst.funcEntry(dst.funcAt(1))), srcInsts1 + 1u)
        << "the plain function DOES get the injected instruction, proving the "
           "policy is effective and the comparison above is not vacuous";
    EXPECT_TRUE(dst.funcNoOptimize(dst.funcAt(0)));
    EXPECT_FALSE(dst.funcNoOptimize(dst.funcAt(1)));
}

// ★★ THE EXEMPTION. A policy that declares itself a MANDATORY NORMALIZATION runs
// on a `noOptimize` function anyway. MEASURED necessity: without this,
// `PruneUnreachable` was neutered and sqlite's `ext/misc/totype.c` — the corpus's
// only no-optimize TU — produced four `I_UnreachableBlock` verifier errors.
// `#pragma optimize` switches optimization off; it does not license invalid IR.
TEST(MirRebuildHelper, MandatoryNormalizationPolicyIsNotNeutered) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir src = buildNoOptimizePair(interner);
    std::uint32_t const srcInsts0 = src.blockInstCount(src.funcEntry(src.funcAt(0)));

    MirBuilder dstB;
    DiagnosticReporter rep;
    ASSERT_EQ(cloneGlobalsOrCarveOut(src, dstB, rep, "MandatoryNormalization"),
              GlobalClonePrelude::Cloned);
    MandatoryInjectingPolicy policy;
    policy.i32Type = interner.primitive(TypeKind::I32);
    for (std::uint32_t i = 0; i < src.moduleFuncCount(); ++i) {
        MirFunctionRebuilder rb{src, dstB, policy};
        rb.rebuildFunction(src.funcAt(i));
    }
    Mir dst = std::move(dstB).finish();
    ASSERT_EQ(dst.moduleFuncCount(), 2u);
    EXPECT_EQ(policy.selectBlocksCalls, 2u)
        << "a mandatory-normalization policy is consulted for EVERY function, "
           "including the noOptimize one";
    EXPECT_EQ(dst.blockInstCount(dst.funcEntry(dst.funcAt(0))), srcInsts0 + 1u)
        << "…and its hooks really ran on the noOptimize function";
    EXPECT_TRUE(dst.funcNoOptimize(dst.funcAt(0)))
        << "…and the flag still rides through the exempt rebuild";
}

// ★ THE `onFunctionNeutered` NOTIFICATION. Fired for the neutered function and
// only for it. MEASURED necessity: `Mem2Reg` plans IDF phis in `analyze`, emits
// them in `onBlockBegin`, and wires them in a POST-rebuild step — silencing the
// hooks without telling it aborted the whole pe64 sqlite build with "phi marker
// has incomings but no emitted phi".
TEST(MirRebuildHelper, NeuteredFunctionIsAnnouncedToThePolicy) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir src = buildNoOptimizePair(interner);

    MirBuilder dstB;
    DiagnosticReporter rep;
    ASSERT_EQ(cloneGlobalsOrCarveOut(src, dstB, rep, "NeuterNotice"),
              GlobalClonePrelude::Cloned);
    NeuterNotedPolicy policy;
    for (std::uint32_t i = 0; i < src.moduleFuncCount(); ++i) {
        MirFunctionRebuilder rb{src, dstB, policy};
        rb.rebuildFunction(src.funcAt(i));
    }
    Mir unused = std::move(dstB).finish();
    (void)unused;
    ASSERT_EQ(policy.neutered.size(), 1u)
        << "exactly one function is noOptimize, so exactly one notification";
    EXPECT_EQ(policy.neutered[0], src.funcAt(0).v);
}

TEST(MirRebuildHelper, CloneGlobalsPreservesConstness) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32 = interner.primitive(TypeKind::I32);

    MirBuilder mb;
    MirLiteralValue v; v.value = std::int64_t{5}; v.core = TypeKind::I32;
    std::uint32_t const lit = mb.literalPoolAdd(v);
    // global #0 CONST (→ .rodata); global #1 MUTABLE (→ .data).
    (void)mb.addGlobal(i32, SymbolId{1}, lit, MirFuncId{},
                       SymbolBinding::Global, SymbolVisibility::Default,
                       /*isConst=*/true, MirThreadStorage::Shared);
    (void)mb.addGlobal(i32, SymbolId{2}, lit, MirFuncId{},
                       SymbolBinding::Global, SymbolVisibility::Default,
                       /*isConst=*/false, MirThreadStorage::Shared);
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

// TLS C1 (D-CSUBSET-THREAD-LOCAL, ★CRIT-3): thread-storage preservation
// across the two SHARED rebuild global-clone fns — the audit's flag-drop
// clone sites #2 and #3 (`cloneGlobalsOrCarveOut` runs on EVERY rebuild
// pass of EVERY optimized compile; `cloneGlobalsVerbatim` on every prune/
// normalize). A dropped flag silently demotes a per-thread object to
// process-shared under optimization — release builds only, the worst kind
// of divergence. Exact per-global assertions; the CloneGlobalsPreservesConstness
// shape mirrored so the two flag classes stay pinned side by side.
// RED-ON-DISABLE: drop the `…globalIsThreadLocal(g)` argument at
// mir_rebuild_helper.cpp (pass MirThreadStorage::Shared) → the TLS
// global's flag flips and the
// EXPECT_TRUE fails.
TEST(MirRebuildHelper, CloneGlobalsPreservesThreadLocal) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32 = interner.primitive(TypeKind::I32);

    MirBuilder mb;
    MirLiteralValue v; v.value = std::int64_t{5}; v.core = TypeKind::I32;
    std::uint32_t const lit = mb.literalPoolAdd(v);
    // global #0 THREAD-LOCAL; global #1 plain.
    (void)mb.addGlobal(i32, SymbolId{1}, lit, MirFuncId{},
                       SymbolBinding::Global, SymbolVisibility::Default,
                       /*isConst=*/false, MirThreadStorage::PerThread);
    (void)mb.addGlobal(i32, SymbolId{2}, lit, MirFuncId{},
                       SymbolBinding::Global, SymbolVisibility::Default,
                       /*isConst=*/false, MirThreadStorage::Shared);
    Mir src = std::move(mb).finish();
    ASSERT_EQ(src.moduleGlobalCount(), 2u);
    ASSERT_TRUE(src.globalIsThreadLocal(src.globalAt(0)));
    ASSERT_FALSE(src.globalIsThreadLocal(src.globalAt(1)));

    // (a) cloneGlobalsVerbatim — the prune/normalize chokepoint.
    {
        MirBuilder dst;
        cloneGlobalsVerbatim(src, dst);
        Mir out = std::move(dst).finish();
        ASSERT_EQ(out.moduleGlobalCount(), 2u);
        EXPECT_TRUE(out.globalIsThreadLocal(out.globalAt(0)))
            << "cloneGlobalsVerbatim must preserve thread storage duration "
               "(else a per-thread object silently becomes process-shared "
               "under a rebuild pass)";
        EXPECT_FALSE(out.globalIsThreadLocal(out.globalAt(1)))
            << "a plain global must stay process-shared";
    }
    // (b) cloneGlobalsOrCarveOut — the rebuild-pass prelude.
    {
        MirBuilder dst;
        DiagnosticReporter rep;
        auto const r = cloneGlobalsOrCarveOut(src, dst, rep, "ThreadLocalTest");
        ASSERT_EQ(r, GlobalClonePrelude::Cloned);
        Mir out = std::move(dst).finish();
        ASSERT_EQ(out.moduleGlobalCount(), 2u);
        EXPECT_TRUE(out.globalIsThreadLocal(out.globalAt(0)))
            << "cloneGlobalsOrCarveOut must preserve thread storage duration";
        EXPECT_FALSE(out.globalIsThreadLocal(out.globalAt(1)));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// EVERY REBUILD ABORT NAMES THE PASS THAT DROVE IT
// (D-OPT-MIR-REBUILDER-FATAL-CANNOT-NAME-THE-PASS)
//
// ~9 policies drive one `MirFunctionRebuilder`, and its fatal text is identical
// whichever one is driving: `rewriteOperand: old MirInstId v=N has no rewrite
// entry` names the HELPER and hides the culprit. ✔MEASURED as a cost — isolating
// D-OPT-ASM-GOTO-WITH-OUTPUTS-ABORTS-THE-MIR-REBUILDER took a `DSS_OPT_TRACE`
// run whose only product was the word `SimplifyCfg`.
//
// ★★ WHY THESE ARE DEATH TESTS AND NOT AN INSPECTION OF THE FORMAT STRING. The
// attribution's whole value is that it SURVIVES THE PROCESS — a fatal is the one
// message a reader cannot follow up interactively. A test that asserted the
// literal existed in the source would pass for a message nothing ever printed;
// only a spawned child that actually dies proves the text reaches stderr. These
// use `EXPECT_DEATH`, so the abort happens in the CHILD and takes no sibling
// test's verdict with it (the failure mode
// D-TEST-ABORT-IN-A-FIXTURE-HAS-NO-GUARD exists to stop).
//
// ★★ THE MATCHERS CARRY NO REGEX METACHARACTERS ON PURPOSE. The obvious witness
// is the bracketed form `[pass=X]`, and `[...]` is a CHARACTER CLASS to both
// regex engines GoogleTest can be built against (POSIX extended on Linux, its own
// simple RE on Windows) — the assertion would then pass on the letters `p`, `a`,
// `s`, `=` in any order and red on nothing. `pass=X` is a literal in both.
//
// COVERAGE: five of the nine fatal paths in `mir_rebuild_helper.cpp` are
// reachable from a hand-built module through a policy; the enumeration of all
// nine, and why the other four are not reachable from a unit test, is at the
// bottom of this block.
namespace {

// The `rewriteOperand` path — THE fatal the row was written about. `shouldEmit`
// drops an instruction that a later instruction still uses, which is precisely
// the "operand referenced a skipped instruction" arm.
class SkipUsedValuePolicy final : public MirRebuildPolicy {
public:
    [[nodiscard]] std::string_view passName() const noexcept override {
        return "SkipUsedValueProbe";
    }
    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        std::vector<MirBlockId> out;
        for (std::uint32_t i = 0; i < src.funcBlockCount(fn); ++i)
            out.push_back(src.funcBlockAt(fn, i));
        return out;
    }
    [[nodiscard]] bool shouldEmit(MirInstId oldId) override {
        return oldId.v != skip.v;
    }
    MirInstId skip{};
};

// The `emitTerminator` successor path: a policy that omits a REACHABLE block
// from `selectBlocks`, so the entry's terminator points at a block the rebuild
// never created.
class DropSuccessorPolicy final : public MirRebuildPolicy {
public:
    [[nodiscard]] std::string_view passName() const noexcept override {
        return "DropSuccessorProbe";
    }
    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        return {src.funcEntry(fn)};   // deliberately incomplete
    }
};

// The absorb-chain cycle path: a policy that absorbs a block into ITSELF, so the
// chase never terminates and trips the block-count cap.
class SelfAbsorbPolicy final : public MirRebuildPolicy {
public:
    [[nodiscard]] std::string_view passName() const noexcept override {
        return "SelfAbsorbProbe";
    }
    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        std::vector<MirBlockId> out;
        for (std::uint32_t i = 0; i < src.funcBlockCount(fn); ++i)
            out.push_back(src.funcBlockAt(fn, i));
        return out;
    }
    [[nodiscard]] std::optional<MirBlockId>
    absorbSuccessor(MirBlockId oldB) override { return oldB; }
};

// The `onZeroPhiIncomings` path: reject every incoming, so a phi ends phase 3
// with none.
class RejectEveryPhiIncomingPolicy final : public MirRebuildPolicy {
public:
    [[nodiscard]] std::string_view passName() const noexcept override {
        return "RejectPhiIncomingProbe";
    }
    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        return mirReversePostOrder(src, src.funcEntry(fn));
    }
    [[nodiscard]] bool acceptPhiIncoming(
        MirPhiIncoming const& /*inc*/, MirBlockId /*oldPhiBlock*/,
        std::unordered_map<std::uint32_t, MirBlockId> const& /*blockMap*/)
        override {
        return false;
    }
};

// ★ THE NEUTER ATTRIBUTION PROBE. Its hooks are the base defaults — it exists
// only to be the policy a rebuilder is CONSTRUCTED with while the policy actually
// driving a `noOptimize` function is `MirIdentityRebuildPolicy`. The fatal must
// name THIS pass, not the substitute.
class NeuterAttributionPolicy final : public MirRebuildPolicy {
public:
    [[nodiscard]] std::string_view passName() const noexcept override {
        return "NeuterAttributionProbe";
    }
    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        std::vector<MirBlockId> out;
        for (std::uint32_t i = 0; i < src.funcBlockCount(fn); ++i)
            out.push_back(src.funcBlockAt(fn, i));
        return out;
    }
};

// Two blocks: entry stores a const then branches; the tail returns the const.
// The Return's operand is produced in the ENTRY block, so dropping that producer
// leaves a live use with no rewrite entry.
[[nodiscard]] Mir buildTwoBlockUse(TypeInterner& interner, MirInstId& producer) {
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tail  = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry);
    MirLiteralValue v; v.value = std::int64_t{41}; v.core = TypeKind::I32;
    producer = mb.addConst(v, i32);
    mb.addBr(tail);
    mb.beginBlock(tail);
    mb.addReturn(producer);
    return std::move(mb).finish();
}

// A diamond with a phi at the join — the shape `acceptPhiIncoming` filters.
[[nodiscard]] Mir buildDiamondWithPhi(TypeInterner& interner) {
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const boolT = interner.primitive(TypeKind::Bool);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const thenB = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const elseB = mb.createBlock(StructCfMarker::IfElse);
    MirBlockId const joinB = mb.createBlock(StructCfMarker::IfJoin);

    mb.beginBlock(entry);
    MirLiteralValue cv; cv.value = true; cv.core = TypeKind::Bool;
    mb.addCondBr(mb.addConst(cv, boolT), thenB, elseB);

    mb.beginBlock(thenB);
    MirLiteralValue t; t.value = std::int64_t{1}; t.core = TypeKind::I32;
    MirInstId const tv = mb.addConst(t, i32);
    mb.addBr(joinB);

    mb.beginBlock(elseB);
    MirLiteralValue e; e.value = std::int64_t{2}; e.core = TypeKind::I32;
    MirInstId const ev = mb.addConst(e, i32);
    mb.addBr(joinB);

    mb.beginBlock(joinB);
    MirPhiIncoming const incs[] = {{tv, thenB}, {ev, elseB}};
    mb.addReturn(mb.addPhi(i32, incs));
    return std::move(mb).finish();
}

// A single `noOptimize` function whose only phi has NO incomings. It is the ONLY
// fatal a neutered rebuild can still reach: `MirIdentityRebuildPolicy` selects
// every block and accepts every incoming, so nothing the SUBSTITUTE does can
// abort — the defect has to be in the source MIR itself.
[[nodiscard]] Mir buildNoOptimizeEmptyPhi(TypeInterner& interner) {
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{1}, SymbolBinding::Global,
                   SymbolVisibility::Default, /*noInline=*/false,
                   /*alwaysInline=*/false, /*noOptimize=*/true);
    MirBlockId const b0 = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(b0);
    mb.addReturn(mb.addPhi(i32));   // zero incomings — the structural violation
    return std::move(mb).finish();
}

} // namespace

// ★★ ONE TEST, FIVE ARMS, AND THE LOOP BODY IS A VOID CALLABLE ON PURPOSE. A
// bare `ASSERT_*` inside a range-for `return`s from the enclosing TEST, so the
// FIRST arm to fail would silently cancel every later arm and the run would still
// read as one failure instead of five unexamined paths. Routing each arm through
// `std::function<void()>` makes the `return` local to the arm. (That exact
// vacuity was caught twice in this repo during the cycle this test was written.)
TEST(MirRebuildHelperDeathTest, EveryReachableRebuildFatalNamesItsPass) {
    // ★ TWO WITNESSES PER ARM, AND THE SECOND IS WHAT KEEPS THE COVERAGE CLAIM
    // HONEST. `passWitness` alone would pass for an arm that died in the WRONG
    // fatal path — every path in this file now prints the same `pass=` field, so
    // e.g. the self-absorb arm reaching `rewriteOperand` instead of the
    // absorb-cycle cap would still match and the block would silently be four
    // paths tested twice rather than five tested once. `pathWitness` pins WHICH
    // abort ran. Neither carries a regex metacharacter; `.*` between them is the
    // only one, and it is deliberate (the pass field precedes the detail).
    struct Arm {
        char const*                  label;
        char const*                  passWitness;
        char const*                  pathWitness;
        std::function<void()>        run;
    };

    std::vector<Arm> const arms = {
        {"rewriteOperand: operand referenced a skipped instruction",
         "pass=SkipUsedValueProbe",
         "rewriteOperand: old MirInstId",
         [] {
             TypeInterner interner{CompilationUnitId{1}};
             MirInstId producer{};
             Mir const src = buildTwoBlockUse(interner, producer);
             MirBuilder dstB;
             SkipUsedValuePolicy policy;
             policy.skip = producer;
             MirFunctionRebuilder rb{src, dstB, policy};
             rb.rebuildFunction(src.funcAt(0));
         }},
        {"emitTerminator: successor omitted from selectBlocks",
         "pass=DropSuccessorProbe",
         "emitTerminator successor old",
         [] {
             TypeInterner interner{CompilationUnitId{1}};
             MirInstId producer{};
             Mir const src = buildTwoBlockUse(interner, producer);
             MirBuilder dstB;
             DropSuccessorPolicy policy;
             MirFunctionRebuilder rb{src, dstB, policy};
             rb.rebuildFunction(src.funcAt(0));
         }},
        {"absorbSuccessor: cycle in the absorb chain",
         "pass=SelfAbsorbProbe",
         "absorbSuccessor chain exceeded block count",
         [] {
             TypeInterner interner{CompilationUnitId{1}};
             MirInstId producer{};
             Mir const src = buildTwoBlockUse(interner, producer);
             MirBuilder dstB;
             SelfAbsorbPolicy policy;
             MirFunctionRebuilder rb{src, dstB, policy};
             rb.rebuildFunction(src.funcAt(0));
         }},
        {"onZeroPhiIncomings: every incoming rejected",
         "pass=RejectPhiIncomingProbe",
         "zero accepted incomings",
         [] {
             TypeInterner interner{CompilationUnitId{1}};
             Mir const src = buildDiamondWithPhi(interner);
             MirBuilder dstB;
             RejectEveryPhiIncomingPolicy policy;
             MirFunctionRebuilder rb{src, dstB, policy};
             rb.rebuildFunction(src.funcAt(0));
         }},
        // ★ THE ONE ARM THAT DISTINGUISHES `drivingPassName` FROM
        // `this->passName()`. The function is `noOptimize`, so TF-C85 swaps in
        // `MirIdentityRebuildPolicy` — and the fatal must still name the pass the
        // rebuilder was CONSTRUCTED with. If the substrate ever reports the
        // ACTIVE policy here it prints `pass=Identity`, which sends the reader
        // looking for an abort in the debug pipeline that never happened, and
        // this arm is the only thing in the suite that would notice.
        {"onZeroPhiIncomings under the noOptimize neuter names the DRIVING pass",
         "pass=NeuterAttributionProbe",
         "zero accepted incomings",
         [] {
             TypeInterner interner{CompilationUnitId{1}};
             Mir const src = buildNoOptimizeEmptyPhi(interner);
             MirBuilder dstB;
             NeuterAttributionPolicy policy;
             MirFunctionRebuilder rb{src, dstB, policy};
             rb.rebuildFunction(src.funcAt(0));
         }},
    };

    ASSERT_EQ(arms.size(), 5u)
        << "arm count is pinned so a deleted arm is a failure, not a quietly "
           "smaller run";

    for (auto const& arm : arms) {
        SCOPED_TRACE(arm.label);
        std::string const witness =
            std::string{arm.passWitness} + ".*" + arm.pathWitness;
        EXPECT_DEATH(arm.run(), witness)
            << "the rebuilder aborted without naming the pass that drove it (or "
               "died in a different path than this arm builds) — a reader of an "
               "un-attributed fatal is left with a helper name and ~9 candidate "
               "policies (D-OPT-MIR-REBUILDER-FATAL-CANNOT-NAME-THE-PASS)";
    }
}

// The subject spelling is shared by ALL nine fatal paths, so it is asserted once
// here rather than nine times above: a message that named the pass but not the
// helper would be a different kind of unusable.
TEST(MirRebuildHelperDeathTest, FatalStillNamesTheHelperAsWellAsThePass) {
    // ⚠ The statement is a NAMED callable rather than an inline lambda: a `{a, b}`
    // (or any top-level comma) inside a macro argument splits it into two
    // arguments and `EXPECT_DEATH` reports "passed 4 arguments, but takes just 2".
    // The same trap is documented in tests/analysis/compilation_unit.
    std::function<void()> const die = [] {
        TypeInterner interner{CompilationUnitId{1}};
        MirInstId producer{};
        Mir const src = buildTwoBlockUse(interner, producer);
        MirBuilder dstB;
        SkipUsedValuePolicy policy;
        policy.skip = producer;
        MirFunctionRebuilder rb{src, dstB, policy};
        rb.rebuildFunction(src.funcAt(0));
    };
    EXPECT_DEATH(die(), "MirFunctionRebuilder fatal")
        << "the pass name ADDS an attribution; it must not REPLACE the subject";
}

// THE ENUMERATION, so a reader can tell coverage from hope. Nine `std::abort`
// paths live in `mir_rebuild_helper.cpp`, all nine now route through the one
// `rebuildFatal` composer that cannot be called without a pass name:
//
//   1. rewriteOperand — no rewrite entry                    ← arm 1 above
//   2. emitTerminator/mapSucc — successor not in blockMap    ← arm 2
//   3. rebuildFunction — absorb-chain cycle                  ← arm 3
//   4. rebuildFunction phase 3 — zero accepted incomings     ← arms 4 and 5
//   5. rebuildFunction phase 3 — redirected pred not in map
//   6. emitValue — BlockAddress target not in blockMap
//   7. emitTerminator — `default:` no clone arm for a terminator opcode
//   8. cloneGlobalsRemappingInitFunc — remap/module size mismatch
//   9. cloneGlobalsRemappingInitFunc — a dropped module-init function
//
// Paths 5–9 are NOT covered by a death test here, and the reason is stated rather
// than left as an absence:
//   * 5 needs a policy that ACCEPTS a phi incoming and simultaneously redirects
//     its pred to an elided block — reachable, but the shape only exists as a
//     self-contradictory policy, i.e. the test would assert the substrate's
//     response to a policy no pass can be written to resemble.
//   * 6 needs a computed-goto (`&&label`) module whose address-taken block a
//     policy then elides; the address-taken guard (MF-B) is what prevents it, so
//     building it means disabling a second mechanism inside this test.
//   * 7 is unreachable BY CONSTRUCTION without adding a terminator opcode to
//     `MirOpcode` — the arm exists for a future opcode, and a test could only
//     reach it by shipping a fake one.
//   * 8 and 9 belong to `cloneGlobalsRemappingInitFunc`, a free function with a
//     single call site (the optimizer's inline-definition strip), not to the
//     rebuilder. They take the pass name as an explicit parameter, so the
//     compiler enforces the attribution the same way the pure virtual does for a
//     policy: there is no overload without it.
// ⇒ What is claimed is that ALL NINE compose through one attributed helper (a
// structural property, and the mutant below is what proves it is load-bearing),
// and that FIVE of them are additionally witnessed end-to-end by a dying process.
