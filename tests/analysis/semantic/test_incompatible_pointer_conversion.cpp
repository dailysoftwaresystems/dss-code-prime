// ===========================================================================
// P68 round 9 (lane `cs`) — D-C-INCOMPATIBLE-POINTER-CONVERSION-REFUSED-WHERE-EVERY-REFERENCE-WARNS
// and the rows that share its rule (D-CSUBSET-INCOMPATIBLE-ELEMENT-ARRAY-TERNARY).
//
// THE PROPERTY THIS FILE OWNS: a pointer mixed with a pointer to an INCOMPATIBLE
// type, or with an integer that is not a null pointer constant, is a C
// CONSTRAINT VIOLATION at every site of the pointer-compatibility constraint —
// initialization, assignment, argument, return (C23 6.7.11p12 / 6.5.17.2p1 /
// 6.5.3.3p2 / 6.8.7.5p3), `==` / `!=` (6.5.10p2), the relational operators
// (6.5.9p2) and the arms of `?:` (6.5.16p3) — so it needs a DIAGNOSTIC, and the
// pinned references give a warning and BUILD the program.
//
// ✔REFERENCE VOTES, 2026-09-23, each case its own translation unit, each
// reference probed SEPARATELY, every build RUN (the lane's `.temp/probe/r1*`, `r3`,
// `r5`): gcc 13.3.0 and clang 18.1.3 at `-std=c2x`, mingw-w64 13.2.0 at `-std=c2x`
// and MSVC 19.51.36260 at `/std:c17` and `/std:clatest` build every object-pointer
// mix with a warning (MSVC silent on some) and run it to the right value; clang
// refuses a function-pointer SIGNATURE mismatch and an integer/pointer mix by
// default, which gcc 13 and MSVC build and run. At `-std=c17 -pedantic-errors` gcc
// and clang refuse them all — the required diagnostic made an error — so a WARNING
// here plus `--warnings-as-errors` covers both postures (GCC 14 made the default an
// error; the pinned gcc is 13.3.0).
//
// THE MEANING: the pointer's value is kept (C 6.3.2.3p7/p8 — only the static type
// changes); an integer converts as the explicit cast would (6.3.2.3p5/p6). And a
// `?:` whose arms are pointers to incompatible types has type `void *` — gcc's and
// clang's answer (MSVC takes the SECOND arm's type); no vendor documents the case,
// and `void *` is the one heterogeneous-pointer rule all three document (object
// pointer beside `void *`). The row carries the decision.
//
// ── RED-ON-DISABLE (the lane's transcript carries each build and its names) ──
//   * the classifier answering None for every pair → every warning arm here turns
//     back into S_TypeMismatch / S_ReturnTypeMismatch (or silence at the
//     comparison / conditional sites); the controls stay green;
//   * either config key set false (`TheConfigKeysAreTheSwitch` perturbs it) → that
//     class is refused again;
//   * the conditional's `void *` arm removed → the conditional pins;
//   * the pre-filter walking through a unary operator again → the `&a == &b`,
//     `c ? &a : &b` and `&c - &i` pins.
// ===========================================================================

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/data_model.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "repo_root.hpp"

#include "semantic_test_fixture.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace dss;
using namespace dss::sem_test;

namespace {

constexpr DiagnosticCode kPtr     = DiagnosticCode::S_IncompatiblePointerConversion;
constexpr DiagnosticCode kSameRep = DiagnosticCode::S_IncompatiblePointerIntegerPointee;
constexpr DiagnosticCode kInt     = DiagnosticCode::S_IntegerPointerConversion;
constexpr DiagnosticCode kCond    = DiagnosticCode::S_ConditionalOperandTypeMismatch;

// `Natural` + 16 is what the shipped x86_64 / arm64 targets declare; without an
// aggregate layout a `sizeof` in a constant expression declines to fold.
[[nodiscard]] SemanticModel analyzeC(std::string const& src,
                                     DataModel dm = DataModel::Lp64) {
    auto cu = buildShippedUnit("c", {src});
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), dm,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
}

