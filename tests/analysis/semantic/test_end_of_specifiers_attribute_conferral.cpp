// ═══════════════════════════════════════════════════════════════════════════
//  WHAT AN ATTRIBUTE RUN AT THE END OF THE DECLARATION SPECIFIERS *CONFERS*
//  (subject: semantics.declarations[typedefDecl].declarationAttrSlotRules in
//   src/dss-config/sources/c.lang.json, and the shared attribute-semantics scan)
// ═══════════════════════════════════════════════════════════════════════════
//
// `typedef int [[deprecated]] T; T x;` drew `warning[S_DeprecatedSymbolUsed]`
// at the use of `T` — a meaning NOT ONE reference confers. ✔MEASURED 2026-09-09
// through the shipped CLI at the pre-change base, and with each reference probed
// SEPARATELY on its own translation unit:
//   gcc 13.3.0       `-std=c2x -Wall -Wextra`  rc 0, `'deprecated' attribute
//                                              ignored [-Wattributes]`, and
//                                              NOTHING at the use of `T`
//   clang 18.1.3     `-std=c23`                rc 1, `error: 'deprecated'
//                                              attribute cannot be applied to
//                                              types`
//   MSVC 19.51.36257 `/std:clatest /W4`        rc 2, `C2059 syntax error:
//                                              'attribute specifier'`
// Acceptance is inside the union (gcc accepts, so DSS parsing it is required);
// the CONFERRAL is above it.
//
// ★★★ THE RULE IS POSITIONAL AND IT IS THE STANDARD'S OWN. C23 6.7p9: an
// attribute specifier sequence "terminating a sequence of declaration
// specifiers appertains to the TYPE determined by the preceding sequence of
// declaration specifiers", and C23 6.7.13.4 / 6.7.13.5 open with Constraints
// admitting `maybe_unused` / `deprecated` on a declaration of a structure,
// union, typedef name, object, member, function, enumeration or enumerator —
// never on a TYPE. So the run in this ONE slot has nothing to decorate.
// ✔MEASURED, and it is a property of the POSITION rather than of the attribute:
// gcc ignores `[[deprecated]]`, `[[maybe_unused]]` AND `[[nodiscard]]` alike
// there (`attribute ignored`), and clang refuses all three
// ("cannot be applied to types"); a POINTER declarator (`typedef int
// [[deprecated]] *P;`) gets the same two verdicts.
//
// ⚠⚠ AND IT IS SPELLING-SPECIFIC, WHICH IS THE WHOLE DIFFICULTY. The GNU twin in
// the SAME slot is ACCEPTED AND HONOURED: ✔MEASURED, `typedef int
// __attribute__((deprecated)) T; T x;` warns at the use on gcc 13.3.0 AND on
// clang 18.1.3, because GNU's own positional rule attaches an `__attribute__`
// written after the specifiers to the DECLARATION. A fix that stopped
// conferral for BOTH spellings would trade one above-the-union defect for a
// below-the-union one, on a program two references compile and honour.
//
// ★★ THE THREE OTHER POSITIONS ARE PINNED HERE AS CONTROLS BECAUSE EACH HAS A
// DIFFERENT MEASURED ANSWER, and a fix keyed on the C23 spelling alone would
// break every one of them:
//   • LEADING the declaration — `[[deprecated]] typedef int T;` — gcc, clang
//     AND MSVC (C4996) all confer.                                  ⇒ CONFERS
//   • the TRAILING typedef run — `typedef int A, B [[deprecated]];` — gcc,
//     clang AND MSVC all confer, on **B** alone.        ⇒ CONFERS, LAST ONLY
//     ⓘ This one was UNMEASURED when the row was written and is measured here:
//     the two typedef attribute positions genuinely have different answers for
//     the same spelling, which is why the grain is per SLOT and not per
//     language.
//   • after the composite keyword — `struct [[deprecated]] S { … };` — gcc and
//     clang both confer (C23 6.7.2.1 puts the sequence in that production and
//     6.7.13.5 admits `deprecated` on a structure).                 ⇒ CONFERS
//
// ★★★ AND THE DROP IS **ANNOUNCED**, WHICH IS THE HALF A CONFERRAL-ONLY FIX
// WOULD HAVE MISSED. Every reference diagnoses this slot — gcc with
// `-Wattributes` and rc 0, clang and MSVC with an error — so a DSS that merely
// stopped conferring would have become the only one of four to accept the
// construct in SILENCE, trading an above-the-union meaning for a below-the-union
// silent drop. ✔MEASURED at the pre-change base, that silence was already half
// present and unnoticed: `typedef int [[maybe_unused]] T;` compiled rc 0 with
// ZERO diagnostics while `typedef int [[deprecated]] T;` conferred and
// `typedef int [[nodiscard]] T;` warned (through the unrelated `appliesTo`
// decl-kind gate, which happens to fire for `nodiscard` because its `appliesTo`
// is `function`). Three standard attributes, one slot, three different wrong
// answers. The grain now gives all three the same one, and it is the shared
// `S_AttributeIgnoredForDeclarationKind` warning — the code whose own comment
// declares it "NAMES NO ATTRIBUTE AND NO EFFECT VERB … one code covers every
// present and future verb".
//
// ── THE SHAPE ──────────────────────────────────────────────────────────────
// `declarationAttrSlotRules` declared a grain per RULE NAME, and the enum's own
// comment stated that "the shape axis is a property of the C23 SPELLING, which
// is exactly why the value is per-RULE and not language-wide". That holds on the
// DECLARATOR side, where `afterDeclaratorAttrRules` names `attrSpec` and
// `stdAttr` DIRECTLY so per-rule IS per-spelling. It does not hold on the
// DECLARATION side, where a slot names a RUN CONTAINER: `typedefAttrRun` is
// `{repeat {alt: [attrSpec, stdAttr]}}`, one name over both spellings, so the
// key could only be all-or-nothing. The slot entry now carries an optional
// `standardSpellingAppertainsTo` grain, and one shared reader composes the two
// axes (spelling, then declarator shape) for every consumer.
//
// ── RED-ON-DISABLE, REMOVE DIRECTION ───────────────────────────────────────
// Two mutants, because the change has two halves and either alone would leave a
// defect standing.
//   (1) CONFIG — delete `"standardSpellingAppertainsTo": "type"` from
//       `typedefAttrRun`'s entry in src/dss-config/sources/c.lang.json. ⚠ No
//       object md5 is involved: `c.lang.json` is copied into the build tree's
//       `dss-config-snapshot` at ctest RUN time, so nothing recompiles and no
//       binary moves — the CONFIG file's own md5 is the artefact that must move
//       and RETURN.
//   (2) ENGINE — make `attrNodeAppertainment` ignore the override (return
//       `run.appertainsTo` unresolved). Build rc and the object md5 for
//       semantic_analyzer.cpp.o must move and return.
// The transcripts are in the lane report.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/parse_diagnostic.hpp"

