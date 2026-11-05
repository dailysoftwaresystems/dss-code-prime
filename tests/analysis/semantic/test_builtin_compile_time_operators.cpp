// P31 (2026-08-24) — the four GNU COMPILE-TIME operand forms this cycle landed,
// pinned where a corpus example structurally cannot pin them: on the REFUSALS,
// and on the exact DIAGNOSTIC CODE each refusal carries.
//
//   * [[D-CSUBSET-ALIGNOF-VALUE-OPERAND]]  `_Alignof e` / `__alignof__ e`
//   * [[D-FFI-OFFSETOF-MACRO]]             `__builtin_offsetof(T, m)`
//   * `__builtin_types_compatible_p(A, B)`
//   * `__builtin_choose_expr(c, a, b)`
//
// ★★ WHY A UNIT SUITE BESIDE THREE RUNNABLE EXAMPLES, RATHER THAN INSTEAD OF
// THEM. A corpus example must BUILD, so it can only ever exercise the ACCEPTING
// half of a feature. Every construct here has a refusing half that matters at
// least as much — an unknown member, a bit-field with no byte offset, a
// non-constant `__builtin_choose_expr` condition — and the property worth pinning
// there is not "the build failed" (a typo achieves that) but "the build failed
// with THIS code, and for THIS reason". Only a unit test can assert that.
//
// ★★ THE SECOND THING THIS FILE OWNS IS THE CONST-EXPR TIER, WHICH THE EXAMPLES
// REACH ONLY INDIRECTLY. `_Static_assert` and an array dimension fold through
// `constIntExpr` at Pass 1.5 — a DIFFERENT consumer from the Pass-2 + HIR path a
// runtime expression takes — and `analyze()`'s direct-API default supplies NO
// `aggregateLayout`, so these pins pass one explicitly (the shape every
// layout-dependent pin in `test_semantic_analyzer_c.cpp` already uses). Without
// it a fold silently declines and a pin that expected a refusal would pass for
// the wrong reason.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::sem_test;

namespace {

// Every pin needs the target's aggregate-layout params, for the reason the file
// header states. `Natural` + 16 is the shape the shipped x86_64/arm64 targets
// declare and the one every sibling layout pin in this directory already uses.
[[nodiscard]] SemanticModel analyzeWithLayout(std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
}

// The dimension the analyzer folded for the array named `name`. A fold that
// DECLINED leaves the symbol un-arrayed or un-sized, so a pin that reads a
// dimension is also a pin that the fold happened at all — which is what makes
// these assertions detect a silent decline rather than only a wrong value.
//
// ⚠ `model.symbols()` IS ONE FLAT LIST ACROSS EVERY SCOPE, STRUCT MEMBERS
// INCLUDED — a trap this helper was written wrong for first, and the failure was
// a green-looking nullopt rather than an error. A fixture whose struct has a
// member `a` and whose array is also `a` makes a first-match-wins lookup return
// the MEMBER (an `int`, not an array), so the pin reports "the fold declined"
// when the fold in fact succeeded. Scanning for the unique ARRAY-typed match
// makes the shadowing harmless instead of silently misleading, and the fixtures
// below still use distinctive names so the ambiguity does not arise at all.
[[nodiscard]] std::optional<std::int64_t>
foldedArrayDim(SemanticModel const& model, std::string_view name) {
    auto const& ti = model.lattice().interner();
    std::optional<std::int64_t> found;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name != name) continue;
        TypeId const t = model.symbols()[i].type;
        if (!t.valid() || ti.kind(t) != TypeKind::Array) continue;
        auto const sc = ti.scalars(t);
        if (sc.empty()) continue;
        if (found.has_value() && *found != sc[0]) return std::nullopt;  // ambiguous
        found = sc[0];
    }
    return found;
}

} // namespace


// ── [[D-CSUBSET-ALIGNOF-VALUE-OPERAND]] ──────────────────────────────────────

