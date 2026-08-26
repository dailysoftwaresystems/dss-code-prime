// THE PER-CALLER CUMULATIVE INLINE GROWTH BUDGET, and the LOUD TRUNCATION
// REPORT that stops the fixpoint's iteration cap from being a silent ceiling.
//
// Closes D-OPT-INLINING-FIXPOINT-TRUNCATES-BEFORE-CONVERGING and
// D-OPT-FIXPOINT-CONVERGENCE-IS-COMPUTED-AND-DISCARDED.
//
// ★★★ WHAT WAS ACTUALLY WRONG, because the row's own prescription was wrong
// and these tests are shaped by the correction. `inlineThreshold` is a
// PER-CALLEE size bound: it refuses ONE callee that is too big. NOTHING
// bounded CUMULATIVE growth — twenty callees of 49 instructions each are
// twenty legal inlines under a threshold of 50. So the only thing standing
// between the release pipeline and unbounded code growth was the fixpoint's
// ITERATION CAP, and ✔MEASURED on the 103-TU sqlite corpus the per-iteration
// splice count converges on EXACTLY 2x per iteration — the fixpoint does not
// converge slowly, it DIVERGES. Raising `max`, which is what the row
// prescribed, trades a working compiler for an out-of-memory kill.
//
// So the pins below are about a BOUND, not about an iteration count:
//   * the budget REFUSES the (k+1)th site in a caller and admits the first k,
//     in a ladder across three growth percentages — non-vacuous in both
//     directions, since the same fixture inlines ALL sites when the budget is
//     opened up and exactly ONE when it is closed down;
//   * `always_inline` WAIVES the budget, as it already waives the per-callee
//     threshold, and is still CHARGED so it cannot be a free-growth channel;
//   * ★ THE LOAD-BEARING ONE: the budget is CUMULATIVE ACROSS FIXPOINT
//     ITERATIONS. A budget re-baselined per invocation compounds
//     `(1 + g)^iterations` and hands the iteration cap straight back its role
//     as the real growth bound — the exact defect being closed. This pin is
//     RED if the engine is ever switched to the single-invocation overload;
//   * a truncated fixpoint SAYS SO (`X_OptFixpointTruncated`, Warning), and a
//     converged one stays silent.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_verifier.hpp"
#include "opt/optimizer.hpp"
#include "opt/passes/inlining.hpp"
#include "diagnostic_count.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>

using namespace dss;
using dss::test_support::countCode;

namespace {

// Same three-line shape as `test_inlining.cpp`'s. Duplicated rather than
// hoisted: two occurrences is below this tree's hoist threshold, and a shared
// header for a struct literal would cost more to find than to retype.
[[nodiscard]] MirLiteralValue i32Lit(std::int64_t v) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = TypeKind::I32;
    return lit;
}

// ── the fixture ───────────────────────────────────────────────────────
//
// ONE caller (`SymbolId{100}`) making `nCalls` DIRECT calls to ONE
// single-block leaf callee (`SymbolId{50}`) of `calleeInsts` instructions.
// The callee's body is `calleeInsts - 1` Consts plus a Return: Consts are
// inert, so nothing but the cost model can refuse the splice, and the
// per-site refusal under test is unambiguously the BUDGET's.
//
// Sizes, stated because every expectation below is arithmetic on them and a
// reader must be able to redo it:
//   callee = calleeInsts
//   caller = nCalls GlobalAddr + nCalls Call + 1 Return = 2*nCalls + 1
[[nodiscard]] Mir buildRepeatedCallModule(TypeInterner& interner,
                                          std::uint32_t calleeInsts,
                                          std::uint32_t nCalls,
                                          bool calleeAlwaysInline = false) {
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    mb.addFunction(fnSig, SymbolId{50}, SymbolBinding::Global,
                   SymbolVisibility::Default, /*noInline=*/false,
                   calleeAlwaysInline);
    MirBlockId const cEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(cEntry);
    MirInstId last{};
    for (std::uint32_t i = 0; i + 1 < calleeInsts; ++i) {
        last = mb.addConst(i32Lit(static_cast<std::int64_t>(i) + 1), i32);
    }
    mb.addReturn(last);

    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    MirInstId lastCall{};
    for (std::uint32_t i = 0; i < nCalls; ++i) {
        MirInstId const calleeAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
        MirInstId const ops[] = {calleeAddr};
        lastCall = mb.addInst(MirOpcode::Call, ops, i32);
    }
    mb.addReturn(lastCall);
    return std::move(mb).finish();
}

