// ═══════════════════════════════════════════════════════════════════════════
//  AN ATTRIBUTE WRITTEN WHERE A TYPE IS WRITTEN — cycle P69, lane `cs`
//
//  Before this cycle the C grammar took an attribute in a declaration's prefix,
//  in its attribute slots and after a declarator. Everywhere else an attribute
//  can be written — among a type's specifiers, in a type name, in a pointer
//  layer, at the start of a parenthesized declarator, after a tag that is only
//  referred to — it was a parse error, or worse: `struct S __attribute__((aligned
//  (16))) w;` parsed, looked up a tag named `aligned`, and with a `struct aligned`
//  in scope `w` silently took THAT structure's size.
// ═══════════════════════════════════════════════════════════════════════════
//
// ── THE REFERENCE MATRIX (lane `cs`'s probes ta1..ta9, each cell compiled and
//    RUN through `dssharness run probe-reference-cc`, 2026-10-08) ─────────────
//  gcc 13.3.0 and clang 18.1.3 (`-std=c2x`, Linux x86_64) · Apple clang (macOS
//  arm64) · mingw-w64 gcc 13.2.0. gcc = mingw gcc on every cell quoted here.
//
//  IN A DECLARATION a run among the specifiers, or after a referred tag, is the
//  DECLARATION's — all four agree: `unsigned __attribute__((aligned(16))) int v;`
//  is a 4-byte object aligned to 16; `unsigned __attribute__((aligned(16))) int
//  *q;` a 16-aligned POINTER to a 4-aligned int; a member written so sits at
//  offset 16.
//
//  IN A TYPE NAME gcc applies `aligned` and clang ignores it with a warning;
//  this compiler takes gcc's reading (the fork and its cost are in the row):
//      __attribute__((aligned(16))) int            sizeof 4   _Alignof 16
//      __attribute__((aligned(16))) int *          8 / 16, pointee 4  (the WHOLE type)
//      int __attribute__((aligned(16))) *          8 / 16, pointee 4
//      int * __attribute__((aligned(16)))          8 / 16, pointee 4  (that pointer)
//      int * __attribute__((aligned(16))) *        8 / 8,  pointee 16
//      __attribute__((aligned(16))) int [3]        12 / 16
//      int (__attribute__((aligned(16))) *)(int)   8 / 8   (a function type takes none)
//      __attribute__((aligned(1))) int             4 / 1   (a type-level request lowers)
//      int [[gnu::aligned(16)]] *                  8 / 8,  pointee 16 (C23: the BASE)
//  and the over-aligned type stays COMPATIBLE with the plain one, at every depth
//  (all four references: `_Generic` selects it, `__builtin_types_compatible_p`
//  answers 1 for the types and for pointers to them).
//
//  INSIDE A DECLARATOR what an attribute decorates is decided by what follows
//  it (`nameAdjacentAttributeSpecifiers`): beside the declared name it is the
//  declared entity's, unless its row keeps it on the type (`aligned`, `packed`,
//  `unused`, `deprecated`); before another pointer layer it decorates a type.
//      int * __attribute__((weak)) p3 = 0, q3 = 0;      `V p3` / `B q3` on gcc and clang
//      static void * __attribute__((constructor)) i(void)  runs before main on all three
//      static int * __attribute__((warn_unused_result)) f(void)  warns at a discarded call
//      struct { char c; int * __attribute__((aligned(16))) m, n; }   offsets 16 and 24
//      struct { char c; int * __attribute__((packed)) m; }  gcc: m at 8, "ignored for
//                                                            type 'int *'"; clang: m at 1
//      int (__attribute__((aligned(16))) arr)[2];           8 bytes aligned to 16 on both
//      int (__attribute__((aligned(16))) *pf)(int);         gcc 8, clang 16
//
// Every pin below asserts a NUMBER a reference printed or a diagnostic a
// reference gives, never merely "no error": the defect this file exists for was
// a clean compile with the wrong size.

#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"   // the four pairs of the convention matrix
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include "semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::sem_test;

namespace {

// Layout parameters are REQUIRED: without them nothing is minted at all (the
// editor's no-parameters analysis must see exactly what it saw before), so a
// fixture without them would pass every "is it 16" pin vacuously red and every
// "is it unchanged" pin vacuously green.
constexpr AggregateLayoutParams kLayout{
    .scalarAlignment       = ScalarAlignmentRule::Natural,
    .maxAlignment          = 16,
    .maxRequestedAlignment = 268435456};

[[nodiscard]] SemanticModel analyzeC(std::initializer_list<std::string> srcs) {
    auto cu = buildShippedUnit("c", srcs);
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kLayout);
}

[[nodiscard]] SymbolRecord const* findSym(SemanticModel const& m, std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i)
        if (m.symbols()[i].name == name) return &m.symbols()[i];
    return nullptr;
}

struct SizeAlign {
    std::uint64_t size  = 0;
    std::uint32_t align = 0;
    friend bool operator==(SizeAlign const&, SizeAlign const&) = default;
};
std::ostream& operator<<(std::ostream& os, SizeAlign const& sa) {
    return os << "{size " << sa.size << ", align " << sa.align << "}";
}

