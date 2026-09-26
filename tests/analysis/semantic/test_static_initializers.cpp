// ===========================================================================
// P68 round 13 (lane `cs`, the static-initializer item) — C 6.7.9p4 (C23 6.7.11p5):
// every expression in an initializer for an object of STATIC or THREAD storage duration
// shall be a constant expression or a string literal.
//
// The semantic tier decided this nowhere. ✔MEASURED 2026-09-24 (lane `cs`'s probes
// `.temp/probe/nci`, `sti` … `sti11`: gcc 13.3.0 and clang 18.1.3 in both modes,
// mingw-w64 13.2.0, every build RUN):
//   * refused by every reference, BUILT by DSS silently: `int y = 42; int x = y;`,
//     `int a[2] = { 1, y };`, a `const volatile` read, a read of a const whose
//     initializer follows it;
//   * refused by every reference, ABORTED DSS (exit 0xC0000409): a local's address and
//     an automatic array's decay in a block-scope `static`;
//   * refused by every reference, refused by DSS under an OBJECT-FORMAT code
//     (K_NoMatchingObjectFormat "has a runtime initializer"): `static int x = g();`.
// Each is now S_StaticInitializerNotConstant at the offending construct.
//
// ★ WHAT THE CHECK MAY CLAIM. It refuses only what is PROVABLY not a constant, in a
// position that is EVALUATED — never "what DSS cannot fold": a reference builds dozens
// of forms the static-data producer could not fold before this item, and a check that
// called them "not constant" would state something false. So every admitted form below
// is one a reference builds, and every refused one is refused by every reference.
//
// The fixed-depth walks the item found beside it are pinned in
// `test_uncapped_semantic_walks.cpp`.
//
// Perturbs the SHIPPED c JSON (the form list, the block), so it links nlohmann_json.
// ===========================================================================

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/constant_form.hpp"
#include "core/types/data_model.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "repo_root.hpp"

#include "semantic_test_fixture.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::sem_test;

namespace {

constexpr DiagnosticCode kNotConstant = DiagnosticCode::S_StaticInitializerNotConstant;
constexpr DiagnosticCode kComma       = DiagnosticCode::S_StaticInitializerUsesTheCommaOperator;

[[nodiscard]] SemanticModel analyzeC(std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
}

[[nodiscard]] std::vector<ParseDiagnostic> ofCode(SemanticModel const& m, DiagnosticCode code) {
    std::vector<ParseDiagnostic> out;
    for (auto const& d : m.diagnostics().all())
        if (d.code == code) out.push_back(d);
    return out;
}

[[nodiscard]] std::string describe(SemanticModel const& m) {
    std::string out;
    for (auto const& d : m.diagnostics().all()) {
        out += "\n  ";
        out += diagnosticCodeName(d.code);
        out += " @";
        out += std::to_string(d.span.start());
        out += ": ";
        out += d.actual;
    }
    return out;
}

// The byte offset of the `nth` occurrence of `needle` in `src` (the construct a
// diagnostic must be positioned at).
[[nodiscard]] std::size_t offsetOf(std::string const& src, std::string const& needle,
                                   int nth = 1) {
    std::size_t at = std::string::npos;
    std::size_t from = 0;
    for (int i = 0; i < nth; ++i) {
        at = src.find(needle, from);
        if (at == std::string::npos) throw std::runtime_error("needle not in the source: " + needle);
        from = at + 1;
    }
    return at;
}

[[nodiscard]] nlohmann::json shippedC() {
    std::ifstream in{dss::test::configRoot() / "sources" / "c.lang.json", std::ios::binary};
    if (!in.good()) throw std::runtime_error("the shipped c.lang.json is unreadable");
    return nlohmann::json::parse(in);
}

[[nodiscard]] SemanticModel analyzeWith(nlohmann::json const& doc, std::string const& src) {
    auto schema = GrammarSchema::loadFromText(doc.dump(), "<static-initializers>");
    if (!schema.has_value()) throw std::runtime_error("the perturbed c schema failed to load");
    UnitBuilder builder{*schema, DiagnosticBudget::libraryDefault()};
    builder.addInMemory(src, "main.c");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
}

// The shipped c with ONE form taken out of `semantics.staticInitializers`.
[[nodiscard]] nlohmann::json withoutForm(std::string_view form) {
    auto doc = shippedC();
    auto& forms = doc.at("semantics").at("staticInitializers").at("otherConstantForms");
    nlohmann::json kept = nlohmann::json::array();
    for (auto const& f : forms)
        if (f.get<std::string>() != form) kept.push_back(f);
    if (kept.size() + 1 != forms.size()) throw std::runtime_error("the shipped c does not name that form");
    forms = kept;
    return doc;
}

struct Refused {
    char const* why;
    std::string src;
    std::string at;   // the construct the diagnostic must point at
    int nth = 1;
};

} // namespace

