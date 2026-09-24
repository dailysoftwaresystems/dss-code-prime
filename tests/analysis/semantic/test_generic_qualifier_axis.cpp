// ===========================================================================
// P68 round 9 (lane `cs`) — `_Generic` ASSOCIATIONS DIFFERING ONLY IN A POINTEE
// QUALIFIER (the round's row of that name; its closing names this file).
//
// THE PROPERTY THIS FILE OWNS: a `_Generic` association matches when its type is
// COMPATIBLE with the controlling expression's type after lvalue conversion and
// array / function decay (C23 6.5.1.1p3), and two pointer types are compatible only
// when their pointed-to types are IDENTICALLY qualified (6.7.6.1p2) — `int *` and
// `const int *` are two association types, `int *volatile *` is not `int **`, a
// function pointer taking `const char *` is not one taking `char *`, and an
// association naming a top-level-qualified type (`const int`, `volatile int`,
// `int *volatile`) names a type no lvalue-converted controlling expression has.
// `const` and `restrict` are not interned, so the comparison asks the qualifier
// SPINE of the association's type name and of the controlling expression
// (`typeNameQualifierSpine`, `expressionQualifierSpine`) through the C23
// redeclaration oracle's own `spinesDiverge`.
//
// ✔REFERENCE VOTES, 2026-09-23, EVERY case below its own translation unit, each
// reference probed SEPARATELY, every build RUN (the lane's `.temp/probe/g`, `g2`,
// `g3`): gcc 13.3.0 and clang 18.1.3 at `-std=c17 -pedantic-errors` and
// `-std=c2x`, mingw-w64 13.2.0 at both, MSVC 19.51 at `/std:c17` and
// `/std:clatest` — all 60 exit 42 in every mode, i.e. every reference selects the
// association whose result is `42`. DSS BEFORE (✔MEASURED, the round's r3c build):
// 18 of the first 30 refused S_GenericSelectionAmbiguous and 6 selected the WRONG
// association in silence (g03, g04, g06, g13-g15; g18 and g25 fell to `default`
// because the controlling array was not decayed).
//
// Each pin asserts no Error and that the selection took the `42` arm — the DIRECT
// observation (`SemanticModel::selectedGenericExpr`), not a downstream type.
//
// ── RED-ON-DISABLE (the lane's transcript carries each build and its names) ──
//   * the spine comparison in `selectGenericAssociation` removed → the pairs that
//     differ only in a pointed-to `const` / `restrict` / parameter qualifier go
//     back to AMBIGUOUS;
//   * the association's top-level qualifier check removed → g13-g15, h19, h20
//     select the qualified association;
//   * the controlling decay removed → g18, g25, k01, k03 fall to `default`;
//   * the resolver's inner-layer volatile dropped again → g06;
//   * the expression spine answering "no claim" everywhere → every pair.
// ===========================================================================

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/data_model.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include "semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::sem_test;

namespace {

[[nodiscard]] SemanticModel analyzeC(std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
}

// The source text of every `_Generic`'s selected result expression.
[[nodiscard]] std::vector<std::string> selectedArms(SemanticModel const& m) {
    std::vector<std::string> out;
    for (auto const& tree : m.unit().trees()) {
        RuleId const gid = tree.schema().semantics().genericRule;
        if (!gid.valid()) continue;
        for (std::uint32_t i = 1; i < tree.nodeCount(); ++i) {
            NodeId const node{i};
            if (tree.kind(node) != NodeKind::Internal || tree.rule(node).v != gid.v) continue;
            NodeId const sel = m.selectedGenericExpr(node);
            std::string text = sel.valid() ? std::string{tree.text(sel)} : "<none>";
            while (!text.empty() && (text.back() == ' ' || text.back() == '\n')) text.pop_back();
            out.push_back(text);
        }
    }
    return out;
}

struct Case {
    char const* name;
    char const* src;
};

void expectSelects42(std::initializer_list<Case> cases) {
    for (Case const& c : cases) {
        auto model = analyzeC(c.src);
        EXPECT_FALSE(model.hasErrors()) << c.name << "\n" << c.src;
        EXPECT_FALSE(hasDiagnosedPointerConversion(model.diagnostics()))
            << "a compatible pointer pair must not be DIAGNOSED (the vacuity sweep)";
        EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_GenericSelectionAmbiguous), 0u)
            << c.name;
        bool took42 = false;
        for (auto const& arm : selectedArms(model)) {
            if (arm == "42") took42 = true;
            EXPECT_TRUE(arm == "42" || arm == "p") << c.name << ": selected `" << arm << "`";
        }
        EXPECT_TRUE(took42) << c.name << ": the `42` association was not selected\n" << c.src;
    }
}

}  // namespace

