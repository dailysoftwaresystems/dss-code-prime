// P69 (lane `cs`): the SEMANTIC tier's side of a compound literal being an OBJECT.
//
//   * D-C-A-COMPOUND-LITERAL-IS-ITS-INITIALIZERS-VALUE-NOT-AN-OBJECT — the literal's object is
//     const as its type name says (a write to one is refused), and `*&E` takes no address.
//   * D-C-A-STORAGE-CLASS-SPECIFIER-IN-A-COMPOUND-LITERAL-IS-A-PARSE-ERROR — C23's
//     `( storage-class-specifiers type-name ) braced-initializer` parses, and its specifiers
//     are judged as the definition `SC typeof(T) ID = { IL };` in the literal's own scope
//     (C23 6.5.3.6p4): the declaration row of that scope reads them.
//   * C23 6.6p6-p7 — a compound literal CONSTANT and a constant's `.member`, even
//     recursively, fold in an integer constant expression
//     (D-C-A-NAMED-CONSTANT-MEMBER-IS-NOT-AN-INTEGER-CONSTANT-EXPRESSION for the named half).
//
// Every accepted and refused line is a reference measurement: gcc 13.3.0 -std=c2x is the one
// reference that builds storage-class literals and named-constant members (clang 18.1.3 and
// MSVC 19.51 parse none of them); clang -std=c2x folds CONST literals and const members as a
// GNU extension. Runs in lane `cs`'s findings: 20260930-212759-cd6e2714, -212815-0195cf2c,
// -212829-175f46fc, -212850-b8162830 (storage), -185149-8dd835aa, -185219-ff240d50,
// -215342-bd6f89d0, -215414-bc6d1ef1, -215506-23709b5b (constants), -212433-3fc88e35,
// -212450-c6fa011f (const literal writes).

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/data_model.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include "semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <stdexcept>
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

[[nodiscard]] std::size_t errorCount(SemanticModel const& m) {
    std::size_t n = 0;
    for (auto const& d : m.diagnostics().all())
        if (d.severity == DiagnosticSeverity::Error) ++n;
    return n;
}

// The one diagnostic of `code` a source draws, positioned at the `nth` `at`.
void expectOneAt(std::string const& src, DiagnosticCode code, std::string const& at,
                 int nth = 1) {
    auto const m = analyzeC(src);
    std::vector<ParseDiagnostic> hits;
    for (auto const& d : m.diagnostics().all())
        if (d.code == code) hits.push_back(d);
    ASSERT_EQ(hits.size(), 1u) << diagnosticCodeName(code) << " for\n" << src << describe(m);
    std::size_t pos = std::string::npos;
    std::size_t from = 0;
    for (int i = 0; i < nth; ++i) {
        pos = src.find(at, from);
        if (pos == std::string::npos) throw std::runtime_error("needle not in the source: " + at);
        from = pos + 1;
    }
    EXPECT_EQ(hits[0].span.start(), pos) << src << describe(m);
}

} // namespace

// ── the object's const-ness ─────────────────────────────────────────────────────────────
// A compound literal DESIGNATES an object; its type name's qualifiers qualify it (an array's
// qualifiers are its elements', C 6.7.3p10), and a `constexpr` one is const (C23 6.7.2p16).
// gcc and clang (both modes) refuse the first three, gcc -std=c2x the last two.
// RED-ON-DISABLE: drop the const-lvalue walk's compound-literal arm → every line draws nothing.
TEST(CompoundLiteralObjects, AWriteToAConstLiteralIsAConstViolation) {
    for (char const* src : {
             "int main(void) { (const int){ 0 } = 5; return 0; }\n",
             "int main(void) { ((const int[]){ 1 })[0] = 5; return 0; }\n",
             "struct S { int a; };\nint main(void) { (const struct S){ 0 }.a = 5; return 0; }\n",
             "int main(void) { (constexpr int){ 0 } = 5; return 0; }\n",
             "int main(void) { (static const int){ 0 } = 5; return 0; }\n",
         }) {
        auto const m = analyzeC(src);
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_ConstViolation), 1u)
            << src << describe(m);
    }
    auto const writable = analyzeC("int main(void) { (int){ 0 } = 5; return 0; }\n");
    EXPECT_EQ(errorCount(writable), 0u) << "a literal is a modifiable lvalue" << describe(writable);
}

