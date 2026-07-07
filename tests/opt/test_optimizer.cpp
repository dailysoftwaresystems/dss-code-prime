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
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"
#include "mir/mir_verifier.hpp"
#include "opt/optimizer.hpp"
#include "opt/passes/cse.hpp"

#include <gtest/gtest.h>

#include <array>
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

std::size_t countOpInModule(Mir const& mir, MirOpcode op) {
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
        EXPECT_EQ(countOpInModule(mir, MirOpcode::Mul), 0u)
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
        EXPECT_GE(countOpInModule(mir, MirOpcode::Mul), 1u)
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
        EXPECT_EQ(countOpInModule(mir, MirOpcode::Mul), 0u);
    }
}

// ── D-OPT-ALIAS-ARC-CORPUS-CAPSTONE-STRICT effectiveness pin ────────
//
// Hand-built mirror of `examples/c-subset/strict_alias_cse_capstone/
// main.c`'s `compute(int* pI, long* pL){int a=*pI; *pL=7; return
// a+*pI;}` shape — but the alloca on `a` is elided because the
// effectiveness arc closes at MIR level on the Args directly (the
// corpus binary differential covers the full alloca-promoted Mem2Reg
// → Cse pipeline path; this pin closes the alias-rule → opcode-count
// → passMutationCount arc target-blind).
//
// Why this is the strict arm's capstone: cycles 10a-10g built up the
// alias substrate (mirMayAlias Rule 6 distinct non-char primitives;
// strict-TBAA opt-in via `MirAliasingMode::StrictTBAA`); cycle 10h
// added the `long`→I64 primitive so `Ptr<I32>` vs `Ptr<I64>` is
// expressible in c-subset source. This pin proves the FULL release
// pipeline observes the effectiveness — the second Load(pI) is
// eliminated AND `passMutationCount[Cse] >= 1`. Without it, the
// corpus row's runtime exit code agreement between baseline + Cse
// arms is necessary but not sufficient: a hypothetical regression
// that disabled CSE entirely would still pass the corpus differential
// (both arms exit 10) while silently losing all CSE effectiveness.
namespace {
Mir buildAliasArcStrictCapstoneMir(TypeInterner& interner) {
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const i64    = interner.primitive(TypeKind::I64);
    TypeId const ptrI32 = interner.pointer(i32);
    TypeId const ptrI64 = interner.pointer(i64);
    TypeId const params[] = {ptrI32, ptrI64};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder b;
    // Match the c-subset config: strictAliasingOnDistinctTypes=true
    // + charTypesAliasAll=true. The capstone exercises Rule 6 (not
    // Rule 5), so charTypesAliasAll's value doesn't gate the
    // outcome — but setting both flags here keeps the fixture
    // faithful to what the c-subset HIR→MIR lowering would stamp.
    b.setAliasingMode(MirAliasingMode::StrictTBAA);
    b.setCharTypesAliasAll(true);
    b.addFunction(fnSig, SymbolId{100},
                  SymbolBinding::Global, SymbolVisibility::Default);
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const pI = b.addArg(0, ptrI32);
    MirInstId const pL = b.addArg(1, ptrI64);

    // int a = *pI;
    MirInstId const lOps[] = {pI};
    MirInstId const ld1 = b.addInst(MirOpcode::Load, lOps, i32);

    // *pL = 7;   (we use a direct I64 Const here — matches the
    // shape the optimizer sees after ConstFold collapses any
    // intervening I32→I64 SExt. Equivalent to `*pL = 7L;`)
    MirLiteralValue v7; v7.value = std::int64_t{7}; v7.core = TypeKind::I64;
    MirInstId const c7    = b.addConst(v7, i64);
    MirInstId const stOps[] = {c7, pL};
    (void)b.addInst(MirOpcode::Store, stOps, InvalidType);

    // return a + *pI;
    MirInstId const ld2 = b.addInst(MirOpcode::Load, lOps, i32);
    MirInstId const addOps[] = {ld1, ld2};
    MirInstId const sum = b.addInst(MirOpcode::Add, addOps, i32);
    b.addReturn(sum);
    return std::move(b).finish();
}
// (countOpInModule is defined earlier in this file's anonymous
// namespace; cycle 10i post-fold unified it with the prior
// `countMulInModule`, eliminating one-pattern-per-opcode growth.)
} // namespace

