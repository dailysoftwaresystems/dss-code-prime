// ★★★ THE STANDING REGRESSION GUARD FOR THE `languageReferences` HEADLINE
// PROPERTY (plan 29 P1+P2, exit criterion (c) — "TESTED, not asserted").
//
// THE PROPERTY UNDER TEST, in the operator's own words: *"we don't have this
// mechanism, and is the only one acceptable, so asm language is reused."*
// Concretely — a host language that declares NONE of the assembly grammar
// reaches the WHOLE of it, plus asm's semantic refusal and asm's HIR-lowering
// row and asm's pipeline-entry row, through one `languageReferences` block and
// ZERO new grammar rules of its own. That is what "reused" means, and until
// this file existed it was measured once by hand and then reverted, leaving the
// headline claim asserted rather than guarded.
//
// ★★ WHY THE HOST IS SYNTHETIC AND LIVES INSIDE THIS FILE. `c-subset` is the
// mechanism's FIRST consumer; the second one does not exist yet. Testing the
// substrate through its only consumer proves that consumer works, not that the
// substrate is reusable — the two are different claims, and this project's
// documented answer is that the test BUILDS the consuming shape itself rather
// than waiting for a real second consumer to expose a miss. So the host below
// is a JSON string, loaded through `GrammarSchema::loadFromText`; the
// referenced `asm.lang.json` resolves from the SHIPPED tree through
// `findShippedConfig` (`dss_add_test` sets `DSS_CONFIG_ROOT`). Nothing
// experimental ships, and no shipped `.lang.json` is touched.
//
// ★★ THE HOST IS DELIBERATELY NOT C-SHAPED. Every token kind, every keyword
// spelling and every rule name below is alien to `c-subset` (`EMIT` not `asm`,
// `|`/`||` not `:`/`::`, `<<`/`>>` not `(`/`)`, `hostValue` not `expression`).
// `AsmHostProbeIsNotCShaped` pins that: a mechanism that only works for a host
// that happens to look like C is a file split, not a reuse mechanism, and this
// suite must not be able to pass by accident because the two vocabularies
// coincided.
//
// ★★ RED-ON-DISABLE IS FAIL-CLOSED BY CONSTRUCTION. The disabled arms are
// SECOND SCHEMA STRINGS built by the same `makeHostDoc()` in this same process
// — no file is mutated, no shipped config is edited, no mutator subprocess is
// spawned and no build-time flag gates them. There is therefore no way for the
// negative arm to be skipped, no-op'd or left half-applied: if the loader ever
// starts handing asm's rules to a host that did not reference asm, the absence
// arms go red on the very same run as the presence arms go green. The two arms
// share ONE list of names (`kAsmRules`) and one document builder, so they are a
// true differential over a single variable — the presence of the
// `languageReferences` block — and cannot drift apart.

#include "core/types/grammar_schema.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace dss;

namespace {

// ── the subject, named exactly ────────────────────────────────────────────
//
// The TWELVE rules `src/dss-config/sources/asm.lang.json` owns. Spelled out
// rather than counted: a count alone stays green if the mechanism imports
// twelve of the WRONG shapes, and the load-bearing claim is that these exact
// rules reach a host that declares none of them. This is also the ONE list the
// positive and negative arms share.
constexpr std::array<std::string_view, 12> kAsmRules{
    "asmStmt",
    "asmOutputsTail",
    "asmInputsTail",
    "asmInputsTailFused",
    "asmClobbersTail",
    "asmClobbersTailFused",
    "asmLabelsTail",
    "asmLabelsTailFused",
    "asmOperandList",
    "asmOperand",
    "asmClobberList",
    "asmGotoLabelList",
};

// asm's declared HOLES — its `requires` block. These are ROLE names, and after
// a successful merge every one of them must have been SUBSTITUTED AWAY for the
// host's own name. A role name surviving into the interned vocabulary would
// mean the grammar merged unsubstituted: it would compile, and it would match
// nothing.
constexpr std::array<std::string_view, 2> kAsmRuleHoles{
    "operandExpr", "templateText"};
constexpr std::array<std::string_view, 13> kAsmTokenHoles{
    "asmKeyword",  "volatileQualifier",     "inlineQualifier",
    "gotoQualifier", "sectionSeparator",    "sectionSeparatorFused",
    "operandSeparator", "symbolName",       "argsOpen",
    "argsClose",   "symbolicNameOpen",      "symbolicNameClose",
    "statementEnd"};

// ── the synthetic host, in three variants over ONE base ───────────────────
//
// The ONLY thing that varies is (a) whether the `languageReferences` block is
// present and (b) whether a host shape descends into `asmStmt`. Everything
// else — version, language block, token table, keyword table, the host's own
// four shapes — is byte-identical across the variants, which is what makes the
// presence/absence comparison a differential over one variable rather than a
// comparison of two unrelated documents.

constexpr std::string_view kDocHead = R"(
  "dssSchemaVersion": 4,
  "language": { "name": "AsmHostProbe", "version": "0.0.1" },
)";

// The whole of the host's participation in the mechanism. Thirteen token roles
// and two rule roles bound to THIS host's alien vocabulary — and not one
// grammar rule.
constexpr std::string_view kAsmReferenceBlock = R"(
  "languageReferences": {
    "asm": {
      "entry": "asmStmt",
      "bindRules": {
        "operandExpr":  "hostValue",
        "templateText": "hostPayload"
      },
      "bindTokens": {
        "asmKeyword":            "EmitWord",
        "volatileQualifier":     "LoudWord",
        "inlineQualifier":       "TightWord",
        "gotoQualifier":         "LeapWord",
        "sectionSeparator":      "PipeMark",
        "sectionSeparatorFused": "PipePipeMark",
        "operandSeparator":      "AmpMark",
        "symbolName":            "Identifier",
        "argsOpen":              "AngleOpen",
        "argsClose":             "AngleClose",
        "symbolicNameOpen":      "BraceOpen",
        "symbolicNameClose":     "BraceClose",
        "statementEnd":          "BangEnd"
      }
    }
  },
)";

// Not one lexeme, keyword or kind here is spelled the way `c-subset` spells it.
constexpr std::string_view kTokensAndKeywords = R"(
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "||": [{ "kind": "PipePipeMark", "priority": 20 }],
    "|":  [{ "kind": "PipeMark",     "priority": 10 }],
    "&":  [{ "kind": "AmpMark" }],
    "<<": [{ "kind": "AngleOpen" }],
    ">>": [{ "kind": "AngleClose" }],
    "{":  [{ "kind": "BraceOpen" }],
    "}":  [{ "kind": "BraceClose" }],
    "!":  [{ "kind": "BangEnd" }],
    "`":  [{ "kind": "TickText" }]
  },
  "keywords": [
    { "word": "EMIT",  "kind": "EmitWord" },
    { "word": "LOUD",  "kind": "LoudWord" },
    { "word": "TIGHT", "kind": "TightWord" },
    { "word": "LEAP",  "kind": "LeapWord" }
  ],
)";

// The host's OWN four shapes, in the arm that descends into the referenced
// grammar. `hostStmt` reaching `asmStmt` is the host's entire grammatical
// participation — one atom in one alt.
constexpr std::string_view kShapesReachingAsm = R"(
  "shapes": {
    "root":          { "sequence": [{ "repeat": "hostStmt" }] },
    "hostStmt":      { "alt": ["asmStmt", "hostPlainStmt"] },
    "hostPlainStmt": { "sequence": ["hostValue", "BangEnd"] },
    "hostValue":     { "sequence": ["Identifier"] },
    "hostPayload":   { "sequence": ["TickText"] }
  }
)";

// The same four shapes with the `asmStmt` atom replaced by a host-local one, so
// the self-contained arm is a COMPLETE language rather than a broken one — the
// absence of asm's rules there is then a fact about the mechanism, not a
// side-effect of a failed load.
constexpr std::string_view kShapesSelfContained = R"(
  "shapes": {
    "root":          { "sequence": [{ "repeat": "hostStmt" }] },
    "hostStmt":      { "alt": ["hostTickStmt", "hostPlainStmt"] },
    "hostTickStmt":  { "sequence": ["hostPayload", "BangEnd"] },
    "hostPlainStmt": { "sequence": ["hostValue", "BangEnd"] },
    "hostValue":     { "sequence": ["Identifier"] },
    "hostPayload":   { "sequence": ["TickText"] }
  }
)";

enum class HostVariant {
    WithReference,          // the mechanism engaged — the property under test
    SelfContained,          // no reference, no asm atom: loads clean, asm ABSENT
    ReachesAsmWithoutRef,   // no reference, asm atom kept: must FAIL LOUD
};

[[nodiscard]] std::string makeHostDoc(HostVariant variant) {
    std::string doc = "{";
    doc += kDocHead;
    if (variant == HostVariant::WithReference) doc += kAsmReferenceBlock;
    doc += kTokensAndKeywords;
    doc += (variant == HostVariant::SelfContained) ? kShapesSelfContained
                                                   : kShapesReachingAsm;
    doc += "}";
    return doc;
}

// The host's OWN declared shape names, read back out of the document the test
// actually loads. Read rather than hand-listed on purpose: a hand-copied list
// would let the host quietly grow an `asmStmt` of its own and the "the host
// declared none of them" claim would rot into a tautology.
[[nodiscard]] std::vector<std::string> declaredShapeNames(std::string const& doc) {
    std::vector<std::string> names;
    auto const parsed = nlohmann::json::parse(doc);
    for (auto const& [key, value] : parsed.at("shapes").items()) {
        if (!key.empty() && key.front() == '$') continue;   // documentation key
        names.push_back(key);
    }
    return names;
}

[[nodiscard]] std::string firstError(
        std::vector<ConfigDiagnostic> const& diags) {
    return diags.empty() ? std::string{"<no diagnostics>"} : diags.front().message;
}

// Load a variant, failing the calling test with the loader's own words rather
// than a bare `nullptr` if it does not load.
[[nodiscard]] std::shared_ptr<GrammarSchema> loadOk(std::string const& doc) {
    auto result = GrammarSchema::loadFromText(doc, "<synthetic-asm-host>");
    if (!result.has_value()) {
        ADD_FAILURE() << "synthetic host failed to load: "
                      << firstError(result.error());
        return nullptr;
    }
    return *result;
}

} // namespace

// ─── 0. the premise: the host declares NONE of asm's twelve rules ─────────
//
// Every later assertion is conditional on this. If the host ever declared one
// of these itself, "the rules were reused" would be unfalsifiable.

TEST(LanguageReferences, SyntheticHostDeclaresNoneOfTheAsmRules) {
    for (auto const variant : {HostVariant::WithReference,
                               HostVariant::SelfContained,
                               HostVariant::ReachesAsmWithoutRef}) {
        auto const doc  = makeHostDoc(variant);
        auto const own  = declaredShapeNames(doc);
        EXPECT_EQ(own.size(), (variant == HostVariant::SelfContained) ? 6u : 5u)
            << "the synthetic host's own shape count changed — update the "
               "expectation deliberately, never to make this pass";
        for (auto const& asmRule : kAsmRules) {
            EXPECT_EQ(std::ranges::find(own, asmRule), own.end())
                << "the synthetic host declares '" << asmRule
                << "' itself — the reuse claim would be vacuous";
        }
    }
}

