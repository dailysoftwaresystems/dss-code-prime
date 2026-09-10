// ═══════════════════════════════════════════════════════════════════════════
//  THE THREE REMAINING GNU `aligned` FORMS — cycle P66, lane `ag`
//   (1) a request WEAKER than natural LOWERS   (2) a MID-LIST typedef
//       declarator gets it and its siblings do NOT   (3) the ZERO-ARGUMENT
//       form is the TARGET's declared maximum useful alignment
// ═══════════════════════════════════════════════════════════════════════════
//
// These are THREE INDEPENDENT defects in one mechanism area, and each owns its
// own pin here: a single pin over three would be
// [[feedback-a-partial-fix-reads-as-a-complete-one]] exactly. The disjointness
// of the three red sets under the three mutants is what proves they are
// independent rather than one bug with three faces.
//
// ── THE REFERENCE MATRIX, EACH REFERENCE PROBED SEPARATELY, ONE TU PER PROBE,
//    rc READ DIRECTLY, `-Wall -Wextra`, MEASURED 2026-09-09 ──────────────────
//  gcc 13.3.0 (`-std=c2x`) · clang 18.1.3 (`-std=c23`) · mingw-w64 gcc 13.2.0 ·
//  aarch64-linux-gnu-gcc 13.3.0 (EXECUTED under qemu-aarch64) ·
//  MSVC 19.51.36257 (`/nologo /c /std:c17`).
//
//  (1) `typedef int A2 __attribute__((aligned(2)));`
//        gcc / clang / mingw / a64-gcc: `_Alignof(A2)` is **2** (the `== 4`
//        twin FAILS on all four), in BOTH orders; `sizeof(A2)` stays 4;
//        `struct T { char c; A2 v; }` is sizeof **6** / align 2 / offsetof(v)
//        **2** (the sizeof-8 twin FAILS); a chained alias keeps 2; a struct
//        alias and a pointer alias lower too; `aligned(1)` reaches 1.
//        DSS kept 4 and laid the struct out at 8/4 — ✔EXECUTED, a DSS-built
//        binary of that shape returned 10 where gcc, clang and aarch64-gcc all
//        returned 42, so the divergence reached the OBJECT, not merely `_Alignof`.
//        MSVC abstains on the surface (`__attribute__` is C2061); through the
//        spelling it implements it does NOT lower (`typedef __declspec(align(2))
//        int A2;` keeps `__alignof` 4). Recorded as a measurement: MSVC has no
//        `__attribute__((aligned))`, and increase-only is a documented property
//        of a DIFFERENT attribute, so it casts no vote on this construct's
//        meaning. Three implementers, unanimous ⇒ lowering is REQUIRED.
//
//  ⛔ AND THE BOUND ON (1), WHICH IS WHY THE FIX IS NOT "MAKE ALIGNMENT
//     LOWERABLE EVERYWHERE": the WHOLE-COMPOSITE channel does **not** lower.
//     `struct __attribute__((aligned(1))) S { int a; };` keeps `_Alignof` 4 on
//     gcc, clang, mingw AND a64-gcc (the `== 1` twin FAILS on all four), and a
//     struct MEMBER's `aligned(2)` does not lower either. `explicitAlign`'s and
//     `fieldAligns`' MAX folds are therefore CORRECT and are pinned unmoved
//     below. Only the TYPE-LEVEL channel lowers.
//
//  (2) `typedef int A __attribute__((aligned(8))), B;`
//        gcc / clang / mingw / a64-gcc all COMPILE it and confer on **A ALONE**
//        — `_Alignof(A)==8` and `_Alignof(B)==4` both PASS, and the
//        `_Alignof(B)==8` twin FAILS on all four. DSS drew `P_UnexpectedToken`.
//        ⚠ THE BINDING IS THE OPPOSITE OF THE `[[deprecated]]` SIBLING lane
//        `td` measured in the same slot, which confers on **B**; neither
//        generalises to the other and both were verified here rather than
//        reasoned from one another. The GNU run in the TRAILING slot has
//        DECLARATOR grain; the decl-specifier PREFIX spelling
//        (`typedef __attribute__((aligned(8))) int A, B;`) has DECLARATION
//        grain and confers on BOTH — ✔MEASURED, and pinned as a control.
//
//  (3) `typedef int AM __attribute__((aligned));`
//        gcc / clang / mingw / a64-gcc all give `_Alignof(AM)` = **16** (the 8
//        and 32 twins FAIL on all four), independent of the decorated type
//        (`char` alias too), on the composite and on a member; ✔EXECUTED on
//        x86_64 AND on aarch64 under qemu. DSS drew `S_UnknownTypeAttribute`.
//        MSVC has no argument-less `__declspec(align)` (C2059) and abstains.
//        16 is what BOTH shipped targets declare as
//        `aggregateLayout.maxAlignment`, which is why the number is READ from
//        the target and never written in `src/`.
//
// ⓘ ONE MEASURED SPLIT, RECORDED AND DELIBERATELY NOT ACTED ON: two `aligned`
//   clauses in ONE list — `__attribute__((aligned(16), aligned(2)))` — fold to
//   **2** on gcc / mingw / a64-gcc (LAST wins) and to **16** on clang (MAX).
//   DSS's cross-clause MAX matches clang, so DSS is inside the union; the
//   cross-clause fold is left exactly as it was and pinned below, because
//   changing it would move DSS from agreeing with one reference to agreeing
//   with the other for no gain.

