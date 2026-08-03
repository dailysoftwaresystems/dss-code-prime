#include "core/types/grammar_schema.hpp"
#include "core/types/lexer_mode.hpp"
#include "core/types/literal_close_token.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace dss;

// ── LexerModeStack stub-driver ──────────────────────────────────────────

TEST(LexerModeStack, EmptyAtConstruction) {
    LexerModeStack s;
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.depth(), 0u);
    EXPECT_FALSE(s.topOrInvalid().valid());
}

TEST(LexerModeStackDeath, TopOnEmptyAborts) {
    LexerModeStack s;
    EXPECT_DEATH({ (void)s.top(); }, "top\\(\\) on empty stack");
}

TEST(LexerModeStackDeath, PopOnEmptyAborts) {
    LexerModeStack s;
    EXPECT_DEATH({ s.pop(); }, "pop\\(\\) on empty stack");
}

TEST(LexerModeStackDeath, ReplaceTopOnEmptyAborts) {
    LexerModeStack s;
    EXPECT_DEATH({ s.replaceTop(LexerModeId{1}); }, "replaceTop\\(\\) on empty stack");
}

TEST(LexerModeStack, TryPopOnEmptyReturnsFalse) {
    LexerModeStack s;
    EXPECT_FALSE(s.tryPop());
    s.push(LexerModeId{1});
    EXPECT_TRUE(s.tryPop());
    EXPECT_TRUE(s.empty());
}

TEST(LexerModeStack, ClearDropsAllFrames) {
    LexerModeStack s;
    s.push(LexerModeId{1});
    s.push(LexerModeId{2});
    s.push(LexerModeId{3});
    s.clear();
    EXPECT_TRUE(s.empty());
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

TEST(LexerModeStack, ReplaceTopSwapsWithoutNesting) {
    LexerModeStack s;
    s.push(LexerModeId{1});
    s.push(LexerModeId{2});
    s.replaceTop(LexerModeId{3});
    EXPECT_EQ(s.depth(), 2u);
    EXPECT_EQ(s.top().v, 3u);
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
    EXPECT_FALSE(s.empty());                          // back to 1 frame

    s.restore(snap);
    EXPECT_EQ(s.depth(), 1u);
    EXPECT_EQ(s.top().v, 1u);
}

TEST(LexerModeStack, SnapshotOfEmptyStackRoundTrip) {
    LexerModeStack s;
    auto snap = s.snapshot();
    s.push(LexerModeId{1});
    s.push(LexerModeId{2});
    s.restore(snap);
    EXPECT_TRUE(s.empty());
}

TEST(LexerModeStackDeath, CrossStackRestoreAborts) {
    LexerModeStack a;
    LexerModeStack b;
    a.push(LexerModeId{1});
    auto snapA = a.snapshot();
    EXPECT_DEATH({ b.restore(snapA); },
                 "restore\\(\\) with a snapshot from a different stack");
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
    EXPECT_EQ(s.schemaTokens().name(mode.defaultToken->kind), "StringChar");
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

// ── FF11 general `popAtNewline` capability — loader ──────────────────────

TEST(LexerModesLoader, PopAtNewlineLoadsAndSetsFlag) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "#": [{ "kind": "Hash", "modeOp": "pushMode", "modeArg": "line" }],
        "<": [{ "kind": "LtOp" }]
      },
      "shapes": { "root": { "sequence": [ "LtOp" ] } },
      "lexerModes": {
        "main": { "tokens": "default" },
        "line": { "tokens": { "<": [{ "kind": "Ang" }] }, "popAtNewline": true }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value())
        << "popAtNewline (no defaultToken) must load cleanly";
    auto const id = (*loaded)->findLexerMode("line");
    ASSERT_TRUE(id.valid());
    EXPECT_TRUE((*loaded)->lexerMode(id).popAtNewline)
        << "the loaded 'line' mode must carry popAtNewline=true";
    // The default 'main' mode is NOT line-scoped.
    auto const mainId = (*loaded)->findLexerMode("main");
    ASSERT_TRUE(mainId.valid());
    EXPECT_FALSE((*loaded)->lexerMode(mainId).popAtNewline);
}

