#include "core/types/grammar_schema.hpp"
// The registered artifact-profile TABLE under test below (name + composition
// verb), plus the shared closed-vocabulary well-formedness guard it is checked
// with — the SAME `isWellFormedKeyVocabulary` every config loader uses, not a
// third copy of the loop (`core/types/config_key_vocabulary.hpp`, TF-C74).
#include "core/types/artifact_profile.hpp"
#include "core/types/config_key_vocabulary.hpp"
// The repo's SHA-256 — the independent oracle the retained `contentDigest()`
// is pinned against (the tests hex-render it themselves; see `hexOracle`).
#include "core/crypto/sha256.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
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

// c31 D-CSUBSET-LABEL-BUDGET-CLIFF: the `commitAfterPrefix` PEG-cut facet must be
// a boolean. A non-boolean is a config typo and is rejected LOUD at load
// (C_UnknownShape "must be a boolean") — never silently ignored. (The sibling
// mutual-exclusion with `commitRequiresTypeName` is also fail-loud in the loader;
// it needs a valid type-position config to exercise and is left to a follow-up.)
TEST(GrammarSchema, CommitAfterPrefixRejectsNonBoolean) {
    constexpr std::string_view bad = R"({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "shapes": {
        "root": { "sequence": ["Identifier"], "commitAfterPrefix": 5 }
      }
    })";
    auto result = GrammarSchema::loadFromText(bad);
    ASSERT_FALSE(result.has_value());
    auto const& diags = result.error();
    EXPECT_TRUE(std::ranges::any_of(diags, [](auto const& d) {
        return d.code == DiagnosticCode::C_UnknownShape;
    }));
}

// ─── loadShipped + the on-disk toy.lang.json ─────────────────────────────

TEST(GrammarSchema, LoadShippedToy) {
    // The ctest cwd is the build dir. loadShipped checks two roots:
    //   <cwd>/src/dss-config/sources/toy.lang.json
    //   <cwd>/../src/dss-config/sources/toy.lang.json
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

// ⓘ "EVERY SHIPPED CONFIG STILL LOADS" — the other direction of every closed
// key set in this loader, and the direction that breaks every language at once
// if a set omits a REAL key — is ALREADY PINNED, by
// `LexerModesLoader.EveryShippedConfigLoadsWithoutWarnings` in
// `tests/core/test_lexer_modes.cpp`. It is stronger than a plain load check
// (zero WARNINGS, not merely no error) and it names the documents EXPLICITLY,
// with a stated reason: "a directory walk that found zero files would pass
// vacuously". A second, globbing copy here would contradict that decision and
// become the drifting duplicate. Adding a new `*.lang.json` means adding its
// name to that list by hand — deliberately.


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
    // FC4 c1: funcDefTail/funcParams/varDeclHead were dissolved by the
    // C11 specifier/declarator split — the declarator core rules + the
    // statement-ambiguity rules replace them.
    for (std::string_view rule : {"root", "topLevel", "topLevelDecl",
                                  "topLevelDeclTail", "topLevelHead",
                                  // c23 D-CSUBSET-EXTERN-MULTI-DECLARATOR: externDecl
                                  // became the extern twin of topLevelDecl (star-free
                                  // head + initDeclaratorList), retiring the single-
                                  // declarator `varDeclTail`/`typeBase`/`typeRef` +
                                  // `externTail`/`externFuncTail` (pinned ABSENT below).
                                  "typeRefAllowingStruct", "typeBaseAllowingStruct",
                                  "declarator", "directDeclarator",
                                  "parenDeclarator", "pointerLayer",
                                  "fnSuffix", "initDeclarator",
                                  "initDeclaratorList", "declHead",
                                  "kwDeclHead", "identDeclHead",
                                  "identVarDecl", "declOrExprStmt",
                                  "forDecl", "forIdentDecl", "forInitAmbig",
                                  "typedefHead",
                                  // c25 D-CSUBSET-UNIFIED-COMPOSITE-SPECIFIER:
                                  // the unified struct/union/enum specifier rules
                                  // (REPLACED the retired *SpecifierBody rules) +
                                  // their factored-out member-body wrapper rules.
                                  "structSpec", "structBody",
                                  "unionSpec", "unionBody",
                                  "enumSpec", "enumBody",
                                  "structTypeRef", "unionTypeRef", "enumTypeRef",
                                  "stdAttr", "varDecl",
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

    // c25: the former specifier-body rules were RETIRED (folded into the
    // unified XxxSpec). c23 D-CSUBSET-EXTERN-MULTI-DECLARATOR: the single-
    // declarator extern spine rules were RETIRED (externDecl routes through
    // typeRefAllowingStruct + initDeclaratorList). Pin their ABSENCE so a stray
    // re-introduction (or an incomplete unification) is visible at load.
    for (std::string_view retired : {"structSpecifierBody",
                                     "unionSpecifierBody",
                                     "enumSpecifierBody",
                                     "typeRef", "typeBase",
                                     "externTail", "externFuncTail",
                                     "varDeclTail"}) {
        EXPECT_FALSE(schema.rules().find(retired).valid())
            << "retired rule must not resolve: " << retired;
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
// auto-interned in schemas that don't use any `expr` shape; a regression that
// unconditionally interns the wrappers would inflate every shipped grammar's
// RuleInterner and silently change RuleId numbering. Pinned against a synthetic
// non-expr grammar (the shipped toy grammar gained an `expr` shape at HR9).
TEST(GrammarSchema, WrapperRulesAbsentInNonExprSchema) {
    constexpr char const* kNonExprSchema = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "NonExpr", "version": "0.0.1", "fileExtensions": [".ne"] },
      "tokens": {
        " ": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
        ";": [{ "kind": "Semi" }]
      },
      "keywords": [ { "word": "go", "kind": "GoKw" } ],
      "shapes": {
        "root": { "sequence": [ { "repeat": "stmt" } ] },
        "stmt": { "sequence": [ "GoKw", "Identifier", "Semi" ] }
      }
    })JSON";
    auto result = GrammarSchema::loadFromText(kNonExprSchema);
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

// c23 D-CSUBSET-EXTERN-MULTI-DECLARATOR: externDecl's head is now the star-free
// `typeRefAllowingStruct` (the retired `typeRef` folded into it). It admits
// `const int const x` (double-const: leading const via the `{repeat headQualifier}`
// prefix, trailing const via the `{opt ConstKeyword}` east-const) AND `volatile`
// in the head — a strict superset of the retired typeRef's const-only leading
// qualifier. Real C allows double-const only with intervening type modifiers; the
// c-subset is deliberately more permissive. Pinned so a future PR doesn't tighten
// this without intent.
TEST(GrammarSchema, CSubsetExternHeadAllowsDoubleConst) {
    auto result = GrammarSchema::loadShipped("c-subset");
    if (!result.has_value()) {
        FAIL() << "loadShipped c-subset failed: " << result.error()[0].message;
    }
    EXPECT_TRUE((*result)->rules().find("typeRefAllowingStruct").valid());
    EXPECT_FALSE((*result)->rules().find("typeRef").valid())
        << "the retired single-declarator typeRef must not resolve";
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

// Every ERROR-severity diagnostic, one per line. ★ Use this instead of
// `diags[0].message` when reporting why a load failed: the list is in emission
// order and its FIRST entry is routinely a WARNING (the shipped c-subset opens
// with a `defaultToken`-without-`tokens` mode warning), so `[0]` regularly
// names something entirely unrelated to the failure and sends the reader
// chasing it.
[[nodiscard]] std::string errorDiags(std::vector<ConfigDiagnostic> const& diags) {
    std::string out;
    for (auto const& d : diags) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        out += std::format("\n  [{}] {}", d.path, d.message);
    }
    return out.empty() ? std::string{"<no error-severity diagnostics>"} : out;
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

// ─── FC1 cycle 2 (2026-06-10): per-prefix float-block loader arms ──────────

namespace {
// Wrap a numberStyle JSON fragment in a minimal loadable schema. Every
// prefix-float loader test differs only in the numberStyle body.
[[nodiscard]] std::string wrapNumberStyle(std::string_view numberStyle) {
    return std::format(R"JSON({{
      "dssSchemaVersion": 4,
      "language": {{ "name": "PfxFloat", "version": "0.1.0" }},
      "tokens": {{ ";": [{{ "kind": "Semi" }}] }},
      "numberStyle": {},
      "shapes": {{
        "root": {{ "sequence": ["IntLiteral", "Semi"] }}
      }}
    }})JSON", numberStyle);
}
}  // namespace

TEST(GrammarSchema, PrefixFloatValidBlockLoads) {
    auto result = GrammarSchema::loadFromText(wrapNumberStyle(R"JSON({
        "decimal": true,
        "integerPrefixes": [
          { "prefix": "0x", "radix": 16, "digits": "0-9a-fA-F",
            "float": { "exponent": { "letters": ["p","P"], "signOptional": true },
                       "exponentDigits": "0-9" } }
        ],
        "fractionPoint": ".",
        "emitKind": { "integer": "IntLiteral", "float": "FloatLiteral" }
      })JSON"));
    ASSERT_TRUE(result.has_value());
    auto const* ns = (*result)->numberStyle();
    ASSERT_NE(ns, nullptr);
    ASSERT_EQ(ns->integerPrefixes.size(), 1u);
    ASSERT_TRUE(ns->integerPrefixes[0].floating.has_value());
    auto const& pf = *ns->integerPrefixes[0].floating;
    ASSERT_EQ(pf.exponentLetters.size(), 2u);
    EXPECT_EQ(pf.exponentLetters[0], 'p');
    EXPECT_EQ(pf.exponentLetters[1], 'P');
    EXPECT_TRUE(pf.exponentSignOptional);
    EXPECT_EQ(pf.exponentDigits, "0-9");
}

TEST(GrammarSchema, PrefixFloatUnknownKeyRejected) {
    // Typo discriminator: `exponentDigitz` must reject, never be
    // silently ignored (the author would believe the class is set
    // while the engine used the default).
    auto result = GrammarSchema::loadFromText(wrapNumberStyle(R"JSON({
        "decimal": true,
        "integerPrefixes": [
          { "prefix": "0x", "radix": 16, "digits": "0-9a-fA-F",
            "float": { "exponent": { "letters": ["p"] },
                       "exponentDigitz": "0-9" } }
        ],
        "fractionPoint": ".",
        "emitKind": { "integer": "IntLiteral", "float": "FloatLiteral" }
      })JSON"));
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_InvalidNumberStyle));
}

// ══ D-ASM-DIALECT-IDENTIFIER-CONTINUATION-NOT-CONFIGURABLE (2026-08-13) ═════
//
// ★★★ THE `identifierClass` BLOCK. Before it, the identifier character set was
// a hardcoded `constexpr` pair in `tokenizer.cpp`, so a language whose names
// legitimately contain a character outside `[A-Za-z0-9_]` had exactly one way
// to spell them: declare each WHOLE NAME as a lexeme in `tokens`. The arm64 gas
// dialect paid that twelve times over (`b.eq`…`b.cs`, each declared as a token
// AND as an instruction row, the two required to agree).
namespace {
[[nodiscard]] std::string wrapIdentifierClass(std::string_view body) {
    return std::format(R"JSON({{
      "dssSchemaVersion": 4,
      "language": {{ "name": "IdClass", "version": "0.1.0" }},
      "tokens": {{ ";": [{{ "kind": "Semi" }}] }},
      "identifierClass": {},
      "shapes": {{
        "root": {{ "sequence": ["Identifier", "Semi"] }}
      }}
    }})JSON", body);
}
}  // namespace

TEST(GrammarSchema, IdentifierClassExtraContinueLoads) {
    auto result =
        GrammarSchema::loadFromText(wrapIdentifierClass(R"({ "extraContinue": "." })"));
    ASSERT_TRUE(result.has_value()) << errorDiags(result.error());
    EXPECT_EQ((*result)->identifierClass().extraContinue, ".");
    EXPECT_TRUE((*result)->identifierClass().continuesIdentifier('.'));
    // ★ ADDITIVE, NEVER SUBTRACTIVE: the universal set still holds.
    EXPECT_TRUE((*result)->identifierClass().continuesIdentifier('_'));
    EXPECT_TRUE((*result)->identifierClass().continuesIdentifier('7'));
    EXPECT_FALSE((*result)->identifierClass().continuesIdentifier(','));
}

// ★ A LANGUAGE THAT DECLARES NOTHING GETS THE UNIVERSAL RULE — and the accessor
// answers rather than returning a null the hot path must branch on.
TEST(GrammarSchema, IdentifierClassAbsentMeansTheUniversalRule) {
    auto result = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(result.has_value()) << errorDiags(result.error());
    EXPECT_TRUE((*result)->identifierClass().extraContinue.empty());
    EXPECT_FALSE((*result)->identifierClass().continuesIdentifier('.'))
        << "a C document must never read `a.b` as one identifier";
    EXPECT_TRUE((*result)->identifierClass().continuesIdentifier('z'));
}

// ★★ `extraStart` IS REFUSED BY NAME, NOT MERELY UNKNOWN. It is the key a
// reader reaches for, and an "unknown key" message alone reads as an oversight
// that the next implementer should fix. A leading character that also
// introduces a directive or an operator is owned by that TOKEN; two mechanisms
// for one byte cannot agree about which construct it opens.
TEST(GrammarSchema, IdentifierClassStartKeyIsRefusedWithItsReason) {
    auto result = GrammarSchema::loadFromText(wrapIdentifierClass(
        R"({ "extraContinue": ".", "extraStart": "." })"));
    ASSERT_FALSE(result.has_value());
    auto const msg = errorDiags(result.error());
    EXPECT_NE(msg.find("extraStart"), std::string::npos) << msg;
    EXPECT_NE(msg.find("may CONTINUE an identifier and may never START one"),
              std::string::npos)
        << "the refusal must say WHY, or it reads as an unimplemented key: "
        << msg;
}

// ★ A CHARACTER THAT ALREADY CONTINUES AN IDENTIFIER IS REFUSED. Declaring it
// changes nothing, so accepting it would be a key that reads as a capability
// and delivers none — and the reader would go looking for the bug elsewhere.
TEST(GrammarSchema, IdentifierClassRedundantCharacterIsRefused) {
    for (auto const* cls : {R"({ "extraContinue": "_" })",
                            R"({ "extraContinue": "0-9" })",
                            R"({ "extraContinue": ".x" })"}) {
        auto result = GrammarSchema::loadFromText(wrapIdentifierClass(cls));
        ASSERT_FALSE(result.has_value()) << cls;
        EXPECT_NE(errorDiags(result.error())
                      .find("already continues an identifier"),
                  std::string::npos)
            << cls << ": " << errorDiags(result.error());
    }
}

// ★★ WHITESPACE AND CONTROL BYTES ARE REFUSED BECAUSE THEY DO NOT WIDEN THE
// CLASS, THEY DISSOLVE TOKENISATION. A space in the class makes `mov x0, x1`
// one identifier; a newline makes a line-oriented language's whole file one.
// Both are WRONG PARSES rather than parse errors — the failure mode this facet's
// own postmortem is about.
TEST(GrammarSchema, IdentifierClassWhitespaceIsRefused) {
    for (auto const* cls : {R"({ "extraContinue": " " })",
                            R"({ "extraContinue": "\n" })",
                            R"({ "extraContinue": "\t" })"}) {
        auto result = GrammarSchema::loadFromText(wrapIdentifierClass(cls));
        ASSERT_FALSE(result.has_value()) << cls;
        EXPECT_NE(errorDiags(result.error())
                      .find("is whitespace or a control character"),
                  std::string::npos)
            << cls << ": " << errorDiags(result.error());
    }
}

// ★ AN EMPTY BLOCK IS REFUSED: a block declaring nothing silently does nothing,
// and the way to say "the universal rule" is to omit the block.
TEST(GrammarSchema, IdentifierClassEmptyBlockIsRefused) {
    auto result = GrammarSchema::loadFromText(wrapIdentifierClass("{}"));
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_MissingField));
}

// ══ D-CONFIG-NUMBERSTYLE-KEYS-UNCHECKED (2026-08-13) ═══════════════════════
//
// ★★★ THE `numberStyle` BLOCK REJECTED UNKNOWN KEYS AT ONE LEVEL AND ACCEPTED
// THEM AT THREE. `numberStyle.exponent` and each prefix's `float` had a typo
// discriminator (the two tests above pin it); `numberStyle` ITSELF, each
// `integerPrefixes[]` ENTRY and `emitKind` had none — every field was read with
// a bare `contains()` probe and nothing looked at the keys left over. So
// `"fracionPoint"`, `"digitSeperator"`, `"trailingFractions"`, a prefix's
// `"radx"` and `emitKind`'s `"floating"` all LOADED CLEAN and silently changed
// numeric lexing: the knob-that-lies archetype, and a direct contradiction of
// the discipline `kDocumentKeys` states in the same loader.
//
// ⓘ EACH TEST NAMES A DIFFERENT LEVEL, deliberately — one test over one level
// would have gone green while the other two stayed open, which is exactly how
// this gap survived (the covered level had tests).

TEST(GrammarSchema, NumberStyleDirectKeyTypoIsRejected) {
    auto result = GrammarSchema::loadFromText(wrapNumberStyle(R"JSON({
        "decimal": true,
        "fracionPoint": ".",
        "emitKind": { "integer": "IntLiteral" }
      })JSON"));
    ASSERT_FALSE(result.has_value())
        << "a misspelled 'fractionPoint' used to load clean and silently leave "
           "the language with no fraction point at all";
    EXPECT_TRUE(hasDiagCode(result.error(),
                            DiagnosticCode::C_InvalidNumberStyle));
    EXPECT_NE(errorDiags(result.error()).find("fracionPoint"),
              std::string::npos)
        << "the diagnostic must name the key that was actually written: "
        << errorDiags(result.error());
}

TEST(GrammarSchema, NumberStyleIntegerPrefixEntryKeyTypoIsRejected) {
    auto result = GrammarSchema::loadFromText(wrapNumberStyle(R"JSON({
        "decimal": true,
        "integerPrefixes": [
          { "prefix": "0x", "radx": 16, "digits": "0-9a-fA-F" }
        ],
        "emitKind": { "integer": "IntLiteral" }
      })JSON"));
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(),
                            DiagnosticCode::C_InvalidNumberStyle));
    EXPECT_NE(errorDiags(result.error()).find("radx"), std::string::npos)
        << "without the entry-level discriminator this surfaced as \"'radix' "
           "must be an integer\" — true, but pointing at a line nobody wrote: "
        << errorDiags(result.error());
}

TEST(GrammarSchema, NumberStyleEmitKindKeyTypoIsRejected) {
    // ★ THE ASYMMETRIC ONE. `integer` is REQUIRED, so misspelling it already
    // surfaced as C_MissingField; `float` is required only when a
    // float-producing facet is declared — so `"floating"` was GENUINELY SILENT
    // for every language without one, and would have become a mystery for the
    // first language that grew one.
    auto result = GrammarSchema::loadFromText(wrapNumberStyle(R"JSON({
        "decimal": true,
        "emitKind": { "integer": "IntLiteral", "floating": "FloatLiteral" }
      })JSON"));
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(),
                            DiagnosticCode::C_InvalidNumberStyle));
    EXPECT_NE(errorDiags(result.error()).find("floating"), std::string::npos)
        << errorDiags(result.error());
}

// ★★ THE OTHER DIRECTION OF A NEW CLOSED KEY SET, AND THE ONE THAT BREAKS
// EVERYTHING AT ONCE IF IT IS WRONG: a set that omits a REAL key rejects every
// document that uses it. Every direct `numberStyle` key a shipped config
// writes must still load — asserted here as one document naming ALL TEN, so a
// key dropped from `kNumberStyleKeys` fails this test rather than a random
// language's.
TEST(GrammarSchema, NumberStyleAcceptsEveryDeclaredKey) {
    auto result = GrammarSchema::loadFromText(wrapNumberStyle(R"JSON({
        "decimal": true,
        "integerPrefixes": [
          { "prefix": "0x", "radix": 16, "digits": "0-9a-fA-F",
            "float": { "exponent": { "letters": ["p"] }, "exponentDigits": "0-9" } }
        ],
        "exponent": { "letters": ["e","E"], "signOptional": true },
        "fractionPoint": ".",
        "digitSeparator": "'",
        "trailingFraction": true,
        "leadingFraction": true,
        "integerSuffixes": ["u","U"],
        "floatSuffixes": ["f","F"],
        "emitKind": { "integer": "IntLiteral", "float": "FloatLiteral" }
      })JSON"));
    ASSERT_TRUE(result.has_value()) << errorDiags(result.error());
}

TEST(GrammarSchema, PrefixFloatMissingExponentRejected) {
    // A prefix-float without an exponent grammar can never complete
    // (C23 mandates the exponent) — a silently-dead config, rejected.
    auto result = GrammarSchema::loadFromText(wrapNumberStyle(R"JSON({
        "decimal": true,
        "integerPrefixes": [
          { "prefix": "0x", "radix": 16, "digits": "0-9a-fA-F",
            "float": { "exponentDigits": "0-9" } }
        ],
        "fractionPoint": ".",
        "emitKind": { "integer": "IntLiteral", "float": "FloatLiteral" }
      })JSON"));
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_MissingField));
}

TEST(GrammarSchema, PrefixFloatExponentLetterInsideDigitClassRejected) {
    // `e` IS a hex mantissa digit — the digit run would always consume
    // it, making the float branch silently unreachable. Load-reject.
    auto result = GrammarSchema::loadFromText(wrapNumberStyle(R"JSON({
        "decimal": true,
        "integerPrefixes": [
          { "prefix": "0x", "radix": 16, "digits": "0-9a-fA-F",
            "float": { "exponent": { "letters": ["e"] } } }
        ],
        "fractionPoint": ".",
        "emitKind": { "integer": "IntLiteral", "float": "FloatLiteral" }
      })JSON"));
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_InvalidNumberStyle));
}

TEST(GrammarSchema, PrefixFloatRequiresFloatEmitKind) {
    // A prefix-float is a float-producing facet — emitKind.float
    // becomes required (the same invariant as exponent/fractionPoint).
    auto result = GrammarSchema::loadFromText(wrapNumberStyle(R"JSON({
        "decimal": true,
        "integerPrefixes": [
          { "prefix": "$", "radix": 16, "digits": "0-9a-fA-F",
            "float": { "exponent": { "letters": ["^"] } } }
        ],
        "emitKind": { "integer": "IntLiteral" }
      })JSON"));
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_MissingField));
}

