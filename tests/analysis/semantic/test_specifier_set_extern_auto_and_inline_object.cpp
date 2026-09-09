// ===========================================================================
// P65 lane `sp` — TWO SPECIFIER-**SET** QUESTIONS ON A FILE-SCOPE DECLARATION
//
// THE PROPERTY THIS FILE OWNS: which specifiers a declaration's specifier SET
// may CONTAIN — as distinct from the ORDER they are written in, which is the
// sibling question [[D-C-FILE-SCOPE-INFERRED-AUTO-MUST-LEAD-THE-DECLARATION-SPECIFIERS]]
// closed earlier this cycle and which this file deliberately does not re-pin.
// The distinction is the whole reason these two items needed a lane of their
// own: ✔MEASURED before any change, `extern auto g = 1;` AND `auto extern
// g = 1;` were refused IDENTICALLY (`error[P_NoAlternativeMatched]`), so the
// order-free run could not help — `extern` was simply not in the admitted set.
//
//   (1) `extern auto g = 1;` — DSS REFUSED, and BOTH working references accept.
//       ⇒ FIXED, in CONFIG ALONE: two localized edits to `c.lang.json` and not
//       one engine line, because the tiers it needed were already built.
//   (2) `inline int x = 1;` / `inline auto x = 1;` / `auto inline x = 1;` —
//       DSS refuses `S_InlineNonFunction`, gcc accepts, clang and MSVC refuse.
//       ⇒ DECIDED: the refusal STAYS, on the experiment in the block above
//       those arms. Not an unclosed hole — a pinned decision.
//   (2b) ⚠ AND THE MEASUREMENT FOR (2) EXPOSED A HOLE IN THAT VERY REFUSAL,
//       which no part of this lane's brief predicted: `extern inline int
//       x = 1;` was rc=0 with the specifier SILENTLY DROPPED, because the
//       6.7.5p2 presence CONSTRAINT and the 6.7.5p7 inline-definition rule were
//       being decided by ONE boolean. ⇒ FIXED in the ENGINE
//       (`specifierPrefixHasInline` now returns `InlineSpelling{present,
//       withoutExtern}`). The hole PRE-DATES this lane — it needs no `auto` at
//       all — but (1) made `extern inline auto x = 1;` reachable, so shipping
//       it unfixed would have ADDED a silent-accept spelling.
//
// ✔REFERENCE VOTES — each reference probed SEPARATELY on its own translation
// unit, 2026-09-08:
//   gcc 13.3.0        `-std=c2x -c`   (and `-pedantic-errors`, which is what
//                                      separates a CONFORMANCE claim from a
//                                      diagnosed extension)
//   clang 18.1.3      `-std=c23 -c`
//   MSVC 19.51.36252  `cl /nologo /c /std:c17`  AND  `/std:clatest`
//
// ── ITEM (1) `extern auto g = 1;` ──────────────────────────────────────────
//   gcc    rc=0, warns "'g' initialized and declared 'extern'", `nm` = `D g`;
//          `-pedantic-errors` rc=0 — a CONFORMING program, not an extension.
//   clang  rc=0, warns -Wextern-initializer, `nm` = `D g`;
//          `-pedantic-errors` rc=0.
//   MSVC   ABSTAINS. It implements no C23 inference at all: `auto g = 1;` is
//          rc=0 with `warning C4042: 'g': has bad storage class` — the C89
//          STORAGE CLASS over an implicit int — and it refuses `static auto
//          g = 1;` with C2159 too. Its C2159 on `extern auto` is therefore a
//          collision between two storage classes, not a verdict on this
//          feature.
//   ISO    ACCEPTS, and both halves were read from N3220 rather than relayed:
//          6.7.2p2 constraint list — "auto may appear with all the others
//          except typedef"; 6.7.2p4 — "auto shall only appear ... along with
//          other storage-class specifiers IF THE TYPE IS TO BE INFERRED FROM AN
//          INITIALIZER", which is exactly this row.
//   ⇒ two working references and ISO C on the accepting side. DSS was BELOW the
//     union and now is not.
//
// ★★★ WHAT IT MEANS IS 6.7.2p15, AND THAT PARAGRAPH IS WHY NO ENGINE LINE
// MOVED. "If auto appears with another storage-class specifier, or if it
// appears in a declaration at file scope, it is IGNORED for the purposes of
// determining a storage duration or linkage. In this case, it indicates only
// that the declared type may be inferred." So `auto` contributes NOTHING to the
// linkage of `extern auto g = 1;` — `extern` decides it alone — and C 6.9.3p1's
// file-scope-initializer rule then makes the declaration a DEFINITION, the
// `extern` redundant rather than contradictory. That override already existed
// (lane `fs`, [[D-C-FILE-SCOPE-EXTERN-WITH-INITIALIZER-IS-A-DEFINITION]]), is
// per-DECLARATOR and scope-gated, and an inference declaration ALWAYS carries an
// initializer — so declaring `extern` in this row's linkage table with the SAME
// `{nonDefining, exclusiveGroup}` cell `topLevelDecl` carries for the same
// keyword was the entire semantic change.
//
// ⚠⚠ THE ARM THAT WOULD HAVE CAUGHT A PARSE-ONLY FIX. Adding `ExternKeyword` to
// the grammar alone would have made every one of these declarations rc 0 while
// `linkageFrom` answered `warning[H_UnknownLinkageSpecifier]` and DROPPED the
// keyword — an ACCEPT with the specifier silently discarded, which is the exact
// shape this row's `register` history records being refuted by measurement. The
// definedness arms below are what separate "it parses" from "it means".
//
// ── ITEM (2) `inline` ON AN OBJECT — A DECIDED DIVERGENCE ──────────────────
// See the block above `InlineOnAFileScopeObjectStaysRefusedInEverySpelling`.
//
// ── RED-ON-DISABLE, REMOVE DIRECTION ───────────────────────────────────────
// THREE mutants with DISJOINT red sets — which is what says each hit its own
// part of a change that has three parts. Two are the CONFIG DOCUMENT and one is
// ENGINE SOURCE, and the difference in what moves is stated rather than left to
// be assumed.
// ⚠ FOR (A) AND (B) **NO OBJECT md5 IS INVOLVED**: `c.lang.json` is copied into
// the build tree's `dss-config-snapshot` at ctest RUN time and never at build
// time (cmake/DssConfigSnapshot.cmake), so no translation unit recompiles and no
// binary moves. What moves is the CONFIG FILE's own md5, and it must move and
// RETURN.
//   (A) GRAMMAR (config): delete `"ExternKeyword"` from
//       `topLevelAutoSpecifier`'s `alt` in src/dss-config/sources/c.lang.json.
//       Every `extern auto` / `auto extern` spelling goes back to
//       `P_NoAlternativeMatched`.
//   (B) SEMANTICS (config): delete the `"extern"` ENTRY from
//       `autoInferredTopLevelDecl`'s `linkageSpecifiers`. The declarations
//       still PARSE — and that is the point: the keyword becomes an UNKNOWN
//       linkage specifier and is dropped, so the definedness and exclusivity
//       arms go red while the parse-shaped ones stay green.
//   (C) ENGINE (source AND object md5 both move and return): in
//       src/analysis/semantic/semantic_analyzer.cpp, put the `inline`
//       constraint arm back on the p7 field — `declInline.withoutExtern` in
//       place of `declInline.present` — which is the pre-P65 behaviour.
//       `InlineBesideExternOnAnObjectIsStillRefused` goes red and nothing else
//       does.
// The per-mutant red sets and the GREEN CONTROLS printed by name are in the
// lane report.
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