#include "semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

using namespace dss;
using namespace dss::sem_test;

namespace {

[[nodiscard]] SymbolRecord const*
findSym(SemanticModel const& m, std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i)
        if (m.symbols()[i].name == name) return &m.symbols()[i];
    return nullptr;
}

// "Is this name marked deprecated?" — ASSERTS the symbol exists first, so a
// typo in a fixture reads as a missing symbol rather than as a silent `false`
// that happens to match the expectation.
[[nodiscard]] bool deprecated(SemanticModel const& m, char const* name) {
    SymbolRecord const* s = findSym(m, name);
    EXPECT_NE(s, nullptr) << "no symbol named '" << name << "'";
    return s != nullptr && s->isDeprecated;
}

[[nodiscard]] std::size_t ignoredCount(SemanticModel const& m) {
    return countCode(m.diagnostics(),
                     DiagnosticCode::S_AttributeIgnoredForDeclarationKind);
}

} // namespace

// ── THE DEFECT: the C23 spelling at the end of the declaration specifiers ───

TEST(EndOfSpecifiersAttributeConferral, TheC23SpellingConfersNothingOnATypedef) {
    auto m = analyzeShipped("c", { "typedef int [[deprecated]] T;\nT x;\n" });
    EXPECT_FALSE(m.hasErrors())
        << "the program still PARSES and is still ACCEPTED — gcc compiles it at "
           "rc 0, so refusing it would be below the union; only the MEANING moved";
    EXPECT_FALSE(deprecated(m, "T"))
        << "NO reference confers here: gcc 13.3.0 ignores the attribute with "
           "-Wattributes and warns nothing at the use, clang 18.1.3 and MSVC "
           "19.51.36257 refuse the declaration outright. C23 6.7p9 makes the "
           "sequence appertain to the TYPE, and 6.7.13.5 admits `deprecated` on "
           "no type";
}