TEST(GrammarSchema, FractionFlagsRequireFractionPoint) {
    // `trailingFraction`/`leadingFraction` without a declared
    // `fractionPoint` is internally inconsistent — rejected.
    auto result = GrammarSchema::loadFromText(wrapNumberStyle(R"JSON({
        "decimal": true,
        "trailingFraction": true,
        "emitKind": { "integer": "IntLiteral", "float": "FloatLiteral" }
      })JSON"));
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_InvalidNumberStyle));
}

TEST(GrammarSchema, ExponentObjectUnknownKeyRejectedAtBothSites) {
    // Audit fold (FC1c2): the shared exponent parser's unknown-key
    // reject needs a red lever at BOTH call sites — a `signOptionl`
    // typo must fail loud whether it sits in the TOP-LEVEL exponent
    // or a prefix's float.exponent (silently defaulting signOptional
    // is the knob-that-lies).
    auto top = GrammarSchema::loadFromText(wrapNumberStyle(R"JSON({
        "decimal": true,
        "exponent": { "letters": ["e"], "signOptionl": true },
        "emitKind": { "integer": "IntLiteral", "float": "FloatLiteral" }
      })JSON"));
    ASSERT_FALSE(top.has_value());
    EXPECT_TRUE(hasDiagCode(top.error(), DiagnosticCode::C_InvalidNumberStyle));

    auto pfx = GrammarSchema::loadFromText(wrapNumberStyle(R"JSON({
        "decimal": true,
        "integerPrefixes": [
          { "prefix": "0x", "radix": 16, "digits": "0-9a-fA-F",
            "float": { "exponent": { "letters": ["p"], "signOptionl": true } } }
        ],
        "fractionPoint": ".",
        "emitKind": { "integer": "IntLiteral", "float": "FloatLiteral" }
      })JSON"));
    ASSERT_FALSE(pfx.has_value());
    EXPECT_TRUE(hasDiagCode(pfx.error(), DiagnosticCode::C_InvalidNumberStyle));
}

TEST(GrammarSchema, PrefixFloatEmptyExponentDigitsRejected) {
    auto result = GrammarSchema::loadFromText(wrapNumberStyle(R"JSON({
        "decimal": true,
        "integerPrefixes": [
          { "prefix": "0x", "radix": 16, "digits": "0-9a-fA-F",
            "float": { "exponent": { "letters": ["p"] },
                       "exponentDigits": "" } }
        ],
        "fractionPoint": ".",
        "emitKind": { "integer": "IntLiteral", "float": "FloatLiteral" }
      })JSON"));
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

// ── `semantics` closed key vocabulary (typo discriminator) ───────────────
//
// A misspelled sub-block name must FAIL THE LOAD. Silently accepting it
// would leave the whole facet unhonored while the config looks correct —
// the knob-that-lies class. Mirrors the `declarators` / `gatedMarkers`
// closed-key pins.

namespace {
// The shared skeleton: a minimal loadable schema whose `semantics` object
// is spliced at %SEM%. Every positive variant below MUST load clean, so the
// skeleton itself carries no semantic requirements of its own.
[[nodiscard]] std::string semanticsSchemaWith(std::string_view semBody) {
    std::string cfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "ClosedKeys", "version": "0.1.0" },
      "tokens": {
        " ": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
        "[": [{ "kind": "BracketOpen" }],
        "]": [{ "kind": "BracketClose" }],
        ";": [{ "kind": "Semi" }]
      },
      "shapes": {
        "root":     { "sequence": [ { "repeat": "stmt" } ] },
        "stmt":     { "alt": [ "attrStmt", "bare" ] },
        "attrStmt": { "sequence": [ "attrSpec", "Semi" ] },
        "attrSpec": { "sequence": [ "BracketOpen", "stdAttr", "BracketClose" ] },
        "stdAttr":  { "sequence": [ "Identifier" ] },
        "bare":     { "sequence": [ "Semi" ] }
      },
      "semantics": %SEM%
    })JSON";
    auto const pos = cfg.find("%SEM%");
    cfg.replace(pos, 5, semBody);
    return cfg;
}

// A well-formed `attributeSemantics` block, spliced at %EXTRA% so each test
// can add exactly one extra key.
[[nodiscard]] std::string attributeSemanticsSchemaWith(std::string_view extra) {
    std::string body = R"JSON({
        "attributeSemantics": {
          %EXTRA%
          "attrSpecRule":      "attrSpec",
          "stdAttrRule":       "stdAttr",
          "bareStatementRule": "bare",
          "effects": [ { "names": ["maybe_unused"], "appliesTo": ["variable"],
                         "effect": "suppressUnused" } ]
        }
      })JSON";
    auto const pos = body.find("%EXTRA%");
    body.replace(pos, 7, extra);
    return semanticsSchemaWith(body);
}
} // namespace

// Baseline: the skeleton with a complete `attributeSemantics` and no extra
// key loads clean — without this the negative pins below could pass for the
// wrong reason (RED if the allowed-key list is over-narrow).
TEST(GrammarSchema, SemanticsClosedKeySkeletonLoadsCleanly) {
    auto r = GrammarSchema::loadFromText(attributeSemanticsSchemaWith(""));
    ASSERT_TRUE(r.has_value())
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
    EXPECT_EQ((*r)->semantics().attrSpecRuleName, "attrSpec");
    ASSERT_EQ((*r)->semantics().attributeEffects.size(), 1u);
}

// An unknown key directly inside `semantics` → C_InvalidSemantics. `alinged`
// is the real-world shape of the bug: the `alignas` facet would silently
// never be configured.
TEST(GrammarSchema, SemanticsUnknownKeyReportsInvalid) {
    auto const cfg = semanticsSchemaWith(R"({
        "scopes": [ "bare" ],
        "alinged": { "keyword": "AlignasKw" }
      })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "a typo'd 'semantics' sub-block must fail the load, not load clean";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// `$`-prefixed keys are the documentation convention and stay EXEMPT — the
// shipped configs carry dozens of `$…Comment` keys here.
TEST(GrammarSchema, SemanticsDollarPrefixedKeyIsExempt) {
    auto const cfg = semanticsSchemaWith(R"({
        "$scopesComment": "block scopes live on the bare statement",
        "scopes": [ "bare" ]
      })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value())
        << "a '$'-prefixed documentation key must not trip the typo "
           "discriminator: "
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
    EXPECT_EQ((*r)->semantics().scopes.size(), 1u);
}

// The EMPTY key must be rejected — the runtime symptom of an under-filled
// closed-key array, and the observable half of the compile-time
// `isWellFormedKeyVocabulary` guard.
//
// ★ `std::array<std::string_view, N>` with FEWER than N initializers is legal
// C++: it value-initializes the tail, so every phantom element is the EMPTY
// string_view and the typo discriminator silently starts whitelisting a key of
// `""` — while quietly shrinking the vocabulary it actually enforces. MEASURED
// on `kSemanticsKeys`: bumping its declared size 56→57 and touching nothing
// else compiled clean, made a `semantics` key of `""` load without a
// diagnostic, and left the whole suite green. The compile-time assert is the
// real guard (and now covers every closed-key table in the loader, not the one
// it was written for); this pins the behavior it protects, so the two must
// break together.
TEST(GrammarSchema, SemanticsEmptyKeyIsRejected) {
    auto const cfg = semanticsSchemaWith(R"({
        "scopes": [ "bare" ],
        "": "an under-filled key array would whitelist this"
      })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "the EMPTY key must never be accepted by a closed-key vocabulary — "
           "accepting it is the signature of an array whose declared size "
           "exceeds its initializer count";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// An unknown key inside `attributeSemantics` → C_InvalidSemantics. A typo'd
// `stdAtrRule` would otherwise leave the block half-wired (no std-attribute
// surface at all) while the load reports success.
TEST(GrammarSchema, SemanticsAttributeSemanticsUnknownKeyReportsInvalid) {
    auto const cfg = attributeSemanticsSchemaWith(R"("stdAtrRule": "stdAttr",)");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "a typo'd 'attributeSemantics' key must fail the load";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// The `$` exemption applies inside `attributeSemantics` too.
TEST(GrammarSchema, SemanticsAttributeSemanticsDollarPrefixedKeyIsExempt) {
    auto const cfg = attributeSemanticsSchemaWith(
        R"("$effectsComment": "maybe_unused silences the unused warning",)");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value())
        << "a '$'-prefixed documentation key must not trip the typo "
           "discriminator: "
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
    ASSERT_EQ((*r)->semantics().attributeEffects.size(), 1u);
    EXPECT_EQ((*r)->semantics().attributeEffects[0].effect,
              AttributeEffect::SuppressUnused);
}

// ── TF-C73 (D-CSUBSET-GNU-ATTRIBUTE-LEADING-ARG-SOUP): the OPTIONAL
//    `attributeSemantics.attributeArgRule` key ──────────────────────────────
//
// This key names the attribute ARGUMENT-group shape so `linkageFrom` can flag
// argument tokens out of its specifier key lookup. It closes half (a) of
// D-CONFIG-ATTRIBUTE-ARG-RULE-DOCUMENTED-BUT-UNIMPLEMENTED: two shipped
// `$comment`s asserted this mechanism in the present tense for a full cycle
// while `attributeArgRule` resolved to NOTHING — no loader key, no field, no
// consumer, no test. These three pins are what make the name greppable to an
// implementation from now on.

// PRESENT + VALID: the key loads and reaches `SemanticConfig`.
TEST(GrammarSchema, AttributeArgRuleLoadsWhenPresent) {
    auto const cfg = attributeSemanticsSchemaWith(
        R"("attributeArgRule": "stdAttr",)");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value())
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
    EXPECT_EQ((*r)->semantics().attributeArgRuleName, "stdAttr");
    EXPECT_TRUE((*r)->semantics().attributeArgRule.valid());
}

// ABSENT: still loads, and the rule stays INVALID. This is the pin that keeps
// the key OPTIONAL — the three sibling rules are required, and copying that
// posture here would refuse to load every language whose attribute clauses take
// no arguments (and every fixture in this file). An invalid id is the documented
// "flag nothing" state, so the consumer degrades to its pre-key behavior rather
// than to a wrong one.
TEST(GrammarSchema, AttributeArgRuleIsOptional) {
    auto r = GrammarSchema::loadFromText(attributeSemanticsSchemaWith(""));
    ASSERT_TRUE(r.has_value())
        << "attributeArgRule must be OPTIONAL — a language may declare an "
           "attribute surface with no argument grammar: "
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
    EXPECT_FALSE((*r)->semantics().attributeArgRule.valid());
    EXPECT_TRUE((*r)->semantics().attributeArgRuleName.empty());
}

// PRESENT but naming a shape that does not exist → C_UnknownShape, LOUD.
// ★ Optional must not mean forgiving: a typo'd rule name that merely left the id
// invalid would silently restore the arg-soup bug this key exists to fix, and the
// load would report success — the exact failure mode
// D-CONFIG-ATTRIBUTE-ARG-RULE-DOCUMENTED-BUT-UNIMPLEMENTED is about.
TEST(GrammarSchema, AttributeArgRuleUnknownShapeReportsInvalid) {
    auto const cfg = attributeSemanticsSchemaWith(
        R"("attributeArgRule": "attrArgsTypo",)");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "an attributeArgRule naming a nonexistent shape must fail the load, "
           "not leave the mechanism silently half-wired";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_UnknownShape));
}

// (The shipped-config regression wall for this vocabulary lives at the end
// of the file, next to the helper that locates c-subset.lang.json.)

// ── TF-C73: the attribute NAME vocabulary — `effect: "align"`, and the
//    loader's ABI-neutral-hint DRIFT cross-check ─────────────────────────────
//
// One skeleton serves all of them: a language with a declaration row that runs
// the strict linkage-specifier scan AND an `attributeSemantics` block, so the
// two lists the cross-check relates are both present and both parameterized.

namespace {
// %EFFECTS%    the `attributeSemantics.effects` array;
// %ROWEXTRA%   extra keys on the single declaration row (trailing comma);
// %BLOCKS%     extra `semantics` sub-blocks (trailing comma).
[[nodiscard]] std::string attrVocabSchema(std::string_view effects,
                                          std::string_view rowExtra,
                                          std::string_view blocks) {
    std::string cfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "AttrVocab", "version": "0.1.0" },
      "tokens": {
        " ": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
        "(": [{ "kind": "ParenOpen" }],
        ")": [{ "kind": "ParenClose" }],
        "[": [{ "kind": "BracketOpen" }],
        "]": [{ "kind": "BracketClose" }],
        ";": [{ "kind": "Semi" }]
      },
      "keywords": [ { "word": "st", "kind": "StKw" } ],
      "shapes": {
        "root":     { "sequence": [ { "repeat": "vdecl" } ] },
        "vdecl":    { "sequence": [ "vprefix", "Identifier", "Semi" ] },
        "vprefix":  { "sequence": [ { "repeat": { "alt": [ "StKw", "attrSpec", "stdAttr" ] } } ] },
        "attrSpec": { "sequence": [ "ParenOpen", "Identifier", "ParenClose" ] },
        "stdAttr":  { "sequence": [ "BracketOpen", "Identifier", "BracketClose" ] },
        "bare":     { "sequence": [ "Semi" ] }
      },
      "semantics": {
        "identifierToken": "Identifier",
        "declarations": [
          { "rule": "vdecl", "name": 0, "kind": "variable",
            "specifierPrefix": "vprefix",
            %ROWEXTRA%
            "linkageSpecifiers": { "st": { "binding": "local" } } }
        ],
        %BLOCKS%
        "attributeSemantics": {
          "attrSpecRule":      "attrSpec",
          "stdAttrRule":       "stdAttr",
          "bareStatementRule": "bare",
          "effects": %EFFECTS%
        }
      }
    })JSON";
    cfg.replace(cfg.find("%ROWEXTRA%"), 10, rowExtra);
    cfg.replace(cfg.find("%BLOCKS%"),    8, blocks);
    cfg.replace(cfg.find("%EFFECTS%"),   9, effects);
    return cfg;
}

// The reference vocabulary: `deprecated` carries a declaration-attached effect
// and IS linkage-ignored; `fallthrough` is inert and deliberately is NOT.
constexpr std::string_view kConsistentEffects =
    R"([ { "names": ["deprecated"],  "appliesTo": ["variable"],
           "effect": "warnOnUse" },
         { "names": ["fallthrough"], "effect": "none"      } ])";
constexpr std::string_view kIgnoresDeprecated =
    R"("linkageSpecifierIgnoredNames": ["deprecated"],)";
} // namespace

// ── the `align` verb ──────────────────────────────────────────────────────

// `effect: "align"` reaches `SemanticConfig` as `AttributeEffect::Align`.
// Without a loader arm this verb would hit the closed-set rejection, and the
// `aligned` attribute could never be declared at all.
TEST(GrammarSchema, AttributeEffectAlignLoads) {
    auto const cfg = attrVocabSchema(
        R"([ { "names": ["aligned"], "appliesTo": ["variable"],
               "effect": "align" } ])",
        R"("linkageSpecifierIgnoredNames": ["aligned"],)", "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value())
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
    ASSERT_EQ((*r)->semantics().attributeEffects.size(), 1u);
    EXPECT_EQ((*r)->semantics().attributeEffects[0].effect,
              AttributeEffect::Align);
}

// A MISSPELLED verb still fails the load — and the closed-set message must
// enumerate EXACTLY the verbs the loader accepts, in BOTH directions.
//
// ★ The both-directions part is the whole pin. The earlier version asserted
// only that the message CONTAINS `align`, and the message was a hand-written
// literal with no mechanical link to the arm chain — so deleting the `align`
// arm left this test GREEN while the sentence it checks became false (MEASURED:
// with the arm removed the loader rejects `align` and still advertises it).
// Here each verb is probed against the real loader and then required to appear
// in the message, and any word IN the message must be a verb that loads — so
// the message can no longer drift from the vocabulary in either direction, and
// the fix that makes this pass is deriving one from the other.
TEST(GrammarSchema, AttributeEffectUnknownVerbListsExactlyTheAcceptedSet) {
    // TF-C78 (D-CSUBSET-NOINLINE) added `noInline`; TF-C81
    // (D-CSUBSET-ALWAYSINLINE) added `alwaysInline`; TF-C92
    // (D-CSUBSET-NO-SANITIZE-THREAD) added `noSanitizeThread`. This list is the
    // hand-maintained mirror of the loader's `kEffectVerbs`, and it going RED
    // on a vocabulary change is the test working as designed — the whole point
    // is that a verb cannot be added to the loader without the closed-set
    // message and this mirror both accounting for it.
    constexpr std::string_view kVerbs[] = {"suppressUnused", "warnOnUse",
                                           "warnOnDiscard", "align",
                                           "noInline", "alwaysInline",
                                           "noSanitizeThread", "none"};
    // The message under test.
    auto const bad = attrVocabSchema(
        R"([ { "names": ["aligned"], "effect": "algin" } ])", "", "");
    auto r = GrammarSchema::loadFromText(bad);
    ASSERT_FALSE(r.has_value())
        << "a misspelled effect verb must fail the load, never silently "
           "default the row to a no-op";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
    std::string closedSetMessage;
    for (auto const& d : r.error()) {
        if (d.message.find("unknown attribute effect") != std::string::npos)
            closedSetMessage = d.message;
    }
    ASSERT_FALSE(closedSetMessage.empty())
        << "the closed-set rejection message was not emitted at all";

    for (auto const verb : kVerbs) {
        // Direction 1 — every verb the LOADER ACCEPTS must be LISTED. A verb
        // the loader takes but the message omits sends an author who is
        // already confused to fix the wrong thing.
        //
        // ★ TF-C93: the probe row now has to respect the `appliesTo` split, and
        // this branch IS the split, stated once: every verb but `none` REQUIRES
        // the key, `none` REFUSES it. Writing one shape for both would make the
        // loop fail for a reason that has nothing to do with the verb vocabulary
        // it is here to check.
        auto const good = attrVocabSchema(
            verb == "none"
                ? std::string{R"([ { "names": ["aligned"], "effect": "none" } ])"}
                : std::format(R"([ {{ "names": ["aligned"],
                                      "appliesTo": ["variable"],
                                      "effect": "{}" }} ])",
                              verb),
            R"("linkageSpecifierIgnoredNames": ["aligned"],)", "");
        auto const ok = GrammarSchema::loadFromText(good);
        ASSERT_TRUE(ok.has_value())
            << "'" << verb << "' must be an accepted effect verb: "
            << errorDiags(ok.error());
        EXPECT_NE(closedSetMessage.find(verb), std::string::npos)
            << "the loader accepts effect verb '" << verb << "' but the "
               "closed-set message does not list it — the message must be "
               "DERIVED from the arms, not restated beside them. Message was: "
            << closedSetMessage;
    }
    // Direction 2 — nothing the message lists may be a verb the loader
    // REJECTS. Counts the listed alternatives so a verb dropped from the arms
    // while left in the message is caught too.
    std::size_t listed = 1;   // n alternatives ⇒ n-1 separators
    for (std::size_t p = closedSetMessage.find(" | ");
         p != std::string::npos;
         p = closedSetMessage.find(" | ", p + 1)) {
        ++listed;
    }
    EXPECT_EQ(listed, std::size(kVerbs))
        << "the closed-set message lists " << listed << " verbs but the "
           "loader accepts " << std::size(kVerbs)
        << " — a listed verb the loader rejects is the same lie in the other "
           "direction. Message was: " << closedSetMessage;
}

// ── the effects TABLE's own well-formedness ───────────────────────────────

// A name may be bound ONCE. Two rows claiming it load cleanly and leave WHICH
// applies to consumer iteration order.
//
// ★ `align` is where that stops being cosmetic. `AttributeEffect::Align`'s own
// contract says demoting `aligned` to a no-op "produces a silently
// UNDER-ALIGNED object — a miscompile, not a missing warning" — and the pair
// below is exactly that demotion, admitted by the loader.
TEST(GrammarSchema, AttributeEffectDuplicateNameAcrossRowsReportsInvalid) {
    auto const cfg = attrVocabSchema(
        R"([ { "names": ["aligned"], "appliesTo": ["variable"],
               "effect": "align" },
             { "names": ["aligned"], "effect": "none"  } ])",
        R"("linkageSpecifierIgnoredNames": ["aligned"],)", "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "the same attribute name bound to two different effects must fail "
           "the load — which one wins is otherwise consumer iteration order, "
           "and for 'align' the losing row silently under-aligns the object";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
    bool namesTheAttribute = false;
    for (auto const& d : r.error()) {
        if (d.message.find("already bound") != std::string::npos
            && d.message.find("aligned") != std::string::npos) {
            namesTheAttribute = true;
        }
    }
    EXPECT_TRUE(namesTheAttribute)
        << "the duplicate diagnostic must name the offending ATTRIBUTE — "
           "'some row is a duplicate' does not tell the author which";
}

// The duplicate check uses the SAME dunder normalizer every other attribute
// comparison uses, so `aligned` and `__aligned__` are ONE binding. Without
// this the conflicting pair is spelled around in one character.
TEST(GrammarSchema, AttributeEffectDuplicateNameIsDunderNormalized) {
    auto const cfg = attrVocabSchema(
        R"([ { "names": ["aligned"], "appliesTo": ["variable"],
               "effect": "align" },
             { "names": ["__aligned__"], "effect": "none"  } ])",
        R"("linkageSpecifierIgnoredNames": ["aligned"],)", "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "'__aligned__' is the SAME attribute as 'aligned' everywhere else "
           "in this loader; the duplicate check must normalize identically or "
           "the conflict is spelled around in two characters";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// A duplicate WITHIN one row is the same defect and must also fail — the check
// is over the table, not over row boundaries.
TEST(GrammarSchema, AttributeEffectDuplicateNameWithinOneRowReportsInvalid) {
    auto const cfg = attrVocabSchema(
        R"([ { "names": ["deprecated", "deprecated"], "appliesTo": ["variable"],
               "effect": "warnOnUse" } ])",
        kIgnoresDeprecated, "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "a name repeated inside one row is still a name bound twice";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// ANTI-OVER-BROAD. Two DIFFERENT names sharing a row, and two rows with
// disjoint names, must keep loading — the check is on the NAME, not on the
// verb or the row count.
TEST(GrammarSchema, AttributeEffectDistinctNamesAcrossRowsStillLoad) {
    auto const cfg = attrVocabSchema(
        R"([ { "names": ["deprecated"], "appliesTo": ["variable"],
               "effect": "warnOnUse" },
             { "names": ["aligned"],    "appliesTo": ["variable"],
               "effect": "align"     },
             { "names": ["likely", "unlikely"],  "effect": "none"      } ])",
        R"("linkageSpecifierIgnoredNames": ["deprecated", "aligned"],)", "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value())
        << "distinct names must keep loading — the duplicate check must not "
           "widen into 'one row only' or 'one name per row': "
        << errorDiags(r.error());
    EXPECT_EQ((*r)->semantics().attributeEffects.size(), 3u);
}