// Each is refused by every reference (gcc 13.3.0, clang 18.1.3 in both modes, mingw-w64
// 13.2.0), and each is now S_StaticInitializerNotConstant at the construct that proves it.
// RED-ON-DISABLE: drop `validateStaticInitializer`'s call in pass2Post and every case
// draws nothing (the first six then BUILD silently, as they did); drop any one proof arm
// and its case draws nothing.
TEST(StaticInitializers, ProvablyNonConstantConstructsAreRefusedWhereTheyStand) {
    std::vector<Refused> const cases = {
        {"a non-const object's value", "int y = 42;\nint x = y;\n", "y;"},
        {"the same, as an aggregate element", "int y = 42;\nint a[2] = { 1, y };\n", "y }"},
        {"a volatile const object's value",
         "static const volatile int v = 42;\nint x = v;\n", "v;"},
        {"a const read before its initializer is visible",
         "extern const int e;\nint x = e;\nconst int e = 42;\n", "e;", 2},
        {"a call to a function this unit defines",
         "static int g(void) { return 42; }\nint a = g();\n", "g()"},
        {"a local's address in a block-scope static",
         "int main(void) { int l = 42; static int *p = &l; return *p; }\n", "l;"},
        {"an automatic array's decay in a block-scope static",
         "int main(void) { int a[2] = { 0, 0 }; static int *p = a; return *p; }\n", "a;"},
        {"a parameter's address",
         "int f(int q) { static int *p = &q; return *p; }\n", "q;"},
        {"a block-scope compound literal's address",
         "int main(void) { static int *p = &(int){ 42 }; return *p; }\n", "(int){ 42 }"},
        {"an assignment", "int y;\nint x = (y = 1);\n", "y = 1"},
        {"an increment", "int y;\nint x = y++;\n", "y++"},
        {"an address in an integer narrower than a pointer (LP64 `int`)",
         "int a[2];\nint x = (int)&a;\n", "(int)&a"},
    };
    for (auto const& c : cases) {
        auto const m = analyzeC(c.src);
        auto const ds = ofCode(m, kNotConstant);
        ASSERT_EQ(ds.size(), 1u) << c.why << describe(m);
        EXPECT_EQ(ds[0].severity, DiagnosticSeverity::Error) << c.why;
        EXPECT_EQ(ds[0].span.start(), offsetOf(c.src, c.at, c.nth)) << c.why << describe(m);
    }
}

// What is NOT evaluated proves nothing — each builds on every reference (`.temp/probe/sti8`).
// RED-ON-DISABLE: walk both `?:` arms / both `&&` `||` operands, or walk into `sizeof`,
// and each case draws a false S_StaticInitializerNotConstant.
TEST(StaticInitializers, OnlyAnEvaluatedConstructIsProof) {
    for (char const* src : {
             "int y = 1;\nint x = 1 ? 42 : y;\n",
             "int y = 1;\nint x = 0 && y;\n",
             "int y = 0;\nint x = 1 || y;\n",
             "static int g(void) { return 1; }\nint x = 1 ? 42 : g();\n",
             "int main(void) { static int s; int l = 0;\n"
             "  static int *p = 1 ? &s : &l; return p == &s ? l : 1; }\n",
             "int y = 1;\nunsigned long x = sizeof(y + 1);\n",
             "int main(void) { int l = 0; static unsigned long x = sizeof l; return (int)x + l; }\n",
             "int y = 1;\nint x = _Generic(1, int: 42, default: y);\n",
         }) {
        auto const m = analyzeC(src);
        EXPECT_EQ(countCode(m.diagnostics(), kNotConstant), 0u) << src << describe(m);
        EXPECT_FALSE(m.hasErrors()) << src << describe(m);
    }
}

