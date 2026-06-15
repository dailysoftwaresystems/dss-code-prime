// FC4 c1 (M4) — the `commitRequiresTypeName` guard-POLARITY matrix, proven
// against a SYNTHETIC grammar (rule names non-shipped: castish/parenish/
// typePos; shipped grammars untouched).
//
// The guard's two polarities differ ONLY in triage rule 4 (a lone
// identifier the binder sketch has NO entry for):
//   * preferType (the FC2 default; the bare-string config form) — commit
//     iff the follower token could not continue a value reading;
//   * requireKnownType (C 6.7.6.3p11) — NEVER commit on Unknown: roll
//     back to the competing reading AND record the
//     AmbiguousTypeNameCandidate so the CU oracle's seeded reparse still
//     gives a cross-file typedef its second chance.
// Rules 1-3 are polarity-independent (keyword-led type child commits;
// sketch-KNOWN Type commits; sketch-KNOWN Value rolls back) — pinned for
// BOTH polarities below.
//
// The discriminating input is `(q) z;` (unknown `q`, non-operator
// follower `z`): preferType COMMITS the castish reading; requireKnownType
// ROLLS BACK (and the value reading then fails to consume `z` — the
// honest C-like outcome for a non-typedef name in that position).

#include "analysis/syntactic/parser.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

using namespace dss;

namespace {

// The schema text is assembled around the guard declaration so every
// variant (bare string / object preferType / object requireKnownType /
// the loader-reject shapes) shares ONE grammar — the matrix varies
// exactly the knob under test.
[[nodiscard]] std::string schemaWithGuard(std::string_view guardJson) {
    return std::format(R"JSON({{
  "dssSchemaVersion": 4,
  "language": {{ "name": "PolSynth", "version": "0.0.1", "fileExtensions": [".psyn"] }},
  "tokens": {{
    " ":  [{{ "kind": "Whitespace", "flags": ["EmptySpace"] }}],
    "\n": [{{ "kind": "Newline",    "flags": ["EmptySpace"] }}],
    "-":  [{{ "kind": "Minus" }}],
    ";":  [{{ "kind": "Semi" }}],
    "(":  [{{ "kind": "ParenOpen" }}],
    ")":  [{{ "kind": "ParenClose" }}]
  }},
  "numberStyle": {{ "decimal": true, "emitKind": {{ "integer": "IntLiteral" }} }},
  "keywords": [
    {{ "word": "let",   "kind": "LetKw" }},
    {{ "word": "alias", "kind": "AliasKw" }},
    {{ "word": "base",  "kind": "BaseKw" }}
  ],
  "operators": {{
    "groups": [
      {{ "precedence": 65, "associativity": "left", "operators": ["-"] }},
      {{ "precedence": 90, "associativity": "right", "arity": "prefix", "operators": ["-"] }}
    ]
  }},
  "syncTokens": [ "Semi" ],
  "shapes": {{
    "root":      {{ "sequence": [ {{ "repeat": "stmt" }} ] }},
    "stmt":      {{ "alt": [ "vardecl", "aliasdecl", "exprStmt" ] }},
    "vardecl":   {{ "sequence": [ "LetKw", "Identifier", "Semi" ] }},
    "aliasdecl": {{ "sequence": [ "AliasKw", "Identifier", "Semi" ] }},
    "exprStmt":  {{ "sequence": [ "expression", "Semi" ] }},
    "expression": {{ "expr": {{ "atom": "operand",
        "wrapperRules": {{ "binary": "binaryExpr", "unary": "unaryExpr",
                           "postfix": "postfixExpr" }} }} }},
    "operand": {{
      "alt": [ "castish", "parenish", "Identifier", "IntLiteral" ],
      "speculative": true,
      "lookahead": 16
    }},
    "castish": {{
      "sequence": [ "ParenOpen", "typePos", "ParenClose", "castOperand" ],
      "commitRequiresTypeName": {}
    }},
    "typePos":  {{ "alt": [ "BaseKw", "Identifier" ] }},
    "parenish": {{ "sequence": [ "ParenOpen", "expression", "ParenClose" ] }},
    "castOperand": {{ "expr": {{ "atom": "operand", "minPrecedence": 90,
        "wrapperRules": {{ "binary": "binaryExpr", "unary": "unaryExpr",
                           "postfix": "postfixExpr" }} }} }}
  }},
  "semantics": {{
    "identifierToken": "Identifier",
    "declarations": [
      {{ "rule": "vardecl",   "name": 1, "kind": "variable" }},
      {{ "rule": "aliasdecl", "name": 1, "kind": "type" }}
    ]
  }}
}})JSON", guardJson);
}