// `*&E` is `E` (C 6.5.3.2p3): a static local initialized by `*&` of an automatic literal reads
// the literal's value and takes no address — gcc -std=c2x and clang build it (probe c11) — while
// `&*&` of it takes the automatic object's address, which every reference refuses.
// RED-ON-DISABLE: drop the static-initializer check's `*&` collapse → the first line draws
// S_StaticInitializerNotConstant.
TEST(CompoundLiteralObjects, StarAmpersandTakesNoAddress) {
    auto const built = analyzeC("int main(void) { static int x = *&(int){ 42 }; return x; }\n");
    EXPECT_EQ(countCode(built.diagnostics(), DiagnosticCode::S_StaticInitializerNotConstant), 0u)
        << describe(built);
    auto const refused =
        analyzeC("int main(void) { static int *p = &*&(int){ 42 }; return *p; }\n");
    EXPECT_EQ(countCode(refused.diagnostics(), DiagnosticCode::S_StaticInitializerNotConstant),
              1u)
        << describe(refused);
}

// ── C23 storage-class specifiers ────────────────────────────────────────────────────────
// Every form gcc -std=c2x builds analyzes clean.
// RED-ON-DISABLE: drop `storageCompoundLiteralExpr` from the `operand` alternatives → every
// line is a parse error.
TEST(CompoundLiteralObjects, EveryStorageClassFormGccBuildsIsClean) {
    for (char const* src : {
             "int main(int argc, char **argv) { (void)argv; return (static int){ 41 } + argc; }\n",
             "int main(int argc, char **argv) { (void)argv; return (register int){ 41 } + argc; }\n",
             "int main(int argc, char **argv) { (void)argv; return (constexpr int){ 41 } + argc; }\n",
             "int main(void) { return (static constexpr int){ 42 }; }\n",
             "int main(void) { return (register constexpr int){ 42 }; }\n",
             "int main(void) { int *p = &(static thread_local int){ 42 }; return *p; }\n",
             "int main(void) { return (_Thread_local static int){ 42 }; }\n",
             "int x = (thread_local int){ 42 };\nint main(void) { return x; }\n",
             "int *p = &(static int){ 42 };\nint main(void) { return *p; }\n",
             "int main(void) { static int *p = &(static int){ 42 }; return *p; }\n",
             "int main(void) { for (int *p = &(static int){ 0 }; *p < 1; ++*p) {} return 42; }\n",
         }) {
        auto const m = analyzeC(src);
        EXPECT_EQ(errorCount(m), 0u) << src << describe(m);
    }
}

// What gcc -std=c2x refuses, DSS refuses — each at the specifier that breaks it, under the
// code the definition it stands for would draw.
// RED-ON-DISABLE: drop `judgeCompoundLiteral`'s reports → every line draws nothing.
TEST(CompoundLiteralObjects, AStorageClassTheDefinitionWouldRefuseIsRefused) {
    // C 6.9p2 — no `register` outside every function body ("file-scope compound literal
    // specifies 'register'").
    expectOneAt("int x = (register int){ 42 };\n",
                DiagnosticCode::S_CompoundLiteralStorageClassInvalid, "register");
    // C23 6.5.3.6 footnote 97 ("duplicate 'static'").
    expectOneAt("int main(void) { return (static static int){ 42 }; }\n",
                DiagnosticCode::S_CompoundLiteralStorageClassInvalid, "static", 2);
    // C 6.7.1p3 — a block-scope thread_local needs `static` ("compound literal implicitly auto
    // and declared '_Thread_local'").
    expectOneAt("int main(void) { return (thread_local int){ 42 }; }\n",
                DiagnosticCode::S_ThreadLocalRequiresStaticOrExtern, "thread_local");
    // Two storage classes (C 6.7.1p2).
    expectOneAt("int main(void) { return (static register int){ 42 }; }\n",
                DiagnosticCode::S_ConflictingStorageClassSpecifiers, "register");
    // thread_local pairs only with static (C 6.7.1p2) and never with constexpr (C23 6.7.2).
    expectOneAt("int main(void) { return (register thread_local int){ 42 }; }\n",
                DiagnosticCode::S_ThreadLocalInvalidCombination, "register");
    expectOneAt("int main(void) { return (static thread_local constexpr int){ 42 }; }\n",
                DiagnosticCode::S_ThreadLocalInvalidCombination, "static");
    // A static or constexpr literal's initializer must be constant ("initializer element is
    // not constant").
    expectOneAt("int main(int argc, char **argv) { (void)argv; int x = argc; "
                "return (static int){ x }; }\n",
                DiagnosticCode::S_StaticInitializerNotConstant, "x }");
    // (The constexpr arm reports at the initializer it judges — the brace list, as it does
    // for a named `constexpr int k = { x };`.)
    expectOneAt("int main(int argc, char **argv) { (void)argv; int x = argc; "
                "return (constexpr int){ x }; }\n",
                DiagnosticCode::S_ConstexprNonConstantInitializer, "{ x }");
}

