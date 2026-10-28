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
