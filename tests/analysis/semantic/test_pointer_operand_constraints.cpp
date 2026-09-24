// ===========================================================================
// P68 round 9 (lane `cs`) — THE POINTER HALF OF THE ARITHMETIC OPERATORS' OPERAND
// CONSTRAINTS, found while measuring
// D-C-INCOMPATIBLE-POINTER-CONVERSION-REFUSED-WHERE-EVERY-REFERENCE-WARNS and fixed
// with it (the coordinator's ruling: one constraint family).
//
// THE PROPERTY THIS FILE OWNS: a pointer operand (an array or a function designator
// decaying to one) takes part in C's arithmetic operators ONLY as C23 6.5.7p2 lets it
// — `+` beside an integer, `-` before an integer or a pointer — and in no
// multiplicative (6.5.6p2), shift (6.5.8p2), bitwise (6.5.11-13) or unary arithmetic
// (6.5.4.3) operator; a compound assignment takes its own constraint (6.5.17.3p1).
// Every other pairing is refused ONCE (S_TypeMismatch at the operator), and a refused
// operation has NO TYPE, so the context it lands in does not judge it again.
//
// ✔REFERENCE VOTES, 2026-09-23, each case its own translation unit, each reference
// probed SEPARATELY, every build RUN (the lane's `.temp/probe/r7`, `r7b`, `r7c`,
// `r7d`): gcc 13.3.0 and clang 18.1.3 at `-std=c17 -pedantic-errors` and `-std=c2x`,
// mingw-w64 13.2.0 and MSVC 19.51 at `/std:c17` and `/std:clatest` REFUSE every
// shape refused below; the admitted controls build and run 42 on all four. Two
// compound shapes C refuses are BUILT by gcc and mingw at `-std=c2x` with an
// int-conversion warning (`x += p`; `p -= q` over compatible pointees) — clang and
// MSVC refuse them — so DSS admits both WITH S_IntegerPointerConversion, and computes
// gcc's values (examples/c/pointer_mixed_compound_assignment_values pins the values).
//
// DSS BEFORE (✔MEASURED at the P0 fold): `p + q`, `p + 1.5`, `1.5 + p`, `5 - p`,
// `p - 1.5`, `x -= p`, `p += q`, `p *= 2`, `p += 1.5` and both `p -= q` were accepted
// SILENTLY; `p * 2` and its siblings were refused only when an initializer's type
// check tripped on the result; `+p` by an internal HIR lowering message.
//
// ── RED-ON-DISABLE (the lane's transcript carries each build and its names) ──
//   * pass2Post's SE4e arm skipped → the refusal pins go silent;
//   * `subtreeType` handing a refused operation the fallback type again → the
//     one-diagnostic pin sees the context's second report;
//   * `compoundPointerOperandPairing` admitting every two-pointer `-=` → the
//     incompatible-pointee refusal goes silent.
// ===========================================================================

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/data_model.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include "semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
#include <string>

using namespace dss;
using namespace dss::sem_test;

namespace {

constexpr DiagnosticCode kRefused = DiagnosticCode::S_TypeMismatch;
constexpr DiagnosticCode kInt     = DiagnosticCode::S_IntegerPointerConversion;

[[nodiscard]] SemanticModel analyzeC(std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
}

constexpr char const* kPre =
    "static int a[4] = { 1, 2, 3, 42 };\n"
    "struct S { int *p; int x; };\n"
    "static int *getp(void) { return a; }\n"
    "enum E { E0, E3 = 3 };\n";

[[nodiscard]] std::string tu(char const* body) { return std::string{kPre} + body; }

struct Shape {
    char const* what;
    char const* body;
};

// Refused, exactly ONE diagnostic in the whole translation unit: the operator's.
void expectRefusedOnce(std::initializer_list<Shape> shapes) {
    for (Shape const& sh : shapes) {
        auto model = analyzeC(tu(sh.body));
        EXPECT_EQ(countCode(model.diagnostics(), kRefused), 1u) << sh.what << "\n" << sh.body;
        EXPECT_EQ(model.diagnostics().all().size(), 1u)
            << sh.what << " — one defect, one diagnostic\n" << sh.body;
    }
}

// Admitted: no Error, and exactly `ints` S_IntegerPointerConversion warnings.
void expectAdmitted(std::initializer_list<Shape> shapes, std::size_t ints) {
    for (Shape const& sh : shapes) {
        auto model = analyzeC(tu(sh.body));
        EXPECT_FALSE(model.hasErrors()) << sh.what << "\n" << sh.body;
        EXPECT_EQ(countCode(model.diagnostics(), kInt), ints) << sh.what << "\n" << sh.body;
    }
}

}  // namespace

