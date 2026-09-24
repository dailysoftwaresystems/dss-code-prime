// ===========================================================================
// P68 round 8 (lane `ht`) — A QUALIFIER REACHED THROUGH A TYPEDEF.
//
// THE PROPERTY THIS FILE OWNS: a declaration spelled with a typedef claims the
// `const` / `restrict` that typedef carries, exactly as the same declaration with
// the typedef written out would. Neither qualifier is interned (type_interner.hpp),
// so the TypeId a typedef name resolves to has no trace of them; the claim rides
// the typedef's OWN row (`typedefDecl`, which derives `constMarker` /
// `restrictMarker` from `c.lang.json`'s semantics block since P68 round 9), and
// the semantic tier applies it wherever a head NAMES the typedef
// (semantic_analyzer.cpp, "A QUALIFIER REACHED THROUGH A TYPEDEF").
//
// ✔REFERENCE VOTES, 2026-09-23, each case one translation unit probed SEPARATELY
// (`.temp/constdef/` in the lane's tree): gcc 13.3.0 and clang 18.1.3 at
// `-std=c17 -pedantic-errors` and `-std=c2x`, mingw-w64 gcc 13.2.0 at c17, MSVC
// 19.51 at `/std:c17` and `/std:clatest`. Every REFUSAL pinned below is refused by
// all four unless its comment says otherwise; every ACCEPTANCE is accepted by all
// four unless its comment says otherwise. Before the fix DSS compiled every
// refusal here with rc 0 (the controls it already got right).
//
// ★ EVERY REFUSAL SITS BESIDE ITS ACCEPTING TWIN, because the refusing half alone
// passes under a check that refuses everything — and two of the twins
// (`TheTagNamespaceAndAStructBodyDoNotBorrowATypedefsConst`) are what fail if the
// head's typedef name is looked for anywhere but the head's own base position.
//
// ── RED-ON-DISABLE (the lane's transcript carries each build and its names) ──
//   * the row's `constMarker` removed from `typedefDecl` → every refusal here
//     (since P68 round 9 the row DERIVES it from `semantics.constMarker`; the
//     equivalent mutant skips that derivation for the typedef row);
//   * the resolver's alias record not written → every refusal here;
//   * the Pass-1.5 application skipped → the object / deeper-level / fold pins;
//   * the redeclaration harvest without the typedef → the redeclaration pin;
//   * the void-parameter read without the typedef → the void-parameter pin;
//   * the head const put on the typedef's level 0 instead of its element's → the
//     array-typedef pin;
//   * no Fn level for a declaration deriving from a function typedef → the
//     `F *` arm of the redeclaration pin;
//   * the redefinition compare removed → the redefinition pin;
//   * the head's type name searched in the whole head subtree → the tag / struct
//     body twins.
//
// ── AND ONE PROPERTY BESIDE IT: `++` / `--` WRITE, AND ASK THE SAME QUESTION ─
// The operand of an increment or decrement must be a modifiable lvalue (C
// 6.5.2.4p1 / 6.5.3.1p1) exactly as an assignment's left operand must (6.5.16p2),
// and since this round both ask the ONE const-write check
// (`reportWriteToConstLvalue`) — the close of [[D-CSUBSET-INCDEC-CONST-LVALUE]].
// It lives here because the typedef spelling is one of its arms. RED-ON-DISABLE:
// the call at the ++/-- operand removed → every spelling in
// `IncrementOrDecrement.OfAConstLvalueIsRefusedAsItsAssignmentIs` red, its
// mutable controls green.
// ===========================================================================

#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_lattice.hpp"

#include "semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

using namespace dss;
using namespace dss::sem_test;

namespace {

[[nodiscard]] SymbolRecord const*
findSymbolNamed(SemanticModel const& model, std::string_view name) {
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == name) return &model.symbols()[i];
    }
    return nullptr;
}

// One translation unit and whether `code` must be reported for it.
struct Case {
    char const* what;
    char const* src;
    bool        refused;
};

void expectVerdicts(std::initializer_list<Case> cases, DiagnosticCode code) {
    for (Case const& c : cases) {
        auto model = analyzeShipped("c", {std::string{c.src}});
        EXPECT_EQ(hasCode(model.diagnostics(), code), c.refused)
            << c.what << " — expected " << (c.refused ? "REFUSED" : "accepted")
            << " by " << diagnosticCodeName(code) << "\n" << c.src;
    }
}

constexpr char const* kCI = "typedef const int CI;\n";

}  // namespace

