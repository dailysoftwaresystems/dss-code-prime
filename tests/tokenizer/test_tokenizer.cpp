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
    // 08.55: numeric scanning is config-driven; toy has no `numberStyle`
    // so we use c-subset which declares the full C-style block.
    auto h      = loadCSubset("12345");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(result.tokens[0].schemaKind,
              h.schema->schemaTokens().find("IntLiteral"));
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "12345");
}

TEST(Tokenizer, FloatLiteralWithFractionalPart) {
    auto h      = loadCSubset("3.14");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::FloatLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "3.14");
}

TEST(Tokenizer, FloatLiteralWithExponent) {
    auto h      = loadCSubset("1e10");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::FloatLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "1e10");
}

TEST(Tokenizer, FloatLiteralWithSignedExponent) {
    auto h      = loadCSubset("1.5e-3");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::FloatLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "1.5e-3");
}

TEST(Tokenizer, FloatLiteralWithFSuffixPromotesIntToFloat) {
    auto h      = loadCSubset("0f");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::FloatLiteral);
}

TEST(Tokenizer, BareENotConsumedAsExponentWhenNoDigitFollows) {
    // `1e+` is NOT a float — no digit after the sign. Tokenizer emits
    // exactly three tokens: IntLiteral("1"), Word("e"), Operator("+").
    auto h      = loadCSubset("1e+");
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
    // `a.foo` member access: the dot after a WORD never enters the
    // numeric scanner — three tokens. (FC1 cycle 2 rewrote this
    // test's input: it previously pinned `3.foo` → `3`+`.`+`foo`,
    // the pre-C23 digit-after-dot-only rule that c-subset's new
    // `trailingFraction` opt-in replaces.)
    auto h      = loadCSubset("a.foo");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 3u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::Word);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "a");
    EXPECT_EQ(textOf(*h.src, result.tokens[1]), ".");
    EXPECT_EQ(textOf(*h.src, result.tokens[2]), "foo");
}

TEST(Tokenizer, TrailingFractionMakesDigitsDotOneFloat) {
    // FC1 cycle 2 (`trailingFraction: true` in c-subset — C23
    // 6.4.4.2 fractional-constant `digit-sequence .`): `3.` is ONE
    // float — and the float-suffix maximal-munch then takes the `f`,
    // so `3.foo` lexes as the float `3.f` + the word `oo`. (Real C
    // absorbs `3.foo` into one ill-formed pp-number; either way the
    // downstream parse diagnoses the adjacency loudly.)
    auto h      = loadCSubset("3.foo");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 2u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::FloatLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "3.f");
    EXPECT_EQ(textOf(*h.src, result.tokens[1]), "oo");

    // The pure trailing-dot form (whitespace-delimited, no suffix
    // ambiguity): exactly `3.`.
    auto h2      = loadCSubset("3. ;");
    auto result2 = lex(h2);
    ASSERT_GE(result2.tokens.size(), 1u);
    EXPECT_EQ(result2.tokens[0].coreKind, CoreTokenKind::FloatLiteral);
    EXPECT_EQ(textOf(*h2.src, result2.tokens[0]), "3.");
}

// ─── FC1 cycle 2 (2026-06-10): C23 hex-float literals ──────────────────────
// The 0x/0X prefixes declare a `float` continuation (letters p/P,
// decimal exponent digits). C23 6.4.4.2: optional fraction with digits
// on at least ONE side of the point + a REQUIRED binary-exponent-part.

namespace {
// One-FloatLiteral-token assertion helper for the valid hex-float forms.
void expectOneFloat(std::string_view src) {
    auto h      = loadCSubset(std::string{src});
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u) << "input: " << src;
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::FloatLiteral)
        << "input: " << src;
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), src);
    EXPECT_TRUE(result.diags.empty()) << "input: " << src;
}
// One-malformed-token assertion helper: tokens[0] spans `expectedSpan`,
// exactly one P_MalformedNumber (the tail may re-lex into more tokens).
void expectMalformedHead(std::string_view src, std::string_view expectedSpan) {
    auto h      = loadCSubset(std::string{src});
    auto result = lex(h);
    ASSERT_GE(result.tokens.size(), 1u) << "input: " << src;
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), expectedSpan)
        << "input: " << src;
    std::size_t malformed = 0;
    for (auto const& d : result.diags) {
        if (d.code == DiagnosticCode::P_MalformedNumber) ++malformed;
    }
    EXPECT_EQ(malformed, 1u) << "input: " << src;
}
}  // namespace

TEST(Tokenizer, HexFloatLexesAsOneFloatToken) {
    expectOneFloat("0x1.8p3");
}

TEST(Tokenizer, HexFloatWithoutFractionLexes) {
    expectOneFloat("0x1p3");
}

TEST(Tokenizer, HexFloatLeadingFractionLexes) {
    // C23: hexadecimal-fractional-constant `. hex-digits` — the
    // fraction point directly after the prefix (the entry gate
    // admits it because 0x declares a float continuation).
    expectOneFloat("0x.8p1");
}

TEST(Tokenizer, HexFloatTrailingDotLexes) {
    // C23: `hex-digits .` (empty fraction) is valid WITH the exponent.
    expectOneFloat("0x1.p3");
}

TEST(Tokenizer, HexFloatNegativeExponentLexes) {
    expectOneFloat("0x1p-2");
}

TEST(Tokenizer, HexFloatSeparatorsSuffixAndFDigitLex) {
    // 'f' here is a MANTISSA DIGIT (1.ff), the separator sits between
    // digits, and the trailing 'f' is the float SUFFIX — three
    // different meanings of the same letter in one literal.
    expectOneFloat("0x1.f'fp3f");
}

TEST(Tokenizer, HexFloatUppercaseFormLexes) {
    expectOneFloat("0X1.8P3");
}

TEST(Tokenizer, HexFloatWithoutExponentIsMalformed) {
    // The binary-exponent-part is REQUIRED (C23) — once the fraction
    // commits the float, a missing exponent is ONE loud malformed
    // token, never a silent `0x1` + `.8` split.
    expectMalformedHead("0x1.8", "0x1.8");
}

TEST(Tokenizer, HexFloatDanglingExponentLetterIsMalformed) {
    expectMalformedHead("0x1.8p", "0x1.8p");
}

TEST(Tokenizer, HexFloatDanglingExponentSignIsMalformed) {
    expectMalformedHead("0x1.8p+", "0x1.8p+");
}

TEST(Tokenizer, HexPrefixDotWithoutDigitsIsMalformedPrefix) {
    // `0x.p3` has no digit on EITHER side of the point — the fraction
    // never commits, so the prefix arm falls out as the malformed
    // `0x` (the same loud shape as `0x'ff`); the tail re-lexes.
    expectMalformedHead("0x.p3", "0x");
}

TEST(Tokenizer, HexIntegerWithTrailingFDigitStaysInteger) {
    // No fraction, no exponent letter → the float continuation never
    // engages; `f` is just a hex digit.
    auto h      = loadCSubset("0x1f");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "0x1f");
    EXPECT_TRUE(result.diags.empty());
}

TEST(Tokenizer, BinaryPrefixTrailingFIsIntegerPlusWord) {
    // The prefix arm deliberately does NOT inherit the decimal arm's
    // f-suffix INT→FLOAT promotion: in a prefix digit class the
    // suffix letters can BE digits, so `0b1f` is the integer `0b1`
    // followed by the word `f` — never a float (strtod would parse
    // "0b1" as 0 and silently zero the value).
    auto h      = loadCSubset("0b1f");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 2u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "0b1");
    EXPECT_EQ(result.tokens[1].coreKind, CoreTokenKind::Word);
    EXPECT_EQ(textOf(*h.src, result.tokens[1]), "f");
}

// ─── FC1 cycle 2: C23 decimal fractional-constant edge forms ───────────────
// (`trailingFraction` + `leadingFraction`, both opt-in, c-subset = true.)

TEST(Tokenizer, TrailingFractionWithExponentAndSuffixLex) {
    expectOneFloat("1.");
    expectOneFloat("1.e3");
    expectOneFloat("1.f");
}

TEST(Tokenizer, LeadingFractionLexes) {
    expectOneFloat(".5");
    expectOneFloat(".5e2");
    expectOneFloat(".5f");
}