// One translation unit: the diagnostic it must draw exactly `count` times, and no
// Error at all.
struct Case {
    char const*    what;
    char const*    src;
    DiagnosticCode code;
    std::size_t    count;
};

void expectAdmittedWith(std::initializer_list<Case> cases,
                        DataModel dm = DataModel::Lp64) {
    for (Case const& c : cases) {
        auto model = analyzeC(c.src, dm);
        EXPECT_EQ(countCode(model.diagnostics(), c.code), c.count)
            << c.what << " — " << diagnosticCodeName(c.code) << "\n" << c.src;
        for (auto const& d : model.diagnostics().all()) {
            EXPECT_NE(d.severity, DiagnosticSeverity::Error)
                << c.what << " — refused " << diagnosticCodeName(d.code) << ": "
                << d.actual << "\n" << c.src;
        }
        // The other classes must stay silent: one conversion, one report.
        for (DiagnosticCode other : {kPtr, kSameRep, kInt}) {
            if (other == c.code) continue;
            EXPECT_EQ(countCode(model.diagnostics(), other), 0u)
                << c.what << " — an unexpected " << diagnosticCodeName(other)
                << "\n" << c.src;
        }
    }
}

constexpr char const* kAB = "struct A { int x; };\nstruct B { int x; };\n";

[[nodiscard]] std::string withAB(char const* body) { return std::string{kAB} + body; }

}  // namespace

// ── the row's five shapes, and every site the assignment rule governs ───────
TEST(IncompatiblePointerConversion, AnIncompatibleObjectPointerIsAdmittedWithAWarningAtEverySite) {
    auto const init = withAB("int main(void) { struct B b = { 42 }; struct A *pa = &b; return pa->x; }\n");
    auto const assign = withAB("int main(void) { struct B b = { 42 }; struct A *pa; pa = &b; return pa->x; }\n");
    auto const arg = withAB("static int take(struct A *pa) { return pa->x; }\n"
                            "int main(void) { struct B b = { 42 }; return take(&b); }\n");
    auto const ret = withAB("static struct A *conv(struct B *pb) { return pb; }\n"
                            "int main(void) { struct B b = { 42 }; return conv(&b)->x; }\n");
    auto const fileScope = withAB("static struct B gb = { 42 };\nstruct A *gpa = &gb;\n"
                                  "int main(void) { return gpa->x; }\n");
    auto const staticLocal = withAB("static struct B gb = { 42 };\n"
                                    "int main(void) { static struct A *spa = &gb; return spa->x; }\n");
    auto const viaFnPtr = withAB("static int take(struct A *pa) { return pa->x; }\n"
                                 "int main(void) { int (*fp)(struct A *) = take; struct B b = { 42 }; return fp(&b); }\n");
    auto const member = withAB("struct H { struct A *p; };\n"
                               "int main(void) { struct B b = { 42 }; struct H h; h.p = &b; return h.p->x; }\n");
    expectAdmittedWith({
        {"initialization", init.c_str(), kPtr, 1},
        {"assignment", assign.c_str(), kPtr, 1},
        {"a call argument", arg.c_str(), kPtr, 1},
        {"a return", ret.c_str(), kPtr, 1},
        {"a file-scope initializer", fileScope.c_str(), kPtr, 1},
        {"a static local's initializer", staticLocal.c_str(), kPtr, 1},
        {"an argument through a function pointer", viaFnPtr.c_str(), kPtr, 1},
        {"a member assignment", member.c_str(), kPtr, 1},
        {"`int *p = &l;` from a `long` (LP64: another width)",
         "int main(void) { long l = 42; int *p = &l; return *p; }\n", kPtr, 1},
    });
}