// A compound literal OUTSIDE a function body has static storage duration (C 6.5.2.5p5), so the
// address of one in a FILE-scope initializer is an address constant — gcc 13.3.0, clang 18.1.3,
// mingw-w64 13.2.0 and MSVC 19.51 all build `int *p = &(int){ 42 };` and run 42
// (probe-reference-cc). The storage duration is the DECLARATION's, never the walk's current
// scope: a file-scope definition opens its own child scope, and the check once read that scope
// as a block and refused this line as automatic (P68 round 13). The block-scope control stays
// refused. RED-ON-DISABLE: judge the compound literal by the walk's scope again (the file-scope
// line draws S_StaticInitializerNotConstant).
TEST(StaticInitializers, AFileScopeCompoundLiteralsAddressIsNoProof) {
    for (char const* src : {
             "int *p = &(int){ 42 };\n",
             "int *q = (int[]){ 1, 42 } + 1;\n",
             "struct S { int k; };\nstruct S *r = &(struct S){ 42 };\n",
         }) {
        auto const m = analyzeC(src);
        EXPECT_EQ(countCode(m.diagnostics(), kNotConstant), 0u) << src << describe(m);
    }
    auto const m = analyzeC("int main(void) { static int *p = &(int){ 42 }; return *p; }\n");
    EXPECT_EQ(countCode(m.diagnostics(), kNotConstant), 1u) << "the block-scope control" << describe(m);
}

// An ELEMENT read of an array compound literal reads an object's value: only a CONST one's is a
// constant (the `constObjectRead` form). MEASURED through probe-reference-cc (P68 round 13):
// gcc 13.3.0, clang 18.1.3 (both modes) and MSVC 19.51 refuse the three refused shapes (DSS
// folded them silently); clang builds the const literal's element and the structure literal's
// member in both modes, gcc -std=c2x and clang the structure value, and every GNU reference the
// scalar value. RED-ON-DISABLE: drop the element-read arm of the compound-literal
// branch (the refused shapes draw nothing); require a non-const element for the const form (the
// const literal's element draws S_StaticInitializerNotConstant).
TEST(StaticInitializers, ACompoundLiteralsElementReadIsProofOnlyWhenItIsNotConst) {
    std::vector<Refused> const refused = {
        {"an element of an array compound literal",
         "int x = ((int[]){ 1, 42 })[1];\n", "(int[]){ 1, 42 }"},
        {"the same without the parentheses", "int x = (int[]){ 1, 42 }[1];\n", "(int[]){ 1, 42 }"},
        {"the same in a static local",
         "int main(void) { static int x = ((int[]){ 1, 42 })[1]; return x; }\n", "(int[]){ 1, 42 }"},
    };
    for (auto const& c : refused) {
        auto const m = analyzeC(c.src);
        auto const ds = ofCode(m, kNotConstant);
        ASSERT_EQ(ds.size(), 1u) << c.why << describe(m);
        EXPECT_EQ(ds[0].span.start(), offsetOf(c.src, c.at, c.nth)) << c.why << describe(m);
    }
    for (char const* src : {
             "int x = ((const int[]){ 1, 42 })[1];\n",
             "struct S { int a, b; };\nint y = ((struct S){ 1, 42 }).b;\n",
             "struct S { int a, b; };\nstruct S s = (struct S){ 1, 42 };\n",
             "int x = (int){ 42 };\n",
         }) {
        auto const m = analyzeC(src);
        EXPECT_EQ(countCode(m.diagnostics(), kNotConstant), 0u) << src << describe(m);
    }
}

