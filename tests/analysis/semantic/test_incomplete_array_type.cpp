// P69 (lane `cs`) — AN ARRAY OF UNKNOWN SIZE: WHERE ONE MAY STAND, AND WHAT `sizeof` OF ONE IS.
//
// C 6.7.6.2: an array declarator with no size gives an INCOMPLETE array type. It is a complete
// pointee (`int (*p)[]`), the type of an `extern` declaration and of a flexible member, a type name
// (`int[]`), and — at file scope with no `extern` — a TENTATIVE definition the unit completes to
// one element when nothing else completes it (C 6.9.2p5). Its ELEMENT must be complete. An object
// of it with no completion anywhere is no object (a block-scope `int a[];`).
//
// `sizeof` of one is a constraint violation in ISO C (6.5.3.4p1), and gcc and clang refuse it; cl
// ACCEPTS it as 0 with C4034 — ✔MEASURED (lane `cs`, probe sz, cl 19.51): an extern array never
// defined, a flexible member, `int[]`, `*p` of `int (*p)[]`, a typedef `int T[]`, a tentative
// `int v[]` — and the zero folds into a constant expression (`[sizeof a + 42]` is 42). By the bar
// (what any reference compiler builds is required), DSS accepts it, folds it to 0 on every target,
// and SAYS so with a warning of its own. An incomplete STRUCTURE or UNION has no size on any
// reference (cl C2027), and is refused by name.
//
// The value is read through an array bound (`char probe[sizeof x + 1]`), so what the constant
// evaluator folds — not only what the type rules say — is what is pinned.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::sem_test;

namespace {

[[nodiscard]] SemanticModel analyzeWithLayout(std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
}

[[nodiscard]] std::string everyDiagnostic(SemanticModel const& m) {
    std::string s;
    for (auto const& d : m.diagnostics().all()) {
        s += "\n  ";
        s += diagnosticCodeName(d.code);
        s += ": ";
        s += d.actual;
    }
    return s.empty() ? std::string{"\n  (no diagnostics)"} : s;
}

// The one element count of the array object `name` (every record of the name agreeing), or nothing.
[[nodiscard]] std::optional<std::int64_t> arrayLength(SemanticModel const& model, std::string_view name) {
    auto const& ti = model.lattice().interner();
    std::optional<std::int64_t> found;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name != name) continue;
        TypeId const t = model.symbols()[i].type;
        if (!t.valid() || ti.kind(t) != TypeKind::Array) continue;
        auto const sc = ti.scalars(t);
        if (sc.empty()) continue;
        if (found.has_value() && *found != sc[0]) return std::nullopt;
        found = sc[0];
    }
    return found;
}

}  // namespace

TEST(IncompleteArrayType, SizeofAnArrayOfUnknownSizeIsZeroAndSaidSo) {
    for (char const* src : {
             "extern int a[];\nchar probe[sizeof a + 1];\n",
             "char probe[sizeof(int[]) + 1];\n",
             "typedef int T[];\nchar probe[sizeof(T) + 1];\n",
             "struct F { int n; int d[]; };\nextern struct F f;\nchar probe[sizeof f.d + 1];\n",
             "extern int (*p)[];\nchar probe[sizeof *p + 1];\n",
             // Before a later declaration completes it, the array is still of unknown size where `sizeof` is
             // written: cl gives 0 there, not the completed size (✔MEASURED, probe sz cell 2).
             "extern int a[];\nchar probe[sizeof a + 1];\nint a[3];\n",
         }) {
        SCOPED_TRACE(src);
        SemanticModel const m = analyzeWithLayout(src);
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_SizeofIncompleteArrayIsZero), 1u)
            << "the zero is an extension, and it is said" << everyDiagnostic(m);
        EXPECT_EQ(arrayLength(m, "probe"), std::optional<std::int64_t>{1})
            << "sizeof of an array of unknown size folds to 0";
    }
    // In a function body the same (C 6.2.7p4): between the declaration of unknown size and the one that completes
    // it, the name is an array of unknown size (cl's 0, said); after it, the completed array, and no word.
    {
        SemanticModel const m = analyzeWithLayout(
            "extern int a[];\nunsigned long f(void) { return sizeof a; }\nint a[3];\n");
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_SizeofIncompleteArrayIsZero), 1u)
            << "a use before the completing declaration reads the array of unknown size" << everyDiagnostic(m);
    }
    for (char const* src : {"extern int a[];\nint a[3];\nunsigned long f(void) { return sizeof a; }\n",
                            "int a[3];\nextern int a[];\nunsigned long f(void) { return sizeof a; }\n"}) {
        SCOPED_TRACE(src);
        SemanticModel const m = analyzeWithLayout(src);
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_SizeofIncompleteArrayIsZero), 0u)
            << "a use after a declaration that gives the size reads it" << everyDiagnostic(m);
    }
    // CONTROL: the same array with its size states it and says nothing.
    SemanticModel const m = analyzeWithLayout("extern int a[4];\nchar probe[sizeof a + 1];\n");
    EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_SizeofIncompleteArrayIsZero), 0u);
    EXPECT_EQ(arrayLength(m, "probe"), std::optional<std::int64_t>{17});
}

