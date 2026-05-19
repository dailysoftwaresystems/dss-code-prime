#include "core/types/grammar_schema.hpp"
#include "core/types/lexer_mode.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>

using namespace dss;

// ── LexerModeStack stub-driver ──────────────────────────────────────────

TEST(LexerModeStack, EmptyAtConstruction) {
    LexerModeStack s;
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.depth(), 0u);
    EXPECT_FALSE(s.top().valid());
}

TEST(LexerModeStack, PushPopRoundTrip) {
    LexerModeStack s;
    s.push(LexerModeId{1});
    EXPECT_EQ(s.depth(), 1u);
    EXPECT_EQ(s.top().v, 1u);

    s.push(LexerModeId{2});
    EXPECT_EQ(s.depth(), 2u);
    EXPECT_EQ(s.top().v, 2u);

    s.pop();
    EXPECT_EQ(s.depth(), 1u);
    EXPECT_EQ(s.top().v, 1u);

    s.pop();
    EXPECT_TRUE(s.empty());
}

TEST(LexerModeStack, PopOnEmptyIsNoOp) {
    LexerModeStack s;
    s.pop();                                          // no abort, no UB
    EXPECT_TRUE(s.empty());
}

TEST(LexerModeStack, ReplaceTopSwapsWithoutNesting) {
    LexerModeStack s;
    s.push(LexerModeId{1});
    s.push(LexerModeId{2});
    s.replaceTop(LexerModeId{3});
    EXPECT_EQ(s.depth(), 2u);
    EXPECT_EQ(s.top().v, 3u);
}

TEST(LexerModeStack, ReplaceTopOnEmptyIsNoOp) {
    LexerModeStack s;
    s.replaceTop(LexerModeId{1});
    EXPECT_TRUE(s.empty());
}

TEST(LexerModeStack, ApplyDispatches) {
    LexerModeStack s;
    s.apply(ModeOp::PushMode, LexerModeId{1});
    EXPECT_EQ(s.top().v, 1u);
    s.apply(ModeOp::PushMode, LexerModeId{2});
    EXPECT_EQ(s.top().v, 2u);
    s.apply(ModeOp::ReplaceMode, LexerModeId{3});
    EXPECT_EQ(s.top().v, 3u);
    s.apply(ModeOp::PopMode, LexerModeId{});
    EXPECT_EQ(s.top().v, 1u);
    s.apply(ModeOp::None, LexerModeId{99});           // no effect
    EXPECT_EQ(s.top().v, 1u);
}

TEST(LexerModeStack, NestedInterpolationRoundTrip) {
    // Mirror $"a {$"{b}"} c": push string-body, push main, pop, push
    // string-body again, pop, pop. Stack must return to empty.
    const LexerModeId stringBody{2};
    const LexerModeId main{1};

    LexerModeStack s;
    s.push(stringBody);
    s.push(main);
    s.pop();
    s.push(stringBody);
    s.pop();
    s.pop();
    EXPECT_TRUE(s.empty());
}

TEST(LexerModeStack, SnapshotRestoreRoundTrip) {
    LexerModeStack s;
    s.push(LexerModeId{1});
    s.push(LexerModeId{2});

    auto snap = s.snapshot();
    s.push(LexerModeId{3});
    s.push(LexerModeId{4});
    EXPECT_EQ(s.depth(), 4u);

    s.restore(snap);
    EXPECT_EQ(s.depth(), 2u);
    EXPECT_EQ(s.top().v, 2u);
}

TEST(LexerModeStack, SnapshotIsValueIndependent) {
    LexerModeStack s;
    s.push(LexerModeId{1});
    auto snap = s.snapshot();

    s.push(LexerModeId{2});
    s.pop();
    s.pop();
    EXPECT_TRUE(s.empty());

    s.restore(snap);
    EXPECT_EQ(s.depth(), 1u);
    EXPECT_EQ(s.top().v, 1u);
}

// ── modeOpName helper ───────────────────────────────────────────────────

TEST(LexerMode, ModeOpNameMapping) {
    EXPECT_EQ(modeOpName(ModeOp::None),        "none");
    EXPECT_EQ(modeOpName(ModeOp::PushMode),    "pushMode");
    EXPECT_EQ(modeOpName(ModeOp::PopMode),     "popMode");
    EXPECT_EQ(modeOpName(ModeOp::ReplaceMode), "replaceMode");
}

// ── Loader: lexerModes registration ────────────────────────────────────

TEST(LexerModesLoader, V1ConfigSynthesizesMainMode) {
    // No `lexerModes` field at all — loader should still synthesize "main"
    // so consumers can pull from lexerModes() without special-casing v1.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    auto const& s = **loaded;

    const auto mainId = s.findLexerMode("main");
    EXPECT_TRUE(mainId.valid());
    EXPECT_EQ(s.lexerMode(mainId).name, "main");

    auto plusOpInMain = s.lookupLexemeInMode(mainId, "+");
    EXPECT_FALSE(plusOpInMain.empty()) << "main mode must inherit top-level tokens";
}

TEST(LexerModesLoader, DeclaredModesRegistered) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } },
      "lexerModes": {
        "main":        { "tokens": "default" },
        "string-body": { "defaultToken": { "kind": "StringChar" } }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    auto const& s = **loaded;

    EXPECT_TRUE(s.findLexerMode("main").valid());
    EXPECT_TRUE(s.findLexerMode("string-body").valid());
    EXPECT_FALSE(s.findLexerMode("nonexistent").valid());
}