TEST(LanguageReferences, SyntheticHostDeclaresNoSemanticsLoweringOrPipelineBlock) {
    // The companion-row half of the property: whatever `semantics.inlineAsm`,
    // `hirLowering` and `pipelineEntry` the loaded schema ends up with, the host
    // document provably did not write them.
    //
    // ★★ THE POSITIVE ANCHORS ARE NOT DECORATION — WITHOUT THEM THIS TEST IS
    // SATISFIED BY NOTHING EXISTING. Three `EXPECT_FALSE(contains(...))` calls
    // stay green over an EMPTY object, over a document builder that returned
    // `{}`, and over a `contains` that answers no to everything: "the host wrote
    // no `semantics`" and "there is no host" are DIFFERENT CLAIMS and only the
    // first is the property. So each variant additionally asserts the keys the
    // host DOES write, and the referencing variant asserts that the three blocks
    // it did not write NEVERTHELESS ARRIVE in the loaded schema — which is the
    // only reason their absence from the document is interesting at all.
    for (auto const variant : {HostVariant::WithReference,
                               HostVariant::SelfContained}) {
        auto const parsed = nlohmann::json::parse(makeHostDoc(variant));
        EXPECT_FALSE(parsed.contains("semantics"));
        EXPECT_FALSE(parsed.contains("hirLowering"));
        EXPECT_FALSE(parsed.contains("pipelineEntry"));
        // ...and this IS the real document, not an empty one.
        EXPECT_TRUE(parsed.contains("language"));
        EXPECT_TRUE(parsed.contains("tokens"));
        EXPECT_TRUE(parsed.contains("keywords"));
        EXPECT_TRUE(parsed.contains("shapes"));
        EXPECT_EQ(parsed.contains("languageReferences"),
                  variant == HostVariant::WithReference)
            << "the ONE variable this suite differentiates on is not where the "
               "variant says it is";
    }
    // The three blocks the host provably did not write are nevertheless THERE
    // once it references asm — inherited, which is the whole claim.
    auto schema = loadOk(makeHostDoc(HostVariant::WithReference));
    ASSERT_NE(schema, nullptr);
    EXPECT_TRUE(schema->semantics().inlineAsm.rule.valid())
        << "the host wrote no 'semantics' AND inherited none — the absence "
           "assertions above would then be true of a schema with no inline-asm "
           "surface at all, which proves nothing";
    EXPECT_EQ(schema->hirLowering().ruleMappings.size(), 1u);
    EXPECT_EQ(schema->pipelineEntry().byRule.size(), 1u);
}

// ─── 1. ZERO NEW GRAMMAR RULES: all twelve arrive, by name ────────────────

TEST(LanguageReferences, HostInheritsEveryAsmRuleByName) {
    auto schema = loadOk(makeHostDoc(HostVariant::WithReference));
    ASSERT_NE(schema, nullptr);

    for (auto const& asmRule : kAsmRules) {
        const auto id = schema->rules().find(asmRule);
        EXPECT_TRUE(id.valid())
            << "rule '" << asmRule << "' did NOT reach the host — the "
               "languageReferences merge stopped importing asm's grammar";
    }
}

TEST(LanguageReferences, InheritedRulesAreCompiledNotMerelyInterned) {
    // An interned NAME with no compiled body is the failure mode a name-only
    // check would sail past: `RuleInterner::find` would still answer "yes" for
    // a rule that matches nothing. A non-empty FIRST set is only produced by
    // `computeFirstAndNullable` over a real body.
    auto schema = loadOk(makeHostDoc(HostVariant::WithReference));
    ASSERT_NE(schema, nullptr);

    for (auto const& asmRule : kAsmRules) {
        const auto id = schema->rules().find(asmRule);
        ASSERT_TRUE(id.valid()) << asmRule;
        EXPECT_FALSE(schema->firstSetOf(id).empty())
            << "rule '" << asmRule << "' is interned but has no FIRST set — "
               "it was merged as a name, not as a grammar";
    }
}

TEST(LanguageReferences, InheritedRulesResolveAgainstTheHostsOwnTokenVocabulary) {
    // The half that separates REUSE from a copy: asm's rules were written in
    // ROLE names, and here they predict on THIS host's alien kinds. If hole
    // substitution silently stopped, the rules would still merge and would
    // predict on nothing.
    auto schema = loadOk(makeHostDoc(HostVariant::WithReference));
    ASSERT_NE(schema, nullptr);

    const auto emit     = schema->schemaTokens().find("EmitWord");
    const auto pipe     = schema->schemaTokens().find("PipeMark");
    const auto pipePipe = schema->schemaTokens().find("PipePipeMark");
    ASSERT_TRUE(emit.valid());
    ASSERT_TRUE(pipe.valid());
    ASSERT_TRUE(pipePipe.valid());

    // `asmStmt` starts with the host's asmKeyword kind, and ONLY that.
    const auto stmt = schema->rules().find("asmStmt");
    ASSERT_TRUE(stmt.valid());
    auto const stmtFirst = schema->firstSetOf(stmt);
    ASSERT_EQ(stmtFirst.size(), 1u);
    EXPECT_EQ(stmtFirst[0].v, emit.v)
        << "asmStmt does not predict on the host's own 'EMIT' keyword kind";

    // The boundary chain: a plain separator opens a section, a FUSED one skips
    // the section before it — two distinct host kinds, both reachable from the
    // outputs tail.
    const auto outputsTail = schema->rules().find("asmOutputsTail");
    ASSERT_TRUE(outputsTail.valid());
    EXPECT_TRUE(schema->firstSetContains(outputsTail, pipe))
        << "asmOutputsTail does not predict on the host's sectionSeparator";
    EXPECT_TRUE(schema->firstSetContains(outputsTail, pipePipe))
        << "asmOutputsTail does not predict on the host's "
           "sectionSeparatorFused — the fused-boundary arms did not rebind";
}

TEST(LanguageReferences, HostSpelledAsmBarrierWalksTheMergedRuleToCompletion) {
    // ★ THE BEHAVIOURAL HALF: not "the schema contains asmStmt" but "an asm
    // statement written in the HOST's spelling walks the merged rule to
    // completion". The walk uses the schema's OWN decision procedure — the same
    // `enterRule`/`advance`/`leaveRule` the descent engine drives — so nothing
    // here re-implements the grammar; it only supplies tokens and checks that
    // each one is accepted. Subject: the empty-template barrier, host-spelled
    //     EMIT << ` >> !
    // which is `__asm__ ("");` in c-subset's vocabulary and shares not one
    // token kind with it.
    auto schema = loadOk(makeHostDoc(HostVariant::WithReference));
    ASSERT_NE(schema, nullptr);

    const auto stmt    = schema->rules().find("asmStmt");
    const auto payload = schema->rules().find("hostPayload");
    ASSERT_TRUE(stmt.valid());
    ASSERT_TRUE(payload.valid());

    auto kind = [&](std::string_view n) {
        const auto k = schema->schemaTokens().find(n);
        EXPECT_TRUE(k.valid()) << "host token kind '" << n << "' is missing";
        return k;
    };

    // `asmKeyword` opens the statement...
    auto cur = schema->advance(schema->enterRule(stmt), kind("EmitWord"));
    ASSERT_TRUE(cur.valid()) << "asmStmt rejected the host's asm keyword";
    // ...the qualifier `{repeat {alt …}}` run is empty here and must be
    // skippable, so `argsOpen` lands next...
    cur = schema->advance(cur, kind("AngleOpen"));
    ASSERT_TRUE(cur.valid())
        << "asmStmt rejected the host's argsOpen after an EMPTY qualifier run";
    // ...and the template slot is now a RuleLeaf for the rule the host bound to
    // the `templateText` HOLE. That it is `hostPayload` — and not a rule named
    // in asm.lang.json — is the substitution proved structurally.
    ASSERT_EQ(schema->slotKind(cur), SlotKind::RuleLeaf);
    EXPECT_EQ(schema->slotRuleRef(cur).v, payload.v)
        << "the template slot does not point at the host's bound rule";

    auto inner = schema->advance(schema->enterRule(payload), kind("TickText"));
    ASSERT_TRUE(inner.valid());
    EXPECT_TRUE(schema->isAtEndOfRule(inner));

    cur = schema->leaveRule(cur);
    ASSERT_TRUE(cur.valid()) << "could not resume asmStmt after the template";
    // The four section tails are all ABSENT on a bare barrier, so `argsClose`
    // must be accepted straight after the template.
    cur = schema->advance(cur, kind("AngleClose"));
    ASSERT_TRUE(cur.valid())
        << "asmStmt rejected argsClose with every section tail absent — the "
           "optional boundary chain did not merge as optional";
    cur = schema->advance(cur, kind("BangEnd"));
    ASSERT_TRUE(cur.valid()) << "asmStmt rejected the host's statementEnd";
    EXPECT_TRUE(schema->isAtEndOfRule(cur))
        << "the barrier walked asmStmt but did not COMPLETE it";
}

TEST(LanguageReferences, AsmRoleNamesAreSubstitutedAwayNotLeaked) {
    // A surviving role name means the merged grammar was interned
    // UNsubstituted: it would load clean and match nothing.
    auto schema = loadOk(makeHostDoc(HostVariant::WithReference));
    ASSERT_NE(schema, nullptr);

    // ⚠ ANCHOR THE MERGE FIRST — MEASURED, not stylistic. Run against a host
    // with NO reference this suite's absence checks below are all trivially
    // true, so without this line the test would stay GREEN on the very
    // regression it exists to catch (a merge that silently stopped importing
    // anything). Pointed at the reference-less host it was the one arm that
    // did not go red; now it does.
    ASSERT_TRUE(schema->rules().contains("asmStmt"))
        << "nothing was merged — the absence checks below would be vacuous";

    for (auto const& hole : kAsmRuleHoles) {
        EXPECT_FALSE(schema->rules().contains(hole))
            << "rule hole '" << hole << "' leaked into the merged grammar "
               "unsubstituted";
    }
    for (auto const& hole : kAsmTokenHoles) {
        EXPECT_FALSE(schema->schemaTokens().contains(hole))
            << "token hole '" << hole << "' leaked into the merged grammar "
               "unsubstituted";
    }
    // ...and the names they were substituted TO are the host's own.
    EXPECT_TRUE(schema->rules().contains("hostValue"));
    EXPECT_TRUE(schema->rules().contains("hostPayload"));
}

// ─── 2. the companion rows travel with the grammar ────────────────────────

TEST(LanguageReferences, HostInheritsAsmSemanticInlineAsmFacet) {
    auto schema = loadOk(makeHostDoc(HostVariant::WithReference));
    ASSERT_NE(schema, nullptr);

    auto const& asmCfg = schema->semantics().inlineAsm;
    ASSERT_TRUE(asmCfg.rule.valid())
        << "the host inherited asm's rules but NOT semantics.inlineAsm — the "
           "refusal ladder (S0057/S0062/S0063) would silently not run";

    EXPECT_EQ(asmCfg.ruleName,                  "asmStmt");
    EXPECT_EQ(asmCfg.outputsTailRuleName,       "asmOutputsTail");
    EXPECT_EQ(asmCfg.inputsTailRuleName,        "asmInputsTail");
    EXPECT_EQ(asmCfg.inputsTailFusedRuleName,   "asmInputsTailFused");
    EXPECT_EQ(asmCfg.clobbersTailRuleName,      "asmClobbersTail");
    EXPECT_EQ(asmCfg.clobbersTailFusedRuleName, "asmClobbersTailFused");
    EXPECT_EQ(asmCfg.labelsTailRuleName,        "asmLabelsTail");
    EXPECT_EQ(asmCfg.labelsTailFusedRuleName,   "asmLabelsTailFused");
    EXPECT_EQ(asmCfg.operandListRuleName,       "asmOperandList");
    EXPECT_EQ(asmCfg.operandRuleName,           "asmOperand");
    EXPECT_EQ(asmCfg.clobberListRuleName,       "asmClobberList");
    EXPECT_EQ(asmCfg.gotoLabelListRuleName,     "asmGotoLabelList");

    // ★★ THE TWO NON-REGISTER CLOBBER SPELLINGS ARRIVE AS *DATA*, WHICH IS THE
    // WHOLE POINT OF THEM BEING HERE. They are GNU-asm vocabulary, so a host
    // that wrote no `semantics` block at all now knows that `"memory"` and
    // `"cc"` are the two clobbers naming no register — and it knows it because
    // the asm document said so, not because `semantic_analyzer.cpp` contains
    // those two bytes.
    EXPECT_EQ(asmCfg.memoryClobber,             "memory");
    EXPECT_EQ(asmCfg.conditionCodeClobber,      "cc");

    // The two facet fields that name a HOLE resolve to the HOST's names — the
    // facet was rebound, not copied.
    EXPECT_EQ(asmCfg.templateRuleName,          "hostPayload");
    EXPECT_EQ(asmCfg.gotoQualifierTokenName,    "LeapWord");

    // Every RuleId in the facet resolves against the merged interner.
    EXPECT_TRUE(asmCfg.templateRule.valid());
    EXPECT_TRUE(asmCfg.outputsTailRule.valid());
    EXPECT_TRUE(asmCfg.inputsTailRule.valid());
    EXPECT_TRUE(asmCfg.inputsTailFusedRule.valid());
    EXPECT_TRUE(asmCfg.clobbersTailRule.valid());
    EXPECT_TRUE(asmCfg.clobbersTailFusedRule.valid());
    EXPECT_TRUE(asmCfg.labelsTailRule.valid());
    EXPECT_TRUE(asmCfg.labelsTailFusedRule.valid());
    EXPECT_TRUE(asmCfg.operandListRule.valid());
    EXPECT_TRUE(asmCfg.operandRule.valid());
    EXPECT_TRUE(asmCfg.clobberListRule.valid());
    EXPECT_TRUE(asmCfg.gotoLabelListRule.valid());
    EXPECT_TRUE(asmCfg.gotoQualifierToken.valid());
    EXPECT_EQ(asmCfg.rule.v, schema->rules().find("asmStmt").v);
}