// ★ THE REGRESSION PIN, AND IT IS FIRST BECAUSE IT GUARDS A FORM THAT ALREADY
// SHIPPED. Landing the value arm moved `hirLowering.alignofRule` from the FORM
// (`alignofType`) onto the new `alignofExpr` WRAPPER, and the Pass-1.5 const-expr
// fold dispatches on that id: a fix that taught only the lowering to descend
// through the wrapper leaves THIS fold matching nothing, and the already-shipped
// `int a[_Alignof(double)]` starts failing S_NonConstantArrayLength. No test of
// the new feature would notice.
TEST(BuiltinCompileTimeOperators, AlignofTypeStillFoldsInAnArrayDimension) {
    auto model = analyzeWithLayout(
        "int a[_Alignof(double)];\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 0u)
        << "the alignof TYPE form must still fold after the value arm landed";
    EXPECT_EQ(foldedArrayDim(model, "a"), 8)
        << "_Alignof(double) is 8 — a declined fold shows up here as nullopt";
}

// The value operand in the same const-expr position. `*(double *)0` is legal
// because the operand is UNEVALUATED (C 6.5.3.4p1) — only its type is read.
TEST(BuiltinCompileTimeOperators, AlignofValueOperandFoldsInAnArrayDimension) {
    auto model = analyzeWithLayout(
        "int a[__alignof__(*(double *)0)];\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_NonConstantArrayLength), 0u)
        << "__alignof__(expr) must fold in a constant expression";
    EXPECT_EQ(foldedArrayDim(model, "a"), 8);
}

// ★ THE OPERAND'S OWN TYPE, NOT A CHILD'S. `*p` on a `char *` is 1; the reading
// this pin rejects — a descent to the first stamped leaf — answers 8. That
// descent is not hypothetical: it is
// [[D-CSUBSET-SIZEOF-DEREF-ARRAY-SILENT-FALLBACK]], which mis-sized `sizeof(*p)`
// and reached a shipped SQLite as a heap corruption.
TEST(BuiltinCompileTimeOperators, AlignofValueOperandReadsThePointeeNotThePointer) {
    auto model = analyzeWithLayout(
        "char b[4];\n"
        "char *p = b;\n"
        "int a[__alignof__(*(char *)0)];\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(foldedArrayDim(model, "a"), 1)
        << "__alignof__(*(char *)0) is the POINTEE's alignment (1), never the "
           "pointer's";
}

// ★ THE ONE SHAPE WHERE ALIGNOF AND SIZEOF DISAGREE — the sole detector for a
// paste of the sizeof value arm into the alignof one. Every other alignof pin in
// this file passes under both readings.
TEST(BuiltinCompileTimeOperators, AlignofValueOperandIsNotSizeof) {
    auto model = analyzeWithLayout(
        "int a[__alignof__(*(char (*)[3])0)];\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(foldedArrayDim(model, "a"), 1)
        << "a char[3] aligns to 1 and SIZES 3 — reading size here yields 3";
}


// ── [[D-FFI-OFFSETOF-MACRO]] ─────────────────────────────────────────────────

TEST(BuiltinCompileTimeOperators, OffsetofFoldsInAConstantExpression) {
    auto model = analyzeWithLayout(
        "struct S { int a; int b; double c; };\n"
        "int dss_dim[__builtin_offsetof(struct S, c)];\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_OffsetofInvalidMember), 0u);
    EXPECT_EQ(foldedArrayDim(model, "dss_dim"), 8)
        << "`double c` follows two ints and is padded to 8";
}

// A nested `.field` step and an `[index]` step, in ONE designator. The index step
// must multiply by the element STRIDE: `r` sits at 12 and `r[2]` at 20, so a step
// that added the index itself would answer 14.
TEST(BuiltinCompileTimeOperators, OffsetofWalksAMixedMemberDesignator) {
    auto model = analyzeWithLayout(
        "struct I { int x; int y; };\n"
        "struct O { int p; struct I q; int r[4]; };\n"
        "int a[__builtin_offsetof(struct O, r[2])];\n"
        "int b[__builtin_offsetof(struct O, q.y)];\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(foldedArrayDim(model, "a"), 20)
        << "r is at 12; index 2 of an int[4] adds the element STRIDE (8)";
    EXPECT_EQ(foldedArrayDim(model, "b"), 8);
}

// ★ REFUSAL 1 — an unknown member. The value is UNKNOWABLE, and `offsetof` feeds
// pointer arithmetic, so the only safe answer is a refusal: a fabricated 0 would
// be a wrong ADDRESS in a program that built and ran. ✔MEASURED that gcc 13.3.0,
// clang 18.1.3 and clang 19.1.1 reject this shape too.
TEST(BuiltinCompileTimeOperators, OffsetofUnknownMemberFailsLoud) {
    auto model = analyzeWithLayout(
        "struct S { int a; };\n"
        "int main(void){ return (int)__builtin_offsetof(struct S, nosuch); }\n");
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_OffsetofInvalidMember), 1u)
        << "an unknown member must refuse — never fold to a plausible 0";
}

// ★ REFUSAL 2 — a BIT-FIELD, which has no byte offset at all (and whose address
// C 6.5.3.2 forbids taking). This is the refusal most likely to be lost to a
// well-meaning simplification, because the member DOES exist and the layout
// engine DOES have a record for it — what it has is a bit position, not a byte
// one. ✔MEASURED that all three reference compilers reject it.
TEST(BuiltinCompileTimeOperators, OffsetofBitFieldMemberFailsLoud) {
    auto model = analyzeWithLayout(
        "struct S { unsigned a : 3; unsigned b : 5; };\n"
        "int main(void){ return (int)__builtin_offsetof(struct S, b); }\n");
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_OffsetofInvalidMember), 1u)
        << "a bit-field has no BYTE offset — refuse rather than report the "
           "storage unit's";
}

// ★ REFUSAL 3 — a non-constant `[index]` step. Folding it would need a runtime
// value at compile time; assuming 0 would silently address the wrong element.
TEST(BuiltinCompileTimeOperators, OffsetofNonConstantIndexFailsLoud) {
    auto model = analyzeWithLayout(
        "struct S { int r[4]; };\n"
        "int main(void){ int i = 1; return (int)__builtin_offsetof(struct S, r[i]); }\n");
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_OffsetofInvalidMember), 1u)
        << "a non-constant designator index must refuse, never assume 0";
}