// ── (1) `extern auto` at FILE scope ────────────────────────────────────────

// THE CORE FACT, ASSERTED ON THE SYMBOL RATHER THAN ON AN EXIT CODE. The
// declaration must be a real DEFINITION with EXTERNAL linkage and an INFERRED
// type — three separate claims, because a change that satisfied only the first
// would still emit an import row for a symbol this TU defines, which is a
// link-time duplicate rather than a diagnostic.
TEST(SpecifierSetExternAutoAndInlineObject, ExternAutoAtFileScopeIsADefinition) {
    auto model = analyzeShipped("c", {"extern auto g = 1;\n"});
    EXPECT_FALSE(model.hasErrors())
        << "C23 6.7.2p2 admits `auto` beside every storage-class specifier but "
           "`typedef`; gcc and clang both rc=0, both -pedantic-errors clean";
    auto const* g = symbolNamed(model, "g");
    ASSERT_NE(g, nullptr);
    ASSERT_TRUE(g->type.valid());
    EXPECT_EQ(model.lattice().interner().kind(g->type), TypeKind::I32)
        << "the type is INFERRED from the initializer (C23 6.7.10p2), not "
           "defaulted — an implicit-int fallback would also be I32 here, which "
           "is why the double/string arm below exists";
    EXPECT_FALSE(g->isExternDeclaration)
        << "C 6.9.3p1: a file-scope declarator WITH an initializer DEFINES the "
           "object, so `extern` is redundant here rather than an announcement "
           "that the storage lives elsewhere. gcc and clang both emit `D g`";
    EXPECT_FALSE(g->isTentativeDefinition)
        << "an inference declaration always carries an initializer, so it is a "
           "real definition and never tentative (C 6.9.3p2)";
    EXPECT_FALSE(g->isInternalLinkage)
        << "6.7.2p15 — `auto` is IGNORED for linkage here, so `extern` decides "
           "it alone: EXTERNAL, which is the `D` (upper case) gcc and clang "
           "both emit";
}