// ★★★ THE SINGLE-OPERAND RULE ARRIVES *COMPILED*, AND REBOUND TO THIS HOST'S
// ALIEN KINDS — NOT MERELY INTERNED. `operandRule` is the first facet field
// that names a rule the semantic tier will walk INTO rather than merely test
// for presence, so "the name resolved" is not the property that matters: a
// `RuleId` can be valid for a rule that was interned with no body, and every
// EXPECT above would still be green while the walk matched nothing.
//
// ★★ THE TEST IS THEREFORE THE FIRST SET, TWICE OVER. Non-empty proves a body
// was compiled (`computeFirstAndNullable` produces nothing otherwise); and the
// KINDS in it are asserted to be THIS host's — `BraceOpen` for the optional
// `[name]` prefix and `TickText` for the constraint string, neither of which
// `c-subset` spells. A facet that merged as text rather than as grammar would
// pass a name check and fail both of these.
TEST(LanguageReferences, InheritedOperandRuleIsCompiledAndReboundToTheHostsKinds) {
    auto schema = loadOk(makeHostDoc(HostVariant::WithReference));
    ASSERT_NE(schema, nullptr);

    auto const& asmCfg = schema->semantics().inlineAsm;
    ASSERT_TRUE(asmCfg.operandRule.valid())
        << "the facet named no single-operand rule — a walker would have to "
           "recover operand boundaries by counting separator tokens, which the "
           "comma OPERATOR inside an operand's parens makes ambiguous";
    ASSERT_EQ(asmCfg.operandRule.v, schema->rules().find("asmOperand").v);

    auto const first = schema->firstSetOf(asmCfg.operandRule);
    ASSERT_FALSE(first.empty())
        << "'asmOperand' is interned but has NO FIRST set — it arrived as a "
           "name, not as a grammar, and the walk this RuleId exists for would "
           "match nothing while every name assertion stayed green";

    // `asmOperand` = `{optional [ symbolicNameOpen symbolName symbolicNameClose ]}`
    // then the constraint. So it predicts on the host's BraceOpen (the optional
    // prefix) AND on whatever the host bound `templateText` to.
    const auto braceOpen = schema->schemaTokens().find("BraceOpen");
    const auto tickText  = schema->schemaTokens().find("TickText");
    ASSERT_TRUE(braceOpen.valid());
    ASSERT_TRUE(tickText.valid());
    EXPECT_TRUE(schema->firstSetContains(asmCfg.operandRule, braceOpen))
        << "asmOperand does not predict on the host's symbolicNameOpen kind — "
           "the `[name]` prefix did not rebind";
    EXPECT_TRUE(schema->firstSetContains(asmCfg.operandRule, tickText))
        << "asmOperand does not predict on the host's constraint-string kind — "
           "the `templateText` hole did not rebind inside the operand";

    // ⚠ AND *ONLY* THOSE TWO. A containment pair stays green over a FIRST set
    // that also carries an unsubstituted role kind — which is exactly what a
    // half-applied merge produces — so the SIZE is asserted too. Update it
    // deliberately if `asmOperand` grows an arm; never to make this pass.
    // (That the host's vocabulary is not C's is pinned separately by
    // `AsmHostProbeIsNotCShaped`.)
    EXPECT_EQ(first.size(), 2u)
        << "asmOperand predicts on " << first.size()
        << " kinds, not the two its `{optional [name]}` + constraint shape has";
}

TEST(LanguageReferences, HostInheritsAsmHirLoweringRow) {
    auto schema = loadOk(makeHostDoc(HostVariant::WithReference));
    ASSERT_NE(schema, nullptr);

    auto const& rows = schema->hirLowering().ruleMappings;
    const auto it = std::ranges::find_if(rows, [](HirRuleMapping const& r) {
        return r.ruleName == "asmStmt";
    });
    ASSERT_NE(it, rows.end())
        << "the host inherited asm's rules but NOT the asmStmt -> InlineAsm "
           "lowering row; rows present: " << rows.size();
    EXPECT_EQ(it->hirKind, "InlineAsm");
    EXPECT_EQ(it->rule.v, schema->rules().find("asmStmt").v);
    // Exactly one row, and it is the inherited one — the host wrote none.
    EXPECT_EQ(rows.size(), 1u);
}

TEST(LanguageReferences, HostInheritsAsmPipelineEntryRow) {
    auto schema = loadOk(makeHostDoc(HostVariant::WithReference));
    ASSERT_NE(schema, nullptr);

    const auto stmt = schema->rules().find("asmStmt");
    ASSERT_TRUE(stmt.valid());
    auto const tier = schema->pipelineEntry().tierForRule(stmt);
    ASSERT_TRUE(tier.has_value())
        << "the host inherited asm's rules but NOT the asmStmt pipelineEntry "
           "row — the construct would silently take a tier nobody declared";
    EXPECT_EQ(*tier, PipelineTier::Hir);
    EXPECT_EQ(schema->pipelineEntry().byRule.size(), 1u);
}

// ─── 3. RED ON DISABLE ────────────────────────────────────────────────────
//
// Fail-closed by construction: these arms build a SECOND schema string in this
// same process and load it through the same entry point. No file is mutated, no
// shipped config is edited and no mutator subprocess is involved, so the
// negative arm cannot be skipped, stubbed out or left half-applied — it runs
// on exactly the runs the positive arms run on, over the same `kAsmRules` list.

TEST(LanguageReferences, WithoutTheReferenceEveryAsmRuleIsAbsent) {
    auto schema = loadOk(makeHostDoc(HostVariant::SelfContained));
    ASSERT_NE(schema, nullptr);

    for (auto const& asmRule : kAsmRules) {
        EXPECT_FALSE(schema->rules().contains(asmRule))
            << "rule '" << asmRule << "' is present in a host that declares "
               "NO languageReferences — the merge is not gated on the block";
    }
    // The behavioural mirror of `HostSpelledAsmBarrierWalksTheMergedRuleToCompletion`:
    // there is no rule to enter, so the barrier cannot be walked at all.
    EXPECT_FALSE(schema->enterRule(schema->rules().find("asmStmt")).valid())
        << "asmStmt is enterable in a host that references nothing";

    // ...and the host's own rules are still there, so this is a real language
    // that simply lacks asm, not a schema that failed to build anything.
    EXPECT_TRUE(schema->rules().contains("hostStmt"));
    EXPECT_TRUE(schema->rules().contains("hostValue"));
    EXPECT_TRUE(schema->rules().contains("hostPayload"));
}

TEST(LanguageReferences, WithoutTheReferenceNoCompanionRowArrives) {
    // ★★ POSITIVE ANCHOR FIRST — THE SUBJECT MUST EXIST BEFORE ITS ABSENCE
    // MEANS ANYTHING. The four assertions below are every one of them satisfied
    // by a loader in which the companion rows never materialise for ANY host, so
    // on their own they would stay green through exactly the regression they
    // exist to catch. Loading the REFERENCING arm first and asserting all three
    // rows ARE there turns this into a differential over one variable — the
    // presence of the `languageReferences` block — which is what the rest of
    // this section is built to be. (Same correction as
    // `AsmRoleNamesAreSubstitutedAwayNotLeaked`'s anchor line above.)
    auto withRef = loadOk(makeHostDoc(HostVariant::WithReference));
    ASSERT_NE(withRef, nullptr);
    ASSERT_TRUE(withRef->semantics().inlineAsm.rule.valid())
        << "nothing was inherited — the absence checks below would be vacuous";
    ASSERT_EQ(withRef->semantics().inlineAsm.ruleName, "asmStmt");
    ASSERT_EQ(withRef->hirLowering().ruleMappings.size(), 1u);
    ASSERT_EQ(withRef->pipelineEntry().byRule.size(), 1u);

    auto schema = loadOk(makeHostDoc(HostVariant::SelfContained));
    ASSERT_NE(schema, nullptr);

    EXPECT_FALSE(schema->semantics().inlineAsm.rule.valid())
        << "semantics.inlineAsm materialised in a host that references nothing";
    EXPECT_TRUE(schema->semantics().inlineAsm.ruleName.empty());
    EXPECT_TRUE(schema->hirLowering().ruleMappings.empty())
        << "a hirLowering row materialised in a host that references nothing";
    EXPECT_TRUE(schema->pipelineEntry().byRule.empty())
        << "a pipelineEntry row materialised in a host that references nothing";
}

TEST(LanguageReferences, ReachingAsmWithoutTheReferenceFailsLoud) {
    // The other half of red-on-disable: the reference is the SOLE provider.
    // Keep the `asmStmt` atom and drop only the block, and the load must fail
    // by NAME — never resolve `asmStmt` from somewhere else, and never degrade
    // to "that alt arm just never matches", which is the silent disable this
    // whole mechanism is built to refuse.
    auto result = GrammarSchema::loadFromText(
        makeHostDoc(HostVariant::ReachesAsmWithoutRef), "<synthetic-asm-host>");
    ASSERT_FALSE(result.has_value())
        << "a host that names 'asmStmt' with NO languageReferences block "
           "loaded CLEAN — asm's grammar reached it from somewhere other than "
           "the reference, or the dangling atom was silently ignored";

    // ✔MEASURED 2026-08-12 on this build: the loader's message is exactly
    // `unknown reference 'asmStmt'`. The assertion below pins the load-bearing
    // half — that the diagnostic NAMES the rule — rather than the full prose,
    // deliberately: the NAME is what makes the failure actionable, and pinning
    // wording would go red on an improvement to it. The measurement is recorded
    // here so the two halves of the fact stay together.
    const bool namesAsmStmt = std::ranges::any_of(
        result.error(), [](ConfigDiagnostic const& d) {
            return d.message.find("asmStmt") != std::string::npos
                || d.path.find("asmStmt") != std::string::npos;
        });
    EXPECT_TRUE(namesAsmStmt)
        << "the load failed but no diagnostic names 'asmStmt': "
        << firstError(result.error());
}

// ─── 4. the host is genuinely not C-shaped ────────────────────────────────

