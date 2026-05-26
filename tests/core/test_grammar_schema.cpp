#include "core/types/grammar_schema.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <string>
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
        return d.code == DiagnosticCode::C_UnknownScopeName;
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
    EXPECT_EQ(schema.schemaVersion(), 4u);

    // Every shape name declared in the JSON must resolve. A typo would
    // currently load cleanly and only fail when a caller asks for the
    // missing name; pinning here makes the regression visible at load.
    for (std::string_view rule : {"root", "topLevel", "topLevelDecl",
                                  "topLevelDeclTail", "funcDefTail",
                                  "funcParams", "varDeclTail",
                                  "typeBase", "typeRef",
                                  "varDeclHead", "varDecl",
                                  "paramList", "param", "block", "statement",
                                  "ifStmt", "whileStmt", "doStmt", "forStmt",
                                  "returnStmt", "exprStmt", "expression",
                                  "operand",
                                  // Pratt-walker wrapper rules auto-interned
                                  // by the loader when the schema declares
                                  // any `expr` shape. c-subset's `expression`
                                  // rule is `expr`-kind, so these must be
                                  // present in the rule interner.
                                  "binaryExpr", "unaryExpr", "postfixExpr"}) {
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

    // `typedef` IS a keyword as of SE5 (typedef resolution): it leads the
    // `typedefDecl` shape and resolves to the TypedefKeyword token kind.
    auto const typedefKw = schema.lookupLexeme("typedef");
    ASSERT_EQ(typedefKw.size(), 1u);
    EXPECT_EQ(schema.schemaTokens().name(typedefKw[0].id), "TypedefKeyword");
}

// Pin the `expr`-shape accessors on c-subset. `expression` is the
// only `expr`-kind rule in the shipped grammar; `operand` is its atom.
// Other rules return false / Invalid / 0.
TEST(GrammarSchema, ExprShapeAccessorsOnCSubset) {
    auto result = GrammarSchema::loadShipped("c-subset");
    if (!result.has_value()) {
        FAIL() << "loadShipped c-subset failed: " << result.error()[0].message;
    }
    auto const& schema = **result;

    const auto expression = schema.rules().find("expression");
    const auto operand    = schema.rules().find("operand");
    const auto statement  = schema.rules().find("statement");
    ASSERT_TRUE(expression.valid());
    ASSERT_TRUE(operand.valid());
    ASSERT_TRUE(statement.valid());

    EXPECT_TRUE (schema.isExprRule(expression));
    EXPECT_EQ   (schema.exprAtom(expression).v, operand.v);
    EXPECT_EQ   (schema.exprMinPrecedence(expression), 0);

    EXPECT_FALSE(schema.isExprRule(operand));
    EXPECT_FALSE(schema.exprAtom(operand).valid());
    EXPECT_EQ   (schema.exprMinPrecedence(operand), 0);

    EXPECT_FALSE(schema.isExprRule(statement));
}

// Wrapper rules `binaryExpr` / `unaryExpr` / `postfixExpr` MUST NOT be
// auto-interned in schemas that don't use any `expr` shape. The toy
// grammar's `expression` rule is `sequence`-shaped (not `expr`); a
// regression that unconditionally interns the wrappers would inflate
// every shipped grammar's RuleInterner and silently change RuleId
// numbering.
TEST(GrammarSchema, WrapperRulesAbsentInNonExprSchema) {
    auto result = GrammarSchema::loadShipped("toy");
    ASSERT_TRUE(result.has_value());
    auto const& schema = **result;

    EXPECT_FALSE(schema.rules().find("binaryExpr").valid());
    EXPECT_FALSE(schema.rules().find("unaryExpr").valid());
    EXPECT_FALSE(schema.rules().find("postfixExpr").valid());
}

// Loader rejects user-declared shapes named `binaryExpr` /
// `unaryExpr` / `postfixExpr` — they're walker-synthesized; a user
// redeclaration would let the schema cursor see a body for them,
// breaking the "transparent wrapper" invariant.
TEST(GrammarSchema, LoaderRejectsReservedWrapperShapeName) {
    // 08.55: wrapper-rule names are declared per-language via
    // `expr.wrapperRules`. The loader rejects a top-level `shapes`
    // entry whose name collides with any wrapper rule name declared
    // by this schema (so the schema cursor can't enter through a
    // user-defined body for what is supposed to be a walker-
    // synthesized frame).
    constexpr std::string_view kReservedShape = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "Bad", "version": "0.1.0" },
      "tokens": { "x": [{ "kind": "X" }] },
      "shapes": {
        "root":       { "sequence": ["expression"] },
        "expression": {
          "expr": {
            "atom": "operand",
            "wrapperRules": {
              "binary":  "binaryExpr",
              "unary":   "unaryExpr",
              "postfix": "postfixExpr"
            }
          }
        },
        "operand":    { "sequence": ["X"] },
        "binaryExpr": { "sequence": ["X"] }
      }
    })JSON";
    auto result = GrammarSchema::loadFromText(kReservedShape);
    ASSERT_FALSE(result.has_value());
    bool sawReservedDiag = false;
    for (auto const& d : result.error()) {
        if (d.message.find("binaryExpr") != std::string::npos
            && d.message.find("wrapper") != std::string::npos) {
            sawReservedDiag = true;
            break;
        }
    }
    EXPECT_TRUE(sawReservedDiag);
}

// Body-default kinds are off-grammar. The loader rejects shape
// references AND scope-forbid entries naming them — both surfaces
// would silently never fire at runtime (the cursor-advance gate
// skips body-default kinds), so surface the misuse at load time.
// PA3: `followSetOf(rule)` walks the position graph at load time.
// Verify the textbook FOLLOW computation: for c-subset's `expression`
// rule, FOLLOW must include the tokens that can legitimately appear
// AFTER an expression — `;` (EndStatement) at statement boundary, `)`
// (ParenClose) at paren-wrapped sub-expression boundary, `,` (Comma)
// in argument-list-like positions if any. These are the resync points
// the parser's panic-mode uses for `expr`-kind rules.
TEST(GrammarSchema, FollowSetOfExpressionIncludesStatementEnders) {
    auto result = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(result.has_value());
    auto const& schema = **result;

    const auto expression = schema.rules().find("expression");
    ASSERT_TRUE(expression.valid());

    const auto follow = schema.followSetOf(expression);
    auto containsName = [&](std::string_view name) {
        const auto id = schema.schemaTokens().find(name);
        return id.valid()
            && std::ranges::find_if(follow,
                   [id](SchemaTokenId s) { return s.v == id.v; })
                != follow.end();
    };
    EXPECT_TRUE(containsName("EndStatement"))
        << "FOLLOW(expression) must include `;` — expression can end a statement";
    EXPECT_TRUE(containsName("ParenClose"))
        << "FOLLOW(expression) must include `)` — expression in `(expr)`";
    EXPECT_TRUE(containsName("Colon"))
        << "FOLLOW(expression) must include `:` — `case expr:` label";
}

// FOLLOW propagation across nullable-tail positions: when a RuleLeaf
// reference is followed by an `optional` body that nullable-skips
// to End, the child's FOLLOW must inherit the parent's FOLLOW. This
// is the textbook case for "FOLLOW transitively includes the
// FOLLOW of every rule whose continuation is nullable".
TEST(GrammarSchema, FollowSetPropagatesAcrossNullableTail) {
    constexpr std::string_view cfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "NullableFollow", "version": "0.1.0" },
      "tokens": {
        " ": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
        ";": [{ "kind": "Semi" }],
        ",": [{ "kind": "Comma" }]
      },
      "shapes": {
        "root":   { "sequence": [ "A", "Semi" ] },
        "A":      { "sequence": [ "B", { "optional": "Comma" } ] },
        "B":      { "sequence": [ "Identifier" ] }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.has_value() ? "" : loaded.error()[0].message);
    auto const& schema = **loaded;

    const auto b    = schema.rules().find("B");
    const auto semi = schema.schemaTokens().find("Semi");
    const auto comma = schema.schemaTokens().find("Comma");
    ASSERT_TRUE(b.valid());
    ASSERT_TRUE(semi.valid());
    ASSERT_TRUE(comma.valid());

    const auto follow = schema.followSetOf(b);
    auto contains = [&](SchemaTokenId id) {
        return std::ranges::find_if(follow,
            [id](SchemaTokenId s) { return s.v == id.v; }) != follow.end();
    };
    EXPECT_TRUE(contains(comma))
        << "B is directly followed by `optional Comma` — Comma in FOLLOW(B)";
    EXPECT_TRUE(contains(semi))
        << "FOLLOW(A) (which is {Semi}) must propagate to FOLLOW(B) "
           "because the optional after B is nullable-tail";
}

