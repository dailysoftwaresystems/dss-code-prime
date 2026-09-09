// ===========================================================================
// P53 lane `ex` — [[D-C-EXTERN-MUST-LEAD-THE-DECLARATION-SPECIFIERS]]
//
// THE PROPERTY THIS FILE OWNS: a top-level declaration LED by a
// declaration-specifier keyword must reach its owning production WITHOUT a
// speculative probe, so its body may be arbitrarily long.
//
// ★★ WHY IT NEEDS A PIN OF ITS OWN, AND WHY THE PIN IS TWO HALVES. `/shapes/
// topLevel` is a SPECULATIVE alt (P42). It used to be affordable for exactly
// one reason: every lead token resolved to a UNIQUE branch, so the parser's
// LL(k) candidate set was a singleton and it took the unique-production DIRECT
// DESCENT — `Parser::Impl::stepOnce`'s `candidates.size() == 1` arm — with no
// probe, no checkpoint and no budget. The moment TWO branches shared a lead
// token, that lead entered a `SpeculationProbe` whose budget is
// `lookahead x 16` = 8 x 16 = 128 tokens, and a top-level probe must swallow
// the whole FUNCTION BODY.
//
// ⚠⚠ P65 [[D-C-FILE-SCOPE-INFERRED-AUTO-MUST-LEAD-THE-DECLARATION-SPECIFIERS]]
// — THE PARAGRAPH ABOVE IS HISTORY NOW, AND HALF 1 CHANGED WITH IT. The parser
// also descends into an alt's FINAL candidate when that candidate is the
// declared-last STRUCTURAL one (`finalCandidateDirectDescent_`), which is the
// rule the all-fail path already replayed with no budget — so a SECOND owner
// declared BEFORE `topLevelDecl` now costs one fast-failing probe and nothing
// more. What still decides whether a long body parses is not HOW MANY branches
// claim the lead but WHICH one claims it LAST, so half 1 asserts that instead.
// The helper below carries the measurement; half 2 is unchanged and is what
// actually caught the proxy going stale — six leads gained a second owner while
// every body-size arm stayed green.
//
// ✔MEASURED 2026-09-02 (P53 lane `ex`, this worktree, through the shipped CLI)
// against the design this row's next attempt will reach for — a companion
// specifier run placed BEFORE `ExternKeyword` inside `externSpecifiers`, which
// puts `inline` / `_Noreturn` / both thread-local spellings into
// FIRST(externDecl) beside FIRST(topLevelDecl):
//
//   lead spelling      body 2 stmts   body 64 stmts   body 512 stmts
//   inline             ok             PARSE-RED       PARSE-RED
//   inline static      ok             PARSE-RED       PARSE-RED
//   static inline      ok             ok              ok        (lead is
//                                                      `static`, unshared)
//   extern / extern inline            ok              ok
//
// ★★★ AND THE FAILURE CANNOT BE ORDERED AWAY — MEASURED, NOT ARGUED. When
// every speculative candidate fails, the parser REPLAYS the declared-LAST
// STRUCTURAL candidate non-speculatively, with no budget. So the alt's
// declaration order picks WHICH of the two overlapping readings survives a
// long body, and the other one dies:
//
//   topLevel alt order              `inline` long body   `inline extern` long
//   …, topLevelDecl, externDecl,…   PARSE-RED            ok
//   …, externDecl, topLevelDecl,…   ok                   PARSE-RED
//
// Two branches sharing a lead token therefore cannot both work. That is the
// measurement that closes the design space to the shared-prefix MERGE (one
// declaration rule that admits both heads and discriminates AFTER the prefix)
// and rules out every arrangement that keeps two competing top-level
// declaration rules.
//
// RED-ON-DISABLE (REMOVE direction), ✔EXERCISED 2026-09-02: delete
// `"InlineKeyword"` from `singleDeclSpecifier`'s alt in
// `src/dss-config/sources/c.lang.json`. Read through `ctest`, by NAME, staged-
// config-snapshot md5 23e62ccf -> 7c9ac6ad -> 23e62ccf (moved and returned):
//   RED  EachSpecifierLeadHasExactlyOneTopLevelOwner
//   RED  ExtensionTopLevelAltCarriesTheSameInvariant
//   RED  InlineLedDefinitionParsesAtEveryBodySize
//   RED  InlineLedStaticDefinitionParsesAtEveryBodySize
//   OK   ExternLedDefinitionParsesAtEveryBodySize      <- the control
//   OK   OtherSpecifierLeadsParseAtEveryBodySize       <- the control
// The two green arms are what says the mutant hit `singleDeclSpecifier` and not
// the `inline` keyword globally: `externSpecifiers` carries its own
// `InlineKeyword`, so `extern inline` is untouched. CONTROL(pre) and
// CONTROL(post) both 2/2.
// ===========================================================================