// An UNKNOWN KEY on an `effects` row. One nesting level under the row whose
// keys this cycle DID close, and the identical knob-that-lies: `"efect":
// "align"` beside a real `effect` loads clean and the intended verb never
// takes.
TEST(GrammarSchema, AttributeEffectRowUnknownKeyReportsInvalid) {
    auto const cfg = attrVocabSchema(
        R"([ { "names": ["deprecated"], "appliesTo": ["variable"],
               "effect": "warnOnUse", "efect": "align" } ])",
        kIgnoresDeprecated, "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "an unknown key on an effects row must fail the load — it reads as "
           "configured and does nothing";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// …and the `$` documentation convention stays exempt there too.
TEST(GrammarSchema, AttributeEffectRowDollarPrefixedKeyIsExempt) {
    auto const cfg = attrVocabSchema(
        R"([ { "$comment": "deprecated warns at every use",
               "names": ["deprecated"], "appliesTo": ["variable"],
               "effect": "warnOnUse" } ])",
        kIgnoresDeprecated, "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value())
        << "a '$'-prefixed documentation key on an effects row must not trip "
           "the typo discriminator: " << errorDiags(r.error());
    EXPECT_EQ((*r)->semantics().attributeEffects.size(), 1u);
}

// A row naming NOTHING binds its verb to nothing: inert config that reads as
// configured.
TEST(GrammarSchema, AttributeEffectEmptyNamesArrayReportsInvalid) {
    auto const cfg = attrVocabSchema(
        R"([ { "names": [], "appliesTo": ["variable"],
               "effect": "warnOnUse" } ])", kIgnoresDeprecated, "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "an effects row with no names can never fire and must fail the load";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// The empty NAME is unreachable by construction — no attribute clause can
// spell it — so it is always a typo or a stray comma.
TEST(GrammarSchema, AttributeEffectEmptyNameStringReportsInvalid) {
    auto const cfg = attrVocabSchema(
        R"([ { "names": [""], "effect": "none" } ])", kIgnoresDeprecated, "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "an empty attribute name could never match and must fail the load";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// ── ★★★ TF-C93: the `appliesTo` KIND-SET key
//    (D-CSUBSET-ATTRIBUTE-IGNORED-FOR-DECL-KIND-SILENT) ─────────────────────
//
// `appliesTo` names the entity kinds an attribute may appertain to, and the
// semantic tier's ONE shared decl-kind gate walks it. Its VALUE therefore needs
// real validation — `DSS_CHECK_KEY_VOCABULARY` does NOT provide any: MEASURED at
// `config_key_vocabulary.hpp:92-97`, it is a `static_assert` on array
// WELL-FORMEDNESS only (no empty/duplicate KEY NAMES) and says nothing about a
// key's contents. Growing `kEffectRowKeys` 2 → 3 correctly extends the
// unknown-KEY loop; everything below is about the VALUE.

// ★★ THE REQUIREMENT ITSELF — the design's blocking correction. A row with a
// declaration-attached effect and NO `appliesTo` must fail the LOAD.
//
// ★ WHY REQUIRED RATHER THAN OPTIONAL-WITH-A-DEFAULT, pinned because the cheap
// alternative is so tempting: "absent ⇒ applies to every kind" makes a row that
// merely FORGOT the key read as a deliberate universal claim, and the gate then
// goes silent on exactly the misuse it exists to report. That is
// [[D-TEST-IGNORE-LIST-IS-A-LICENSE-TO-DROP]] one cycle after that row was
// written. A required key cannot be forgotten quietly.
TEST(GrammarSchema, AttributeEffectAppliesToMissingOnDeclAttachedRowReportsInvalid) {
    auto const cfg = attrVocabSchema(
        R"([ { "names": ["deprecated"], "effect": "warnOnUse" } ])",
        kIgnoresDeprecated, "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "a row whose effect acts on the DECLARED ENTITY must say which entity "
           "kinds it appertains to — an absent key must NOT degrade to a "
           "permissive 'every kind' default";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
    bool rightDiag = false;
    for (auto const& d : r.error()) {
        if (d.code == DiagnosticCode::C_InvalidSemantics
            && d.path == "/semantics/attributeSemantics/effects/0/appliesTo"
            && d.message.find("REQUIRED") != std::string::npos
            && d.message.find("warnOnUse") != std::string::npos) {
            rightDiag = true;
        }
    }
    EXPECT_TRUE(rightDiag)
        << "the diagnostic must be AT the missing key's path and NAME the verb "
           "that made it required — 'something is invalid' does not tell the "
           "author which row to edit: "
        << errorDiags(r.error());
}

// THE EXEMPTION, asserted in the same file as the requirement so neither can
// drift. A `none`-verb row bundles function-only, type-only, statement-only and
// elsewhere-consumed names in ONE list, so no single kind set is correct for it
// — and the loader must not demand one.
TEST(GrammarSchema, AttributeEffectAppliesToIsExemptOnANoneVerbRow) {
    auto const cfg = attrVocabSchema(
        R"([ { "names": ["fallthrough", "likely"], "effect": "none" } ])",
        kIgnoresDeprecated, "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value())
        << "a 'none' row must stay exempt — demanding a kind set from a row that "
           "mixes statement-, type- and function-attached names would force the "
           "author to invent a wrong answer: "
        << errorDiags(r.error());
    ASSERT_EQ((*r)->semantics().attributeEffects.size(), 1u);
    EXPECT_TRUE((*r)->semantics().attributeEffects[0].appliesTo.empty())
        << "and it must reach SemanticConfig with an EMPTY set — that emptiness "
           "IS the exemption the engine's gate reads (it tests the config, never "
           "the verb)";
}

// …and the key is REFUSED on a `none` row, not silently accepted. The gate can
// never read it there (an empty set is what marks the exemption), so accepting
// it would be a knob that lies — the same defect class as every unknown-key
// check in this loader.
TEST(GrammarSchema, AttributeEffectAppliesToOnANoneVerbRowReportsInvalid) {
    auto const cfg = attrVocabSchema(
        R"([ { "names": ["fallthrough"], "appliesTo": ["variable"],
               "effect": "none" } ])",
        kIgnoresDeprecated, "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "'appliesTo' on a 'none' row could never be read — accepting it would "
           "let an author believe a kind restriction is in force when none is";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// An UNKNOWN kind string. The rejection must ENUMERATE the closed set, and the
// enumeration must be DERIVED from the vocabulary table rather than restated
// beside it — the drift discipline this loader already applies to the effect-verb
// closed set, where a hand-written literal was MEASURED to keep advertising a
// verb the loader had stopped accepting.
TEST(GrammarSchema, AttributeEffectAppliesToUnknownKindEnumeratesTheClosedSet) {
    auto const cfg = attrVocabSchema(
        R"([ { "names": ["deprecated"], "appliesTo": ["varaible"],
               "effect": "warnOnUse" } ])",
        kIgnoresDeprecated, "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "a misspelled declaration kind must fail the load — silently dropping "
           "it would shrink the declared set and make the gate fire on correct C";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
    std::string msg;
    for (auto const& d : r.error()) {
        if (d.message.find("unknown declaration kind") != std::string::npos
            && d.message.find("appliesTo") != std::string::npos) {
            msg = d.message;
        }
    }
    ASSERT_FALSE(msg.empty())
        << "the unknown-kind rejection was not emitted at all: "
        << errorDiags(r.error());
    EXPECT_NE(msg.find("varaible"), std::string::npos)
        << "it must quote what the author WROTE: " << msg;
    // Every kind the loader ACCEPTS must appear in the message — probed against
    // the real loader, so the sentence cannot drift from the vocabulary.
    for (auto const* kind : {"variable", "function", "table", "type"}) {
        auto const good = attrVocabSchema(
            std::format(R"([ {{ "names": ["deprecated"], "appliesTo": ["{}"],
                                "effect": "warnOnUse" }} ])", kind),
            kIgnoresDeprecated, "");
        auto const ok = GrammarSchema::loadFromText(good);
        ASSERT_TRUE(ok.has_value())
            << "'" << kind << "' must be an accepted declaration kind: "
            << errorDiags(ok.error());
        EXPECT_NE(msg.find(kind), std::string::npos)
            << "the loader accepts kind '" << kind << "' but the closed-set "
               "message omits it — the message must be DERIVED from the "
               "vocabulary table, not restated beside it. Message was: " << msg;
    }
}

// An EMPTY `appliesTo`. "Appertains to nothing" is not a coherent claim: the
// row's effect would still fire nowhere while every declaration spelling the
// attribute warned. Rejected rather than treated as a silent disable.
TEST(GrammarSchema, AttributeEffectAppliesToEmptyArrayReportsInvalid) {
    auto const cfg = attrVocabSchema(
        R"([ { "names": ["deprecated"], "appliesTo": [],
               "effect": "warnOnUse" } ])",
        kIgnoresDeprecated, "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "an empty kind set must fail the load — it is also indistinguishable "
           "from the 'none'-row exemption the engine's gate reads";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// A DUPLICATE kind. The list is a SET; a repeat changes nothing and is always a
// typo or a botched merge. Left accepted it trains an author to read the list as
// unchecked prose.
TEST(GrammarSchema, AttributeEffectAppliesToDuplicateKindReportsInvalid) {
    auto const cfg = attrVocabSchema(
        R"([ { "names": ["deprecated"], "appliesTo": ["variable", "variable"],
               "effect": "warnOnUse" } ])",
        kIgnoresDeprecated, "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "a declaration kind listed twice must fail the load";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
    bool namesTheKind = false;
    for (auto const& d : r.error()) {
        if (d.message.find("listed twice") != std::string::npos
            && d.message.find("variable") != std::string::npos) {
            namesTheKind = true;
        }
    }
    EXPECT_TRUE(namesTheKind)
        << "and it must name WHICH kind repeats: " << errorDiags(r.error());
}

// NOT AN ARRAY — the shape mistake a hand-edited config makes first
// (`"appliesTo": "function"`). Rejected with the closed set, so the author can
// fix both the shape and the value from one message.
TEST(GrammarSchema, AttributeEffectAppliesToNotAnArrayReportsInvalid) {
    auto const cfg = attrVocabSchema(
        R"([ { "names": ["deprecated"], "appliesTo": "variable",
               "effect": "warnOnUse" } ])",
        kIgnoresDeprecated, "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "a bare string must not be silently accepted as a one-element set";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// A NON-STRING ELEMENT. `["variable", 3]` must not load with the good element
// kept and the bad one dropped — a partially-parsed kind set is a SHRUNKEN one,
// which makes the gate warn on correct C.
TEST(GrammarSchema, AttributeEffectAppliesToNonStringElementReportsInvalid) {
    auto const cfg = attrVocabSchema(
        R"([ { "names": ["deprecated"], "appliesTo": ["variable", 3],
               "effect": "warnOnUse" } ])",
        kIgnoresDeprecated, "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "a non-string entry must fail the load, not be skipped — the "
           "surviving set would be silently narrower than the author wrote";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// POSITIVE: a well-formed multi-kind set REACHES `SemanticConfig` in order. This
// is the pin that keeps every negative above from passing for the wrong reason,
// and it is the shape the shipped `aligned` row uses (the negative control whose
// own sink judges all three kinds).
TEST(GrammarSchema, AttributeEffectAppliesToMultiKindSetReachesConfig) {
    auto const cfg = attrVocabSchema(
        R"([ { "names": ["aligned"], "appliesTo": ["variable", "function", "type"],
               "effect": "align" } ])",
        R"("linkageSpecifierIgnoredNames": ["aligned"],)", "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value()) << errorDiags(r.error());
    ASSERT_EQ((*r)->semantics().attributeEffects.size(), 1u);
    auto const& applies = (*r)->semantics().attributeEffects[0].appliesTo;
    ASSERT_EQ(applies.size(), 3u);
    EXPECT_EQ(applies[0], DeclarationKind::Variable);
    EXPECT_EQ(applies[1], DeclarationKind::Function);
    EXPECT_EQ(applies[2], DeclarationKind::Type);
}

// ★ THE SHIPPED CONFIG ITSELF loads with every non-`none` row carrying a
// non-empty `appliesTo`. This is the pin that catches a half-landed config edit
// (a new verb added to `effects` without its kind set) at the tier where it is
// cheapest to see — and it asserts the PROPERTY over the whole table rather than
// naming the eight rows individually, so it keeps holding as the table grows.
//
// ⚠ ROWS ≠ VERBS, and the distinction is why the count below is a `_GE` on ROWS.
// c-subset ships EIGHT declaration-attached rows carrying SEVEN distinct effect
// verbs: `warnOnDiscard` is spelled by TWO rows (`nodiscard` and its GNU twin
// `warn_unused_result`), because the two names share one effect but have DIFFERENT
// applicability sets — `nodiscard` is function-only per C23 6.7.13.3 while clang
// enumerates typedefs among the valid positions for the GNU spelling. That split
// is the whole reason a row-count and a verb-count diverge here, so counting verbs
// would under-count the table by one and hide a lost row.
TEST(GrammarSchema, AppliesToIsPresentOnEveryDeclAttachedRowOfShippedCSubset) {
    auto r = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(r.has_value())
        << "the shipped c-subset config must satisfy its own `appliesTo` rule";
    std::size_t declAttached = 0;
    for (auto const& row : (*r)->semantics().attributeEffects) {
        if (row.effect == AttributeEffect::None) {
            EXPECT_TRUE(row.appliesTo.empty())
                << "a 'none' row must carry NO kind set — an empty set is what "
                   "marks the exemption the engine's gate reads";
            continue;
        }
        ++declAttached;
        EXPECT_FALSE(row.appliesTo.empty())
            << "every declaration-attached row must declare its kinds; row "
               "naming '"
            << (row.names.empty() ? "<none>" : row.names[0]) << "' does not";
    }
    EXPECT_GE(declAttached, 8u)
        << "c-subset ships EIGHT declaration-attached effect ROWS carrying SEVEN "
           "distinct effect VERBS — suppressUnused / warnOnUse / warnOnDiscard "
           "(TWO rows: `nodiscard` and the GNU `warn_unused_result`, one verb but "
           "two applicability sets) / align / noInline / alwaysInline / "
           "noSanitizeThread. A lower count means a row was lost, demoted to "
           "'none', or merged back into a sibling — each of which is how a sink "
           "goes silent, and the middle two are invisible to a verb-count";
}

// ── the drift cross-check ─────────────────────────────────────────────────

// BASELINE. A consistent vocabulary loads clean, so every negative below
// cannot pass for the wrong reason. Note what this pins POSITIVELY: the inert
// `fallthrough` is NOT linkage-ignored and that is FINE — a statement-attached
// attribute never reaches a declaration's specifier scan, and a cross-check
// that demanded it would reject correct config.
TEST(GrammarSchema, AttributeVocabularyConsistentPairLoadsCleanly) {
    auto const cfg =
        attrVocabSchema(kConsistentEffects, kIgnoresDeprecated, "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value())
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
    EXPECT_EQ((*r)->semantics().attributeEffects.size(), 2u);
}

// CLAUSE B, DRIFTED. `deprecated` keeps its declaration-attached effect but is
// dropped from the row's ignore list — exactly what happens when someone edits
// one list and not the other. Left unchecked this is not a load error but a
// COMPILE error on legal source: a leading `__attribute__((deprecated))` fails
// H_UnknownLinkageSpecifier, so the effects row can never fire.
TEST(GrammarSchema, AttributeVocabularyDriftedIgnoreListReportsInvalid) {
    auto const cfg = attrVocabSchema(
        kConsistentEffects, R"("linkageSpecifierIgnoredNames": ["noreturn"],)",
        "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "an effects name with a declaration-attached effect that the row's "
           "strict linkage scan neither ignores nor recognizes must fail the "
           "load";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
    bool namesBothLists = false;
    for (auto const& d : r.error()) {
        if (d.message.find("attributeSemantics.effects") != std::string::npos
            && d.message.find("linkageSpecifierIgnoredNames")
                   != std::string::npos) {
            namesBothLists = true;
        }
    }
    EXPECT_TRUE(namesBothLists)
        << "the drift message must name BOTH lists — the reader has to know "
           "which two surfaces disagree, not merely that something does";
}

// CLAUSE B, INERT NAMES STAY EXEMPT. The same drifted shape but with the name
// carrying `effect: "none"` must still load. This is the pin that keeps the
// cross-check from becoming the over-broad "every effects name must be
// ignored everywhere" rule, which would reject the shipped c-subset (whose
// `fallthrough`/`likely`/`packed` rows are deliberately not ignore-listed).
TEST(GrammarSchema, AttributeVocabularyInertEffectNeedsNoIgnoreEntry) {
    auto const cfg = attrVocabSchema(
        R"([ { "names": ["deprecated"], "appliesTo": ["variable"],
               "effect": "warnOnUse" },
             { "names": ["likely", "unlikely"], "effect": "none" } ])",
        kIgnoresDeprecated, "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value())
        << "an inert ('none') attribute name may be statement- or type-"
           "attached and must not be forced into a declaration row's ignore "
           "list: "
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
}

// CLAUSE B, THE GATE — and this is a REPLACEMENT for a pin that cemented a
// hole. The gate used to be "does this row list at least one ignored NAME?",
// and the test here asserted that a row listing none stays EXEMPT. That made an
// invariant out of the check's own blind spot: deleting ONE name from a list
// failed correctly, while deleting the WHOLE key emptied the list and silently
// exempted the row — so the larger, more plausible edit was the one that
// escaped, and this file said that was intended. A test that pins a known false
// negative is worse than no test: it converts "we have not closed this" into
// "we decided not to", and the next reader has no way to tell them apart.
//
// The gate now reads `linkageSpecifierIgnoredRules` FIRST and the name list
// only as a fallback, giving three tiers. The four tests below pin each.

// (i) TIER 2 — DELETING THE WHOLE NAMES KEY MUST STILL FAIL. The row ignores
// `stdAttr` wholesale but leaves `attrSpec` LIVE, so it has demonstrably
// reasoned about attribute syntax reaching this declaration form and part of
// that syntax still routes identifiers to the strict name lookup. The gate
// therefore keys on the RULES list, which this edit does not touch — the whole
// point, since deleting the names list is exactly how the old gate was
// switched off.
TEST(GrammarSchema, AttributeVocabularyWholeIgnoredNamesKeyDeletedReportsInvalid) {
    auto const cfg = attrVocabSchema(
        kConsistentEffects,
        R"("linkageSpecifierIgnoredRules": ["stdAttr"],)", "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "deleting the ENTIRE 'linkageSpecifierIgnoredNames' key is the SAME "
           "drift as deleting one entry from it, and must not be the version "
           "that escapes by emptying the list the gate reads";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
    bool rightDiag = false;
    for (auto const& d : r.error()) {
        if (d.code == DiagnosticCode::C_InvalidSemantics
            && d.path == "/semantics/declarations/0/"
                         "linkageSpecifierIgnoredNames"
            && d.message.find("attribute-vocabulary drift") != std::string::npos
            && d.message.find("deprecated") != std::string::npos) {
            rightDiag = true;
        }
    }
    EXPECT_TRUE(rightDiag)
        << "the diagnostic must be the drift one, AT the ignore-names path, "
           "naming 'deprecated' — asserting only that the load failed would "
           "pass for any unrelated error";
}

// ★★ TF-C92 (D-CSUBSET-NO-SANITIZE-THREAD): the NEW verb participates in the SAME
// mechanical coupling — asserted per-verb rather than assumed from the check's
// `effect != None` shape.
//
// ★ WHY A DEDICATED CASE FOR THIS VERB RATHER THAN TRUSTING THE GENERIC GATE. The
// shipped c-subset config needed THREE coordinated edits for this attribute (the
// `effects` row plus BOTH declaration rows' `linkageSpecifierIgnoredNames`), and the
// only thing that makes forgetting one of them impossible is this cross-check. Its
// generality is a property of the current implementation, not of the contract — a
// future refactor could special-case a verb and nothing would notice. This pins the
// contract for the verb whose config edits are newest and therefore likeliest to be
// half-reverted.
//
// BOTH DIRECTIONS ARE ASSERTED: without the ignored name the load must FAIL with the
// drift diagnostic naming the attribute; with it the load must SUCCEED. The second
// half matters as much as the first — a check that rejected the consistent
// configuration too would be indistinguishable from a working one here.
TEST(GrammarSchema, NoSanitizeThreadEffectRequiresTheIgnoredNameToo) {
    constexpr std::string_view kEffects =
        R"([ { "names": ["no_sanitize_thread"], "appliesTo": ["function"],
               "effect": "noSanitizeThread" } ])";

    // (a) the effect row alone — the half-landed config edit.
    auto const missing = attrVocabSchema(
        kEffects, R"("linkageSpecifierIgnoredRules": ["stdAttr"],)", "");
    auto rBad = GrammarSchema::loadFromText(missing);
    ASSERT_FALSE(rBad.has_value())
        << "a declaration-attached `noSanitizeThread` effect whose name is NOT in "
           "this row's linkageSpecifierIgnoredNames must fail the LOAD — otherwise "
           "the effects row is unreachable config: a leading "
           "__attribute__((no_sanitize_thread)) would die at the linkage tier "
           "before the semantic scan ever runs";
    EXPECT_TRUE(hasDiagCode(rBad.error(), DiagnosticCode::C_InvalidSemantics));
    bool rightDrift = false;
    for (auto const& d : rBad.error()) {
        if (d.code == DiagnosticCode::C_InvalidSemantics
            && d.path == "/semantics/declarations/0/"
                         "linkageSpecifierIgnoredNames"
            && d.message.find("attribute-vocabulary drift") != std::string::npos
            && d.message.find("no_sanitize_thread") != std::string::npos) {
            rightDrift = true;
        }
    }
    EXPECT_TRUE(rightDrift)
        << "the diagnostic must be the drift one, AT the ignore-names path, and it "
           "must NAME the attribute the author has to add: "
        << errorDiags(rBad.error());

    // (b) both halves present — must load clean. This is the shape the shipped
    // c-subset config uses on BOTH its topLevelDecl and externDecl rows.
    auto const consistent = attrVocabSchema(
        kEffects, R"("linkageSpecifierIgnoredNames": ["no_sanitize_thread"],)", "");
    auto rGood = GrammarSchema::loadFromText(consistent);
    ASSERT_TRUE(rGood.has_value())
        << "the CONSISTENT pairing must load — a gate that also rejected this "
           "would look identical to a working one in half (a): "
        << errorDiags(rGood.error());
}

