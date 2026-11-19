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

// ── P68 round 10 (lane `cs`): a qualifier INSIDE an abstract declarator's group is its
//    layer's, never the base's ────────────────────────────────────────────────────
// With no star at the type name's own level, the base-qualifier scan walked into the
// abstract declarator, so `int (*volatile *)[2]` named an array of `volatile int` — the
// association missed the very object DECLARED with it, and a cast through it built a
// pointer the declared one converted from with S_IncompatiblePointerConversion. The
// same held for `_Atomic` and for a function pointer's group (the base there is the
// RESULT type). `const` rides the spine and was right. ✔MEASURED 2026-09-24 (lane
// `cs`'s `.temp/probe/hxv`, every program RUN): gcc 13.3.0, clang 18.1.3 (both modes),
// mingw-w64 13.2.0 and MSVC 19.51 (both modes; no `_Atomic`: abstains on m04) select
// every `42` below; DSS fell to `default` on m01-m04.
TEST(GenericQualifierAxis, AQualifierInsideAnAbstractGroupStaysInItsLayer) {
    expectSelects42({
        {"m01 &p of `int (*volatile p)[2]`",
         "int main(void) { int a[2] = { 1, 2 }; int (*volatile p)[2] = &a;\n"
         "  return _Generic(&p, int (**)[2]: 1, int (*volatile *)[2]: 42, default: 3); }\n"},
        {"m02 an object declared with the association's own type",
         "int main(void) { int (*volatile *q)[2] = 0;\n"
         "  return _Generic(q, int (**)[2]: 1, int (*volatile *)[2]: 42, default: 3); }\n"},
        {"m03 a volatile function pointer's address — the base is the result type",
         "static int g(void) { return 1; }\n"
         "int main(void) { int (*volatile fp)(void) = g;\n"
         "  return _Generic(&fp, int (**)(void): 1, int (*volatile *)(void): 42, default: 3); }\n"},
        {"m04 an `_Atomic` pointer to an array",
         "int main(void) { static int a[2] = { 1, 2 }; int (*_Atomic p)[2] = &a;\n"
         "  return _Generic(&p, int (**)[2]: 1, int (*_Atomic *)[2]: 42, default: 3); }\n"},
        {"m05 the control: `const` inside the group",
         "int main(void) { int a[2] = { 1, 2 }; int (*const p)[2] = &a;\n"
         "  return _Generic(&p, int (**)[2]: 1, int (*const *)[2]: 42, default: 3); }\n"},
    });
    // The cast through the group converts to the declared object with no diagnostic.
    auto m = analyzeC("int main(void) { int a[2] = { 1, 2 }; int (*p)[2] = &a;\n"
                      "  int (*volatile *pp)[2] = (int (*volatile *)[2])&p; return (**pp)[0]; }\n");
    EXPECT_FALSE(m.hasErrors());
    EXPECT_FALSE(hasDiagnosedPointerConversion(m.diagnostics()))
        << "the cast's type IS the declared type — nothing converts";
}