#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include "semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

using namespace dss;
using namespace dss::sem_test;

namespace {

// The fixture MUST carry layout params: every arm of the sink is gated on
// `s.aggregateLayout.has_value()` (the LSP's no-params shape mints nothing), and
// form (3) READS `maxAlignment` out of exactly this block — 16, the value both
// shipped targets declare and the value all four references measure.
constexpr AggregateLayoutParams kAlignLayout{
    .scalarAlignment       = ScalarAlignmentRule::Natural,
    .maxAlignment          = 16,
    .maxRequestedAlignment = 268435456};

// A SECOND params block whose ONLY difference is the declared maximum. Form (3)
// must answer 32 under it, which is what proves the number is READ FROM THE
// TARGET rather than compiled in — an assertion of 16 alone is satisfied by a
// constant, and a constant is precisely what the old refusal said it would not
// invent.
constexpr AggregateLayoutParams kAlign32Layout{
    .scalarAlignment       = ScalarAlignmentRule::Natural,
    .maxAlignment          = 32,
    .maxRequestedAlignment = 268435456};

[[nodiscard]] SemanticModel
analyzeWith(AggregateLayoutParams p, std::initializer_list<std::string> srcs) {
    auto cu = buildShippedUnit("c", srcs);
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, p);
}

[[nodiscard]] SemanticModel analyzeWithLayout(std::initializer_list<std::string> srcs) {
    return analyzeWith(kAlignLayout, srcs);
}

[[nodiscard]] SymbolRecord const*
findSym(SemanticModel const& m, std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i)
        if (m.symbols()[i].name == name) return &m.symbols()[i];
    return nullptr;
}

// The alignment a NAME's type actually LAYS OUT at — not the raw skin scalar.
// `typeAlignOverride` answers "is a skin present", which is an adjacent
// question: it cannot tell a conferred 2 from a dropped request, because an
// undecorated `int` and a skin-less alias both read 0 while laying out at 4.
// Going through `computeLayout` asks the question every consumer asks.
[[nodiscard]] std::uint32_t layoutAlignOf(SemanticModel const& m, char const* name) {
    SymbolRecord const* s = findSym(m, name);
    EXPECT_NE(s, nullptr) << "no symbol named '" << name << "'";
    if (s == nullptr) return 0;
    auto const l = computeLayout(s->type, m.lattice().interner(), kAlignLayout,
                                 DataModel::Lp64);
    EXPECT_TRUE(l.has_value()) << "no layout for '" << name << "'";
    return l ? l->align.bytes() : 0;
}

[[nodiscard]] std::uint32_t layoutSizeOf(SemanticModel const& m, char const* name) {
    SymbolRecord const* s = findSym(m, name);
    EXPECT_NE(s, nullptr) << "no symbol named '" << name << "'";
    if (s == nullptr) return 0;
    auto const l = computeLayout(s->type, m.lattice().interner(), kAlignLayout,
                                 DataModel::Lp64);
    EXPECT_TRUE(l.has_value()) << "no layout for '" << name << "'";
    return l ? static_cast<std::uint32_t>(l->size) : 0;
}

} // namespace

