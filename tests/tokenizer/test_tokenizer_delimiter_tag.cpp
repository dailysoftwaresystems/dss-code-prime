// THE DYNAMIC DELIMITER TAG, READ BY THE TOKENIZER (P69, lane `cs`, fold f26d, ruling 643 (3)).
//
// A style with `delimiterTag: "matched"` carries a BOUNDED tag its document states whole
// (`"tag": {"characters", "minLength", "maxLength"}`): at the opener's end the tokenizer reads
// the longest run of admitted bytes, at most `maxLength` of them; the run is the tag when it is
// at least `minLength` long (otherwise nothing is captured and nothing consumed), and the body
// closes only at `endsAt` followed by that tag. Before this fold the tag was a regular
// expression run over the rest of the buffer, and no tokenizer test read it at all.
//
// The inline language: `<<` opens a body whose close is `>>` + the tag (a here-document's
// shape); the tag's alphabet is A-Z. `[` pushes a plain mode in which `@` REPLACES the frame
// with the same tagged body, so a replaced frame is held to the same capture.

#include "core/types/diagnostic_budget.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/token.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

// minLength 1, maxLength 4.
constexpr std::string_view kTagSchema = R"JSON({
  "dssSchemaVersion": 2,
  "language": { "name": "X", "version": "0.1.0" },
  "lexerModes": {
    "main":   { "tokens": "default" },
    "plain":  { },
    "here":   { "defaultToken": { "kind": "HereChar" }, "unterminatedAs": "string" }
  },
  "tokens": {
    "<<": [{ "kind": "HereOpen", "modeOp": "pushMode", "modeArg": "here",
             "stringStyle": { "escapeKind": "none", "endsAt": ">>", "multiline": true,
                              "delimiterTag": "matched",
                              "tag": { "characters": "A-Z", "minLength": 1, "maxLength": 4 } } }],
    "[":  [{ "kind": "PlainOpen", "modeOp": "pushMode", "modeArg": "plain" }],
    "@":  [{ "kind": "Swap", "modeOp": "replaceMode", "modeArg": "here",
             "stringStyle": { "escapeKind": "none", "endsAt": ">>", "multiline": true,
                              "delimiterTag": "matched",
                              "tag": { "characters": "A-Z", "minLength": 1, "maxLength": 4 } } }]
  },
  "shapes": { "root": { "alt": [ "HereOpen", "PlainOpen" ] } }
})JSON";

// minLength 2: a run of one admitted byte is no tag.
constexpr std::string_view kMinTwoSchema = R"JSON({
  "dssSchemaVersion": 2,
  "language": { "name": "X", "version": "0.1.0" },
  "lexerModes": {
    "main": { "tokens": "default" },
    "here": { "defaultToken": { "kind": "HereChar" }, "unterminatedAs": "string" }
  },
  "tokens": {
    "<<": [{ "kind": "HereOpen", "modeOp": "pushMode", "modeArg": "here",
             "stringStyle": { "escapeKind": "none", "endsAt": ">>", "multiline": true,
                              "delimiterTag": "matched",
                              "tag": { "characters": "A-Z", "minLength": 2, "maxLength": 4 } } }]
  },
  "shapes": { "root": { "sequence": [ "HereOpen" ] } }
})JSON";

// maxLength 2,000,000: a tag of a million bytes is a legal one, read by a loop.
constexpr std::string_view kLongTagSchema = R"JSON({
  "dssSchemaVersion": 2,
  "language": { "name": "X", "version": "0.1.0" },
  "lexerModes": {
    "main": { "tokens": "default" },
    "here": { "defaultToken": { "kind": "HereChar" }, "unterminatedAs": "string" }
  },
  "tokens": {
    "<<": [{ "kind": "HereOpen", "modeOp": "pushMode", "modeArg": "here",
             "stringStyle": { "escapeKind": "none", "endsAt": ">>", "multiline": true,
                              "delimiterTag": "matched",
                              "tag": { "characters": "A-Z", "minLength": 1, "maxLength": 2000000 } } }]
  },
  "shapes": { "root": { "sequence": [ "HereOpen" ] } }
})JSON";

