// P32 lane A — [[D-CSUBSET-NODISCARD-INDIRECT-DISCARD-CONTEXT]]
//                and [[D-CSUBSET-ATTRIBUTE-PARAM-POSITION]]'s semantic half
//
// ── the nodiscard discard CONTEXT ────────────────────────────────────────────
// The discard test was TWO-HOP-EXACT — parent(call)==`expression` and
// grandparent(call)==`exprStmt` — which is right for the dominant `f();` idiom
// and blind to every other way C discards a value.
//
// ✔MEASURED at the pre-change HEAD through the shipped CLI, one TU per shape and
// a bare `g();` positive control alongside: `(g());`, `g(), calls;`,
// `for(g();;)` and `for(;;g())` ALL compiled with NO diagnostic. ✔MEASURED
// against BOTH references, same shapes, same one-TU-per-case discipline: gcc
// 13.3.0 and clang 19.1.7 warn on all four.
//
// ★★ THE PINS COME IN PAIRS, and that is the design rather than thoroughness for
// its own sake. A discard test can fail in two opposite ways: it can MISS a
// discard (the defect this row names — a warning-only miss, never wrong bytes),
// or it can FIRE on a value that is genuinely used (a false positive on legal C,
// which is worse, because the programmer cannot make it go away). So every
// must-warn shape below has a must-NOT-warn sibling — the `for` CONDITION beside
// the `for` init and step, the assignment and the nested call beside the
// statement discards — and a fix that widened the walk too far reddens the second
// half exactly as a fix that never landed reddens the first.
//
// ★ `(void)g();` IS A MUST-NOT-WARN PIN, and it is a DECISION, not an omission.
// ✔MEASURED: gcc warns, clang does not. The cast is the universal idiom for "I
// meant to discard this", so the disjunction rule leaves DSS silent — the same
// reasoning that made the keyword-attribute token class take clang's superset
// elsewhere in this cycle. If a future cycle wants gcc's behaviour it must change
// THIS pin first, deliberately.
//
// ── the parameter-attribute EFFECT ──────────────────────────────────────────
// [[D-CSUBSET-ATTRIBUTE-PARAM-POSITION]]'s runnable example can only witness that
// the decorated parameter PARSES, and a grammar-only fix passes that while the
// attributes reach no reader at all. The pin at the bottom of this file asserts
// the EFFECT instead: a `deprecated` attribute written on a PARAMETER must
// produce a real warn-on-use diagnostic at the parameter's USE site — which
// ✔MEASURED both references also do — and that can only happen if the run reached
// `scanAttributeSemantics` and landed on the parameter's symbol.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

using namespace dss;
using namespace dss::sem_test;

namespace {

// Every source below declares the same nodiscard function, so the ONE thing that
// varies between a must-warn case and a must-not-warn case is the discard
// CONTEXT — which is the variable under test.
constexpr char const* kPrelude =
    "int g(void) __attribute__((warn_unused_result));\n"
    "int g(void){ return 1; }\n";

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

// How many discard warnings does this function BODY produce? The body is spliced
// into `main` so every case is one whole translation unit and no case can mask
// the next.
[[nodiscard]] std::size_t discardWarnings(std::string const& body) {
    std::string const src =
        std::string{kPrelude} + "int main(void){ " + body + " return 0; }\n";
    EXPECT_EQ(parseErrorsFor(src), 0u) << "fixture must parse: " << body;
    auto model = analyzeC(src);
    return countCode(model.diagnostics(),
                     DiagnosticCode::S_NodiscardResultDiscarded);
}

} // namespace


// The idiom that already worked. It is a CONTROL, not a feature pin: if it ever
// stops warning, every "must not warn" pin below becomes vacuous, because a test
// suite in which nothing warns passes them all.
TEST(NodiscardDiscardContext, BareDiscardStillWarns) {
    EXPECT_EQ(discardWarnings("g();"), 1u);
}

// ── the four shapes the row names, each MEASURED to warn on gcc AND clang ────

TEST(NodiscardDiscardContext, ParenWrappedDiscardWarns) {
    EXPECT_EQ(discardWarnings("(g());"), 1u)
        << "a parenthesised expression forwards the value unchanged, so the "
           "discard is the same discard";
}

TEST(NodiscardDiscardContext, CommaOperandDiscardWarns) {
    EXPECT_EQ(discardWarnings("g(), 1;"), 1u)
        << "the left operand of a comma is evaluated and thrown away";
}

TEST(NodiscardDiscardContext, ForStepClauseDiscardWarns) {
    EXPECT_EQ(discardWarnings("for(;;g()) break;"), 1u);
}

