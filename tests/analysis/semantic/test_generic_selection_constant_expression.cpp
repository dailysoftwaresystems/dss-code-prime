// P49 (2026-09-01) — [[D-CSUBSET-GENERIC-SELECTION-IS-NOT-AN-INTEGER-CONSTANT-EXPRESSION]].
//
// C23 6.5.1.1p3: a generic selection IS an integer constant expression when the
// SELECTED assignment-expression is one, and the UNSELECTED associations are
// unevaluated. DSS folded neither half, so `_Static_assert(_Generic(x, int: 1,
// default: 0), "")` was refused, `int a[_Generic(x, int: 4, default: 1)]`
// silently became a VLA at block scope and was refused at file scope, and an
// enumerator initialized from a selection was refused as well.
//
// ★★ WHY A UNIT SUITE BESIDE A RUNNABLE EXAMPLE, RATHER THAN INSTEAD OF IT.
// `examples/c/generic_selection_constant_expression/` owns the accepting half
// and the RUNTIME proof — it observes the array's element count through
// `sizeof`, so it witnesses the selected VALUE and not merely that compilation
// succeeded. What an example structurally cannot carry is the REFUSING half,
// and the refusing half is the entire boundary of this fix:
//
//   * A selection whose SELECTED arm is not constant must STAY refused. This is
//     the control arm. Without it, "fold a generic selection" is
//     indistinguishable from "a generic selection is always constant", and the
//     over-broad reading passes every accepting pin in this file.
//   * A selection whose UNSELECTED arm is not constant must FOLD ANYWAY. An
//     evaluator that folds the whole selection tree passes the easy cases and
//     fails exactly this one, and it fails toward REFUSING legal code.
//
// The two are opposite directions of the same one-line change, so each is
// asserted beside its nearest twin rather than alone.
//
// ★ THE FOUR CONSUMERS ARE PINNED SEPARATELY EVEN THOUGH ONE EVALUATOR SERVES
// THEM ALL. `_Static_assert`, an array dimension, a struct-member array and an
// enumerator value each reach `constIntExpr` from their own caller and each
// fails loud in its OWN words (`S_StaticAssertFailed`,
// `S_NonConstantArrayLength`, `S_FlexibleArraySoleMember`,
// `S_NonConstantEnumeratorValue`). That they share an evaluator today is a fact
// about the current tree, not a guarantee, and a future caller that grew its own
// fold would be invisible to a pin that only checked one of them.
//
// ✔MEASURED 2026-09-01, the three references probed SEPARATELY on one
// self-contained TU carrying all of these shapes: gcc 13.3.0 (`-std=c2x -Wall
// -Wextra`) and clang 18.1.3 both compile rc=0 with no warnings AND the built
// program RUNS returning 42; MSVC 19.51.36252 (`-c -W4`, `-std:c17` and
// `-std:clatest`) compiles rc=0. All three REFUSE the non-constant-SELECTED-arm
// control — gcc "expression in static assertion is not constant", clang "static
// assertion expression is not an integral constant expression", MSVC
// "C2057: expected constant expression". Unanimous both ways.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

using namespace dss;
using namespace dss::sem_test;

namespace {

// The layout params every const-expr pin in this directory passes explicitly:
// `analyze()`'s direct-API default supplies NO `aggregateLayout`, and without it
// a layout-dependent fold silently DECLINES — so a pin expecting a refusal would
// pass for the wrong reason. `Natural` + 16 is what the shipped x86_64/arm64
// targets declare. (The same note is on `test_builtin_compile_time_operators`'s
// helper, whose shape this mirrors.)
[[nodiscard]] SemanticModel analyzeWithLayout(std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
}

// The dimension the analyzer folded for the array named `name`. A DECLINED fold
// leaves the symbol un-arrayed (or, at block scope, a VLA — which carries no
// static extent), so reading a dimension back is simultaneously a pin that the
// fold happened AT ALL and a pin on which arm won. Scans for the unique
// ARRAY-typed match because `symbols()` is ONE FLAT LIST across every scope,
// struct members included.
[[nodiscard]] std::optional<std::int64_t>
foldedArrayDim(SemanticModel const& model, std::string_view name) {
    auto const& ti = model.lattice().interner();
    std::optional<std::int64_t> found;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name != name) continue;
        TypeId const t = model.symbols()[i].type;
        if (!t.valid() || ti.kind(t) != TypeKind::Array) continue;
        auto const sc = ti.scalars(t);
        if (sc.empty()) continue;
        if (found.has_value() && *found != sc[0]) return std::nullopt;  // ambiguous
        found = sc[0];
    }
    return found;
}

// The value the analyzer folded for the enumerator named `name`, or nullopt when
// no such symbol exists. The enumerator consumer reaches the same evaluator by
// its own path and reports `S_NonConstantEnumeratorValue`, so it is pinned on
// the VALUE rather than only on the absence of that code.
[[nodiscard]] std::optional<std::int64_t>
foldedEnumValue(SemanticModel const& model, std::string_view name) {
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == name) return model.symbols()[i].enumValue;
    }
    return std::nullopt;
}

} // namespace