// ── P69 (lane `cs`): `typeof` names its operand's QUALIFIED type, and a function
//    typedef carries its parameters' qualifiers ────────────────────────────────────
// D-C-GENERIC-MATCHES-FUNCTION-TYPES-DIFFERING-IN-A-POINTEE-CONST and its two riders,
// D-C-TYPEOF-DROPS-ITS-OPERAND-QUALIFIERS and
// D-C-A-FUNCTION-TYPEDEFS-PARAMETER-QUALIFIERS-ARE-NOT-CLAIMED: a `typeof` head claimed no
// qualifier at all and a function typedef claimed no parameter, so an association
// spelled through either one matched a controlling expression C calls incompatible
// with it. ✔MEASURED (lane `cs`'s probes r4 and r4g, every build RUN): gcc 13.3.0 and
// clang 18.1.3 at `-std=c17 -pedantic-errors` and `-std=c2x`, MSVC 19.51 at
// `/std:clatest` — every case exits 42. DSS at HEAD took the `1` on r4a, r4b, r4i, r4j,
// r4k, r4p, r4r and r4v.
TEST(GenericQualifierAxis, TypeofAndAFunctionTypedefCarryTheirQualifiers) {
    expectSelects42({
        {"r4a __typeof__(&f_const) beside &f_plain",
         "void f_plain(char *p) { (void)p; }\nvoid f_const(const char *p) { (void)p; }\n"
         "int main(void) { return _Generic(&f_plain, __typeof__(&f_const): 1, default: 42); }\n"},
        {"r4b __typeof__(&f_plain) beside &f_const",
         "void f_plain(char *p) { (void)p; }\nvoid f_const(const char *p) { (void)p; }\n"
         "int main(void) { return _Generic(&f_const, __typeof__(&f_plain): 1, default: 42); }\n"},
        {"r4i __typeof__(cx) of a const int names a qualified type",
         "int main(void) { const int cx = 1; (void)cx;\n"
         "  return _Generic(0, __typeof__(cx): 1, default: 42); }\n"},
        {"r4j __typeof__(char *) beside a const char *",
         "int main(void) { return _Generic((const char *)0, __typeof__(char *): 1, default: 42); }\n"},
        {"r4k FC * — a function typedef with a const parameter",
         "void f_plain(char *p) { (void)p; }\ntypedef void FC(const char *);\n"
         "int main(void) { return _Generic(&f_plain, FC *: 1, default: 42); }\n"},
        {"r4p __typeof__ of a function-pointer object",
         "void f_plain(char *p) { (void)p; }\nvoid f_const(const char *p) { (void)p; }\n"
         "int main(void) { void (*fc)(const char *) = f_const;\n"
         "  return _Generic(&f_plain, __typeof__(fc): 1, default: 42); }\n"},
        {"r4r a char ** parameter beside a char *const * one",
         "void g1(char **p) { (void)p; }\nvoid g2(char *const *p) { (void)p; }\n"
         "int main(void) { return _Generic(&g1, __typeof__(&g2): 1, default: 42); }\n"},
        {"r4v __typeof__(t) of a const char * beside a char *",
         "int main(void) { char *s = 0; const char *t = 0; (void)s; (void)t;\n"
         "  return _Generic(s, __typeof__(t): 1, default: 42); }\n"},
        // THE CONTROLS: the same type spelled through `typeof` or a function typedef,
        // and a parameter's own top-level const, which is not part of the type.
        {"r4h a parameter's own top-level const",
         "void f_plain(char *p) { (void)p; }\nvoid f_top(char *const p) { (void)p; }\n"
         "int main(void) { return _Generic(&f_plain, __typeof__(&f_top): 42, default: 1); }\n"},
        {"r4m __typeof__ of the controlling expression itself",
         "void f_const(const char *p) { (void)p; }\n"
         "int main(void) { return _Generic(&f_const, __typeof__(&f_const): 42, default: 1); }\n"},
        {"r4s __typeof__ of a type name",
         "void f_const(const char *p) { (void)p; }\n"
         "int main(void) { return _Generic(&f_const, __typeof__(void (*)(const char *)): 42, default: 1); }\n"},
        {"r4t __typeof__(s) of the controlling object",
         "int main(void) { const char *s = 0; (void)s; return _Generic(s, __typeof__(s): 42, default: 1); }\n"},
        {"r4u __typeof__(&s)",
         "int main(void) { const char *s = 0; (void)s; return _Generic(&s, __typeof__(&s): 42, default: 1); }\n"},
        {"w01 the matching function typedef",
         "void f_const(const char *p) { (void)p; }\ntypedef void FC(const char *);\n"
         "int main(void) { return _Generic(&f_const, FC *: 42, default: 1); }\n"},
        {"w03 a function declared through the typedef keeps its parameters",
         "typedef int F(const char *);\nF h;\nint h(const char *s) { return s[0]; }\n"
         "int main(void) { return _Generic(&h, int (*)(const char *): 42, int (*)(char *): 2, default: 1); }\n"},
    });
}