// ── the OBJECT the typedef declares is const, at every scope and write form ──
TEST(ConstThroughTypedef, AConstTypedefObjectRefusesEveryWriteItsDirectSpellingRefuses) {
    std::string const ci = kCI;
    auto const g = ci + "CI x = 1;\nvoid g(void) { x = 2; }\n";
    auto const l = ci + "void g(void) { CI y = 1; y = 2; }\n";
    auto const p = ci + "void g(CI p) { p = 2; }\n";
    auto const chain = ci + "typedef CI CI2;\nvoid g(void) { CI2 z = 1; z = 2; }\n";
    auto const compound = ci + "void g(void) { CI y = 1; y += 2; }\n";
    expectVerdicts({
        {"a file-scope object", g.c_str(), true},
        {"a local", l.c_str(), true},
        {"a parameter", p.c_str(), true},
        {"a typedef of the const typedef", chain.c_str(), true},
        {"a compound assignment", compound.c_str(), true},
        {"`typedef int *const CIP; CIP q; q = 0;` — the typedef's POINTER is const",
         "typedef int *const CIP;\nvoid g(void) { int v = 0; CIP q = &v; q = 0; }\n", true},
        {"the direct twin `const int x`",
         "const int x = 1;\nvoid g(void) { x = 2; }\n", true},
        // THE CONTROLS: nothing here is const, and must not become so.
        {"a plain typedef", "typedef int PI;\nvoid g(void) { PI y = 1; y = 2; (void)y; }\n",
         false},
        {"a typedef of a mutable pointer",
         "typedef int *IP;\nvoid g(void) { int v = 0; IP q = &v; q = 0; (void)q; }\n", false},
        {"`typedef const char *CCP;` declares a MUTABLE pointer to const",
         "typedef const char *CCP;\nvoid g(CCP s) { s = 0; (void)s; }\n", false},
    }, DiagnosticCode::S_ConstViolation);
}

// ── a DEEPER level the typedef qualifies: a pointee, a member, an element ────
TEST(ConstThroughTypedef, ADeeperLevelReachedThroughATypedefIsConst) {
    std::string const ci = kCI;
    auto const pointee = ci + "void g(CI *p) { *p = 3; }\n";
    auto const member = ci + "struct S { CI m; };\nvoid g(struct S *s) { s->m = 1; }\n";
    auto const element = ci + "CI arr[2] = {1, 2};\nvoid g(void) { arr[0] = 3; }\n";
    auto const arrayOfCI = ci + "typedef CI A3[3];\nvoid g(void) { A3 a = {1, 2, 3}; a[0] = 4; }\n";
    auto const rebind = ci + "void g(CI *p, CI *q) { p = q; (void)p; }\n";
    expectVerdicts({
        {"the pointee of `CI *`", pointee.c_str(), true},
        {"a member declared `CI m`", member.c_str(), true},
        {"an element of `CI arr[2]`", element.c_str(), true},
        {"an element of an array typedef of CI", arrayOfCI.c_str(), true},
        {"the pointee of `typedef const char *CCP`",
         "typedef const char *CCP;\nvoid g(CCP s) { *s = 'a'; }\n", true},
        {"a member of an object of `typedef const struct P CP`",
         "struct P { int a, b; };\ntypedef const struct P CP;\n"
         "void g(void) { CP pt = {1, 2}; pt.a = 3; }\n", true},
        {"a member through a `CP *`",
         "struct P { int a, b; };\ntypedef const struct P CP;\n"
         "void g(CP *pp) { pp->a = 3; }\n", true},
        // THE CONTROLS: the pointer itself, and a member the struct leaves mutable.
        {"rebinding a `CI *` (the pointer is mutable)", rebind.c_str(), false},
        {"a mutable member of a struct typedef that has a const one",
         "typedef struct { const int m; int n; } S;\nvoid g(S *s) { s->n = 1; }\n", false},
    }, DiagnosticCode::S_ConstViolation);
}

// ── a const written AT the use, on an array typedef, qualifies the ELEMENTS ──
// C11 6.7.3p9: "If the specification of an array type includes any type
// qualifiers, the element type is so-qualified" (C23 6.7.4.1p10: both are). The
// typedef hides the array level, so the const has to land on the level beneath it.
TEST(ConstThroughTypedef, AConstAppliedToAnArrayTypedefQualifiesItsElements) {
    expectVerdicts({
        {"`const A3 arr` for `typedef int A3[3]`",
         "typedef int A3[3];\nvoid g(void) { const A3 arr = {1, 2, 3}; arr[0] = 4; }\n",
         true},
        {"`const PA arr` for `typedef int *PA[2]` — the elements are `int *const`",
         "typedef int *PA[2];\nvoid g(void) { const PA arr = {0, 0}; arr[0] = 0; }\n",
         true},
        // THE CONTROL: the elements are const POINTERS; what they point to is not.
        {"a write through an element of `const PA`",
         "typedef int *PA[2];\nint v;\nvoid g(void) { const PA arr = {&v, &v}; *arr[0] = 1; }\n",
         false},
    }, DiagnosticCode::S_ConstViolation);
}