TEST(Tokenizer, FractionFlagsDefaultFalseKeepsSplitLexing) {
    // Audit fold (FC1c2): the knobs' FALSE direction must be a
    // BEHAVIOR pin, not just load-validation coupling — the audit
    // demonstrated that dropping the scanner's
    // trailingFraction/leadingFraction guards left the whole suite
    // green. tsql-subset is a SHIPPED config that declares
    // fractionPoint but NOT the flags: `1.` must stay the integer
    // `1` + a separate dot, and `.5` must stay a dot + the integer
    // `5`. Red-on-disable: ignore either knob in scanNumber/dispatch
    // and these exact-text pins flip.
    auto loaded = GrammarSchema::loadShipped("tsql-subset");
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    {
        auto src = SourceBuffer::fromString("1.", "<tsql>");
        Tokenizer tk{src, schema};
        auto [stream, _] = std::move(tk).tokenize();
        std::vector<Token> tokens;
        while (!stream.isAtEnd()) tokens.push_back(stream.advance());
        ASSERT_GE(tokens.size(), 2u);
        EXPECT_EQ(tokens[0].coreKind, CoreTokenKind::IntLiteral);
        EXPECT_EQ(textOf(*src, tokens[0]), "1");
        EXPECT_EQ(textOf(*src, tokens[1]), ".");
    }
    {
        auto src = SourceBuffer::fromString(".5", "<tsql>");
        Tokenizer tk{src, schema};
        auto [stream, _] = std::move(tk).tokenize();
        std::vector<Token> tokens;
        while (!stream.isAtEnd()) tokens.push_back(stream.advance());
        ASSERT_GE(tokens.size(), 2u);
        EXPECT_NE(tokens[0].coreKind, CoreTokenKind::FloatLiteral);
        EXPECT_EQ(textOf(*src, tokens[0]), ".");
        EXPECT_EQ(tokens[1].coreKind, CoreTokenKind::IntLiteral);
        EXPECT_EQ(textOf(*src, tokens[1]), "5");
    }
}

TEST(Tokenizer, LoneDotIsNotANumber) {
    // The leading-fraction dispatch admits `.` ONLY when a decimal
    // digit follows — a bare dot (or `.foo`) stays the language's
    // member-access dot.
    auto h      = loadCSubset(".foo");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 2u);
    EXPECT_NE(result.tokens[0].coreKind, CoreTokenKind::FloatLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), ".");
    EXPECT_EQ(textOf(*h.src, result.tokens[1]), "foo");
}

TEST(Tokenizer, HexLiteralLexedAsOneIntToken) {
    // `0xff` MUST become one IntLiteral, not `0` + identifier `xff`.
    auto h      = loadCSubset("0xff");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "0xff");
}

TEST(Tokenizer, HexLiteralWithApostropheSeparators) {
    // FC1 (2026-06-10): c-subset's digitSeparator flipped `_` → `'`
    // (the C23 separator — `_` was never C).
    auto h      = loadCSubset("0xDEAD'BEEF");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "0xDEAD'BEEF");
}

TEST(Tokenizer, BinaryLiteralLexedAsOneIntToken) {
    auto h      = loadCSubset("0b1010'0101");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "0b1010'0101");
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

TEST(Tokenizer, BareHexPrefixAtEofIsMalformed) {
    // `0x` at EOF — prefix matched, no body byte to consume. F7
    // (08.55 remediation): emit exactly one P_MalformedNumber and
    // consume the prefix as a malformed IntLiteral. The previous
    // behavior silently split `0x` into `0` (IntLiteral) + `x`
    // (Word), masking the user's typo.
    auto h      = loadCSubset("0x");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "0x");
    ASSERT_EQ(result.diags.size(), 1u);
    EXPECT_EQ(result.diags[0].code, DiagnosticCode::P_MalformedNumber);
}

TEST(Tokenizer, BareBinaryPrefixAtEofIsMalformed) {
    auto h      = loadCSubset("0b");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "0b");
    ASSERT_EQ(result.diags.size(), 1u);
    EXPECT_EQ(result.diags[0].code, DiagnosticCode::P_MalformedNumber);
}

TEST(Tokenizer, BareOctalPrefixAtEofIsMalformed) {
    auto h      = loadCSubset("0o");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "0o");
    ASSERT_EQ(result.diags.size(), 1u);
    EXPECT_EQ(result.diags[0].code, DiagnosticCode::P_MalformedNumber);
}

TEST(Tokenizer, HexPrefixFollowedBySeparatorOnlyEmitsMalformedDiagnostic) {
    // FC1 rewrite (2026-06-10): the pre-FC1 form used `0x_` (the old
    // `_` separator). With c-subset's separator now `'` (C23) and the
    // flanked-by-digits rule, `0x'` enters the hex-prefix arm (the
    // entry check admits a separator after the prefix so the typo is
    // DIAGNOSED rather than split into `0` + identifier), finds no
    // digit body, and emits a malformed `0x` IntLiteral. The dangling
    // `'` re-enters the dispatch as a char-literal start, so only the
    // first token + the malformed count are pinned. (`0x_` itself now
    // lexes as `0` + identifier `x_` — `_` is no longer special.)
    auto h      = loadCSubset("0x'");
    auto result = lex(h);
    ASSERT_GE(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "0x");
    std::size_t malformedCount = 0;
    for (auto const& d : result.diags) {
        if (d.code == DiagnosticCode::P_MalformedNumber) ++malformedCount;
    }
    EXPECT_EQ(malformedCount, 1u);
}

TEST(Tokenizer, BinaryPrefixWithoutDigitsIsMalformed) {
    // FC1 rewrite (2026-06-10): the pre-FC1 form of this test
    // (`0b___`) relied on the OLD unconditional separator consume —
    // separators are now flanked-by-digits (C23 6.4.4.1), so a
    // separator-only body no longer extends the token. The intent —
    // a multi-char prefix with no digit body is ONE malformed
    // IntLiteral — is pinned via the EOF-after-prefix form.
    auto h      = loadCSubset("0b");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "0b");
    ASSERT_EQ(result.diags.size(), 1u);
    EXPECT_EQ(result.diags[0].code, DiagnosticCode::P_MalformedNumber);
}

TEST(Tokenizer, HexPrefixFollowedBySeparatorIsMalformedPerC23) {
    // FC1 INVERSION (2026-06-10) of the pre-FC1
    // `ValidHexWithLeadingUnderscoreThenDigitIsAccepted`: C23
    // 6.4.4.1 requires every digit separator to be flanked by
    // digits, so `0x'ff` is ill-formed — the prefix arm stops at the
    // leading separator and `0x` surfaces as a malformed IntLiteral
    // (exactly one P_MalformedNumber). The tail `'ff` re-enters the
    // dispatch as a char-literal start, so only the FIRST token +
    // the malformed-count are pinned here (the tail's char-literal
    // diagnostics are its own feature's concern).
    auto h      = loadCSubset("0x'ff");
    auto result = lex(h);
    ASSERT_GE(result.tokens.size(), 1u);
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "0x");
    std::size_t malformedCount = 0;
    for (auto const& d : result.diags) {
        if (d.code == DiagnosticCode::P_MalformedNumber) ++malformedCount;
    }
    EXPECT_EQ(malformedCount, 1u);
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
    // c-subset line-comment mode: `//` pushes mode; everything up to but
    // EXCLUDING the next `\n` becomes a CommentChar. c22
    // (D-PP-LINE-COMMENT-BEFORE-DIRECTIVE): the `\n` is NOT consumed into the
    // comment — the `//` mode is `endsAtExclusive`, so the terminating newline
    // survives as its OWN Newline token (so a following `#` directive keeps its
    // line boundary). The tokenizer is then back in main mode for the rest.
    auto h      = loadCSubset("// hi\nvar x;");
    auto result = lex(h);
    // Tokens: LineCommentStart, ' ', 'h', 'i', Newline('\n'), Word("var"),
    // ' ', Word("x"), Punctuation(';')  ⇒ 9 (the `\n` is now a Newline, not a
    // CommentChar — same count, different kind at [4]).
    ASSERT_EQ(result.tokens.size(), 9u);
    EXPECT_EQ(result.tokens[0].schemaKind,
              h.schema->schemaTokens().find("LineCommentStart"));
    // The chars BEFORE the newline share the CommentChar schemaKind.
    const auto commentCharKind = h.schema->schemaTokens().find("CommentChar");
    EXPECT_EQ(result.tokens[1].schemaKind, commentCharKind);
    EXPECT_EQ(result.tokens[2].schemaKind, commentCharKind);
    EXPECT_EQ(result.tokens[3].schemaKind, commentCharKind);
    // The terminating newline is its OWN Newline token (NOT a CommentChar) —
    // the c22 line-boundary-preserving fix.
    EXPECT_EQ(result.tokens[4].coreKind, CoreTokenKind::Newline);
    EXPECT_EQ(result.tokens[4].schemaKind,
              h.schema->schemaTokens().find("Newline"));
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

// ── Context-sensitive per-mode lexing (agnostic engine capability) ─────────
//
// A grammar declares a lexer mode carrying a per-mode `tokens` override
// table. While the tokenizer is in that mode the scanner consults the
// override table FIRST and falls back to the global table for anything the
// mode does not override. So the SAME source character lexes to a
// DIFFERENT token kind depending on the active mode — the substrate FF11
// will use to make `<` open a header path inside `#include` while staying
// a normal operator everywhere else. Proven here on a SYNTHETIC grammar
// (loadFromText, no shipped .lang.json) so the capability is pinned even
// though it ships UNCONSUMED.
namespace {

// `[` pushes the `hdr` mode; inside `hdr`, `<` is overridden to
// HeaderOpen (vs LtOp globally) and `]` pops back. `<` is NOT listed in
// the override table outside its single in-mode meaning, so OUTSIDE the
// mode it must still resolve to the global LtOp via the fallback.
constexpr std::string_view kPerModeOverrideSchema = R"JSON({
  "dssSchemaVersion": 2,
  "language": { "name": "PerMode", "version": "0.1.0" },
  "tokens": {
    "<": [{ "kind": "LtOp" }],
    "<<": [{ "kind": "ShlOp" }],
    "[": [{ "kind": "OpenBracket", "modeOp": "pushMode", "modeArg": "hdr" }],
    "]": [{ "kind": "CloseBracket" }]
  },
  "shapes": { "root": { "sequence": [ "LtOp" ] } },
  "lexerModes": {
    "hdr": {
      "tokens": {
        "<": [{ "kind": "HeaderOpen" }],
        "]": [{ "kind": "CloseBracket", "modeOp": "popMode" }]
      }
    }
  }
})JSON";

} // namespace