TEST(Optimizer, EffectivenessAliasArcStrictTBAACapstone) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& target = **targetR;

    // ARM 1 — Identity-only baseline. CSE not in pipeline → both
    // Loads survive; passMutationCount[Cse] == 0. Establishes the
    // un-optimised shape against which ARM 2 + ARM 3 diff.
    {
        TypeInterner interner{CompilationUnitId{1}};
        Mir mir = buildAliasArcStrictCapstoneMir(interner);
        ASSERT_EQ(mir.aliasingMode(), MirAliasingMode::StrictTBAA)
            << "MirBuilder must propagate the strict-TBAA flag onto "
               "the finished Mir — a flag-propagation regression would "
               "make ARMs 2/3 silently behave like Permissive";
        opt::OptPipeline const noop{
            "identity-only", {opt::PassId::Identity}, /*maxIterations*/1};
        DiagnosticReporter rep;
        auto const result = opt::optimize(mir, target, interner, noop, rep);
        ASSERT_TRUE(result.ok);
        EXPECT_EQ(countOpInModule(mir, MirOpcode::Load), 2u)
            << "Identity-only must preserve both Loads — this is the "
               "baseline shape against which the Cse arms diff";
        EXPECT_EQ(result.mutationCount(opt::PassId::Cse), 0u)
            << "Identity-only never invokes Cse";
    }

    // ARM 2 — `[Cse, Dce]` minimal effectiveness pipeline. CSE
    // substitutes uses of Load2 with Load1 (instructionsCsed++)
    // but leaves the redundant Load instruction in the rebuild —
    // DCE is the pass that physically removes it once it's dead.
    // Together the pair drops the Load count from 2 → 1 AND
    // `passMutationCount[Cse] >= 1`. Without CSE: 2 Loads remain
    // (Load2 still has uses via Add). Without DCE: CSE counter
    // increments but Load2 lingers in the rebuilt MIR.
    //
    // Why both passes are required: this captures how the corpus
    // capstone's effectiveness is observable post-DCE — the same
    // way the existing `EffectivenessConstFoldRerunPostMem2Reg`
    // test pairs ConstFold with DCE to make the Mul disappear.
    // Tests that don't include the sweep would erroneously
    // measure the substrate, not the user-visible outcome.
    // Fixture-property anchor for the `== 1u` count below (T3 post-fold
    // 2026-06-04 audit): `buildAliasArcStrictCapstoneMir` produces
    // exactly TWO Load insts and NO other CSE-eligible duplicate
    // (1 Const, 1 Store, 1 Add — none commutatively-equal in pairs).
    // A future fixture edit adding a second Const(7) or similar would
    // break the exact-one-CSE-event invariant; the tightened ARM 2
    // count below makes that surface loudly.
    {
        TypeInterner interner{CompilationUnitId{1}};
        Mir mir = buildAliasArcStrictCapstoneMir(interner);
        ASSERT_EQ(mir.aliasingMode(), MirAliasingMode::StrictTBAA);
        opt::OptPipeline const cseDce{
            "cse-dce", {opt::PassId::Cse, opt::PassId::Dce},
            /*maxIterations*/1};
        DiagnosticReporter rep;
        auto const result = opt::optimize(mir, target, interner, cseDce, rep);
        ASSERT_TRUE(result.ok);
        // T1 (HIGH, audit FOLD-NOW 2026-06-04): tightened from `>= 1u`
        // to `== 1u`. The fixture has exactly one CSE candidate pair
        // (the two Loads); an incidental extra mutation would surface
        // as a regression here rather than silently passing.
        EXPECT_EQ(result.mutationCount(opt::PassId::Cse), 1u)
            << "passMutationCount[Cse] must be EXACTLY 1 — fixture "
               "has exactly one CSE candidate (the two Loads). A "
               "higher count means a non-Load CSE fired (incidental "
               "regression class); a lower count means Load CSE "
               "didn't fire (alias-rule regression).";
        EXPECT_EQ(countOpInModule(mir, MirOpcode::Load), 1u)
            << "after Cse-then-Dce the redundant Load must be swept. "
               "Two surviving Loads = either Cse didn't fire (alias "
               "regression) OR Dce didn't sweep (DCE regression).";
        // T2 (HIGH, audit FOLD-NOW 2026-06-04): pin CSE substitution
        // direction, not just "a Load disappeared". The capstone's
        // single Add must now have BOTH operands resolving to the
        // SAME MirInstId — proves Load2 was redirected to Load1 (or
        // vice versa) at the USE site, not merely DCE'd. A regression
        // where CSE incremented its counter but didn't rewrite the
        // Add's operand would leave Add(loadX, loadY) with distinct
        // ids; this assertion catches that class.
        MirInstId addId = InvalidMirInst;
        std::size_t const nfn = mir.moduleFuncCount();
        for (std::uint32_t fi = 0; fi < nfn; ++fi) {
            MirFuncId const f = mir.funcAt(fi);
            std::uint32_t const nb = mir.funcBlockCount(f);
            for (std::uint32_t bi = 0; bi < nb; ++bi) {
                MirBlockId const b = mir.funcBlockAt(f, bi);
                std::uint32_t const ni = mir.blockInstCount(b);
                for (std::uint32_t ii = 0; ii < ni; ++ii) {
                    MirInstId const id = mir.blockInstAt(b, ii);
                    if (mir.instOpcode(id) == MirOpcode::Add) {
                        addId = id;
                        break;
                    }
                }
            }
        }
        ASSERT_TRUE(addId.valid())
            << "post-opt MIR must still contain the Add inst — "
               "the function returns its sum";
        auto const addOps = mir.instOperands(addId);
        ASSERT_EQ(addOps.size(), 2u);
        EXPECT_EQ(addOps[0].v, addOps[1].v)
            << "Add(load1, load2) post-CSE must collapse to "
               "Add(loadN, loadN) — both operand ids point at the "
               "surviving Load. A regression that didn't rewrite "
               "the Add's operand would surface as distinct ids.";
    }

    // ARM 3 — SHIPPED release pipeline integration. Closes the loop
    // between the in-test inline pipeline (ARM 2) and the actually-
    // shipped JSON. A future edit to release.pipeline.json that
    // drops Cse OR reorders such that the prior pass clobbers the
    // CSE opportunity would regress here without ARM 2 catching it.
    //
    // Two pins: (a) the shipped JSON's `passes[]` MUST contain Cse —
    // a config edit removing Cse breaks this loudly with a JSON-
    // content message, distinct from the effectiveness message
    // (a) below; (b) the effectiveness shape (Load==1 + Cse≥1)
    // matches ARM 2 — proves the JSON pipeline doesn't accidentally
    // INHIBIT the CSE opportunity through pass ordering.
    {
        auto pipelineR = opt::loadShippedPipeline("release");
        ASSERT_TRUE(pipelineR.has_value())
            << "shipped release.pipeline.json must load";

        // Pin (a): the shipped pipeline MUST contain Cse. A JSON edit
        // that drops Cse fails this distinct from the effectiveness
        // check below, so the diagnostic surfaces the right cause.
        bool containsCse = false;
        for (auto const p : pipelineR->passes) {
            if (p == opt::PassId::Cse) { containsCse = true; break; }
        }
        EXPECT_TRUE(containsCse)
            << "release.pipeline.json must contain Cse — a JSON edit "
               "removing it would silently lose all CSE effectiveness; "
               "this pin makes the regression cause attributable.";

        TypeInterner interner{CompilationUnitId{1}};
        Mir mir = buildAliasArcStrictCapstoneMir(interner);
        ASSERT_EQ(mir.aliasingMode(), MirAliasingMode::StrictTBAA)
            << "ARM 3 silent-failure pin: a regression that drops the "
               "strict-TBAA flag at finish() would let ARM 3 still pass "
               "via incidental CSE under Permissive — match ARMs 1+2.";
        DiagnosticReporter rep;
        auto const result = opt::optimize(mir, target, interner, *pipelineR, rep);
        ASSERT_TRUE(result.ok);
        EXPECT_EQ(countOpInModule(mir, MirOpcode::Load), 1u)
            << "shipped release pipeline must achieve the same Load-"
               "count effectiveness as ARM 2 — pinning the JSON "
               "config + the inline pipeline together";
        // Shipped pipeline runs fixed-point iterations so Cse may fire
        // more than once across iterations (e.g., second iteration
        // hitting another opportunity surfaced by an intervening
        // pass). `>= 1u` is the correct shape here — the EXACT-one-
        // mutation invariant is enforced by ARM 2 on the minimal
        // [Cse, Dce] pipeline.
        EXPECT_GE(result.mutationCount(opt::PassId::Cse), 1u);
        // T2 (HIGH, audit FOLD-NOW 2026-06-04): same Add-operand-
        // identity pin as ARM 2 — proves the shipped pipeline rewrote
        // the Add's operand (not just incremented a counter +
        // DCE-swept a dead Load).
        MirInstId addId = InvalidMirInst;
        std::size_t const nfn = mir.moduleFuncCount();
        for (std::uint32_t fi = 0; fi < nfn; ++fi) {
            MirFuncId const f = mir.funcAt(fi);
            std::uint32_t const nb = mir.funcBlockCount(f);
            for (std::uint32_t bi = 0; bi < nb; ++bi) {
                MirBlockId const b = mir.funcBlockAt(f, bi);
                std::uint32_t const ni = mir.blockInstCount(b);
                for (std::uint32_t ii = 0; ii < ni; ++ii) {
                    MirInstId const id = mir.blockInstAt(b, ii);
                    if (mir.instOpcode(id) == MirOpcode::Add) {
                        addId = id;
                        break;
                    }
                }
            }
        }
        ASSERT_TRUE(addId.valid());
        auto const addOps = mir.instOperands(addId);
        ASSERT_EQ(addOps.size(), 2u);
        EXPECT_EQ(addOps[0].v, addOps[1].v)
            << "shipped pipeline must also collapse Add(load1, load2) "
               "to Add(loadN, loadN) — substitution direction pinned";
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

// D-CSUBSET-DIVISION-OP-CODEGEN closure-gate item (b) (cycle 10r,
// 2026-06-04). The corpus example (`examples/c-subset/division/`)
// exercises the full source→optimized→binary pipeline, but a
// hypothetical regression that silently CONST-FOLDS the divide away
// (e.g. a future inlining pass that propagates the literal 100, 7
// across the call) would make the corpus exit-code witness pass with
// NO live IDIV in the binary — defeating the gate's purpose. This
// MIR-level test is the COMPLEMENTARY pin: a hand-built function
// shaped `int divide(int a, int b) { return a / b; }` (with args =
// runtime-opaque MirArg values, NOT constants) runs through the
// SHIPPED release pipeline; the post-optimization MIR MUST still
// contain at least one MirOpcode::SDiv. Without this assertion, the
// closure gate's "optimized arm contains a live idiv" requirement
// reduces to "we ran the release pipeline and didn't crash" — which
// is necessary but not sufficient.
//
// Why this matters specifically for cycle 10r: the REX-overlap bug
// (cycle 10q's compound encoding) was a CODEGEN bug — it would only
// manifest when the IDIV instruction actually reached the assembler.
// If a future optimizer regression silently optimized away the
// divide BEFORE codegen, the corpus would still exit 14 (because
// ConstFold would compute 100/7=14 at compile time and the binary
// would just return 14 via mov), giving the FALSE impression that
// the codegen is healthy. This MIR-level test pins that the
// optimizer DOES NOT remove the divide when its operands are
// runtime-opaque — preserving the codegen path for the byte-pin
// tests to attest.
TEST(Optimizer, EffectivenessSDivSurvivesShippedReleasePipeline) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& target = **targetR;

    auto buildDivideArgsMir = [](TypeInterner& interner) -> Mir {
        TypeId const i32 = interner.primitive(TypeKind::I32);
        TypeId const params[] = {i32, i32};
        TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
        MirBuilder b;
        b.addFunction(fnSig, SymbolId{100},
                      SymbolBinding::Global, SymbolVisibility::Default);
        MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
        b.beginBlock(entry);
        MirInstId const a = b.addArg(0, i32);  // runtime-opaque
        MirInstId const c = b.addArg(1, i32);  // runtime-opaque
        MirInstId const ops[] = {a, c};
        MirInstId const q = b.addInst(MirOpcode::SDiv, ops, i32);
        b.addReturn(q);
        return std::move(b).finish();
    };

    auto pipelineR = opt::loadShippedPipeline("release");
    ASSERT_TRUE(pipelineR.has_value())
        << "shipped release.pipeline.json must load";

    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildDivideArgsMir(interner);
    DiagnosticReporter rep;
    auto const result = opt::optimize(mir, target, interner, *pipelineR, rep);
    ASSERT_TRUE(result.ok);
    EXPECT_TRUE(result.fixedPointReached);

    // 7-agent review fold (silent-failure F3 8/10 + type-design
    // Concern 4 7/10, 2026-06-04): tightened from `EXPECT_GE >= 1` to
    // `ASSERT_EQ == 1`. The fixture builds EXACTLY ONE SDiv; an
    // ASSERT_EQ catches BOTH (a) the original silent-miscompile
    // class (optimizer removes the divide → count drops to 0) AND
    // (b) a new failure mode (a faulty LICM/peephole that DUPLICATES
    // the SDiv — e.g. cloning across a loop header → count rises to
    // 2). ASSERT (not EXPECT) makes the test stop on this load-
    // bearing invariant rather than soft-fail with no further
    // attribution.
    ASSERT_EQ(countOpInModule(mir, MirOpcode::SDiv), 1u)
        << "MIR SDiv with runtime-opaque (MirArg) operands MUST survive "
           "the SHIPPED release pipeline exactly once — ConstFold "
           "cannot fold args, DCE cannot eliminate a Return-feeding "
           "inst, CopyProp/CSE/SimplifyCFG/LICM have no transformation "
           "that removes a single-use divide. A regression that "
           "silently eliminates this divide (e.g. an over-eager "
           "inliner) OR silently duplicates it (e.g. faulty LICM "
           "hoist that clones across a header) defeats the codegen-"
           "tier byte-pin tests' purpose — they only run when "
           "exactly one IDIV reaches the assembler. (D-CSUBSET-"
           "DIVISION-OP-CODEGEN closure-gate item (b), 2026-06-04.)";
}

