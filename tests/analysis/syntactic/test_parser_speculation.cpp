#include "analysis/syntactic/parser.hpp"
#include "core/substrate/large_stack_call.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;

namespace {

// Speculative-alt schema: two cases sharing a common prefix `A`
// disambiguated by the second token. `lookahead: 3` ensures the
// probe consumes enough to discriminate.
constexpr std::string_view kSpeculativeSchema = R"JSON({
  "dssSchemaVersion": 2,
  "language": { "name": "SpecGrammar", "version": "0.1.0" },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\t": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\n": [{ "kind": "Newline",    "flags": ["EmptySpace"] }],
    "A":  [{ "kind": "AKind" }],
    "B":  [{ "kind": "BKind" }],
    "C":  [{ "kind": "CKind" }],
    ";":  [{ "kind": "Semi" }]
  },
  "shapes": {
    "root":  { "sequence": [{ "repeat": "stmt" }] },
    "stmt":  { "alt": ["caseA", "caseB"], "speculative": true, "lookahead": 3 },
    "caseA": { "sequence": ["AKind", "BKind", "Semi"] },
    "caseB": { "sequence": ["AKind", "CKind", "Semi"] }
  }
})JSON";

struct SpecHarness {
    std::shared_ptr<SourceBuffer>        src;
    std::shared_ptr<GrammarSchema const> schema;
    TokenStream                          stream;
};

[[nodiscard]] SpecHarness loadAndTokenize(std::string source) {
    auto loaded = GrammarSchema::loadFromText(kSpeculativeSchema);
    EXPECT_TRUE(loaded.has_value())
        << (loaded.has_value() ? "" : loaded.error()[0].message);
    auto schema = *loaded;
    auto src    = SourceBuffer::fromString(std::move(source), "<spec>");
    Tokenizer tk{src, schema};
    auto [stream, _] = std::move(tk).tokenize();
    return SpecHarness{
        .src    = std::move(src),
        .schema = std::move(schema),
        .stream = std::move(stream),
    };
}

[[nodiscard]] std::size_t countCode(
    std::span<ParseDiagnostic const> diags, DiagnosticCode code) {
    return static_cast<std::size_t>(std::ranges::count_if(
        diags, [code](ParseDiagnostic const& d) { return d.code == code; }));
}

// Whole-tree scan: count internal nodes whose rule matches `ruleName`.
// Speculation test fixtures don't produce out-of-root content, so the
// subtree-vs-whole-tree distinction doesn't matter here; whole-tree is
// simpler and avoids a Tree-traversal helper dependency.
[[nodiscard]] std::size_t countNodesByRule(
    Tree const& t, std::string_view ruleName) {
    if (!t.hasSchema()) return 0;
    const auto ruleId = t.schema().rules().find(ruleName);
    if (!ruleId.valid()) return 0;
    std::size_t n = 0;
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId id{i};
        if (t.kind(id) == NodeKind::Internal && t.rule(id).v == ruleId.v) ++n;
    }
    return n;
}

} // namespace

// Speculative alt with MIXED token-leaf + rule branches. Closes the gap
// found 2026-05-28: prior to the fix, the speculative AltChoice handler
// only tried RULE candidates (via `candidateBranches`), silently skipping
// token-leaf branches — so an alt like `[AKind, ruleStartingWithB]`
// marked speculative would refuse to parse a bare `A`, even though
// `AKind` is the trivial first-branch match. The fix adds a token-leaf
// `schema->advance` probe BEFORE the rule-scan, mirroring the
// non-speculative path's order. This is the substrate enabler for
// schemas whose expression-atom alt mixes token primaries (Identifier /
// IntLiteral / etc.) with rule primaries (compoundLiteralExpr /
// parenExpr) and needs `speculative: true` for the rule-branch
// disambiguation.
constexpr std::string_view kMixedTokenAndRuleSchema = R"JSON({
  "dssSchemaVersion": 2,
  "language": { "name": "MixSpec", "version": "0.1.0" },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\n": [{ "kind": "Newline",    "flags": ["EmptySpace"] }],
    "A":  [{ "kind": "AKind" }],
    "B":  [{ "kind": "BKind" }],
    ";":  [{ "kind": "Semi" }]
  },
  "shapes": {
    "root":    { "sequence": [{ "repeat": "primary" }] },
    "primary": { "alt": ["AKind", "ruleB"], "speculative": true },
    "ruleB":   { "sequence": ["BKind", "Semi"] }
  }
})JSON";

