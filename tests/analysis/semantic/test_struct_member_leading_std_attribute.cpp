// ═══════════════════════════════════════════════════════════════════════════
//  THE LEADING C23 `[[…]]` ATTRIBUTE SEQUENCE ON A STRUCT/UNION MEMBER
//  (subject: /shapes/structMemberDeclSpecifiers in
//   src/dss-config/sources/c.lang.json)
// ═══════════════════════════════════════════════════════════════════════════
//
// `struct S { [[maybe_unused]] int a; };` was `error[P_NoAlternativeMatched]:
// expected 'Identifier', 'BlockClose', 'VoidKeyword', … 'AttributeKeyword' or
// 'ExtensionKeyword' — got '['` at the `[`. ✔MEASURED through the shipped CLI
// at the pre-change HEAD. `structMemberDeclSpecifier` carried `extensionRun`,
// `alignasSpec` and `attrSpec` and no `stdAttr` at all, so the C23 spelling of
// the position the GNU spelling already had was a loud parse error.
//
// ★★★ ALL THREE REFERENCES ACCEPT IT — UNANIMOUS, which is as clean as this
// bar ever gets. ✔MEASURED 2026-09-09, each probed SEPARATELY on its own
// translation unit:
//   gcc 13.3.0   `-std=c2x`         rc 0, SILENT at `-Wall -Wextra` and at
//                                   `-pedantic-errors`
//   clang 18.1.3 `-std=c23`         rc 0, likewise
//   MSVC 19.51.36257 `/std:clatest` rc 0, SILENT at /W4
// C23 6.7.2.1 writes it into the grammar directly — `member-declaration:
// attribute-specifier-sequence_opt specifier-qualifier-list
// member-declarator-list_opt ;` — and 6.7.2.1p9 gives it a meaning: "The
// optional attribute specifier sequence in a member declaration appertains to
// each of the members declared by the member declarator list", which is why
// `struct S { [[deprecated]] int a, b; };` flags BOTH members on gcc and on
// clang (✔MEASURED) and why the run is folded at DECLARATION grain here.
// Both references refuse it before C23 (`-std=c17 -pedantic-errors`: gcc `ISO C
// does not support '[[]]' attributes before C2X`, clang `[[]] attributes are a
// C23 extension`), which is why the conformance probe carries
// `@min-stdc 202311`.
//
// ★ THE SHAPE MIRRORS `/shapes/declSpecifiers`' P66 TOPOLOGY EXACTLY: the run's
// LEAD element became `{alt: [{sequence: [stdAttr, {repeat stdAttr}]},
// structMemberDeclSpecifier]}`, and the tail repeat is untouched. FIRST(stdAttr)
// = {BracketOpen} is disjoint from FIRST(structMemberDeclSpecifier) =
// {ExtensionKeyword, AlignasKeyword, AlignasLegacyKeyword, AttributeKeyword},
// from FIRST(typeRefAllowingStruct) and from the `BlockClose` that ends the
// body — so nothing became speculative and `structMemberDeclSpecifier` still
// wraps the first specifier of every already-parsing member (which is what
// keeps `ParserCSmoke.StructMemberLeadingGnuAttributeRidesTheSpecifierPrefix`
// green, byte for byte).
//
// ★★ THE POSITION IS HONOURED, NOT PARSED-AND-DROPPED, AND THAT COMES FREE
// RATHER THAN BY HOPE: `structMemberDeclSpecifiers` is already the
// structField/unionField rows' declared `specifierPrefix`, which is
// `scanAttributeSemantics`'s FIRST root, and `collectAttrNodes` finds a
// `stdAttr` by descending until it matches rather than by reading a fixed
// child. The `UnknownAttributeReachesTheScan` arm below is the instrument that
// says so, with the accepted-name arm beside it as the discriminating control.
//
// ⚠ WHAT STAYS REFUSED, AND EVERY ONE OF THESE IS A REFERENCE MEASUREMENT
// RATHER THAN A CHOICE (✔MEASURED 2026-09-09):
//   • `struct S { _Alignas(16) [[maybe_unused]] int a; };` — gcc `expected
//     specifier-qualifier-list before 'int'`, clang `an attribute list cannot
//     appear here`. The sequence TERMINATES the specifier-qualifier list.
//   • `struct S { __attribute__((aligned(16))) [[maybe_unused]] int a; };` —
//     refused by gcc and clang for the same reason.
//   • `struct S { [[maybe_unused]] __extension__ int a; };` — refused by gcc
//     and clang.
//   • `struct S { int [[maybe_unused]] a; };` — the END-of-specifier-qualifier-
//     list slot, which is a DIFFERENT position and is NOT opened here: C23
//     6.7.2.1p9 makes that sequence appertain to the TYPE, and C23 6.7.13.4's
//     Constraints do not admit `maybe_unused` on a type. clang refuses it
//     (`'maybe_unused' attribute cannot be applied to types`), MSVC refuses it
//     (C2143/C2059) and gcc accepts-and-DROPS it (`attribute ignored`, and the
//     effect is measurably gone). DSS refusing it is inside the union, and this
//     arm is what keeps this change from silently widening into that one.
//
// ⓘ ONE ADJACENT DIRECTION-A GAP IS MEASURED AND **NOT** CLOSED HERE, said out
// loud so a reader does not mistake this file for covering it:
// `struct S { __extension__ [[maybe_unused]] int a; };` is ACCEPTED by gcc AND
// clang and REFUSED by DSS — before this change and after it, unchanged,
// because `extensionRun` is admitted only as the LEAD of
// `structMemberDeclSpecifier` and the tail admits no `stdAttr`. It is
// deliberately NOT pinned as a refusal arm: an assertion that DSS refuses what
// two references accept is a ratchet against ever fixing it.
//
// ── RED-ON-DISABLE, REMOVE DIRECTION ───────────────────────────────────────
// CONFIG DOCUMENT, so there is no object md5 to move — the CONFIG md5 is the
// moved artefact (`c.lang.json` is copied into `dss-config-snapshot` at ctest
// RUN time, so nothing recompiles). Collapse
// `/shapes/structMemberDeclSpecifiers`' lead `{alt}` back to the bare
// `"structMemberDeclSpecifier"`: every acceptance arm below goes red and every
// refusal arm and every control stays green. The transcript is in the lane
// report.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include "semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>