struct Lexed {
    std::vector<Token>           tokens;
    std::vector<ParseDiagnostic> diags;
};

[[nodiscard]] Lexed lexWith(std::string_view schemaText, std::string sourceText) {
    auto loaded = GrammarSchema::loadFromText(schemaText);
    if (!loaded.has_value()) {
        ADD_FAILURE() << "the inline schema did not load: "
                      << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
        return {};
    }
    Tokenizer t{SourceBuffer::fromString(std::move(sourceText), "<inline>"), *loaded,
                DiagnosticBudget::libraryDefault()};
    auto [stream, reporter] = std::move(t).tokenize();
    Lexed out;
    while (!stream.isAtEnd()) out.tokens.push_back(stream.advance());
    auto const all = reporter->all();
    out.diags.assign(all.begin(), all.end());
    return out;
}

} // namespace

TEST(TokenizerDelimiterTag, TheBodyClosesOnlyAtTheDelimiterFollowedByItsTag) {
    // `<<EOF` captures EOF; `>>X` is body (its tag differs); `>>EOF` closes. Tokens: the
    // opener, five body bytes (a > > X b), the close.
    auto const lexed = lexWith(kTagSchema, "<<EOFa>>Xb>>EOF");
    EXPECT_TRUE(lexed.diags.empty()) << lexed.diags.front().actual;
    ASSERT_EQ(lexed.tokens.size(), 7u);
    EXPECT_EQ(lexed.tokens.back().span.length(), 5u) << "the close is `>>EOF`";
}

TEST(TokenizerDelimiterTag, TheTagRunStopsAtItsMaximumLength) {
    // maxLength 4: `<<ABCDEF` captures ABCD, so EF is body and `>>ABCD` closes.
    auto const lexed = lexWith(kTagSchema, "<<ABCDEFx>>ABCD");
    EXPECT_TRUE(lexed.diags.empty()) << lexed.diags.front().actual;
    ASSERT_EQ(lexed.tokens.size(), 5u) << "the opener, E F x, the close";
    EXPECT_EQ(lexed.tokens.back().span.length(), 6u) << "the close is `>>ABCD`";
}

TEST(TokenizerDelimiterTag, ARunShorterThanTheMinimumIsNoTagAndIsNotConsumed) {
    // minLength 2: `<<A` holds a run of ONE admitted byte — no tag, nothing consumed — so
    // `A` is body and the bare `>>` closes.
    auto const lexed = lexWith(kMinTwoSchema, "<<Ax>>");
    EXPECT_TRUE(lexed.diags.empty()) << lexed.diags.front().actual;
    ASSERT_EQ(lexed.tokens.size(), 4u) << "the opener, A x, the close";
    EXPECT_EQ(lexed.tokens.back().span.length(), 2u) << "the close is the bare `>>`";
}

TEST(TokenizerDelimiterTag, AMillionByteTagIsReadByALoop) {
    // The tag is read over its whole bounded run with no recursion: a million admitted
    // bytes are one tag, and the body closes at `>>` followed by all of them.
    std::string const tag(1'000'000, 'Q');
    auto const lexed = lexWith(kLongTagSchema, "<<" + tag + "x>>" + tag);
    EXPECT_TRUE(lexed.diags.empty()) << lexed.diags.front().actual;
    ASSERT_EQ(lexed.tokens.size(), 3u) << "the opener, x, the close";
    EXPECT_EQ(lexed.tokens.back().span.length(), 2u + tag.size());
}

TEST(TokenizerDelimiterTag, AReplacedFrameCapturesItsStylesTag) {
    // `[` pushes `plain`; `@` REPLACES it with the tagged body, capturing EOF; `>>EOF`
    // closes. Before fold f26d a replaced frame captured no tag, so its body closed at the
    // bare `>>` and `EOF` fell to the main scan.
    auto const lexed = lexWith(kTagSchema, "[@EOFa>>EOF");
    EXPECT_TRUE(lexed.diags.empty()) << lexed.diags.front().actual;
    ASSERT_EQ(lexed.tokens.size(), 4u) << "[, @, a, the close";
    EXPECT_EQ(lexed.tokens.back().span.length(), 5u) << "the close is `>>EOF`";
}
