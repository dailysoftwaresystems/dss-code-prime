#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/token.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

struct H {
    std::shared_ptr<SourceBuffer>        src;
    std::shared_ptr<GrammarSchema const> schema;
};

[[nodiscard]] H loadToy(std::string text) {
    auto loaded = GrammarSchema::loadShipped("toy");
    EXPECT_TRUE(loaded.has_value()) << "toy.lang.json load failed";
    return H{
        .src    = SourceBuffer::fromString(std::move(text), "<test>"),
        .schema = loaded.has_value() ? *loaded : nullptr,
    };
}

[[nodiscard]] H loadCSubset(std::string text) {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    EXPECT_TRUE(loaded.has_value()) << "c-subset.lang.json load failed";
    return H{
        .src    = SourceBuffer::fromString(std::move(text), "<test>"),
        .schema = loaded.has_value() ? *loaded : nullptr,
    };
}

// Collect every non-Eof token from a tokenize() call, plus the
// diagnostic reporter's contents. Caller asserts shape against the
// returned vectors.
struct LexResult {
    std::vector<Token>              tokens;
    std::vector<ParseDiagnostic>    diags;
};

[[nodiscard]] LexResult lex(H h) {
    Tokenizer t{h.src, h.schema};
    auto [stream, reporter] = std::move(t).tokenize();
    LexResult out;
    while (!stream.isAtEnd()) out.tokens.push_back(stream.advance());
    auto const all = reporter->all();
    out.diags.assign(all.begin(), all.end());
    return out;
}

[[nodiscard]] std::string_view textOf(SourceBuffer const& src, Token const& t) {
    return src.slice(t.span);
}

} // namespace

// ── empty / EOF ───────────────────────────────────────────────────────────

TEST(Tokenizer, EmptySourceEmitsOnlyEof) {
    auto h = loadToy("");
    Tokenizer t{h.src, h.schema};
    auto [stream, reporter] = std::move(t).tokenize();
    EXPECT_EQ(stream.size(), 1u);
    EXPECT_EQ(stream.peek().coreKind, CoreTokenKind::Eof);
    EXPECT_TRUE(reporter->all().empty());
}

TEST(Tokenizer, EofSpanIsZeroWidthAtEndOfBuffer) {
    auto h = loadToy("var");
    Tokenizer t{h.src, h.schema};
    auto [stream, reporter] = std::move(t).tokenize();
    while (!stream.isAtEnd()) (void)stream.advance();
    auto const eof = stream.advance();
    EXPECT_EQ(eof.coreKind, CoreTokenKind::Eof);
    EXPECT_EQ(eof.span.start(), 3u);
    EXPECT_EQ(eof.span.end(),   3u);
}

// ── whitespace ────────────────────────────────────────────────────────────

TEST(Tokenizer, WhitespaceRunsEmitOneTokenPerChar) {
    auto h     = loadToy("  \t\n");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 4u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::Whitespace);   // ' '
    EXPECT_EQ(result.tokens[1].coreKind, CoreTokenKind::Whitespace);   // ' '
    EXPECT_EQ(result.tokens[2].coreKind, CoreTokenKind::Whitespace);   // '\t'
    EXPECT_EQ(result.tokens[3].coreKind, CoreTokenKind::Newline);      // '\n'
}

TEST(Tokenizer, WhitespaceCarriesSchemaKindWithEmptySpaceFlag) {
    auto h      = loadToy(" ");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_TRUE(result.tokens[0].schemaKind.valid());
    EXPECT_EQ(result.tokens[0].schemaKind,
              h.schema->schemaTokens().find("Whitespace"));
    EXPECT_TRUE(h.schema->isEmptySpace(result.tokens[0].schemaKind));
}

// ── identifiers + keywords ────────────────────────────────────────────────

TEST(Tokenizer, KeywordResolvesToSchemaKind) {
    auto h      = loadToy("var");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::Word);
    EXPECT_EQ(result.tokens[0].schemaKind,
              h.schema->schemaTokens().find("VarKeyword"));
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "var");
}