TEST(IncompleteArrayType, SizeofAnIncompleteStructureOrUnionIsRefusedByName) {
    for (char const* src : {
             "struct S;\nunsigned long n = sizeof(struct S);\n",
             "union U;\nextern union U u;\nunsigned long n = sizeof u;\n",
         }) {
        SCOPED_TRACE(src);
        SemanticModel const m = analyzeWithLayout(src);
        EXPECT_TRUE(m.hasErrors());
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_SizeofIncompleteType), 1u) << everyDiagnostic(m);
    }
    // CONTROL: completed, it has a size.
    SemanticModel const m = analyzeWithLayout("struct S { int x; };\nunsigned long n = sizeof(struct S);\n");
    EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_SizeofIncompleteType), 0u);
}

// C 6.7.6.2p1: "The element type shall not be an incomplete or function type." ✔MEASURED (lane `cs`, probe el:
// gcc 13.3.0 and clang 18.1.3 run gnu-0 20261015-171648-25167599, cl 19.51 run msvc-0 of 2026-10-15): an element
// that is itself an array of unknown size — `int a[3][]`, `int (*p)[2][]`, `typedef int T[2][]`, `int a[][]` — is
// refused by all three (cl C2087 "missing subscript"), and is refused here by name. An element of an INCOMPLETE
// STRUCTURE in a declaration that makes no object — `extern struct S arr[3];`, `extern struct S arr[];`, `typedef
// struct S T[2];`, a parameter `struct S arr[]` — is refused by gcc and clang and BUILT by cl, silently at /W4 (the
// structure may be completed later; nothing needs its size there), so by the disjunction it is accepted here too.
TEST(IncompleteArrayType, AnElementOfUnknownSizeIsRefusedAndAnIncompleteStructureElementIsCls) {
    for (char const* src : {
             "extern int a[3][];\n",
             "extern int (*p)[2][];\n",
             "typedef int T[2][];\n",
             "extern int a[][];\n",
         }) {
        SCOPED_TRACE(src);
        SemanticModel const m = analyzeWithLayout(src);
        EXPECT_TRUE(m.hasErrors());
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_ArrayElementTypeIncomplete), 1u)
            << "an omitted bound that is not the outermost is refused by name" << everyDiagnostic(m);
    }
    for (char const* src : {
             "struct S;\nextern struct S arr[3];\n",
             "struct S;\nextern struct S arr[];\n",
             "struct S;\ntypedef struct S T[2];\n",
             "struct S;\nvoid f(struct S arr[]);\n",
             // CONTROL: an array of POINTERS to the incomplete type is an array of complete elements.
             "struct S;\nextern struct S *arr[3];\n",
         }) {
        SCOPED_TRACE(src);
        SemanticModel const m = analyzeWithLayout(src);
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
    }
}

