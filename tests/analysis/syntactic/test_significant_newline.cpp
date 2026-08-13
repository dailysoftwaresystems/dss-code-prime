// A language may declare its newline SIGNIFICANT — the unit-tier guard.
//
// ★★★ WHAT THIS PINS AND WHY IT NEEDS ITS OWN FILE. Two independent hardcodes
// made the `tokens` map's `"\n"` entry a knob that lies, and each one alone is
// enough to make a line boundary vanish:
//   1. `tokenizer.cpp` special-cased `'\n'` and emitted
//      `schemaTokens().find("Newline")` UNCONDITIONALLY, never consulting the
//      lexeme table (D-TOKENIZER-NEWLINE-LEXEME-HARDCODED);
//   2. `parser.cpp`'s `isSkippableTrivia` returned true for
//      `CoreTokenKind::Newline` UNCONDITIONALLY, so even a correctly-kinded
//      token was skipped before any rule could name it
//      (D-PARSER-TRIVIA-KEYED-ON-CORE-KIND-NOT-CONFIG).
// Neither is an `if (lang == …)` and neither shows up in the agnosticism grep —
// the identity they branch on is a TOKEN KIND. They were invisible for as long
// as all three shipped languages agreed with the hardcode, which they did.
//
// ★★ THE COST WAS A WRONG PARSE, NOT A PARSE ERROR, which is the whole reason
// this file exists rather than a smoke test. With the newline invisible, an
// assembly `ret` on one line took the NEXT line's mnemonic as its operand and
// the file still parsed — clean build, wrong program. `TwoStatementsDoNotMerge`
// below is the arm that catches exactly that, and it is the one that would have
// failed on the pre-fix tree.
//
// ★ THE THREE CONTROLS ARE NOT PADDING. A fix that simply inverted the default
// would pass the significant-newline arm and break every other language, so the
// two "still trivia" arms are what pin that the change is a RE-SCOPING and not
// a flip: a language that declares `"\n"` AS `EmptySpace` must still skip it,
// and a language that declares no `"\n"` at all must keep the historical
// core-kind default byte-for-byte. That third case is the subtle one — it is
// why `declaredLexemeTokens` exists as a set separate from `emptySpaceTokens`
// (see the anchor row).

#include "analysis/syntactic/parser.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/tree.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

// A two-token language whose only interesting property is what it says about
// `"\n"`. `newlineDecl` is spliced in verbatim so each arm differs by EXACTLY
// that one declaration — an arm that also changed the grammar would not be
// measuring the newline.
[[nodiscard]] std::string schemaJson(std::string_view newlineDecl,
                                     std::string_view rootShape) {
    std::string doc = R"({
      "dssSchemaVersion": 4,
      "language": { "name": "NlProbe", "version": "0.0.1", "fileExtensions": [".nl"] },
      "tokens": {
        " ": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
        "!": [{ "kind": "Bang" }])";
    if (!newlineDecl.empty()) {
        doc += ",\n        ";
        doc += newlineDecl;
    }
    doc += R"(
      },
      "shapes": {
        "root": )";
    doc += rootShape;
    doc += R"(
      }
    })";
    return doc;
}

[[nodiscard]] std::shared_ptr<GrammarSchema> load(std::string const& json) {
    auto loaded = GrammarSchema::loadFromText(json, "<nl-probe>");
    if (!loaded) {
        ADD_FAILURE() << "synthetic newline schema failed to load";
        return nullptr;
    }
    return *loaded;
}

// Every token the tokenizer produced, so an arm can assert on the STREAM
// independently of what the parser later does with it.
[[nodiscard]] std::vector<Token> lex(std::shared_ptr<GrammarSchema> schema,
                                     std::string source) {
    auto src = SourceBuffer::fromString(std::move(source), "<nl-src>");
    Tokenizer tk{src, std::move(schema)};
    auto [stream, diags] = std::move(tk).tokenize();
    (void)diags;
    std::vector<Token> out;
    while (!stream.isAtEnd()) out.push_back(stream.advance());
    return out;
}

[[nodiscard]] ParseResult parse(std::shared_ptr<GrammarSchema> schema,
                                std::string source) {
    auto src = SourceBuffer::fromString(std::move(source), "<nl-src>");
    Tokenizer tk{src, schema};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    Parser p{std::move(src), std::move(schema), std::move(stream),
             ParserConfig{}, std::move(lexDiags)};
    return std::move(p).parse();
}

constexpr std::string_view kSignificant = R"("\n": [{ "kind": "LineEnd" }])";
constexpr std::string_view kTrivia =
    R"("\n": [{ "kind": "LineEnd", "flags": ["EmptySpace"] }])";

// One `!` per line, newline-terminated — the minimal shape of a line-oriented
// language. NON-nullable per line, so the repeat terminates.
constexpr std::string_view kLineRoot =
    R"({ "repeat": { "sequence": ["Bang", "LineEnd"] } })";

} // namespace

// ── tier 1: the TOKENIZER honours the declared kind ──────────────────────────