[[nodiscard]] SizeAlign layoutOfType(SemanticModel const& m, TypeId t) {
    auto const l = computeLayout(t, m.lattice().interner(), kLayout, DataModel::Lp64);
    EXPECT_TRUE(l.has_value()) << "the type has no layout";
    return l ? SizeAlign{static_cast<std::uint64_t>(l->size), l->align.bytes()}
             : SizeAlign{};
}

// What a NAME's type lays out at. For an OBJECT this is the type's own answer:
// an alignment the declaration requested for the object (not for its type) is
// `SymbolRecord::explicitAlignment` and is asserted separately, because the two
// are different facts and the references keep them apart.
[[nodiscard]] SizeAlign layoutOf(SemanticModel const& m, char const* name) {
    SymbolRecord const* s = findSym(m, name);
    EXPECT_NE(s, nullptr) << "no symbol named '" << name << "'";
    return s == nullptr ? SizeAlign{} : layoutOfType(m, s->type);
}

// The pointee of a pointer-typed name, laid out.
[[nodiscard]] SizeAlign pointeeLayoutOf(SemanticModel const& m, char const* name) {
    SymbolRecord const* s = findSym(m, name);
    EXPECT_NE(s, nullptr) << "no symbol named '" << name << "'";
    if (s == nullptr) return {};
    TypeInterner const& in = m.lattice().interner();
    EXPECT_EQ(in.kind(s->type), TypeKind::Ptr) << "'" << name << "' is not a pointer";
    if (in.kind(s->type) != TypeKind::Ptr) return {};
    return layoutOfType(m, in.operands(s->type)[0]);
}

// `sizeof (T)` and `_Alignof (T)` of a TYPE NAME, read the way a program reads
// them: as the two array bounds they fold to. Going through the constant
// evaluator (rather than reading a symbol's type) is what makes these pins about
// the type NAME — the resolver's answer at the one position the grammar opened.
[[nodiscard]] SizeAlign sizeAlignOfTypeName(std::string const& typeName,
                                            std::string const& preamble = {}) {
    auto m = analyzeC({ preamble
                        + "char probe_size[sizeof(" + typeName + ")];\n"
                        + "char probe_align[_Alignof(" + typeName + ")];\n" });
    EXPECT_FALSE(m.hasErrors()) << "type name: " << typeName;
    return SizeAlign{layoutOf(m, "probe_size").size,
                     static_cast<std::uint32_t>(layoutOf(m, "probe_align").size)};
}

// The value an integer constant expression folds to, read as an array bound.
[[nodiscard]] std::uint64_t foldedTo(std::string const& expr,
                                     std::string const& preamble = {}) {
    auto m = analyzeC({ preamble + "char probe_value[" + expr + "];\n" });
    EXPECT_FALSE(m.hasErrors()) << "expression: " << expr;
    return layoutOf(m, "probe_value").size;
}

constexpr char const* kAl16 = "__attribute__((aligned(16)))";

} // namespace

// ── THE TAG FACE ───────────────────────────────────────────────────────────