TEST(Tokenizer, PerModeOverrideLexesSameCharDifferentlyInsideMode) {
    auto loaded = GrammarSchema::loadFromText(kPerModeOverrideSchema);
    ASSERT_TRUE(loaded.has_value()) << "synthetic per-mode override schema must load";
    auto schema = *loaded;

    const auto ltOp       = schema->schemaTokens().find("LtOp");
    const auto headerOpen = schema->schemaTokens().find("HeaderOpen");
    const auto openBr     = schema->schemaTokens().find("OpenBracket");
    const auto closeBr     = schema->schemaTokens().find("CloseBracket");
    ASSERT_TRUE(ltOp.valid());
    ASSERT_TRUE(headerOpen.valid());
    // The override kind and the global kind are genuinely distinct ids —
    // otherwise the test could pass trivially.
    ASSERT_NE(ltOp, headerOpen);

    // Input crosses the mode boundary twice: `<` in main → `[` enters hdr
    // → `<` in hdr → `]` exits hdr → `<` back in main. (No whitespace so
    // the token indices are exact.)
    H h{
        .src    = SourceBuffer::fromString("<[<]<", "<permode>"),
        .schema = schema,
    };
    auto result = lex(h);

    // 5 operator tokens, no whitespace.
    ASSERT_EQ(result.tokens.size(), 5u);
    EXPECT_EQ(result.tokens[0].schemaKind, ltOp)
        << "`<` OUTSIDE the mode must be the global LtOp";
    EXPECT_EQ(result.tokens[1].schemaKind, openBr);
    EXPECT_EQ(result.tokens[2].schemaKind, headerOpen)
        << "`<` INSIDE the mode must use the per-mode HeaderOpen override "
           "(RED-on-disable: falls back to LtOp if the mode lookup is unwired)";
    EXPECT_EQ(result.tokens[3].schemaKind, closeBr);
    EXPECT_EQ(result.tokens[4].schemaKind, ltOp)
        << "after popping the mode, `<` must return to the global LtOp — "
           "lexing OUTSIDE the mode is unchanged";

    // The same byte `<` produced two different schema kinds in one stream.
    EXPECT_NE(result.tokens[0].schemaKind, result.tokens[2].schemaKind);
    EXPECT_EQ(result.tokens[0].schemaKind, result.tokens[4].schemaKind);
    EXPECT_TRUE(result.diags.empty());
}

TEST(Tokenizer, PerModeOverrideFallsBackToGlobalForNonOverriddenLexeme) {
    // Inside `hdr` the only overrides are `<` and `]`. A `<`-adjacent but
    // non-overridden global lexeme encountered in the mode must resolve
    // via the global fallback. Here `[` is global-only; reaching it inside
    // the mode (via a nested push is not modeled, so instead we prove the
    // fallback by checking a global operator that the mode omits: `]` pops,
    // then in main `[` pushes again). The decisive fallback check: the
    // FIRST `<` (main) and a global-only token both resolve normally while
    // the override is active only for the chars it lists.
    auto loaded = GrammarSchema::loadFromText(kPerModeOverrideSchema);
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    const auto headerOpen = schema->schemaTokens().find("HeaderOpen");
    const auto openBr     = schema->schemaTokens().find("OpenBracket");

    // `[<[` — outer `[` enters hdr, `<` is HeaderOpen, then `[` inside hdr
    // is NOT overridden so it falls back to the GLOBAL `[` (OpenBracket,
    // which pushes hdr again — harmless for tokenization).
    H h{
        .src    = SourceBuffer::fromString("[<[", "<permode-fallback>"),
        .schema = schema,
    };
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 3u);
    EXPECT_EQ(result.tokens[0].schemaKind, openBr);
    EXPECT_EQ(result.tokens[1].schemaKind, headerOpen)
        << "`<` inside hdr uses the override";
    EXPECT_EQ(result.tokens[2].schemaKind, openBr)
        << "`[` inside hdr is NOT overridden → resolves via the global fallback";
}

TEST(Tokenizer, PerModeOverrideShortCircuitsLongerGlobalLexeme) {
    // The documented override-first short-circuit (language-config-spec.md): a per-mode
    // override wins at ITS length even if a LONGER global lexeme shares the prefix. The
    // `hdr` mode overrides 1-char `<` but NOT 2-char `<<`; the global table has `<<`→ShlOp.
    // INSIDE the mode, `<<` lexes as TWO HeaderOpen (the override hits at len 1, SUPPRESSING
    // the global 2-char `<<`); OUTSIDE, `<<` is the global ShlOp. This is the intended
    // context-sensitive semantic (a mode redefines what `<` means); the footgun is documented.
    auto loaded = GrammarSchema::loadFromText(kPerModeOverrideSchema);
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    const auto headerOpen = schema->schemaTokens().find("HeaderOpen");
    const auto shlOp      = schema->schemaTokens().find("ShlOp");
    const auto openBr     = schema->schemaTokens().find("OpenBracket");
    ASSERT_TRUE(headerOpen.valid());
    ASSERT_TRUE(shlOp.valid());
    ASSERT_NE(headerOpen, shlOp);

    // OUTSIDE the mode: `<<` is the global 2-char ShlOp (longest-match within the main table).
    {
        H h{ .src = SourceBuffer::fromString("<<", "<sc-out>"), .schema = schema };
        auto r = lex(h);
        ASSERT_EQ(r.tokens.size(), 1u);
        EXPECT_EQ(r.tokens[0].schemaKind, shlOp)
            << "`<<` outside any override mode is the global 2-char ShlOp";
    }
    // INSIDE the mode: `[` enters hdr; `<<` → TWO 1-char HeaderOpen (the override short-circuits
    // the global `<<`); then `]` pops. (RED if `longestMatchInMode` consulted global first.)
    {
        H h{ .src = SourceBuffer::fromString("[<<]", "<sc-in>"), .schema = schema };
        auto r = lex(h);
        ASSERT_EQ(r.tokens.size(), 4u);
        EXPECT_EQ(r.tokens[0].schemaKind, openBr);
        EXPECT_EQ(r.tokens[1].schemaKind, headerOpen)
            << "first `<` in hdr → override HeaderOpen (len 1)";
        EXPECT_EQ(r.tokens[2].schemaKind, headerOpen)
            << "second `<` in hdr → override HeaderOpen — the global 2-char `<<` is SUPPRESSED "
               "(override-first short-circuit)";
        EXPECT_NE(r.tokens[1].schemaKind, shlOp)
            << "the global `<<` must NOT win inside the override mode";
    }
}

// FF11 drift-guard scoping (footgun pin): a NON-MAIN mode whose `tokens` is
// the literal `"default"` is a verbatim copy of the GLOBAL lexeme table — it
// introduces no new kind meaning. The `modeIntroducedKinds` harvest must
// NOT dump that copied global table into the builder's synthetic-meaning
// drift-guard exception set, which would broadly weaken the guard. Only a
// REAL inline override (a mode whose table genuinely differs) may contribute
// its kinds. This synthetic grammar pairs BOTH: a `"default"` mode (`dfl`)
// and an inline-override mode (`hdr` → `HeaderOpen`).
namespace {
constexpr std::string_view kDefaultModeHarvestSchema = R"JSON({
  "dssSchemaVersion": 2,
  "language": { "name": "DefaultModeHarvest", "version": "0.1.0" },
  "tokens": {
    "<": [{ "kind": "LtOp" }],
    "[": [{ "kind": "OpenBracket", "modeOp": "pushMode", "modeArg": "hdr" }],
    "{": [{ "kind": "OpenBrace", "modeOp": "pushMode", "modeArg": "dfl" }],
    "]": [{ "kind": "CloseBracket" }],
    "}": [{ "kind": "CloseBrace" }]
  },
  "shapes": { "root": { "sequence": [ "LtOp" ] } },
  "lexerModes": {
    "hdr": {
      "tokens": {
        "<": [{ "kind": "HeaderOpen" }],
        "]": [{ "kind": "CloseBracket", "modeOp": "popMode" }]
      }
    },
    "dfl": {
      "tokens": "default"
    }
  }
})JSON";
} // namespace

