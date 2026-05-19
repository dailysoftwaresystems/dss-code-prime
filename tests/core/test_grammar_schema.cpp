#include "core/types/grammar_schema.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string_view>

using namespace dss;

namespace {

// A minimal-but-complete JSON config used by most positive tests.
constexpr std::string_view kHappyConfig = R"({
  "dssSchemaVersion": 1,
  "language": {
    "name": "MiniLang",
    "version": "1.0.0",
    "fileExtensions": [".ml", ".mini"]
  },
  "tokens": {
    " ":  [{ "kind": "Whitespace",   "flags": ["EmptySpace"] }],
    "+":  [
      { "kind": "SumOperator",          "priority": 10 },
      { "kind": "StringAppendOperator", "priority": 20 }
    ],
    "<":  [
      { "kind": "LtOperator",              "priority": 10 },
      { "kind": "GenericDefinitionOpener", "priority": 5, "opensScope": "Generic" }
    ],
    "{":  [{ "kind": "BlockOpen",  "opensScope": "Block" }],
    "}":  [{ "kind": "BlockClose", "closesScope": true }],
    ";":  [{ "kind": "EndCommand" }],
    "=":  [{ "kind": "AssignmentOperator" }]
  },
  "keywords": [
    { "word": "var", "kind": "VarKeyword" },
    { "word": "if",  "kind": "IfKeyword" }
  ],
  "scopes": {
    "validity": [
      { "scope": "Generic", "forbid": ["LtOperator"] }
    ]
  },
  "shapes": {
    "root":       { "sequence": [{ "repeat": "statement" }] },
    "statement":  { "alt":      ["varDecl", "exprStmt"] },
    "varDecl":    { "sequence": ["VarKeyword", "Identifier", "AssignmentOperator", "expression", "EndCommand"] },
    "exprStmt":   { "sequence": ["expression", "EndCommand"] },
    "expression": { "sequence": ["Identifier"] }
  }
})";

} // namespace

// ─── Happy-path load ─────────────────────────────────────────────────────

TEST(GrammarSchema, LoadsValidConfig) {
    auto result = GrammarSchema::loadFromText(kHappyConfig);
    ASSERT_TRUE(result.has_value()) << "loadFromText failed: "
        << (result.error().empty() ? "<no diagnostics>" : result.error()[0].message);
    auto schema = *result;
    ASSERT_NE(schema, nullptr);
    EXPECT_EQ(schema->name(), "MiniLang");
    EXPECT_EQ(schema->version(), "1.0.0");
    EXPECT_EQ(schema->schemaVersion(), 1u);

    auto exts = schema->fileExtensions();
    ASSERT_EQ(exts.size(), 2u);
    EXPECT_EQ(exts[0], ".ml");
    EXPECT_EQ(exts[1], ".mini");
}

TEST(GrammarSchema, LookupLexemeReturnsAllMeaningsInPriorityOrder) {
    auto schema = *GrammarSchema::loadFromText(kHappyConfig);
    auto meanings = schema->lookupLexeme("+");
    ASSERT_EQ(meanings.size(), 2u);
    // Priorities 10 < 20 → SumOperator wins on tiebreak; should be first.
    EXPECT_EQ(meanings[0].priority, 10);
    EXPECT_EQ(meanings[1].priority, 20);
    EXPECT_EQ(schema->schemaTokens().name(meanings[0].id), "SumOperator");
    EXPECT_EQ(schema->schemaTokens().name(meanings[1].id), "StringAppendOperator");
}

TEST(GrammarSchema, MultiTypedLexemeOpensScope) {
    auto schema = *GrammarSchema::loadFromText(kHappyConfig);
    auto meanings = schema->lookupLexeme("<");
    ASSERT_EQ(meanings.size(), 2u);
    // GenericDefinitionOpener has lower priority (5) so should sort first.
    EXPECT_EQ(schema->schemaTokens().name(meanings[0].id), "GenericDefinitionOpener");
    EXPECT_EQ(meanings[0].opensScope, ScopeKind::Generic);
    EXPECT_EQ(meanings[1].opensScope, ScopeKind::None);   // LtOperator
}

TEST(GrammarSchema, KeywordsLandInLexemeTable) {
    auto schema = *GrammarSchema::loadFromText(kHappyConfig);
    auto meanings = schema->lookupLexeme("var");
    ASSERT_EQ(meanings.size(), 1u);
    EXPECT_EQ(schema->schemaTokens().name(meanings[0].id), "VarKeyword");
}

