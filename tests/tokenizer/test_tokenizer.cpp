#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/token.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_builder.hpp"
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

TEST(Tokenizer, EmptyHexPrefixFallsBackToZeroPlusWord) {
    // `0x` alone (no digit, no underscore, EOF after) must NOT be
    // consumed as a hex literal. Tokenizer emits `0` (Int) then `x`
    // (Word). Companion to ZeroFollowedByLetterIsNotHexUnlessLetterIsValidDigit
    // for the EOF-after-prefix case.
    auto h      = loadCSubset("0x");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 2u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "0");
    EXPECT_EQ(result.tokens[1].coreKind, CoreTokenKind::Word);
    EXPECT_EQ(textOf(*h.src, result.tokens[1]), "x");
    EXPECT_TRUE(result.diags.empty());
}

TEST(Tokenizer, HexPrefixFollowedByUnderscoreOnlyEmitsMalformedDiagnostic) {
    // `0x_` has no actual hex digits — only the underscore that the
    // base-prefix branch accepts as a body char. The tokenizer still
    // emits a single IntLiteral spanning `0x_` (so the source span is
    // covered) but flags it with P_MalformedNumber for the downstream
    // value parser. Companion: `0b_`, `0o_` behave identically.
    auto h      = loadCSubset("0x_");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "0x_");
    ASSERT_EQ(result.diags.size(), 1u);
    EXPECT_EQ(result.diags[0].code, DiagnosticCode::P_MalformedNumber);
    EXPECT_EQ(result.diags[0].severity, DiagnosticSeverity::Error);
}

TEST(Tokenizer, BinaryPrefixFollowedByUnderscoreOnlyIsMalformed) {
    auto h      = loadCSubset("0b___");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "0b___");
    ASSERT_EQ(result.diags.size(), 1u);
    EXPECT_EQ(result.diags[0].code, DiagnosticCode::P_MalformedNumber);
}

TEST(Tokenizer, ValidHexWithLeadingUnderscoreThenDigitIsAccepted) {
    // `0x_ff` is well-formed: the underscore is a digit-separator and
    // `ff` provides actual hex digits. No P_MalformedNumber.
    auto h      = loadCSubset("0x_ff");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "0x_ff");
    EXPECT_TRUE(result.diags.empty());
}

TEST(Tokenizer, ZeroFollowedByLetterPinsSchemaKindAsIntLiteral) {
    // Tightens the previous `ZeroFollowedByLetter...` test: also pin
    // the `0` token's schemaKind to the IntLiteral built-in. Catches
    // a regression where decimal `0` is mis-tagged.
    auto h      = loadCSubset("0xy");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 2u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(result.tokens[0].schemaKind,
              h.schema->schemaTokens().find("IntLiteral"));
}

// ── UTF-8 boundary tests (D1) ────────────────────────────────────────────

TEST(Tokenizer, Utf8LeadByteBoundary_0xC1IsRejected) {
    // 0xC1 is in the reserved range 0xC0..0xC1 — never appears in
    // well-formed UTF-8. Tokenizer routes it through the illegal-char
    // path rather than starting an identifier.
    auto h      = loadToy("\xC1");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::Error);
    ASSERT_EQ(result.diags.size(), 1u);
    EXPECT_EQ(result.diags[0].code, DiagnosticCode::P_IllegalChar);
}

TEST(Tokenizer, Utf8LeadByteBoundary_0xC2IsAcceptedAsIdStart) {
    // 0xC2 is the smallest valid UTF-8 lead byte. Combined with a
    // continuation byte it forms a 2-byte codepoint; the tokenizer
    // accepts the pair as a single Word token.
    auto h      = loadToy("\xC2\xA0");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::Word);
    EXPECT_TRUE(result.diags.empty());
}

TEST(Tokenizer, Utf8FourByteLeadAcceptedAsIdStart) {
    // 0xF0 starts a 4-byte UTF-8 sequence (e.g., emoji). Tokenizer
    // accepts the lead byte as identifier-start; subsequent
    // continuation bytes are picked up by `isIdContinue`.
    auto h      = loadToy("\xF0\x9F\x98\x80");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::Word);
    EXPECT_TRUE(result.diags.empty());
}

