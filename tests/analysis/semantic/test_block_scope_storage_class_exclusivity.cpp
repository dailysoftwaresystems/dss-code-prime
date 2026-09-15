// ===========================================================================
// P65 lane `sr` — [[D-CSUBSET-BLOCK-SCOPE-STORAGE-CLASS-EXCLUSIVITY-UNSTATED]]
//
// THE PROPERTY THIS FILE OWNS: C23 6.7.2p2's "at most, one storage-class
// specifier" constraint at BLOCK scope and in a FOR-INIT — the two declaration
// forms the file-scope twin (P53, `topLevelDecl`) never covered. Before this
// cycle DSS compiled, at rc 0 with no diagnostic at all, eight block-scope
// spellings that gcc, clang and MSVC each REFUSE:
//
//     static register int x = 0;      auto static int x = 0;
//     register static int x = 0;      static auto int x = 0;
//     auto register int x = 0;        auto constexpr int x = 0;
//     register auto int x = 0;        constexpr auto int x = 0;
//
// plus four of the same shapes in a for-init, every one of them also in the
// `const`-led order, and eighteen three-specifier permutations. That is DSS
// sitting ABOVE the reference union, which under `DSS = (gcc u clang u MSVC) u
// ISO C` is a defect in the bidirectional direction.
//
// ★★★ WHY THE `auto` HALF IS NOT A FLAT EXCLUSIVITY RULE, AND WHY GETTING THAT
// WRONG WOULD HAVE BEEN WORSE THAN THE HOLE. C23 6.7.2p2's second bullet says
// "auto may appear with all the others except typedef", so a flat group on
// `auto` would REFUSE `static auto x = 0;` — which gcc and clang ACCEPT and
// which the shipped `examples/c/auto_type_inference` actually RUNS. What makes
// the eight spellings above illegal is 6.7.2p4, a DIFFERENT sentence: "auto
// shall only appear in the declaration specifiers of an identifier with file
// scope or along with other storage-class specifiers IF THE TYPE IS TO BE
// INFERRED FROM AN INITIALIZER." An explicit `int` means nothing is inferred,
// so the p2 exception's precondition fails. The two sentences are expressed by
// WHICH ROW MATCHED — `varDecl`/`forDecl` carry an explicit type head and give
// `auto` no `compatibleWith`; `autoInferredVarDecl` is the inference row and
// names every sibling. The engine holds no pair list and no language name.
//
// ★★ SO EVERY REFUSAL ARM BELOW HAS A LIVE ACCEPTANCE TWIN, and the twins are
// the half that makes the arms non-vacuous: refusing more is not the goal,
// matching the union is. `StorageClassExceptionsStillCompileAtBlockScope`
// would go red on the tempting wrong fix (a flat group on every specifier),
// and it is printed by name in the red-on-disable transcript for that reason.
//
// ✔REFERENCE VOTES — 376 fixtures x 5 configurations, each reference probed
// SEPARATELY on its own translation unit, 2026-09-08:
//   gcc 13.3.0   `-std=c2x -c`   and  `-std=c17 -c`
//   clang 18.1.3 `-std=c23 -c`
//   MSVC 19.51.36252  `cl /nologo /c /std:c17`  and  `/std:clatest`
// Every spelling asserted REFUSED below is refused by all five. Every spelling
// asserted ACCEPTED below is accepted by gcc `-std=c2x`; where clang and MSVC
// refuse a `constexpr` pairing they do so because neither implements C23
// `constexpr` in C at all ("unknown type name 'constexpr'" / C2054), which is
// an ABSTENTION on that pair and not a vote against it.
//
// ── RED-ON-DISABLE, REMOVE DIRECTION ────────────────────────────────────────
// ⚠ THE MUTANT IS THE DOCUMENT, SO NO OBJECT md5 IS INVOLVED — stating that is
// part of the transcript, not an omission. `c.lang.json` is read at RUN time
// through `$DSS_CONFIG_ROOT`, so no translation unit recompiles and no binary
// moves; what moves is the CONFIG FILE's md5, recorded moved-and-returned in
// the lane report.
//
// The REMOVE-direction mutant deletes the `register` / `auto` / `constexpr`
// ENTRIES from `varDecl`'s, `forDecl`'s and `autoInferredVarDecl`'s
// `linkageSpecifiers` (taking the vocabulary AWAY — an ADD-direction mutant
// stays green when the real config loses the feature).
//
// ✔TRANSCRIPT, RUN 2026-09-08 through `ctest`, never a bare `.exe` for the
// verdict. The config md5 MOVED 2f01b7e8… -> 30857626… and RETURNED to
// 2f01b7e8…, byte-identical to the shipped file. 5 of the 10 tests went red,
// named by the runner:
//   BlockScopeStorageClassConflictRefusedInEveryOrder
//   ForInitStorageClassConflictRefusedInEveryOrder
//   AutoIsExemptOnlyWhenTheTypeIsInferred                (its REFUSAL half)
//   InferredTypeRowStillRefusesAConflictBetweenTwoNonAutoSpecifiers
//   StorageClassConflictNamesBothSpecifiersAndTheGroupAtBlockScope
// and 5 stayed GREEN — the non-vacuous controls, listed because a mutant that
// reds everything proves only that the document was damaged:
//   StorageClassExceptionsStillCompileAtBlockScope
//   ThreadLocalKeepsItsOwnExemptionAtBlockScope
//   RepeatedStorageClassSpecifierStillCompilesAtBlockScope
//   ForInitGatedSpecifiersStillReportTheirOwnCode
//   CoPresentFacetsSurviveBesideTheNewGroupOnlySpecifiers
// The sibling `analysis/semantic/test_decl_specifier_order` and the runnable
// `examples/c/decl_specifier_order` also stayed green in the same mutant run,
// which is what says the mutant took away THIS rule and not the neighbourhood.
// ===========================================================================