// ── [[D-C-A-CONSTANTS-MEMBER-NAME-IS-RESOLVED-BY-A-SCOPE-WALK]] (P69, lane `cs`) ──
//
// The member name of a constant — each step of `offsetof`, the `&((T *)0)->m`
// spelling, the member step of an object-size operand — is resolved by the ONE
// member rule (`anon_member_search::memberByteOffset`): the composite's OWN member,
// or one promoted through its anonymous members, the composite keyed by its
// unqualified type. Before that function each asker walked the SCOPE CHAIN from the
// member scope, which is wrong both ways, and every pin below is one of the shapes
// ✔MEASURED 2026-10-08 against gcc 13.3.0, clang 18.1.3, mingw-w64 gcc 13.2.0 and
// cl 14.51 (the lane's probe `mo1`).

namespace {

// The text of every diagnostic of `code`, in emission order.
[[nodiscard]] std::vector<std::string> textsOf(SemanticModel const& model, DiagnosticCode code) {
    std::vector<std::string> out;
    for (ParseDiagnostic const& d : model.diagnostics().all())
        if (d.code == code) out.push_back(d.actual);
    return out;
}

// Did the bound of the array named `name` fold to a LENGTH? A declined fold leaves
// the array without one — no array type at all, or the lattice's negative "not a
// constant" length — so a pin that a constant must NOT fold reads this, not a value.
[[nodiscard]] bool boundFoldedToALength(SemanticModel const& model, std::string_view name) {
    auto const dim = foldedArrayDim(model, name);
    return dim.has_value() && *dim >= 0;
}

} // namespace