TEST(Tokenizer, MidSourceBomBytesAreAbsorbedByIdentifierScan) {
    // Stray BOM bytes mid-source are indistinguishable from a
    // legitimate U+FEFF codepoint. The byte-pass-through identifier
    // model absorbs the three BOM bytes into whatever identifier
    // scan is in flight — `var\xEF\xBB\xBFx` produces ONE Word token
    // spanning all 7 bytes. Documenting this here as a deliberate
    // design choice (see tokenizer.cpp BOM-handling comment). Full
    // Unicode identifier semantics would let us reject control-class
    // codepoints; that's v3 territory.
    auto h      = loadToy("var\xEF\xBB\xBFx");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::Word);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "var\xEF\xBB\xBFx");
    EXPECT_TRUE(result.diags.empty());
}

// ── resolveMeaning fast-path ambiguity (CR1) ──────────────────────────────
//
// The TZ1 review-fix added a same-priority ambiguity scan to the
// tokenizer-pre-resolved fast-path in `tree_builder.cpp::resolveMeaning`.
// Toy's `+` has different priorities (10 vs 20), so the existing
// `SingleCharOperatorResolves` test never exercises the ambiguity
// branch. Use an inline schema with two same-priority meanings for
// the same lexeme to pin the warning end-to-end through tokenizer
// → builder.

namespace {

constexpr std::string_view kAmbiguousSchema = R"JSON({
  "dssSchemaVersion": 1,
  "language": { "name": "Ambig", "version": "0.1.0" },
  "tokens": {
    "?": [
      { "kind": "MeaningA", "priority": 10 },
      { "kind": "MeaningB", "priority": 10 }
    ]
  },
  "shapes": { "root": { "sequence": [ "MeaningA" ] } }
})JSON";

} // namespace

TEST(Tokenizer, AmbiguousMeaningsEmitWarningViaFastPath) {
    auto loaded = GrammarSchema::loadFromText(kAmbiguousSchema);
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    auto src = SourceBuffer::fromString("?", "<ambig>");

    Tokenizer tk{src, schema};
    auto res = std::move(tk).tokenize();
    ASSERT_FALSE(res.stream.isAtEnd());

    // The tokenizer-resolved schemaKind is the priority-winner
    // (first declared on tie = MeaningA). Drive the token through
    // the builder; expect P_AmbiguousToken to fire via the fast-
    // path's same-priority scan.
    TreeBuilder b{src, schema};
    {
        auto root = b.open(schema->rules().find("root"));
        b.pushToken(res.stream.advance());
    }
    Tree t = std::move(b).finish();

    const auto diags = t.diagnostics().all();
    std::size_t ambiguousCount = 0;
    for (auto const& d : diags) {
        if (d.code == DiagnosticCode::P_AmbiguousToken) ++ambiguousCount;
    }
    EXPECT_EQ(ambiguousCount, 1u)
        << "fast-path ambiguity scan must emit one P_AmbiguousToken";
}

TEST(TokenStream, DefaultThenAssignedTokenStreamWorks) {
    // The E2EHarness pattern depends on TokenStream being default-
    // constructible AND survivable through move-assignment from a
    // real Tokenizer output. Pin that pattern explicitly.
    TokenStream s;
    {
        auto h = loadToy("var");
        Tokenizer t{h.src, h.schema};
        s = std::move(t).tokenize().stream;
    }
    // After move-assignment from a Tokenizer-produced stream, every
    // method works normally.
    EXPECT_EQ(s.peek().coreKind, CoreTokenKind::Word);
    EXPECT_EQ(s.advance().coreKind, CoreTokenKind::Word);
    EXPECT_TRUE(s.isAtEnd());
    EXPECT_EQ(s.advance().coreKind, CoreTokenKind::Eof);
}


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

// ── TZ2: lexer modes + string/comment body lexing ─────────────────────────
//
// TZ2 wires the LexerModeStack into Tokenizer. The body-mode branch
// honors `stringStyle.endsAt` / `escapeKind` / `endsAtLongestMatch`
// and the new `defaultToken.flags` field on `LexerMode`. Comment
// modes in c-subset (line-comment + block-comment) close
// v2-gap-catalog row 3's authoring task.

namespace {

[[nodiscard]] H loadTsql(std::string text) {
    auto loaded = GrammarSchema::loadShipped("tsql-subset");
    EXPECT_TRUE(loaded.has_value()) << "tsql-subset.lang.json load failed";
    return H{
        .src    = SourceBuffer::fromString(std::move(text), "<test>"),
        .schema = loaded.has_value() ? *loaded : nullptr,
    };
}

} // namespace