TEST(IncompleteArrayType, APointerToAnArrayOfUnknownSizeIsCompatibleWithOneOfAnySize) {
    for (char const* src : {
             "int buf[4];\nint (*p)[] = &buf;\n",
             "int buf[4];\nint (*p)[4] = &buf;\nint (*q)[] = 0;\nvoid f(void) { q = p; p = q; }\n",
         }) {
        SCOPED_TRACE(src);
        SemanticModel const m = analyzeWithLayout(src);
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_FALSE(hasDiagnosedPointerConversion(m.diagnostics()))
            << "C 6.7.6.2p6: an array of unknown size is compatible with one of the same element"
            << everyDiagnostic(m);
    }
    // The REDECLARATION oracles ask the same relation (C 6.7.6.2p6): a function whose prototype's parameter is a
    // pointer to an array of unknown size and whose definition's is one of three, and an object declared both ways,
    // whose type is then the composite (C 6.2.7p3) whichever declaration comes first. ✔MEASURED (lane `cs`, probe
    // ak cells 5-9): gcc 13.3.0 and clang 18.1.3 build and run each, `sizeof *p` reading the three elements.
    for (char const* src : {"int f(int (*q)[]);\nint f(int (*q)[3]) { return (*q)[2]; }\n",
                            "int f(int n, int a[n][*]);\nint f(int n, int a[n][3]) { return a[1][2] + n; }\n"}) {
        SCOPED_TRACE(src);
        SemanticModel const m = analyzeWithLayout(src);
        EXPECT_FALSE(m.hasErrors()) << "a parameter that points to an array of unknown size is compatible with one "
                                       "of a known size" << everyDiagnostic(m);
    }
    for (char const* src : {"extern int (*p)[];\nint (*p)[3];\nchar probe[sizeof *p + 1];\n",
                            "int (*p)[3];\nextern int (*p)[];\nchar probe[sizeof *p + 1];\n",
                            "int (*p)[];\nint (*p)[3];\nchar probe[sizeof *p + 1];\n"}) {
        SCOPED_TRACE(src);
        SemanticModel const m = analyzeWithLayout(src);
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_EQ(arrayLength(m, "probe"), std::optional<std::int64_t>{13})
            << "the object's type is the composite, a pointer to three elements" << everyDiagnostic(m);
    }
    // C 6.2.7p4 ONE POINTER DOWN: a use BEFORE the declaration that completes the pointee reads the pointer to an
    // array of unknown size written so far, a use after it the composite. ✔MEASURED (lane `cs` probe rv3 cells 1
    // and 2): `sizeof *p` of `extern int (*p)[];` is 0 under cl 19.51 (C4034) — the language document's
    // `sizeof.incompleteArraySize`, said — and refused by gcc 13.3.0, clang 18.1.3 and mingw-w64 gcc 13.2.0; the
    // redeclaration with three elements is built by gcc and clang (probe ak) and refused by cl (C2369). Each reading
    // is taken where a reference gives it: `before` reads 0 + 1, `after` 12 + 1.
    {
        SemanticModel const m = analyzeWithLayout(
            "extern int (*p)[];\nchar before[sizeof *p + 1];\nint (*p)[3];\nchar after[sizeof *p + 1];\n");
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_SizeofIncompleteArrayIsZero), 1u) << everyDiagnostic(m);
        EXPECT_EQ(arrayLength(m, "before"), std::optional<std::int64_t>{1})
            << "a use before the completing declaration read its composite" << everyDiagnostic(m);
        EXPECT_EQ(arrayLength(m, "after"), std::optional<std::int64_t>{13}) << everyDiagnostic(m);
    }
    // CONTROLS: two KNOWN sizes that differ are not compatible, in a conversion or a redeclaration.
    {
        SemanticModel const c = analyzeWithLayout("extern int (*p)[2];\nint (*p)[3];\n");
        EXPECT_EQ(countCode(c.diagnostics(), DiagnosticCode::S_IncompatibleRedeclaration), 1u) << everyDiagnostic(c);
    }
    SemanticModel const m = analyzeWithLayout("int buf[4];\nint (*p)[5] = &buf;\n");
    EXPECT_TRUE(hasDiagnosedPointerConversion(m.diagnostics())) << everyDiagnostic(m);
}