TEST(Tokenizer, NonKeywordWordLeavesSchemaKindInvalidForBuilderFallback) {
    auto h      = loadToy("myVariable");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::Word);
    // Not a keyword in toy.lang.json — tokenizer leaves schemaKind
    // invalid so builder.pushToken's Identifier-fallback fires.
    EXPECT_FALSE(result.tokens[0].schemaKind.valid());
}

TEST(Tokenizer, IdentifiersAcceptUnderscoreAndDigits) {
    auto h      = loadToy("_x9");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::Word);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "_x9");
}

// ── numeric literals ──────────────────────────────────────────────────────

TEST(Tokenizer, IntegerLiteral) {
    auto h      = loadToy("12345");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(result.tokens[0].schemaKind,
              h.schema->schemaTokens().find("IntLiteral"));
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "12345");
}

TEST(Tokenizer, FloatLiteralWithFractionalPart) {
    auto h      = loadToy("3.14");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::FloatLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "3.14");
}

TEST(Tokenizer, FloatLiteralWithExponent) {
    auto h      = loadToy("1e10");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::FloatLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "1e10");
}

TEST(Tokenizer, FloatLiteralWithSignedExponent) {
    auto h      = loadToy("1.5e-3");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::FloatLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "1.5e-3");
}

TEST(Tokenizer, FloatLiteralWithFSuffixPromotesIntToFloat) {
    auto h      = loadToy("0f");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::FloatLiteral);
}

TEST(Tokenizer, BareENotConsumedAsExponentWhenNoDigitFollows) {
    // `1e+` is NOT a float — no digit after the sign. Tokenizer emits
    // exactly three tokens: IntLiteral("1"), Word("e"), Operator("+").
    auto h      = loadToy("1e+");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 3u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "1");
    EXPECT_EQ(result.tokens[1].coreKind, CoreTokenKind::Word);
    EXPECT_EQ(textOf(*h.src, result.tokens[1]), "e");
    EXPECT_EQ(result.tokens[2].coreKind, CoreTokenKind::Operator);
    EXPECT_EQ(textOf(*h.src, result.tokens[2]), "+");
}

TEST(Tokenizer, MemberAccessDotIsNotPartOfFloat) {
    // `a.b` must NOT be consumed as one float-like token. The `.` is
    // only fractional when it follows a digit AND a digit follows. Here
    // `a` starts a word.
    auto h      = loadCSubset("3.foo");
    auto result = lex(h);
    // Expected: IntLiteral("3"), Punctuation("."), Word("foo") — exactly 3.
    ASSERT_EQ(result.tokens.size(), 3u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "3");
    EXPECT_EQ(textOf(*h.src, result.tokens[2]), "foo");
}

TEST(Tokenizer, HexLiteralLexedAsOneIntToken) {
    // `0xff` MUST become one IntLiteral, not `0` + identifier `xff`.
    auto h      = loadCSubset("0xff");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "0xff");
}

TEST(Tokenizer, HexLiteralWithUnderscoreSeparators) {
    auto h      = loadCSubset("0xDEAD_BEEF");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "0xDEAD_BEEF");
}

TEST(Tokenizer, BinaryLiteralLexedAsOneIntToken) {
    auto h      = loadCSubset("0b1010_0101");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "0b1010_0101");
}

TEST(Tokenizer, OctalLiteralLexedAsOneIntToken) {
    auto h      = loadCSubset("0o755");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "0o755");
}

TEST(Tokenizer, ZeroFollowedByLetterIsNotHexUnlessLetterIsValidDigit) {
    // `0xy` is NOT a hex literal because `y` isn't a hex digit. The
    // tokenizer falls back to plain decimal `0`, then `xy` as a word.
    auto h      = loadCSubset("0xy");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 2u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "0");
    EXPECT_EQ(result.tokens[1].coreKind, CoreTokenKind::Word);
    EXPECT_EQ(textOf(*h.src, result.tokens[1]), "xy");
}

