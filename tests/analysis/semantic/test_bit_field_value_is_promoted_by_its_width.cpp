// ══ A BIT-FIELD'S VALUE IS PROMOTED BY ITS WIDTH ════════════════════════════════════
// (cycle P69 round 1, lane `lm`; D-C-A-BIT-FIELD-NARROWER-THAN-INT-IS-NOT-PROMOTED-TO-INT.)
//
// C 6.3.1.1p2: "If an int can represent all values of the original type (as restricted
// by the width, for a bit-field), the value is converted to an int; otherwise, it is
// converted to an unsigned int." So an `unsigned b : 3` is an `int` in an expression,
// an `unsigned b : 32` an `unsigned int`, and an `unsigned long long b : 3` an `int`.
// DSS read every bit-field at its declared type, in both tiers and in the constant
// evaluator alike. ✔MEASURED 2026-10-15 (17 fields × 51 programs, pe64, both pipelines,
// images run; the references AppleClang 21, clang 18, gcc 13 and mingw-w64 gcc 13.2):
// 305 cells off the four — `v.b - 6 < 0` over an `unsigned b : 3` holding 5 ran 0 where
// they run 1, `_Generic(v.b + 0, …)` chose `unsigned int` where they choose `int`, and
// `x /= v.b` left 1431655763 in an `int x = -6` where they leave -2.
//
// WHICH expression is a bit-field's value is ONE statement both tiers ask
// (`bitFieldValueWidth`, core/types/decl_prefix_strip.hpp); the TYPE it has is one rule
// (`bitFieldPromotedType`, analysis/semantic/type_rules.hpp). Each form of the statement
// is pinned below in BOTH tiers — the semantic typer's answer (`_Generic` in a static
// assertion) and the lowering's operator (the type the HIR operator node is built at) —
// and so is each form the statement leaves out, by a fork decided by the references
// (clang 18 and AppleClang 21): a postfix `E++` and a statement expression `({ v.b; })`
// are NOT a bit-field's value. A conditional is not one either: its arms are each a
// bit-field's value, promoted, and then balanced.
//
// The values themselves are run by `examples/c/bit_field_value_is_promoted_by_its_width`;
// a unit case cannot run a program.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "hir/hir.hpp"
#include "hir/hir_node.hpp"
#include "hir/hir_op.hpp"
#include "hir/lowering/cst_to_hir.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::sem_test;

namespace {

// The objects every case reads. `pad` keeps each field off offset 0; every field holds 5
// (`n` holds 3, `f` holds 1).
constexpr char const* kObjects =
    "struct S { int pad; unsigned b : 3; unsigned w : 31; unsigned x : 32;\n"
    "           unsigned long long q : 3; long long n : 3; unsigned long long big : 33;\n"
    "           _Bool f : 1; unsigned char c : 3; };\n"
    "struct N { int k; struct S s; };\n"
    "static volatile struct S v = { 0, 5, 5, 5, 5, 3, 5, 1, 5 };\n"
    "static volatile struct S arr[1] = { { 0, 5, 5, 5, 5, 3, 5, 1, 5 } };\n"
    "static volatile struct N nest = { 0, { 0, 5, 5, 5, 5, 3, 5, 1, 5 } };\n"
    "static volatile struct S *volatile p = &v;\n"
    "static const struct S g = { 0, 5, 5, 5, 5, 3, 5, 1, 5 };\n"
    "#define IS_INT(e) _Generic((e), int: 1, default: 0)\n"
    "#define IS_UINT(e) _Generic((e), unsigned int: 1, default: 0)\n"
    "#define IS_ULL(e) _Generic((e), unsigned long long: 1, default: 0)\n"
    "#define IS_LL(e) _Generic((e), long long: 1, default: 0)\n";

[[nodiscard]] SemanticModel analyzeC(std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
}

[[nodiscard]] std::size_t parseErrorsFor(std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    std::size_t n = 0;
    for (auto const& t : cu->trees())
        for (auto const& d : t.diagnostics().all())
            if (d.severity == DiagnosticSeverity::Error) ++n;
    return n;
}

[[nodiscard]] std::string allActuals(SemanticModel const& m, DiagnosticCode code) {
    std::string out;
    for (auto const& d : m.diagnostics().all())
        if (d.code == code) out += "\n    " + d.actual;
    return out;
}

// THE SEMANTIC TIER: each line of `asserts` is a block-scope static assertion; how many
// of them failed.
[[nodiscard]] std::size_t failedAssertions(std::string const& asserts, std::string* actuals) {
    std::string const src = std::string{kObjects} +
        "int main(void) {\n" + asserts + "    return 0;\n}\n";
    EXPECT_EQ(parseErrorsFor(src), 0u) << src;
    auto model = analyzeC(src);
    if (actuals != nullptr) *actuals = allActuals(model, DiagnosticCode::S_StaticAssertFailed);
    std::size_t const failed = countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed);
    EXPECT_EQ(model.diagnostics().errorCount(), failed)
        << "an error other than a failed assertion:\n" << src;
    return failed;
}

