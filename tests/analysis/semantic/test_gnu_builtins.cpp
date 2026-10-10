// P69 (lane `cs`, D-CSUBSET-GNUC-PREDEFINE-SELECTS-UNIMPLEMENTED-BUILTIN) — the GNU builtins
// DSS's own `__GNUC__` claim routes real code into, at the SEMANTIC tier: which calls are
// constants, which operands a builtin requires to be constant or of a given type, and what a
// LIBRARY builtin (`__builtin_strlen`) binds to.
//
// Every expectation was MEASURED on the references first (`dssharness run probe-reference-cc`,
// lane `cs`'s probes r5s + r5t; gcc 13.3.0 and clang 18.1.3 at -std=c17 -pedantic-errors and
// -std=c2x): runs 20261001-025010-fa56e411, -025104-82961e14, -025126-2f33c6d9,
// -025147-c2261a03, -025208-dc494ffd and -030754-952a61cb (linux-x86_64-debug).

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace dss;
using namespace dss::sem_test;
namespace fs = std::filesystem;

namespace {

// No platform: the language's own surface (the builtins are rows of the `c` schema).
[[nodiscard]] SemanticModel analyzeC(std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
}

[[nodiscard]] fs::path shippedLibsDir() {
    fs::path const dir = dss::test::configRoot() / "shippedLibs";
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        throw std::runtime_error("shipped-lib descriptor directory missing: " + dir.string());
    return dir;
}

// A PLATFORM in play (the format active, the real descriptor corpus on the system include
// path) — what a library builtin needs to bind: the shape `test_redeclaration_compat.cpp`'s
// `analyzeWithShipped` uses.
// `budget` is the semantic tier's reporter budget (a refusal count must not lean on its
// recent-duplicate window: `withoutDedup()` turns it off).
[[nodiscard]] SemanticModel analyzeOn(std::string src, ObjectFormatKind format,
                                      DataModel dataModel,
                                      DiagnosticBudget budget = DiagnosticBudget::libraryDefault()) {
    auto schema = loadShippedSchema("c");
    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
    builder.addSystemDir(shippedLibsDir());
    builder.setActiveFormat(format);
    builder.addInMemory(std::move(src), "main.c");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    assertNoBuilderErrors(*cu);
    return analyze(cu, budget, dataModel,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16},
                   std::nullopt, SelectableObjectFormatKind::of(format), "x86_64");
}

[[nodiscard]] std::string describe(SemanticModel const& m) {
    std::string out;
    for (auto const& d : m.diagnostics().all()) {
        out += "\n  ";
        out += diagnosticCodeName(d.code);
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

[[nodiscard]] std::size_t codeCount(SemanticModel const& m, DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : m.diagnostics().all())
        if (d.code == code) ++n;
    return n;
}

}  // namespace