// ── the other pointer pairs C calls incompatible ────────────────────────────
TEST(IncompatiblePointerConversion, EveryIncompatiblePointerPairIsAdmittedWithAWarning) {
    expectAdmittedWith({
        {"`int *` from `unsigned *` (signedness)",
         "int main(void) { unsigned u = 42; int *p = &u; return *p; }\n", kPtr, 1},
        {"`char *` from `unsigned char[2]` (the array decays first)",
         "int main(void) { unsigned char b[2] = { 42, 0 }; char *p = b; return *p; }\n", kPtr, 1},
        {"`int (*)[2]` from `int (*)[3]` (another bound)",
         "int main(void) { int a[3] = { 42, 1, 2 }; int (*p)[2] = &a; return (*p)[0]; }\n", kPtr, 1},
        {"`int *` from `int **` (levels of indirection)",
         "int main(void) { int x = 42; int *px = &x; int *q = &px; return **(int **)q; }\n", kPtr, 1},
        {"`void **` from `int **`",
         "int main(void) { int x = 42; int *px = &x; void **vp = &px; return *(int *)*vp; }\n", kPtr, 1},
        {"two incomplete tags",
         "struct P;\nstruct Q;\nint main(void) { struct P *p = 0; struct Q *q = p; return q ? 0 : 42; }\n",
         kPtr, 1},
        {"`int *` from `enum E *`",
         "enum E { EA, EB };\nint main(void) { enum E e = EB; int *p = &e; return *p + 41; }\n", kPtr, 1},
        {"`int *` from `struct S *` (the first-member idiom)",
         "struct S { int x; };\nint main(void) { struct S s = { 42 }; int *p = &s; return *p; }\n", kPtr, 1},
        {"`int *` from a string literal",
         "int main(void) { int *p = \"abc\"; return p ? 42 : 0; }\n", kPtr, 1},
        {"a function pointer from an object pointer",
         "int main(void) { int x = 42; int (*fp)(void) = &x; return fp ? 42 : 0; }\n", kPtr, 1},
        {"an object pointer from a function designator",
         "static int f(void) { return 42; }\nint main(void) { char *p = f; return ((int (*)(void))p)(); }\n",
         kPtr, 1},
        {"a function pointer from another signature (clang refuses, gcc 13 and MSVC build)",
         "static int f(int a) { return a; }\nint main(void) { int (*fp)(void) = f; return fp ? 42 : 0; }\n",
         kPtr, 1},
    });
}

// ── a same-representation integer pointee keeps its own, narrower code ──────
TEST(IncompatiblePointerConversion, ASameRepresentationIntegerPointeeKeepsItsOwnCodeAtEverySite) {
    // LP64: `long` and `long long` are one representation, two types.
    expectAdmittedWith({
        {"init `long *` from `long long *`",
         "int main(void) { long long v = 42; long *p = &v; return (int)*p; }\n", kSameRep, 1},
        {"assignment", "int main(void) { long long v = 42; long *p; p = &v; return (int)*p; }\n",
         kSameRep, 1},
        {"return", "static long *g(long long *q) { return q; }\n"
                   "int main(void) { long long v = 42; return (int)*g(&v); }\n", kSameRep, 1},
        {"an argument through a function POINTER (no longer a direct call only)",
         "static int take(long *p) { return (int)*p; }\n"
         "int main(void) { int (*fp)(long *) = take; long long v = 42; return fp(&v); }\n",
         kSameRep, 1},
    }, DataModel::Lp64);
    // LLP64: `int` and `long` are one representation, two types; `long long` is another.
    expectAdmittedWith({
        {"LLP64 `long *` from `int *`",
         "int main(void) { int v = 42; long *p = &v; return (int)*p; }\n", kSameRep, 1},
        {"LLP64 `long *` from `long long *` (another width: the general code)",
         "int main(void) { long long v = 42; long *p = &v; return (int)*p; }\n", kPtr, 1},
    }, DataModel::Llp64);
}