// THE LOWERING: the core kinds of the types the HIR builds the operator `op` at in
// `int f(int y) { <body> }` (a `BinaryOp` or `UnaryOp` node of that operator; with
// `ternary`, the `Ternary` nodes instead) — the program's only such nodes.
[[nodiscard]] std::vector<TypeKind> operatorKinds(std::string const& body, HirOpKind op,
                                                  bool ternary = false) {
    std::string const src = std::string{kObjects} + "int f(int y) {\n" + body + "\n}\n"
                            "int main(void) { return f(-1); }\n";
    EXPECT_EQ(parseErrorsFor(src), 0u) << src;
    auto cu = buildShippedUnit("c", {src});
    SemanticModel model =
        analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_FALSE(model.hasErrors()) << src;
    DiagnosticReporter reporter{};
    auto lowered = lowerToHir(model, reporter);
    std::vector<TypeKind> kinds;
    if (lowered == nullptr) {
        ADD_FAILURE() << "the source did not lower: " << src;
        return kinds;
    }
    EXPECT_EQ(reporter.errorCount(), 0u) << src;
    Hir const& hir = lowered->hir;
    auto const& in = model.lattice().interner();
    std::vector<HirNodeId> stack{hir.root()};
    while (!stack.empty()) {
        HirNodeId const id = stack.back();
        stack.pop_back();
        for (HirNodeId c : hir.children(id)) stack.push_back(c);
        HirKind const k = hir.kind(id);
        bool const counted = ternary
            ? k == HirKind::Ternary
            : (k == HirKind::BinaryOp || k == HirKind::UnaryOp) && hir.payload(id) == encodeOp(op);
        if (counted) kinds.push_back(in.kind(hir.typeId(id)));
    }
    return kinds;
}

// Every `op` of `body` is built at `want` — and there is one at least.
void expectOperatorAt(std::string const& body, HirOpKind op, TypeKind want, bool ternary = false) {
    auto const ks = operatorKinds(body, op, ternary);
    EXPECT_FALSE(ks.empty()) << "no such operator in:\n" << body;
    for (TypeKind k : ks)
        EXPECT_EQ(static_cast<int>(k), static_cast<int>(want))
            << "an operator built at another type than the one C gives it:\n" << body;
}

// How many positions of `f` the lowering marks as never reached (the `Unreachable` it puts
// after a statement control cannot continue past) — what it concluded from a fold.
[[nodiscard]] std::size_t positionsMarkedNeverReached(std::string const& body) {
    std::string const src = std::string{kObjects} + "int f(int y) {\n" + body + "\n}\n"
                            "int main(void) { return f(-1); }\n";
    auto cu = buildShippedUnit("c", {src});
    SemanticModel model =
        analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_FALSE(model.hasErrors()) << src;
    DiagnosticReporter reporter{};
    auto lowered = lowerToHir(model, reporter);
    if (lowered == nullptr) {
        ADD_FAILURE() << "the source did not lower: " << src;
        return 0;
    }
    Hir const& hir = lowered->hir;
    std::size_t marks = 0;
    std::vector<HirNodeId> stack{hir.root()};
    while (!stack.empty()) {
        HirNodeId const id = stack.back();
        stack.pop_back();
        for (HirNodeId c : hir.children(id)) stack.push_back(c);
        if (hir.kind(id) == HirKind::Unreachable) ++marks;
    }
    return marks;
}

} // namespace