// ★ FACE 1 — A NAME THAT IS NOT A MEMBER, BUT IS BOUND IN A SCOPE THAT ENCLOSES THE
// STRUCT. The walk found it there and read ITS `fieldIndex` as a member's: `offsetof`
// COMPILED and answered another member's offset (0 for the first three shapes, 4 for
// the last two) where all four references say "no member named". Each shape is
// refused BY NAME, and — the half a count cannot see — its array bound does not fold.
TEST(BuiltinCompileTimeOperators, OffsetofRefusesANameBoundOutsideTheStruct) {
    struct Shape {
        char const* what;
        char const* declarations;
        char const* type;
        char const* name;
    };
    Shape const shapes[] = {
        {"a file-scope object", "int g;\nstruct S { int a; int b; };\n", "struct S", "g"},
        {"a function", "int fn(void);\nstruct S { int a; int b; };\n", "struct S", "fn"},
        {"an enumerator of the enclosing scope",
         "enum { ENUMERATOR = 1 };\nstruct S { int a; int b; };\n", "struct S", "ENUMERATOR"},
        {"an enumerator declared INSIDE the struct",
         "struct S { enum { INNER = 1 } e; int b; };\n", "struct S", "INNER"},
        {"a typedef name", "typedef int alias_t;\nstruct S { int a; int b; };\n", "struct S",
         "alias_t"},
        {"a member of the ENCLOSING struct",
         "struct O { int first; struct I { int x; int y; } i; };\n", "struct I", "i"},
    };
    for (Shape const& shape : shapes) {
        SCOPED_TRACE(shape.what);
        std::string const call = std::string{"__builtin_offsetof("} + shape.type + ", "
                               + shape.name + ")";
        // (a) the refusal, by name and by reason.
        auto const asked = analyzeWithLayout(
            std::string{shape.declarations} + "int main(void){ return (int)" + call + "; }\n");
        auto const texts = textsOf(asked, DiagnosticCode::S_OffsetofInvalidMember);
        ASSERT_EQ(texts.size(), 1u) << "exactly one refusal of the designator";
        EXPECT_NE(texts[0].find(std::string{"'"} + shape.name
                                + "' is not a member of that struct/union"),
                  std::string::npos)
            << texts[0];
        // (b) no plausible offset reaches a constant: the bound does not fold.
        auto const bound = analyzeWithLayout(
            std::string{shape.declarations} + "int dss_dim[" + call + " + 1];\n"
            "int main(void){ return 0; }\n");
        EXPECT_TRUE(bound.diagnostics().hasErrors());
        EXPECT_FALSE(boundFoldedToALength(bound, "dss_dim"))
            << "the walk folded this to another member's offset plus one: "
            << foldedArrayDim(bound, "dss_dim").value_or(-1);
    }
}

