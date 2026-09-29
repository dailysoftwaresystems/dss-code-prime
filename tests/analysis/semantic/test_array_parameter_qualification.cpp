// ===========================================================================
// P68 round 9 (lane `cs`) — THE ROUND'S P0 MERGE BLOCKER: an ARRAY PARAMETER took
// its ELEMENT's qualifiers as its own.
//
// C 6.7.6.3p7: "A declaration of a parameter as 'array of type' shall be adjusted to
// 'qualified pointer to type', where the type qualifiers (if any) are those specified
// within the [ and ] of the array type derivation." The adjusted POINTER — the
// parameter object — carries exactly the bracket's qualifiers, and the ELEMENT (the
// pointee) keeps its own:
//   * `O *const objv[]`   → a MODIFIABLE `O *const *`: `objv++` is legal, `objv[0] = q` is not;
//   * `const int p[]`     → a modifiable `const int *`;
//   * `int p[const]`      → an `int *const`: `p++` is refused, `p[1] = 42` is legal;
//   * `O *const p[const 4]` → both;
//   * `int p[static 4]`   → modifiable;
//   * `typedef int A[4]; void f(const A p)` → the head `const` qualifies the ELEMENT
//     (C 6.7.3p10), so `p` is a modifiable `const int *`.
//
// ✔MEASURED 2026-09-23 (the lane's `.temp/probe/p0`, `p0b`), every program RUN: gcc
// 13.3.0 and clang 18.1.3 at -std=c17 -pedantic-errors and -std=c2x, mingw-w64 13.2.0 and
// MSVC 19.51 agree on every line above (MSVC implements no bracket qualifiers — C2143 —
// and abstains on those; gcc 13 -pedantic-errors refuses the 2-D shape's CALL under the
// pre-C2X array-qualifier rule). DSS refused `objv++`, `p++` on `const int p[]`, on
// `const A p`, on `const int p[][2]` and on `main`'s `char *const argv[]`, and ACCEPTED
// `p++` on `int p[const]`. sqlite's `Tcl_Obj *CONST objv[]` … `objv++` (src/test1.c, in
// every leg's testfixture, and ext/session/test_session.c) was refused on every leg —
// measured by lane `mig` on 4d9a24c4. The refusal came from round 8's `++`/`--` const
// check meeting an adjustment that was already mis-qualified: `declaratorObjectIsConst`
// answered the ELEMENT's pointer layer (and a typedef head's `const`) for the object.
//
// RED-ON-DISABLE (the lane's transcript carries each build and the names): the Pass-1
// application removed → every declarator-array accepted shape reds and `int p[const]`
// is accepted again; the Pass-1.5 (typedef) application removed → the typedef'd-array
// shape reds; the bracket reader answering "no qualifier" → the `[const]` shapes are
// accepted again.
// ===========================================================================

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/data_model.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include "semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::sem_test;

namespace {

[[nodiscard]] SemanticModel analyzeC(std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault());
}

struct Case {
    char const* what;
    char const* src;
};

constexpr char const* kO = "typedef struct O { int v; } O;\n";

[[nodiscard]] std::string withO(char const* body) { return std::string{kO} + body; }

// The SOURCE TEXT of each `_Generic`'s SELECTED result expression, in source order —
// the direct observation of which association won (a `default` arm is an answer too).
[[nodiscard]] std::vector<std::string> selectedGenericArms(SemanticModel const& m) {
    std::vector<std::string> out;
    for (auto const& tree : m.unit().trees()) {
        RuleId const gid = tree.schema().rules().find("genericExpr");
        if (!gid.valid()) continue;
        for (std::uint32_t i = 1; i < tree.nodeCount(); ++i) {
            NodeId const node{i};
            if (tree.kind(node) != NodeKind::Internal || tree.rule(node).v != gid.v) continue;
            NodeId const sel = m.selectedGenericExpr(node);
            out.push_back(sel.valid() ? std::string{tree.text(sel)} : std::string{"<none>"});
        }
    }
    return out;
}

// The resolved type of the FIRST symbol named `name`.
[[nodiscard]] TypeId symbolType(SemanticModel const& m, std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i)
        if (m.symbols()[i].name == name) return m.symbols()[i].type;
    ADD_FAILURE() << "symbol '" << name << "' not found";
    return InvalidType;
}

