// P68 round 8, lane `ht`, part 2 — WHAT A FUNCTION TYPE MAY SPELL, AND WHEN ITS TYPES MUST BE COMPLETE.
//
// Five C constraints the front end accepted, and one meaning it refused, each MEASURED against gcc 13.3.0 and
// clang 18.1.3 (`-std=c17 -pedantic-errors`), mingw-w64 13.2.0 and MSVC 19.51 (`/std:c17`) — every accepting
// reference's program BUILT AND RUN — before anything here was written:
//
//   * C 6.7.6.3p1 — a function declarator returns neither a function nor an ARRAY. All four refuse every spelling;
//     DSS refused only a function return on a declared function or typedef, and `int f(void)[3] { … }` compiled.
//   * C 6.9.1p3 / C 6.7.6.3p4 — a function DEFINITION's return type and parameters are complete. All four refuse;
//     DSS accepted, and died at HIR→MIR under a sentence about by-value struct calling conventions (or compiled,
//     when the composite was completed later in the translation unit).
//   * C 6.5.2.2p1 / p4 — a CALL's result and every argument are complete at the call. gcc, clang and mingw
//     refuse; MSVC accepts and its programs FAIL — a crash (0xC0000005) or a wrong sum — which is why both codes
//     are unsuppressable. DSS accepted.
//   * the `(void)` parameter list, and the NAMED void parameter gcc gives a meaning to (a parameter of an
//     incomplete type at which a call's arguments END) — see `normalizeSoleVoidParams` for the whole measured
//     table.
//
// ★ Completeness is POSITIONAL (C 6.2.5p1): a composite completed LATER in the translation unit is still
// incomplete at an earlier definition or call, and every such pin has a completed-BEFORE twin that must stay
// accepted — a check that refused everything would pass the refusing half alone.
//
// ⚠ Every test checks the PARSE first: a fixture that does not parse emits none of the codes below, and every
// `EXPECT_EQ(…, 0u)` would pass while asserting nothing. And each refusal is counted EXACTLY — one fact, one
// diagnostic — so a doubled report (a nested declarator re-resolved with emission on) is red here too.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::sem_test;

namespace {

[[nodiscard]] SemanticModel analyzeC(std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
}

[[nodiscard]] std::size_t parseErrorsFor(std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    std::size_t n = 0;
    for (auto const& t : cu->trees())
        for (auto const& d : t.diagnostics().all())
            if (d.severity == DiagnosticSeverity::Error) ++n;
    return n;
}

[[nodiscard]] TypeId typeOfSymbol(SemanticModel const& m, std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i)
        if (m.symbols()[i].name == name) return m.symbols()[i].type;
    ADD_FAILURE() << "no symbol named '" << name << "'";
    return InvalidType;
}

// Every Error-severity diagnostic's code and text — the `<<` payload of a failing EXPECT.
[[nodiscard]] std::string errorsOf(SemanticModel const& m) {
    std::string s;
    for (auto const& d : m.diagnostics().all()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        s += "\n  ";
        s += diagnosticCodeName(d.code);
        s += ": ";
        s += d.actual;
    }
    return s;
}

// Each source must parse, and must draw `code` EXACTLY once.
void expectRefusedOnce(std::vector<std::string> const& sources, DiagnosticCode code) {
    for (std::string const& src : sources) {
        ASSERT_EQ(parseErrorsFor(src), 0u) << src;
        auto const model = analyzeC(src);
        EXPECT_EQ(countCode(model.diagnostics(), code), 1u)
            << diagnosticCodeName(code) << " — exactly once for:\n" << src << errorsOf(model);
    }
}

// Each source must parse and analyze with NO error at all.
void expectAccepted(std::vector<std::string> const& sources) {
    for (std::string const& src : sources) {
        ASSERT_EQ(parseErrorsFor(src), 0u) << src;
        auto const model = analyzeC(src);
        EXPECT_FALSE(model.hasErrors()) << "must be accepted:\n" << src << errorsOf(model);
        EXPECT_FALSE(hasDiagnosedPointerConversion(model.diagnostics()))
            << "a compatible pointer pair must not be DIAGNOSED (the vacuity sweep)";
    }
}

} // namespace

// ═══ C 6.7.6.3p1 — NO FUNCTION OR ARRAY RETURN, IN ANY DECLARATOR ════════════════════════════════════════════