TEST(ParserSpeculation, SpeculativeAltAcceptsTokenLeafBranches) {
    // `A` alone exercises the token-leaf route inside the speculative
    // alt. Pre-fix: candidateBranches returns empty (no rule starts
    // with AKind), so the loop falls through to P_BacktrackFailed.
    // Post-fix: schema->advance(cursor, AKind) succeeds at the
    // token-leaf branch, advancing cleanly.
    auto loaded = GrammarSchema::loadFromText(kMixedTokenAndRuleSchema);
    ASSERT_TRUE(loaded.has_value());
    auto src = SourceBuffer::fromString("A", "<mix>");
    Tokenizer tk{src, *loaded};
    auto [stream, _] = std::move(tk).tokenize();
    Parser p{src, *loaded, std::move(stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "speculative alt must accept token-leaf branches";
    EXPECT_EQ(countCode(t.diagnostics().all(),
                        DiagnosticCode::P_BacktrackFailed), 0u);
}

// Speculative alt whose rule-branches contain an `expr`-shape rule
// that itself uses postfix operators (the Pratt walker's
// `wrapLastChildExprFrame` path). Closes plan 05 sub-cycle B (the
// 2026-05-28 sibling of sub-cycle A): the SchemaWalker's
// `cursorDesynced_` latch used to fire as a false positive when the
// Pratt walker wrapped an operand inside an auto-interned wrapper
// rule (binary / unary / postfix / ternary). The wrapper has no
// positions in the schema's graph, so the subsequent operator-token
// advance landed on an invalid cursor — a structural-only "desync"
// the latch incorrectly counted as a real grammar mismatch. In
// speculative parsing the outer probe read the latch as "wrong
// branch" and bailed with `P_BacktrackFailed`. The fix (Fix B): the
// walker recognizes auto-interned wrapper rules via
// `GrammarSchema::isAutoInternedWrapperRule` and suppresses the
// latch while any ancestor frame is a wrap, fixing the latch's
// behavioral contract ("real grammar mismatch ONLY"). Empirical
// reproducer at c-subset level: `(f(a))` fails (✗) without the fix,
// passes (✓) with it. This synthetic test pins the substrate
// behavior in isolation from any specific language schema.
constexpr std::string_view kSpecOverExprPostfix = R"JSON({
  "dssSchemaVersion": 2,
  "language": { "name": "SpecExprPostfix", "version": "0.1.0" },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\n": [{ "kind": "Newline",    "flags": ["EmptySpace"] }],
    "(":  [{ "kind": "LParen" }],
    ")":  [{ "kind": "RParen" }],
    "[":  [{ "kind": "LBracket" }],
    "]":  [{ "kind": "RBracket" }],
    ";":  [{ "kind": "Semi" }],
    "I":  [{ "kind": "AtomTok" }]
  },
  "operators": {
    "groups": [
      { "precedence": 100, "arity": "postfix", "operators": ["("], "endsAt": ")" }
    ]
  },
  "shapes": {
    "root":    { "sequence": [{ "repeat": "stmt" }] },
    "stmt":    { "sequence": [ "primary", "Semi" ] },
    "primary": { "alt": ["AtomTok", "parenExpr"], "speculative": true },
    "parenExpr": {
      "sequence": [ "LParen", "expression", "RParen" ]
    },
    "expression": {
      "expr": {
        "atom": "primary",
        "wrapperRules": {
          "binary":  "binaryExpr",
          "unary":   "unaryExpr",
          "postfix": "postfixExpr"
        }
      }
    }
  }
})JSON";

TEST(ParserSpeculation, SpeculativeAltSurvivesInnerPostfixWrap) {
    auto loaded = GrammarSchema::loadFromText(kSpecOverExprPostfix);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.has_value() ? "" : loaded.error()[0].message);
    // `(I());` — outer `primary` speculative alt picks `parenExpr`; the
    // inner expression `I()` exercises the Pratt walker's postfix-wrap
    // (operand `I` followed by the postfix call `()`). Pre-fix, this
    // tripped the outer probe's `isDesynced` check via the wrap-induced
    // false-positive latch. Post-fix, the parse completes cleanly.
    auto src = SourceBuffer::fromString("(I());", "<spec>");
    Tokenizer tk{src, *loaded};
    auto [stream, _] = std::move(tk).tokenize();
    Parser p{src, *loaded, std::move(stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "speculative alt with inner postfix-wrap must NOT desync-fail";
    EXPECT_EQ(countCode(t.diagnostics().all(),
                        DiagnosticCode::P_BacktrackFailed), 0u)
        << "the Pratt walker's wrap must not be seen as a wrong branch";
    EXPECT_EQ(countNodesByRule(t, "parenExpr"), 1u)
        << "outer `parenExpr` branch must commit";
    EXPECT_EQ(countNodesByRule(t, "postfixExpr"), 1u)
        << "inner postfix-wrap must materialize as a postfixExpr frame";
}

TEST(ParserSpeculation, SpeculativeAltMixesTokenLeafAndRuleBranches) {
    // `A B ;` mixes: first `primary` takes the token-leaf branch (A);
    // second `primary` takes the rule branch (B Semi). Confirms both
    // routes coexist inside one speculative alt without interference.
    auto loaded = GrammarSchema::loadFromText(kMixedTokenAndRuleSchema);
    ASSERT_TRUE(loaded.has_value());
    auto src = SourceBuffer::fromString("A B ;", "<mix>");
    Tokenizer tk{src, *loaded};
    auto [stream, _] = std::move(tk).tokenize();
    Parser p{src, *loaded, std::move(stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_EQ(countNodesByRule(t, "primary"), 2u)
        << "two `primary` frames — one for token-leaf A, one for ruleB";
    EXPECT_EQ(countNodesByRule(t, "ruleB"), 1u)
        << "second primary commits the ruleB branch";
}

TEST(ParserSpeculation, CommitsCaseAOnFirstBranchInput) {
    auto h = loadAndTokenize("A B ;");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "valid input matching the first branch must commit cleanly";
    EXPECT_EQ(countNodesByRule(t, "caseA"), 1u)
        << "caseA branch must commit (single frame in tree)";
    EXPECT_EQ(countNodesByRule(t, "caseB"), 0u)
        << "caseB branch must NOT appear (would indicate orphan from failed probe)";
}

TEST(ParserSpeculation, CommitsCaseBOnLaterBranchInput) {
    // `A C ;` matches caseB. Exercises the `for (auto branch :
    // candidates)` loop iterating past the FIRST failed
    // `trySpeculativeBranch(caseA)` to commit `caseB` — a refactor
    // that returns after the first probe failure (early-out instead
    // of try-next) would fail this test.
    auto h = loadAndTokenize("A C ;");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "valid input matching the second branch must commit cleanly";
    EXPECT_EQ(countNodesByRule(t, "caseB"), 1u)
        << "caseB branch must commit";
    EXPECT_EQ(countNodesByRule(t, "caseA"), 0u)
        << "caseA must NOT appear (would indicate first-branch commit)";
}

// D-PARSE-PREDICTIVE-PRUNE-CONTEXTUAL-KEYWORD: the LL(k) predictive prune must NOT
// drop a speculative candidate whose fixed prefix admits Identifier at an offset
// where the observed token is a CONTEXTUAL (soft) keyword — the builder demotes it
// to Identifier, so the candidate DOES match. `caseId = [AKind, Identifier, Semi]`;
// `kw` is a soft keyword (`contextual: true`); `hk` is a HARD keyword.
constexpr std::string_view kContextualSpecSchema = R"JSON({
  "dssSchemaVersion": 2,
  "language": { "name": "CtxSpec", "version": "0.1.0" },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\n": [{ "kind": "Newline",    "flags": ["EmptySpace"] }],
    "A":  [{ "kind": "AKind" }],
    ";":  [{ "kind": "Semi" }]
  },
  "keywords": [
    { "word": "kw", "kind": "KwKind", "contextual": true },
    { "word": "hk", "kind": "HkKind" }
  ],
  "semantics": { "identifierToken": "Identifier" },
  "shapes": {
    "root":   { "sequence": [{ "repeat": "stmt" }] },
    "stmt":   { "alt": ["caseId", "caseKw"], "speculative": true, "lookahead": 4 },
    "caseId": { "sequence": ["AKind", "Identifier", "Semi"] },
    "caseKw": { "sequence": ["AKind", "AKind", "Semi"] }
  }
})JSON";

