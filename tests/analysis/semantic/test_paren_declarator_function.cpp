// P41 lane N — [[D-CSUBSET-PARENTHESIZED-FUNCTION-DEFINITION-DECLARATOR-REFUSED]]
//
// C 6.7.6p1 lets any declarator be wrapped in redundant parentheses, so
// `int (foo)(int x) { … }` defines the FUNCTION `foo` — the standard glibc/musl
// idiom for defining a name that is also a function-like macro. DSS refused it.
//
// ✔MEASURED through the shipped CLI at the pre-fix HEAD, with gcc 13.3.0 and clang
// 18.1.3 probed SEPARATELY and a deliberately-broken negative control in every
// probe run (a run in which the controls do not split is an instrument failure, not
// a compiler verdict). Both references COMPILE AND RUN every accepting shape below;
// DSS produced THREE different symptoms from ONE cause — a declarator-shape walk
// that looked only at the name's OWN direct declarator and could not step out
// through a parenthesis:
//     int (foo)(int x) { return x + 1; }   S0018 "a function definition's
//                                          declarator must be a function declarator"
//     int (foo)(int x);                    never became a PROTOTYPE, so it bound as
//                                          an object and its definition collided
//                                          (S0002 + S0018 "function prototype
//                                          declarations are not supported here")
//     …the same definition's body          S0001 on its OWN parameter — the params
//                                          were scoped as a function POINTER's
//
// ★★ WHY THIS FILE EXISTS BESIDE `examples/c/paren_fn_def_declarator/`. The example
// owns the ACCEPTING half and the runtime proof. What an example structurally
// cannot carry is the REFUSING half — and the refusing half is the whole risk here,
// because the obvious way to make the accepting shapes work is to weaken the
// function-definition constraint until it accepts everything. `int (*fp)(int x){…}`,
// `int (a)[3]{…}` and `int (x){…}` are all REFUSED by gcc and by clang (✔MEASURED),
// and they must stay refused, LOUD, by code.
//
// ★ AND THE SHARPEST PAIR IN THE FILE is the two-fn-suffix one. In
// `int (*(chooser)(int k))(int w) { … }` two function suffixes sit over ONE name:
// the INNER `(int k)` declares `chooser`, the OUTER `(int w)` belongs to the
// RETURNED function type. ✔MEASURED: gcc and clang both ACCEPT a body reading `k`
// and both REFUSE a body reading `w` ("'w' undeclared" / "use of undeclared
// identifier 'w'"). A shape-only test cannot separate those two suffixes; the
// engine's `fnSuffixDeclaresTheName` separates them by NODE IDENTITY against the
// name's innermost derivation, and this pair is what proves it did.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

using namespace dss;
using namespace dss::sem_test;

namespace {

[[nodiscard]] SemanticModel analyzeC(std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
}

// ⚠ A SEMANTIC PIN THAT NEVER CHECKS THE PARSE CAN BE GREEN ON A SOURCE THE
// COMPILER NEVER READ. If the fixture text does not parse, `analyze()` sees a
// broken tree, emits none of the codes below, and every EXPECT_EQ(…, 0u) passes
// while asserting nothing at all. Every test here calls this FIRST.
[[nodiscard]] std::size_t parseErrorsFor(std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    std::size_t n = 0;
    for (auto const& t : cu->trees())
        for (auto const& d : t.diagnostics().all())
            if (d.severity == DiagnosticSeverity::Error) ++n;
    return n;
}

[[nodiscard]] SymbolRecord const* symbolNamed(SemanticModel const& m,
                                              std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i)
        if (m.symbols()[i].name == name) return &m.symbols()[i];
    return nullptr;
}

} // namespace


