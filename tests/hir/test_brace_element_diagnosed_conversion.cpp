// ===========================================================================
// P68 round 9 (lane `cs`) — A BRACE-INIT ELEMENT'S DIAGNOSED CONVERSION
// (D-C-INCOMPATIBLE-POINTER-CONVERSION-REFUSED-WHERE-EVERY-REFERENCE-WARNS).
//
// A brace-init element's conversion is realized by the HIR lowering, in the slot the
// brace-level walk places it in, so the conversions the language admits WITH A
// DIAGNOSTIC are reported by the lowering for it (`lowerBraceElementValue`), in the
// SAME code and sentence the semantic tier reports at every other initialization, and
// `coerce` realizes them. (A pair NO rule admits is the semantic tier's since P69 —
// the second half of this file.)
//
// ✔MEASURED 2026-09-23, each reference separately and every program RUN (the lane's
// `.temp/probe/r1`, `r1b`, `r5`): `struct A *arr[1] = { &b };`, `struct H h = { &b };`,
// `struct H h = { .p = &b };`, a file-scope `struct A *gtab[1] = { &gb };`, `{ v }` for
// an integer `v` and `int *a[1] = { 5 };` are built and run by gcc 13.3.0, mingw-w64
// 13.2.0 and MSVC 19.51 with a warning (clang 18.1.3 too for the pointer pairs); a
// `'\0'` or an enumeration constant 0 element is a null pointer constant, silent on
// gcc, mingw and MSVC. BEFORE: DSS stopped the pointer pairs with an INTERNAL verifier
// message (H_VerifierFailure — `coerce` passed the element through with its own type),
// accepted `{ 5 }` into a pointer SILENTLY, and refused the `'\0'` / enumerator zeros
// with H_VerifierFailure.
//
// RED-ON-DISABLE: `lowerBraceElementValue` not reporting → the warning counts below
// drop to 0 (the lowering still succeeds); the `coerce` realize arm removed → the
// pointer-pair cases fail the lowering with H_VerifierFailure again.
// ===========================================================================

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "shipped_schema_or_throw.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace dss;

namespace {

constexpr DiagnosticCode kPtr = DiagnosticCode::S_IncompatiblePointerConversion;
constexpr DiagnosticCode kInt = DiagnosticCode::S_IntegerPointerConversion;

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& r, DiagnosticCode c) {
    std::size_t n = 0;
    for (auto const& d : r.all()) if (d.code == c) ++n;
    return n;
}

[[nodiscard]] std::size_t countErrors(DiagnosticReporter const& r) {
    std::size_t n = 0;
    for (auto const& d : r.all()) if (d.severity == DiagnosticSeverity::Error) ++n;
    return n;
}

struct Lowered {
    std::size_t semanticErrors = 0;
    std::size_t semanticPtr = 0;
    std::size_t semanticInt = 0;
    bool        ok = false;
    std::size_t hirErrors = 0;
    std::size_t hirPtr = 0;
    std::size_t hirInt = 0;
};

// c source → semantic model → HIR, counting the two classes in each tier separately:
// a brace element must be reported by the LOWERING and by nothing else.
[[nodiscard]] Lowered lowerC(std::string src) {
    auto const loaded = dss::test_support::shippedSchemaOrThrow("c");
    UnitBuilder builder{loaded, DiagnosticBudget::libraryDefault()};
    builder.addInMemory(std::move(src), "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    SemanticModel model = analyze(cu, DiagnosticBudget::libraryDefault());
    Lowered out;
    out.semanticErrors = countErrors(model.diagnostics());
    out.semanticPtr = countCode(model.diagnostics(), kPtr);
    out.semanticInt = countCode(model.diagnostics(), kInt);
    if (out.semanticErrors != 0) return out;
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    out.ok = res->ok;
    out.hirErrors = countErrors(r);
    out.hirPtr = countCode(r, kPtr);
    out.hirInt = countCode(r, kInt);
    return out;
}

struct Case {
    char const*    what;
    char const*    src;
    DiagnosticCode code;
    std::size_t    count;
};

void expectLoweredWith(std::initializer_list<Case> cases) {
    for (Case const& c : cases) {
        Lowered const l = lowerC(c.src);
        ASSERT_EQ(l.semanticErrors, 0u) << c.what << "\n" << c.src;
        EXPECT_EQ(l.semanticPtr + l.semanticInt, 0u)
            << c.what << " — a brace element is judged by the lowering, not twice\n" << c.src;
        EXPECT_TRUE(l.ok) << c.what << "\n" << c.src;
        EXPECT_EQ(l.hirErrors, 0u) << c.what << "\n" << c.src;
        EXPECT_EQ(c.code == kPtr ? l.hirPtr : l.hirInt, c.count) << c.what << "\n" << c.src;
        EXPECT_EQ(c.code == kPtr ? l.hirInt : l.hirPtr, 0u) << c.what << "\n" << c.src;
    }
}

constexpr char const* kAB = "struct A { int x; };\nstruct B { int x; };\n";

}  // namespace