// ── Integer constant expressions ──────────────────────────────────────────────────────────
// gcc and clang take `__builtin_expect(1, 1)` (s01-s04) and the bit builtins (s08) in every
// integer-constant-expression position, both modes; clang takes `__builtin_object_size` too
// (t06, t07 — gcc refuses it there, the union accepts). The value is the one the MIR tier
// computes for the same verb (`foldBuiltinVerb`, the one arithmetic both evaluators share).
// RED-ON-DISABLE: drop `env.resolveBuiltinCall` from `buildConstEvalEnv` → every line is
// refused (S_StaticAssertFailed / S_NonConstantArrayLength / a non-constant enumerator).
TEST(GnuBuiltins, ConstantCallsFoldInEveryIntegerConstantExpression) {
    for (char const* src : {
             "_Static_assert(__builtin_expect(1, 1), \"\");\n",
             "enum { E = __builtin_expect(5, 0) };\n_Static_assert(E == 5, \"\");\n",
             "static int a[__builtin_expect(3, 1)];\n"
             "_Static_assert(sizeof a == 3 * sizeof(int), \"\");\n",
             "int f(int x) { switch (x) { case __builtin_expect(1, 0): return 1; } return 0; }\n",
             "_Static_assert(__builtin_expect_with_probability(4, 1, 0.5) == 4, \"\");\n",
             "_Static_assert(__builtin_ffs(8) == 4, \"\");\n"
             "_Static_assert(__builtin_ffs(0) == 0, \"\");\n"
             "_Static_assert(__builtin_ffsll(1LL << 40) == 41, \"\");\n",
             "_Static_assert(__builtin_parity(7) == 1, \"\");\n"
             "_Static_assert(__builtin_parity(3) == 0, \"\");\n"
             "_Static_assert(__builtin_parityll(0x8000000000000001ull) == 0, \"\");\n",
             "_Static_assert(__builtin_popcountl(7) == 3, \"\");\n"
             "_Static_assert(__builtin_popcount(0xffu) == 8, \"\");\n"
             "_Static_assert(__builtin_clzl(1ul) == 63, \"\");\n"
             "_Static_assert(__builtin_ctzl(8ul) == 3, \"\");\n",
             "_Static_assert(__builtin_clz(1u) == 31, \"\");\n"
             "_Static_assert(__builtin_ctz(8u) == 3, \"\");\n"
             "_Static_assert(__builtin_bswap16(0x1234) == 0x3412, \"\");\n",
             "_Static_assert(__builtin_stdc_bit_ceil_ui(5u) == 8u, \"\");\n"
             "_Static_assert(__builtin_stdc_first_trailing_one_ui(8u) == 4u, \"\");\n",
             "static char g[16];\n"
             "_Static_assert(__builtin_object_size(g, 0) == 16, \"\");\n"
             "_Static_assert(__builtin_object_size(g + 4, 0) == 12, \"\");\n"
             "_Static_assert(__builtin_object_size(&g[3], 2) == 13, \"\");\n",
             "struct S { char a[4]; char b[8]; };\nstatic struct S s;\n"
             "_Static_assert(__builtin_object_size(&s.b[2], 3) == 6, \"\");\n"
             "_Static_assert(__builtin_object_size(&s.a[1], 1) == 3, \"\");\n"
             "_Static_assert(__builtin_object_size(&s.a[1], 0) == 11, \"\");\n"
             "_Static_assert(__builtin_object_size((char *)&s + 4, 1) == 8, \"\");\n"
             "_Static_assert(__builtin_object_size(s.a, 1) == 4, \"\");\n",
             "_Static_assert(__builtin_object_size(\"abc\", 0) == 4, \"\");\n",
         }) {
        auto const m = analyzeC(src);
        EXPECT_EQ(errorCount(m), 0u) << src << describe(m);
    }
}

// ── `__builtin_object_size`: the documented UNKNOWN answer ─────────────────────────────────
// Where no object is visible — a pointer variable or parameter, a runtime offset, an operand
// with a side effect — gcc and clang both answer (size_t)-1 for types 0/1 and 0 for 2/3
// (s14, t03); out of the object's bounds is 0 (clang; t04). The operand is never evaluated.
TEST(GnuBuiltins, ObjectSizeAnswersUnknownWhereNoObjectIsVisible) {
    for (char const* src : {
             "int f(char *p) {\n"
             "  _Static_assert(__builtin_object_size(p, 0) == (unsigned long)-1, \"\");\n"
             "  _Static_assert(__builtin_object_size(p, 2) == 0, \"\");\n  return 0; }\n",
             "int f(int n) { char buf[10];\n"
             "  _Static_assert(__builtin_object_size(buf + 0, 0) == 10, \"\");\n"
             "  return (int)__builtin_object_size(buf + n, 0); }\n",
             "int f(void) { char buf[10]; char *q = buf;\n"
             "  _Static_assert(__builtin_object_size(q++, 0) == (unsigned long)-1, \"\");\n"
             "  _Static_assert(__builtin_object_size(buf - 1, 0) == 0, \"\");\n"
             "  _Static_assert(__builtin_object_size(buf + 12, 1) == 0, \"\");\n"
             "  return q == buf; }\n",
         }) {
        auto const m = analyzeC(src);
        EXPECT_EQ(errorCount(m), 0u) << src << describe(m);
    }
}