// ── `typeof` of a FUNCTION type reads as a function typedef does ────────────────
// A function type name spells its Fn level first (the designator's shape), and a head
// naming a function type — a typedef or a `typeof` — gets that level back beneath a
// derivation, carrying the parameter claims. ✔MEASURED (lane `cs`'s probe r4k, every
// build RUN): gcc 13.3.0 and clang 18.1.3 at `-std=c2x` exit 42 on every case (k07 is
// prototype-census's own judge shape); MSVC 19.51 refuses `typeof(<function type name>)`
// (C2066) and builds the rest.
TEST(GenericQualifierAxis, TypeofOfAFunctionTypeCarriesItsParameters) {
    expectSelects42({
        {"k01 typeof(void (const char *)) * beside &f_plain",
         "void f_plain(char *p) { (void)p; }\n"
         "int main(void) { return _Generic(&f_plain, typeof(void (const char *)) *: 1, default: 42); }\n"},
        {"k02 typeof(void (char *)) * matches it",
         "void f_plain(char *p) { (void)p; }\n"
         "int main(void) { return _Generic(&f_plain, typeof(void (char *)) *: 42, default: 1); }\n"},
        {"k03 typeof(f_const) * beside &f_plain",
         "void f_plain(char *p) { (void)p; }\nvoid f_const(const char *p) { (void)p; }\n"
         "int main(void) { return _Generic(&f_plain, typeof(f_const) *: 1, default: 42); }\n"},
        {"k04 a function declared `typeof(f_const) h;` keeps the const parameter",
         "void f_const(const char *p) { (void)p; }\ntypeof(f_const) h;\nvoid h(const char *p) { (void)p; }\n"
         "int main(void) { return _Generic(&h, void (*)(const char *): 42, void (*)(char *): 1, default: 2); }\n"},
        {"k06 an object declared `typeof(f_const) *fp`",
         "void f_const(const char *p) { (void)p; }\n"
         "int main(void) { typeof(f_const) *fp = f_const;\n"
         "  return _Generic(fp, void (*)(const char *): 42, void (*)(char *): 1, default: 2); }\n"},
        {"k07 prototype-census's judge shape",
         "char *strcpy_like(char *restrict d, const char *restrict s) { (void)s; return d; }\n"
         "int main(void) { return _Generic(&strcpy_like,\n"
         "  typeof(char *(char *restrict, const char *restrict)) *: 42, default: 1); }\n"},
    });
}

// ── `typeof_unqual` drops the operand's OWN qualifiers and keeps the rest ───────
// ✔MEASURED (probes r4d t07-t10 and r4g w02, every build RUN): gcc 13.3.0 and clang
// 18.1.3 at `-std=c2x` (C17 has no `typeof`), MSVC 19.51 at `/std:clatest` — 42 on
// every one. The operands are declared at FILE scope, the section the block-scope pins
// above do not reach.
TEST(GenericQualifierAxis, TypeofUnqualDropsOnlyTheOperandsOwnQualifiers) {
    expectSelects42({
        {"t07 typeof_unqual(cx) of a const int is int",
         "const int cx = 3;\n"
         "int main(void) { return _Generic((typeof_unqual(cx) *)0, int *: 42, default: 1); }\n"},
        {"t08 typeof_unqual(t) keeps the pointee's const",
         "const char *t;\n"
         "int main(void) { return _Generic((typeof_unqual(t) *)0, const char **: 42, char **: 2, default: 1); }\n"},
        {"t09 typeof_unqual of a char *const is char *",
         "char *const t = 0;\n"
         "int main(void) { return _Generic((typeof_unqual(t) *)0, char **: 42, default: 1); }\n"},
        {"w02 typeof(t) keeps it too",
         "const char *t;\n"
         "int main(void) { return _Generic((typeof(t) *)0, const char **: 42, char **: 2, default: 1); }\n"},
        {"t10 *(const char *)q reads a const char, lvalue-converted to char",
         "int main(void) { const char c = 'x'; char *q = (char *)&c;\n"
         "  return _Generic(*(const char *)q, char: 42, default: 1); }\n"},
        // Each `typeof` shape of the test above beside its `typeof_unqual` twin
        // (✔MEASURED, probe r4j q01-q06: 42 on gcc 13.3.0 and clang 18.1.3 at `-std=c2x`
        // and MSVC 19.51 at `/std:clatest`).
        {"q01 typeof_unqual(cx) — r4i's twin — is int",
         "int main(void) { const int cx = 1; (void)cx; return _Generic(0, typeof_unqual(cx): 42, default: 1); }\n"},
        {"q02 typeof_unqual(t) — r4v's twin — keeps the pointee's const",
         "int main(void) { char *s = 0; const char *t = 0; (void)s; (void)t;\n"
         "  return _Generic(s, typeof_unqual(t): 1, default: 42); }\n"},
        {"q03 typeof_unqual(&f_const) — r4a's twin — keeps the parameter's",
         "void f_plain(char *p) { (void)p; }\nvoid f_const(const char *p) { (void)p; }\n"
         "int main(void) { return _Generic(&f_plain, typeof_unqual(&f_const): 1, default: 42); }\n"},
        {"q04 typeof_unqual(&f_const) — r4m's twin",
         "void f_const(const char *p) { (void)p; }\n"
         "int main(void) { return _Generic(&f_const, typeof_unqual(&f_const): 42, default: 1); }\n"},
        {"q06 typeof(cp) names `char *const`, typeof_unqual(cp) `char *`",
         "int main(void) { char *const cp = 0; (void)cp;\n"
         "  return _Generic(cp, typeof(cp): 1, typeof_unqual(cp): 42, default: 2); }\n"},
        {"q05 the volatile control: a volatile pointee parameter",
         "void f_plain(char *p) { (void)p; }\nvoid f_vol(volatile char *p) { (void)p; }\n"
         "int main(void) { return _Generic(&f_plain, __typeof__(&f_vol): 1, default: 42); }\n"},
    });
}