// The array return, in every spelling the references refuse. Before the move DSS accepted all of them.
TEST(FunctionDeclaratorReturn, AnArrayReturnIsRefusedInEverySpelling) {
    expectRefusedOnce({
        "int f(void)[3];\n",
        "int f(void)[3] { for (;;) {} }\n",
        "typedef int A[3];\nA f(void);\n",
        "typedef int A[3];\nA f(void) { for (;;) {} }\n",
        "int (*fp)(void)[3];\n",
        "typedef int F(void)[3];\n",
        "void g(int p(void)[3]);\n",
        "int main(void) { return (int)sizeof(int (*)(void)[3]); }\n",
        "int main(void) { void *q = 0; (void)(int (*)(void)[3])q; return 0; }\n",
    }, DiagnosticCode::S_InvalidFunctionDeclarator);
}

// The function return under a pointer and in a type name — the spellings the named-declarator check never saw.
TEST(FunctionDeclaratorReturn, AFunctionReturnIsRefusedUnderAPointerAndInATypeName) {
    expectRefusedOnce({
        "int (*fp)(void)(void);\n",
        "typedef int G(void)(void);\n",
        "int main(void) { return (int)sizeof(int (*)(void)(void)); }\n",
    }, DiagnosticCode::S_InvalidFunctionDeclarator);
}

// The legal siblings: returning a POINTER to an array or to a function is exactly how C spells the intent.
TEST(FunctionDeclaratorReturn, ReturningAPointerToAnArrayOrAFunctionStaysLegal) {
    expectAccepted({
        "int (*f(void))[3];\n",
        "int (*(*fp)(void))(void);\n",
        "typedef int A[3];\nA *h(void);\n",
    });
}

// Positioned at the suffix that makes the illegal return — `(void)`, not the name or the whole declaration.
TEST(FunctionDeclaratorReturn, TheRefusalIsPositionedAtTheFunctionSuffix) {
    std::string const src = "int f(void)[3];\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto cu = buildShippedUnit("c", {src});
    auto const model = analyze(cu, DiagnosticBudget::libraryDefault());
    ASSERT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_InvalidFunctionDeclarator), 1u);
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_InvalidFunctionDeclarator) continue;
        // The span is remapped onto its ORIGIN buffer — slice that one, never the synth tree source.
        std::string_view text;
        for (auto const& buf : cu->auxiliaryBuffers())
            if (buf != nullptr && buf->id() == d.buffer) { text = buf->slice(d.span); break; }
        EXPECT_EQ(text, "(void)");
        EXPECT_NE(d.actual.find("array return type (C 6.7.6.3p1)"), std::string::npos) << d.actual;
    }
}

// ═══ C 6.9.1p3 — A DEFINITION'S RETURN TYPE IS COMPLETE ════════════════════════════════════════════════════

TEST(IncompleteReturn, ADefinitionReturningAnIncompleteTypeIsRefused) {
    expectRefusedOnce({
        "struct S;\nstruct S f(void) { for (;;) {} }\n",
        "union U;\nunion U f(void) { for (;;) {} }\n",
        "struct S;\nstatic struct S f(void) { for (;;) {} }\nint main(void) { return 0; }\n",
        // completed LATER in the translation unit: still incomplete at the definition (C 6.2.5p1)
        "struct S;\nstruct S f(void) { for (;;) {} }\nstruct S { int x; };\n",
    }, DiagnosticCode::S_IncompleteReturnType);
}

TEST(IncompleteReturn, APrototypeMayNameItAndACompletedOneIsLegal) {
    expectAccepted({
        "struct S;\nstruct S f(void);\n",
        "struct S;\nstruct S f(void);\nstruct S { int x; };\n"
        "struct S f(void) { struct S s = {1}; return s; }\n",
        "struct S;\nstruct S *f(void) { return 0; }\n",
        // defined IN the return type's own specifier: complete before the name
        "struct S { int a; } f(void) { struct S s = {1}; return s; }\n",
    });
}

// ═══ C 6.7.6.3p4 — A DEFINITION'S PARAMETERS ARE COMPLETE ══════════════════════════════════════════════════

TEST(IncompleteParameter, ADefinitionWithAnIncompleteParameterIsRefused) {
    expectRefusedOnce({
        "struct S;\nvoid f(struct S s) { (void)0; }\n",
        "struct S;\nvoid f(struct S) { (void)0; }\n",
        "struct S;\nvoid f(struct S s) { (void)0; }\nstruct S { int x; };\n",
    }, DiagnosticCode::S_IncompleteTypeObject);
}

TEST(IncompleteParameter, APrototypeAPointerAndAnArrayParameterStayLegal) {
    expectAccepted({
        "struct S;\nvoid f(struct S s);\n",
        "struct S;\nvoid f(struct S s);\nstruct S { int x; };\nvoid f(struct S s) { (void)s; }\n",
        "struct S;\nvoid f(struct S *p) { (void)p; }\n",
        // (ii): an array of an incomplete element adjusts to a pointer — MSVC runs it; no change
        "struct S;\nvoid f(struct S a[]) { (void)a; }\n",
    });
}