TEST(AttributeTypePositions, ARunAfterAReferredTagIsTheDeclarationsAndNoTagName) {
    // THE DEFECT, with the colliding tag PRESENT: the name extraction took the
    // attribute's clause name for the tag, so `w` was a `struct aligned` — 64
    // bytes — in a program that compiled clean.
    auto m = analyzeC({
        "struct S { int a; };\n"
        "struct aligned { char c[64]; };\n"
        "struct S __attribute__((aligned(16))) w;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutOf(m, "w").size, 4u)
        << "`w` is a `struct S`; gcc, clang, Apple clang and mingw gcc all give 4";
    SymbolRecord const* w = findSym(m, "w");
    ASSERT_NE(w, nullptr);
    ASSERT_TRUE(w->explicitAlignment.has_value())
        << "the run is the DECLARATION's: every reference aligns `w` to 16";
    EXPECT_EQ(*w->explicitAlignment, 16u);
}

TEST(AttributeTypePositions, TheSameDeclarationWithNoCollidingTagInScope) {
    // The other half of the pair: with no `struct aligned` anywhere the old
    // reading had nothing to find. Same answer, for the same reason.
    auto m = analyzeC({
        "struct S { int a; };\n"
        "struct S __attribute__((aligned(16))) w;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutOf(m, "w").size, 4u);
    SymbolRecord const* w = findSym(m, "w");
    ASSERT_NE(w, nullptr);
    ASSERT_TRUE(w->explicitAlignment.has_value());
    EXPECT_EQ(*w->explicitAlignment, 16u);
}

TEST(AttributeTypePositions, AMemberWhoseTypeIsAReferredTagWithARunSitsWhereTheRunSays) {
    auto m = analyzeC({
        "struct S { int a; };\n"
        "struct aligned { char c[64]; };\n"
        "struct H { char c; struct S __attribute__((aligned(16))) m; };\n"
        "struct H h;\n" });
    EXPECT_FALSE(m.hasErrors());
    SymbolRecord const* h = findSym(m, "h");
    ASSERT_NE(h, nullptr);
    auto const l = computeLayout(h->type, m.lattice().interner(), kLayout,
                                 DataModel::Lp64);
    ASSERT_TRUE(l.has_value());
    ASSERT_EQ(l->fieldOffsets.size(), 2u);
    EXPECT_EQ(l->fieldOffsets[1], 16u) << "offsetof(struct H, m)";
    EXPECT_EQ(l->size, 32u);
    EXPECT_EQ(l->align.bytes(), 16u);
}

TEST(AttributeTypePositions, AKeywordNamedClauseIsNotTheQualifierItSpells) {
    // The MARKER-SCAN face of the same blindness: an attribute's clause name may
    // be spelled with a keyword token, and a scan for the `volatile` marker read
    // `__attribute__((volatile))` in a pointer layer as the qualifier.
    auto m = analyzeC({ "int * __attribute__((volatile)) p;\nint *q;\n" });
    SymbolRecord const* p = findSym(m, "p");
    SymbolRecord const* q = findSym(m, "q");
    ASSERT_NE(p, nullptr);
    ASSERT_NE(q, nullptr);
    EXPECT_FALSE(m.lattice().interner().isVolatileQualified(p->type))
        << "an attribute NAMED volatile is an unknown attribute, not a qualifier";
    EXPECT_EQ(p->type.v, q->type.v);
}

// ── A RUN AMONG THE SPECIFIERS OF A DECLARATION ────────────────────────────

TEST(AttributeTypePositions, ARunBetweenTwoSpecifiersAlignsTheDeclaredObject) {
    auto m = analyzeC({ "unsigned __attribute__((aligned(16))) int v;\nunsigned int plain;\n" });
    EXPECT_FALSE(m.hasErrors());
    SymbolRecord const* v = findSym(m, "v");
    SymbolRecord const* plain = findSym(m, "plain");
    ASSERT_NE(v, nullptr);
    ASSERT_NE(plain, nullptr);
    EXPECT_EQ(v->type.v, plain->type.v)
        << "the run decorates the DECLARATION; the type is `unsigned int`";
    ASSERT_TRUE(v->explicitAlignment.has_value());
    EXPECT_EQ(*v->explicitAlignment, 16u);
}

TEST(AttributeTypePositions, ARunBetweenTwoSpecifiersAlignsThePointerItDeclaresNotThePointee) {
    // `unsigned __attribute__((aligned(16))) int *q;` — 16 / 4 on all four.
    auto m = analyzeC({ "unsigned __attribute__((aligned(16))) int *q;\n" });
    EXPECT_FALSE(m.hasErrors());
    SymbolRecord const* q = findSym(m, "q");
    ASSERT_NE(q, nullptr);
    ASSERT_TRUE(q->explicitAlignment.has_value());
    EXPECT_EQ(*q->explicitAlignment, 16u);
    EXPECT_EQ(pointeeLayoutOf(m, "q"), (SizeAlign{4, 4}));
}

TEST(AttributeTypePositions, ARunBetweenTwoSpecifiersOfAMemberMovesTheMember) {
    auto m = analyzeC({
        "struct M { char c; unsigned __attribute__((aligned(16))) int m; };\n"
        "struct M g;\n" });
    EXPECT_FALSE(m.hasErrors());
    SymbolRecord const* g = findSym(m, "g");
    ASSERT_NE(g, nullptr);
    auto const l = computeLayout(g->type, m.lattice().interner(), kLayout,
                                 DataModel::Lp64);
    ASSERT_TRUE(l.has_value());
    ASSERT_EQ(l->fieldOffsets.size(), 2u);
    EXPECT_EQ(l->fieldOffsets[1], 16u);
    EXPECT_EQ(l->size, 32u);
}

TEST(AttributeTypePositions, ARunBetweenTwoSpecifiersOfATypedefAlignsTheAlias) {
    auto m = analyzeC({ "typedef unsigned __attribute__((aligned(16))) int T;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutOf(m, "T"), (SizeAlign{4, 16}));
}

TEST(AttributeTypePositions, ARunBetweenTwoSpecifiersDoesNotChangeWhichTypeTheyName) {
    // The specifier MULTISET must not count the run: `long __attribute__((unused))
    // long` is `long long`, 8 bytes — it read as `long`, 4 on an LLP64 pair, in
    // silence, when the grammar took the run and the multiset arm did not skip it.
    auto m = analyzeC({
        "long __attribute__((unused)) long a;\n"
        "unsigned __attribute__((unused)) long __attribute__((unused)) long b;\n"
        "long long c; unsigned long long d;\n" });
    EXPECT_FALSE(m.hasErrors());
    ASSERT_NE(findSym(m, "a"), nullptr);
    ASSERT_NE(findSym(m, "b"), nullptr);
    EXPECT_EQ(findSym(m, "a")->type.v, findSym(m, "c")->type.v);
    EXPECT_EQ(findSym(m, "b")->type.v, findSym(m, "d")->type.v);
}

// ── `aligned` IN A TYPE NAME ───────────────────────────────────────────────

TEST(AttributeTypePositions, TypeNameALeadingRunAlignsTheWholeNamedType) {
    EXPECT_EQ(sizeAlignOfTypeName(std::string{kAl16} + " int"), (SizeAlign{4, 16}));
    EXPECT_EQ(sizeAlignOfTypeName(std::string{kAl16} + " int *"), (SizeAlign{8, 16}))
        << "the POINTER is aligned: gcc gives 8 / 16 with a 4-aligned pointee";
    EXPECT_EQ(sizeAlignOfTypeName(std::string{kAl16} + " int [3]"), (SizeAlign{12, 16}));
}

TEST(AttributeTypePositions, TypeNameARunAfterTheBaseAlignsTheWholeNamedTypeToo) {
    EXPECT_EQ(sizeAlignOfTypeName(std::string{"int "} + kAl16), (SizeAlign{4, 16}));
    EXPECT_EQ(sizeAlignOfTypeName(std::string{"int "} + kAl16 + " *"), (SizeAlign{8, 16}));
    EXPECT_EQ(sizeAlignOfTypeName(std::string{"unsigned "} + kAl16 + " int"),
              (SizeAlign{4, 16}))
        << "a run BETWEEN two specifiers of a type name";
}

TEST(AttributeTypePositions, TypeNameAWholeTypeRunLeavesThePointeeAlone) {
    auto m = analyzeC({
        "typedef __typeof__(__attribute__((aligned(16))) int *) A;\n"
        "typedef __typeof__(int __attribute__((aligned(16))) *) B;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutOf(m, "A"), (SizeAlign{8, 16}));
    EXPECT_EQ(pointeeLayoutOf(m, "A"), (SizeAlign{4, 4}));
    EXPECT_EQ(layoutOf(m, "B"), (SizeAlign{8, 16}));
    EXPECT_EQ(pointeeLayoutOf(m, "B"), (SizeAlign{4, 4}));
}

TEST(AttributeTypePositions, TypeNameARunInAPointerLayerAlignsThatPointer) {
    auto m = analyzeC({
        "typedef __typeof__(int * __attribute__((aligned(16)))) Outer;\n"
        "typedef __typeof__(int * __attribute__((aligned(16))) *) Inner;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutOf(m, "Outer"), (SizeAlign{8, 16}))
        << "the outermost layer keeps its alignment: a cast's rvalue drops "
           "qualifiers, never the type's alignment";
    EXPECT_EQ(pointeeLayoutOf(m, "Outer"), (SizeAlign{4, 4}));
    EXPECT_EQ(layoutOf(m, "Inner"), (SizeAlign{8, 8}));
    EXPECT_EQ(pointeeLayoutOf(m, "Inner"), (SizeAlign{8, 16}))
        << "gcc: an 8-aligned pointer to a 16-aligned pointer";
}

TEST(AttributeTypePositions, TypeNameAFunctionTypeTakesNoAlignment) {
    EXPECT_EQ(sizeAlignOfTypeName(std::string{"int ("} + kAl16 + " *)(int)"),
              (SizeAlign{8, 8}))
        << "gcc leaves the pointer at 8: the run asks it of the FUNCTION type";
}

TEST(AttributeTypePositions, TypeNameARequestBelowNaturalLowers) {
    EXPECT_EQ(sizeAlignOfTypeName("__attribute__((aligned(1))) int"), (SizeAlign{4, 1}));
}

TEST(AttributeTypePositions, TypeNameSeveralRequestsTakeTheLargestAndTheBareFormIsTheTargets) {
    EXPECT_EQ(sizeAlignOfTypeName(
                  "__attribute__((aligned(16))) __attribute__((aligned(32))) int"),
              (SizeAlign{4, 32}));
    EXPECT_EQ(sizeAlignOfTypeName("__attribute__((aligned)) char"), (SizeAlign{1, 16}));
}

TEST(AttributeTypePositions, TypeNameTheC23SpellingAfterTheBaseAlignsTheBase) {
    // C23 6.7.3.1: the sequence that ENDS a specifier list appertains to the
    // type the specifiers determine. gcc: an 8-aligned pointer to a 16-aligned
    // int — the opposite of the GNU spelling in the same place.
    auto m = analyzeC({ "typedef __typeof__(int [[gnu::aligned(16)]] *) T;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutOf(m, "T"), (SizeAlign{8, 8}));
    EXPECT_EQ(pointeeLayoutOf(m, "T"), (SizeAlign{4, 16}));
}

TEST(AttributeTypePositions, TypeNameTheC23SpellingInAPointerLayerAlignsThatPointer) {
    auto m = analyzeC({ "typedef __typeof__(int * [[gnu::aligned(16)]]) T;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutOf(m, "T"), (SizeAlign{8, 16}));
    EXPECT_EQ(pointeeLayoutOf(m, "T"), (SizeAlign{4, 4}));
}

TEST(AttributeTypePositions, TypeNameARunAfterAReferredTagAlignsTheNamedType) {
    EXPECT_EQ(sizeAlignOfTypeName(std::string{"struct S "} + kAl16,
                                  "struct S { int a; int b; };\n"),
              (SizeAlign{8, 16}))
        << "size is deliberately untouched by a type-level alignment";
}

TEST(AttributeTypePositions, AnOverAlignedTypeNameStaysCompatibleWithThePlainType) {
    // All four references: 1 and 1. An alignment request never makes a type a
    // different type for `_Generic` or for the compatibility predicate.
    EXPECT_EQ(foldedTo(std::string{"_Generic((int)0, "} + kAl16 + " int: 1, default: 2)"), 1u);
    EXPECT_EQ(foldedTo(std::string{"__builtin_types_compatible_p("} + kAl16
                       + " int, int) ? 1 : 2"),
              1u);
}

TEST(AttributeTypePositions, TheAlignmentOfAnArrayTypeNameSurvivesItsCompletion) {
    // `(__attribute__((aligned(16))) int[]){ 1, 2 }` — the type name is an
    // incomplete array until its initializer completes it; gcc reports 16.
    auto m = analyzeC({
        "typedef __typeof__((__attribute__((aligned(16))) int[]){ 1, 2 }) Completed;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutOf(m, "Completed"), (SizeAlign{8, 16}));
}

// ── INSIDE A DECLARATOR ────────────────────────────────────────────────────

TEST(AttributeTypePositions, DeclaratorAlignedBesideTheNameAlignsThePointer) {
    auto m = analyzeC({ "int * __attribute__((aligned(16))) p;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutOf(m, "p"), (SizeAlign{8, 16})) << "gcc and clang: 8 / 16";
    EXPECT_EQ(pointeeLayoutOf(m, "p"), (SizeAlign{4, 4}));
}

TEST(AttributeTypePositions, DeclaratorAlignedInTheLayerOfTheFirstOfTwoMembers) {
    // offsets 16 and 24 on every reference: `m` is an aligned POINTER, and `n`
    // — the second declarator, which has no layer of its own — a plain int.
    auto m = analyzeC({
        "struct S18 { char c; int * __attribute__((aligned(16))) m, n; };\n"
        "struct S18 g;\n" });
    EXPECT_FALSE(m.hasErrors());
    SymbolRecord const* g = findSym(m, "g");
    ASSERT_NE(g, nullptr);
    auto const l = computeLayout(g->type, m.lattice().interner(), kLayout,
                                 DataModel::Lp64);
    ASSERT_TRUE(l.has_value());
    ASSERT_EQ(l->fieldOffsets.size(), 3u);
    EXPECT_EQ(l->fieldOffsets[1], 16u);
    EXPECT_EQ(l->fieldOffsets[2], 24u);
}

TEST(AttributeTypePositions, DeclaratorAlignedBeforeAnotherStarAlignsThePointee) {
    // `int * __attribute__((aligned(16))) *pp;` — gcc: 8 / 16 (pointee).
    auto m = analyzeC({ "int * __attribute__((aligned(16))) *pp;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutOf(m, "pp"), (SizeAlign{8, 8}));
    EXPECT_EQ(pointeeLayoutOf(m, "pp"), (SizeAlign{8, 16}));
}

TEST(AttributeTypePositions, DeclaratorAlignedAtTheStartOfAGroupAlignsWhatTheGroupDerivesFrom) {
    auto m = analyzeC({
        "int (__attribute__((aligned(16))) arr)[2];\n"
        "int (__attribute__((aligned(16))) *pf)(int);\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutOf(m, "arr"), (SizeAlign{8, 16})) << "gcc and clang: 8 / 16";
    EXPECT_EQ(layoutOf(m, "pf"), (SizeAlign{8, 8}))
        << "the group derives a FUNCTION type, which takes none (gcc: 8)";
}

TEST(AttributeTypePositions, DeclaratorPackedBesideAMemberNameIsIgnoredAndSaidSo) {
    // THE CELL WHERE THE REFERENCES' LAYOUTS DIFFER: gcc ignores it ("ignored
    // for type 'int *'") and puts `m` at 8; clang applies it and puts `m` at 1.
    // gcc's layout, and gcc's warning with it.
    auto m = analyzeC({
        "struct P { char c; int * __attribute__((packed)) m; };\n"
        "struct P g;\n" });
    EXPECT_FALSE(m.hasErrors());
    SymbolRecord const* g = findSym(m, "g");
    ASSERT_NE(g, nullptr);
    auto const l = computeLayout(g->type, m.lattice().interner(), kLayout,
                                 DataModel::Lp64);
    ASSERT_TRUE(l.has_value());
    ASSERT_EQ(l->fieldOffsets.size(), 2u);
    EXPECT_EQ(l->fieldOffsets[1], 8u);
    EXPECT_EQ(countCode(m.diagnostics(),
                        DiagnosticCode::S_AttributeIgnoredForDeclarationKind),
              1u)
        << "ignored, and said so by name, once";
}

TEST(AttributeTypePositions, DeclaratorUnusedAndDeprecatedBesideTheNameStayOnTheTypeInSilence) {
    auto m = analyzeC({
        "int * __attribute__((unused)) pu;\n"
        "int * __attribute__((deprecated)) pd;\n"
        "int * __attribute__((deprecated)) fd(void);\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(m.diagnostics().all().size(), 0u) << "gcc is silent at all three";
    ASSERT_NE(findSym(m, "pd"), nullptr);
    ASSERT_NE(findSym(m, "fd"), nullptr);
    EXPECT_FALSE(findSym(m, "pd")->isDeprecated);
    EXPECT_FALSE(findSym(m, "fd")->isDeprecated)
        << "gcc does not deprecate a function whose return type's star carries "
           "the attribute (clang does; the fork is in the row)";
}

TEST(AttributeTypePositions, DeclaratorAnAttributeBesideTheNameIsTheDeclaredEntitys) {
    auto m = analyzeC({
        "static int * __attribute__((warn_unused_result)) fr(void);\n"
        "static int * __attribute__((noinline)) fn(void) { return 0; }\n"
        "static void * __attribute__((constructor)) fc(void) { return 0; }\n"
        "void (* __attribute__((noreturn)) np)(void);\n"
        "int use(void) { return fn() != 0; }\n" });
    EXPECT_FALSE(m.hasErrors());
    ASSERT_NE(findSym(m, "fr"), nullptr);
    ASSERT_NE(findSym(m, "fn"), nullptr);
    ASSERT_NE(findSym(m, "fc"), nullptr);
    ASSERT_NE(findSym(m, "np"), nullptr);
    EXPECT_TRUE(findSym(m, "fr")->isNodiscard);
    EXPECT_TRUE(findSym(m, "fn")->isNoInline);
    EXPECT_TRUE(findSym(m, "fc")->staticInit.beforeEntry().has_value())
        << "runs before main on gcc, clang and Apple clang";
    EXPECT_TRUE(findSym(m, "np")->isNoreturn);
    EXPECT_EQ(countCode(m.diagnostics(),
                        DiagnosticCode::S_AttributeIgnoredForDeclarationKind),
              0u);
}

TEST(AttributeTypePositions, DeclaratorOnlyTheDeclaratorThatCarriesTheRunGetsIt) {
    auto m = analyzeC({ "void (* __attribute__((noreturn)) a)(void), (*b)(void);\n" });
    EXPECT_FALSE(m.hasErrors());
    ASSERT_NE(findSym(m, "a"), nullptr);
    ASSERT_NE(findSym(m, "b"), nullptr);
    EXPECT_TRUE(findSym(m, "a")->isNoreturn);
    EXPECT_FALSE(findSym(m, "b")->isNoreturn);
}

TEST(AttributeTypePositions, DeclaratorParenthesesThatDeriveNothingAreLookedThrough) {
    // `int * __attribute__((X)) (f)(void)`: what follows the run is a function
    // declarator, whatever its name is wrapped in. And `void (* __attribute__((X))
    // (np))(void)`: the parentheses round the name derive nothing, so the run
    // still stands beside the name.
    auto m = analyzeC({
        "static int * __attribute__((warn_unused_result)) (fp)(void);\n"
        "void (* __attribute__((noreturn)) (np))(void);\n" });
    EXPECT_FALSE(m.hasErrors());
    ASSERT_NE(findSym(m, "fp"), nullptr);
    ASSERT_NE(findSym(m, "np"), nullptr);
    EXPECT_TRUE(findSym(m, "fp")->isNodiscard);
    EXPECT_TRUE(findSym(m, "np")->isNoreturn);
}

TEST(AttributeTypePositions, DeclaratorAnEntityAttributeBeforeAnotherStarDecoratesATypeAndIsSaidSo) {
    // gcc at each: "'weak' attribute does not apply to types" — and the symbol
    // stays strong. clang makes it weak; this compiler reads as gcc does.
    auto m = analyzeC({
        "int * __attribute__((weak)) *pp;\n"
        "int (__attribute__((noinline)) *pf)(int);\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(countCode(m.diagnostics(),
                        DiagnosticCode::S_AttributeIgnoredForDeclarationKind),
              2u);
}

TEST(AttributeTypePositions, DeclaratorATypeOrFunctionPointerAttributeBeforeAnotherStarIsSilent) {
    auto m = analyzeC({
        "int * __attribute__((unused)) *pa;\n"
        "int (__attribute__((unused)) *pb)(int);\n"
        "int (__attribute__((format(printf, 1, 2))) *pc)(char const *, ...);\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(m.diagnostics().all().size(), 0u);
}

// ── WHAT IS SAID OF AN ATTRIBUTE IN A TYPE NAME ────────────────────────────

TEST(AttributeTypePositions, TypeNameAnUnknownNameIsWarnedOnce) {
    auto m = analyzeC({ "char a[sizeof(int __attribute__((frobnicate)) *)];\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_UnknownAttribute), 1u);
}

TEST(AttributeTypePositions, TypeNameAnEntityAttributeIsWarnedAsIgnoredAndATypeOneIsSilent) {
    auto warned = analyzeC({ "char a[sizeof(int * __attribute__((weak)))];\n" });
    EXPECT_FALSE(warned.hasErrors());
    EXPECT_EQ(countCode(warned.diagnostics(),
                        DiagnosticCode::S_AttributeIgnoredForDeclarationKind),
              1u);
    auto silent = analyzeC({ "char a[sizeof(__attribute__((unused)) int)];\n" });
    EXPECT_EQ(silent.diagnostics().all().size(), 0u);
}

TEST(AttributeTypePositions, TypeNameABadAlignmentOperandIsRefusedExactlyOnce) {
    // The resolver visits a type name more than once and the operand ladder
    // reports unconditionally; the application is net-silent and Pass 2 says it
    // ONCE. A count of 1 is the pin on both halves.
    auto m = analyzeC({ "char a[sizeof(__attribute__((aligned(3))) int)];\n" });
    EXPECT_TRUE(m.hasErrors());
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_AlignasNotPowerOfTwo), 1u);
}

TEST(AttributeTypePositions, DeclaratorABadAlignmentOperandBesideTheNameIsRefusedExactlyOnce) {
    auto m = analyzeC({ "int * __attribute__((aligned(3))) p;\n" });
    EXPECT_TRUE(m.hasErrors());
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_AlignasNotPowerOfTwo), 1u);
}

// ── THE TWO VERBS JUDGED PER SPECIFIER ─────────────────────────────────────

TEST(AttributeTypePositions, AnAttributeThatChangesTheTypeAndCannotBeHonouredIsRefusedByName) {
    // Was: a warning that the name was unknown, and `sizeof(v4)` 4 where every
    // reference gives 16.
    for (char const* src : {
             "typedef int v4 __attribute__((vector_size(16)));\n",
             "int __attribute__((vector_size(16))) g;\n",
             "char a[sizeof(__attribute__((vector_size(16))) int)];\n",
             "int * __attribute__((vector_size(16))) p;\n",
             "int x __attribute__((mode(DI)));\n" }) {
        auto m = analyzeC({ src });
        EXPECT_TRUE(m.hasErrors()) << src;
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_AttributeNotHonoured), 1u)
            << src;
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_UnknownAttribute), 0u)
            << "refused, not also called unknown: " << src;
    }
}

TEST(AttributeTypePositions, ACallingConventionNameWithNoPairInScopeIsRefusedByName) {
    auto m = analyzeC({ "void __attribute__((ms_abi)) f(void);\n" });
    EXPECT_TRUE(m.hasErrors());
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_AttributeNotHonoured), 1u);
}

// A name that selects a calling convention has THREE answers, and which one is
// read from the two documents of the pair being compiled for, never from the name:
//   ACTIVE   — one of the row's ids is the pair's own convention: silent, nothing
//              to do (`sysv_abi` on x86-64 Linux);
//   FOREIGN  — one of its ids is a convention the target KNOWS (a row of the target
//              document, or one it lists as unimplemented): refused by name, because
//              a per-function convention is not something this compiler emits;
//   UNKNOWN  — no id is known to the target at all: an unknown word, warned
//              (`aarch64_vector_pcs` on x86-64, where every reference ignores it).
// The whole matrix, seven names on the four shipped pairs.
//
// RED-ON-DISABLE, one per answer: drop the "is the active convention" test and
// every ACTIVE cell is refused; drop the foreign refusal and every FOREIGN cell is
// an unknown-attribute warning on a successful analysis; make the target's standing
// answer `Implemented` for every id and every UNKNOWN cell is refused.
TEST(AttributeTypePositions, AConventionNameIsActiveForeignOrUnknownByThePairsOwnFacts) {
    enum class Answer { Active, Foreign, Unknown };
    struct Pair {
        char const*      target;
        char const*      convention;
        ObjectFormatKind format;
    };
    constexpr Pair kPairs[4] = {
        {"x86_64", "sysv_amd64", ObjectFormatKind::Elf},
        {"x86_64", "ms_x64", ObjectFormatKind::Pe},
        {"arm64", "aapcs64", ObjectFormatKind::Elf},
        {"arm64", "apple_arm64", ObjectFormatKind::MachO}};
    struct Name {
        char const* name;
        Answer      on[4];   // one per pair, in `kPairs` order
    };
    constexpr Answer A = Answer::Active;
    constexpr Answer F = Answer::Foreign;
    constexpr Answer U = Answer::Unknown;
    constexpr Name kNames[7] = {
        {"ms_abi", {F, A, F, F}},
        {"sysv_abi", {A, F, U, U}},
        {"vectorcall", {F, F, U, U}},
        {"regcall", {F, F, U, U}},
        {"preserve_most", {F, F, F, F}},
        {"preserve_all", {F, F, F, F}},
        {"aarch64_vector_pcs", {U, U, F, F}}};
    std::size_t seen[3] = {0, 0, 0};
    for (std::size_t p = 0; p < 4; ++p) {
        auto const target = TargetSchema::loadShipped(kPairs[p].target);
        ASSERT_TRUE(target.has_value()) << kPairs[p].target;
        ASSERT_NE((*target)->callingConventionByName(kPairs[p].convention), nullptr)
            << "the pair's convention must be a row of its target document: "
            << kPairs[p].convention;
        for (Name const& n : kNames) {
            std::string const src =
                std::string{"void __attribute__(("} + n.name + ")) f(void);\n";
            auto cu = buildShippedUnit("c", {src});
            assertNoBuilderErrors(*cu);
            auto const m = analyze(
                cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kLayout,
                std::nullopt, SelectableObjectFormatKind::of(kPairs[p].format),
                std::nullopt, LongDoubleFormat::None, target->get(),
                /*deepRecursionReserveBytes=*/0, /*roleResolver=*/nullptr,
                EnumCompatibleTypeRule::Gnu, kPairs[p].convention);
            std::string const where = std::string{n.name} + " on " + kPairs[p].target
                                    + "/" + kPairs[p].convention;
            std::size_t const refused =
                countCode(m.diagnostics(), DiagnosticCode::S_AttributeNotHonoured);
            std::size_t const unknown =
                countCode(m.diagnostics(), DiagnosticCode::S_UnknownAttribute);
            ++seen[static_cast<std::size_t>(n.on[p])];
            switch (n.on[p]) {
                case Answer::Active:
                    EXPECT_FALSE(m.hasErrors()) << where;
                    EXPECT_EQ(refused, 0u) << where;
                    EXPECT_EQ(unknown, 0u) << where;
                    break;
                case Answer::Foreign: {
                    EXPECT_TRUE(m.hasErrors()) << where;
                    EXPECT_EQ(refused, 1u) << where;
                    EXPECT_EQ(unknown, 0u) << where;
                    bool namesActive = false;
                    for (auto const& d : m.diagnostics().all())
                        if (d.code == DiagnosticCode::S_AttributeNotHonoured
                            && d.actual.find(std::string{"'"} + kPairs[p].convention + "'")
                                   != std::string::npos
                            && d.actual.find(std::string{"'"} + n.name + "'")
                                   != std::string::npos)
                            namesActive = true;
                    EXPECT_TRUE(namesActive)
                        << where << " — the refusal names the attribute and the "
                                    "convention the unit is compiled for";
                    break;
                }
                case Answer::Unknown:
                    EXPECT_FALSE(m.hasErrors()) << where;
                    EXPECT_EQ(refused, 0u) << where;
                    EXPECT_EQ(unknown, 1u) << where;
                    break;
            }
        }
    }
    // The matrix exercises every answer (a table that lost one would still pass).
    EXPECT_EQ(seen[static_cast<std::size_t>(A)], 2u);
    EXPECT_EQ(seen[static_cast<std::size_t>(F)], 18u);
    EXPECT_EQ(seen[static_cast<std::size_t>(U)], 8u);
}

// A CONVENTION ID OF THE LANGUAGE'S TABLE THAT NO SHIPPED TARGET KNOWS would select
// nothing on any pair: its name would be an unknown word everywhere, and the row
// that carries it dead vocabulary that reads as a modelled attribute. The language
// document is loaded with no target document in hand, so that question cannot be
// asked at load — it is asked here, of the shipped set, in both directions that
// can rot: every id a language row names is known to some target (a row of it, or
// one it lists as unimplemented), and every id a target lists as unimplemented is
// named by some language row (or nothing could ever ask the target about it).
//
// RED-ON-DISABLE: misspell one id in a `conventions` list of the language document,
// or in a target's `unimplementedCallingConventions`, and the matching half is red.
TEST(AttributeTypePositions, EveryConventionIdIsSharedByTheLanguageAndSomeTarget) {
    auto const schema = loadShippedSchema("c");
    ASSERT_NE(schema, nullptr);
    std::vector<std::shared_ptr<TargetSchema>> targets;
    for (char const* name : {"x86_64", "arm64"}) {
        auto loaded = TargetSchema::loadShipped(name);
        ASSERT_TRUE(loaded.has_value()) << name;
        targets.push_back(*loaded);
    }
    std::vector<std::string> languageIds;
    for (auto const& row : schema->semantics().attributeEffects) {
        if (row.effect != AttributeEffect::CallingConvention) continue;
        for (std::string const& id : row.conventions) {
            languageIds.push_back(id);
            bool known = false;
            for (auto const& t : targets) {
                if (t->callingConventionStanding(id)
                    != TargetSchema::CallingConventionStanding::Unknown) {
                    known = true;
                }
            }
            EXPECT_TRUE(known) << "the language names the convention id '" << id
                               << "', which no shipped target document knows";
        }
    }
    EXPECT_GE(languageIds.size(), 7u)
        << "the seven convention names of the shipped table each name at least one id";
    std::size_t listed = 0;
    for (auto const& t : targets) {
        for (std::string const& id : t->unimplementedCallingConventions()) {
            ++listed;
            bool named = false;
            for (std::string const& l : languageIds) named = named || l == id;
            EXPECT_TRUE(named) << "a target lists '" << id
                               << "' as unimplemented, and no language row names it";
        }
    }
    EXPECT_GT(listed, 0u) << "the control must have read at least one listed id";
}

// ── `_Noreturn` WRITTEN AS AN ATTRIBUTE NAME ───────────────────────────────

TEST(AttributeTypePositions, TheKeywordSpelledAttributeNameIsKnown) {
    // C23 6.7.13.7. It was honoured AND warned as unknown; gcc is silent.
    auto m = analyzeC({ "[[_Noreturn]] void k(void);\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_UnknownAttribute), 0u);
    ASSERT_NE(findSym(m, "k"), nullptr);
    EXPECT_TRUE(findSym(m, "k")->isNoreturn);
}