TEST(LanguageReferences, AsmHostProbeIsNotCShaped) {
    // Guards against the whole suite passing merely because the synthetic host
    // happened to reuse `c-subset`'s vocabulary — in which case it would be
    // testing one consumer twice rather than the substrate once.
    auto schema = loadOk(makeHostDoc(HostVariant::WithReference));
    ASSERT_NE(schema, nullptr);
    EXPECT_EQ(schema->name(), "AsmHostProbe");

    // ⚠ THE CLAIM IS A CONJUNCTION AND BOTH HALVES BELONG IN ONE TEST: this
    // host has NONE of c-subset's vocabulary AND has all twelve asm rules
    // anyway. Asserting only the first half leaves the test green when the
    // merge stops entirely — MEASURED: pointed at the reference-less host it
    // was one of two arms that did not go red.
    for (auto const& asmRule : kAsmRules) {
        ASSERT_TRUE(schema->rules().contains(asmRule))
            << "a non-C-shaped host did NOT inherit '" << asmRule << "'";
    }

    // c-subset's bindings for TWELVE of the thirteen token roles: none of them
    // exist here, yet all twelve asm rules do. The thirteenth binding —
    // `symbolName` -> `Identifier` — is deliberately NOT asserted absent: a
    // spelling that generic proves nothing about independence, since a host may
    // legitimately own a token called `Identifier` for its own reasons.
    // (Count stated explicitly because it does NOT match `kAsmTokenHoles.size()`;
    // an earlier revision of this comment claimed "thirteen" over this 12-entry
    // list — the [[D-CONFIG-COMMENT-CLAIM-ROT]] shape, in test code.)
    for (auto const& cKind : {"AsmKeyword", "VolatileKeyword", "InlineKeyword",
                              "GotoKeyword", "Colon", "ColonColonOp", "Comma",
                              "ParenOpen", "ParenClose", "BracketOpen",
                              "BracketClose", "EndStatement"}) {
        EXPECT_FALSE(schema->schemaTokens().contains(cKind))
            << "the synthetic host declares c-subset's token kind '" << cKind
            << "' — it is no longer an independent second consumer";
    }
    // c-subset's TWO rule-role bindings (`expression` <- operandExpr,
    // `stringLiteralExpr` <- templateText), plus two further c-subset rules that
    // are NOT role bindings at all (`statement`, `assignmentExpr`) — four names,
    // asserted absent for the same independence reason. (Spelled out because the
    // count differs from `kAsmRuleHoles.size()`; an earlier revision said "the two
    // rule roles" over this 4-entry list.)
    for (auto const& cRule : {"expression", "stringLiteralExpr", "statement",
                              "assignmentExpr"}) {
        EXPECT_FALSE(schema->rules().contains(cRule))
            << "the synthetic host declares c-subset's rule '" << cRule << "'";
    }
    // The host's own alien vocabulary IS there.
    for (auto const& hostKind : {"EmitWord", "LoudWord", "TightWord",
                                 "LeapWord", "PipeMark", "PipePipeMark",
                                 "AmpMark", "AngleOpen", "AngleClose",
                                 "BraceOpen", "BraceClose", "BangEnd",
                                 "TickText"}) {
        EXPECT_TRUE(schema->schemaTokens().contains(hostKind)) << hostKind;
    }
}

// ═══ 5. THE REFUSAL BATTERY — every guard, asserted BY CODE ═══════════════
//
// ★★★ WHY THIS SECTION EXISTS, AS A MEASUREMENT AND NOT AN OPINION. Before it,
// DELETING ANY ONE of the mechanism's refusal branches left the whole ctest
// suite GREEN. That includes `validateInlineAsmGate` clause (c) — the branch
// whose own source comment boasts that twelve semantic inline-asm pins stay
// green without it, i.e. the cycle's own safety net was itself unguarded.
// Sections 1–4 above prove the mechanism WORKS; a mechanism that silently
// stops REFUSING is the failure mode this project spends its diagnostics on,
// and it had no witness at all.
//
// ★★ EVERY ARM ASSERTS THE EXACT `DiagnosticCode` PLUS A SUBJECT NAMED IN THE
// DIAGNOSTIC — never merely "the load failed". "Failed" is satisfied by ANY
// breakage in the mutated document, including a typo in this test file's own
// JSON, so a load-failed assertion would prove nothing about the branch it is
// pointed at. Naming the subject is also what keeps the diagnostic ACTIONABLE:
// a refusal that does not say WHICH hole, WHICH block or WHICH document sends
// the reader to the wrong file.
//
// ★ THE MUTATIONS ARE JSON-LEVEL EDITS OF THE SAME DOCUMENTS SECTIONS 1–4
// LOAD, and each battery has a PREMISE test pinning that its UNMUTATED base
// loads CLEAN. Without that premise every arm below would be satisfiable by a
// base document that was broken all along — the same "absence is satisfied by
// nothing existing" defect this cycle corrected two sections up.

namespace {

[[nodiscard]] std::string describe(std::vector<ConfigDiagnostic> const& diags) {
    if (diags.empty()) return "\n    <no diagnostics>";
    std::string out;
    for (auto const& d : diags) {
        out += "\n    [";
        out += diagnosticCodeName(d.code);
        out += "] " + d.path + ": " + d.message;
    }
    return out;
}

// Load a document that MUST fail, returning the loader's own diagnostics. A
// clean load is reported here rather than left to the caller so every arm gets
// the same, unambiguous "the branch under test is gone" message.
[[nodiscard]] std::vector<ConfigDiagnostic> loadExpectingRefusal(
        nlohmann::json const& doc, char const* what) {
    auto result = GrammarSchema::loadFromText(doc.dump(2), "<refusal-probe>");
    if (result.has_value()) {
        ADD_FAILURE() << what << ": the mutated document LOADED CLEAN — the "
                         "refusal branch under test is gone, or the mutation "
                         "never reached it";
        return {};
    }
    return result.error();
}

// The exact-code + named-subject assertion. `needles` must ALL appear in ONE
// diagnostic (its message or its path): a code that happens to be emitted about
// something else does not satisfy the arm.
void expectRefusal(std::vector<ConfigDiagnostic> const& diags,
                   DiagnosticCode code,
                   std::vector<std::string_view> const& needles,
                   char const* what) {
    const bool found = std::ranges::any_of(
        diags, [&](ConfigDiagnostic const& d) {
            if (d.code != code) return false;
            return std::ranges::all_of(needles, [&](std::string_view n) {
                return d.message.find(n) != std::string::npos
                    || d.path.find(n) != std::string::npos;
            });
        });
    std::string wanted;
    for (auto const& n : needles) {
        if (!wanted.empty()) wanted += "' + '";
        wanted += n;
    }
    EXPECT_TRUE(found)
        << what << ": expected one " << diagnosticCodeName(code)
        << " naming '" << wanted << "'. Got:" << describe(diags);
}

// ── battery A: the HOST side, mutating the very document section 1 loads ──

[[nodiscard]] nlohmann::json referencingHostJson() {
    return nlohmann::json::parse(makeHostDoc(HostVariant::WithReference));
}

// ── battery B: the REFERENCED side ───────────────────────────────────────
//
// ★ WHY A REAL FILE AND NOT A STRING. `mergeLanguageReferences` resolves a
// referenced document through `findShippedConfig` — off the filesystem, by
// logical name. There is no text entry point for the referenced side, and
// inventing one for the test would exercise a path production never takes. The
// referenced-document refusals (a `root` shape, a transitive
// `languageReferences`, a block the merge does not consume, a cross-document
// duplicate shape, a malformed `requires`) are reachable ONLY through a file
// the resolver can actually find.
//
// ★★ IT MOVES THE CWD, NOT THE ENVIRONMENT, AND THAT IS A DELIBERATE CHOICE.
// `findShippedConfig` consults `$DSS_CONFIG_ROOT` FIRST and falls THROUGH to an
// 8-ancestor cwd walk on a miss. Entering this root therefore ADDS `probe`
// without REMOVING anything: `asm.lang.json` still resolves through whatever
// `$DSS_CONFIG_ROOT` ctest exported (or through the walk, in-tree), so every
// other arm in this file is untouched by construction. Overriding the
// environment instead would have taken `asm` away AND written the environment,
// which `config_path_walk.cpp` documents as a READ-only lookup whose
// race-freedom the project intends to keep. The cwd is restored in the
// destructor — before `remove_all`, because Windows refuses to delete the
// current directory — so it survives a gtest `ASSERT_` early return.
class ProbeConfigRoot {
public:
    explicit ProbeConfigRoot(nlohmann::json const& referencedDoc) {
        namespace fs = std::filesystem;
        static int counter = 0;
        std::error_code ec;
        root_ = fs::temp_directory_path()
              / ("dss-langref-probe-" + std::to_string(++counter));
        fs::remove_all(root_, ec);              // a crashed earlier run
        const fs::path sources = root_ / "src" / "dss-config" / "sources";
        fs::create_directories(sources, ec);
        if (ec) {
            ADD_FAILURE() << "could not create the probe config root: "
                          << ec.message();
            return;
        }
        {
            std::ofstream out(sources / "probe.lang.json", std::ios::binary);
            out << referencedDoc.dump(2);
        }
        previous_ = fs::current_path(ec);
        fs::current_path(root_, ec);
        if (ec) ADD_FAILURE() << "could not enter the probe config root: "
                              << ec.message();
    }
    ~ProbeConfigRoot() {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!previous_.empty()) fs::current_path(previous_, ec);
        fs::remove_all(root_, ec);
    }
    ProbeConfigRoot(ProbeConfigRoot const&)            = delete;
    ProbeConfigRoot& operator=(ProbeConfigRoot const&) = delete;

private:
    std::filesystem::path root_;
    std::filesystem::path previous_;
};

// The synthetic REFERENCED document: two holes, one shape, nothing else. Kept
// minimal on purpose — every battery-B arm is this document plus ONE key, so
// the diagnostic can only be about the key that was added.
[[nodiscard]] nlohmann::json probeReferencedDoc() {
    return nlohmann::json::parse(R"({
      "dssSchemaVersion": 4,
      "language": { "name": "probe", "version": "0.0.1" },
      "requires": { "rules": ["probeValue"], "tokens": ["probeMark"] },
      "shapes": {
        "probeStmt": { "sequence": ["probeMark", "probeValue"] }
      }
    })");
}

// A host that reaches `probe`. Its own vocabulary is the same alien one the asm
// host uses, so the two batteries share a lexical surface and differ only in
// which document they reach into.
[[nodiscard]] nlohmann::json probeHostJson() {
    std::string doc = R"({
      "dssSchemaVersion": 4,
      "language": { "name": "RefHostProbe", "version": "0.0.1" },
      "languageReferences": {
        "probe": {
          "entry": "probeStmt",
          "bindRules":  { "probeValue": "hostValue" },
          "bindTokens": { "probeMark":  "BraceOpen" }
        }
      },
    )";
    doc += kTokensAndKeywords;
    doc += R"(
      "shapes": {
        "root":          { "sequence": [{ "repeat": "hostStmt" }] },
        "hostStmt":      { "alt": ["probeStmt", "hostPlainStmt"] },
        "hostPlainStmt": { "sequence": ["hostValue", "BangEnd"] },
        "hostValue":     { "sequence": ["Identifier"] }
      }
    })";
    return nlohmann::json::parse(doc);
}

// ── battery C: `validateInlineAsmGate` clause (c) ─────────────────────────
//
// A SELF-CONTAINED host declaring its OWN `semantics.inlineAsm` facet. That is
// the only way to reach clause (c) from a test: a host that REFERENCES asm
// inherits the facet and the lowering row TOGETHER and cannot separate them —
// writing either one itself collides with the inherited copy first, by design.
// `rowHirKind` is the single variable: absent ⇒ no `hirLowering` block at all,
// engaged ⇒ one row for the gated rule with that HIR kind.
[[nodiscard]] nlohmann::json gateHostJson(std::optional<std::string> rowHirKind) {
    nlohmann::json doc = nlohmann::json::parse(R"({
      "dssSchemaVersion": 4,
      "language": { "name": "InlineAsmGateProbe", "version": "0.0.1" },
      "tokens": {
        " ": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
        "!": [{ "kind": "BangEnd" }],
        "`": [{ "kind": "TickText" }]
      },
      "keywords": [ { "word": "LEAP", "kind": "LeapWord" } ],
      "shapes": {
        "root":        { "sequence": [{ "repeat": "gateStmt" }] },
        "gateStmt":    { "sequence": ["gatePayload", "BangEnd"] },
        "gatePayload": { "sequence": ["TickText"] }
      },
      "semantics": {
        "inlineAsm": {
          "rule": "gateStmt",
          "templateRule": "gatePayload",
          "outputsTailRule": "gatePayload",
          "inputsTailRule": "gatePayload",
          "inputsTailFusedRule": "gatePayload",
          "clobbersTailRule": "gatePayload",
          "clobbersTailFusedRule": "gatePayload",
          "labelsTailRule": "gatePayload",
          "labelsTailFusedRule": "gatePayload",
          "operandListRule": "gatePayload",
          "operandRule": "gatePayload",
          "clobberListRule": "gatePayload",
          "gotoLabelListRule": "gatePayload",
          "gotoQualifierToken": "LeapWord",
          "memoryClobber": "memory",
          "conditionCodeClobber": "cc"
        }
      }
    })");
    if (rowHirKind.has_value()) {
        nlohmann::json row = nlohmann::json::object();
        row["rule"]    = "gateStmt";
        row["hirKind"] = *rowHirKind;
        nlohmann::json rows = nlohmann::json::array();
        rows.push_back(std::move(row));
        doc["hirLowering"]["ruleMappings"] = std::move(rows);
    }
    return doc;
}

} // namespace