// ── THE RULE: the type by the width ─────────────────────────────────────────────────
TEST(BitFieldValueIsPromotedByItsWidth, TheTypeIsDecidedByTheWidth) {
    std::string actuals;
    EXPECT_EQ(failedAssertions(
        "    _Static_assert(IS_INT(v.b + 0), \"unsigned : 3 -> int\");\n"
        "    _Static_assert(IS_INT(v.w + 0), \"unsigned : 31 -> int\");\n"
        "    _Static_assert(IS_UINT(v.x + 0), \"unsigned : 32 -> unsigned int\");\n"
        "    _Static_assert(IS_INT(v.q + 0), \"unsigned long long : 3 -> int\");\n"
        "    _Static_assert(IS_INT(v.n + 0), \"long long : 3 -> int\");\n"
        "    _Static_assert(IS_ULL(v.big + 0), \"unsigned long long : 33 -> as declared\");\n"
        "    _Static_assert(IS_INT(v.f + 0), \"_Bool : 1 -> int\");\n"
        "    _Static_assert(IS_INT(v.c + 0), \"unsigned char : 3 -> int\");\n"
        "    _Static_assert(sizeof(v.q + 0) == sizeof(int), \"the size follows the type\");\n",
        &actuals), 0u) << actuals;
    // What was answered before: the declared type. Each is FALSE of C and must fail.
    char const* const wasSilentlyTrue[] = {
        "    _Static_assert(IS_UINT(v.b + 0), \"m\");\n",
        "    _Static_assert(IS_UINT(v.w + 0), \"m\");\n",
        "    _Static_assert(IS_ULL(v.q + 0), \"m\");\n",
        "    _Static_assert(IS_LL(v.n + 0), \"m\");\n",
        "    _Static_assert(sizeof(v.q + 0) == 8, \"m\");\n",
    };
    for (char const* line : wasSilentlyTrue) EXPECT_EQ(failedAssertions(line, nullptr), 1u) << line;
    expectOperatorAt("    return v.b - 6 < 0;", HirOpKind::Sub, TypeKind::I32);
    expectOperatorAt("    return v.x - 6 < 0;", HirOpKind::Sub, TypeKind::U32);
}

// ── THE FORMS THAT ARE A BIT-FIELD'S VALUE ──────────────────────────────────────────
TEST(BitFieldValueIsPromotedByItsWidth, AMemberReadThroughAnyAccessAndAnyWrapper) {
    std::string actuals;
    EXPECT_EQ(failedAssertions(
        "    _Static_assert(IS_INT(p->b + 0), \"through ->\");\n"
        "    _Static_assert(IS_INT(arr[0].b + 0), \"an element's member\");\n"
        "    _Static_assert(IS_INT(nest.s.b + 0), \"a nested member\");\n"
        "    _Static_assert(IS_INT((v.b) + 0), \"parenthesized\");\n"
        "    _Static_assert(IS_INT((__extension__ v.b) + 0), \"__extension__\");\n",
        &actuals), 0u) << actuals;
    expectOperatorAt("    return p->b - 6 < 0;", HirOpKind::Sub, TypeKind::I32);
    expectOperatorAt("    return arr[0].b - nest.s.b - 6 < 0;", HirOpKind::Sub, TypeKind::I32);
    expectOperatorAt("    return (__extension__ (v.b)) - 6 < 0;", HirOpKind::Sub, TypeKind::I32);
}

TEST(BitFieldValueIsPromotedByItsWidth, ASelectionsResult) {
    std::string actuals;
    EXPECT_EQ(failedAssertions(
        "    _Static_assert(IS_INT(_Generic(0, int: v.b) + 0), \"_Generic\");\n"
        "    _Static_assert(IS_INT(__builtin_choose_expr(1, v.b, 0) + 0), \"__builtin_choose_expr\");\n",
        &actuals), 0u) << actuals;
    expectOperatorAt("    return _Generic(0, int: v.b) - 6 < 0;", HirOpKind::Sub, TypeKind::I32);
    expectOperatorAt("    return __builtin_choose_expr(1, v.b, 0) - 6 < 0;", HirOpKind::Sub, TypeKind::I32);
}

TEST(BitFieldValueIsPromotedByItsWidth, TheCommasRightOperand) {
    std::string actuals;
    EXPECT_EQ(failedAssertions(
        "    _Static_assert(IS_INT((0, v.b) + 0), \"the comma's right operand\");\n", &actuals),
        0u) << actuals;
    expectOperatorAt("    return (y, v.b) - 6 < 0;", HirOpKind::Sub, TypeKind::I32);
}

TEST(BitFieldValueIsPromotedByItsWidth, AnAssignmentAndACompoundAssignment) {
    std::string actuals;
    EXPECT_EQ(failedAssertions(
        "    _Static_assert(IS_INT((v.b = 1) + 0), \"an assignment\");\n"
        "    _Static_assert(IS_INT((v.b += 1) + 0), \"a compound assignment\");\n", &actuals),
        0u) << actuals;
    expectOperatorAt("    return (v.b = 5) - 6 < 0;", HirOpKind::Sub, TypeKind::I32);
    expectOperatorAt("    return (v.b += 0) - 6 < 0;", HirOpKind::Sub, TypeKind::I32);
}