TEST(GrammarSchema, EmptySpaceFlagDetection) {
    auto schema = *GrammarSchema::loadFromText(kHappyConfig);
    auto ws = schema->lookupLexeme(" ");
    ASSERT_EQ(ws.size(), 1u);
    EXPECT_TRUE(schema->isEmptySpace(ws[0].id));

    auto plus = schema->lookupLexeme("+");
    ASSERT_FALSE(plus.empty());
    EXPECT_FALSE(schema->isEmptySpace(plus[0].id));
}

TEST(GrammarSchema, ScopeForbidRejectsInsideScope) {
    auto schema = *GrammarSchema::loadFromText(kHappyConfig);
    auto lt = schema->lookupLexeme("<")[1].id;     // LtOperator
    // Outside Generic — valid.
    ScopeKind stackOutside[] = { ScopeKind::Root };
    EXPECT_TRUE(schema->isTokenValidInScope(lt, stackOutside));
    // Inside Generic — forbidden.
    ScopeKind stackInside[] = { ScopeKind::Root, ScopeKind::Generic };
    EXPECT_FALSE(schema->isTokenValidInScope(lt, stackInside));
}

TEST(GrammarSchema, RulesAndTokensInternersFrozenPostLoad) {
    auto schema = *GrammarSchema::loadFromText(kHappyConfig);
    EXPECT_TRUE(schema->rules().isFrozen());
    EXPECT_TRUE(schema->schemaTokens().isFrozen());
}

TEST(GrammarSchema, RootCursorIsValid) {
    auto schema = *GrammarSchema::loadFromText(kHappyConfig);
    EXPECT_TRUE(schema->rootCursor().valid());
}

TEST(GrammarSchema, ExpectedAtReturnsStableSpan) {
    auto schema = *GrammarSchema::loadFromText(kHappyConfig);
    auto root = schema->rootCursor();
    auto a = schema->expectedAt(root);
    auto b = schema->expectedAt(root);
    // Pointer identity proves the span comes from schema-owned storage
    // (no allocation per call).
    EXPECT_EQ(a.data(), b.data());
    EXPECT_EQ(a.size(), b.size());
}

TEST(GrammarSchema, BuiltinIdentifierIsKnownEvenWithoutDeclaration) {
    // Built-in CoreTokenKind names are pre-interned so shapes like the
    // "expression": ["Identifier"] reference resolves without the user
    // declaring an Identifier token entry.
    auto schema = *GrammarSchema::loadFromText(kHappyConfig);
    EXPECT_TRUE(schema->schemaTokens().contains("Identifier"));
}

// ─── Negative paths ──────────────────────────────────────────────────────

TEST(GrammarSchema, MalformedJsonReportsCode) {
    auto result = GrammarSchema::loadFromText("not valid json {{{ ");
    ASSERT_FALSE(result.has_value());
    auto const& diags = result.error();
    ASSERT_FALSE(diags.empty());
    EXPECT_EQ(diags[0].code, DiagnosticCode::C_MalformedJson);
}

TEST(GrammarSchema, MissingDssSchemaVersionReportsCode) {
    auto result = GrammarSchema::loadFromText(R"({ "language": {"name":"X","version":"0.1.0"} })");
    ASSERT_FALSE(result.has_value());
    auto const& diags = result.error();
    EXPECT_TRUE(std::ranges::any_of(diags, [](auto const& d) {
        return d.code == DiagnosticCode::C_MissingField;
    }));
}

TEST(GrammarSchema, UnsupportedSchemaVersionReportsCode) {
    auto result = GrammarSchema::loadFromText(
        R"({"dssSchemaVersion":99,"language":{"name":"X","version":"0.1.0"}})");
    ASSERT_FALSE(result.has_value());
    auto const& diags = result.error();
    EXPECT_TRUE(std::ranges::any_of(diags, [](auto const& d) {
        return d.code == DiagnosticCode::C_VersionMismatch;
    }));
}

TEST(GrammarSchema, UnknownShapeReferenceReportsCode) {
    constexpr std::string_view bad = R"({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "shapes": {
        "root": { "sequence": ["doesNotExist"] }
      }
    })";
    auto result = GrammarSchema::loadFromText(bad);
    ASSERT_FALSE(result.has_value());
    auto const& diags = result.error();
    EXPECT_TRUE(std::ranges::any_of(diags, [](auto const& d) {
        return d.code == DiagnosticCode::C_UnknownShape;
    }));
}