// ★ FACE 2 — A MEMBER REACHED THROUGH ANONYMOUS MEMBERS (C23 6.7.3.2p15: it IS a
// member of the containing struct). Refused as "not a member" until the member rule
// was asked; every reference folds each of these. The offsets are SUMS along the
// anonymous members crossed: `c` is 4 + 4, `f` is 16 + 0 + 2 — an answer that read
// only the member's own index inside its anonymous struct would say 4 and 2.
TEST(BuiltinCompileTimeOperators, OffsetofReachesAMemberThroughAnonymousMembers) {
    auto model = analyzeWithLayout(
        "struct A { char a; struct { int b; int c; };\n"
        "           union { long long d; struct { char e; short f; }; }; };\n"
        "struct B { struct A in; char tail; };\n"
        "struct T { char lead; struct A wide[2]; };\n"
        "int d_one[__builtin_offsetof(struct A, c)];\n"
        "int d_two[__builtin_offsetof(struct A, f)];\n"
        "int d_dotted[__builtin_offsetof(struct B, in.c)];\n"
        "int d_indexed[__builtin_offsetof(struct T, wide[1].c)];\n"
        "int d_address[(unsigned long)&((struct A *)0)->f];\n"
        "_Static_assert((unsigned long)&((struct A *)0)->c == 8, \"c\");\n"
        "_Static_assert(__builtin_offsetof(struct A, d) == 16, \"d\");\n"
        "int main(void){ return 0; }\n");
    EXPECT_FALSE(model.diagnostics().hasErrors())
        << (model.diagnostics().all().empty() ? std::string{}
                                              : model.diagnostics().all()[0].actual);
    EXPECT_EQ(foldedArrayDim(model, "d_one"), 8) << "one anonymous level";
    EXPECT_EQ(foldedArrayDim(model, "d_two"), 18)
        << "two: a struct inside a union inside the struct";
    EXPECT_EQ(foldedArrayDim(model, "d_dotted"), 8) << "a later step of a dotted designator";
    EXPECT_EQ(foldedArrayDim(model, "d_indexed"), 40)
        << "after a subscript: wide is at 8, one element is 24, c adds 8";
    EXPECT_EQ(foldedArrayDim(model, "d_address"), 18) << "the `&((T *)0)->m` spelling";
}

// A BIT-FIELD inside an anonymous member is found — and refused as what it is. The
// walk said "not a member", which is false; the address spelling must not fold it
// either.
TEST(BuiltinCompileTimeOperators, OffsetofNamesABitFieldInsideAnAnonymousMember) {
    auto const asked = analyzeWithLayout(
        "struct F { int a; struct { int bf : 3; int z; }; };\n"
        "int main(void){ return (int)__builtin_offsetof(struct F, bf); }\n");
    auto const texts = textsOf(asked, DiagnosticCode::S_OffsetofInvalidMember);
    ASSERT_EQ(texts.size(), 1u);
    EXPECT_NE(texts[0].find("'bf' is a BIT-FIELD"), std::string::npos) << texts[0];

    auto const address = analyzeWithLayout(
        "struct F { int a; struct { int bf : 3; int z; }; };\n"
        "int dss_dim[(unsigned long)&((struct F *)0)->bf + 1];\n"
        "int main(void){ return 0; }\n");
    EXPECT_TRUE(address.diagnostics().hasErrors());
    EXPECT_FALSE(boundFoldedToALength(address, "dss_dim"));
}

// Two DIFFERENT members of one spelling through sibling anonymous members: neither
// is THE member, and the designator says so rather than taking the first.
TEST(BuiltinCompileTimeOperators, OffsetofRefusesAnAmbiguousPromotedMember) {
    auto const model = analyzeWithLayout(
        "struct M { struct { int x; int p; }; struct { int q; int x; }; };\n"
        "int main(void){ return (int)__builtin_offsetof(struct M, x); }\n");
    auto const texts = textsOf(model, DiagnosticCode::S_OffsetofInvalidMember);
    ASSERT_EQ(texts.size(), 1u);
    EXPECT_NE(texts[0].find("'x' names two different members"), std::string::npos) << texts[0];
}