// ★ THE ROW'S EXACT REPRO, pinned on the SYMBOL and not merely on "no error".
// "Zero diagnostics" would also be satisfied by a fix that stopped checking
// function definitions altogether; only the symbol's KIND says the declarator was
// actually understood as a function.
TEST(ParenFunctionDeclarator, AParenthesizedNameStillDeclaresAFunction) {
    std::string const src = "int (foo)(int x) { return x + 1; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidFunctionDeclarator), 0u)
        << "C 6.7.6p1 permits parentheses around a declarator; gcc and clang both "
           "compile and run this";
    auto const* foo = symbolNamed(model, "foo");
    ASSERT_NE(foo, nullptr);
    EXPECT_EQ(foo->kind, DeclarationKind::Function);
    EXPECT_FALSE(foo->isProtoDeclaration)
        << "a body is present — this is the DEFINITION, not a prototype";
}

// ★★ THE SCOPE HALF, and it is a SEPARATE failure from the one above. The
// definition's own parameters used to be scoped as if the suffix belonged to a
// function POINTER, so the body could not see them. This test is red on that bug
// even when the declarator constraint is satisfied, which is exactly why it is
// stated as its own pin rather than folded into "the file compiles".
TEST(ParenFunctionDeclarator, TheDefinitionsOwnParametersAreVisibleInItsBody) {
    std::string const src = "int (add)(int a, int b) { return a + b; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UndeclaredIdentifier), 0u)
        << "a definition's parameters bind into the definition's scope, so its "
           "body must see them (C 6.2.1p4)";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidFunctionDeclarator), 0u);
}

// The PROTOTYPE half. A parenthesized prototype must BE a prototype, or it binds
// as an object and the later definition collides — the S0002 half of the measured
// symptom set. Both spellings are pinned in one source so the merge is exercised
// in the direction the glibc idiom actually writes it.
TEST(ParenFunctionDeclarator, AParenthesizedPrototypeMergesWithItsDefinition) {
    std::string const src =
        "int (foo)(int x);\n"
        "int foo(int x) { return x + 1; }\n"
        "int main(void) { return foo(41) == 42 ? 0 : 1; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "the prototype and the definition are ONE function, not two symbols";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidFunctionDeclarator), 0u);
    auto const* foo = symbolNamed(model, "foo");
    ASSERT_NE(foo, nullptr);
    EXPECT_EQ(foo->kind, DeclarationKind::Function);
}

// The GENERAL form, not just the headline shape: the walk must step out of MORE
// than one parenthesis, and a pointer RESULT must not be mistaken for a pointer
// OBJECT (`*` binds to the result here, so `pick` is still a function).
TEST(ParenFunctionDeclarator, NestedParenthesesAndAPointerResultAreFunctionsToo) {
    std::string const src =
        "int ((neg))(int v) { return -v; }\n"
        "int *(pick)(int *p, int *q) { return *p > *q ? p : q; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidFunctionDeclarator), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UndeclaredIdentifier), 0u);
    auto const* neg = symbolNamed(model, "neg");
    ASSERT_NE(neg, nullptr);
    EXPECT_EQ(neg->kind, DeclarationKind::Function);
    auto const* pick = symbolNamed(model, "pick");
    ASSERT_NE(pick, nullptr);
    EXPECT_EQ(pick->kind, DeclarationKind::Function);
}

// ★★ THE ACCEPTING HALF OF THE TWO-SUFFIX PAIR. `chooser` is a function taking
// `int k` and returning `int (*)(int)`. The INNER suffix declares it, so `k` is a
// real parameter and the body must see it. ✔MEASURED: gcc and clang both compile
// and run this.
TEST(ParenFunctionDeclarator, TheInnerSuffixDeclaresTheNameAndBindsItsParameters) {
    std::string const src =
        "static int helper(int y) { return y * 2; }\n"
        "int (*(chooser)(int k))(int) { return k ? helper : helper; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UndeclaredIdentifier), 0u)
        << "`k` is chooser's OWN parameter — the inner suffix is the one that "
           "declares the name";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidFunctionDeclarator), 0u);
    auto const* chooser = symbolNamed(model, "chooser");
    ASSERT_NE(chooser, nullptr);
    EXPECT_EQ(chooser->kind, DeclarationKind::Function);
}