TEST(LexerModesLoader, PopAtNewlineWithDefaultTokenIsLoadError) {
    // Mutually exclusive: a body mode (defaultToken) already closes at
    // its endsAt; a second auto-close at newline would fight it (and
    // silently truncate a multiline body). Reject loudly.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "\"": [{ "kind": "Str", "modeOp": "pushMode", "modeArg": "body",
                 "stringStyle": { "escapeKind": "none", "endsAt": "\"" } }]
      },
      "shapes": { "root": { "sequence": [ "Str" ] } },
      "lexerModes": {
        "main": { "tokens": "default" },
        "body": { "defaultToken": { "kind": "Ch" }, "popAtNewline": true }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_ConflictingField &&
               d.message.find("mutually exclusive") != std::string::npos;
    })) << "popAtNewline + defaultToken must fail loud as C_ConflictingField";
}

TEST(LexerModesLoader, PopAtNewlineNonBoolIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "#": [{ "kind": "Hash", "modeOp": "pushMode", "modeArg": "line" }]
      },
      "shapes": { "root": { "sequence": [ "Hash" ] } },
      "lexerModes": {
        "main": { "tokens": "default" },
        "line": { "tokens": "default", "popAtNewline": "yes" }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_ConflictingField &&
               d.message.find("popAtNewline") != std::string::npos;
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

// ── replaceMode loader parsing ──────────────────────────────────────────

TEST(LexerModesLoader, ReplaceModeWithModeArgParses) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "@": [{ "kind": "AtOp", "modeOp": "replaceMode", "modeArg": "string-body" }]
      },
      "shapes": { "root": { "sequence": [ "AtOp" ] } },
      "lexerModes": { "string-body": { } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    auto const& m = (*loaded)->lookupLexeme("@");
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(m[0].modeOp, ModeOp::ReplaceMode);
    EXPECT_TRUE(m[0].modeArg.valid());
}

TEST(LexerModesLoader, ReplaceModeWithoutModeArgIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "@": [{ "kind": "AtOp", "modeOp": "replaceMode" }]
      },
      "shapes": { "root": { "sequence": [ "AtOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_ConflictingField &&
               d.message.find("'modeOp: replaceMode' requires 'modeArg'") != std::string::npos;
    }));
}

TEST(LexerModesLoader, NonStringModeArgIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "@": [{ "kind": "AtOp", "modeOp": "pushMode", "modeArg": 42 }]
      },
      "shapes": { "root": { "sequence": [ "AtOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_ConflictingField &&
               d.path.find("modeArg") != std::string::npos;
    }));
}

// ── Malformed defaultToken diagnostics ──────────────────────────────────

TEST(LexerModesLoader, DefaultTokenWrongTypeIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } },
      "lexerModes": { "m": { "defaultToken": "StringChar" } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_MissingField &&
               d.path.find("defaultToken") != std::string::npos;
    }));
}

TEST(LexerModesLoader, DefaultTokenMissingKindIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } },
      "lexerModes": { "m": { "defaultToken": { } } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_MissingField &&
               d.path.find("defaultToken") != std::string::npos;
    }));
}

TEST(LexerModesLoader, TokensWrongTypeIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } },
      "lexerModes": { "m": { "tokens": 42 } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_ConflictingField &&
               d.path.find("tokens") != std::string::npos;
    }));
}

// ── Inline tokens override parses into the per-mode table + `tokens:
//    "default"` inheritance ──────────────────────────────────────────────

TEST(LexerModesLoader, InlineTokensObjectParsesIntoPerModeTable) {
    // An inline per-mode `tokens` object is now parsed into the mode's
    // override table (the deferral warning it used to emit is retired).
    // The override lexeme resolves via lookupLexemeInMode for that mode
    // and does NOT leak into the global lexemeTable.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } },
      "lexerModes": {
        "m": { "tokens": { "@": [{ "kind": "AtOp" }] } }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value())
        << "inline per-mode tokens override must load cleanly";
    auto const& s = **loaded;
    const auto mId = s.findLexerMode("m");
    ASSERT_TRUE(mId.valid());
    auto inMode = s.lookupLexemeInMode(mId, "@");
    ASSERT_FALSE(inMode.empty())
        << "per-mode '@' override must be parsed into the mode table";
    // Global table is untouched by the per-mode override.
    EXPECT_TRUE(s.lookupLexeme("@").empty())
        << "per-mode override must not leak into the global lexemeTable";
}