#include "analysis/syntactic/parser.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/schema_cursor.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

using namespace dss;

namespace {

[[nodiscard]] std::shared_ptr<GrammarSchema const> cSchema() {
    auto loaded = GrammarSchema::loadShipped("c");
    EXPECT_TRUE(loaded.has_value()) << "the shipped `c` schema must load";
    return loaded.value_or(nullptr);
}

// Parse `source` with the shipped `c` grammar and report whether the parse
// produced any ERROR diagnostic. The whole point of this file is the PARSER
// tier: no semantic analysis runs, so a body that is nonsense to the semantic
// tier but well-formed to the grammar is still a valid probe.
[[nodiscard]] bool parsesClean(std::shared_ptr<GrammarSchema const> const& schema,
                               std::string source, std::string* firstError) {
    auto src = SourceBuffer::fromString(std::move(source), "<decl-lead>");
    Tokenizer tk{src, schema, DiagnosticBudget::libraryDefault()};
    auto [stream, _] = std::move(tk).tokenize();
    Parser p{src, schema, std::move(stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& diags = result.tree.diagnostics();
    if (!diags.hasErrors()) return true;
    if (firstError != nullptr) {
        for (auto const& d : diags.all()) {
            if (d.severity == DiagnosticSeverity::Error) {
                *firstError = std::string{diagnosticCodeName(d.code)};
                break;
            }
        }
    }
    return false;
}

// A top-level FUNCTION DEFINITION whose declaration specifiers are `lead` and
// whose body holds `statements` statements. `statements` is the axis the
// speculative-probe budget is sensitive to.
[[nodiscard]] std::string definitionWithBody(std::string_view lead,
                                             std::size_t statements) {
    std::string s{lead};
    if (!s.empty()) s += ' ';
    s += "int payload(int x) {\n    int acc = x;\n";
    for (std::size_t i = 0; i < statements; ++i) {
        s += "    acc += 1;\n";
    }
    s += "    return acc;\n}\n";
    return s;
}

// Body sizes that straddle the `lookahead x 16` = 128-token probe budget.
// 2 is under it by any counting; 512 statements is ~2500 tokens, twenty times
// over — the size at which a lost direct descent is unambiguous rather than
// marginal.
constexpr std::size_t kBodySizes[] = {0, 2, 8, 64, 512};

}  // namespace

// ── HALF 1: the STRUCTURAL invariant that makes half 2 possible ─────────────
//
// For every declaration-specifier keyword that may LEAD a top-level
// declaration, the DECLARED-LAST branch of `/shapes/topLevel` admitting it is
// `topLevelDecl` — the alt's fallback reading, which the parser reaches with no
// probe and no budget. This is the property the refuted design breaks, and it
// breaks INSTANTLY — no long body required — which is why it is pinned
// separately from the behaviour it protects.
//
// ⓘ `AttributeKeyword` and `BracketOpen` are DELIBERATELY ABSENT from the list
// below, and for a reason the P65 restatement makes narrower rather than
// wider: they are a documented FIRST-set overlap ({typedefDecl,
// autoInferredTopLevelDecl, topLevelDecl} — a leading attribute may precede a
// typedef, an inferred `auto`, or an ordinary declaration) and the reason this
// alt carries `speculative: true`. `topLevelDecl` IS their declared-last owner
// too, so they would in fact pass the assertion below; they stay out because
// this list is the one the P53 row named and adding to it silently would blur
// which leads the row measured.
namespace {

// Assert that at the alt `cur` points at, the DECLARED-LAST branch admitting
// each declaration-specifier keyword lead is the merged declaration rule
// `topLevelDecl`. `where` names the site for the failure message.
//
// ⚠⚠ P65 (D-C-FILE-SCOPE-INFERRED-AUTO-MUST-LEAD-THE-DECLARATION-SPECIFIERS) —
// THIS HELPER USED TO ASSERT `count == 1`, AND THAT PROXY IS REFUTED. It read:
// "two owners put every declaration led by this keyword through a 128-token
// speculative probe that must swallow the function body", which was TRUE of the
// parser it was written against and is no longer true of this one.
// `finalCandidateDirectDescent_` DESCENDS into an outermost alt's final
// candidate — with no probe, no checkpoint and no budget — whenever that
// candidate is the declared-last STRUCTURAL candidate, which is exactly the
// rule the all-fail path already replayed. A second owner declared BEFORE
// `topLevelDecl` therefore costs one fast-failing probe and nothing else.
//
// ✔MEASURED, and the count-based reading was already refusing a grammar the
// behaviour half of this very file says is fine: with `autoInferredTopLevelDecl`
// admitting a leading specifier run, six of the seven leads below have two
// owners, while `InlineLedDefinitionParsesAtEveryBodySize`,
// `InlineLedStaticDefinitionParsesAtEveryBodySize`,
// `ExternLedDefinitionParsesAtEveryBodySize`,
// `ReversedExternOrdersParseAtEveryBodySize` and
// `OtherSpecifierLeadsParseAtEveryBodySize` are all GREEN at 512 body
// statements — a proxy disagreeing with the property it proxies for.
//
// ★★★ WHAT REPLACES IT IS STRICTLY STRONGER, NOT WEAKER, and it is the fact the
// long body actually depends on: the declaration rule must be the alt's
// FALLBACK READING. Both the descent and the all-fail replay target the
// declared-LAST structural candidate (parser.cpp's `lastStructuralCandidate_`),
// so if any branch declared AFTER `topLevelDecl` claimed one of these leads,
// that branch — not the declaration — would be the budget-free reading, and
// every long-bodied definition with that lead would go through the probe. The
// old assertion could not see that at all: it counted owners without asking
// WHICH, so it stayed green under the one rearrangement that breaks the
// behaviour, and went red under the one that does not.
void expectSpecifierLeadFallsBackToTheDeclarationRule(
    GrammarSchema const& schema, SchemaCursor cur, std::string_view where) {
    const std::span<RuleId const> branches = schema.altRuleBranches(cur);
    ASSERT_FALSE(branches.empty())
        << where << " must be an alt with enumerable rule branches";

    for (std::string_view const kind : {"StaticKeyword", "ExternKeyword",
                                        "InlineKeyword", "NoreturnKeyword",
                                        "ThreadLocalKeyword",
                                        "ThreadLocalC23Keyword",
                                        "ConstexprKeyword"}) {
        const SchemaTokenId tok = schema.schemaTokens().find(kind);
        ASSERT_TRUE(tok.valid()) << "unknown token kind " << kind;

        std::string owners;
        std::string last;
        for (RuleId const branch : branches) {
            if (schema.firstSetContains(branch, tok)) {
                last = schema.rules().name(branch);
                owners += last;
                owners += ' ';
            }
        }
        EXPECT_EQ(last, "topLevelDecl")
            << kind << " must have the merged declaration rule as the "
               "declared-LAST branch of " << where
            << " that admits it, because that is the reading the parser "
               "descends into (and replays) with NO probe budget; owners = [ "
            << owners << "]. A branch declared AFTER `topLevelDecl` claiming "
               "this lead would take the budget-free reading away from every "
               "declaration and put the function body inside a probe.";
    }
}

}  // namespace

TEST(ParserDeclSpecifierLead, EachSpecifierLeadHasExactlyOneTopLevelOwner) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);