// P68 round 13, fold F7 — a const object's value is no constant when the OBJECT is volatile, and
// an object is volatile when its type, looked through its ARRAY spine, is volatile-qualified
// (C 6.7.3p10 puts an array's qualifier on its ELEMENT type, where the check once missed it and
// F5 built `cva[1]` silently, running 42). MEASURED through `dssharness run probe-reference-cc`
// (lane `cs`'s `.temp/probe/f7`: gcc 13.3.0, clang 18.1.3, mingw-w64 13.2.0, MSVC 19.51, both
// modes each): all four refuse every refused line; gcc and mingw-w64 build every admitted line
// and run 42 (clang and MSVC refuse the volatile-member reads, clang builds the sibling and the
// whole copy) — a volatile MEMBER does not make the object that holds it volatile — and all
// four build the two addresses, which read nothing.
// RED-ON-DISABLE: ask the object's type at its top level only (`isVolatileQualified`) — the
// array, 2-D, array-of-structures and typedef'd-element lines draw nothing; refuse on a volatile
// member on the way down too — the member lines draw S_StaticInitializerNotConstant.
TEST(StaticInitializers, AnObjectIsVolatileThroughItsArraySpineNotThroughAMember) {
    std::vector<Refused> const refused = {
        {"a const volatile array's element",
         "static const volatile int cva[2] = { 1, 42 };\nint x = cva[1];\n", "cva[1]"},
        {"a 2-D one's", "static const volatile int m[2][2] = { { 1, 2 }, { 3, 42 } };\n"
                        "int x = m[1][1];\n", "m[1][1]"},
        {"a const volatile structure's member",
         "static const volatile struct { int v; } s = { 42 };\nint x = s.v;\n", "s.v"},
        {"a member of an element of an array of const volatile structures",
         "static const volatile struct { int v; } a[2] = { { 1 }, { 42 } };\nint x = a[1].v;\n",
         "a[1]"},
        {"a typedef'd volatile element's",
         "typedef volatile int VI;\nstatic const VI a[2] = { 1, 42 };\nint x = a[1];\n", "a[1]"},
        {"a typedef'd volatile structure's member",
         "typedef volatile struct { int v; } VS;\nstatic const VS s = { 42 };\nint x = s.v;\n", "s.v"},
        {"a const volatile structure copied whole",
         "struct S { int a, b; };\nstatic const volatile struct S cs = { 1, 42 };\n"
         "struct S s = cs;\n", "cs;"},
    };
    for (auto const& c : refused) {
        auto const m = analyzeC(c.src);
        auto const ds = ofCode(m, kNotConstant);
        ASSERT_EQ(ds.size(), 1u) << c.why << describe(m);
        EXPECT_EQ(ds[0].severity, DiagnosticSeverity::Error) << c.why;
        EXPECT_EQ(ds[0].span.start(), offsetOf(c.src, c.at, c.nth)) << c.why << describe(m);
        EXPECT_NE(ds[0].actual.find("a volatile object"), std::string::npos) << c.why << describe(m);
    }
    for (char const* src : {
             "static const struct { volatile int v; } cs = { 42 };\nint x = cs.v;\n",
             "static const struct { volatile int v[2]; } s = { { 1, 42 } };\nint x = s.v[1];\n",
             "struct S { volatile int v; };\nstatic const struct S a[2] = { { 1 }, { 42 } };\n"
             "int x = a[1].v;\n",
             "static const struct { struct { volatile int v; } in; } s = { { 42 } };\n"
             "int x = s.in.v;\n",
             "static const struct { volatile int v; int w; } s = { 1, 42 };\nint x = s.w;\n",
             "struct P { int a; volatile int b; };\nstatic const struct P cp = { 1, 42 };\n"
             "struct P whole = cp;\n",
             "static const volatile int cva[2] = { 1, 42 };\nconst volatile int *p = &cva[1];\n",
             "static const volatile int cva[2] = { 1, 42 };\nconst volatile int *p = cva;\n",
         }) {
        auto const m = analyzeC(src);
        EXPECT_EQ(countCode(m.diagnostics(), kNotConstant), 0u) << src << describe(m);
        EXPECT_FALSE(m.hasErrors()) << src << describe(m);
    }
}