// ── (1) A REQUEST WEAKER THAN NATURAL — THE SILENT, ABI-VISIBLE ONE ────────

TEST(GnuAlignedThreeForms, AWeakerThanNaturalRequestLowersTheAlias) {
    auto m = analyzeWithLayout({ "typedef int A2 __attribute__((aligned(2)));\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutAlignOf(m, "A2"), 2u)
        << "gcc 13.3.0, clang 18.1.3, mingw-w64 gcc 13.2.0 and aarch64-linux-gnu-gcc "
           "all LOWER a typedef's alignment below natural; keeping 4 was a silent "
           "ABI divergence from every reference that implements the construct";
    EXPECT_EQ(layoutSizeOf(m, "A2"), 4u) << "size is deliberately untouched";
}

TEST(GnuAlignedThreeForms, TheLoweringWorksInTheLeadingOrderToo) {
    auto m = analyzeWithLayout({ "typedef int __attribute__((aligned(2))) A2;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutAlignOf(m, "A2"), 2u)
        << "every reference lowers in BOTH orders";
}

TEST(GnuAlignedThreeForms, TheLoweringReachesAStructMemberOffsetAndTheAggregateSize) {
    // THE ARM THAT MAKES THE DIVERGENCE ABI-VISIBLE RATHER THAN COSMETIC: an
    // `_Alignof`-only fix would leave this at 8/4 and read as complete.
    auto m = analyzeWithLayout({
        "typedef int A2 __attribute__((aligned(2)));\n"
        "struct T { char c; A2 v; };\n"
        "struct T gt;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutSizeOf(m, "gt"), 6u)
        << "gcc/clang/mingw/a64-gcc: sizeof(struct T) == 6; DSS laid it out at 8, and "
           "an EXECUTED DSS binary of this shape returned 10 where all three "
           "references returned 42";
    EXPECT_EQ(layoutAlignOf(m, "gt"), 2u);
    SymbolRecord const* s = findSym(m, "gt");
    ASSERT_NE(s, nullptr);
    auto const l = computeLayout(s->type, m.lattice().interner(), kAlignLayout,
                                 DataModel::Lp64);
    ASSERT_TRUE(l.has_value());
    ASSERT_EQ(l->fieldOffsets.size(), 2u);
    EXPECT_EQ(l->fieldOffsets[1], 2u) << "offsetof(struct T, v) is 2, not 4";
}

TEST(GnuAlignedThreeForms, ARequestEqualToNaturalStillMintsNoSkin) {
    // THE REGRESSION WALL, and it is the SDK-dominant spelling: `typedef
    // u_int64_t T __attribute__((aligned(8)));` where 8 EQUALS natural 8 must
    // keep interning to the SAME TypeId as its aliasee. The guard moved from
    // `want <= natural` to `want == natural` for form (1); this pin is what says
    // the `==` half did not move with it.
    auto m = analyzeWithLayout({
        "typedef unsigned long long U64;\n"
        "typedef U64 T __attribute__((aligned(8)));\n"
        "U64 a; T b;\n" });
    EXPECT_FALSE(m.hasErrors());
    SymbolRecord const* sa = findSym(m, "a");
    SymbolRecord const* sb = findSym(m, "b");
    ASSERT_NE(sa, nullptr);
    ASSERT_NE(sb, nullptr);
    EXPECT_EQ(sa->type.v, sb->type.v)
        << "a no-op request must not cost type identity";
    EXPECT_EQ(m.lattice().interner().typeAlignOverride(sb->type), 0u);
    EXPECT_EQ(layoutAlignOf(m, "b"), 8u);
}

TEST(GnuAlignedThreeForms, AReAliasWithAWeakerRequestLowersFurther) {
    // MEASURED on gcc, clang, mingw and a64-gcc: A8 stays 8 and A2 becomes 2.
    // This is the pin on the INTERNER's merge: under the old MAX merge the
    // second, weaker request could not lower what the first one raised.
    auto m = analyzeWithLayout({
        "typedef int A8 __attribute__((aligned(8)));\n"
        "typedef A8 A2 __attribute__((aligned(2)));\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutAlignOf(m, "A8"), 8u);
    EXPECT_EQ(layoutAlignOf(m, "A2"), 2u)
        << "the outer alias's request REPLACES the inner one";
}

TEST(GnuAlignedThreeForms, AVolatileWrapDoesNotDropTheAlignment) {
    // The merge became REPLACE-ON-NON-ZERO rather than MAX, and the
    // guarded-on-non-zero half is what stops a `volatile` wrap (addAlign 0)
    // from erasing an alignment the skin already carries. Without the guard
    // this is a silent loss of alignment on every qualified alias.
    auto m = analyzeWithLayout({
        "typedef int A8 __attribute__((aligned(8)));\n"
        "volatile A8 v;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutAlignOf(m, "v"), 8u);
}

// ── (1) THE CONTROLS THAT BOUND THE LOWERING ──────────────────────────────

TEST(GnuAlignedThreeForms, TheWholeCompositeChannelStillDoesNotLower) {
    // MEASURED: gcc, clang, mingw AND a64-gcc all keep `_Alignof(struct S)` at
    // 4 here (the `== 1` twin FAILS on all four). `explicitAlign`'s MAX fold is
    // CORRECT and must not travel with form (1).
    auto m = analyzeWithLayout({
        "struct __attribute__((aligned(1))) S { int a; };\n"
        "struct S gs;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutAlignOf(m, "gs"), 4u)
        << "the whole-composite channel is increase-only in every reference";
}

TEST(GnuAlignedThreeForms, AStructMemberRequestStillDoesNotLower) {
    // MEASURED: `struct M { char c; int v __attribute__((aligned(2))); }` keeps
    // offsetof(v) at 4 on all four references (the `== 2` assertion FAILS on all
    // four). `fieldAligns`' MAX fold is correct too.
    auto m = analyzeWithLayout({
        "struct M { char c; int v __attribute__((aligned(2))); };\n"
        "struct M gm;\n" });
    EXPECT_FALSE(m.hasErrors());
    SymbolRecord const* s = findSym(m, "gm");
    ASSERT_NE(s, nullptr);
    auto const l = computeLayout(s->type, m.lattice().interner(), kAlignLayout,
                                 DataModel::Lp64);
    ASSERT_TRUE(l.has_value());
    ASSERT_EQ(l->fieldOffsets.size(), 2u);
    EXPECT_EQ(l->fieldOffsets[1], 4u);
}

TEST(GnuAlignedThreeForms, TheCrossClauseFoldIsStillMax) {
    // gcc / mingw / a64-gcc fold two clauses in ONE list LAST-WINS (2); clang
    // folds MAX (16). DSS folds MAX, i.e. it agrees with clang and is inside the
    // union. Left exactly as it was; this pin is what stops form (1) from being
    // "generalised" into the cross-clause fold, where it would only swap which
    // reference DSS agrees with.
    auto m = analyzeWithLayout({
        "typedef int A __attribute__((aligned(16), aligned(2)));\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutAlignOf(m, "A"), 16u);
}

TEST(GnuAlignedThreeForms, TheIsoAlignasSpellingOnATypedefIsStillRefused) {
    // [[D-CSUBSET-ALIGNAS-TYPEDEF-PARAM-PARSE]] (P58): every reference REFUSES
    // this and DSS is right to. It must not be widened alongside the GNU forms.
    auto m = analyzeWithLayout({ "typedef _Alignas(2) int A2;\n" });
    EXPECT_TRUE(m.hasErrors());
    EXPECT_GE(countCode(m.diagnostics(), DiagnosticCode::S_AlignasInvalidContext), 1u);
}

// ── (2) THE MULTI-DECLARATOR TYPEDEF ──────────────────────────────────────

TEST(GnuAlignedThreeForms, AMidListDeclaratorIsAcceptedAndConfersOnItselfAlone) {
    auto m = analyzeWithLayout({
        "typedef int A __attribute__((aligned(8))), B;\n" });
    EXPECT_FALSE(m.hasErrors())
        << "gcc, clang, mingw and a64-gcc all COMPILE this; DSS drew P_UnexpectedToken";
    EXPECT_EQ(layoutAlignOf(m, "A"), 8u);
    EXPECT_EQ(layoutAlignOf(m, "B"), 4u)
        << "B must NOT get it -- conferring on both would be a NEW above-the-union "
           "defect that no acceptance test could see";
}

TEST(GnuAlignedThreeForms, AThreeSlotListDecoratesOnlyTheSlotThatCarriesTheRun) {
    auto m = analyzeWithLayout({
        "typedef int A, B __attribute__((aligned(16))), C;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutAlignOf(m, "A"), 4u);
    EXPECT_EQ(layoutAlignOf(m, "B"), 16u);
    EXPECT_EQ(layoutAlignOf(m, "C"), 4u);
}

TEST(GnuAlignedThreeForms, TheDeclSpecifierPrefixSpellingStillConfersOnEveryDeclarator) {
    // THE OPPOSITE GRAIN, AND IT IS MEASURED: a run written BEFORE the
    // declarators appertains to every declared entity (GNU 6.34), and gcc,
    // clang, mingw and a64-gcc all give BOTH aliases 8 here.
    auto m = analyzeWithLayout({
        "typedef __attribute__((aligned(8))) int A, B;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutAlignOf(m, "A"), 8u);
    EXPECT_EQ(layoutAlignOf(m, "B"), 8u);
}

TEST(GnuAlignedThreeForms, TheLastDeclaratorSpellingIsUnmoved) {
    // This shape ALREADY worked (through `typedefTrailingAttrRun`) and must keep
    // working now that the per-slot run is greedy and takes it first -- same
    // declarator, same grain, same answer.
    auto m = analyzeWithLayout({
        "typedef int A, B __attribute__((aligned(8)));\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutAlignOf(m, "A"), 4u);
    EXPECT_EQ(layoutAlignOf(m, "B"), 8u);
}

TEST(GnuAlignedThreeForms, TheSdkSingleDeclaratorSpellingIsUnchanged) {
    // `typedef u_int64_t au_asflgs_t __attribute__((aligned(8)));` -- bsm/audit.h.
    // The per-slot run now swallows this where `typedefTrailingAttrRun` used to;
    // for ONE declarator the two are the same declarator, so the answer must not
    // move. THE REGRESSION WALL for the grammar half.
    auto m = analyzeWithLayout({
        "typedef unsigned long long U64;\n"
        "typedef U64 T __attribute__((aligned(8)));\n"
        "T x;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutAlignOf(m, "x"), 8u);
    EXPECT_EQ(layoutSizeOf(m, "x"), 8u);
}

TEST(GnuAlignedThreeForms, EachSlotKeepsItsOwnDerivedType) {
    // The per-slot wrapper must not disturb per-declarator type derivation:
    // `typedef int *P, A[4];` still types P as int* and A as int[4]. A wrapper
    // missed at ONE of the descent sites shows up here as a wrong TYPE rather
    // than as a wrong alignment.
    auto m = analyzeWithLayout({
        "typedef int *P, A[4];\n"
        "P p; A a;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutSizeOf(m, "p"), 8u);
    EXPECT_EQ(layoutSizeOf(m, "a"), 16u);
}

// ── (3) THE ZERO-ARGUMENT FORM ────────────────────────────────────────────

TEST(GnuAlignedThreeForms, TheZeroArgumentFormTakesTheTargetsDeclaredMaximum) {
    auto m = analyzeWithLayout({ "typedef int AM __attribute__((aligned));\n" });
    EXPECT_FALSE(m.hasErrors())
        << "gcc, clang, mingw and a64-gcc all accept the bare form; DSS drew "
           "S_UnknownTypeAttribute";
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_UnknownTypeAttribute), 0u);
    EXPECT_EQ(layoutAlignOf(m, "AM"), 16u);
}

TEST(GnuAlignedThreeForms, TheZeroArgumentFormIsReadFromTheTargetNotCompiledIn) {
    // THE CONFIG-DRIVEN PIN, AND IT IS THE ONE THAT MATTERS. An assertion of 16
    // alone is satisfied by a constant `16` in `src/`, which is exactly what the
    // old refusal said this engine had no business inventing. Under a params
    // block whose ONLY difference is `maxAlignment = 32`, the SAME source must
    // answer 32 -- which a constant cannot do.
    auto m = analyzeWith(kAlign32Layout,
                         { "typedef int AM __attribute__((aligned));\n"
                           "AM v;\n" });
    EXPECT_FALSE(m.hasErrors());
    SymbolRecord const* s = findSym(m, "v");
    ASSERT_NE(s, nullptr);
    auto const l = computeLayout(s->type, m.lattice().interner(), kAlign32Layout,
                                 DataModel::Lp64);
    ASSERT_TRUE(l.has_value());
    EXPECT_EQ(l->align.bytes(), 32u)
        << "the bare form must READ aggregateLayout.maxAlignment, never a constant";
}

TEST(GnuAlignedThreeForms, TheDunderSpellingOfTheZeroArgumentFormWorksToo) {
    auto m = analyzeWithLayout({ "typedef int AM __attribute__((__aligned__));\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutAlignOf(m, "AM"), 16u);
}

TEST(GnuAlignedThreeForms, TheZeroArgumentFormOnACompositeTakesTheSameMaximum) {
    // The composite arm is a SECOND site with the same question, and it had its
    // own copy of the refusal. MEASURED: gcc/clang/mingw/a64-gcc all give
    // `struct __attribute__((aligned)) S { char c; }` align 16 AND sizeof 16.
    auto m = analyzeWithLayout({
        "struct __attribute__((aligned)) S { char c; };\n"
        "struct S gs;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_UnknownTypeAttribute), 0u);
    EXPECT_EQ(layoutAlignOf(m, "gs"), 16u);
    EXPECT_EQ(layoutSizeOf(m, "gs"), 16u);
}

TEST(GnuAlignedThreeForms, TheZeroArgumentFormOnAnObjectTakesTheSameMaximum) {
    auto m = analyzeWithLayout({ "int mo __attribute__((aligned));\n" });
    EXPECT_FALSE(m.hasErrors());
    SymbolRecord const* s = findSym(m, "mo");
    ASSERT_NE(s, nullptr);
    ASSERT_TRUE(s->explicitAlignment.has_value());
    EXPECT_EQ(*s->explicitAlignment, 16u);
}

TEST(GnuAlignedThreeForms, TheZeroArgumentFormInAMidListSlotBindsLikeAnyOther) {
    // The three forms COMPOSE: (3) written in (2)'s slot. MEASURED on gcc, clang
    // and mingw: A is 16 and B is untouched.
    auto m = analyzeWithLayout({
        "typedef char A __attribute__((aligned)), B;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutAlignOf(m, "A"), 16u);
    EXPECT_EQ(layoutAlignOf(m, "B"), 1u);
}

TEST(GnuAlignedThreeForms, AnExplicitArgumentStillWinsBesideTheBareForm) {
    // `((aligned(32), aligned))` is 32 under the MAX cross-clause fold, matching
    // clang, and it proves the bare arm did not swallow the explicit one.
    auto m = analyzeWithLayout({
        "typedef int A __attribute__((aligned(32), aligned));\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(layoutAlignOf(m, "A"), 32u);
}

TEST(GnuAlignedThreeForms, AnUnknownAttributeNameIsStillReportedNotSwallowed) {
    // The bare-`aligned` arm was a user of `S_UnknownTypeAttribute` inside the
    // Align verb; removing its refusal must not disarm the typo protection the
    // same code carries for every other name.
    // ⚠ THIS PIN'S FIRST DRAFT ASSERTED `hasErrors()` AND WAS REFUTED BY THE
    // TREE, which is the pin working before it ever guarded anything: on a
    // TYPEDEF the report is a WARNING, because c's `typedefDecl` row declares
    // `unknownStrictAttributeIsError: false`. ✔RE-MEASURED through the shipped
    // CLI — `typedef int A __attribute__((alignedd(8)));` is rc 0 with
    // `warning[S_UnknownAttribute]`, and that is the PRE-EXISTING posture, not
    // something this row moved. Asserting the diagnostic that is actually there
    // is what makes this discriminating; asserting an error would have made it
    // red for a reason unrelated to its subject.
    auto m = analyzeWithLayout({ "typedef int A __attribute__((alignedd(8)));\n" });
    EXPECT_GE(countCode(m.diagnostics(), DiagnosticCode::S_UnknownAttribute), 1u)
        << "a typo must still be named; the bare-form arm no longer reports, and "
           "that must not have taken the unknown-name report with it";
}

TEST(GnuAlignedThreeForms, ANonPowerOfTwoRequestIsStillRefusedLoudly) {
    auto m = analyzeWithLayout({ "typedef int A3 __attribute__((aligned(3)));\n" });
    EXPECT_TRUE(m.hasErrors());
    EXPECT_GE(countCode(m.diagnostics(), DiagnosticCode::S_AlignasNotPowerOfTwo), 1u);
}