    const RuleId topLevel = schema->rules().find("topLevel");
    ASSERT_TRUE(topLevel.valid()) << "the `c` grammar must declare `topLevel`";
    expectSpecifierLeadFallsBackToTheDeclarationRule(
        *schema, schema->enterRule(topLevel), "/shapes/topLevel");

    // ✔MEASURED 2026-09-02 by printing every branch's `predictivePrefixLen`
    // here: `topLevelDecl` reports ZERO (a length-1 prefix is dropped by
    // `computePredictivePrefixes`, and the rule enters on a variable-width
    // element). So the LL(k) PRUNE never separated this branch from a sibling
    // sharing its lead and cannot be restored to — the FALLBACK-READING
    // invariant asserted above is the ONLY thing keeping a long body off the
    // speculative probe. Asserted rather than narrated, because a repair that
    // hopes to buy the prune back with a two-token lead is chasing a mechanism
    // that was never running. (`externDecl` stood beside it here until P53
    // merged it into `topLevelDecl`; the rule no longer exists, so asking about
    // it would assert on nothing.)
    for (RuleId const branch :
         schema->altRuleBranches(schema->enterRule(topLevel))) {
        if (branch.v == schema->rules().find("topLevelDecl").v) {
            EXPECT_LT(schema->predictivePrefixLen(branch), 2U)
                << schema->rules().name(branch)
                << " has no multi-token predictive prefix, so nothing but the "
                   "fallback-reading invariant keeps it off the speculative "
                   "probe";
        }
    }