// ─── 5.0 the premises: each battery's UNMUTATED base loads CLEAN ──────────

TEST(LanguageReferenceRefusals, HostBatteryBaseLoadsClean) {
    auto result = GrammarSchema::loadFromText(referencingHostJson().dump(2),
                                              "<refusal-probe>");
    ASSERT_TRUE(result.has_value())
        << "the unmutated host does not load, so every battery-A arm below "
           "would be green for the wrong reason: " << describe(result.error());
    EXPECT_TRUE((*result)->rules().contains("asmStmt"));
}

TEST(LanguageReferenceRefusals, ProbeBatteryBaseLoadsClean) {
    ProbeConfigRoot root{probeReferencedDoc()};
    auto result = GrammarSchema::loadFromText(probeHostJson().dump(2),
                                              "<refusal-probe>");
    ASSERT_TRUE(result.has_value())
        << "the synthetic referenced document does not load through the probe "
           "host, so every battery-B arm below would be green for the wrong "
           "reason: " << describe(result.error());
    // The positive anchor: the referenced rule really did merge, so a later
    // arm's REFUSAL is a refusal of something that would otherwise have worked.
    EXPECT_TRUE((*result)->rules().contains("probeStmt"));
    EXPECT_FALSE((*result)->rules().contains("probeValue"))
        << "the rule hole leaked unsubstituted";
}

TEST(LanguageReferenceRefusals, InlineAsmGateBaseLoadsClean) {
    auto result = GrammarSchema::loadFromText(
        gateHostJson(std::string{"InlineAsm"}).dump(2), "<refusal-probe>");
    ASSERT_TRUE(result.has_value())
        << "a facet WITH its matching 'InlineAsm' row must load — otherwise the "
           "two clause-(c) arms below prove nothing about clause (c): "
        << describe(result.error());
    EXPECT_TRUE((*result)->semantics().inlineAsm.rule.valid());
    EXPECT_EQ((*result)->hirLowering().ruleMappings.size(), 1u);
}

// ─── 5.1 battery A: the host's own `languageReferences` block ─────────────

TEST(LanguageReferenceRefusals, UnboundHoleFailsLoud) {
    // HARD REQUIREMENT 1: an unbound hole may never degrade into "that rule
    // just never matches", which is a silent disable of a whole construct.
    auto doc = referencingHostJson();
    doc["languageReferences"]["asm"]["bindTokens"].erase("statementEnd");
    expectRefusal(loadExpectingRefusal(doc, "unbound hole"),
                  DiagnosticCode::C_MissingField, {"statementEnd", "UNBOUND"},
                  "unbound token hole");
}

TEST(LanguageReferenceRefusals, UnboundRuleHoleFailsLoud) {
    auto doc = referencingHostJson();
    doc["languageReferences"]["asm"]["bindRules"].erase("templateText");
    expectRefusal(loadExpectingRefusal(doc, "unbound rule hole"),
                  DiagnosticCode::C_MissingField, {"templateText", "UNBOUND"},
                  "unbound rule hole");
}

TEST(LanguageReferenceRefusals, BindingAHoleTheDocumentDoesNotDeclareFailsLoud) {
    // A binding with no hole silently does nothing — the reader believes they
    // rebound something and did not.
    auto doc = referencingHostJson();
    doc["languageReferences"]["asm"]["bindTokens"]["notAHole"] = "Identifier";
    expectRefusal(loadExpectingRefusal(doc, "binding an undeclared hole"),
                  DiagnosticCode::C_UnknownShape, {"notAHole"},
                  "binding an undeclared hole");
}

TEST(LanguageReferenceRefusals, BindingARuleHoleUnderBindTokensFailsLoud) {
    // Not a near miss: binding a RULE hole under `bindTokens` would rewrite a
    // rule reference to a token kind and the grammar would compile to a
    // different shape. The message must say which side it belongs on.
    auto doc = referencingHostJson();
    doc["languageReferences"]["asm"]["bindRules"].erase("operandExpr");
    doc["languageReferences"]["asm"]["bindTokens"]["operandExpr"] = "Identifier";
    expectRefusal(loadExpectingRefusal(doc, "rule hole bound as a token"),
                  DiagnosticCode::C_ConflictingField,
                  {"operandExpr", "bindRules"}, "rule hole bound as a token");
}

TEST(LanguageReferenceRefusals, BindingATokenHoleUnderBindRulesFailsLoud) {
    auto doc = referencingHostJson();
    doc["languageReferences"]["asm"]["bindTokens"].erase("asmKeyword");
    doc["languageReferences"]["asm"]["bindRules"]["asmKeyword"] = "hostValue";
    expectRefusal(loadExpectingRefusal(doc, "token hole bound as a rule"),
                  DiagnosticCode::C_ConflictingField,
                  {"asmKeyword", "bindTokens"}, "token hole bound as a rule");
}

TEST(LanguageReferenceRefusals, BoundRuleThatDoesNotExistFailsLoud) {
    auto doc = referencingHostJson();
    doc["languageReferences"]["asm"]["bindRules"]["operandExpr"] =
        "noSuchHostRule";
    expectRefusal(loadExpectingRefusal(doc, "hole bound to a missing rule"),
                  DiagnosticCode::C_UnknownShape,
                  {"operandExpr", "noSuchHostRule"},
                  "hole bound to a rule the merged grammar does not have");
}

TEST(LanguageReferenceRefusals, BoundTokenThatDoesNotExistFailsLoud) {
    auto doc = referencingHostJson();
    doc["languageReferences"]["asm"]["bindTokens"]["asmKeyword"] = "NoSuchKind";
    expectRefusal(loadExpectingRefusal(doc, "hole bound to a missing token"),
                  DiagnosticCode::C_UnknownToken, {"asmKeyword", "NoSuchKind"},
                  "hole bound to a token kind this language does not declare");
}

TEST(LanguageReferenceRefusals, EntryThatIsNotAShapeOfTheReferencedDocFailsLoud) {
    auto doc = referencingHostJson();
    doc["languageReferences"]["asm"]["entry"] = "asmNotAShape";
    expectRefusal(loadExpectingRefusal(doc, "entry names no shape"),
                  DiagnosticCode::C_UnknownShape, {"asmNotAShape"},
                  "entry naming a shape the referenced document does not declare");
}

TEST(LanguageReferenceRefusals, EntryNeverReachedByTheHostFailsLoud) {
    // ★ THE SILENT FAILURE THE WHOLE BLOCK EXISTS TO PREVENT: every rule merges,
    // every binding validates, the load is clean — and the construct is still a
    // parse error, because no host rule ever descends into it. `asmOperand` IS a
    // real shape of `asm.lang.json`, so this is the reachability check firing and
    // not the unknown-shape one above.
    auto doc = referencingHostJson();
    doc["languageReferences"]["asm"]["entry"] = "asmOperand";
    expectRefusal(loadExpectingRefusal(doc, "entry never reached"),
                  DiagnosticCode::C_UnknownShape,
                  {"asmOperand", "references the entry rule"},
                  "entry the host never descends into");
}

TEST(LanguageReferenceRefusals, UnknownKeyInALanguageReferencesEntryFailsLoud) {
    // The typo discriminator: `bindtokens` would otherwise leave all thirteen
    // token holes unbound, and the reader would be reading a table the loader
    // never saw.
    auto doc = referencingHostJson();
    doc["languageReferences"]["asm"]["bindtokens"] = nlohmann::json::object();
    expectRefusal(loadExpectingRefusal(doc, "unknown reference key"),
                  DiagnosticCode::C_MalformedJson, {"bindtokens"},
                  "unknown key in a 'languageReferences' entry");
}

TEST(LanguageReferenceRefusals, ShapeDeclaredByBothDocumentsFailsLoud) {
    // Last-wins in silence was the pre-guard behaviour: the host's rule would be
    // replaced wholesale and the language would parse something nobody wrote.
    // The diagnostic must name BOTH ends — you cannot fix a collision you can
    // only see one side of.
    auto doc = referencingHostJson();
    doc["shapes"]["asmOperand"] =
        nlohmann::json::parse(R"({ "sequence": ["Identifier"] })");
    expectRefusal(loadExpectingRefusal(doc, "duplicate shape"),
                  DiagnosticCode::C_ConflictingField,
                  {"asmOperand", "asm.lang.json"}, "duplicate shape across documents");
}

// ─── 5.2 battery B: what a REFERENCED document may declare ────────────────

TEST(LanguageReferenceRefusals, ReferencedDocumentDeclaringRootFailsLoud) {
    // `root` is interned by NAME and becomes THE language's entry point, so a
    // fragment's root would silently become the root of a host that has none.
    auto refDoc = probeReferencedDoc();
    refDoc["shapes"]["root"] =
        nlohmann::json::parse(R"({ "sequence": ["probeStmt"] })");
    ProbeConfigRoot root{refDoc};
    expectRefusal(loadExpectingRefusal(probeHostJson(), "referenced root"),
                  DiagnosticCode::C_ConflictingField,
                  {"probe.lang.json", "root"},
                  "a referenced document declaring 'root'");
}

TEST(LanguageReferenceRefusals, TransitiveLanguageReferenceFailsLoud) {
    // ONE LEVEL, ENFORCED, NOT ASSUMED — a transitive chain needs bottom-up hole
    // composition and a cycle guard that nothing in the tree can exercise yet,
    // and a silent ignore would be worse than the refusal.
    auto refDoc = probeReferencedDoc();
    refDoc["languageReferences"] = nlohmann::json::parse(R"({
      "asm": { "entry": "asmStmt", "bindRules": {}, "bindTokens": {} }
    })");
    ProbeConfigRoot root{refDoc};
    expectRefusal(loadExpectingRefusal(probeHostJson(), "transitive reference"),
                  DiagnosticCode::C_ConflictingField,
                  {"probe.lang.json", "transitive"},
                  "a referenced document declaring its own 'languageReferences'");
}