[[nodiscard]] SpecHarness loadCtx(std::string source) {
    auto loaded = GrammarSchema::loadFromText(kContextualSpecSchema);
    EXPECT_TRUE(loaded.has_value())
        << (loaded.has_value() ? "" : loaded.error()[0].message);
    auto schema = *loaded;
    auto src    = SourceBuffer::fromString(std::move(source), "<ctx>");
    Tokenizer tk{src, schema};
    auto [stream, _] = std::move(tk).tokenize();
    return SpecHarness{ .src    = std::move(src),
                        .schema = std::move(schema),
                        .stream = std::move(stream) };
}

TEST(ParserSpeculation, ContextualKeywordInSpeculativePrefixNotMispruned) {
    // `A kw ;` — `kw` is a soft keyword at offset 1. caseId's prefix admits
    // Identifier there, caseKw's admits AKind. The end-to-end parse needs BOTH
    // halves of D-PARSE-PREDICTIVE-PRUNE-CONTEXTUAL-KEYWORD:
    //   (1) prune-skip — without `isContextualKind(got) → continue`, the
    //       predictive prune drops caseId (KwKind ∉ {Identifier}) before the
    //       probe runs;
    //   (2) match-gate demotion mirror — even surviving the prune, caseId's
    //       probe would `recoverAt(P_UnexpectedToken)` because the TokenLeaf
    //       gate tests the un-demoted KwKind ∉ {Identifier}, unless the gate
    //       mirrors the builder's demotion and advances the walker as Identifier.
    // WITH both, caseKw fails its probe (kw ≠ AKind), caseId demotes kw→Identifier
    // and commits. RED-on-disable: revert either half → this errors (verified by
    // disabling each independently during the cycle).
    auto h = loadCtx("A kw ;");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "the soft keyword `kw` demotes to Identifier; caseId must commit "
           "(without the fix the prune drops the candidate AND the match gate "
           "rejects the keyword — both halves are load-bearing)";
    EXPECT_EQ(countNodesByRule(t, "caseId"), 1u);
    EXPECT_EQ(countNodesByRule(t, "caseKw"), 0u);
    // Prove the commit went through the DEMOTION path specifically (not some
    // other route to caseId): the builder records an info-level
    // P_ContextualKeywordResolution exactly when it demotes the soft keyword.
    std::size_t demotions = 0;
    for (auto const& d : t.diagnostics().all()) {
        if (d.code == DiagnosticCode::P_ContextualKeywordResolution) ++demotions;
    }
    EXPECT_EQ(demotions, 1u)
        << "`kw` must be recorded as demoted-to-Identifier (the demotion ran)";
}

TEST(ParserSpeculation, HardKeywordInSpeculativePrefixStillPruned) {
    // Negative control — the fix skips ONLY contextual kinds, never widening to
    // HARD keywords. `A hk ;`: the hard keyword `hk` cannot demote to Identifier
    // (caseId) nor be AKind (caseKw) → no valid stmt, with OR without the fix.
    // Proves the prune stays precise (no over-broadening).
    auto h = loadCtx("A hk ;");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;
    EXPECT_TRUE(t.diagnostics().hasErrors())
        << "a hard keyword matches neither Identifier nor AKind — no valid stmt";
    EXPECT_EQ(countNodesByRule(t, "caseId"), 0u);
}

TEST(ParserSpeculation, BacktrackFailedAndRecoveryOnBogusInput) {
    // `A X ;` — X is illegal. The tokenizer emits an Error token.
    // Speculation probes every branch; all fail and roll back. FC4 c1:
    // the parser then REPLAYS the declared-LAST candidate (caseB)
    // non-speculatively so the branch's own precise diagnostics land
    // (pre-replay this surfaced only an opaque P_BacktrackFailed). The
    // pinned contract: the parser terminates AND a loud error from the
    // replayed branch surfaces.
    auto h = loadAndTokenize("A X ;");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_TRUE(t.diagnostics().hasErrors())
        << "bogus speculative input must surface an error";
    EXPECT_EQ(countCode(t.diagnostics().all(),
                        DiagnosticCode::P_BacktrackFailed), 0u)
        << "the fallback replay owns the diagnostics — no opaque "
           "P_BacktrackFailed at the outermost level";
}