using namespace dss;
using namespace dss::sem_test;

namespace {

// The layout parameters the alignas pins use (natural scalar alignment, 16-byte
// stack alignment), so `_Alignof` folds are EXACT. ⚠ `analyzeShipped` supplies
// none, and without them an aggregate `_Alignof` cannot fold at all — measured
// the hard way: the witness arm below reported an error under `analyzeShipped`
// for a program the shipped CLI compiles at rc 0.
constexpr AggregateLayoutParams kLayout{ScalarAlignmentRule::Natural, 16};

[[nodiscard]] std::size_t unknownAttrCount(SemanticModel const& model) {
    return countCode(model.diagnostics(), DiagnosticCode::S_UnknownAttribute);
}

[[nodiscard]] SemanticModel analyzeWithLayout(std::string source) {
    auto cu = buildShippedUnit("c", {std::move(source)});
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                   kLayout);
}

// "Does DSS refuse this program?" is a question about the WHOLE front end: a
// construct the PARSER rejects never reaches the analyzer, so a model-only read
// can come back error-free for a translation unit the compiler refused.
[[nodiscard]] bool frontEndRefuses(std::string source) {
    auto cu = buildShippedUnit("c", {std::move(source)});
    for (auto const& t : cu->trees())
        if (t.diagnostics().hasErrors()) return true;
    return analyze(cu, DiagnosticBudget::libraryDefault()).hasErrors();
}

} // namespace

// ── THE CONTROLS FIRST: every member-prefix spelling that already worked ───