TEST(BraceElementDiagnosedConversion, AnIncompatiblePointerElementIsReportedAndRealized) {
    std::string const arrayElem = std::string{kAB} +
        "int main(void) { struct B b = { 42 }; struct A *arr[1] = { &b }; return arr[0]->x; }\n";
    std::string const member = std::string{kAB} + "struct H { struct A *p; int *q; };\n"
        "int main(void) { struct B b = { 42 }; struct H h = { &b, 0 }; return h.p->x; }\n";
    std::string const designated = std::string{kAB} + "struct H { struct A *p; };\n"
        "int main(void) { struct B b = { 42 }; struct H h = { .p = &b }; return h.p->x; }\n";
    std::string const fileScope = std::string{kAB} +
        "static struct B gb = { 42 };\nstruct A *gtab[1] = { &gb };\n"
        "int main(void) { return gtab[0]->x; }\n";
    std::string const scalarBrace = std::string{kAB} +
        "int main(void) { struct B b = { 42 }; struct A *p = { &b }; return p->x; }\n";
    std::string const compound = std::string{kAB} + "struct H { struct A *p; };\n"
        "int main(void) { struct B b = { 42 }; return (struct H){ &b }.p->x; }\n";
    expectLoweredWith({
        {"an array element", arrayElem.c_str(), kPtr, 1},
        {"a positional member", member.c_str(), kPtr, 1},
        {"a designated member", designated.c_str(), kPtr, 1},
        {"a file-scope array element", fileScope.c_str(), kPtr, 1},
        {"a scalar's braced initializer", scalarBrace.c_str(), kPtr, 1},
        {"a compound literal's member", compound.c_str(), kPtr, 1},
    });
}

TEST(BraceElementDiagnosedConversion, AnIntegerPointerElementIsReportedUnlessItIsANullPointerConstant) {
    expectLoweredWith({
        {"`{ 5 }` into a pointer element",
         "int main(void) { int *a[1] = { 5 }; return a[0] ? 42 : 0; }\n", kInt, 1},
        {"an integer object into a pointer member",
         "struct H { int *p; };\nint main(void) { long v = 0; struct H h = { v }; return h.p ? 0 : 42; }\n",
         kInt, 1},
        {"a pointer into an integer member",
         "struct H { long v; };\nint main(void) { int x; struct H h = { &x }; return h.v ? 42 : 0; }\n",
         kInt, 1},
        // THE CONTROLS: null pointer constants — `0`, `'\0'`, an enumerator 0, `1 - 1`.
        {"`{ 0 }`", "int main(void) { int *a[1] = { 0 }; return a[0] ? 0 : 42; }\n", kInt, 0},
        {"`{ '\\0', Z, 1 - 1 }`",
         "enum { Z };\nint main(void) { int *a[3] = { '\\0', Z, 1 - 1 };\n"
         "  return (a[0] == 0 && a[1] == 0 && a[2] == 0) ? 42 : 1; }\n", kInt, 0},
    });
}

TEST(BraceElementDiagnosedConversion, ACompatibleElementDrawsNothing) {
    expectLoweredWith({
        {"a matching pointer element",
         "int main(void) { int x = 42; int *a[1] = { &x }; return *a[0]; }\n", kPtr, 0},
        {"`void *` from an object pointer",
         "int main(void) { int x = 42; void *a[1] = { &x }; return *(int *)a[0]; }\n", kPtr, 0},
        {"a string literal into a `char *` member",
         "struct H { const char *s; };\nint main(void) { struct H h = { \"ab\" }; return h.s[0] == 'a' ? 42 : 0; }\n",
         kPtr, 0},
        {"a string literal into a `char` array member",
         "struct H { char s[4]; };\nint main(void) { struct H h = { \"ab\" }; return h.s[0] == 'a' ? 42 : 0; }\n",
         kPtr, 0},
    });
}