// THE SET IS A SET, SO BOTH WRITTEN ORDERS MUST REACH THE SAME MEANING — and
// this arm is what proves the fix was to the admitted SET and not to an order.
// ✔MEASURED: both spellings are rc=0 with `nm` = `D g` on gcc AND clang, and
// both were `P_NoAlternativeMatched` on DSS before this change.
TEST(SpecifierSetExternAutoAndInlineObject, ExternAutoIsOrderFreeAndBothOrdersAgree) {
    for (char const* const src : {"extern auto g = 1;\n", "auto extern g = 1;\n"}) {
        auto model = analyzeShipped("c", {std::string{src}});
        EXPECT_FALSE(model.hasErrors()) << src;
        auto const* g = symbolNamed(model, "g");
        ASSERT_NE(g, nullptr) << src;
        EXPECT_FALSE(g->isExternDeclaration) << src;
        EXPECT_FALSE(g->isTentativeDefinition) << src;
        EXPECT_FALSE(g->isInternalLinkage) << src;
        ASSERT_TRUE(g->type.valid()) << src;
        EXPECT_EQ(model.lattice().interner().kind(g->type), TypeKind::I32) << src;
    }
}

// The inference is a real type computation under `extern`, not an int default —
// three initializers, three declared types, including the C23 6.7.10p2
// array-to-pointer DECAY. This is the arm an implicit-int fallback fails.
TEST(SpecifierSetExternAutoAndInlineObject, ExternAutoInfersTheInitializersType) {
    auto model = analyzeShipped("c", {
        "extern auto d = 1.5;\n"
        "extern auto s = \"hi\";\n"
        "auto extern c = (char)3;\n"});
    EXPECT_FALSE(model.hasErrors());
    auto const* d = symbolNamed(model, "d");
    auto const* s = symbolNamed(model, "s");
    auto const* c = symbolNamed(model, "c");
    ASSERT_NE(d, nullptr);
    ASSERT_NE(s, nullptr);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(d->type.valid());
    ASSERT_TRUE(s->type.valid());
    ASSERT_TRUE(c->type.valid());
    EXPECT_EQ(model.lattice().interner().kind(d->type), TypeKind::F64);
    EXPECT_EQ(model.lattice().interner().kind(s->type), TypeKind::Ptr)
        << "C23 6.7.10p2 infers the type AFTER array-to-pointer conversion";
    EXPECT_NE(model.lattice().interner().kind(c->type), TypeKind::I32)
        << "`(char)3` under `auto extern` infers CHAR, not int — an "
           "implementation that defaulted to int would pass every other arm";
}

// THE REFUSAL THAT MUST STILL FIRE. Two real definitions of one file-scope
// object collide — the arm that would go silent if the fix had made the
// construct rc 0 by classifying it as a non-defining declaration instead of as
// a definition. ✔MEASURED: `extern auto g = 1; int g = 2;` is rc=1 on gcc AND
// clang, both "redefinition of 'g'".
TEST(SpecifierSetExternAutoAndInlineObject, ExternAutoStillCollidesWithASecondDefinition) {
    auto model = analyzeShipped("c", {"extern auto g = 1;\nint g = 2;\n"});
    EXPECT_TRUE(hasCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol))
        << "gcc and clang both report `redefinition of 'g'`";
}

