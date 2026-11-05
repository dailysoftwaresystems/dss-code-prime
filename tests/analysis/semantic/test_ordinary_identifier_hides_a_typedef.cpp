// ===========================================================================
// P69 L2 (lane `lm`) — AN ORDINARY IDENTIFIER HIDES A TYPEDEF OF ITS SPELLING.
//
// THE PROPERTY THIS FILE OWNS: in a TYPE-NAME position the type resolver answers
// with the declaration C puts in scope there. An object, a function, a parameter or
// an enumeration constant declared before the position — its declarator complete
// (C 6.2.1p7) — hides a typedef of its spelling (C 6.2.1p4), so the position names
// no type and the resolver's miss reports it. A declaration AFTER the position, a
// declaration inside whose own declarator the position sits, a structure MEMBER (its
// structure's own name space, C 6.2.3) and a TAG hide nothing.
//
// ✔MEASURED at eb6f22a3 (pe64, baseline and release, images run natively): the
// resolver walked past every non-type binding to the outer typedef, so DSS built and
// RAN `typedef int T; int main(void) { int T = 1; return _Generic(0, T: 1, default:
// 42) + T - 1; }` (1) and the same under `va_arg(ap, T)` (42). gcc 13.3.0 and clang
// 18.1.3, -std=gnu2x and -std=c2x -pedantic-errors, refuse each refusal below
// ("unknown type name 'T'" / "expected specifier-qualifier-list before 'T'") and
// build every acceptance (probes k7a, k7c, k7d, k8a, k8b, k9a-k9f in the lane's
// work directory).
//
// ── RED-ON-DISABLE ─────────────────────────────────────────────────────────
//   * the resolver skipping every non-type binding again (its predicate's
//     non-type arm answering false) → every REFUSAL here red, the acceptances green;
//   * the position test dropped (any non-type binding accepted) → the `after the
//     use` and `own declarator` acceptances red;
//   * the member test dropped → the two member acceptances red.
// ===========================================================================

#include "core/types/parse_diagnostic.hpp"

#include "semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <tuple>

using namespace dss;
using namespace dss::sem_test;

namespace {

struct Case {
    char const* what;
    char const* src;
    bool        refused;
};

void expectVerdicts(std::initializer_list<Case> cases, DiagnosticCode code) {
    for (Case const& c : cases) {
        auto cu = buildShippedUnit("c", {std::string{c.src}});
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
        EXPECT_EQ(hasCode(model.diagnostics(), code), c.refused)
            << c.what << " — expected " << (c.refused ? "REFUSED" : "accepted")
            << " by " << diagnosticCodeName(code) << "\n" << c.src;
    }
}

}  // namespace

// ── each kind of ordinary identifier hides the typedef in a type-name position ──
TEST(OrdinaryIdentifierHidesATypedef, InATypeNamePositionAnOrdinaryIdentifierNamesNoType) {
    expectVerdicts({
        {"an object, before a `_Generic` association",
         "typedef int T;\nint main(void) { int T = 1; "
         "return _Generic(0, T: 1, default: 42) + T - 1; }\n", true},
        {"an object, before `va_arg`'s type",
         "typedef int T;\nint f(int n, ...) { int T = 0; va_list ap; va_start(ap, n); "
         "int r = va_arg(ap, T); va_end(ap); return r + T; }\n", true},
        {"a parameter, in the body",
         "typedef int T;\nint f(int T) { return _Generic(0, T: 1, default: 2) + T; }\n",
         true},
        {"an enumeration constant",
         "typedef int T;\nint main(void) { enum { T = 40 }; "
         "return _Generic(0, T: 1, default: 2) + T; }\n", true},
        {"an object of an ENCLOSING block",
         "typedef int T;\nint main(void) { int T = 1; { "
         "return _Generic(0, T: 1, default: 2) + T; } }\n", true},
        // THE CONTROLS: the same positions where nothing hides the typedef.
        {"the typedef alone",
         "typedef int T;\nint main(void) { return _Generic(0, T: 42, default: 1); }\n",
         false},
        {"an object declared AFTER the use in the same block",
         "typedef int T;\nint main(void) { int r = _Generic(0, T: 42, default: 1); "
         "int T = 0; return r + T; }\n", false},
        {"the use inside the object's OWN declarator",
         "typedef int T;\nint main(void) { int T[_Generic(0, T: 3, default: 1)]; "
         "T[0] = 0; return T[0]; }\n", false},
        {"a structure MEMBER spelled like the typedef",
         "typedef int T;\nstruct S { int T; T x; };\n"
         "int main(void) { struct S s = { 1, 41 }; return s.T + s.x; }\n", false},
        {"a member, then a cast through the typedef in the next member's bound",
         "typedef int T;\nstruct S { int T; char a[(T) 42]; };\n"
         "int main(void) { struct S s; s.T = 0; return s.T; }\n", false},
        {"a TAG spelled like the typedef",
         "typedef int T;\nint main(void) { struct T { int a; }; "
         "return _Generic(0, T: 42, default: 1); }\n", false},
    }, DiagnosticCode::S_UnknownType);
}