// ── an integer and a pointer, either direction, not a null pointer constant ─
TEST(IncompatiblePointerConversion, AnIntegerAndAPointerConvertWithAWarning) {
    expectAdmittedWith({
        {"`int *` from an integer object",
         "int main(void) { int x = 42; long v = (long)&x; int *p = v; return *p; }\n", kInt, 1},
        {"an integer object from `int *`",
         "int main(void) { int x = 42; long v = &x; return v ? 42 : 0; }\n", kInt, 1},
        {"`int *` from a NON-ZERO literal",
         "int main(void) { int *p = 42; return p ? 42 : 0; }\n", kInt, 1},
        {"an argument: `int g(int *)` given an integer",
         "static int g(int *p) { return p ? 42 : 0; }\nint main(void) { long v = 7; return g(v); }\n",
         kInt, 1},
        {"a return: an integer from a pointer",
         "static long g(int *p) { return p; }\nint main(void) { int x; return g(&x) ? 42 : 0; }\n",
         kInt, 1},
        {"an enum object from a pointer",
         "enum E { EA };\nint main(void) { int x; enum E e = &x; return e ? 42 : 0; }\n", kInt, 1},
        // THE CONTROLS: a null pointer constant, a `_Bool` target and an explicit
        // cast are C's own conversions and draw nothing.
        {"`int *p = 0;`", "int main(void) { int *p = 0; return p ? 0 : 42; }\n", kInt, 0},
        {"`int *p = 1 - 1;` (a folded null pointer constant)",
         "int main(void) { int *p = 1 - 1; return p ? 0 : 42; }\n", kInt, 0},
        {"`_Bool b = p;`", "int main(void) { int x; int *p = &x; _Bool b = p; return b ? 42 : 0; }\n",
         kInt, 0},
        {"`int *p = (int *)v;`",
         "int main(void) { int x = 42; long v = (long)&x; int *p = (int *)v; return *p; }\n", kInt, 0},
    });
}

// ── an integer constant expression with value 0 of ANY integer type ─────────
// C 6.3.2.3p3 says "an integer constant expression with the value 0" and C
// 6.2.5p17 makes `char`, `bool` and an enumeration integer types. ✔MEASURED
// 2026-09-23 (`.temp/probe/r5`, every program RUN): gcc 13.3.0, mingw-w64 13.2.0
// and MSVC 19.51 accept every one below SILENTLY and run 42 (clang 18.1.3 adds a
// style note; gcc 13 at `-std=c2x` alone refuses C23 `false`); DSS REFUSED
// `Z`, `(char)0`, `false` and `return Z;` — its folded path named the int RANKS only.
TEST(IncompatiblePointerConversion, AnyIntegerConstantExpressionWithValueZeroIsANullPointerConstant) {
    expectAdmittedWith({
        {"an enumeration constant", "enum { Z };\nint main(void) { int *p = Z; return p == 0 ? 42 : 1; }\n",
         kInt, 0},
        {"`(char)0`", "int main(void) { int *p = (char)0; return p == 0 ? 42 : 1; }\n", kInt, 0},
        {"C23 `false`", "int main(void) { int *p = false; return p == 0 ? 42 : 1; }\n", kInt, 0},
        {"`'\\0'`", "int main(void) { int *p = '\\0'; return p == 0 ? 42 : 1; }\n", kInt, 0},
        {"`(int)0.0`", "int main(void) { int *p = (int)0.0; return p == 0 ? 42 : 1; }\n", kInt, 0},
        {"a return", "enum { Z };\nstatic int *g(void) { return Z; }\n"
                     "int main(void) { return g() == 0 ? 42 : 1; }\n", kInt, 0},
        {"an argument", "enum { Z };\nstatic int g(int *p) { return p == 0 ? 42 : 1; }\n"
                        "int main(void) { return g(Z); }\n", kInt, 0},
        {"an assignment", "enum { Z };\nint main(void) { int x; int *p = &x; p = Z; return p == 0 ? 42 : 1; }\n",
         kInt, 0},
        // … and a NON-zero constant of the same kinds is still the diagnosed class.
        {"a non-zero enumeration constant", "enum { Z, O };\nint main(void) { int *p = O; return p ? 42 : 1; }\n",
         kInt, 1},
    });
}

