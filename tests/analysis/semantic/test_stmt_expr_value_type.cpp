// P32 lane A — [[D-C-SUBTREE-TYPE-WALKS-INTO-A-STATEMENT-EXPRESSION-BODY]]
//
// The semantic tier's expression typer (`subtreeType`) used to descend into a GNU
// statement expression's BODY and take a type from a DECLARATION inside it, so a
// use of the construct's VALUE was judged against the wrong operand.
//
// ✔MEASURED through the shipped CLI at the pre-change HEAD, bisected so the
// trigger is exact rather than guessed:
//     `*({ *q; })`                        ✅   no declaration in the body
//     `*({ int *r = p; r; })`             ✅   declarator type == yielded type
//     `int x = *({ int **q = &p; *q; });` ✗    false `S0003`
// ⇒ the trigger is a declaration whose type DIFFERS from what the body yields.
//
// ★★ WHY THIS FILE EXISTS BESIDE A RUNNABLE EXAMPLE. The accepting half is a
// program that must BUILD, and `examples/c/stmt_expr_value_type_from_last_item/`
// owns it. What an example structurally cannot carry is the REFUSING half — and
// the refusing half is where this fix could go wrong in the other direction. A
// statement expression whose last item is NOT an expression statement has type
// `void` (GNU C 6.1), and using it as a VALUE is refused by gcc and by clang;
// the old descent would happily have answered with some declarator's type
// instead, which is a silent answer to a question the language says has none.
// So the `void` cases are pinned here, by CODE, not by "the build failed".
//
// ★ AND THE FIX IS DERIVED FROM THE LOWERING, not written twice: the value is
// the last item's expression, recognised by HIR kind `ExprStmt` exactly as
// `stmtExprItems` recognises it in `cst_to_hir.cpp`. The two tiers therefore
// cannot disagree about what a body yields, which is the property the pins below
// are really protecting.
//
// ⓘ THE VLA C4a-LOCAL NOTE IN THE SAME FILE WAS CHECKED AND IS NOT THIS BUG —
// the registry row asked, and the answer is measured rather than assumed. That
// note is about a node the earlier pass already STAMPED with a decayed pointer
// type, which `subtreeType` short-circuits on BEFORE any structural arm runs; it
// is a stamped-node question, not a descent question, and no boundary in the walk
// can reach it. It stayed its own deferral
// ([[D-CSUBSET-VLA-PTR-INIT-FORM-TYPING]]) — and CLOSED in P34 from a THIRD place
// again: not this walk and not that stamp, but the DECLARATOR resolver, which had
// been building `Ptr<incompleteArray<int>>` for `(*p)[n]` whenever an initializer
// was present. The conclusion recorded here — that it is not this bug — held.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "hir/lowering/cst_to_hir.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

using namespace dss;
using namespace dss::sem_test;

namespace {

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

// The `void`-statement-expression refusal fires at the HIR LOWERING tier, not in
// `analyze()` — whether a VALUE is required is a property of the lowering
// position, and the lowering is the tier that knows which position it is in. A
// pin that read `model.diagnostics()` alone would report ZERO for a source the
// compiler in fact refuses: green, asserting nothing. (This exact mistake is
// recorded in `test_gnu_extension_and_statement_expr.cpp`; it is not repeated.)
[[nodiscard]] std::size_t loweringCodeCount(std::string const& src,
                                            DiagnosticCode code) {
    auto cu = buildShippedUnit("c", {src});
    SemanticModel model =
        analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    DiagnosticReporter reporter{};
    auto lowered = lowerToHir(model, reporter);
    (void)lowered;
    return countCode(reporter, code) + countCode(model.diagnostics(), code);
}

} // namespace


// ★ THE ROW'S EXACT REPRO. The dereference is checked against the body's YIELDED
// type (`int *`), not against the `int **` a declaration inside the body happens
// to introduce. Asserting on `S_TypeMismatch` and not merely on "zero errors" is
// deliberate: this is a false-POSITIVE bug, so the pin names the diagnostic that
// must not appear.
TEST(StmtExprValueType, DeclarationInsideTheBodyDoesNotTypeTheConstruct) {
    std::string const src =
        "int main(void){ int v = 7; int *p = &v;\n"
        "                int x = *({ int **q = &p; *q; });\n"
        "                return x - 7; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u)
        << "the body yields `int *`; the `int **` declared inside it is not the "
           "operand of the dereference";
}

// The two bisect CONTROLS, which passed before the fix and must still pass: a
// body with no declaration, and a body whose declarator type happens to equal
// what it yields. Without these, a fix that simply stopped typing statement
// expressions at all would look identical to the real one.
TEST(StmtExprValueType, TheTwoShapesThatAlreadyWorkedStillWork) {
    std::string const src =
        "int main(void){ int v = 7; int *p = &v;\n"
        "                int a = *({ p; });\n"
        "                int b = *({ int *r = p; r; });\n"
        "                return a + b - 14; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
}

// A body whose declarations share NOTHING with what it yields — the walk that
// took the first declarator would come back with `double`.
TEST(StmtExprValueType, ADeclarationOfAnUnrelatedTypeDoesNotLeakOut) {
    std::string const src =
        "int main(void){ int c = ({ double d = 0.5; int t = 10; (void)d; t; });\n"
        "                return c - 10; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
}

// ★★ THE REFUSING HALF — the half a corpus example cannot carry. A body whose
// LAST item is a declaration yields nothing at all, and using that as a value is
// refused by gcc ("void value not ignored as it ought to be") and by clang. The
// typer must answer "no type" rather than the declaration's type, which is what
// lets the lowering emit its refusal instead of silently accepting.
TEST(StmtExprValueType, ABodyEndingInADeclarationHasNoValueAndIsRefused) {
    std::string const src =
        "int main(void){ int y = ({ int t = 1; });\n"
        "                return y; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u)
        << "the construct PARSES; the refusal below is semantic, which is what "
           "makes it possible to state a reason";
    EXPECT_EQ(loweringCodeCount(src, DiagnosticCode::S_StatementExprHasNoValue), 1u)
        << "a `void` statement expression in VALUE position must be refused, not "
           "silently typed from whatever the body declared";
}

// The same `void` body in a DISCARD position is legal — it is a block, and its
// value is nobody's business. The pair is what keeps the refusal above from
// being over-broad.
TEST(StmtExprValueType, TheSameVoidBodyIsFineWhereNoValueIsRequired) {
    std::string const src =
        "int main(void){ ({ int t = 1; (void)t; });\n"
        "                return 0; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    EXPECT_EQ(loweringCodeCount(src, DiagnosticCode::S_StatementExprHasNoValue), 0u);
}

// `sizeof` reads the semantic tier's type DIRECTLY, with no lowering behind it —
// so it is the sharpest available probe of what `subtreeType` actually answered.
// A body yielding `char` must fold to 1, not to the width of the `double` its
// body declares.
TEST(StmtExprValueType, SizeofReadsTheYieldedTypeAndNotADeclaredOne) {
    std::string const src =
        "int main(void){ char arr[sizeof(({ double d = 0.5; char ch = 'a'; "
        "(void)d; ch; }))];\n"
        "                return (int)sizeof(arr) - 1; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NonConstantArrayLength),
              0u)
        << "the fold must SUCCEED — a declined fold is the silent failure mode "
           "this probe exists to catch";
}