TEST(LexerModesLoader, InlineTokensNonArrayValueIsLoadError) {
    // A per-mode tokens value that is not an array of meaning objects is
    // a real config error — same fail-loud discipline as the top-level
    // table. Mirrors the malformed-config contract for the new path.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } },
      "lexerModes": {
        "m": { "tokens": { "@": { "kind": "AtOp" } } }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_UnknownToken &&
               d.path.find("/lexerModes/m/tokens/@") != std::string::npos;
    }));
}

TEST(LexerModesLoader, InlineTokensMeaningMissingKindIsLoadError) {
    // A per-mode meaning entry missing its required `kind` flows through
    // the SAME shared meaning parser the global table uses, so it fails
    // loud identically.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } },
      "lexerModes": {
        "m": { "tokens": { "@": [{ "priority": 1 }] } }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_MissingField &&
               d.path.find("/lexerModes/m/tokens/@/0") != std::string::npos;
    }));
}

TEST(LexerModesLoader, NonMainModeTokensDefaultInheritsFromTopLevel) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "$$": [{ "kind": "DoubleDollar" }] },
      "shapes": { "root": { "sequence": [ "DoubleDollar" ] } },
      "lexerModes": { "alt-mode": { "tokens": "default" } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    auto const& s = **loaded;
    const auto altId = s.findLexerMode("alt-mode");
    ASSERT_TRUE(altId.valid());
    auto entries = s.lookupLexemeInMode(altId, "$$");
    EXPECT_FALSE(entries.empty())
        << "tokens: \"default\" on a non-main mode must inherit top-level table";
}

// ── lexerModes() span hides the sentinel ────────────────────────────────

TEST(LexerModesLoader, LexerModesSpanHidesSentinel) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } },
      "lexerModes": { "alpha": { }, "beta": { } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    auto modes = (*loaded)->lexerModes();
    // Three real modes: main (synthesized) + alpha + beta.
    ASSERT_EQ(modes.size(), 3u);
    for (auto const& m : modes) {
        EXPECT_FALSE(m.name.empty()) << "no sentinel entry should be exposed";
        EXPECT_TRUE(m.id.valid());
    }
}

// ── Keywords reject modeOp/modeArg (C1) ─────────────────────────────────

TEST(LexerModesLoader, KeywordWithModeOpIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "keywords": [
        { "word": "if", "kind": "IfKw", "modeOp": "pushMode", "modeArg": "main" }
      ],
      "shapes": { "root": { "sequence": [ "IfKw" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_ConflictingField &&
               d.message.find("keywords cannot switch lexer modes") != std::string::npos;
    }));
}

// ── Case-fold near-miss warning ─────────────────────────────────────────

TEST(LexerModesLoader, CaseFoldedDuplicateModeWarns) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": {
        "root":      { "alt": ["A", "B"] },
        "A":         { "sequence": [ "PlusOp" ] },
        "B":         { "sequence": [ "PlusOp" ] }
      },
      "lexerModes": { "string-body": { }, "String-Body": { } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_ConflictingField &&
               d.message.find("differs only by case") != std::string::npos;
    }));
}

// ── lookupLexemeInMode aborts on invalid id (C4) ────────────────────────

TEST(LookupLexemeInModeDeath, InvalidLexerModeIdAborts) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_DEATH({ (void)(*loaded)->lookupLexemeInMode(InvalidLexerMode, "+"); },
                 "invalid LexerModeId");
}

// ── H1: mode with only defaultToken (no tokens) warns ──────────────────

TEST(LexerModesLoader, DefaultTokenWithoutTokensFieldWarns) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": {
        "root":      { "alt": ["A", "B"] },
        "A":         { "sequence": [ "PlusOp" ] },
        "B":         { "sequence": [ "PlusOp" ] }
      },
      "lexerModes": {
        "string-body": { "defaultToken": { "kind": "StringChar" } }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_RedundantField &&
               d.message.find("only 'defaultToken' will ever match") != std::string::npos;
    }));
}