constexpr std::string_view kBareStringGuard = R"("typePos")";
constexpr std::string_view kPreferTypeGuard =
    R"({ "rule": "typePos", "polarity": "preferType" })";
constexpr std::string_view kRequireKnownGuard =
    R"({ "rule": "typePos", "polarity": "requireKnownType" })";

[[nodiscard]] std::shared_ptr<GrammarSchema const>
loadPolSchema(std::string_view guardJson) {
    auto loaded = GrammarSchema::loadFromText(schemaWithGuard(guardJson),
                                              "<pol-synth>");
    if (!loaded) {
        ADD_FAILURE() << "synthetic polarity schema failed to load";
        std::abort();
    }
    return *loaded;
}

[[nodiscard]] ParseResult parsePol(std::string_view guardJson,
                                   std::string source,
                                   ParserConfig config = {}) {
    auto schema = loadPolSchema(guardJson);
    auto src = SourceBuffer::fromString(std::move(source), "<pol-src>");
    Tokenizer tk{src, schema};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    Parser p{std::move(src), std::move(schema), std::move(stream),
             std::move(config), std::move(lexDiags)};
    return std::move(p).parse();
}

[[nodiscard]] bool hasRule(Tree const& t, std::string_view ruleName) {
    if (!t.hasSchema()) return false;
    const auto ruleId = t.schema().rules().find(ruleName);
    if (!ruleId.valid()) return false;
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId id{i};
        if (t.kind(id) == NodeKind::Internal && t.rule(id).v == ruleId.v) {
            return true;
        }
    }
    return false;
}

} // namespace

// ── loader shapes ────────────────────────────────────────────────────────

TEST(CommitPolarityLoader, BareStringFormStillLoads) {
    EXPECT_TRUE(GrammarSchema::loadFromText(schemaWithGuard(kBareStringGuard),
                                            "<pol>").has_value());
}

TEST(CommitPolarityLoader, ObjectFormBothPolaritiesLoad) {
    EXPECT_TRUE(GrammarSchema::loadFromText(schemaWithGuard(kPreferTypeGuard),
                                            "<pol>").has_value());
    EXPECT_TRUE(GrammarSchema::loadFromText(schemaWithGuard(kRequireKnownGuard),
                                            "<pol>").has_value());
}

TEST(CommitPolarityLoader, UnknownPolarityValueRejects) {
    EXPECT_FALSE(GrammarSchema::loadFromText(
        schemaWithGuard(R"({ "rule": "typePos", "polarity": "requireType" })"),
        "<pol>").has_value());
}

TEST(CommitPolarityLoader, UnknownObjectKeyRejects) {
    EXPECT_FALSE(GrammarSchema::loadFromText(
        schemaWithGuard(R"({ "rule": "typePos",
                             "polarity": "requireKnownType",
                             "polarityy": "x" })"),
        "<pol>").has_value());
}

TEST(CommitPolarityLoader, ObjectFormMissingPolarityRejects) {
    EXPECT_FALSE(GrammarSchema::loadFromText(
        schemaWithGuard(R"({ "rule": "typePos" })"), "<pol>").has_value());
}

TEST(CommitPolarityLoader, ObjectFormMissingRuleRejects) {
    EXPECT_FALSE(GrammarSchema::loadFromText(
        schemaWithGuard(R"({ "polarity": "requireKnownType" })"),
        "<pol>").has_value());
}

TEST(CommitPolarityLoader, ObjectFormDanglingRuleRejects) {
    EXPECT_FALSE(GrammarSchema::loadFromText(
        schemaWithGuard(
            R"({ "rule": "noSuchRule", "polarity": "requireKnownType" })"),
        "<pol>").has_value());
}

// ── the polarity matrix: rule 4 (UNKNOWN lone identifier) ───────────────

// preferType + non-operator follower → COMMIT (the FC2 behavior).
TEST(CommitPolarityMatrix, PreferTypeCommitsUnknownOnNonOperatorFollower) {
    auto r = parsePol(kPreferTypeGuard, "(q) z;");
    EXPECT_FALSE(r.tree.diagnostics().hasErrors());
    EXPECT_TRUE(hasRule(r.tree, "castish"));
    EXPECT_TRUE(r.typeNameCandidates.empty());
}