// ── what stays a refusal: no floating or aggregate value converts ───────────
TEST(IncompatiblePointerConversion, AFloatingOrAggregateValueIsStillRefused) {
    for (char const* src : {
             "int main(void) { double d = 1.0; int *p = d; return p ? 1 : 0; }\n",
             "int main(void) { int x; double d = &x; return d > 0; }\n",
             "struct S { int x; };\nint main(void) { struct S s = { 1 }; int *p = s; return p ? 1 : 0; }\n",
             "struct S { int x; };\nint main(void) { int x = 0; int *p = &x; struct S s = p; return s.x; }\n",
         }) {
        auto model = analyzeC(src);
        EXPECT_TRUE(hasCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch)) << src;
        EXPECT_EQ(countCode(model.diagnostics(), kPtr) + countCode(model.diagnostics(), kInt), 0u)
            << src;
    }
}

// ── the controls: compatible conversions draw nothing ───────────────────────
TEST(IncompatiblePointerConversion, CompatiblePointerConversionsStaySilent) {
    for (char const* src : {
             "int main(void) { int x = 42; int *p = &x; const int *c = p; return *c; }\n",
             "int main(void) { int x = 42; void *v = &x; int *p = v; return *p; }\n",
             "int main(void) { int x = 42; int *p = &x; void *v = p; return *(int *)v; }\n",
             "typedef int I;\nint main(void) { int x = 42; I *p = &x; return *p; }\n",
             "int main(void) { char buf[4] = \"abc\"; char *p = buf; return p[0] == 'a' ? 42 : 0; }\n",
             "static int f(void) { return 42; }\nint main(void) { int (*fp)(void) = f; return fp(); }\n",
             "static int f(void) { return 42; }\nint main(void) { void *v = f; return v ? 42 : 0; }\n",
             "int main(void) { int x = 42; volatile int *v = &x; return *v; }\n",
             // P68 round 10 (lane `cs`): an ARRAY decaying into a pointer to a more-
             // qualified element adds the qualifier as a pointer does (C 6.5.16.1p1) —
             // at an initialization, an assignment and an argument. Each used to draw
             // S_IncompatiblePointerIntegerPointee (the array arm compared the element
             // by identity); all four references are silent (`.temp/probe/bv` bv24).
             "int main(void) { int a[2] = { 40, 2 }; volatile int *v = a; return v[0] + v[1]; }\n",
             "int main(void) { int a[2] = { 40, 2 }; volatile int *v; v = a; return v[0] + v[1]; }\n",
             "static int f(volatile int *p) { return p[0] + p[1]; }\n"
             "int main(void) { int a[2] = { 40, 2 }; return f(a); }\n",
         }) {
        auto model = analyzeC(src);
        EXPECT_FALSE(model.hasErrors()) << src;
        for (DiagnosticCode c : {kPtr, kSameRep, kInt}) {
            EXPECT_EQ(countCode(model.diagnostics(), c), 0u)
                << diagnosticCodeName(c) << "\n" << src;
        }
    }
}