// CONTROL for the arm above, so the collision cannot be passing because every
// redeclaration now collides. ✔MEASURED: `extern auto g = 1; extern int g;` and
// `extern auto g = 1; int g;` are BOTH rc=0 on gcc and clang, `nm` = `D g` —
// the later declaration merges into the definition this one made.
TEST(SpecifierSetExternAutoAndInlineObject, ExternAutoMergesWithALaterDeclaration) {
    for (char const* const src : {
             "extern auto g = 1;\nextern int g;\n",
             "extern auto g = 1;\nint g;\n",
         }) {
        auto model = analyzeShipped("c", {std::string{src}});
        EXPECT_FALSE(model.hasErrors()) << src;
        auto const* g = symbolNamed(model, "g");
        ASSERT_NE(g, nullptr) << src;
        EXPECT_FALSE(g->isExternDeclaration)
            << "the inference declaration DEFINED it; a later plain or extern "
               "declaration of the same object must not undo that: " << src;
    }
}

// ⚠ THE ORDER-SENSITIVE REFUSAL C23'S UNDERSPECIFIED-DECLARATION RULE OWNS, and
// it must survive the new specifier. ✔MEASURED, gcc and clang both rc=1 on
// `static int g; extern auto g = 1;` and on `int g; extern auto g = 41;` (gcc
// "underspecified declaration of 'g', which is already declared in this scope",
// clang "redeclaration of 'g' with a different type: 'auto' vs 'int'") while
// the OPPOSITE order is rc=0 on both — pinned by the merge arm above. An
// inference declaration derives its type from its own initializer, so a prior
// declaration in scope has nothing to agree with.
TEST(SpecifierSetExternAutoAndInlineObject, ExternAutoAfterAPriorDeclarationIsRefused) {
    for (char const* const src : {
             "static int g;\nextern auto g = 1;\n",
             "int g;\nextern auto g = 41;\n",
             "extern int g;\nextern auto g = 41;\n",
         }) {
        auto model = analyzeShipped("c", {std::string{src}});
        EXPECT_TRUE(model.hasErrors())
            << "C23's underspecified-declaration rule; gcc and clang both "
               "rc=1: " << src;
    }
}

// C 6.7.1p2 / C23 6.7.2p2 EXCLUSIVITY under the new entry, and the pairs that
// must stay REFUSED are as load-bearing as the pair now admitted. `extern`
// names `auto` in `compatibleWith` and nothing else, so:
//   `extern static auto`    ✔gcc "multiple storage classes in declaration
//                             specifiers", clang "cannot combine with previous
//                             'extern' declaration specifier";
//   `extern constexpr auto` ✔gcc — the only reference implementing C constexpr
//                             — "error: 'constexpr' used with 'extern'";
//                             6.7.2p2's third bullet lists constexpr as
//                             compatible with auto, register or static, NEVER
//                             with extern. clang and MSVC ABSTAIN (no C
//                             constexpr at all).
// ⚠ ASSERTED BY DIAGNOSTIC CODE, NOT BY "did it error". The grammar now admits
// both tokens in this run, so these declarations PARSE and the refusal has to
// come from the SEMANTIC exclusivity check — an arm that accepted any error
// would also pass if the fix had accidentally reintroduced a parse failure,
// which is the opposite of what this pins.
TEST(SpecifierSetExternAutoAndInlineObject, ExternIsExclusiveWithTheOtherStorageClasses) {
    for (char const* const src : {
             "extern static auto g = 1;\n",
             "static extern auto g = 1;\n",
             "extern constexpr auto g = 1;\n",
             "constexpr extern auto g = 1;\n",
         }) {
        auto cu = buildShippedUnit("c", {std::string{src}});
        EXPECT_FALSE(hasParseError(*cu))
            << "both specifiers are in this run's admitted set, so the refusal "
               "must be the C 6.7.1p2 / C23 6.7.2p2 SEMANTIC one: " << src;
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
        EXPECT_TRUE(hasCode(model.diagnostics(),
                            DiagnosticCode::S_ConflictingStorageClassSpecifiers))
            << "C23 6.7.2p2 admits neither pairing and both references refuse: "
            << src;
    }
}