#include "core/types/parse_diagnostic.hpp"

#include "semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

using namespace dss;
using namespace dss::sem_test;

namespace {

// One conflict report per DECLARATION -- the storage scan is called from six
// sites and emits nothing itself, so a count of 1 is also a pin on the report
// living at the single per-declaration visit.
void expectOneConflict(std::string_view src, std::string_view why) {
    auto cu = buildShippedUnit("c", {std::string{src}});
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ConflictingStorageClassSpecifiers),
              1u)
        << src << " -- " << why;
}

void expectNoConflict(std::string_view src, std::string_view why) {
    auto cu = buildShippedUnit("c", {std::string{src}});
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_ConflictingStorageClassSpecifiers),
              0u)
        << src << " -- " << why;
}

std::string inMain(std::string_view decl) {
    return "int main(void) {\n  " + std::string{decl} + "\n  return 0;\n}\n";
}

std::string inForInit(std::string_view decl) {
    return "int main(void) {\n  for (" + std::string{decl}
           + "; 0; ) { }\n  return 0;\n}\n";
}

} // namespace

// ---------------------------------------------------------------------------
// THE DEFECT, BOTH ORDERS, WITH AND WITHOUT A LEADING QUALIFIER.
// The qualifier variant is not padding: since P65 a leading `const` lands in
// the SPECIFIER PREFIX beside the storage classes, so it changes the tree the
// conflict scan walks. A guard that only worked on the bare spelling would be
// half a fix.
// ---------------------------------------------------------------------------
TEST(BlockScopeStorageClassExclusivity,
     BlockScopeStorageClassConflictRefusedInEveryOrder) {
    for (std::string_view const decl : {"static register int x = 0;",
                                        "register static int x = 0;",
                                        "static auto int x = 0;",
                                        "auto static int x = 0;",
                                        "register auto int x = 0;",
                                        "auto register int x = 0;",
                                        "constexpr auto int x = 0;",
                                        "auto constexpr int x = 0;"}) {
        expectOneConflict(inMain(decl),
                          "C23 6.7.2p2 admits at most one storage-class "
                          "specifier; gcc, clang and MSVC all refuse this "
                          "spelling, so DSS must too");
        expectOneConflict(inMain("const " + std::string{decl}),
                          "the leading qualifier lands in the specifier prefix "
                          "beside the storage classes and must not hide the "
                          "conflict from the scan");
    }
}

