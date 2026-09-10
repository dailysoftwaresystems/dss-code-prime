// ═══════════════════════════════════════════════════════════════════════════
//  GNU `__attribute__((aligned(N)))` ON A TYPEDEF: ACCEPTED **AND CONFERRED**
//  (subject: the `attrFacts.alignment` sink's `DeclarationKind::Type` arm in
//   src/analysis/semantic/semantic_analyzer.cpp, and the type-level alignment
//   skin in src/core/types/type_lattice/)
// ═══════════════════════════════════════════════════════════════════════════
//
// `typedef int __attribute__((aligned(8))) A8;` drew
// `error[S_AlignasInvalidContext]` rc 1 from DSS while gcc 13.3.0 and clang
// 18.1.3 each compile it rc 0 SILENT and CONFER the alignment. DSS was BELOW
// the union on a program every reference compiles, in the SDK-dominant spelling.
//
// ── WHAT WAS MEASURED, EACH REFERENCE SEPARATELY, rc READ DIRECTLY ──────────
// ✔MEASURED 2026-09-09, one translation unit per probe, never through a pipe:
//   gcc 13.3.0   `-std=c17 -Wall -Wextra`  rc 0, stderr EMPTY. `_Alignof(A8)`
//                                          is 8 (the `== 4` twin FAILS), in
//                                          BOTH orders. `sizeof(A8)` stays 4.
//                                          `struct S { char c; A8 v; }` is
//                                          sizeof 16 / align 8 / offsetof(v) 8.
//                                          An EXECUTED binary places a static
//                                          AND an automatic object on 8 at -O0
//                                          and -O2.
//   clang 18.1.3 `-std=c17 -Wall -Wextra`  identical on every arm.
//   mingw gcc 13.2.0                       identical on the arms given it.
//   MSVC 19.51.36257                       ABSTAINS ON THE SPELLING ONLY — it
//                                          has no `__attribute__` syntax at all
//                                          (C2143). Its vote on the CONSTRUCT is
//                                          cast through `__declspec(align(8))`
//                                          and is the SAME vote: the
//                                          `__alignof(A8)==8` arm compiles, the
//                                          `==4` twin fails C2118.
// So an over-aligned type ALIAS is conferred by ALL THREE references. An
// abstention on a surface spelling is recorded AS an abstention, never as
// agreement.
//
// ★★★ THE TRAP, AND IT IS THE WHOLE DIFFICULTY OF THIS ROW. There are TWO
// questions here and the references answer them OPPOSITELY:
//
//   construct                                   gcc     clang   MSVC    DSS
//   `typedef _Alignas(8) int A8;`               refuse  refuse  C7704   REFUSE
//   `typedef int __attribute__((aligned(8)))…`  confer  confer  (abst.) CONFER
//
// The ISO row is [[D-CSUBSET-ALIGNAS-TYPEDEF-PARAM-PARSE]] (P58); ISO C 6.7.6p2
// names `typedef` explicitly, all three references refuse, and DSS is RIGHT to
// refuse. ✔RE-MEASURED 2026-09-09 and still holding. A fix keyed on the CONTEXT
// (`this is a typedef`) rather than the SPELLING would have admitted BOTH and
// put DSS ABOVE the union in the same stroke it fixed being below it —
// [[feedback-a-partial-fix-reads-as-a-complete-one]] with the sign flipped.
// `TheIsoAlignasSpellingOnATypedefIsStillRefused` below is the control that
// fails the moment anyone does that.
//
// ⓘ MECHANISM, MEASURED RATHER THAN ASSUMED — AND IT REFUTES THE ROW'S OWN
// FRAMING. The row said "DSS currently gives ONE answer to BOTH". It gives two
// answers that happen to share one diagnostic CODE: the ISO spelling reaches the
// `declAlignasSpec` context ladder and says `alignas on a typedef`, the GNU
// spelling reaches the separate `attrFacts.alignment` sink and said
// `__attribute__((aligned(8))) on a typedef cannot be honored: …`. ✔MEASURED
// through the shipped CLI at the pre-change base — two different messages from
// two different code paths that never meet. The refusal was therefore ALREADY
// spelling-keyed, and the real blocker was REPRESENTATIONAL: a typedef interned
// to the SAME TypeId as its aliasee, so there was nowhere to put the alignment.
// That is what the type-level alignment skin fixes.

#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_interner.hpp"

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

// ★★ THESE FIXTURES MUST CARRY LAYOUT PARAMS, AND THAT IS THE SINK'S CONTRACT, NOT a
// harness detail. The conferral is gated on `s.aggregateLayout.has_value()` and on
// `want > natural` — the SAME guard the refusal it replaced carried — because
// deciding whether a request adds anything needs the aliasee's natural alignment.
// `analyzeShipped` passes NO params (it is the LSP's shape), so under it nothing is
// minted and every assertion below would read 0. Mirrors `kAlignasLayout` in
// test_semantic_analyzer_c.cpp; designated initialisers because a positional init of
// a params struct is a latent mis-assignment.
constexpr AggregateLayoutParams kAlignLayout{
    .scalarAlignment       = ScalarAlignmentRule::Natural,
    .maxAlignment          = 16,
    .maxRequestedAlignment = 268435456};

