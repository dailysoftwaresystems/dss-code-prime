// ===========================================================================
// P65 lane `fs` — FILE-SCOPE DECLARATION DEFINEDNESS
//
// THE PROPERTY THIS FILE OWNS: what a FILE-SCOPE declaration DEFINES, on the
// three axes where DSS sat BELOW the reference union under
// `DSS = (gcc u clang u MSVC) u ISO C`:
//
//   (1) the FILE-SCOPE half of [[D-FF2-3]], which this cycle narrows —
//       `extern int x = 0;` at FILE scope. C 6.9.2p1: "a declaration of an
//       identifier for an object that has file scope WITH AN INITIALIZER is a
//       definition". The `extern` is REDUNDANT there, not contradictory.
//       DSS refused it with H_ExternHasInitializer, whose message
//       ("storage lives in another translation unit") stated something the
//       standard does not.
//   (2) [[D-CSUBSET-AUTO-FILE-SCOPE]] — file-scope `auto g = 42;` (C23 6.7.9).
//       DSS had no inference form in the top-level grammar at all, so the
//       construct was a loud P_NoAlternativeMatched.
//   (3) The DECIDED DIVERGENCE that pairs with them: file-scope `register`
//       stays REFUSED in every spelling — see the block at the bottom, which
//       records the measurement that decided it rather than assuming it.
//
// ★★★ THE POINT IS WHAT THE CONSTRUCT MEANS, NOT THAT IT PARSES. Both new
// capabilities are DEFINITIONS: they must take storage, take the right
// LINKAGE, and still COLLIDE with a second definition. A change that only made
// them rc 0 would be the worse failure — this project shipped a grammar change
// in this very cycle that made `const int g = 1; g = 2;` compile clean. So
// every acceptance arm below is paired with the definedness fact it must carry
// and with a refusal that must STILL fire.
//
// ✔REFERENCE VOTES — each reference probed SEPARATELY on its own translation
// unit, 2026-09-08:
//   gcc 13.3.0    `-std=c2x -c`
//   clang 18.1.3  `-std=c23 -c`
//   MSVC 19.51.36231  `cl /nologo /c /std:c17`  AND  `/std:clatest`
//
//   `extern int x = 0;` at FILE scope   gcc rc=0 (warns "'x' initialized and
//       declared 'extern'"), clang rc=0 (warns -Wextern-initializer), MSVC
//       rc=0 SILENTLY in both modes. `nm` on gcc's and clang's objects shows a
//       DEFINED symbol; two such definitions across TUs collide at link on
//       both (rc=1). ⇒ REQUIRED, and required to MEAN a definition.
//   `extern int x = 0;` at BLOCK scope  ALL THREE REFUSE (C 6.7.11p5):
//       gcc "'x' has both 'extern' and initializer", clang "declaration of
//       block scope identifier with linkage cannot have an initializer",
//       MSVC error C2205. ⇒ the loud refusal MUST survive.
//   file-scope `auto g = 42;`           gcc rc=0, clang rc=0; `nm` shows `D g`
//       on both (DEFINED, external linkage), and a sibling TU's
//       `extern int g;` links and reads 11 back. MSVC ABSTAINS: it accepts the
//       spelling but as the C89 STORAGE CLASS over an implicit `int` (it also
//       accepts `auto g;`, and REFUSES `static auto g = 42;` with C2159
//       "more than one storage class"), so it is not voting on C23 inference.
//   `static auto g = 42;`               gcc rc=0, clang rc=0, `nm` shows `d g`
//       (INTERNAL linkage). MSVC abstains as above.
//
// ── RED-ON-DISABLE, REMOVE DIRECTION ────────────────────────────────────────
// ⚠ THE (2)/(3) MUTANT IS THE DOCUMENT, SO NO OBJECT md5 IS INVOLVED, and
// saying so is part of the transcript rather than an omission: `c.lang.json` is
// read at RUN time through `$DSS_CONFIG_ROOT`, so no translation unit
// recompiles and no binary moves. What moves is the CONFIG FILE's md5, recorded
// moved-and-returned in the lane report. Mutant: DELETE `autoInferredTopLevelDecl`
// from `topLevel`'s `alt` list (taking the capability AWAY — an ADD-direction
// mutant would stay green when the real config loses the feature).
// The (1) mutant IS engine source, so source AND object md5 both move and
// return: delete the `rec->isExternDeclaration` split in `lowerExternDeclInto`
// (cst_to_hir.cpp) so every initialized declarator goes back to
// H_ExternHasInitializer.
// CONTROLS printed by name in the transcript, which must stay GREEN through the
// (2) mutant and prove the arms are not passing for a shared reason:
// `BlockScopeExternInitializerStaysNonDefining` (item 1 is engine-side, not
// grammar) and `FileScopeRegisterStaysRefusedInEverySpelling` (the decided
// divergence).
//
// ⚠ THE `auto` HALF LANDED WITH A NAMED, MEASURED RESIDUE, AND P65's LANE `pl`
// CLOSED IT —
// [[D-C-FILE-SCOPE-INFERRED-AUTO-MUST-LEAD-THE-DECLARATION-SPECIFIERS]].
// The head-less top-level rule was `auto`-LED because a
// specifier-led one gave {static, const, …} a second top-level candidate and
// put `topLevelDecl` — function bodies and all — inside a token-budgeted probe.
// The parser now DESCENDS into an outermost alt's final candidate when that
// candidate is the declared-last STRUCTURAL one, which is the rule the all-fail
// path already replayed with no budget, so the budget never binds on the
// reading the parser was going to take. `topLevelAutoSpecifiers` is therefore an
// ORDER-FREE run, `SpecifierLedFileScopeAutoIsARefusedResidue` keeps its NAME
// and holds the half that did NOT invert, and the positives moved to
// `SpecifierLedFileScopeAutoMeansWhatTheAutoLedOrderMeans`.
//
// ⚠⚠ THE SECOND RED-ON-DISABLE MUTANT, AND IT IS AN ENGINE ONE (source AND
// object md5 both move, unlike the config mutant above): delete the
// `finalCandidateDirectDescent_()` call from `abandonAndAdvance_` in
// src/analysis/syntactic/parser.cpp. ✔EXERCISED 2026-09-08 through `ctest`,
// source md5 f3fd6495 -> 8eaa1f76 -> f3fd6495 and object md5
// fcd17dc4 -> a04dab35 -> fcd17dc4 (both moved and RETURNED). RED in this
// file: `LargeQualifierLedFileScopeInitializerIsNotProbed`,
// `LargeQualifierLedFunctionBodyIsNotProbed`,
// `LargeFileScopeInitializerAfterALeadingSpecifierIsNotProbed`, AND — ⚠ this
// is the half a prediction got wrong and the run corrected —
// `LargeFileScopeInitializerSurvivesTheOrder` and
// `LargeStaticFunctionBodySurvivesTheOrder` GO RED TOO. They were written as
// `static`-led on the reasoning that `static` had ONE top-level owner, and
// that stopped being true in the SAME change: the order-free specifier run
// gives `static` a second owner, so those two now depend on the descent
// exactly as the new arms do. Their prose still says "nothing probes it",
// which is true — the reason it is true simply moved from the FIRST-set to
// the descent.
// GREEN CONTROLS, printed by name from that run: every arm of
// `SpecifierLedFileScopeAutoMeansWhatTheAutoLedOrderMeans` and of
// `SpecifierLedFileScopeAutoIsARefusedResidue` (their declarations are short,
// so no budget binds — the GRAMMAR half is untouched), both runners of
// `examples/c/auto_file_scope_specifier_order`, and, in the syntactic suite,
// `ParserSpeculation.FinalCandidateDescentKeepsTheReplaysDiagnostics` and
// `.FinalCandidateDescentIsGatedOnTheFallbackReading` — the two arms that
// assert the descent changes NOTHING, which must stay green with the descent
// removed or they were never asserting that.
// The config mutant and this one have DISJOINT red sets, which is what says
// each hit its own half.
// ===========================================================================