// ---------------------------------------------------------------------------
// THE FOR-INIT TWIN. A separate declaration ROW (`forDecl`), so a separate
// vocabulary and a separate arm -- the block-scope table does not reach it.
// `static` and the thread-storage spellings are absent from this list on
// purpose: `forDecl`'s own `gatedMarkers` refuse them outright (C 6.8.5p3), so
// they never reach an exclusivity verdict and are asserted below instead.
// ---------------------------------------------------------------------------
TEST(BlockScopeStorageClassExclusivity,
     ForInitStorageClassConflictRefusedInEveryOrder) {
    for (std::string_view const decl : {"register auto int x = 0",
                                        "auto register int x = 0",
                                        "constexpr auto int x = 0",
                                        "auto constexpr int x = 0"}) {
        expectOneConflict(inForInit(decl),
                          "a for-init declaration is its own row and needs its "
                          "own storage-class vocabulary");
        expectOneConflict(inForInit("const " + std::string{decl}),
                          "the qualifier-led for-init spelling must refuse too");
    }
}

// ---------------------------------------------------------------------------
// ★★★ THE CONTROL THAT WOULD CATCH THE TEMPTING WRONG FIX.
// A flat `exclusiveGroup` on every storage-class specifier refuses ALL of
// these, and every one is accepted (and, for `static auto`, RUN) by a
// reference. This arm is what separates "matches the union" from "refuses
// more", and it stays GREEN under the REMOVE-direction mutant, which is what
// makes it a live control rather than a second copy of the arms above.
// ---------------------------------------------------------------------------
TEST(BlockScopeStorageClassExclusivity,
     StorageClassExceptionsStillCompileAtBlockScope) {
    // C23 6.7.2p2 bullet 3 -- constexpr with static or register. gcc -std=c2x
    // accepts each; clang and MSVC abstain (no C constexpr at all).
    for (std::string_view const decl : {"static constexpr int x = 0;",
                                        "constexpr static int x = 0;",
                                        "register constexpr int x = 0;",
                                        "constexpr register int x = 0;"}) {
        expectNoConflict(inMain(decl),
                         "C23 6.7.2p2 excepts constexpr with static or "
                         "register; refusing it would put DSS BELOW the union");
        expectNoConflict(inMain("const " + std::string{decl}),
                         "the qualifier-led spelling is the same declaration");
    }
    // The for-init half of the same bullet.
    for (std::string_view const decl : {"register constexpr int x = 0",
                                        "constexpr register int x = 0"}) {
        expectNoConflict(inForInit(decl),
                         "C 6.8.5p3 admits register and constexpr in a "
                         "for-init and 6.7.2p2 excepts the pair");
    }
}

// ---------------------------------------------------------------------------
// ★★★ THE AXIS THAT DECIDES `auto`: NOT WHICH SPECIFIERS, BUT WHETHER THE TYPE
// IS INFERRED (C23 6.7.2p4). Both halves in ONE test on purpose -- they are the
// same declaration written twice, and reading them side by side is the only way
// the distinction is legible. ✔MEASURED: gcc -std=c2x and clang -std=c23 accept
// the inferred spelling and refuse the explicit one; DSS accepted BOTH before
// this cycle.
// ---------------------------------------------------------------------------
TEST(BlockScopeStorageClassExclusivity, AutoIsExemptOnlyWhenTheTypeIsInferred) {
    for (std::string_view const spec : {"static auto", "auto static",
                                        "register auto", "auto register",
                                        "constexpr auto", "auto constexpr"}) {
        expectNoConflict(inMain(std::string{spec} + " x = 0;"),
                         "the type IS inferred, so C23 6.7.2p4's precondition "
                         "holds and 6.7.2p2's auto bullet applies");
        expectOneConflict(inMain(std::string{spec} + " int x = 0;"),
                          "an explicit type means nothing is inferred, so the "
                          "auto exception does NOT apply -- this is the pair "
                          "the same two keywords make illegal");
    }
}

// ---------------------------------------------------------------------------
// ★★★ THE CONFLICT ONLY A THREE-SPECIFIER PROBE CAN SEE, AND THE REASON THE
// INFERENCE ROW CARRIES `static`/`register` ENTRIES AT ALL. On that row `auto`
// is compatible with BOTH, but they are not compatible with EACH OTHER -- so a
// pair sweep finds nothing and would have shipped those entries as decoration.
// ✔MEASURED: all six permutations compiled rc=0 before this cycle and gcc
// -std=c2x refuses every one.
// ---------------------------------------------------------------------------
TEST(BlockScopeStorageClassExclusivity,
     InferredTypeRowStillRefusesAConflictBetweenTwoNonAutoSpecifiers) {
    for (std::string_view const spec : {"auto register static",
                                        "auto static register",
                                        "register auto static",
                                        "register static auto",
                                        "static auto register",
                                        "static register auto"}) {
        expectOneConflict(inMain(std::string{spec} + " x = 0;"),
                          "the type is inferred so `auto` is exempt, but "
                          "`static` and `register` still conflict with each "
                          "other and the row must say so");
    }
}