// ★ FACE 3 — A QUALIFIED CONTAINER. The member scope is registered under the
// UNQUALIFIED type; the walk looked it up under the qualified one, found none, and
// called a complete struct "INCOMPLETE here". All four references fold these.
TEST(BuiltinCompileTimeOperators, OffsetofSeesThroughTheContainersQualifiers) {
    auto model = analyzeWithLayout(
        "struct S { int a; int b; };\n"
        "struct O { char lead; volatile struct S in; };\n"
        "int d_volatile[__builtin_offsetof(volatile struct S, b)];\n"
        "int d_both[__builtin_offsetof(const volatile struct S, b)];\n"
        "int d_member[__builtin_offsetof(struct O, in.b)];\n"
        "int d_address[(unsigned long)&((volatile struct S *)0)->b];\n"
        "int d_address_both[(unsigned long)&((const volatile struct S *)0)->b];\n"
        "int main(void){ return 0; }\n");
    EXPECT_FALSE(model.diagnostics().hasErrors())
        << (model.diagnostics().all().empty() ? std::string{}
                                              : model.diagnostics().all()[0].actual);
    EXPECT_EQ(foldedArrayDim(model, "d_volatile"), 4);
    EXPECT_EQ(foldedArrayDim(model, "d_both"), 4);
    EXPECT_EQ(foldedArrayDim(model, "d_member"), 8)
        << "a volatile-qualified MEMBER on the way of a dotted designator";
    EXPECT_EQ(foldedArrayDim(model, "d_address"), 4);
    EXPECT_EQ(foldedArrayDim(model, "d_address_both"), 4);
}

// ★ FACE 4 — THE OBJECT-SIZE BUILTIN'S MEMBER OPERAND. "Unknown" is an answer the
// builtin is allowed to give, so nothing was wrong — but gcc, clang and mingw-w64 gcc
// answer the subobject's bytes for a promoted member, and so does the member rule.
TEST(BuiltinCompileTimeOperators, ObjectSizeSizesAMemberPromotedThroughAnonymousMembers) {
    auto model = analyzeWithLayout(
        "struct A { char a; struct { int b; int c; };\n"
        "           union { long long d; struct { char e; short f; }; }; };\n"
        "struct A ga;\n"
        "int d_closest[__builtin_object_size(&ga.c, 1)];\n"
        "int d_whole[__builtin_object_size(&ga.f, 0)];\n"
        "int d_direct[__builtin_object_size(&ga.a, 1)];\n"
        "int main(void){ return 0; }\n");
    EXPECT_FALSE(model.diagnostics().hasErrors())
        << (model.diagnostics().all().empty() ? std::string{}
                                              : model.diagnostics().all()[0].actual);
    EXPECT_EQ(foldedArrayDim(model, "d_closest"), 4) << "the subobject `c` itself";
    EXPECT_EQ(foldedArrayDim(model, "d_whole"), 6) << "from `f` (at 18) to the end of 24";
    EXPECT_EQ(foldedArrayDim(model, "d_direct"), 1) << "the control: a direct member";
}

// `offsetof` KEEPS THE SENTENCE FOR EACH WAY THERE IS NO ANSWER. The texts moved
// from five inline branches to one switch over the member rule's reason; each
// reachable reason is asked for by name.
TEST(BuiltinCompileTimeOperators, OffsetofStillSaysWhyThereIsNoOffset) {
    struct Case {
        char const* source;
        char const* text;
    };
    Case const cases[] = {
        {"struct S { int a; };\n"
         "int main(void){ return (int)__builtin_offsetof(struct S, a.x); }\n",
         "'x' cannot be reached: the type it is looked up in is not a struct or union"},
        {"struct Later;\n"
         "int main(void){ return (int)__builtin_offsetof(struct Later, m); }\n",
         "'m' cannot be reached: its containing struct/union is INCOMPLETE here"},
        {"struct S { int a; };\n"
         "int main(void){ return (int)__builtin_offsetof(struct S, nosuch); }\n",
         "'nosuch' is not a member of that struct/union"},
        {"struct S { unsigned a : 3; unsigned b : 5; };\n"
         "int main(void){ return (int)__builtin_offsetof(struct S, b); }\n",
         "'b' is a BIT-FIELD, which has no byte offset"},
    };
    for (Case const& c : cases) {
        SCOPED_TRACE(c.text);
        auto const model = analyzeWithLayout(c.source);
        auto const texts = textsOf(model, DiagnosticCode::S_OffsetofInvalidMember);
        ASSERT_EQ(texts.size(), 1u);
        EXPECT_NE(texts[0].find(c.text), std::string::npos) << texts[0];
    }
}