TEST(NodiscardDiscardContext, ForInitClauseDiscardWarns) {
    EXPECT_EQ(discardWarnings("for(g();;) break;"), 1u);
}

// ── the siblings: contexts where the value IS used, which must stay silent ───

// ★ THE SHARPEST ONE. The `for` init, condition and step are the SAME rule in the
// SAME node — only the `;`-separated CLAUSE INDEX tells them apart, and the index
// is counted with `hirLowering.forClauseSeparator`, the very key the CST→HIR
// `for` prologue segments the header with. A walk that treated the host node as a
// discard position wholesale would warn here, on code neither reference warns
// about.
TEST(NodiscardDiscardContext, ForConditionIsNotADiscardAndMustNotWarn) {
    EXPECT_EQ(discardWarnings("for(;g();) break;"), 0u);
}

TEST(NodiscardDiscardContext, CastToVoidStaysSilentFollowingClang) {
    EXPECT_EQ(discardWarnings("(void)g();"), 0u)
        << "gcc warns and clang does not; the cast is the idiom for a deliberate "
           "discard, so DSS follows clang — changing this is a decision, not a fix";
}

TEST(NodiscardDiscardContext, AssignedResultMustNotWarn) {
    EXPECT_EQ(discardWarnings("int x = g(); return x - 1;"), 0u);
}

TEST(NodiscardDiscardContext, ResultPassedToAnotherCallMustNotWarn) {
    std::string const src =
        std::string{kPrelude} +
        "int h(int a){ return a; }\n"
        "int main(void){ return h(g()) - 1; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NodiscardResultDiscarded), 0u);
}

// ★ A non-comma binary operator must NOT be transparent. `binaryExpr` is the
// SHARED Pratt wrapper for every binary operator, so a fix that declared the RULE
// transparent instead of the rule-plus-OPERATOR would warn here — the entry names
// `Comma` specifically, and this is the pin that proves the qualifier is live.
TEST(NodiscardDiscardContext, ANonCommaBinaryOperatorIsNotTransparent) {
    EXPECT_EQ(discardWarnings("1 + g();"), 0u);
}


// ── [[D-CSUBSET-ATTRIBUTE-PARAM-POSITION]] — the EFFECT, not the parse ───────

// ★★ THE PIN THAT SEPARATES THE FIX FROM A GRAMMAR-ONLY WIDENING. A parameter
// attribute that merely PARSES reaches no reader and is dropped in silence. This
// asserts the opposite: the attribute is scanned, its effect lands on the
// parameter's symbol, and the USE of that parameter warns — which is exactly what
// ✔MEASURED gcc 13.3.0 and clang 19.1.7 both do for this source.
TEST(ParamPositionAttribute, DeprecatedOnAParameterIsHonouredAtTheUseSite) {
    std::string const src =
        "int f(__attribute__((deprecated)) int p){ return p; }\n"
        "int main(void){ return f(0); }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_DeprecatedSymbolUsed), 1u)
        << "the attribute must reach `scanAttributeSemantics` and land on the "
           "PARAMETER's symbol — a run that only parsed would leave this at 0";
}

// The undecorated control. Without it the pin above could pass for the wrong
// reason (every parameter use warning, for instance).
TEST(ParamPositionAttribute, AnUndecoratedParameterDoesNotWarn) {
    std::string const src =
        "int f(int p){ return p; }\n"
        "int main(void){ return f(0); }\n";
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_DeprecatedSymbolUsed), 0u);
}

// Both positions and both spellings PARSE, and the parameter still binds. A
// prefix rule that swallowed its declaration would parse perfectly and bind
// nothing — which is why the assertion is "the program still type-checks", not
// "it parsed".
TEST(ParamPositionAttribute, EveryDecoratedPositionStillBindsItsParameter) {
    std::string const src =
        "static int only_second(int a, [[maybe_unused]] int b){ return a; }\n"
        "static int gnu_unused(int a, __attribute__((__unused__)) int b){ return a; }\n"
        "static int after_decl(int a, int b __attribute__((__unused__))){ return a; }\n"
        "int decl_only([[maybe_unused]] int p);\n"
        "int decl_only(int p){ return p; }\n"
        "int main(void){ return only_second(20,1) + gnu_unused(12,1)\n"
        "                     + after_decl(6,1) + decl_only(4); }\n";
    EXPECT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ArgCountMismatch),
              0u)
        << "the attribute run must be STRIPPED from the positional child count — "
           "an unstripped prefix makes the head become the declarator and the "
           "arity go wrong";
}
