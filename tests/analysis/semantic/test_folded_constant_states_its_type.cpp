// ══ A FOLDED CONSTANT STATES ITS C TYPE, AND A FLOAT ONE IS ROUNDED TO IT ══════════════════
// (cycle P69 round 1, L2, lane `lm`; the review of checkpoint 29, BLOCKER 1 and MAJOR 3.)
//
// The constant evaluators read a folded value's CORE as its C type: an operator computes in
// it (C 6.3.1.8, 6.5.7p3) and a conditional balances its arms by it (6.5.15p5). Two producers
// misstated it:
//   * the float kernel (`applyBinaryFloat`, src/hir/const_eval_arith.hpp, shared by the CST
//     and the HIR evaluator) computed every host-backed operation in binary64 and handed the
//     result on as an F64 — so `16777216.0f + 1.0f` held 2^24 + 1, which no float holds, while
//     DSS's own run time rounds every float operation to float (FLT_EVAL_METHOD 0 on every
//     reference measured, and MIR's meaning of an F32 operation on every target). An integer
//     operand went to it as a signed 64-bit pattern through a double: 2^64 - 1 read as -1, and
//     `0x1000001000000001` rounded twice (to 2^60 + 2^36, then a tie to 2^60) where C converts
//     it once, to 2^60 + 2^37;
//   * the folded-constant rules (`__builtin_types_compatible_p`, `__builtin_offsetof`) were
//     stamped a 64-bit unsigned whatever their type: `__builtin_types_compatible_p(int, long) -
//     1` was 2^64 - 1, where C's `int` (c.lang.json `typesCompatible`) gives -1.
// ✔MEASURED 2026-10-20 on checkpoint 29's bytes (pe64 and ELF, both pipelines, images run):
// `_Static_assert((int)(16777216.0f + 1.0f - 16777216.0f) == 0)` FAILED and `static int s =
// … == 0 ? 42 : 7` RAN 7; `if (__builtin_types_compatible_p(int, long) - 1 >= 0) return 1;
// return 2;` DIED of an illegal instruction (the fold marked `return 2` never reached) and
// its assertion `< 0` FAILED — clang 18.1.3 and gcc 13.3.0 hold every assertion here and run
// every program to C's value. (The integer-operand cells: see the row.)
//
// ★ THE EVALUATION METHOD IS THE TYPE'S, ON EVERY TARGET DSS HAS: the fold rounds each
// operation to the type its core states, which is the run time's rounding by construction.
// No target key states it — a key the folder alone read could disagree with the lowering, the
// silent wrong value this file pins against; it comes with the first target or type whose run
// time evaluates wider, read by the lowering too (the row says so).

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "hir/hir.hpp"
#include "hir/hir_node.hpp"
#include "hir/lowering/cst_to_hir.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

using namespace dss;
using namespace dss::sem_test;

