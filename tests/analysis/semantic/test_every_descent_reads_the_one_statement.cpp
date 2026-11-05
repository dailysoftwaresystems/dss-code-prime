// ══ EVERY DESCENT TAKES ITS STEPS BY THE ONE STATEMENT OF WHAT A WRAPPER IS ═══════════
// (cycle P69 round 1, lane `lm`; a review of the constant-evaluator fold, its finding B.)
//
// `nodeSaysSomethingItself` (`src/core/types/decl_prefix_strip.hpp`) is the one statement
// of which nodes are grouping and which say something themselves; a descent asking a VALUE
// question takes the steps the statement leaves to it — a statement expression's sole
// expression, a compile-time selection's result — by name. Six descents took their steps
// by a list or a count of their own. What each got wrong, ✔MEASURED 2026-10-15 (pe64,
// baseline and release alike, every image run; the references gcc 13, clang 18,
// AppleClang 21 and mingw-w64 gcc 13.2):
//   * the constant evaluator (`wrapperChild`, `evalNode`'s tail) peeled a statement
//     expression to its one item by count, whatever the item was: `({ { 5; } })` and
//     `({ return 5; })` folded to 5 as an enumerator — all four refuse both;
//   * the builtins' operand peel stepped through `sizeof` and a statement expression and
//     never into a selection: `__builtin_object_size` of `_Generic(0, int: buf)`,
//     `__builtin_choose_expr(1, buf, 0)` and `({ buf; })` was the unknown -1 where all four
//     say 10; `__builtin_nan(_Generic(0, int: "1"))` was refused as a static initializer
//     that all four fold;
//   * the overflow predicates' third operand re-derived which expression is a bit-field's
//     value: `(0, v.b)`, `(v.b = 0)`, `++v.b`, `_Generic(0, int: v.b)` and
//     `__builtin_choose_expr(1, v.b, 0)` were read at the declared type where gcc and
//     mingw-w64 gcc (the references that have the builtin) read the field's width;
//   * the search that sizes an array of unknown size searched every node for the first
//     string literal: `char s[] = (0, "abc");` and `({ "abc"; })` were sized from the
//     literal inside and refused for the type, where all four refuse them as no array
//     initializer at all.
// The lowering's role peel (`peelToCore`) and designator peel (`peelToDesignatorLeaf`) read
// the statement too; nothing a program does is changed by it (every construct the
// statement names is an alternative under the `operand` node, which the role peel stops at
// first; no designator stands under a node that says something) — no case here.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "hir/hir.hpp"
#include "hir/hir_node.hpp"
#include "hir/lowering/cst_to_hir.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace dss;
using namespace dss::sem_test;

namespace {

[[nodiscard]] SemanticModel analyzeC(std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
}

[[nodiscard]] std::string described(SemanticModel const& m) {
    std::string out;
    for (auto const& d : m.diagnostics().all())
        out += "\n    " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return out;
}

[[nodiscard]] std::size_t errorsOf(SemanticModel const& m) {
    std::size_t n = 0;
    for (auto const& d : m.diagnostics().all())
        if (d.severity == DiagnosticSeverity::Error) ++n;
    return n;
}

// Every overflow predicate's recorded third-operand type in the unit, in node order.
[[nodiscard]] std::vector<TypeId> overflowTargets(SemanticModel const& m) {
    std::vector<TypeId> out;
    for (auto const& t : m.unit().trees())
        for (std::uint32_t i = 1; i < t.nodeCount(); ++i)
            if (TypeId const ty = m.overflowPredicateTargetFor(NodeId{i}); ty.valid())
                out.push_back(ty);
    return out;
}

// Every `__builtin_nan` payload the analyzer folded, in node order.
[[nodiscard]] std::size_t nanPayloads(SemanticModel const& m) {
    std::size_t n = 0;
    for (auto const& t : m.unit().trees())
        for (std::uint32_t i = 1; i < t.nodeCount(); ++i)
            if (m.nanPayloadFor(NodeId{i}) != nullptr) ++n;
    return n;
}

} // namespace