// ── the accepting half: each of the four consumers, one at a time ────────────

// `_Static_assert` over a selection, BOTH directions of the choice. Asserting
// only the `int:` arm would pass under an evaluator that folded a selection to
// its FIRST association regardless of the controlling type, so the `default:`
// case is here to make the controlling type load-bearing.
TEST(GenericSelectionConstantExpression, StaticAssertFoldsTheSelectedArm) {
    auto model = analyzeWithLayout(
        "int main(void){\n"
        "  int x = 0; double d = 0;\n"
        "  _Static_assert(_Generic(x, int: 1, default: 0), \"int arm\");\n"
        "  _Static_assert(_Generic(d, int: 0, default: 1), \"default arm\");\n"
        "  (void)x; (void)d; return 0;\n"
        "}\n");
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "a generic selection whose selected arm is a constant IS an integer "
           "constant expression (C23 6.5.1.1p3)";
}

// An array DIMENSION, at FILE scope, where a non-constant length has no VLA
// reading to fall back on. Pinned on the folded EXTENT rather than on a clean
// build: a fix that merely stopped diagnosing would leave the extent unset.
TEST(GenericSelectionConstantExpression, FileScopeArrayDimensionFolds) {
    auto model = analyzeWithLayout(
        "int g = 0;\n"
        "double h = 0;\n"
        "int fromIntArm[_Generic(g, int: 4, default: 1)];\n"
        "int fromDefaultArm[_Generic(h, int: 1, default: 9)];\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(foldedArrayDim(model, "fromIntArm"), 4);
    EXPECT_EQ(foldedArrayDim(model, "fromDefaultArm"), 9);
}

// ★ THE BLOCK-SCOPE ARRAY IS ITS OWN PIN BECAUSE ITS FAILURE MODE WAS SILENT.
// At block scope a non-constant length is a legal VLA, so the pre-fix compiler
// did not refuse `int a[_Generic(x, int: 4, default: 1)]` at all — it built a
// VLA, changing the TYPE of a conforming declaration with nothing said. A pin
// that only counted diagnostics would have called that green. Reading a STATIC
// extent back is what separates "folded to 4" from "became a runtime-sized
// array that happens to be 4 elements long".
TEST(GenericSelectionConstantExpression, BlockScopeArrayIsNotSilentlyAVla) {
    auto model = analyzeWithLayout(
        "int main(void){\n"
        "  int x = 0;\n"
        "  int local[_Generic(x, int: 4, default: 1)];\n"
        "  (void)x; (void)local; return 0;\n"
        "}\n");
    EXPECT_EQ(foldedArrayDim(model, "local"), 4)
        << "a foldable length must produce a constant-extent array, never a VLA";
}

// A struct MEMBER array. Its pre-fix diagnostic was `S_FlexibleArraySoleMember`
// — a length that would not fold read as an absent one, so the member was
// classified as a flexible array member. A third consumer, a third wrong answer.
TEST(GenericSelectionConstantExpression, StructMemberArrayDimensionFolds) {
    auto model = analyzeWithLayout(
        "int g = 0;\n"
        "struct S { int member[_Generic(g, int: 4, default: 1)]; };\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(foldedArrayDim(model, "member"), 4);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_FlexibleArraySoleMember), 0u)
        << "a foldable member length is not a flexible array member";
}

// An ENUMERATOR value — the fourth consumer.
TEST(GenericSelectionConstantExpression, EnumeratorValueFolds) {
    auto model = analyzeWithLayout(
        "int g = 0;\n"
        "enum E { K = _Generic(g, int: 7, default: 1) };\n"
        "int main(void){ return K; }\n");
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantEnumeratorValue), 0u);
    EXPECT_EQ(foldedEnumValue(model, "K"), 7);
}

// A selection is an OPERAND, not only a whole condition — it has to compose with
// the surrounding arithmetic, and with itself.
TEST(GenericSelectionConstantExpression, SelectionComposesWithArithmeticAndNesting) {
    auto model = analyzeWithLayout(
        "int g = 0;\n"
        "int scaled[_Generic(g, int: 2, default: 1) + 3];\n"
        "int nested[_Generic(g, int: _Generic(g, int: 6, default: 1), default: 1)];\n"
        "int main(void){\n"
        "  _Static_assert(_Generic(g, int: 4, default: 1) * 2 == 8, \"arith\");\n"
        "  return 0;\n"
        "}\n");
    EXPECT_EQ(foldedArrayDim(model, "scaled"), 5);
    EXPECT_EQ(foldedArrayDim(model, "nested"), 6);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u);
}


// ── the two boundary pins, in OPPOSITE directions ────────────────────────────