TEST(LanguageReferenceRefusals, ReferencedDocumentDeclaringANonMergedBlockFailsLoud) {
    // ★ THE SILENTLY-DISCARDED BLOCK. `checkDocumentKeys` validates a referenced
    // document against the full 21-key document vocabulary, but the merge folds
    // in exactly four blocks. A correctly-spelled block outside that set
    // therefore used to pass the typo discriminator, load clean, and VANISH.
    //
    // ⚠ THE SUBJECT USED TO BE `tokens`, AND P2.5 (2026-08-12) MOVED IT — read
    // this before "restoring" it. `tokens` is now a STANDALONE-IDENTITY block:
    // `asm.lang.json` declares its own lexical surface so a `.s` can be
    // compiled directly, and that block is consumed by the standalone load
    // path, so for it "not merged" no longer means "does nothing". The refusal
    // itself is unchanged and still has teeth — it is demonstrated here on
    // `operators`, which genuinely has no consumer on either side. The
    // companion test below pins the OTHER polarity, so narrowing the guard
    // again cannot pass silently.
    auto refDoc = probeReferencedDoc();
    refDoc["operators"] = nlohmann::json::parse(
        R"({ "binary": [{ "token": "ProbeAtMark", "precedence": 1 }] })");
    ProbeConfigRoot root{refDoc};
    auto const diags = loadExpectingRefusal(probeHostJson(), "non-merged block");
    expectRefusal(diags, DiagnosticCode::C_ConflictingField,
                  {"probe.lang.json", "operators", "SILENTLY DISCARDED"},
                  "a referenced document declaring a block the merge drops");
    // ...and the block really would have been dropped: nothing it declared
    // reached the merged schema. (Asserted through the refusal's own subject
    // rather than by loading it, because it cannot be loaded.)
    EXPECT_TRUE(std::ranges::none_of(diags, [](ConfigDiagnostic const& d) {
        return d.message.find("ProbeAtMark") != std::string::npos;
    })) << "the token kind was consumed somewhere after all — re-check whether "
           "the merge grew a consumer for 'operators'";
}

TEST(LanguageReferenceRefusals, ReferencedDocumentStandaloneSurfaceBlockIsAllowed) {
    // ★★ THE OTHER POLARITY OF THE NARROWING ABOVE, AND THE REASON IT IS SAFE.
    // A referenced document may declare the blocks that describe how IT reads a
    // file of its own — `tokens` here. That is not an unconsumed block: the
    // standalone load path consumes it, which is the whole point of
    // `asm.lang.json` being compilable as a `.s` language rather than only
    // embeddable. A host that references the document reads its input with its
    // OWN tokenizer (that is what `bindTokens` is for), so the block is
    // deliberately not merged rather than silently lost.
    //
    // ⚠ WITHOUT THIS TEST the previous test alone would let someone "fix" a
    // future diagnostic by appending any key to `kStandaloneOnlyDocumentKeys`,
    // and the guard would erode one key at a time with every test still green —
    // the weakened-guard failure this project has already been bitten by twice.
    // Here the allowance is asserted as INTENDED behaviour, so the two tests
    // together say exactly which keys may pass and which may not.
    auto refDoc = probeReferencedDoc();
    refDoc["tokens"] = nlohmann::json::parse(
        R"({ "@": [{ "kind": "ProbeAtMark" }] })");
    ProbeConfigRoot root{refDoc};
    auto result = GrammarSchema::loadFromText(probeHostJson().dump(2),
                                              "<refusal-probe>");
    ASSERT_TRUE(result.has_value())
        << "a referenced document's standalone-surface 'tokens' block must LOAD "
           "(deliberately not merged, not an error): " << describe(result.error());
    // ...and it really was NOT merged: the host's token table is untouched, so
    // the referenced document's own lexical surface cannot contest the host's.
    EXPECT_FALSE((*result)->schemaTokens().contains("ProbeAtMark"))
        << "the referenced document's token kind reached the HOST's token "
           "table — a referenced tokenizer must never silently join the host's, "
           "or two token tables contest one input";
}

TEST(LanguageReferenceRefusals, ReferencedDocumentDeclaringAConsumedBlockStillLoads) {
    // The other side of the same coin, and the reason the check above is a
    // NARROWER key table rather than a blanket ban: the four blocks the merge
    // DOES consume must keep working. `pipelineEntry` is the one added last and
    // the one a blanket ban would have broken first.
    auto refDoc = probeReferencedDoc();
    refDoc["pipelineEntry"] = nlohmann::json::parse(R"({
      "byRule": [ { "rule": "probeStmt", "tier": "hir" } ]
    })");
    ProbeConfigRoot root{refDoc};
    auto result = GrammarSchema::loadFromText(probeHostJson().dump(2),
                                              "<refusal-probe>");
    ASSERT_TRUE(result.has_value())
        << "a referenced 'pipelineEntry' block is CONSUMED and must still load: "
        << describe(result.error());
    ASSERT_EQ((*result)->pipelineEntry().byRule.size(), 1u);
    auto const tier = (*result)->pipelineEntry().tierForRule(
        (*result)->rules().find("probeStmt"));
    ASSERT_TRUE(tier.has_value());
    EXPECT_EQ(*tier, PipelineTier::Hir);
}

TEST(LanguageReferenceRefusals, ShapeDeclaredByBothTheHostAndTheReferencedDocFailsLoud) {
    // ★★ THE COLLIDING SHAPE MUST BE INSIDE THE ENTRY'S REACHABLE CLOSURE, and
    // this arm used to get that WRONG in a way only a later change exposed.
    // It added `hostValue` — the host rule the `probeValue` HOLE is bound to —
    // as a shape of the referenced document, and the merge duly refused it. But
    // the reason it was "reachable" was an artifact: the closure walk ran AFTER
    // hole substitution, so `probeStmt`'s body had become `["probeMark",
    // "hostValue"]` and the walk followed a name the referenced document never
    // wrote. ✔MEASURED 2026-08-12: moving the walk to the UNSUBSTITUTED bodies
    // (which is what "the document's own references" means) made this arm load
    // CLEAN — correctly, because an unreachable shape does not merge and so
    // cannot collide.
    // ⇒ Re-aimed at a collision that is real: `probeTail` is spelled by
    // `probeStmt` (so the closure imports it) AND declared by the host. That is
    // strictly stronger than the old form — it pins the guard for a shape the
    // reference actually contributes, which is the only case where last-wins
    // could have silently replaced a host rule.
    auto refDoc = probeReferencedDoc();
    refDoc["shapes"]["probeStmt"] =
        nlohmann::json::parse(R"({ "sequence": ["probeMark", "probeValue", "probeTail"] })");
    refDoc["shapes"]["probeTail"] =
        nlohmann::json::parse(R"({ "sequence": ["probeMark"] })");
    auto hostDoc = probeHostJson();
    hostDoc["shapes"]["probeTail"] =
        nlohmann::json::parse(R"({ "sequence": ["BraceOpen"] })");
    ProbeConfigRoot root{refDoc};
    expectRefusal(loadExpectingRefusal(hostDoc, "duplicate shape"),
                  DiagnosticCode::C_ConflictingField,
                  {"probeTail", "probe.lang.json"},
                  "a shape declared by both documents");
}

TEST(LanguageReferenceRefusals, UnknownKeyInRequiresFailsLoud) {
    // `ruls` would leave the rule holes undeclared, so every binding for one
    // would then be reported as binding a hole that does not exist — a page of
    // consequences pointing away from the typo.
    auto refDoc = probeReferencedDoc();
    refDoc["requires"]["ruls"] = nlohmann::json::array();
    ProbeConfigRoot root{refDoc};
    expectRefusal(loadExpectingRefusal(probeHostJson(), "unknown requires key"),
                  DiagnosticCode::C_MalformedJson, {"ruls"},
                  "unknown key in 'requires'");
}

TEST(LanguageReferenceRefusals, UnknownKeyInPipelineEntryFailsLoud) {
    // A typo'd `byRule` must be a load error and never "no overrides": the
    // silent default routes hand-written assembly through MIR and the OPTIMIZER.
    auto refDoc = probeReferencedDoc();
    refDoc["pipelineEntry"] = nlohmann::json::parse(R"({
      "byRules": [ { "rule": "probeStmt", "tier": "hir" } ]
    })");
    ProbeConfigRoot root{refDoc};
    expectRefusal(loadExpectingRefusal(probeHostJson(), "unknown pipelineEntry key"),
                  DiagnosticCode::C_InvalidHirLowering, {"byRules"},
                  "unknown key in 'pipelineEntry'");
}

// ─── 5.3 battery C: `validateInlineAsmGate` clause (c) ────────────────────
//
// ★★★ THE BRANCH WHOSE OWN COMMENT SAYS TWELVE PINS STAY GREEN WITHOUT IT.
// Clause (c) walks from `semantics.inlineAsm.rule` BACK to `hirLowering`: a
// gated construct must reach the lowering its gate assumes. Both arms below
// were, until this test, deletable with ctest still green.

TEST(LanguageReferenceRefusals, InlineAsmFacetWithNoLoweringRowFailsLoud) {
    // The whole semantic refusal ladder would still run and every
    // diagnostic-tier witness would stay green while the construct lowered to
    // NOTHING — the compiler barrier silently dropped.
    expectRefusal(loadExpectingRefusal(gateHostJson(std::nullopt),
                                       "facet with no lowering row"),
                  DiagnosticCode::C_MissingField,
                  {"gateStmt", "hirLowering"},
                  "an inline-asm facet with NO hirLowering row");
}

TEST(LanguageReferenceRefusals, InlineAsmFacetWithASkipLoweringRowFailsLoud) {
    // ★ THIS IS THE EXACT CONFIGURATION THE OLD RED-ON-DISABLE RECIPE IN
    // `tests/mir/test_mir_lowering_c_subset.cpp` PRESCRIBED ("flip the asmStmt
    // row to Skip; the schema still loads"). Clause (c) closed that path, which
    // is why the recipe there had to be rewritten — and this arm is the standing
    // record of WHY, so the two facts cannot drift apart.
    expectRefusal(loadExpectingRefusal(gateHostJson(std::string{"Skip"}),
                                       "facet gated onto a Skip row"),
                  DiagnosticCode::C_InvalidHirLowering,
                  {"gateStmt", "Skip", "InlineAsm"},
                  "an inline-asm facet whose rule lowers to something else");
}

// ─── 5.4 battery D: the P5 wave-2 facet rows, red-on-disable ──────────────
//
// ★★★ EVERY ONE OF THESE ARMS IS A REFUSAL THIS CYCLE ADDED, EXERCISED RATHER
// THAN READ. The ALL-OR-NOTHING claim on `semantics.inlineAsm` is only worth
// the diagnostics that enforce it, and three new keys means three new ways to
// half-configure the facet. Each arm below mutates the SAME base that
// `InlineAsmGateBaseLoadsClean` pins as loading clean, so a green arm can never
// mean "the base was broken all along".
//
// ★★ FAIL-CLOSED BY CONSTRUCTION, exactly like every other battery here: the
// mutant is a SECOND JSON VALUE built in this process and handed to
// `loadFromText`. No file is written, nothing must be restored, and there is no
// build flag or environment variable that could skip an arm. `loadExpectingRefusal`
// itself ADD_FAILUREs on a clean load, so "the mutation never reached the check"
// is reported as a failure rather than passing quietly.

namespace {
// The clean base, with ONE key mutated. Returning the whole document keeps each
// arm a true differential over a single key.
[[nodiscard]] nlohmann::json gateHostWithoutKey(char const* key) {
    auto doc = gateHostJson(std::string{"InlineAsm"});
    doc["semantics"]["inlineAsm"].erase(key);
    return doc;
}
[[nodiscard]] nlohmann::json gateHostWithKey(char const* key,
                                             nlohmann::json value) {
    auto doc = gateHostJson(std::string{"InlineAsm"});
    doc["semantics"]["inlineAsm"][key] = std::move(value);
    return doc;
}
} // namespace

TEST(LanguageReferenceRefusals, InlineAsmFacetMissingOperandRuleFailsLoud) {
    // Omit it and a walker has no node to key an operand on, so it falls back to
    // counting separator commas — which the comma OPERATOR inside an operand's
    // parens (`"r"(a, b)`, accepted by gcc 13.3.0 AND clang 18.1.3) makes
    // ambiguous. The wrong operand would bind to `%0`, silently.
    expectRefusal(loadExpectingRefusal(gateHostWithoutKey("operandRule"),
                                       "facet without operandRule"),
                  DiagnosticCode::C_MissingField,
                  {"operandRule", "ALL-OR-NOTHING"},
                  "an inline-asm facet missing 'operandRule'");
}

TEST(LanguageReferenceRefusals, InlineAsmFacetMissingClobberSpellingsFailsLoud) {
    // ⚠ BOTH DIRECTIONS, because a check that only fired for one key would let
    // the other be the silently-omitted one.
    for (char const* key : {"memoryClobber", "conditionCodeClobber"}) {
        expectRefusal(loadExpectingRefusal(gateHostWithoutKey(key),
                                           "facet without a clobber spelling"),
                      DiagnosticCode::C_MissingField,
                      {key, "ALL-OR-NOTHING", "VOCABULARY"},
                      "an inline-asm facet missing a clobber spelling");
    }
}