// THE LIVE TWIN of the arm above — the pairings that MUST still compile, so the
// exclusivity cannot be passing because `extern` now collides with everything.
// ✔MEASURED: `extern thread_local auto g = 1;` is rc=0 with `nm` = `D g` on gcc
// AND clang (6.7.2p2's FIRST bullet: thread_local may appear with extern), and
// `extern const auto k = 1;` is rc=0 with `nm` = `R k` on both.
TEST(SpecifierSetExternAutoAndInlineObject, ExternPairsThatMustStillCompile) {
    for (char const* const src : {
             "extern thread_local auto g = 1;\n",
             "extern const auto k = 1;\n",
             "extern volatile auto v = 1;\n",
             "static auto s = 1;\n",
             "constexpr auto k2 = 1;\n",
             "auto plain = 1;\n",
         }) {
        auto model = analyzeShipped("c", {std::string{src}});
        EXPECT_FALSE(model.hasErrors())
            << "the new `extern` entry must not narrow what this row already "
               "admitted: " << src;
    }
}

// ⚠ THE SCOPE GATE. C 6.7.11p5 forbids an initializer on a BLOCK-scope
// declaration of an identifier with linkage, an inference declaration ALWAYS
// carries one, and ✔all three references refuse it: gcc "'g' has both 'extern'
// and initializer", clang "declaration of block scope identifier with linkage
// cannot have an initializer", MSVC C2205. `extern` was added to the FILE-scope
// inference row's specifier set ONLY — the block-scope twin
// (`localDeclSpecifier` / `autoInferredVarDecl`) is untouched, so this must stay
// a loud refusal, and if this arm ever goes green the fix leaked one scope down.
TEST(SpecifierSetExternAutoAndInlineObject, ExternAutoAtBlockScopeStaysRefused) {
    for (char const* const src : {
             "int main(void) { extern auto g = 1; return g; }\n",
             "int main(void) { auto extern g = 1; return g; }\n",
         }) {
        auto cu = buildShippedUnit("c", {std::string{src}});
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
        EXPECT_TRUE(hasParseError(*cu) || model.hasErrors())
            << "C 6.7.11p5; all three references refuse: " << src;
    }
}

// The inference row's own gates must keep firing THROUGH the new specifier, by
// NAME rather than as a generic refusal — a specifier-set widening that made
// them fall back to `P_NoAlternativeMatched` would read as "still refused" while
// having lost the diagnostic. ✔MEASURED: gcc refuses `extern auto g;` ("'auto'
// requires an initialized data declaration") and `extern auto *p = 0;` /
// `extern auto f(void);` ("'auto' requires a plain identifier ... as
// declarator"); clang refuses all three too.
TEST(SpecifierSetExternAutoAndInlineObject, ExternAutoKeepsTheInferenceRowsOwnGates) {
    {
        auto model = analyzeShipped("c", {"extern auto g;\n"});
        EXPECT_TRUE(hasCode(model.diagnostics(),
                            DiagnosticCode::S_AutoRequiresInitializer));
    }
    {
        auto model = analyzeShipped("c", {"extern auto *p = 0;\n"});
        EXPECT_TRUE(hasCode(model.diagnostics(),
                            DiagnosticCode::S_AutoRequiresPlainIdentifier));
    }
    {
        auto model = analyzeShipped("c", {"extern auto a = 1, b = 2;\n"});
        EXPECT_TRUE(hasCode(model.diagnostics(),
                            DiagnosticCode::S_AutoRequiresSingleDeclarator));
    }
}

// ⚠⚠ THE REGRESSION THIS CHANGE ACTUALLY RISKS, AND IT IS NOT A SEMANTIC ONE.
// `ExternKeyword` is now in FIRST(`autoInferredTopLevelDecl`), so an `extern`
// lead gains a SECOND top-level candidate and `topLevelDecl` — which contains
// file-scope FUNCTION DEFINITIONS, an UNBOUNDED token class — becomes the last
// candidate at that lead. That is exactly the shape that produced
// `P_SpeculationBudgetExhausted` on ordinary C before P65 lane `pl` made the
// parser DESCEND into an outermost alt's declared-last STRUCTURAL candidate
// rather than probe it (`finalCandidateDirectDescent_`,
// src/analysis/syntactic/parser.cpp). These two arms are what say the descent
// covers the token this lane added, rather than assuming a mechanism measured
// on `static` and `const` generalizes to `extern` — a template generalizes the
// CODE, never the MEASUREMENT.
TEST(SpecifierSetExternAutoAndInlineObject, LargeExternLedFileScopeInitializerIsNotProbed) {
    std::string src = "extern int big[] = {";
    for (int i = 0; i < 3001; ++i) { src += "1,"; }
    src += "0};\nint main(void) { return big[3000]; }\n";
    auto cu = buildShippedUnit("c", {src});
    EXPECT_FALSE(hasParseError(*cu))
        << "an `extern`-led file-scope initializer must not be probed under the "
           "speculation budget now that `extern` leads two top-level rules";
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_FALSE(model.hasErrors());
    EXPECT_NE(symbolNamed(model, "big"), nullptr);
}