// No error, and none of the diagnosed pointer conversions (a warning that would mean
// the two types were judged incompatible after all).
void expectNoIncompatibility(SemanticModel const& m, Case const& c) {
    EXPECT_FALSE(m.hasErrors())
        << c.what << "\n" << c.src << "\nfirst: "
        << (m.diagnostics().all().empty() ? "" : m.diagnostics().all()[0].actual);
    for (DiagnosticCode const code : {DiagnosticCode::S_IncompatiblePointerConversion,
                                      DiagnosticCode::S_IncompatiblePointerIntegerPointee,
                                      DiagnosticCode::S_IntegerPointerConversion}) {
        EXPECT_EQ(countCode(m.diagnostics(), code), 0u) << c.what << "\n" << c.src;
    }
}

// Every `_Generic` in `src` selects the arm spelled `1`.
void expectEverySelectionIsOne(Case const& c) {
    auto m = analyzeC(c.src);
    expectNoIncompatibility(m, c);
    auto const arms = selectedGenericArms(m);
    EXPECT_FALSE(arms.empty()) << c.what;
    for (std::string const& arm : arms) EXPECT_EQ(arm, "1") << c.what << "\n" << c.src;
}

}  // namespace

// Every shape the references accept builds CLEAN — the write each one performs is to
// a modifiable object.
TEST(ArrayParameterQualification, TheAdjustedPointerIsModifiableUnlessTheBracketSaysConst) {
    std::string const ptrConstElem = withO(
        "static int f(int n, O *const objv[]) { objv++; return objv[0]->v + n; }\n");
    std::string const bothRead = withO(
        "static int f(O *const p[const 4]) { return p[1]->v; }\n");
    std::string const control = withO(
        "static int f(int n, O *const *objv) { objv++; return objv[0]->v + n; }\n");
    for (Case const& c : std::initializer_list<Case>{
             {"`O *const objv[]`, then `objv++`", ptrConstElem.c_str()},
             {"`const int p[]`, then `p++`",
              "static int f(const int p[]) { p++; return p[0]; }\n"},
             {"`int p[const]`, then a write through it",
              "static int f(int p[const]) { p[1] = 42; return p[1]; }\n"},
             {"`O *const p[const 4]`, read only", bothRead.c_str()},
             {"`int p[static 4]`, then `p++`",
              "static int f(int p[static 4]) { p++; return p[2]; }\n"},
             {"`int p[restrict]`, then `p++`",
              "static int f(int p[restrict]) { p++; return p[0]; }\n"},
             {"a typedef'd array `const A p`, then `p++` (the const is the ELEMENT's)",
              "typedef int A[4];\nstatic int f(const A p) { p++; return p[2]; }\n"},
             {"`const int p[][2]`, then `p++`",
              "static int f(const int p[][2]) { p++; return p[0][1]; }\n"},
             {"`main`'s `char *const argv[]`, then `argv++`",
              "int main(int argc, char *const argv[]) { argv++; (void)argv; return argc; }\n"},
             {"an array of a typedef'd const pointer, then `p++`",
              "typedef char *const CP;\nstatic int f(CP p[]) { p++; return p[0][0]; }\n"},
             {"sqlite's own shape: `Tcl_Obj *CONST objv[]` behind a macro, then `objv++`",
              "#define CONST const\n"
              "typedef struct Tcl_Obj { int v; } Tcl_Obj;\n"
              "static int test_bind(int objc, Tcl_Obj *CONST objv[]) {\n"
              "    objv++; objc--; return objv[0]->v + objc; }\n"},
             {"the control: `O *const *objv`, then `objv++`", control.c_str()},
         }) {
        auto model = analyzeC(c.src);
        EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 0u)
            << c.what << "\n" << c.src;
        EXPECT_FALSE(model.hasErrors()) << c.what << "\n" << c.src;
    }
}