// C 6.2.7p3 AT EVERY DERIVATION LEVEL (P69 lane `cs`, fold f26b): two declarations of one object whose types are
// compatible below an array of pointers, inside a pointed-to function's parameter or result, or only as a composite
// neither declaration spells, are one object of the composite type. ✔MEASURED (lane `cs`, probe rc5): gcc 13.3.0,
// clang 18.1.3 and mingw-w64 gcc 13.2.0 build and run each cell (cl 19.51 refuses the array-level ones, C2369), and
// all four refuse the two controls, two known sizes that differ. DSS refused all nine, and warned an incompatible
// pointer conversion on the compatible assignment of cells 3 and 9.
TEST(IncompleteArrayType, ACompositeIsTakenAtEveryDerivationLevel) {
    // The array lengths each pointed-to array parameter of the function `name` points to has (every record agreeing).
    auto const paramArrayLengths = [](SemanticModel const& model, std::string_view name) {
        auto const&               ti = model.lattice().interner();
        std::vector<std::int64_t> lengths;
        for (std::size_t i = 1; i < model.symbols().size(); ++i) {
            if (model.symbols()[i].name != name) continue;
            TypeId const t = model.symbols()[i].type;
            if (!t.valid() || ti.kind(t) != TypeKind::Ptr) continue;
            TypeId const fn = ti.stripVolatile(ti.operands(t)[0]);
            if (ti.kind(fn) != TypeKind::FnSig) continue;
            std::vector<std::int64_t> mine;
            auto const ops = ti.operands(fn);
            for (std::size_t p = 1; p < ops.size(); ++p) {
                TypeId const param = ti.stripVolatile(ops[p]);
                TypeId const arr   = ti.kind(param) == TypeKind::Ptr ? ti.stripVolatile(ti.operands(param)[0])
                                                                     : InvalidType;
                mine.push_back(arr.valid() && ti.kind(arr) == TypeKind::Array ? ti.scalars(arr)[0] : -99);
            }
            if (!lengths.empty() && lengths != mine) return std::vector<std::int64_t>{};
            lengths = mine;
        }
        return lengths;
    };
    // Cells 1 and 2: an array of pointers to arrays, both orders — `sizeof *ap[0]` after them is 12.
    for (char const* src : {"int (*ap[2])[];\nint (*ap[2])[3];\nchar probe[sizeof *ap[0] + 1];\n",
                            "int (*ap[2])[3];\nint (*ap[2])[];\nchar probe[sizeof *ap[0] + 1];\n"}) {
        SCOPED_TRACE(src);
        SemanticModel const m = analyzeWithLayout(src);
        EXPECT_FALSE(m.hasErrors()) << "an array of pointers to compatible arrays was refused" << everyDiagnostic(m);
        EXPECT_EQ(arrayLength(m, "probe"), std::optional<std::int64_t>{13})
            << "the object's type is not the composite, an array of pointers to three elements" << everyDiagnostic(m);
    }
    // Cells 3 and 4: a pointer to a function whose parameter points to an array, both orders — and the function the
    // composite's type names is assigned to it with no word.
    for (char const* src : {"void (*fp)(int (*)[]);\nvoid (*fp)(int (*)[3]);\n"
                            "static void t(int (*p)[3]) { (void)p; }\nvoid g(void) { fp = t; }\n",
                            "void (*fp)(int (*)[3]);\nvoid (*fp)(int (*)[]);\n"
                            "static void t(int (*p)[3]) { (void)p; }\nvoid g(void) { fp = t; }\n"}) {
        SCOPED_TRACE(src);
        SemanticModel const m = analyzeWithLayout(src);
        EXPECT_FALSE(m.hasErrors()) << "a pointer to a function with a compatible parameter was refused"
                                    << everyDiagnostic(m);
        EXPECT_FALSE(hasDiagnosedPointerConversion(m.diagnostics()))
            << "the assignment of a compatible function was warned an incompatible pointer conversion"
            << everyDiagnostic(m);
        EXPECT_EQ(paramArrayLengths(m, "fp"), (std::vector<std::int64_t>{3})) << everyDiagnostic(m);
    }
    // Cell 5: a pointer to a function returning a pointer to an array — `sizeof *rp()` after both reads three
    // elements, with no word (before the second it is the array of unknown size, said).
    {
        SemanticModel const m = analyzeWithLayout(
            "int (*(*rp)(void))[];\nint (*(*rp)(void))[3];\nunsigned long f(void) { return sizeof *rp(); }\n");
        EXPECT_FALSE(m.hasErrors()) << "a pointer to a function with a compatible result was refused"
                                    << everyDiagnostic(m);
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_SizeofIncompleteArrayIsZero), 0u)
            << "the result's composite was not read after the declaration that completes it" << everyDiagnostic(m);
        SemanticModel const before = analyzeWithLayout(
            "int (*(*rp)(void))[];\nunsigned long f(void) { return sizeof *rp(); }\nint (*(*rp)(void))[3];\n");
        EXPECT_FALSE(before.hasErrors()) << everyDiagnostic(before);
        EXPECT_EQ(countCode(before.diagnostics(), DiagnosticCode::S_SizeofIncompleteArrayIsZero), 1u)
            << "CONTROL: a use before the completing declaration reads the array of unknown size"
            << everyDiagnostic(before);
    }
    // Cell 6: an array of unknown size of pointers to arrays of unknown size, then both sizes — 16 and 12.
    {
        SemanticModel const m = analyzeWithLayout(
            "int (*aq[])[];\nint (*aq[2])[3];\nchar probe[sizeof aq + sizeof *aq[0] + 1];\n");
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_EQ(arrayLength(m, "aq"), std::optional<std::int64_t>{2}) << everyDiagnostic(m);
        EXPECT_EQ(arrayLength(m, "probe"), std::optional<std::int64_t>{29}) << everyDiagnostic(m);
    }
    // Cell 9: a composite NEITHER declaration spells — `void (*)(int (*)[2], int (*)[3])` — built, and the function
    // of that type assigned with no word.
    {
        SemanticModel const m = analyzeWithLayout(
            "void (*fp)(int (*)[], int (*)[3]);\nvoid (*fp)(int (*)[2], int (*)[]);\n"
            "static void t(int (*p)[2], int (*q)[3]) { (void)p; (void)q; }\nvoid g(void) { fp = t; }\n");
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_FALSE(hasDiagnosedPointerConversion(m.diagnostics())) << everyDiagnostic(m);
        EXPECT_EQ(paramArrayLengths(m, "fp"), (std::vector<std::int64_t>{2, 3}))
            << "the object's type is not the composite neither declaration spells" << everyDiagnostic(m);
    }
    // CONTROLS (cells 7 and 8): two known sizes that differ, below an array of pointers and inside a parameter.
    for (char const* src : {"int (*ap[2])[2];\nint (*ap[2])[3];\n", "void (*fp)(int (*)[2]);\nvoid (*fp)(int (*)[3]);\n"}) {
        SCOPED_TRACE(src);
        SemanticModel const c = analyzeWithLayout(src);
        EXPECT_EQ(countCode(c.diagnostics(), DiagnosticCode::S_IncompatibleRedeclaration), 1u) << everyDiagnostic(c);
    }
}

