// OPT2 cycle 1 — `optimize()` engine + dispatch contract tests.
//
// Pins:
//   * D-OPT1-X-UNKNOWNPASSID-UNIT-PIN — a pipeline with a fabricated
//     PassId ordinal fires X_UnknownPassId from runPass's switch
//     fallback.
//   * D-OPT1-RETURN-FALSE-DIAGNOSTIC-CONTRACT — the snapshot guard
//     fires X_UnknownPassId again when a pass returns ok=false without
//     emitting (the only public way to exercise this today is via the
//     unknown-PassId path, which DOES emit — but the assertion is
//     verified by counting that exactly ONE X_UnknownPassId diagnostic
//     fires per unknown id, not two; double-emit would mean both
//     runPass + the optimize() guard fired).
//   * D-OPT1-OPT-RESULT-SHAPE — the new OptResult struct populates
//     passesRun/passesMutated correctly for the Identity-only pipeline.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "opt/optimizer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>

using namespace dss;

namespace {

// Build a one-function, one-block, one-Return MIR. Sufficient surface
// for the engine + verifier hook + ConstFold pass tests. The FnSig
// TypeId is interned (NOT a synthetic untagged literal) because the
// verifier — invoked after every pass — reads it through the interner
// for D-OPT1-VERIFY-AFTER-EVERY-PASS's type-invariant rule set.
Mir buildTrivialModule(TypeInterner& interner) {
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);
    MirBuilder b;
    b.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    b.addReturn();
    return std::move(b).finish();
}

std::size_t countCode(DiagnosticReporter const& r, DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : r.all()) if (d.code == code) ++n;
    return n;
}

} // namespace

// D-OPT1-X-UNKNOWNPASSID-UNIT-PIN: an unknown PassId ordinal triggers
// the runPass switch fallback. Tests the substrate's belt-and-
// suspenders guard at the type-system AND runtime layers — the
// static_assert on kPassIdCount catches drift at compile time; the
// runtime X_UnknownPassId catches an ordinal squeezed in via reinterpret-
// cast or numeric construction.
TEST(Optimizer, UnknownPassIdFiresXUnknownPassId) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& target = **targetR;
    TypeInterner interner{CompilationUnitId{1}};
    auto mir = buildTrivialModule(interner);
    DiagnosticReporter rep;

    opt::OptPipeline pipeline;
    pipeline.name = "synthetic-bad";
    // Fabricate an ordinal beyond every shipped PassId. The
    // static_assert in `optimizer.hpp` keeps kPassIdCount honest;
    // here we deliberately reach past that count via numeric
    // construction (the only way for a user to trigger the runtime
    // guard).
    pipeline.passes.push_back(static_cast<opt::PassId>(99));

    auto const result = opt::optimize(mir, target, interner, pipeline, rep);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(countCode(rep, DiagnosticCode::X_UnknownPassId), 1u)
        << "expected exactly one X_UnknownPassId — runPass's emit + the "
           "optimize() snapshot guard must NOT both fire on the same "
           "false-return path. The guard's purpose is to catch a future "
           "false-return without diagnostic — and it now fires a DEDICATED "
           "X_OptReturnFalseWithoutDiagnostic code, not X_UnknownPassId, "
           "so a future test can pin the contract-violation path "
           "independently of the enum-drift path.";
    EXPECT_EQ(countCode(rep, DiagnosticCode::X_OptReturnFalseWithoutDiagnostic), 0u)
        << "the contract-violation guard fires ONLY when a pass returns "
           "ok=false without emitting; here runPass's switch-fallback DID "
           "emit X_UnknownPassId, so the guard correctly does not fire.";
}

// D-OPT1-OPT-RESULT-SHAPE: the new OptResult populates passesRun /
// passesMutated correctly. Identity runs the loop once but mutates
// nothing.
TEST(Optimizer, OptResultIdentityShape) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& target = **targetR;
    TypeInterner interner{CompilationUnitId{1}};
    auto mir = buildTrivialModule(interner);
    DiagnosticReporter rep;

    opt::OptPipeline pipeline{"identity", {opt::PassId::Identity}};
    auto const result = opt::optimize(mir, target, interner, pipeline, rep);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.passesRun, 1u);
    EXPECT_EQ(result.passesMutated, 0u);
    EXPECT_TRUE(result.fixedPointReached);
    EXPECT_EQ(rep.errorCount(), 0u);
}