TEST(Tokenizer, NumericSuffixRestrictedToTypeLetters) {
    // `123abc` was a single-token regression risk pre-review-fix. The
    // suffix scanner now only accepts u/U/l/L/f/F/d/D, so `123abc`
    // becomes `123` (Int) + `abc` (Word).
    auto h      = loadCSubset("123abc");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 2u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "123");
    EXPECT_EQ(result.tokens[1].coreKind, CoreTokenKind::Word);
    EXPECT_EQ(textOf(*h.src, result.tokens[1]), "abc");
}

TEST(Tokenizer, IntegerWithULSuffixStillIntegerKind) {
    // `42ull` — recognised C unsigned-long-long suffix. Stays IntLiteral.
    auto h      = loadCSubset("42ull");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "42ull");
}

// ── operators / punctuation ───────────────────────────────────────────────

TEST(Tokenizer, SingleCharOperatorResolves) {
    auto h      = loadToy("+");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::Operator);
    // `+` has two meanings in toy; tokenizer picks priority 10
    // (SumOperator wins over StringAppendOperator priority 20).
    EXPECT_EQ(result.tokens[0].schemaKind,
              h.schema->schemaTokens().find("SumOperator"));
}

TEST(Tokenizer, PunctuationVsOperatorCoreKindSplit) {
    auto h      = loadToy("{};(");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 4u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::Punctuation);   // {
    EXPECT_EQ(result.tokens[1].coreKind, CoreTokenKind::Punctuation);   // }
    EXPECT_EQ(result.tokens[2].coreKind, CoreTokenKind::Punctuation);   // ;
    EXPECT_EQ(result.tokens[3].coreKind, CoreTokenKind::Punctuation);   // (
}

TEST(Tokenizer, LongestMatchPicks2CharOperatorOverPrefix) {
    auto h      = loadCSubset("==");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::Operator);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "==");
    EXPECT_EQ(result.tokens[0].schemaKind,
              h.schema->schemaTokens().find("EqEqOp"));
}

TEST(Tokenizer, LongestMatchDistinguishesAdjacentLexemes) {
    // `===` is `==` followed by `=` in c-subset (no `===` lexeme
    // declared). Longest-match consumes `==` greedily.
    auto h      = loadCSubset("===");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 2u);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "==");
    EXPECT_EQ(textOf(*h.src, result.tokens[1]), "=");
    EXPECT_EQ(result.tokens[1].schemaKind,
              h.schema->schemaTokens().find("AssignOp"));
}

// ── error recovery ────────────────────────────────────────────────────────

TEST(Tokenizer, IllegalCharEmitsErrorTokenAndContinues) {
    auto h      = loadToy("$x");
    auto result = lex(h);
    // `$` isn't in toy's tokens or identifier-start set. Expected:
    // Error("$"), Word("x"), Eof.
    ASSERT_EQ(result.tokens.size(), 2u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::Error);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "$");
    EXPECT_EQ(result.tokens[1].coreKind, CoreTokenKind::Word);
    EXPECT_EQ(textOf(*h.src, result.tokens[1]), "x");

    ASSERT_EQ(result.diags.size(), 1u);
    EXPECT_EQ(result.diags[0].code, DiagnosticCode::P_IllegalChar);
    EXPECT_EQ(result.diags[0].severity, DiagnosticSeverity::Error);
}

TEST(Tokenizer, MultipleIllegalCharsEmitSeparateTokensAndDiagnostics) {
    auto h      = loadToy("$@!");
    auto result = lex(h);
    // None of these are in toy. (`!` isn't either.)
    EXPECT_EQ(result.tokens.size(), 3u);
    for (auto const& tok : result.tokens) {
        EXPECT_EQ(tok.coreKind, CoreTokenKind::Error);
    }
    EXPECT_EQ(result.diags.size(), 3u);
}

// ── UTF-8 ─────────────────────────────────────────────────────────────────