// ── Operands a builtin requires to be constant ─────────────────────────────────────────────
// Both references refuse a `__builtin_object_size` type that is not an integer constant
// expression from 0 to 3 (s33). RED-ON-DISABLE: drop `checkBuiltinVerbCall`'s ObjectSize
// constant check → no S_BuiltinArgumentNotConstant (and the call then has no answer to lower).
TEST(GnuBuiltins, ConstantOperandsAreRequiredWhereTheBuiltinSaysSo) {
    for (char const* src : {
             "unsigned long f(int n) { char b[4]; return __builtin_object_size(b, n); }\n",
             "unsigned long f(void) { char b[4]; return __builtin_object_size(b, 4); }\n",
         }) {
        auto const m = analyzeC(src);
        EXPECT_EQ(codeCount(m, DiagnosticCode::S_BuiltinArgumentNotConstant), 1u)
            << src << describe(m);
    }
}

// ── The frame reads are not builtins of this language yet ──────────────────────────────────
// `__builtin_frame_address(n)` / `__builtin_return_address(n)` answer from a walk of the FRAME
// RECORDS (level n is the n-th caller's frame), and DSS keeps no frame record yet, so neither
// name is declared: a use is the loud S_UndeclaredIdentifier, never an answer the frame cannot
// back. The requirement, and the frame-record build it waits on, are
// D-C-GNU-BUILTIN-FAMILIES-GCC-AND-CLANG-RUN-ARE-UNDECLARED's.
TEST(GnuBuiltins, TheFrameReadsAreUndeclaredUntilFrameRecordsExist) {
    for (char const* src : {
             "void *f(void) { return __builtin_frame_address(0); }\n",
             "void *f(void) { return __builtin_return_address(0); }\n",
         }) {
        auto const m = analyzeC(src);
        EXPECT_EQ(codeCount(m, DiagnosticCode::S_UndeclaredIdentifier), 1u)
            << src << describe(m);
    }
}

// ── The type-generic checked arithmetic ────────────────────────────────────────────────────
// Both references refuse a non-integer result object (s34); clang ACCEPTS `_Bool *` and an
// enumeration pointer where gcc refuses (s17, s18) — the union accepts. A pointer to const
// is refused by both. RED-ON-DISABLE: drop the overflow arm of `checkBuiltinVerbCall` → the
// refusals vanish (and a `double *` result reaches the lowering).
TEST(GnuBuiltins, OverflowOperandsAreIntegersAndTheResultAModifiableIntegerObject) {
    for (char const* src : {
             "int f(void) { double d; return __builtin_add_overflow(1, 2, &d); }\n",
             "int f(void) { int r; return __builtin_add_overflow(1.0, 2, &r); }\n",
             "int f(int const *p) { return __builtin_mul_overflow(1, 2, p); }\n",
             "int f(void) { int r; return __builtin_sub_overflow(1, &r); }\n",
         }) {
        auto const m = analyzeC(src);
        EXPECT_EQ(codeCount(m, DiagnosticCode::S_BuiltinOverflowOperandType), 1u)
            << src << describe(m);
    }
    for (char const* src : {
             "int f(void) { _Bool b; return __builtin_add_overflow(1, 1, &b); }\n",
             "enum E { A, B };\nint f(void) { enum E e; return __builtin_mul_overflow(2, 3, &e); }\n",
             "int f(void) { unsigned char c; return __builtin_sub_overflow(0u, 1, &c); }\n",
             "int f(long long a) { long long r; return __builtin_smulll_overflow(a, a, &r); }\n",
             "int f(void) { unsigned u; return __builtin_uadd_overflow(1u, 2u, &u); }\n",
         }) {
        auto const m = analyzeC(src);
        EXPECT_EQ(errorCount(m), 0u) << src << describe(m);
    }
}