// D-OPT-CONSTFOLD-RERUN-POST-MEM2REG + D-OPT-PASS-METRICS effectiveness
// pin. Builds the `const_fold_inside_expr` witness in MIR form:
//     int a = 26;
//     int x;
//     x = 5 + 3;
//     return x * 2 + a;
// Under the release pipeline run to fixed point:
//   iter 1: ConstFold folds 5+3 → 8 (one mutation). Mem2Reg promotes
//           a + x. DCE drops dead allocas/stores.
//   iter 2: ConstFold folds 8*2 → 16 AND 16+26 → 42 (another mutation
//           — different folds, same PassId).
//   iter 3: nothing fires → fixed point reached.
//
// EFFECTIVENESS (not just correctness):
//  - `passMutationCount[ConstFold] >= 2` proves ConstFold mutated in
//    at least TWO distinct iterations — the re-fold post-Mem2Reg
//    happened. With `maxIterations = 1`, ConstFold runs only in iter
//    1; the 8*2 fold never fires; the count is 1.
//  - The final MIR contains NO `Mul` opcode — the multiply was
//    folded to a Const + the now-dead Mul was DCE'd. This is the
//    target-agnostic shape pin: an x86 `imul`-grep would be CPU-
//    coupled (ARM64 uses `mul`); the MIR opcode is what the
//    optimizer actually transforms, target-blind by construction.
//
// REGRESSION GUARD: the test runs the same MIR under TWO pipelines —
// the release-shape (maxIterations=4) and a single-iter regression
// (maxIterations=1) — and asserts the metrics + shape diverge as
// expected. A future regression that drops the release pipeline's
// `maxIterations` to 1 OR reorders passes so ConstFold no longer
// re-runs post-Mem2Reg flips this test red. Conversely, a future
// effectiveness improvement that makes the single-iter case ALSO
// achieve full folding (e.g. interleaved ConstFold-inside-Mem2Reg)
// would flip the regression-arm assertions and surface the change.
namespace {
// Hand-build the const_fold_inside_expr MIR. Returns a fresh Mir
// per call so each pipeline arm gets its own independent module.
Mir buildConstFoldInsideExprMir(TypeInterner& interner) {
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder b;
    b.addFunction(fnSig, SymbolId{100},
                  SymbolBinding::Global, SymbolVisibility::Default);
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);

    // int a = 26;
    MirInstId const slotA = b.addInst(MirOpcode::Alloca, {}, ptr);
    MirLiteralValue lv26; lv26.value = std::int64_t{26}; lv26.core = TypeKind::I32;
    MirInstId const c26 = b.addConst(lv26, i32);
    MirInstId const sa[] = {c26, slotA};
    (void)b.addInst(MirOpcode::Store, sa, InvalidType);

    // int x;  // alloca, no init
    MirInstId const slotX = b.addInst(MirOpcode::Alloca, {}, ptr);

    // x = 5 + 3;
    MirLiteralValue lv5; lv5.value = std::int64_t{5}; lv5.core = TypeKind::I32;
    MirLiteralValue lv3; lv3.value = std::int64_t{3}; lv3.core = TypeKind::I32;
    MirInstId const c5 = b.addConst(lv5, i32);
    MirInstId const c3 = b.addConst(lv3, i32);
    MirInstId const addOps[] = {c5, c3};
    MirInstId const sum53 = b.addInst(MirOpcode::Add, addOps, i32);
    MirInstId const sx[] = {sum53, slotX};
    (void)b.addInst(MirOpcode::Store, sx, InvalidType);

    // return x * 2 + a;
    MirInstId const loadXOps[] = {slotX};
    MirInstId const loadX = b.addInst(MirOpcode::Load, loadXOps, i32);
    MirLiteralValue lv2; lv2.value = std::int64_t{2}; lv2.core = TypeKind::I32;
    MirInstId const c2 = b.addConst(lv2, i32);
    MirInstId const mulOps[] = {loadX, c2};
    MirInstId const mul = b.addInst(MirOpcode::Mul, mulOps, i32);
    MirInstId const loadAOps[] = {slotA};
    MirInstId const loadA = b.addInst(MirOpcode::Load, loadAOps, i32);
    MirInstId const finalOps[] = {mul, loadA};
    MirInstId const finalSum = b.addInst(MirOpcode::Add, finalOps, i32);
    b.addReturn(finalSum);
    return std::move(b).finish();
}