TEST(GrammarSchema, ForbidReferencingUnknownTokenReportsCode) {
    constexpr std::string_view bad = R"({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "scopes": { "validity": [ { "scope": "Generic", "forbid": ["NotDeclared"] } ] },
      "shapes": { "root": { "sequence": [] } }
    })";
    auto result = GrammarSchema::loadFromText(bad);
    ASSERT_FALSE(result.has_value());
    auto const& diags = result.error();
    EXPECT_TRUE(std::ranges::any_of(diags, [](auto const& d) {
        return d.code == DiagnosticCode::C_UnknownToken;
    }));
}

TEST(GrammarSchema, UnknownScopeNameReportsCode) {
    constexpr std::string_view bad = R"({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "<": [{ "kind": "K", "opensScope": "Fictional" }] },
      "shapes": { "root": { "sequence": [] } }
    })";
    auto result = GrammarSchema::loadFromText(bad);
    ASSERT_FALSE(result.has_value());
    auto const& diags = result.error();
    EXPECT_TRUE(std::ranges::any_of(diags, [](auto const& d) {
        return d.code == DiagnosticCode::C_UnclosableScope;
    }));
}

TEST(GrammarSchema, MissingRootShapeReportsCode) {
    constexpr std::string_view bad = R"({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "shapes": { "alpha": { "sequence": [] } }
    })";
    auto result = GrammarSchema::loadFromText(bad);
    ASSERT_FALSE(result.has_value());
    auto const& diags = result.error();
    EXPECT_TRUE(std::ranges::any_of(diags, [](auto const& d) {
        return d.code == DiagnosticCode::C_MissingField;
    }));
}

// ─── loadShipped + the on-disk toy.lang.json ─────────────────────────────

TEST(GrammarSchema, LoadShippedToy) {
    // The ctest cwd is the build dir. loadShipped checks two roots:
    //   <cwd>/src/source-config/languages/toy.lang.json
    //   <cwd>/../src/source-config/languages/toy.lang.json
    // From build/, the second hits.
    auto result = GrammarSchema::loadShipped("toy");
    if (!result.has_value()) {
        // Skip gracefully if the toy config can't be located from this
        // invocation context — tests run in too many cwd configurations
        // to make this hard-fail. Diagnostic dumped below for triage.
        FAIL() << "loadShipped failed: " << result.error()[0].message
               << " (cwd=" << std::filesystem::current_path().string() << ")";
    } else {
        EXPECT_EQ((*result)->name(), "Toy");
        EXPECT_TRUE((*result)->isEmptySpace(
            (*result)->lookupLexeme(" ")[0].id));
    }
}

// Pins the cookbook example in docs/language-config-spec.md §7 against the
// loader. If this fails, the doc's "Loads cleanly because:" claims are wrong.
TEST(GrammarSchema, DocsCookbookCalcExampleLoadsCleanly) {
    constexpr std::string_view kCalcCookbook = R"JSON({
  "dssSchemaVersion": 1,

  "language": {
    "name":           "Calc",
    "version":        "0.1.0",
    "fileExtensions": [".calc"]
  },

  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\t": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\n": [{ "kind": "Newline",    "flags": ["EmptySpace"] }],

    "+":  [{ "kind": "PlusOp" }],
    "-":  [{ "kind": "MinusOp" }],
    "=":  [{ "kind": "EqOp" }],
    ";":  [{ "kind": "End" }],

    "(":  [{ "kind": "ParenOpen",  "opensScope": "Paren" }],
    ")":  [{ "kind": "ParenClose", "closesScope": true   }]
  },

  "keywords": [
    { "word": "let", "kind": "LetKeyword" }
  ],

  "shapes": {
    "root":     { "sequence": [{ "repeat": "stmt" }] },
    "stmt":     { "alt":      ["letDecl", "exprStmt"] },
    "letDecl":  { "sequence": ["LetKeyword", "Identifier", "EqOp", "IntLiteral", "End"] },
    "exprStmt": { "sequence": ["IntLiteral", "End"] }
  }
})JSON";

    auto result = GrammarSchema::loadFromText(kCalcCookbook);
    ASSERT_TRUE(result.has_value())
        << "docs cookbook failed to load: "
        << (result.error().empty() ? "<no diagnostics>" : result.error()[0].message);
    EXPECT_EQ((*result)->name(), "Calc");
}

TEST(GrammarSchema, LoadShippedRejectsPathLikeNames) {
    auto a = GrammarSchema::loadShipped("../etc/passwd");
    auto b = GrammarSchema::loadShipped("/abs/path");
    auto c = GrammarSchema::loadShipped(".hidden");
    auto d = GrammarSchema::loadShipped("");
    EXPECT_FALSE(a.has_value());
    EXPECT_FALSE(b.has_value());
    EXPECT_FALSE(c.has_value());
    EXPECT_FALSE(d.has_value());
}