TEST(SignificantNewline, TokenizerEmitsTheDeclaredKindNotTheBuiltIn) {
    auto schema = load(schemaJson(kSignificant, kLineRoot));
    ASSERT_TRUE(schema);
    const auto lineEnd = schema->schemaTokens().find("LineEnd");
    ASSERT_TRUE(lineEnd.valid())
        << "the probe schema must intern its own newline kind";

    auto const tokens = lex(schema, "!\n");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[1].coreKind, CoreTokenKind::Newline)
        << "the CORE kind stays Newline — only the SCHEMA kind is configurable";
    // ★ THE ASSERTION THE HARDCODE FAILED. Pre-fix the tokenizer emitted
    // `find("Newline")`, which this schema never declares, so the id was the
    // built-in's (or invalid) rather than `LineEnd`.
    EXPECT_EQ(tokens[1].schemaKind.v, lineEnd.v)
        << "the tokenizer ignored the language's own `\"\\n\"` declaration";
    EXPECT_FALSE(schema->isEmptySpace(tokens[1].schemaKind))
        << "a newline declared without `EmptySpace` must not be trivia";
}

// ── tier 2: the PARSER does not skip it ──────────────────────────────────────

TEST(SignificantNewline, ParserMatchesADeclaredSignificantNewline) {
    auto schema = load(schemaJson(kSignificant, kLineRoot));
    ASSERT_TRUE(schema);
    auto result = parse(schema, "!\n!\n");
    EXPECT_FALSE(result.tree.hasDiagnostics()
                 && result.tree.diagnostics().hasErrors())
        << "a grammar naming its own newline must parse a newline-terminated "
           "file";
}

// ★★★ THE ARM THAT CATCHES THE WRONG PARSE. Two complete lines must stay two
// statements. With the newline invisible the SECOND `!` is consumed as though
// it followed the first directly, so a grammar requiring a terminator after
// every `Bang` sees `Bang Bang` and fails — and a grammar that TOLERATES the
// run (assembly's operand list did) silently merges the lines instead. Either
// way the line boundary is gone, which is the defect; asserting the clean parse
// of a two-line file is what pins it.
TEST(SignificantNewline, TwoStatementsDoNotMerge) {
    auto schema = load(schemaJson(kSignificant, kLineRoot));
    ASSERT_TRUE(schema);

    auto const tokens = lex(schema, "!\n!\n");
    ASSERT_EQ(tokens.size(), 4u)
        << "both newlines must survive as their own tokens";
    const auto lineEnd = schema->schemaTokens().find("LineEnd");
    EXPECT_EQ(tokens[1].schemaKind.v, lineEnd.v);
    EXPECT_EQ(tokens[3].schemaKind.v, lineEnd.v);

    // And the tree agrees: two `LineEnd` leaves reached it. A skipped-as-trivia
    // newline is still PUSHED into the tree by the builder, so counting tree
    // leaves alone would not discriminate — the discriminating fact is that the
    // parse is clean under a grammar that REQUIRES the terminator.
    auto result = parse(schema, "!\n!\n");
    EXPECT_FALSE(result.tree.hasDiagnostics()
                 && result.tree.diagnostics().hasErrors())
        << "the two lines merged — the newline was skipped as trivia";
}

// ── the controls: the change is a re-scoping, not a flip ─────────────────────

TEST(SignificantNewline, ADeclaredEmptySpaceNewlineIsStillTrivia) {
    // The ordinary case — every shipped language. Same grammar as above but
    // WITHOUT the `LineEnd` requirement, so a skipped newline is what makes it
    // parse.
    auto schema = load(schemaJson(kTrivia, R"({ "repeat": "Bang" })"));
    ASSERT_TRUE(schema);
    auto const tokens = lex(schema, "!\n!\n");
    ASSERT_EQ(tokens.size(), 4u);
    EXPECT_TRUE(schema->isEmptySpace(tokens[1].schemaKind))
        << "a newline declared WITH `EmptySpace` must stay trivia — a fix that "
           "merely inverted the default would break every shipped language";
    auto result = parse(schema, "!\n!\n");
    EXPECT_FALSE(result.tree.hasDiagnostics()
                 && result.tree.diagnostics().hasErrors());
}

TEST(SignificantNewline, AnUndeclaredNewlineKeepsTheCoreKindDefault) {
    // ★★ THE SUBTLE CONTROL, and the reason `declaredLexemeTokens` is a set
    // separate from `emptySpaceTokens`. A schema that never mentions `"\n"` is
    // ABSENT from `emptySpaceTokens` for the same reason a deliberately-
    // significant one is. Reading absence as "significant" would make every
    // synthetic test schema in this repo that omits `"\n"` suddenly start
    // seeing newline tokens — a broad, silent behaviour change dressed up as a
    // config-driven fix.
    auto schema = load(schemaJson("", R"({ "repeat": "Bang" })"));
    ASSERT_TRUE(schema);
    auto result = parse(schema, "!\n!\n");
    EXPECT_FALSE(result.tree.hasDiagnostics()
                 && result.tree.diagnostics().hasErrors())
        << "a language that declares no newline must keep the historical "
           "skip-it-as-trivia default";
}