// ── T1-T4: stack API gaps from round-2 review ───────────────────────────

TEST(LexerModeStack, TopOrInvalidReturnsActualTopWhenNonEmpty) {
    LexerModeStack s;
    s.push(LexerModeId{7});
    s.push(LexerModeId{9});
    EXPECT_EQ(s.topOrInvalid().v, 9u);
    s.pop();
    EXPECT_EQ(s.topOrInvalid().v, 7u);
}

TEST(LexerModeStack, ClearLeavesStackReusable) {
    LexerModeStack s;
    s.push(LexerModeId{1});
    s.push(LexerModeId{2});
    s.clear();
    EXPECT_TRUE(s.empty());
    // Reuse after clear: push, snapshot, mutate, restore.
    s.push(LexerModeId{5});
    auto snap = s.snapshot();
    s.push(LexerModeId{6});
    s.push(LexerModeId{7});
    s.restore(snap);
    ASSERT_EQ(s.depth(), 1u);
    EXPECT_EQ(s.top().v, 5u);
}

TEST(LexerModeStackDeath, ApplyReplaceModeOnEmptyAborts) {
    LexerModeStack s;
    EXPECT_DEATH({ s.apply(ModeOp::ReplaceMode, LexerModeId{1}); },
                 "replaceTop\\(\\) on empty stack");
}

TEST(LexerModeStack, FramesAccessorReturnsBottomToTop) {
    LexerModeStack s;
    s.push(LexerModeId{1});
    s.push(LexerModeId{2});
    s.push(LexerModeId{3});
    auto fr = s.frames();
    ASSERT_EQ(fr.size(), 3u);
    EXPECT_EQ(fr[0].v, 1u);
    EXPECT_EQ(fr[1].v, 2u);
    EXPECT_EQ(fr[2].v, 3u);
}

// ── Generation-counter behavior: two distinct stacks get distinct ids ──

TEST(LexerModeStackDeath, SecondInstanceAtSameAddressCannotImpersonate) {
    // Hard to test address recycling directly without UB. Instead test
    // the equivalent: snapshot from one stack does not restore into a
    // separately-constructed second stack, even after the first is
    // destroyed and the second starts empty. This pins the per-instance
    // id stamp regardless of address reuse.
    LexerModeStack::Snapshot snap;
    {
        LexerModeStack a;
        a.push(LexerModeId{1});
        snap = a.snapshot();
    }
    LexerModeStack b;
    EXPECT_DEATH({ b.restore(snap); },
                 "restore\\(\\) with a snapshot from a different stack");
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

// ── D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN: `defaultToken.closeToken` loader
//    rules ─────────────────────────────────────────────────────────────────
//
// Five fail-louds in grammar_schema_json.cpp guard the coalesced-body closer.
// Each gets a NEGATIVE pin below asserting its SPECIFIC diagnostic code, plus
// one POSITIVE pin that the five shipped coalesced modes still load clean — a
// check that rejected the shipped grammar would be worse than no check at all.
//
// Precedent: PopAtNewlineWithDefaultTokenIsLoadError above — inline JSON
// through `loadFromText`, assert on `loaded.error()` by code + message
// fragment. Every config below differs from a CLEAN one in exactly the one
// field its rule is about, so a firing diagnostic can only be that rule's.

// (1) :2273 — CLOSED KEY VOCABULARY for `defaultToken`. A mis-spelled knob
// must not load clean and do nothing. `closeTokn` is the exact typo the rule
// exists for: without this check the config below would load as a
// coalesce-without-closeToken and report a MISSING field the author
// demonstrably DID write.
TEST(LexerModesLoader, DefaultTokenUnknownKeyIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "\"": [{ "kind": "Str", "modeOp": "pushMode", "modeArg": "body",
                 "stringStyle": { "escapeKind": "none", "endsAt": "\"" } }]
      },
      "shapes": { "root": { "sequence": [ "Str" ] } },
      "lexerModes": {
        "main": { "tokens": "default" },
        "body": { "defaultToken": { "kind": "Ch", "coalesce": true,
                                    "closeToken": "End", "closeTokn": "End" } }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value())
        << "a mis-spelled defaultToken key must not load clean";
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_ConflictingField &&
               d.message.find("unknown key 'closeTokn'") != std::string::npos;
    })) << "an unknown 'defaultToken' key must fail loud as C_ConflictingField "
           "naming the offending key";
}