[[nodiscard]] SemanticModel analyzeWithLayout(std::initializer_list<std::string> srcs) {
    auto cu = buildShippedUnit("c", srcs);
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kAlignLayout);
}

[[nodiscard]] SymbolRecord const*
findSym(SemanticModel const& m, std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i)
        if (m.symbols()[i].name == name) return &m.symbols()[i];
    return nullptr;
}

// "What alignment does the TYPE of this name carry?" — ASSERTS the symbol exists
// first, so a typo in a fixture reads as a missing symbol rather than as a silent
// 0 that happens to match a negative expectation.
[[nodiscard]] std::uint32_t typeAlignOf(SemanticModel const& m, char const* name) {
    SymbolRecord const* s = findSym(m, name);
    EXPECT_NE(s, nullptr) << "no symbol named '" << name << "'";
    if (s == nullptr) return 0;
    return m.lattice().interner().typeAlignOverride(s->type);
}

[[nodiscard]] std::size_t badCtx(SemanticModel const& m) {
    return countCode(m.diagnostics(), DiagnosticCode::S_AlignasInvalidContext);
}

} // namespace

// ── THE DEFECT ─────────────────────────────────────────────────────────────

TEST(GnuAlignedTypedefConferral, TheAttributeIsAcceptedAndTheAlignmentIsConferred) {
    auto m = analyzeWithLayout({ "typedef int __attribute__((aligned(8))) A8;\n" });
    EXPECT_FALSE(m.hasErrors())
        << "gcc 13.3.0 and clang 18.1.3 both compile this rc 0 with stderr EMPTY; "
           "refusing it left DSS below the union";
    EXPECT_EQ(badCtx(m), 0u)
        << "the S_AlignasInvalidContext this construct used to draw must be GONE";
    EXPECT_EQ(typeAlignOf(m, "A8"), 8u)
        << "ACCEPTING AND DROPPING would trade a loud refusal for a silent wrong "
           "answer, which is the one class this project treats as unacceptable — "
           "and a mis-aligned object is a real fault on arm64, not a cosmetic one";
}

