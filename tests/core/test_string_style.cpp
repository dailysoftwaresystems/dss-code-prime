#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/string_style.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>

using namespace dss;

// ── escapeKindName helper ──────────────────────────────────────────────

TEST(StringStyle, EscapeKindNameMapping) {
    EXPECT_EQ(escapeKindName(EscapeKind::None),             "none");
    EXPECT_EQ(escapeKindName(EscapeKind::Char),             "char");
    EXPECT_EQ(escapeKindName(EscapeKind::DoubledDelimiter), "doubled-delimiter");
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
    EXPECT_FALSE(style->hasMatchedDelimiterTag);
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
    EXPECT_TRUE(style->hasMatchedDelimiterTag);
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
    EXPECT_FALSE(m[0].stringStyleIdx.has_value());
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
    ASSERT_TRUE(a.stringStyleIdx.has_value());
    ASSERT_TRUE(b.stringStyleIdx.has_value());
    EXPECT_NE(*a.stringStyleIdx, *b.stringStyleIdx);

    auto const* styleA = (*loaded)->stringStyle(a);
    auto const* styleB = (*loaded)->stringStyle(b);
    ASSERT_NE(styleA, nullptr);
    ASSERT_NE(styleB, nullptr);
    EXPECT_EQ(styleA->escapeKind, EscapeKind::Char);
    EXPECT_EQ(styleB->escapeKind, EscapeKind::DoubledDelimiter);
}