TEST(LanguageReferenceRefusals, InlineAsmFacetWithANonStringClobberSpellingFailsLoud) {
    // Present-but-wrong-typed must land on the SAME refusal as absent: a
    // `true`/`42`/`null` value would otherwise read as "declared" while
    // supplying nothing to compare against.
    for (auto value : {nlohmann::json(42), nlohmann::json(true),
                       nlohmann::json(nullptr),
                       nlohmann::json(nlohmann::json::array())}) {
        expectRefusal(loadExpectingRefusal(gateHostWithKey("memoryClobber", value),
                                           "non-string clobber spelling"),
                      DiagnosticCode::C_MissingField,
                      {"memoryClobber", "must be a string"},
                      "a non-string clobber spelling");
    }
}

TEST(LanguageReferenceRefusals, InlineAsmFacetWithAnEmptyClobberSpellingFailsLoud) {
    // ★ EMPTY IS A DIFFERENT DEFECT FROM ABSENT AND NEEDS ITS OWN ARM. `""`
    // satisfies "is a string" and then matches no clobber any source can write,
    // so the check it feeds is permanently, silently false — a disarmed guard
    // reached by a typo rather than by an omission. Both keys, both directions.
    for (char const* key : {"memoryClobber", "conditionCodeClobber"}) {
        expectRefusal(loadExpectingRefusal(gateHostWithKey(key, nlohmann::json("")),
                                           "empty clobber spelling"),
                      DiagnosticCode::C_InvalidSemantics,
                      {key, "EMPTY"},
                      "an EMPTY clobber spelling");
    }
}

TEST(LanguageReferenceRefusals, InlineAsmFacetWithTwoEqualClobberSpellingsFailsLoud) {
    // ★ THE COPY-PASTE ARM. Declared equal, one of the two checks can never
    // fire and nothing downstream could tell which — the silent
    // half-configuration this whole facet is all-or-nothing to prevent, reached
    // without omitting anything at all.
    expectRefusal(loadExpectingRefusal(
                      gateHostWithKey("conditionCodeClobber",
                                      nlohmann::json("memory")),
                      "two equal clobber spellings"),
                  DiagnosticCode::C_ConflictingField,
                  {"memoryClobber", "conditionCodeClobber", "memory"},
                  "two EQUAL clobber spellings");
}

TEST(LanguageReferenceRefusals, InlineAsmFacetOperandRuleNamingNoShapeFailsLoud) {
    // The name half: a row naming a shape that does not exist would resolve to
    // an invalid RuleId and the walk would silently match nothing.
    expectRefusal(loadExpectingRefusal(
                      gateHostWithKey("operandRule",
                                      nlohmann::json("noSuchShape")),
                      "operandRule naming no shape"),
                  DiagnosticCode::C_UnknownShape,
                  {"operandRule", "noSuchShape"},
                  "an 'operandRule' naming no declared shape");
}

// ★★ AND THE POSITIVE CONTROL FOR THE WHOLE BATTERY: the two spellings the
// SHIPPED asm document declares are exactly the two GNU-asm clobbers that name
// no register. Asserted against the real file (reached through the reference
// merge, not read as text) so a rename in `asm.lang.json` cannot pass unnoticed
// while every refusal arm above stays green.
TEST(LanguageReferences, ShippedAsmDeclaresTheTwoNonRegisterClobberSpellings) {
    auto schema = loadOk(makeHostDoc(HostVariant::WithReference));
    ASSERT_NE(schema, nullptr);
    auto const& asmCfg = schema->semantics().inlineAsm;
    EXPECT_EQ(asmCfg.memoryClobber, "memory");
    EXPECT_EQ(asmCfg.conditionCodeClobber, "cc");
    EXPECT_NE(asmCfg.memoryClobber, asmCfg.conditionCodeClobber);
}

// ═══ 6. THE **STANDALONE** ENTRY, AND THE TEMPLATE PLACEHOLDER SURFACE ═════
//
// ★★★ WHY A SECOND HOST RATHER THAN MORE ASSERTIONS ON THE FIRST. `asm.lang.json`
// has TWO entries and the loader imports the closure of the ONE a host names.
// Every test above enters at `asmStmt` (the embedded `__asm__(…)` surface), so
// not one of them can see the seven line-structure rules or the five placeholder
// rules — they are simply not in that closure, by design. A claim about the
// STANDALONE closure therefore needs a host that names it, and a host that names
// it owes a DIFFERENT set of holes. Bolting the claim onto the existing host
// would have meant changing its entry, which would have silently deleted the
// coverage every test above provides.
//
// ★★ AND THE HOST IS ALIEN ON PURPOSE, EXACTLY AS THE FIRST ONE IS. Not one
// lexeme below is spelled the way either shipped dialect spells it: the
// placeholder sigil is `?`, the label sigil `?^`, the brackets `(` and `)`, the
// line terminator `;`. If the shipped documents' spellings leaked into the
// merged rules — if `%` were baked into `asm.lang.json` anywhere — these rules
// would predict on kinds this host never declares and every assertion below
// would go red.

namespace {

// ⚠ THE RAW-STRING DELIMITER IS `JSON`, NOT THE FILE'S USUAL BARE `R"(`, AND
// IT HAS TO BE: this host declares `)` as a token lexeme, so the two bytes `)"`
// appear inside the payload and the default delimiter closes the literal there.
// ✔MEASURED as a wall of parse errors the first time it was written the usual
// way. Keeping `(`/`)` as the alien brackets is deliberate — the shipped
// dialects use `[`/`]`, and a host that reused those would weaken the "not one
// lexeme is spelled the way the real ones spell it" claim.
//
// The standalone-entry host. ⚠ `?^` IS TWO BYTES AND `?` IS ONE, which is the
// same maximal-munch relation the shipped dialects rely on for `%l` versus `%`
// — stated here because a one-byte label sigil would make this host pass while
// the real ones could not work.
constexpr std::string_view kLineHostDoc = R"JSON({
  "dssSchemaVersion": 4,
  "language": { "name": "AsmLineHostProbe", "version": "0.0.1" },
  "languageReferences": {
    "asm": {
      "entry": "asmLine",
      "bindRules": { "asmStandaloneOperand": "probeOperand" },
      "bindTokens": {
        "lineEnd":                  "StopMark",
        "labelSeparator":           "TieMark",
        "directiveIntroducer":      "LeadMark",
        "operandSeparator":         "SplitMark",
        "symbolName":               "Identifier",
        "templatePlaceholder":      "SlotMark",
        "templateLabelPlaceholder": "SlotLabelMark",
        "templateOperandIndex":     "IntLiteral",
        "symbolicNameOpen":         "HugOpen",
        "symbolicNameClose":        "HugClose"
      }
    }
  },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    ";":  [{ "kind": "StopMark" }],
    "->": [{ "kind": "TieMark" }],
    "~":  [{ "kind": "LeadMark" }],
    "&":  [{ "kind": "SplitMark" }],
    "?^": [{ "kind": "SlotLabelMark" }],
    "?":  [{ "kind": "SlotMark" }],
    "(":  [{ "kind": "HugOpen" }],
    ")":  [{ "kind": "HugClose" }],
    "$":  [{ "kind": "CoinMark" }]
  },
  "numberStyle": {
    "decimal": true,
    "emitKind": { "integer": "IntLiteral" }
  },
  "shapes": {
    "root":         { "sequence": [{ "repeat": "asmLine" }] },
    "probeOperand": { "sequence": ["CoinMark", "Identifier"] }
  }
})JSON";

// The FIVE rules the placeholder surface adds, spelled out rather than counted
// — the discipline `kAsmRules` states: a count stays green if the mechanism
// imports five of the WRONG shapes.
constexpr std::array<std::string_view, 5> kAsmTemplateRules{
    "asmTemplateOperand",
    "asmTemplateOperandRef",
    "asmTemplateSelector",
    "asmTemplateSymbolicName",
    "asmTemplateLabelRef",
};

// The line-structure rules the STANDALONE entry pulls in, including the alt
// wrapper the placeholder hangs off.
constexpr std::array<std::string_view, 8> kAsmLineRules{
    "asmLine",       "asmElement",       "asmDirective",  "asmStatement",
    "asmStatementTail", "asmLabelTail",  "asmOperandSeq", "asmOperandItem",
};

[[nodiscard]] nlohmann::json lineHostJson() {
    return nlohmann::json::parse(kLineHostDoc);
}

} // namespace

TEST(LanguageReferences, TheStandaloneEntryImportsItsOwnClosureAndNoOther) {
    auto schema = loadOk(std::string{kLineHostDoc});
    ASSERT_NE(schema, nullptr);

    for (auto const& r : kAsmLineRules) {
        EXPECT_TRUE(schema->rules().contains(r))
            << "the standalone entry did not import '" << r << "'";
    }
    for (auto const& r : kAsmTemplateRules) {
        EXPECT_TRUE(schema->rules().contains(r))
            << "the standalone entry did not import '" << r
            << "' — the placeholder surface is not reachable from `asmLine`, "
               "so no dialect could ever spell it";
    }
    // ★ AND THE EMBEDDED CLOSURE MUST **NOT** ARRIVE. Entry-closure scoping is
    // the property that lets a `.s` dialect owe no `operandExpr` binding; if
    // this host silently received `asmStmt` too it would owe holes it has no
    // vocabulary for, and the scoping would be decorative.
    for (auto const& r : {"asmStmt", "asmOperand", "asmOperandList",
                          "asmClobberList", "asmOutputsTail"}) {
        EXPECT_FALSE(schema->rules().contains(r))
            << "the standalone entry leaked the EMBEDDED rule '" << r << "'";
    }
}

// ★★★ COMPILED, NOT MERELY INTERNED — THE DISTINCTION THIS WHOLE FILE EXISTS
// FOR. `rules().contains(...)` is satisfied by a NAME in an interner; it says
// nothing about whether the rule has a position table, a FIRST set, or any
// binding to the host's vocabulary. A merge that imported the JSON body and
// failed to substitute the holes would pass the section above and predict on
// NOTHING here.
TEST(LanguageReferences, ThePlaceholderRulesArriveCompiledOnTheHostsOwnKinds) {
    auto schema = loadOk(std::string{kLineHostDoc});
    ASSERT_NE(schema, nullptr);

    auto kind = [&](std::string_view n) {
        const auto k = schema->schemaTokens().find(n);
        EXPECT_TRUE(k.valid()) << "host token kind '" << n << "' is missing";
        return k;
    };
    auto rule = [&](std::string_view n) {
        const auto r = schema->rules().find(n);
        EXPECT_TRUE(r.valid()) << "rule '" << n << "' is missing";
        return r;
    };

    const auto slot      = kind("SlotMark");
    const auto slotLabel = kind("SlotLabelMark");
    const auto hugOpen   = kind("HugOpen");
    const auto coin      = kind("CoinMark");
    const auto intLit    = kind("IntLiteral");

    // Every one of the five has a NON-EMPTY FIRST set — the interned-but-dead
    // failure mode's exact signature is an empty one.
    for (auto const& r : kAsmTemplateRules) {
        EXPECT_FALSE(schema->firstSetOf(rule(r)).empty())
            << "'" << r << "' arrived with an EMPTY FIRST set — it is interned "
               "but not compiled, so it would match nothing and say nothing";
    }

    // And each predicts on THIS host's alien kinds, which is the substitution
    // proved rather than assumed.
    EXPECT_TRUE(schema->firstSetContains(rule("asmTemplateOperandRef"), slot));
    EXPECT_TRUE(schema->firstSetContains(rule("asmTemplateLabelRef"), slotLabel));
    EXPECT_TRUE(schema->firstSetContains(rule("asmTemplateSymbolicName"), hugOpen));
    EXPECT_TRUE(schema->firstSetContains(rule("asmTemplateSelector"), intLit));
    EXPECT_TRUE(schema->firstSetContains(rule("asmTemplateSelector"), hugOpen));

    auto const anyFirst = schema->firstSetOf(rule("asmTemplateOperand"));
    ASSERT_EQ(anyFirst.size(), 2u)
        << "`asmTemplateOperand` must predict on exactly the two placeholder "
           "sigils";
    EXPECT_TRUE(schema->firstSetContains(rule("asmTemplateOperand"), slot));
    EXPECT_TRUE(schema->firstSetContains(rule("asmTemplateOperand"), slotLabel));

    // ★★ THE REACHABILITY CLAIM, WHICH IS SEPARATE FROM THE COMPILATION ONE. A
    // placeholder rule that compiled perfectly and hung off nothing would be
    // just as useless. `asmOperandSeq` must predict on the placeholder sigils
    // AND on the host's own operand production — the alt is the join.
    for (auto const k : {slot, slotLabel, coin}) {
        EXPECT_TRUE(schema->firstSetContains(rule("asmOperandSeq"), k))
            << "the instruction operand list does not predict on one of the "
               "three operand openings — the placeholder is compiled but "
               "unreachable, or the dialect's own operand production is";
    }
}

