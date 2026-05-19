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

// ── Inline tokens deferral warning + `tokens: "default"` inheritance ────

TEST(LexerModesLoader, InlineTokensObjectEmitsDeferralWarning) {
    // The warning doesn't fail the load on its own; pair with an
    // ambiguity error so the diagnostic vector reaches us.
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
        "m": { "tokens": { "@": [{ "kind": "AtOp" }] } }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_RedundantField &&
               d.message.find("not yet parsed") != std::string::npos;
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
