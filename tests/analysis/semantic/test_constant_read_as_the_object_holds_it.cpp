// ══ A CONSTANT IS READ AS THE OBJECT HOLDS IT ═══════════════════════════════════════
// (cycle P69 round 1, lane `lm`.)
//
// A named constant, or a member of a constant aggregate, read in a constant
// expression is the value the OBJECT holds: its initializer CONVERTED to the object's
// type (C 6.7.9p11), and for a bit-field restricted to the field's width (C
// 6.7.2.1p10). The CST evaluator folded the initializer's own value instead, in two
// places:
//   * a NAMED constant was read at its initializer's type — `static const unsigned u
//     = 5;` made `(unsigned long long)-u` 18446744073709551611 (C: 4294967291), a
//     `double r = 3` made `r / 2` 1, an `unsigned char` given 300 stayed 300;
//   * a BIT-FIELD member was read at its declared type alone — `unsigned b : 3` given
//     -1 was 4294967295 (C: 7), an `int b : 3` given 7 stayed 7 (C: -1).
// ✔MEASURED 2026-10-12 (pe64, baseline and release alike; the static assertions of 23
// forms, of which DSS folded 18 to another value than clang 18 and AppleClang 21,
// which fold C's value in every one).
//
// ★ WHY A WRONG FOLD HERE IS NOT A WRONG NUMBER BUT A DEAD PROGRAM. The lowering asks
// the same evaluator whether an `if` can complete (`ifNeverCompletes`): a condition it
// folds true over an arm that returns marks what follows the statement as never
// reached. The object itself holds C's value, so the program's own test goes the other
// way and walks into that mark: ✔MEASURED the same day, 17 of the 23 forms written as
// `if (<the wrong fold says true>) return 7; return 42;` died of an illegal instruction
// where gcc 13, mingw-w64 gcc 13.2, clang 18 and AppleClang 21 exit 42 in all 23.
// So the semantic tier's resolver AND the lowering's are each pinned below — they are
// two functions handing the evaluator the same fact, and either alone going back is
// the defect whole in its tier.
//
// ⚠ ONE CELL IS NOT HERE, AND IS STILL WRONG IN THE SAME WAY ON BOTH SIDES: a bit-field
// narrower than `int` is `int` in an expression (C 6.3.1.1p2), and DSS computes it in
// its declared type, at run time and in a fold alike — `v.b - 6 < 0` over an `unsigned
// b : 3` holding 5 is false here and true on the four references. The fold must not
// know more C than the code it decides reachability for, so the promotion goes into
// both together or into neither.
//
// The run-time half — the objects' own values — is `examples/c`'s business; a unit
// case cannot run a program.

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
#include "hir/lowering/cst_to_hir.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

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

[[nodiscard]] std::string allActuals(SemanticModel const& m, DiagnosticCode code) {
    std::string out;
    for (auto const& d : m.diagnostics().all())
        if (d.code == code) out += "\n    " + d.actual;
    return out;
}

// How many positions of `src` the lowering marks as never reached (the `Unreachable`
// it puts after a statement control cannot continue past).
[[nodiscard]] std::size_t positionsMarkedNeverReached(std::string const& src) {
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
    EXPECT_EQ(reporter.errorCount(), 0u) << src;
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

// The objects both tiers read. `second` shares its unit with another field; `unnamed`
// has a field no initializer names.
constexpr char const* kBitFields =
    "struct U3 { unsigned b : 3; };\n"
    "struct I3 { int b : 3; };\n"
    "struct S3 { signed int b : 3; };\n"
    "struct L40 { unsigned long long b : 40; };\n"
    "struct B1 { _Bool b : 1; };\n"
    "struct AB { unsigned a : 5; unsigned b : 3; };\n"
    "static const struct U3 fits = { .b = 5 };\n"
    "static const struct U3 wraps = { .b = -1 };\n"
    "static const struct U3 nine = { 9 };\n"
    "static const struct I3 i7 = { 7 };\n"
    "static const struct S3 s7 = { 7 };\n"
    "static const struct L40 wide = { .b = -1 };\n"
    "static const struct B1 flag = { 2 };\n"
    "static const struct AB second = { .a = 1, .b = 15 };\n"
    "static const struct AB unnamed = { .a = 33 };\n";

constexpr char const* kNamed =
    "static const unsigned u = 5;\n"
    "static const unsigned m1 = -1;\n"
    "static const double r = 3;\n"
    "static const unsigned char uc = 300;\n"
    "static const signed char sc = 200;\n"
    "static const unsigned short us = -1;\n"
    "static const _Bool flagged = 2;\n"
    "static const float fl = 0.1;\n"
    "static const int wideInt = 0x100000001;\n";

} // namespace