// ── a parenthesised object before a parenthesised operand is a CALL of it ──
// (refused, as both references refuse it), never a cast through the outer typedef.
TEST(OrdinaryIdentifierHidesATypedef, AParenthesisedObjectIsCalledNotCast) {
    expectVerdicts({
        {"`(T)(x)` with T an int object",
         "typedef int T;\nint main(void) { int T = 2; int x = 40; return (T)(x); }\n",
         true},
        {"CONTROL: `(T)(x)` with T the typedef",
         "typedef int T;\nint main(void) { int x = 42; return (T)(x); }\n", false},
    }, DiagnosticCode::S_NotCallable);
}

// ── a TAG alone names no type: `(T)(x)` with only `struct T` declared ──
// Both references refuse it ("use of undeclared identifier 'T'"): the name is no
// ordinary identifier at all, so the parenthesised name is a call of nothing.
TEST(OrdinaryIdentifierHidesATypedef, ATagAloneIsNoOrdinaryIdentifier) {
    expectVerdicts({
        {"`(T)(x)` with only `struct T` declared",
         "struct T { int a; };\nint x = 3;\nint main(void) { return (T)(x); }\n", true},
        {"CONTROL: the same with a typedef of the tag",
         "struct T { int a; };\ntypedef struct T T;\nint x = 3;\n"
         "int main(void) { return (int)sizeof((T *)0); }\n", false},
    }, DiagnosticCode::S_UndeclaredIdentifier);
}

// ── P69 L2a2 (the L2a review's N9): a PARAMETER of a function declarator that is not a
// definition's own has function prototype scope (C 6.2.1p4) — it ends with that declarator,
// and the typedef it shadowed is a type again: `(T)(x)` is a cast. The parser's sketch kept the
// parameter visible for the rest of the block, so `(T)(x)` parsed as a call and was refused
// S_NotCallable. ✔MEASURED 2026-10-28 (WSL gcc 13.3.0 / clang 18.1.3, gnu2x and c2x
// -pedantic-errors, built and run): every accepted case below exits 42; the CONTROL (a
// definition's OWN parameter, which its body sees) is refused by both ("called object type
// 'int' is not a function or function pointer").
TEST(OrdinaryIdentifierHidesATypedef, APrototypeParameterHidesNothingPastItsDeclarator) {
    expectVerdicts({
        {"a function-pointer object's parameter",
         "typedef int T;\nint main(void) { int (*fp)(int T) = 0; int x = 42; (void)fp; "
         "return (T)(x); }\n", false},
        {"a block-scope prototype's parameter",
         "typedef int T;\nint main(void) { int f(int T); int x = 42; return (T)(x); }\n", false},
        {"a block-scope typedef's parameter",
         "typedef int T;\nint main(void) { typedef int (*F)(int T); F f = 0; int x = 42; "
         "(void)f; return (T)(x); }\n", false},
        {"a cast's type name's parameter",
         "typedef int T;\nint main(void) { (void)(int (*)(int T))0; int x = 42; "
         "return (T)(x); }\n", false},
        {"a definition's RETURNED function type's parameter, in the definition's body",
         "typedef int T;\nstatic int h(int t) { return t; }\n"
         "static int (*g(int a))(int T) { return (a == (T)(a)) ? h : 0; }\n"
         "int main(void) { return g(42)(42); }\n", false},
        // THE CONTROL: a definition's OWN parameter is in scope in its body and hides it.
        {"a definition's own parameter, in its body",
         "typedef int T;\nstatic int f(int T, int x) { return (T)(x); }\n"
         "int main(void) { return f(1, 42); }\n", true},
    }, DiagnosticCode::S_NotCallable);
}