TEST(SpecifierSetExternAutoAndInlineObject, LargeExternLedFunctionBodyIsNotProbed) {
    std::string src = "extern int f(void) {\n    int a = 0;\n";
    for (int i = 0; i < 1200; ++i) { src += "    a = a + 1;\n"; }
    src += "    return a;\n}\nint main(void) { return f() - 1200; }\n";
    auto cu = buildShippedUnit("c", {src});
    EXPECT_FALSE(hasParseError(*cu))
        << "C 6.9.1 admits `extern` on a function DEFINITION; its body cannot "
           "fit any probe budget";
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_FALSE(model.hasErrors());
    EXPECT_NE(symbolNamed(model, "f"), nullptr);
}

// ── (2) THE DECIDED DIVERGENCE: `inline` on an OBJECT stays refused ─────────
//
// ⚠ THIS IS A DECISION WITH AN EXPERIMENT BEHIND IT, NOT AN UNCLOSED HOLE. The
// question was NOT "gcc accepts it, so must DSS" — this project's rule is that
// the union is taken over what WORKS, not over what is merely ACCEPTED, so the
// question is whether gcc HONOURS the specifier or drops it.
//
// ✔THE EXPERIMENT, stated as a prediction before it was run:
//   H_honoured predicts an `inline` object behaves like C 6.7.5p7's inline
//     FUNCTION — no external definition emitted, so two translation units each
//     spelling it would NOT collide at link.
//   H_dropped  predicts emission IDENTICAL to the undecorated control, and a
//     COLLISION.
// RESULT, gcc 13.3.0 `-std=c2x -O0`, both sources compiled from the SAME
// filename so the object's `.symtab` FILE entry could not confound the
// comparison:
//   `inline int x = 1;` and `int x = 1;` produce BYTE-IDENTICAL object files
//     (md5 592ab0b1a5c0a7901325e07b063e3a37 for both);
//   METHOD CONTROL — `static int x = 1;` vs `int x = 1;` DIFFER, so the
//     comparison can see a specifier that means something;
//   `nm`/`readelf -sW`/`objdump -h .data` agree: `OBJECT GLOBAL DEFAULT 2 x`
//     either way, at -O0 AND -O2;
//   two TUs each spelling `inline int x = 1;` FAIL to link ("multiple
//     definition of `x'"), while the CONTROL — two TUs each spelling
//     `inline int f(void){ return 1; }`, where 6.7.5p7 genuinely applies —
//     links rc=0 on gcc AND clang.
//   ⇒ H_dropped. gcc parses the specifier and honours it NOWHERE.
//
// AND gcc'S OWN CONFORMANCE MODE AGREES: `-pedantic-errors` turns its warning
// into `error: variable 'x' declared 'inline'`, rc=1 — so gcc's rc=0 is a
// diagnosed extension, not a conformance claim. This is the same discriminator
// that decided file-scope `register` earlier this cycle.
//
// THE REMAINING VOTES:
//   clang 18.1.3      REFUSES, rc=1, "'inline' can only appear on functions";
//   MSVC 19.51.36252  REFUSES, rc=2, C2433 "'inline' not permitted on data
//                     declarations" — on-point in BOTH /std:c17 and
//                     /std:clatest, and NOT an abstention: MSVC implements
//                     `inline` and accepts `inline int f(void){…}` rc=0;
//   ISO C23 6.7.5p2   "Function specifiers shall be used only in the
//                     declaration of an identifier for a function." A
//                     constraint.
// ⇒ two working references refuse on point, ISO refuses, and gcc's acceptance
//   carries no observable meaning. DSS's loud `S_InlineNonFunction` is CORRECT
//   and stays. `inline auto x = 1;` and `auto inline x = 1;` fold into the same
//   answer: the specifier is a FUNCTION specifier and none of these declares a
//   function.
//
// ⚠ MEASURED CONTRADICTION WITH AN ADJACENT SHIPPED ROW, recorded rather than
// smoothed over: `_Noreturn int n = 1;` behaves IDENTICALLY to `inline int
// x = 1;` on every reference this file consulted — gcc rc=0 + warn + drop with
// `-pedantic-errors` rc=1 ("variable 'n' declared '_Noreturn'"), clang refuse,
// MSVC refuse (C3829) — and yet [[D-CSUBSET-NORETURN-NON-FUNCTION-OBJECT]]
// (P50) shipped an ACCEPT-with-warning for it while this arm keeps an ERROR.
// The `-pedantic-errors` promotion does NOT discriminate the two; it fires for
// both. What separates them is the direction of the failure: a dropped
// `_Noreturn` costs only an optimization the caller never sees, whereas
// `S_InlineNonFunction` is what a reader of DSS is told when the specifier
// cannot be honoured, and this project's bar forbids a silently dropped
// specifier. That asymmetry is a judgement, and it is written down here so the
// next cycle can overturn it deliberately rather than discover it.
TEST(SpecifierSetExternAutoAndInlineObject, InlineOnAFileScopeObjectStaysRefusedInEverySpelling) {
    for (char const* const src : {
             "inline int x = 1;\n",
             "inline auto x = 1;\n",
             "auto inline x = 1;\n",
             "static inline int x = 1;\n",
             "inline static auto x = 1;\n",
             "int main(void) { inline int y = 1; return y; }\n",
         }) {
        auto model = analyzeShipped("c", {std::string{src}});
        EXPECT_TRUE(hasCode(model.diagnostics(),
                            DiagnosticCode::S_InlineNonFunction))
            << "C23 6.7.5p2 confines a function specifier to a function "
               "declaration; clang and MSVC refuse on point and gcc's accept "
               "emits a BYTE-IDENTICAL object: " << src;
    }
}

