#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/string_style.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <format>
#include <set>
#include <string>
#include <string_view>

using namespace dss;

// ── escapeKindName helper ──────────────────────────────────────────────

TEST(StringStyle, EscapeKindNameMapping) {
    EXPECT_EQ(escapeKindName(EscapeKind::None),             "none");
    EXPECT_EQ(escapeKindName(EscapeKind::Char),             "char");
    EXPECT_EQ(escapeKindName(EscapeKind::DoubledDelimiter), "doubled-delimiter");
}

// ── bracketInnerText: doubled-delimiter un-escaping ────────────────────
//
// The shared decoder both the semantic engine and the import resolver call
// to read a bracket-quoted identifier's body. It MUST match the tokenizer's
// `EscapeKind::DoubledDelimiter` rule for the `[` opener: a `]]` is an
// escaped literal `]`, a lone `]` is the close.

TEST(StringStyle, BracketInnerTextPlain) {
    EXPECT_EQ(bracketInnerText("[Orders]", 0), "Orders");
}

TEST(StringStyle, BracketInnerTextDoubledDelimiterUnescapes) {
    // `[a]]b]` → the single identifier `a]b` (the `]]` un-doubles to one `]`).
    EXPECT_EQ(bracketInnerText("[a]]b]", 0), "a]b");
    // `[Ord]]ers]` → `Ord]ers` (the ground-truth shape from the tsql tests).
    EXPECT_EQ(bracketInnerText("[Ord]]ers]", 0), "Ord]ers");
}

TEST(StringStyle, BracketInnerTextTrailingDoubledPair) {
    // A `]]` immediately before the close is two escaped `]` then the close:
    // `[a]]]]` → body bytes `]]` `]]`... actually `[` then `a` `]]` `]]` —
    // four `]` after `a` are two escaped pairs → `a]]`, then EOF with no
    // close → malformed → empty. Pin that EOF-without-close is empty.
    EXPECT_EQ(bracketInnerText("[a]]]]", 0), "");
    // `[a]]]` → `a` then `]]` (escaped `]`) then a lone `]` (close) → `a]`.
    EXPECT_EQ(bracketInnerText("[a]]]", 0), "a]");
}

TEST(StringStyle, BracketInnerTextMalformedReturnsEmpty) {
    EXPECT_EQ(bracketInnerText("Orders]", 0), "");   // no opener at `open`
    EXPECT_EQ(bracketInnerText("[Orders", 0), "");    // no close
    EXPECT_EQ(bracketInnerText("[", 0), "");          // bare opener
    EXPECT_EQ(bracketInnerText("", 0), "");           // empty source
    EXPECT_EQ(bracketInnerText("x[a]", 99), "");      // open past end
}

TEST(StringStyle, BracketInnerTextWithOffsetOpener) {
    // The opener need not be at byte 0 — read from an interior `[`.
    EXPECT_EQ(bracketInnerText("FROM [Orders];", 5), "Orders");
}

// ── Happy-path loader ──────────────────────────────────────────────────