// Every write the references refuse is refused — once, with the const-write code.
TEST(ArrayParameterQualification, AWriteToAConstPointerOrThroughAConstElementIsRefused) {
    std::string const elemWrite = withO(
        "static int f(O *const objv[], O *q) { objv[0] = q; return 0; }\n");
    std::string const bothInc = withO(
        "static int f(O *const p[const 4]) { p++; return 0; }\n");
    std::string const bothElem = withO(
        "static int f(O *const p[const 4], O *q) { p[0] = q; return 0; }\n");
    for (Case const& c : std::initializer_list<Case>{
             {"`objv[0] = q` for `O *const objv[]` (the element is const)", elemWrite.c_str()},
             {"`p[0] = 1` for `const int p[]`",
              "static int f(const int p[]) { p[0] = 1; return 0; }\n"},
             {"`p++` for `int p[const]` (the POINTER is const)",
              "static int f(int p[const]) { p++; return p[0]; }\n"},
             {"`p++` for `O *const p[const 4]`", bothInc.c_str()},
             {"`p[0] = q` for `O *const p[const 4]`", bothElem.c_str()},
             {"`p[0] = 1` for a typedef'd array `const A p`",
              "typedef int A[4];\nstatic int f(const A p) { p[0] = 1; return 0; }\n"},
             {"the control: `p++` for `int *const p`",
              "static int f(int *const p) { p++; return 0; }\n"},
             {"a LOCAL `const int b[3]` is untouched: `b[0] = 5`",
              "static int f(void) { const int b[3] = { 1, 2, 3 }; b[0] = 5; return b[0]; }\n"},
         }) {
        auto model = analyzeC(c.src);
        EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ConstViolation), 1u)
            << c.what << "\n" << c.src;
    }
}

// ===========================================================================
// P68 round 10 (lane `cs`) — THE BRACKET'S INTERNED QUALIFIERS, and the two rules
// the fix had to meet on its way.
//
// `const` / `restrict` ride the symbol (round 9, above); `volatile` is interned on the
// TYPE, so the adjustment itself must build `int *volatile` for `int p[volatile]`. Two
// neighbouring rules decide whether that type is usable:
//   * C 6.7.6.3p15 — a FUNCTION type takes each parameter's UNQUALIFIED type, so a
//     function-pointer initialization, `_Generic` and a redeclaration see
//     `int (int *)` whichever way the parameter is spelled (the explicit
//     `int *volatile p` spelling was already mishandled: only the redeclaration
//     comparison dropped the qualifier);
//   * C 6.7.3p10 — a qualifier on a typedef'd ARRAY is its ELEMENT's, so
//     `volatile A p` for `typedef int A[2]` points at `volatile int`.
//
// ✔MEASURED 2026-09-24 (the lane's `.temp/probe/bv`, 36 cases, every program RUN):
// gcc 13.3.0 and clang 18.1.3 at -std=c17 -pedantic-errors and -std=c2x, and
// mingw-w64 13.2.0, select every arm spelled `1` below; MSVC 19.51 implements no
// bracket qualifier (C2143) and abstains on those, and selects with gcc on the
// explicit spellings and the typedef'd arrays. DSS dropped the bracket's `volatile`,
// kept a parameter's qualifier in the function type, and put a typedef'd array's
// qualifier on the array instead of its element — all silently.
// ===========================================================================