// ── the constant evaluator ──────────────────────────────────────────────────────────
// A statement expression has a value only when its last item is an expression statement
// (GNU C 6.1); ONE such item is a constant's value, by the named step
// (`soleExpressionOfStatementExpression`). Any other body is no constant — all four
// references refuse each of these as an enumerator.
TEST(EveryDescentReadsTheOneStatement, AStatementExpressionWhoseItemIsNoExpressionIsNoConstant) {
    for (char const* const body : {"({ { 5; } })", "({ return 5; })", "({ int y = 5; })"}) {
        auto const m = analyzeC(std::string{"int main(void) {\n    enum { V = "} + body
                                + " };\n    return V;\n}\n");
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_NonConstantEnumeratorValue), 1u)
            << body << " gives no value, so no enumerator's" << described(m);
    }
}

// THE POSITIVE TWIN: a statement expression of one expression statement IS its value as
// a constant (both clangs fold each; gcc folds none of them, and one working reference
// makes it required).
TEST(EveryDescentReadsTheOneStatement, AStatementExpressionOfOneExpressionIsItsValue) {
    auto const m = analyzeC(
        "int main(void) {\n"
        "    enum { A = ({ 5; }), B = ({ (5); }), C = __extension__ ({ -5; }) };\n"
        "    _Static_assert(A == 5 && B == 5 && C == -5, \"the sole expression's value\");\n"
        "    return 0;\n}\n");
    EXPECT_EQ(errorsOf(m), 0u) << described(m);
}

// A SCALAR'S BRACED INITIALIZER IS ITS VALUE (C 6.7.9p11), the one other value step the
// evaluator takes by name: a named constant initialized `{ 5 }` reads 5 (gcc 13, mingw-w64
// gcc 13.2 and AppleClang 21 build and run it; clang 18 has no `constexpr`).
TEST(EveryDescentReadsTheOneStatement, AScalarsBracedInitializerIsItsValue) {
    auto const m = analyzeC(
        "int main(void) {\n"
        "    constexpr int c = { 5 };\n"
        "    enum { V = c };\n"
        "    _Static_assert(V == 5, \"the braced scalar's value\");\n"
        "    return 0;\n}\n");
    EXPECT_EQ(errorsOf(m), 0u) << described(m);
}

// ── the builtins' operand: its VALUE through a selection and a statement expression ────
TEST(EveryDescentReadsTheOneStatement, ObjectSizeDesignatesThroughASelectionAndAStatementExpression) {
    auto const m = analyzeC(
        "static char buf[10];\n"
        "_Static_assert(__builtin_object_size(_Generic(0, int: buf), 0) == 10, \"_Generic\");\n"
        "_Static_assert(__builtin_object_size(__builtin_choose_expr(1, buf, 0), 0) == 10,"
        " \"choose\");\n"
        "_Static_assert(__builtin_object_size((buf), 0) == 10, \"the control: grouping\");\n"
        "int f(void) {\n"
        "    _Static_assert(__builtin_object_size(({ buf; }), 0) == 10, \"one expression\");\n"
        "    return 0;\n}\n");
    EXPECT_EQ(errorsOf(m), 0u) << described(m);
}

// THE OTHER SIDE: a statement expression whose body may DO something is taken to have a
// side effect, and the answer is the unknown one (gcc's manual).
TEST(EveryDescentReadsTheOneStatement, ObjectSizeOfABodyThatMayActIsUnknown) {
    auto const m = analyzeC(
        "static char buf[10];\nstatic int n;\n"
        "int f(void) {\n"
        "    _Static_assert(__builtin_object_size(({ n = 1; buf; }), 0) == (unsigned long)-1,"
        " \"an assignment in the body\");\n"
        "    return 0;\n}\n");
    EXPECT_EQ(errorsOf(m), 0u) << described(m);
}