    // ★★★ P53, THE MERGE'S OWN INVARIANT — that `extern` is owned by the
    // DECLARATION rule and not by a second top-level declaration rule of its
    // own. The pre-P65 helper counted owners without asking which, and would
    // have stayed green if the merge were UNDONE and `externDecl` came back as
    // this alt's sole `extern` owner — one owner either way. The helper above
    // now asserts the identity for all seven leads, so this block is that
    // assertion's `extern` case restated where the P53 row can find it, and it
    // is kept rather than deleted because the row cites the fact by name: what
    // made every ordering parse is that `extern` became an ordinary
    // `singleDeclSpecifier` in the SAME rule as `inline` / `_Noreturn` / the
    // thread-local pair.
    {
        const SchemaTokenId ext = schema->schemaTokens().find("ExternKeyword");
        ASSERT_TRUE(ext.valid());
        std::string owner;
        for (RuleId const branch :
             schema->altRuleBranches(schema->enterRule(topLevel))) {
            if (schema->firstSetContains(branch, ext))
                owner = schema->rules().name(branch);
        }
        EXPECT_EQ(owner, "topLevelDecl")
            << "`extern` must be led by the MERGED declaration rule, not by a "
               "second top-level declaration rule of its own — that is what "
               "makes C 6.7.1's specifier set genuinely unordered";
    }
}

// `extensionTopLevel` (`__extension__ <declaration>`) carries its OWN
// speculative alt over the same declaration rules, so it carries the same
// obligation — a repair that fixes only `/shapes/topLevel` would leave every
// `__extension__`-prefixed declaration on the probe. glibc's headers write
// that prefix, so this is a reachable position, not a hypothetical one.
TEST(ParserDeclSpecifierLead, ExtensionTopLevelAltCarriesTheSameInvariant) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);

    const RuleId ext = schema->rules().find("extensionTopLevel");
    ASSERT_TRUE(ext.valid());
    const SchemaTokenId kw = schema->schemaTokens().find("ExtensionKeyword");
    ASSERT_TRUE(kw.valid());
    const SchemaCursor afterKeyword =
        schema->advance(schema->enterRule(ext), kw);
    ASSERT_TRUE(afterKeyword.valid())
        << "`extensionTopLevel` must begin with `ExtensionKeyword`";
    expectSpecifierLeadFallsBackToTheDeclarationRule(*schema, afterKeyword,
                                   "/shapes/extensionTopLevel's inner alt");
}

// ── HALF 2: the behaviour the invariant protects ────────────────────────────

// The exact shape the refuted design broke: a NON-STATIC `inline` function
// definition. ✔MEASURED under a companion run before `ExternKeyword`: clean at
// 8 body statements, PARSE-RED from 16 upward, at every size tested to 512.
TEST(ParserDeclSpecifierLead, InlineLedDefinitionParsesAtEveryBodySize) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);
    for (std::size_t const n : kBodySizes) {
        std::string err;
        EXPECT_TRUE(parsesClean(schema, definitionWithBody("inline", n), &err))
            << "a non-static `inline` definition with " << n
            << " body statements must parse; first error = " << err;
    }
}

// `inline static` is `inline`-LED, so it shares the fate of the case above —
// and it is the one the P53 bisection ADDED to the blast radius: `static
// inline` is fine (its lead is `static`, which no second branch claims) while
// `inline static` is not. A pin that tested only the bare keyword would have
// missed half the damage.
TEST(ParserDeclSpecifierLead, InlineLedStaticDefinitionParsesAtEveryBodySize) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);
    for (std::size_t const n : kBodySizes) {
        std::string err;
        EXPECT_TRUE(
            parsesClean(schema, definitionWithBody("inline static", n), &err))
            << "`inline static` with " << n
            << " body statements must parse; first error = " << err;
    }
    // The SAME two keywords in the other order. Against the companion-run
    // design this is the discriminating control — ✔MEASURED green at 512 while
    // `inline static` was PARSE-RED at 64 — because candidacy is decided by the
    // LEAD token and `static` is claimed by one branch only. (Against this
    // file's own REMOVE-direction mutant it goes red WITH its sibling, which is
    // why the header's control arms are the `extern`-led and other-lead tests.)
    for (std::size_t const n : kBodySizes) {
        std::string err;
        EXPECT_TRUE(
            parsesClean(schema, definitionWithBody("static inline", n), &err))
            << "`static inline` with " << n
            << " body statements must parse; first error = " << err;
    }
}