TEST(Tokenizer, LineCommentEmitsOpenerThenCommentCharsToNewline) {
    // c-subset line-comment mode: `//` pushes mode; everything up to
    // the next `\n` becomes a CommentChar; the `\n` consumed as the
    // closer (last CommentChar in body mode). Tokenizer should then
    // be back in main mode for any trailing source.
    auto h      = loadCSubset("// hi\nvar x;");
    auto result = lex(h);
    // Tokens: LineCommentStart, ' ', 'h', 'i', '\n', Word("var"),
    // ' ', Word("x"), Punctuation(';')  ⇒ 9
    ASSERT_EQ(result.tokens.size(), 9u);
    EXPECT_EQ(result.tokens[0].schemaKind,
              h.schema->schemaTokens().find("LineCommentStart"));
    // Body chars all share the CommentChar schemaKind.
    const auto commentCharKind = h.schema->schemaTokens().find("CommentChar");
    EXPECT_EQ(result.tokens[1].schemaKind, commentCharKind);
    EXPECT_EQ(result.tokens[2].schemaKind, commentCharKind);
    EXPECT_EQ(result.tokens[3].schemaKind, commentCharKind);
    EXPECT_EQ(result.tokens[4].schemaKind, commentCharKind);
    // After the comment body closes, we're back in main mode.
    EXPECT_EQ(result.tokens[5].coreKind, CoreTokenKind::Word);
    EXPECT_EQ(textOf(*h.src, result.tokens[5]), "var");
    EXPECT_TRUE(result.diags.empty());
}

TEST(Tokenizer, BlockCommentEmitsOpenerCharsAndClosing) {
    auto h      = loadCSubset("/* hi */var");
    auto result = lex(h);
    // BlockCommentStart, ' ', 'h', 'i', ' ', '*/' (closer chunked
    // as one defaultToken because endsAt is 2 bytes), Word("var")
    ASSERT_EQ(result.tokens.size(), 7u);
    EXPECT_EQ(result.tokens[0].schemaKind,
              h.schema->schemaTokens().find("BlockCommentStart"));
    const auto commentCharKind = h.schema->schemaTokens().find("CommentChar");
    EXPECT_EQ(result.tokens[1].schemaKind, commentCharKind);
    EXPECT_EQ(textOf(*h.src, result.tokens[1]), " ");
    EXPECT_EQ(result.tokens[2].schemaKind, commentCharKind);
    EXPECT_EQ(textOf(*h.src, result.tokens[2]), "h");
    EXPECT_EQ(result.tokens[3].schemaKind, commentCharKind);
    EXPECT_EQ(textOf(*h.src, result.tokens[3]), "i");
    EXPECT_EQ(result.tokens[4].schemaKind, commentCharKind);
    EXPECT_EQ(textOf(*h.src, result.tokens[4]), " ");
    EXPECT_EQ(result.tokens[5].schemaKind, commentCharKind);
    EXPECT_EQ(textOf(*h.src, result.tokens[5]), "*/");
    EXPECT_EQ(result.tokens[5].span.length(), 2u);
    EXPECT_EQ(result.tokens[6].coreKind, CoreTokenKind::Word);
    EXPECT_EQ(textOf(*h.src, result.tokens[6]), "var");
    EXPECT_TRUE(result.diags.empty());
}

TEST(Tokenizer, UnterminatedLineCommentEmitsDiagnostic) {
    // Line comment that runs to EOF without a newline. Tokenizer
    // emits the opener + 11 body chars (` no newline` = 11 bytes) +
    // a P_UnterminatedComment when EOF is reached with the frame
    // stack still open.
    auto h      = loadCSubset("// no newline");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 12u);
    EXPECT_EQ(result.tokens[0].schemaKind,
              h.schema->schemaTokens().find("LineCommentStart"));
    const auto commentCharKind = h.schema->schemaTokens().find("CommentChar");
    for (std::size_t i = 1; i < result.tokens.size(); ++i) {
        EXPECT_EQ(result.tokens[i].schemaKind, commentCharKind)
            << "token " << i << " should be CommentChar";
    }
    ASSERT_EQ(result.diags.size(), 1u);
    EXPECT_EQ(result.diags[0].code, DiagnosticCode::P_UnterminatedComment);
}