// ═══ C 6.5.2.2p1 / p4 — A CALL'S RESULT AND ARGUMENTS ARE COMPLETE AT THE CALL ════════════════════════════

TEST(IncompleteAtACall, ACallThroughAnIncompleteReturnIsRefused) {
    expectRefusedOnce({
        "struct S;\nstruct S f(void);\nvoid g(void) { f(); }\n",
        // completed later: MSVC, which accepts it, builds a program that crashes
        "struct S;\nstruct S f(void);\nint g(void) { f(); return 1; }\nstruct S { char big[256]; };\n",
    }, DiagnosticCode::S_IncompleteReturnType);
}

TEST(IncompleteAtACall, AnIncompleteArgumentIsRefusedTheVariadicTailIncluded) {
    expectRefusedOnce({
        "struct S;\nextern struct S x;\nvoid f(struct S s);\nvoid g(void) { f(x); }\n",
        "struct S;\nextern struct S x;\nvoid f(struct S s);\nvoid g(void) { f(x); }\n"
        "struct S { int v[8]; };\n",
        "void v(int n, ...);\nstruct S;\nextern struct S x;\nvoid g(void) { v(1, x); }\n",
    }, DiagnosticCode::S_IncompleteArgumentType);
}

// One fact, one verdict: an argument refused for its incompleteness is not ALSO judged for assignability —
// passed where an `int` is expected, it would otherwise draw a second, S_TypeMismatch, verdict.
TEST(IncompleteAtACall, AnIncompleteArgumentDrawsNoSecondVerdict) {
    std::string const src = "struct S;\nextern struct S x;\nvoid f(int n);\nvoid g(void) { f(x); }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto const model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_IncompleteArgumentType), 1u) << errorsOf(model);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u) << errorsOf(model);
}

TEST(IncompleteAtACall, ACallAfterTheCompletionIsLegal) {
    expectAccepted({
        "struct S;\nstruct S f(void);\nstruct S { int a; };\nint g(void) { return f().a; }\n",
        "struct S { int v; };\nvoid f(struct S s);\nvoid g(struct S y) { f(y); }\n",
    });
}

// ═══ THE VOID PARAMETER ════════════════════════════════════════════════════════════════════════════════════

// gcc's meaning: a NAMED void is a parameter of an incomplete type, legal in a declaration that is not a
// definition — and the signature KEEPS it, so it is a type distinct from `void(void)`.
TEST(VoidParameter, ANamedVoidInADeclarationIsKeptAsAParameter) {
    std::string const src = "void f(void v);\nvoid g(void);\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto const model = analyzeC(src);
    EXPECT_FALSE(model.hasErrors()) << errorsOf(model);
    auto const& in = model.lattice().interner();
    TypeId const f = typeOfSymbol(model, "f");
    TypeId const g = typeOfSymbol(model, "g");
    ASSERT_TRUE(f.valid() && g.valid());
    ASSERT_EQ(in.fnParams(f).size(), 1u);
    EXPECT_EQ(in.kind(in.fnParams(f)[0]), TypeKind::Void);
    EXPECT_EQ(in.fnParams(g).size(), 0u);
    EXPECT_NE(f, g) << "`void(void v)` and `void(void)` are distinct types (gcc: conflicting types)";
    // …and the lattice's one answer to "which parameters take an argument" is the list BEFORE the void.
    EXPECT_EQ(in.fnArgumentParams(f).size(), 0u);
    EXPECT_FALSE(in.fnArgumentsVariadic(f));
}

// A call's arguments END at the first unqualified void: gcc's measured rule, argument for argument.
TEST(VoidParameter, ACallsArgumentsEndAtTheVoidParameter) {
    expectAccepted({
        "void f(void v);\nvoid g(void) { f(); }\n",
        "void f(void v, void w);\nvoid g(void) { f(); }\n",
        "void f(int a, void v);\nvoid g(void) { f(42); }\n",
        "void f(void v, int a);\nvoid g(void) { f(); }\n",
        "void f(void v, ...);\nvoid g(void) { f(); }\n",
        "void f(void v);\nvoid (*p)(void w) = f;\nvoid g(void) { p(); }\n",
    });
    expectRefusedOnce({
        "void f(void v);\nvoid g(void) { f(0); }\n",               // at the void: too many
        "void f(int a, void v);\nvoid g(void) { f(1, 2); }\n",     // past it: too many
        "void f(int a, void v);\nvoid g(void) { f(); }\n",         // before it: too few
        "void f(void v, ...);\nvoid g(void) { f(1); }\n",          // the `...` after it is unreachable
    }, DiagnosticCode::S_ArgCountMismatch);
}

