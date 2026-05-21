#include "core/types/grammar_schema.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/token.hpp"
#include "tokenizer/tokenizer.hpp"
#include "tokenizer/token_stream.hpp"

#include <gtest/gtest.h>

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

[[nodiscard]] TokenStream stream(std::string text) {
    auto h = loadToy(std::move(text));
    Tokenizer t{h.src, h.schema};
    return std::move(std::move(t).tokenize().stream);
}

} // namespace

TEST(TokenStream, EmptySourceYieldsOnlyEof) {
    auto s = stream("");
    EXPECT_EQ(s.size(), 1u);                            // just Eof
    EXPECT_EQ(s.peek().coreKind, CoreTokenKind::Eof);
    EXPECT_TRUE(s.isAtEnd());                            // at-end == position past last non-Eof
}

TEST(TokenStream, PeekDoesNotConsume) {
    auto s = stream("var");
    const auto before = s.position();
    auto const& a = s.peek();
    auto const& b = s.peek();
    EXPECT_EQ(a.span.start(), b.span.start());
    EXPECT_EQ(s.position(), before);
}

TEST(TokenStream, AdvanceMovesForwardOnce) {
    auto s = stream("var x");
    const auto first = s.advance();
    EXPECT_EQ(first.coreKind, CoreTokenKind::Word);
    EXPECT_EQ(first.span.start(), 0u);
    EXPECT_EQ(first.span.end(),   3u);
    EXPECT_EQ(s.position(), 1u);
}

TEST(TokenStream, PeekLookaheadInspectsFutureTokensWithoutAdvance) {
    auto s = stream("var x;");
    // The schema's whitespace policy produces one Whitespace token between
    // `var` and `x`, so lookahead 2 reaches `x`.
    EXPECT_EQ(s.peek(0).coreKind, CoreTokenKind::Word);                // var
    EXPECT_EQ(s.peek(1).coreKind, CoreTokenKind::Whitespace);          // " "
    EXPECT_EQ(s.peek(2).coreKind, CoreTokenKind::Word);                // x
    EXPECT_EQ(s.peek(3).coreKind, CoreTokenKind::Punctuation);         // ;
    EXPECT_EQ(s.position(), 0u);                                        // unchanged
}

TEST(TokenStream, PeekPastEofReturnsEof) {
    auto s = stream("var");
    // Tokens: Word("var"), Eof. peek(0)=var, peek(1)=Eof, peek(99)=Eof.
    EXPECT_EQ(s.peek(0).coreKind, CoreTokenKind::Word);
    EXPECT_EQ(s.peek(1).coreKind, CoreTokenKind::Eof);
    EXPECT_EQ(s.peek(99).coreKind, CoreTokenKind::Eof);
}

TEST(TokenStream, AdvanceAtEofIsIdempotent) {
    auto s = stream("var");
    (void)s.advance();           // consume var
    EXPECT_TRUE(s.isAtEnd());
    const auto a = s.advance();  // first call at end
    const auto b = s.advance();  // second call — must NOT advance further
    EXPECT_EQ(a.coreKind, CoreTokenKind::Eof);
    EXPECT_EQ(b.coreKind, CoreTokenKind::Eof);
    EXPECT_EQ(a.span.start(), b.span.start());
    EXPECT_EQ(a.span.end(),   b.span.end());
}

TEST(TokenStream, RewindRepositionsCursor) {
    auto s = stream("var x");
    (void)s.advance();
    (void)s.advance();
    EXPECT_EQ(s.position(), 2u);   // var, ' ' consumed → exactly 2
    s.rewind(0);
    EXPECT_EQ(s.position(), 0u);
    EXPECT_EQ(s.peek().coreKind, CoreTokenKind::Word);
}

TEST(TokenStream, RewindPastEndClampsToEof) {
    auto s = stream("var");
    s.rewind(999);
    EXPECT_TRUE(s.isAtEnd());
}

TEST(TokenStream, MarkRestoreRoundTrips) {
    auto s = stream("var x;");
    (void)s.advance();                     // consume var
    auto bm = s.mark();
    const auto savedPos = s.position();
    (void)s.advance();                     // consume " "
    (void)s.advance();                     // consume x
    EXPECT_NE(s.position(), savedPos);
    s.restore(bm);
    EXPECT_EQ(s.position(), savedPos);
}

TEST(TokenStreamDeath, RestoreOnDefaultBookmarkAborts) {
    auto s = stream("var");
    TokenStream::Bookmark bm{};
    EXPECT_FALSE(bm.valid());
    EXPECT_DEATH({ s.restore(bm); },
                 "dss::TokenStream fatal: restore on default-constructed Bookmark");
}

TEST(TokenStreamDeath, RestoreCrossInstanceAborts) {
    auto a = stream("var");
    auto b = stream("var");
    auto bmA = a.mark();
    // bmA is owned by `a`; using it on `b` is the misuse the
    // owner-stamp guards against.
    EXPECT_DEATH({ b.restore(bmA); },
                 "dss::TokenStream fatal: restore on Bookmark from a different");
}

namespace {
void peekOnDefault() {
    TokenStream s;            // default-constructed: empty tokens_, id == 0
    (void)s.peek();
}
void advanceOnMovedFrom() {
    auto live = stream("var");
    TokenStream gutted = std::move(live);   // `live` is now moved-from
    (void)live.advance();
}
} // namespace

TEST(TokenStreamDeath, DefaultConstructedPeekAborts) {
    // SKILL.md's fail-loud discipline: default-constructed / moved-from
    // streams must not silently succeed with UB. Guard fires before
    // size()-1 underflow.
    EXPECT_DEATH(peekOnDefault(),
                 "dss::TokenStream fatal: operation on default-constructed or moved-from");
}

TEST(TokenStreamDeath, MovedFromAdvanceAborts) {
    EXPECT_DEATH(advanceOnMovedFrom(),
                 "dss::TokenStream fatal: operation on default-constructed or moved-from");
}

TEST(TokenStream, EmittedTokensHaveResolvedSchemaKind) {
    auto h = loadToy("var x;");
    Tokenizer t{h.src, h.schema};
    auto [s, reporter] = std::move(t).tokenize();
    EXPECT_TRUE(reporter->all().empty());

    // var is the VarKeyword from the schema; ; is EndCommand. Both
    // should have valid schemaKind set by the tokenizer.
    EXPECT_EQ(s.peek(0).coreKind, CoreTokenKind::Word);
    EXPECT_TRUE(s.peek(0).schemaKind.valid());
    EXPECT_EQ(s.peek(0).schemaKind, h.schema->schemaTokens().find("VarKeyword"));

    // peek(3) = `;`
    EXPECT_EQ(s.peek(3).coreKind, CoreTokenKind::Punctuation);
    EXPECT_EQ(s.peek(3).schemaKind, h.schema->schemaTokens().find("EndCommand"));
}