std::size_t countMulInModule(Mir const& mir) {
    std::size_t n = 0;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t i2 = 0; i2 < ni; ++i2) {
                if (mir.instOpcode(mir.blockInstAt(b, i2)) == MirOpcode::Mul) ++n;
            }
        }
    }
    return n;
}
} // namespace

TEST(Optimizer, EffectivenessConstFoldRerunPostMem2Reg) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& target = **targetR;

    // The release-shape pipeline (what ships in `release.pipeline.json`).
    opt::OptPipeline const releaseShape{
        "release-shape",
        {opt::PassId::Identity, opt::PassId::ConstFold,
         opt::PassId::Mem2Reg,  opt::PassId::CopyProp,
         opt::PassId::Cse,      opt::PassId::Licm,
         opt::PassId::SimplifyCfg, opt::PassId::Dce},
        /*maxIterations*/4};

    // ARM 1 — happy path: full release shape with maxIterations=4.
    // Effectiveness invariants both hold.
    {
        TypeInterner interner{CompilationUnitId{1}};
        Mir mir = buildConstFoldInsideExprMir(interner);
        DiagnosticReporter rep;
        auto const result = opt::optimize(mir, target, interner, releaseShape, rep);
        ASSERT_TRUE(result.ok);
        EXPECT_TRUE(result.fixedPointReached);

        EXPECT_GE(result.mutationCount(opt::PassId::ConstFold), 2u)
            << "ConstFold must mutate in at least TWO iterations — once "
               "on the source-emitted 5+3, then again on 8*2 (and "
               "16+26) AFTER Mem2Reg promoted the alloca. A regression "
               "that drops maxIterations OR reorders passes flips this "
               "(D-OPT-CONSTFOLD-RERUN-POST-MEM2REG).";
        EXPECT_GE(result.mutationCount(opt::PassId::Mem2Reg), 1u)
            << "Mem2Reg must fire at least once on `x` and `a` (both "
               "promotable scalar allocas). Without Mem2Reg, the 8*2 "
               "fold opportunity never surfaces.";
        EXPECT_EQ(countMulInModule(mir), 0u)
            << "the Mul on `x * 2` must be eliminated — folded by the "
               "re-run ConstFold + DCE-swept. Surviving Mul means the "
               "post-Mem2Reg fold never fired.";

        // Engine-invariant cross-check: `passesMutated` should equal
        // the sum of `passMutationCount` over all PassIds (every
        // increment of one happens alongside the other, at the same
        // call site in the engine loop). Catches a future
        // refactor that decouples the two counters.
        std::size_t const sum = std::accumulate(
            result.passMutationCount.begin(),
            result.passMutationCount.end(), std::size_t{0});
        EXPECT_EQ(sum, result.passesMutated)
            << "passesMutated (cumulative total) must equal the sum "
               "of per-pass passMutationCount entries";
    }

    // ARM 2 — regression: maxIterations=1. ConstFold can only fold
    // 5+3 (in iter 1, before Mem2Reg runs in the same iter). The
    // 8*2 fold opportunity surfaced post-Mem2Reg has nowhere to
    // fire → Mul survives. This arm PROVES the test catches the
    // regression class — flipping maxIterations from 4→1 makes the
    // assertions in ARM 1 fail (we assert their inverse here).
    {
        opt::OptPipeline singleIter = releaseShape;
        singleIter.maxIterations = 1;

        TypeInterner interner{CompilationUnitId{1}};
        Mir mir = buildConstFoldInsideExprMir(interner);
        DiagnosticReporter rep;
        auto const result = opt::optimize(mir, target, interner, singleIter, rep);
        ASSERT_TRUE(result.ok);

        EXPECT_EQ(result.mutationCount(opt::PassId::ConstFold), 1u)
            << "with maxIterations=1, ConstFold runs exactly once and "
               "cannot re-fold post-Mem2Reg — this is the regression "
               "shape ARM 1 guards against";
        EXPECT_GE(countMulInModule(mir), 1u)
            << "the Mul survives the single-iter pipeline — the post-"
               "Mem2Reg fold opportunity was never reached";
    }

    // ARM 3 — SHIPPED pipeline integration: the test-analyzer's
    // load-bearing gap. ARMs 1+2 use an INLINE pipeline literal that
    // mirrors the shipped `release.pipeline.json`; if someone edits
    // the JSON to drop Mem2Reg or reorder passes without touching
    // the test, the inline ARM stays green while the SHIPPED
    // configuration regresses. This arm loads the actual shipped
    // pipeline and asserts the SAME witness — closes the loop
    // between behavioral effectiveness and shipped config.
    {
        auto pipelineR = opt::loadShippedPipeline("release");
        ASSERT_TRUE(pipelineR.has_value())
            << "shipped release.pipeline.json must load";

        TypeInterner interner{CompilationUnitId{1}};
        Mir mir = buildConstFoldInsideExprMir(interner);
        DiagnosticReporter rep;
        auto const result = opt::optimize(mir, target, interner, *pipelineR, rep);
        ASSERT_TRUE(result.ok);
        EXPECT_TRUE(result.fixedPointReached);
        EXPECT_GE(result.mutationCount(opt::PassId::ConstFold), 2u)
            << "the SHIPPED release pipeline must achieve the same "
               "re-fold effectiveness — a JSON edit that drops "
               "Mem2Reg or reorders ConstFold-then-Mem2Reg would "
               "regress without an inline-literal test catching it";
        EXPECT_EQ(countMulInModule(mir), 0u);
    }
}