// P68 round 13, fold F7 — a READ through a COMPOUND LITERAL. clang, the one reference that
// reads a compound literal's element or member, refuses every read whose lvalue is
// volatile-qualified — the literal a volatile object, or a volatile member or element on the
// read's path — so all four refuse each refused line (`.temp/probe/f7`, both modes each), where
// the evaluator folds a literal's element or member. A literal's own VALUE is no such read (gcc
// -std=c2x, mingw-w64 -std=c2x and clang build the first three admitted lines), nor is its
// address (all four build the fourth); clang builds the two non-volatile reads.
// RED-ON-DISABLE: drop the volatile arm of the compound-literal branch (every refused line draws
// nothing); ask the literal's own type only (the three volatile-member lines draw nothing).
TEST(StaticInitializers, AVolatileReadThroughACompoundLiteralIsProof) {
    std::vector<Refused> const refused = {
        {"a const volatile array literal's element",
         "int x = ((const volatile int[]){ 1, 42 })[1];\n", "(const volatile int[]){ 1, 42 }"},
        {"the same in a static local",
         "int main(void) { static int x = ((const volatile int[]){ 1, 42 })[1]; return x; }\n",
         "(const volatile int[]){ 1, 42 }"},
        {"a 2-D one's",
         "int x = ((const volatile int[][2]){ { 1, 2 }, { 3, 42 } })[1][1];\n",
         "(const volatile int[][2]){ { 1, 2 }, { 3, 42 } }"},
        {"a typedef'd volatile element's",
         "typedef volatile int VI;\nint x = ((const VI[]){ 1, 42 })[1];\n", "(const VI[]){ 1, 42 }"},
        {"a const volatile structure literal's member",
         "int x = ((const volatile struct { int v; }){ 42 }).v;\n",
         "(const volatile struct { int v; }){ 42 }"},
        {"a volatile member of a const structure literal",
         "int x = ((const struct { volatile int v; }){ 42 }).v;\n",
         "(const struct { volatile int v; }){ 42 }"},
        {"a volatile member of a non-const one",
         "int x = ((struct { volatile int v; }){ 42 }).v;\n", "(struct { volatile int v; }){ 42 }"},
        {"an element of a volatile array member",
         "int x = ((const struct { volatile int v[2]; }){ { 1, 42 } }).v[1];\n",
         "(const struct { volatile int v[2]; }){ { 1, 42 } }"},
    };
    for (auto const& c : refused) {
        auto const m = analyzeC(c.src);
        auto const ds = ofCode(m, kNotConstant);
        ASSERT_EQ(ds.size(), 1u) << c.why << describe(m);
        EXPECT_EQ(ds[0].span.start(), offsetOf(c.src, c.at, c.nth)) << c.why << describe(m);
        EXPECT_NE(ds[0].actual.find("volatile-qualified"), std::string::npos) << c.why << describe(m);
    }
    for (char const* src : {
             "int x = (const volatile int){ 42 };\n",
             "int main(void) { static int x = (const volatile int){ 42 }; return x; }\n",
             "struct S { int a, b; };\nstruct S s = (const volatile struct S){ 1, 42 };\n",
             "const volatile int *p = (const volatile int[]){ 1, 42 };\n",
             "int x = ((const int[]){ 1, 42 })[1];\n",
             "int x = ((const struct { int v; }){ 42 }).v;\n",
         }) {
        auto const m = analyzeC(src);
        EXPECT_EQ(countCode(m.diagnostics(), kNotConstant), 0u) << src << describe(m);
    }
}