TEST(ArrayParameterQualification, TheBracketsVolatileQualifiesTheAdjustedPointer) {
    for (Case const& c : std::initializer_list<Case>{
             {"`int p[volatile]`",
              "static int f(int p[volatile]) {\n"
              "    return _Generic(&p, int *volatile *: 1, int **: 2, default: 3); }\n"},
             {"`int p[const volatile]` (the const rides the symbol, the volatile the type)",
              "static int f(int p[const volatile]) {\n"
              "    return _Generic(&p, int *const volatile *: 1, int *volatile *: 2,\n"
              "                    int *const *: 3, int **: 4, default: 5); }\n"},
             {"`int p[static volatile 2]`",
              "static int f(int p[static volatile 2]) {\n"
              "    return _Generic(&p, int *volatile *: 1, int **: 2, default: 3); }\n"},
             {"`int p[volatile static 2]`",
              "static int f(int p[volatile static 2]) {\n"
              "    return _Generic(&p, int *volatile *: 1, int **: 2, default: 3); }\n"},
             {"`int p[volatile][2]` (only the OUTERMOST bracket is the pointer's)",
              "static int f(int p[volatile][2]) {\n"
              "    return _Generic(&p, int (*volatile *)[2]: 1, int (**)[2]: 2, default: 3); }\n"},
             {"a VLA parameter `int p[volatile n]`",
              "static int f(int n, int p[volatile n]) {\n"
              "    return _Generic(&p, int *volatile *: 1, int **: 2, default: 3) + 0 * n; }\n"},
             {"an array of a typedef'd array, `A p[volatile 3]`",
              "typedef int A[2];\n"
              "static int f(A p[volatile 3]) {\n"
              "    return _Generic(&p, int (*volatile *)[2]: 1, int (**)[2]: 2, default: 3); }\n"},
             {"a plain prototype, then the bracket definition (C 6.7.6.3p15: one function)",
              "static int f(int *p);\n"
              "static int f(int p[volatile]) {\n"
              "    return _Generic(&p, int *volatile *: 1, int **: 2, default: 3); }\n"},
             {"a bracket prototype, then a plain definition — ITS `p` is plain",
              "static int f(int p[volatile]);\n"
              "static int f(int *p) {\n"
              "    return _Generic(&p, int **: 1, int *volatile *: 2, default: 3); }\n"},
             {"the VALUE of `p` is an `int *` (the qualifier is the pointer's, not the pointee's)",
              "static int f(int p[volatile]) {\n"
              "    return _Generic(p, int *: 1, volatile int *: 2, default: 3); }\n"},
             {"the control: the explicit spelling `int *volatile p`",
              "static int f(int *volatile p) {\n"
              "    return _Generic(&p, int *volatile *: 1, int **: 2, default: 3); }\n"},
         }) {
        expectEverySelectionIsOne(c);
    }
    // The OBJECT is volatile — its type, which is what every access of `p` is flagged by.
    auto m = analyzeC("static int f(int p[volatile]) { return p[0]; }\n");
    TypeId const p = symbolType(m, "p");
    ASSERT_TRUE(p.valid());
    EXPECT_TRUE(m.lattice().interner().isVolatileQualified(p));
    EXPECT_EQ(m.lattice().interner().kind(p), TypeKind::Ptr);
}

// C 6.7.6.2p1's bracket holds a `type-qualifier-list`, and `_Atomic` is a type qualifier;
// its star form is `[ type-qualifier-list_opt * ]`. gcc 13.3.0, clang 18.1.3 and
// mingw-w64 13.2.0 accept every form below (✔MEASURED 2026-09-24, the lane's
// `.temp/probe/bva`); DSS's grammar refused `_Atomic` in a bracket and any qualifier
// before a star. THE MEANING FORK, decided: gcc and mingw type `int p[_Atomic]` as
// `int *_Atomic`, clang as `int *`; neither vendor documents the case, and C
// 6.7.6.3p7 gives the adjusted pointer "the type qualifiers ... within the [ and ]" —
// gcc's reading. An `_Atomic` pointer is a distinct type in the function's type too (C
// 6.2.5p27), so a plain `int *p` definition CONFLICTS with the bracket prototype, as
// gcc says (clang, by its own reading, accepts).
TEST(ArrayParameterQualification, TheBracketTakesEveryQualifierAndAQualifiedStar) {
    for (Case const& c : std::initializer_list<Case>{
             {"`int p[_Atomic]`",
              "static int f(int p[_Atomic]) {\n"
              "    return _Generic(&p, int *_Atomic *: 1, int **: 2, default: 3); }\n"},
             {"`int p[static _Atomic 2]`",
              "static int f(int p[static _Atomic 2]) {\n"
              "    return _Generic(&p, int *_Atomic *: 1, int **: 2, default: 3); }\n"},
             {"a qualified star prototype, then the VLA definition",
              "static int f(int n, int p[volatile *]);\n"
              "static int f(int n, int p[volatile n]) {\n"
              "    return _Generic(&p, int *volatile *: 1, int **: 2, default: 3) + 0 * n; }\n"},
         }) {
        expectEverySelectionIsOne(c);
    }
    for (Case const& c : std::initializer_list<Case>{
             {"`int p[_Atomic (2)]`: the parenthesized bound is a bound",
              "static int f(int p[_Atomic (2)]) { return p[0]; }\n"},
             {"`int p[const *]` and `int p[restrict *]` prototypes",
              "int f(int n, int p[const *]);\n"
              "int g(int n, int p[restrict *]);\n"},
             {"the abstract `int [volatile *]`", "int f(int, int [volatile *]);\n"},
             {"the control: a deref bound `[*m]` still parses as a bound",
              "int f(int *m, int p[*m]);\n"},
         }) {
        auto m = analyzeC(c.src);
        EXPECT_FALSE(m.hasErrors())
            << c.what << "\n" << c.src << "\nfirst: "
            << (m.diagnostics().all().empty() ? "" : m.diagnostics().all()[0].actual);
    }
    // The `_Atomic` stays in the function's type, so the plain definition conflicts.
    {
        auto m = analyzeC("static int f(int p[_Atomic]);\n"
                          "static int f(int *p) { return p[0]; }\n");
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_IncompatibleRedeclaration), 1u);
    }
    // A bracket qualifier outside a parameter stays refused, `_Atomic` included.
    {
        auto m = analyzeC("int a[_Atomic 2];\n");
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_ArrayParamQualifierNonParameter), 1u);
    }
}