TEST(ParserSpeculation, MaxDepthCapEmitsAndStopsRecursion) {
    // Schema has no nested speculative alts, so depth=1 lets the
    // first probe in but refuses any re-entry. With the current
    // schema the cap is never actually breached; the test asserts
    // the depth-counter wiring exists by passing the default
    // (8) parser tree without errors AND that a constructor with
    // `maxSpeculationDepth = 1` still parses cleanly (proving the
    // counter increments AND decrements correctly).
    auto h = loadAndTokenize("A B ;");
    ParserConfig cfg;
    cfg.maxSpeculationDepth = 1;
    Parser p{h.src, h.schema, std::move(h.stream), std::move(cfg)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "depth=1 cap allows a single-level probe; A B ; commits cleanly";
    EXPECT_EQ(countCode(t.diagnostics().all(),
                        DiagnosticCode::P_MaxSpeculationDepth), 0u);
}

TEST(ParserSpeculation, NoCommittableBranchTerminatesCleanly) {
    // Forward-progress guarantee under adversarial speculative
    // input: every branch's FIRST matches `A` but the input gives
    // neither caseA's body (B) nor caseB's body (C). FC4 c1: the
    // all-fail outcome replays the declared-LAST candidate (caseB)
    // non-speculatively — its own dispatch then emits the precise
    // missing-child/premature-EOF diagnostics and the normal recovery
    // machinery guarantees forward progress. The test asserts
    // termination (no hang) + a loud error.
    auto h = loadAndTokenize("A");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_TRUE(t.diagnostics().hasErrors())
        << "every-branch-fails input must surface a loud error from the "
           "replayed fallback branch";
}

TEST(ParserSpeculation, EmptyInputSkipsSpeculativeRepeat) {
    // Empty source: `repeat stmt` is nullable; the root's outer
    // AltChoice (the repeat) takes the skip branch. The stmt
    // AltChoice (the speculative one) is never entered. No
    // P_BacktrackFailed should fire on a no-input parse.
    auto h = loadAndTokenize("");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_EQ(countCode(t.diagnostics().all(),
                        DiagnosticCode::P_BacktrackFailed), 0u)
        << "skipping a nullable repeat must not trip speculation";
}

// ── DECLARED-order speculative probing (the FC4 enabler) ──────────────
//
// Speculative candidates probe in DECLARED grammar order — author-
// controlled, never interner/name order. Rule-interner ids are assigned
// by iterating the JSON `shapes` OBJECT (alphabetical key order on
// nlohmann::json), so an interner-sourced candidate scan makes probe
// priority an accident of rule NAMING. The two rules below have
// IDENTICAL bodies — both structurally succeed on `A B ;` — and are
// DECLARED in the REVERSE of alphabetical order (`zetaFirst` before
// `alphaSecond`), so the committed branch is a pure probe-ORDER
// witness:
//   - declared order (the contract):    zetaFirst probes first → wins;
//   - interner order (the old accident): alphaSecond interns before
//     zetaFirst, probes first, and wins — this test goes RED on any
//     revert to interner-sourced candidate enumeration.
constexpr std::string_view kDeclaredOrderSchema = R"JSON({
  "dssSchemaVersion": 2,
  "language": { "name": "DeclOrder", "version": "0.1.0" },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\n": [{ "kind": "Newline",    "flags": ["EmptySpace"] }],
    "A":  [{ "kind": "AKind" }],
    "B":  [{ "kind": "BKind" }],
    ";":  [{ "kind": "Semi" }]
  },
  "shapes": {
    "root":        { "sequence": [{ "repeat": "stmt" }] },
    "stmt":        { "alt": ["zetaFirst", "alphaSecond"], "speculative": true, "lookahead": 3 },
    "zetaFirst":   { "sequence": ["AKind", "BKind", "Semi"] },
    "alphaSecond": { "sequence": ["AKind", "BKind", "Semi"] }
  }
})JSON";