// ★ THE UNSELECTED ARM IS NEVER EVALUATED, PROVEN BY PUTTING SOMETHING
// NON-CONSTANT IN IT. C23 6.5.1.1p3 makes the unselected associations
// unevaluated, so a `default:` holding a function call must not poison a
// selection that picks a constant `int:` arm. An evaluator that folded the whole
// selection tree would decline here and pass every accepting pin above — this is
// the one that separates the two. The sharper spelling of the same property is
// the divide-by-zero variant: a merely-unused arm would still be VISITED, and
// visiting `1 / 0` trips the engine's own wall.
TEST(GenericSelectionConstantExpression, UnselectedArmIsNeverEvaluated) {
    auto model = analyzeWithLayout(
        "extern int nc(void);\n"
        "int g = 0;\n"
        "int withCall[_Generic(g, int: 3, default: nc())];\n"
        "int withTrap[_Generic(g, int: 5, default: 1 / 0)];\n"
        "int main(void){\n"
        "  _Static_assert(_Generic(g, int: 1, default: nc()), \"unselected\");\n"
        "  return 0;\n"
        "}\n");
    EXPECT_EQ(foldedArrayDim(model, "withCall"), 3)
        << "a non-constant UNSELECTED arm must not poison the selection";
    EXPECT_EQ(foldedArrayDim(model, "withTrap"), 5)
        << "a divide-by-zero in the UNSELECTED arm must not be evaluated";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u);
}

// ★★ THE CONTROL ARM. C23 6.5.1.1p3 makes the selection an integer constant
// expression ONLY when the SELECTED assignment-expression is one, so this must
// STAY refused — and it is the single assertion that stops this whole change
// closing into "a generic selection is always constant". Every accepting pin
// above passes under that over-broad reading; this one does not.
// ✔MEASURED refused by gcc 13.3.0, clang 18.1.3 and MSVC 19.51.36252 alike.
TEST(GenericSelectionConstantExpression, NonConstantSelectedArmStaysRefused) {
    auto model = analyzeWithLayout(
        "extern int nc(void);\n"
        "int main(void){\n"
        "  int x = 0;\n"
        "  _Static_assert(_Generic(x, int: nc(), default: 1), \"selected\");\n"
        "  (void)x; return 0;\n"
        "}\n");
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 1u)
        << "the SELECTED arm must itself be an integer constant expression";
}

// The array twin of the control, at FILE scope so a VLA reading cannot absorb
// the refusal into a legal declaration. Filed as its own pin rather than folded
// into the one above because the two consumers fail through different callers
// and could regress independently.
//
// ⚠ THE SECOND ASSERTION CORRECTS THIS PIN'S FIRST DRAFT, and the correction is
// worth stating because the wrong version passed for the wrong reason nowhere —
// it FAILED, which is how it was caught. A refused file-scope length does NOT
// leave the symbol un-arrayed: ✔MEASURED, the declarator still builds an array
// type carrying the `kVlaLength` (-2) runtime-length sentinel, so the loud
// diagnostic is accompanied by a marker type rather than by nothing. What must
// never appear is a CONSTANT extent — a fold that wrongly succeeded would put
// the `default:` arm's 1 here — so the sentinel is what is asserted.
TEST(GenericSelectionConstantExpression, NonConstantSelectedArmStaysRefusedInAnArrayDimension) {
    auto model = analyzeWithLayout(
        "extern int nc(void);\n"
        "int g = 0;\n"
        "int refused[_Generic(g, int: nc(), default: 1)];\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 1u)
        << "a non-constant SELECTED arm must still refuse a file-scope length";
    auto const extent = foldedArrayDim(model, "refused");
    ASSERT_TRUE(extent.has_value());
    EXPECT_EQ(*extent, kVlaLength)
        << "a refused length must leave the runtime-length sentinel, never a "
           "folded constant extent";
}

// ★ THE SELECTION MUST BE UNRESOLVABLE-SAFE. A `_Generic` whose controlling type
// matches no association and has no `default:` has no winner at all; the fold
// must DECLINE (leaving the const-expr caller to fail loud in its own words) and
// Pass 2 must still emit the constraint violation exactly once. Guessing an
// association here would pick a value the rest of the compiler does not agree
// with — a program that builds and computes something no reference computes.
TEST(GenericSelectionConstantExpression, NoMatchingAssociationRefusesAndDiagnosesOnce) {
    auto model = analyzeWithLayout(
        "double h = 0;\n"
        "int refused[_Generic(h, int: 4, char: 2)];\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_GenericSelectionNoMatch), 1u)
        << "the const-expr fold must not add a second copy of Pass 2's "
           "constraint diagnostic, nor swallow it";
    // The same sentinel the twin above measures — a winnerless selection must
    // not produce a constant extent from any association.
    auto const extent = foldedArrayDim(model, "refused");
    ASSERT_TRUE(extent.has_value());
    EXPECT_EQ(*extent, kVlaLength)
        << "a selection with no winner must not fold an association's value";
}


// ── the sibling construct this change shares an engine arm with ──────────────

// ★ REGRESSION CONTROL. `__builtin_choose_expr` and `_Generic` now reach ONE
// compile-time-selection arm in the CST const-eval engine and ONE closure on the
// semantic side; generalizing that arm is exactly the edit that could have
// silently unhooked the construct that was already using it. Pinned here, beside
// the change, rather than relying on the P31 suite to notice.
TEST(GenericSelectionConstantExpression, ChooseExprSelectionStillFolds) {
    auto model = analyzeWithLayout(
        "int chosen[__builtin_choose_expr(1, 5, 1 / 0)];\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(foldedArrayDim(model, "chosen"), 5);
}