TEST(Tokenizer, UnterminatedBlockCommentEmitsDiagnostic) {
    auto h      = loadCSubset("/* nope");
    auto result = lex(h);
    ASSERT_EQ(result.diags.size(), 1u);
    EXPECT_EQ(result.diags[0].code, DiagnosticCode::P_UnterminatedComment);
}

TEST(Tokenizer, TsqlSingleStringDoubledDelimiterEscape) {
    // SQL string with a doubled single-quote — `'a''b'` represents the
    // literal `a'b`. Token breakdown:
    //   [0] `'`  StringStart (opener)
    //   [1] `a`  StringChar  (per-codepoint default)
    //   [2] `''` StringChar  (doubled-delim escape — 2 bytes)
    //   [3] `b`  StringChar  (per-codepoint default)
    //   [4] `'`  StringChar  (endsAt-match close — pops frame)
    auto h      = loadTsql("'a''b'");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 5u);
    EXPECT_EQ(result.tokens[0].schemaKind,
              h.schema->schemaTokens().find("StringStart"));
    const auto stringCharKind = h.schema->schemaTokens().find("StringChar");
    EXPECT_EQ(result.tokens[1].schemaKind, stringCharKind);
    EXPECT_EQ(result.tokens[2].schemaKind, stringCharKind);
    EXPECT_EQ(result.tokens[3].schemaKind, stringCharKind);
    EXPECT_EQ(result.tokens[4].schemaKind, stringCharKind);
    // The doubled-delim token spans 2 bytes; the close spans 1.
    EXPECT_EQ(result.tokens[2].span.length(), 2u);
    EXPECT_EQ(result.tokens[4].span.length(), 1u);
    EXPECT_TRUE(result.diags.empty());
}

TEST(Tokenizer, TsqlBracketIdScansToClosingBracket) {
    // T-SQL bracket-id (`[col name]`) uses doubled-delimiter escape on
    // `]`. The body mode emits per-codepoint defaultTokens. Token
    // breakdown: BracketIdStart + 8 chars (`col name`) + `]` closer = 10.
    auto h      = loadTsql("[col name]");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 10u);
    EXPECT_EQ(result.tokens[0].schemaKind,
              h.schema->schemaTokens().find("BracketIdStart"));
    const auto bracketCharKind = h.schema->schemaTokens().find("BracketIdChar");
    for (std::size_t i = 1; i < result.tokens.size(); ++i) {
        EXPECT_EQ(result.tokens[i].schemaKind, bracketCharKind)
            << "token " << i << " should be BracketIdChar (body defaultToken)";
    }
    EXPECT_TRUE(result.diags.empty());
}

TEST(Tokenizer, UnterminatedTsqlStringEmitsP_UnterminatedString) {
    auto h      = loadTsql("'open");
    auto result = lex(h);
    ASSERT_EQ(result.diags.size(), 1u);
    EXPECT_EQ(result.diags[0].code, DiagnosticCode::P_UnterminatedString);
}

TEST(Tokenizer, NestedBlockCommentsDoNotNestPerCStandard) {
    // C block comments do not nest — `/* /* */ */` closes on the
    // first `*/`. Token-by-token breakdown (source positions):
    //   [0] `/*`  (0..2) BlockCommentStart  opener pushes body mode
    //   [1] ` `   (2)    CommentChar         body codepoint
    //   [2] `/`   (3)    CommentChar         body codepoint (NOT opener — we're in body)
    //   [3] `*`   (4)    CommentChar         body codepoint
    //   [4] ` `   (5)    CommentChar         body codepoint
    //   [5] `*/`  (6..8) CommentChar         endsAt match closes body
    //   [6] ` `   (8)    Whitespace          back in main mode
    //   [7] `*`   (9)    StarOp              stray main-mode operator
    //   [8] `/`   (10)   SlashOp             stray main-mode operator
    auto h      = loadCSubset("/* /* */ */");
    auto result = lex(h);
    EXPECT_TRUE(result.diags.empty());
    ASSERT_EQ(result.tokens.size(), 9u);
    EXPECT_EQ(result.tokens[0].schemaKind,
              h.schema->schemaTokens().find("BlockCommentStart"));
    EXPECT_EQ(result.tokens[5].schemaKind,
              h.schema->schemaTokens().find("CommentChar"));
    EXPECT_EQ(textOf(*h.src, result.tokens[5]), "*/");
    // Pin position: trailing `*` must be at source byte 9 (the second
    // `*/` pair), not somewhere earlier where a regression that
    // mishandled the nested `/*` would mis-place it.
    EXPECT_EQ(result.tokens[7].schemaKind,
              h.schema->schemaTokens().find("StarOp"));
    EXPECT_EQ(result.tokens[7].span.start(), 9u);
    EXPECT_EQ(result.tokens[8].schemaKind,
              h.schema->schemaTokens().find("SlashOp"));
    EXPECT_EQ(result.tokens[8].span.start(), 10u);
}