// `extern`-led definitions are probe-free today only because FIRST(externDecl)
// is DISJOINT from every sibling. That disjointness is exactly what the
// shared-prefix merge must preserve (by leaving one branch, not two), so it is
// pinned rather than assumed.
TEST(ParserDeclSpecifierLead, ExternLedDefinitionParsesAtEveryBodySize) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);
    for (std::string_view const lead : {"extern", "extern inline",
                                        "extern __inline"}) {
        for (std::size_t const n : kBodySizes) {
            std::string err;
            EXPECT_TRUE(parsesClean(schema, definitionWithBody(lead, n), &err))
                << '`' << lead << "` with " << n
                << " body statements must parse; first error = " << err;
        }
    }
}

// ★★★ P53 — THE ORDERINGS THIS ROW EXISTS FOR, AT EVERY BODY SIZE.
//
// Before the merge each of these was `error[P_NoAlternativeMatched]` at ANY
// body size, because `extern` was the HEAD of its own file-scope declaration
// rule and `topLevelDecl`'s head never matched the keyword. Now they are all
// the same rule, so they must parse — AND they must parse at 512 body
// statements, which is the half that says the merge did not buy acceptance by
// putting the lead back on the 128-token speculative probe.
//
// ⚠ `_Noreturn inline extern` is the case that retires the enumerate-a-two-
// token-lead family on CONFORMANCE rather than on taste: gcc 13.3.0, clang
// 18.1.3 and MSVC 19.51.36252 all accept TWO leading specifiers before
// `extern` (✔MEASURED separately 2026-09-02), so a repair admitting only one
// would still have been below the reference union while reading as complete.
TEST(ParserDeclSpecifierLead, ReversedExternOrdersParseAtEveryBodySize) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);
    for (std::string_view const lead : {"inline extern", "__inline extern",
                                        "_Noreturn extern",
                                        "_Thread_local extern",
                                        "thread_local extern",
                                        "_Noreturn inline extern",
                                        "extern inline _Noreturn"}) {
        for (std::size_t const n : kBodySizes) {
            std::string err;
            EXPECT_TRUE(parsesClean(schema, definitionWithBody(lead, n), &err))
                << '`' << lead << "` with " << n
                << " body statements must parse — C 6.7.1 makes the "
                   "declaration specifiers an unordered SET; first error = "
                << err;
        }
    }
}

// The same orderings in DECLARATION position (no body), where the parse has no
// block to swallow and a failure can only be the specifier set itself. Kept
// separate from the definition arm so a regression says WHICH half broke.
TEST(ParserDeclSpecifierLead, ReversedExternOrdersParseAsDeclarations) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);
    for (std::string_view const decl : {"inline extern int p(int);\n",
                                        "__inline extern int p(int);\n",
                                        "_Noreturn extern void die(int);\n",
                                        "_Thread_local extern int e;\n",
                                        "thread_local extern int e;\n",
                                        "_Noreturn inline extern void die(int);\n",
                                        "extern inline _Noreturn void die(int);\n",
                                        "__extension__ extern int ffsll(long long);\n"}) {
        std::string err;
        EXPECT_TRUE(parsesClean(schema, std::string{decl}, &err))
            << decl << " must parse; first error = " << err;
    }
}

// The remaining single-token specifier leads. Each is a keyword the refuted
// design would have moved into FIRST(externDecl); pinning them here means a
// future attempt reds on the keyword it actually widened, not only on
// `inline`.
TEST(ParserDeclSpecifierLead, OtherSpecifierLeadsParseAtEveryBodySize) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);
    for (std::string_view const lead : {"_Noreturn", "_Thread_local",
                                        "thread_local", "static",
                                        "constexpr"}) {
        for (std::size_t const n : kBodySizes) {
            std::string err;
            EXPECT_TRUE(parsesClean(schema, definitionWithBody(lead, n), &err))
                << '`' << lead << "` with " << n
                << " body statements must parse; first error = " << err;
        }
    }
}