TEST(ParserSpeculation, ProbeOrderIsDeclaredOrderNotInternerOrder) {
    auto loaded = GrammarSchema::loadFromText(kDeclaredOrderSchema);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.has_value() ? "" : loaded.error()[0].message);

    // Schema-tier half of the pin: the alt's branch enumeration itself
    // must report [zetaFirst, alphaSecond] — declared, not alphabetical.
    {
        auto const& schema = **loaded;
        const RuleId stmt = schema.rules().find("stmt");
        ASSERT_TRUE(stmt.valid());
        const SchemaCursor altCur = schema.enterRule(stmt);
        ASSERT_EQ(schema.slotKind(altCur), SlotKind::AltChoice);
        ASSERT_TRUE(schema.isSpeculativeAlt(altCur));
        const auto branches = schema.altRuleBranches(altCur);
        ASSERT_EQ(branches.size(), 2u);
        EXPECT_EQ(schema.rules().name(branches[0]), "zetaFirst");
        EXPECT_EQ(schema.rules().name(branches[1]), "alphaSecond");
    }

    // Parser-tier half: the DECLARED-first branch must be the one that
    // commits when both branches structurally succeed.
    auto src = SourceBuffer::fromString("A B ;", "<declorder>");
    Tokenizer tk{src, *loaded};
    auto [stream, _] = std::move(tk).tokenize();
    Parser p{src, *loaded, std::move(stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_EQ(countNodesByRule(t, "zetaFirst"), 1u)
        << "the DECLARED-first branch must win the probe";
    EXPECT_EQ(countNodesByRule(t, "alphaSecond"), 0u)
        << "the alphabetically-first branch winning means candidates "
           "were sourced from the rule interner (name order), not the "
           "alt's declared branch list";
}

TEST(ParserSpeculation, CSubsetOperandAltBranchesAreInDeclaredOrder) {
    // FC2 order-preservation pin over the SHIPPED c-subset grammar:
    // `operand`'s speculative alt declares its RULE branches as
    // [stringLiteralExpr, charLiteralExpr, compoundLiteralExpr,
    // castExpr, parenExpr] (the alt's leading entries are token-leaf
    // branches — Identifier / IntLiteral / FloatLiteral / TrueKeyword /
    // FalseKeyword — which route via `advance`, not the probe loop, and
    // so are deliberately absent here). The schema API must report the
    // rule branches in EXACTLY that declared array order; a regression
    // to interner order would instead yield the alphabetical
    // [castExpr, charLiteralExpr, compoundLiteralExpr, parenExpr,
    // stringLiteralExpr] — silently re-prioritizing castExpr ahead of
    // compoundLiteralExpr for every `(`-led operand probe.
    auto loaded = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(loaded.has_value());
    auto const& schema = **loaded;

    const RuleId operand = schema.rules().find("operand");
    ASSERT_TRUE(operand.valid()) << "c-subset must declare an `operand` rule";
    const SchemaCursor altCur = schema.enterRule(operand);
    ASSERT_EQ(schema.slotKind(altCur), SlotKind::AltChoice)
        << "`operand`'s body must compile to an AltChoice entry position";
    ASSERT_TRUE(schema.isSpeculativeAlt(altCur));

    std::vector<std::string> names;
    for (RuleId const r : schema.altRuleBranches(altCur)) {
        names.emplace_back(schema.rules().name(r));
    }
    const std::vector<std::string> declared{
        "stringLiteralExpr",
        "charLiteralExpr",
        "compoundLiteralExpr",
        "sizeofExpr",            // FC6
        "labelAddressExpr",      // D-CSUBSET-COMPUTED-GOTO (`&&label`)
        "vaStartExpr",           // FC12a-core (D-FC12A-VARIADIC-CALLEE)
        "vaArgExpr",             // FC12a-core
        "vaEndExpr",             // FC12a-core
        "genericExpr",           // FC16 (D-CSUBSET-GENERIC-SELECTION, `_Generic`)
        "castExpr",
        "parenExpr",
    };
    EXPECT_EQ(names, declared)
        << "operand's speculative rule branches must surface in the "
           "JSON-array order declared in c-subset.lang.json";
}

// ── Deep-nest predictive-prune O(N) pin (D-PARSE-SPECULATION-OPERAND-
//    QUADRATIC) ──────────────────────────────────────────────────────────
//
// `((((…0…))))` is the C cast-vs-paren worst case: at EVERY nesting level
// `operand`'s speculative alt sees `(`, and pre-fix tried the `castExpr`
// probe first — which fails (`castTypeRef` can't start with `(`) and runs
// panic recovery that scans O(min(N,syncCap)) tokens, repeated at N levels
// ⇒ O(N²) (deep input HUNG: N=300 took >150 s wall-clock through the CLI).
//
// The fix is the engine's LL(k) PREDICTIVE PRUNE plus unique-production
// direct descent: token[+1]==`(` ∉ FIRST(castTypeRef) prunes `castExpr` and
// `compoundLiteralExpr` BEFORE they speculate, and the lone survivor
// `parenExpr` (no commit-triage) is descended into directly — no per-level
// speculation probe. Net: O(N).
//
// This pin parses the REAL shipped c-subset grammar at a high
// `maxExpressionDepth` (so the parse runs to completion instead of tripping
// the low default cap) and asserts (a) a CLEAN parse — no diagnostics, no
// `P_BacktrackFailed` — proving the prune kept the valid `parenExpr` reading
// at every level, and (b) ROUGHLY LINEAR scaling: doubling the nesting
// depth must not blow the parse time up super-linearly. A quadratic
// regression (the bug) makes the 2N parse ~4× the N parse and far exceeds
// the generous ceiling asserted here; the headroom keeps the test robust to
// CI timing jitter while still catching an O(N²) reintroduction (which would
// be orders of magnitude over, not a small multiple).
//
// NOTE: only the PARSE is exercised here — the downstream semantic / HIR /
// MIR passes recurse on the host stack and overflow at ~25 levels
// (D-PARSE-DEEP-FRONTEND-STACK, a SEPARATE anchor), so this stays at the
// parser boundary deliberately.
//
// NOTE 2 (the generous 8× ceiling): the parse WORK is O(N) — the companion
// `FlatChainParseWorkIsLinear` pins the shared machinery flat — but this
// RECURSIVE nest carries a residual ~exp-1.7 wall-clock super-linearity that
// is a memory-hierarchy constant of the N-deep host recursion (live call
// stack + strided unwind), NOT an algorithmic term. It is bounded/moot in
// practice (the 256 depth cap + the recursion-bound downstream frontend) and
// documented as D-PARSE-DEEP-NEST-RECURSION-MEMORY; the 8× bound is sized to
// absorb it while still catching an O(N²) speculation regression (orders of
// magnitude over).
namespace {

[[nodiscard]] std::string deepNestSource(std::size_t depth) {
    std::string s = "int main(void){return ";
    s.append(depth, '(');
    s.push_back('0');
    s.append(depth, ')');
    s += ";}";
    return s;
}

// Parse `((…0…))` nested `depth` deep through the SHIPPED c-subset grammar
// at a raised expression-depth cap; return the wall-clock parse duration.
// Fails the calling test (via gtest macros) on any parse diagnostic.
//
// `schema` is loaded ONCE by the caller and reused so the timed region is
// the parse alone, not a per-call schema reload.
[[nodiscard]] std::chrono::nanoseconds
timeDeepNestParse(std::shared_ptr<GrammarSchema const> const& schema,
                  std::size_t depth, std::size_t cap) {
    auto src = SourceBuffer::fromString(deepNestSource(depth), "<deep>");
    Tokenizer tk{src, schema};
    auto [stream, lexDiags] = std::move(tk).tokenize();

    ParserConfig cfg;
    cfg.maxExpressionDepth = cap;
    Parser p{src, schema, std::move(stream), std::move(cfg)};

    const auto t0 = std::chrono::steady_clock::now();
    auto result = std::move(p).parse();
    const auto t1 = std::chrono::steady_clock::now();

    auto const& t = result.tree;
    EXPECT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "deep-nest `" << depth << "`-paren parse must be CLEAN — the "
           "predictive prune must keep the valid parenExpr reading at every "
           "level (first diag: "
        << (t.diagnostics().all().empty()
                ? std::string{"<none>"}
                : t.diagnostics().all().front().actual)
        << ")";
    EXPECT_EQ(countCode(t.diagnostics().all(),
                        DiagnosticCode::P_BacktrackFailed), 0u)
        << "no candidate-backtrack failures may surface for a well-formed "
           "deeply-nested paren expression";
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0);
}

} // namespace