// (i-b) TIER 3 — a row that mentions NO attribute rule at all and ignores
// nothing by name stays EXEMPT, and that limit is DELIBERATE, not an oversight.
// The loader holds only a name↔id rule table; it cannot see whether such a
// row's prefix grammar admits an attribute clause at all, and c-subset's
// `externDecl` is exactly this shape (its specifier prefix is `extern` plus the
// thread-local twins, so no attribute identifier can occur there).
//
// ★ Pinning the limit is the honest alternative to pretending it is closed. It
// is also the reason (i) uses the RULES signal rather than simply dropping the
// old gate: dropping it outright was MEASURED to reject the shipped c-subset on
// this very row.
TEST(GrammarSchema, AttributeVocabularyRowClaimingNothingAboutAttrsIsExempt) {
    auto const cfg = attrVocabSchema(kConsistentEffects, "", "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value())
        << "a row that neither ignores an attribute rule nor ignores anything "
           "by name makes no visible claim about attribute syntax, and the "
           "loader cannot prove its prefix grammar admits one — this "
           "reachability limit is documented and must stay exempt: "
        << errorDiags(r.error());
}

// (ii) THE EXEMPTION THAT IS ACTUALLY SOUND. A row that skips every declared
// attribute rule WHOLESALE (`linkageSpecifierIgnoredRules`) cannot route an
// attribute identifier to its name lookup at all, so no per-name entry could
// change its behavior.
//
// ★ Without this, the check is wrong in the OTHER direction, and wrong in a way
// that pushes config the wrong way. MEASURED on the shipped c-subset: adding
// ONE unrelated name to `varDecl` — a row that already ignores `attrSpec` and
// `stdAttr` wholesale — flipped the old gate on and demanded six more names be
// added to a silence list that could never be consulted. A guard whose remedy
// is "silence more things" teaches the wrong lesson even when it is right.
TEST(GrammarSchema, AttributeVocabularyRowIgnoringAttrRulesWholesaleIsExempt) {
    auto const cfg = attrVocabSchema(
        kConsistentEffects,
        R"("linkageSpecifierIgnoredNames": ["restrict"],
           "linkageSpecifierIgnoredRules": ["attrSpec", "stdAttr"],)", "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value())
        << "a row that skips every attribute rule wholesale can never route an "
           "attribute identifier to its name lookup, so it must stay exempt "
           "however many unrelated names it happens to ignore: "
        << errorDiags(r.error());
}

// (iii) THE EXEMPTION IS ALL-OR-NOTHING. Skipping `attrSpec` while leaving
// `stdAttr` live still lets a `[[deprecated]]` identifier reach the strict
// lookup, so a PARTIAL wholesale skip must NOT exempt the row. Without this the
// fix for (ii) would be "any ignoredRules entry disarms the check" — trading
// one silent hole for another.
TEST(GrammarSchema, AttributeVocabularyPartialWholesaleSkipIsNotExempt) {
    auto const cfg = attrVocabSchema(
        kConsistentEffects,
        R"("linkageSpecifierIgnoredRules": ["attrSpec"],)", "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "ignoring only SOME attribute rules still leaves the others' "
           "identifiers reaching the strict lookup — the wholesale exemption "
           "must require ALL of them";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// CLAUSE A, DRIFTED. `packed.attributeNames` names a spelling the dedicated
// composite scan CONSUMES, but no `effects` row declares it known — so the
// `[[...]]` spelling of that very name warns S_UnknownAttribute while the GNU
// spelling is acted upon. That contradiction is what `AttributeEffect::None`
// exists to prevent, and it was documented in prose only until now.
TEST(GrammarSchema, AttributeVocabularyDriftedPackedNameReportsInvalid) {
    auto const cfg = attrVocabSchema(
        kConsistentEffects, kIgnoresDeprecated,
        R"("packed": { "listRule": "attrSpec",
                       "attributeNames": ["packed"] },)");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "a dedicated-scan attribute name missing from the effects table "
           "must fail the load";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
    bool namesBothLists = false;
    for (auto const& d : r.error()) {
        if (d.message.find("semantics.packed.attributeNames")
                != std::string::npos
            && d.message.find("attributeSemantics.effects")
                   != std::string::npos) {
            namesBothLists = true;
        }
    }
    EXPECT_TRUE(namesBothLists)
        << "the drift message must name BOTH the dedicated-scan list and the "
           "effects table";
}

// CLAUSE A, DUNDER-NORMALIZED. `__packed__` in the dedicated list is satisfied
// by a `packed` effects row — the cross-check must use the SAME normalizer the
// runtime scans use, or it would fire on config that works perfectly.
TEST(GrammarSchema, AttributeVocabularyDedicatedNameMatchesDunderNormalized) {
    auto const cfg = attrVocabSchema(
        R"([ { "names": ["deprecated"], "appliesTo": ["variable"],
               "effect": "warnOnUse" },
             { "names": ["packed"],     "effect": "none"      } ])",
        kIgnoresDeprecated,
        R"("packed": { "listRule": "attrSpec",
                       "attributeNames": ["__packed__"] },)");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value())
        << "the cross-check must dunder-normalize exactly like the scans it "
           "guards: "
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
}

// CLAUSE A, THE GATE. It keys off `stdAttrRule` — "does this language HAVE a
// standard-attribute surface?" — NOT off "is the effects table non-empty?".
//
// ★ Those coincide until exactly the moment they matter. Commenting the
// `effects` rows out to debug something emptied the table and DISARMED the
// clause, so the edit most likely to introduce the drift was also the one that
// switched the detector off. Here the effects table is empty while
// `packed.attributeNames` still names a spelling the dedicated scan consumes —
// which is itself drift, and must report.
TEST(GrammarSchema, AttributeVocabularyClauseAFiresWithEmptyEffectsTable) {
    auto const cfg = attrVocabSchema(
        R"([ ])", kIgnoresDeprecated,
        R"("packed": { "listRule": "attrSpec",
                       "attributeNames": ["packed"] },)");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "emptying the effects table must not DISARM the dedicated-scan "
           "cross-check — commenting rows out to debug is precisely when the "
           "check is needed, and a guard that switches itself off then is not "
           "a guard";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
    bool namesPackedList = false;
    for (auto const& d : r.error()) {
        if (d.message.find("attribute-vocabulary drift") != std::string::npos
            && d.message.find("semantics.packed.attributeNames")
                   != std::string::npos) {
            namesPackedList = true;
        }
    }
    EXPECT_TRUE(namesPackedList)
        << "the diagnostic must be clause A's drift one naming the "
           "dedicated-scan list, not some unrelated failure";
}

// ANTI-OVER-BROAD for that gate: a language with NO attribute surface at all
// (no `attributeSemantics` block, so no `stdAttrRule`) is not forced to declare
// one. This is the case the old `attributeEffects.empty()` gate was reaching
// for, and it keeps working under the new one.
TEST(GrammarSchema, AttributeVocabularyClauseAExemptsLanguageWithNoAttrSurface) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "NoAttrs", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }], "(": [{ "kind": "ParenOpen" }],
                  ")": [{ "kind": "ParenClose" }] },
      "shapes": { "root":     { "sequence": [ "Semi" ] },
                  "attrSpec": { "sequence": [ "ParenOpen", "Identifier",
                                              "ParenClose" ] } },
      "semantics": {
        "identifierToken": "Identifier",
        "packed": { "listRule": "attrSpec", "attributeNames": ["packed"] }
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(r.has_value())
        << "a language that declares no standard-attribute surface has no "
           "S_UnknownAttribute warning to contradict and must not be forced "
           "to declare an effects table: " << errorDiags(r.error());
}

// ── declarationAttrSlotRules: the degenerate shapes ───────────────────────

// A repeated slot rule makes the semantic attribute scan visit that slot
// TWICE, so every clause under it is seen twice — per-attribute diagnostics
// fire twice and per-attribute effects apply twice. Silent before this.
TEST(GrammarSchema, DeclarationAttrSlotRulesDuplicateReportsInvalid) {
    auto const cfg = attrVocabSchema(
        kConsistentEffects,
        R"("linkageSpecifierIgnoredNames": ["deprecated"],
           "declarationAttrSlotRules": ["attrSpec", "attrSpec"],)", "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "a repeated attribute-slot rule must fail the load — the scan would "
           "visit that slot twice and double every finding under it";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// An EMPTY list is byte-identical to omitting the key, so writing it says
// nothing while reading as a configured attribute surface.
TEST(GrammarSchema, DeclarationAttrSlotRulesEmptyArrayReportsInvalid) {
    auto const cfg = attrVocabSchema(
        kConsistentEffects,
        R"("linkageSpecifierIgnoredNames": ["deprecated"],
           "declarationAttrSlotRules": [],)", "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "an empty declarationAttrSlotRules is identical to omitting the "
           "key — a knob that reads as configured and does nothing";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// ANTI-OVER-BROAD: a well-formed list of DISTINCT slot rules still loads and
// still reaches SemanticConfig.
TEST(GrammarSchema, DeclarationAttrSlotRulesDistinctEntriesLoad) {
    auto const cfg = attrVocabSchema(
        kConsistentEffects,
        R"("linkageSpecifierIgnoredNames": ["deprecated"],
           "declarationAttrSlotRules": ["attrSpec", "stdAttr"],)", "");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value()) << errorDiags(r.error());
    EXPECT_EQ((*r)->semantics().declarations[0]
                  .declarationAttrSlotRules.size(), 2u);
}

// ── linkageSpecifiers: the EFFECT object's closed keys ────────────────────

// A typo on ONE axis while another axis is set. The "sets at least one axis"
// guard only catches an object whose EVERY key is misspelled, so this shape
// loaded clean and silently dropped the visibility — a linkage knob that lies,
// one nesting level under the declaration row whose keys this cycle closed.
TEST(GrammarSchema, LinkageSpecifierEffectUnknownKeyReportsInvalid) {
    auto cfg = attrVocabSchema(kConsistentEffects, kIgnoresDeprecated, "");
    constexpr std::string_view kNeedle = R"({ "binding": "local" })";
    auto const pos = cfg.find(kNeedle);
    ASSERT_NE(pos, std::string::npos);
    cfg.replace(pos, kNeedle.size(),
                R"({ "binding": "local", "visibilty": "hidden" })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "a misspelled axis on a linkage effect must fail the load — with a "
           "good axis present the 'sets at least one' guard is satisfied and "
           "the typo'd one is silently dropped";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
    bool namesTheKey = false;
    for (auto const& d : r.error()) {
        if (d.message.find("visibilty") != std::string::npos
            && d.message.find("linkage effect") != std::string::npos) {
            namesTheKey = true;
        }
    }
    EXPECT_TRUE(namesTheKey)
        << "the diagnostic must name the offending KEY — the pre-existing "
           "'must set at least one axis' error fires on a different shape and "
           "would satisfy a weaker assertion";
}

// …and the `$` convention stays exempt inside a linkage effect too.
TEST(GrammarSchema, LinkageSpecifierEffectDollarPrefixedKeyIsExempt) {
    auto cfg = attrVocabSchema(kConsistentEffects, kIgnoresDeprecated, "");
    constexpr std::string_view kNeedle = R"({ "binding": "local" })";
    auto const pos = cfg.find(kNeedle);
    ASSERT_NE(pos, std::string::npos);
    cfg.replace(pos, kNeedle.size(),
                R"({ "$comment": "internal linkage", "binding": "local" })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value())
        << "a '$'-prefixed documentation key inside a linkage effect must not "
           "trip the typo discriminator: " << errorDiags(r.error());
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

// ── c25 D-CSUBSET-UNIFIED-COMPOSITE-SPECIFIER: dual-mode loader guards ──────
//
// `definesWhenChild` makes ONE declaration row dual-mode (a DEFINITION when the
// named body child is present, else a tag REFERENCE resolved by a paired
// `references[]` row). Two loader guards keep a misconfigured language config
// from silently mis-resolving; each gets a red-on-disable negative pin (the
// guard-needs-a-red-test discipline, cluster lessons c8/c21).

// A `definesWhenChild` row with NO paired `references` row for the same rule →
// C_InvalidSemantics (a body-absent occurrence would otherwise resolve to nothing).
TEST(GrammarSchema, SemanticsDefinesWhenChildWithoutReferenceRowReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "{": [{ "kind": "BlockOpen" }], "}": [{ "kind": "BlockClose" }],
                  ";": [{ "kind": "Semi" }] },
      "shapes": {
        "root": { "sequence": [ "spec", "Semi" ] },
        "spec": { "sequence": [ "Identifier", { "optional": "body" } ] },
        "body": { "sequence": [ "BlockOpen", "BlockClose" ] }
      },
      "semantics": {
        "declarations": [ { "rule": "spec", "name": 0, "kind": "type",
                            "definesWhenChild": "body" } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// A `definesWhenChild` naming a child rule that no shape declares → C_UnknownShape.
// (The paired references row is present, so this isolates the child-rule guard.)
TEST(GrammarSchema, SemanticsDefinesWhenChildUnknownChildRuleReportsUnknownShape) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": {
        "root": { "sequence": [ "spec", "Semi" ] },
        "spec": { "sequence": [ "Identifier" ] }
      },
      "semantics": {
        "declarations": [ { "rule": "spec", "name": 0, "kind": "type",
                            "definesWhenChild": "ghostBody" } ],
        "references": [ { "rule": "spec" } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_UnknownShape));
}

// ── FC17.5 D-CSUBSET-AUTO-TYPE-INFERENCE: inference-row loader cross-checks ──
//
// `inferTypeFromInitializer` waives the declarator-mode `head` requirement
// (the type derives from the sole declarator's initializer at Pass 1.5) and
// `requiredSpecifierToken` is the loader-resolved presence-gate token. Four
// pins hold the cross-check lattice: the head∧infer conflict, the positive
// headless load, the unknown gate-token reject (row DROPPED — a half-wired
// gate would silently accept C89 implicit-int), and the PRE-EXISTING
// no-head-no-infer C_MissingField gate staying intact (the relaxation must
// not have widened it). Each uses the minimal synthetic declarator language
// (the test_declarator_engine shapes, trimmed) so the pins are independent
// of the shipped c-subset config.

namespace {
// The shared skeleton: tokens + declarator shapes + the `declarators` role
// block; each test splices its own `declarations` row into %DECLS%.
[[nodiscard]] std::string inferSchemaWithDeclRow(std::string_view declRow) {
    std::string cfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "InferSynth", "version": "0.0.1" },
      "tokens": {
        " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
        "*":  [{ "kind": "Star" }],
        ",":  [{ "kind": "Comma" }],
        ";":  [{ "kind": "Semi" }],
        "=":  [{ "kind": "Eq" }],
        "(":  [{ "kind": "ParenOpen" }],
        ")":  [{ "kind": "ParenClose" }],
        "[":  [{ "kind": "BracketOpen" }],
        "]":  [{ "kind": "BracketClose" }]
      },
      "numberStyle": { "decimal": true, "emitKind": { "integer": "IntLiteral" } },
      "keywords": [
        { "word": "base", "kind": "BaseKw" },
        { "word": "inf",  "kind": "InfKw" }
      ],
      "shapes": {
        "root":    { "sequence": [ { "repeat": "vdecl" } ] },
        "vdecl":   { "sequence": [ "heads", "dlist", "Semi" ] },
        "heads":   { "sequence": [ { "optional": "InfKw" }, "BaseKw" ] },
        "dlist":   { "sequence": [ "idecl", { "repeat": { "sequence": [ "Comma", "idecl" ] } } ] },
        "idecl":   { "sequence": [ "dtor", { "optional": { "sequence": [ "Eq", "IntLiteral" ] } } ] },
        "dtor":    { "sequence": [ { "repeat": "player" }, "ddirect" ] },
        "player":  { "sequence": [ "Star" ] },
        "ddirect": { "sequence": [ { "alt": [ "Identifier", "dgroup" ] },
                                   { "repeat": { "alt": [ "fsuf", "asuf" ] } } ] },
        "dgroup":  { "sequence": [ "ParenOpen", "dtor", "ParenClose" ] },
        "fsuf":    { "sequence": [ "ParenOpen", "ParenClose" ] },
        "asuf":    { "sequence": [ "BracketOpen", "IntLiteral", "BracketClose" ] }
      },
      "semantics": {
        "identifierToken": "Identifier",
        "declarators": {
          "declaratorRule":     "dtor",
          "pointerLayerRule":   "player",
          "pointerToken":       "Star",
          "directRule":         "ddirect",
          "groupRule":          "dgroup",
          "nameToken":          "Identifier",
          "fnSuffixRule":       "fsuf",
          "arraySuffixRule":    "asuf",
          "initDeclaratorRule": "idecl",
          "listRule":           "dlist"
        },
        "declarations": [ %DECLS% ],
        "builtinTypes": [ { "name": "base", "core": "I32" } ],
        "literalTypes": [ { "literal": "IntLiteral", "core": "I32" } ]
      }
    })JSON";
    auto const pos = cfg.find("%DECLS%");
    cfg.replace(pos, 7, declRow);
    return cfg;
}
} // namespace

// (a) A row setting BOTH `head` and `inferTypeFromInitializer` — two
// competing type sources — must reject C_ConflictingField, never resolve by
// precedence (a silent winner would hide a config authoring error).
TEST(GrammarSchema, InferTypeRowWithHeadReportsConflictingField) {
    auto const cfg = inferSchemaWithDeclRow(
        R"({ "rule": "vdecl", "head": 0, "declaratorList": 1,
             "kind": "variable", "inferTypeFromInitializer": true })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_ConflictingField));
}

// (b) The POSITIVE load: an inference row with NO head and a declaratorList
// loads cleanly, with all three fields resolved on the row (the loader's
// head-requirement waiver — RED if the relaxation is dropped).
TEST(GrammarSchema, InferTypeRowWithoutHeadLoadsCleanly) {
    auto const cfg = inferSchemaWithDeclRow(
        R"({ "rule": "vdecl", "declaratorList": 1, "kind": "variable",
             "inferTypeFromInitializer": true,
             "requiredSpecifierToken": "InfKw" })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value())
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
    auto const& sem = (*r)->semantics();
    ASSERT_EQ(sem.declarations.size(), 1u);
    auto const& row = sem.declarations[0];
    EXPECT_TRUE(row.inferTypeFromInitializer);
    EXPECT_FALSE(row.headChild.has_value());
    ASSERT_TRUE(row.declaratorListChild.has_value());
    EXPECT_EQ(*row.declaratorListChild, 1u);
    ASSERT_TRUE(row.requiredSpecifierToken.has_value());
    EXPECT_TRUE(row.requiredSpecifierToken->valid());
    EXPECT_TRUE(row.isDeclaratorMode())
        << "a declaratorList-only inference row is still declarator-mode";
}

// (c) An UNKNOWN `requiredSpecifierToken` name must reject C_UnknownToken
// AND drop the row — a half-wired presence gate would silently accept the
// C89 implicit-int shapes the gate exists to keep loud.
TEST(GrammarSchema, InferTypeRowUnknownRequiredSpecifierTokenRejects) {
    auto const cfg = inferSchemaWithDeclRow(
        R"({ "rule": "vdecl", "declaratorList": 1, "kind": "variable",
             "inferTypeFromInitializer": true,
             "requiredSpecifierToken": "NoSuchKw" })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_UnknownToken));
}

// (d) The PRE-EXISTING gate is un-widened: a declarator-mode row with
// NEITHER `head` NOR `inferTypeFromInitializer` still reports
// C_MissingField (the relaxation waives the head for inference rows ONLY).
TEST(GrammarSchema, DeclaratorModeRowWithoutHeadStillReportsMissingField) {
    auto const cfg = inferSchemaWithDeclRow(
        R"({ "rule": "vdecl", "declaratorList": 1, "kind": "variable" })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_MissingField));
}

// ── `declarations[]` ROW closed key vocabulary (typo discriminator) ──────
//
// Nearly every row field is an OPTIONAL opt-in facet whose absence is
// silent, so a near-miss spelling used to load clean and simply no-op the
// facet. These reuse the skeleton above, which already loads cleanly.

// Baseline: the reference row (no extra key) loads clean, so the negative
// pin below cannot pass for the wrong reason.
TEST(GrammarSchema, DeclarationRowClosedKeyBaselineLoadsCleanly) {
    auto const cfg = inferSchemaWithDeclRow(
        R"({ "rule": "vdecl", "declaratorList": 1, "kind": "variable",
             "inferTypeFromInitializer": true })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value())
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
    EXPECT_EQ((*r)->semantics().declarations.size(), 1u);
}

// An unknown key on a declaration row → C_InvalidSemantics. The SINGULAR
// `linkageSpecifierIgnoredName` is the real-world near-miss: the loader only
// ever reads the plural, so the ignore-list silently stayed empty.
TEST(GrammarSchema, DeclarationRowUnknownKeyReportsInvalid) {
    auto const cfg = inferSchemaWithDeclRow(
        R"({ "rule": "vdecl", "declaratorList": 1, "kind": "variable",
             "inferTypeFromInitializer": true,
             "linkageSpecifierIgnoredName": ["C"] })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "a typo'd declaration-row key must fail the load, not silently "
           "no-op the facet";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// A second near-miss on a different facet — `specifierPrefx` — to pin that
// the check is a vocabulary, not a single special-cased name.
TEST(GrammarSchema, DeclarationRowMisspelledSpecifierPrefixReportsInvalid) {
    auto const cfg = inferSchemaWithDeclRow(
        R"({ "rule": "vdecl", "declaratorList": 1, "kind": "variable",
             "inferTypeFromInitializer": true, "specifierPrefx": 0 })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// ── TF-C73: `declarationAttrSlotRules` + `unknownStrictAttributeIsError` ──
//
// The two new row keys. Both had to join `kDeclarationRowKeys` in the same
// edit, so these also cover that the closed vocabulary grew with them (an
// unlisted key would be rejected by the typo discriminator above).

// `declarationAttrSlotRules` resolves each NAME to a rule id and keeps the
// source spelling for diagnostics.
TEST(GrammarSchema, DeclarationAttrSlotRulesLoad) {
    auto const cfg = inferSchemaWithDeclRow(
        R"({ "rule": "vdecl", "head": 0, "declaratorList": 1,
             "kind": "variable",
             "declarationAttrSlotRules": ["heads", "idecl"] })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value())
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
    ASSERT_EQ((*r)->semantics().declarations.size(), 1u);
    auto const& d = (*r)->semantics().declarations[0];
    ASSERT_EQ(d.declarationAttrSlotRules.size(), 2u);
    EXPECT_TRUE(d.declarationAttrSlotRules[0].valid());
    EXPECT_TRUE(d.declarationAttrSlotRules[1].valid());
    ASSERT_EQ(d.declarationAttrSlotRuleNames.size(), 2u);
    EXPECT_EQ(d.declarationAttrSlotRuleNames[0], "heads");
    EXPECT_EQ(d.declarationAttrSlotRuleNames[1], "idecl");
}

