// ===========================================================================
// P69 L2b2 (lane `lm`) — THE SUBSCRIPT CONSTRAINT IS JUDGED AT EVERY SUBSCRIPT.
//
// THE PROPERTY THIS FILE OWNS: C 6.5.3.2p1 — one operand of `E1[E2]` a pointer to a
// complete object type, the other of integer type — is judged by the ANALYSIS at every
// subscript, wherever it stands: evaluated or not (`sizeof`, `__typeof__`, `_Alignof`,
// a `_Generic` controlling expression), through ONE law (type_rules.hpp
// `subscriptConstraintOf`) whose words the lowering reports too.
//
// ✔MEASURED 2026-11-05 (the lane's probes l2b2t, x86-64 and AAPCS64 ELF): gcc 13.3.0,
// clang 18.1.3, cl 19.51 and Apple clang refuse a struct subscripted in a return, in
// `sizeof` and in `__typeof__`, and an int subscripted in `__typeof__`, AT THE SUBSCRIPT
// ("subscripted value is neither array nor pointer nor vector" / "is not an array,
// pointer, or vector" / C2088, C2109). DSS refused only the evaluated one (the lowering
// judged it); in `sizeof` and `__typeof__` it left the subscript untyped and a later
// tier refused with an internal message.
//
// ── RED-ON-DISABLE ─────────────────────────────────────────────────────────
//   * the analysis's arm removed → every UNEVALUATED refusal cell counts 0 (the
//     evaluated one is the analysis's too: also 0 here, the lowering not having run);
//   * the law's container half disabled → the struct / int / two-pointer cells;
//   * the law's integer half disabled → the float- and struct-index cells.
// The controls (an array, the commuted spelling, a pointer and an integer, in the same
// unevaluated positions) pin that only a broken pair is refused.
// ===========================================================================

#include "core/types/parse_diagnostic.hpp"

#include "semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
#include <string>

using namespace dss;
using namespace dss::sem_test;

namespace {

constexpr char const* kDecls =
    "struct S { int x; };\n"
    "struct S s;\n"
    "int i;\n"
    "int a[4];\n"
    "int *p, *q;\n"
    "double d;\n";

[[nodiscard]] std::string dump(DiagnosticReporter const& r) {
    std::string out;
    for (auto const& dg : r.all())
        out += std::string{"\n    "} + std::string{diagnosticCodeName(dg.code)} + ": " + dg.actual;
    return out;
}

struct Cell { char const* what; char const* code; char const* subscript; bool refused; };

void expectCells(std::initializer_list<Cell> cells) {
    for (Cell const& c : cells) {
        std::string const src = std::string{kDecls} + c.code;
        auto cu = buildShippedUnit("c", {src});
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
        auto const& r = model.diagnostics();
        EXPECT_EQ(countCode(r, DiagnosticCode::S_SubscriptOperandsNotPointerAndInteger),
                  c.refused ? 1u : 0u)
            << c.what << " — expected " << (c.refused ? "REFUSED once" : "accepted")
            << ", as every reference; it said:" << dump(r) << "\n" << src;
        if (!c.refused) {
            EXPECT_FALSE(r.hasErrors()) << c.what << ":" << dump(r) << "\n" << src;
            continue;
        }
        std::size_t const at = src.find(c.subscript, std::string{kDecls}.size());
        ASSERT_NE(at, std::string::npos) << c.what;
        for (auto const& dg : r.all()) {
            if (dg.code != DiagnosticCode::S_SubscriptOperandsNotPointerAndInteger) continue;
            EXPECT_EQ(dg.span.start(), at)
                << c.what << ": the refusal stands at the subscript, where every reference "
                << "puts it\n" << src;
        }
    }
}

}  // namespace

TEST(SubscriptConstraint, AnUnevaluatedSubscriptIsJudgedAsAnEvaluatedOneIs) {
    expectCells({
        {"a struct subscripted, evaluated",
         "int main(void) { return s[0].x; }\n", "s[0]", true},
        {"a struct subscripted in sizeof",
         "int main(void) { return (int)sizeof(s[0]); }\n", "s[0]", true},
        {"a struct subscripted in __typeof__ at file scope",
         "__typeof__(s[0]) *t;\nint main(void) { return t != 0; }\n", "s[0]", true},
        {"an int subscripted in __typeof__",
         "__typeof__(i[0]) *t;\nint main(void) { return t != 0; }\n", "i[0]", true},
        {"a struct subscripted in _Alignof",
         "int main(void) { return (int)_Alignof(s[0]); }\n", "s[0]", true},
        {"a struct subscripted as _Generic's controlling expression",
         "int main(void) { return _Generic(s[0], int: 1, default: 2); }\n", "s[0]", true},
        {"two pointers subscripted in sizeof",
         "int main(void) { return (int)sizeof(p[q]); }\n", "p[q]", true},
        {"a float index in sizeof",
         "int main(void) { return (int)sizeof(p[d]); }\n", "p[d]", true},
        {"a struct index in sizeof",
         "int main(void) { return (int)sizeof(p[s]); }\n", "p[s]", true},
    });
}

TEST(SubscriptConstraint, OnlyABrokenPairIsRefused) {
    expectCells({
        {"an array in sizeof", "int main(void) { return (int)sizeof(a[0]); }\n", "a[0]", false},
        {"the commuted spelling in sizeof",
         "int main(void) { return (int)sizeof(0[a]); }\n", "0[a]", false},
        {"a pointer and an integer in __typeof__",
         "__typeof__(p[i]) *t;\nint main(void) { return t != 0; }\n", "p[i]", false},
        {"an array, evaluated", "int main(void) { a[1] = 42; return a[1]; }\n", "a[1]", false},
    });
}