// P68 round 13, fold F7 — an address converted to an integer narrower than a pointer is proof
// only when its BASE is a symbol's: a NULL-based address is a compile-time integer, no relocation
// is involved, and all four references build every admitted line (`.temp/probe/f7`, both modes
// each; clang -std=c17 -pedantic-errors alone refuses the pointer arithmetic), where the check
// once refused each as "an address no relocation can hold". Every refused line is refused by all
// four; each proof stands at its narrowing cast.
// RED-ON-DISABLE: prove at the cast whatever the base (every admitted line draws
// S_StaticInitializerNotConstant); drop the base's proof at an addressed object (the refused
// lines draw nothing).
TEST(StaticInitializers, ANarrowedAddressIsProofOnlyForASymbolsAddress) {
    std::vector<Refused> const refused = {
        {"an element's address", "static int a[2];\nint y = (int)&a[1];\n", "(int)&a[1]"},
        {"an array's decay plus one", "static int a[2];\nint y = (int)(a + 1);\n", "(int)(a + 1)"},
        {"a member's address",
         "struct P { int k, m; };\nstatic struct P s;\nint y = (int)&s.m;\n", "(int)&s.m"},
        {"through a pointer-to-pointer conversion",
         "static int a[2];\nint y = (int)(char *)&a[1];\n", "(int)(char *)&a[1]"},
        {"a function's address", "int f(void);\nint y = (int)f;\n", "(int)f"},
        {"a string literal's", "int y = (int)\"abc\";\n", "(int)\"abc\""},
        {"a file-scope compound literal's", "int y = (int)&(int){ 42 };\n", "(int)&(int){ 42 }"},
        {"a row of a 2-D array, dereferenced to decay",
         "static int m[2][2];\nint y = (int)*m;\n", "(int)*m"},
    };
    for (auto const& c : refused) {
        auto const m = analyzeC(c.src);
        auto const ds = ofCode(m, kNotConstant);
        ASSERT_EQ(ds.size(), 1u) << c.why << describe(m);
        EXPECT_EQ(ds[0].span.start(), offsetOf(c.src, c.at, c.nth)) << c.why << describe(m);
        EXPECT_NE(ds[0].actual.find("narrower than a pointer"), std::string::npos)
            << c.why << describe(m);
    }
    for (char const* src : {
             "struct T { int a; int m; };\nint off = (int)&((struct T *)0)->m;\n",
             "struct T { int a; int m; };\nchar c = (char)&((struct T *)0)->m;\n",
             "int z = (int)(void *)0;\n",
             "short s = (short)(char *)5;\n",
             "int off = (int)&((int *)0)[3];\n",
             "int off = (int)((char *)0 + 12);\n",
         }) {
        auto const m = analyzeC(src);
        EXPECT_EQ(countCode(m.diagnostics(), kNotConstant), 0u) << src << describe(m);
    }
}

// Every ISO form (6.6p7-p9) and every form the language names (6.6p10) draws nothing: an
// address constant through each path, the address algebra, a const object's value. Each
// line builds on a reference (the example `static_initializer_address_constants` and
// `static_initializer_other_constant_forms` run them).
TEST(StaticInitializers, EveryConstantFormIsAdmitted) {
    std::string const src =
        "struct S { int k; int m; };\n"
        "static int g(void) { return 42; }\n"
        "int a[4] = { 1, 42, 3, 4 }, b[2];\n"
        "struct S s = { 1, 42 };\n"
        "int *p1 = a + 1;\n"
        "int *p2 = 1 + a;\n"
        "int *p3 = &*a;\n"
        "char *p4 = (char *)&a + sizeof(int);\n"
        "int *p5 = 1 ? &a[0] : &a[1];\n"
        "int *p6 = &(&s)->m;\n"
        "int (*fp)(void) = *g;\n"
        "char const *str = \"x*\" + 1;\n"
        "int n1 = (int *)0 == 0;\n"
        "unsigned long long u1 = (unsigned long long)&a + 5;\n"
        "long l1 = (long)&a;\n"
        "int *p7 = (int *)((unsigned long long)&a + sizeof(int));\n"
        "_Bool t1 = &a;\n"
        "int t2 = !&a[0];\n"
        "int c1 = &a[1] < &a[2];\n"
        "long d1 = &a[3] - &a[1];\n"
        "int e1 = &a[0] == &b[0];\n"
        "unsigned long long z1 = (unsigned long long)&a * 0;\n"
        "static const int base = 40;\n"
        "static int sum = base + 2;\n"
        "static const int ca[2] = { 1, 42 };\n"
        "int r1 = ca[1];\n"
        "static const struct S cs = { 1, 42 };\n"
        "int r2 = cs.m;\n"
        "int *const cp = &a[1];\n"
        "int *r3 = cp;\n"
        "int r4 = \"*abc\"[0];\n"
        "int main(void) {\n"
        "  static int ss[3];\n"
        "  static int *q1 = &ss[2] - 1;\n"
        "  static const char *fn = __func__;\n"
        "  static void *lp = &&L;\n"
        "L:\n"
        "  return *q1 + fn[0] + (lp != 0);\n"
        "}\n";
    auto const m = analyzeC(src);
    EXPECT_EQ(countCode(m.diagnostics(), kNotConstant), 0u) << describe(m);
    EXPECT_EQ(countCode(m.diagnostics(), kComma), 0u) << describe(m);
}