// D-CSUBSET-DIVISION-OP-CODEGEN closure-gate item (b) — UDiv analog
// (cycle 10r 7-agent review fold pr-test #3 7/10, 2026-06-04). The
// SDiv test above pins that the signed divide survives the shipped
// release pipeline. UDiv has NO source-language entry in c-subset
// today (no unsigned types yet — anchored D-CSUBSET-UDIV-RUNTIME-
// HIGH-BIT-PIN), so the only way UDiv reaches codegen is via hand-
// built MIR — exactly the path this test pins. Without this analog,
// a future opt-pass regression that folded MirOpcode::UDiv with
// opaque args would silently disappear the entire `xor_rdx_zero +
// div_op` codegen with no corpus row to catch it (the corpus
// `division/main.c` exercises SDiv, not UDiv).
TEST(Optimizer, EffectivenessUDivSurvivesShippedReleasePipeline) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& target = **targetR;

    auto buildUDivideArgsMir = [](TypeInterner& interner) -> Mir {
        TypeId const i32 = interner.primitive(TypeKind::I32);
        TypeId const params[] = {i32, i32};
        TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
        MirBuilder b;
        b.addFunction(fnSig, SymbolId{100},
                      SymbolBinding::Global, SymbolVisibility::Default);
        MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
        b.beginBlock(entry);
        MirInstId const a = b.addArg(0, i32);  // runtime-opaque
        MirInstId const c = b.addArg(1, i32);  // runtime-opaque
        MirInstId const ops[] = {a, c};
        MirInstId const q = b.addInst(MirOpcode::UDiv, ops, i32);
        b.addReturn(q);
        return std::move(b).finish();
    };

    auto pipelineR = opt::loadShippedPipeline("release");
    ASSERT_TRUE(pipelineR.has_value());

    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildUDivideArgsMir(interner);
    DiagnosticReporter rep;
    auto const result = opt::optimize(mir, target, interner, *pipelineR, rep);
    ASSERT_TRUE(result.ok);

    ASSERT_EQ(countOpInModule(mir, MirOpcode::UDiv), 1u)
        << "MIR UDiv with runtime-opaque operands MUST survive the "
           "shipped release pipeline exactly once — same invariant as "
           "SDiv's analog. No c-subset corpus row exercises UDiv today "
           "(anchored D-CSUBSET-UDIV-RUNTIME-HIGH-BIT-PIN); this MIR-"
           "level pin is the sole codegen-tier-survives guard for the "
           "`xor_rdx_zero + div_op` shape until unsigned types land.";
}

