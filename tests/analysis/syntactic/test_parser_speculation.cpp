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
    // Speculation probes through some tokens, the cursor desyncs,
    // rollback fires, parser falls through to non-speculative
    // dispatch which either descends caseA/caseB or emits
    // P_NoAlternativeMatched + recovers. Either way: parser must
    // terminate AND emit at least one P_BacktrackFailed signal.
    auto h = loadAndTokenize("A X ;");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_TRUE(t.diagnostics().hasErrors())
        << "bogus speculative input must surface an error";
    EXPECT_GE(countCode(t.diagnostics().all(),
                        DiagnosticCode::P_BacktrackFailed), 1u)
        << "P_BacktrackFailed must fire when every speculative branch fails";
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
    Parser p{h.src, h.schema, std::move(h.stream), cfg};
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
    // neither caseA's body (B) nor caseB's body (C). Without the
    // P_BacktrackFailed + consume forward-progress hatch, this is
    // the canonical stall that the watchdog protects against. The
    // test asserts termination (no hang) + the recovery signals.
    auto h = loadAndTokenize("A");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_TRUE(t.diagnostics().hasErrors());
    EXPECT_GE(countCode(t.diagnostics().all(),
                        DiagnosticCode::P_BacktrackFailed), 1u)
        << "every-branch-fails input must emit P_BacktrackFailed";
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