TEST(Tokenizer, ModeStackResetsBetweenTopLevelStatements) {
    // After a closed comment, main mode is restored. Multiple comments
    // in sequence each open + close their own mode without leaking.
    // Token breakdown for `// one\n/* two */// three\nvar`:
    //   `// one\n`     → 1 opener + 5 body chars (` one\n` close) = 6
    //   `/* two */`    → 1 opener + 6 body chars (` two `, `*/` close) = 7
    //   `// three\n`   → 1 opener + 7 body chars (` three\n` close) = 8
    //   `var`          → 1 Word = 1
    // Total = 22 tokens (Eof is not pushed into the test vector).
    auto h      = loadCSubset("// one\n/* two */// three\nvar");
    auto result = lex(h);
    EXPECT_TRUE(result.diags.empty());
    ASSERT_EQ(result.tokens.size(), 22u);
    // Openers land at the expected indices — proves frames closed
    // correctly between them (otherwise the second comment would
    // never run its opener; its `/*`/`//` would be body chars).
    EXPECT_EQ(result.tokens[0].schemaKind,
              h.schema->schemaTokens().find("LineCommentStart"));
    EXPECT_EQ(result.tokens[6].schemaKind,
              h.schema->schemaTokens().find("BlockCommentStart"));
    EXPECT_EQ(result.tokens[13].schemaKind,
              h.schema->schemaTokens().find("LineCommentStart"));
    // Last non-Eof token must be `var` in main mode.
    EXPECT_EQ(result.tokens.back().coreKind, CoreTokenKind::Word);
    EXPECT_EQ(textOf(*h.src, result.tokens.back()), "var");
}

TEST(Tokenizer, CommentDefaultTokenFlagsAreEmptySpaceForAstCursorSkip) {
    // Closes v2-gap-catalog row 3 — the test that pins the contract:
    // body-mode defaultTokens carry the EmptySpace flag declared in
    // the schema's `lexerModes.<mode>.defaultToken.flags`. Without
    // this, the AST cursor wouldn't skip comment bodies.
    //
    // The flag travels through the schema, not the Token itself
    // (TZ1's Token has no flags field; flags are applied at builder
    // time via `meaning.flagsApplied`). The relevant assertion is
    // that the schema's CommentChar registration carries the flag,
    // which the builder applies on pushToken. Pin the schema-side
    // half here; the builder-side half is exercised by the c-subset
    // E2E test in TZ3.
    auto loaded = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    const auto lineCommentModeId = schema->findLexerMode("line-comment");
    ASSERT_TRUE(lineCommentModeId.valid());
    auto const& mode = schema->lexerMode(lineCommentModeId);
    ASSERT_TRUE(mode.defaultToken.has_value());
    EXPECT_TRUE(isEmptySpace(mode.defaultToken->flags))
        << "line-comment defaultToken.flags must include EmptySpace";
}

// ── TZ2 review-fix: previously-untested body-mode branches (H6–H10) ──────
//
// The shipped configs exercise the doubled-delimiter and no-escape branches
// of the body-mode dispatcher. The remaining four branches — char-escape,
// endsAtLongestMatch, replaceMode, and explicit popMode — had no end-to-end
// tokenizer coverage prior to the TZ2 review. Inline schemas pin each.

namespace {

// H6: escapeKind:"char" — backslash escape fuses lead byte + next
// codepoint into one defaultToken. Body branch order puts char-escape
// AFTER the endsAt check, so `\"` inside the string does NOT close.
constexpr std::string_view kCharEscapeSchema = R"JSON({
  "dssSchemaVersion": 2,
  "language": { "name": "X", "version": "0.1.0" },
  "lexerModes": {
    "main":   { "tokens": "default" },
    "string": {
      "defaultToken":   { "kind": "StringChar" },
      "unterminatedAs": "string"
    }
  },
  "tokens": {
    "\"": [{
      "kind":        "StringStart",
      "modeOp":      "pushMode",
      "modeArg":     "string",
      "stringStyle": { "escapeKind": "char", "escapeChar": "\\", "endsAt": "\"" }
    }]
  },
  "shapes": { "root": { "sequence": [ "StringStart" ] } }
})JSON";