// ===========================================================================
// P69 (lane `cs`) — AN ELEMENT NO RULE ADMITS IS REFUSED BY THE SEMANTIC TIER, AT THE
// ELEMENT (D-C-A-INCOMPATIBLE-BRACE-ELEMENT-IS-REFUSED-BY-AN-INTERNAL-VERIFIER-FAILURE).
//
// C 6.7.9p11: an element that initializes a scalar takes the type constraints of simple
// assignment, as `T x = e;` does. The semantic tier places every element with the
// lowering's own cursor (`checkBraceInitializerElements`) and refuses, with S_TypeMismatch
// at the element, a pair the ordinary rules, a null pointer constant and the diagnosed
// conversions all decline — so the lowering never sees it. ✔MEASURED before the check
// (lane `cs`'s probes br1-br4, the pe64 build): every refused shape below reached the HIR
// verifier (H_VerifierFailure, "ConstructAggregate … child type doesn't match") and `int
// x = { s };` the MIR tier's I_StoreValueTypeMismatch; gcc 13.3.0, clang 18.1.3 and
// mingw-w64 13.2.0 refuse every one at the element, and MSVC 19.51 every one but the
// `_Bool` element (below) — the static one with C2099, its non-constant initializer.
//
// ★ THE ONE SHAPE A REFERENCE BUILDS, AND WHY DSS STILL REFUSES IT. MSVC 19.51 converts a
// structure to `_Bool` — as this brace element, as a plain initializer and in an
// assignment — silently even at /W4, through the structure's FIRST member (`{ 0, 5 }` is
// false and `{ 0.5 }` true: probe br4). C has no conversion from a structure to any
// scalar type, not even by a cast (C 6.5.4p2); the pairing is a constraint violation a
// conforming implementation must diagnose (C 5.1.1.3); the value MSVC gives it is
// documented nowhere; gcc, clang and mingw refuse it. That is a reference ACCEPTING an
// invalid program with a meaning of its own, not a working program the disjunction makes
// owed, so DSS refuses it — at the plain initializer as before, and at the element now.
//
// RED-ON-DISABLE: the element judge never refusing → each refused case below reaches the
// lowering, which fails it (H_VerifierFailure), and no S_TypeMismatch is drawn; the
// compound-literal or the declaration call site removed → that site's cases do.
// ===========================================================================