// Self-recursive rule (`list = Identifier (Comma list)?`): the
// snapshot-per-pass guard in `computeFollowSets` ensures the fixed-
// point converges to a stable FOLLOW set rather than diverging or
// producing iteration-order-dependent results. FOLLOW(list) must
// include Semi (from the parent root sequence).
TEST(GrammarSchema, FollowSetConvergesOnSelfRecursiveRule) {
    constexpr std::string_view cfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "SelfRec", "version": "0.1.0" },
      "tokens": {
        " ": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
        ";": [{ "kind": "Semi" }],
        ",": [{ "kind": "Comma" }]
      },
      "shapes": {
        "root":   { "sequence": [ "list", "Semi" ] },
        "list":   { "sequence": [ "Identifier", { "optional": { "sequence": [ "Comma", "list" ] } } ] }
      }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(loaded.has_value())
        << (loaded.has_value() ? "" : loaded.error()[0].message);
    auto const& schema = **loaded;

    const auto list = schema.rules().find("list");
    const auto semi = schema.schemaTokens().find("Semi");
    ASSERT_TRUE(list.valid());
    const auto follow = schema.followSetOf(list);
    auto contains = [&](SchemaTokenId id) {
        return std::ranges::find_if(follow,
            [id](SchemaTokenId s) { return s.v == id.v; }) != follow.end();
    };
    EXPECT_TRUE(contains(semi))
        << "FOLLOW(list) must include Semi via the root sequence";
}

// Root rule has no parent reference; its FOLLOW must be empty. The
// parser's `canEndSource` check is what authorizes EOF — no implicit
// EOF in FOLLOW(root).
TEST(GrammarSchema, FollowSetOfRootIsEmpty) {
    auto result = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(result.has_value());
    auto const& schema = **result;
    const auto root = schema.rules().find("root");
    ASSERT_TRUE(root.valid());
    EXPECT_TRUE(schema.followSetOf(root).empty());
}

// PA3: `syncTokens` field round-trips. Loader rejects unknown kind
// names, Eof, and Error (each with its own loader diagnostic shape).
TEST(GrammarSchema, SyncTokensRoundTrip) {
    auto result = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(result.has_value());
    auto const& schema = **result;
    const auto sync = schema.syncTokens();

    auto containsName = [&](std::string_view name) {
        const auto id = schema.schemaTokens().find(name);
        return id.valid()
            && std::ranges::find_if(sync,
                   [id](SchemaTokenId s) { return s.v == id.v; })
                != sync.end();
    };
    EXPECT_TRUE(containsName("EndStatement"));
    EXPECT_TRUE(containsName("BlockClose"));
}

TEST(GrammarSchema, SyncTokensRejectsUnknownKind) {
    constexpr std::string_view cfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "Bad", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "syncTokens": ["NotAKind"],
      "shapes": { "root": { "sequence": ["Semi"] } }
    })JSON";
    auto result = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(result.has_value());
    bool saw = false;
    for (auto const& d : result.error()) {
        if (d.code == DiagnosticCode::C_UnknownToken
            && d.message.find("NotAKind") != std::string::npos) {
            saw = true;
            break;
        }
    }
    EXPECT_TRUE(saw);
}

TEST(GrammarSchema, SyncTokensRejectsReservedEof) {
    constexpr std::string_view cfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "Bad", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "syncTokens": ["Eof"],
      "shapes": { "root": { "sequence": ["Semi"] } }
    })JSON";
    auto result = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(result.has_value());
    bool saw = false;
    for (auto const& d : result.error()) {
        if (d.code == DiagnosticCode::C_ConflictingField
            && d.message.find("reserved") != std::string::npos) {
            saw = true;
            break;
        }
    }
    EXPECT_TRUE(saw);
}

TEST(GrammarSchema, RejectsBodyDefaultKindInShape) {
    constexpr std::string_view cfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "BadShape", "version": "0.1.0" },
      "lexerModes": {
        "main": { "tokens": "default" },
        "body": { "defaultToken": { "kind": "BodyChar" }, "unterminatedAs": "string" }
      },
      "tokens": {
        "(": [{ "kind": "Open", "modeOp": "pushMode", "modeArg": "body",
                "stringStyle": { "escapeKind": "none", "endsAt": ")" } }]
      },
      "shapes": {
        "root": { "sequence": ["Open", "BodyChar"] }
      }
    })JSON";
    auto result = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(result.has_value());

    bool saw = false;
    for (auto const& d : result.error()) {
        if (d.code == DiagnosticCode::C_BodyDefaultKindInShape
            && d.message.find("BodyChar") != std::string::npos) {
            saw = true;
            break;
        }
    }
    EXPECT_TRUE(saw)
        << "C_BodyDefaultKindInShape must fire for a shape referencing "
           "a body-default token kind";
}

TEST(GrammarSchema, RejectsBodyDefaultKindInScopeForbid) {
    constexpr std::string_view cfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "BadForbid", "version": "0.1.0" },
      "lexerModes": {
        "main": { "tokens": "default" },
        "body": { "defaultToken": { "kind": "BodyChar" }, "unterminatedAs": "string" }
      },
      "tokens": {
        "{": [{ "kind": "BlockOpen", "opensScope": "Block" }],
        "(": [{ "kind": "Open", "modeOp": "pushMode", "modeArg": "body",
                "stringStyle": { "escapeKind": "none", "endsAt": ")" } }]
      },
      "scopes": {
        "validity": [ { "scope": "Block", "forbid": ["BodyChar"] } ]
      },
      "shapes": {
        "root": { "sequence": ["Open"] }
      }
    })JSON";
    auto result = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(result.has_value());

    bool saw = false;
    for (auto const& d : result.error()) {
        if (d.code == DiagnosticCode::C_BodyDefaultKindInShape
            && d.message.find("BodyChar") != std::string::npos
            && d.message.find("forbidden") != std::string::npos) {
            saw = true;
            break;
        }
    }
    EXPECT_TRUE(saw)
        << "C_BodyDefaultKindInShape must fire for scope-forbid "
           "entries referencing a body-default kind";
}

// Pin the FIRST-set augmentation: `expr`-shape rules see prefix
// operator tokens added to their FIRST set so the dispatch loop's
// `tokInFirst` check accepts bare-prefix expressions (`-a;` etc.)
// without the walker. Without this, the dispatch would emit
// `P_NoAlternativeMatched` before the walker ever ran.
TEST(GrammarSchema, ExprRuleFirstSetIncludesPrefixOperators) {
    constexpr std::string_view kPrefixSchema = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "PrefixFirst", "version": "0.1.0" },
      "tokens": {
        "-":  [{ "kind": "MinusOp" }]
      },
      "operators": {
        "groups": [
          { "precedence": 90, "associativity": "right", "arity": "prefix", "operators": ["-"] }
        ]
      },
      "shapes": {
        "root":       { "sequence": ["expression"] },
        "expression": {
          "expr": {
            "atom": "operand",
            "wrapperRules": {
              "binary":  "binaryExpr",
              "unary":   "unaryExpr",
              "postfix": "postfixExpr"
            }
          }
        },
        "operand":    { "alt": ["Identifier"] }
      }
    })JSON";
    auto result = GrammarSchema::loadFromText(kPrefixSchema);
    ASSERT_TRUE(result.has_value())
        << (result.has_value() ? "" : result.error()[0].message);
    auto const& schema = **result;

    const auto expression = schema.rules().find("expression");
    const auto minusOp    = schema.schemaTokens().find("MinusOp");
    const auto identifier = schema.schemaTokens().find("Identifier");
    ASSERT_TRUE(expression.valid());
    ASSERT_TRUE(minusOp.valid());
    ASSERT_TRUE(identifier.valid());

    const auto firstSet = schema.firstSetOf(expression);
    const auto hasMinus = std::ranges::find_if(firstSet,
        [minusOp](SchemaTokenId id) { return id.v == minusOp.v; })
        != firstSet.end();
    const auto hasIdent = std::ranges::find_if(firstSet,
        [identifier](SchemaTokenId id) { return id.v == identifier.v; })
        != firstSet.end();
    EXPECT_TRUE(hasMinus)
        << "FIRST(expression) must include MinusOp (Prefix op union)";
    EXPECT_TRUE(hasIdent)
        << "FIRST(expression) must include Identifier (atom FIRST)";
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

// dssSchemaVersion 2 must load AND emit zero diagnostics — a future
// warning-on-version-2 regression would silently pass without this.
TEST(GrammarSchema, SchemaVersionTwoAccepted) {
    auto result = GrammarSchema::loadFromText(
        R"({"dssSchemaVersion":2,"language":{"name":"X","version":"0.1.0"}})");
    ASSERT_TRUE(result.has_value())
        << "v2 doc should load: "
        << (result.error().empty() ? "<no diagnostics>" : result.error()[0].message);
    EXPECT_EQ((*result)->schemaVersion(), 2u);
}