// ── P69 L2b1 (the L2a2 review's A1): A DEFINITION RETURNING A FUNCTION POINTER keeps its OWN
// parameters in its body whatever the returned type's parameter lists are — `(T)(x)` there calls
// the int parameter and is refused S_NotCallable, as both references refuse it ("called object
// type 'int' is not a function"; the own-parameter control above is the measured form) — and a
// DECLARATION of one gives its own parameters to nothing after its declarator. The sketch dropped a
// definition's own list at every `param` row of the returned lists, so the body read `(T)(x)` as a
// cast. ✔MEASURED 2026-11-03 (probe-reference-cc run 20261103-200559-43e50237, gcc 13.3.0 and clang
// 18.1.3, built and run): the same shapes with `(T) - 1` in the body all exit 42.
TEST(OrdinaryIdentifierHidesATypedef, ADefinitionsOwnParameterSurvivesTheListsOfTheTypeItReturns) {
    struct Shape { char const* what; char const* head; char const* tail; char const* ret; };
    // `ret` returns a value of the declared type from the helpers below.
    Shape const shapes[] = {
        {"one level, the returned list unnamed", "int (*", ")(int)", "h"},
        {"one level, the returned list named", "int (*", ")(int u)", "h"},
        {"one level, the returned list (void)", "void (*", ")(void)", "hv"},
        {"two levels, unnamed", "int (*(*", ")(int))(int)", "h2"},
        {"two levels, named", "int (*(*", ")(int u))(int v)", "h2"},
        {"two levels, (void)", "void (*(*", ")(void))(void)", "hv2"},
    };
    char const* const helpers =
        "typedef int T;\nstatic int h(int t) { return t; }\nstatic void hv(void) {}\n"
        "static int (*h2(int u))(int) { (void)u; return h; }\n"
        "static void (*hv2(void))(void) { return hv; }\n";
    for (Shape const& s : shapes) {
        std::string const decl = std::string{s.head} + "g(int T, int x)" + s.tail;
        std::string const def = std::string{helpers} + "static " + decl
                                + " { int y = (T)(x); (void)y; return " + s.ret + "; }\n";
        std::string const dcl = std::string{helpers} + "static " + decl
                                + ";\nint main(void) { int x = 42; return (T)(x); }\n";
        for (auto const& [src, refused, form] :
             {std::tuple{def, true, "definition"}, std::tuple{dcl, false, "declaration"}}) {
            auto cu = buildShippedUnit("c", {src});
            assertNoBuilderErrors(*cu);
            auto model = analyze(cu, DiagnosticBudget::libraryDefault());
            EXPECT_EQ(hasCode(model.diagnostics(), DiagnosticCode::S_NotCallable), refused)
                << form << ", " << s.what << "\n" << src;
        }
    }
    // The `signal` shape: a function-pointer parameter beside a returned function pointer.
    auto cu = buildShippedUnit(
        "c", {std::string{"typedef int T;\nstatic void (*s(int T, void (*fn)(int), int x))(int) "
                          "{ int y = (T)(x); (void)y; return fn; }\n"}});
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_TRUE(hasCode(model.diagnostics(), DiagnosticCode::S_NotCallable)) << "the signal shape";
}