// ★ THE REASON THE KEY IS NAME-BASED. An unknown rule NAME fails the load.
// The rejected design was a list of visible-child INDICES: a wrong index is
// SILENT — the scan descends the named child, finds no attribute specifier
// under it, and asserts nothing, so the config looks configured while the
// attributes are never honored. This test is the measurement that the chosen
// identifier space has the loud failure mode the indexed one lacks.
TEST(GrammarSchema, DeclarationAttrSlotRulesUnknownNameReportsInvalid) {
    auto const cfg = inferSchemaWithDeclRow(
        R"({ "rule": "vdecl", "head": 0, "declaratorList": 1,
             "kind": "variable",
             "declarationAttrSlotRules": ["heads", "attrRunTypo"] })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "an attribute slot naming a nonexistent shape must fail the load — "
           "a silently-unresolved slot is exactly the failure mode the "
           "name-based design exists to rule out";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// A non-array value → C_InvalidSemantics (never coerced to a one-element list).
TEST(GrammarSchema, DeclarationAttrSlotRulesNonArrayReportsInvalid) {
    auto const cfg = inferSchemaWithDeclRow(
        R"({ "rule": "vdecl", "head": 0, "declaratorList": 1,
             "kind": "variable", "declarationAttrSlotRules": "heads" })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "a scalar 'declarationAttrSlotRules' must fail the load, not be "
           "silently promoted to a single-entry list";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// A non-string ELEMENT is rejected too — the array type check alone would let
// `[0]` through, which is precisely the indexed spelling this key rejects.
TEST(GrammarSchema, DeclarationAttrSlotRulesNonStringEntryReportsInvalid) {
    auto const cfg = inferSchemaWithDeclRow(
        R"({ "rule": "vdecl", "head": 0, "declaratorList": 1,
             "kind": "variable", "declarationAttrSlotRules": [0] })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "an INDEX where a rule name belongs must fail the load";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// `unknownStrictAttributeIsError` loads, and DEFAULTS to false (= today's
// behavior) when absent, so adding the key changes no existing config.
TEST(GrammarSchema, UnknownStrictAttributeIsErrorLoads) {
    auto const withKey = inferSchemaWithDeclRow(
        R"({ "rule": "vdecl", "head": 0, "declaratorList": 1,
             "kind": "variable", "unknownStrictAttributeIsError": true })");
    auto r = GrammarSchema::loadFromText(withKey);
    ASSERT_TRUE(r.has_value())
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
    ASSERT_EQ((*r)->semantics().declarations.size(), 1u);
    EXPECT_TRUE((*r)->semantics().declarations[0].unknownStrictAttributeIsError);

    auto const without = inferSchemaWithDeclRow(
        R"({ "rule": "vdecl", "head": 0, "declaratorList": 1,
             "kind": "variable" })");
    auto r2 = GrammarSchema::loadFromText(without);
    ASSERT_TRUE(r2.has_value())
        << (r2.error().empty() ? "<no diagnostics>" : r2.error()[0].message);
    ASSERT_EQ((*r2)->semantics().declarations.size(), 1u);
    EXPECT_FALSE(
        (*r2)->semantics().declarations[0].unknownStrictAttributeIsError)
        << "the default must be TODAY's behavior — a key that tightens on "
           "absence would change every shipped config's meaning silently";
}

// A non-bool → C_InvalidSemantics. `"true"` is the realistic typo, and JSON
// truthiness would happily accept it as a non-empty string.
TEST(GrammarSchema, UnknownStrictAttributeIsErrorNonBoolReportsInvalid) {
    auto const cfg = inferSchemaWithDeclRow(
        R"({ "rule": "vdecl", "head": 0, "declaratorList": 1,
             "kind": "variable", "unknownStrictAttributeIsError": "true" })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "a stringly-typed boolean must fail the load, not be coerced";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// `$`-prefixed documentation keys stay EXEMPT on a row.
TEST(GrammarSchema, DeclarationRowDollarPrefixedKeyIsExempt) {
    auto const cfg = inferSchemaWithDeclRow(
        R"({ "rule": "vdecl", "declaratorList": 1, "kind": "variable",
             "inferTypeFromInitializer": true,
             "$kindComment": "variables only; functions are a separate row" })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value())
        << "a '$'-prefixed documentation key must not trip the typo "
           "discriminator: "
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
    EXPECT_EQ((*r)->semantics().declarations.size(), 1u);
}

// ── `keywords[]` ENTRY closed key vocabulary (typo discriminator) ─────────
//
// The last per-entry table in this loader that had none: the block type-checked
// `word`/`kind`/`contextual` and rejected misplaced `modeOp`/`stringStyle`, but
// ran no vocabulary check, so any OTHER key was silently ignored.
//
// ★ WHY THIS ENTRY IS THE SHARP ONE. `contextual` is an opt-in whose absence is
// meaningful and whose DEFAULT IS THE DANGEROUS VALUE. A near-miss spelling
// loaded clean, left the flag false, and installed the keyword as a HARD
// RESERVED WORD — the exact opposite of the authored intent — so user code that
// legally uses the word as an identifier starts failing with no diagnostic
// anywhere. Every pin below therefore checks the DIAGNOSTIC, not merely that the
// load failed: "it failed" is equally true of a config that failed for an
// unrelated reason.

namespace {
// The minimal loadable document from §keywords, with one keyword ENTRY body
// spliced at %X%. `Identifier` is the root shape so no entry's `kind` has to be
// referenced by a shape — the pins are about KEYS, and a shape reference would
// add a second reason for the load to fail.
[[nodiscard]] std::string keywordEntrySchemaWith(std::string_view entryBody) {
    std::string cfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "KwKeys", "version": "0.1.0" },
      "keywords": [ %X% ],
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";
    auto const pos = cfg.find("%X%");
    cfg.replace(pos, 3, entryBody);
    return cfg;
}

// The same minimal document with a whole top-level BLOCK spliced RAW at %X%.
// The entry-level helper above cannot express a `keywords` whose value is not
// an array, which is exactly what the type pins below need.
[[nodiscard]] std::string keywordBlockSchemaWith(std::string_view rawBlock) {
    std::string cfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "KwKeys", "version": "0.1.0" },
      %X%
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";
    auto const pos = cfg.find("%X%");
    cfg.replace(pos, 3, rawBlock);
    return cfg;
}

// Does ANY diagnostic carry this substring? Used to pin the MESSAGE, because a
// code alone cannot distinguish "rejected the typo'd key" from "rejected for
// some other reason that happens to share the code".
[[nodiscard]] bool hasDiagMessage(std::vector<ConfigDiagnostic> const& diags,
                                  std::string_view needle) {
    return std::ranges::any_of(diags, [needle](auto const& d) {
        return d.message.find(needle) != std::string::npos;
    });
}
} // namespace

// BASELINE, and it carries the "not too tight" half of the pin: ALL THREE legal
// keys load, and `contextual` still reaches `LexemeMeaning`. A vocabulary that
// omitted one of these would break every shipped language at LOAD, so this is
// not a courtesy test — it is the guard against over-tightening.
TEST(GrammarSchema, KeywordEntryClosedKeyBaselineLoadsAllThreeLegalKeys) {
    auto const cfg = keywordEntrySchemaWith(
        R"({ "word": "if", "kind": "IfKw" },
            { "word": "await", "kind": "AwaitKw", "contextual": true })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value()) << errorDiags(r.error());
    auto const ifKw = (*r)->lookupLexeme("if");
    ASSERT_EQ(ifKw.size(), 1u);
    EXPECT_FALSE(ifKw[0].contextual);
    auto const awaitKw = (*r)->lookupLexeme("await");
    ASSERT_EQ(awaitKw.size(), 1u);
    EXPECT_TRUE(awaitKw[0].contextual)
        << "the baseline must prove `contextual` is READ, not merely tolerated — "
           "otherwise a vocabulary that accepted the key while the loader ignored "
           "it would pass this test";
}

// ★ THE DEFECT THIS CLOSES, in its most damaging spelling. `contextul` used to
// load clean and turn a soft keyword into a hard reserved word.
TEST(GrammarSchema, KeywordEntryMisspelledContextualReportsConflict) {
    auto const cfg = keywordEntrySchemaWith(
        R"({ "word": "await", "kind": "AwaitKw", "contextul": true })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "a typo'd `contextual` must fail the load — silently shipping the "
           "keyword as HARD when the author declared it SOFT is the "
           "knob-that-lies, and it breaks user code that uses the word as an "
           "identifier";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_ConflictingField));
    EXPECT_TRUE(hasDiagMessage(r.error(), "unknown key 'contextul' in a "
                                          "'keywords' entry"))
        << "the pin is on the DIAGNOSTIC, not on the failure: the message must "
           "name the offending key. Got:" << errorDiags(r.error());
}

// Case is part of the spelling — `Contextual` is a different key, and JSON has
// no case-folding rule that would make it the same one.
TEST(GrammarSchema, KeywordEntryCasedContextualReportsConflict) {
    auto const cfg = keywordEntrySchemaWith(
        R"({ "word": "await", "kind": "AwaitKw", "Contextual": true })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagMessage(r.error(), "unknown key 'Contextual'"))
        << errorDiags(r.error());
}

// A second axis: the vocabulary is a VOCABULARY, not one special-cased name. A
// trailing-underscore near-miss on `contextual` and a misspelling of the
// REQUIRED `word` are both caught.
TEST(GrammarSchema, KeywordEntryTrailingUnderscoreKeyReportsConflict) {
    auto const cfg = keywordEntrySchemaWith(
        R"({ "word": "await", "kind": "AwaitKw", "contextual_": true })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagMessage(r.error(), "unknown key 'contextual_'"))
        << errorDiags(r.error());
}

// ★ WHY THE VOCABULARY RUNS BEFORE THE REQUIRED-FIELD CHECK. With only the
// required-field rule, `wrd` was reported as a MISSING 'word' — a field the
// author demonstrably DID write — which sends them hunting in the wrong place.
// Both diagnostics must now fire, and the typo must be NAMED.
TEST(GrammarSchema, KeywordEntryMisspelledWordNamesTheTypoNotJustTheAbsence) {
    auto const cfg = keywordEntrySchemaWith(
        R"({ "wrd": "await", "kind": "AwaitKw" })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagMessage(r.error(), "unknown key 'wrd'"))
        << "the required-'word' diagnostic alone names a field the author DID "
           "write; the vocabulary must run first and name the key. Got:"
        << errorDiags(r.error());
    EXPECT_TRUE(hasDiagMessage(r.error(),
                               "keyword entry needs string 'word' and string 'kind'"))
        << "and the required-field rule must still fire — the vocabulary check "
           "reports, it does not swallow. Got:" << errorDiags(r.error());
}

// The two MISPLACED keys keep their own, more specific redirect and must NOT be
// reported as "unknown": this loader knows exactly what `modeOp` and
// `stringStyle` are and which block they belong on, so calling them unknown
// would be a false statement in a diagnostic. This pins the ORDER of the two
// checks, which is the only thing that makes that true.
TEST(GrammarSchema, KeywordEntryMisplacedModeOpKeepsItsRedirectDiagnostic) {
    auto const cfg = keywordEntrySchemaWith(
        R"({ "word": "if", "kind": "IfKw", "modeOp": "pushMode", "modeArg": "main" })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagMessage(r.error(), "keywords cannot switch lexer modes"))
        << errorDiags(r.error());
    EXPECT_FALSE(hasDiagMessage(r.error(), "unknown key 'modeOp'"))
        << "`modeOp` is a key this loader KNOWS — reporting it as unknown would "
           "be false, and it would bury the redirect that says where the entry "
           "belongs. Got:" << errorDiags(r.error());
}

TEST(GrammarSchema, KeywordEntryMisplacedStringStyleKeepsItsRedirectDiagnostic) {
    auto const cfg = keywordEntrySchemaWith(
        R"({ "word": "string", "kind": "StringKw",
             "stringStyle": { "escapeKind": "none", "endsAt": "'" } })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagMessage(r.error(), "keywords are word-shaped"))
        << errorDiags(r.error());
    EXPECT_FALSE(hasDiagMessage(r.error(), "unknown key 'stringStyle'"))
        << errorDiags(r.error());
}

// `$`-prefixed documentation keys stay EXEMPT on a keyword entry too — the same
// carve-out every sibling vocabulary applies.
TEST(GrammarSchema, KeywordEntryDollarPrefixedKeyIsExempt) {
    auto const cfg = keywordEntrySchemaWith(
        R"({ "word": "await", "kind": "AwaitKw", "contextual": true,
             "$contextualComment": "soft: `await` is a valid identifier here" })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value())
        << "a '$'-prefixed documentation key must not trip the typo "
           "discriminator: " << errorDiags(r.error());
    auto const awaitKw = (*r)->lookupLexeme("await");
    ASSERT_EQ(awaitKw.size(), 1u);
    EXPECT_TRUE(awaitKw[0].contextual);
}

// ── the top-level blocks that were SILENTLY SKIPPED on a WRONG TYPE ───────
//
// A SECOND defect found in the same `if` statement while closing the vocabulary
// above, and measured rather than assumed. Every OTHER top-level block ENTERS
// its branch and then DIAGNOSES a wrong type — `tokens` emits "'tokens' must be
// an object", and `lexerModes` / `operators` / `syncTokens` / `shapes` /
// `semantics` all do the same. `keywords` and `scopes` alone folded the type
// test INTO the `if` CONDITION:
//
//     if (doc.contains("keywords") && doc.at("keywords").is_array()) { … }
//
// so a present-but-wrong-type value fell out of the dispatch entirely and the
// document loaded PERFECTLY CLEAN with the whole table dropped — every keyword
// silently demoted to a plain `Identifier`, or every scope-validity rule gone.
// Same shape as the vocabulary gap: the authored intent and the shipped
// behaviour are opposites and nothing says so.
//
// ⚠ THE COUNTERPART FORM IS PINNED TOO. Both blocks are OPTIONAL, so ABSENT
// must stay silent, and `scopes` without `validity` must stay silent. A check
// that also reds on absence is a different bug wearing this fix's clothes, and
// only the positive pins can tell the two apart.

TEST(GrammarSchema, KeywordsBlockNonArrayFailsLoud) {
    auto const cfg = keywordBlockSchemaWith(R"("keywords": { "if": "IfKw" },)");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "a `keywords` that is not an array must FAIL, not vanish: dropping "
           "the whole keyword table turns every keyword in the language into a "
           "plain Identifier with no diagnostic";
    EXPECT_TRUE(hasDiagMessage(r.error(), "'keywords' must be an array"))
        << errorDiags(r.error());
}

// A second wrong type, so the check is about the TYPE and not about objects.
TEST(GrammarSchema, KeywordsBlockStringValueFailsLoud) {
    auto const cfg = keywordBlockSchemaWith(R"("keywords": "if",)");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagMessage(r.error(), "'keywords' must be an array"))
        << errorDiags(r.error());
}

// ABSENT `keywords` is legal — a language may have none.
TEST(GrammarSchema, KeywordsBlockAbsentStaysSilent) {
    auto r = GrammarSchema::loadFromText(keywordBlockSchemaWith(""));
    ASSERT_TRUE(r.has_value()) << errorDiags(r.error());
}

// An EMPTY array is also legal, and it is the case a naive "is it there and
// non-trivial" check would wrongly reject.
TEST(GrammarSchema, KeywordsBlockEmptyArrayStaysSilent) {
    auto r = GrammarSchema::loadFromText(keywordBlockSchemaWith(R"("keywords": [],)"));
    ASSERT_TRUE(r.has_value()) << errorDiags(r.error());
}

// ★ The `scopes` twin. ⚠ `semantics.scopes` IS an array (every shipped config
// declares one) — this is the TOP-LEVEL `scopes`, whose one member is
// `validity`. Confusing the two is the reason the wrong-type value looks
// plausible enough to write by accident.
TEST(GrammarSchema, ScopesBlockNonObjectFailsLoud) {
    auto const cfg = keywordBlockSchemaWith(R"("scopes": [ "block" ],)");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "a top-level `scopes` that is not an object must FAIL: silently "
           "dropping it disables every scope-validity rule the author wrote";
    EXPECT_TRUE(hasDiagMessage(r.error(), "'scopes' must be an object"))
        << errorDiags(r.error());
}

TEST(GrammarSchema, ScopesValidityNonArrayFailsLoud) {
    auto const cfg =
        keywordBlockSchemaWith(R"("scopes": { "validity": { "scope": "B" } },)");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "`validity` is the ONLY member `scopes` has — a wrong type there "
           "empties the block just as completely as a wrong-typed `scopes`";
    EXPECT_TRUE(hasDiagMessage(r.error(), "'validity' must be an array"))
        << errorDiags(r.error());
}

// `scopes` present with NO `validity`, and `scopes` absent: both legal.
TEST(GrammarSchema, ScopesBlockWithoutValidityStaysSilent) {
    auto r = GrammarSchema::loadFromText(keywordBlockSchemaWith(R"("scopes": {},)"));
    ASSERT_TRUE(r.has_value()) << errorDiags(r.error());
}

TEST(GrammarSchema, ScopesBlockEmptyValidityStaysSilent) {
    auto r = GrammarSchema::loadFromText(
        keywordBlockSchemaWith(R"("scopes": { "validity": [] },)"));
    ASSERT_TRUE(r.has_value()) << errorDiags(r.error());
}

// ── `language` BLOCK closed key vocabulary (typo discriminator) ──────────
//
// ⚠⚠ THIS BLOCK HAD NO DISCRIMINATOR AT ALL until 2026-08-20, and the absence
// had already bent the schema rather than merely leaving a diagnostic unfired:
// `isa` and `identifierClass` sit at TOP LEVEL — where neither belongs — for the
// stated reason that `language` could not check them, so language-scoped keys
// were pushed out of the language block to borrow a check living elsewhere.
// The direct failure was silent in the worst way: `fileExtensons` loaded
// perfectly clean and produced an EMPTY extension list, i.e. a language that
// recognises no source file, reported as nothing whatsoever.
// D-CONFIG-GRAMMAR-LANGUAGE-BLOCK-HAS-NO-TYPO-DISCRIMINATOR.

namespace {
// A minimal loadable document whose `language` block carries one extra key.
[[nodiscard]] std::string languageBlockWith(std::string_view extra) {
    std::string cfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "LangKeys", "version": "0.1.0"%X% },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } }
    })JSON";
    auto const pos = cfg.find("%X%");
    cfg.replace(pos, 3, extra);
    return cfg;
}
} // namespace

// Baseline, and it is not decoration: without it a mutant that rejects EVERY
// key would still turn the negative cases below green, and the pin would be
// asserting nothing.
TEST(GrammarSchema, LanguageBlockClosedKeyBaselineLoadsCleanly) {
    auto r = GrammarSchema::loadFromText(languageBlockWith(
        R"(, "fileExtensions": [".lk"])"));
    ASSERT_TRUE(r.has_value())
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
}

// The exact real-world shape: one transposed letter in the OPTIONAL key, whose
// absence means "no extensions". This is the case that used to load clean.
TEST(GrammarSchema, LanguageBlockMisspelledFileExtensionsReportsMalformed) {
    auto r = GrammarSchema::loadFromText(languageBlockWith(
        R"(, "fileExtensons": [".lk"])"));
    ASSERT_FALSE(r.has_value())
        << "a misspelled 'fileExtensions' must fail the load — it yields an "
           "EMPTY extension list, i.e. a language matching no source file";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// A second, differently-spelled key, so the pin asserts the VOCABULARY rather
// than one special-cased name.
TEST(GrammarSchema, LanguageBlockUnknownKeyReportsMalformed) {
    auto r = GrammarSchema::loadFromText(languageBlockWith(
        R"(, "identifierClass": "unicode")"));
    ASSERT_FALSE(r.has_value())
        << "'identifierClass' is a TOP-LEVEL key; inside 'language' it is read "
           "by nothing and must not be accepted in silence";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// `$`-prefixed documentation keys stay EXEMPT here too — the carve-out is
// codebase-wide, and a check that refused them would red every shipped
// document that annotates its own language block.
TEST(GrammarSchema, LanguageBlockDocumentationKeyStaysExempt) {
    auto r = GrammarSchema::loadFromText(languageBlockWith(
        R"(, "$comment": "why this language exists", "$originComment": "x")"));
    ASSERT_TRUE(r.has_value())
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
}

// ── TOP-LEVEL document closed key vocabulary (typo discriminator) ────────
//
// The widest-blast-radius instance: every block is optional and absence is
// MEANINGFUL, so a misspelled block name used to load clean and silently
// drop that entire phase.

namespace {
// A minimal loadable document with one extra top-level key spliced at %X%.
[[nodiscard]] std::string documentSchemaWith(std::string_view extra) {
    std::string cfg = R"JSON({
      %X%
      "dssSchemaVersion": 4,
      "language": { "name": "DocKeys", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } }
    })JSON";
    auto const pos = cfg.find("%X%");
    cfg.replace(pos, 3, extra);
    return cfg;
}
} // namespace

// Baseline: the skeleton with no extra key loads clean.
TEST(GrammarSchema, DocumentClosedKeyBaselineLoadsCleanly) {
    auto r = GrammarSchema::loadFromText(documentSchemaWith(""));
    ASSERT_TRUE(r.has_value())
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
}