// dssSchemaVersion 3 added the optional `typeExtensions[]` field (SP2). A v3
// doc with no extensions loads cleanly.
TEST(GrammarSchema, SchemaVersionThreeAccepted) {
    auto result = GrammarSchema::loadFromText(
        R"({"dssSchemaVersion":3,"language":{"name":"X","version":"0.1.0"}})");
    ASSERT_TRUE(result.has_value())
        << "v3 doc should load: "
        << (result.error().empty() ? "<no diagnostics>" : result.error()[0].message);
    EXPECT_EQ((*result)->schemaVersion(), 3u);
    EXPECT_TRUE((*result)->typeExtensions().empty());
}

// dssSchemaVersion 4 is the upper bound of the loader's accepted window since
// the config-driven import refactor (it adds the optional `imports` block). A
// v4 doc with no `imports` block loads cleanly and defaults to strategy None.
TEST(GrammarSchema, SchemaVersionFourAccepted) {
    auto result = GrammarSchema::loadFromText(
        R"({"dssSchemaVersion":4,"language":{"name":"X","version":"0.1.0"}})");
    ASSERT_TRUE(result.has_value())
        << "v4 doc should load: "
        << (result.error().empty() ? "<no diagnostics>" : result.error()[0].message);
    EXPECT_EQ((*result)->schemaVersion(), 4u);
    EXPECT_EQ((*result)->imports().strategy, ImportStrategy::None);
}

// Outside the accepted window, the loader must emit C_VersionMismatch with a
// message naming the supported range. The range string is the load-bearing
// fragment — full-message equality would over-pin the surrounding prose, but
// the range itself MUST be there or the diagnostic is uselessly opaque.
TEST(GrammarSchema, SchemaVersionFiveRejectedWithRangeMessage) {
    auto result = GrammarSchema::loadFromText(
        R"({"dssSchemaVersion":5,"language":{"name":"X","version":"0.1.0"}})");
    ASSERT_FALSE(result.has_value());
    auto const& diags = result.error();
    auto it = std::ranges::find_if(diags, [](auto const& d) {
        return d.code == DiagnosticCode::C_VersionMismatch;
    });
    ASSERT_NE(it, diags.end());
    EXPECT_NE(it->message.find("1..4"), std::string::npos)
        << "version-mismatch message should name the supported range; got: "
        << it->message;
}

// ── typeExtensions[] (SP2, schema v3) ──────────────────────────────────────

namespace {
[[nodiscard]] bool hasDiagCode(std::vector<ConfigDiagnostic> const& diags, DiagnosticCode code) {
    return std::ranges::any_of(diags, [code](auto const& d) { return d.code == code; });
}
} // namespace

TEST(GrammarSchema, TypeExtensionsLoadAndPopulate) {
    auto result = GrammarSchema::loadFromText(R"JSON({
        "dssSchemaVersion": 3,
        "language": { "name": "TsqlSubset", "version": "0.1.0" },
        "typeExtensions": [
            { "name": "TSQL::Varchar", "parameters": [ { "name": "N", "kind": "Integer" } ] },
            { "name": "TSQL::RowType" }
        ]
    })JSON");
    ASSERT_TRUE(result.has_value())
        << (result.error().empty() ? "<no diagnostics>" : result.error()[0].message);
    auto exts = (*result)->typeExtensions();
    ASSERT_EQ(exts.size(), 2u);
    EXPECT_EQ(exts[0].name, "TSQL::Varchar");
    ASSERT_EQ(exts[0].parameters.size(), 1u);
    EXPECT_EQ(exts[0].parameters[0].name, "N");
    EXPECT_EQ(exts[0].parameters[0].kind, TypeParamKind::Integer);
    EXPECT_EQ(exts[1].name, "TSQL::RowType");
    EXPECT_TRUE(exts[1].parameters.empty());
}

TEST(GrammarSchema, TypeExtensionBadParamKindReportsMismatch) {
    auto result = GrammarSchema::loadFromText(R"JSON({
        "dssSchemaVersion": 3,
        "language": { "name": "X", "version": "0.1.0" },
        "typeExtensions": [ { "name": "X::Foo", "parameters": [ { "name": "N", "kind": "Nope" } ] } ]
    })JSON");
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_TypeExtensionParamMismatch));
}

TEST(GrammarSchema, TypeExtensionsNotArrayReportsUnknown) {
    auto result = GrammarSchema::loadFromText(R"JSON({
        "dssSchemaVersion": 3,
        "language": { "name": "X", "version": "0.1.0" },
        "typeExtensions": { "name": "X::Foo" }
    })JSON");
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_UnknownTypeExtension));
}

TEST(GrammarSchema, TypeExtensionEntryNotObjectReportsUnknown) {
    auto result = GrammarSchema::loadFromText(R"JSON({
        "dssSchemaVersion": 3,
        "language": { "name": "X", "version": "0.1.0" },
        "typeExtensions": [ 42 ]
    })JSON");
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_UnknownTypeExtension));
}

TEST(GrammarSchema, TypeExtensionDuplicateNameReportsConflict) {
    auto result = GrammarSchema::loadFromText(R"JSON({
        "dssSchemaVersion": 3,
        "language": { "name": "X", "version": "0.1.0" },
        "typeExtensions": [ { "name": "X::Foo" }, { "name": "X::Foo" } ]
    })JSON");
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_ConflictingField));
}

TEST(GrammarSchema, TypeExtensionTypeKindParamLoads) {
    auto result = GrammarSchema::loadFromText(R"JSON({
        "dssSchemaVersion": 3,
        "language": { "name": "X", "version": "0.1.0" },
        "typeExtensions": [ { "name": "X::Boxed", "parameters": [ { "name": "T", "kind": "Type" } ] } ]
    })JSON");
    ASSERT_TRUE(result.has_value())
        << (result.error().empty() ? "<no diagnostics>" : result.error()[0].message);
    auto exts = (*result)->typeExtensions();
    ASSERT_EQ(exts.size(), 1u);
    ASSERT_EQ(exts[0].parameters.size(), 1u);
    EXPECT_EQ(exts[0].parameters[0].kind, TypeParamKind::Type);
}

TEST(GrammarSchema, TypeExtensionMissingNameReportsMissingField) {
    auto result = GrammarSchema::loadFromText(R"JSON({
        "dssSchemaVersion": 3,
        "language": { "name": "X", "version": "0.1.0" },
        "typeExtensions": [ { "parameters": [] } ]
    })JSON");
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_MissingField));
}

TEST(GrammarSchema, TypeExtensionParametersNotArrayReportsMismatch) {
    auto result = GrammarSchema::loadFromText(R"JSON({
        "dssSchemaVersion": 3,
        "language": { "name": "X", "version": "0.1.0" },
        "typeExtensions": [ { "name": "X::Foo", "parameters": { "name": "N" } } ]
    })JSON");
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_TypeExtensionParamMismatch));
}

// ── imports block (schema v4) ──────────────────────────────────────────────

// An include-following `imports` block populates every parameterized field —
// the resolver reads these instead of hardcoding rule/token names.
TEST(GrammarSchema, ImportsIncludeFollowingLoadsAndPopulates) {
    auto result = GrammarSchema::loadFromText(R"JSON({
        "dssSchemaVersion": 4,
        "language": { "name": "CSubset", "version": "0.1.0" },
        "tokens": { "#": [ { "kind": "IncludeKeyword" } ],
                    "\"": [ { "kind": "StringStart" } ] },
        "shapes": {
            "root": { "sequence": [ "includeDirective" ] },
            "includeDirective": { "sequence": [ "IncludeKeyword", "StringStart" ] }
        }
    })JSON");
    ASSERT_TRUE(result.has_value())
        << (result.error().empty() ? "<no diagnostics>" : result.error()[0].message);
    auto const& cfg = (*result)->imports();
    EXPECT_EQ(cfg.strategy, ImportStrategy::None)  // no block declared yet
        << "control: a config without an `imports` block stays None";
}

TEST(GrammarSchema, ImportsIncludeFollowingFieldsParse) {
    auto result = GrammarSchema::loadFromText(R"JSON({
        "dssSchemaVersion": 4,
        "language": { "name": "CSubset", "version": "0.1.0" },
        "imports": { "strategy": "include-following",
                     "directiveRule": "includeDirective", "pathToken": "StringStart" },
        "tokens": { "#": [ { "kind": "IncludeKeyword" } ],
                    "\"": [ { "kind": "StringStart" } ] },
        "shapes": {
            "root": { "sequence": [ "includeDirective" ] },
            "includeDirective": { "sequence": [ "IncludeKeyword", "StringStart" ] }
        }
    })JSON");
    ASSERT_TRUE(result.has_value())
        << (result.error().empty() ? "<no diagnostics>" : result.error()[0].message);
    auto const& cfg = (*result)->imports();
    EXPECT_EQ(cfg.strategy, ImportStrategy::IncludeFollowing);
    EXPECT_EQ(cfg.directiveRule, "includeDirective");
    EXPECT_EQ(cfg.pathToken, "StringStart");
    EXPECT_TRUE(cfg.caseSensitive);  // default
}