[[nodiscard]] std::uint32_t countOpcodeIn(Mir const& mir, std::uint32_t sym,
                                          MirOpcode op) {
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        if (mir.funcSymbol(f).v != sym) continue;
        std::uint32_t n = 0;
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t ii = 0; ii < ni; ++ii) {
                if (mir.instOpcode(mir.blockInstAt(b, ii)) == op) ++n;
            }
        }
        return n;
    }
    return 0;
}

// The fixture's numbers, chosen so the ladder below lands on DIFFERENT
// answers at each rung rather than on the same one twice.
constexpr std::uint32_t kCalleeInsts = 6;
constexpr std::uint32_t kNCalls      = 5;
constexpr std::uint32_t kThreshold   = kCalleeInsts;   // callee exactly fits
constexpr std::uint32_t kCallerInsts = 2 * kNCalls + 1;  // 11

// One single-invocation run. Returns {inlined, budgeted}.
[[nodiscard]] std::pair<std::size_t, std::size_t>
runOnce(std::uint32_t growthPercent, bool alwaysInline = false) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildRepeatedCallModule(interner, kCalleeInsts, kNCalls,
                                      alwaysInline);
    EXPECT_EQ(countOpcodeIn(mir, 100, MirOpcode::Call), kNCalls)
        << "fixture drift: the caller must start with exactly kNCalls calls";
    DiagnosticReporter rep;
    opt::passes::InlineGrowthLedger ledger;
    auto const r = opt::passes::runInlining(mir, interner, rep, kThreshold,
                                            growthPercent, ledger);
    EXPECT_TRUE(r.ok);
    MirVerifier verifier{mir, &interner};
    EXPECT_TRUE(verifier.verify(rep))
        << "a budget-refused splice must leave a VERIFIABLE module — the "
           "refusal path is the ordinary 'leave the Call as-is' path";
    return {r.callsInlined, r.callsBudgeted};
}

} // namespace

// ── the ladder ────────────────────────────────────────────────────────
//
// ceiling = original + max(original * pct / 100, threshold), with
// original = 11, threshold = 6, and each admitted splice charging the
// callee's 6 instructions against a `projected` that starts at 11:
//   pct = 0    -> allowance max(0, 6)  = 6  -> ceiling 17 -> 11,17     -> 1
//   pct = 200  -> allowance max(22, 6) = 22 -> ceiling 33 -> 11..29    -> 3
//   pct = MAX  -> allowance enormous         -> ceiling huge           -> 5
//
// ★ THREE RUNGS, NOT TWO, AND THAT IS THE POINT. A two-rung test (all vs
// nothing) is satisfied by a budget that is merely an on/off switch. The
// middle rung is the one that can only pass if the budget is actually being
// SPENT per site, in order, and compared against a ceiling.
TEST(InlineGrowthBudget, RefusesPerCallerOnceTheCeilingIsReached) {
    auto const [inlined0, budgeted0] = runOnce(0);
    EXPECT_EQ(inlined0, 1u)
        << "growth 0 still admits ONE maximal callee — the allowance floors "
           "at inlineThreshold, or the per-callee threshold would be a lie "
           "for every small caller";
    EXPECT_EQ(budgeted0, kNCalls - 1)
        << "every other site is refused BY THE BUDGET, and says so";

    auto const [inlined200, budgeted200] = runOnce(200);
    EXPECT_EQ(inlined200, 3u)
        << "allowance 22 over an original of 11 admits three 6-instruction "
           "callees (11 -> 17 -> 23 -> 29, ceiling 33) and refuses the fourth";
    EXPECT_EQ(budgeted200, 2u);

    auto const [inlinedMax, budgetedMax] = runOnce(opt::kMaxInlineCallerGrowthPercent);
    EXPECT_EQ(inlinedMax, kNCalls)
        << "NON-VACUITY: the same fixture inlines EVERY site when the budget "
           "is opened, so the refusals above are the budget's and not some "
           "other gate rule quietly declining";
    EXPECT_EQ(budgetedMax, 0u);
}

