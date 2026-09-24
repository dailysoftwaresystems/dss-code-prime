// ===========================================================================
// P68 round 9 (lane `cs`) — A WRITE THROUGH A DESIGNATION THE CONST WALK USED TO
// STOP SHORT OF.
//
// THE PROPERTY THIS FILE OWNS: the operand of an assignment, a compound
// assignment and `++` / `--` must be a MODIFIABLE lvalue (C 6.5.16p2, 6.5.2.4p1,
// 6.5.3.1p1), and the one check that asks (`reportWriteToConstLvalue` over the
// designation walk `constQualifiedLvalue`) must reach the object however the
// designation is spelled. Two gaps closed here, both in that one walk:
//
//   (1) `__func__`'s ELEMENTS. C 6.4.2.2p1 declares it
//       `static const char __func__[] = "…";`, but the synthetic symbol carried
//       only `isConst` (level 0, the array object), so `__func__[0] = 'x'`,
//       `*__func__ = 'x'` and `__func__[0]++` compiled into an image that FAULTS
//       on the write to read-only data. [[D-C-A-WRITE-TO-AN-ELEMENT-OF-FUNC-IS-ACCEPTED-INTO-READ-ONLY-DATA]].
//   (2) a pointer COMPUTED on the way to the object — `*(p + 1)`, `*(1 + p)`,
//       `*(p - 1)`, `(p + 1)[0]`, `*(c ? p : q)`, `*(z, p)`, `*(&a[0] + 1)` — which
//       the walk met as an operator it did not know, so it made no claim and a
//       write through a `const char *` compiled (and `*(a + 1)` on a
//       `static const char a[4]` faulted). Found in passing, same owner.
//
// ✔REFERENCE VOTES, 2026-09-23, each case one translation unit probed SEPARATELY
// (the lane's `.temp/probe/r2/`): gcc 13.3.0 and clang 18.1.3 (WSL) at
// `-std=c17 -pedantic-errors` and `-std=c2x`, mingw-w64 gcc 13.2.0 at both, MSVC
// 19.51.36260 at `/std:c17` and `/std:clatest`, every build RUN.
//   * every refusal below is refused by gcc, clang and mingw ("assignment of
//     read-only location"); MSVC refuses the pointer-arithmetic family (C2166)
//     and ACCEPTS the `__func__` element writes — and its program does not work:
//     it materializes `__func__` per mention, so the write does not read back
//     (exit 1 at /Od). No reference makes those writes WORK, so the union owes the
//     refusal (the row's decision, re-measured).
//   * every control below is accepted by all four and runs.
//
// ★ EVERY REFUSAL SITS BESIDE AN ACCEPTING TWIN: a check that refused every
// write would pass the refusals alone.
//
// ── RED-ON-DISABLE (the lane's transcript carries each build and its names) ──
//   * the `__func__` spine not minted → `AWriteToAnElementOfFuncIsRefused`, and
//     only that test;
//   * the binary arm (pointer ± integer, comma) removed → the arithmetic and
//     comma arms of `AWriteThroughAComputedPointerToConstIsRefused` and the
//     `*(__func__ + 1)` arm;
//   * the conditional arm removed → the `?:` arms, both orders;
//   * the `&` arm removed → the `*(&a[0] + 1)` arm.
// ===========================================================================

#include "core/types/parse_diagnostic.hpp"

#include "semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>

using namespace dss;
using namespace dss::sem_test;

namespace {

// One translation unit and whether S_ConstViolation must be reported for it.
struct Case {
    char const* what;
    char const* src;
    bool        refused;
};

void expectConstVerdicts(std::initializer_list<Case> cases) {
    for (Case const& c : cases) {
        auto model = analyzeShipped("c", {std::string{c.src}});
        EXPECT_EQ(hasCode(model.diagnostics(), DiagnosticCode::S_ConstViolation),
                  c.refused)
            << c.what << " — expected "
            << (c.refused ? "REFUSED S_ConstViolation" : "accepted") << "\n"
            << c.src;
        // A control must also be CLEAN: no other error may stand in for the one
        // under test, and none may hide a refusal behind a cascade.
        if (!c.refused) {
            for (auto const& d : model.diagnostics().all()) {
                EXPECT_NE(d.severity, DiagnosticSeverity::Error)
                    << c.what << " — control drew " << diagnosticCodeName(d.code)
                    << ": " << d.actual << "\n" << c.src;
            }
        }
    }
}

}  // namespace

