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
    EXPECT_FALSE(style->delimiterTag.has_value());
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

// P69 (lane `cs`, fold f26d, ruling 643 (3)): the dynamic tag is a BOUNDED tag the
// document states whole — `characters`, `minLength`, `maxLength` — read by a loop.
TEST(StringStyleLoader, RawStringMatchedDelimiterTagPopulates) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "R\"": [{ "kind": "RawStringStart",
                   "stringStyle": {
                     "escapeKind":   "none",
                     "endsAt":       ")\"",
                     "delimiterTag": "matched",
                     "tag": { "characters": "A-Za-z0-9_", "minLength": 0, "maxLength": 16 }
                   } }]
      },
      "shapes": { "root": { "sequence": [ "RawStringStart" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    auto const& m = (*loaded)->lookupLexeme("R\"");
    auto const* style = (*loaded)->stringStyle(m[0]);
    ASSERT_NE(style, nullptr);
    EXPECT_EQ(style->escapeKind, EscapeKind::None);
    ASSERT_TRUE(style->delimiterTag.has_value())
        << "a present delimiterTag IS the dynamic-tag signal";
    DelimiterTag const& tag = *style->delimiterTag;
    EXPECT_EQ(tag.minLength, 0u);
    EXPECT_EQ(tag.maxLength, 16u);
    // Every byte: admitted exactly when it is a letter, a digit or `_`.
    for (unsigned b = 0; b < 256; ++b) {
        bool const want = (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z')
                          || (b >= '0' && b <= '9') || b == '_';
        EXPECT_EQ(tag.characters[b], want) << "byte " << b;
    }
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

TEST(StringStyleLoader, CustomTagBoundsRetained) {
    // A `-` that is first or last is the byte itself, never a range.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "R\"": [{ "kind": "RawStringStart",
                   "stringStyle": {
                     "escapeKind":   "none",
                     "endsAt":       ")\"",
                     "delimiterTag": "matched",
                     "tag": { "characters": "-a-c#", "minLength": 1, "maxLength": 8,
                              "$comment": "a documentation key passes" }
                   } }]
      },
      "shapes": { "root": { "sequence": [ "RawStringStart" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    auto const* style = (*loaded)->stringStyle((*loaded)->lookupLexeme("R\"")[0]);
    ASSERT_NE(style, nullptr);
    ASSERT_TRUE(style->delimiterTag.has_value());
    EXPECT_EQ(style->delimiterTag->minLength, 1u);
    EXPECT_EQ(style->delimiterTag->maxLength, 8u);
    std::string admitted;
    for (unsigned b = 0; b < 256; ++b)
        if (style->delimiterTag->characters[b]) admitted.push_back(static_cast<char>(b));
    EXPECT_EQ(admitted, "#-abc");
}

TEST(StringStyleLoader, ATagRangeEndingInADashIsTheDashItself) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "R\"": [{ "kind": "RawStringStart",
                   "stringStyle": {
                     "escapeKind":   "none",
                     "endsAt":       ")\"",
                     "delimiterTag": "matched",
                     "tag": { "characters": "x-", "minLength": 0, "maxLength": 2 }
                   } }]
      },
      "shapes": { "root": { "sequence": [ "RawStringStart" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    auto const* style = (*loaded)->stringStyle((*loaded)->lookupLexeme("R\"")[0]);
    ASSERT_NE(style, nullptr);
    ASSERT_TRUE(style->delimiterTag.has_value());
    std::string admitted;
    for (unsigned b = 0; b < 256; ++b)
        if (style->delimiterTag->characters[b]) admitted.push_back(static_cast<char>(b));
    EXPECT_EQ(admitted, "-x");
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

// ── The bounded tag's refusals, each BY NAME (P69, fold f26d, ruling 643 (3)) ──
//
// Every row is one `stringStyle` object on a raw-string opener; each must be refused with
// C_InvalidStringStyle at the named path, with the named words in the message — and be the
// ONLY error, so a row cannot pass on another key's refusal.
namespace {

struct TagRefusal {
    char const* name;
    char const* style;      // the stringStyle object's text
    char const* pathEnd;    // the diagnostic's path ends so
    char const* says;       // the message holds these words
};

constexpr TagRefusal kTagRefusals[] = {
    {"the retired tagPattern",
     R"J({ "escapeKind": "none", "endsAt": ")\"", "delimiterTag": "matched", "tagPattern": "[a-z]{1,8}" })J",
     "/stringStyle/tagPattern", "'tagPattern' is retired"},
    {"the retired tagPattern without delimiterTag",
     R"J({ "escapeKind": "none", "endsAt": ")\"", "tagPattern": "[a-z]+" })J",
     "/stringStyle/tagPattern", "'tagPattern' is retired"},
    {"delimiterTag without tag",
     R"J({ "escapeKind": "none", "endsAt": ")\"", "delimiterTag": "matched" })J",
     "/stringStyle/tag", "requires 'tag'"},
    {"tag without delimiterTag",
     R"J({ "escapeKind": "none", "endsAt": ")\"", "tag": { "characters": "a-z", "minLength": 0, "maxLength": 8 } })J",
     "/stringStyle/tag", "'tag' requires 'delimiterTag: \"matched\"'"},
    {"a tag that is no object",
     R"J({ "escapeKind": "none", "endsAt": ")\"", "delimiterTag": "matched", "tag": "a-z" })J",
     "/stringStyle/tag", "'tag' must be an object"},
    {"a missing maxLength",
     R"J({ "escapeKind": "none", "endsAt": ")\"", "delimiterTag": "matched", "tag": { "characters": "a-z", "minLength": 0 } })J",
     "/stringStyle/tag/maxLength", "at most maxLength characters"},
    {"a missing minLength",
     R"J({ "escapeKind": "none", "endsAt": ")\"", "delimiterTag": "matched", "tag": { "characters": "a-z", "maxLength": 8 } })J",
     "/stringStyle/tag/minLength", "'minLength' is required"},
    {"a fractional maxLength",
     R"J({ "escapeKind": "none", "endsAt": ")\"", "delimiterTag": "matched", "tag": { "characters": "a-z", "minLength": 0, "maxLength": 8.5 } })J",
     "/stringStyle/tag/maxLength", "non-negative integer"},
    {"a negative minLength",
     R"J({ "escapeKind": "none", "endsAt": ")\"", "delimiterTag": "matched", "tag": { "characters": "a-z", "minLength": -1, "maxLength": 8 } })J",
     "/stringStyle/tag/minLength", "non-negative integer"},
    {"a maxLength spelled as a string",
     R"J({ "escapeKind": "none", "endsAt": ")\"", "delimiterTag": "matched", "tag": { "characters": "a-z", "minLength": 0, "maxLength": "8" } })J",
     "/stringStyle/tag/maxLength", "non-negative integer"},
    {"a maxLength of zero",
     R"J({ "escapeKind": "none", "endsAt": ")\"", "delimiterTag": "matched", "tag": { "characters": "a-z", "minLength": 0, "maxLength": 0 } })J",
     "/stringStyle/tag/maxLength", "at least one byte"},
    {"a maxLength below the minLength",
     R"J({ "escapeKind": "none", "endsAt": ")\"", "delimiterTag": "matched", "tag": { "characters": "a-z", "minLength": 4, "maxLength": 3 } })J",
     "/stringStyle/tag/maxLength", "at least 'minLength'"},
    {"an empty character set",
     R"J({ "escapeKind": "none", "endsAt": ")\"", "delimiterTag": "matched", "tag": { "characters": "", "minLength": 0, "maxLength": 8 } })J",
     "/stringStyle/tag/characters", "'characters' is required"},
    {"a missing character set",
     R"J({ "escapeKind": "none", "endsAt": ")\"", "delimiterTag": "matched", "tag": { "minLength": 0, "maxLength": 8 } })J",
     "/stringStyle/tag/characters", "'characters' is required"},
    {"a reversed range",
     R"J({ "escapeKind": "none", "endsAt": ")\"", "delimiterTag": "matched", "tag": { "characters": "z-a", "minLength": 0, "maxLength": 8 } })J",
     "/stringStyle/tag/characters", "reversed range 'z-a'"},
    {"a byte outside ASCII",
     R"J({ "escapeKind": "none", "endsAt": ")\"", "delimiterTag": "matched", "tag": { "characters": "aé", "minLength": 0, "maxLength": 8 } })J",
     "/stringStyle/tag/characters", "is not ASCII"},
    {"an unknown tag key",
     R"J({ "escapeKind": "none", "endsAt": ")\"", "delimiterTag": "matched", "tag": { "characters": "a-z", "minLength": 0, "maxLength": 8, "maxLen": 4 } })J",
     "/stringStyle/tag/maxLen", "unknown key 'maxLen'"},
    {"an unknown stringStyle key",
     R"J({ "escapeKind": "none", "endsAt": "\"", "multiLine": true })J",
     "/stringStyle/multiLine", "unknown key 'multiLine'"},
};

} // namespace