// FC1 (V2-4.X, 2026-06-10) — the SMod analog of the two effectiveness
// pins above (D-CSUBSET-MOD-OP-CODEGEN closure-gate). The modulo
// corpus example uses runtime-call operands under the BASELINE
// (unoptimized) pipeline; this MIR-level pin is what keeps the
// OPTIMIZED codegen path honest: an SMod whose operands are
// runtime-opaque MirArgs must survive the shipped release pipeline
// exactly once (ConstFold cannot fold args; a regression that folds
// or duplicates it would let the codegen-tier remainder-capture shape
// rot unobserved).
TEST(Optimizer, EffectivenessSModSurvivesShippedReleasePipeline) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& target = **targetR;

    auto buildModuloArgsMir = [](TypeInterner& interner) -> Mir {
        TypeId const i32 = interner.primitive(TypeKind::I32);
        TypeId const params[] = {i32, i32};
        TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
        MirBuilder b;
        b.addFunction(fnSig, SymbolId{100},
                      SymbolBinding::Global, SymbolVisibility::Default);
        MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
        b.beginBlock(entry);
        MirInstId const a = b.addArg(0, i32);  // runtime-opaque
        MirInstId const c = b.addArg(1, i32);  // runtime-opaque
        MirInstId const ops[] = {a, c};
        MirInstId const m = b.addInst(MirOpcode::SMod, ops, i32);
        b.addReturn(m);
        return std::move(b).finish();
    };

    auto pipelineR = opt::loadShippedPipeline("release");
    ASSERT_TRUE(pipelineR.has_value());

    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildModuloArgsMir(interner);
    DiagnosticReporter rep;
    auto const result = opt::optimize(mir, target, interner, *pipelineR, rep);
    ASSERT_TRUE(result.ok);

    ASSERT_EQ(countOpInModule(mir, MirOpcode::SMod), 1u)
        << "MIR SMod with runtime-opaque (MirArg) operands MUST survive "
           "the shipped release pipeline exactly once — the optimized "
           "arm of the modulo feature is only as honest as this pin "
           "(D-CSUBSET-MOD-OP-CODEGEN closure gate).";
}