// The comma operator (6.6p3's constraint names it): clang and MSVC build `(1, 42)`, so the
// language names `commaOperator` and it is ADMITTED WITH the diagnostic the constraint
// asks for. Without the form, it is refused. RED-ON-DISABLE: drop the warning's emission
// (first arm draws no S_StaticInitializerUsesTheCommaOperator), or admit the comma
// unconditionally (the second draws no S_StaticInitializerNotConstant).
TEST(StaticInitializers, TheCommaOperatorIsAdmittedWithAWarningOnlyWhenTheLanguageNamesIt) {
    std::string const src = "int x = (1, 42);\n";
    auto const m = analyzeC(src);
    EXPECT_EQ(countCode(m.diagnostics(), kNotConstant), 0u) << describe(m);
    auto const w = ofCode(m, kComma);
    ASSERT_EQ(w.size(), 1u) << describe(m);
    EXPECT_EQ(w[0].severity, DiagnosticSeverity::Warning);
    EXPECT_EQ(w[0].span.start(), offsetOf(src, "1, 42"));

    auto const refused = analyzeWith(withoutForm("commaOperator"), src);
    EXPECT_EQ(countCode(refused.diagnostics(), kNotConstant), 1u) << describe(refused);
    EXPECT_EQ(countCode(refused.diagnostics(), kComma), 0u) << describe(refused);
}

// `constObjectRead` is the switch for reading ANY object: without it a const object's
// value is proof too. RED-ON-DISABLE: ignore the form (the second model draws nothing).
TEST(StaticInitializers, WithoutConstObjectReadEveryObjectReadIsProof) {
    std::string const src = "static const int y = 40;\nint x = y + 2;\n";
    auto const admitted = analyzeC(src);
    EXPECT_EQ(countCode(admitted.diagnostics(), kNotConstant), 0u) << describe(admitted);
    auto const refused = analyzeWith(withoutForm("constObjectRead"), src);
    auto const ds = ofCode(refused, kNotConstant);
    ASSERT_EQ(ds.size(), 1u) << describe(refused);
    EXPECT_EQ(ds[0].span.start(), offsetOf(src, "y + 2"));
}

// The block is the constraint: a language that declares none is unchecked.
// RED-ON-DISABLE: run the check without the block (the second model draws one).
TEST(StaticInitializers, ALanguageWithoutTheBlockIsUnchecked) {
    std::string const src = "int y = 42;\nint x = y;\n";
    EXPECT_EQ(countCode(analyzeC(src).diagnostics(), kNotConstant), 1u);
    auto doc = shippedC();
    doc.at("semantics").erase("staticInitializers");
    auto const m = analyzeWith(doc, src);
    EXPECT_EQ(countCode(m.diagnostics(), kNotConstant), 0u) << describe(m);
}

// A call is proof only when the callee is DEFINED here: a library function a reference
// knows as a builtin folds there (`strlen("abc")`: gcc -std=c2x, clang in both modes,
// mingw-w64 -std=c2x — `.temp/probe/sti10`), so a prototype alone proves nothing.
// RED-ON-DISABLE: treat every call as proof (the first model draws one).
TEST(StaticInitializers, ACallIsProofOnlyForAFunctionThisUnitDefines) {
    auto const lib = analyzeC("unsigned long strlen(const char *);\n"
                              "unsigned long n = strlen(\"abc\");\n");
    EXPECT_EQ(countCode(lib.diagnostics(), kNotConstant), 0u) << describe(lib);
    auto const own = analyzeC("static int g(void) { return 42; }\nint a = g();\n");
    EXPECT_EQ(countCode(own.diagnostics(), kNotConstant), 1u) << describe(own);
}

// A thread-local object's address is not an address constant (6.6p9), and that refusal
// has ONE owner — S_ThreadLocalAddressNotConstant, at the static-data producer — so the
// semantic check says nothing about it.
TEST(StaticInitializers, AThreadLocalAddressIsLeftToItsOwnCode) {
    auto const m = analyzeC("_Thread_local int t;\nint *p = &t;\n");
    EXPECT_EQ(countCode(m.diagnostics(), kNotConstant), 0u) << describe(m);
}