// ── `__builtin_types_compatible_p` ───────────────────────────────────────────

// ★ THE CROSS-DATA-MODEL PROPERTY, PINNED ON THE MODEL WHERE IT BREAKS. Under
// LLP64 `long` and `int` share a REPRESENTATION, so a compatibility test written
// as a width comparison answers 1 here and 0 under LP64 — from one source, with
// no diagnostic anywhere. C compatibility is TYPE IDENTITY, which
// [[D-LANG-TYPE-IDENTITY-VOCABULARY]] made the interned identity carry, so the
// answer is 0 under BOTH. The LP64 twin below is the control: a pin that only
// ran under LLP64 could pass because the fold declined entirely.
TEST(BuiltinCompileTimeOperators, TypesCompatibleIsIdentityNotRepresentationLlp64) {
    auto cu = buildShippedUnit("c", {
        "int a[__builtin_types_compatible_p(int, long) + 1];\n"
        "int main(void){ return 0; }\n"});
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Llp64,
                         AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(foldedArrayDim(model, "a"), 1)
        << "int and long are DISTINCT types under LLP64 too, where they happen "
           "to share a representation — the answer is 0, so the array is [1]";
}

TEST(BuiltinCompileTimeOperators, TypesCompatibleIsIdentityNotRepresentationLp64) {
    auto model = analyzeWithLayout(
        "int a[__builtin_types_compatible_p(int, long) + 1];\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(foldedArrayDim(model, "a"), 1);
}

// A typedef IS its underlying type; two different pointee types are not.
TEST(BuiltinCompileTimeOperators, TypesCompatibleSeesThroughATypedef) {
    auto model = analyzeWithLayout(
        "typedef int myint;\n"
        "int a[__builtin_types_compatible_p(int, myint) + 1];\n"
        "int b[__builtin_types_compatible_p(char *, int *) + 1];\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(foldedArrayDim(model, "a"), 2) << "compatible ⇒ 1, so the array is [2]";
    EXPECT_EQ(foldedArrayDim(model, "b"), 1) << "incompatible ⇒ 0, so the array is [1]";
}


// ── `__builtin_choose_expr` ──────────────────────────────────────────────────

TEST(BuiltinCompileTimeOperators, ChooseExprSelectsTheConstantConditionsArm) {
    auto model = analyzeWithLayout(
        "int a[__builtin_choose_expr(1, 4, 9)];\n"
        "int b[__builtin_choose_expr(0, 4, 9)];\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(foldedArrayDim(model, "a"), 4);
    EXPECT_EQ(foldedArrayDim(model, "b"), 9);
}

// ★ THE DISCARDED ARM IS NEVER VISITED, PROVEN BY PUTTING SOMETHING FATAL IN IT.
// If the const-eval engine walked both arms it would trip its own divide-by-zero
// wall and decline the dimension; folding to 4 is possible ONLY if the arm is
// never evaluated at all. This is the sharpest available statement of
// "unevaluated", because a merely-unused arm would still be visited.
TEST(BuiltinCompileTimeOperators, ChooseExprNeverEvaluatesTheDiscardedArm) {
    auto model = analyzeWithLayout(
        "int a[__builtin_choose_expr(1, 4, 1 / 0)];\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(foldedArrayDim(model, "a"), 4)
        << "a divide-by-zero in the DISCARDED arm must not be evaluated";
}

// ★ REFUSAL — a non-constant condition. Reading it as false would pick the OTHER
// arm, and both arms are valid programs, so the failure would be a program that
// builds and does the wrong thing. ✔MEASURED that all three reference compilers
// reject it as well.
TEST(BuiltinCompileTimeOperators, ChooseExprNonConstantConditionFailsLoud) {
    auto model = analyzeWithLayout(
        "int main(void){ int v = 1; return __builtin_choose_expr(v, 1, 2); }\n");
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_BuiltinChooseExprNonConstant), 1u)
        << "a non-constant condition must refuse — never default to an arm";
}