TEST(GrammarSchema, ImportsNameMatchingFieldsParse) {
    auto result = GrammarSchema::loadFromText(R"JSON({
        "dssSchemaVersion": 4,
        "language": { "name": "TsqlSubset", "version": "0.1.0" },
        "imports": { "strategy": "name-matching",
                     "nameRule": "qualifiedName", "definitionRule": "createTableStmt",
                     "referenceParents": [ "tableRef" ], "nameToken": "Identifier",
                     "caseSensitive": false },
        "shapes": {
            "root": { "sequence": [ "createTableStmt", "tableRef" ] },
            "qualifiedName": { "sequence": [ "Identifier" ] },
            "createTableStmt": { "sequence": [ "qualifiedName" ] },
            "tableRef": { "sequence": [ "qualifiedName" ] }
        }
    })JSON");
    ASSERT_TRUE(result.has_value())
        << (result.error().empty() ? "<no diagnostics>" : result.error()[0].message);
    auto const& cfg = (*result)->imports();
    EXPECT_EQ(cfg.strategy, ImportStrategy::NameMatching);
    EXPECT_EQ(cfg.nameRule, "qualifiedName");
    EXPECT_EQ(cfg.definitionRule, "createTableStmt");
    ASSERT_EQ(cfg.referenceParents.size(), 1u);
    EXPECT_EQ(cfg.referenceParents[0], "tableRef");
    EXPECT_EQ(cfg.nameToken, "Identifier");
    EXPECT_FALSE(cfg.caseSensitive);
}

// An unknown `strategy` is a malformed block — C_InvalidImports.
TEST(GrammarSchema, ImportsUnknownStrategyReportsInvalid) {
    auto result = GrammarSchema::loadFromText(R"JSON({
        "dssSchemaVersion": 4,
        "language": { "name": "X", "version": "0.1.0" },
        "imports": { "strategy": "telepathy" }
    })JSON");
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_InvalidImports));
}

// include-following without `directiveRule` is missing a required field.
TEST(GrammarSchema, ImportsIncludeFollowingMissingDirectiveRuleReportsMissingField) {
    auto result = GrammarSchema::loadFromText(R"JSON({
        "dssSchemaVersion": 4,
        "language": { "name": "X", "version": "0.1.0" },
        "imports": { "strategy": "include-following", "pathToken": "StringStart" },
        "tokens": { "\"": [ { "kind": "StringStart" } ] },
        "shapes": { "root": { "sequence": [ "StringStart" ] } }
    })JSON");
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_MissingField));
}

// A `directiveRule` that names no declared shape is C_UnknownShape (the loader
// parses `imports` late, after shapes are interned, so it can check existence).
TEST(GrammarSchema, ImportsUnknownRuleReportsUnknownShape) {
    auto result = GrammarSchema::loadFromText(R"JSON({
        "dssSchemaVersion": 4,
        "language": { "name": "X", "version": "0.1.0" },
        "imports": { "strategy": "include-following",
                     "directiveRule": "ghostRule", "pathToken": "StringStart" },
        "tokens": { "\"": [ { "kind": "StringStart" } ] },
        "shapes": { "root": { "sequence": [ "StringStart" ] } }
    })JSON");
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_UnknownShape));
}

// A `pathToken` that names no declared token kind is C_UnknownToken — the
// token-side analogue of C_UnknownShape above (loader checks both interners).
TEST(GrammarSchema, ImportsUnknownTokenReportsUnknownToken) {
    auto result = GrammarSchema::loadFromText(R"JSON({
        "dssSchemaVersion": 4,
        "language": { "name": "X", "version": "0.1.0" },
        "imports": { "strategy": "include-following",
                     "directiveRule": "includeDirective", "pathToken": "GhostToken" },
        "tokens": { "\"": [ { "kind": "StringStart" } ] },
        "shapes": {
            "root": { "sequence": [ "includeDirective" ] },
            "includeDirective": { "sequence": [ "StringStart" ] }
        }
    })JSON");
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_UnknownToken));
}

// A `referenceParents[i]` that names no declared shape is C_UnknownShape —
// the per-entry analogue of the scalar-field dangling-name check.
TEST(GrammarSchema, ImportsUnknownReferenceParentReportsUnknownShape) {
    auto result = GrammarSchema::loadFromText(R"JSON({
        "dssSchemaVersion": 4,
        "language": { "name": "X", "version": "0.1.0" },
        "imports": { "strategy": "name-matching",
                     "nameRule": "qualifiedName", "definitionRule": "createTableStmt",
                     "referenceParents": [ "ghostParent" ], "nameToken": "Identifier" },
        "tokens": { "x": [ { "kind": "Identifier" } ] },
        "shapes": {
            "root": { "sequence": [ "qualifiedName" ] },
            "qualifiedName": { "sequence": [ "Identifier" ] },
            "createTableStmt": { "sequence": [ "qualifiedName" ] },
            "tableRef": { "sequence": [ "qualifiedName" ] }
        }
    })JSON");
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_UnknownShape));
}

// name-matching with an empty `referenceParents` array is malformed —
// without parents there is nowhere to anchor reference recognition.
TEST(GrammarSchema, ImportsNameMatchingEmptyReferenceParentsReportsInvalid) {
    auto result = GrammarSchema::loadFromText(R"JSON({
        "dssSchemaVersion": 4,
        "language": { "name": "X", "version": "0.1.0" },
        "imports": { "strategy": "name-matching",
                     "nameRule": "qualifiedName", "definitionRule": "createTableStmt",
                     "referenceParents": [], "nameToken": "Identifier" },
        "tokens": { "x": [ { "kind": "Identifier" } ] },
        "shapes": {
            "root": { "sequence": [ "qualifiedName" ] },
            "qualifiedName": { "sequence": [ "Identifier" ] },
            "createTableStmt": { "sequence": [ "qualifiedName" ] }
        }
    })JSON");
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_InvalidImports) ||
                hasDiagCode(result.error(), DiagnosticCode::C_MissingField));
}

// `caseSensitive` is documented as an optional bool — any other JSON type is
// a malformed block (C_InvalidImports), not silently coerced.
TEST(GrammarSchema, ImportsCaseSensitiveWrongTypeReportsInvalid) {
    auto result = GrammarSchema::loadFromText(R"JSON({
        "dssSchemaVersion": 4,
        "language": { "name": "X", "version": "0.1.0" },
        "imports": { "strategy": "name-matching",
                     "nameRule": "qualifiedName", "definitionRule": "createTableStmt",
                     "referenceParents": [ "tableRef" ], "nameToken": "Identifier",
                     "caseSensitive": "yes" },
        "tokens": { "x": [ { "kind": "Identifier" } ] },
        "shapes": {
            "root": { "sequence": [ "qualifiedName" ] },
            "qualifiedName": { "sequence": [ "Identifier" ] },
            "createTableStmt": { "sequence": [ "qualifiedName" ] },
            "tableRef": { "sequence": [ "qualifiedName" ] }
        }
    })JSON");
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_InvalidImports));
}

// `imports` itself must be a JSON object, not a string/array/number — the
// shape-level type guard.
TEST(GrammarSchema, ImportsNotAnObjectReportsInvalid) {
    auto result = GrammarSchema::loadFromText(R"JSON({
        "dssSchemaVersion": 4,
        "language": { "name": "X", "version": "0.1.0" },
        "imports": "include-following",
        "tokens": { "x": [ { "kind": "Identifier" } ] },
        "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON");
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_InvalidImports));
}

// Pins the cookbook example in docs/language-config-spec.md §7 against the
// loader. If this fails, the doc's "Loads cleanly because:" claims are wrong.
TEST(GrammarSchema, DocsCookbookCalcExampleLoadsCleanly) {
    constexpr std::string_view kCalcCookbook = R"JSON({
  "dssSchemaVersion": 4,

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

  "numberStyle": {
    "decimal":  true,
    "emitKind": { "integer": "IntLiteral" }
  },

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

// ── 08.55 cleanup: wrapperRules + numberStyle strict pins ──────────────

// Missing `wrapperRules` block fails to load with C_MissingWrapperRules.
TEST(GrammarSchema, ExprShapeWithoutWrapperRulesIsRejected) {
    constexpr std::string_view kNoWrap = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "NoWrap", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": {
        "root":       { "sequence": ["expression", "Semi"] },
        "expression": { "expr": { "atom": "operand" } },
        "operand":    { "alt": ["Identifier"] }
      }
    })JSON";
    auto result = GrammarSchema::loadFromText(kNoWrap);
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_MissingWrapperRules));
}