// ── the binary operators: every pointer pairing C23 6.5.6-6.5.13 does not admit ─
TEST(PointerOperandConstraints, ABinaryOperatorRefusesEveryPointerPairingCDoesNotAdmit) {
    expectRefusedOnce({
        {"`p + q`", "int main(void) { int *p = a, *q = a; int *r = p + q; return r ? 42 : 1; }\n"},
        {"`p + 1.5`", "int main(void) { int *p = a; int *r = p + 1.5; return r ? 42 : 1; }\n"},
        {"`1.5 + p`", "int main(void) { int *p = a; int *r = 1.5 + p; return r ? 42 : 1; }\n"},
        {"`5 - p`", "int main(void) { int *p = a; long r = 5 - p; return r ? 42 : 1; }\n"},
        {"`p - 1.5`", "int main(void) { int *p = a; int *r = p - 1.5; return r ? 42 : 1; }\n"},
        {"`p * 2`", "int main(void) { int *p = a; long r = p * 2; return r ? 42 : 1; }\n"},
        {"`p / 2`", "int main(void) { int *p = a; long r = p / 2; return r ? 42 : 1; }\n"},
        {"`p % 2`", "int main(void) { int *p = a; long r = p % 2; return r ? 42 : 1; }\n"},
        {"`p << 1`", "int main(void) { int *p = a; long r = p << 1; return r ? 42 : 1; }\n"},
        {"`p & 1`", "int main(void) { int *p = a; long r = p & 1; return r ? 42 : 1; }\n"},
        {"`p | 1`", "int main(void) { int *p = a; long r = p | 1; return r ? 42 : 1; }\n"},
        {"`p ^ 1`", "int main(void) { int *p = a; long r = p ^ 1; return r ? 42 : 1; }\n"},
        // … through every operand shape the pre-filter walks.
        {"`s.p * 2` (a member)", "int main(void) { struct S s = { a, 0 }; long r = s.p * 2; return r ? 42 : 1; }\n"},
        {"`sp->p * 2` (an arrow)",
         "int main(void) { struct S s = { a, 0 }; struct S *sp = &s; long r = sp->p * 2; return r ? 42 : 1; }\n"},
        {"`getp() * 2` (a call result)", "int main(void) { long r = getp() * 2; return r ? 42 : 1; }\n"},
        {"`(p) * 2` (parentheses)", "int main(void) { int *p = a; long r = (p) * 2; return r ? 42 : 1; }\n"},
        {"`a * 2` (an array)", "int main(void) { long r = a * 2; return r ? 42 : 1; }\n"},
        {"`getp * 2` (a function designator)", "int main(void) { long r = getp * 2; return r ? 42 : 1; }\n"},
        {"`(int *)0 * 2` (a cast)", "int main(void) { long r = (int *)0 * 2; return r ? 42 : 1; }\n"},
        {"`&x * 2` (an address)", "int main(void) { int x = 0; long r = &x * 2; return r ? 42 : 1; }\n"},
        {"`s.p + s.p`", "int main(void) { struct S s = { a, 0 }; int *r = s.p + s.p; return r ? 42 : 1; }\n"},
        {"`5 - s.p`", "int main(void) { struct S s = { a, 0 }; long r = 5 - s.p; return r ? 42 : 1; }\n"},
        {"`sizeof(p * 2)`", "int main(void) { int *p = a; return (int)sizeof(p * 2); }\n"},
    });
}

// ── the unary arithmetic operators take no pointer ──────────────────────────
TEST(PointerOperandConstraints, AUnaryArithmeticOperatorRefusesAPointer) {
    expectRefusedOnce({
        {"`-p`", "int main(void) { int *p = a; long r = -p; return r ? 42 : 1; }\n"},
        {"`~p`", "int main(void) { int *p = a; long r = ~p; return r ? 42 : 1; }\n"},
        {"`+p`", "int main(void) { int *p = a; int *r = +p; return r ? 42 : 1; }\n"},
        {"`-s.p`", "int main(void) { struct S s = { a, 0 }; long r = -s.p; return r ? 42 : 1; }\n"},
    });
}