// A FUNCTION DESIGNATOR DECAYS TO A POINTER TO ANY COMPATIBLE FUNCTION TYPE (P69 lane `cs`, fold f26b). C 6.3.2.1p4
// makes the designator a pointer to its own type, and C 6.5.16.1p1 lets that reach a pointer to a COMPATIBLE one
// (6.7.6.3p15) — a result or a parameter pointing to an array of unknown, variable or known size beside one of another
// of those (6.7.6.2p6), at any depth. ✔MEASURED (lane `cs` probe ft; gnu runs 20261022-012401-745eac15 and
// 20261022-012430-1cff4804, mingw 20261022-012501-bc135a7b, msvc 20261022-012508-b142c317): gcc 13.3.0 and clang
// 18.1.3 at -pedantic-errors and mingw-w64 gcc 13.2.0 build and run every cell below in silence (cl 19.51 runs the
// constant-size ones with C4048 / C4113, and has no run-time bound); DSS warned each conversion of a designator an
// incompatible pointer conversion — the decay asked for the IDENTICAL signature. `_Generic` asks the same relation:
// gcc, clang and mingw select the compatible association (cl selects `default`; DECIDED for C 6.5.1.1p2 and the three).
// CONTROLS: two known sizes in a result or a parameter, another element type — gcc and clang refuse each, cl builds
// each with a warning, and here each is still warned.
TEST(IncompleteArrayType, AFunctionDecaysToAPointerToACompatibleFunctionType) {
    constexpr char kPrelude[] = "int store[8];\n"
                                "int (*gu(void))[] { return (int (*)[])store; }\n"
                                "int (*g3(void))[3] { return (int (*)[3])store; }\n"
                                "int hu(int (*p)[]) { return (*p)[1]; }\n"
                                "int h3(int (*p)[3]) { return (*p)[1]; }\n"
                                "int apply3(int (*(*cb)(void))[3]) { return (*cb())[1]; }\n";
    for (char const* cell : {
             // a result, unknown -> known size, and the reverse
             "int c1(void) { int (*(*fp)(void))[3] = gu; return (*fp())[1]; }\n",
             "int c2(void) { int (*(*fp)(void))[] = g3; return (*fp())[1]; }\n",
             // a result against a run-time bound, both ways (lane `lm`'s shape)
             "int c3(int m) { int (*(*fp)(void))[m] = gu; return (*fp())[1]; }\n",
             "int c4(int m) { int (*(*fp)(void))[m] = g3; return (*fp())[1]; }\n",
             // a parameter, both ways, and against a run-time bound
             "int c5(void) { int (*fp)(int (*)[3]) = hu; return fp((int (*)[3])store); }\n",
             "int c6(void) { int (*fp)(int (*)[]) = h3; return fp((int (*)[])store); }\n",
             "int c7(int m) { int (*fp)(int (*)[m]) = hu; return fp((int (*)[m])store); }\n",
             // a function returning a pointer to a function returning a pointer to an array
             "int (*(*ggu(void))(void))[] { return gu; }\n"
             "int c8(void) { int (*(*(*fpp)(void))(void))[3] = ggu; return (*fpp()())[1]; }\n",
             // an assignment, a call argument and a return statement
             "int c9(void) { int (*(*fp)(void))[3] = 0; fp = gu; return (*fp())[1]; }\n",
             "int c10(void) { return apply3(gu); }\n",
             "int (*(*pick(void))(void))[3] { return gu; }\n",
             // a result and a parameter each knowing a size the other does not
             "int (*hm(int (*p)[2]))[] { return (int (*)[])*p; }\n"
             "int c22(void) { int (*(*fp)(int (*)[]))[3] = hm; return (*fp((int (*)[])store))[1]; }\n",
             // a redeclaration of a parameter that points to such a function
             "int k(int (*(*cb)(void))[]);\nint k(int (*(*cb)(void))[3]) { return (*cb())[1]; }\n",
         }) {
        std::string const src = std::string{kPrelude} + cell;
        SCOPED_TRACE(cell);
        SemanticModel const m = analyzeWithLayout(src);
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_FALSE(hasDiagnosedPointerConversion(m.diagnostics()))
            << "a designator of a compatible function type was warned an incompatible pointer conversion"
            << everyDiagnostic(m);
    }
    // `_Generic`: the designator (a pointer to a function returning `int (*)[]`) selects the association of a pointer
    // to a function returning `int (*)[3]` — 13 elements, where `default` would give one.
    {
        SemanticModel const m = analyzeWithLayout(std::string{kPrelude}
                                                  + "char probe[_Generic(gu, int (*(*)(void))[3]: 13, default: 1)];\n");
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_EQ(arrayLength(m, "probe"), std::optional<std::int64_t>{13})
            << "_Generic did not select the compatible association" << everyDiagnostic(m);
    }
    for (char const* cell : {
             "int d18(void) { int (*(*fp)(void))[4] = g3; return (*fp())[1]; }\n",
             "int d19(void) { long (*(*fp)(void))[] = gu; return (int)(*fp())[0]; }\n",
             "int d20(void) { int (*fp)(int (*)[4]) = h3; return fp((int (*)[4])store); }\n",
         }) {
        std::string const src = std::string{kPrelude} + cell;
        SCOPED_TRACE(cell);
        SemanticModel const m = analyzeWithLayout(src);
        EXPECT_TRUE(hasDiagnosedPointerConversion(m.diagnostics()))
            << "CONTROL: an incompatible function type was converted in silence" << everyDiagnostic(m);
    }
}

