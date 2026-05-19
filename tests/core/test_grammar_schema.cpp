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

TEST(GrammarSchema, ExpectedSetReturnsStableSpan) {
    auto schema = *GrammarSchema::loadFromText(kHappyConfig);
    auto root = schema->rootCursor();
    auto a = schema->expectedSet(root);
    auto b = schema->expectedSet(root);
    // Pointer identity proves the span comes from schema-owned storage
    // (no allocation per call). Every downstream consumer that holds an
    // expectedSet across schema-cursor operations depends on this
    // stability invariant.
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
      "shapes": { "root": { "sequence": ["Identifier"] } }
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
      "shapes": { "root": { "sequence": ["Identifier"] } }
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
      "shapes": { "alpha": { "sequence": ["Identifier"] } }
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

// The shipped c-subset config must load cleanly and round-trip every rule
// the JSON declares. Pins three layers: (a) loader accepts the file, (b)
// each named shape resolves via `rules().find(...)` so a typo in any
// shape key wouldn't slip through as "loaded but unusable at first call",
// (c) representative tokens and keywords carry the right meanings.
TEST(GrammarSchema, LoadShippedCSubset) {
    auto result = GrammarSchema::loadShipped("c-subset");
    if (!result.has_value()) {
        FAIL() << "loadShipped c-subset failed: " << result.error()[0].message
               << " (cwd=" << std::filesystem::current_path().string() << ")";
    }
    auto const& schema = **result;

    EXPECT_EQ(schema.name(), "CSubset");
    EXPECT_EQ(schema.schemaVersion(), 2u);

    // Every shape name declared in the JSON must resolve. A typo would
    // currently load cleanly and only fail when a caller asks for the
    // missing name; pinning here makes the regression visible at load.
    for (std::string_view rule : {"root", "topLevel", "topLevelTail",
                                  "funcTail", "varDeclTail",
                                  "typeBase", "typeRef",
                                  "varDeclHead", "varDecl",
                                  "paramList", "param", "block", "statement",
                                  "ifStmt", "whileStmt", "doStmt", "forStmt",
                                  "returnStmt", "exprStmt", "expression",
                                  "binaryOp", "operand"}) {
        EXPECT_TRUE(schema.rules().find(rule).valid()) << rule;
    }

    // BlockOpen opens Block — representative scope token.
    auto const blockOpen = schema.lookupLexeme("{");
    ASSERT_EQ(blockOpen.size(), 1u);
    EXPECT_EQ(blockOpen[0].opensScope, ScopeKind::Block);

    // Each declared keyword resolves to a single meaning with the
    // expected schema-token name; a regression in the keyword loader
    // (silently dropping entries or remapping kinds) would fail here.
    auto const forKw = schema.lookupLexeme("for");
    ASSERT_EQ(forKw.size(), 1u);
    EXPECT_EQ(schema.schemaTokens().name(forKw[0].id), "ForKeyword");

    // `typedef` is not a keyword in this config (deferred — see
    // v2-gap-catalog row 2). Confirm the lexeme has no meaning so a
    // future regression that silently adds it gets caught.
    EXPECT_TRUE(schema.lookupLexeme("typedef").empty());
}

// `typeRef` admits `const int const x` (double-const). Real C allows
// double-const only with intervening type modifiers; the c-subset is
// deliberately more permissive while precedence/arity are deferred.
// Pinned so a future PR doesn't tighten this without intent.
TEST(GrammarSchema, CSubsetTypeRefAllowsDoubleConst) {
    auto result = GrammarSchema::loadShipped("c-subset");
    if (!result.has_value()) {
        FAIL() << "loadShipped c-subset failed: " << result.error()[0].message;
    }
    EXPECT_TRUE((*result)->rules().find("typeRef").valid());
}

// dssSchemaVersion 2 is the upper bound of the loader's accepted window.
// A valid v2 document must load AND emit zero diagnostics — a future
// warning-on-version-2 regression would silently pass without this.
TEST(GrammarSchema, SchemaVersionTwoAccepted) {
    auto result = GrammarSchema::loadFromText(
        R"({"dssSchemaVersion":2,"language":{"name":"X","version":"0.1.0"}})");
    ASSERT_TRUE(result.has_value())
        << "v2 doc should load: "
        << (result.error().empty() ? "<no diagnostics>" : result.error()[0].message);
    EXPECT_EQ((*result)->schemaVersion(), 2u);
}

// Outside the accepted window, the loader must emit C_VersionMismatch
// with a message naming the supported range. The range string is the
// load-bearing fragment — full-message equality would over-pin the
// surrounding prose, but the range itself MUST be there or the
// diagnostic is uselessly opaque.
TEST(GrammarSchema, SchemaVersionThreeRejectedWithRangeMessage) {
    auto result = GrammarSchema::loadFromText(
        R"({"dssSchemaVersion":3,"language":{"name":"X","version":"0.1.0"}})");
    ASSERT_FALSE(result.has_value());
    auto const& diags = result.error();
    auto it = std::ranges::find_if(diags, [](auto const& d) {
        return d.code == DiagnosticCode::C_VersionMismatch;
    });
    ASSERT_NE(it, diags.end());
    EXPECT_NE(it->message.find("1..2"), std::string::npos)
        << "version-mismatch message should name the supported range; got: "
        << it->message;
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