// ⚠⚠ THE HOLE THE MEASUREMENT ABOVE EXPOSED, AND IT WAS NOT PART OF THE BRIEF.
// The p2 CONSTRAINT and the p7 INLINE-DEFINITION rule were being decided by ONE
// boolean — `inline` present WITHOUT `extern` — so an `extern` anywhere in the
// specifier prefix switched the CONSTRAINT off. ✔MEASURED at the PRE-CHANGE
// tree through the shipped CLI: `inline int x = 1;` was
// `error[S_InlineNonFunction]` rc=1 while `extern inline int x = 1;` and
// `inline extern int x = 1;` were **rc=0 with no mention of `inline` at all** —
// an accept with the specifier SILENTLY DROPPED, which is exactly the failure
// this project's bar names, on a program clang 18.1.3 REFUSES ("'inline' can
// only appear on functions") and MSVC 19.51.36252 REFUSES in both `/std:c17`
// and `/std:clatest` (`C2433: 'inline' not permitted on data declarations`).
// gcc 13.3.0 accepts it and emits an object BYTE-IDENTICAL to the undecorated
// control, so it casts no vote; ISO C23 6.7.5p2 is a presence rule with no
// `extern` exemption. ⓘ The hole PRE-DATES this lane — it needs no `auto` at
// all — but this lane's other row made `extern inline auto x = 1;` reachable,
// so shipping it unfixed would have ADDED a silent-accept spelling.
TEST(SpecifierSetExternAutoAndInlineObject, InlineBesideExternOnAnObjectIsStillRefused) {
    for (char const* const src : {
             "extern inline int x = 1;\n",
             "inline extern int x = 1;\n",
             "extern inline auto x = 1;\n",
             "inline extern auto x = 1;\n",
             "auto extern inline x = 1;\n",
         }) {
        auto model = analyzeShipped("c", {std::string{src}});
        EXPECT_TRUE(hasCode(model.diagnostics(),
                            DiagnosticCode::S_InlineNonFunction))
            << "C23 6.7.5p2 is about PRESENCE — no clause exempts a function "
               "specifier written beside `extern`: " << src;
    }
}