TEST(IncompleteArrayType, AFileScopeTentativeArrayIsCompletedToOneElementOrByItsDefinition) {
    {
        SemanticModel const m = analyzeWithLayout("int v[];\n");
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_TentativeArrayAssumedOneElement), 1u)
            << everyDiagnostic(m);
        EXPECT_EQ(arrayLength(m, "v"), std::optional<std::int64_t>{1}) << "C 6.9.2p5: one element";
    }
    for (char const* src : {"int v[];\nint v[10];\n", "int v[10];\nint v[];\n", "int v[];\nint v[10] = {1};\n"}) {
        SCOPED_TRACE(src);
        SemanticModel const m = analyzeWithLayout(src);
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_TentativeArrayAssumedOneElement), 0u)
            << "the unit completes it" << everyDiagnostic(m);
        EXPECT_EQ(arrayLength(m, "v"), std::optional<std::int64_t>{10});
    }
    // From the declaration that completes it on, the name has the composite type (C 6.2.7p4): a bound
    // written after it reads ten elements, whichever declaration came first (✔MEASURED, probe fs cell 2).
    for (char const* src : {"int v[];\nint v[10];\nchar probe[sizeof v + 1];\n",
                            "int v[10];\nint v[];\nchar probe[sizeof v + 1];\n",
                            "extern int v[];\nint v[10];\nchar probe[sizeof v + 1];\n"}) {
        SCOPED_TRACE(src);
        SemanticModel const m = analyzeWithLayout(src);
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_SizeofIncompleteArrayIsZero), 0u)
            << everyDiagnostic(m);
        EXPECT_EQ(arrayLength(m, "probe"), std::optional<std::int64_t>{41}) << everyDiagnostic(m);
    }
}