#include "analysis/semantic/semantic_model.hpp"
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
symbolNamed(SemanticModel const& model, std::string_view name) {
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == name) return &model.symbols()[i];
    }
    return nullptr;
}

// True iff ANY tree in the unit reported an ERROR-severity diagnostic — the
// PARSE tier, which `SemanticModel::hasErrors()` does not cover. A construct
// the grammar refuses never reaches semantic analysis at all, so a test that
// asked only the model would read a parse rejection as success.
[[nodiscard]] bool hasParseError(CompilationUnit const& cu) {
    for (auto const& t : cu.trees())
        for (auto const& d : t.diagnostics().all())
            if (d.severity == DiagnosticSeverity::Error) return true;
    return false;
}

} // namespace

// ── (1) `extern <init>` at FILE scope is a DEFINITION ───────────────────────

// The core fact, on the SYMBOL rather than on the exit code: the declaration
// must stop being an extern DECLARATION and stop being a TENTATIVE definition
// — it is a real definition, which is the only reading under which a second
// definition can still collide.
TEST(FileScopeDeclarationDefinedness, ExternWithInitializerIsADefinition) {
    auto model = analyzeShipped("c", {"extern int x = 7;\n"});
    EXPECT_FALSE(model.hasErrors())
        << "C 6.9.2p1: gcc, clang and MSVC all accept this at file scope";
    auto const* x = symbolNamed(model, "x");
    ASSERT_NE(x, nullptr);
    EXPECT_FALSE(x->isExternDeclaration)
        << "the initializer DEFINES the object; `extern` is redundant, so this "
           "declaration must not announce storage in another translation unit";
    EXPECT_FALSE(x->isTentativeDefinition)
        << "a declarator WITH an initializer is a real definition, never "
           "tentative (C 6.9.2p2)";
    EXPECT_FALSE(x->isInternalLinkage)
        << "no internal-linkage declaration precedes it, so C 6.2.2p4 leaves "
           "it EXTERNAL — gcc and clang both emit a global `D x`";
}

// PER-DECLARATOR, which is the half a declaration-level answer gets wrong in
// both directions at once. ✔MEASURED: `extern int a = 1, b;` on gcc AND clang
// emits `D a` and NO symbol at all for `b`.
TEST(FileScopeDeclarationDefinedness, ExternInitializerIsPerDeclarator) {
    auto model = analyzeShipped("c", {"extern int a = 1, b;\n"});
    EXPECT_FALSE(model.hasErrors());
    auto const* a = symbolNamed(model, "a");
    auto const* b = symbolNamed(model, "b");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_FALSE(a->isExternDeclaration) << "`a = 1` defines";
    EXPECT_TRUE(b->isExternDeclaration)
        << "`b` carries no initializer, so it stays an ordinary extern "
           "declaration — a declaration-level answer would invent storage for "
           "it";
}

// C 6.2.2p4: a prior INTERNAL-linkage declaration outranks the redundant
// `extern`. ✔MEASURED on gcc: `static int x; extern int x = 0;` emits a LOCAL
// symbol (`b x`, lower case), not a global one — so this is a linkage fact,
// not a formatting one.
TEST(FileScopeDeclarationDefinedness, ExternInitializerInheritsInternalLinkage) {
    auto model = analyzeShipped("c", {"static int x;\nextern int x = 0;\n"});
    EXPECT_FALSE(model.hasErrors());
    auto const* x = symbolNamed(model, "x");
    ASSERT_NE(x, nullptr);
    EXPECT_TRUE(x->isInternalLinkage)
        << "the `static` tentative declaration precedes it, so the entity keeps "
           "INTERNAL linkage (C 6.2.2p4)";
}