namespace {

[[nodiscard]] SemanticModel analyzeC(std::string const& src, DataModel dm) {
    auto cu = buildShippedUnit("c", {src});
    return analyze(cu, DiagnosticBudget::libraryDefault(), dm,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
}

// Every error of the analysis of `src`, in words; empty when it has none.
[[nodiscard]] std::string analysisErrors(std::string const& src, DataModel dm) {
    std::string out;
    auto const m = analyzeC(src, dm);
    for (auto const& d : m.diagnostics().all())
        if (d.severity == DiagnosticSeverity::Error) out += "\n    error: " + d.actual;
    return out;
}

// How many static assertions of `src` FAIL.
[[nodiscard]] std::size_t failedAssertions(std::string const& src, DataModel dm) {
    std::size_t n = 0;
    auto const m = analyzeC(src, dm);
    for (auto const& d : m.diagnostics().all())
        if (d.code == DiagnosticCode::S_StaticAssertFailed) ++n;
    return n;
}

// How many positions the lowering marks never reached — what it concluded from folding a
// condition; SIZE_MAX when the unit does not lower.
[[nodiscard]] std::size_t positionsMarkedNeverReached(std::string const& src, DataModel dm) {
    auto cu = buildShippedUnit("c", {src});
    SemanticModel model = analyze(cu, DiagnosticBudget::libraryDefault(), dm,
                                  AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    if (model.hasErrors()) return SIZE_MAX;
    DiagnosticReporter reporter{};
    auto lowered = lowerToHir(model, reporter);
    if (lowered == nullptr || reporter.errorCount() != 0) return SIZE_MAX;
    Hir const& hir = lowered->hir;
    std::size_t marks = 0;
    for (std::uint32_t i = 1; i < hir.nodeCount(); ++i)
        if (hir.kind(HirNodeId{i, hir.id().v}) == HirKind::Unreachable) ++marks;
    return marks;
}

[[nodiscard]] std::string inMain(std::string const& body) {
    return "int main(void) {\n" + body + "\n    return 0;\n}\n";
}

// `body` — which ends in its own `return` — as the whole body of `int f(void)`: nothing follows
// it that a fold could be credited with marking.
[[nodiscard]] std::string inF(std::string const& body) {
    return "int f(void) {\n" + body + "\n}\nint main(void) { return f(); }\n";
}

constexpr DataModel kModels[] = {DataModel::Lp64, DataModel::Llp64};

}  // namespace

// THE FLOAT FOLD IS AT THE TYPE: each operation of a float chain rounds to float, so the
// chain is C's value in an assertion, an `if` and an array bound — and its negation fails.
TEST(FoldedConstantStatesItsType, AFloatOperationRoundsToFloat) {
    for (DataModel const dm : kModels) {
        EXPECT_EQ(analysisErrors(inMain(
                      "    _Static_assert((int)(16777216.0f + 1.0f - 16777216.0f) == 0, \"rounded\");\n"
                      "    _Static_assert(16777216.0f + 1.0f == 16777216.0f, \"2^24 + 1 is no float\");\n"
                      "    _Static_assert(1.0f / 3.0f != 1.0 / 3.0, \"a float third is no double third\");\n"
                      "    _Static_assert((double)(1.0f / 3.0f) == (double)(float)(1.0 / 3.0),\n"
                      "                   \"the float third, rounded once\");\n"
                      "    _Static_assert(16777216.0 + 1.0 - 16777216.0 == 1.0, \"a double keeps it\");\n"
                      "    int a[(int)(16777216.0f + 1.0f - 16777216.0f) + 1];\n"
                      "    _Static_assert(sizeof a == sizeof(int), \"the bound\");"),
                  dm),
                  "");
        EXPECT_EQ(failedAssertions(inMain(
                      "    _Static_assert((int)(16777216.0f + 1.0f - 16777216.0f) == 1, \"binary64\");"),
                      dm),
                  1u);
        EXPECT_EQ(positionsMarkedNeverReached(inF(
                      "    if (16777216.0f + 1.0f - 16777216.0f == 0.0f) return 42;\n    return 7;"),
                      dm),
                  1u)
            << "the condition is true in C: the position after the `if` is never reached";
    }
}

// AN INTEGER OPERAND CONVERTS TO THE FLOAT TYPE FIRST, ONCE, BY ITS OWN SIGNEDNESS — in an
// operation exactly as in a cast (C 6.3.1.8, 6.3.1.4p2).
TEST(FoldedConstantStatesItsType, AnIntegerOperandConvertsToTheFloatTypeOnce) {
    for (DataModel const dm : kModels) {
        EXPECT_EQ(analysisErrors(inMain(
                      "    _Static_assert(16777217 == 16777216.0f, \"16777217 converts to 16777216\");\n"
                      "    _Static_assert(18446744073709551615ULL * 1.0f > 0, \"2^64 - 1 is no -1\");\n"
                      "    _Static_assert(0x1000001000000001ULL * 1.0f == 0x1.000002p60f, \"rounded once\");\n"
                      "    _Static_assert(0x1000001000000001ULL * 1.0 == 0x1.000001p60, \"to double\");"),
                  dm),
                  "")
            << "an integer operand converts to the operation's float type, once, by its signedness";
        EXPECT_EQ(analysisErrors(inMain(
                      "    _Static_assert((float)0x1000001000000001ULL == 0x1.000002p60f, \"the cast\");\n"
                      "    _Static_assert((float)0x8000008000000001ULL == 0x1.000002p63f, \"unsigned\");\n"
                      "    _Static_assert((float)-0x1000001000000001LL == -0x1.000002p60f, \"negative\");\n"
                      "    _Static_assert((double)0x8000008000000001ULL == 0x1.000001p63, \"to double\");\n"
                      "    _Static_assert((float)18446744073709551615ULL == 0x1p64f, \"2^64 - 1\");"),
                  dm),
                  "")
            << "an integer converted to a float type is rounded once, to that type";
    }
}

// A FOLDED BUILTIN HAS ITS RULE'S TYPE: `__builtin_types_compatible_p` is an `int` and
// `__builtin_offsetof` a `size_t`, so the arithmetic on them is theirs.
TEST(FoldedConstantStatesItsType, AFoldedBuiltinHasItsRulesType) {
    for (DataModel const dm : kModels) {
        EXPECT_EQ(analysisErrors(
                      "struct S { int a; int b; };\n" +
                      inMain("    _Static_assert(__builtin_types_compatible_p(int, long) - 1 < 0, \"an int\");\n"
                             "    _Static_assert(__builtin_types_compatible_p(int, int) - 2 < 0, \"an int\");\n"
                             "    _Static_assert(__builtin_offsetof(struct S, a) - 1 > 0, \"a size_t\");"),
                      dm),
                  "");
        EXPECT_EQ(failedAssertions(inMain(
                      "    _Static_assert(__builtin_types_compatible_p(int, long) - 1 >= 0, \"u64\");"),
                      dm),
                  1u);
        EXPECT_EQ(positionsMarkedNeverReached(inF(
                      "    if (__builtin_types_compatible_p(int, long) - 1 >= 0) return 1;\n    return 2;"),
                      dm),
                  0u)
            << "the condition is false in C: nothing after the `if` may be marked never reached";
    }
}