TEST(EveryDescentReadsTheOneStatement, NanFoldsAStringThroughASelectionAndAStatementExpression) {
    auto const m = analyzeC(
        "static double a = __builtin_nan((\"1\"));\n"
        "static double b = __builtin_nan(_Generic(0, int: \"1\"));\n"
        "static double c = __builtin_nan(__builtin_choose_expr(1, \"1\", \"2\"));\n"
        "int main(void) {\n"
        "    static double d = __builtin_nan(({ \"1\"; }));\n"
        "    return a != a && b != b && c != c && d != d ? 0 : 1;\n}\n");
    EXPECT_EQ(errorsOf(m), 0u) << described(m);
    EXPECT_EQ(nanPayloads(m), 4u)
        << "each call is the string it is given, folded: grouping, `_Generic`, "
           "`__builtin_choose_expr`, a statement expression of one expression";
}

// ── the overflow predicates' third operand: the ONE statement of a bit-field's value ────
// gcc's manual: "If the third argument is a bit-field, the type used for the result cast
// has the precision and signedness of the given bit-field". WHICH expression is one is
// `bitFieldValueWidth`, the statement the operators' promotion asks too; gcc and mingw-w64
// gcc read every form below as their own answer says — the same for all but `v.b++` and
// `({ v.b; })`, which they read at the width too and DSS, by the forks the promotion row
// records, at the declared type.
TEST(EveryDescentReadsTheOneStatement, OverflowPredicateThirdOperandIsABitFieldByTheOneStatement) {
    struct Case { char const* form; TypeKind kind; std::int64_t bits; };
    for (Case const& c : {Case{"v.b", TypeKind::BitInt, 3}, Case{"(v.b)", TypeKind::BitInt, 3},
                          Case{"__extension__ v.b", TypeKind::BitInt, 3},
                          Case{"(0, v.b)", TypeKind::BitInt, 3}, Case{"(v.b = 0)", TypeKind::BitInt, 3},
                          Case{"++v.b", TypeKind::BitInt, 3},
                          Case{"_Generic(0, int: v.b)", TypeKind::BitInt, 3},
                          Case{"__builtin_choose_expr(1, v.b, 0)", TypeKind::BitInt, 3},
                          Case{"v.b++", TypeKind::U32, 0}, Case{"({ v.b; })", TypeKind::U32, 0},
                          Case{"(unsigned)v.b", TypeKind::U32, 0}, Case{"+v.b", TypeKind::I32, 0},
                          Case{"(1 ? v.b : v.b)", TypeKind::I32, 0}}) {
        auto const m = analyzeC(std::string{"struct V { unsigned b : 3; };\nstatic struct V v;\n"
                                            "int f(void) { return __builtin_add_overflow_p(1, 7, "}
                                + c.form + "); }\n");
        EXPECT_EQ(errorsOf(m), 0u) << c.form << described(m);
        auto const targets = overflowTargets(m);
        ASSERT_EQ(targets.size(), 1u) << c.form;
        auto const& in = m.lattice().interner();
        EXPECT_EQ(static_cast<int>(in.kind(targets[0])), static_cast<int>(c.kind))
            << c.form << ": the type the result is cast to";
        // The width is read only of a type that HAS one: a wrong kind is this case's red already,
        // and asking a non-BitInt for its width stops the whole binary (the lattice's loud fatal),
        // which would read as a crash rather than as this case (MEASURED, check-mutations run
        // 20261020-090118-4c132d4b: the arm exited 134 and wrote no report).
        if (c.kind == TypeKind::BitInt && in.kind(targets[0]) == TypeKind::BitInt) {
            EXPECT_EQ(in.bitIntWidth(targets[0]), c.bits) << c.form;
            EXPECT_FALSE(in.bitIntIsSigned(targets[0])) << c.form;
        }
    }
}