// ── the pointee qualifier splits two associations ─────────────────────────────
TEST(GenericQualifierAxis, APointeeQualifierSplitsTwoAssociations) {
    expectSelects42({
        {"g01 const int * controlling",
         "int main(void) { const int *p = 0; return _Generic(p, int *: 1, const int *: 42, default: 3); }\n"},
        {"g02 int * controlling",
         "int main(void) { int *p = 0; return _Generic(p, int *: 42, const int *: 1, default: 3); }\n"},
        {"g03 const int * beside only int *",
         "int main(void) { const int *p = 0; return _Generic(p, int *: 1, default: 42); }\n"},
        {"g04 int * beside only const int *",
         "int main(void) { int *p = 0; return _Generic(p, const int *: 1, default: 42); }\n"},
        {"g05 volatile pointee",
         "int main(void) { volatile int *p = 0; return _Generic(p, int *: 1, volatile int *: 42, default: 3); }\n"},
        {"g06 an inner volatile layer",
         "int main(void) { int *volatile *pp = 0;\n"
         "  return _Generic(pp, int **: 1, int *volatile *: 42, default: 3); }\n"},
        {"g07 an inner const layer",
         "int main(void) { int *const *pp = 0;\n"
         "  return _Generic(pp, int **: 1, int *const *: 42, default: 3); }\n"},
        {"g08 const int **",
         "int main(void) { const int **pp = 0;\n"
         "  return _Generic(pp, int **: 1, const int **: 42, default: 3); }\n"},
        {"g27 an inner restrict layer",
         "int main(void) { int *restrict *pp = 0;\n"
         "  return _Generic(pp, int **: 1, int *restrict *: 42, default: 3); }\n"},
        {"g30 const void *",
         "int main(void) { const void *v = 0; return _Generic(v, void *: 1, const void *: 42, default: 3); }\n"},
    });
}

// ── the controlling type is lvalue-converted: its OWN top level is gone ───────
TEST(GenericQualifierAxis, TheControllingTypeLosesItsTopLevelQualifiers) {
    expectSelects42({
        {"g09 int *const",
         "int main(void) { int x = 0; int *const p = &x; return _Generic(p, int *: 42, default: 1); }\n"},
        {"g10 const int",
         "int main(void) { const int x = 0; return _Generic(x, int: 42, default: 1); }\n"},
        {"g11 int *restrict",
         "int main(void) { int x = 0; int *restrict p = &x; return _Generic(p, int *: 42, default: 1); }\n"},
    });
}

// ── an association naming a top-level-qualified type never matches ───────────
TEST(GenericQualifierAxis, ATopLevelQualifiedAssociationNeverMatches) {
    expectSelects42({
        {"g12 const int beside int",
         "int main(void) { int x = 0; return _Generic(x, const int: 1, int: 42, default: 3); }\n"},
        {"g13 const int alone",
         "int main(void) { int x = 0; return _Generic(x, const int: 1, default: 42); }\n"},
        {"g14 volatile int alone",
         "int main(void) { int x = 0; return _Generic(x, volatile int: 1, default: 42); }\n"},
        {"g15 int *volatile alone",
         "int main(void) { int *p = 0; return _Generic(p, int *volatile: 1, default: 42); }\n"},
        {"h19 const int beside int, no default",
         "int main(void) { int x = 0; return _Generic(x, const int: 1, int: 42); }\n"},
        {"h20 int *const beside int *",
         "int main(void) { int *const p = 0;\n"
         "  return _Generic(p, int *const: 1, int *: 42, default: 3); }\n"},
    });
}