// The bare-string form keeps the SAME default — backward compatible.
TEST(CommitPolarityMatrix, BareStringFormKeepsPreferTypeDefault) {
    auto r = parsePol(kBareStringGuard, "(q) z;");
    EXPECT_FALSE(r.tree.diagnostics().hasErrors());
    EXPECT_TRUE(hasRule(r.tree, "castish"));
    EXPECT_TRUE(r.typeNameCandidates.empty());
}

// requireKnownType + the SAME input → ROLLBACK + candidate recorded. (The
// value reading then cannot consume `z` — diagnostics are expected; the
// pinned facts are the ABSENT castish and the RECORDED candidate.)
TEST(CommitPolarityMatrix, RequireKnownTypeRollsBackUnknownAndRecords) {
    auto r = parsePol(kRequireKnownGuard, "(q) z;");
    EXPECT_FALSE(hasRule(r.tree, "castish"));
    ASSERT_EQ(r.typeNameCandidates.size(), 1u);
    EXPECT_EQ(r.typeNameCandidates[0].name, "q");
    EXPECT_EQ(r.tree.source().slice(r.typeNameCandidates[0].span), "q");
}

// Operator follower: BOTH polarities roll back (`(q)-x` stays binary
// minus); requireKnownType records the candidate exactly like preferType.
TEST(CommitPolarityMatrix, BothPolaritiesRollBackOnOperatorFollower) {
    for (auto guard : {kPreferTypeGuard, kRequireKnownGuard}) {
        auto r = parsePol(guard, "(q)-x;");
        EXPECT_FALSE(r.tree.diagnostics().hasErrors()) << guard;
        EXPECT_FALSE(hasRule(r.tree, "castish")) << guard;
        ASSERT_EQ(r.typeNameCandidates.size(), 1u) << guard;
        EXPECT_EQ(r.typeNameCandidates[0].name, "q") << guard;
    }
}

// ── rules 1-3 are polarity-independent ──────────────────────────────────

// Rule 1: keyword-led type child commits under BOTH polarities.
TEST(CommitPolarityMatrix, KeywordTypeChildCommitsUnderBothPolarities) {
    for (auto guard : {kPreferTypeGuard, kRequireKnownGuard}) {
        auto r = parsePol(guard, "(base) z;");
        EXPECT_FALSE(r.tree.diagnostics().hasErrors()) << guard;
        EXPECT_TRUE(hasRule(r.tree, "castish")) << guard;
        EXPECT_TRUE(r.typeNameCandidates.empty()) << guard;
    }
}

// Rule 2: a sketch-KNOWN type commits under requireKnownType — the strict
// polarity admits exactly the names the binder sketch knows.
TEST(CommitPolarityMatrix, KnownTypeNameCommitsUnderRequireKnownType) {
    auto r = parsePol(kRequireKnownGuard, "alias T; (T) z;");
    EXPECT_FALSE(r.tree.diagnostics().hasErrors());
    EXPECT_TRUE(hasRule(r.tree, "castish"));
    EXPECT_TRUE(r.typeNameCandidates.empty());
}

// Rule 3: a sketch-KNOWN value rolls back under BOTH polarities — and a
// KNOWN name records NO candidate (the oracle is for unknowns only).
TEST(CommitPolarityMatrix, KnownValueRollsBackUnderBothPolarities) {
    for (auto guard : {kPreferTypeGuard, kRequireKnownGuard}) {
        auto r = parsePol(guard, "let v; (v)-x;");
        EXPECT_FALSE(r.tree.diagnostics().hasErrors()) << guard;
        EXPECT_FALSE(hasRule(r.tree, "castish")) << guard;
        EXPECT_TRUE(r.typeNameCandidates.empty()) << guard;
    }
}

// ── the oracle channel under the strict polarity ────────────────────────

// The SAME unknown-name input that rolled back commits the castish once
// the name is SEEDED as a global type (the CU oracle's reparse channel) —
// so a cross-file typedef in the guarded position resolves on the second
// pass exactly as designed.
TEST(CommitPolarityMatrix, SeededTypeNameFlipsRequireKnownTypeSiteToCommit) {
    ParserConfig cfg;
    cfg.seedGlobalTypeNames.push_back("q");
    auto r = parsePol(kRequireKnownGuard, "(q) z;", std::move(cfg));
    EXPECT_FALSE(r.tree.diagnostics().hasErrors());
    EXPECT_TRUE(hasRule(r.tree, "castish"));
    EXPECT_TRUE(r.typeNameCandidates.empty());
}