// A QUALIFIED void is not the end — gcc calls `f()` through one "too few arguments".
TEST(VoidParameter, AQualifiedVoidParameterDoesNotEndTheList) {
    expectAccepted({"void f(volatile void v);\n"});
    expectRefusedOnce({"void f(volatile void v);\nvoid g(void) { f(); }\n"},
                      DiagnosticCode::S_ArgCountMismatch);
}

// Identity keeps the whole declared list, as gcc's does: the truncation is not the same type.
TEST(VoidParameter, ANamedVoidSignatureConflictsWithItsTruncation) {
    expectRefusedOnce({
        "void f(void v);\nvoid f(void) { }\n",
        "void f(int a, void v);\nvoid f(int a);\n",
        "void f(void v);\nvoid f(void v, void w);\n",
    }, DiagnosticCode::S_IncompatibleRedeclaration);
    expectAccepted({"void f(void v);\nvoid f(void w);\n"});   // a parameter's NAME is not its type
}

// In a DEFINITION every reference refuses a named void: the parameter is an object of an incomplete type.
TEST(VoidParameter, ANamedVoidInADefinitionIsRefused) {
    expectRefusedOnce({
        "void f(void v) { }\n",
        "void f(void v, int a) { (void)a; }\n",
    }, DiagnosticCode::S_InvalidVoidParam);
}

// MSVC's measured meaning for a qualified sole void in a declaration is `(void)`, and its programs run; in a
// DEFINITION all four references refuse it.
TEST(VoidParameter, AQualifiedSoleVoidIsVoidInADeclarationAndRefusedInADefinition) {
    for (std::string const src : {std::string{"void f(const void);\n"}, std::string{"void f(void const);\n"},
                                  std::string{"void f(volatile void);\n"},
                                  std::string{"typedef volatile void VV;\nvoid f(VV);\n"}}) {
        ASSERT_EQ(parseErrorsFor(src), 0u) << src;
        auto const model = analyzeC(src);
        EXPECT_FALSE(model.hasErrors()) << src << errorsOf(model);
        TypeId const f = typeOfSymbol(model, "f");
        ASSERT_TRUE(f.valid());
        EXPECT_EQ(model.lattice().interner().fnParams(f).size(), 0u) << src;
    }
    expectRefusedOnce({
        "void f(const void) { }\n",
        "void f(void const) { }\n",
        "void f(volatile void) { }\n",
        "typedef volatile void VV;\nvoid f(VV) { }\n",
    }, DiagnosticCode::S_InvalidVoidParam);
}

// `register` is a storage class, not a qualifier: clang and MSVC build and RUN `void f(register void) {}`.
TEST(VoidParameter, ARegisterSoleVoidIsVoidEvenInADefinition) {
    expectAccepted({"void f(register void) { }\nint main(void) { f(); return 0; }\n"});
}

// `_Atomic` is the one qualification NO reference accepts on the sole void, in any declaration.
TEST(VoidParameter, AnAtomicSoleVoidIsRefusedEverywhere) {
    expectRefusedOnce({
        "void f(_Atomic void);\n",
        "void f(_Atomic void) { }\n",
    }, DiagnosticCode::S_InvalidVoidParam);
}

// The `...` is an item of the list too: `(void, ...)` is not C 6.7.6.3p10's `(void)`. DSS read it as `(...)`.
TEST(VoidParameter, AnUnnamedVoidBesideAnEllipsisIsRefused) {
    expectRefusedOnce({"void f(void, ...);\n", "void f(void, int x);\n"},
                      DiagnosticCode::S_InvalidVoidParam);
}

// THE STATED RESIDUAL, tracked OPEN in the production registry (the const-qualified named-void row, which names
// this pin): gcc accepts the bare declaration and refuses every call through it and every definition of it; a
// DSS signature cannot carry the qualifier, so accepting it would let `f()` end its argument list where no
// reference does. The refusal must SAY that — and must not claim the declaration breaks a C constraint.
TEST(VoidParameter, AConstNamedVoidIsRefusedForTheReasonItStates) {
    std::string const src = "void f(const void v);\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto const model = analyzeC(src);
    ASSERT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_InvalidVoidParam), 1u) << errorsOf(model);
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_InvalidVoidParam) continue;
        EXPECT_NE(d.actual.find("would end the argument list"), std::string::npos) << d.actual;
        EXPECT_NE(d.actual.find("no reference compiler accepts"), std::string::npos) << d.actual;
    }
}