// ★★ THE REFUSING HALF OF THE SAME PAIR — the one an example cannot carry. The
// OUTER suffix's parameter belongs to the RETURNED function type and has function-
// prototype scope (C 6.2.1p4), so the body must NOT see it. ✔MEASURED: gcc says
// "'w' undeclared (first use in this function)", clang "use of undeclared
// identifier 'w'". Without this pin, "make the parameters visible" could be
// implemented by making ALL of them visible, and every accepting test above would
// still pass.
TEST(ParenFunctionDeclarator, TheOuterSuffixsParameterIsNotVisibleInTheBody) {
    std::string const src =
        "static int helper(int y) { return y * 2; }\n"
        "int (*(chooser)(int k))(int w) { (void)k; return w ? helper : helper; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u)
        << "the construct PARSES; the refusal below is semantic, which is what "
           "lets it name a reason";
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UndeclaredIdentifier), 1u)
        << "`w` names a parameter of the RETURNED function type — it has "
           "function-prototype scope and is invisible to chooser's body";
}

// ★★ THE OVER-REACH GUARD. A function POINTER declarator with a body is NOT a
// function definition (C 6.9.1p2 — the identifier must have FUNCTION type), and it
// is refused by gcc and by clang (✔MEASURED). The pointer layer reaches the name
// before any suffix does, which is precisely the distinction the derivation walk
// must keep drawing after it learned to see through parentheses. Loud, by code —
// accepting this would be a silent miscompile of a pointer as a function.
TEST(ParenFunctionDeclarator, AFunctionPointerDeclaratorIsStillNotADefinition) {
    std::string const src = "int (*fp)(int x) { return x; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_GE(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidFunctionDeclarator), 1u)
        << "`fp` is a POINTER to function, not a function — this must stay refused";
}

// The same guard one parenthesis deeper: the ascent must not lose the pointer
// layer just because it stepped out of a group.
TEST(ParenFunctionDeclarator, ANestedFunctionPointerDeclaratorIsStillNotADefinition) {
    std::string const src = "int ((*fp))(int x) { return x; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_GE(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidFunctionDeclarator), 1u);
}

// An ARRAY suffix is a derivation too, and it reaches the name first here — so the
// name is an array, not a function. Refused by gcc and by clang (✔MEASURED). This
// is the pin that stops the walk from answering "function" for any suffix at all.
TEST(ParenFunctionDeclarator, AParenthesizedArrayDeclaratorIsStillNotADefinition) {
    std::string const src = "int (a)[3] { return 1; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_GE(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidFunctionDeclarator), 1u)
        << "`a` is an ARRAY — parentheses around the name do not make it a function";
}

// And a parenthesized name with NO derivation at all: its type is the head's, so a
// body is meaningless. Refused by gcc and by clang (✔MEASURED). This is the arm
// where the ascent runs out of parentheses and must answer "no derivation" rather
// than falling through to a default.
TEST(ParenFunctionDeclarator, AnUndecoratedParenthesizedNameIsStillNotADefinition) {
    std::string const src = "int (x) { return 1; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_GE(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidFunctionDeclarator), 1u);
}

// ★★ THE CONSTRAINT THAT HAD TO BE CHECKED ON PURPOSE ONCE THE ACCIDENT WAS GONE.
// C 6.7.6.3p1: a function declarator shall not specify a function RETURN type.
// ✔MEASURED: gcc says "'f' declared as function returning a function", clang
// "function cannot return function type".
//
// The typedef spelling (`typedef int Fn(int); Fn (f)(int);` — pinned in
// `test_semantic_analyzer_c.cpp`) used to be refused only as a SIDE EFFECT of the
// shape walk being blind to redundant parentheses. Teaching the walk to see through
// them removes that side effect, so the constraint is now checked off the RESOLVED
// type — which is also why the INLINE spelling below, never covered by the accident,
// is refused too. Both directions of that change are load-bearing: without the new
// check this is a SILENT acceptance of a function value returned by value.
TEST(ParenFunctionDeclarator, AFunctionCannotReturnAFunctionType) {
    std::string const src = "int f(int)(int);\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_GE(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidFunctionDeclarator), 1u)
        << "C 6.7.6.3p1 — a function declarator cannot specify a function return "
           "type; both references refuse this";
}