TEST(BitFieldValueIsPromotedByItsWidth, APrefixIncrementAndDecrement) {
    std::string actuals;
    EXPECT_EQ(failedAssertions(
        "    _Static_assert(IS_INT(++v.b + 0), \"a prefix ++\");\n"
        "    _Static_assert(IS_INT(--v.b + 0), \"a prefix --\");\n", &actuals),
        0u) << actuals;
    expectOperatorAt("    return ++v.b - 6 < 0;", HirOpKind::Sub, TypeKind::I32);
    // `--` desugars to a `Sub` of its own (at the field's type, the stored value); the operator asked about
    // is the `Mul` that reads the decrement's value.
    expectOperatorAt("    return --v.b * 2 < 7;", HirOpKind::Mul, TypeKind::I32);
}

// ── EVERY OPERATOR READS IT SO ──────────────────────────────────────────────────────
TEST(BitFieldValueIsPromotedByItsWidth, TheUnaryOperatorsTheShiftAndTheConditional) {
    std::string actuals;
    EXPECT_EQ(failedAssertions(
        "    _Static_assert(IS_INT(+v.b) && IS_INT(-v.b) && IS_INT(~v.b), \"unary\");\n"
        "    _Static_assert(IS_INT(v.b << 1), \"the shift's left operand\");\n"
        "    _Static_assert(IS_UINT(1u << v.b), \"the shift's count gives nothing\");\n"
        "    _Static_assert(IS_INT(1 ? v.b : v.b), \"both arms\");\n"
        "    _Static_assert(IS_INT(1 ? v.b : 0), \"one arm\");\n"
        "    _Static_assert(IS_INT(v.b * v.b), \"both operands\");\n", &actuals),
        0u) << actuals;
    expectOperatorAt("    return -v.b < 0;", HirOpKind::Neg, TypeKind::I32);
    expectOperatorAt("    return ~v.b < 0;", HirOpKind::BitNot, TypeKind::I32);
    expectOperatorAt("    return +v.b - 6 < 0;", HirOpKind::Sub, TypeKind::I32);
    expectOperatorAt("    return (v.b << 1) - 11 < 0;", HirOpKind::Shl, TypeKind::I32);
    expectOperatorAt("    return (y ? v.b : v.b) - 6 < 0;", HirOpKind::Sub, TypeKind::I32, /*ternary=*/true);
    expectOperatorAt("    return (y ? v.b : 0) - 6 < 0;", HirOpKind::Sub, TypeKind::I32, /*ternary=*/true);
}

// C 6.5.16.2p3: `E1 op= E2` is `E1 = E1 op (E2)` — the operator computes at the
// promoted operands, either side. ✔MEASURED (above): `x /= v.b` with x = -6 and the
// field holding 3 leaves -2 on all four references (1431655763 here before); `v.b /= y`
// with y = -1 stores -3, read back 5 (0 here before).
TEST(BitFieldValueIsPromotedByItsWidth, ACompoundAssignmentComputesAtThePromotedOperands) {
    expectOperatorAt("    int x = y; x /= v.b; return x;", HirOpKind::Div, TypeKind::I32);
    expectOperatorAt("    int x = y; x %= v.b; return x;", HirOpKind::Rem, TypeKind::I32);
    expectOperatorAt("    v.b /= y; return 0;", HirOpKind::Div, TypeKind::I32);
    expectOperatorAt("    return (v.b /= y) + 0;", HirOpKind::Div, TypeKind::I32);
    expectOperatorAt("    int x = y; return (x /= v.b) + 0;", HirOpKind::Div, TypeKind::I32);
    // the control: a field of exactly `int`'s width computes unsigned, as C says
    expectOperatorAt("    int x = y; x /= v.x; return x;", HirOpKind::Div, TypeKind::U32);
}

// ── THE FORMS THAT ARE NOT ──────────────────────────────────────────────────────────
// Fork, decided by clang 18 and AppleClang 21 (and cl 19.51): the value of `E++` is the
// object's old value at the declared type; gcc 13 and mingw-w64 gcc 13.2 promote it.
TEST(BitFieldValueIsPromotedByItsWidth, NotAPostfixIncrement) {
    std::string actuals;
    EXPECT_EQ(failedAssertions(
        "    _Static_assert(IS_UINT(v.b++ + 0), \"E++ is the declared type\");\n", &actuals),
        0u) << actuals;
    expectOperatorAt("    return v.b++ - 6 < 0;", HirOpKind::Sub, TypeKind::U32);
}

