// ===========================================================================
// P68 round 13 (lane `cs`, found by the static-initializer item) — the semantic walks that
// STOPPED SILENTLY at a fixed depth and answered as if they had finished:
//   * `descendInitWrappers` (64 wrappers) returned the 64th wrapper AS the initializer, so a
//     constexpr pointer's null cast inside 64 or more parentheses was refused unseen;
//   * the constexpr null-pointer cast chain (16 casts) refused a longer chain;
//   * the constexpr aggregate walk (100000 items) stopped as if it had checked everything —
//     the stack pops the LAST element first, so the FIRST elements were never checked and a
//     non-constant one there was ADMITTED;
//   * the `typeof` bit-field probe (64 wrappers) never reached a member access below them,
//     so a bit-field operand in 64 or more parentheses escaped C 6.7.2.5's constraint;
//   * the nodiscard discard walk (64 ancestors) stopped before it saw the statement, so a
//     call discarded inside enough parentheses drew no warning.
// Each walk now goes step by step to a strict descendant (or ancestor), so it ends on any
// tree; the tree's own node count bounds it, and only a cyclic (malformed) tree could
// exceed that — which stops the process loud (`failOnCyclicTree`), never an answer as if
// the walk had ended. No input-proportional recursion anywhere: every walk is a loop.
//
// Each pin is RED-ON-DISABLE against its cap restored.
// ===========================================================================

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/data_model.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include "semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <stdexcept>
#include <string>

using namespace dss;
using namespace dss::sem_test;

namespace {

[[nodiscard]] SemanticModel analyzeC(std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
}

[[nodiscard]] std::string parens(int depth, std::string const& inner) {
    return std::string(static_cast<std::size_t>(depth), '(') + inner
         + std::string(static_cast<std::size_t>(depth), ')');
}

[[nodiscard]] std::string describe(SemanticModel const& m) {
    std::string out;
    for (auto const& d : m.diagnostics().all()) {
        out += "\n  ";
        out += diagnosticCodeName(d.code);
        out += ": ";
        out += d.actual;
    }
    return out;
}

} // namespace

// RED-ON-DISABLE: restore `descendInitWrappers`' 64-step cap (this draws
// S_ConstexprNonConstantInitializer). The CONTROL is the same shape at depth 4.
TEST(UncappedSemanticWalks, AConstexprNullCastInsideDeepParenthesesIsAccepted) {
    for (int depth : {4, 70}) {
        auto const m = analyzeC("constexpr int *p = " + parens(depth, "(int *)0") + ";\n"
                                "int main(void) { return p == 0 ? 0 : 1; }\n");
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_ConstexprNonConstantInitializer), 0u)
            << "depth " << depth << describe(m);
    }
}

// RED-ON-DISABLE: restore the 16-step cap of the null-pointer cast chain.
TEST(UncappedSemanticWalks, AConstexprChainOfManyPointerCastsOverZeroIsAccepted) {
    for (int casts : {2, 20}) {
        std::string chain;
        for (int i = 0; i < casts; ++i) chain += (i % 2 == 0) ? "(int *)" : "(void *)";
        auto const m = analyzeC("constexpr int *p = " + chain + "0;\n"
                                "int main(void) { return p == 0 ? 0 : 1; }\n");
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_ConstexprNonConstantInitializer), 0u)
            << casts << " casts" << describe(m);
    }
}

// RED-ON-DISABLE: restore the 100000-step cap (this draws nothing).
TEST(UncappedSemanticWalks, EveryElementOfALargeConstexprArrayIsChecked) {
    constexpr int kElements = 100001;
    std::string src = "int g;\nconstexpr int a[" + std::to_string(kElements) + "] = { g";
    src.reserve(src.size() + static_cast<std::size_t>(kElements) * 3 + 64);
    for (int i = 1; i < kElements; ++i) src += ", 0";
    src += " };\nint main(void) { return a[1]; }\n";
    auto const m = analyzeC(src);
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_ConstexprNonConstantInitializer), 1u)
        << "the first element is not a constant";
}

// C 6.7.2.5: `typeof` of a bit-field is a constraint violation however it is parenthesized.
// RED-ON-DISABLE: restore the probe's 64-step cap (the deep case draws nothing).
TEST(UncappedSemanticWalks, ABitFieldTypeofOperandIsRefusedAtAnyParenthesisDepth) {
    for (int depth : {1, 70}) {
        auto const m = analyzeC("struct S { unsigned f : 3; };\nstruct S s;\n"
                                "typeof(" + parens(depth, "s.f") + ") v;\n");
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_TypeofBitfieldOperand), 1u)
            << "depth " << depth << describe(m);
    }
}

// A discarded nodiscard result warns however deeply the call is parenthesized (gcc and clang
// warn on `(g());`). RED-ON-DISABLE: restore the walk's 64-step cap (the deep case is silent).
TEST(UncappedSemanticWalks, ADiscardedResultWarnsAtAnyParenthesisDepth) {
    for (int depth : {1, 70}) {
        auto const m = analyzeC("int g(void) __attribute__((warn_unused_result));\n"
                                "int g(void) { return 1; }\n"
                                "int main(void) { " + parens(depth, "g()") + "; return 0; }\n");
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_NodiscardResultDiscarded), 1u)
            << "depth " << depth << describe(m);
    }
}