TEST(ConstantReadAsTheObjectHoldsIt, ABitFieldMemberIsReducedToItsWidth) {
    std::string const src = std::string{kBitFields} +
        "int main(void) {\n"
        "    const struct U3 local = { .b = -1 };\n"
        "    _Static_assert(fits.b == 5, \"a value the field holds\");\n"
        "    _Static_assert(wraps.b == 7, \"-1 in an unsigned field of 3 bits\");\n"
        "    _Static_assert(nine.b == 1, \"9 in an unsigned field of 3 bits\");\n"
        "    _Static_assert(i7.b == -1, \"7 in an int field of 3 bits\");\n"
        "    _Static_assert(s7.b == -1, \"7 in a signed int field of 3 bits\");\n"
        "    _Static_assert(wide.b == 0xFFFFFFFFFFull, \"-1 in an unsigned long long field of 40 bits\");\n"
        "    _Static_assert(flag.b == 1, \"2 in a _Bool field\");\n"
        "    _Static_assert(second.a == 1 && second.b == 7, \"15 in the second field of a unit\");\n"
        "    _Static_assert(unnamed.a == 1 && unnamed.b == 0, \"33 in 5 bits, and a field no initializer names\");\n"
        "    _Static_assert(local.b == 7, \"an automatic object's field\");\n"
        "    return 0;\n"
        "}\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 0u)
        << "the fields read at another value:"
        << allActuals(model, DiagnosticCode::S_StaticAssertFailed);
    EXPECT_FALSE(model.hasErrors());
    // What was folded before: the initializer at the declared type, the width unread.
    // Each is FALSE of the object and must fail, by this code.
    char const* const wasSilentlyTrue[] = {
        "_Static_assert(wraps.b == 4294967295u, \"m\");",
        "_Static_assert(nine.b == 9, \"m\");",
        "_Static_assert(i7.b == 7, \"m\");",
        "_Static_assert(s7.b == 7, \"m\");",
        "_Static_assert(wide.b == 0xFFFFFFFFFFFFFFFFull, \"m\");",
        "_Static_assert(second.b == 15, \"m\");",
        "_Static_assert(unnamed.a == 33, \"m\");",
    };
    for (char const* line : wasSilentlyTrue) {
        auto m = analyzeC(std::string{kBitFields} + "int main(void){ " + line + " return 0; }\n");
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 1u) << line;
    }
}