// ── Library builtins ───────────────────────────────────────────────────────────────────────
// `__builtin_strlen` IS `strlen`: with no header, with a header plus the program's own
// prototype, and with a LOCAL `int strlen` in scope (s25-s27) — and the library name itself
// stays undeclared where no header declares it. On pe, `__builtin_printf` binds stdio.json's
// pe row (s29), whatever owns its body there. A function no descriptor provides on the
// target is refused AT EVERY USE, once per use however many of Pass 2's arms resolve it
// (P69 review n12).
// RED-ON-DISABLE: drop the `resolveLibraryBuiltinUse` call from Pass 2's reference
// resolution → every accepted program below is S_UndeclaredIdentifier; report a refusal
// only where the answer is first computed (the per-tree memo answering silently) → the
// two-use arms count 1; forget which uses are reported (both arms report one use) → the
// window-off arms count 4 and 2 (the default window hides it); refuse a row whose body is
// shipped source → the header-less `__builtin_strndup` pe arm is S_LibraryBuiltinUnavailable.
TEST(GnuBuiltins, ALibraryBuiltinCallsTheLibraryFunction) {
    struct Case { ObjectFormatKind format; DataModel dm; char const* src; };
    for (Case const& c : {
             Case{ObjectFormatKind::Elf, DataModel::Lp64,
                  "int main(void) { return (int)__builtin_strlen(\"abc\") - 3; }\n"},
             Case{ObjectFormatKind::Pe, DataModel::Llp64,
                  "int main(void) { return (int)__builtin_strlen(\"abc\") - 3; }\n"},
             Case{ObjectFormatKind::Elf, DataModel::Lp64,
                  "#include <string.h>\nsize_t strlen(const char *);\n"
                  "int main(void) { return (int)(__builtin_strlen(\"a\") - strlen(\"a\")); }\n"},
             Case{ObjectFormatKind::Elf, DataModel::Lp64,
                  "int main(void) { int strlen = 3; return (int)__builtin_strlen(\"abc\") - strlen; }\n"},
             Case{ObjectFormatKind::Elf, DataModel::Lp64,
                  "int main(void) { char b[4]; __builtin_memset(b, 0, 4);\n"
                  "  __builtin_memcpy(b, \"ab\", 3); return __builtin_strcmp(b, \"ab\")\n"
                  "    + __builtin_memcmp(b, \"ab\", 2) + (int)__builtin_ceil(0.5) - 1; }\n"},
             Case{ObjectFormatKind::Pe, DataModel::Llp64,
                  "int main(void) { return __builtin_printf(\"%d\\n\", 7) - 2; }\n"},
             // No header declares strndup, so the binder binds the platform's row, whose pe
             // body is DSS's shipped source (string.json), as the `#include` path binds it.
             Case{ObjectFormatKind::Pe, DataModel::Llp64,
                  "void free(void *);\n"
                  "int main(void) { char *p = __builtin_strndup(\"abc\", 2);\n"
                  "  int const r = p == 0; free(p); return r; }\n"},
             Case{ObjectFormatKind::Elf, DataModel::Lp64,
                  "int main(int argc, char **argv) { (void)argv; if (argc > 5) __builtin_abort();\n"
                  "  return 0; }\n"},
         }) {
        auto const m = analyzeOn(c.src, c.format, c.dm);
        EXPECT_EQ(errorCount(m), 0u) << c.src << describe(m);
    }
    // The library NAME stays undeclared where nothing declares it.
    {
        auto const m = analyzeOn("int main(void) { return (int)__builtin_strlen(\"a\")\n"
                                 "  + (int)strlen(\"a\"); }\n",
                                 ObjectFormatKind::Elf, DataModel::Lp64);
        EXPECT_EQ(codeCount(m, DiagnosticCode::S_UndeclaredIdentifier), 1u) << describe(m);
        EXPECT_EQ(errorCount(m), 1u) << describe(m);
    }
    // A library function no descriptor provides here: refused at EACH use — and at each
    // only once, though the reference arm and the call arm both resolve a call's callee.
    {
        auto const m = analyzeOn("double f(double x) { return __builtin_cbrt(x) + __builtin_cbrt(x); }\n",
                                 ObjectFormatKind::Elf, DataModel::Lp64);
        EXPECT_EQ(codeCount(m, DiagnosticCode::S_LibraryBuiltinUnavailable), 2u) << describe(m);
        EXPECT_EQ(errorCount(m), 2u) << describe(m);
    }
    // The once-per-use guarantee is the BINDER's, not the reporter's recent-duplicate window
    // (4 diagnostics deep, and absent from a reporter built without one): with the window off
    // the two uses are still exactly two reports, and a parenthesized callee one.
    for (auto const& [src, uses] : {
             std::pair<char const*, std::size_t>{
                 "double f(double x) { return __builtin_cbrt(x) + __builtin_cbrt(x); }\n", 2u},
             std::pair<char const*, std::size_t>{
                 "double f(double x) { return (__builtin_cbrt)(x); }\n", 1u},
         }) {
        auto const m = analyzeOn(src, ObjectFormatKind::Elf, DataModel::Lp64,
                                 DiagnosticBudget::libraryDefault().withoutDedup());
        EXPECT_EQ(codeCount(m, DiagnosticCode::S_LibraryBuiltinUnavailable), uses)
            << src << describe(m);
        EXPECT_EQ(errorCount(m), uses) << src << describe(m);
    }
    {
        auto const m = analyzeOn("double f(double x) { return (__builtin_cbrt)(x); }\n",
                                 ObjectFormatKind::Elf, DataModel::Lp64);
        EXPECT_EQ(codeCount(m, DiagnosticCode::S_LibraryBuiltinUnavailable), 1u) << describe(m);
        EXPECT_EQ(errorCount(m), 1u) << describe(m);
    }
}

