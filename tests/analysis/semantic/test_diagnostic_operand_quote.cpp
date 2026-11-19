// analysis/semantic/test_diagnostic_operand_quote — A DIAGNOSTIC'S SIZE DOES NOT GROW WITH THE OPERAND IT NAMES
// ([[D-DIAG-A-DIAGNOSTIC-QUOTES-ITS-OPERAND-WHOLE]], cycle P69 f26d, lane `cs`).
//
// ✔MEASURED before the fix, conditionals nested 8 / 32 / 128 deep with a pointer arm against the integer `1` at
// every level (one S_ConditionalOperandTypeMismatch per level), one level per line: DSS printed 4181 / 20047 /
// 36259 bytes, every warning quoting its pointer arm — the whole nest below it — where gcc 13.3.0 (1003 / 3739 /
// 14811) and clang 18.1.3 (3133 / 16390 / 69895) stay linear. The cells here, through the shipped C front end:
//   * the nest, one level per line AND on one line: every warning is the SAME sentence whatever the depth, one line,
//     naming the arms by what they are and quoting neither (clang's shape: the caret marks the integer arm);
//   * an operand quoted on purpose (a case label that is not a constant): one line and bounded by the configured
//     limit whatever the operand's size, the same cut at 32 terms as at 128;
//   * THE NEGATIVE CELL: a short operand is still named in full.

#include "semantic_test_fixture.hpp"

#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_quote.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace dss {
namespace {

using sem_test::analyzeShipped;

[[nodiscard]] std::vector<std::string> actualsOf(SemanticModel const& m, DiagnosticCode code) {
    std::vector<std::string> out;
    for (auto const& d : m.diagnostics().all()) {
        if (d.code == code) out.push_back(d.actual);
    }
    return out;
}

// `(c ? (c ? … p … : 1) : 1)`, `depth` levels, each a pointer arm against `1`.
[[nodiscard]] std::string conditionalNest(int depth, bool onePerLine) {
    std::string e = "p";
    for (int k = 0; k < depth; ++k) e = onePerLine ? "(c\n ? " + e + "\n : 1)" : "(c ? " + e + " : 1)";
    return "int main(void) {\n  int c = 1;\n  int *p = 0;\n  (void)" + e + ";\n  return 0;\n}\n";
}

TEST(DiagnosticOperandQuote, EveryLevelOfAConditionalNestIsTheSameBoundedSentence) {
    std::size_t const cap = DiagnosticBudget::libraryDefault().maxPerCode();
    std::string reference;
    for (bool const onePerLine : {true, false}) {
        for (int const depth : {8, 32, 128}) {
            auto const model = analyzeShipped("c", {conditionalNest(depth, onePerLine)});
            auto const actuals = actualsOf(model, DiagnosticCode::S_ConditionalOperandTypeMismatch);
            SCOPED_TRACE(std::to_string(depth) + (onePerLine ? " deep, one level per line" : " deep, one line"));
            ASSERT_EQ(actuals.size(), std::min<std::size_t>(static_cast<std::size_t>(depth), cap));
            if (reference.empty()) reference = actuals.front();
            for (auto const& a : actuals) {
                EXPECT_EQ(a, reference) << "a level's warning grew with the nest below it";
                EXPECT_EQ(a.find('\n'), std::string::npos) << a;
                EXPECT_EQ(a.find("(c"), std::string::npos) << "an arm is quoted: " << a;
            }
        }
    }
    EXPECT_LT(reference.size(), 200u) << reference;
}

// `switch (n) { case n + n + … : ; }` — a label of `terms` terms, one per line, that is not a constant.
[[nodiscard]] std::string caseLabelOf(int terms) {
    std::string e = "n";
    for (int k = 1; k < terms; ++k) e += "\n      + n";
    return "int f(int n) {\n  switch (n) {\n    case " + e + ": return 1;\n  }\n  return 0;\n}\n";
}

TEST(DiagnosticOperandQuote, AQuotedOperandIsOneBoundedLineWhateverItsSize) {
    std::size_t const limit = diagnosticQuoteLimit();
    std::string at32;
    for (int const terms : {32, 128}) {
        auto const model = analyzeShipped("c", {caseLabelOf(terms)});
        auto const actuals = actualsOf(model, DiagnosticCode::S_CaseLabelNotConstant);
        SCOPED_TRACE(std::to_string(terms) + " terms");
        ASSERT_EQ(actuals.size(), 1u);
        std::string const& a = actuals.front();
        EXPECT_EQ(a.find('\n'), std::string::npos) << a;
        EXPECT_NE(a.find("…"), std::string::npos) << "a quote past the limit says it is cut: " << a;
        std::string const prose = "case label '' is not an integer constant expression (C 6.8.4.2p3)";
        EXPECT_LE(a.size(), prose.size() + limit + std::string{"…"}.size()) << a;
        if (at32.empty()) at32 = a;
        EXPECT_EQ(a, at32) << "the quote grew with the operand past the limit";
    }
}

TEST(DiagnosticOperandQuote, AShortOperandIsStillNamedInFull) {
    auto const one = analyzeShipped("c", {caseLabelOf(1)});
    EXPECT_EQ(actualsOf(one, DiagnosticCode::S_CaseLabelNotConstant),
              (std::vector<std::string>{"case label 'n' is not an integer constant expression (C 6.8.4.2p3)"}));
    // Eight terms on eight lines are 29 bytes on one: under the limit, so quoted whole.
    auto const eight = analyzeShipped("c", {caseLabelOf(8)});
    EXPECT_EQ(actualsOf(eight, DiagnosticCode::S_CaseLabelNotConstant),
              (std::vector<std::string>{"case label 'n + n + n + n + n + n + n + n' is not an integer constant "
                                        "expression (C 6.8.4.2p3)"}));
}

} // namespace
} // namespace dss