TEST(StructMemberLeadingStdAttribute, TheExistingMemberPrefixSpellingsAreUntouched) {
    EXPECT_FALSE(frontEndRefuses(
        "struct A { __attribute__((aligned(8))) int a; };\n"
        "struct B { _Alignas(16) int a; };\n"
        "struct C { alignas(16) int a; };\n"
        "struct D { __extension__ long long b; };\n"
        "struct E { __extension__ __attribute__((aligned(4))) int a; };\n"
        "struct F { int x __attribute__((aligned(8))); };\n"
        "struct G { int a, b; };\n"
        "union  H { __attribute__((aligned(8))) int a; long b; };\n"));
}

// ── THE GAP: the C23 spelling of the position the GNU spelling already had ──

TEST(StructMemberLeadingStdAttribute, ALeadingSequenceIsAcceptedOnAStructMember) {
    EXPECT_FALSE(frontEndRefuses("struct S { [[maybe_unused]] int a; };\n"))
        << "gcc 13.3.0, clang 18.1.3 AND MSVC 19.51.36257 all accept this, "
           "silently — C23 6.7.2.1's member-declaration production";
}

TEST(StructMemberLeadingStdAttribute, ALeadingSequenceIsAcceptedOnAUnionMember) {
    EXPECT_FALSE(frontEndRefuses("union U { [[maybe_unused]] int a; long b; };\n"))
        << "one attribute must not mean two things depending on whether the "
           "composite is a struct or a union";
}

// It is an attribute-specifier-SEQUENCE: more than one `[[…]]` may lead.
TEST(StructMemberLeadingStdAttribute, TwoLeadingSequencesAreAccepted) {
    EXPECT_FALSE(frontEndRefuses(
        "struct S { [[maybe_unused]] [[deprecated]] int a; };\n"));
}

// The sequence LEADS the specifier-qualifier list, so an alignment specifier
// or a GNU attribute may follow it — both accepted by gcc and clang.
TEST(StructMemberLeadingStdAttribute, ASpecifierMayFollowTheLeadingSequence) {
    EXPECT_FALSE(frontEndRefuses(
        "struct S { [[maybe_unused]] _Alignas(16) int a; };\n"
        "struct T { [[maybe_unused]] __attribute__((aligned(16))) int a; };\n"));
}

// ★★ AND THE FOLLOWING SPECIFIER IS STILL HONOURED — the assertion that says
// the new lead branch did not make `firstAlignasSpecInPrefix` miss the run it
// reads. A `_Static_assert` is the instrument because it fails the COMPILE if
// the alignment did not reach the type.
TEST(StructMemberLeadingStdAttribute, AlignasSurvivesTheLeadingSequence) {
    auto model = analyzeWithLayout(
        "struct S { [[maybe_unused]] _Alignas(16) int a; };\n"
        "_Static_assert(_Alignof(struct S) == 16, \"alignas must survive\");\n");
    EXPECT_FALSE(model.hasErrors())
        << "the leading `[[…]]` run must not displace the alignment specifier "
           "the same prefix carries";
}

// The DISCRIMINATING control for the arm above, printed by name: the same
// `_Static_assert` against a member with NO alignment request must FAIL, or the
// assertion above is satisfied by an assert that never evaluates.
TEST(StructMemberLeadingStdAttribute, TheAlignasWitnessIsDiscriminating) {
    auto model = analyzeWithLayout(
        "struct S { [[maybe_unused]] int a; };\n"
        "_Static_assert(_Alignof(struct S) == 16, \"CONTROL: must fail\");\n");
    EXPECT_TRUE(hasCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed))
        << "CONTROL: without `_Alignas(16)` the witness must not pass, else "
           "the previous arm proves nothing";
}

TEST(StructMemberLeadingStdAttribute, ItRidesTheDeclarationGrainAcrossADeclaratorList) {
    EXPECT_FALSE(frontEndRefuses("struct S { [[deprecated]] int a, b; };\n"))
        << "C23 6.7.2.1p9: the member-declaration sequence appertains to EACH "
           "member of the declarator list — gcc and clang both flag `a` and `b`";
}