TEST(Tokenizer, DefaultModeDoesNotDumpGlobalKindsIntoModeIntroducedSet) {
    auto loaded = GrammarSchema::loadFromText(kDefaultModeHarvestSchema);
    ASSERT_TRUE(loaded.has_value())
        << "synthetic default-mode-harvest schema must load";
    auto schema = *loaded;

    const auto ltOp       = schema->schemaTokens().find("LtOp");
    const auto openBr     = schema->schemaTokens().find("OpenBracket");
    const auto headerOpen = schema->schemaTokens().find("HeaderOpen");
    ASSERT_TRUE(ltOp.valid());
    ASSERT_TRUE(openBr.valid());
    ASSERT_TRUE(headerOpen.valid());

    // THE FIX: a global kind that appears in the `dfl` mode ONLY because
    // `tokens: "default"` copied the whole global table must NOT be treated
    // as mode-introduced. RED-on-regression: without the `"default"`-skip
    // guard, the harvest inserts EVERY global kind (LtOp, OpenBracket, ...)
    // into modeIntroducedKinds, broadly weakening the drift guard.
    EXPECT_FALSE(schema->isModeIntroducedKind(ltOp))
        << "`LtOp` is a global kind — a `tokens:\"default\"` copy must not "
           "make it mode-introduced";
    EXPECT_FALSE(schema->isModeIntroducedKind(openBr))
        << "no global kind copied via `tokens:\"default\"` may enter the "
           "drift-guard exception set";

    // CONTROL: a genuine inline override (`hdr` introduces `HeaderOpen`,
    // which has NO global entry) MUST still be mode-introduced — the fix
    // narrows the harvest to real overrides, it does not disable it.
    EXPECT_TRUE(schema->isModeIntroducedKind(headerOpen))
        << "a real inline-override kind must remain mode-introduced";
}

TEST(Tokenizer, UnterminatedCoalescedStringEmitsDiagnostic) {
    // A `"`-opened string with no closing quote. The coalesce-body path scans
    // the body to EOF, emits ONE StringLiteral token, leaves the mode open, and
    // the post-loop handler reports P_UnterminatedString. (New coalesce-path
    // EOF branch — must not be silently swallowed.)
    auto h      = loadCSubset("\"abc");
    auto result = lex(h);
    bool sawUnterminated = false;
    for (auto const& d : result.diags)
        if (d.code == DiagnosticCode::P_UnterminatedString) { sawUnterminated = true; break; }
    EXPECT_TRUE(sawUnterminated) << "unterminated coalesced string must report P_UnterminatedString";
    // Body is one coalesced token, not per-byte: [StringStart, StringLiteral].
    EXPECT_EQ(result.tokens.size(), 2u);
    EXPECT_EQ(result.tokens[0].schemaKind, h.schema->schemaTokens().find("StringStart"));
    EXPECT_EQ(result.tokens[1].schemaKind, h.schema->schemaTokens().find("StringLiteral"));
}

TEST(Tokenizer, WideStringOpenersTokenizeViaLongestMatch) {
    // C11/C23 6.4.5: the wide/UTF openers `L"`/`u"`/`U"`/`u8"` are multi-char
    // lexemes that START with an id-start byte. The tokenizer's longestMatchInMode
    // must make each opener beat the bare identifier run — `L"AB"` is
    // [WideStringStart, StringLiteral], NOT [Identifier(L), StringStart, ...]. Each
    // pushes the SAME shared `string` mode → one coalesced StringLiteral body.
    struct Case { char const* src; char const* opener; };
    for (auto const& c : {Case{"L\"AB\"", "WideStringStart"},
                          Case{"u\"AB\"", "Utf16StringStart"},
                          Case{"U\"AB\"", "Utf32StringStart"},
                          Case{"u8\"AB\"", "Utf8StringStart"}}) {
        auto h      = loadCSubset(c.src);
        auto result = lex(h);
        ASSERT_EQ(result.tokens.size(), 2u) << "opener=" << c.opener;
        EXPECT_EQ(result.tokens[0].schemaKind, h.schema->schemaTokens().find(c.opener))
            << "the multi-char opener must win over the bare id-run for " << c.src;
        EXPECT_EQ(result.tokens[1].schemaKind, h.schema->schemaTokens().find("StringLiteral"));
        EXPECT_EQ(textOf(*h.src, result.tokens[1]), "AB");
    }
}

TEST(Tokenizer, U8StringOpenerBeatsUOpenerAndIdentifier) {
    // The `u8"` opener (3 bytes) must beat BOTH the `u"` opener (2 bytes) and the
    // `u8` identifier run — longestMatchInMode picks the longest lexeme. A
    // regression to `u"` would split `u8"…"` as [Utf16StringStart, "8…"] (wrong).
    auto h      = loadCSubset("u8\"x\"");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 2u);
    EXPECT_EQ(result.tokens[0].schemaKind, h.schema->schemaTokens().find("Utf8StringStart"))
        << "u8\" must win over u\" and the u8 identifier";
    EXPECT_EQ(textOf(*h.src, result.tokens[1]), "x");
}

TEST(Tokenizer, IdentifierStartingWithUIsNotAStringOpener) {
    // Regression: a bare identifier that merely STARTS with an opener-prefix byte
    // (`user`, `L_var`) must stay ONE Identifier — the opener only wins when the
    // quote immediately follows (longestMatch covers the whole `u"` lexeme, but
    // `user` has no quote so the id-run wins).
    auto h      = loadCSubset("user");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 1u) << "`user` must stay ONE identifier, not split at u\"";
    EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::Word);
    EXPECT_EQ(textOf(*h.src, result.tokens[0]), "user");
}

TEST(Tokenizer, WideCharOpenersTokenizeViaLongestMatch) {
    // C11/C23 6.4.4.4: the wide/UTF CHAR openers `L'`/`u'`/`U'`/`u8'` are multi-char
    // lexemes that START with an id-start byte. longestMatchInMode must make each
    // opener beat the bare identifier run — `L'A'` is [WideCharStart, CharLiteral],
    // NOT [Identifier(L), CharStart, ...]. Each pushes the SAME shared `charBody`
    // mode as `'` → one coalesced CharLiteral body.
    struct Case { char const* src; char const* opener; };
    for (auto const& c : {Case{"L'A'", "WideCharStart"},
                          Case{"u'A'", "Utf16CharStart"},
                          Case{"U'A'", "Utf32CharStart"},
                          Case{"u8'A'", "Utf8CharStart"}}) {
        auto h      = loadCSubset(c.src);
        auto result = lex(h);
        ASSERT_EQ(result.tokens.size(), 2u) << "opener=" << c.opener;
        EXPECT_EQ(result.tokens[0].schemaKind, h.schema->schemaTokens().find(c.opener))
            << "the multi-char opener must win over the bare id-run for " << c.src;
        EXPECT_EQ(result.tokens[1].schemaKind, h.schema->schemaTokens().find("CharLiteral"));
        EXPECT_EQ(textOf(*h.src, result.tokens[1]), "A");
    }
}

TEST(Tokenizer, U8CharOpenerBeatsUCharOpenerAndIdentifier) {
    // The `u8'` opener (3 bytes) must beat BOTH the `u'` opener (2 bytes) and the
    // `u8` identifier run. A regression to `u'` would split `u8'x'` as
    // [Utf16CharStart, "8x"] (wrong). Mirrors the wide-STRING u8" longest-match pin.
    auto h      = loadCSubset("u8'x'");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 2u);
    EXPECT_EQ(result.tokens[0].schemaKind, h.schema->schemaTokens().find("Utf8CharStart"))
        << "u8' must win over u' and the u8 identifier";
    EXPECT_EQ(textOf(*h.src, result.tokens[1]), "x");
}

TEST(Tokenizer, CharAndStringOpenersCoexistOnSamePrefixByte) {
    // Both `u'`/`u"` (and `u8'`/`u8"`) start with `u`. longestMatchInMode must pick
    // the CHAR opener when a `'` follows and the STRING opener when a `"` follows —
    // the two literal families coexist without either shadowing the other.
    {
        auto h = loadCSubset("u'x'");
        auto r = lex(h);
        ASSERT_EQ(r.tokens.size(), 2u);
        EXPECT_EQ(r.tokens[0].schemaKind, h.schema->schemaTokens().find("Utf16CharStart"));
    }
    {
        auto h = loadCSubset("u\"x\"");
        auto r = lex(h);
        ASSERT_EQ(r.tokens.size(), 2u);
        EXPECT_EQ(r.tokens[0].schemaKind, h.schema->schemaTokens().find("Utf16StringStart"));
    }
}

