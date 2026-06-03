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