// H7: endsAtLongestMatch with a 1-char endsAt. Body's `]` close eats
// the entire trailing run (Lua heredoc-style — 3 `]` produce ONE close
// emission, not three).
constexpr std::string_view kLongestMatchSchema = R"JSON({
  "dssSchemaVersion": 2,
  "language": { "name": "X", "version": "0.1.0" },
  "lexerModes": {
    "main":   { "tokens": "default" },
    "heredoc": {
      "defaultToken":   { "kind": "HereChar" },
      "unterminatedAs": "string"
    }
  },
  "tokens": {
    "[": [{
      "kind":        "HereOpen",
      "modeOp":      "pushMode",
      "modeArg":     "heredoc",
      "stringStyle": { "escapeKind": "none", "endsAt": "]", "endsAtLongestMatch": true }
    }]
  },
  "shapes": { "root": { "sequence": [ "HereOpen" ] } }
})JSON";

// H8: replaceMode swaps the active body without push/pop. Mode-a has
// no defaultToken so the global lookup runs there — `@` resolves to
// the Swap meaning, replacing frames.back() with mode-b. The `]` then
// pops; frame stack ends at depth-1 (no unterminated diagnostic).
constexpr std::string_view kReplaceModeSchema = R"JSON({
  "dssSchemaVersion": 2,
  "language": { "name": "X", "version": "0.1.0" },
  "lexerModes": {
    "main":   { "tokens": "default" },
    "mode-a": { },
    "mode-b": { }
  },
  "tokens": {
    "[": [{ "kind": "AStart", "modeOp": "pushMode",    "modeArg": "mode-a" }],
    "@": [{ "kind": "Swap",   "modeOp": "replaceMode", "modeArg": "mode-b" }],
    "]": [{ "kind": "BEnd",   "modeOp": "popMode" }]
  },
  "shapes": { "root": { "sequence": [ "AStart" ] } }
})JSON";

// H9: explicit popMode — independent of stringStyle.endsAt. The body's
// own opener pushes mode; an explicit `>` token (in non-body subMode)
// pops it. Distinct from comment modes which pop via endsAt match.
constexpr std::string_view kExplicitPopSchema = R"JSON({
  "dssSchemaVersion": 2,
  "language": { "name": "X", "version": "0.1.0" },
  "lexerModes": {
    "main": { "tokens": "default" },
    "sub":  { }
  },
  "tokens": {
    "<": [{ "kind": "SubOpen",  "modeOp": "pushMode", "modeArg": "sub" }],
    ">": [{ "kind": "SubClose", "modeOp": "popMode" }]
  },
  "shapes": { "root": { "sequence": [ "SubOpen" ] } }
})JSON";

// H10: doubledDelimiter — a non-doubled endsAt closes the body. The
// branch order is doubled-delim FIRST, endsAt SECOND; pin that a
// single occurrence falls through to endsAt and pops the frame.
constexpr std::string_view kDoubledDelimSchema = R"JSON({
  "dssSchemaVersion": 2,
  "language": { "name": "X", "version": "0.1.0" },
  "lexerModes": {
    "main":   { "tokens": "default" },
    "string": {
      "defaultToken":   { "kind": "StringChar" },
      "unterminatedAs": "string"
    }
  },
  "tokens": {
    "'": [{
      "kind":        "StringStart",
      "modeOp":      "pushMode",
      "modeArg":     "string",
      "stringStyle": { "escapeKind": "doubled-delimiter", "endsAt": "'" }
    }]
  },
  "shapes": { "root": { "sequence": [ "StringStart" ] } }
})JSON";

[[nodiscard]] LexResult lexInline(std::string_view schemaText, std::string sourceText) {
    auto loaded = GrammarSchema::loadFromText(schemaText);
    EXPECT_TRUE(loaded.has_value())
        << "inline schema load failed: "
        << (loaded.has_value() ? "<ok>" : loaded.error()[0].message);
    H h{
        .src    = SourceBuffer::fromString(std::move(sourceText), "<inline>"),
        .schema = loaded.has_value() ? *loaded : nullptr,
    };
    return lex(std::move(h));
}

} // namespace