// ── every controlling-expression shape carries its qualifiers ─────────────────
TEST(GenericQualifierAxis, EveryControllingShapeCarriesItsQualifiers) {
    expectSelects42({
        {"g16 &x of a const int",
         "int main(void) { const int x = 0; return _Generic(&x, int *: 1, const int *: 42, default: 3); }\n"},
        {"g17 &x of an int",
         "int main(void) { int x = 0; return _Generic(&x, int *: 42, const int *: 1, default: 3); }\n"},
        {"g18 a const array decays",
         "int main(void) { static const int a[2] = { 1, 2 };\n"
         "  return _Generic(a, int *: 1, const int *: 42, default: 3); }\n"},
        {"g19 a cast",
         "int main(void) { return _Generic((const int *)0, int *: 1, const int *: 42, default: 3); }\n"},
        {"g20 a member",
         "struct S { const int *p; };\n"
         "int main(void) { struct S s = { 0 }; return _Generic(s.p, int *: 1, const int *: 42, default: 3); }\n"},
        {"g21 a call result",
         "static const int *f(void) { return 0; }\n"
         "int main(void) { return _Generic(f(), int *: 1, const int *: 42, default: 3); }\n"},
        {"g22 *pp",
         "int main(void) { const int *q = 0; const int **pp = &q;\n"
         "  return _Generic(*pp, int *: 1, const int *: 42, default: 3); }\n"},
        {"g23 a typedef'd object",
         "typedef const int CI;\n"
         "int main(void) { CI *p = 0; return _Generic(p, int *: 1, const int *: 42, default: 3); }\n"},
        {"g24 a conditional (both arms' pointee qualifiers)",
         "int main(void) { int x = 0; const int *c = &x; int *p = &x; int k = 1;\n"
         "  return _Generic(k ? p : c, int *: 1, const int *: 42, default: 3); }\n"},
        {"g25 a string literal decays to char *",
         "int main(void) { return _Generic(\"ab\", char *: 42, const char *: 1, default: 3); }\n"},
        {"g26 pointer arithmetic",
         "int main(void) { static const int a[2] = { 1, 2 }; const int *p = a;\n"
         "  return _Generic(p + 1, int *: 1, const int *: 42, default: 3); }\n"},
        {"g28 a parameter",
         "static int f(const char *s) { return _Generic(s, char *: 1, const char *: 42, default: 3); }\n"
         "int main(void) { return f(0); }\n"},
        {"g29 a typedef'd association",
         "typedef const char *CS;\n"
         "int main(void) { const char *s = 0; return _Generic(s, char *: 1, CS: 42, default: 3); }\n"},
        {"h01 &member of a const struct",
         "struct S { int x; };\n"
         "int main(void) { const struct S s = { 1 };\n"
         "  return _Generic(&s.x, int *: 1, const int *: 42, default: 3); }\n"},
        {"h02 &member through a pointer to const",
         "struct S { int x; };\n"
         "int main(void) { struct S v = { 1 }; const struct S *sp = &v;\n"
         "  return _Generic(&sp->x, int *: 1, const int *: 42, default: 3); }\n"},
        {"h03 &member of a plain struct",
         "struct S { int x; };\n"
         "int main(void) { struct S s = { 1 };\n"
         "  return _Generic(&s.x, int *: 42, const int *: 1, default: 3); }\n"},
        {"h04 &a[0] of an array of const char *",
         "int main(void) { const char *a[2] = { 0, 0 };\n"
         "  return _Generic(&a[0], char **: 1, const char **: 42, default: 3); }\n"},
        {"h05 a nested _Generic",
         "int main(void) { const int *p = 0;\n"
         "  return _Generic(_Generic(p, default: p), int *: 1, const int *: 42, default: 3); }\n"},
        {"h08 a conditional with a null pointer constant arm",
         "int main(void) { int x = 0; const int *c = &x; int k = 1;\n"
         "  return _Generic(k ? c : 0, int *: 1, const int *: 42, default: 3); }\n"},
        {"h09 a typedef'd pointer object",
         "typedef const char *CS;\n"
         "int main(void) { CS s = 0; return _Generic(s, char *: 1, const char *: 42, default: 3); }\n"},
        {"h10 *&p",
         "int main(void) { const int *p = 0;\n"
         "  return _Generic(*&p, int *: 1, const int *: 42, default: 3); }\n"},
        {"h11 &*pp keeps an inner restrict",
         "int main(void) { int *restrict *pp = 0;\n"
         "  return _Generic(&*pp, int **: 1, int *restrict *: 42, default: 3); }\n"},
        {"h12 &s of a struct",
         "struct S { int x; };\n"
         "int main(void) { struct S s = { 1 };\n"
         "  return _Generic(&s, const struct S *: 1, struct S *: 42, default: 3); }\n"},
        {"h13 &s of a const struct",
         "struct S { int x; };\n"
         "int main(void) { const struct S s = { 1 };\n"
         "  return _Generic(&s, const struct S *: 42, struct S *: 1, default: 3); }\n"},
        {"h14 an assignment's value",
         "int main(void) { const int *p = 0, *q = 0;\n"
         "  return _Generic(p = q, int *: 1, const int *: 42, default: 3); }\n"},
        {"h15 p++",
         "int main(void) { static const int a[2] = { 1, 2 }; const int *p = a;\n"
         "  return _Generic(p++, int *: 1, const int *: 42, default: 3); }\n"},
        {"h16 a comma",
         "int main(void) { const int *p = 0; int x = 0;\n"
         "  return _Generic((x, p), int *: 1, const int *: 42, default: 3); }\n"},
        {"h17 a call through a function pointer",
         "static const int *g(void) { return 0; }\n"
         "int main(void) { const int *(*fp)(void) = g;\n"
         "  return _Generic(fp(), int *: 1, const int *: 42, default: 3); }\n"},
        {"h18 a compound literal",
         "int main(void) { return _Generic((const int *){ 0 }, int *: 1, const int *: 42, default: 3); }\n"},
        {"k01 an array member of a const object decays to const",
         "struct S { int arr[2]; };\n"
         "int main(void) { const struct S s = { { 1, 2 } };\n"
         "  return _Generic(s.arr, int *: 1, const int *: 42, default: 3); }\n"},
        {"k03 a const array member",
         "struct S { const int arr[2]; };\n"
         "int main(void) { struct S s = { { 1, 2 } };\n"
         "  return _Generic(s.arr, int *: 1, const int *: 42, default: 3); }\n"},
        {"k04 &a[0] of a const array",
         "int main(void) { static const int a[2] = { 1, 2 };\n"
         "  return _Generic(&a[0], int *: 1, const int *: 42, default: 3); }\n"},
    });
}