// ── `==`, `!=` and the relational operators ─────────────────────────────────
TEST(IncompatiblePointerConversion, AComparisonOfDistinctPointersOrOfAPointerAndAnIntegerWarns) {
    auto const eqAB = withAB("int main(void) { struct A a = { 1 }; struct B b = { 2 }; return (&a == &b) ? 0 : 42; }\n");
    expectAdmittedWith({
        {"`&a == &b` (struct A * vs struct B *; both operands are `&` expressions)", eqAB.c_str(), kPtr, 1},
        {"`&x <= lp` (long long * vs long *, one representation on LP64)",
         "int main(void) { long long x = 0; long *lp = (long *)&x; return (&x <= lp) ? 42 : 0; }\n",
         kSameRep, 1},
        {"`&x <= lp` (int * vs long *: two widths on LP64, the general code)",
         "int main(void) { int x = 0; long *lp = (long *)&x; return (&x <= lp) ? 42 : 0; }\n", kPtr, 1},
        {"`v < p` (void * vs int *: 6.5.9p2 wants compatible OBJECT types)",
         "int main(void) { int x[2] = { 0 }; void *v = &x[0]; int *p = &x[1]; return (v < p) ? 42 : 1; }\n",
         kPtr, 1},
        {"`b != p` (char[2] decays to char *, vs int *)",
         "int main(void) { char b[2] = { 0 }; int x = 0; int *p = &x; return (b != p) ? 42 : 1; }\n",
         kPtr, 1},
        {"`p == f` (an object pointer vs a function)",
         "static int f(void) { return 0; }\nint main(void) { int x = 1; int *p = &x; return (p == f) ? 0 : 42; }\n",
         kPtr, 1},
        {"`p == 5`",
         "int main(void) { int x = 1; int *p = &x; return (p == 5) ? 0 : 42; }\n", kInt, 1},
        {"`p > 5`",
         "int main(void) { int x = 0; int *p = &x; return (p > 5) ? 42 : 1; }\n", kInt, 1},
        {"`p != E1` (a non-zero enumeration constant)",
         "enum { E1 = 1 };\nint main(void) { int x = 0; int *p = &x; return (p != E1) ? 42 : 1; }\n",
         kInt, 1},
        // THE CONTROLS — every one silent on all four references too.
        {"`v == p` (void * beside an object pointer, 6.5.10p2)",
         "int main(void) { int x = 0; void *v = &x; int *p = &x; return (v == p) ? 42 : 1; }\n", kPtr, 0},
        {"`p != 0`", "int main(void) { int x = 0; int *p = &x; return (p != 0) ? 42 : 1; }\n", kInt, 0},
        {"`p != 1 - 1` (a folded null pointer constant)",
         "int main(void) { int x = 0; int *p = &x; return (p != 1 - 1) ? 42 : 1; }\n", kInt, 0},
        {"`p > 0` (every reference is silent by default)",
         "int main(void) { int x = 0; int *p = &x; return (p > 0) ? 42 : 1; }\n", kInt, 0},
        {"`c == p` (a qualifier apart)",
         "int main(void) { int x = 0; const int *c = &x; int *p = &x; return (c == p) ? 42 : 1; }\n",
         kPtr, 0},
        {"`a < b` for two same-signature function pointers",
         "static int f(void) { return 0; }\nstatic int g(void) { return 1; }\n"
         "int main(void) { int (*a)(void) = f; int (*b)(void) = g; return (a < b || a > b) ? 42 : 42; }\n",
         kPtr, 0},
        {"`&x == &y` over two ints (the pre-filter's arithmetic stamps say nothing about `&`)",
         "int main(void) { int x = 0, y = 0; return (&x == &y) ? 1 : 42; }\n", kPtr, 0},
    });
}