// Fork, decided by clang 18 and AppleClang 21: a statement expression's value is no
// bit-field's; gcc 13 and mingw-w64 gcc 13.2 promote it with the own-width bit-field
// type DSS declines for a field wider than `int`.
TEST(BitFieldValueIsPromotedByItsWidth, NotAStatementExpression) {
    std::string actuals;
    EXPECT_EQ(failedAssertions(
        "    _Static_assert(IS_UINT(({ v.b; }) + 0), \"({ v.b; }) is the declared type\");\n",
        &actuals), 0u) << actuals;
    expectOperatorAt("    return ({ v.b; }) - 6 < 0;", HirOpKind::Sub, TypeKind::U32);
}

// A cast is a conversion to the type it names: `(unsigned)v.b` is an `unsigned int`.
TEST(BitFieldValueIsPromotedByItsWidth, NotACast) {
    std::string actuals;
    EXPECT_EQ(failedAssertions(
        "    _Static_assert(IS_UINT((unsigned)v.b + 0), \"a cast\");\n", &actuals), 0u) << actuals;
    expectOperatorAt("    return (unsigned)v.b - 6 < 0;", HirOpKind::Sub, TypeKind::U32);
}

// ── THE CONSTANT IS FOLDED AT THE SAME TYPE ─────────────────────────────────────────
// A fold must not know more C than the run time computes, nor less: the constant read of
// a bit-field has its promoted type (`readThroughBitField`), as the operators read it.
// clang 18 and AppleClang 21 fold each of these; gcc 13 refuses the enumerator form (a
// member of a `const` object is no constant there), and all four build and run the
// static initializers at these values.
TEST(BitFieldValueIsPromotedByItsWidth, TheConstantIsFoldedAtThePromotedType) {
    std::string actuals;
    EXPECT_EQ(failedAssertions(
        "    enum { K = g.b - 6 < 0 };\n"
        "    _Static_assert(K == 1, \"an enumerator\");\n"
        "    _Static_assert(g.b - 6 < 0, \"unsigned : 3\");\n"
        "    _Static_assert((long long)(g.b - 6) == -1, \"unsigned : 3, widened\");\n"
        "    _Static_assert(-g.b < 0 && ~g.b < 0, \"unary\");\n"
        "    _Static_assert((1 ? g.b : g.b) - 6 < 0, \"the conditional\");\n"
        "    _Static_assert((long long)(g.q - 6) == -1, \"unsigned long long : 3\");\n"
        "    _Static_assert((long long)(g.x - 6) == 4294967295ll, \"unsigned : 32 stays unsigned\");\n"
        "    _Static_assert((long long)(g.big - 6) == -1, \"unsigned long long : 33, as declared\");\n"
        "    _Static_assert(g.f + g.f == 2, \"_Bool : 1\");\n",
        &actuals), 0u) << actuals;
    char const* const wasSilentlyTrue[] = {
        "    _Static_assert((g.b - 6 < 0) == 0, \"m\");\n",
        "    _Static_assert((long long)(g.b - 6) == 4294967295ll, \"m\");\n",
        "    _Static_assert((long long)(g.q - 6) == -1 && sizeof(g.q + 0) == 8, \"m\");\n",
    };
    for (char const* line : wasSilentlyTrue) EXPECT_EQ(failedAssertions(line, nullptr), 1u) << line;
}

// THE LOWERING FOLDS AT THE SAME TYPE. It asks the evaluator whether an `if` can complete
// (`ifNeverCompletes`): a condition folded true over an arm that returns marks what follows
// as never reached. `g.b - 6 < 0` is TRUE in C (the field holds 5): the position after the
// `if` is marked. `g.b - 6 >= 0` is FALSE: nothing is — a fold at the declared type
// (`unsigned int`) would mark it, and the program's own test would walk into the mark.
TEST(BitFieldValueIsPromotedByItsWidth, TheLoweringFoldsAtThePromotedType) {
    EXPECT_EQ(positionsMarkedNeverReached("    if (g.b - 6 < 0) return 7;\n    return 42;"), 1u)
        << "the condition C makes true did not fold true";
    EXPECT_EQ(positionsMarkedNeverReached("    if (g.b - 6 >= 0) return 7;\n    return 42;"), 0u)
        << "the condition C makes false was folded true";
    EXPECT_EQ(positionsMarkedNeverReached("    if (g.q - 6 < 0) return 7;\n    return 42;"), 1u)
        << "unsigned long long : 3 did not fold as an int";
}