// The `$`-doc-key exemption is part of the same rule: `$comment` /
// `$…Comment` is the codebase-wide documentation convention and never a knob,
// so the closed vocabulary must let it through. Without this pin, tightening
// the check to "no unlisted keys at all" would reject every documented mode in
// the shipped configs.
TEST(LexerModesLoader, DefaultTokenDollarDocKeyIsExemptFromClosedVocabulary) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "\"": [{ "kind": "Str", "modeOp": "pushMode", "modeArg": "body",
                 "stringStyle": { "escapeKind": "none", "endsAt": "\"" } }]
      },
      "shapes": { "root": { "sequence": [ "Str" ] } },
      "lexerModes": {
        "main": { "tokens": "default" },
        "body": { "defaultToken": { "$comment": "documented, not a knob",
                                    "kind": "Ch", "coalesce": true,
                                    "closeToken": "End" } }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
}

// (2) :2352 — `coalesce: true` REQUIRES `closeToken`. There is no defensible
// default: falling back to `kind` re-creates the silent decode corruption rule
// (4) rejects, and emitting nothing restores the very defect this anchor
// closed (the closer's bytes belonging to no token's span).
TEST(LexerModesLoader, CoalesceWithoutCloseTokenIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "\"": [{ "kind": "Str", "modeOp": "pushMode", "modeArg": "body",
                 "stringStyle": { "escapeKind": "none", "endsAt": "\"" } }]
      },
      "shapes": { "root": { "sequence": [ "Str" ] } },
      "lexerModes": {
        "main": { "tokens": "default" },
        "body": { "defaultToken": { "kind": "Ch", "coalesce": true } }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value())
        << "a coalesced body mode with no closeToken must not load";
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_MissingField &&
               d.message.find("'defaultToken.coalesce' is true but no") !=
                   std::string::npos;
    })) << "coalesce without closeToken must fail loud as C_MissingField";
}

// (3) :2366 — the converse knob-that-lies. ONLY the coalesced tokenizer path
// reads `closeToken`; a per-codepoint mode emits its closer through the
// ordinary body path, so a `closeToken` there is config the engine silently
// never consults.
TEST(LexerModesLoader, CloseTokenWithoutCoalesceIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "\"": [{ "kind": "Str", "modeOp": "pushMode", "modeArg": "body",
                 "stringStyle": { "escapeKind": "none", "endsAt": "\"" } }]
      },
      "shapes": { "root": { "sequence": [ "Str" ] } },
      "lexerModes": {
        "main": { "tokens": "default" },
        "body": { "defaultToken": { "kind": "Ch", "closeToken": "End" } }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value())
        << "closeToken on a per-codepoint mode must not load clean";
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_ConflictingField &&
               d.message.find("requires 'defaultToken.coalesce': true") !=
                   std::string::npos;
    })) << "closeToken without coalesce must fail loud as C_ConflictingField";
}

// (4) :2382 — the closer must NOT reuse the body's kind. `decodeAdjacent-
// StringBodies` selects segments by FILTERING children on the body kind, so a
// same-kind closer decodes INTO the value (`"abc"` → `abc"`) with the semantic
// and HIR tiers agreeing on the same wrong length — no cross-tier guard can
// fire. Load time is the only place the mistake is still cheap.
TEST(LexerModesLoader, CloseTokenEqualToBodyKindIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "\"": [{ "kind": "Str", "modeOp": "pushMode", "modeArg": "body",
                 "stringStyle": { "escapeKind": "none", "endsAt": "\"" } }]
      },
      "shapes": { "root": { "sequence": [ "Str" ] } },
      "lexerModes": {
        "main": { "tokens": "default" },
        "body": { "defaultToken": { "kind": "Ch", "coalesce": true,
                                    "closeToken": "Ch" } }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value())
        << "a closer reusing the body kind must not load";
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_ConflictingField &&
               d.message.find("must differ from 'defaultToken.kind'") !=
                   std::string::npos;
    })) << "closeToken == kind must fail loud as C_ConflictingField";
}