// ── `?:` with pointer arms to incompatible types: a warning, and `void *` ───
TEST(IncompatiblePointerConversion, AConditionalOfIncompatiblePointersWarnsAndIsAVoidPointer) {
    auto const structs = withAB(
        "int main(void) { struct A a = { 42 }; struct B b = { 7 }; int c = 1;\n"
        "  struct A *p = c ? &a : &b; return p->x; }\n");
    expectAdmittedWith({
        {"`c ? &a : &b` into a `struct A *`", structs.c_str(), kPtr, 1},
        {"`c ? \"ab\" : arr` (row 3: char[3] vs int[2], both decay)",
         "int main(void) { int arr[2] = { 7, 8 }; int c = 1; const char *s = c ? \"ab\" : arr;\n"
         "  return s[0] == 'a' ? 42 : 1; }\n", kPtr, 1},
        {"row 3's else arm into a `const int *` (void * converts to it)",
         "int main(void) { int arr[2] = { 42, 8 }; int c = 0; const int *p = c ? \"ab\" : arr; return p[0]; }\n",
         kPtr, 1},
        {"two arrays of different elements",
         "int main(void) { short a[2] = { 42, 1 }; long b[2] = { 7, 8 }; int c = 1;\n"
         "  const short *p = c ? a : b; return p[0]; }\n", kPtr, 1},
        {"an object pointer beside a function designator",
         "static int f(void) { return 0; }\n"
         "int main(void) { int x = 42; int c = 1; void *v = c ? &x : f; return *(int *)v; }\n", kPtr, 1},
    });
    // THE TYPE is `void *`: a `_Generic` selects the `void *` association, and
    // `sizeof` measures a pointer.
    auto generic = analyzeC(
        "int main(void) { int i = 1; float f = 2.0f; int c = 1;\n"
        "  return _Generic((c ? &i : &f), void *: 42, int *: 2, float *: 3, default: 4); }\n");
    EXPECT_FALSE(generic.hasErrors());
    EXPECT_EQ(countCode(generic.diagnostics(), kPtr), 1u);
    auto sized = analyzeC(
        "int arr[2];\nenum { Z = sizeof(1 ? \"ab\" : arr) };\n"
        "_Static_assert(Z == sizeof(void *), \"a pointer, not an array\");\n"
        "int main(void) { return Z; }\n");
    EXPECT_FALSE(sized.hasErrors());
    // A member access through the `void *` conditional is refused, as gcc and
    // clang refuse it (MSVC's second-arm meaning would accept it).
    auto member = analyzeC(withAB(
        "int main(void) { struct A a = { 42 }; struct B b = { 7 }; int c = 1;\n"
        "  return (c ? &a : &b)->x; }\n"));
    EXPECT_TRUE(member.hasErrors())
        << "a member of a `void *` conditional does not exist";
    // THE CONTROLS: compatible arms keep their pointer type and draw nothing, and
    // a pointer beside an integer keeps the conditional's own code — including when
    // the pointer arm is an `&` expression the stamp pre-filter used to misread.
    auto same = analyzeC(
        "int main(void) { int x = 42, y = 7; int c = 1; int *p = c ? &x : &y; return *p; }\n");
    EXPECT_FALSE(same.hasErrors());
    EXPECT_EQ(countCode(same.diagnostics(), kPtr), 0u);
    auto withInt = analyzeC(
        "int main(void) { int x = 42; long n = 5; int c = 1; long v = (long)(c ? &x : n); return v ? 42 : 0; }\n");
    EXPECT_EQ(countCode(withInt.diagnostics(), kCond), 1u);
    EXPECT_EQ(countCode(withInt.diagnostics(), kPtr), 0u);
}

// ── the stamp pre-filter no longer reads THROUGH a unary operator ───────────
// `stampedOperandMayBePointer` followed "the sole non-token child" and so walked
// from `&c` into `c`, answering a definitive NO for a pointer. SE4c's
// pointer-difference warning — an existing check — went silent for `&`
// operands the same way.
TEST(IncompatiblePointerConversion, ThePointerPreFilterDoesNotReadThroughAnOperator) {
    auto diff = analyzeC(
        "int main(void) { char c = 0; int i = 0; long d = &c - (char *)&i; return d ? 42 : 42; }\n");
    EXPECT_FALSE(diff.hasErrors());
    auto mixed = analyzeC(
        "int main(void) { char c[4] = { 0 }; int i[4] = { 0 }; long d = &c[2] - &i[1]; return d ? 42 : 42; }\n");
    EXPECT_EQ(countCode(mixed.diagnostics(),
                        DiagnosticCode::S_PointerDifferenceIncompatiblePointee), 1u)
        << "`&c[2] - &i[1]`: char * minus int * — the existing SE4c warning";
}