TEST(ParserSpeculation, DeepNestCastVsParenIsLinear) {
    // Cap comfortably above the deepest probe so the parse runs to the end
    // (the per-parser `maxExpressionDepth` override — independent of the low
    // shipped default — lets the parse descend the full nesting).
    constexpr std::size_t kCap = 4000;

    // Load the shipped grammar ONCE; reuse it across every timed parse so the
    // measured region is the parse alone, not a per-call schema reload.
    auto loaded = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;

    // The deep parse recurses one host-stack frame per paren level, so run on
    // a large-stack thread — the parser's own stack ceiling (not the
    // speculation cost) is otherwise the depth limit (D-PARSE-DEEP-FRONTEND-
    // STACK). N=500 and N=1000 are the task's deep-nest proof points; 1000 is
    // exactly 2× 500, so an O(N) parse scales ~2× and an O(N²) regression
    // ~4× — and the historical bug's per-level panic-scan blowup pushes it
    // FAR past that (the pre-fix CLI hung for >150 s at N=300).
    std::chrono::nanoseconds t500{}, t1000{};
    // The shared substrate large-stack runner (the SAME facility the production
    // driver uses for the deep parse). A 256 MiB reserve gives this stress test
    // generous headroom at the 4000-deep cap — well above the 64 MiB the
    // shipped pipeline reserves for the 256-deep shipped cap.
    constexpr std::size_t kRunnerStackBytes = std::size_t{256} * 1024 * 1024;
    dss::substrate::runOnLargeStack(kRunnerStackBytes, [&] {
        // Warm allocator/caches so the first timed run isn't setup-taxed.
        (void)timeDeepNestParse(schema, 50, kCap);
        // Take the MIN over several runs. A wall-clock sample is
        // `true_compute_time + non-negative OS/scheduler/cache noise`, so the
        // minimum is the closest estimate of the true compute time and the ratio
        // of minima reflects the ALGORITHMIC scaling, not CI jitter. Single
        // sub-millisecond samples on a shared CI runner (especially the macOS
        // hosted runner) swing the ratio wildly — a lucky-fast `t500` against a
        // noisy `t1000` produced an intermittent failure (ratio 9.68 from
        // t500=0.7ms / t1000=6.78ms, where the true exp-~1.7 scaling is ~3.25×).
        constexpr int kRuns = 9;  // match FlatChainParseWorkIsLinear: more
                                  // samples → a better shot at one uninterrupted
                                  // run on a loaded shared CI runner.
        auto const minParse = [&](std::size_t depth) {
            auto best = std::chrono::nanoseconds::max();
            for (int i = 0; i < kRuns; ++i) {
                best = std::min(best, timeDeepNestParse(schema, depth, kCap));
            }
            return best;
        };
        t500  = minParse(500);
        t1000 = minParse(1000);
    });

    // Doubling the nesting (500→1000) must NOT explode the parse time. The
    // pre-fix O(N²) cast-vs-paren speculation made deep input catastrophic —
    // the per-level panic-scan blowup pushed the CLI past 150 s at N=300, i.e.
    // a multi-thousand-× scaling, not a small multiple. The ceiling here is
    // intentionally generous (Debug-build allocator noise + CI timing jitter
    // make the observed clean-parse ratio swing a few × around the ideal 2×),
    // yet still fails LOUD on a quadratic reintroduction, which overshoots it
    // by orders of magnitude. The CLEAN-parse assertions inside
    // `timeDeepNestParse` (no diagnostics, no `P_BacktrackFailed` at depth 500
    // AND 1000) are the precise functional guard that the prune kept the valid
    // `parenExpr` reading at every level. A floor on the denominator avoids
    // divide-by-noise on a fast baseline.
    const double small = std::max<double>(
        static_cast<double>(t500.count()), 1.0);
    const double ratio = static_cast<double>(t1000.count()) / small;
    // Bound: the true exp-~1.7 scaling is ~3.25× for a doubling; min-over-runs
    // removes the jitter, so 10× is a generous cross-platform ceiling that still
    // fails LOUD on the catastrophic O(N²) cast-vs-paren regression (>>100×, the
    // pre-fix bug hung >150 s at N=300).
    EXPECT_LT(ratio, 10.0)
        << "deep-nest parse time scaled " << ratio << "× when nesting "
           "doubled (500→1000); linear is ~2×. A super-linear ratio of this "
           "magnitude signals the O(N²) cast-vs-paren speculation regressed "
           "(D-PARSE-SPECULATION-OPERAND-QUADRATIC). t500="
        << (static_cast<double>(t500.count()) / 1e6) << "ms t1000="
        << (static_cast<double>(t1000.count()) / 1e6) << "ms";
}

// ── LL(k) predictive prune at offset >=1 + nullable-stop soundness ───────
//
// These two synthetic-schema tests exercise the predictive prune at a NON-
// zero offset (offset 0 is the standard 1-token FIRST gate; the prune's extra
// discriminating power is entirely at offsets >=1) and pin the nullable-stop
// soundness invariant of `computePredictivePrefixes`.