// ── a compound assignment takes its own constraint (C23 6.5.17.3p1) ────────
TEST(PointerOperandConstraints, ACompoundAssignmentTakesItsOwnConstraint) {
    expectRefusedOnce({
        {"`x -= p`", "int main(void) { long x = 0; int *p = a; x -= p; return 42; }\n"},
        {"`p += q`", "int main(void) { int *p = a, *q = a; p += q; return 42; }\n"},
        {"`p *= 2`", "int main(void) { int *p = a; p *= 2; return 42; }\n"},
        {"`p += 1.5`", "int main(void) { int *p = a; p += 1.5; return 42; }\n"},
        {"`p <<= 1`", "int main(void) { int *p = a; p <<= 1; return p ? 42 : 1; }\n"},
        {"`x *= p`", "int main(void) { long x = 1; int *p = a; x *= p; return x ? 42 : 1; }\n"},
        {"`x |= p`", "int main(void) { long x = 0; int *p = a; x |= p; return x ? 42 : 1; }\n"},
        // every reference refuses it: `p - q` itself has no meaning for gcc here.
        {"`p -= q` over incompatible pointees (int * and char *)",
         "int main(void) { int *p = a + 3; char *q = (char *)a; p -= q; return p ? 42 : 1; }\n"},
    });
    // gcc's `E1 = E1 op E2`, each with the integer/pointer conversion it draws.
    expectAdmitted({
        {"`x += p` (an integer plus a pointer is a pointer, converted to x)",
         "int main(void) { int x = 0; int *p = a; x += p; return 42; }\n"},
        {"`y += a` (an array right operand decays first)",
         "int main(void) { long long y = 2; y += a; return y ? 42 : 1; }\n"},
        {"`w = (z += p)` (value position)",
         "int main(void) { long long z = 1; int *p = a; long long w = (z += p); return w ? 42 : 1; }\n"},
        {"`p -= q` over one pointee (the element difference, converted to p)",
         "int main(void) { int *p = a + 3, *q = a; p -= q; return p ? 42 : 1; }\n"},
        {"`p -= q` with a const-qualified right pointee (still compatible)",
         "int main(void) { int *p = a + 2; const int *q = a; p -= q; return p ? 42 : 1; }\n"},
    }, 1u);
}

// ── the pairings C admits stay silent ───────────────────────────────────────
TEST(PointerOperandConstraints, ThePairingsCAdmitStaySilent) {
    expectAdmitted({
        {"`p + c` (char)", "int main(void) { int *p = a; char c = 3; return *(p + c); }\n"},
        {"`c + p`", "int main(void) { int *p = a; char c = 3; return *(c + p); }\n"},
        {"`p + b` (_Bool)", "int main(void) { int *p = a + 1; _Bool b = 1; p = p + b; return *(p + b); }\n"},
        {"`p + e` (an enumeration)", "int main(void) { int *p = a; enum E e = E3; return *(p + e); }\n"},
        {"`p - u` (unsigned)", "int main(void) { int *p = a + 4; unsigned u = 1u; return *(p - u); }\n"},
        {"`p - n` (long long)", "int main(void) { int *p = a + 4; long long n = 1; return *(p - n); }\n"},
        {"`'3' + p` (a character constant)", "int main(void) { int *p = a - 48; return *('3' + p); }\n"},
        {"member / arrow / call operands",
         "int main(void) { struct S s = { a, 0 }; struct S *sp = &s;\n"
         "  return *(s.p + 3) == *(3 + sp->p) ? *(getp() + 3) : 1; }\n"},
        {"`(a + 3) - a` (two pointers, one pointee)",
         "int main(void) { return (int)((a + 3) - a) == 3 ? a[3] : 1; }\n"},
        {"`p += 3; p -= 1; p++`",
         "int main(void) { int *p = a, *q = a + 3; p += 3; p -= 1; p++;\n"
         "  return (q - a == 3 && p == q) ? *p : 1; }\n"},
        {"`p -= c` / `p += c` (a compound pointer with a char)",
         "int main(void) { int *p = a + 4; char c = 1; p -= c; p += c; p -= c; return *p; }\n"},
        {"`!q && p && (q || p)` (the logical operators take any scalar)",
         "int main(void) { int *p = a, *q = 0; return (!q && p && (q || p)) ? 42 : 1; }\n"},
    }, 0u);
}

// ── a refused operation has no type for its context to judge again ─────────
// ✔MEASURED at the round's first SE4e build: `long r = p * 2;` drew the operator's
// S_TypeMismatch AND the initialization's pointer-to-integer warning, because the
// typer handed `p * 2` the pointer type its fallback picks.
TEST(PointerOperandConstraints, ARefusedOperationHasNoTypeForItsContextToJudgeAgain) {
    expectRefusedOnce({
        {"an initializer", "int main(void) { int *p = a; long r = p * 2; return r ? 42 : 1; }\n"},
        {"an argument",
         "static int g(long v) { return v ? 42 : 1; }\nint main(void) { int *p = a; return g(p | 1); }\n"},
        {"a return", "static long g(int *p) { return p & 1; }\nint main(void) { return g(a) ? 42 : 1; }\n"},
        {"an assignment", "int main(void) { int *p = a; long r; r = ~p; return r ? 42 : 1; }\n"},
        {"a unary operand in an initializer", "int main(void) { int *p = a; long r = -p; return r ? 42 : 1; }\n"},
    });
}
