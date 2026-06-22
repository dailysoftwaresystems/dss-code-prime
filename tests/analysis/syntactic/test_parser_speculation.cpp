#include "analysis/syntactic/parser.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
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
        "vaStartExpr",           // FC12a-core (D-FC12A-VARIADIC-CALLEE)
        "vaArgExpr",             // FC12a-core
        "vaEndExpr",             // FC12a-core
        "castExpr",
        "parenExpr",
    };
    EXPECT_EQ(names, declared)
        << "operand's speculative rule branches must surface in the "
           "JSON-array order declared in c-subset.lang.json";
}