// D-OPT1-PASS-DUP-POLICY engine-arm pin: a pipeline declaring
// `{ConstFold, ConstFold}` doesn't get silently de-duped by the engine.
// The loader admits the shape (test_pipeline_loader); the engine must
// actually dispatch both. Catches a future regression where the
// dispatch loop adds "skip if PassId already visited" optimization.
TEST(Optimizer, DuplicatePassesInPipelineBothDispatch) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& target = **targetR;
    TypeInterner interner{CompilationUnitId{1}};

    // Fixture: a foldable Add. ConstFold mutates on first run, then
    // on the SECOND run finds nothing left to fold. With the engine
    // dispatching both entries, passMutationCount[ConstFold] must be
    // at least 1 from the first run; both dispatches contribute to
    // passesRun.
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
    // `maxIterations = 1` keeps the outer loop from re-running the
    // pipeline (which would dispatch ConstFold even more times) so
    // `passesRun == 2` cleanly attributes to BOTH pipeline entries
    // being honored.
    opt::OptPipeline pipeline{"dup-const-fold",
                              {opt::PassId::ConstFold, opt::PassId::ConstFold}};
    pipeline.maxIterations = 1;
    auto const result = opt::optimize(mir, target, interner, pipeline, rep);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.passesRun, 2u)
        << "engine must dispatch BOTH ConstFold entries (no silent dedup)";
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
    EXPECT_EQ(opt::optPassIdFromName("Inlining"),  opt::PassId::Inlining);
    EXPECT_FALSE(opt::optPassIdFromName("DoesNotExist").has_value());
}