TEST(Tokenizer, TsqlSingleStringDoubledDelimiterEscape) {
    // SQL string with a doubled single-quote — `'a''b'` represents the
    // literal `a'b`. The body mode coalesces (HR10), so the whole body is
    // one in-grammar token; the doubled-delim escape is preserved verbatim
    // in the body text and decoded later (decodeDoubledDelimiterBody). Token
    // breakdown:
    //   [0] `'`    StringStart   (opener)
    //   [1] `a''b` StringLiteral (coalesced body; doubled `''` kept raw)
    // The closing `'` is consumed on mode-pop, not emitted as a token.
    auto h      = loadTsql("'a''b'");
    auto result = lex(h);
    ASSERT_EQ(result.tokens.size(), 2u);
    EXPECT_EQ(result.tokens[0].schemaKind,
              h.schema->schemaTokens().find("StringStart"));
    EXPECT_EQ(result.tokens[1].schemaKind,
              h.schema->schemaTokens().find("StringLiteral"));
    // The coalesced body spans `a''b` — 4 bytes (the doubled-delim escape
    // kept raw; the close delimiter is not part of the body).
    EXPECT_EQ(result.tokens[1].span.length(), 4u);
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
    //   `// one\n`     → 1 opener + 4 body chars (` one`) + 1 Newline = 6
    //   `/* two */`    → 1 opener + 6 body chars (` two `, `*/` close) = 7
    //   `// three\n`   → 1 opener + 6 body chars (` three`) + 1 Newline = 8
    //   `var`          → 1 Word = 1
    // Total = 22 tokens (Eof is not pushed into the test vector). c22: the
    // line-comment `\n` is now a separate Newline token (endsAtExclusive), not a
    // consumed body char — same per-comment token COUNT, so the total stays 22.
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

// ── identifier-branch probe (id-start byte vs longer global lexeme) ─────

namespace {

// Schema where `N` is a 1-byte global lexeme AND `N'` is a 2-byte
// global lexeme. The identifier branch must prefer the longer global
// hit when it strictly beats the id-run, and prefer the id-run when
// the global hit is shorter or equal.
constexpr std::string_view kIdProbeSchema = R"JSON({
  "dssSchemaVersion": 1,
  "language": { "name": "IdProbe", "version": "0.1.0" },
  "tokens": {
    "N":  [{ "kind": "ShortN" }],
    "N'": [{ "kind": "UniStringStart" }],
    "'":  [{ "kind": "Apostrophe" }],
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    ";":  [{ "kind": "EndCommand" }]
  },
  "keywords": [
    { "word": "if", "kind": "IfKeyword" }
  ],
  "shapes": { "root": { "sequence": [{ "repeat": "Identifier" }] } }
})JSON";

} // namespace

// `N'` strictly beats the 1-byte id-run, so the global hit wins. The
// emitted token must cover both bytes with UniStringStart.
TEST(Tokenizer, IdStartProbe_LongerGlobalLexemeBeatsIdRun) {
    auto loaded = GrammarSchema::loadFromText(kIdProbeSchema);
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    auto src    = SourceBuffer::fromString("N'", "<idprobe>");

    Tokenizer tk{src, schema};
    auto res = std::move(tk).tokenize();
    ASSERT_FALSE(res.stream.isAtEnd());
    const Token first = res.stream.advance();

    EXPECT_EQ(first.span.length(), 2u);
    EXPECT_EQ(first.schemaKind.v,
              schema->schemaTokens().find("UniStringStart").v);
}

// `Nxyz` extends past the global `N` lexeme, so the id-run (4 bytes)
// strictly beats the 1-byte global hit. Token must be a single
// Identifier covering `Nxyz`, not a ShortN + identifier split. Pinning
// this catches a regression that flipped `>` to `>=` (which would let
// the global hit win on equal length and split the identifier).
TEST(Tokenizer, IdStartProbe_LongerIdRunBeatsShorterGlobalLexeme) {
    auto loaded = GrammarSchema::loadFromText(kIdProbeSchema);
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    auto src    = SourceBuffer::fromString("Nxyz", "<idprobe>");

    Tokenizer tk{src, schema};
    auto res = std::move(tk).tokenize();
    ASSERT_FALSE(res.stream.isAtEnd());
    const Token first = res.stream.advance();

    EXPECT_EQ(first.span.length(), 4u);
    EXPECT_EQ(first.coreKind, CoreTokenKind::Word);
    // `Nxyz` isn't a keyword; tokenizer leaves schemaKind invalid for
    // the builder's Identifier fallback to take over.
    EXPECT_FALSE(first.schemaKind.valid());
    // No subsequent ShortN — the whole 4-byte run is one token.
    const Token next = res.stream.advance();
    EXPECT_EQ(next.coreKind, CoreTokenKind::Eof);
}

// Equal-length case: global lexeme `N` is 1 byte; id-run for `N` alone
// is also 1 byte. The strict `>` gate keeps the keyword/global hit
// from stealing — but only when the next byte isn't id-continue. With
// only `N` as input, both paths produce a 1-byte token; the global
// hit's `ShortN` wins because it's a real schema entry.
TEST(Tokenizer, IdStartProbe_EqualLengthFavorsIdRun) {
    auto loaded = GrammarSchema::loadFromText(kIdProbeSchema);
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    auto src    = SourceBuffer::fromString("N", "<idprobe>");

    Tokenizer tk{src, schema};
    auto res = std::move(tk).tokenize();
    ASSERT_FALSE(res.stream.isAtEnd());
    const Token first = res.stream.advance();

    EXPECT_EQ(first.span.length(), 1u);
    // Equal-length: id-run fall-through path runs longestMatch over
    // the lexeme `N` and finds ShortN — the identifier branch's
    // `fullMatch` gate kicks in and stamps the keyword/short-lexeme
    // schemaKind on the Word token.
    EXPECT_EQ(first.coreKind, CoreTokenKind::Word);
    EXPECT_EQ(first.schemaKind.v,
              schema->schemaTokens().find("ShortN").v);
}

// `if_foo` must stay a single identifier despite `if` being a keyword.
// The id-run length (6) strictly beats `if`'s 2-byte global hit, so
// the longer-wins gate sends the lexer down the id-run path. Pinning
// this catches a regression where the prefer-longer branch flipped
// the comparison or dropped the strict ordering.
TEST(Tokenizer, IdStartProbe_KeywordPrefixDoesNotSplitIdentifier) {
    auto loaded = GrammarSchema::loadFromText(kIdProbeSchema);
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    auto src    = SourceBuffer::fromString("if_foo", "<idprobe>");

    Tokenizer tk{src, schema};
    auto res = std::move(tk).tokenize();
    ASSERT_FALSE(res.stream.isAtEnd());
    const Token first = res.stream.advance();

    EXPECT_EQ(first.span.length(), 6u);
    EXPECT_EQ(first.coreKind, CoreTokenKind::Word);
    EXPECT_FALSE(first.schemaKind.valid())
        << "if_foo must NOT resolve to IfKeyword — only the exact lexeme `if` should";
}

// Bare `if` resolves to the keyword, NOT the identifier fallback. Pins
// the equal-length keyword case — a regression that flipped the
// comparison would still pass `if_foo` (id-run wins via length) but
// might mis-handle lone `if` (equal-length → fall-through stamps the
// keyword schemaKind on the Word token).
TEST(Tokenizer, IdStartProbe_BareKeywordResolvesToKeywordKind) {
    auto loaded = GrammarSchema::loadFromText(kIdProbeSchema);
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    auto src    = SourceBuffer::fromString("if", "<idprobe>");

    Tokenizer tk{src, schema};
    auto res = std::move(tk).tokenize();
    ASSERT_FALSE(res.stream.isAtEnd());
    const Token first = res.stream.advance();

    EXPECT_EQ(first.span.length(), 2u);
    EXPECT_EQ(first.coreKind, CoreTokenKind::Word);
    EXPECT_EQ(first.schemaKind.v,
              schema->schemaTokens().find("IfKeyword").v)
        << "lone `if` must resolve to IfKeyword via the equal-length keyword path";
}

// Mode-pushing 2-byte id-start opener: `r"..."` style. The 2-byte
// `r"` lexeme triggers a pushMode, which is a stronger interaction
// than the `N'` case (no mode change). Pinning this catches a
// regression where the longer-wins gate accepted the global hit's
// span but dropped its `modeOp` side effect.
namespace {
constexpr std::string_view kRawStringSchema = R"JSON({
  "dssSchemaVersion": 2,
  "language": { "name": "RawStr", "version": "0.1.0" },
  "lexerModes": {
    "main":   { "tokens": "default" },
    "rawStr": { "defaultToken": { "kind": "RawChar" }, "unterminatedAs": "string" }
  },
  "tokens": {
    "r\"": [{ "kind": "RawOpen", "modeOp": "pushMode", "modeArg": "rawStr",
              "stringStyle": { "escapeKind": "none", "endsAt": "\"" } }],
    "\"":  [{ "kind": "PlainOpen" }]
  },
  "shapes": { "root": { "sequence": [ "RawOpen" ] } }
})JSON";
}

TEST(Tokenizer, IdStartProbe_RawStringOpenerPushesMode) {
    auto loaded = GrammarSchema::loadFromText(kRawStringSchema);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.has_value() ? "" : loaded.error()[0].message);
    auto schema = *loaded;
    auto src    = SourceBuffer::fromString("r\"abc\"", "<rawstr>");

    Tokenizer tk{src, schema};
    auto res = std::move(tk).tokenize();
    ASSERT_FALSE(res.stream.isAtEnd());
    const Token first = res.stream.advance();

    EXPECT_EQ(first.span.length(), 2u);
    EXPECT_EQ(first.schemaKind.v,
              schema->schemaTokens().find("RawOpen").v);

    // The next emitted tokens should be RawChar (body-mode default),
    // proving the modeOp fired during the global-hit path. If a
    // regression took the id-run path instead, `r` would be a Word
    // token and `"` would open the PlainOpen mode (no push).
    const Token rawA = res.stream.advance();
    EXPECT_EQ(rawA.schemaKind.v,
              schema->schemaTokens().find("RawChar").v)
        << "expected RawChar body token after `r\"`; modeOp must have fired";
}