// C 6.7.6.2p1's other half: the decorations sit "only in the outermost array type
// derivation". gcc 13.3.0, clang 18.1.3 and mingw-w64 13.2.0 refuse a qualifier on a
// second dimension or on an array a pointer points at (✔MEASURED 2026-09-24, the lane's
// `.temp/probe/bv` bv38 / bv39); DSS built them and dropped the qualifier. The controls:
// the outermost bracket, a `[*]` in a later derivation (C allows it anywhere in a
// prototype), and a nested function parameter's OWN outermost bracket.
TEST(ArrayParameterQualification, ADecorationOffTheOutermostBracketIsRefused) {
    for (Case const& c : std::initializer_list<Case>{
             {"a qualifier on a pointer's pointee array",
              "static int f(int (*p)[volatile 3]) { return p != 0; }\n"},
             {"a qualifier on the second dimension",
              "static int f(int p[2][volatile 3]) { return p != 0; }\n"},
             {"`static` on the second dimension",
              "static int f(int p[2][static 3]) { return p != 0; }\n"},
             {"a `const` on a pointer's pointee array, in a prototype",
              "int f(int (*p)[const 3]);\n"},
         }) {
        auto m = analyzeC(c.src);
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_ArrayParamQualifierNonParameter), 1u)
            << c.what << "\n" << c.src;
    }
    for (Case const& c : std::initializer_list<Case>{
             {"the outermost bracket", "static int f(int p[volatile][3]) { return p != 0; }\n"},
             {"a `[*]` in a later derivation", "int f(int n, int p[][*]);\n"},
             {"a nested function parameter's own outermost bracket",
              "static int g(int (*h)(int q[volatile])) { return h != 0; }\n"},
         }) {
        auto m = analyzeC(c.src);
        EXPECT_FALSE(m.hasErrors())
            << c.what << "\n" << c.src << "\nfirst: "
            << (m.diagnostics().all().empty() ? "" : m.diagnostics().all()[0].actual);
    }
}