TEST(StringStyleLoader, CStyleQuotedStringLoadsAndPopulates) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "\"": [{ "kind": "StringStart",
                  "stringStyle": {
                    "escapeKind": "char",
                    "escapeChar": "\\",
                    "endsAt":     "\""
                  } }]
      },
      "shapes": { "root": { "sequence": [ "StringStart" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);

    auto const& m = (*loaded)->lookupLexeme("\"");
    ASSERT_EQ(m.size(), 1u);
    auto const* style = (*loaded)->stringStyle(m[0]);
    ASSERT_NE(style, nullptr);
    EXPECT_EQ(style->escapeKind, EscapeKind::Char);
    EXPECT_EQ(style->escapeChar, '\\');
    EXPECT_EQ(style->endsAt, "\"");
    EXPECT_FALSE(style->endsAtLongestMatch);
    EXPECT_FALSE(style->multiline);
    EXPECT_TRUE(style->tagPattern.empty());
}

TEST(StringStyleLoader, VerbatimStringDoubledDelimiterPopulates) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "@\"": [{ "kind": "VerbatimStringStart",
                   "stringStyle": {
                     "escapeKind": "doubled-delimiter",
                     "endsAt":     "\""
                   } }]
      },
      "shapes": { "root": { "sequence": [ "VerbatimStringStart" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    auto const& m = (*loaded)->lookupLexeme("@\"");
    auto const* style = (*loaded)->stringStyle(m[0]);
    ASSERT_NE(style, nullptr);
    EXPECT_EQ(style->escapeKind, EscapeKind::DoubledDelimiter);
    EXPECT_EQ(style->endsAt, "\"");
}

TEST(StringStyleLoader, RawStringMatchedDelimiterTagPopulates) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "R\"": [{ "kind": "RawStringStart",
                   "stringStyle": {
                     "escapeKind":   "none",
                     "endsAt":       ")\"",
                     "delimiterTag": "matched"
                   } }]
      },
      "shapes": { "root": { "sequence": [ "RawStringStart" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    auto const& m = (*loaded)->lookupLexeme("R\"");
    auto const* style = (*loaded)->stringStyle(m[0]);
    ASSERT_NE(style, nullptr);
    EXPECT_EQ(style->escapeKind, EscapeKind::None);
    EXPECT_FALSE(style->tagPattern.empty())
        << "non-empty tagPattern IS the dynamic-tag signal";
    EXPECT_EQ(style->tagPattern, "[A-Za-z0-9_]{0,16}")
        << "delimiterTag: 'matched' without explicit tagPattern must default to "
           "[A-Za-z0-9_]{0,16}";
}

TEST(StringStyleLoader, TripleQuotedLongestMatchPopulates) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "\"\"\"": [{ "kind": "TripleStringStart",
                      "stringStyle": {
                        "escapeKind":         "char",
                        "escapeChar":         "\\",
                        "endsAt":             "\"\"\"",
                        "endsAtLongestMatch": true,
                        "multiline":          true
                      } }]
      },
      "shapes": { "root": { "sequence": [ "TripleStringStart" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    auto const& m = (*loaded)->lookupLexeme("\"\"\"");
    auto const* style = (*loaded)->stringStyle(m[0]);
    ASSERT_NE(style, nullptr);
    EXPECT_EQ(style->endsAt, "\"\"\"");
    EXPECT_TRUE(style->endsAtLongestMatch);
    EXPECT_TRUE(style->multiline);
}

TEST(StringStyleLoader, CustomTagPatternRetained) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "R\"": [{ "kind": "RawStringStart",
                   "stringStyle": {
                     "escapeKind":   "none",
                     "endsAt":       ")\"",
                     "delimiterTag": "matched",
                     "tagPattern":   "[a-z]{1,8}"
                   } }]
      },
      "shapes": { "root": { "sequence": [ "RawStringStart" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    auto const* style = (*loaded)->stringStyle((*loaded)->lookupLexeme("R\"")[0]);
    ASSERT_NE(style, nullptr);
    EXPECT_EQ(style->tagPattern, "[a-z]{1,8}");
}

// ── Loader error paths ─────────────────────────────────────────────────

TEST(StringStyleLoader, MissingEscapeKindIsError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "\"": [{ "kind": "StringStart", "stringStyle": { "endsAt": "\"" } }]
      },
      "shapes": { "root": { "sequence": [ "StringStart" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_InvalidStringStyle &&
               d.path.find("escapeKind") != std::string::npos;
    }));
}

TEST(StringStyleLoader, UnknownEscapeKindIsError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "\"": [{ "kind": "StringStart",
                  "stringStyle": { "escapeKind": "wonky", "endsAt": "\"" } }]
      },
      "shapes": { "root": { "sequence": [ "StringStart" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_InvalidStringStyle &&
               d.message.find("wonky") != std::string::npos;
    }));
}

TEST(StringStyleLoader, CharEscapeKindRequiresEscapeChar) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "\"": [{ "kind": "StringStart",
                  "stringStyle": { "escapeKind": "char", "endsAt": "\"" } }]
      },
      "shapes": { "root": { "sequence": [ "StringStart" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_InvalidStringStyle &&
               d.path.find("escapeChar") != std::string::npos;
    }));
}

TEST(StringStyleLoader, EscapeCharMustBeSingleCharacter) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "\"": [{ "kind": "StringStart",
                  "stringStyle": { "escapeKind": "char",
                                    "escapeChar": "\\\\",
                                    "endsAt":     "\"" } }]
      },
      "shapes": { "root": { "sequence": [ "StringStart" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_InvalidStringStyle;
    }));
}

TEST(StringStyleLoader, MissingEndsAtIsError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "\"": [{ "kind": "StringStart",
                  "stringStyle": { "escapeKind": "none" } }]
      },
      "shapes": { "root": { "sequence": [ "StringStart" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_MissingField &&
               d.path.find("endsAt") != std::string::npos;
    }));
}

TEST(StringStyleLoader, EmptyEndsAtIsError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "\"": [{ "kind": "StringStart",
                  "stringStyle": { "escapeKind": "none", "endsAt": "" } }]
      },
      "shapes": { "root": { "sequence": [ "StringStart" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_MissingField &&
               d.path.find("endsAt") != std::string::npos;
    }));
}