// THE LIVE TWIN of the arm above, and it is the one the fix most risked: with
// the two readings split, the constraint arm no longer gets its `!isFnSig` term
// for free from an `else`, so an `extern inline` FUNCTION must be checked
// explicitly or the repair refuses ordinary C. ✔MEASURED: gcc 13.3.0, clang
// 18.1.3 AND MSVC 19.51.36252 all take every line below rc=0.
TEST(SpecifierSetExternAutoAndInlineObject, ExternInlineOnAFunctionStaysAccepted) {
    for (char const* const src : {
             "extern inline int f(void);\nint main(void) { return 0; }\n",
             "inline extern int f(void);\nint main(void) { return 0; }\n",
             "extern inline int f(void) { return 1; }\n"
             "int main(void) { return f() - 1; }\n",
             "static inline int f(void) { return 1; }\n"
             "int main(void) { return f() - 1; }\n",
         }) {
        auto model = analyzeShipped("c", {std::string{src}});
        EXPECT_FALSE(hasCode(model.diagnostics(),
                             DiagnosticCode::S_InlineNonFunction))
            << "C 6.9.1 and C23 6.7.5 both admit `extern inline` on a FUNCTION; "
               "all three references take it rc=0: " << src;
        EXPECT_FALSE(model.hasErrors()) << src;
    }
}

// AND THE p7 READING MUST SURVIVE THE SPLIT UNCHANGED — the half that keeps
// `.withoutExtern` as its own field rather than collapsing both arms onto
// `.present`. C23 6.7.5p7: a definition is an INLINE definition (providing no
// external definition) only if EVERY file-scope declaration spells `inline`
// WITHOUT `extern`; one `extern inline` declaration restores the external
// definition. ✔MEASURED with clang -std=c99 + `nm`: `inline int p(int){…}`
// alone gives `U _p` (not emitted) while `extern inline int p(int); inline int
// p(int){…}` gives `T _p`. If this arm ever goes green-by-accident the split
// has collapsed and 6.7.5p7 is being decided by a presence test.
TEST(SpecifierSetExternAutoAndInlineObject, ExternInlineStillSuppressesTheInlineDefinitionReading) {
    {
        auto model = analyzeShipped(
            "c", {"inline int p(int a) { return a + 1; }\n"});
        auto const* p = symbolNamed(model, "p");
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(p->isInline)
            << "`inline` WITHOUT `extern` is the 6.7.5p7 inline-definition "
               "reading and must still be recorded";
    }
    {
        auto model = analyzeShipped(
            "c", {"extern inline int p(int a) { return a + 1; }\n"});
        auto const* p = symbolNamed(model, "p");
        ASSERT_NE(p, nullptr);
        EXPECT_FALSE(p->isInline)
            << "`extern inline` means the OPPOSITE — it provides an EXTERNAL "
               "definition (6.7.5p7), so the flag must stay FALSE even though "
               "the keyword is PRESENT; that is the whole reason the scan "
               "returns two fields instead of one";
    }
}

// THE LIVE TWIN, so the refusal above cannot be passing because DSS refuses
// `inline` everywhere. `inline` on a real FUNCTION must still be accepted AND
// RECORDED — the flag HIR→MIR reads for C 6.7.5p7's no-external-definition
// rule. If this goes red the refusal has spread past the constraint that
// justifies it.
TEST(SpecifierSetExternAutoAndInlineObject, InlineOnAFunctionStillCompilesAndIsRecorded) {
    auto model = analyzeShipped(
        "c", {"inline int f(int a) { return a + 1; }\n"
              "int g(int a) { return a; }\n"
              "int main(void) { return f(0) + g(0) - 1; }\n"});
    EXPECT_FALSE(model.hasErrors());
    EXPECT_FALSE(hasCode(model.diagnostics(),
                         DiagnosticCode::S_InlineNonFunction));
    auto const* f = symbolNamed(model, "f");
    auto const* g = symbolNamed(model, "g");
    ASSERT_NE(f, nullptr);
    ASSERT_NE(g, nullptr);
    EXPECT_TRUE(f->isInline)
        << "C23 6.7.5p7's inline-without-extern reading must still be stored";
    EXPECT_FALSE(g->isInline)
        << "and the undecorated sibling must NOT be — an unconditional store "
           "would make the arm above vacuous";
}