// ★★★ THE BEHAVIOURAL HALF — the schema's OWN decision procedure walks
// `? 0` and `?^ ( done )` to COMPLETION. Nothing here re-implements the
// grammar; it supplies host tokens and checks each is accepted, which is the
// same instrument `HostSpelledAsmBarrierWalksTheMergedRuleToCompletion` uses on
// the embedded entry.
TEST(LanguageReferences, HostSpelledPlaceholdersWalkTheMergedRulesToCompletion) {
    auto schema = loadOk(std::string{kLineHostDoc});
    ASSERT_NE(schema, nullptr);

    auto kind = [&](std::string_view n) {
        const auto k = schema->schemaTokens().find(n);
        EXPECT_TRUE(k.valid()) << "host token kind '" << n << "' is missing";
        return k;
    };
    auto rule = [&](std::string_view n) {
        const auto r = schema->rules().find(n);
        EXPECT_TRUE(r.valid()) << "rule '" << n << "' is missing";
        return r;
    };

    const auto indexRef  = rule("asmTemplateOperandRef");
    const auto labelRef  = rule("asmTemplateLabelRef");
    const auto selector  = rule("asmTemplateSelector");
    const auto bracketed = rule("asmTemplateSymbolicName");

    // ★ THE WALK ENTERS THE TWO **FAMILY** RULES, NOT THE `alt` ABOVE THEM, AND
    // THAT IS THE SCHEMA'S SHAPE RATHER THAN A CONVENIENCE. `asmTemplateOperand`
    // is an alt of two RULE references, so its first slot is a RuleLeaf to be
    // descended into, not a token to be consumed — the same structure the
    // embedded barrier walk handles at the template slot. Its FIRST set is
    // asserted in the test above; what is walked here is what the cursor
    // actually does.

    // `? 0` — an operand by INDEX, in this host's spelling.
    {
        auto cur = schema->advance(schema->enterRule(indexRef),
                                   kind("SlotMark"));
        ASSERT_TRUE(cur.valid())
            << "asmTemplateOperandRef rejected the host's placeholder sigil";
        // ⚠ AND THE SIGIL ALONE IS NOT AN OPERAND. A grammar that completed
        // here would bind a placeholder naming nothing.
        EXPECT_FALSE(schema->isAtEndOfRule(cur))
            << "a bare placeholder sigil COMPLETED the rule";
        // The selector is a RULE, and it is the FACTORING that makes the two
        // selector forms expressible without two arms sharing a FIRST token.
        ASSERT_EQ(schema->slotKind(cur), SlotKind::RuleLeaf);
        EXPECT_EQ(schema->slotRuleRef(cur).v, selector.v)
            << "the slot after the sigil is not the selector rule";

        auto inner = schema->advance(schema->enterRule(selector),
                                     kind("IntLiteral"));
        ASSERT_TRUE(inner.valid())
            << "the index selector rejected the host's integer kind";
        EXPECT_TRUE(schema->isAtEndOfRule(inner));

        cur = schema->leaveRule(cur);
        ASSERT_TRUE(cur.valid()) << "could not resume after the selector";
        EXPECT_TRUE(schema->isAtEndOfRule(cur))
            << "`? 0` walked the rule but did not COMPLETE it";
    }

    // `?^ ( done )` and `?^ 2` — an `asm goto` label, by NAME and by INDEX.
    // ★ THE LABEL SIGIL IS A DIFFERENT KIND FROM THE OPERAND SIGIL, so this arm
    // also proves the two placeholder families did not collapse into one during
    // the merge.
    //
    // ★★★ AND THE SLOT AFTER THE SIGIL IS THE **SELECTOR**, NOT THE BRACKETED
    // NAME — THE SYMMETRY P20 ADDED, ASSERTED AS THE SCHEMA'S OWN SHAPE. Before
    // it, this arm expected `asmTemplateSymbolicName` directly and the
    // positional label form did not parse at all; ✔MEASURED in cycle P20 that
    // gcc and clang both accept `%l<N>`, so refusing it was a conformance
    // defect. Naming the selector is what makes BOTH forms reachable with zero
    // rules added, and asserting the slot is what stops the two families from
    // drifting back into two shapes.
    {
        auto cur = schema->advance(schema->enterRule(labelRef),
                                   kind("SlotLabelMark"));
        ASSERT_TRUE(cur.valid())
            << "asmTemplateLabelRef rejected the host's LABEL sigil";
        EXPECT_FALSE(schema->isAtEndOfRule(cur))
            << "a bare label sigil COMPLETED the rule";
        ASSERT_EQ(schema->slotKind(cur), SlotKind::RuleLeaf);
        EXPECT_EQ(schema->slotRuleRef(cur).v, selector.v)
            << "a label reference does not descend into the SELECTOR — the two "
               "placeholder families are no longer sharing one selector rule, "
               "which is what makes `?^ 2` unparseable";

        // The POSITIONAL form, walked to completion through the selector's
        // TOKEN branch. ⚠ THE SELECTOR'S TWO BRANCHES ARE NOT WALKED THE SAME
        // WAY, AND THAT IS THE SCHEMA'S SHAPE RATHER THAN AN INCONSISTENCY
        // HERE: `advance` routes an AltChoice by FIRST token and can then
        // CONSUME a TokenLeaf branch, but a RuleLeaf branch has to be descended
        // into with `enterRule` — the same structure the alt above `labelRef`
        // has, and the reason the operand arm is written this way too.
        {
            auto inner = schema->advance(schema->enterRule(selector),
                                         kind("IntLiteral"));
            ASSERT_TRUE(inner.valid())
                << "the selector rejected the host's integer kind on the LABEL "
                   "path — the positional `%l<N>` form both references accept "
                   "does not parse";
            EXPECT_TRUE(schema->isAtEndOfRule(inner));
        }
        // ...and the BRACKETED form, through the SAME selector's RULE branch.
        // ⚠ BOTH ARMS, OR THIS PROVES NOTHING: a selector that predicted only
        // on the index would satisfy the slot assertion above and would have
        // silently deleted `%l[name]`.
        {
            EXPECT_TRUE(schema->expectedSetContains(schema->enterRule(selector),
                                                    kind("HugOpen")))
                << "the selector does not predict on the host's bracket — a "
                   "label by NAME no longer reaches the symbolic-name rule";
            auto inner = schema->advance(schema->enterRule(bracketed),
                                         kind("HugOpen"));
            ASSERT_TRUE(inner.valid()) << "the bracketed name did not open";
            inner = schema->advance(inner, kind("Identifier"));
            ASSERT_TRUE(inner.valid()) << "the label name was rejected";
            inner = schema->advance(inner, kind("HugClose"));
            ASSERT_TRUE(inner.valid()) << "the bracketed name did not close";
            EXPECT_TRUE(schema->isAtEndOfRule(inner));
        }

        cur = schema->leaveRule(cur);
        ASSERT_TRUE(cur.valid());
        EXPECT_TRUE(schema->isAtEndOfRule(cur))
            << "`?^ <selector>` walked the rule but did not COMPLETE it";
    }

    // ⚠ AND THE SIGILS ARE NOT INTERCHANGEABLE. If the merge had bound both
    // roles to one kind — the easy way to get this wrong — every assertion
    // above would still pass and `%0` would parse as a label.
    EXPECT_FALSE(schema->advance(schema->enterRule(indexRef),
                                 kind("SlotLabelMark")).valid())
        << "the OPERAND rule accepted the LABEL sigil";
    EXPECT_FALSE(schema->advance(schema->enterRule(labelRef),
                                 kind("SlotMark")).valid())
        << "the LABEL rule accepted the OPERAND sigil";
}

// ─── red-on-disable, FAIL-CLOSED ──────────────────────────────────────────
//
// ★★★ THE MUTANT MUST BE PROVEN READ, AND THE INSTRUMENT HERE IS THE LOADER'S
// OWN REFUSAL NAMING THE HOLE. Erasing a placeholder binding is only a load
// error if the imported rules GENUINELY SPELL that hole — i.e. if the
// placeholder surface really did arrive in this entry's closure. So this arm
// does double duty: it proves the binding is load-bearing AND it re-proves the
// import, by a mechanism completely independent of the FIRST-set assertions
// above. If the merge silently stopped importing the placeholder rules, the
// hole would stop being spelled and the erase would load CLEAN — which is
// exactly the failure this arm is shaped to catch.
TEST(LanguageReferenceRefusals, AnUnboundPlaceholderHoleFailsLoud) {
    for (auto const* hole : {"templatePlaceholder", "templateLabelPlaceholder",
                             "templateOperandIndex", "symbolicNameOpen",
                             "symbolicNameClose"}) {
        auto doc = lineHostJson();
        doc["languageReferences"]["asm"]["bindTokens"].erase(hole);
        auto const result =
            GrammarSchema::loadFromText(doc.dump(), "<asm-line-host>");
        ASSERT_FALSE(result.has_value())
            << "erasing the '" << hole << "' binding LOADED CLEAN — either the "
               "hole is not spelled by the `asmLine` closure (the placeholder "
               "rules did not arrive) or an unbound hole has stopped being a "
               "load error";
        bool named = false;
        for (auto const& d : result.error()) {
            named = named
                || (d.message.find(hole) != std::string::npos
                    && d.message.find("UNBOUND") != std::string::npos)
                || d.path.find(hole) != std::string::npos;
        }
        EXPECT_TRUE(named)
            << "the refusal for '" << hole << "' does not name it: "
            << firstError(result.error());
    }
}

// ★★ AND THE OTHER DIRECTION: the rules must come FROM THE REFERENCE. A host
// with the block removed must not have them — otherwise every assertion above
// is measuring something the loader would have produced anyway.
TEST(LanguageReferences, WithoutTheReferenceThePlaceholderRulesAreAbsent) {
    auto doc = lineHostJson();
    doc.erase("languageReferences");
    // The root must stop naming `asmLine`, or the document is simply broken and
    // its emptiness would prove nothing about the mechanism.
    doc["shapes"]["root"] = nlohmann::json::parse(
        R"({ "sequence": [{ "repeat": "probeOperand" }] })");
    auto const result =
        GrammarSchema::loadFromText(doc.dump(), "<asm-line-host-noref>");
    ASSERT_TRUE(result.has_value())
        << "the reference-less control must be a COMPLETE language, or its "
           "emptiness is a side effect of a failed load: "
        << firstError(result.error());
    for (auto const& r : kAsmTemplateRules) {
        EXPECT_FALSE((*result)->rules().contains(r))
            << "'" << r << "' exists in a host that references NOTHING";
    }
    for (auto const& r : kAsmLineRules) {
        EXPECT_FALSE((*result)->rules().contains(r))
            << "'" << r << "' exists in a host that references NOTHING";
    }
}