// ── the head's typedef name is the head's BASE type, nothing inside it ──────
// Two twins every reference accepts, each of which a looser search for "the"
// type name would refuse: a TAG named like a const typedef (the tag namespace is
// not the typedef namespace), and a struct BODY holding a member spelled with the
// const typedef (that member is const, the object declared with the struct is
// not).
TEST(ConstThroughTypedef, TheTagNamespaceAndAStructBodyDoNotBorrowATypedefsConst) {
    std::string const body = std::string{kCI}
        + "void g(void) { struct { CI m; int n; } x = {1, 2}; x.n = 3; (void)x; }\n";
    expectVerdicts({
        {"`struct S x` beside `typedef const struct S S`",
         "struct S { int a; };\ntypedef const struct S S;\n"
         "void g(void) { struct S x = {1}; x.a = 2; (void)x; }\n", false},
        {"an object whose struct BODY holds a `CI` member", body.c_str(), false},
    }, DiagnosticCode::S_ConstViolation);
}

// ── what the symbol records: the object const and the spine ─────────────────
// `isConst` is also what places a file-scope object: MutabilityAttr →
// `MirGlobal.isConst` → a read-only section (pinned end to end at MIR by
// `MirLoweringC.AGlobalDeclaredThroughAConstTypedefIsReadOnlyData`).
TEST(ConstThroughTypedef, ObjectConstThroughATypedefMatchesTheDirectSpelling) {
    auto model = analyzeShipped("c", {
        "struct P { int a, b; };\n"
        "typedef const int CI;\n"
        "typedef const struct P CP;\n"
        "typedef const char *const CCPC;\n"
        "typedef int *const IPC;\n"
        "typedef const char *CCP;\n"
        "static int zero;\n"
        "CI g = 5;              const int gD = 5;\n"
        "CI tab[3] = {1, 2, 3}; const int tabD[3] = {1, 2, 3};\n"
        "CP pt = {1, 2};        const struct P ptD = {1, 2};\n"
        "CCPC s = \"x\";          const char *const sD = \"x\";\n"
        "IPC ps = &zero;        int *const psD = &zero;\n"
        "CCP m = \"y\";           int k = 1;\n"
        "CI *p;                 const int *pD;\n",
    });
    EXPECT_FALSE(model.hasErrors());
    EXPECT_FALSE(hasDiagnosedPointerConversion(model.diagnostics()))
        << "a compatible pointer pair must not be DIAGNOSED (the vacuity sweep)";
    for (auto const& [viaTypedef, direct] :
         {std::pair{"g", "gD"}, {"tab", "tabD"}, {"pt", "ptD"}, {"s", "sD"},
          {"ps", "psD"}}) {
        auto const* a = findSymbolNamed(model, viaTypedef);
        auto const* b = findSymbolNamed(model, direct);
        ASSERT_NE(a, nullptr) << viaTypedef;
        ASSERT_NE(b, nullptr) << direct;
        EXPECT_TRUE(b->isConst) << direct << " (the direct twin)";
        EXPECT_TRUE(a->isConst)
            << viaTypedef << " must be the const object " << direct << " is";
    }
    // THE CONTROLS: a mutable pointer to const, and a plain object.
    auto const* m = findSymbolNamed(model, "m");
    auto const* k = findSymbolNamed(model, "k");
    ASSERT_NE(m, nullptr);
    ASSERT_NE(k, nullptr);
    EXPECT_FALSE(m->isConst) << "`CCP m` is a MUTABLE pointer to const char";
    EXPECT_FALSE(k->isConst);
    // The spine: `CI *p` claims exactly what `const int *pD` claims.
    auto const* p  = findSymbolNamed(model, "p");
    auto const* pD = findSymbolNamed(model, "pD");
    ASSERT_NE(p, nullptr);
    ASSERT_NE(pD, nullptr);
    ASSERT_TRUE(pD->qualSpine.has_value());
    ASSERT_TRUE(p->qualSpine.has_value()) << "`CI *p` must make a claim";
    EXPECT_EQ(p->qualSpine->levels, pD->qualSpine->levels);
    EXPECT_EQ(p->qualSpine->constBits, pD->qualSpine->constBits)
        << "[Ptr, const int] for both spellings";
    EXPECT_EQ(p->qualSpine->constBits, std::uint64_t{0b10});
}