// ── (1) `__func__` is `static const char[]`: its ELEMENTS are const ─────────
TEST(ConstDesignationWalk, AWriteToAnElementOfFuncIsRefused) {
    expectConstVerdicts({
        {"`__func__[0] = 'x'`",
         "int main(void) { __func__[0] = 'x'; return 0; }\n", true},
        {"`__func__[0]++`",
         "int main(void) { __func__[0]++; return 0; }\n", true},
        {"`*__func__ = 'x'`",
         "int main(void) { *__func__ = 'x'; return 0; }\n", true},
        {"`__func__[1] += 1` (compound)",
         "int main(void) { __func__[1] += 1; return 0; }\n", true},
        {"`--__func__[0]`",
         "int main(void) { --__func__[0]; return 0; }\n", true},
        {"`__FUNCTION__[0] = 'x'` (the config's GNU alias)",
         "int main(void) { __FUNCTION__[0] = 'x'; return 0; }\n", true},
        {"`*(__func__ + 1) = 'x'` (the decayed pointer, computed)",
         "int main(void) { *(__func__ + 1) = 'x'; return 0; }\n", true},
        // THE CONTROLS: reading, decaying to a pointer to const, taking the
        // address and measuring the object are all legal, and all four accept them.
        {"a read `__func__[0]`",
         "int main(void) { return __func__[0] == 'm' ? 0 : 1; }\n", false},
        {"decay to `const char *`",
         "int main(void) { const char *p = __func__; return p[3] == 'n' ? 0 : 1; }\n",
         false},
        {"`&__func__` into `const char (*)[5]`",
         "int main(void) { const char (*pa)[5] = &__func__; return (*pa)[1] == 'a' ? 0 : 1; }\n",
         false},
        {"`sizeof __func__`",
         "int main(void) { return sizeof __func__ == 5 ? 0 : 1; }\n", false},
        {"`__func__ == __func__` (C 6.4.2.2: ONE object)",
         "int main(void) { const char *a = __func__; const char *b = __func__; return a == b ? 0 : 1; }\n",
         false},
        {"a write to a LOCAL copy of the name",
         "int main(void) { char buf[5] = \"main\"; buf[0] = __func__[1]; return buf[0] == 'a' ? 0 : 1; }\n",
         false},
    });
}

// ── (2) a pointer COMPUTED on the way to a const object ─────────────────────
TEST(ConstDesignationWalk, AWriteThroughAComputedPointerToConstIsRefused) {
    expectConstVerdicts({
        {"`*(p + 1) = 'x'`",
         "int main(void) { char b[4] = \"abc\"; const char *p = b; *(p + 1) = 'x'; return 0; }\n",
         true},
        {"`*(1 + p) = 'x'` (the commuted sum)",
         "int main(void) { char b[4] = \"abc\"; const char *p = b; *(1 + p) = 'x'; return 0; }\n",
         true},
        {"`*(p - 1) = 'x'`",
         "int main(void) { char b[4] = \"abc\"; const char *p = b + 2; *(p - 1) = 'x'; return 0; }\n",
         true},
        {"`*(a + 1) = 'x'` on a const ARRAY",
         "int main(void) { static const char a[4] = \"abc\"; *(a + 1) = 'x'; return 0; }\n",
         true},
        {"`(p + 1)[0] = 'x'`",
         "int main(void) { char b[4] = \"abc\"; const char *p = b; (p + 1)[0] = 'x'; return 0; }\n",
         true},
        {"`(*(p + 1))++`",
         "int main(void) { char b[4] = \"abc\"; const char *p = b; (*(p + 1))++; return 0; }\n",
         true},
        {"`*(z, p) = 'x'` (the comma yields its right operand)",
         "int main(void) { char b[4] = \"abc\"; const char *p = b; int z = 0; *(z, p) = 'x'; return z; }\n",
         true},
        {"`*(&a[0] + 1) = 'x'` (`&` undoes one level)",
         "int main(void) { static const char a[4] = \"abc\"; *(&a[0] + 1) = 'x'; return 0; }\n",
         true},
        {"`*(c ? p : q) = 'x'`, both arms const",
         "int main(void) { char b[4] = \"abc\"; const char *p = b, *q = b; int c = 1; *(c ? p : q) = 'x'; return 0; }\n",
         true},
        {"`*(c ? m : p) = 'x'`, only the ELSE arm const (C 6.5.15p6: the union of "
         "both arms' qualifiers)",
         "int main(void) { char b[4] = \"abc\"; char *m = b; const char *p = b; int c = 1; *(c ? m : p) = 'x'; return 0; }\n",
         true},
        {"`*(c ? p : m) = 'x'`, only the THEN arm const",
         "int main(void) { char b[4] = \"abc\"; char *m = b; const char *p = b; int c = 1; *(c ? p : m) = 'x'; return 0; }\n",
         true},
        // THE CONTROLS: the same designations over MUTABLE pointees, and the
        // pointer-to-const itself moved (the pointer object is not const).
        {"`*(m + 1) = 'x'` through a mutable pointer",
         "int main(void) { char b[4] = \"abc\"; char *m = b; *(m + 1) = 'x'; return b[1] == 'x' ? 0 : 1; }\n",
         false},
        {"`*(c ? m : n) = 'x'`, both arms mutable",
         "int main(void) { char b[4] = \"abc\"; char *m = b, *n = b; int c = 1; *(c ? m : n) = 'x'; return 0; }\n",
         false},
        {"`*(z, m) = 'x'` through a mutable pointer",
         "int main(void) { char b[4] = \"abc\"; char *m = b; int z = 0; *(z, m) = 'x'; return z; }\n",
         false},
        {"`*(&b[0] + 1) = 'x'` on a mutable array",
         "int main(void) { char b[4] = \"abc\"; *(&b[0] + 1) = 'x'; return 0; }\n",
         false},
        {"`p = p + 1` moves a MUTABLE pointer to const",
         "int main(void) { char b[4] = \"abc\"; const char *p = b; p = p + 1; return *p == 'b' ? 0 : 1; }\n",
         false},
    });
}