// ── a conditional over two pointers the TypeIds call one type, and C does not ────
// `c ? &f_plain : &f_const` over `int (char *)` / `int (const char *)`, `c ? pp : qq`
// over `char **` / `const char **`: the pointees are incompatible (C 6.7.6.1p2,
// 6.7.6.3p15), so the conditional is `void *` — the fork P68 round 9 decided for every
// incompatible pair — with the S_IncompatiblePointerConversion warning. ✔MEASURED
// (probe r4e, every build RUN): gcc 13.3.0 and clang 18.1.3 (c2x) select `void *` with
// "pointer type mismatch" on u01-u03 and u07; MSVC 19.51 selects an arm's type,
// silently. The controls (u04-u06, w04) select the pointer type on all three.
TEST(GenericQualifierAxis, AConditionalOverAQualifierApartPairIsAVoidPointer) {
    constexpr char const* kFns =
        "static int f_plain(char *s) { return s[0]; }\n"
        "static int f_const(const char *s) { return s[0]; }\n"
        "static int f_plain2(char *s) { return s[1]; }\n";
    struct Cond { char const* name; std::string src; std::size_t warnings; };
    auto const body = [&](char const* ctl) {
        return std::string{kFns}
             + "int main(int argc, char **argv) { char *a = \"x\"; const char *b = \"y\";\n"
               "  char **pp = &a; const char **qq = &b; char *const *cq = &a;\n"
               "  (void)argv; (void)pp; (void)qq; (void)cq;\n"
               "  return " + ctl + "; }\n";
    };
    for (Cond const& c : {
             Cond{"u02 two designators",
                  body("_Generic(argc > 0 ? f_plain : f_const, void *: 42, default: 1)"), 1},
             Cond{"u03 a designator beside &f",
                  body("_Generic(argc > 0 ? f_plain : &f_const, void *: 42, default: 1)"), 1},
             Cond{"r4o &f_plain beside &f_const",
                  body("_Generic(argc > 0 ? &f_plain : &f_const, int (*)(char *): 1,"
                       " int (*)(const char *): 2, default: 42)"), 1},
             Cond{"u01 char ** beside const char **",
                  body("_Generic(argc > 0 ? pp : qq, void *: 42, char **: 1, const char **: 2, default: 3)"), 1},
             // THE CONTROLS: one signature, a pointee's OWN qualifier, a parameter's own
             // top-level const, the same function twice.
             Cond{"u04 a designator beside &f of one signature",
                  body("_Generic(argc > 0 ? f_plain : &f_plain2, int (*)(char *): 42, default: 1)"), 0},
             Cond{"u05 char *const * beside char ** is char *const *",
                  body("_Generic(argc > 0 ? cq : pp, char *const *: 42, char **: 1, default: 2)"), 0},
             Cond{"w04 the same function in both arms",
                  body("_Generic(argc > 0 ? &f_const : &f_const, int (*)(const char *): 42,"
                       " int (*)(char *): 2, default: 1)"), 0},
         }) {
        auto model = analyzeC(c.src);
        EXPECT_FALSE(model.hasErrors()) << c.name << "\n" << c.src;
        EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_IncompatiblePointerConversion),
                  c.warnings) << c.name << "\n" << c.src;
        auto const arms = selectedArms(model);
        ASSERT_EQ(arms.size(), 1u) << c.name;
        EXPECT_EQ(arms[0], "42") << c.name << "\n" << c.src;
    }
    // A CALL through the `void *` conditional is refused, as gcc and clang refuse it.
    auto call = analyzeC(std::string{kFns}
        + "int main(int argc, char **argv) { (void)argv; char b[] = \"x\";\n"
          "  return (argc > 0 ? &f_plain : &f_const)(b); }\n");
    EXPECT_TRUE(call.hasErrors()) << "a `void *` is not a function";
}