// ── a const typedef'd object folds where a direct const object folds ───────
// DSS folds a const object's initializer in an array dimension (clang 18.1.3 does
// too in its default mode, `-Wgnu-folding-constant`; gcc refuses both spellings at
// file scope). Before the typedef was applied the direct spelling folded and the
// typedef'd one was refused — a refusal below the union.
TEST(ConstThroughTypedef, AConstTypedefConstantFoldsLikeADirectConst) {
    auto model = analyzeShipped("c", {
        "typedef const int CI;\n"
        "typedef CI CI2;\n"
        "CI N = 4;\n"
        "CI2 M = 3;\n"
        "static int table[N][M];\n"
        "int size(void) { return (int)sizeof table; }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);
    auto const* table = findSymbolNamed(model, "table");
    ASSERT_NE(table, nullptr);
    ASSERT_TRUE(table->type.valid());
    auto const& in = model.lattice().interner();
    ASSERT_EQ(in.kind(table->type), TypeKind::Array);
    EXPECT_FALSE(in.typeContainsVla(table->type))
        << "both dimensions fold: `table` is int[4][3], not a variably modified type";
}

// ── the C23 redeclaration axis reads the typedef's qualifiers too ───────────
// Every refusal here has a DIRECT twin DSS already refused (`int f(const char *);
// int f(char *);` …). gcc 13.3.0, clang 18.1.3 and mingw-w64 13.2.0 refuse each
// pair; MSVC 19.51 refuses the result one and only warns (C4028) on the parameter
// ones — the same split the direct spellings have.
TEST(ConstThroughTypedef, TheRedeclarationAxisSeesAQualifierReachedThroughATypedef) {
    expectVerdicts({
        {"a parameter's pointee",
         "typedef const char *CCP;\nint f(CCP);\nint f(char *);\n", true},
        {"the result's pointee",
         "typedef const char *CCP;\nCCP g(void);\nchar *g(void);\n", true},
        {"a nested `restrict`",
         "typedef int *restrict RP;\nint k(RP *);\nint k(int **);\n", true},
        {"a function-pointer parameter's parameter",
         "typedef const char *CCP;\nint f(int (*)(CCP));\nint f(int (*)(char *));\n",
         true},
        {"a function declared through a function typedef",
         "typedef const char *F(void);\nF h;\nchar *h(void);\n", true},
        {"a pointer to a function typedef (the Fn level between)",
         "typedef const char *F(void);\nint f(F *);\nint f(char *(*)(void));\n", true},
        // THE CONTROLS: the same claims spelled both ways, and the top-level
        // qualifiers C 6.7.6.3p15 / C23 6.7.7.4p4 take out of the function type.
        {"a typedef against its direct spelling",
         "typedef const char *CCP;\nint f(CCP);\nint f(const char *);\n", false},
        {"a pointer to a function typedef against its direct spelling",
         "typedef const char *F(void);\nint f(F *);\nint f(const char *(*)(void));\n",
         false},
        {"a parameter's top-level const",
         "typedef const int CI;\nint h(CI);\nint h(int);\n", false},
        {"a parameter's top-level restrict",
         "typedef int *restrict RP;\nint m(RP);\nint m(int *);\n", false},
        {"a result's top-level const (gcc and mingw accept; clang and MSVC refuse)",
         "typedef const int F(void);\nF h;\nint h(void);\n", false},
    }, DiagnosticCode::S_IncompatibleRedeclaration);
}

// ── a void parameter qualified through a typedef is a QUALIFIED void ────────
// The direct spellings are Part 2's: a definition's `(const void)` is refused by
// all four; a prototype's `(const void)` is `(void)` (MSVC accepts and runs it); a
// NAMED `const void v` is refused (the stated residual).
TEST(ConstThroughTypedef, AVoidParameterQualifiedThroughATypedefIsAQualifiedVoid) {
    expectVerdicts({
        {"a definition's `(CV)`", "typedef const void CV;\nvoid f(CV) { }\n", true},
        {"a named `CV v`", "typedef const void CV;\nvoid f(CV v);\n", true},
        // THE CONTROLS.
        {"a prototype's `(CV)`, called with no argument",
         "typedef const void CV;\nvoid f(CV);\nvoid g(void) { f(); }\n", false},
        {"a definition's `(V)` for an unqualified `typedef void V`",
         "typedef void V;\nvoid f(V) { }\n", false},
    }, DiagnosticCode::S_InvalidVoidParam);
}

// ── a typedef may be redefined only to the SAME type — its qualifiers included
// C11 6.7p3. The two TypeIds of each refused pair are EQUAL (neither qualifier is
// interned); only the typedef rows' own claims tell them apart.
TEST(ConstThroughTypedef, ATypedefRedefinitionMustKeepItsQualifiers) {
    expectVerdicts({
        {"const dropped", "typedef const int T;\ntypedef int T;\n", true},
        {"a pointee's const dropped",
         "typedef const char *P;\ntypedef char *P;\n", true},
        {"a pointer's own const dropped", "typedef int *const Q;\ntypedef int *Q;\n",
         true},
        {"a function typedef's result pointee",
         "typedef const char *G(void);\ntypedef char *G(void);\n", true},
        // THE CONTROLS.
        {"redefined identically", "typedef const int T;\ntypedef const int T;\n", false},
        {"redefined through another const typedef",
         "typedef const int CI;\ntypedef const int T;\ntypedef CI T;\n", false},
        {"a function typedef's result top-level const (C23 6.7.7.4p4; gcc accepts)",
         "typedef const int F(void);\ntypedef int F(void);\n", false},
    }, DiagnosticCode::S_IncompatibleRedeclaration);
}

// ── `++` / `--` on a const lvalue: refused at every spelling ────────────────
// ✔MEASURED 2026-09-23: gcc 13.3.0, clang 18.1.3, mingw-w64 13.2.0 and MSVC 19.51
// refuse every refusal below and accept every control; DSS compiled them all
// before the const-write check reached the ++/-- operand.
TEST(IncrementOrDecrement, OfAConstLvalueIsRefusedAsItsAssignmentIs) {
    std::string const ci = kCI;
    auto const viaTypedef = ci + "void g(void) { CI y = 1; y++; }\n";
    expectVerdicts({
        {"`y++`", "void g(void) { const int y = 1; y++; }\n", true},
        {"`++y`", "void g(void) { const int y = 1; ++y; }\n", true},
        {"`y--`", "void g(void) { const int y = 1; y--; }\n", true},
        {"`--y`", "void g(void) { const int y = 1; --y; }\n", true},
        {"`(*p)++` through a `const int *`", "void g(const int *p) { (*p)++; }\n", true},
        {"`--s->m` on a const member",
         "struct S { const int m; int n; };\nvoid g(struct S *s) { --s->m; }\n", true},
        {"`a[0]++` on a const array", "const int a[2] = {1, 2};\nvoid g(void) { a[0]++; }\n",
         true},
        {"`y++` for a `CI y`", viaTypedef.c_str(), true},
        {"a file-scope const in a value position",
         "const int k = 1;\nint g(void) { return k++; }\n", true},
        // THE CONTROLS: the same spellings where nothing written is const.
        {"a mutable local", "void g(void) { int y = 1; y++; ++y; y--; --y; (void)y; }\n",
         false},
        {"a MUTABLE pointer to const, moved both ways",
         "void g(const char *p) { p++; --p; (void)p; }\n", false},
        {"the pointee of a const POINTER", "void g(int *const p) { (*p)++; }\n", false},
        {"a mutable member of a struct with a const one",
         "struct S { const int m; int n; };\nvoid g(struct S *s) { s->n++; }\n", false},
    }, DiagnosticCode::S_ConstViolation);
}

// The one shape the references SPLIT on keeps its assignment's severity: a member
// declared `const` that is also a BIT-FIELD. gcc 13.3.0 and mingw-w64 13.2.0
// compile `s->v++` (rc 0) while clang 18.1.3 and MSVC 19.51 refuse it, so DSS
// reports it and COMPILES — a Warning, exactly as `s->v = 1` is.
TEST(IncrementOrDecrement, OfAConstBitFieldMemberIsAWarningAsItsAssignmentIs) {
    for (char const* src :
         {"struct S { const int v : 3; int w; };\nvoid g(struct S *s) { s->v++; }\n",
          "struct S { const int v : 3; int w; };\nvoid g(struct S *s) { s->v = 1; }\n"}) {
        auto model = analyzeShipped("c", {std::string{src}});
        std::size_t warnings = 0;
        std::size_t errors   = 0;
        for (auto const& d : model.diagnostics().all()) {
            if (d.code != DiagnosticCode::S_ConstViolation) continue;
            (d.severity == DiagnosticSeverity::Warning ? warnings : errors) += 1;
        }
        EXPECT_EQ(warnings, 1u) << src;
        EXPECT_EQ(errors, 0u) << src;
        EXPECT_FALSE(model.hasErrors()) << src;
    }
}