// Both orders. GNU's positional rule accepts the attribute before OR after the
// specifiers, and ✔MEASURED, gcc and clang confer from both.
TEST(GnuAlignedTypedefConferral, GnuAlignedOnATypedefInLeadingPositionConfersToo) {
    auto m = analyzeWithLayout({ "typedef __attribute__((aligned(8))) int A8;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(typeAlignOf(m, "A8"), 8u)
        << "the pre-specifier order is the one an SDK header is most likely to "
           "write; both orders are rc 0 and confer 8 on gcc and clang alike";
}

// ★ THE REPRESENTATIONAL CLAIM the old refusal said was impossible: the alias is
// a DISTINCT interned type from its aliasee. Without this, nothing else here can
// be true — every user of the alias would read the aliasee's layout.
TEST(GnuAlignedTypedefConferral, TheAliasTypeIsDistinctFromItsAliasee) {
    auto m = analyzeWithLayout({ "typedef int __attribute__((aligned(8))) A8;\ntypedef int Plain;\n" });
    ASSERT_FALSE(m.hasErrors());
    SymbolRecord const* a8 = findSym(m, "A8");
    SymbolRecord const* pl = findSym(m, "Plain");
    ASSERT_NE(a8, nullptr);
    ASSERT_NE(pl, nullptr);
    EXPECT_NE(a8->type.v, pl->type.v)
        << "the old refusal's stated reason was 'a typedef interns to the SAME "
           "TypeId as its aliasee, so writing the alignment is provably INERT'. "
           "That premise is what the skin removes; if this ever collapses again "
           "the conferral silently becomes a no-op";
    EXPECT_EQ(m.lattice().interner().typeAlignOverride(pl->type), 0u);
}

// A chained alias must not LOSE the alignment — the skin merges rather than
// nesting, so `B8` carries what `A8` carried. ✔MEASURED: gcc and clang both keep
// `_Alignof(B8) == 8` through `typedef A8 B8;`.
TEST(GnuAlignedTypedefConferral, AChainedAliasKeepsTheAlignment) {
    auto m = analyzeWithLayout({ "typedef int __attribute__((aligned(8))) A8;\ntypedef A8 B8;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(typeAlignOf(m, "B8"), 8u);
}

// An OBJECT declared through the alias inherits it, which is the half that makes
// the alignment reach codegen at all. The alias's type IS the object's type, so
// this is the property that carries through `resolveTypeNodeImpl`'s single
// hand-out site.
TEST(GnuAlignedTypedefConferral, AnObjectDeclaredThroughTheAliasCarriesTheAlignment) {
    auto m = analyzeWithLayout({ "typedef int __attribute__((aligned(8))) A8;\nA8 g;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(typeAlignOf(m, "g"), 8u)
        << "every later spelling of the alias routes through ONE resolver that "
           "hands back the typedef record's type verbatim — if the wrapper is not "
           "on that record, no object ever sees it";
}

// ── THE CONTROLS: WHAT MUST NOT MOVE ───────────────────────────────────────

// ★★★ THE ARM A CONTEXT-KEYED FIX WOULD BREAK. All three references REFUSE the
// ISO spelling on a typedef and ISO C 6.7.6p2 names `typedef` explicitly, so DSS
// must keep refusing it. This test and the first one in this file cannot both
// pass under a fix that keys on "is this a typedef".
TEST(GnuAlignedTypedefConferral, TheIsoAlignasSpellingOnATypedefIsStillRefused) {
    auto legacy = analyzeWithLayout({ "typedef _Alignas(8) int A8;\n" });
    EXPECT_TRUE(legacy.hasErrors());
    EXPECT_EQ(badCtx(legacy), 1u)
        << "gcc: 'alignment specified for typedef'; clang: 'only applies to "
           "variables and fields'; MSVC 19.51: C7704. Four refusals plus ISO C, "
           "so the union REQUIRES this refusal — [[D-CSUBSET-ALIGNAS-TYPEDEF-PARAM-PARSE]]";

    auto kw = analyzeWithLayout({ "typedef alignas(8) int A8;\n" });
    EXPECT_TRUE(kw.hasErrors());
    EXPECT_EQ(badCtx(kw), 1u) << "the C23 keyword spelling of the same request";
}

// The validation ladder is UPSTREAM of the sink and must still fire — accepting
// the construct must not have accepted a request no reference accepts.
// ✔MEASURED: gcc 'requested alignment 3 is not a positive power of 2', clang
// 'requested alignment is not a power of 2'.
TEST(GnuAlignedTypedefConferral, ANonPowerOfTwoRequestIsStillRefusedLoudly) {
    auto m = analyzeWithLayout({ "typedef int __attribute__((aligned(3))) A3;\n" });
    EXPECT_TRUE(m.hasErrors());
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_AlignasNotPowerOfTwo), 1u);
}

// An UNDECORATED typedef must gain nothing. The negative control that makes every
// positive arm above discrimination rather than a constant-true predicate.
TEST(GnuAlignedTypedefConferral, AnUndecoratedTypedefCarriesNoAlignment) {
    auto m = analyzeWithLayout({ "typedef int Plain;\nPlain p;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(typeAlignOf(m, "Plain"), 0u);
    EXPECT_EQ(typeAlignOf(m, "p"), 0u);
}

// The sibling kinds the SAME sink judges must keep their verdicts — the sink
// discriminates by kind ITSELF, in graded arms, and only the TYPE arm moved.
TEST(GnuAlignedTypedefConferral, TheFunctionAndBitFieldArmsOfTheSameSinkAreUnmoved) {
    auto fn = analyzeWithLayout({ "__attribute__((aligned(16))) int f(void);\n" });
    EXPECT_TRUE(fn.hasErrors());
    EXPECT_EQ(badCtx(fn), 1u)
        << "a FUNCTION stays LOUD: DSS has no sink for aligning code, and the "
           "measured SDK cost of refusing is 0 of 204 `aligned` sites";

    auto bf = analyzeWithLayout({ "struct S { int a : 3, __attribute__((aligned(16))) b : 5; };\n" });
    EXPECT_TRUE(bf.hasErrors());
    EXPECT_EQ(badCtx(bf), 1u) << "a BIT-FIELD member stays LOUD";
}

// A plain OBJECT carrying the attribute directly is the channel that already
// worked; it must be untouched by the typedef arm's change.
TEST(GnuAlignedTypedefConferral, TheDirectObjectChannelStillWorksAndIsNotATypeSkin) {
    auto m = analyzeWithLayout({ "__attribute__((aligned(8))) int g = 7;\n" });
    EXPECT_FALSE(m.hasErrors());
    SymbolRecord const* g = findSym(m, "g");
    ASSERT_NE(g, nullptr);
    EXPECT_TRUE(g->explicitAlignment.has_value());
    EXPECT_EQ(*g->explicitAlignment, 8u);
    EXPECT_EQ(m.lattice().interner().typeAlignOverride(g->type), 0u)
        << "an OBJECT's own request rides SymbolRecord.explicitAlignment, not the "
           "TYPE — the two channels are separate and must stay so, or `int` itself "
           "would become 8-aligned for the whole translation unit";
}