// ── `__builtin_nan`: a string literal folds, anything else is the library's `nan` ──────────
// gcc and clang fold a literal gcc's parser consumes wholly (s10, s11) and call libm's `nan`
// for a non-literal operand (s12, which fails only at LINK there without -lm).
TEST(GnuBuiltins, NanFoldsALiteralAndCallsTheLibraryOtherwise) {
    for (char const* src : {
             "static double d = __builtin_nan(\"\");\nstatic float f = __builtin_nanf(\"1\");\n"
             "static double i = __builtin_inf();\nstatic float h = __builtin_huge_valf();\n",
             "double f(char const *s) { return __builtin_nan(s); }\n",
             "double f(void) { return __builtin_nan(\"abc\"); }\n",   // not wholly consumed
         }) {
        auto const m = analyzeOn(src, ObjectFormatKind::Elf, DataModel::Lp64);
        EXPECT_EQ(errorCount(m), 0u) << src << describe(m);
    }
}

// ── gcc's `_p` predicates: an integer constant expression when `a` and `b` are ──────────────────
// `__builtin_{add,sub,mul}_overflow_p(a, b, c)`: the infinite-precision a OP b cast to the type
// of `c` — its own, unpromoted, or a bit-field's precision — and whether the cast changed it.
// ✔MEASURED (lane `cs`'s probe p12, linux run 20261001-044659-fe33d205): gcc 13.3.0 builds this
// very source at -std=c17 -pedantic-errors and -std=c2x and runs it to 42 — gcc's manual's own
// example in an enum, a variable third operand (only its TYPE counts), a bit-field's width, the
// u64 edges, a static initializer and an array bound. clang has none of the three ("unknown
// builtin").
// RED-ON-DISABLE: drop the `_p` answer from `builtinCallPlan` → every constant context refuses.
TEST(GnuBuiltins, OverflowPredicatesAreConstantWhenTheirValueOperandsAre) {
    auto const m = analyzeC(
        "enum { A = 2147483647, B = 3,\n"
        "       C = __builtin_add_overflow_p(A, B, (__typeof__(A + B))0) ? 0 : A + B,\n"
        "       D = __builtin_add_overflow_p(1, 127, (signed char)0),\n"
        "       E = __builtin_sub_overflow_p(0, 1, (unsigned)0),\n"
        "       F = __builtin_mul_overflow_p(65536, 65536, (long long)0) };\n"
        "_Static_assert(C == 0, \"C\");\n"
        "_Static_assert(D == 1, \"D\");\n"
        "_Static_assert(E == 1, \"E\");\n"
        "_Static_assert(F == 0, \"F\");\n"
        "_Static_assert(__builtin_add_overflow_p(100, 27, (signed char)0) == 0, \"127 fits\");\n"
        "_Static_assert(__builtin_add_overflow_p(-1, 0, (unsigned char)0) == 1, \"-1\");\n"
        "_Static_assert(__builtin_mul_overflow_p(0xffffffffffffffffull, 2, 0ull) == 1, \"u64\");\n"
        "_Static_assert(__builtin_mul_overflow_p(-1LL, -9223372036854775807LL - 1, 0ull) == 0,"
        " \"2^63 fits\");\n"
        "int x;\n"
        "enum { G = __builtin_add_overflow_p(2147483647, 1, x) };\n"
        "_Static_assert(G == 1, \"G\");\n"
        "struct S { int b : 3; unsigned u : 3; } s;\n"
        "_Static_assert(__builtin_add_overflow_p(3, 1, s.b) == 1, \"3-bit signed\");\n"
        "_Static_assert(__builtin_add_overflow_p(7, 0, s.u) == 0, \"3-bit unsigned\");\n"
        "_Static_assert(__builtin_add_overflow_p(7, 1, s.u) == 1, \"3-bit unsigned overflows\");\n"
        "static int st = __builtin_mul_overflow_p(65536, 65536, (int)0);\n"
        "int arr[__builtin_add_overflow_p(1, 1, (int)0) + 1];\n");
    EXPECT_EQ(errorCount(m), 0u) << describe(m);
    // CONTROL: a predicate whose answer is the OTHER one fails its assertion — the folding
    // is read, not assumed.
    auto const wrong = analyzeC("_Static_assert(__builtin_add_overflow_p(100, 28, (signed char)0) "
                                "== 0, \"128 does not fit\");\n");
    EXPECT_EQ(errorCount(wrong), 1u) << describe(wrong);
}