// ---------------------------------------------------------------------------
// THE MESSAGE. A report that names one specifier sends the reader hunting for
// an offender it never identifies, and the group NAME must come from config
// verbatim so the engine holds no hardcoded word for it. The specifier walk
// pops children in REVERSE source order, so this arm is what catches a lost
// sort.
// ---------------------------------------------------------------------------
TEST(BlockScopeStorageClassExclusivity,
     StorageClassConflictNamesBothSpecifiersAndTheGroupAtBlockScope) {
    auto cu = buildShippedUnit("c", {inMain("static register int x = 0;")});
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    std::string msg;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_ConflictingStorageClassSpecifiers) {
            msg = d.actual;
            break;
        }
    }
    ASSERT_FALSE(msg.empty());
    auto const posStatic = msg.find("static");
    auto const posRegister = msg.find("register");
    EXPECT_NE(posStatic, std::string::npos) << msg;
    EXPECT_NE(posRegister, std::string::npos) << msg;
    EXPECT_LT(posStatic, posRegister)
        << "the message must name the specifiers in SOURCE order; got: " << msg;
    EXPECT_NE(msg.find("storage-class"), std::string::npos)
        << "the group's NAME is config text and must be rendered verbatim; "
           "got: " << msg;
}

// ---------------------------------------------------------------------------
// C23 6.7.2p2 BULLET 1 AT BLOCK SCOPE, expressed by the thread-storage
// spellings simply NOT declaring the group -- the standard's own sentence
// living in the vocabulary rather than as an engine carve-out. `static
// thread_local int x;` is legal and must stay silent on this code.
// ⚠ Its complement (`register` beside a thread spelling) is owned one facility
// over by `semantics.threadLocal.incompatibleSpecifierTokens`, so this arm
// asserts only that THIS guard keeps its hands off.
// ---------------------------------------------------------------------------
TEST(BlockScopeStorageClassExclusivity,
     ThreadLocalKeepsItsOwnExemptionAtBlockScope) {
    for (std::string_view const decl : {"static _Thread_local int x = 0;",
                                        "_Thread_local static int x = 0;",
                                        "static thread_local int x = 0;",
                                        "thread_local static int x = 0;"}) {
        expectNoConflict(inMain(decl),
                         "6.7.2p2 excepts a thread-storage specifier beside "
                         "static, and the config states it by declaring no "
                         "group on those two spellings");
    }
}

// ---------------------------------------------------------------------------
// DISTINCT-KEYED, NOT COUNT-KEYED -- the same asymmetry the file-scope twin
// pins. ✔MEASURED: clang 18.1.3 compiles a repeated storage-class specifier
// where gcc and MSVC refuse, so the union rule makes a repeat legal for DSS. A
// guard written as "at most one member of the group" reds here, which is
// exactly the wrong answer.
// ---------------------------------------------------------------------------
TEST(BlockScopeStorageClassExclusivity,
     RepeatedStorageClassSpecifierStillCompilesAtBlockScope) {
    for (std::string_view const decl : {"static static int x = 0;",
                                        "register register int x = 0;",
                                        "auto auto int x = 0;"}) {
        expectNoConflict(inMain(decl),
                         "clang accepts a repeated storage-class specifier, so "
                         "the guard must compare DISTINCT members only");
    }
}