TEST(StringStyleLoader, UnknownDelimiterTagValueIsError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "R\"": [{ "kind": "RawStringStart",
                   "stringStyle": { "escapeKind":   "none",
                                     "endsAt":       ")\"",
                                     "delimiterTag": "static" } }]
      },
      "shapes": { "root": { "sequence": [ "RawStringStart" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_InvalidStringStyle &&
               d.path.find("delimiterTag") != std::string::npos;
    }));
}

TEST(StringStyleLoader, InvalidTagPatternRegexIsError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "R\"": [{ "kind": "RawStringStart",
                   "stringStyle": { "escapeKind":   "none",
                                     "endsAt":       ")\"",
                                     "delimiterTag": "matched",
                                     "tagPattern":   "[a-z" } }]
      },
      "shapes": { "root": { "sequence": [ "RawStringStart" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_InvalidStringStyle &&
               d.path.find("tagPattern") != std::string::npos;
    }));
}

TEST(StringStyleLoader, WrongTypeBoolFieldIsError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "\"": [{ "kind": "StringStart",
                  "stringStyle": { "escapeKind": "none",
                                    "endsAt":     "\"",
                                    "multiline":  "yes" } }]
      },
      "shapes": { "root": { "sequence": [ "StringStart" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_InvalidStringStyle &&
               d.path.find("multiline") != std::string::npos;
    }));
}

TEST(StringStyleLoader, StringStyleWrongTypeIsError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "\"": [{ "kind": "StringStart", "stringStyle": "char" }] },
      "shapes": { "root": { "sequence": [ "StringStart" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_InvalidStringStyle;
    }));
}

// ── Keyword rejection ──────────────────────────────────────────────────

TEST(StringStyleLoader, KeywordWithStringStyleIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "keywords": [
        { "word": "string", "kind": "StringKw",
          "stringStyle": { "escapeKind": "none", "endsAt": "\"" } }
      ],
      "shapes": { "root": { "sequence": [ "StringKw" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_ConflictingField &&
               d.message.find("keywords are word-shaped") != std::string::npos;
    }));
}

// ── Backwards-compat: no stringStyle means no metadata ─────────────────

TEST(StringStyleLoader, TokenWithoutStringStyleHasNoMetadata) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    auto const& m = (*loaded)->lookupLexeme("+");
    ASSERT_EQ(m.size(), 1u);
    EXPECT_FALSE(m[0].stringStyleId.valid());
    EXPECT_EQ((*loaded)->stringStyle(m[0]), nullptr);
}

TEST(StringStyleLoader, ToyConfigStillLoads) {
    auto loaded = GrammarSchema::loadShipped("toy");
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
}

TEST(StringStyleLoader, CSubsetConfigStillLoads) {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
}

// ── Multiple stringStyle entries — pool indexing ───────────────────────

TEST(StringStyleLoader, MultipleStringStylesAllocateDistinctIndices) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "\"":  [{ "kind": "StringStart",
                   "stringStyle": { "escapeKind": "char",
                                     "escapeChar": "\\",
                                     "endsAt":     "\"" } }],
        "@\"": [{ "kind": "VerbatimStringStart",
                   "stringStyle": { "escapeKind": "doubled-delimiter",
                                     "endsAt":     "\"" } }]
      },
      "shapes": { "root": { "alt": [ "StringStart", "VerbatimStringStart" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    auto const& a = (*loaded)->lookupLexeme("\"")[0];
    auto const& b = (*loaded)->lookupLexeme("@\"")[0];
    ASSERT_TRUE(a.stringStyleId.valid());
    ASSERT_TRUE(b.stringStyleId.valid());
    EXPECT_NE(a.stringStyleId.v, b.stringStyleId.v);

    auto const* styleA = (*loaded)->stringStyle(a);
    auto const* styleB = (*loaded)->stringStyle(b);
    ASSERT_NE(styleA, nullptr);
    ASSERT_NE(styleB, nullptr);
    EXPECT_EQ(styleA->escapeKind, EscapeKind::Char);
    EXPECT_EQ(styleB->escapeKind, EscapeKind::DoubledDelimiter);
}

// ── C1: escapeChar with non-Char escapeKind now fail-fast ──────────────

TEST(StringStyleLoader, EscapeCharWithNonCharKindIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "\"": [{ "kind": "StringStart",
                  "stringStyle": { "escapeKind": "none",
                                    "escapeChar": "\\",
                                    "endsAt":     "\"" } }]
      },
      "shapes": { "root": { "sequence": [ "StringStart" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_InvalidStringStyle &&
               d.message.find("only meaningful when escapeKind is 'char'") != std::string::npos;
    }));
}

// ── C2: tagPattern without delimiterTag is now a load error ────────────

TEST(StringStyleLoader, TagPatternWithoutDelimiterTagIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "R\"": [{ "kind": "RawStringStart",
                   "stringStyle": { "escapeKind": "none",
                                     "endsAt":     ")\"",
                                     "tagPattern": "[a-z]+" } }]
      },
      "shapes": { "root": { "sequence": [ "RawStringStart" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_InvalidStringStyle &&
               d.message.find("requires 'delimiterTag: \"matched\"'") != std::string::npos;
    }));
}