// Partial `wrapperRules` (missing one of binary/unary/postfix) is rejected.
TEST(GrammarSchema, ExprShapeWithPartialWrapperRulesIsRejected) {
    constexpr std::string_view kPartial = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "PartialWrap", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": {
        "root":       { "sequence": ["expression", "Semi"] },
        "expression": {
          "expr": {
            "atom": "operand",
            "wrapperRules": { "binary": "bExpr", "unary": "uExpr" }
          }
        },
        "operand":    { "alt": ["Identifier"] }
      }
    })JSON";
    auto result = GrammarSchema::loadFromText(kPartial);
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_MissingWrapperRules));
}

// Happy-path: distinct (non-c-subset) wrapper-rule names load cleanly,
// the parser-side schema lookup returns the right RuleIds. Genericity
// pin: the engine has NO hardcoded `binaryExpr`/`unaryExpr`/`postfixExpr`
// names — any names work.
TEST(GrammarSchema, ExprShapeWithCustomWrapperRuleNamesIsAccepted) {
    constexpr std::string_view kCustom = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "CustomWrap", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": {
        "root":       { "sequence": ["expression", "Semi"] },
        "expression": {
          "expr": {
            "atom": "operand",
            "wrapperRules": {
              "binary":  "bExpr",
              "unary":   "uExpr",
              "postfix": "pExpr"
            }
          }
        },
        "operand":    { "alt": ["Identifier"] }
      }
    })JSON";
    auto result = GrammarSchema::loadFromText(kCustom);
    ASSERT_TRUE(result.has_value())
        << (result.error().empty() ? "<no diagnostics>" : result.error()[0].message);
    auto& schema = **result;
    const auto exprRule = schema.rules().find("expression");
    ASSERT_TRUE(exprRule.valid());
    const auto pack = schema.exprWrapperRules(exprRule);
    EXPECT_TRUE(pack.valid());
    EXPECT_EQ(schema.rules().name(pack.binary),  "bExpr");
    EXPECT_EQ(schema.rules().name(pack.unary),   "uExpr");
    EXPECT_EQ(schema.rules().name(pack.postfix), "pExpr");
}

// numberStyle absent + IntLiteral referenced in a shape is rejected.
TEST(GrammarSchema, IntLiteralInShapeWithoutNumberStyleIsRejected) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "NoNum", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": {
        "root": { "sequence": ["IntLiteral", "Semi"] }
      }
    })JSON";
    auto result = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_MissingNumberStyle));
}

// Happy-path: numberStyle parsed cleanly, all fields round-trip.
TEST(GrammarSchema, NumberStyleHappyPathRoundTrips) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "Nums", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "numberStyle": {
        "decimal":         true,
        "integerPrefixes": [
          { "prefix": "0x", "radix": 16, "digits": "0-9a-fA-F" }
        ],
        "exponent":        { "letters": ["e", "E"], "signOptional": true },
        "fractionPoint":   ".",
        "digitSeparator":  "_",
        "integerSuffixes": ["u", "L"],
        "floatSuffixes":   ["f"],
        "emitKind":        { "integer": "IntLiteral", "float": "FloatLiteral" }
      },
      "shapes": {
        "root": { "sequence": ["IntLiteral", "Semi"] }
      }
    })JSON";
    auto result = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(result.has_value())
        << (result.error().empty() ? "<no diagnostics>" : result.error()[0].message);
    auto const* ns = (*result)->numberStyle();
    ASSERT_NE(ns, nullptr);
    EXPECT_TRUE(ns->decimal);
    ASSERT_EQ(ns->integerPrefixes.size(), 1u);
    EXPECT_EQ(ns->integerPrefixes[0].prefix, "0x");
    EXPECT_EQ(ns->integerPrefixes[0].radix,  16u);
    ASSERT_TRUE(ns->exponent.has_value());
    EXPECT_EQ(ns->exponent->letters.size(), 2u);
    EXPECT_EQ(ns->exponent->letters[0], 'e');
    EXPECT_TRUE(ns->exponent->signOptional);
    ASSERT_TRUE(ns->fractionPoint.has_value());
    EXPECT_EQ(*ns->fractionPoint, '.');
    ASSERT_TRUE(ns->digitSeparator.has_value());
    EXPECT_EQ(*ns->digitSeparator, '_');
    EXPECT_EQ(ns->integerSuffixes.size(), 2u);
    EXPECT_EQ(ns->floatSuffixes.size(),   1u);
}

// emitKind.integer is required.
TEST(GrammarSchema, NumberStyleMissingEmitKindIntegerIsRejected) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "NoEmit", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "numberStyle": {
        "decimal":  true,
        "emitKind": { "float": "FloatLiteral" }
      },
      "shapes": {
        "root": { "sequence": ["IntLiteral", "Semi"] }
      }
    })JSON";
    auto result = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(result.has_value());
    // F13 (08.55 remediation): C_MissingNumberStyle is reserved for
    // "block entirely absent". A required sub-field that is missing
    // or empty inside an existing block uses C_MissingField.
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_MissingField));
}

// F13: out-of-range radix is now C_InvalidNumberStyle (was overloaded
// onto C_MissingNumberStyle prior to the 08.55 remediation pass).
TEST(GrammarSchema, NumberStyleRadixOutOfRangeReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "BadRadix", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "numberStyle": {
        "decimal": true,
        "integerPrefixes": [ { "prefix": "0x", "radix": 99, "digits": "0-9a-fA-F" } ],
        "emitKind": { "integer": "IntLiteral" }
      },
      "shapes": {
        "root": { "sequence": ["IntLiteral", "Semi"] }
      }
    })JSON";
    auto result = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_InvalidNumberStyle));
}

// F13: numberStyle that isn't an object is C_InvalidNumberStyle (the
// block is present but malformed at the top level).
TEST(GrammarSchema, NumberStyleNotAnObjectReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "BadType", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "numberStyle": "wrong",
      "shapes": {
        "root": { "sequence": ["IntLiteral", "Semi"] }
      }
    })JSON";
    auto result = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_InvalidNumberStyle));
}

// F3: tsql-subset declares typeExtensions[] for parameterized types
// (VARCHAR(N) — Integer parameter). Verify the shipped config carries
// the registration so a future grammar-level update can wire the
// shape parser through it.
TEST(GrammarSchema, TsqlSubsetTypeExtensionsAreDeclared) {
    auto r = GrammarSchema::loadShipped("tsql-subset");
    ASSERT_TRUE(r.has_value());
    auto exts = (*r)->typeExtensions();
    ASSERT_FALSE(exts.empty());
    bool sawVarchar = false;
    for (auto const& e : exts) {
        if (e.name == "TSQL::Varchar") {
            sawVarchar = true;
            ASSERT_EQ(e.parameters.size(), 1u);
            EXPECT_EQ(e.parameters[0].kind, TypeParamKind::Integer);
        }
    }
    EXPECT_TRUE(sawVarchar);
}

// F5: pairwise-distinct check on wrapperRules. The walker tags
// Pratt frames by RuleId; two rules collapsing to the same id
// would silently miscount nesting depth.
TEST(GrammarSchema, ExprWrapperRulesDuplicateRuleNamesRejected) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "DupWrap", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp" }],
        ";": [{ "kind": "Semi" }]
      },
      "operators": {
        "groups": [ { "precedence": 10, "operators": ["+"] } ]
      },
      "shapes": {
        "root":     { "sequence": ["expression", "Semi"] },
        "expression": {
          "expr": {
            "atom": "operand",
            "wrapperRules": {
              "binary":  "X",
              "unary":   "X",
              "postfix": "Y"
            }
          }
        },
        "operand": { "sequence": ["Identifier"] }
      }
    })JSON";
    auto result = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_DuplicateWrapperRules));
}

// F17: unknown wrapperRules key is C_UnknownShape (matches the
// sibling expr-body unknown-key check), NOT C_MissingWrapperRules
// (reserved for "field absent/empty").
TEST(GrammarSchema, ExprWrapperRulesUnknownKeyReportsUnknownShape) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "UnkKey", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp" }],
        ";": [{ "kind": "Semi" }]
      },
      "operators": {
        "groups": [ { "precedence": 10, "operators": ["+"] } ]
      },
      "shapes": {
        "root":     { "sequence": ["expression", "Semi"] },
        "expression": {
          "expr": {
            "atom": "operand",
            "wrapperRules": {
              "binary":  "B",
              "unary":   "U",
              "postfix": "P",
              "infix":   "I"
            }
          }
        },
        "operand": { "sequence": ["Identifier"] }
      }
    })JSON";
    auto result = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_UnknownShape));
}