// ── abstract declarators: pointers to arrays, pointers to functions ───────────
TEST(GenericQualifierAxis, AnAbstractDeclaratorCarriesItsQualifiersAndItsParameters) {
    expectSelects42({
        {"h06 a pointer to an array of const int",
         "int main(void) { static const int a[2] = { 1, 2 };\n"
         "  return _Generic(&a, int (*)[2]: 1, const int (*)[2]: 42, default: 3); }\n"},
        {"k02 &array member of a const object",
         "struct S { int arr[2]; };\n"
         "int main(void) { const struct S s = { { 1, 2 } };\n"
         "  return _Generic(&s.arr, int (*)[2]: 1, const int (*)[2]: 42, default: 3); }\n"},
        {"k05 &array of int",
         "int main(void) { static int a[2] = { 1, 2 };\n"
         "  return _Generic(&a, int (*)[2]: 42, const int (*)[2]: 1, default: 3); }\n"},
        {"h07 &f of a function taking const char *",
         "static void f(const char *s) { (void)s; }\n"
         "int main(void) { return _Generic(&f, void (*)(char *): 1,\n"
         "  void (*)(const char *): 42, default: 3); }\n"},
        {"k06 a function-pointer object",
         "int main(void) { void (*fp)(const char *) = 0;\n"
         "  return _Generic(fp, void (*)(char *): 1, void (*)(const char *): 42, default: 3); }\n"},
        {"k07 &f beside only the char * association",
         "static void f(const char *s) { (void)s; }\n"
         "int main(void) { return _Generic(&f, void (*)(char *): 1, default: 42); }\n"},
        {"k08 a bare function designator",
         "static void f(const char *s) { (void)s; }\n"
         "int main(void) { return _Generic(f, void (*)(const char *): 42, void (*)(char *): 1, default: 3); }\n"},
        {"k09 a const-qualified result",
         "static const char *g(void) { return 0; }\n"
         "int main(void) { return _Generic(&g, char *(*)(void): 1, const char *(*)(void): 42, default: 3); }\n"},
        {"k10 a parameter's own top-level const is not part of the function type",
         "static void h(const int x) { (void)x; }\n"
         "int main(void) { return _Generic(&h, void (*)(int): 42, default: 1); }\n"},
    });
}