// `always_inline` overrides PROFITABILITY vetoes, and a cumulative growth
// bound is one — the same reasoning that lets it waive `inlineThreshold`
// (TF-C81). It is still CHARGED, so it cannot become a channel through which
// a caller grows without the budget noticing.
TEST(InlineGrowthBudget, AlwaysInlineWaivesTheBudgetButIsStillCharged) {
    auto const [inlined, budgeted] = runOnce(0, /*alwaysInline=*/true);
    EXPECT_EQ(inlined, kNCalls)
        << "every site inlines despite a growth budget that admits one";
    EXPECT_EQ(budgeted, 0u)
        << "an always_inline site is never REFUSED, so it never reports as "
           "budget-refused";

    // The paired negative is the pct=0 rung of the ladder above, on the
    // byte-identical module with the flag CLEAR: 1 inlined, 4 budgeted. Two
    // fixtures differing only in the attribute is what makes this a pin on
    // the ATTRIBUTE rather than on the fixture.
}

// ★★★ THE ONE THAT MATTERS. The budget must span the WHOLE optimize() call.
//
// Re-baselining per invocation is not a small error: at growth g it compounds
// to `(1 + g)^iterations`, which puts the iteration cap back in charge of how
// big the program gets — precisely
// D-OPT-INLINING-FIXPOINT-TRUNCATES-BEFORE-CONVERGING.
//
// Arithmetic, so the margin is visible. Fixture: caller 11, callee 6,
// threshold 6, growth 0 => allowance 6, ceiling 17. A splice removes the Call
// and copies the callee's 5 non-terminator instructions, so the caller grows
// by 4 per splice.
//   CUMULATIVE (correct): iter 1 admits one site (11 -> 17 projected), caller
//     becomes 15. iter 2 measures against the SAME original 11, ceiling 17,
//     current 15, next charge 6 -> 21 > 17 -> refused -> nothing mutated ->
//     the fixpoint CONVERGES. Total splices: 1.
//   RE-BASELINED (the bug): iter 2 takes original = 15, ceiling 21, current
//     15, charge 6 -> 21 <= 21 -> admits. And again, and again: one splice per
//     iteration until `max` runs out. Total splices: 4.
// 1 versus 4 on a four-iteration cap, and the gap widens with the cap — which
// is the whole property being pinned.
TEST(InlineGrowthBudget, BudgetIsCumulativeAcrossFixpointIterations) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& target = **targetR;
    TypeInterner interner{CompilationUnitId{1}};

    Mir mir = buildRepeatedCallModule(interner, kCalleeInsts, kNCalls);
    ASSERT_EQ(countOpcodeIn(mir, 100, MirOpcode::Call), kNCalls);

    opt::OptPipeline pipe;
    pipe.name = "budget-fixpoint";
    pipe.inlineThreshold = kThreshold;
    pipe.inlineCallerGrowthPercent = 0;
    pipe.schedule = opt::OptPipelineNode::fixpoint(
        4u, {opt::OptPipelineNode::leaf(opt::PassId::Inlining)});

    DiagnosticReporter rep;
    auto const res = opt::optimize(mir, target, interner, pipe, rep);
    ASSERT_TRUE(res.ok);

    EXPECT_EQ(countOpcodeIn(mir, 100, MirOpcode::Call), kNCalls - 1)
        << "EXACTLY ONE call was spliced across all four iterations. More "
           "than one means the growth budget re-baselined against each "
           "iteration's entry size instead of the caller's original size, "
           "which compounds and puts the iteration cap back in charge of "
           "code growth";
    EXPECT_EQ(res.mutationCount(opt::PassId::Inlining), 1u)
        << "Inlining mutated on iteration 1 only; iteration 2 found the "
           "budget already spent and returned mutated=false";
    EXPECT_TRUE(res.fixedPointReached)
        << "with the growth budget in place the fixpoint TERMINATES on its "
           "own — it stops because there is nothing left it is willing to "
           "do, not because it ran out of iterations";
}

// The budget is a bound on GROWTH, not a refusal of inlining. A caller with
// room inlines exactly as it did before the budget existed — otherwise the
// ladder above would be satisfied by a pass that had simply been disabled.
TEST(InlineGrowthBudget, ShippedDefaultStillInlinesAWrapper) {
    TypeInterner interner{CompilationUnitId{1}};
    // A 3-instruction wrapper calling a 30-instruction helper: relative
    // growth alone (30% of 3 == 0) would refuse it forever. The floor at
    // `inlineThreshold` is what admits it, and wrappers are where inlining
    // pays most.
    Mir mir = buildRepeatedCallModule(interner, 30, 1);
    DiagnosticReporter rep;
    opt::passes::InlineGrowthLedger ledger;
    auto const r = opt::passes::runInlining(
        mir, interner, rep, /*inlineThreshold=*/30,
        opt::kDefaultInlineCallerGrowthPercent, ledger);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.callsInlined, 1u)
        << "the allowance floors at inlineThreshold so a tiny caller can "
           "still absorb one maximal legal callee";
    EXPECT_EQ(r.callsBudgeted, 0u);
}

