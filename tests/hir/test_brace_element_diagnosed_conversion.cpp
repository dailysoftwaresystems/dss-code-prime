// ===========================================================================
// P68 round 9 (lane `cs`) — A BRACE-INIT ELEMENT'S DIAGNOSED CONVERSION
// (D-C-INCOMPATIBLE-POINTER-CONVERSION-REFUSED-WHERE-EVERY-REFERENCE-WARNS).
//
// A brace-init element is the one initialization position the semantic tier never
// judges — only the HIR lowering knows which slot a positional or designated element
// lands in — so the conversions the language admits WITH A DIAGNOSTIC are reported by
// the lowering for it (`lowerBraceElementValue`), in the SAME code and sentence the
// semantic tier reports at every other initialization, and `coerce` realizes them.
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