// The refusal that must STILL fire: two real definitions of one file-scope
// object collide. This is the arm that would go silent if the fix had merely
// stopped refusing the construct instead of re-classifying it.
TEST(FileScopeDeclarationDefinedness, ExternInitializerStillCollidesWithADefinition) {
    auto model = analyzeShipped("c", {"extern int x = 0;\nint x = 1;\n"});
    EXPECT_TRUE(hasCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol))
        << "gcc and clang both report `redefinition of 'x'` and MSVC C2374";
}

// CONTROL for the arm above — a plain redeclaration WITHOUT a second
// initializer must still merge silently, so the collision cannot be passing
// because every redeclaration now collides.
TEST(FileScopeDeclarationDefinedness, ExternInitializerMergesWithATentativeDefinition) {
    auto model = analyzeShipped("c", {"extern int x = 5;\nint x;\n"});
    EXPECT_FALSE(model.hasErrors())
        << "a tentative definition after a real one merges — gcc and clang "
           "both rc=0";
}

// ⚠ THE SCOPE GATE, and it is the arm that keeps the fix from being a
// regression. C 6.7.11p5 forbids an initializer on a block-scope declaration of
// an identifier WITH LINKAGE, and all three references refuse it: gcc "'x' has
// both 'extern' and initializer", clang "declaration of block scope identifier
// with linkage cannot have an initializer", MSVC error C2205.
//
// ⚠⚠ THE ASSERTION IS ON THE SYMBOL, NOT ON `H_ExternHasInitializer`, AND THAT
// IS DELIBERATE RATHER THAN A WEAKER TEST. This suite's fixture runs the front
// end to SEMANTIC ANALYSIS; `H_ExternHasInitializer` is an HIR-LOWERING
// diagnostic and never reaches a `SemanticModel`, so an arm that asked for it
// here would assert on a diagnostic that cannot appear — it would go green the
// day the refusal was DELETED. What this arm pins is the discriminator the
// refusal is BUILT ON: Pass 1 must still mark a block-scope `extern` with an
// initializer as an extern DECLARATION, because `lowerExternDeclInto` reads
// exactly that field (`rec->isExternDeclaration`) to choose between emitting the
// loud refusal and emitting a Global. Flip this bit and the block-scope arm
// silently becomes a local definition.
// ✔The end-to-end refusal IS measured, through the shipped CLI rather than here:
//   dsscp --compile … -e 'int main(void){ extern int x = 0; return x; }'
//   → error[H_ExternHasInitializer] … "at block scope" … rc=1
TEST(FileScopeDeclarationDefinedness, BlockScopeExternInitializerStaysNonDefining) {
    auto model = analyzeShipped(
        "c", {"int main(void) { extern int x = 0; return x; }\n"});
    auto const* x = symbolNamed(model, "x");
    ASSERT_NE(x, nullptr);
    EXPECT_TRUE(x->isExternDeclaration)
        << "C 6.7.11p5 — the file-scope C 6.9.2p1 override must NOT reach block "
           "scope; this is the field the HIR tier's loud refusal branches on";
}

// The aggregate and qualified spellings ride the same route, so a fix that
// only handled a scalar `int` would read as complete. All four are accepted by
// gcc and clang (with the extern-initializer warning) and by MSVC silently.
TEST(FileScopeDeclarationDefinedness, ExternInitializerCarriesTheDeclaratorsShape) {
    for (char const* const src : {
             "extern const int c = 5;\n",
             "extern int arr[3] = {1, 2, 3};\n",
             "extern int *p = 0;\n",
             "extern int x;\nextern int x = 9;\n",
         }) {
        auto model = analyzeShipped("c", {std::string{src}});
        EXPECT_FALSE(model.hasErrors()) << src;
    }
}

// ── (2) file-scope `auto` inference ─────────────────────────────────────────

// The positive that [[D-CSUBSET-AUTO-FILE-SCOPE]] existed for. The type is
// INFERRED, which is the fact a mere "it parses" assertion would miss.
TEST(FileScopeDeclarationDefinedness, AutoFileScopeInfersAndDefinesAGlobal) {
    auto model = analyzeShipped("c", {"auto g = 42;\n"});
    EXPECT_FALSE(model.hasErrors())
        << "C23 6.7.9 admits file-scope auto; gcc and clang both accept";
    auto const* g = symbolNamed(model, "g");
    ASSERT_NE(g, nullptr);
    ASSERT_TRUE(g->type.valid());
    EXPECT_EQ(model.lattice().interner().kind(g->type), TypeKind::I32);
    EXPECT_FALSE(g->isExternDeclaration);
    EXPECT_FALSE(g->isTentativeDefinition)
        << "an inference declaration ALWAYS carries an initializer, so it is "
           "always a real definition — never tentative (C 6.9.2)";
    EXPECT_FALSE(g->isInternalLinkage)
        << "gcc and clang both emit `D g` — EXTERNAL linkage, and a sibling "
           "TU's `extern int g;` links against it";
}