// `extern`, `typedef` and `auto` have no meaning in a compound literal (C23 6.5.3.6p7) and do
// not parse — gcc: "expected expression before 'extern'".
TEST(CompoundLiteralObjects, SpecifiersWithoutAMeaningDoNotParse) {
    for (char const* src : {
             "int main(void) { return (extern int){ 42 }; }\n",
             "int main(void) { return (typedef int){ 42 }; }\n",
             "int main(void) { return (auto int){ 42 }; }\n",
         }) {
        auto cu = buildShippedUnit("c", {std::string{src}});
        bool parseError = false;
        for (auto const& t : cu->trees())
            for (auto const& d : t.diagnostics().all())
                if (d.severity == DiagnosticSeverity::Error) parseError = true;
        EXPECT_TRUE(parseError) << src;
    }
}

// ── C23 6.6p6-p7: constants and their members in integer constant expressions ──────────────
// Each line folds where gcc -std=c2x folds it (or clang -std=c2x, for the const ones).
// RED-ON-DISABLE: drop `resolveConstantSubobject` from the environment → every line draws
// S_StaticAssertFailed ("not an integer constant expression") or a VLA/length refusal.
TEST(CompoundLiteralObjects, ConstantsAndTheirMembersFoldInIntegerConstantExpressions) {
    for (char const* src : {
             "_Static_assert((constexpr int){ 42 } == 42, \"\");\n",
             "int a[(constexpr int){ 3 }];\n_Static_assert(sizeof a == 3 * sizeof(int), \"\");\n",
             "enum { N = (constexpr int){ 40 } + 2 };\n_Static_assert(N == 42, \"\");\n",
             "struct S { int a, b; };\n"
             "_Static_assert((constexpr struct S){ 1, 42 }.b == 42, \"\");\n",
             "struct T { int v; };\nstruct S { struct T t; int b; };\n"
             "_Static_assert((constexpr struct S){ .t = { 42 } }.t.v == 42, \"\");\n",
             "struct T { int v; };\nstruct S { struct T t; int b; };\n"
             "constexpr struct S s = { { 42 }, 1 };\n_Static_assert(s.t.v == 42, \"\");\n",
             "struct S { int a, b; };\nconstexpr struct S s = { .b = 42 };\n"
             "_Static_assert(s.a == 0 && s.b == 42, \"\");\n",
             "union U { int i; float f; };\nconstexpr union U u = { .i = 42 };\n"
             "_Static_assert(u.i == 42, \"\");\n",
             "struct T { int x, y; };\nstruct S { struct T t; int z; };\n"
             "constexpr struct S s = { 1, 42, 3 };\n_Static_assert(s.t.y == 42 && s.z == 3, \"\");\n",
             "struct S { int a, b; };\nconstexpr struct S s = { 1, 42 };\n"
             "constexpr struct S s2 = s;\n_Static_assert(s2.b == 42, \"\");\n",
             "struct S { long a; unsigned b; };\nconstexpr struct S s = { -1, 1 };\n"
             "_Static_assert((-1 < s.b) == 0, \"\");\n",
             // clang -std=c2x's GNU folding: ANY non-volatile literal (n01-n05), and a CONST
             // named structure's member (m05).
             "_Static_assert((const int){ 42 } == 42, \"\");\n",
             "enum { M = (const int){ 42 } };\n_Static_assert(M == 42, \"\");\n",
             "_Static_assert((int){ 42 } == 42, \"\");\n",
             "struct S { int a, b; };\n_Static_assert((struct S){ 1, 42 }.b == 42, \"\");\n",
             "struct S { int a, b; };\nconst struct S s = { 1, 3 };\nint a[s.b];\n"
             "_Static_assert(sizeof a == 3 * sizeof(int), \"\");\n",
         }) {
        auto const m = analyzeC(src);
        EXPECT_EQ(errorCount(m), 0u) << src << describe(m);
    }
}