// (5) :2539 — CROSS-MODE. Two coalesced modes MAY share a body kind (tsql's
// `single-string` / `unicode-string` both emit `StringLiteral`; only the
// OPENER tells `'a'` from `N'a'`), but they MUST then agree on the closer.
//
// ★ This is the invariant `closeTokenForCoalescedBody`
// (core/types/literal_close_token.hpp) DEPENDS on: its lookup is a FIRST-MATCH
// scan keyed on the body kind. Two answers for one body kind and that scan
// hands every consumer the FIRST-DECLARED mode's closer — for literals lexed
// by the second mode, silently the wrong kind, with mode-declaration ORDER as
// the deciding factor and no diagnostic anywhere. Enforced at load time or the
// resolver is a silent-wrong-answer waiting for its second consumer.
TEST(LexerModesLoader, CoalescedModesSharingBodyKindMustAgreeOnCloseToken) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "\"": [{ "kind": "StrA", "modeOp": "pushMode", "modeArg": "bodyA",
                 "stringStyle": { "escapeKind": "none", "endsAt": "\"" } }],
        "N\"": [{ "kind": "StrB", "modeOp": "pushMode", "modeArg": "bodyB",
                  "stringStyle": { "escapeKind": "none", "endsAt": "\"" } }]
      },
      "shapes": { "root": { "sequence": [ "StrA" ] } },
      "lexerModes": {
        "main":  { "tokens": "default" },
        "bodyA": { "defaultToken": { "kind": "Body", "coalesce": true,
                                     "closeToken": "EndA" } },
        "bodyB": { "defaultToken": { "kind": "Body", "coalesce": true,
                                     "closeToken": "EndB" } }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value())
        << "two coalesced modes sharing a body kind with DIFFERENT closers "
           "must not load — the consumer-side resolver would silently answer "
           "by declaration order";
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_ConflictingField &&
               d.message.find("share the body kind") != std::string::npos;
    })) << "a cross-mode closer disagreement must fail loud as "
           "C_ConflictingField";
}

// The SAME-closer case is the one the rule deliberately ALLOWS — tsql ships
// exactly this shape. Pinned so a future tightening to "body kinds must be
// unique per mode" cannot land unnoticed: it would reject the shipped grammar.
TEST(LexerModesLoader, CoalescedModesMaySharedBodyKindWithTheSameCloseToken) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "\"": [{ "kind": "StrA", "modeOp": "pushMode", "modeArg": "bodyA",
                 "stringStyle": { "escapeKind": "none", "endsAt": "\"" } }],
        "N\"": [{ "kind": "StrB", "modeOp": "pushMode", "modeArg": "bodyB",
                  "stringStyle": { "escapeKind": "none", "endsAt": "\"" } }]
      },
      "shapes": { "root": { "sequence": [ "StrA" ] } },
      "lexerModes": {
        "main":  { "tokens": "default" },
        "bodyA": { "defaultToken": { "kind": "Body", "coalesce": true,
                                     "closeToken": "End" } },
        "bodyB": { "defaultToken": { "kind": "Body", "coalesce": true,
                                     "closeToken": "End" } }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    // …and the resolver's first-match scan is therefore order-independent: it
    // returns THE closer, not "whichever mode happened to be declared first".
    // Ids come off the loaded modes rather than a fresh intern, so the pin
    // reads the same table the resolver walks.
    GrammarSchema const& s   = **loaded;
    LexerModeId const    aId = s.findLexerMode("bodyA");
    LexerModeId const    bId = s.findLexerMode("bodyB");
    ASSERT_TRUE(aId.valid());
    ASSERT_TRUE(bId.valid());
    auto const& a = s.lexerMode(aId).defaultToken;
    auto const& b = s.lexerMode(bId).defaultToken;
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(a->kind, b->kind);
    EXPECT_EQ(a->closeToken, b->closeToken);
    EXPECT_EQ(closeTokenForCoalescedBody(s, a->kind), a->closeToken);
    EXPECT_EQ(closeTokenForCoalescedBody(s, b->kind), b->closeToken);
}