// The inference is a real type computation at file scope, not an int default:
// three initializers, three different declared types, including the C23
// array/function DECAY the block-scope row already performs.
TEST(FileScopeDeclarationDefinedness, AutoFileScopeInfersTheInitializersType) {
    auto model = analyzeShipped("c", {
        "auto d = 1.5;\n"
        "auto s = \"hi\";\n"
        "auto c = (char)3;\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const& in = model.lattice().interner();
    auto const* d = symbolNamed(model, "d");
    auto const* s = symbolNamed(model, "s");
    auto const* c = symbolNamed(model, "c");
    ASSERT_NE(d, nullptr);
    ASSERT_NE(s, nullptr);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(d->type.valid());
    ASSERT_TRUE(s->type.valid());
    ASSERT_TRUE(c->type.valid());
    EXPECT_EQ(in.kind(d->type), TypeKind::F64);
    EXPECT_EQ(in.kind(s->type), TypeKind::Ptr)
        << "C23 6.7.9: an array initializer DECAYS — gcc and clang both make "
           "`sizeof(s) == sizeof(char *)`";
    EXPECT_NE(in.kind(c->type), TypeKind::I32)
        << "`(char)3` infers CHAR, not int — a default-to-int implementation "
           "would pass every other arm in this test";
}

// The specifier combinations C23 6.7.2p2 admits beside an inferred `auto`, all
// of which gcc and clang accept at file scope. `static` must confer INTERNAL
// linkage (gcc emits `d g`, lower case), which is the meaning half.
TEST(FileScopeDeclarationDefinedness, AutoFileScopeTakesItsStorageClass) {
    auto stat = analyzeShipped("c", {"auto static g = 42;\n"});
    EXPECT_FALSE(stat.hasErrors());
    auto const* sg = symbolNamed(stat, "g");
    ASSERT_NE(sg, nullptr);
    EXPECT_TRUE(sg->isInternalLinkage)
        << "`auto static` at file scope is INTERNAL — gcc emits `d g`";

    auto konst = analyzeShipped("c", {"auto const k = 42;\n"});
    EXPECT_FALSE(konst.hasErrors());
    auto const* kk = symbolNamed(konst, "k");
    ASSERT_NE(kk, nullptr);
    EXPECT_TRUE(kk->isConst)
        << "the qualifier binds through the specifier PREFIX exactly as it "
           "does at block scope; gcc and clang both refuse a later `k = 6;`, "
           "and gcc emits `R k` (read-only)";

    for (char const* const src : {
             "auto constexpr k = 42;\n",
             "auto thread_local k = 42;\n",
             "auto const static k = 42;\n",
         }) {
        auto model = analyzeShipped("c", {std::string{src}});
        EXPECT_FALSE(model.hasErrors()) << src;
    }
}

// ⚠⚠ P65, lane `pl` — THE RESIDUE THIS ARM WAS OPENED FOR INVERTED, AND THE
// NAME IS KEPT ON PURPOSE because [[D-CSUBSET-AUTO-FILE-SCOPE]]'s registry row
// cites it. [[D-C-FILE-SCOPE-INFERRED-AUTO-MUST-LEAD-THE-DECLARATION-SPECIFIERS]]
// CLOSED in P65: `topLevelAutoSpecifiers` is an ORDER-FREE run around a required
// `AutoKeyword`, so `static auto g = 42;`, `const auto k = 42;`,
// `constexpr auto k = 42;` and `thread_local auto g = 42;` now parse, infer and
// carry their specifier's meaning. Their assertions moved to the named sibling
// `SpecifierLedFileScopeAutoMeansWhatTheAutoLedOrderMeans` below, which asserts
// the two orders agree on MEANING and not merely on acceptance.
//
// WHAT THIS PIN KEEPS IS THE HALF THAT DID NOT INVERT: a SPECIFIER-LED spelling
// must not become a silent accept just because the specifier run now admits a
// leading specifier. Every source below leads with a specifier, structurally
// matches the head-less production, and is refused — each one by a NAMED
// diagnostic rather than by the grammar running out of alternatives, which is
// the improvement the order-free run buys even where the answer is still "no".
// ✔MEASURED 2026-09-08 through the shipped CLI (`--compile`), each reference
// probed SEPARATELY on its own translation unit:
//   const auto p = 1, q = 2;   DSS error[S_AutoRequiresSingleDeclarator]
//       — C23 6.7.9p2 admits ONE declarator; gcc 13.3.0 (-std=c2x) REFUSES it
//         too. ⚠ clang 18.1.3 (-std=c23) ACCEPTS it (`nm`: R p, R q), so this
//         is a DECIDED divergence inherited from [[D-CSUBSET-AUTO-FILE-SCOPE]]
//         (its `auto g = 1, h = 2;` arm), not a new one, and it is stated here
//         rather than left to look like agreement.
//   static auto *p = 0;        DSS error[S_AutoRequiresPlainIdentifier]
//       — gcc AND clang both REFUSE.
//   static auto g;             DSS error[S_AutoRequiresInitializer]
//       — gcc AND clang both REFUSE.
//   static x = 5;              still a loud refusal, no `auto` to infer from.
//
// ⚠⚠ P65, lane `sp` — TWO ENTRIES LEFT THIS LIST, AND THEY WERE THE TWO THIS
// ARM'S OWN PROSE ALREADY SAID DID NOT BELONG IN IT. `extern auto g = 1;` and
// `auto extern g = 1;` were listed as refusals while the paragraph above them
// called them "ONE MEASURED NON-MEMBER … a specifier-SET gap, not an ORDER one,
// so it is not this row's residue" — the sentence and the assertion disagreed
// from the day both were written, and the assertion is what a run reads. The
// SET gap is now closed: both spellings are rc=0, both DEFINE an externally
// linked object, and their meaning is pinned by
// `SpecifierSetExternAutoAndInlineObject.*` in
// tests/analysis/semantic/test_specifier_set_extern_auto_and_inline_object.cpp.
// ✔The references had said so all along: gcc 13.3.0 (-std=c2x) and clang 18.1.3
// (-std=c23) each rc=0 with `nm` reading `D g`, `-pedantic-errors` clean on
// both, and C23 6.7.2p2 lists `auto` as combinable with every storage-class
// specifier except `typedef`.
//
// ⓘ THE `inline` HALF OF THAT PARAGRAPH STAYS TRUE and is now wider: `inline
// auto i = 1;`, `auto inline i = 1;`, plain `inline int i = 1;` AND — since the
// same lane repaired a hole in that refusal — `extern inline int i = 1;` and
// `extern inline auto i = 1;` all give the same S_InlineNonFunction. It is a
// DECIDED divergence with an experiment behind it (gcc accepts and emits a
// BYTE-IDENTICAL object; clang and MSVC refuse on point), order-independent,
// and it is owned by the sibling file rather than re-pinned here.
//
// WHAT REMAINS BELOW is the half that genuinely did not invert, and the list is
// still non-vacuous four ways over plus the live control that follows it.
TEST(FileScopeDeclarationDefinedness, SpecifierLedFileScopeAutoIsARefusedResidue) {
    for (char const* const src : {
             "const auto p = 1, q = 2;\n",
             "static auto *p = 0;\n",
             "static auto g;\n",
             "static x = 5;\n",
         }) {
        auto cu = buildShippedUnit("c", {std::string{src}});
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
        EXPECT_TRUE(hasParseError(*cu) || model.hasErrors())
            << "must stay LOUD rather than become a silent implicit-int: "
            << src;
    }

    // THE LIVE CONTROL, and it is what makes the arms above non-vacuous: the
    // specifier-LED order itself is accepted now, so none of them can be
    // passing because a leading specifier is still refused wholesale.
    auto ok = analyzeShipped("c", {"static auto g = 42;\n"});
    EXPECT_FALSE(ok.hasErrors())
        << "the row "
           "[[D-C-FILE-SCOPE-INFERRED-AUTO-MUST-LEAD-THE-DECLARATION-SPECIFIERS]]"
           " closed in P65 — C 6.7p2 makes the declaration specifiers an "
           "unordered set and gcc AND clang both accept this";
}

// ★★★ THE INVERTED HALF, AND IT ASSERTS MEANING RATHER THAN ACCEPTANCE. A
// grammar change that makes a declaration PARSE into the right rule while the
// specifier written on the WRONG SIDE of `auto` is never read again is the
// silent failure this suite's header warns about — a `static` that stops
// conferring internal linkage, or a `const` that stops binding. So each arm
// below pairs the specifier-LED spelling with its `auto`-LED twin and asserts
// they agree on the FACT the specifier carries, not merely that both compile.
//
// ✔MEASURED 2026-09-08, each reference probed SEPARATELY on its own
// translation unit, `nm` read on the object:
//   static auto g = 42; / auto static g = 42;   gcc rc=0 `d g` for BOTH orders
//       (INTERNAL); clang rc=0 for both (it elides the unused internal object,
//       so its `nm` is empty for both — equally, which is the point).
//   const auto k = 42;  / auto const k = 42;    gcc AND clang rc=0, `nm` R k
//       for BOTH orders on BOTH references.
//   thread_local auto t = 5;                     gcc AND clang rc=0, `nm` D t.
//   constexpr auto k = 42;                       gcc rc=0 `nm` r k; clang
//       ABSTAINS (it implements no C `constexpr`).
//   volatile auto v = 1;                         gcc AND clang rc=0, `nm` D v.
TEST(FileScopeDeclarationDefinedness,
     SpecifierLedFileScopeAutoMeansWhatTheAutoLedOrderMeans) {
    // LINKAGE — the fact `static` carries, on both sides of `auto`.
    auto lead = analyzeShipped("c", {"static auto g = 42;\n"});
    auto trail = analyzeShipped("c", {"auto static g = 42;\n"});
    EXPECT_FALSE(lead.hasErrors());
    EXPECT_FALSE(trail.hasErrors());
    auto const* lg = symbolNamed(lead, "g");
    auto const* tg = symbolNamed(trail, "g");
    ASSERT_NE(lg, nullptr);
    ASSERT_NE(tg, nullptr);
    EXPECT_TRUE(lg->isInternalLinkage)
        << "`static auto` at file scope is INTERNAL — gcc emits `d g` for this "
           "order exactly as it does for `auto static`";
    EXPECT_EQ(lg->isInternalLinkage, tg->isInternalLinkage);
    EXPECT_EQ(lg->isTentativeDefinition, tg->isTentativeDefinition);
    EXPECT_EQ(lg->isExternDeclaration, tg->isExternDeclaration);

    // CONTROL for the linkage arm: the UNQUALIFIED specifier-led inference is
    // EXTERNAL, so the assertion above cannot be passing because every
    // specifier-led inference became internal.
    auto plainModel = analyzeShipped("c", {"const auto e = 42;\n"});
    auto const* pe = symbolNamed(plainModel, "e");
    ASSERT_NE(pe, nullptr);
    EXPECT_FALSE(pe->isInternalLinkage)
        << "`const auto` confers no internal linkage — gcc emits `R k`, an "
           "external read-only symbol";

    // THE INFERRED TYPE — the same initializer must infer the same type from
    // either side of `auto`.
    auto leadD  = analyzeShipped("c", {"const auto d = 2.0;\n"});
    auto trailD = analyzeShipped("c", {"auto const d = 2.0;\n"});
    auto const* ld = symbolNamed(leadD, "d");
    auto const* td = symbolNamed(trailD, "d");
    ASSERT_NE(ld, nullptr);
    ASSERT_NE(td, nullptr);
    ASSERT_TRUE(ld->type.valid());
    ASSERT_TRUE(td->type.valid());
    EXPECT_EQ(leadD.lattice().interner().kind(ld->type), TypeKind::F64);
    EXPECT_EQ(leadD.lattice().interner().kind(ld->type),
              trailD.lattice().interner().kind(td->type));

    // THE QUALIFIER — a LEADING `const` must still REFUSE the assignment. This
    // is the arm that separates "the qualifier parsed" from "the qualifier
    // means something", and the specifier prefix is where a leading qualifier
    // would be silently dropped.
    auto konst = analyzeShipped("c", {
        "const auto k = 42;\n"
        "int main(void) { k = 6; return k; }\n",
    });
    EXPECT_TRUE(hasCode(konst.diagnostics(), DiagnosticCode::S_ConstViolation))
        << "gcc and clang each refuse the assignment after a LEADING `const`, "
           "exactly as they do after a trailing one";

    // CONTROL — an UNQUALIFIED specifier-led inference is NOT const, so the
    // arm above cannot be passing because every specifier-led `auto` became
    // const.
    auto notConst = analyzeShipped("c", {
        "static auto k = 42;\n"
        "int main(void) { k = 6; return k; }\n",
    });
    EXPECT_FALSE(hasCode(notConst.diagnostics(),
                         DiagnosticCode::S_ConstViolation));

    // The remaining specifiers C23 6.7.2p2 admits beside an inferred `auto`,
    // written BEFORE it, each accepted by at least one reference that WORKS.
    for (char const* const src : {
             "constexpr auto k = 42;\n",
             "thread_local auto t = 5;\n",
             "volatile auto v = 1;\n",
             "const static auto k = 42;\n",
             "static const auto k = 42;\n",
             "alignas(16) auto a = 3;\n",
             "__attribute__((weak)) auto w = 1;\n",
             "[[maybe_unused]] auto m = 1;\n",
         }) {
        auto model = analyzeShipped("c", {std::string{src}});
        EXPECT_FALSE(model.hasErrors()) << src;
    }
}

// ★★★ THE PARSER PROPERTY THE ORDER-FREE RUN RIDES ON, MEASURED AT THE C TIER
// AND ON DECLARATIONS THAT HAVE NOTHING TO DO WITH `auto`. Admitting a leading
// specifier gives {static, const, constexpr, thread_local, inline, alignas,
// __attribute__, [[} a SECOND top-level candidate, so `topLevelDecl` — which
// contains file-scope FUNCTION DEFINITIONS, an unbounded token class — becomes
// the LAST candidate at those leads. That used to mean a token BUDGET.
// `finalCandidateDirectDescent_` (src/analysis/syntactic/parser.cpp) descends
// into an outermost alt's final candidate when it is the declared-last
// STRUCTURAL candidate — the exact rule the all-fail path already replayed with
// no budget — so nothing here is probed.
//
// ⚠⚠ AND THE DEFECT WAS ALREADY LIVE WITHOUT ANY OF THIS. A qualifier or
// attribute lead ALREADY had a second candidate before P65 — a typedef may
// carry leading qualifiers and attributes, so `typedefDecl` claimed those leads
// too. ✔MEASURED 2026-09-08 through the shipped CLI on the pre-change tree,
// every one of these was `error[P_SpeculationBudgetExhausted]` rc=1 on ordinary
// C that gcc, clang and MSVC all compile, and every one is rc=0 after:
//     const int big[] = {3001 elements};        volatile int big[] = {…};
//     _Atomic int big[] = {…};                  [[maybe_unused]] int big[] = {…};
//     __attribute__((aligned(16))) int big[] = {…};
//     const int f(void) { 1200 statements }
// ⚠ THESE ARMS ARE NOT A RESTATEMENT OF THE TWO ORDER PINS BELOW, AND THE
// REASON IS NOT THE ONE IT LOOKS LIKE. Those two use a `static` lead, which had
// ONE top-level owner before this cycle and was never probed — but the
// order-free specifier run gave `static` a second owner in the same change, so
// they now ride the descent too (✔MEASURED: the engine mutant reddens all five).
// What separates these arms is the LEAD: `const`, `volatile`, `_Atomic`,
// `__attribute__` and `[[` shared their lead with `typedefDecl` ALREADY, so
// these declarations were refused on the tree as it stood before P65 touched
// the grammar at all.
TEST(FileScopeDeclarationDefinedness,
     LargeQualifierLedFileScopeInitializerIsNotProbed) {
    for (char const* const lead : {"const", "volatile", "_Atomic",
                                   "[[maybe_unused]]",
                                   "__attribute__((aligned(16)))",
                                   "static", "constexpr"}) {
        std::string src = std::string{lead} + " int big[] = {";
        for (int i = 0; i < 3000; ++i) { src += "1,"; }
        src += "0};\n";
        auto cu = buildShippedUnit("c", {src});
        EXPECT_FALSE(hasParseError(*cu))
            << "a file-scope initializer far past `topLevel`'s 1024-token "
               "probe budget must parse, because the declaration is the alt's "
               "fallback reading and is DESCENDED into, never probed: "
            << lead;
    }
}

// The same property for a file-scope FUNCTION DEFINITION behind a QUALIFIER
// lead, which is the shape no probe budget can EVER cover: a function body is
// unbounded, so "raise the lookahead" is not a fix for it, only a longer fuse.
TEST(FileScopeDeclarationDefinedness,
     LargeQualifierLedFunctionBodyIsNotProbed) {
    for (char const* const lead : {"const", "__attribute__((noinline))",
                                   "inline", "static"}) {
        std::string src = std::string{lead} + " int f(void) {\n    int a = 0;\n";
        for (int i = 0; i < 1200; ++i) { src += "    a = a + 1;\n"; }
        src += "    return a;\n}\n";
        auto cu = buildShippedUnit("c", {src});
        EXPECT_FALSE(hasParseError(*cu))
            << "a function body cannot fit any probe budget, so the "
               "declaration must be reached by descent: " << lead;
    }
}

// The same, with the SPECIFIER-LED inference rule actually in the candidate set
// — the arm that says the descent survives having lost a probe to the new rule
// first. `static` leads both `autoInferredTopLevelDecl` (which fast-fails at
// the `int`) and `topLevelDecl` (which is descended into), so this is the exact
// two-candidate shape [[D-CSUBSET-AUTO-FILE-SCOPE]] measured breaking.
TEST(FileScopeDeclarationDefinedness,
     LargeFileScopeInitializerAfterALeadingSpecifierIsNotProbed) {
    std::string src = "static const int big[] = {";
    for (int i = 0; i < 3000; ++i) { src += "1,"; }
    src += "0};\nstatic auto lead = 7;\n"
           "int main(void) { return big[0] - 1 + lead - 7; }\n";
    auto cu = buildShippedUnit("c", {src});
    EXPECT_FALSE(hasParseError(*cu))
        << "the inference rule's probe must fast-fail and leave the "
           "declaration to the descent";
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_FALSE(model.hasErrors());
    auto const* big = symbolNamed(model, "big");
    auto const* lead = symbolNamed(model, "lead");
    ASSERT_NE(big, nullptr);
    ASSERT_NE(lead, nullptr);
    ASSERT_TRUE(big->type.valid());
    ASSERT_TRUE(lead->type.valid());
    auto const& in = model.lattice().interner();
    ASSERT_EQ(in.kind(big->type), TypeKind::Array);
    EXPECT_EQ(in.scalars(big->type)[0], 3001);
    EXPECT_EQ(in.kind(lead->type), TypeKind::I32);
}

// A `const` file-scope inference must REFUSE the assignment — the arm that
// separates "the qualifier parsed" from "the qualifier means something".
TEST(FileScopeDeclarationDefinedness, AutoFileScopeConstIsConst) {
    auto model = analyzeShipped("c", {
        "auto const k = 42;\n"
        "int main(void) { k = 6; return k; }\n",
    });
    EXPECT_TRUE(hasCode(model.diagnostics(), DiagnosticCode::S_ConstViolation))
        << "gcc and clang each refuse the assignment after `auto const`";

    // CONTROL — an UNQUALIFIED file-scope inference is NOT const, so the arm
    // above cannot be passing because every file-scope `auto` became const.
    auto plain = analyzeShipped("c", {
        "auto k = 42;\n"
        "int main(void) { k = 6; return k; }\n",
    });
    EXPECT_FALSE(hasCode(plain.diagnostics(), DiagnosticCode::S_ConstViolation));
}

// C23's UNDERSPECIFIED-DECLARATION rule, and it is ASYMMETRIC — which is
// exactly why it needs its own arm rather than falling out of the merge table.
// ✔MEASURED, gcc AND clang, all four cases: a prior declaration of the name
// makes the inference declaration an ERROR ("underspecified declaration of 'g',
// which is already declared in this scope" / "redefinition of 'g' with a
// different type: 'auto' vs 'int'"), while the SAME two declarations in the
// opposite order are rc=0 on both.
TEST(FileScopeDeclarationDefinedness, AutoFileScopeRefusesAPriorDeclaration) {
    for (char const* const src : {
             "extern int g;\nauto g = 42;\n",
             "int g;\nauto g = 42;\n",
             "static int g;\nauto g = 42;\n",
             "auto g = 42;\nauto g = 42;\n",
         }) {
        auto model = analyzeShipped("c", {std::string{src}});
        EXPECT_TRUE(hasCode(model.diagnostics(),
                            DiagnosticCode::S_RedeclaredSymbol))
            << "gcc and clang both refuse this order: " << src;
    }

    // CONTROL — the OPPOSITE order is legal on both references and must stay
    // silent, so the arm above cannot be passing because the inference row
    // collides with everything.
    for (char const* const src : {
             "auto g = 42;\nextern int g;\n",
             "auto g = 42;\nint g;\n",
             "auto static g = 42;\nextern int g;\n",
         }) {
        auto model = analyzeShipped("c", {std::string{src}});
        EXPECT_FALSE(model.hasErrors())
            << "gcc and clang are both rc=0 in this order: " << src;
    }
}

// The NEGATIVE boundaries of the new top-level rule — every one of them
// refused by gcc AND clang, and every one of them a shape the new headless
// production STRUCTURALLY matches, so without these gates the rule would trade
// a loud parse error for a silent C89 implicit-int.
TEST(FileScopeDeclarationDefinedness, AutoFileScopeNegativesStayLoud) {
    for (char const* const src : {
             "auto g;\n",                    // no initializer to infer from
             "extern auto g;\n",             // ditto, and `extern` cannot lead it
             "auto g = 1, h = 2;\n",         // C23 6.7.9p2: a single declarator
             "auto f(void) { return 1; }\n", // not a plain identifier
             "auto *p = 0;\n",               // derived declarator
             "static x = 5;\n",              // C89 implicit int is NOT C23 auto
             "constexpr y = 5;\n",           // the same hole, other specifier
             "auto register g = 1;\n",       // C 6.9p2 bars file-scope register
         }) {
        auto cu = buildShippedUnit("c", {std::string{src}});
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
        EXPECT_TRUE(hasParseError(*cu) || model.hasErrors())
            << "must stay LOUD (gcc and clang each refuse it): " << src;
    }
}

// ★★★ THE COST PIN (the ★C2-style audit the registry row demanded, at FILE
// scope) — and it pins an ABSENCE, which is why it needs this much prose.
// `autoInferredTopLevelDecl` is `auto`-LED, so AutoKeyword is in NO other
// top-level alternative's FIRST and the predictive prune leaves EXACTLY ONE
// candidate for every lead token the language has. Both declarations below
// therefore keep the unique-production DIRECT DESCENT they had before this
// cycle: no probe, no checkpoint, no token budget.
// ⚠ THE ALTERNATIVE SHAPE WAS BUILT AND MEASURED BREAKING BOTH OF THEM. A
// specifier-run-led rule (the straight mirror of block scope's
// `autoInferredVarDecl`) puts {static, const, constexpr, thread_local, …} into
// this rule's FIRST, giving those leads a SECOND candidate — and `topLevelDecl`
// is then PROBED under `topLevel`'s budget of 1024 (its default lookahead 8 ×
// `parser.speculationBudgetFactor` 128). Both declarations below exceeded it and
// came back `error[P_SpeculationBudgetExhausted]` on ordinary C the same tree
// had compiled a moment earlier.
// ⚠ AND THE ALL-FAIL REPLAY DOES NOT RESCUE IT, although the block-scope
// sibling's ★C2 note reads as though it would: `finishFailedSpeculation_`
// REPORTS the latched ceiling BEFORE it replays, so the construct parses on the
// replay and the compile still exits 1. Block scope survives the same order
// because `declOrAttrStmt` declares `lookahead: 256` and a block-scope
// declaration is BOUNDED.
TEST(FileScopeDeclarationDefinedness, LargeFileScopeInitializerSurvivesTheOrder) {
    std::string src = "static const int big[] = {";
    for (int i = 0; i < 3000; ++i) { src += "1,"; }
    src += "0};\nint main(void) { return big[0] - 1; }\n";
    auto cu = buildShippedUnit("c", {src});
    EXPECT_FALSE(hasParseError(*cu))
        << "a file-scope initializer far past `topLevel`'s 1024-token probe "
           "budget must still parse, because nothing probes it";
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    auto const* big = symbolNamed(model, "big");
    ASSERT_NE(big, nullptr);
    ASSERT_TRUE(big->type.valid());
    auto const& in = model.lattice().interner();
    ASSERT_EQ(in.kind(big->type), TypeKind::Array);
    EXPECT_EQ(in.scalars(big->type)[0], 3001);
}

// The same property for a file-scope FUNCTION DEFINITION, which is the shape no
// probe budget can EVER cover: a function body is unbounded, so "raise the
// lookahead" is not a fix for it, only a longer fuse. This is the pin that makes
// the `auto`-led restriction a measured decision rather than a preference.
TEST(FileScopeDeclarationDefinedness, LargeStaticFunctionBodySurvivesTheOrder) {
    std::string src = "static int f(void) {\n    int a = 0;\n";
    for (int i = 0; i < 1200; ++i) { src += "    a = a + 1;\n"; }
    src += "    return a;\n}\nint main(void) { return f() - 1200; }\n";
    auto cu = buildShippedUnit("c", {src});
    EXPECT_FALSE(hasParseError(*cu))
        << "a `static`-led function definition must stay a ONE-candidate lead: "
           "its body cannot fit any probe budget";
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_FALSE(model.hasErrors());
    EXPECT_NE(symbolNamed(model, "f"), nullptr);
}

// ── (3) THE DECIDED DIVERGENCE: file-scope `register` stays refused ─────────
//
// ⚠ THIS ARM IS A DECISION WITH A MEASUREMENT BEHIND IT, NOT AN UNCLOSED HOLE,
// and it CONTRADICTS the premise it was raised under. The premise relayed to
// this lane was "gcc accepts file-scope `constexpr register int x = 0;`, so
// under the union DSS must too". ✔MEASURED 2026-09-08, and the premise is only
// half true:
//   * gcc 13.3.0 `-std=c2x -c`  rc=0 — CONFIRMED;
//   * gcc 13.3.0 `-std=c2x -pedantic-errors -c` rc=1:
//         error: file-scope declaration of 'x' specifies 'register' [-Wpedantic]
//     so gcc's own CONFORMANCE mode refuses it. Its rc=0 is a diagnosed
//     extension, not a conformance claim;
//   * clang 18.1.3 and MSVC 19.51.36231 ABSTAIN — neither implements C
//     `constexpr` at all ("unknown type name 'constexpr'" / C2054), so neither
//     votes for or against the pairing;
//   * ISO C23 6.9p2 is explicit: "The storage-class specifiers auto and
//     register shall not appear in the declaration specifiers in an external
//     declaration." The `u ISO C` half of the union is on the REFUSING side.
// And the BARE spelling settles the rest: `register int x = 0;` at file scope
// is REFUSED by gcc ("register name not specified for 'x'" — gcc reads
// file-scope `register` as its GLOBAL REGISTER VARIABLE syntax, a feature with
// real meaning that DSS does not implement) and by clang ("illegal storage
// class on file-scoped variable"). Only MSVC accepts it, and its own diagnostic
// says what it did with it: warning C4042, "'x': has bad storage class" — an
// accept-by-DROPPING-the-specifier, which under this project's rule ("the union
// is over what WORKS, not what is ACCEPTED — a reference that accepts then
// silently drops meaning casts no vote") is not a vote either.
// ⇒ DSS keeps the refusal, and this test is what makes that a PINNED decision
// instead of an accident. The BLOCK-scope pairing, where C23 6.7.2p2 does admit
// `constexpr register` and gcc accepts it, is a different question and is
// already open for business — its live twin is the last arm here.
TEST(FileScopeDeclarationDefinedness, FileScopeRegisterStaysRefusedInEverySpelling) {
    for (char const* const src : {
             "register int x = 0;\n",
             "constexpr register int x = 0;\n",
             "register constexpr int x = 0;\n",
             "register auto g = 1;\n",
             "auto register g = 1;\n",
         }) {
        auto cu = buildShippedUnit("c", {std::string{src}});
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
        EXPECT_TRUE(hasParseError(*cu) || model.hasErrors())
            << "C 6.9p2 forbids `register` in an external declaration, gcc's "
               "-pedantic-errors agrees, and gcc + clang both refuse the bare "
               "spelling outright: " << src;
    }
}

// THE LIVE TWIN that keeps the refusal above from reading as "DSS refuses
// `register` everywhere". At BLOCK scope C23 6.7.2p2's third bullet DOES admit
// `constexpr` beside `register`, gcc 13.3.0 accepts it (clang and MSVC abstain
// on C `constexpr`), and DSS accepts it through `varDecl`'s own
// `compatibleWith` table. If this arm ever goes red the refusal above has
// spread past the scope that justifies it.
TEST(FileScopeDeclarationDefinedness, BlockScopeConstexprRegisterStillCompiles) {
    for (char const* const src : {
             "int main(void) { constexpr register int x = 0; return x; }\n",
             "int main(void) { register constexpr int x = 0; return x; }\n",
             "int main(void) { register int r = 3; return r - 3; }\n",
         }) {
        auto model = analyzeShipped("c", {std::string{src}});
        EXPECT_FALSE(model.hasErrors())
            << "gcc accepts this at block scope and C23 6.7.2p2 admits the "
               "pairing: " << src;
    }
}