namespace {

constexpr DiagnosticCode kMismatch = DiagnosticCode::S_TypeMismatch;
constexpr DiagnosticCode kVerifier = DiagnosticCode::H_VerifierFailure;

struct Judged {
    std::size_t              semanticErrors = 0;
    std::vector<std::string> mismatches;   // each S_TypeMismatch's sentence
    bool                     lowered = false;
    bool                     ok = false;
    std::size_t              hirErrors = 0;
    std::size_t              verifierFailures = 0;
};

// c source → semantic model → (only when the semantic tier admitted it) HIR.
[[nodiscard]] Judged judgeC(std::string src) {
    auto const loaded = dss::test_support::shippedSchemaOrThrow("c");
    UnitBuilder builder{loaded, DiagnosticBudget::libraryDefault()};
    builder.addInMemory(std::move(src), "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    SemanticModel model = analyze(cu, DiagnosticBudget::libraryDefault());
    Judged out;
    out.semanticErrors = countErrors(model.diagnostics());
    for (auto const& d : model.diagnostics().all())
        if (d.code == kMismatch) out.mismatches.push_back(d.actual);
    if (out.semanticErrors != 0) return out;
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    out.lowered = true;
    out.ok = res->ok;
    out.hirErrors = countErrors(r);
    out.verifierFailures = countCode(r, kVerifier);
    return out;
}

struct Shape {
    std::string what;
    std::string src;
    std::string sentence;      // a refusal's leading words (empty for an admitted shape)
    std::size_t errors = 1;    // every error a refused unit draws (a static one draws two)
};

std::string const kS = "struct S { int v; };\n";
std::string const kSR = "struct S { int v; };\nstruct R { int v; };\n";

}  // namespace

TEST(BraceElementAssignmentConstraint, AnElementNoRuleAdmitsIsRefusedAtTheElementBeforeHir) {
    std::string const intSlot = "`s` is a structure, and what this brace element initializes is an integer";
    std::vector<Shape> const refused = {
        {"a compound literal's first member (d02)",
         kS + "int main(void) { struct S s = { 1 }; struct S *p = &(struct S){ s }; return p->v; }\n",
         intSlot},
        {"an array element",
         kS + "int main(void) { struct S s = { 1 }; int a[1] = { s }; return a[0]; }\n", intSlot},
        {"a pointer member",
         kS + "struct Q { int *p; };\nint main(void) { struct S s = { 1 }; struct Q q = { s }; return q.p != 0; }\n",
         "`s` is a structure, and what this brace element initializes is a pointer"},
        {"a double member",
         kS + "struct D { double d; };\nint main(void) { struct S s = { 1 }; struct D x = { s }; return (int)x.d; }\n",
         "`s` is a structure, and what this brace element initializes is an arithmetic value"},
        {"a union's first member",
         kS + "union U { int i; float f; };\nint main(void) { struct S s = { 1 }; union U u = { s }; return u.i; }\n",
         intSlot},
        {"a designated member",
         kS + "struct O { int a; int b; };\nint main(void) { struct S s = { 1 }; struct O o = { .b = s }; return o.b; }\n",
         intSlot},
        {"a deep designated member",
         kS + "struct O { struct S in; };\nint main(void) { struct S s = { 1 }; struct O o = { .in.v = s }; return o.in.v; }\n",
         intSlot},
        {"an array compound literal's element",
         kS + "int main(void) { struct S s = { 1 }; int *p = (int[]){ s }; return *p; }\n", intSlot},
        {"a double into a pointer element",
         "int main(void) { double d = 1.0; int *a[1] = { d }; return a[0] != 0; }\n",
         "`d` is an arithmetic value, and what this brace element initializes is a pointer"},
        {"another structure elided into a member",
         kSR + "struct O { struct S in; };\nint main(void) { struct R r = { 1 }; struct O o = { r }; return o.in.v; }\n",
         "`r` is a structure, and what this brace element initializes is an integer"},
        {"another structure elided into an array element's member",
         kSR + "int main(void) { struct R t = { 1 }; struct S a[2] = { 1, t }; return a[1].v; }\n",
         "`t` is a structure, and what this brace element initializes is an integer"},
        {"a scalar's braced initializer",
         kS + "int main(void) { struct S s = { 1 }; int x = { s }; return x; }\n", intSlot},
        {"a nested list's element",
         kS + "struct O { int a[2]; };\nint main(void) { struct S s = { 1 }; struct O o = { { 1, s } }; return o.a[0]; }\n",
         intSlot},
        {"a `_Bool` element (the shape MSVC builds)",
         kS + "int main(void) { struct S s = { 1 }; _Bool b[1] = { s }; return b[0]; }\n", intSlot},
        {"a `_Complex` element",
         kS + "int main(void) { struct S s = { 1 }; double _Complex a[1] = { s }; return 0; }\n",
         "`s` is a structure, and what this brace element initializes is an arithmetic value"},
        {"a static array's element (its non-constant initializer is refused too)",
         kS + "static struct S s = { 1 };\nstatic int a[1] = { s };\nint main(void) { return a[0]; }\n",
         intSlot, 2},
    };
    for (Shape const& c : refused) {
        Judged const j = judgeC(c.src);
        EXPECT_FALSE(j.lowered) << c.what << " — the semantic tier must refuse it\n" << c.src;
        EXPECT_EQ(j.semanticErrors, c.errors) << c.what << "\n" << c.src;
        ASSERT_EQ(j.mismatches.size(), 1u) << c.what << "\n" << c.src;
        EXPECT_EQ(j.mismatches[0].rfind(c.sentence, 0), 0u)
            << c.what << ": the refusal names the element and its subobject\n  got: "
            << j.mismatches[0] << "\n" << c.src;
    }
}

TEST(BraceElementAssignmentConstraint, NoAdmittedBraceElementReachesTheVerifier) {
    std::vector<Shape> const admitted = {
        {"a structure member initialized whole",
         kS + "struct O { int a; struct S in; };\nint main(void) { struct S s = { 1 }; struct O o = { 1, s }; return o.in.v; }\n",
         ""},
        {"a structure leading an elided list",
         kS + "struct O { struct S in; int w; };\nint main(void) { struct S s = { 1 }; struct O o = { s, 2 }; return o.w; }\n",
         ""},
        {"a structure's own braces",
         kS + "struct O { struct S in; };\nint main(void) { int i = 1; struct O o = { { i } }; return o.in.v; }\n", ""},
        {"an array of whole structures",
         kS + "int main(void) { struct S s = { 21 }; struct S a[2] = { s, s }; return a[0].v + a[1].v; }\n", ""},
        {"a union's structure member whole",
         kS + "union U { struct S s; int i; };\nint main(void) { struct S s = { 42 }; union U u = { s }; return u.s.v; }\n",
         ""},
        {"a compound literal initializing a member whole",
         kS + "struct O { struct S in; };\nint main(void) { return (struct O){ (struct S){ 42 } }.in.v; }\n", ""},
        {"a braced string", "int main(void) { char s[4] = { \"*b\" }; return s[0]; }\n", ""},
        {"a nested braced string",
         "struct T { char s[4]; int n; };\nint main(void) { struct T t = { { \"*b\" }, 5 }; return t.n; }\n", ""},
        {"a null pointer constant", "int main(void) { int *a[1] = { 0 }; return a[0] == 0; }\n", ""},
        {"`void *` into an object pointer",
         "int main(void) { int x = 42; void *v = &x; int *a[1] = { v }; return *a[0]; }\n", ""},
        {"a string into `char *`", "int main(void) { char *a[1] = { \"*\" }; return a[0][0]; }\n", ""},
        {"a double into an `int`", "int main(void) { double d = 42.9; int a[1] = { d }; return a[0]; }\n", ""},
        {"an enumerator zero into a pointer",
         "enum E { Z };\nint main(void) { int *a[1] = { Z }; return a[0] == 0; }\n", ""},
        {"a scalar's braced initializer", "int main(void) { int x = { 42 }; return x; }\n", ""},
        {"`_Bool` from a pointer", "int main(void) { int x; _Bool a[1] = { &x }; return a[0]; }\n", ""},
        // The conversions admitted WITH a diagnostic: the lowering reports and realizes them.
        {"an incompatible pointer element",
         "int main(void) { float f = 1; int *a[1] = { &f }; return a[0] != 0; }\n", ""},
        {"an integer into a pointer element", "int main(void) { int *a[1] = { 5 }; return a[0] != 0; }\n", ""},
        {"a pointer into an integer element",
         "int main(void) { int x = 1; long a[1] = { &x }; return a[0] != 0; }\n", ""},
        {"a string into an `int` element (MSVC builds it)",
         "int main(void) { int a[1] = { \"*\" }; return a[0] != 0; }\n", ""},
        {"a function into an object pointer element",
         "int f(void) { return 42; }\nint main(void) { int *a[1] = { f }; return a[0] != 0; }\n", ""},
        {"an incompatible pointer in a compound literal",
         "int main(void) { float f = 1; int **p = (int *[]){ &f }; return p[0] != 0; }\n", ""},
        {"an incompatible pointer at file scope",
         "float f = 1;\nint *a[1] = { &f };\nint main(void) { return a[0] != 0; }\n", ""},
    };
    for (Shape const& c : admitted) {
        Judged const j = judgeC(c.src);
        ASSERT_EQ(j.semanticErrors, 0u) << c.what << "\n" << c.src;
        EXPECT_TRUE(j.mismatches.empty()) << c.what << "\n" << c.src;
        EXPECT_TRUE(j.ok) << c.what << "\n" << c.src;
        EXPECT_EQ(j.hirErrors, 0u) << c.what << "\n" << c.src;
        EXPECT_EQ(j.verifierFailures, 0u)
            << c.what << " — an admitted element reached the verifier\n" << c.src;
    }
}