TEST(ArrayParameterQualification, AFunctionTypeTakesEachParametersUnqualifiedType) {
    for (Case const& c : std::initializer_list<Case>{
             {"`int f(int *volatile p)` is an `int (*)(int *)`",
              "static int f(int *volatile p) { return p[0]; }\n"
              "int g(void) { return _Generic(f, int (*)(int *): 1, default: 2); }\n"},
             {"`int f(int p[volatile])` is an `int (*)(int *)`",
              "static int f(int p[volatile]) { return p[0]; }\n"
              "int g(void) { return _Generic(f, int (*)(int *): 1, default: 2); }\n"},
             {"the association spelled qualified matches the plain function",
              "static int f(int *p) { return p[0]; }\n"
              "int g(void) { return _Generic(f, int (*)(int *volatile): 1, default: 2); }\n"},
             {"a scalar `volatile int x`",
              "static int f(volatile int x) { return x; }\n"
              "int g(void) { return _Generic(f, int (*)(int): 1, default: 2); }\n"},
             {"a NESTED function type's parameter",
              "static int g(int (*h)(int *volatile), int *a) { return h(a); }\n"
              "int k(void) { return _Generic(g, int (*)(int (*)(int *), int *): 1, default: 2); }\n"},
             {"the control: `_Atomic` is a distinct type and STAYS (C 6.2.5p27)",
              "static int f(_Atomic int x) { return x; }\n"
              "int g(void) { return _Generic(f, int (*)(_Atomic int): 1, int (*)(int): 2, default: 3); }\n"},
         }) {
        expectEverySelectionIsOne(c);
    }
    // An initialization from the function converts nothing — no pointer-conversion
    // diagnostic (the explicit spelling drew S_IncompatiblePointerConversion before).
    for (Case const& c : std::initializer_list<Case>{
             {"explicit", "static int f(int *volatile p) { return p[0]; }\n"
                          "int g(int *a) { int (*fp)(int *) = f; return fp(a); }\n"},
             {"bracket", "static int f(int p[volatile]) { return p[0]; }\n"
                         "int g(int *a) { int (*fp)(int *) = f; return fp(a); }\n"},
         }) {
        expectNoIncompatibility(analyzeC(c.src), c);
    }
    // The qualifiers a `void` parameter carries are READ before the unqualification: a
    // definition's sole `volatile void` stays refused, and a NAMED `volatile void v`
    // still does not end argument matching (only an unqualified void does).
    {
        auto m = analyzeC("static int k(volatile void) { return 0; }\n");
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_InvalidVoidParam), 1u);
    }
    {
        auto m = analyzeC("void f(volatile void v);\nint g(void) { f(); return 0; }\n");
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_ArgCountMismatch), 1u);
    }
}

TEST(ArrayParameterQualification, AQualifierOnATypedefdArrayIsItsElements) {
    for (Case const& c : std::initializer_list<Case>{
             {"a parameter `volatile A p` points at `volatile int`",
              "typedef int A[2];\n"
              "static int f(volatile A p) {\n"
              "    return _Generic(&p, volatile int **: 1, int *volatile *: 2, int **: 3, default: 4); }\n"},
             {"a local's element address",
              "typedef int A[2];\n"
              "int g(void) { volatile A x = { 40, 2 };\n"
              "    return _Generic(&x[0], volatile int *: 1, int *: 2, default: 3); }\n"},
             {"a local's decay",
              "typedef int A[2];\n"
              "int g(void) { volatile A x = { 40, 2 };\n"
              "    return _Generic(x, volatile int *: 1, int *: 2, default: 3); }\n"},
             {"a local's address: the array of `volatile int`",
              "typedef int A[2];\n"
              "int g(void) { volatile A x = { 40, 2 };\n"
              "    return _Generic(&x, volatile int (*)[2]: 1, int (*)[2]: 2, default: 3); }\n"},
             {"a 2-D typedef: the INNERMOST element",
              "typedef int M[2][3];\n"
              "int g(void) { volatile M m = { { 0 } };\n"
              "    return _Generic(&m[1][2], volatile int *: 1, int *: 2, default: 3); }\n"},
         }) {
        expectEverySelectionIsOne(c);
    }
    // The type itself: an array of `volatile int` with no skin on the array, and no size
    // change. (Analyzed WITH the shipped targets' aggregate layout — without one a
    // `sizeof` of an array declines to fold in a constant expression.)
    auto cu = buildShippedUnit("c", {std::string{
        "typedef int A[2];\n"
        "_Static_assert(sizeof(volatile A) == sizeof(A), \"a qualifier changes no size\");\n"
        "volatile A x;\n"}});
    assertNoBuilderErrors(*cu);
    auto m = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                     AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_FALSE(m.hasErrors())
        << (m.diagnostics().all().empty() ? "" : m.diagnostics().all()[0].actual);
    TypeId const x = symbolType(m, "x");
    ASSERT_TRUE(x.valid());
    auto const& in = m.lattice().interner();
    EXPECT_EQ(in.qualifierBits(x), 0) << "the ARRAY carries no qualifier skin";
    ASSERT_EQ(in.kind(x), TypeKind::Array);
    auto const elem = in.operands(x);
    ASSERT_FALSE(elem.empty());
    EXPECT_TRUE(in.isVolatileQualified(elem[0])) << "the ELEMENT is `volatile int`";
}