// ── C4: out-of-range stringStyleId aborts ──────────────────────────────

TEST(StringStyleDeath, OutOfRangeStringStyleIdAborts) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    LexemeMeaning fake = (*loaded)->lookupLexeme("+")[0];
    fake.stringStyleId = StringStyleId{999};  // out of range
    EXPECT_DEATH({ (void)(*loaded)->stringStyle(fake); },
                 "out-of-range stringStyleId");
}

// ── C3: cross-schema stringStyle lookup aborts ─────────────────────────

TEST(StringStyleDeath, CrossSchemaStringStyleLookupAborts) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loadedA = GrammarSchema::loadFromText(kCfg);
    auto loadedB = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loadedA.has_value());
    ASSERT_TRUE(loadedB.has_value());
    LexemeMeaning fromA = (*loadedA)->lookupLexeme("+")[0];
    EXPECT_DEATH({ (void)(*loadedB)->stringStyle(fromA); },
                 "belongs to a different schema");
}

TEST(StringStyle, SchemaIdStampedOnMeanings) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    auto const& m = (*loaded)->lookupLexeme("+")[0];
    EXPECT_EQ(m.schemaId.v, (*loaded)->schemaId().v);
    EXPECT_TRUE(m.schemaId.valid());
}

// ── S1: pool stress — many stringStyles allocate distinct ids ──────────

TEST(StringStyleLoader, ManyStringStylesGetDistinctIds) {
    // Build a config with 50 string-opener tokens, each with a
    // stringStyle. Verify every meaning gets a distinct, valid
    // StringStyleId and `stringStyle(m)` resolves correctly across the
    // full pool — guards against future pool-storage regressions that
    // might invalidate ids on growth.
    std::string cfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {)JSON";
    for (int i = 0; i < 50; ++i) {
        if (i > 0) cfg += ",";
        cfg += std::format(R"JSON(
            "tok{}": [{{ "kind": "Tok{}",
                          "stringStyle": {{ "escapeKind": "none",
                                             "endsAt":     "end{}" }} }}])JSON",
                            i, i, i);
    }
    cfg += R"JSON(
      },
      "shapes": { "root": { "sequence": [ "Tok0" ] } }
    })JSON";

    auto loaded = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);

    std::set<std::uint32_t> seenIds;
    for (int i = 0; i < 50; ++i) {
        const auto lex = std::format("tok{}", i);
        auto const& m = (*loaded)->lookupLexeme(lex);
        ASSERT_EQ(m.size(), 1u) << "tok" << i;
        ASSERT_TRUE(m[0].stringStyleId.valid()) << "tok" << i;
        EXPECT_TRUE(seenIds.insert(m[0].stringStyleId.v).second)
            << "duplicate stringStyleId at tok" << i;

        auto const* style = (*loaded)->stringStyle(m[0]);
        ASSERT_NE(style, nullptr) << "tok" << i;
        EXPECT_EQ(style->endsAt, std::format("end{}", i));
    }
    EXPECT_EQ(seenIds.size(), 50u);
}

// ── S2: endsAtLongestMatch + 1-char endsAt warns ───────────────────────

TEST(StringStyleLoader, LongestMatchWithSingleCharEndsAtWarns) {
    // Pair with a sibling ambiguity error so the diagnostic vector
    // reaches the test via the error channel.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "\"": [{ "kind": "StringStart",
                  "stringStyle": { "escapeKind":        "none",
                                    "endsAt":            "\"",
                                    "endsAtLongestMatch": true } }]
      },
      "shapes": {
        "root":   { "alt": ["A", "B"] },
        "A":      { "sequence": [ "StringStart" ] },
        "B":      { "sequence": [ "StringStart" ] }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_RedundantField &&
               d.message.find("1-character 'endsAt'") != std::string::npos;
    }));
}