TEST(Tokenizer, CharEscapeBranchFusesLeadAndNextCodepoint) {
    // Source: `"a\"b"` — opener, 'a', escape-pair `\"`, 'b', closer = 5.
    // The `\"` MUST NOT close the string; it's swallowed by char-escape.
    auto result = lexInline(kCharEscapeSchema, "\"a\\\"b\"");
    ASSERT_EQ(result.tokens.size(), 5u);
    EXPECT_TRUE(result.diags.empty());
    // First token is the opener; remaining four are StringChar.
    auto loaded = GrammarSchema::loadFromText(kCharEscapeSchema);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(result.tokens[0].schemaKind,
              (*loaded)->schemaTokens().find("StringStart"));
    const auto stringChar = (*loaded)->schemaTokens().find("StringChar");
    EXPECT_EQ(result.tokens[1].schemaKind, stringChar);  // 'a'
    EXPECT_EQ(result.tokens[2].schemaKind, stringChar);  // '\"' fused
    EXPECT_EQ(result.tokens[3].schemaKind, stringChar);  // 'b'
    EXPECT_EQ(result.tokens[4].schemaKind, stringChar);  // closer
    // The escape-pair token is 2 bytes wide (`\` + `"`), not 1.
    EXPECT_EQ(result.tokens[2].span.length(), 2u);
}

TEST(Tokenizer, EndsAtLongestMatchEatsConsecutiveRun) {
    // Source: `[x]]]`. After the opener, body emits one HereChar for
    // 'x' then a single endsAt match consuming THREE `]` chars (the
    // longest-run rule). Total tokens: opener + 'x' + close = 3.
    auto result = lexInline(kLongestMatchSchema, "[x]]]");
    EXPECT_TRUE(result.diags.empty());
    ASSERT_EQ(result.tokens.size(), 3u);
    auto loaded = GrammarSchema::loadFromText(kLongestMatchSchema);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(result.tokens[0].schemaKind,
              (*loaded)->schemaTokens().find("HereOpen"));
    const auto hereChar = (*loaded)->schemaTokens().find("HereChar");
    EXPECT_EQ(result.tokens[1].schemaKind, hereChar);
    EXPECT_EQ(result.tokens[2].schemaKind, hereChar);
    // The close token covers all three `]` bytes.
    EXPECT_EQ(result.tokens[2].span.length(), 3u);
}

TEST(Tokenizer, ReplaceModeSwapsActiveFrameWithoutDepthChange) {
    // Source: `[@]`. AStart pushes mode-a (depth 2); `@` runs Swap
    // which replaceMode's frames.back() to mode-b (still depth 2);
    // `]` runs BEnd popMode (depth 1). No unterminated diagnostic.
    auto result = lexInline(kReplaceModeSchema, "[@]");
    EXPECT_TRUE(result.diags.empty());
    ASSERT_EQ(result.tokens.size(), 3u);
    auto loaded = GrammarSchema::loadFromText(kReplaceModeSchema);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(result.tokens[0].schemaKind,
              (*loaded)->schemaTokens().find("AStart"));
    EXPECT_EQ(result.tokens[1].schemaKind,
              (*loaded)->schemaTokens().find("Swap"));
    EXPECT_EQ(result.tokens[2].schemaKind,
              (*loaded)->schemaTokens().find("BEnd"));
}

TEST(Tokenizer, ExplicitPopModeClosesFrameWithoutEndsAt) {
    // Source: `<>`. SubOpen pushes mode `sub`; SubClose pops it.
    // No defaultToken means the global lookup runs inside `sub` — the
    // popMode path is exercised independently of endsAt-based close.
    auto result = lexInline(kExplicitPopSchema, "<>");
    EXPECT_TRUE(result.diags.empty());
    ASSERT_EQ(result.tokens.size(), 2u);
    auto loaded = GrammarSchema::loadFromText(kExplicitPopSchema);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(result.tokens[0].schemaKind,
              (*loaded)->schemaTokens().find("SubOpen"));
    EXPECT_EQ(result.tokens[1].schemaKind,
              (*loaded)->schemaTokens().find("SubClose"));
}