TEST(ConstantReadAsTheObjectHoldsIt, ANamedConstantIsReadAtItsDeclaredType) {
    std::string const src = std::string{kNamed} +
        "enum E : unsigned { A = 5 };\n"
        "int main(void) {\n"
        "    const unsigned lu = 5;\n"
        "    _Static_assert(u == 5, \"a value the type holds\");\n"
        "    _Static_assert((unsigned long long)-u == 4294967291ull, \"an unsigned negated, widened\");\n"
        "    _Static_assert(-u > 0, \"an unsigned negated is positive\");\n"
        "    _Static_assert((long long)m1 == 4294967295ll, \"an unsigned given -1, widened\");\n"
        "    _Static_assert(r / 2 == 1.5, \"a double given an int divides as a double\");\n"
        "    _Static_assert(uc == 44, \"an unsigned char given 300\");\n"
        "    _Static_assert(sc == -56, \"a signed char given 200\");\n"
        "    _Static_assert(us == 65535, \"an unsigned short given -1\");\n"
        "    _Static_assert(flagged == 1, \"a _Bool given 2\");\n"
        "    _Static_assert((fl == 0.1) == 0, \"a float given 0.1 is not the double 0.1\");\n"
        "    _Static_assert(wideInt == 1, \"an int given 0x100000001\");\n"
        "    _Static_assert((unsigned long long)-A == 4294967291ull, \"an enumerator of a fixed unsigned type\");\n"
        "    _Static_assert((unsigned long long)-lu == 4294967291ull, \"an automatic constant\");\n"
        "    return 0;\n"
        "}\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 0u)
        << "the constants read at another value:"
        << allActuals(model, DiagnosticCode::S_StaticAssertFailed);
    EXPECT_FALSE(model.hasErrors());
    // What was folded before: the initializer's own value at the initializer's type.
    char const* const wasSilentlyTrue[] = {
        "_Static_assert((unsigned long long)-u == 18446744073709551611ull, \"m\");",
        "_Static_assert((-u > 0) == 0, \"m\");",
        "_Static_assert((long long)m1 == -1, \"m\");",
        "_Static_assert(r / 2 == 1, \"m\");",
        "_Static_assert(uc == 300, \"m\");",
        "_Static_assert(sc == 200, \"m\");",
        "_Static_assert(flagged == 2, \"m\");",
        "_Static_assert(fl == 0.1, \"m\");",
        "_Static_assert(wideInt == 0x100000001, \"m\");",
    };
    for (char const* line : wasSilentlyTrue) {
        auto m = analyzeC(std::string{kNamed} + "int main(void){ " + line + " return 0; }\n");
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 1u) << line;
    }
}

// THE LOWERING HANDS ITS EVALUATOR THE SAME FACTS. Two programs over the same
// objects: in the first every condition is FALSE in C, so nothing after an `if` may
// be marked as never reached (each function was marked, and each died, before); in
// the second every condition is TRUE, its arm returns, and the position after each
// `if` IS marked — which is what shows the evaluator folded the constant at all, at
// C's value, rather than declining it.
TEST(ConstantReadAsTheObjectHoldsIt, TheLoweringMarksNoReachedCodeAsNeverReached) {
    std::string const objects = std::string{kBitFields} + kNamed;
    std::string const falseInC = objects +
        "int a(void) { if (wraps.b != 7) return 7; return 42; }\n"
        "int b(void) { if (i7.b != -1) return 7; return 42; }\n"
        "int c(int x) { if ((unsigned long long)-u > 0xFFFFFFFFull) return 3; return x; }\n"
        "int d(int x) { if (uc != 44) return 3; return x; }\n"
        "int main(void) { return a() + b() + c(0) + d(0); }\n";
    ASSERT_EQ(parseErrorsFor(falseInC), 0u);
    EXPECT_EQ(positionsMarkedNeverReached(falseInC), 0u)
        << "a condition C makes false was folded true: the code after its statement "
           "is marked as never reached, and the program's own test walks into it";

    std::string const trueInC = objects +
        "int a(void) { if (wraps.b == 7) return 7; return 42; }\n"
        "int b(void) { if (i7.b == -1) return 7; return 42; }\n"
        "int c(int x) { if ((unsigned long long)-u == 4294967291ull) return 3; return x; }\n"
        "int d(int x) { if (uc == 44) return 3; return x; }\n"
        "int main(void) { return a() + b() + c(0) + d(0); }\n";
    ASSERT_EQ(parseErrorsFor(trueInC), 0u);
    EXPECT_EQ(positionsMarkedNeverReached(trueInC), 4u)
        << "the premise: each of the four conditions folds, true, over an arm that "
           "returns — one mark per function";
}
