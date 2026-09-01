// ★★★ THE INSTRUMENT'S OWN PIN —
// D-LIR-TEST-FRONT-END-LOWERS-A-MANY-ARG-CALL-TO-NOTHING-SO-PINS-MEASURE-ZERO.
//
// `lowered_lir_fixture.hpp`'s `lowerCToLir` is the front end that ~20 files in
// `tests/lir` and `tests/asm` drive. It used to return a `LoweredLir` and say
// nothing about it: three reporters and three `ok` flags rode inside the struct,
// no caller read the first two tiers, and a source the pipeline REFUSED came
// back looking exactly like a source it lowered. Every assertion a pin then made
// was an assertion about a MUTILATED module — a "no bad instruction appears"
// check passing because the construct under test was simply absent.
//
// ✔MEASURED (lane `lt`, P49), and it REFUTES the row's own diagnosis twice:
//
//   * THE FRONT END WAS NEVER SILENT. `double g(double,…); double f(void){
//     return g(…); }` reached LIR as ONE instruction — with `HirToMirResult.ok`
//     FALSE and TWO `H_UnsupportedLoweringForKind` errors on `mirReporter`, the
//     second of which even names the fix ("tests must attach the attribute
//     manually until then"). The row records "zero diagnostics emitted"; the
//     lanes that measured it read `lowerOk`/`allocOk`/`rewriteOk` — the LIR and
//     regalloc verdicts — and never the MIR one. The silence was the FIXTURE's.
//
//   * THE TRIGGER IS NOT ARGUMENT COUNT, NOT STACK PRESSURE, AND NOT "SHAPES
//     THE FRONT END CANNOT BUILD". It is the PRESENCE OF A PROTOTYPE. A
//     prototype mints an `ExternFunction` node; the fixture passed `ffiMap =
//     nullptr`; the HIR→MIR extern pre-pass refuses a node with no
//     `FfiMetadata`, drops the extern row, and the CALL to it then fails again
//     as an "HIR Ref to unbound symbol" — taking the whole call statement with
//     it. `SameEightyDoublesAsParametersAlwaysLowered` below is the control that
//     settles it: the identical 80 doubles as PARAMETERS of a definition (no
//     prototype ⇒ no extern node) always lowered clean, on the same fixture, at
//     the same argument count.
//
// So this file pins the two halves of the closure, in BOTH directions each:
//   (A) the fixture REPORTS a refusal, and reports a MISSING refusal too;
//   (B) a legitimately empty body is still ACCEPTED — the false-red control;
//   (C) the shapes the row was filed over now lower to a POSITIVE, large
//       instruction count, which is what goes red if the FFI map is unthreaded.

#include "lir/lir.hpp"
#include "lowered_lir_fixture.hpp"
#include "mir/mir.hpp"

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <format>
#include <string>

using namespace dss;
using namespace dss::test_support;

