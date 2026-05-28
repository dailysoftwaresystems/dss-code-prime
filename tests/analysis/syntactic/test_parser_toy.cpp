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
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>

using namespace dss;

namespace {

struct ToyHarness {
    std::shared_ptr<SourceBuffer>        src;
    std::shared_ptr<GrammarSchema const> schema;
    TokenStream                          stream;
};

[[nodiscard]] ToyHarness loadAndTokenize(std::string source) {
    auto loaded = GrammarSchema::loadShipped("toy");
    EXPECT_TRUE(loaded.has_value());
    auto schema = *loaded;
    auto src    = SourceBuffer::fromString(std::move(source), "<toy>");
    Tokenizer tk{src, schema};
    auto [stream, _] = std::move(tk).tokenize();
    return ToyHarness{
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

// Assert exact counts per code AND zero of every other parser-side
// code we care about. Whitelist covers the parser-emitted recovery
// codes PLUS the builder-emitted invariant/internal codes — silent-
// failure regressions in either layer surface as an unexpected
// non-zero count when the caller passes `{}`.
void expectExactCodes(std::span<ParseDiagnostic const> diags,
                      std::initializer_list<std::pair<DiagnosticCode, std::size_t>> expect) {
    std::unordered_map<int, std::size_t> wanted;
    for (auto const& [c, n] : expect) wanted[static_cast<int>(c)] = n;
    for (auto const code : {
            DiagnosticCode::P_UnexpectedToken,
            DiagnosticCode::P_MissingRequiredChild,
            DiagnosticCode::P_NoAlternativeMatched,
            DiagnosticCode::P_BacktrackFailed,
            DiagnosticCode::P_UnknownToken,
            DiagnosticCode::P_PrematureEndOfInput,
            DiagnosticCode::P_AmbiguousToken,
            DiagnosticCode::P_SchemaCursorDesync,
            DiagnosticCode::P_BuilderInvariant,
            DiagnosticCode::P_MaxSpeculationDepth,
            DiagnosticCode::P_UncommittedCheckpoint,
            DiagnosticCode::P_RecoveryStalled}) {
        const auto want = wanted.count(static_cast<int>(code))
                              ? wanted.at(static_cast<int>(code)) : 0u;
        EXPECT_EQ(countCode(diags, code), want)
            << "diagnostic code " << diagnosticCodeName(code)
            << ": expected " << want << " occurrences";
    }
}

// Count direct children of `parent` whose internal-node rule name
// matches `ruleName`. Returns 0 for token leaves.
[[nodiscard]] std::size_t countChildRule(Tree const& t, NodeId parent,
                                         std::string_view ruleName) {
    if (!t.hasSchema()) return 0;
    const auto ruleId = t.schema().rules().find(ruleName);
    if (!ruleId.valid()) return 0;
    std::size_t n = 0;
    for (auto c : t.children(parent)) {
        if (t.kind(c) == NodeKind::Internal && t.rule(c).v == ruleId.v) ++n;
    }
    return n;
}

} // namespace

// ── lexer-diagnostic folding (08-compilation-unit-plan §2.6 C2-L1) ────────

TEST(ParserToy, LexerDiagnosticsFoldedIntoTree) {
    // The optional 5th Parser ctor arg folds the tokenizer's lexer
    // diagnostics into the produced Tree. `@` is an illegal char →
    // P_IllegalChar from the lexer, which must appear in the Tree.
    auto loaded = GrammarSchema::loadShipped("toy");
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    auto src = SourceBuffer::fromString("var x : int = @;", "<toy>");
    Tokenizer tk{src, schema};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    Parser p{src, schema, std::move(stream), {}, std::move(lexDiags)};
    auto result = std::move(p).parse();

    EXPECT_GE(countCode(result.tree.diagnostics().all(),
                        DiagnosticCode::P_IllegalChar), 1u)
        << "lexer P_IllegalChar must be folded into the Tree when "
           "lexerDiagnostics is passed";
}

TEST(ParserToy, NullLexerDiagsDoesNotFoldLexerErrors) {
    // The default 4-arg ctor (lexerDiagnostics = nullptr) leaves lexer
    // diagnostics in the discarded tokenizer reporter — they do NOT appear
    // in the Tree. Backward-compat for existing callers (LSP, tests). The
    // parser still emits its own P_NoAlternativeMatched at the `@`.
    auto h = loadAndTokenize("var x : int = @;");   // discards lexer diags
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();

    EXPECT_EQ(countCode(result.tree.diagnostics().all(),
                        DiagnosticCode::P_IllegalChar), 0u)
        << "without lexerDiagnostics, lexer diags must NOT appear in the Tree";
    EXPECT_GE(countCode(result.tree.diagnostics().all(),
                        DiagnosticCode::P_NoAlternativeMatched), 1u);
}

// ── happy paths ─────────────────────────────────────────────────────────

TEST(ParserToy, HappyPath_SingleVarDecl) {
    auto h = loadAndTokenize("var x : int = y;");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    // Shape pin: root contains exactly one statement frame.
    EXPECT_EQ(countChildRule(t, t.root(), "topLevel"), 1u);
    expectExactCodes(t.diagnostics().all(), {});
}

TEST(ParserToy, HappyPath_FuncDef) {
    // The other top-level form; its body exercises a block, an expression
    // statement, and a return.
    auto h = loadAndTokenize("func f() -> int { x; return y; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_EQ(countChildRule(t, t.root(), "topLevel"), 1u);
    expectExactCodes(t.diagnostics().all(), {});
}

TEST(ParserToy, HappyPath_MultipleStatements) {
    auto h = loadAndTokenize("var x : int = a; var w : int = b; func f() -> void { x; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_EQ(countChildRule(t, t.root(), "topLevel"), 3u)
        << "three top-level frames expected (two var globals, one funcDef)";
    expectExactCodes(t.diagnostics().all(), {});
}

TEST(ParserToy, HappyPath_EmptySource) {
    // Toy root is `repeat topLevel`: nullable, so empty source must
    // parse cleanly with the root present but childless.
    auto h = loadAndTokenize("");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_EQ(countChildRule(t, t.root(), "topLevel"), 0u);
    expectExactCodes(t.diagnostics().all(), {});
}

// ── broken paths ────────────────────────────────────────────────────────

TEST(ParserToy, BrokenPath_UnknownTokenInExpressionPosition) {
    // `@` is illegal; the tokenizer emits a P_IllegalChar +
    // CoreTokenKind::Error token. The parser's `effectiveKind` maps
    // Error tokens to the schema's `Error` kind, which won't match
    // the cursor's expected set — so the parser intercepts with
    // `pushError` (emitting P_UnexpectedToken) before the builder's
    // pushToken would have synthesized its own P_UnknownToken.
    auto h = loadAndTokenize("var x : int = @;");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_TRUE(t.diagnostics().hasErrors());
    // `@` lands at the `expression` RuleLeaf slot; FIRST(expression)
    // doesn't contain Error kind and `expression` isn't nullable, so
    // the RuleLeaf handler emits P_NoAlternativeMatched (not the
    // TokenLeaf handler's P_UnexpectedToken).
    EXPECT_GE(countCode(t.diagnostics().all(),
                        DiagnosticCode::P_NoAlternativeMatched), 1u)
        << "parser must emit P_NoAlternativeMatched at the RuleLeaf miss";
}

TEST(ParserToy, BrokenPath_PrematureEofMidRule) {
    // `var x` truncates before `= expr ;`. The parser specifically
    // emits P_MissingRequiredChild at each frame it has to close
    // without seeing the required body.
    auto h = loadAndTokenize("var x");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_TRUE(t.diagnostics().hasErrors());
    EXPECT_GE(countCode(t.diagnostics().all(),
                        DiagnosticCode::P_MissingRequiredChild), 1u)
        << "parser must emit P_MissingRequiredChild on premature EOF";
}

TEST(ParserToy, BrokenPath_TokenNotInAnyFirstSet) {
    // `;` at top level isn't in FIRST(topLevel). Parser emits
    // exactly one P_NoAlternativeMatched and consumes — no
    // P_UnexpectedToken should fire (that's a TokenLeaf mismatch
    // signal, not an AltChoice signal).
    auto h = loadAndTokenize(";");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    expectExactCodes(t.diagnostics().all(),
                     {{DiagnosticCode::P_NoAlternativeMatched, 1u}});
}

// ── tighter coverage of dispatch paths ──────────────────────────────────

TEST(ParserToy, RuleLeafSkipNullable_DoesNotConsumeTokens) {
    // Empty source exercises the repeat-AltChoice's
    // takeNullableBranch (the "skip the loop" branch). Verify that
    // it does NOT emit any P_* diagnostic — skipping a nullable
    // branch is a clean path, not recovery.
    auto h = loadAndTokenize("");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    expectExactCodes(result.tree.diagnostics().all(), {});
}

TEST(ParserToy, NullableBranchNegativePin_DoesNotFireOnFirstMatch) {
    // When the AltChoice union DOES contain peek, the parser must
    // dispatch into the matching branch — NOT take the nullable
    // skip. `var x : int = y;` exercises the repeat's `innerStart`
    // branch (topLevel RuleLeaf) and must produce a top-level frame.
    auto h = loadAndTokenize("var x : int = y;");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    EXPECT_GE(countChildRule(result.tree, result.tree.root(), "topLevel"), 1u)
        << "non-empty input must take the body branch, not the skip";
    expectExactCodes(result.tree.diagnostics().all(), {});
}

// ── trivia coverage ─────────────────────────────────────────────────────

TEST(ParserToy, TriviaInteriorWhitespacePreservedAtAllSlots) {
    // Whitespace interleaved at every dispatch site: between
    // VarKeyword (TokenLeaf), Identifier (TokenLeaf), AssignmentOp
    // (TokenLeaf), expression (RuleLeaf entry), and EndCommand
    // (TokenLeaf). Each trivia push must not affect dispatch state.
    auto h = loadAndTokenize("var   x  :  int  =   y ;");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_EQ(countChildRule(t, t.root(), "topLevel"), 1u);
    expectExactCodes(t.diagnostics().all(), {});
}

TEST(ParserToy, LeadingAndTrailingTrivia) {
    // Head-of-stream drain at `parse()` entry + trailing trivia
    // after final statement. Pinned because the drain bypasses the
    // walker and a regression there could mis-align iteration 0's
    // watchdog seed.
    auto h = loadAndTokenize("\n\n  var x : int = y;\n\n  ");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_EQ(countChildRule(t, t.root(), "topLevel"), 1u);
    expectExactCodes(t.diagnostics().all(), {});
}

TEST(ParserToy, AllTriviaSource) {
    // Pure-whitespace input — head-of-stream drain consumes
    // everything; dispatch never runs a meaningful iteration; parse
    // terminates via root nullable-tail.
    auto h = loadAndTokenize("\n\n  \t\n  ");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_EQ(countChildRule(t, t.root(), "topLevel"), 0u);
    expectExactCodes(t.diagnostics().all(), {});
}

// ── death tests ─────────────────────────────────────────────────────────

TEST(ParserToyDeath, NullSchemaAborts) {
    auto src = SourceBuffer::fromString("", "<x>");
    TokenStream empty;
    EXPECT_DEATH(
        Parser(src, nullptr, std::move(empty)),
        "dss::Parser::Parser: schema is null");
}

TEST(ParserToyDeath, NullSourceAborts) {
    auto loaded = GrammarSchema::loadShipped("toy");
    ASSERT_TRUE(loaded.has_value());
    TokenStream empty;
    EXPECT_DEATH(
        Parser(nullptr, *loaded, std::move(empty)),
        "dss::Parser::Parser: source buffer is null");
}

TEST(ParserToyDeath, ZeroSpeculationDepthAborts) {
    auto loaded = GrammarSchema::loadShipped("toy");
    ASSERT_TRUE(loaded.has_value());
    auto src = SourceBuffer::fromString("", "<x>");
    TokenStream empty;
    ParserConfig cfg;
    cfg.maxSpeculationDepth = 0;
    EXPECT_DEATH(
        Parser(src, *loaded, std::move(empty), std::move(cfg)),
        "maxSpeculationDepth must be >= 1");
}

TEST(ParserToyDeath, ZeroExpressionDepthAborts) {
    auto loaded = GrammarSchema::loadShipped("toy");
    ASSERT_TRUE(loaded.has_value());
    auto src = SourceBuffer::fromString("", "<x>");
    TokenStream empty;
    ParserConfig cfg;
    cfg.maxExpressionDepth = 0;
    EXPECT_DEATH(
        Parser(src, *loaded, std::move(empty), std::move(cfg)),
        "maxExpressionDepth must be >= 1");
}