// F15: built-in token kinds are exactly the eight universal
// categories. Paradigm-specific kinds (CharLiteral, BoolLiteral,
// NullLiteral) are NOT pre-interned and must be declared by the
// language.
TEST(GrammarSchema, BuiltinTokenKindsAreExactlyUniversal) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "Empty", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": ["Semi"] } }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(r.has_value());
    auto const& tokens = (*r)->schemaTokens();
    EXPECT_TRUE(tokens.contains("Identifier"));
    EXPECT_TRUE(tokens.contains("IntLiteral"));
    EXPECT_TRUE(tokens.contains("FloatLiteral"));
    EXPECT_TRUE(tokens.contains("StringLiteral"));
    EXPECT_TRUE(tokens.contains("Eof"));
    EXPECT_TRUE(tokens.contains("Error"));
    EXPECT_TRUE(tokens.contains("Whitespace"));
    EXPECT_TRUE(tokens.contains("Newline"));
    EXPECT_FALSE(tokens.contains("CharLiteral"));
    EXPECT_FALSE(tokens.contains("BoolLiteral"));
    EXPECT_FALSE(tokens.contains("NullLiteral"));
}

// F15: a config that references a demoted built-in without
// declaring it must emit C_UnknownToken.
TEST(GrammarSchema, DemotedBuiltinReferencedWithoutDeclarationReportsCode) {
    for (auto const* name : {"BoolLiteral", "CharLiteral", "NullLiteral"}) {
        const std::string kCfg = std::format(R"JSON({{
          "dssSchemaVersion": 4,
          "language": {{ "name": "Demoted", "version": "0.1.0" }},
          "tokens": {{ ";": [{{ "kind": "Semi" }}] }},
          "shapes": {{ "root": {{ "sequence": ["{}", "Semi"] }} }}
        }})JSON", name);
        auto r = GrammarSchema::loadFromText(kCfg);
        ASSERT_FALSE(r.has_value()) << "unexpected success for " << name;
        EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_UnknownShape)
                    || hasDiagCode(r.error(), DiagnosticCode::C_UnknownToken))
            << "no diagnostic flagging the demoted built-in '" << name << "'";
    }
}

// F15: a config that DECLARES `BoolLiteral` via keywords loads
// cleanly. The demotion is value-neutral — the kind is just no
// longer auto-interned at schema-build time.
TEST(GrammarSchema, DemotedBuiltinDeclaredExplicitlyLoadsCleanly) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "WithBool", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "keywords": [
        { "word": "true",  "kind": "BoolLiteral" },
        { "word": "false", "kind": "BoolLiteral" }
      ],
      "shapes": { "root": { "sequence": ["BoolLiteral", "Semi"] } }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(r.has_value())
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
    EXPECT_TRUE((*r)->schemaTokens().contains("BoolLiteral"));
}

// ── semantics block (schema v4; plan 08.6) ───────────────────────────────

// Happy-path: a complete `semantics` block round-trips, exposing
// declarations / references / scopes / builtinTypes / typeShapes /
// literalTypes via SemanticConfig.
TEST(GrammarSchema, SemanticsBlockHappyPathRoundTrips) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        " ": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
        "=": [{ "kind": "Eq" }],
        ";": [{ "kind": "Semi" }]
      },
      "keywords": [ { "word": "let", "kind": "LetKw" } ],
      "shapes": {
        "root":  { "sequence": [ { "repeat": "stmt" } ] },
        "stmt":  { "alt": [ "decl", "use" ] },
        "decl":  { "sequence": [ "LetKw", "Identifier", "Eq", "use", "Semi" ] },
        "use":   { "sequence": [ "Identifier" ] },
        "block": { "sequence": [ "stmt" ] }
      },
      "semantics": {
        "declarations": [
          { "rule": "decl", "name": 1, "init": 3, "kind": "variable" }
        ],
        "references": [
          { "rule": "use" }
        ],
        "scopes": [ "block" ],
        "builtinTypes": [
          { "name": "int",  "core": "I32"  },
          { "name": "bool", "core": "Bool" }
        ],
        "literalTypes": []
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(r.has_value())
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
    auto const& sem = (*r)->semantics();
    ASSERT_EQ(sem.declarations.size(), 1u);
    EXPECT_EQ(sem.declarations[0].ruleName, "decl");
    EXPECT_EQ(sem.declarations[0].nameChild, 1);
    EXPECT_EQ(sem.declarations[0].initChild, 3);
    EXPECT_EQ(sem.declarations[0].kind, DeclarationKind::Variable);
    ASSERT_EQ(sem.references.size(), 1u);
    EXPECT_EQ(sem.references[0].ruleName, "use");
    ASSERT_EQ(sem.scopes.size(), 1u);
    EXPECT_EQ(sem.scopes[0].ruleName, "block");
    ASSERT_EQ(sem.builtinTypes.size(), 2u);
    EXPECT_EQ(sem.builtinTypes[0].name, "int");
    EXPECT_EQ(sem.builtinTypes[0].core, TypeKind::I32);
    EXPECT_EQ(sem.builtinTypes[1].core, TypeKind::Bool);
}

// Absent `semantics` block is fine — analyzer just doesn't analyze.
TEST(GrammarSchema, SemanticsAbsentLoadsCleanly) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(r.has_value());
    auto const& sem = (*r)->semantics();
    EXPECT_TRUE(sem.declarations.empty());
    EXPECT_TRUE(sem.references.empty());
    EXPECT_TRUE(sem.scopes.empty());
    EXPECT_TRUE(sem.builtinTypes.empty());
}

// `semantics` itself must be an object — array/string is malformed.
TEST(GrammarSchema, SemanticsNotAnObjectReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": [1, 2, 3]
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// Missing required `rule` field on a declaration entry → C_MissingField.
TEST(GrammarSchema, SemanticsDeclarationMissingRuleField) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": {
        "declarations": [ { "name": 0 } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_MissingField));
}

// A declaration whose `rule` names no declared shape → C_UnknownShape.
TEST(GrammarSchema, SemanticsDeclarationUnknownRuleShape) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": {
        "declarations": [ { "rule": "ghostDecl", "name": 0 } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_UnknownShape));
}

// Unknown declaration `kind` string → C_InvalidSemantics.
TEST(GrammarSchema, SemanticsUnknownDeclKindReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": {
        "declarations": [ { "rule": "root", "name": 0, "kind": "telepathic" } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// `literalTypes[i].literal` that names no declared token → C_UnknownToken.
TEST(GrammarSchema, SemanticsLiteralUnknownTokenReportsUnknownToken) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": {
        "literalTypes": [ { "literal": "GhostLit", "core": "I32" } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_UnknownToken));
}

// An unknown `core` TypeKind string → C_InvalidSemantics.
TEST(GrammarSchema, SemanticsUnknownCoreKindReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": {
        "builtinTypes": [ { "name": "weird", "core": "NotAKind" } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// Unknown typeShape `constructor` string → C_InvalidSemantics.
TEST(GrammarSchema, SemanticsUnknownTypeConstructorReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": {
        "typeShapes": [ { "rule": "root", "constructor": "magic" } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// `scopes[i]` referencing an unknown shape → C_UnknownShape.
TEST(GrammarSchema, SemanticsScopesUnknownRuleReportsUnknownShape) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": {
        "scopes": [ "ghostScope" ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_UnknownShape));
}

// `identifierToken` round-trips: a valid token name resolves to a
// SchemaTokenId exposed on the SemanticConfig.
TEST(GrammarSchema, SemanticsIdentifierTokenRoundTrips) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Identifier", "Semi" ] } },
      "semantics": { "identifierToken": "Identifier" }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(r.has_value())
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
    auto const& sem = (*r)->semantics();
    EXPECT_TRUE(sem.identifierToken.valid());
    EXPECT_EQ(sem.identifierToken.v,
              (*r)->schemaTokens().find("Identifier").v);
}

// An `identifierToken` naming a token kind that doesn't exist →
// C_UnknownToken.
TEST(GrammarSchema, SemanticsIdentifierTokenUnknownReportsUnknownToken) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "identifierToken": "GhostToken" }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_UnknownToken));
}

// A `nameMatch: "lastIdentifier"` rule WITHOUT an `identifierToken` is a
// config gap → C_MissingField (the engine has no token kind to scan for).
TEST(GrammarSchema, SemanticsLastIdentifierWithoutIdentifierTokenReportsMissing) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": {
        "root": { "sequence": [ "Identifier", "Semi" ] },
        "qname": { "sequence": [ "Identifier" ] }
      },
      "semantics": {
        "references": [ { "rule": "qname", "nameMatch": "lastIdentifier" } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_MissingField));
}