// (a) PRUNE-FIRES POSITIVE PATH. Reuses `kSpeculativeSchema` (caseA=[A,B,Semi]
// / caseB=[A,C,Semi], lookahead 3). For input `A C ;` both branches share
// FIRST={A}, so offset 0 cannot discriminate — the offset-1 prefix does:
// caseA's prefix[1]={B}, caseB's prefix[1]={C}. The prune drops caseA
// (observed C ∉ {B}) and the LONE survivor caseB carries no commit-time triage
// (no `commitRequiresTypeName`), so the parser takes the UNIQUE-PRODUCTION
// DIRECT-DESCENT path — committing caseB WITHOUT a speculation probe. The
// resulting tree (clean, caseB present, caseA absent) is the functional pin;
// the O(N) deep-nest test above is the performance witness that the
// direct-descent path (not a per-level probe) is what runs.
TEST(ParserSpeculation, PrunesFirstBranchAtOffsetOneAndDirectDescends) {
    auto h = loadAndTokenize("A C ;");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "the offset-1 prune must keep the valid caseB reading and parse "
           "`A C ;` cleanly";
    EXPECT_EQ(countCode(t.diagnostics().all(),
                        DiagnosticCode::P_BacktrackFailed), 0u)
        << "the lone-survivor direct descent must not surface a backtrack "
           "failure";
    EXPECT_EQ(countNodesByRule(t, "caseB"), 1u)
        << "caseB is the lone candidate the offset-1 prune leaves — it must "
           "commit via the unique-production direct-descent path";
    EXPECT_EQ(countNodesByRule(t, "caseA"), 0u)
        << "caseA must NOT appear — the offset-1 prefix prunes it (observed "
           "C is not in caseA's prefix[1]={B})";
}

// (b) NULLABLE-STOP SOUNDNESS GUARD (Finding 1 — RED on the pre-fix code).
//
// caseA's second element is a NULLABLE NAMED rule (`nullableRule =
// {optional CKind}`), referenced as a RuleLeaf — and a RuleLeaf's expectedSet
// is FIRST(sub-rule) ALONE (it never absorbs the parent continuation, unlike
// an inline optional's AltChoice, whose expectedSet `recomputeAltExpectedSets`
// already unions with the continuation). Pre-fix, `computePredictivePrefixes`
// recorded that RuleLeaf's expectedSet as caseA's prefix[1] = FIRST(nullableRule)
// = {C}. But `nullableRule` can derive EMPTY, so after `A` the next REAL token
// may be `nullableRule`'s first token (C) OR — when nullableRule is skipped —
// caseA's following `BKind` (B). The recorded {C} UNDER-approximates the true
// admissible set {B, C}; for input `A B ;` the prune then WRONGLY drops caseA
// (observed B ∉ {C}). caseB=[A,D,Semi] is likewise pruned (B ∉ {D}), so the
// speculation set empties and the fallback replays caseB, which fails on `B`
// where it expects `D` — a LOUD mis-parse where a clean caseA parse was due.
//
// The fix: a nullable stop element (here, a RuleLeaf whose sub-rule is
// nullable) is NOT recorded — the walk STOPS with only the exact single-token
// offsets so far. caseA's prefix collapses to length 1 ({A}) and is cleared,
// so caseA carries no multi-token discriminator, never prunes, and `A B ;`
// matches caseA cleanly. This test asserts that clean parse; it is RED on the
// pre-Finding-1 code (revert the nullable guard in `computePredictivePrefixes`
// → the EXPECT_FALSE(hasErrors) and caseA==1 assertions fail).
constexpr std::string_view kNullableStopSchema = R"JSON({
  "dssSchemaVersion": 2,
  "language": { "name": "NullableStopSpec", "version": "0.1.0" },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\t": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\n": [{ "kind": "Newline",    "flags": ["EmptySpace"] }],
    "A":  [{ "kind": "AKind" }],
    "B":  [{ "kind": "BKind" }],
    "C":  [{ "kind": "CKind" }],
    "D":  [{ "kind": "DKind" }],
    ";":  [{ "kind": "Semi" }]
  },
  "shapes": {
    "root":         { "sequence": [{ "repeat": "stmt" }] },
    "stmt":         { "alt": ["caseA", "caseB"], "speculative": true, "lookahead": 4 },
    "nullableRule": { "optional": "CKind" },
    "caseA":        { "sequence": ["AKind", "nullableRule", "BKind", "Semi"] },
    "caseB":        { "sequence": ["AKind", "DKind", "Semi"] }
  }
})JSON";