// What every reference refuses stays refused: an INACTIVE union member (gcc, m11), an element
// of an array member (gcc, m12 — only `.` reads a constant), a VOLATILE literal, and a
// NON-const named structure's member (clang refuses it too, n06).
TEST(CompoundLiteralObjects, ANonConstantSubobjectIsNotAConstant) {
    for (char const* src : {
             "union U { int i; float f; };\nconstexpr union U u = { .i = 42 };\n"
             "_Static_assert(u.f != 0, \"\");\n",
             "struct S { int v[2]; };\nconstexpr struct S s = { { 1, 42 } };\n"
             "_Static_assert(s.v[1] == 42, \"\");\n",
             "_Static_assert((volatile int){ 42 } == 42, \"\");\n",
             "struct S { int a, b; };\nstruct S s = { 1, 42 };\n_Static_assert(s.b == 42, \"\");\n",
         }) {
        auto const m = analyzeC(src);
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 1u)
            << src << describe(m);
    }
}

// ── `sizeof` / `_Alignof` of a compound literal (D-C-SIZEOF-OF-A-COMPOUND-LITERAL-IS-A-PARSE-ERROR) ──
// `sizeof (int){ 7 }` is `sizeof` of the unary-expression `(int){ 7 }` (C 6.5.2.5, a
// postfix-expression), not the `sizeof ( type-name )` form; the grammar's `notFollowedBy` on
// `sizeofType` / `alignofType` sends a `( T )` followed by `{` to the value form. Each operand
// is typed as gcc 13.3.0, clang 18.1.3, mingw-w64 13.2.0 and MSVC 19.51 type it (lane `cs`'s
// probe r3, runs 20260930-165337-5e798292, -165358-546e7581, -165420-d6af6f42): its own
// literal's type — an unsized array literal's own length included — and the type form stays
// the type form. RED-ON-DISABLE: drop `notFollowedBy` from `sizeofType` → every `sizeof (T){`
// line is P_UnexpectedToken again (and `alignofType` → the `_Alignof` line).
TEST(CompoundLiteralObjects, SizeofAndAlignofOfACompoundLiteralTakeTheValueForm) {
    for (char const* src : {
             "_Static_assert(sizeof (int){ 7 } == sizeof(int), \"\");\n",
             "struct S { int a; char b[12]; };\n"
             "_Static_assert(sizeof (struct S){ 1 } == sizeof(struct S), \"\");\n",
             "_Static_assert(sizeof (int[]){ 1, 2, 3 } == 3 * sizeof(int), \"\");\n",
             "_Static_assert(sizeof (char[]){ \"abcde\" } == 6, \"\");\n",
             "_Static_assert(sizeof (int[]){ 1, 2, 3 }[0] == sizeof(int), \"\");\n",
             "struct D { int a; double d; };\n"
             "_Static_assert(sizeof (struct D){ 0 }.d == sizeof(double), \"\");\n",
             "_Static_assert(sizeof (int){ 7 } + 1 == sizeof(int) + 1, \"\");\n",
             "_Static_assert(sizeof (int){ 7 } * 2 == 2 * sizeof(int), \"\");\n",
             "int a[sizeof (int[]){ 1, 2, 3 } / sizeof(int)];\n"
             "_Static_assert(sizeof a == 3 * sizeof(int), \"\");\n",
             "_Static_assert(_Alignof (double){ 7 } == _Alignof(double), \"\");\n",
             // the type form is untouched where no brace follows
             "_Static_assert(sizeof (int) + 1 == sizeof(int) + 1, \"\");\n",
             // P69 (restarted lane): the FOLLOWER is the next SIGNIFICANT token — a space, a
             // newline or a comment between `)` and `{` changes nothing (✔MEASURED before the
             // fix: each of these three was P_UnexpectedToken, lane `cs`'s probe nfb; gcc 13.3.0
             // and clang 18.1.3 in both modes, linux run 20261002-000105-51b261cc, and MSVC 19.51
             // /std:clatest and /std:c17, run 20261002-000130-44530ca7, all build and run 42).
             "_Static_assert(sizeof (int[]) { 1, 2, 3 } == 3 * sizeof(int), \"\");\n",
             "_Static_assert(sizeof (int[])\n    { 1, 2, 3 } == 3 * sizeof(int), \"\");\n",
             "_Static_assert(sizeof (int[]) /* the literal */ { 1, 2 } == 2 * sizeof(int), \"\");\n",
             "_Static_assert(_Alignof (double) { 7 } == _Alignof(double), \"\");\n",
         }) {
        auto const m = analyzeC(src);
        EXPECT_EQ(errorCount(m), 0u) << src << describe(m);
    }
}