// ---------------------------------------------------------------------------
// THE GATED SPELLINGS KEEP THEIR OWN, BETTER DIAGNOSTIC. `forDecl` gaining a
// `linkageSpecifiers` table must not move `static`/`thread_local` in a for-init
// off `gatedMarkers` and onto the generic conflict code -- the gate names the
// actual C 6.8.5p3 / 6.7.2p3 violation, which the pair message would not.
// ---------------------------------------------------------------------------
TEST(BlockScopeStorageClassExclusivity,
     ForInitGatedSpecifiersStillReportTheirOwnCode) {
    {
        auto cu = buildShippedUnit("c", {inForInit("static int x = 0")});
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
        EXPECT_TRUE(hasCode(model.diagnostics(),
                            DiagnosticCode::S_StaticStorageInForInit))
            << "a static for-init is C 6.8.5p3, reported by the row's gate";
    }
    for (std::string_view const decl : {"thread_local int x = 0",
                                        "_Thread_local int x = 0"}) {
        auto cu = buildShippedUnit("c", {inForInit(decl)});
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
        EXPECT_TRUE(hasCode(model.diagnostics(),
                            DiagnosticCode::S_ThreadLocalRequiresStaticOrExtern))
            << decl << " -- the thread-storage gate owns this refusal";
    }
}

// ---------------------------------------------------------------------------
// ★★ THE SILENT-DIRECTION GUARD ON THE FIX ITSELF. The recorded hazard for
// giving a storage-class keyword a `linkageSpecifiers` entry is that the new
// entry joins the effect fold and CLOBBERS a co-present specifier's facet --
// the noreturn `{binding:global}` shape, where a default value overwrote a real
// one. A block-scope `static` silently losing its static storage duration is a
// far worse defect than the one being closed. The new entries declare only
// `exclusiveGroup`/`compatibleWith`, so they carry no value to overwrite WITH;
// these arms assert the CONSEQUENCE rather than the shape, because "the JSON
// has no binding key" is a claim about the document while "the co-present
// facet is still there" is a claim about the compiler.
//
// ⚠ WHAT THIS FILE CANNOT SEE, NAMED RATHER THAN LEFT IMPLICIT. Block-scope
// static STORAGE DURATION is not a semantic-model fact: C 6.2.2p6 gives a block
// static NO linkage, so `isInternalLinkage` is correctly false for it, and the
// routing to a hidden module-global happens one tier down in `linkageFrom`'s
// `staticStorageOut`. The guard for that half is the shipped runnable example
// `examples/c/auto_type_inference`, which writes `static auto acc = 3;` and
// asserts at RUNTIME that `bump()` accumulates across calls (exit 42, with a
// `release` arm) -- it goes red the moment the storage duration is dropped, on
// every leg. Asserting a weaker in-process proxy here and calling it the
// storage pin would be the instrument that answers an adjacent question.
// ---------------------------------------------------------------------------
TEST(BlockScopeStorageClassExclusivity,
     CoPresentFacetsSurviveBesideTheNewGroupOnlySpecifiers) {
    // The thread-storage facet comes from the SAME table as the new group
    // cells, on a declaration where `static` now also carries a group. If a
    // group-only entry could disturb the fold, this is where it would show.
    for (std::string_view const decl : {"static thread_local int tv = 0;",
                                        "thread_local static int tv = 0;",
                                        "static _Thread_local int tv = 0;"}) {
        auto model = analyzeShipped("c", {inMain(decl)});
        bool sawThread = false;
        for (std::size_t i = 1; i < model.symbols().size(); ++i) {
            if (model.symbols()[i].name == "tv")
                sawThread = model.symbols()[i].isThreadLocal;
        }
        EXPECT_TRUE(sawThread)
            << decl
            << " -- a facetless group entry beside a facet-carrying one must "
               "not clear the facet; both folds accumulate monotonically";
    }
    // `constexpr` moved OUT of `linkageSpecifierIgnoredKinds` and INTO the
    // table. The semantic constexpr hook reads the keyword by a different
    // route (semantics.constexpr.keywordToken), and this asserts that route
    // still fires -- i.e. the ignore-list edit did not take the keyword away
    // from the tier that owns its meaning.
    for (std::string_view const decl : {"constexpr int cv = 7;",
                                        "static constexpr int cv = 7;",
                                        "constexpr static int cv = 7;"}) {
        auto model = analyzeShipped("c", {inMain(decl)});
        bool sawConstexpr = false;
        for (std::size_t i = 1; i < model.symbols().size(); ++i) {
            if (model.symbols()[i].name == "cv")
                sawConstexpr = model.symbols()[i].isConstexpr;
        }
        EXPECT_TRUE(sawConstexpr)
            << decl
            << " -- the constexpr keyword must still reach the semantic hook "
               "after gaining a linkageSpecifiers entry";
    }
}