// The same slot, the same attribute, one derivation over. The answer is a
// property of the POSITION, so a pointer declarator must not change it.
TEST(EndOfSpecifiersAttributeConferral, TheC23SpellingConfersNothingThroughAPointerTypedef) {
    auto m = analyzeShipped("c", { "typedef int [[deprecated]] *P;\nP p;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_FALSE(deprecated(m, "P"))
        << "gcc ignores and clang refuses this exactly as for the bare "
           "declarator — the grain is positional, not declarator-shaped";
}

// ★★★ THE DISCRIMINATING CONTROL, AND IT IS THE ARM THAT MAKES THE FIX HARD.
// The GNU spelling occupies the SAME slot and gcc AND clang both HONOUR it. A
// fix that narrowed the slot rather than the spelling would pass every arm
// above and fail HERE.
TEST(EndOfSpecifiersAttributeConferral, TheGnuSpellingStillConfersInTheSameSlot) {
    auto m = analyzeShipped("c",
        { "typedef int __attribute__((deprecated)) T;\nT x;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_TRUE(deprecated(m, "T"))
        << "✔MEASURED: gcc 13.3.0 AND clang 18.1.3 both warn at the use of `T` "
           "for this program (MSVC abstains — it implements no __attribute__ in "
           "any position). One slot, two spellings, two grains";
}

// ── THE POSITIONS THAT MUST NOT MOVE ───────────────────────────────────────

// LEADING the declaration: all three references confer, in both spellings.
TEST(EndOfSpecifiersAttributeConferral, TheLeadingPositionStillConfers) {
    auto std_ = analyzeShipped("c", { "[[deprecated]] typedef int T;\nT x;\n" });
    EXPECT_FALSE(std_.hasErrors());
    EXPECT_TRUE(deprecated(std_, "T"))
        << "gcc, clang AND MSVC (C4996) all confer from the leading position — "
           "the arm a spelling-wide fix would have broken";

    auto gnu = analyzeShipped("c",
        { "__attribute__((deprecated)) typedef int T;\nT x;\n" });
    EXPECT_FALSE(gnu.hasErrors());
    EXPECT_TRUE(deprecated(gnu, "T"));
}

// ★★ THE TRAILING TYPEDEF RUN — the sibling slot, and the one the row that
// opened this defect recorded as NOT MEASURED. ✔MEASURED 2026-09-09, all three
// references agreeing and in BOTH spellings: `typedef int A, B [[deprecated]];`
// warns at a use of **B** and is clean at a use of **A** on gcc 13.3.0, clang
// 18.1.3 and MSVC 19.51.36257 alike. So this position CONFERS for the C23
// spelling while the post-head one does not — two positions, one spelling, two
// answers, which is why the grain is per SLOT.
TEST(EndOfSpecifiersAttributeConferral, TheTrailingTypedefRunStillConfersOnTheLastAlias) {
    auto std_ = analyzeShipped("c",
        { "typedef int A, B [[deprecated]];\nA p;\nB q;\n" });
    EXPECT_FALSE(std_.hasErrors());
    EXPECT_TRUE(deprecated(std_, "B"))
        << "all three references confer on B here — the post-head slot's answer "
           "must not have been generalized onto this one";
    EXPECT_FALSE(deprecated(std_, "A"))
        << "and it reaches the LAST alias only, in all three";

    auto gnu = analyzeShipped("c",
        { "typedef int A, B __attribute__((deprecated));\nA p;\nB q;\n" });
    EXPECT_FALSE(gnu.hasErrors());
    EXPECT_TRUE(deprecated(gnu, "B"));
    EXPECT_FALSE(deprecated(gnu, "A"));
}

// The ordinary object and function positions, both spellings. Every one of
// these is a program all three (or, for the GNU spelling, both gnu) references
// confer from, and none of them is a declaration-specifier slot.
TEST(EndOfSpecifiersAttributeConferral, TheObjectPositionsStillConfer) {
    auto afterDeclarator = analyzeShipped("c", { "int x [[deprecated]];\n" });
    EXPECT_FALSE(afterDeclarator.hasErrors());
    EXPECT_TRUE(deprecated(afterDeclarator, "x"))
        << "gcc, clang AND MSVC confer after a bare IDENTIFIER declarator";

    auto gnuPostHead = analyzeShipped("c",
        { "static int __attribute__((deprecated)) m = 1;\n" });
    EXPECT_FALSE(gnuPostHead.hasErrors());
    EXPECT_TRUE(deprecated(gnuPostHead, "m"))
        << "the GNU post-head object slot (`declAttrRun`) is the same shape as "
           "the typedef one and gcc and clang both honour it";
}

// The composite after-keyword slot is a DIFFERENT production with a DIFFERENT
// answer: C23 6.7.2.1 puts the sequence there and 6.7.13.5 admits `deprecated`
// on a structure, so gcc and clang both confer (✔MEASURED). A grain applied to
// every `declaration`-slot rule instead of to the one that needs it would have
// silenced this.
TEST(EndOfSpecifiersAttributeConferral, TheCompositeLeadSlotStillConfers) {
    auto m = analyzeShipped("c",
        { "struct [[deprecated]] S { int a; };\nstruct S v;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_TRUE(deprecated(m, "S"))
        << "gcc 13.3.0 and clang 18.1.3 both warn at `struct S v;` here";
}

// ── THE DROP IS ANNOUNCED, NOT SILENT ──────────────────────────────────────

// ★★ The half a conferral-only fix would have missed. All three references
// diagnose this slot; DSS must not become the only one that accepts it quietly.
TEST(EndOfSpecifiersAttributeConferral, TheIgnoredAttributeIsAnnounced) {
    auto dep = analyzeShipped("c", { "typedef int [[deprecated]] T;\n" });
    EXPECT_FALSE(dep.hasErrors());
    EXPECT_EQ(ignoredCount(dep), 1u)
        << "gcc says `'deprecated' attribute ignored [-Wattributes]` and exits "
           "0; clang and MSVC refuse. Silence would be a below-the-union drop "
           "traded for the above-the-union meaning that was removed";

    // ✔MEASURED at the pre-change base: this one compiled rc 0 with ZERO
    // diagnostics — the same defect, already silent, and unnoticed because no
    // arm asserted the conferral in either direction.
    auto mu = analyzeShipped("c", { "typedef int [[maybe_unused]] T;\n" });
    EXPECT_FALSE(mu.hasErrors());
    EXPECT_EQ(ignoredCount(mu), 1u)
        << "gcc ignores `maybe_unused` in this slot exactly as it ignores "
           "`deprecated`, and clang refuses it exactly as loudly — the answer is "
           "the position's, not the attribute's";
}

// ONE report per clause, not one per gate. `nodiscard` already reached the
// unrelated `appliesTo` decl-kind gate here (its `appliesTo` is `function` and a
// typedef declares a type), so this arm is what proves the positional grain
// REPLACED that report rather than doubling it.
TEST(EndOfSpecifiersAttributeConferral, TheAnnouncementIsNotDoubled) {
    auto m = analyzeShipped("c", { "typedef int [[nodiscard]] T;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(ignoredCount(m), 1u)
        << "two gates, one clause, one diagnostic";
}

// ★★ THE RUN IS STILL SCANNED — the arm that separates "confers nothing" from
// "is not read at all". A fix that skipped the slot would silently retire this
// position's unknown-name diagnostic, which gcc AND clang both emit
// (`attribute ignored` / `unknown attribute 'frobnicate' ignored`, rc 0 on both).
TEST(EndOfSpecifiersAttributeConferral, AnUnknownStandardAttributeStillReachesTheScan) {
    auto m = analyzeShipped("c", { "typedef int [[frobnicate]] T;\n" });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_TRUE(hasCode(m.diagnostics(), DiagnosticCode::S_UnknownAttribute))
        << "the scan must still WALK a run it confers nothing from — an "
           "attribute that appertains to a type is still typo-checkable";
    EXPECT_EQ(ignoredCount(m), 0u)
        << "an UNKNOWN name is reported as unknown and not additionally as "
           "ignored: one clause, one verdict";
}

// The QUIET control for the arm above, printed by name: a KNOWN name in the same
// position must NOT produce S_UnknownAttribute, or that assertion is satisfied
// by a scan that calls everything unknown.
TEST(EndOfSpecifiersAttributeConferral, AKnownStandardAttributeIsNotCalledUnknown) {
    auto m = analyzeShipped("c", { "typedef int [[deprecated]] T;\n" });
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_UnknownAttribute), 0u);
}

// ★★ THE GNU ARGUMENT-BEARING SPELLING IN THIS VERY SLOT — the shape that
// exercises `attrArgs` rather than a bare clause name. The MESSAGE is the
// witness: it can only be present if the clause's ARGUMENT was folded, so this
// arm fails if the run reached the scan but the argument path did not.
TEST(EndOfSpecifiersAttributeConferral, TheGnuSpellingCarriesItsArgumentFromThisSlot) {
    auto m = analyzeShipped("c",
        { "typedef int __attribute__((deprecated(\"use U instead\"))) T;\nT x;\n" });
    EXPECT_FALSE(m.hasErrors());
    SymbolRecord const* t = findSym(m, "T");
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(t->isDeprecated);
    EXPECT_EQ(t->deprecatedMessage, "use U instead")
        << "the GNU run in the post-head typedef slot still folds its ARGUMENT, "
           "not merely its name";
}

// ── THE SIBLING POSITION THE SAME GRAIN GOVERNS ────────────────────────────

// ★★ P56 stopped `void f(void) [[deprecated]];` from conferring and left the
// drop SILENT — ✔MEASURED at this base, rc came from the missing definition
// alone and the attribute produced nothing at all, where gcc says `'deprecated'
// attribute ignored` and clang refuses the program. The announcement is a
// property of the GRAIN, so routing both paths through one reader fixes this
// position too; pinned here so a future narrowing of the announcement to the
// declaration slot alone reds rather than passes.
TEST(EndOfSpecifiersAttributeConferral, ATypeDerivedDeclaratorRunIsAnnouncedToo) {
    auto fn = analyzeShipped("c", { "void f(void) [[deprecated]];\n" });
    EXPECT_FALSE(deprecated(fn, "f"))
        << "P56: no reference confers after a function declarator";
    EXPECT_EQ(ignoredCount(fn), 1u)
        << "and gcc ANNOUNCES the drop there exactly as it does in the "
           "end-of-specifiers slot";

    // The CONTROL that keeps it honest: a bare identifier declarator derives no
    // type, so the same spelling CONFERS and must NOT be announced.
    auto obj = analyzeShipped("c", { "int x [[deprecated]];\n" });
    EXPECT_TRUE(deprecated(obj, "x"));
    EXPECT_EQ(ignoredCount(obj), 0u)
        << "CONTROL: an announcement that fired here would mean the grain is "
           "not being read at all";
}