TEST(StructMemberLeadingStdAttribute, ItComposesWithABitField) {
    EXPECT_FALSE(frontEndRefuses("struct S { [[maybe_unused]] int a : 3; };\n"))
        << "gcc and clang both accept a leading sequence on a bit-field member";
}

// ── HONOURED, NOT PARSED-AND-DROPPED ───────────────────────────────────────
//
// The failure this repository refuses is an attribute that parses and then
// disappears. `S_UnknownAttribute` is the instrument that proves the new
// position REACHES `scanAttributeSemantics`: it can only fire from a run the
// scan actually walked.

TEST(StructMemberLeadingStdAttribute, UnknownAttributeReachesTheScan) {
    auto model = analyzeShipped("c", {"struct S { [[frobnicate]] int a; };\n"});
    EXPECT_EQ(unknownAttrCount(model), 1u)
        << "the leading member run must be SCANNED — a position that parses "
           "and is never read is a silently dropped attribute";
}

// One attribute, two spellings, one behaviour: the GNU spelling in the SAME
// position already reports, and this is what says the C23 spelling is not
// getting a quieter deal.
TEST(StructMemberLeadingStdAttribute, TheGnuSpellingInTheSamePositionAgrees) {
    auto model = analyzeShipped("c", {
        "struct S { __attribute__((frobnicate)) int a; };\n",
    });
    EXPECT_EQ(unknownAttrCount(model), 1u);
}

// The discriminating control: a KNOWN name in the same position reports
// nothing, so the arm above is measuring the name and not the position.
TEST(StructMemberLeadingStdAttribute, AKnownAttributeInTheSamePositionIsQuiet) {
    auto model = analyzeShipped("c", {"struct S { [[maybe_unused]] int a; };\n"});
    EXPECT_EQ(unknownAttrCount(model), 0u);
}

// ── WHAT STAYS REFUSED, AND WHY — every one a reference measurement ────────

TEST(StructMemberLeadingStdAttribute, ASequenceAfterAnotherSpecifierIsRefused) {
    EXPECT_TRUE(frontEndRefuses(
        "struct S { _Alignas(16) [[maybe_unused]] int a; };\n"))
        << "gcc: `expected specifier-qualifier-list before 'int'`; clang: "
           "`an attribute list cannot appear here`";

    EXPECT_TRUE(frontEndRefuses(
        "struct S { __attribute__((aligned(16))) [[maybe_unused]] int a; };\n"))
        << "gcc and clang both refuse a `[[…]]` after a GNU attribute here";
}

TEST(StructMemberLeadingStdAttribute, ASequenceBeforeExtensionIsRefused) {
    EXPECT_TRUE(frontEndRefuses(
        "struct S { [[maybe_unused]] __extension__ int a; };\n"))
        << "gcc: `expected identifier or '(' before '__extension__'`; clang: "
           "`type name requires a specifier or qualifier`";
}

// ★★ THE END-OF-SPECIFIER-QUALIFIER-LIST SLOT IS A DIFFERENT POSITION AND IS
// DELIBERATELY NOT OPENED. C23 6.7.2.1p9 makes that sequence appertain to the
// TYPE denoted by the preceding specifier-qualifiers, and C23 6.7.13.4's
// Constraints admit `maybe_unused` only on a declaration of a structure, union,
// typedef name, object, member, function, enumeration, enumerator or label —
// never on a type. ✔MEASURED: clang `'maybe_unused' attribute cannot be applied
// to types` rc 1, MSVC C2143/C2059 rc 2, and gcc rc 0 with `attribute ignored`
// AND the effect measurably gone. A loud refusal is inside the union.
TEST(StructMemberLeadingStdAttribute, TheEndOfSpecifierListSlotStaysRefused) {
    EXPECT_TRUE(frontEndRefuses("struct S { int [[maybe_unused]] a; };\n"))
        << "clang and MSVC refuse this; gcc accepts it and DROPS the "
           "attribute, so its acceptance buys nothing the union requires";
}