// A misspelled top-level BLOCK must fail the load. `semantcs` is the worst
// case in the whole defect class: the loader's contract is "absent ⇒ the
// analyzer performs no semantic analysis", so this used to load perfectly
// clean and silently disable semantic analysis entirely.
TEST(GrammarSchema, DocumentUnknownTopLevelKeyReportsMalformed) {
    auto const cfg = documentSchemaWith(R"("semantcs": { "scopes": [] },)");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "a misspelled top-level block must fail the load — silently "
           "dropping the whole phase is the knob-that-lies";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// A second misspelling on a different block, to pin the vocabulary rather
// than one special-cased name.
TEST(GrammarSchema, DocumentMisspelledShapesKeyReportsMalformed) {
    auto const cfg = documentSchemaWith(R"("shape": { "x": {} },)");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// `$`-prefixed documentation keys stay EXEMPT at document level too.
TEST(GrammarSchema, DocumentDollarPrefixedKeyIsExempt) {
    auto const cfg = documentSchemaWith(
        R"("$comment": "the minimal document used by the closed-key pins",)");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value())
        << "a '$'-prefixed documentation key must not trip the typo "
           "discriminator: "
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
}

// ── the `$` convention at the REMAINING key-iteration sites ───────────────
//
// The shapes-map fix closed ONE instance and the cycle asserted the class was
// closed with it ("every site now calls isDocumentationKey"). It was not: five
// more loops read an object's keys and rejected a `$comment` with a HARD LOAD
// FAILURE, and two more read them AS IDENTIFIERS and silently MINTED the
// documentation key as a real one. Each pin below is a site that was measured
// broken; together they are what makes the class claim true rather than
// asserted. The two shapes — closed-key vocabulary vs. identifier-valued map —
// need different assertions, so both are spelled out per site.

// (1) `tokens` — NOT a site for the convention, and the pins below say why.
//
// ★ This was the audit finding that turned out to be REFUTED, and refuting it
// mattered: a key in the `tokens` map is a LEXEME — arbitrary SOURCE TEXT the
// target language defines — not an identifier out of a vocabulary this loader
// owns. Adding the `$` carve-out here was MEASURED to break
// `core/test_operator_table` (a language whose `$` is an infix operator) and
// seven `core/test_lexer_modes` cases (C#-style `$"…"` interpolation and `$$`).
// A source-agnostic frontend cannot reserve a punctuation character, so the
// exemption was reverted and these pins keep it reverted.
TEST(GrammarSchema, TokensMapDollarLexemeIsARealOperatorNotAComment) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "DollarLang", "version": "0.1.0" },
      "tokens": { "$": [{ "kind": "DollarOp" }],
                  "$$": [{ "kind": "DoubleDollarOp" }],
                  ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(r.has_value()) << errorDiags(r.error());
    EXPECT_EQ((*r)->lookupLexeme("$").size(), 1u)
        << "'$' is an ordinary operator character in many languages — the "
           "documentation-key convention must NOT reach a map whose keys are "
           "source text, or declaring it silently deletes the operator";
    EXPECT_EQ((*r)->lookupLexeme("$$").size(), 1u);
}

// The per-MODE token override table shares that key space, so it must behave
// identically. Pinned separately because it is a separate loop that a future
// "apply the convention everywhere" sweep would find on its own.
TEST(GrammarSchema, PerModeTokensMapDollarLexemeIsHonored) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 2,
      "language": { "name": "DollarLang", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "lexerModes": {
        "alt": { "tokens": { "$$": [{ "kind": "DoubleDollarOp" }] } }
      },
      "shapes": { "root": { "sequence": [ "Semi" ] } }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(r.has_value()) << errorDiags(r.error());
    auto const mode = (*r)->findLexerMode("alt");
    ASSERT_TRUE(mode.valid());
    EXPECT_EQ((*r)->lookupLexemeInMode(mode, "$$").size(), 1u)
        << "the per-mode token table has the same source-text key space as the "
           "global one and must not reserve '$' either";
}

// `linkageSpecifiers` is the third map keyed by SOURCE TEXT (its keys are
// matched against `tree().text(token)`), so it gets the same treatment — the
// audit listed it as a missing carve-out, and it is deliberately absent.
// Documentation for that map goes on a `$`-prefixed sibling of the declaration
// ROW's keys, which IS the loader's own vocabulary; the shipped c-subset's
// `$linkageSpecifiersComment` already does exactly that.
TEST(GrammarSchema, LinkageSpecifiersMapDollarKeyIsASpecifierNotAComment) {
    auto cfg = attrVocabSchema(kConsistentEffects, kIgnoresDeprecated, "");
    constexpr std::string_view kNeedle =
        R"("linkageSpecifiers": { "st": { "binding": "local" } })";
    auto const pos = cfg.find(kNeedle);
    ASSERT_NE(pos, std::string::npos);
    cfg.replace(pos, kNeedle.size(),
                R"("linkageSpecifiers": { "$st": { "binding": "local" },
                                          "st":  { "binding": "local" } })");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value()) << errorDiags(r.error());
    auto const& ls = (*r)->semantics().declarations[0].linkageSpecifiers;
    EXPECT_EQ(ls.size(), 2u)
        << "a specifier whose source spelling begins with '$' must remain "
           "declarable — this key space is source text, not loader vocabulary";
    EXPECT_TRUE(ls.contains("$st"));
}

// (2) `lexerModes` — the other identifier-valued map. A `$comment` here used to
// REGISTER a lexer mode named `$comment`.
TEST(GrammarSchema, LexerModesMapDollarPrefixedKeyIsExempt) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "ModeKeys", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "lexerModes": {
        "$comment": "the mode table",
        "line-comment": { "defaultToken": { "kind": "CommentBody" } }
      },
      "shapes": { "root": { "sequence": [ "Semi" ] } }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(r.has_value())
        << "a '$'-prefixed documentation key in the lexerModes map must not be "
           "registered as a mode: " << errorDiags(r.error());
    EXPECT_FALSE((*r)->findLexerMode("$comment").valid())
        << "the documentation key must not be REGISTERED as a lexer mode";
}

// (3) `semantics.pointerAliasing` — a CLOSED-key map: every key must be one of
// its declared fields, and an unrecognized one is C_InvalidSemantics. A
// `$comment` can therefore never be a legal key, so skipping it forecloses
// nothing — and the closed-key rejection must not fire ON the documentation key.
//
// ⓘ This case previously used `semantics.externLibraryByFormat`, retired in
// UCRT-P4 (Decision 1) along with the per-language library default it carried.
// The PROPERTY under test is the `$`-key exemption on a closed-key semantics map,
// not that field — so it is retargeted at a surviving one rather than deleted,
// keeping the enumeration's case (3) covered.
TEST(GrammarSchema, ClosedKeySemanticsMapDollarPrefixedKeyIsExempt) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": {
        "pointerAliasing": { "$comment": "the aliasing lattice",
                             "charTypesAliasAll": true }
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(r.has_value())
        << "a '$'-prefixed documentation key must not be judged against the "
           "closed field set: " << errorDiags(r.error());
    EXPECT_TRUE((*r)->semantics().pointerAliasing.charTypesAliasAll)
        << "and the real field beside it must still be read";
}

// (4) `declarations[].gatedMarkers[]` — a closed-key vocabulary that rejected
// `$comment` outright.
TEST(GrammarSchema, GatedMarkerDollarPrefixedKeyIsExempt) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }], "!": [{ "kind": "Bang" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] },
                  "vdecl": { "sequence": [ "Identifier", "Semi" ] } },
      "semantics": {
        "identifierToken": "Identifier",
        "declarations": [
          { "rule": "vdecl", "name": 0, "kind": "variable",
            "gatedMarkers": [ { "$comment": "not supported yet",
                                "token": "Bang",
                                "code": "H_UnknownLinkageSpecifier" } ] }
        ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(r.has_value())
        << "a '$'-prefixed documentation key on a gatedMarkers entry must not "
           "trip the typo discriminator: " << errorDiags(r.error());
}

// (5) `numberStyle.exponent` — a closed-key vocabulary shared by the top-level
// exponent block and every prefix's `float.exponent`, so one exemption covers
// both readers.
TEST(GrammarSchema, NumberStyleExponentDollarPrefixedKeyIsExempt) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }], "#": [{ "kind": "Num" }],
                  "~": [{ "kind": "Flt" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "numberStyle": {
        "emitKind": { "integer": "Num", "float": "Flt" },
        "exponent": { "$comment": "both e and E introduce an exponent",
                      "letters": ["e", "E"], "signOptional": true }
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(r.has_value())
        << "a '$'-prefixed documentation key in numberStyle.exponent must not "
           "trip the typo discriminator: " << errorDiags(r.error());
}

// ANTI-OVER-SKIP for that same block: the real typo it exists to catch must
// still fail. `signOptionl` is the misspelling the discriminator was written
// for, and it must not have been widened away by the `$` exemption.
TEST(GrammarSchema, NumberStyleExponentMisspelledKeyStillReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }], "#": [{ "kind": "Num" }],
                  "~": [{ "kind": "Flt" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "numberStyle": {
        "emitKind": { "integer": "Num", "float": "Flt" },
        "exponent": { "letters": ["e"], "signOptionl": true }
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value())
        << "a misspelled exponent key must still fail — the '$' exemption "
           "must not widen into accepting any unknown key";
    bool namesTheKey = false;
    for (auto const& d : r.error()) {
        if (d.message.find("signOptionl") != std::string::npos) namesTheKey = true;
    }
    EXPECT_TRUE(namesTheKey)
        << "the diagnostic must name the misspelled key — a load that fails "
           "for some other reason would satisfy a weaker assertion";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidNumberStyle));
}

// (6) a shape's `commitRequiresTypeName` guard object — the last closed-key
// vocabulary that rejected `$comment`.
TEST(GrammarSchema, TypeNameCommitGuardDollarPrefixedKeyIsExempt) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": {
        "root":     { "sequence": [ "Semi" ] },
        "typeName": { "sequence": [ "Identifier" ] },
        "vdecl":    { "sequence": [ "typeName", "Identifier", "Semi" ],
                      "commitRequiresTypeName": {
                        "$comment": "only commit once a real type name is seen",
                        "rule": "typeName",
                        "polarity": "requireKnownType" } }
      },
      "semantics": { "identifierToken": "Identifier" }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(r.has_value())
        << "a '$'-prefixed documentation key in commitRequiresTypeName must "
           "not trip the typo discriminator: " << errorDiags(r.error());
}

// ── the `$` convention inside the `shapes` MAP ────────────────────────────
//
// The `$comment` convention was honored at the document top level, inside
// `semantics`, inside `attributeSemantics`, on every `declarations[]` row and
// inside an individual shape BODY — but NOT as a sibling of the rule names in
// the `shapes` map, where the key was read as a SHAPE DEFINITION and its prose
// value as a rule REFERENCE. The whole load failed with the paragraph printed
// back as if it were a rule name. Hit for real while authoring c-subset.

namespace {
// One extra entry spliced into the `shapes` map at %X%, ahead of `root`.
[[nodiscard]] std::string shapesMapSchemaWith(std::string_view extraEntry) {
    std::string cfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "ShapeKeys", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": {
        %X%
        "root": { "sequence": [ "Semi" ] }
      }
    })JSON";
    cfg.replace(cfg.find("%X%"), 3, extraEntry);
    return cfg;
}
} // namespace

// Baseline: the skeleton with no extra entry loads clean.
TEST(GrammarSchema, ShapesMapBaselineLoadsCleanly) {
    auto r = GrammarSchema::loadFromText(shapesMapSchemaWith(""));
    ASSERT_TRUE(r.has_value())
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
}

// (a) A `$`-prefixed key at the shapes-MAP level is documentation: the load
// succeeds AND no rule by that name is minted. ★ Both halves matter. A fix
// that only stopped the reference check would still INTERN the key, leaving a
// junk rule in the table that nothing defines — greppable from the outside as
// a real shape, and a `nameMatch`/`kindByChild` typo could then "resolve" to
// it. Asserting only "loads clean" would pass for that broken fix.
TEST(GrammarSchema, ShapesMapDollarPrefixedKeyIsExempt) {
    auto const cfg = shapesMapSchemaWith(
        R"("$rootComment": "root is a lone statement terminator — this is prose, not a rule",)");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(r.has_value())
        << "a '$'-prefixed documentation key in the shapes map must not be "
           "read as a shape definition: "
        << errorDiags(r.error());
    EXPECT_FALSE((*r)->rules().contains("$rootComment"))
        << "the documentation key must not be INTERNED as a rule — a junk "
           "rule that nothing defines is exactly the silent state the "
           "reference check exists to prevent";
}

// (b) ANTI-OVER-SKIP. A non-`$` shapes entry whose body is a bad reference
// must still fail loud. Without this pin the fix could be "skip more than
// intended" — silently dropping real, misspelled shapes — and nothing would
// notice. The exemption is for the `$` sigil, not for string-valued entries.
TEST(GrammarSchema, ShapesMapNonDollarBadEntryStillReportsUnknownShape) {
    auto const cfg = shapesMapSchemaWith(
        R"("notAShape": "this prose is not a rule name",)");
    auto r = GrammarSchema::loadFromText(cfg);
    ASSERT_FALSE(r.has_value())
        << "a NON-'$' shapes entry naming an unresolvable reference must "
           "still fail the load — the '$' exemption must not widen into "
           "skipping real shapes";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_UnknownShape));
}

// (c) THE FILTER MUST NOT SWALLOW THE MISSING-ROOT CHECK. A `shapes` block
// holding NOTHING BUT documentation keys is a block the author DID write, and
// it declares no `root` — so it must fail exactly as any other rootless
// non-empty block does.
//
// ★ This is the pin the `$` filter itself broke, and it is the one shape the
// exemption pin above cannot see (that fixture always keeps a real `root`, so
// the filtered and unfiltered maps are both non-empty and the two tests agree
// no matter which one the check reads). The failure it guards is not cosmetic:
// an author bisecting a grammar comments shapes out one at a time, and the last
// state before "nothing left" is a lone `$wipComment`. Testing the FILTERED map
// makes that state report SUCCESS with `rootRule` invalid — a compiler holding
// no grammar at all, and no diagnostic anywhere saying so.
TEST(GrammarSchema, ShapesMapOfOnlyDocumentationKeysStillDemandsRoot) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "ShapeKeys", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "$comment": "everything commented out while I bisect" }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value())
        << "a 'shapes' block the author wrote — even one holding only "
           "documentation keys — declares no 'root' and must say so; loading "
           "clean leaves the compiler with NO grammar and no diagnostic";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_MissingField));
    bool namesShapes = false;
    for (auto const& d : r.error()) {
        if (d.code == DiagnosticCode::C_MissingField && d.path == "/shapes"
            && d.message.find("root") != std::string::npos) {
            namesShapes = true;
        }
    }
    EXPECT_TRUE(namesShapes)
        << "the diagnostic must be the missing-'root' one AT '/shapes' — a "
           "load that merely fails for some other reason would pass a weaker "
           "assertion while the regression stayed live";
}

// (d) THE EXEMPTION BOUNDARY. An EMPTY `shapes` object still loads. A config
// that has not reached its grammar yet has always been allowed to say so, and
// (c) must not widen into "every config must declare a root".
TEST(GrammarSchema, ShapesMapEmptyObjectStillLoads) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "ShapeKeys", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(r.has_value())
        << "an EMPTY shapes block is a config that has not reached its grammar "
           "yet and must keep loading: "
        << errorDiags(r.error());
}

// ── c35 D-CSUBSET-FORWARD-STRUCT-DECLARATION: references[].compositeKind ──────
//
// `compositeKind` on a `references[]` row drives the opaque-tag forward-mint
// (struct/union mint an incomplete tag on a miss; enum keeps the fail-loud miss).
// It is meaningful ONLY on a tag-reference row, and only for the three composite
// kinds — each misconfiguration gets a red-on-disable loader pin (the
// guard-needs-a-red-test discipline).