// ── the array of unknown size: what its initializer IS ─────────────────────────────────
TEST(EveryDescentReadsTheOneStatement, AnArrayIsSizedByWhatItsInitializerIsNotByALiteralInsideIt) {
    for (char const* const init : {"(0, \"abc\")", "(1 ? \"abc\" : \"de\")", "({ \"abc\"; })"}) {
        auto const m = analyzeC(std::string{"int main(void) {\n    char s[] = "} + init
                                + ";\n    return s[0];\n}\n");
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_ArrayLengthOutOfRange), 1u)
            << init << " is no string literal and no brace list" << described(m);
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u)
            << init << ": refused for what it is, not sized from the literal inside and "
                       "then refused for the type" << described(m);
    }
}

// THE POSITIVE TWIN, AND THE SELECTION'S RESULT: a selection sizes the array by the string
// it SELECTS (C 6.5.1.1p3; `char s[] = _Generic(0, int: "abc");` is four elements on all
// four references), whichever association comes first.
TEST(EveryDescentReadsTheOneStatement, ASelectionSizesTheArrayByTheStringItSelects) {
    auto const m = analyzeC(
        "int main(void) {\n"
        "    char a[] = _Generic(0, int: \"abc\");\n"
        "    char b[] = _Generic(0, int: \"abc\", long: \"abcdefgh\");\n"
        "    char c[] = _Generic(0L, int: \"abc\", long: \"abcdefgh\");\n"
        "    char d[] = __builtin_choose_expr(0, \"abcdefgh\", \"abc\");\n"
        "    char e[] = (\"abcd\");\n"
        "    _Static_assert(sizeof a == 4 && sizeof b == 4 && sizeof c == 9 && sizeof d == 4"
        " && sizeof e == 5, \"the selected string's length\");\n"
        "    return a[0] + b[0] + c[0] + d[0] + e[0];\n}\n");
    EXPECT_EQ(errorsOf(m), 0u) << described(m);
}

// ── the lowering: the value the statement names ──────────────────────────────────────
// (a review of checkpoint 21, its N1.) A statement expression's value is the expression of
// its LAST item when that item is an expression statement (GNU C 6.1) —
// `statementExpressionBody`, which the type (`subtreeType`) and every value step above ask.
// The lowering read the same thing with a peel of its own (`stmtExprItems` through
// `peelToCore`), so "one reader" was two; it asks the statement now. `({ x = 1; 7; })`
// lowers to the sequence of the assignment, run as a statement, and the literal 7: the
// value is the last item's, never the first's.
TEST(EveryDescentReadsTheOneStatement, TheLoweringTakesTheValueTheStatementNames) {
    auto m = analyzeC("int f(int x) {\n    return ({ x = 1; 7; });\n}\n");
    ASSERT_EQ(errorsOf(m), 0u) << described(m);
    DiagnosticReporter reporter{};
    auto const lowered = lowerToHir(m, reporter);
    ASSERT_NE(lowered, nullptr);
    ASSERT_EQ(reporter.errorCount(), 0u);
    Hir const& hir = lowered->hir;
    std::vector<HirNodeId> sequences;
    std::vector<HirNodeId> stack{hir.root()};
    while (!stack.empty()) {
        HirNodeId const id = stack.back();
        stack.pop_back();
        for (HirNodeId c : hir.children(id)) stack.push_back(c);
        if (hir.kind(id) == HirKind::SeqExpr) sequences.push_back(id);
    }
    ASSERT_EQ(sequences.size(), 1u)
        << "the one statement expression is one sequence - the value is the LAST item's expression, "
           "the literal 7, so no other item is lowered as a value (an assignment's value is a sequence "
           "of its own)";
    auto const items = hir.children(sequences[0]);
    ASSERT_EQ(items.size(), 2u) << "the assignment run as a statement, then the value";
    EXPECT_EQ(hir.kind(items[1]), HirKind::Literal)
        << "the value is the LAST item's expression, the literal 7";
    EXPECT_NE(hir.kind(items[0]), HirKind::Literal)
        << "the first item is run for its effect, not taken for the value";
}