// The third operand is an integer other than an enumeration or a `_Bool`, and the first two are
// integers. ✔MEASURED (lane `cs`'s probes p06-p09, p11, gcc 13.3.0, both modes): an enumerated,
// boolean or `double` third operand and a pointer first operand are refused; `_Bool` and
// enumeration FIRST operands are accepted.
TEST(GnuBuiltins, AnOverflowPredicatesOperandsAreIntegersAndItsTargetNoEnumOrBool) {
    for (char const* src : {
             "enum E { e0 }; int f(void) { return __builtin_add_overflow_p(1, 2, (enum E)0); }\n",
             "int f(void) { return __builtin_sub_overflow_p(1, 2, (_Bool)0); }\n",
             "int f(void) { return __builtin_mul_overflow_p(1, 2, 0.0); }\n",
             "int f(int *p) { return __builtin_add_overflow_p(p, 1, (long)0); }\n",
             "int f(void) { return __builtin_add_overflow_p(1, 2); }\n",
         }) {
        auto const m = analyzeC(src);
        EXPECT_EQ(codeCount(m, DiagnosticCode::S_BuiltinOverflowOperandType), 1u)
            << src << describe(m);
    }
    auto const ok = analyzeC("enum E { e0, e1 };\n"
                             "int f(_Bool t, enum E e) { return __builtin_add_overflow_p(t, e, (int)0); }\n");
    EXPECT_EQ(errorCount(ok), 0u) << "CONTROL: `_Bool` and enumeration value operands" << describe(ok);
}