namespace {

// ── SOURCES ─────────────────────────────────────────────────────────────────

// The row's filing shape (lane `fo`): a prototype plus an 80-argument call.
[[nodiscard]] std::string eightyArgumentCall() {
    std::string proto = "double g(";
    std::string call  = "double f(void) {\n  return g(";
    for (int i = 0; i < 80; ++i) {
        if (i != 0) { proto += ","; call += ","; }
        proto += "double";
        call  += std::format("{}.0", i + 1);
    }
    return proto + ");\n" + call + ");\n}\n";
}

// The row's P47 amendment shape (lane `cw`): ONE argument per call, four values
// live across later calls. The CLI spills it and saves an xmm; this fixture used
// to hand back six instructions with an empty saved-register set.
[[nodiscard]] std::string oneArgumentCallsWithValuesLiveAcross() {
    return "double k(double);\n"
           "double f(double x) {\n"
           "  double a = k(x);\n"
           "  double b = k(a);\n"
           "  double c = k(b);\n"
           "  double d = k(c);\n"
           "  return a + b + c + d;\n"
           "}\n";
}

// The CONTROL for both of the above: the same eighty doubles, as PARAMETERS of a
// definition. No prototype, so no `ExternFunction` node, so nothing to refuse.
[[nodiscard]] std::string eightyDoubleParameters() {
    std::string s = "double f(";
    for (int i = 0; i < 80; ++i) {
        if (i != 0) s += ",";
        s += std::format("double p{}", i);
    }
    return s + ") {\n  return p8 + p79;\n}\n";
}

// A source a tier genuinely REFUSES, for the arms about reporting. The subject
// is the refusal's VISIBILITY, not its content: a `+` in the INPUT section has
// no output entry and no lvalue address, so the write half cannot exist, and
// `tieAsmReadWriteOperands` says so at LIR (D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE).
constexpr char const* kRefusedSource =
    "void f(int *p, int v){ __asm__(\"nop\" : : \"+r\"(*p), \"r\"(v)); }";

// ── HELPERS ─────────────────────────────────────────────────────────────────

[[nodiscard]] std::uint32_t soleFunctionInstCount(LoweredLir const& out) {
    Lir const& lir = out.lir.lir;
    if (lir.moduleFuncCount() != 1u) return 0;
    auto const f = lir.funcAt(0);
    std::uint32_t n = 0;
    for (std::uint32_t b = 0; b < lir.funcBlockCount(f); ++b)
        n += lir.blockInstCount(lir.funcBlockAt(f, b));
    return n;
}

// ── (A) THE FIXTURE REPORTS A REFUSAL — AND REPORTS A MISSING ONE ───────────
//
// ⚠⚠ THESE ARMS ASSERT THAT `lowerCToLir` FAILS, which is why they run it
// inside `EXPECT_NONFATAL_FAILURE`. An instrument whose whole purpose is to
// report is worth nothing until it has been SEEN to report; a pin that merely
// drove a refused source and then checked `lir.ok` would be testing the
// pipeline, not the fixture, and would have passed before this row was closed.

TEST(LirFixtureVacuityGuard, ARefusedSourceIsReportedByTheFixtureItself) {
    EXPECT_NONFATAL_FAILURE(
        { auto const out = lowerCToLir(kRefusedSource, "x86_64"); (void)out; },
        "A TIER REFUSED THIS SOURCE");
}

TEST(LirFixtureVacuityGuard, TheReportNamesTheRefusingTierAndItsDiagnosticCode) {
    // The message must carry the SYMBOLIC CODE, not just a count: a reader of
    // the red needs to know WHICH refusal fired before deciding whether the pin
    // wants a different source, hand-built MIR, or `Refuses`.
    EXPECT_NONFATAL_FAILURE(
        { auto const out = lowerCToLir(kRefusedSource, "x86_64"); (void)out; },
        "L_UnsupportedLoweringForOpcode");
}

TEST(LirFixtureVacuityGuard, ADeclaredRefusalIsAcceptedWithoutFailing) {
    // The opt-in direction: a NEGATIVE pin says `Refuses` and is left alone.
    auto const out = lowerCToLir(kRefusedSource, "x86_64", /*mirCcIndex=*/0,
                                 LoweringExpectation::Refuses);
    EXPECT_FALSE(out.lir.ok)
        << "the source chosen for these arms must still be one a tier refuses — "
           "if this ever lowers clean the arms above are measuring nothing";
}

TEST(LirFixtureVacuityGuard, ARefusalPinThatStoppedRefusingGoesRed) {
    // ★ THE SECOND DIRECTION, and the reason `Refuses` is an ASSERTION rather
    // than a suppression. Without this arm, `Refuses` would be a way to make any
    // pin green — indistinguishable from the silence this row exists to end.
    EXPECT_NONFATAL_FAILURE(
        {
            auto const out =
                lowerCToLir("int f(int a) { return a + 1; }\n", "x86_64",
                            /*mirCcIndex=*/0, LoweringExpectation::Refuses);
            (void)out;
        },
        "declared LoweringExpectation::Refuses");
}

// ── (B) THE FALSE-RED CONTROLS — A LEGITIMATELY EMPTY BODY IS ACCEPTED ──────
//
// ★ THE PREDICATE IS "A DEFINITION SURVIVED", NOT "IT PRODUCED ENOUGH
// INSTRUCTIONS", and these two arms are why. Both bodies below are correct C
// that lowers to exactly ONE LIR instruction — the function's own terminator —
// so any refusal keyed on an instruction-count threshold above 1 reddens honest
// work. A count threshold would ALSO have missed both shapes this row was filed
// over: the 80-argument call survived as 1 instruction and the spilling function
// as 6, neither of them zero.

TEST(LirFixtureVacuityGuard, AnEmptyFunctionBodyIsNotAVacuousLowering) {
    auto const out = lowerCToLir("void f(void) { }\n", "x86_64");
    EXPECT_EQ(soleFunctionInstCount(out), 1u)
        << "an empty body is one `ret` — if this ever grows, re-read the "
           "controls above before trusting the threshold argument they make";
}

TEST(LirFixtureVacuityGuard, ABodyWhoseOnlyStatementIsAReturnIsNotVacuous) {
    auto const out = lowerCToLir("void f(void) { return; }\n", "x86_64");
    EXPECT_EQ(soleFunctionInstCount(out), 1u)
        << "one statement, one instruction: a body with statements does not owe "
           "a second instruction, so the survival predicate cannot count them";
}

// ── (C) THE SHAPES THE ROW WAS FILED OVER NOW LOWER FOR REAL ───────────────
//
// ⚠ EVERY ARM HERE ASSERTS A POSITIVE COUNT, which is the interim discipline
// this row imposed on `tests/lir` and the discipline that CAUGHT it: lane `cw`'s
// first draft reddened on its own positive-count assertion rather than passing
// over an empty list. These are the arms that go red if the FFI map is
// unthreaded from `lowerCToLir` — the REMOVE-direction mutant for this closure.
//
// The counts are floors, not equalities: the exact instruction count is the
// lowerer's business and would make this file a brake on every future codegen
// improvement. What is pinned is that the call SURVIVED — the pre-fix value was
// 1 on both shipped targets, which no floor here can be confused with.

TEST(LirFixtureVacuityGuard, AnEightyArgumentCallSurvivesToLir) {
    for (char const* targetName : {"x86_64", "arm64"}) {
        auto const out = lowerCToLir(eightyArgumentCall(), targetName);
        EXPECT_GT(soleFunctionInstCount(out), 80u)
            << targetName
            << ": eighty arguments must each reach the callee somehow, so the "
               "function cannot be smaller than its argument list. Before this "
               "row closed it was ONE instruction, with the two MIR errors "
               "explaining why sitting unread inside the returned struct.";
    }
}

TEST(LirFixtureVacuityGuard, AOneArgumentCallChainSurvivesToLir) {
    // The P47 amendment's shape. The row generalised from it to "the front end
    // does not build the shapes its fixtures inspect"; the real common factor
    // with the arm above is narrower and cheaper — both sources declare a
    // prototype.
    auto const out = lowerCToLir(oneArgumentCallsWithValuesLiveAcross(), "x86_64");
    EXPECT_GT(soleFunctionInstCount(out), 6u)
        << "four calls and three adds cannot be six instructions — six is what "
           "this fixture returned while the four calls were being dropped";
}

TEST(LirFixtureVacuityGuard, SameEightyDoublesAsParametersAlwaysLowered) {
    // ★★ THE CONTROL THAT REFUTES THE ROW'S DIAGNOSIS. Eighty doubles, same
    // count, same target, same fixture — but as PARAMETERS of a definition, so
    // the source declares no prototype and mints no `ExternFunction` node. This
    // arm passed identically BEFORE the closure. Argument pressure was never the
    // trigger, and a fix aimed at argument count would have changed nothing.
    auto const out = lowerCToLir(eightyDoubleParameters(), "arm64");
    EXPECT_GT(soleFunctionInstCount(out), 80u)
        << "the control must lower whether or not the closure is in place — if "
           "this ever reddens, the diagnosis in this file's banner is wrong";
}

// ── (D) THE STRUCTURAL ARM — A BODY MAY NOT VANISH IN SILENCE ──────────────

// ★★★ AND IT IS PROVEN ABLE TO FAIL, BY SYNTHESIZING THE NEGATIVE
// ([[feedback-a-fixture-must-synthesize-the-negative]]). No C source available
// to this fixture makes a definition vanish with `ok` true and no diagnostic —
// that would be a live front-end defect — so the two arms below BUILD the state
// instead: lower a source with one definition, then graft on the tier result
// from a DONOR source that has no functions at all. HIR still says a definition
// exists; the grafted tier says it does not; the check must say so. Removing
// either branch of `enforceLoweringExpectation` reddens exactly one of these.
//
// ⚠ A REMOVE-direction construction on purpose: the donor's tier result is a
// module the pipeline really produced with FEWER functions, not a hand-forged
// "impossible" object. An ADD-direction fixture (grafting something IN) would
// pass over a check that had been deleted outright.

TEST(LirFixtureVacuityGuard, AHirDefinitionMissingFromMirIsReported) {
    auto donor = lowerCToLir("int g;\n", "x86_64");
    ASSERT_EQ(donor.mir.mir.moduleFuncCount(), 0u)
        << "the donor must carry no functions, or it grafts a body back in";
    auto out = lowerCToLir("int f(int a) { return a + 1; }\n", "x86_64");
    out.mir = std::move(donor.mir);
    EXPECT_NONFATAL_FAILURE(
        fixture_detail::enforceLoweringExpectation(out, LoweringExpectation::Lowers),
        "that HIR->MIR dropped");
}

TEST(LirFixtureVacuityGuard, AMirBodyLoweredToNoInstructionsIsReported) {
    auto donor = lowerCToLir("int g;\n", "x86_64");
    ASSERT_EQ(donor.lir.lir.moduleFuncCount(), 0u)
        << "the donor must carry no LIR functions, or it grafts a body back in";
    auto out = lowerCToLir("int f(int a) { return a + 1; }\n", "x86_64");
    out.lir = std::move(donor.lir);
    EXPECT_NONFATAL_FAILURE(
        fixture_detail::enforceLoweringExpectation(out, LoweringExpectation::Lowers),
        "lowered to NO instructions");
}

TEST(LirFixtureVacuityGuard, EveryHirFunctionDefinitionReachesLirWithInstructions) {
    // The second, independent half of the refusal: not observed firing today,
    // and checked anyway, because a future lowering that drops a function
    // WITHOUT reporting is exactly the shape half (A) can no longer hide. Two
    // definitions so the check is over a set, not a singleton.
    auto const out = lowerCToLir(
        "static int helper(int a) { return a * 3; }\n"
        "int f(int a) { return helper(a) + 1; }\n",
        "x86_64");
    Lir const& lir = out.lir.lir;
    ASSERT_EQ(lir.moduleFuncCount(), 2u)
        << "both definitions must reach LIR — a dropped one is the defect";
    for (std::uint32_t i = 0; i < lir.moduleFuncCount(); ++i) {
        auto const f = lir.funcAt(i);
        std::uint32_t n = 0;
        for (std::uint32_t b = 0; b < lir.funcBlockCount(f); ++b)
            n += lir.blockInstCount(lir.funcBlockAt(f, b));
        EXPECT_GT(n, 0u) << "LIR function " << i << " has an empty body";
    }
}

} // namespace