// ── the truncation report ─────────────────────────────────────────────
//
// D-OPT-FIXPOINT-CONVERGENCE-IS-COMPUTED-AND-DISCARDED: `fixedPointReached`
// was computed and had ZERO consumers, so the compiler detected that it had
// capped an unconverged optimization fixpoint and said nothing.
//
// ⚠ THE CLOSING WORK IS NOT "FAIL THE BUILD ON TRUNCATION". Before the growth
// budget existed the release fixpoint truncated on every real program, so a
// consumer that failed the build would have refused everything. It is a
// WARNING that names the node, its cap, and the fact that it was still
// mutating — a policy whose cost is visible instead of a silent ceiling.
TEST(FixpointTruncation, CappedUnconvergedFixpointReportsItself) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& target = **targetR;
    TypeInterner interner{CompilationUnitId{1}};

    Mir mir = buildRepeatedCallModule(interner, kCalleeInsts, kNCalls);
    opt::OptPipeline pipe;
    pipe.name = "cap1";
    pipe.inlineThreshold = kThreshold;
    pipe.inlineCallerGrowthPercent = opt::kMaxInlineCallerGrowthPercent;
    // ONE traversal over a pass that WILL mutate ⇒ the cap is reached with
    // the module still changing: truncation by construction.
    pipe.schedule = opt::OptPipelineNode::fixpoint(
        1u, {opt::OptPipelineNode::leaf(opt::PassId::Inlining)});

    DiagnosticReporter rep;
    auto const res = opt::optimize(mir, target, interner, pipe, rep);
    ASSERT_TRUE(res.ok) << "a truncated fixpoint is not a FAILED one";
    ASSERT_FALSE(res.fixedPointReached);

    ASSERT_EQ(countCode(rep, DiagnosticCode::X_OptFixpointTruncated), 1u)
        << "the engine must SAY it stopped on the cap rather than on "
           "convergence — this is the whole of the row being closed";
    bool checked = false;
    for (auto const& d : rep.all()) {
        if (d.code != DiagnosticCode::X_OptFixpointTruncated) continue;
        checked = true;
        EXPECT_EQ(d.severity, DiagnosticSeverity::Warning)
            << "a truncated fixpoint emits a CORRECT program; failing the "
               "build here would refuse programs that compile fine today";
        // The three facts the message must carry, so a reader can act:
        // which pipeline, which cap, and that it was STILL MUTATING.
        EXPECT_NE(d.actual.find("cap1"), std::string::npos)
            << "names the pipeline: " << d.actual;
        EXPECT_NE(d.actual.find("cap of 1"), std::string::npos)
            << "names the cap it exhausted: " << d.actual;
        EXPECT_NE(d.actual.find("still mutating"), std::string::npos)
            << "states that it had not converged: " << d.actual;
    }
    EXPECT_TRUE(checked);
    EXPECT_EQ(rep.errorCount(), 0u)
        << "Warning, not Error — the build proceeds";
}

// The paired NEGATIVE, and it is what stops the pin above from being
// satisfied by a compiler that warns unconditionally. Same fixture, same
// pass, a cap large enough to converge ⇒ SILENCE.
TEST(FixpointTruncation, ConvergedFixpointSaysNothing) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& target = **targetR;
    TypeInterner interner{CompilationUnitId{1}};

    Mir mir = buildRepeatedCallModule(interner, kCalleeInsts, kNCalls);
    opt::OptPipeline pipe;
    pipe.name = "converges";
    pipe.inlineThreshold = kThreshold;
    pipe.inlineCallerGrowthPercent = opt::kMaxInlineCallerGrowthPercent;
    pipe.schedule = opt::OptPipelineNode::fixpoint(
        8u, {opt::OptPipelineNode::leaf(opt::PassId::Inlining)});

    DiagnosticReporter rep;
    auto const res = opt::optimize(mir, target, interner, pipe, rep);
    ASSERT_TRUE(res.ok);
    ASSERT_TRUE(res.fixedPointReached)
        << "8 iterations is ample for this module — if this fails the "
           "fixture stopped converging and the negative arm is vacuous";
    EXPECT_EQ(countCode(rep, DiagnosticCode::X_OptFixpointTruncated), 0u)
        << "a converged fixpoint must be SILENT, or the warning carries no "
           "information";
}