// ── 08.55: numberStyle genericity pin ──────────────────────────────────────
//
// A synthetic, NON-C-style numberStyle drives scanNumber correctly. Tests
// pin: `$` hex prefix, `^` exponent letter, `'` digit separator, the
// Pascal-style `B` integer suffix, ``q`` float suffix, and a custom
// fraction point. If any of these were hardcoded anywhere in the engine,
// this test would tokenize incorrectly.
namespace {
constexpr std::string_view kGenericNumSchema = R"JSON({
  "dssSchemaVersion": 4,
  "language": { "name": "GenericNums", "version": "0.1.0" },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    ";":  [{ "kind": "Semi" }]
  },
  "numberStyle": {
    "decimal":         true,
    "integerPrefixes": [
      { "prefix": "$",  "radix": 16, "digits": "0-9a-fA-F",
        "float": { "exponent": { "letters": ["^"], "signOptional": true },
                   "exponentDigits": "01" } },
      { "prefix": "%",  "radix": 2,  "digits": "01",
        "float": { "exponent": { "letters": ["@"], "signOptional": false },
                   "exponentDigits": "0-7" } }
    ],
    "exponent":        { "letters": ["^"], "signOptional": false },
    "fractionPoint":   ".",
    "digitSeparator":  "'",
    "integerSuffixes": ["B"],
    "floatSuffixes":   ["q"],
    "emitKind":        { "integer": "IntLiteral", "float": "FloatLiteral" }
  },
  "shapes": {
    "root": { "sequence": [ "IntLiteral", "Semi" ] }
  }
})JSON";
}

TEST(Tokenizer, GenericNumberStyleHexPrefixDollar) {
    auto loaded = GrammarSchema::loadFromText(kGenericNumSchema);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "" : loaded.error()[0].message);
    auto schema = *loaded;
    auto src    = SourceBuffer::fromString("$ff", "<gen>");
    Tokenizer tk{src, schema};
    auto [stream, _] = std::move(tk).tokenize();
    std::vector<Token> tokens;
    while (!stream.isAtEnd()) tokens.push_back(stream.advance());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*src, tokens[0]), "$ff");
}

TEST(Tokenizer, GenericNumberStyleCustomExponentLetter) {
    auto loaded = GrammarSchema::loadFromText(kGenericNumSchema);
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    auto src    = SourceBuffer::fromString("1.5^3", "<gen>");
    Tokenizer tk{src, schema};
    auto [stream, _] = std::move(tk).tokenize();
    std::vector<Token> tokens;
    while (!stream.isAtEnd()) tokens.push_back(stream.advance());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].coreKind, CoreTokenKind::FloatLiteral);
    EXPECT_EQ(textOf(*src, tokens[0]), "1.5^3");
}

TEST(Tokenizer, GenericNumberStyleApostropheSeparator) {
    auto loaded = GrammarSchema::loadFromText(kGenericNumSchema);
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    auto src    = SourceBuffer::fromString("1'000'000", "<gen>");
    Tokenizer tk{src, schema};
    auto [stream, _] = std::move(tk).tokenize();
    std::vector<Token> tokens;
    while (!stream.isAtEnd()) tokens.push_back(stream.advance());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*src, tokens[0]), "1'000'000");
}

TEST(Tokenizer, GenericSeparatorTrailingIsNotConsumed) {
    // FC1 between-digits rule (C23 6.4.4.1, applied universally to
    // any schema-declared separator): a TRAILING separator is not
    // part of the number. Pre-FC1 the scanner consumed it
    // unconditionally — `1'000'` lexed as ONE token swallowing the
    // final quote (red-on-disable lever: revert the flanked-by-
    // digits guard and tokens[0] becomes "1'000'"). The synthetic
    // non-C schema proves the rule is engine-universal, not a
    // c-subset special case.
    auto loaded = GrammarSchema::loadFromText(kGenericNumSchema);
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    auto src    = SourceBuffer::fromString("1'000'", "<gen>");
    Tokenizer tk{src, schema};
    auto [stream, _] = std::move(tk).tokenize();
    std::vector<Token> tokens;
    while (!stream.isAtEnd()) tokens.push_back(stream.advance());
    ASSERT_GE(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*src, tokens[0]), "1'000");
}

TEST(Tokenizer, GenericSeparatorMustBeFlankedByDigits) {
    // FC1: a DOUBLED separator ends the number at the last digit —
    // `1''2` is two-sided ill-formed per C23, so the number is just
    // "1" (pre-FC1 the loose scanner produced one "1''2" token).
    auto loaded = GrammarSchema::loadFromText(kGenericNumSchema);
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    auto src    = SourceBuffer::fromString("1''2", "<gen>");
    Tokenizer tk{src, schema};
    auto [stream, _] = std::move(tk).tokenize();
    std::vector<Token> tokens;
    while (!stream.isAtEnd()) tokens.push_back(stream.advance());
    ASSERT_GE(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*src, tokens[0]), "1");
}

// ─── FC1 cycle 2: prefix-float genericity pins ─────────────────────────────
// The synthetic schema's `$` prefix declares a float continuation with
// the NON-C letter `^` and BINARY exponent digits ("01"); the `%`
// prefix uses `@` with sign-NOT-optional and octal exponent digits. If
// any of `p`/`.`-after-`0x`/decimal-exponent-digits were hardcoded in
// the engine, these would tokenize wrong.

namespace {
[[nodiscard]] std::vector<Token>
lexGeneric(std::string_view text, std::shared_ptr<SourceBuffer>& srcOut) {
    auto loaded = GrammarSchema::loadFromText(kGenericNumSchema);
    if (!loaded.has_value()) {
        ADD_FAILURE() << "kGenericNumSchema must load";
        return {};
    }
    auto schema = *loaded;
    srcOut      = SourceBuffer::fromString(std::string{text}, "<gen>");
    Tokenizer tk{srcOut, schema};
    auto [stream, _] = std::move(tk).tokenize();
    std::vector<Token> tokens;
    while (!stream.isAtEnd()) tokens.push_back(stream.advance());
    return tokens;
}
}  // namespace

TEST(Tokenizer, GenericPrefixFloatLexesWithCustomLetterAndDigits) {
    std::shared_ptr<SourceBuffer> src;
    auto tokens = lexGeneric("$1.8^11", src);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].coreKind, CoreTokenKind::FloatLiteral);
    EXPECT_EQ(textOf(*src, tokens[0]), "$1.8^11");
}

TEST(Tokenizer, GenericPrefixFloatExponentDigitsClassIsHonored) {
    // `2` is NOT in the `$` prefix's exponentDigits class ("01") —
    // the letter commits the float, the digit run can't start, the
    // span is ONE malformed token. The knob-that-lies guard for
    // `exponentDigits`: a hardcoded "0-9" would happily accept it.
    std::shared_ptr<SourceBuffer> src;
    auto tokens = lexGeneric("$1.8^2", src);
    ASSERT_GE(tokens.size(), 1u);
    EXPECT_EQ(textOf(*src, tokens[0]), "$1.8^");
}

TEST(Tokenizer, GenericPrefixFloatExponentIsRequired) {
    // Engine rule (no knob until a language needs one): a committed
    // prefix-float without its exponent is malformed.
    std::shared_ptr<SourceBuffer> src;
    auto tokens = lexGeneric("$1.8", src);
    ASSERT_GE(tokens.size(), 1u);
    EXPECT_EQ(textOf(*src, tokens[0]), "$1.8");
    EXPECT_EQ(tokens[0].coreKind, CoreTokenKind::FloatLiteral);
}

TEST(Tokenizer, GenericPrefixFloatSignNotOptionalIsCommittedMalformed) {
    // `%`'s float declares signOptional=false: `@+1` does NOT accept
    // the sign, the committed float dies loudly at `%1.1@` (this
    // DIVERGES from the decimal exponent's documented
    // leave-the-letter split — a prefix-float has no valid split to
    // fall back to; see NumberPrefixFloat's doc).
    std::shared_ptr<SourceBuffer> src;
    auto tokens = lexGeneric("%1.1@+1", src);
    ASSERT_GE(tokens.size(), 1u);
    EXPECT_EQ(textOf(*src, tokens[0]), "%1.1@");

    auto tokens2 = lexGeneric("%1.1@7", src);
    ASSERT_EQ(tokens2.size(), 1u);
    EXPECT_EQ(tokens2[0].coreKind, CoreTokenKind::FloatLiteral);
    EXPECT_EQ(textOf(*src, tokens2[0]), "%1.1@7");
}