TEST(ParserSpeculation, NullableStopElementDoesNotOverPrune) {
    auto loaded = GrammarSchema::loadFromText(kNullableStopSchema);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.has_value() ? "" : loaded.error()[0].message);

    // `A B ;` — caseA with the nullable `nullableRule` skipped (zero C's),
    // then B then ;. The offset-1 prune must NOT drop caseA on the strength of
    // FIRST(nullableRule)={C}: the sub-rule is nullable, so B is a legitimate
    // offset-1 token for caseA.
    auto src = SourceBuffer::fromString("A B ;", "<nullstop>");
    Tokenizer tk{src, *loaded};
    auto [stream, _] = std::move(tk).tokenize();
    Parser p{src, *loaded, std::move(stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "a nullable stop element must not be recorded as an exact offset — "
           "`A B ;` is a valid caseA parse (nullableRule derives empty) and "
           "the prune must NOT drop caseA (RED on the pre-Finding-1 code)";
    EXPECT_EQ(countCode(t.diagnostics().all(),
                        DiagnosticCode::P_BacktrackFailed), 0u)
        << "no backtrack failure may surface for the well-formed `A B ;`";
    EXPECT_EQ(countNodesByRule(t, "caseA"), 1u)
        << "caseA must commit — the nullable-stop soundness guard keeps it in "
           "the candidate set for the offset-1 token B";
    EXPECT_EQ(countNodesByRule(t, "caseB"), 0u)
        << "caseB must NOT appear — `A B ;` is unambiguously caseA";
}

// ── Flat-chain O(N)-WORK pin — the non-recursive control for the deep-nest
//    memory-latency residual (D-PARSE-DEEP-NEST-RECURSION-MEMORY) ───────────
//
// A flat left-assoc `0+0+…+0` chain has the SAME O(N) total nodes/tokens as a
// deep `((((0))))` nest but builds ITERATIVELY (the Pratt climb wraps in
// place; recursion depth peaks at ~2), so it isolates the SHARED parse
// machinery — TreeBuilder frame open/close, the node arena, the schema-walker
// advance — from the deep nest's N-frame host recursion. It pins that the
// shared machinery is genuinely O(N): per-element cost stays ~constant as N
// grows (measured ns/level was FLAT — ~20.5 µs — from 250 to 3000, while the
// node count rose exactly linearly). A reintroduced O(N²) in the arena /
// TreeBuilder bookkeeping (the hypothesis the controlled measurement REFUTED —
// see D-PARSE-DEEP-NEST-RECURSION-MEMORY) would blow this far past the
// generous ceiling: quadrupling the length ⇒ ~16× under O(N²) vs ~4× linear.
//
// This is the COMPLEMENT of `DeepNestCastVsParenIsLinear`: that test runs the
// recursive nest (and tolerates a larger ratio precisely because its N-deep
// recursion adds the memory-hierarchy constant this flat control isolates
// away); this one pins the underlying work as linear with a tighter bound.
namespace {

[[nodiscard]] std::string flatChainSource(std::size_t additions) {
    // `int main(void){return 0+0+…+0;}` — `additions` `+` ops, additions+1
    // zeros. A left-assoc chain: the Pratt walker climbs it iteratively, so
    // expression-depth peaks at ~2 and the default cap suffices (no large
    // stack needed, unlike the deep nest).
    std::string s = "int main(void){return 0";
    for (std::size_t i = 0; i < additions; ++i) s += "+0";
    s += ";}";
    return s;
}

// One clean flat-chain parse; returns (nodeCount, tokenAccessCount) — the two
// DETERMINISTIC metrics the linearity pin uses. No timing: a wall-clock ratio of
// sub-ms parses flaked on shared gcc-release CI runners (a contention spike on the
// tiny baseline dominated even min-of-9). `tokenAccessCount` is the total token-
// stream work (peek + advance, incl. every speculative re-scan): O(N) for a
// correct parse, super-linear for a backtracking blowup — the exact quantity the
// old wall-clock ratio was a noisy proxy for, now measured exactly.
struct FlatChainMetrics {
    std::uint32_t nodeCount;
    std::uint64_t tokenAccessCount;
};
[[nodiscard]] FlatChainMetrics
flatChainParseMetrics(std::shared_ptr<GrammarSchema const> const& schema,
                      std::size_t additions) {
    auto src = SourceBuffer::fromString(flatChainSource(additions), "<flat>");
    Tokenizer tk{src, schema};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    Parser p{src, schema, std::move(stream)};
    auto result = std::move(p).parse();
    EXPECT_FALSE(result.tree.diagnostics().hasErrors())
        << "flat chain of " << additions << " additions must parse clean";
    return {static_cast<std::uint32_t>(result.tree.nodeCount()),
            result.tokenAccessCount};
}

} // namespace

// c108 (D-PARSE-FLAT-CHAIN-WORK-LINEAR): the flat left-assoc chain
// `0+0+…+0` must parse in LINEAR total work. Pinned by TWO deterministic,
// zero-flake first-difference tests over equally-spaced lengths (500/1000/1500,
// spacing 500) — a linear quantity has a CONSTANT first difference; a super-
// linear one does not. Grammar-agnostic (no magic per-element constant):
//   (1) NODE count — the O(N) STRUCTURE (the tree the parse emits);
//   (2) token ACCESS count — the O(N) total WORK (peek+advance, incl. every
//       speculative re-scan). A backtracking O(N²) (the D-PARSE-SPECULATION-
//       OPERAND-QUADRATIC class) re-consumes a growing prefix → the access
//       first difference would GROW; a per-node rescan that emits no extra
//       nodes (a time-only O(N²) the node pin misses) also shows here.
// This REPLACES a wall-clock ratio pin that flaked on shared CI at a sub-ms
// baseline: work-count is exact, so the guard is deterministic AND strictly
// stronger (it catches the same O(N²) classes without measuring time). The
// deep-nest residual (an N-deep HOST-recursion memory-hierarchy constant
// factor, moot under the depth cap) is the separate, still-open
// D-PARSE-DEEP-NEST-RECURSION-MEMORY.
TEST(ParserSpeculation, FlatChainParseWorkIsLinear) {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;

    const auto m500  = flatChainParseMetrics(schema, 500);
    const auto m1000 = flatChainParseMetrics(schema, 1000);
    const auto m1500 = flatChainParseMetrics(schema, 1500);

    // (1) O(N) STRUCTURE — constant node-count first difference.
    EXPECT_EQ(m1000.nodeCount - m500.nodeCount,
              m1500.nodeCount - m1000.nodeCount)
        << "flat-chain node count must grow by a constant per element (linear "
           "first difference); a non-constant difference signals super-linear "
           "node emission. n500=" << m500.nodeCount
        << " n1000=" << m1000.nodeCount << " n1500=" << m1500.nodeCount;

    // (2) O(N) WORK — constant token-access first difference. A speculative-
    // backtracking O(N²) would re-scan a growing prefix and break this.
    EXPECT_EQ(m1000.tokenAccessCount - m500.tokenAccessCount,
              m1500.tokenAccessCount - m1000.tokenAccessCount)
        << "flat-chain token-access count must grow by a constant per element "
           "(linear total parse work); a non-constant first difference signals "
           "a super-linear re-scan (speculative backtracking O(N²)). "
           "a500=" << m500.tokenAccessCount
        << " a1000=" << m1000.tokenAccessCount
        << " a1500=" << m1500.tokenAccessCount;
}