// `compositeKind` on a NON-tag-reference row → C_InvalidSemantics (it would be a
// silently-ignored field — a config author would expect it to do something).
TEST(GrammarSchema, ReferenceCompositeKindOnNonTagRowReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": {
        "root": { "sequence": [ "ref", "Semi" ] },
        "ref": { "sequence": [ "Identifier" ] }
      },
      "semantics": {
        "references": [ { "rule": "ref", "compositeKind": "struct" } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// An UNKNOWN `compositeKind` string on a tag-reference row → C_InvalidSemantics
// (a typo must not silently default to Struct and mis-mint a union/enum tag).
TEST(GrammarSchema, ReferenceCompositeKindUnknownValueReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": {
        "root": { "sequence": [ "ref", "Semi" ] },
        "ref": { "sequence": [ "Identifier" ] }
      },
      "semantics": {
        "references": [ { "rule": "ref", "isTagReference": true,
                          "compositeKind": "telepathic" } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// POSITIVE: a tag-reference row WITH a valid `compositeKind` loads clean (the
// happy path the negatives above bracket).
TEST(GrammarSchema, ReferenceCompositeKindOnTagRowLoadsClean) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": {
        "root": { "sequence": [ "ref", "Semi" ] },
        "ref": { "sequence": [ "Identifier" ] }
      },
      "semantics": {
        "references": [ { "rule": "ref", "isTagReference": true,
                          "compositeKind": "union" } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    EXPECT_TRUE(r.has_value())
        << "a tag-reference row with a valid compositeKind must load clean";
}

// ── D-LK10-ENTRY-MAIN-IMPLICIT-RETURN loader negative tests ───────
//
// `implicitReturnZeroForFunctionNames` on `DeclarationRule` accepts
// an array of non-empty strings. Each malformed shape must emit
// `C_InvalidSemantics` so a typo in a language config can't silently
// produce an empty list (which would make the implicit-return-0
// rule never fire for that language).

TEST(GrammarSchema, SemanticsImplicitReturnZeroNonArrayReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": {
        "declarations": [ { "rule": "root", "name": 0, "kind": "function",
                            "implicitReturnZeroForFunctionNames": "main" } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

TEST(GrammarSchema, SemanticsImplicitReturnZeroNonStringElementReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": {
        "declarations": [ { "rule": "root", "name": 0, "kind": "function",
                            "implicitReturnZeroForFunctionNames": ["main", 42] } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// D-CONFIG-LOADER-UNKNOWN-KEYS-FAIL-LOUD pins for the
// `semantics.pointerAliasing` block (D-OPT-LOAD-ALIAS-ANALYSIS arc,
// cycle 10g). A typo'd sub-key (e.g. `strictAliasng` missing 'i' or
// `charTypeAliasAll` missing 's') would otherwise silently fall back
// to the default and flip the language's optimization polarity —
// strict-aliasing silently disabled for c-subset / char-exception
// silently disabled for a hypothetical Rust frontend.
TEST(GrammarSchema, SemanticsPointerAliasingUnknownKeyReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": {
        "pointerAliasing": {
          "strictAliasng": true
        }
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value())
        << "typo'd key under pointerAliasing must reject; otherwise the "
           "language's optimization polarity silently flips";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

TEST(GrammarSchema, SemanticsPointerAliasingUnknownKeyCharTypoReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": {
        "pointerAliasing": {
          "charTypeAliasAll": true
        }
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value())
        << "typo'd key `charTypeAliasAll` (missing 's') must reject";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

TEST(GrammarSchema, SemanticsPointerAliasingNonObjectReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": {
        "pointerAliasing": true
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

TEST(GrammarSchema, SemanticsPointerAliasingNonBoolFieldReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": {
        "pointerAliasing": {
          "strictAliasingOnDistinctTypes": "yes"
        }
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value())
        << "non-bool field value must reject (a typo'd literal otherwise "
           "silently parses as truthy)";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// Positive complement: a CORRECTLY-spelled `pointerAliasing` block
// with both fields loads cleanly. Pairs with the typo tests so a
// future loader regression that REJECTS the canonical shape (over-
// correcting to "any pointerAliasing block invalid") fails this arm.
TEST(GrammarSchema, SemanticsPointerAliasingCanonicalShapeLoadsCleanly) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": {
        "pointerAliasing": {
          "strictAliasingOnDistinctTypes": true,
          "charTypesAliasAll": false
        }
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(r.has_value())
        << "canonical pointerAliasing shape must load — see error count="
        << (r.has_value() ? 0u : r.error().size());
    EXPECT_TRUE((*r)->semantics().pointerAliasing.strictAliasingOnDistinctTypes);
    EXPECT_FALSE((*r)->semantics().pointerAliasing.charTypesAliasAll);
}

TEST(GrammarSchema, SemanticsImplicitReturnZeroEmptyStringElementReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": {
        "declarations": [ { "rule": "root", "name": 0, "kind": "function",
                            "implicitReturnZeroForFunctionNames": ["main", ""] } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// Positive pin: multi-name lists load cleanly and preserve order +
// identity. The substrate accepts an arbitrary-length vector, but
// c-subset's shipped config declares only `["main"]`, so the
// multi-element path has zero in-shipped-config coverage. A
// regression that silently truncated the list to the first element
// would pass every negative test + every e2e test (c-subset's single
// "main" entry still works). Code-architect Q10-A4 FOLD-NOW on
// 39897eb's 3rd-order audit.
TEST(GrammarSchema, SemanticsImplicitReturnZeroMultiNameListLoadsCleanly) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": {
        "declarations": [ { "rule": "root", "name": 0, "kind": "function",
                            "implicitReturnZeroForFunctionNames":
                              ["main", "WinMain", "_start"] } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(r.has_value())
        << "multi-element implicitReturnZeroForFunctionNames must load";
    auto const& decls = (*r)->semantics().declarations;
    ASSERT_EQ(decls.size(), 1u);
    auto const& names = decls[0].implicitReturnZeroForFunctionNames;
    ASSERT_EQ(names.size(), 3u)
        << "all three entries must be preserved — a silent truncation "
           "to the first element would still pass downstream lookup "
           "for `main` but break languages declaring additional names";
    EXPECT_EQ(names[0], "main");
    EXPECT_EQ(names[1], "WinMain");
    EXPECT_EQ(names[2], "_start");
}

// Duplicate entries are a paste-error class — emit C_InvalidSemantics
// per duplicate occurrence. Silent-failure F2 fold (3rd-order audit
// on 39897eb).
TEST(GrammarSchema, SemanticsImplicitReturnZeroDuplicateElementReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": {
        "declarations": [ { "rule": "root", "name": 0, "kind": "function",
                            "implicitReturnZeroForFunctionNames": ["main", "main"] } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// FC5 (D-LK10-ENTRY-MAIN-IMPLICIT-RETURN) + D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE — the
// de-conflation pin. `entryFunctions` (the program-entry MAPPING, read by entry
// resolution) and `implicitReturnZeroForFunctionNames` (the C main-style return-0
// NAME set) load INDEPENDENTLY. They are now different SHAPES as well as different
// concepts, which makes aliasing them impossible by construction — but the pin is
// kept, and strengthened, because it now also proves the mapping's per-name shape
// list survives the load with its VERB intact. A loader that dropped the verb, or
// flattened the mapping back to a name list, fails this.
TEST(GrammarSchema, SemanticsEntryFunctionsLoadIndependentlyFromReturnZeroSet) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": {
        "declarations": [ { "rule": "root", "name": 0, "kind": "function",
                            "implicitReturnZeroForFunctionNames": ["main"],
                            "entryFunctions": {
                              "custom_entry": [ { "returns": "i32", "params": [],
                                                  "verb": "none" } ],
                              "_start": [ { "returns": "i32",
                                            "params": ["i32", "ptr-ptr-char"],
                                            "verb": "argc-argv" } ] } } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(r.has_value())
        << "a config declaring both name lists must load";
    auto const& decls = (*r)->semantics().declarations;
    ASSERT_EQ(decls.size(), 1u);
    // The two fields are SEPARATE — neither aliases the other.
    EXPECT_EQ(decls[0].implicitReturnZeroForFunctionNames,
              (std::vector<std::string>{"main"}));
    // The mapping flattens to a row list. JSON object keys iterate in sorted order
    // (nlohmann's default `json` is an ordered map), so `_start` precedes
    // `custom_entry` — asserted explicitly rather than sorted-for-convenience, so a
    // future change in iteration order is a VISIBLE failure and not a silent
    // reordering of program-entry candidates.
    ASSERT_EQ(decls[0].entryFunctions.size(), 2u);
    EXPECT_EQ(decls[0].entryFunctions[0].name, "_start");
    EXPECT_EQ(decls[0].entryFunctions[0].returns, EntryReturnShape::I32);
    EXPECT_EQ(decls[0].entryFunctions[0].params,
              (std::vector<EntryParamShape>{EntryParamShape::I32,
                                            EntryParamShape::PtrPtrChar}));
    // The VERB is the field the whole format-intersection turns on; a loader that
    // parsed the signature but dropped the verb would leave every entry
    // unrealizable, so it is asserted per row rather than assumed.
    EXPECT_EQ(decls[0].entryFunctions[0].verb, EntryMaterialization::ArgcArgv);
    EXPECT_EQ(decls[0].entryFunctions[1].name, "custom_entry");
    EXPECT_TRUE(decls[0].entryFunctions[1].params.empty());
    EXPECT_EQ(decls[0].entryFunctions[1].verb, EntryMaterialization::None);
}

// FC5 + D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE — `entryFunctions` carries a load-time
// duplicate check, and the mapping makes it a STRONGER claim than the retired name
// list's. A duplicate NAME is now impossible (JSON object keys are unique), so the
// check that matters is a duplicate SIGNATURE under one name: two rows with the same
// shape and different verbs would make declaration ORDER decide which verb an entry
// materializes — a silent wrong-verb on the program entry. Refused at load.
TEST(GrammarSchema, SemanticsEntryFunctionsDuplicateSignatureReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": {
        "declarations": [ { "rule": "root", "name": 0, "kind": "function",
                            "entryFunctions": {
                              "main": [
                                { "returns": "i32",
                                  "params": ["i32", "ptr-ptr-char"],
                                  "verb": "argc-argv" },
                                { "returns": "i32",
                                  "params": ["i32", "ptr-ptr-char"],
                                  "verb": "argc-argv" } ] } } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics));
}

// ── `externLibraryByFormat` IS RETIRED — THE KEY ITSELF IS NOW THE ERROR ─────
//
// This replaces the six tests that pinned the per-language `externLibraryByFormat`
// LOADER (happy path, non-object, unknown key, sentinel key, non-string value,
// empty-string value). That loader is gone: the field was never a fact about a
// LANGUAGE. "Which runtime image owns this symbol" is a fact about a PLATFORM, and
// the shipped-descriptor corpus owns it PER SYMBOL — so one string per language was
// a GUESS and a SECOND OWNER of a fact the corpus already owned. MEASURED
// consequences of the guess: on pe a hand-written
// `extern int printf(const char*, ...);` imported the LEGACY C runtime beside the
// modern one (two C runtimes in one image, no diagnostic at any stage); on elf a
// hand-written `extern double sin(double);` bound `libc.so.6` and the binary died
// at LOAD with "undefined symbol: sin", because glibc ships `sin` in `libm.so.6`.
//
// ★ WHAT THIS ONE TEST PINS THAT SIX PASSING LOADER TESTS COULD NOT: that the key
// cannot COME BACK. Deleting a loader while leaving its key in the closed
// `semantics` vocabulary would leave a silently-ignored config knob — an author
// would write it, see a clean load, and get none of the behaviour. Refusal at LOAD
// is the only outcome that tells them.
//
// RED-ON-DISABLE: re-add "externLibraryByFormat" to the allowed `semantics` key
// list in `grammar_schema_json.cpp` and this load starts succeeding.
TEST(GrammarSchema, SemanticsExternLibraryByFormatKeyIsRefusedAtLoad) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": {
        "externLibraryByFormat": { "pe": "ucrtbase.dll", "elf": "libc.so.6" }
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value())
        << "a language config declaring the retired per-format library default "
           "must be REFUSED, not silently ignored — a silently-ignored config "
           "knob is worse than a missing one";
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidSemantics))
        << errorDiags(r.error());
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

// An ungated `assignments` entry (no operatorToken) sharing a rule with
// another entry → C_ConflictingField. An ungated entry matches every node of
// its rule, so it must be the sole entry for that rule (else it would shadow
// the gated entries under the engine's first-match-wins loop).
TEST(GrammarSchema, SemanticsAssignmentsUngatedMixedWithGatedConflicts) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }], "=": [{ "kind": "Assign" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "assignments": [
        { "rule": "root", "lhs": 0, "rhs": 1 },
        { "rule": "root", "operatorToken": "Assign", "lhs": 0, "rhs": 1 }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_ConflictingField));
}

// Two GATED entries on the same rule (distinct operatorTokens) is fine —
// only the ungated-mixed case conflicts.
TEST(GrammarSchema, SemanticsAssignmentsTwoGatedSameRuleLoadsCleanly) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        ";":  [{ "kind": "Semi" }],
        "=":  [{ "kind": "Assign" }],
        "+=": [{ "kind": "PlusAssign" }]
      },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": { "assignments": [
        { "rule": "root", "operatorToken": "Assign",     "lhs": 0, "rhs": 1 },
        { "rule": "root", "operatorToken": "PlusAssign", "lhs": 0, "rhs": 1 }
      ] }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(r.has_value());
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

// ─── artifactProfiles (plan 06 AP1) ───────────────────────────────────────

namespace {
[[nodiscard]] std::size_t countConfigCode(
    std::vector<ConfigDiagnostic> const& diags, DiagnosticCode code) {
    return static_cast<std::size_t>(
        std::ranges::count_if(diags, [code](ConfigDiagnostic const& d) {
            return d.code == code;
        }));
}
} // namespace

// ── builtinTypes extension form + warnIfUnused (D3 / D8) ───────────────────
// A `builtinTypes` entry may carry `extension: "Name"` (mutually exclusive
// with `core`) mapping a type name to a registered `typeExtensions[]` entry.

// Happy path: an `extension`-form mapping naming a declared typeExtension
// loads cleanly (no diagnostics) and round-trips the resolved name.
TEST(GrammarSchema, BuiltinTypeExtensionFormLoadsCleanly) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "typeExtensions": [ { "name": "Ext::Foo" } ],
      "semantics": {
        "builtinTypes": [ { "name": "MyT", "extension": "Ext::Foo" } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(r.has_value())
        << (r.error().empty() ? "<no diagnostics>" : r.error()[0].message);
    auto const& sem = (*r)->semantics();
    ASSERT_EQ(sem.builtinTypes.size(), 1u);
    EXPECT_EQ(sem.builtinTypes[0].name, "MyT");
    ASSERT_TRUE(sem.builtinTypes[0].extension.has_value());
    EXPECT_EQ(*sem.builtinTypes[0].extension, "Ext::Foo");
}

// Both `core` AND `extension` on one entry → exactly one C_ConflictingField.
TEST(GrammarSchema, BuiltinTypeCoreAndExtensionReportsConflict) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "typeExtensions": [ { "name": "Ext::Foo" } ],
      "semantics": {
        "builtinTypes": [ { "name": "MyT", "core": "I32", "extension": "Ext::Foo" } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(countConfigCode(r.error(), DiagnosticCode::C_ConflictingField), 1u);
}

// Neither `core` NOR `extension` → exactly one C_MissingField.
TEST(GrammarSchema, BuiltinTypeNeitherCoreNorExtensionReportsMissing) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "semantics": {
        "builtinTypes": [ { "name": "MyT" } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(countConfigCode(r.error(), DiagnosticCode::C_MissingField), 1u);
}

// `extension` value not a string → exactly one C_InvalidSemantics.
TEST(GrammarSchema, BuiltinTypeExtensionNonStringReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "typeExtensions": [ { "name": "Ext::Foo" } ],
      "semantics": {
        "builtinTypes": [ { "name": "MyT", "extension": 42 } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(countConfigCode(r.error(), DiagnosticCode::C_InvalidSemantics), 1u);
}

// `extension` naming an extension NOT declared in `typeExtensions[]` →
// exactly one C_UnknownTypeExtension.
TEST(GrammarSchema, BuiltinTypeExtensionUndeclaredReportsUnknown) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "typeExtensions": [ { "name": "Ext::Foo" } ],
      "semantics": {
        "builtinTypes": [ { "name": "MyT", "extension": "Ext::Ghost" } ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(countConfigCode(r.error(), DiagnosticCode::C_UnknownTypeExtension), 1u);
}

// A non-boolean `warnIfUnused` on a declaration entry → exactly one
// C_InvalidSemantics.
TEST(GrammarSchema, DeclarationWarnIfUnusedNonBooleanReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Identifier", "Semi" ] } },
      "semantics": {
        "declarations": [
          { "rule": "root", "name": 0, "kind": "variable", "warnIfUnused": "yes" }
        ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(countConfigCode(r.error(), DiagnosticCode::C_InvalidSemantics), 1u);
}

// A config declaring valid profiles loads cleanly and `artifactProfiles()`
// returns the exact declared vector, in order.
TEST(GrammarSchema, ArtifactProfilesValidLoadsAndReturnsExactVector) {
    constexpr std::string_view cfg = R"({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "artifactProfiles": ["cli", "lib", "staticlib"],
      "shapes": { "root": { "sequence": ["Identifier"] } }
    })";
    auto result = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(result.has_value())
        << (result.error().empty() ? "<no diagnostics>" : result.error()[0].message);
    auto profiles = (*result)->artifactProfiles();
    ASSERT_EQ(profiles.size(), 3u);
    EXPECT_EQ(profiles[0], "cli");
    EXPECT_EQ(profiles[1], "lib");
    EXPECT_EQ(profiles[2], "staticlib");
}

// An unknown profile name emits exactly one C_UnknownArtifactProfile.
TEST(GrammarSchema, ArtifactProfilesUnknownNameReportsCode) {
    constexpr std::string_view bad = R"({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "artifactProfiles": ["cli", "wat"],
      "shapes": { "root": { "sequence": ["Identifier"] } }
    })";
    auto result = GrammarSchema::loadFromText(bad);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(countConfigCode(result.error(),
                              DiagnosticCode::C_UnknownArtifactProfile), 1u);
}

// Absent field → `artifactProfiles()` is empty AND the load is clean (no
// spurious diagnostic for the missing optional block).
TEST(GrammarSchema, ArtifactProfilesAbsentIsEmptyAndClean) {
    constexpr std::string_view cfg = R"({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "shapes": { "root": { "sequence": ["Identifier"] } }
    })";
    auto result = GrammarSchema::loadFromText(cfg);
    ASSERT_TRUE(result.has_value())
        << (result.error().empty() ? "<no diagnostics>" : result.error()[0].message);
    EXPECT_TRUE((*result)->artifactProfiles().empty());
}

// A non-array value emits exactly one C_UnknownArtifactProfile.
TEST(GrammarSchema, ArtifactProfilesNonArrayReportsCode) {
    constexpr std::string_view bad = R"({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "artifactProfiles": "cli",
      "shapes": { "root": { "sequence": ["Identifier"] } }
    })";
    auto result = GrammarSchema::loadFromText(bad);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(countConfigCode(result.error(),
                              DiagnosticCode::C_UnknownArtifactProfile), 1u);
}

// A non-string entry emits exactly one C_UnknownArtifactProfile.
TEST(GrammarSchema, ArtifactProfilesNonStringEntryReportsCode) {
    constexpr std::string_view bad = R"({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "artifactProfiles": ["cli", 42],
      "shapes": { "root": { "sequence": ["Identifier"] } }
    })";
    auto result = GrammarSchema::loadFromText(bad);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(countConfigCode(result.error(),
                              DiagnosticCode::C_UnknownArtifactProfile), 1u);
}

// A profile name listed twice emits exactly one C_ConflictingField (matching
// the typeExtensions duplicate-name precedent) and the load hard-fails.
TEST(GrammarSchema, ArtifactProfilesDuplicateNameReportsConflict) {
    constexpr std::string_view bad = R"({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "artifactProfiles": ["cli", "cli"],
      "shapes": { "root": { "sequence": ["Identifier"] } }
    })";
    auto result = GrammarSchema::loadFromText(bad);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(countConfigCode(result.error(),
                              DiagnosticCode::C_ConflictingField), 1u);
}

// The shipped configs declare their per-language profile sets (plan 06 §4).
TEST(GrammarSchema, ShippedConfigsDeclareArtifactProfiles) {
    auto toy = GrammarSchema::loadShipped("toy");
    if (!toy.has_value()) {
        FAIL() << "loadShipped toy failed: " << toy.error()[0].message
               << " (cwd=" << std::filesystem::current_path().string() << ")";
    }
    {
        auto p = (*toy)->artifactProfiles();
        ASSERT_EQ(p.size(), 1u);
        EXPECT_EQ(p[0], "cli");
    }

    auto c = GrammarSchema::loadShipped("c-subset");
    if (!c.has_value()) {
        FAIL() << "loadShipped c-subset failed: " << c.error()[0].message;
    }
    {
        auto p = (*c)->artifactProfiles();
        ASSERT_EQ(p.size(), 4u);
        EXPECT_EQ(p[0], "cli");
        EXPECT_EQ(p[1], "lib");
        EXPECT_EQ(p[2], "staticlib");
        // `module` (SourceMerge) — c-subset is the only shipped language that
        // declares it; toy and tsql-subset deliberately do NOT, which is what
        // the two sibling blocks in this test pin.
        EXPECT_EQ(p[3], "module");
    }

    auto t = GrammarSchema::loadShipped("tsql-subset");
    if (!t.has_value()) {
        FAIL() << "loadShipped tsql-subset failed: " << t.error()[0].message;
    }
    {
        auto p = (*t)->artifactProfiles();
        ASSERT_EQ(p.size(), 2u);
        EXPECT_EQ(p[0], "script");
        EXPECT_EQ(p[1], "sproc");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// The registered artifact-profile TABLE (`core/types/artifact_profile.hpp`)
// ═══════════════════════════════════════════════════════════════════════════
//
// The vocabulary stopped being a flat name array when `dependsOn` needed a
// per-dependency COMPOSITION decision: each row now carries a
// `DependencyComposition` verb, and the engine switches on the VERB, never on
// the profile NAME (the header's standing agnosticism veto, and the same rule
// `program.cpp`'s archive fork obeys via the format `container`).
//
// A table the engine dispatches on is a table whose CONTENT is semantics, so
// it is pinned here the way a config vocabulary is pinned — exactly, wholly,
// and positionally:
//
//   (a) WELL-FORMEDNESS at compile time, through the SHARED
//       `isWellFormedKeyVocabulary` guard (TF-C74) rather than a third
//       hand-rolled duplicate/empty loop;
//   (b) the ROW COUNT as a `static_assert` HERE in the test TU, so growing
//       the vocabulary without updating these pins is a BUILD error, not a
//       still-green test suite;
//   (c) EVERY row's name AND verb, positionally — not a spot check. A wrong
//       verb is a miscompile-class bug: a `cli` dependency that composed as
//       `SourceMerge` would splice that dependency's `main()` into the
//       consumer's translation set and the build would SUCCEED, wrongly.

namespace {

inline constexpr std::size_t kArtifactProfileRowCount =
    std::size(kRegisteredArtifactProfiles);

// (b) The row-count tripwire. Deliberately in the TEST TU: a `static_assert`
// living in the header would break the header's own build on growth, which
// says nothing about whether anybody re-pinned the verbs. Here, the tenth-
// plus-one profile cannot be added without a human landing in this file and
// answering for its composition verb below.
static_assert(kArtifactProfileRowCount == 10,
              "kRegisteredArtifactProfiles grew or shrank: update the exact "
              "row pins in ArtifactProfileTable.* below (name AND "
              "DependencyComposition verb) before bumping this number");

// The table's NAME column, projected so the shared closed-vocabulary guard
// applies verbatim. (a) is then the same compile-time check every
// `kSomethingKeys` table gets: no empty entry, no duplicate name.
[[nodiscard]] constexpr std::array<std::string_view, kArtifactProfileRowCount>
artifactProfileNameColumn() {
    std::array<std::string_view, kArtifactProfileRowCount> names{};
    for (std::size_t i = 0; i < kArtifactProfileRowCount; ++i) {
        names[i] = kRegisteredArtifactProfiles[i].name;
    }
    return names;
}
inline constexpr auto kArtifactProfileNameColumn = artifactProfileNameColumn();
DSS_CHECK_KEY_VOCABULARY(kArtifactProfileNameColumn);

// (c) The full expected table, written out INDEPENDENTLY of the header so the
// comparison below is a pin and not a tautology.
struct ExpectedArtifactProfileRow {
    std::string_view      name;
    DependencyComposition verb;
};
inline constexpr ExpectedArtifactProfileRow kExpectedArtifactProfileRows[] = {
    {"cli",       DependencyComposition::NotConsumable},
    {"gui",       DependencyComposition::NotConsumable},
    {"lib",       DependencyComposition::ArtifactLink},
    {"staticlib", DependencyComposition::ArtifactLink},
    {"script",    DependencyComposition::NotConsumable},
    {"sproc",     DependencyComposition::NotConsumable},
    {"transpile", DependencyComposition::NotConsumable},
    {"shader",    DependencyComposition::NotConsumable},
    {"hdl",       DependencyComposition::NotConsumable},
    {"module",    DependencyComposition::SourceMerge},
};
static_assert(std::size(kExpectedArtifactProfileRows) == kArtifactProfileRowCount,
              "the expectation table and kRegisteredArtifactProfiles must "
              "describe the same number of rows");

// Human-readable verb name for failure messages only — a scoped enum prints as
// an integer otherwise, and "expected 1, got 2" is a poor bug report for a
// mis-declared composition.
[[nodiscard]] std::string_view verbName(DependencyComposition v) {
    switch (v) {
        case DependencyComposition::SourceMerge:   return "SourceMerge";
        case DependencyComposition::ArtifactLink:  return "ArtifactLink";
        case DependencyComposition::NotConsumable: return "NotConsumable";
    }
    return "<unhandled DependencyComposition>";
}

} // namespace

// (a)+(b) restated as a runtime assertion so the guarantee is visible in the
// test report, not only in a compile that silently succeeded.
TEST(ArtifactProfileTable, IsWellFormedAndExactlyTenRows) {
    EXPECT_EQ(std::size(kRegisteredArtifactProfiles), 10u);
    EXPECT_TRUE(dss::detail::isWellFormedKeyVocabulary(kArtifactProfileNameColumn))
        << "every registered profile name must be non-empty and unique";
    for (auto const& row : kRegisteredArtifactProfiles) {
        EXPECT_FALSE(row.name.empty());
    }
}

// (c) EVERY row, by exact name AND exact verb, in exact order. Order is
// load-bearing: `registeredArtifactProfileList()` derives the user-facing
// diagnostic list from this sequence in place.
TEST(ArtifactProfileTable, EveryRowPinsItsNameAndCompositionVerb) {
    ASSERT_EQ(std::size(kRegisteredArtifactProfiles), kArtifactProfileRowCount);
    for (std::size_t i = 0; i < kArtifactProfileRowCount; ++i) {
        auto const& got  = kRegisteredArtifactProfiles[i];
        auto const& want = kExpectedArtifactProfileRows[i];
        EXPECT_EQ(got.name, want.name) << "row " << i << " name drifted";
        EXPECT_EQ(got.dependencyComposition, want.verb)
            << "row " << i << " ('" << got.name << "') composes as "
            << verbName(got.dependencyComposition) << ", expected "
            << verbName(want.verb);
    }
}

// The lookup the engine actually calls must agree with the table for EVERY
// registered name — a row is only as good as the accessor that reads it.
TEST(ArtifactProfileTable, LookupReturnsEveryRowsVerb) {
    for (auto const& want : kExpectedArtifactProfileRows) {
        auto const got = dependencyCompositionForProfile(want.name);
        ASSERT_TRUE(got.has_value())
            << "'" << want.name << "' is registered but has no composition verb";
        EXPECT_EQ(*got, want.verb)
            << "'" << want.name << "' composes as " << verbName(*got)
            << ", expected " << verbName(want.verb);
    }
}

// Fail-loud: an UNREGISTERED name yields no verb at all. `NotConsumable` would
// be the tempting "safe" answer and is exactly wrong — it is a real
// instruction ("this profile is terminal, reject the dependency"), so
// returning it for a typo would make `"modul"` indistinguishable from `"cli"`
// and produce the wrong diagnostic for the wrong reason.
TEST(ArtifactProfileTable, UnregisteredProfileHasNoCompositionVerb) {
    for (std::string_view bad : {"modul", "module ", "Module", "clii", "",
                                 "sharedlib", "dll", "exe"}) {
        EXPECT_FALSE(isRegisteredArtifactProfile(bad))
            << "'" << bad << "' must not be a registered profile";
        EXPECT_FALSE(dependencyCompositionForProfile(bad).has_value())
            << "'" << bad << "' must not map to any composition verb";
    }
}

// Membership matches the table exactly — every row in, nothing else in.
TEST(ArtifactProfileTable, MembershipHoldsForEveryRow) {
    for (auto const& want : kExpectedArtifactProfileRows) {
        EXPECT_TRUE(isRegisteredArtifactProfile(want.name))
            << "'" << want.name << "' is a table row but not registered";
    }
}

// The diagnostic list is DERIVED from the table (never hand-typed) and its
// text is user-visible, so it is pinned as one exact string — including the
// separator and the append position of `module`.
TEST(ArtifactProfileTable, RegisteredListIsExactAndDerivedInTableOrder) {
    EXPECT_EQ(registeredArtifactProfileList(),
              "cli, gui, lib, staticlib, script, sproc, transpile, shader, "
              "hdl, module");
}

// ── HR10: hirLowering fail-loud config validation ──────────────────────────

// `refExtensionKind` naming a kind absent from `extensionKinds` → loud at LOAD
// (C_InvalidHirLowering), so the engine's extKind() lookup stays total.
TEST(GrammarSchema, HirLoweringRefExtensionKindUndeclaredReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "hirLowering": {
        "extensionKinds": [ { "name": "X::Foo", "lang": "X" } ],
        "refExtensionKind": "X::Bar"
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidHirLowering));
}

// `nullLiteral.hirKind` naming an undeclared extension kind → C_InvalidHirLowering.
TEST(GrammarSchema, HirLoweringNullExtensionKindUndeclaredReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "hirLowering": {
        "extensionKinds": [ { "name": "X::Foo", "lang": "X" } ],
        "nullLiteral": { "token": "Semi", "hirKind": "X::Bar" }
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidHirLowering));
}