TEST(Tokenizer, GenericNumberStyleFloatSuffixPromotesKind) {
    auto loaded = GrammarSchema::loadFromText(kGenericNumSchema);
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    // `42q` — q is a float suffix in this schema (NOT C-style).
    auto src    = SourceBuffer::fromString("42q", "<gen>");
    Tokenizer tk{src, schema};
    auto [stream, _] = std::move(tk).tokenize();
    std::vector<Token> tokens;
    while (!stream.isAtEnd()) tokens.push_back(stream.advance());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].coreKind, CoreTokenKind::FloatLiteral);
}

// F1: `l`/`L` are integer suffixes (long / long long), NOT float
// suffixes. An earlier 08.55 c-subset config listed `l`/`L` under
// `floatSuffixes`, which silently promoted long integers to the
// floating-literal kind — a regression vs. the hand-coded scanner.
// The fix removes them from `floatSuffixes`; pin the corrected
// behavior here (every L-suffixed integer stays an IntLiteral).
TEST(Tokenizer, CSubsetLongIntegerSuffixesStayInteger) {
    {
        auto h      = loadCSubset("42L");
        auto result = lex(h);
        ASSERT_EQ(result.tokens.size(), 1u);
        EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
        EXPECT_EQ(textOf(*h.src, result.tokens[0]), "42L");
    }
    {
        auto h      = loadCSubset("42l");
        auto result = lex(h);
        ASSERT_EQ(result.tokens.size(), 1u);
        EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
        EXPECT_EQ(textOf(*h.src, result.tokens[0]), "42l");
    }
    {
        auto h      = loadCSubset("42LL");
        auto result = lex(h);
        ASSERT_EQ(result.tokens.size(), 1u);
        EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
        EXPECT_EQ(textOf(*h.src, result.tokens[0]), "42LL");
    }
    {
        auto h      = loadCSubset("42ll");
        auto result = lex(h);
        ASSERT_EQ(result.tokens.size(), 1u);
        EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
        EXPECT_EQ(textOf(*h.src, result.tokens[0]), "42ll");
    }
}

TEST(Tokenizer, CSubsetFloatSuffixesPromoteToFloat) {
    {
        auto h      = loadCSubset("1.5f");
        auto result = lex(h);
        ASSERT_EQ(result.tokens.size(), 1u);
        EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::FloatLiteral);
        EXPECT_EQ(textOf(*h.src, result.tokens[0]), "1.5f");
    }
    {
        auto h      = loadCSubset("1.5F");
        auto result = lex(h);
        ASSERT_EQ(result.tokens.size(), 1u);
        EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::FloatLiteral);
        EXPECT_EQ(textOf(*h.src, result.tokens[0]), "1.5F");
    }
    {
        auto h      = loadCSubset("1.5e3");
        auto result = lex(h);
        ASSERT_EQ(result.tokens.size(), 1u);
        EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::FloatLiteral);
        EXPECT_EQ(textOf(*h.src, result.tokens[0]), "1.5e3");
    }
    {
        auto h      = loadCSubset("1.5e3f");
        auto result = lex(h);
        ASSERT_EQ(result.tokens.size(), 1u);
        EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::FloatLiteral);
        EXPECT_EQ(textOf(*h.src, result.tokens[0]), "1.5e3f");
    }
}

// F8: c-subset declares `0` (octal) prefix so `017` tokenizes as an
// IntLiteral (octal 15), not decimal 17 + nothing. `0x17` continues
// to tokenize as a single hex literal because the longer prefix wins
// the declaration-order loop. `09` is malformed (8 is not a valid
// octal digit, so the prefix arm sees no body digit and surfaces
// P_MalformedNumber).
TEST(Tokenizer, CSubsetBareZeroOctalPrefixWorks) {
    {
        auto h      = loadCSubset("017");
        auto result = lex(h);
        ASSERT_EQ(result.tokens.size(), 1u);
        EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
        EXPECT_EQ(textOf(*h.src, result.tokens[0]), "017");
        EXPECT_TRUE(result.diags.empty());
    }
    {
        auto h      = loadCSubset("0x17");
        auto result = lex(h);
        ASSERT_EQ(result.tokens.size(), 1u);
        EXPECT_EQ(result.tokens[0].coreKind, CoreTokenKind::IntLiteral);
        EXPECT_EQ(textOf(*h.src, result.tokens[0]), "0x17");
        EXPECT_TRUE(result.diags.empty());
    }
}

// F18: `signOptional: false` rejects a sign between the exponent
// letter and digits. `2^+3` tokenizes as three tokens: IntLiteral `2`,
// the schema-mapped `^` (XorOp in c-subset), IntLiteral `3`. Pin via
// an inline schema since none of the shipped configs use the
// non-default value.
TEST(Tokenizer, NumberStyleSignOptionalFalseRejectsSign) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "SignTest", "version": "0.1.0", "fileExtensions": [".st"] },
      "tokens": {
        "+": [{ "kind": "PlusOp" }],
        ";": [{ "kind": "Semi" }]
      },
      "numberStyle": {
        "decimal": true,
        "exponent": { "letters": ["^"], "signOptional": false },
        "emitKind": { "integer": "IntLiteral", "float": "FloatLiteral" }
      },
      "shapes": {
        "root": { "sequence": ["IntLiteral", "Semi"] }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    auto src    = SourceBuffer::fromString("2^+3", "<sign>");
    Tokenizer tk{src, schema};
    auto [stream, _] = std::move(tk).tokenize();
    std::vector<Token> tokens;
    while (!stream.isAtEnd()) tokens.push_back(stream.advance());
    // Three tokens: `2`, `+`, `3`. The exponent letter `^` does NOT
    // appear because signOptional=false rejects the `+` follow-up,
    // so the exponent branch fails AND the trailing letter is left
    // unconsumed (it then has no longest-match landing in this
    // schema, so it falls into the illegal-char path — manifests as
    // an Error token).
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].coreKind, CoreTokenKind::IntLiteral);
    EXPECT_EQ(textOf(*src, tokens[0]), "2");
    // tokens[1] is the unmatched `^` — emitted as an Error
    EXPECT_EQ(tokens[1].coreKind, CoreTokenKind::Error);
    // tokens[2] is `+`
    EXPECT_EQ(textOf(*src, tokens[2]), "+");
}

// F22: T-SQL numberStyle smoke test. `1.5e3` is a FloatLiteral; T-SQL
// declares no integer-prefix block, so `0x10` is NOT one number.
TEST(Tokenizer, TsqlNumberStyleSmoke) {
    auto schemaR = GrammarSchema::loadShipped("tsql-subset");
    ASSERT_TRUE(schemaR.has_value());
    auto schema = *schemaR;
    {
        auto src = SourceBuffer::fromString("1.5e3", "<sql>");
        Tokenizer tk{src, schema};
        auto [stream, _] = std::move(tk).tokenize();
        std::vector<Token> tokens;
        while (!stream.isAtEnd()) tokens.push_back(stream.advance());
        ASSERT_EQ(tokens.size(), 1u);
        EXPECT_EQ(tokens[0].coreKind, CoreTokenKind::FloatLiteral);
        EXPECT_EQ(textOf(*src, tokens[0]), "1.5e3");
    }
    {
        auto src = SourceBuffer::fromString("0x10", "<sql>");
        Tokenizer tk{src, schema};
        auto [stream, _] = std::move(tk).tokenize();
        std::vector<Token> tokens;
        while (!stream.isAtEnd()) tokens.push_back(stream.advance());
        // T-SQL declares no integer prefixes, so this is two tokens:
        // IntLiteral `0` + Identifier-ish `x10` (a Word run).
        ASSERT_GE(tokens.size(), 2u);
        EXPECT_EQ(tokens[0].coreKind, CoreTokenKind::IntLiteral);
        EXPECT_EQ(textOf(*src, tokens[0]), "0");
    }
}

// ─── FF11: angle-include (`#include <h>`) context-sensitive lexing ────────
//
// `#` pushes the line-scoped `include-directive` mode; inside it `<` opens
// a header path (replaceMode → header-body) instead of meaning LtOp; the
// coalesced `HeaderPath` body captures the path bytes; `>` (the
// stringStyle.endsAt) closes it. These pin the cycle-20 per-mode capability
// as FF11's first consumer — and that the ADDITIVE paths (quote include,
// `<` as an operator) are untouched.

namespace {

// Non-trivia tokens (drops Whitespace/Newline) for compact sequence pins.
[[nodiscard]] std::vector<Token> codeTokens(LexResult const& r) {
    std::vector<Token> out;
    for (auto const& t : r.tokens) {
        if (t.coreKind == CoreTokenKind::Whitespace
            || t.coreKind == CoreTokenKind::Newline
            || t.coreKind == CoreTokenKind::Eof) continue;
        out.push_back(t);
    }
    return out;
}

} // namespace