TEST(LexerModesLoader, DefaultTokenPopulated) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } },
      "lexerModes": {
        "string-body": { "defaultToken": { "kind": "StringChar" } }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    auto const& s = **loaded;

    const auto modeId = s.findLexerMode("string-body");
    ASSERT_TRUE(modeId.valid());
    auto const& mode = s.lexerMode(modeId);
    ASSERT_TRUE(mode.defaultToken.has_value());
    EXPECT_EQ(s.schemaTokens().name(*mode.defaultToken), "StringChar");
}

// ── Loader: modeOp / modeArg parsing on token meanings ──────────────────

TEST(LexerModesLoader, ModeOpPushModePopulatesMeaning) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "$\"": [{ "kind": "InterpString", "modeOp": "pushMode", "modeArg": "string-body" }]
      },
      "shapes": { "root": { "sequence": [ "InterpString" ] } },
      "lexerModes": { "string-body": { } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    auto const& m = (*loaded)->lookupLexeme("$\"");
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(m[0].modeOp, ModeOp::PushMode);
    EXPECT_TRUE(m[0].modeArg.valid());
    EXPECT_EQ((*loaded)->lexerMode(m[0].modeArg).name, "string-body");
}

TEST(LexerModesLoader, ModeOpPopModeRequiresNoArg) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "\"": [{ "kind": "StringEnd", "modeOp": "popMode" }]
      },
      "shapes": { "root": { "sequence": [ "StringEnd" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    auto const& m = (*loaded)->lookupLexeme("\"");
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(m[0].modeOp, ModeOp::PopMode);
    EXPECT_FALSE(m[0].modeArg.valid());
}

TEST(LexerModesLoader, UnknownModeArgIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "$\"": [{ "kind": "InterpString", "modeOp": "pushMode", "modeArg": "fictional" }]
      },
      "shapes": { "root": { "sequence": [ "InterpString" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_UnknownLexerMode &&
               d.path.find("modeArg") != std::string::npos;
    }));
}

TEST(LexerModesLoader, UnknownModeOpIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "$\"": [{ "kind": "InterpString", "modeOp": "warpMode", "modeArg": "main" }]
      },
      "shapes": { "root": { "sequence": [ "InterpString" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_ConflictingField &&
               d.message.find("warpMode") != std::string::npos;
    }));
}

TEST(LexerModesLoader, PushModeWithoutModeArgIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "$\"": [{ "kind": "InterpString", "modeOp": "pushMode" }]
      },
      "shapes": { "root": { "sequence": [ "InterpString" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_ConflictingField &&
               d.message.find("requires 'modeArg'") != std::string::npos;
    }));
}

TEST(LexerModesLoader, ModeArgWithPopModeWarns) {
    // The warning doesn't fail the load. Pair with a sibling error
    // (ambiguity-detect) to surface the warning via the error vector.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "\"": [{ "kind": "StringEnd", "modeOp": "popMode", "modeArg": "main" }]
      },
      "shapes": {
        "root":      { "alt": ["A", "B"] },
        "A":         { "sequence": [ "StringEnd" ] },
        "B":         { "sequence": [ "StringEnd" ] }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_RedundantField &&
               d.message.find("popMode") != std::string::npos;
    }));
}

TEST(LexerModesLoader, ModeArgWithoutModeOpIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "$\"": [{ "kind": "InterpString", "modeArg": "main" }]
      },
      "shapes": { "root": { "sequence": [ "InterpString" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_ConflictingField &&
               d.message.find("'modeArg' is meaningless") != std::string::npos;
    }));
}

// ── Cyclic mode references load cleanly ─────────────────────────────────

TEST(LexerModesLoader, CyclicModeReferencesAcceptedAtLoad) {
    // main pushes string-body; string-body pushes main. Cyclic mode
    // references are NORMAL (interpolation revisits the outer mode);
    // the loader must accept them.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "$\"": [{ "kind": "InterpStart", "modeOp": "pushMode", "modeArg": "string-body" }],
        "{":   [{ "kind": "InterpOpen",  "modeOp": "pushMode", "modeArg": "main" }]
      },
      "shapes": { "root": { "sequence": [ "InterpStart" ] } },
      "lexerModes": { "string-body": { } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    EXPECT_TRUE(loaded.has_value())
        << "cyclic mode references must load cleanly: "
        << (loaded.has_value() ? "<ok>" : loaded.error()[0].message);
}

// ── Backwards compat: shipped configs still load ────────────────────────

TEST(LexerModesLoader, ToyConfigStillLoadsAndHasMainMode) {
    auto loaded = GrammarSchema::loadShipped("toy");
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    auto const& s = **loaded;
    const auto mainId = s.findLexerMode("main");
    ASSERT_TRUE(mainId.valid());
    auto varInMain = s.lookupLexemeInMode(mainId, "var");
    EXPECT_FALSE(varInMain.empty());
}

TEST(LexerModesLoader, CSubsetConfigStillLoadsAndHasMainMode) {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    auto const& s = **loaded;
    EXPECT_TRUE(s.findLexerMode("main").valid());
}

// ── No mode metadata on a meaning means ModeOp::None ────────────────────

TEST(LexerModesLoader, TokenWithoutModeOpDefaultsToNone) {
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
    EXPECT_EQ(m[0].modeOp, ModeOp::None);
    EXPECT_FALSE(m[0].modeArg.valid());
}