// ── a label on the last item ───────────────────────────────────────────────────────────
// A statement expression whose LAST item is labeled has the labeled expression's value:
// ✔MEASURED 2026-10-15, `({ int r = 0; if (g) goto in; r = 1; in: r; })` is 1 for g == 0
// and 0 otherwise, `({ x: 5; })` is 5, and two labels in a row are stepped through alike,
// on AppleClang 21, gcc 13.3 (x86_64 and arm64) and clang 18 — and DSS refused the
// construct ("this statement expression has no value"). The one statement steps through a
// label (`statementExpressionBody`), so the type the semantic tier gives `({ x: 5; }) + 1`
// is an `int`'s; and the lowering places each label, on an empty statement, right before
// the value — where the body's `goto` reaches the computation of the value.
TEST(EveryDescentReadsTheOneStatement, ALabeledLastItemIsTheValue) {
    auto m = analyzeC(
        "int f(int g) {\n"
        "    return ({ int r = 0; if (g) goto in; r = 1; in: a: r; });\n"
        "}\n"
        "int h(void) { return ({ x: 5; }) + 1; }\n");
    ASSERT_EQ(errorsOf(m), 0u) << "the labeled last item: refused by the semantic tier" << described(m);
    DiagnosticReporter reporter{};
    auto const lowered = lowerToHir(m, reporter);
    ASSERT_NE(lowered, nullptr) << "the labeled last item: the lowering gave no HIR";
    std::string words;
    for (auto const& d : reporter.all()) words += "\n    " + d.actual;
    ASSERT_EQ(reporter.errorCount(), 0u) << "the labeled last item: refused by the lowering" << words;
    ASSERT_TRUE(lowered->ok) << "the labeled last item: refused by the lowering" << words;
    Hir const& hir = lowered->hir;
    std::vector<HirNodeId> sequences;
    std::vector<HirNodeId> stack{hir.root()};
    while (!stack.empty()) {
        HirNodeId const id = stack.back();
        stack.pop_back();
        for (HirNodeId c : hir.children(id)) stack.push_back(c);
        if (hir.kind(id) == HirKind::SeqExpr) sequences.push_back(id);
    }
    ASSERT_EQ(sequences.size(), 2u)
        << "the labeled last item: one statement expression in each function";
    std::size_t withTwoLabels = 0, withOneLabel = 0;
    for (HirNodeId const seq : sequences) {
        auto const items = hir.children(seq);
        ASSERT_GE(items.size(), 2u)
            << "the labeled last item: each sequence holds its label before the value";
        std::size_t labels = 0;
        for (HirNodeId const c : items)
            if (hir.kind(c) == HirKind::LabelStmt) ++labels;
        EXPECT_NE(hir.kind(items.back()), HirKind::LabelStmt)
            << "the labeled last item: the value comes last";
        EXPECT_EQ(hir.kind(items[items.size() - 2]), HirKind::LabelStmt)
            << "the labeled last item: a label stands right before the value it labels";
        if (labels == 2) {
            ++withTwoLabels;
            EXPECT_EQ(hir.kind(items[items.size() - 3]), HirKind::LabelStmt)
                << "the labeled last item: `in: a: r;` — both labels, in a row, before the value";
        } else if (labels == 1) {
            ++withOneLabel;
            ASSERT_EQ(items.size(), 2u) << "the labeled last item: `({ x: 5; })` — the label, then the value";
            EXPECT_EQ(hir.kind(items[1]), HirKind::Literal)
                << "the labeled last item: the value is the literal 5";
        }
    }
    EXPECT_EQ(withTwoLabels, 1u) << "the labeled last item: `in: a: r;` is placed with its two labels";
    EXPECT_EQ(withOneLabel, 1u) << "the labeled last item: `({ x: 5; })` is placed with its label";
}