// ── a TYPE NAME naming a never-declared struct / union tag declares it ──────
// Found while moving a bare parameter-list tag into the body's scope (the row's
// closing): `(struct Z *)&x` with no declaration of `Z` anywhere was refused
// S_UndeclaredIdentifier — Pass 2's reference arm reported the tag's miss before
// the type resolver (which forward-mints an undeclared struct or union tag, C
// 6.7.3.4) could run. ✔MEASURED 2026-09-23 (`.temp/probe/r6`, every program RUN):
// gcc 13.3.0, clang 18.1.3, mingw-w64 13.2.0 and MSVC 19.51 build every one below
// and run 42. (An undeclared ENUM tag is refused S_UnknownType by the resolver,
// which gcc, clang and mingw refuse too; MSVC's acceptance of `sizeof(enum E)` is
// its own row.)
TEST(IncompatiblePointerConversion, ATypeNameNamingAnUndeclaredTagDeclaresIt) {
    for (char const* src : {
             "int main(void) { int x = 0; return (struct Z *)&x ? 42 : 0; }\n",
             "int main(void) { return sizeof(struct Z *) == sizeof(void *) ? 42 : 0; }\n",
             "int main(void) { int x = 0; void *v = (struct Z *)&x; struct Z *p = v;\n"
             "  return p == v ? 42 : 0; }\n",
             "int main(void) { int x = 0; return (union U *)&x ? 42 : 0; }\n",
             "int main(void) { int x = 0; int *p = &x;\n"
             "  return _Generic(p, struct Z *: 1, int *: 42, default: 3); }\n",
             "enum { N = sizeof(struct Z *) };\n"
             "int main(void) { return N == sizeof(void *) ? 42 : 0; }\n",
         }) {
        auto model = analyzeC(src);
        EXPECT_FALSE(model.hasErrors()) << src;
        EXPECT_FALSE(hasDiagnosedPointerConversion(model.diagnostics()))
            << "a compatible pointer pair must not be DIAGNOSED (the vacuity sweep)";
        EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u) << src;
    }
}

// ── the config keys are the switch ──────────────────────────────────────────
TEST(IncompatiblePointerConversion, TheConfigKeysAreTheSwitch) {
    std::ifstream in{dss::test::configRoot() / "sources" / "c.lang.json", std::ios::binary};
    ASSERT_TRUE(in.good());
    nlohmann::json const shipped = nlohmann::json::parse(in);
    auto analyzeWith = [](nlohmann::json const& doc, std::string const& src) {
        auto schema = GrammarSchema::loadFromText(doc.dump(), "<pointer-conversion-keys>");
        if (!schema.has_value())
            throw std::runtime_error("the perturbed c schema failed to load");
        UnitBuilder builder{*schema, DiagnosticBudget::libraryDefault()};
        builder.addInMemory(src, "main.c");
        auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
        return analyze(cu, DiagnosticBudget::libraryDefault());
    };
    std::string const ptrSrc =
        std::string{kAB} + "int main(void) { struct B b = { 42 }; struct A *pa = &b; return pa->x; }\n";
    std::string const intSrc =
        "int main(void) { int x = 42; long v = (long)&x; int *p = v; return *p; }\n";
    // Both keys ON (the shipped config): admitted with a warning.
    EXPECT_FALSE(analyzeWith(shipped, ptrSrc).hasErrors());
    EXPECT_FALSE(analyzeWith(shipped, intSrc).hasErrors());
    // The pointer key OFF: the pointer mix is refused again, the integer mix is not.
    nlohmann::json noPtr = shipped;
    noPtr["semantics"]["pointerConversions"]["incompatiblePointerConvertsDiagnosed"] = false;
    auto const refusedPtr = analyzeWith(noPtr, ptrSrc);
    EXPECT_TRUE(hasCode(refusedPtr.diagnostics(), DiagnosticCode::S_TypeMismatch));
    EXPECT_EQ(countCode(refusedPtr.diagnostics(), kPtr), 0u);
    EXPECT_FALSE(analyzeWith(noPtr, intSrc).hasErrors());
    // The integer key OFF: the integer mix is refused again, the pointer mix is not.
    nlohmann::json noInt = shipped;
    noInt["semantics"]["pointerConversions"]["integerPointerConvertsDiagnosed"] = false;
    auto const refusedInt = analyzeWith(noInt, intSrc);
    EXPECT_TRUE(hasCode(refusedInt.diagnostics(), DiagnosticCode::S_TypeMismatch));
    EXPECT_EQ(countCode(refusedInt.diagnostics(), kInt), 0u);
    EXPECT_FALSE(analyzeWith(noInt, ptrSrc).hasErrors());
    // The RETIRED key is refused by the loader, not silently read as nothing.
    nlohmann::json stale = shipped;
    stale["semantics"]["pointerConversions"]["directCallIntPointeeCompat"] = true;
    EXPECT_FALSE(GrammarSchema::loadFromText(stale.dump(), "<stale-key>").has_value())
        << "`directCallIntPointeeCompat` was retired; a config still naming it must fail loud";
}