// (6) POSITIVE — the five shipped coalesced modes load clean and satisfy every
// rule above. A validation that rejects the grammar it ships with would be
// worse than no validation, and none of the five negative pins proves the
// checks are SATISFIABLE. Also re-derives the cross-mode invariant over the
// REAL configs, so the resolver's first-match scan is pinned against the data
// it actually runs on rather than a hand-built fixture.
TEST(LexerModesLoader, ShippedCoalescedModesDeclareValidDistinctCloseTokens) {
    struct Expect { std::string_view mode, body, closer; };
    struct Cfg { std::string_view name; std::vector<Expect> modes; };
    std::vector<Cfg> const shipped{
        {"c-subset", {{"string",         "StringLiteral", "StringEnd"},
                      {"charBody",       "CharLiteral",   "CharEnd"},
                      {"header-body",    "HeaderPath",    "HeaderEnd"}}},
        {"tsql-subset", {{"single-string",  "StringLiteral", "StringEnd"},
                         {"unicode-string", "StringLiteral", "StringEnd"}}},
    };
    std::size_t totalCoalesced = 0;
    for (Cfg const& cfg : shipped) {
        auto loaded = GrammarSchema::loadShipped(cfg.name);
        ASSERT_TRUE(loaded.has_value())
            << cfg.name << " must load clean: "
            << (loaded.error().empty() ? "<no diagnostics>"
                                       : loaded.error()[0].message);
        GrammarSchema const& s = **loaded;
        // body kind -> closer, rebuilt from the loaded modes. Agreement here
        // is what makes closeTokenForCoalescedBody's first-match scan sound.
        std::unordered_map<std::uint32_t, SchemaTokenId> closerOf;
        std::size_t coalesced = 0;
        for (LexerMode const& m : s.lexerModes()) {
            if (!m.defaultToken || !m.defaultToken->coalesce) continue;
            ++coalesced;
            EXPECT_TRUE(m.defaultToken->closeToken.valid())
                << cfg.name << '/' << m.name << ": a coalesced mode must "
                << "declare a closeToken";
            EXPECT_NE(m.defaultToken->closeToken, m.defaultToken->kind)
                << cfg.name << '/' << m.name << ": the closer must not reuse "
                << "the body kind";
            auto const [it, inserted] = closerOf.try_emplace(
                m.defaultToken->kind.v, m.defaultToken->closeToken);
            EXPECT_EQ(it->second, m.defaultToken->closeToken)
                << cfg.name << '/' << m.name << ": modes sharing a body kind "
                << "must agree on the closer";
            (void)inserted;
        }
        totalCoalesced += coalesced;
        EXPECT_EQ(coalesced, cfg.modes.size())
            << cfg.name << ": coalesced-mode count drifted — update this pin "
            << "AND check the new mode declares a closer";
        for (Expect const& e : cfg.modes) {
            LexerModeId const id = s.findLexerMode(e.mode);
            ASSERT_TRUE(id.valid()) << cfg.name << '/' << e.mode;
            LexerMode const& m = s.lexerMode(id);
            ASSERT_TRUE(m.defaultToken.has_value()) << cfg.name << '/' << e.mode;
            EXPECT_TRUE(m.defaultToken->coalesce) << cfg.name << '/' << e.mode;
            EXPECT_EQ(s.schemaTokens().name(m.defaultToken->kind), e.body);
            EXPECT_EQ(s.schemaTokens().name(m.defaultToken->closeToken), e.closer);
            // The consumer-side resolver agrees with the mode table — this is
            // the path every real consumer (cst_to_hir, the preprocessor's
            // include arms) actually takes.
            EXPECT_EQ(s.schemaTokens().name(
                          closeTokenForCoalescedBody(s, m.defaultToken->kind)),
                      e.closer);
        }
    }
    EXPECT_EQ(totalCoalesced, 5u)
        << "the shipped grammars declare five coalesced body modes";
}