TEST(IncompleteArrayType, ABlockScopeObjectOfUnknownSizeIsRefused) {
    SemanticModel const m = analyzeWithLayout("void f(void) { int a[]; (void)a; }\n");
    EXPECT_TRUE(m.hasErrors());
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_IncompleteTypeObject), 1u) << everyDiagnostic(m);
    // CONTROLS: completed by its initializer, or only declared, it is no object of incomplete type.
    for (char const* src : {"void f(void) { int a[] = {1, 2}; (void)a; }\n",
                            "void f(void) { extern int a[]; (void)a; }\n"}) {
        SCOPED_TRACE(src);
        SemanticModel const c = analyzeWithLayout(src);
        EXPECT_FALSE(c.hasErrors()) << everyDiagnostic(c);
    }
}

// A member whose bound does not fold is no flexible member: a member cannot have a variably modified
// type (C 6.7.2.1p9), and the bound is refused by name. The flexible-member reading laid the structure
// out with no room for the elements the program indexes — and beside another member it was ACCEPTED.
// ✔MEASURED (lane `cs`, probe vm): at file scope gcc 13.3.0, clang 18.1.3 and cl 19.51 refuse it; at
// BLOCK scope gcc builds and runs it (its documented extension) where clang and cl refuse. That build is
// owed and not yet made, so the block-scope cell below is a refusal BY NAME, never a wrong layout, and it
// becomes an acceptance the day the member is built.
TEST(IncompleteArrayType, AMemberWithARuntimeBoundIsRefusedNotReadAsAFlexibleMember) {
    for (char const* src : {
             "int n;\nstruct S { int x; int a[n]; };\n",
             "void f(int n) { struct S { int x; int a[n]; } s; s.x = 0; (void)s; }\n",
             "int n;\nstruct S { int x; int a[2][n]; };\n",
         }) {
        SCOPED_TRACE(src);
        SemanticModel const m = analyzeWithLayout(src);
        EXPECT_TRUE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_NonConstantArrayLength), 1u) << everyDiagnostic(m);
    }
    // CONTROLS: an absent bound on the last member IS the flexible member, and a constant one folds.
    for (char const* src : {"struct S { int x; int a[]; };\n", "enum { N = 3 };\nstruct S { int x; int a[N]; };\n"}) {
        SCOPED_TRACE(src);
        SemanticModel const m = analyzeWithLayout(src);
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
    }
}