// Unknown `nameMatch` mode string → C_InvalidSemantics.
TEST(GrammarSchema, SemanticsUnknownNameMatchReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": {
        "references": [ { "rule": "root", "nameMatch": "telepathy" } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// ── SE4-SE6 facet negative tests ─────────────────────────────────────────
// Each one mirrors the SE1-SE3 facet-test style: tight scenario, one
// diagnostic-code assertion, JSON-pointer path implicit in the surface.

// `assignments`: not-an-array → C_InvalidSemantics.
TEST(GrammarSchema, SemanticsAssignmentsNotArrayReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "assignments": { "rule": "root" } }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// `assignments[0]` not an object → C_InvalidSemantics.
TEST(GrammarSchema, SemanticsAssignmentsEntryNotObjectReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "assignments": [ "root" ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// `assignments[0].rule` missing → C_MissingField.
TEST(GrammarSchema, SemanticsAssignmentsMissingRule) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "assignments": [ { "lhs": 0, "rhs": 1 } ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_MissingField));
}

// `assignments[0].rule` references unknown shape → C_UnknownShape.
TEST(GrammarSchema, SemanticsAssignmentsUnknownRule) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "assignments": [
        { "rule": "ghostRule", "lhs": 0, "rhs": 1 }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_UnknownShape));
}

// `assignments[0].lhs` missing → C_MissingField.
TEST(GrammarSchema, SemanticsAssignmentsMissingLhs) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "assignments": [ { "rule": "root", "rhs": 1 } ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_MissingField));
}

// `assignments[0].rhs` non-integer → C_InvalidSemantics.
TEST(GrammarSchema, SemanticsAssignmentsRhsNotInteger) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "assignments": [
        { "rule": "root", "lhs": 0, "rhs": "two" }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// `assignments[0].lhs` negative → C_InvalidSemantics.
TEST(GrammarSchema, SemanticsAssignmentsLhsNegative) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "assignments": [
        { "rule": "root", "lhs": -1, "rhs": 0 }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// `assignments[0].operatorToken` unknown → C_UnknownToken.
TEST(GrammarSchema, SemanticsAssignmentsUnknownOperatorToken) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "assignments": [
        { "rule": "root", "lhs": 0, "rhs": 0, "operatorToken": "GhostOp" }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_UnknownToken));
}

// ── callRules ─────────────────────────────────────────────────────────────

TEST(GrammarSchema, SemanticsCallRulesNotArrayReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "callRules": { "rule": "root" } }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

TEST(GrammarSchema, SemanticsCallRulesMissingRule) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "callRules": [ { "callee": 0, "args": 1 } ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_MissingField));
}

TEST(GrammarSchema, SemanticsCallRulesUnknownRule) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "callRules": [
        { "rule": "ghostCall", "callee": 0, "args": 1 }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_UnknownShape));
}

TEST(GrammarSchema, SemanticsCallRulesMissingCallee) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "callRules": [ { "rule": "root", "args": 1 } ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_MissingField));
}

TEST(GrammarSchema, SemanticsCallRulesMissingArgs) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "callRules": [ { "rule": "root", "callee": 0 } ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_MissingField));
}

// `callRules[0].callee` non-integer → C_InvalidSemantics. Mirrors
// SemanticsAssignmentsRhsNotInteger for the call-rule facet so the
// readReqIndex validation path is pinned on BOTH semantics blocks.
TEST(GrammarSchema, SemanticsCallRulesCalleeNotInteger) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "callRules": [
        { "rule": "root", "callee": "zero", "args": 1 }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// `callRules[0].args` negative → C_InvalidSemantics. Mirrors
// SemanticsAssignmentsLhsNegative — the same range check has to gate
// every facet that reads a visible-child index.
TEST(GrammarSchema, SemanticsCallRulesArgsNegative) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "callRules": [
        { "rule": "root", "callee": 0, "args": -1 }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// `callRules[0].operatorToken` unknown → C_UnknownToken. Mirrors
// SemanticsAssignmentsUnknownOperatorToken — the optional operator-gate
// token must be a declared token kind for BOTH facets.
TEST(GrammarSchema, SemanticsCallRulesUnknownOperatorToken) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "callRules": [
        { "rule": "root", "callee": 0, "args": 0, "operatorToken": "GhostOp" }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_UnknownToken));
}

// ── builtinFunctions ──────────────────────────────────────────────────────

TEST(GrammarSchema, SemanticsBuiltinFunctionsMissingName) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "builtinFunctions": [ { "result": "I32" } ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_MissingField));
}

TEST(GrammarSchema, SemanticsBuiltinFunctionsMissingResult) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "builtinFunctions": [ { "name": "FOO" } ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_MissingField));
}

TEST(GrammarSchema, SemanticsBuiltinFunctionsUnknownResultCore) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "builtinFunctions": [
        { "name": "FOO", "result": "NotAKind" }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

TEST(GrammarSchema, SemanticsBuiltinFunctionsParamsNotArray) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "builtinFunctions": [
        { "name": "FOO", "result": "I32", "params": "I32" }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

TEST(GrammarSchema, SemanticsBuiltinFunctionsParamEntryNotString) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "builtinFunctions": [
        { "name": "FOO", "result": "I32", "params": [ 42 ] }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

TEST(GrammarSchema, SemanticsBuiltinFunctionsParamUnknownCore) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "builtinFunctions": [
        { "name": "FOO", "result": "I32", "params": [ "NotAKind" ] }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

TEST(GrammarSchema, SemanticsBuiltinFunctionsVariadicNotBool) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "builtinFunctions": [
        { "name": "FOO", "result": "I32", "variadic": "yes" }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// ── constMarker (on a declaration entry) ─────────────────────────────────

TEST(GrammarSchema, SemanticsConstMarkerUnknownTokenReportsUnknownToken) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "declarations": [
        { "rule": "root", "name": 0, "kind": "variable",
          "constMarker": "GhostConst" }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_UnknownToken));
}

TEST(GrammarSchema, SemanticsConstMarkerNotStringReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "declarations": [
        { "rule": "root", "name": 0, "kind": "variable",
          "constMarker": 42 }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// ── kindByChild ───────────────────────────────────────────────────────────

TEST(GrammarSchema, SemanticsKindByChildMissingChild) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "declarations": [
        { "rule": "root", "name": 0, "kind": "variable",
          "kindByChild": { "whenRule": "root" } }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_MissingField));
}

TEST(GrammarSchema, SemanticsKindByChildMissingWhenRule) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "declarations": [
        { "rule": "root", "name": 0, "kind": "variable",
          "kindByChild": { "child": 0 } }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_MissingField));
}

TEST(GrammarSchema, SemanticsKindByChildUnknownWhenRule) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "declarations": [
        { "rule": "root", "name": 0, "kind": "variable",
          "kindByChild": { "child": 0, "whenRule": "ghostRule" } }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_UnknownShape));
}