// …and its legal sibling, so the check above is not over-broad: returning a POINTER
// to function is exactly how C spells the intent, and it must stay accepted.
TEST(ParenFunctionDeclarator, ReturningAPointerToFunctionIsStillAccepted) {
    std::string const src =
        "static int helper(int y) { return y * 2; }\n"
        "int (*(chooser)(int k))(int) { return k ? helper : helper; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidFunctionDeclarator), 0u)
        << "Ptr<FnSig> is a pointer, not a function — the C 6.7.6.3p1 check must "
           "not fire on it";
}

// ★★ THE PRE-EXISTING SILENT ACCEPTANCE THIS CYCLE HAD TO CLOSE RATHER THAN WIDEN.
// C 6.7.2.1p9: a struct/union member shall not have function type. The TYPEDEF
// spelling (`typedef int Fn(int); struct S { Fn f; };`) was always refused loud, but
// the SYNTACTIC one was MEASURED ACCEPTED CLEAN through the shipped CLI — the name
// carries a `()` suffix, so a purely syntactic prototype test flagged the FIELD as a
// prototype and the resolve arm upgraded it in silence. That predates this cycle (the
// plain spelling behaves identically before and after the walk change), but teaching
// the walk to see through parentheses hands `struct S { int (f)(int); };` the same
// path — so the two are pinned together, as a PAIR, because a fix that caught only
// one spelling would look identical to a fix that caught both.
TEST(ParenFunctionDeclarator, AFunctionTypedMemberIsRefusedInBothSpellings) {
    for (std::string const& src : {std::string{"struct S { int f(int); };\n"},
                                   std::string{"struct S { int (f)(int); };\n"}}) {
        ASSERT_EQ(parseErrorsFor(src), 0u) << src;
        auto model = analyzeC(src);
        EXPECT_GE(countCode(model.diagnostics(),
                            DiagnosticCode::S_InvalidFunctionDeclarator), 1u)
            << "a member of function type must fail loud, in EVERY spelling: " << src;
    }
}

// …and the legal member, so the refusal above is not over-broad: a POINTER to
// function is exactly what C expects here and must stay accepted.
TEST(ParenFunctionDeclarator, AFunctionPointerMemberIsStillAccepted) {
    std::string const src = "struct S { int (*fp)(int); };\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidFunctionDeclarator), 0u);
}

// The CONTROL. Every shape above is a variation on a plain function definition,
// and a change that broke the plain one while fixing the parenthesized ones would
// be caught by the whole suite — but not by anything in THIS file, which is where
// a reader will look first when one of these goes red.
TEST(ParenFunctionDeclarator, ThePlainUnparenthesizedFormIsUnchanged) {
    std::string const src =
        "int foo(int x);\n"
        "int foo(int x) { return x + 1; }\n"
        "int (*fpv)(int) = foo;\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_InvalidFunctionDeclarator), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_RedeclaredSymbol), 0u);
    auto const* foo = symbolNamed(model, "foo");
    ASSERT_NE(foo, nullptr);
    EXPECT_EQ(foo->kind, DeclarationKind::Function);
    auto const* fpv = symbolNamed(model, "fpv");
    ASSERT_NE(fpv, nullptr);
    EXPECT_EQ(fpv->kind, DeclarationKind::Variable)
        << "a function POINTER object stays a Variable";
}