// An unknown `childGathering` lower verb (not expr/flatExpr/ext/ref/varDecl) →
// C_InvalidHirLowering at LOAD, the counterpart to the unreachable lowerSlot
// default. Guards the closed ChildLower verb set.
TEST(GrammarSchema, HirLoweringUnknownChildLowerVerbReportsInvalid) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": { "root": { "sequence": [ "Semi" ] } },
      "hirLowering": {
        "extensionKinds": [ { "name": "X::Foo", "lang": "X" } ],
        "ruleMappings": [
          { "rule": "root", "hirKind": "X::Foo", "childGathering": [
            { "match": { "rule": "root" }, "lower": "bogusVerb", "role": "x" }
          ] }
        ]
      }
    })JSON";
    auto r = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasDiagCode(r.error(), DiagnosticCode::C_InvalidHirLowering));
}

// ─── parser.maxExpressionDepth (plan-24 Stage 7: config-driven cap) ─────────
//
// The optional top-level `parser` block carries the expression-nesting cap.
// These pin the loader contract: a valid positive value round-trips to the
// schema accessor; an absent block leaves the accessor `nullopt` (so the CU
// build keeps the ParserConfig C++ fallback); and a non-positive / wrong-type
// value FAILS LOUD with `C_ConflictingField` rather than silently defaulting.

namespace {
// `kHappyConfig`-shaped body + an optional `parser` fragment spliced in as a
// sibling of `language`; pass "" for no `parser` block.
[[nodiscard]] std::string happyConfigWithParser(std::string_view parserFragment) {
    std::string out = R"({
  "dssSchemaVersion": 1,
  "language": { "name": "MiniLang", "version": "1.0.0" },)";
    out += parserFragment;
    out += R"(
  "tokens": { ";": [{ "kind": "EndCommand" }] },
  "shapes": {
    "root":       { "sequence": [{ "repeat": "statement" }] },
    "statement":  { "sequence": ["Identifier", "EndCommand"] }
  }
})";
    return out;
}
} // namespace

TEST(GrammarSchema, ParserMaxExpressionDepthRoundTrips) {
    auto result = GrammarSchema::loadFromText(
        happyConfigWithParser(R"( "parser": { "maxExpressionDepth": 4096 },)"));
    ASSERT_TRUE(result.has_value())
        << "valid parser.maxExpressionDepth must load";
    auto cap = (*result)->maxExpressionDepth();
    ASSERT_TRUE(cap.has_value());
    EXPECT_EQ(*cap, 4096u);
}

TEST(GrammarSchema, ParserBlockAbsentLeavesMaxExpressionDepthUnset) {
    auto result = GrammarSchema::loadFromText(happyConfigWithParser(""));
    ASSERT_TRUE(result.has_value());
    // nullopt — the CU build then keeps the ParserConfig C++ fallback (256),
    // NOT a silently-fabricated value.
    EXPECT_FALSE((*result)->maxExpressionDepth().has_value());
}

TEST(GrammarSchema, ParserMaxExpressionDepthZeroReportsCode) {
    auto result = GrammarSchema::loadFromText(
        happyConfigWithParser(R"( "parser": { "maxExpressionDepth": 0 },)"));
    ASSERT_FALSE(result.has_value())
        << "a zero cap (every expression trips it) must fail loud";
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_ConflictingField));
}

TEST(GrammarSchema, ParserMaxExpressionDepthWrongTypeReportsCode) {
    auto result = GrammarSchema::loadFromText(
        happyConfigWithParser(R"( "parser": { "maxExpressionDepth": "lots" },)"));
    ASSERT_FALSE(result.has_value())
        << "a non-integer cap must fail loud, not silently default";
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_ConflictingField));
}

// C11/C23 6.4.5: the shipped c-subset text with `stringLiteralPrefixes`, for
// mutation-based validation of the `elementCoreByFormat` per-format core map.
namespace {
// Located through the ONE test-side resolver (`repo_root.hpp`:
// $DSS_CONFIG_ROOT → the CMake-baked repo root → the cwd ancestor walk). The
// private cwd walk this replaces resolved nothing in an OUT-OF-TREE build,
// whose cwd has no `src/dss-config` in its ancestry, so every mutation test
// below then ran against an empty string. `configRoot()` throws on an
// unresolvable root — GoogleTest reports that as a failure of the one running
// test, never an `abort()` that would cost this binary's other tests their
// verdicts.
[[nodiscard]] std::string shippedCSubsetTextForPrefixTest() {
    namespace fs = std::filesystem;
    fs::path const cand =
        dss::test::configRoot() / "sources" / "c-subset.lang.json";
    std::ifstream in{cand, std::ios::binary};
    if (!in) {
        ADD_FAILURE() << "cannot open shipped c-subset.lang.json at "
                      << cand.string();
        return {};
    }
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}
} // namespace

TEST(GrammarSchema, StringPrefixUnknownFormatKeyReportsCode) {
    // An unknown object-format key in `elementCoreByFormat` must FAIL LOUD (a typo'd
    // format would otherwise silently never override, baking the wrong wchar width).
    std::string text = shippedCSubsetTextForPrefixTest();
    ASSERT_FALSE(text.empty());
    // Baseline: the unmutated shipped config loads clean.
    ASSERT_TRUE(GrammarSchema::loadFromText(text).has_value())
        << "shipped c-subset must load clean before mutation";
    // Swap the WideStringStart row's valid `"pe"` key for a bogus format name.
    std::string const needle = "\"elementCoreByFormat\": { \"pe\": \"U16\"";
    auto const pos = text.find(needle);
    ASSERT_NE(pos, std::string::npos) << "elementCoreByFormat pe-key not found in shipped config";
    text.replace(pos, needle.size(), "\"elementCoreByFormat\": { \"windoze\": \"U16\"");
    auto result = GrammarSchema::loadFromText(text);
    ASSERT_FALSE(result.has_value())
        << "an unknown object-format key must fail the load, not silently ignore";
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_InvalidHirLowering));
}

// ★ THE SENTINEL VARIANT of the test above, and the WORST of the family. This
// map is ALREADY keyed on `ObjectFormatKind`, so `"unknown"` does not merely sit
// dead — it stores a LIVE `ObjectFormatKind::Unknown` row. `resolveElementCore`
// takes an `optional<ObjectFormatKind>`, so any caller holding a
// default-constructed kind (== Unknown, NOT nullopt) MATCHES that row and takes
// a wchar_t element width nothing intended. A dead entry is a silent no-op; this
// one is a silent WRONG ANSWER.
//
// RED-ON-DISABLE: remove the `isSelectableObjectFormatKind` branch in the
// `elementCoreByFormat` loop and the mutated config loads clean.
TEST(GrammarSchema, StringPrefixSentinelFormatKeyReportsCode) {
    std::string text = shippedCSubsetTextForPrefixTest();
    ASSERT_FALSE(text.empty());
    ASSERT_TRUE(GrammarSchema::loadFromText(text).has_value())
        << "shipped c-subset must load clean before mutation";
    std::string const needle = "\"elementCoreByFormat\": { \"pe\": \"U16\"";
    auto const pos = text.find(needle);
    ASSERT_NE(pos, std::string::npos)
        << "elementCoreByFormat pe-key not found in shipped config";
    text.replace(pos, needle.size(),
                 "\"elementCoreByFormat\": { \"unknown\": \"U16\"");
    auto result = GrammarSchema::loadFromText(text);
    ASSERT_FALSE(result.has_value())
        << "the 'unknown' sentinel must fail the load — it resolves through the "
           "name table, so it would be STORED as a live per-format override";
    EXPECT_TRUE(hasDiagCode(result.error(),
                            DiagnosticCode::C_InvalidHirLowering));
    EXPECT_TRUE(std::ranges::any_of(result.error(), [](auto const& d) {
        return d.message.find("sentinel") != std::string::npos;
    })) << errorDiags(result.error());
}

TEST(GrammarSchema, StringPrefixUnknownElementCoreReportsCode) {
    // A per-format value that is not a known TypeKind must FAIL LOUD.
    std::string text = shippedCSubsetTextForPrefixTest();
    ASSERT_FALSE(text.empty());
    std::string const needle = "\"elementCoreByFormat\": { \"pe\": \"U16\"";
    auto const pos = text.find(needle);
    ASSERT_NE(pos, std::string::npos);
    text.replace(pos, needle.size(), "\"elementCoreByFormat\": { \"pe\": \"U17\"");
    auto result = GrammarSchema::loadFromText(text);
    ASSERT_FALSE(result.has_value())
        << "an unknown per-format TypeKind must fail the load";
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_InvalidHirLowering));
}

// C11/C23 6.4.4.4: `charLiteralPrefixes` shares the SAME validator as
// `stringLiteralPrefixes` (one loader lambda) — this mutates the WIDE-CHAR row's
// format key to prove the char table is parsed + closed-key-validated too (a typo'd
// char wchar format would otherwise silently bake the wrong char width).
TEST(GrammarSchema, CharPrefixUnknownFormatKeyReportsCode) {
    std::string text = shippedCSubsetTextForPrefixTest();
    ASSERT_FALSE(text.empty());
    ASSERT_TRUE(GrammarSchema::loadFromText(text).has_value())
        << "shipped c-subset must load clean before mutation";
    // The WideCharStart row's `elementCoreByFormat` (the SECOND such snippet — the
    // first belongs to WideStringStart).
    std::string const needle = "\"elementCoreByFormat\": { \"pe\": \"U16\"";
    auto const first = text.find(needle);
    ASSERT_NE(first, std::string::npos);
    auto const pos = text.find(needle, first + needle.size());
    ASSERT_NE(pos, std::string::npos) << "the WideCharStart elementCoreByFormat row was not found";
    text.replace(pos, needle.size(), "\"elementCoreByFormat\": { \"windoze\": \"U16\"");
    auto result = GrammarSchema::loadFromText(text);
    ASSERT_FALSE(result.has_value())
        << "an unknown object-format key in charLiteralPrefixes must fail the load";
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_InvalidHirLowering));
}

// The regression wall for the CLOSED `semantics` key vocabulary: every key
// the shipped c-subset config actually uses must survive it. An over-narrow
// allowed-key list would reject the real config — and the fix is always the
// list, never the config.
TEST(GrammarSchema, SemanticsClosedKeysAcceptShippedCSubset) {
    std::string const text = shippedCSubsetTextForPrefixTest();
    ASSERT_FALSE(text.empty());
    auto result = GrammarSchema::loadFromText(text);
    ASSERT_TRUE(result.has_value())
        << "the shipped c-subset config must still load under the closed "
           "'semantics' key vocabulary: "
        << (result.error().empty() ? "<no diagnostics>"
                                   : result.error()[0].message);
}

// ── TF-C73: the drift cross-check, against the REAL config ────────────────
//
// Two halves, and both are needed.
//
// (1) THE WALL. The shipped c-subset's three ABI-neutral name lists are 18 /
//     12 / 2 entries with a pairwise overlap of only 6 — they answer different
//     questions and must NOT be merged. A cross-check that mistook them for
//     three copies of one set would reject the real config, and the fix would
//     always be the check, never the config. This is the pin that keeps the
//     rule honest about that.
TEST(GrammarSchema, AttributeVocabularyCrossCheckAcceptsShippedCSubset) {
    std::string const text = shippedCSubsetTextForPrefixTest();
    ASSERT_FALSE(text.empty());
    auto result = GrammarSchema::loadFromText(text);
    ASSERT_TRUE(result.has_value())
        << "the shipped c-subset config must still load under the attribute-"
           "vocabulary drift cross-check — the three lists are deliberately "
           "different and a check that demanded they agree would be wrong: "
        << errorDiags(result.error());
}

// (2) THE LIVE PROOF. A wall alone can pass because the check never runs. So
//     drift the REAL config by one character — delete `deprecated` from
//     `topLevelDecl`'s ignore list while its `warnOnUse` effects row stays —
//     and the load must FAIL. Without this, a cross-check that silently
//     no-ops on the shipped config would look identical to one that works.
TEST(GrammarSchema, AttributeVocabularyCrossCheckFiresOnDriftedShippedCSubset) {
    std::string text = shippedCSubsetTextForPrefixTest();
    ASSERT_FALSE(text.empty());
    ASSERT_TRUE(GrammarSchema::loadFromText(text).has_value())
        << "shipped c-subset must load clean before mutation";
    std::string const needle = R"("linkageSpecifierIgnoredNames": ["noreturn", "deprecated",)";
    auto const pos = text.find(needle);
    ASSERT_NE(pos, std::string::npos)
        << "the topLevelDecl ignore list was not found — if the list was "
           "reformatted, update this needle rather than dropping the pin";
    text.replace(pos, needle.size(),
                 R"("linkageSpecifierIgnoredNames": ["noreturn",)");
    auto result = GrammarSchema::loadFromText(text);
    ASSERT_FALSE(result.has_value())
        << "dropping a declaration-attached effect's name from the real "
           "config's ignore list must fail the load — otherwise a leading "
           "__attribute__((deprecated)) rejects legal C at COMPILE time and "
           "nothing said so at LOAD time";
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_InvalidSemantics));
}

// (3) THE LIVE PROOF FOR THE WHOLE-KEY EDIT. Deleting ONE entry (above) is the
//     small edit; deleting the ENTIRE `linkageSpecifierIgnoredNames` key is the
//     large one, and it used to be the one that escaped — the gate exempted any
//     row whose list was empty, so emptying the list disarmed the check for that
//     row. Same config, same drift, one keystroke further.
TEST(GrammarSchema, AttributeVocabularyCrossCheckFiresOnWholeKeyDeletedFromShippedCSubset) {
    std::string text = shippedCSubsetTextForPrefixTest();
    ASSERT_FALSE(text.empty());
    ASSERT_TRUE(GrammarSchema::loadFromText(text).has_value())
        << "shipped c-subset must load clean before mutation";
    // `topLevelDecl`'s list — the row that does NOT ignore `attrSpec`
    // wholesale, so its per-name opt-in is the only thing keeping a leading
    // `__attribute__((deprecated))` from failing H_UnknownLinkageSpecifier.
    std::string const needle = R"("linkageSpecifierIgnoredNames": ["noreturn", "deprecated",)";
    auto const pos = text.find(needle);
    ASSERT_NE(pos, std::string::npos)
        << "the topLevelDecl ignore list was not found — if the list was "
           "reformatted, update this needle rather than dropping the pin";
    auto const close = text.find("],", pos);
    ASSERT_NE(close, std::string::npos);
    text.erase(pos, close + 2 - pos);
    auto result = GrammarSchema::loadFromText(text);
    ASSERT_FALSE(result.has_value())
        << "deleting the ENTIRE ignore-names key from the real config must "
           "fail the load — an empty list is not an opt-out, it is the same "
           "drift as deleting one entry and must not be the version that "
           "escapes";
    EXPECT_TRUE(hasDiagCode(result.error(), DiagnosticCode::C_InvalidSemantics));
}

// (4) THE LIVE PROOF THAT THE CHECK IS NOT OVER-STRICT. `varDecl` ignores
//     `attrSpec` and `stdAttr` WHOLESALE BY RULE, so no attribute identifier
//     can reach its name lookup and it names nothing. Adding one unrelated
//     ignored NAME to it must NOT suddenly demand the whole declaration-attached
//     vocabulary — MEASURED under the old gate: it demanded six.
//
//     ★ Note the shape of what this prevents: a guard whose remedy is "add more
//     names to the silence list" pushes config in the wrong direction, and it
//     does so most convincingly when the guard is otherwise correct.
TEST(GrammarSchema, AttributeVocabularyCrossCheckStaysQuietOnWholesaleIgnoringShippedRow) {
    std::string text = shippedCSubsetTextForPrefixTest();
    ASSERT_FALSE(text.empty());
    std::string const needle =
        R"("linkageSpecifierIgnoredRules": ["attrSpec", "stdAttr", "alignasSpec"] },
      { "rule": "identVarDecl")";
    auto const pos = text.find(needle);
    ASSERT_NE(pos, std::string::npos)
        << "the varDecl row tail was not found — if the row was reformatted, "
           "update this needle rather than dropping the pin";
    text.replace(pos, needle.size(),
        R"("linkageSpecifierIgnoredNames": ["restrict"],
        "linkageSpecifierIgnoredRules": ["attrSpec", "stdAttr", "alignasSpec"] },
      { "rule": "identVarDecl")");
    auto result = GrammarSchema::loadFromText(text);
    ASSERT_TRUE(result.has_value())
        << "a row that already ignores every attribute rule wholesale cannot "
           "route an attribute identifier to its name lookup, so ignoring one "
           "more unrelated specifier by name must stay clean: "
        << errorDiags(result.error());
}

// ─────────────────────────────────────────────────────────────────────────
// `contentDigest()` — the retained content digest
// ─────────────────────────────────────────────────────────────────────────
//
// `GrammarSchema::loadFromText` retains the lowercase 64-hex SHA-256 of the
// EXACT document bytes it was handed, computed at the one chokepoint where
// those bytes are already in memory. It exists so the runtime-object cache can
// key on the config a build actually LOADED without re-walking
// `src/dss-config/` from disk — ~165 ms per invocation, MEASURED 2026-08-17
// (86 files, 2,078,133 bytes; I/O-dominated: walk+read 152-160 ms, hash only
// 9-13 ms), which would be paid on every build.
//
// ★★ THE ONE-BYTE ARM IS BUILT AT EQUAL LENGTH, AND THAT IS THE POINT OF IT.
// A "digest" that had quietly become a size or length stamp would sail through
// a mutation test whose two inputs differ in SIZE — and the mutation this cache
// has to tell apart is exactly the equal-length kind (MEASURED: a real
// descriptor mutation was 9149 bytes before AND after). So the fixture ASSERTS
// equal length and EXACTLY ONE differing byte rather than merely being
// constructed that way, and it perturbs a byte of STRUCTURAL JSON WHITESPACE so
// the two documents PARSE IDENTICALLY — the digest cannot then be coming from
// anything the parser produced.

namespace {

// Lowercase-hex render, written here rather than reached for from
// `dss::crypto::toHexLower`: an oracle that shares code with the subject
// cannot witness the subject.
[[nodiscard]] std::string hexOracle(std::array<std::uint8_t, 32> const& digest) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (std::uint8_t const byte : digest) {
        out.push_back(kHexDigits[byte >> 4]);
        out.push_back(kHexDigits[byte & 0x0fu]);
    }
    return out;
}

// SHA-256 over `text`'s exact bytes — the INDEPENDENT expectation the retained
// digest is pinned against, computed here and never read back off the schema.
[[nodiscard]] std::string digestOracle(std::string_view text) {
    return hexOracle(dss::crypto::sha256(std::span<std::uint8_t const>{
        reinterpret_cast<std::uint8_t const*>(text.data()), text.size()}));
}

// Number of positions at which two strings differ; `npos` if their lengths do
// (so a length change can never be mistaken for a one-byte change).
[[nodiscard]] std::size_t differingBytes(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return std::string_view::npos;
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) ++n;
    }
    return n;
}

// `text` with its first LF turned into a space: same length, one byte
// different, and provably the same document to the parser — JSON forbids a raw
// newline inside a string, so every LF in a valid document is structural
// whitespace and a space is its equal in every position it can occupy.
[[nodiscard]] std::string withOneWhitespaceByteChanged(std::string_view text) {
    std::string out{text};
    auto const pos = out.find('\n');
    EXPECT_NE(pos, std::string::npos)
        << "the fixture carries no LF to perturb — reach for another "
           "same-length mutation rather than dropping this arm";
    if (pos != std::string::npos) out[pos] = ' ';
    return out;
}

} // namespace

TEST(GrammarSchemaContentDigest, IsSixtyFourLowercaseHexDigits) {
    auto result = GrammarSchema::loadFromText(kHappyConfig);
    ASSERT_TRUE(result.has_value()) << errorDiags(result.error());
    auto const digest = (*result)->contentDigest();
    EXPECT_EQ(digest.size(), 64u);
    EXPECT_EQ(digest.find_first_not_of("0123456789abcdef"),
              std::string_view::npos)
        << "not lowercase hex: " << digest;
}

TEST(GrammarSchemaContentDigest, SameTextTwiceYieldsTheSameDigest) {
    auto a = GrammarSchema::loadFromText(kHappyConfig);
    auto b = GrammarSchema::loadFromText(kHappyConfig);
    ASSERT_TRUE(a.has_value()) << errorDiags(a.error());
    ASSERT_TRUE(b.has_value()) << errorDiags(b.error());
    ASSERT_NE(a->get(), b->get())
        << "the two loads returned the SAME object, so an equal digest would "
           "be a tautology rather than a determinism claim";
    EXPECT_EQ((*a)->contentDigest(), (*b)->contentDigest());
}

TEST(GrammarSchemaContentDigest, OneByteAtEqualLengthChangesTheDigest) {
    std::string const original{kHappyConfig};
    std::string const perturbed = withOneWhitespaceByteChanged(original);

    ASSERT_EQ(original.size(), perturbed.size())
        << "the two inputs must be the SAME LENGTH, or a size stamp would "
           "pass this test";
    ASSERT_EQ(differingBytes(original, perturbed), 1u);

    auto a = GrammarSchema::loadFromText(original);
    auto b = GrammarSchema::loadFromText(perturbed);
    ASSERT_TRUE(a.has_value()) << errorDiags(a.error());
    ASSERT_TRUE(b.has_value()) << errorDiags(b.error());

    // Parse-identical — the perturbed byte was JSON whitespace …
    EXPECT_EQ((*a)->name(), (*b)->name());
    EXPECT_EQ((*a)->version(), (*b)->version());
    // … and still byte-distinguishable, which is the whole contract.
    EXPECT_NE((*a)->contentDigest(), (*b)->contentDigest());
}

TEST(GrammarSchemaContentDigest, EqualsAnIndependentSha256OfTheLoadedBytes) {
    auto result = GrammarSchema::loadFromText(kHappyConfig);
    ASSERT_TRUE(result.has_value()) << errorDiags(result.error());
    EXPECT_EQ((*result)->contentDigest(), digestOracle(kHappyConfig));
}

// ⚠ EMPTY MEANS UNKNOWN, NEVER WRONG. The public `GrammarSchemaData` ctor is
// the documented bypass (tests build schemas without JSON), and it has no
// document bytes to digest. An empty digest is a DETECTABLE unknown a cache can
// refuse to key on; a fabricated or inherited one is a silent wrong key.
TEST(GrammarSchemaContentDigest, ConstructionBypassingLoadFromTextLeavesItEmpty) {
    GrammarSchema const schema{detail::GrammarSchemaData{}};
    EXPECT_TRUE(schema.contentDigest().empty())
        << "a schema with no document bytes reported a digest: "
        << schema.contentDigest();
}