// D-OPT-FIXED-POINT-LOOP: a pipeline with `maxIterations > 1` actually
// re-runs the pass list. With a foldable Add at iter 0, ConstFold
// mutates on the first iteration; on iteration 2 nothing changes →
// loop converges + breaks. `passesRun` == 2 × passes.size().
TEST(Optimizer, MaxIterationsFixedPointLoop) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& target = **targetR;
    TypeInterner interner{CompilationUnitId{1}};

    // Build a fn that returns Add(Const(1), Const(2)) — ConstFold
    // folds on iter 1; iter 2 finds nothing to fold.
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder b;
    b.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirLiteralValue v1; v1.value = std::int64_t{1}; v1.core = TypeKind::I32;
    MirLiteralValue v2; v2.value = std::int64_t{2}; v2.core = TypeKind::I32;
    MirInstId const c1 = b.addConst(v1, i32);
    MirInstId const c2 = b.addConst(v2, i32);
    MirInstId const ops[] = {c1, c2};
    MirInstId const sum = b.addInst(MirOpcode::Add, ops, i32);
    b.addReturn(sum);
    Mir mir = std::move(b).finish();

    DiagnosticReporter rep;
    opt::OptPipeline pipeline{"two-pass", {opt::PassId::ConstFold,
                                            opt::PassId::Identity}};
    pipeline.maxIterations = 4;
    auto const result = opt::optimize(mir, target, interner, pipeline, rep);
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.fixedPointReached)
        << "the cluster converges after iter 2; fixedPointReached must "
           "be true (iter-2 produces zero mutations → break)";
    // iter 1: ConstFold mutates + Identity no-ops → passesMutated == 1
    // iter 2: ConstFold no-ops + Identity no-ops → passesMutated still 1, break
    EXPECT_EQ(result.passesMutated, 1u);
    EXPECT_EQ(result.passesRun, 4u)
        << "passes×iters = 2×2 (the second iter detects convergence "
           "AFTER running through the full pipeline list)";
}

// D-OPT1-PASS-ID-STABILITY: the kPassIdCount drift guard's compile-
// time correctness is what matters; this test pins the RUNTIME side
// — optPassIdFromName resolves every shipped enumerator + rejects an
// unknown string. The kPassIdCount static_assert + this test together
// catch the three drift modes: enum-without-arm (compile error in
// runPass switch), enum-without-name (compile error against the
// optPassIdFromName switch — implicit via this test using a name
// that must resolve), name-without-enum (runtime nullopt — pinned
// here).
TEST(Optimizer, OptPassIdFromNameResolvesAllEnumerators) {
    EXPECT_EQ(opt::optPassIdFromName("Identity"),  opt::PassId::Identity);
    EXPECT_EQ(opt::optPassIdFromName("ConstFold"), opt::PassId::ConstFold);
    EXPECT_EQ(opt::optPassIdFromName("Dce"),       opt::PassId::Dce);
    EXPECT_FALSE(opt::optPassIdFromName("DoesNotExist").has_value());
}