TEST(Tokenizer, DoubledDelimiterSingleOccurrenceClosesBody) {
    // Source: `'a'`. Body branch order: doubled-delim FIRST (needs 2
    // `'`, fails — only 1 left). Falls through to endsAt match (1 `'`,
    // succeeds). Body pops; total tokens: opener + 'a' + close = 3.
    auto result = lexInline(kDoubledDelimSchema, "'a'");
    EXPECT_TRUE(result.diags.empty());
    ASSERT_EQ(result.tokens.size(), 3u);
    auto loaded = GrammarSchema::loadFromText(kDoubledDelimSchema);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(result.tokens[0].schemaKind,
              (*loaded)->schemaTokens().find("StringStart"));
    const auto stringChar = (*loaded)->schemaTokens().find("StringChar");
    EXPECT_EQ(result.tokens[1].schemaKind, stringChar);
    EXPECT_EQ(result.tokens[2].schemaKind, stringChar);
    // The closer is 1 byte; the doubled-delim branch did NOT swallow it.
    EXPECT_EQ(result.tokens[2].span.length(), 1u);
}

// ── CR1 follow-on: Token.flags reach AST nodes via TreeBuilder ───────────
//
// The CommentDefaultTokenFlagsAreEmptySpaceForAstCursorSkip test pins
// the schema half (the mode's defaultToken.flags carries EmptySpace).
// This test pins the end-to-end half: tokenizer stamps Token.flags from
// the mode's defaultToken.flags, TreeBuilder's effectiveFlags OR-merges
// `meaning.flagsApplied | tok.flags`, and the leaf node carries the bit.
//
// Constructing a config where the OR truly *needs* `tok.flags` requires
// the body's schemaKind to resolve via the global lexeme table to a
// meaning whose own flagsApplied does NOT carry EmptySpace. That way a
// regression that drops `tok.flags` from the merge would leave the
// resulting node without EmptySpace, and the test fails loudly.
//
// `plain` token (no flags) appears in the global table; the lexer mode
// `body` declares defaultToken { kind: Plain, flags: ["EmptySpace"] }.
// The body emits `plain`-kind tokens with EmptySpace set on Token.flags.
// The builder's resolveMeaning re-looks-up `x` against the global table,
// finds the Plain meaning (no flags), then OR-merges Token.flags →
// effectiveFlags = EmptySpace. Drop the OR and the leaf loses the bit.

namespace {

constexpr std::string_view kFlagPropagationSchema = R"JSON({
  "dssSchemaVersion": 2,
  "language": { "name": "X", "version": "0.1.0" },
  "lexerModes": {
    "main": { "tokens": "default" },
    "body": {
      "defaultToken":   { "kind": "Plain", "flags": ["EmptySpace"] },
      "unterminatedAs": "string"
    }
  },
  "tokens": {
    "x": [{ "kind": "Plain" }],
    "(": [{
      "kind": "BodyOpen", "modeOp": "pushMode", "modeArg": "body",
      "stringStyle": { "escapeKind": "none", "endsAt": ")" }
    }]
  },
  "shapes": { "root": { "sequence": [ "BodyOpen" ] } }
})JSON";

} // namespace

TEST(Tokenizer, TokenFlags_PropagateToBuilderLeafFlags) {
    auto loaded = GrammarSchema::loadFromText(kFlagPropagationSchema);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    auto schema = *loaded;
    auto src    = SourceBuffer::fromString("(x)", "<flag-prop>");

    Tokenizer tk{src, schema};
    auto tokenizeResult = std::move(tk).tokenize();

    TreeBuilder b{src, schema};
    {
        auto root = b.open(schema->rules().find("root"));
        while (!tokenizeResult.stream.isAtEnd()) {
            b.pushToken(tokenizeResult.stream.advance());
        }
    }
    Tree t = std::move(b).finish();

    // Locate the `x` leaf. Its global meaning is Plain (no flags), so
    // any EmptySpace flag on the resulting node MUST have come from
    // Token.flags via the effectiveFlags OR.
    const auto plainKind = schema->schemaTokens().find("Plain");
    NodeId xLeaf{};
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId id{i};
        if (t.kind(id) != NodeKind::Token) continue;
        if (t.tokenKind(id) != plainKind) continue;
        // The `x` byte is at source position 1 (opener `(` is byte 0).
        if (t.span(id).start() != 1) continue;
        xLeaf = id;
        break;
    }
    ASSERT_TRUE(xLeaf.valid()) << "Plain leaf for `x` not found in tree";
    EXPECT_TRUE(isEmptySpace(t.flags(xLeaf)))
        << "Token.flags did not OR-merge into the leaf's effective flags — "
        << "CR1's effectiveFlags wiring is regressed";
}