TEST(GrammarSchema, SemanticsKindByChildUnknownWhenKind) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "declarations": [
        { "rule": "root", "name": 0, "kind": "variable",
          "kindByChild": { "child": 0, "whenRule": "root",
                            "whenKind": "telepathic" } }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

TEST(GrammarSchema, SemanticsKindByChildParamsPathNotArray) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "declarations": [
        { "rule": "root", "name": 0, "kind": "variable",
          "kindByChild": { "child": 0, "whenRule": "root",
                            "paramsPath": 1 } }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

TEST(GrammarSchema, SemanticsKindByChildBodyPathEntryNotInteger) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "declarations": [
        { "rule": "root", "name": 0, "kind": "variable",
          "kindByChild": { "child": 0, "whenRule": "root",
                            "bodyPath": [ "one" ] } }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

TEST(GrammarSchema, SemanticsKindByChildBothChildAndChildPathConflicts) {
    // Specifying both `child` and `childPath` is a config bug → conflict.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "declarations": [
        { "rule": "root", "name": 0, "kind": "variable",
          "kindByChild": { "child": 0, "childPath": [0],
                            "whenRule": "root" } }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_ConflictingField));
}

// Happy-path round-trip: a schema declaring all SE4-SE6 facets loads
// and the SemanticConfig accessors return correctly-populated values.
// Mirrors `SemanticsBlockHappyPathRoundTrips` but for the new facets.
TEST(GrammarSchema, SemanticsSE4SE6FacetsHappyPathRoundTrips) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        " ": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
        "=": [{ "kind": "Eq" }],
        "(": [{ "kind": "LParen", "opensScope": "Paren" }],
        ")": [{ "kind": "RParen", "closesScope": true }],
        ";": [{ "kind": "Semi" }]
      },
      "keywords": [
        { "word": "lock", "kind": "Lock" }
      ],
      "shapes": {
        "root": { "sequence": [ { "repeat": "decl" } ] },
        "decl": { "sequence": [ "Identifier", "tail" ] },
        "tail": { "alt": [ "fnTail", "varTail" ] },
        "fnTail": { "sequence": [ "LParen", "RParen" ] },
        "varTail": { "sequence": [ "Semi" ] },
        "assign": { "sequence": [ "Identifier", "Eq", "Identifier", "Semi" ] },
        "call":   { "sequence": [ "Identifier", "LParen", "RParen" ] }
      },
      "semantics": {
        "identifierToken": "Identifier",
        "declarations": [
          { "rule": "decl", "name": 0, "kind": "variable",
            "constMarker": "Lock",
            "kindByChild": {
              "childPath": [1, 0],
              "whenRule": "fnTail",
              "whenKind": "function",
              "paramsPath": [],
              "bodyPath":   []
            } }
        ],
        "assignments": [
          { "rule": "assign", "operatorToken": "Eq", "lhs": 0, "rhs": 2 }
        ],
        "callRules": [
          { "rule": "call", "callee": 0, "args": 1, "operatorToken": "LParen" }
        ],
        "builtinFunctions": [
          { "name": "SUM", "params": [ "I32", "I32" ], "result": "I32" },
          { "name": "ANY", "result": "I32", "variadic": true }
        ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(r.has_value())
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
    auto const& sem = (*r)->semantics();
    ASSERT_EQ(sem.declarations.size(), 1u);
    EXPECT_EQ(sem.declarations[0].ruleName, "decl");
    ASSERT_TRUE(sem.declarations[0].constMarker.has_value());
    ASSERT_TRUE(sem.declarations[0].kindByChild.has_value());
    auto const& disc = *sem.declarations[0].kindByChild;
    ASSERT_EQ(disc.childPath.size(), 2u);
    EXPECT_EQ(disc.childPath[0], 1u);
    EXPECT_EQ(disc.childPath[1], 0u);
    EXPECT_EQ(disc.whenRuleName, "fnTail");
    EXPECT_EQ(disc.whenKind, DeclarationKind::Function);
    ASSERT_EQ(sem.assignments.size(), 1u);
    EXPECT_EQ(sem.assignments[0].ruleName, "assign");
    EXPECT_EQ(sem.assignments[0].lhsChild, 0u);
    EXPECT_EQ(sem.assignments[0].rhsChild, 2u);
    ASSERT_TRUE(sem.assignments[0].operatorToken.has_value());
    ASSERT_EQ(sem.callRules.size(), 1u);
    EXPECT_EQ(sem.callRules[0].ruleName, "call");
    EXPECT_EQ(sem.callRules[0].calleeChild, 0u);
    EXPECT_EQ(sem.callRules[0].argsChild, 1u);
    ASSERT_TRUE(sem.callRules[0].operatorToken.has_value());
    ASSERT_EQ(sem.builtinFunctions.size(), 2u);
    EXPECT_EQ(sem.builtinFunctions[0].name, "SUM");
    EXPECT_EQ(sem.builtinFunctions[0].paramCores.size(), 2u);
    EXPECT_EQ(sem.builtinFunctions[0].resultCore, TypeKind::I32);
    EXPECT_FALSE(sem.builtinFunctions[0].variadic);
    EXPECT_EQ(sem.builtinFunctions[1].name, "ANY");
    EXPECT_TRUE(sem.builtinFunctions[1].variadic);
}

// ── GAP A/C/D facet negative + happy-path tests ───────────────────────────

// `returnRules`: not-an-array → C_InvalidSemantics.
TEST(GrammarSchema, SemanticsReturnRulesNotArrayReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "returnRules": { "rule": "root" } }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// `returnRules[i].rule` naming no declared shape → C_UnknownShape.
TEST(GrammarSchema, SemanticsReturnRulesUnknownShape) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "returnRules": [ { "rule": "ghostRet" } ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_UnknownShape));
}

// `returnRules[i].value` of the wrong type → C_InvalidSemantics.
TEST(GrammarSchema, SemanticsReturnRulesValueNotInteger) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "returnRules": [ { "rule": "root", "value": "x" } ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// `loopRules[i]` naming no declared shape → C_UnknownShape.
TEST(GrammarSchema, SemanticsLoopRulesUnknownShape) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "loopRules": [ "ghostLoop" ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_UnknownShape));
}

// `loopControls[i].rule` naming no declared shape → C_UnknownShape.
TEST(GrammarSchema, SemanticsLoopControlsUnknownShape) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "loopControls": [ { "rule": "ghostBreak" } ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_UnknownShape));
}

// `loopControls[i].rule` missing → C_MissingField.
TEST(GrammarSchema, SemanticsLoopControlsMissingRule) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "loopControls": [ { } ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_MissingField));
}

// FIX 7: `returnRules[i]` missing `rule` → C_MissingField (the loader's
// required-field branch). Entry is an object but has no `rule` key.
TEST(GrammarSchema, SemanticsReturnRulesMissingRuleReportsMissingField) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "returnRules": [ { "value": 0 } ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_MissingField));
}

// FIX 7: `returnRules[i].value` out of the int32 range → C_InvalidSemantics
// (the loader's range branch; distinct from the wrong-TYPE branch tested by
// SemanticsReturnRulesValueNotInteger). 9999999999 > INT32_MAX.
TEST(GrammarSchema, SemanticsReturnRulesValueOutOfRangeReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "returnRules": [ { "rule": "root", "value": 9999999999 } ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// FIX 7: a `loopRules` entry that is not a string → C_InvalidSemantics (the
// per-entry type guard; distinct from the not-an-ARRAY guard which is the
// scalar-shaped block). The array is present but an entry is an object.
TEST(GrammarSchema, SemanticsLoopRulesEntryNotStringReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "loopRules": [ { "rule": "root" } ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// FIX 7: a `loopControls` entry that is not an object → C_InvalidSemantics
// (the per-entry object guard; distinct from the not-an-ARRAY guard). The
// array is present but an entry is a bare string.
TEST(GrammarSchema, SemanticsLoopControlsEntryNotObjectReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "loopControls": [ "root" ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// FIX 7: a `loopControls` that is not an array → C_InvalidSemantics (the
// block-level type guard, the array analogue of the per-entry guards above).
TEST(GrammarSchema, SemanticsLoopControlsNotArrayReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "loopControls": { "rule": "root" } }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// `bracketIdentifierToken` naming no declared token → C_UnknownToken.
TEST(GrammarSchema, SemanticsBracketIdentifierTokenUnknownReportsUnknownToken) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "bracketIdentifierToken": "GhostBracket" }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_UnknownToken));
}

// `bracketIdentifierToken` of the wrong JSON type → C_InvalidSemantics.
TEST(GrammarSchema, SemanticsBracketIdentifierTokenNotStringReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "bracketIdentifierToken": 7 }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// Happy-path: returnRules / loopRules / loopControls / bracketIdentifierToken
// round-trip onto SemanticConfig with the expected shapes.
TEST(GrammarSchema, SemanticsGapAcdFacetsHappyPathRoundTrips) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        " ": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
        ";": [{ "kind": "Semi" }],
        "[": [{ "kind": "Brk" }]
      },
      "keywords": [
        { "word": "ret",  "kind": "RetKw"  },
        { "word": "spin", "kind": "SpinKw" },
        { "word": "halt", "kind": "HaltKw" }
      ],
      "shapes": {
        "root":     { "sequence": [ { "repeat": "stmt" } ] },
        "stmt":     { "alt": [ "retStmt", "spinStmt", "haltStmt" ] },
        "retStmt":  { "sequence": [ "RetKw", { "optional": "Identifier" }, "Semi" ] },
        "spinStmt": { "sequence": [ "SpinKw", "Semi" ] },
        "haltStmt": { "sequence": [ "HaltKw", "Semi" ] }
      },
      "semantics": {
        "identifierToken": "Identifier",
        "bracketIdentifierToken": "Brk",
        "returnRules":  [ { "rule": "retStmt", "value": 1 }, { "rule": "haltStmt" } ],
        "loopRules":    [ "spinStmt" ],
        "loopControls": [ { "rule": "haltStmt" } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(r.has_value())
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
    auto const& sem = (*r)->semantics();
    ASSERT_EQ(sem.returnRules.size(), 2u);
    EXPECT_EQ(sem.returnRules[0].ruleName, "retStmt");
    ASSERT_TRUE(sem.returnRules[0].valueChild.has_value());
    EXPECT_EQ(*sem.returnRules[0].valueChild, 1u);
    EXPECT_EQ(sem.returnRules[1].ruleName, "haltStmt");
    EXPECT_FALSE(sem.returnRules[1].valueChild.has_value()) << "absent value ⇒ bare-return shape";
    ASSERT_EQ(sem.loopRules.size(), 1u);
    EXPECT_EQ(sem.loopRules[0].ruleName, "spinStmt");
    ASSERT_EQ(sem.loopControls.size(), 1u);
    EXPECT_EQ(sem.loopControls[0].ruleName, "haltStmt");
    ASSERT_TRUE(sem.bracketIdentifierToken.has_value());
    EXPECT_TRUE(sem.bracketIdentifierToken->valid());
    EXPECT_EQ(sem.bracketIdentifierToken->v, (*r)->schemaTokens().find("Brk").v);
}