TEST(Tokenizer, FF11AngleIncludeLexesAsOpenerPathCloser) {
    auto h = loadCSubset("#include <stdio.h>\n");
    auto schema = h.schema;
    ASSERT_TRUE(schema != nullptr);
    auto srcBuf = h.src;   // keep the buffer alive past lex()'s by-value consume
    auto result = lex(std::move(h));
    EXPECT_TRUE(result.diags.empty()) << "well-formed angle include must lex cleanly";

    const auto hashOp      = schema->schemaTokens().find("HashOp");
    const auto includeKw   = schema->schemaTokens().find("IncludeKeyword");
    const auto headerStart = schema->schemaTokens().find("HeaderStart");
    const auto headerPath  = schema->schemaTokens().find("HeaderPath");
    const auto ltOp        = schema->schemaTokens().find("LtOp");
    ASSERT_TRUE(headerStart.valid());
    ASSERT_TRUE(headerPath.valid());
    ASSERT_NE(headerStart, ltOp) << "HeaderStart and LtOp must be distinct kinds";

    auto code = codeTokens(result);
    // Exactly: HashOp, IncludeKeyword(Word), HeaderStart, HeaderPath.
    ASSERT_EQ(code.size(), 4u);
    EXPECT_EQ(code[0].schemaKind, hashOp);
    EXPECT_EQ(code[1].schemaKind, includeKw);
    EXPECT_EQ(code[2].schemaKind, headerStart)
        << "`<` after `#include` must be HeaderStart (RED-on-disable: would "
           "be LtOp if the include-directive mode override is unwired)";
    EXPECT_EQ(code[3].schemaKind, headerPath);
    // The path token captures exactly the bytes between `<` and `>`.
    EXPECT_EQ(textOf(*srcBuf, code[3]), "stdio.h");
}

TEST(Tokenizer, FF11AngleBracketIsLtOpOutsideDirective) {
    // The SAME `<` byte outside a directive stays the LtOp operator —
    // the mode override is scoped to the directive, not global.
    auto h = loadCSubset("a < b;\n");
    auto schema = h.schema;
    auto result = lex(std::move(h));
    const auto ltOp = schema->schemaTokens().find("LtOp");
    auto code = codeTokens(result);
    bool sawLt = false;
    for (auto const& t : code) if (t.schemaKind == ltOp) sawLt = true;
    EXPECT_TRUE(sawLt) << "`<` outside a directive must remain LtOp";
}

TEST(Tokenizer, FF11QuoteIncludeStillProducesStringStart) {
    // ADDITIVE: the quote form is unchanged — `#` enters the directive
    // mode (popAtNewline), but `"` still uses the GLOBAL StringStart →
    // string body, so `imports.pathToken: StringStart` keeps matching.
    auto h = loadCSubset("#include \"local.h\"\n");
    auto schema = h.schema;
    auto result = lex(std::move(h));
    EXPECT_TRUE(result.diags.empty());
    const auto stringStart = schema->schemaTokens().find("StringStart");
    auto code = codeTokens(result);
    bool sawStringStart = false;
    for (auto const& t : code) if (t.schemaKind == stringStart) sawStringStart = true;
    EXPECT_TRUE(sawStringStart)
        << "quote include must still produce StringStart (additive path)";
}

TEST(Tokenizer, FF11DirectiveModeDoesNotLeakToNextLine) {
    // The line-scoped `include-directive` mode (popAtNewline) must NOT
    // carry `<`-as-header-opener past the newline. A malformed `#include`
    // (no header) on line 1, then `a < b` on line 2: the line-2 `<` must
    // be LtOp, proving the directive frame popped at the newline.
    auto h = loadCSubset("#include\na < b;\n");
    auto schema = h.schema;
    auto result = lex(std::move(h));
    const auto ltOp        = schema->schemaTokens().find("LtOp");
    const auto headerStart = schema->schemaTokens().find("HeaderStart");
    auto code = codeTokens(result);
    bool sawLt = false, sawHeaderStart = false;
    for (auto const& t : code) {
        if (t.schemaKind == ltOp)        sawLt = true;
        if (t.schemaKind == headerStart) sawHeaderStart = true;
    }
    EXPECT_TRUE(sawLt)
        << "line-2 `<` must be LtOp (RED-on-disable: would be HeaderStart "
           "if the directive mode leaked past the newline)";
    EXPECT_FALSE(sawHeaderStart)
        << "no HeaderStart — the line-2 `<` is an operator, not a header opener";
}

// FF11 fail-loud pin: a malformed angle include `#include <foo` with NO
// closing `>` must be reported (a diagnostic), NOT silently accepted. The
// `header-body` mode mirrors the string body — `<` opens a coalesced
// HeaderPath whose `endsAt` is `>`; reaching EOF without `>` leaves the
// body frame open and the post-loop unterminated handler emits
// P_UnterminatedString (same flavor as an unterminated `"`). This guards
// against a future silent-acceptance regression. (The UX polish — a
// header-specific flavor + newline-scoped termination so the rest of the
// file is not consumed — is deferred: anchor
// D-FFI-ANGLE-INCLUDE-LINE-SCOPED-HEADER. This test only pins fail-loud.)
TEST(Tokenizer, FF11MalformedAngleIncludeNoCloserFailsLoud) {
    auto h      = loadCSubset("#include <foo\n");
    auto result = lex(std::move(h));
    EXPECT_FALSE(result.diags.empty())
        << "a malformed `#include <foo` (no closing `>`) must fail loud — "
           "a diagnostic, not silent acceptance";
    // Today's flavor is the string-body P_UnterminatedString (identical to
    // an unterminated `\"`). Asserted to document the CURRENT behavior; the
    // load-bearing guarantee is the fail-loud check above. If the deferred
    // anchor lands a header-specific flavor, update this line.
    bool sawUnterminated = false;
    for (auto const& d : result.diags)
        if (d.code == DiagnosticCode::P_UnterminatedString) sawUnterminated = true;
    EXPECT_TRUE(sawUnterminated)
        << "the unterminated header body reports P_UnterminatedString "
           "(string-body flavor) today";
}

// ─── General `popAtNewline` capability (NOT c-subset-specific) ────────────
//
// A line-scoped mode that auto-pops at the next newline. Synthetic schema
// so the capability is pinned independently of FF11's grammar. C
// preprocessor directives and assembly lines are the motivating general
// use case.

namespace {

constexpr std::string_view kPopAtNewlineSchema = R"JSON({
  "dssSchemaVersion": 2,
  "language": { "name": "PopLine", "version": "0.1.0" },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\n": [{ "kind": "Newline",    "flags": ["EmptySpace"] }],
    "#":  [{ "kind": "Hash", "modeOp": "pushMode", "modeArg": "directive" }],
    "<":  [{ "kind": "LtOp" }]
  },
  "shapes": { "root": { "sequence": [ "LtOp" ] } },
  "lexerModes": {
    "main":      { "tokens": "default" },
    "directive": {
      "tokens": { "<": [{ "kind": "AngleOpen" }] },
      "popAtNewline": true
    }
  }
})JSON";

} // namespace

TEST(Tokenizer, PopAtNewlineModeAutoPopsAtNewline) {
    auto loaded = GrammarSchema::loadFromText(kPopAtNewlineSchema);
    ASSERT_TRUE(loaded.has_value()) << "popAtNewline synthetic schema must load";
    auto schema = *loaded;
    const auto ltOp      = schema->schemaTokens().find("LtOp");
    const auto angleOpen = schema->schemaTokens().find("AngleOpen");
    ASSERT_TRUE(angleOpen.valid());
    ASSERT_NE(ltOp, angleOpen);

    // Line 1: `#` pushes directive → `<` is AngleOpen. Newline pops the
    // directive frame. Line 2: `<` is back to the global LtOp.
    H h{
        .src    = SourceBuffer::fromString("#<\n<", "<popline>"),
        .schema = schema,
    };
    auto result = lex(h);
    EXPECT_TRUE(result.diags.empty())
        << "no unterminated diagnostic — the directive popped at the newline";

    std::vector<Token> code;
    for (auto const& t : result.tokens) {
        if (t.coreKind == CoreTokenKind::Whitespace
            || t.coreKind == CoreTokenKind::Newline
            || t.coreKind == CoreTokenKind::Eof) continue;
        code.push_back(t);
    }
    ASSERT_EQ(code.size(), 3u);   // Hash, AngleOpen, LtOp
    EXPECT_EQ(code[1].schemaKind, angleOpen)
        << "`<` inside the directive uses the override";
    EXPECT_EQ(code[2].schemaKind, ltOp)
        << "after the newline auto-pop, `<` returns to the global LtOp "
           "(RED-on-disable: stays AngleOpen if popAtNewline is unwired)";
}

TEST(Tokenizer, PopAtNewlineModeAutoPopsAtEofWithoutNewline) {
    // A line-scoped mode left open at EOF (no trailing newline) is NOT
    // unterminated — EOF validly ends the last line. No diagnostic.
    auto loaded = GrammarSchema::loadFromText(kPopAtNewlineSchema);
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    H h{
        .src    = SourceBuffer::fromString("#<", "<popline-eof>"),
        .schema = schema,
    };
    auto result = lex(h);
    EXPECT_TRUE(result.diags.empty())
        << "a popAtNewline mode at EOF closes silently — not unterminated";
}