TEST(StringStyleLoader, EveryMalformedDelimiterTagIsRefusedByName) {
    for (TagRefusal const& row : kTagRefusals) {
        SCOPED_TRACE(row.name);
        std::string const cfg = std::string{R"JSON({
          "dssSchemaVersion": 2,
          "language": { "name": "X", "version": "0.1.0" },
          "tokens": { "R\"": [{ "kind": "RawStringStart", "stringStyle": )JSON"}
                                + row.style + R"JSON( }] },
          "shapes": { "root": { "sequence": [ "RawStringStart" ] } }
        })JSON";
        auto loaded = GrammarSchema::loadFromText(cfg);
        ASSERT_FALSE(loaded.has_value()) << "the document loaded";
        auto const errors = std::ranges::count_if(loaded.error(), [](auto const& d) {
            return d.severity != DiagnosticSeverity::Warning;
        });
        EXPECT_EQ(errors, 1) << "one refusal, and only it";
        EXPECT_TRUE(std::ranges::any_of(loaded.error(), [&](auto const& d) {
            return d.code == DiagnosticCode::C_InvalidStringStyle
                   && d.path.ends_with(row.pathEnd)
                   && d.message.find(row.says) != std::string::npos;
        })) << "first diagnostic: " << loaded.error()[0].path << ": " << loaded.error()[0].message;
    }
}

// The control: the same opener with a WELL-FORMED tag loads, so each refusal above is
// its own key's.
TEST(StringStyleLoader, AWellFormedDelimiterTagLoads) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "R\"": [{ "kind": "RawStringStart", "stringStyle":
        { "escapeKind": "none", "endsAt": ")\"", "delimiterTag": "matched",
          "$tagComment": "a documentation key passes",
          "tag": { "characters": "a-z", "minLength": 4, "maxLength": 4 } } }] },
      "shapes": { "root": { "sequence": [ "RawStringStart" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
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

// c22 (D-PP-LINE-COMMENT-BEFORE-DIRECTIVE): the new `endsAtExclusive` bool must
// be type-validated like the other optional bool fields — a non-bool value is a
// loud config error, not a silent ignore.
TEST(StringStyleLoader, WrongTypeEndsAtExclusiveIsError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "\"": [{ "kind": "StringStart",
                  "stringStyle": { "escapeKind":      "none",
                                    "endsAt":          "\"",
                                    "endsAtExclusive": "yes" } }]
      },
      "shapes": { "root": { "sequence": [ "StringStart" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_InvalidStringStyle &&
               d.path.find("endsAtExclusive") != std::string::npos;
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

TEST(StringStyleLoader, CConfigStillLoads) {
    auto loaded = GrammarSchema::loadShipped("c");
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

// ── C2 (a tag without delimiterTag is a load error) is a row of
// EveryMalformedDelimiterTagIsRefusedByName since P69 fold f26d. ──────────

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
    // ⚠ THE TWO LABELS ARE LOAD-BEARING AS OF 2026-08-25 (cycle P35), AND THIS
    // IS THE ARM WHERE IT MATTERS MOST. The content-addressed memo
    // (D-CONFIG-A-SCHEMA-DOCUMENT-IS-REBUILT-ONCE-PER-LOAD-INSIDE-ONE-PROCESS)
    // keys on (schema family, sourceLabel, digest), so under the DEFAULT label
    // these two loads would be ONE schema — the lookup would be same-schema,
    // legal, and would NOT abort, silently turning a fail-loud guard into a
    // green test that pins nothing. Distinct labels give two genuinely
    // different schemas, which is what "cross-schema" has always meant here.
    // The subject and the expected abort message are unchanged.
    auto loadedA = GrammarSchema::loadFromText(kCfg, "cross-schema-a");
    auto loadedB = GrammarSchema::loadFromText(kCfg, "cross-schema-b");
    ASSERT_TRUE(loadedA.has_value());
    ASSERT_TRUE(loadedB.has_value());
    ASSERT_NE(loadedA->get(), loadedB->get())
        << "the two loads returned the SAME schema, so the lookup below is not "
           "a CROSS-schema lookup at all and could not abort for the reason "
           "this test exists to pin";
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