// OPT7 effectiveness pin: the Inlining pass, driven through the engine
// on a module with an eligible Global single-block leaf call, records
// `mutationCount(PassId::Inlining) >= 1`. Proves the pass is reachable
// via the engine dispatch + its mutated=true accounting flows into the
// per-PassId metrics array (D-OPT-PASS-METRICS), not only via the
// pass's own InliningResult counter. Fixture: main() { return g(); }
// with g() a Global leaf returning 7 → main's call is inlined.
TEST(Optimizer, EffectivenessInliningFiresOnLeafCall) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& target = **targetR;
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder b;
    // g (SymbolId 50): Global single-block leaf returning 7.
    b.addFunction(fnSig, SymbolId{50});
    MirBlockId const gEntry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(gEntry);
    MirLiteralValue seven; seven.value = std::int64_t{7}; seven.core = TypeKind::I32;
    b.addReturn(b.addConst(seven, i32));
    // main (SymbolId 100): return g();
    b.addFunction(fnSig, SymbolId{100});
    MirBlockId const mEntry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(mEntry);
    MirInstId const gAddr = b.addGlobalAddr(SymbolId{50}, fnSig);
    MirInstId const callOps[] = {gAddr};
    MirInstId const call = b.addInst(MirOpcode::Call, callOps, i32);
    b.addReturn(call);
    Mir mir = std::move(b).finish();

    DiagnosticReporter rep;
    opt::OptPipeline pipeline{"inlining", {opt::PassId::Inlining}};
    auto const result = opt::optimize(mir, target, interner, pipeline, rep);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_GE(result.mutationCount(opt::PassId::Inlining), 1u)
        << "Inlining must record >= 1 mutation on a module with an "
           "eligible Global single-block leaf call";
}

