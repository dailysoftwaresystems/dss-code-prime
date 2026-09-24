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
#include "core/types/parse_diagnostic.hpp"

#include "semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
#include <string>

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