TEST(Tokenizer, Utf8BomAtStartIsSkippedSilently) {
    // Some editors prepend the UTF-8 BOM (EF BB BF). The tokenizer
    // must NOT treat it as a 3-byte identifier (the prior `isIdStart`
    // accepted any byte ≥ 0x80). Source "\xEF\xBB\xBFvar" should
    // tokenize as just `var`.
    auto h      = loadToy("\xEF\xBB\xBF""var");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::Word);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "var");
    EXPECT_TRUE(result.diags.empty());
}

TEST(Tokenizer, LoneUtf8ContinuationByteIsIllegal) {
    // 0xA9 alone (a continuation byte appearing without a leading byte)
    // is malformed UTF-8. Tokenizer emits Error + P_IllegalChar rather
    // than silently classifying it as an identifier.
    auto h      = loadToy("\xA9");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::Error);
    ASSERT_EQ(result.diags.size(), 1u);
    EXPECT_EQ(result.diags[0].code, DiagnosticCode::P_IllegalChar);
}

TEST(Tokenizer, MultiByteUtf8IdentifierAcceptedAsOneToken) {
    // A 2-byte UTF-8 sequence (`é` = C3 A9) is a valid identifier in
    // the byte-pass-through model — the schema's lexeme keys are byte
    // strings, so multi-byte chars in identifiers are accepted as one
    // contiguous Word token.
    auto h      = loadToy("\xC3\xA9x");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::Word);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "\xC3\xA9x");
    EXPECT_TRUE(result.diags.empty());
}

// ── integration: real source ──────────────────────────────────────────────

TEST(Tokenizer, FullVarDeclProducesExpectedStream) {
    auto h      = loadToy("var x = y;");
    auto result = lex(h);
    // var, ' ', x, ' ', =, ' ', y, ;
    ASSERT_EQ(result.tokens.size(), 8u);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "var");
    EXPECT_EQ(textOf(*h.src, result.tokens[1]), " ");
    EXPECT_EQ(textOf(*h.src, result.tokens[2]), "x");
    EXPECT_EQ(textOf(*h.src, result.tokens[3]), " ");
    EXPECT_EQ(textOf(*h.src, result.tokens[4]), "=");
    EXPECT_EQ(textOf(*h.src, result.tokens[5]), " ");
    EXPECT_EQ(textOf(*h.src, result.tokens[6]), "y");
    EXPECT_EQ(textOf(*h.src, result.tokens[7]), ";");
    EXPECT_TRUE(result.diags.empty());
}

TEST(Tokenizer, SchemaKindsMatchExpectedForVarDecl) {
    auto h      = loadToy("var x = y;");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 8u);
    auto const& sch = h.schema->schemaTokens();
    EXPECT_EQ(result.tokens[0].schemaKind, sch.find("VarKeyword"));
    EXPECT_FALSE(result.tokens[2].schemaKind.valid());      // `x` = builder→Identifier
    EXPECT_EQ(result.tokens[4].schemaKind, sch.find("AssignmentOperator"));
    EXPECT_FALSE(result.tokens[6].schemaKind.valid());      // `y` = builder→Identifier
    EXPECT_EQ(result.tokens[7].schemaKind, sch.find("EndCommand"));
}

// ── ctor invariants ───────────────────────────────────────────────────────
//
// EXPECT_DEATH is a macro; commas in the body land in macro-arg territory.
// Helper free functions keep the action expression comma-free.

namespace {
void makeWithNullSource(std::shared_ptr<GrammarSchema const> schema) {
    Tokenizer t{nullptr, std::move(schema)};
    (void)t;
}

void makeWithNullSchema(std::shared_ptr<SourceBuffer> src) {
    Tokenizer t{std::move(src), nullptr};
    (void)t;
}
} // namespace

TEST(TokenizerDeath, NullSourceAborts) {
    auto loaded = GrammarSchema::loadShipped("toy");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_DEATH(makeWithNullSource(*loaded),
                 "dss::Tokenizer fatal: source is null");
}

TEST(TokenizerDeath, NullSchemaAborts) {
    auto src = SourceBuffer::fromString("", "<test>");
    EXPECT_DEATH(makeWithNullSchema(src),
                 "dss::Tokenizer fatal: schema is null");
}