// ── D15 cycle C (e): the SHIPPED release pipeline COMPILES both probe
// shapes — MIR tier, target-blind. ──────────────────────────────────
// The two acceptance probes (rec.c recursion, iter.c loop) are exactly
// the RC-A (SimplifyCfg post-fold reachability) + RC-B (MultiBlockInliner
// topological layout) shapes. This pin runs the SHIPPED
// release.pipeline.json ITSELF (loadShippedPipeline — name/maxIterations/
// inlineThreshold from the file, zero drift) over each probe's MIR and
// asserts (a) optimize() ok + (b) the post-pipeline module is verifier-
// clean (incl. the new I_LayoutUseBeforeDef rule). The release pipeline now
// verifies ONCE at pipeline end (`verifyEveryPass=false`, the production posture
// — D-OPT1-VERIFY-FREQUENCY-CONFIG), so result.ok implies the FINAL module is
// verifier-clean; the explicit final verify below is the belt-and-suspenders
// MIR-tier capstone.
// (The CORPUS rows release_pipeline_recursion / release_pipeline_loop
// carry the runtime exit-42 proof across all four targets; this is the
// target-blind MIR-tier twin.)
TEST(Optimizer, ShippedReleasePipelineCompilesBothProbeShapes) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& target = **targetR;

    // rec(k){ if(k) return rec(k-1)+1; return 37; } main(){ return rec(5); }
    // — recursion (RC-A: a constant base-case CondBr folds; SimplifyCfg
    // must prune the dead arm in-rebuild). The SCC gate keeps rec
    // out-of-line; main→rec inlines.
    auto buildRecModule = [](TypeInterner& interner) -> Mir {
        TypeId const i32   = interner.primitive(TypeKind::I32);
        TypeId const boolT = interner.primitive(TypeKind::Bool);
        TypeId const params[] = {i32};
        TypeId const recSig  = interner.fnSig(params, i32, CallConv::CcSysV);
        TypeId const mainSig = interner.fnSig({}, i32, CallConv::CcSysV);
        MirBuilder b;
        b.addFunction(recSig, SymbolId{50},
                      SymbolBinding::Global, SymbolVisibility::Default);
        MirBlockId const rEntry = b.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const rThen  = b.createBlock(StructCfMarker::IfThen);
        MirBlockId const rElse  = b.createBlock(StructCfMarker::IfElse);
        b.beginBlock(rEntry);
        MirInstId const kArg = b.addArg(0, i32);
        MirLiteralValue z; z.value = std::int64_t{0}; z.core = TypeKind::I32;
        MirInstId const zero = b.addConst(z, i32);
        MirInstId const neOps[] = {kArg, zero};
        MirInstId const ne = b.addInst(MirOpcode::ICmpNe, neOps, boolT);
        b.addCondBr(ne, rThen, rElse);
        b.beginBlock(rThen);
        MirInstId const selfAddr = b.addGlobalAddr(SymbolId{50}, recSig);
        MirLiteralValue o; o.value = std::int64_t{1}; o.core = TypeKind::I32;
        MirInstId const one = b.addConst(o, i32);
        MirInstId const subOps[] = {kArg, one};
        MirInstId const km1 = b.addInst(MirOpcode::Sub, subOps, i32);
        MirInstId const recOps[] = {selfAddr, km1};
        MirInstId const recCall = b.addInst(MirOpcode::Call, recOps, i32);
        MirInstId const one2 = b.addConst(o, i32);
        MirInstId const addOps[] = {recCall, one2};
        MirInstId const sum = b.addInst(MirOpcode::Add, addOps, i32);
        b.addReturn(sum);
        b.beginBlock(rElse);
        MirLiteralValue t; t.value = std::int64_t{37}; t.core = TypeKind::I32;
        b.addReturn(b.addConst(t, i32));
        b.addFunction(mainSig, SymbolId{100},
                      SymbolBinding::Global, SymbolVisibility::Default);
        MirBlockId const mEntry = b.createBlock(StructCfMarker::EntryBlock);
        b.beginBlock(mEntry);
        MirInstId const recAddr = b.addGlobalAddr(SymbolId{50}, recSig);
        MirLiteralValue five; five.value = std::int64_t{5}; five.core = TypeKind::I32;
        MirInstId const arg5 = b.addConst(five, i32);
        MirInstId const callOps[] = {recAddr, arg5};
        MirInstId const call = b.addInst(MirOpcode::Call, callOps, i32);
        b.addReturn(call);
        return std::move(b).finish();
    };

    // f(k){ int acc=37; while(k){acc++; k--;} return acc; } (alloca-form)
    // main(){ return f(5); } — loop (RC-B: the multi-block splice's
    // continuation consumes a clone-defined value; C1 lays it out
    // topologically).
    auto buildLoopModule = [](TypeInterner& interner) -> Mir {
        TypeId const i32   = interner.primitive(TypeKind::I32);
        TypeId const boolT = interner.primitive(TypeKind::Bool);
        TypeId const i32p  = interner.pointer(i32);
        TypeId const params[] = {i32};
        TypeId const fSig    = interner.fnSig(params, i32, CallConv::CcSysV);
        TypeId const mainSig = interner.fnSig({}, i32, CallConv::CcSysV);
        MirBuilder b;
        b.addFunction(fSig, SymbolId{50},
                      SymbolBinding::Global, SymbolVisibility::Default);
        MirBlockId const fEntry  = b.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const fHeader = b.createBlock(StructCfMarker::LoopHeader);
        MirBlockId const fBody   = b.createBlock(StructCfMarker::Linear);
        MirBlockId const fExit   = b.createBlock(StructCfMarker::Linear);
        b.beginBlock(fEntry);
        MirInstId const kArg    = b.addArg(0, i32);
        MirInstId const accSlot = b.addInst(MirOpcode::Alloca, {}, i32p);
        MirInstId const kSlot   = b.addInst(MirOpcode::Alloca, {}, i32p);
        MirInstId const st0[] = {kArg, kSlot};
        (void)b.addInst(MirOpcode::Store, st0, InvalidType);
        MirLiteralValue t; t.value = std::int64_t{37}; t.core = TypeKind::I32;
        MirInstId const c37 = b.addConst(t, i32);
        MirInstId const st1[] = {c37, accSlot};
        (void)b.addInst(MirOpcode::Store, st1, InvalidType);
        b.addBr(fHeader);
        b.beginBlock(fHeader);
        MirInstId const kv = b.addInst(MirOpcode::Load, std::array{kSlot}, i32);
        MirLiteralValue z; z.value = std::int64_t{0}; z.core = TypeKind::I32;
        MirInstId const zero = b.addConst(z, i32);
        MirInstId const ne = b.addInst(MirOpcode::ICmpNe, std::array{kv, zero}, boolT);
        b.addCondBr(ne, fBody, fExit);
        b.beginBlock(fBody);
        MirInstId const a = b.addInst(MirOpcode::Load, std::array{accSlot}, i32);
        MirLiteralValue o; o.value = std::int64_t{1}; o.core = TypeKind::I32;
        MirInstId const one = b.addConst(o, i32);
        MirInstId const a1 = b.addInst(MirOpcode::Add, std::array{a, one}, i32);
        MirInstId const sta[] = {a1, accSlot};
        (void)b.addInst(MirOpcode::Store, sta, InvalidType);
        MirInstId const kk = b.addInst(MirOpcode::Load, std::array{kSlot}, i32);
        MirInstId const one2 = b.addConst(o, i32);
        MirInstId const kk1 = b.addInst(MirOpcode::Sub, std::array{kk, one2}, i32);
        MirInstId const stk[] = {kk1, kSlot};
        (void)b.addInst(MirOpcode::Store, stk, InvalidType);
        b.addBr(fHeader);
        b.beginBlock(fExit);
        MirInstId const rv = b.addInst(MirOpcode::Load, std::array{accSlot}, i32);
        b.addReturn(rv);
        b.addFunction(mainSig, SymbolId{100},
                      SymbolBinding::Global, SymbolVisibility::Default);
        MirBlockId const mEntry = b.createBlock(StructCfMarker::EntryBlock);
        b.beginBlock(mEntry);
        MirInstId const fAddr = b.addGlobalAddr(SymbolId{50}, fSig);
        MirLiteralValue five; five.value = std::int64_t{5}; five.core = TypeKind::I32;
        MirInstId const arg5 = b.addConst(five, i32);
        MirInstId const callOps[] = {fAddr, arg5};
        MirInstId const call = b.addInst(MirOpcode::Call, callOps, i32);
        b.addReturn(call);
        return std::move(b).finish();
    };

    auto pipelineR = opt::loadShippedPipeline("release");
    ASSERT_TRUE(pipelineR.has_value())
        << "shipped release.pipeline.json must load";

    // Probe 1: recursion (RC-A).
    {
        TypeInterner interner{CompilationUnitId{1}};
        Mir mir = buildRecModule(interner);
        // Canonical markers (what a real frontend stamps post-lowering) so
        // the test isolates the optimizer, not hand-marker bookkeeping.
        rederiveStructCfMarkers(mir);
        DiagnosticReporter rep;
        auto const result = opt::optimize(mir, target, interner, *pipelineR, rep);
        EXPECT_TRUE(result.ok)
            << "the SHIPPED release pipeline must optimize the recursion "
               "probe shape (RC-A) without error";
        EXPECT_EQ(rep.errorCount(), 0u);
        EXPECT_GE(result.mutationCount(opt::PassId::Inlining), 1u)
            << "the SHIPPED release pipeline must actually RUN Inlining on "
               "the recursion probe (main->rec inlines via the cross-SCC "
               "edge) — proves this arm EXERCISES the composition that broke, "
               "not a vacuous no-op that would mask the RC-A/RC-B fix";
        MirVerifier verifier{mir, &interner};
        EXPECT_TRUE(verifier.verify(rep))
            << "post-release-pipeline recursion module must be verifier-clean";
    }

    // Probe 2: loop (RC-B).
    {
        TypeInterner interner{CompilationUnitId{1}};
        Mir mir = buildLoopModule(interner);
        rederiveStructCfMarkers(mir);
        DiagnosticReporter rep;
        auto const result = opt::optimize(mir, target, interner, *pipelineR, rep);
        EXPECT_TRUE(result.ok)
            << "the SHIPPED release pipeline must optimize the loop probe "
               "shape (RC-B) without error";
        EXPECT_EQ(rep.errorCount(), 0u);
        EXPECT_GE(result.mutationCount(opt::PassId::Inlining), 1u)
            << "the SHIPPED release pipeline must actually RUN Inlining on "
               "the loop probe (main->f inlines the multi-block leaf) — "
               "proves this arm EXERCISES the RC-B splice, not a vacuous "
               "no-op that would mask the fix";
        MirVerifier verifier{mir, &interner};
        EXPECT_TRUE(verifier.verify(rep))
            << "post-release-pipeline loop module must be verifier-clean";
    }
}
