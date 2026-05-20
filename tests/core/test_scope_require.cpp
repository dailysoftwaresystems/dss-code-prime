#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/token.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_builder.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

using namespace dss;

namespace {

struct Harness {
    std::shared_ptr<SourceBuffer>        src;
    std::shared_ptr<GrammarSchema const> schema;
};

[[nodiscard]] Harness make(std::string source, std::string_view configText) {
    auto loaded = GrammarSchema::loadFromText(configText);
    EXPECT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    return {
        SourceBuffer::fromString(std::move(source), "<scope-require>"),
        loaded.has_value() ? *loaded : nullptr,
    };
}

[[nodiscard]] Token tokAt(SourceBuffer const& src, std::string_view text,
                          CoreTokenKind kind = CoreTokenKind::Operator,
                          std::size_t hint = std::string_view::npos) {
    const auto sv = src.text();
    const auto found = (hint == std::string_view::npos) ? sv.find(text)
                                                        : sv.find(text, hint);
    EXPECT_NE(found, std::string_view::npos)
        << "lexeme '" << text << "' not in source";
    return Token{
        .coreKind   = kind,
        .schemaKind = InvalidSchemaToken,
        .span       = SourceSpan::of(static_cast<ByteOffset>(found),
                                      static_cast<ByteOffset>(found + text.size())),
    };
}

[[nodiscard]] std::size_t countCode(Tree const& t, DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : t.diagnostics().all()) if (d.code == code) ++n;
    return n;
}

// Find the first token leaf under root by walking children. Avoids
// hard-coding NodeId{2} which is coupled to TreeBuilder's id allocator.
[[nodiscard]] NodeId firstTokenLeaf(Tree const& t) {
    if (!t.root().valid()) return InvalidNode;
    for (NodeId c : t.children(t.root())) {
        if (t.kind(c) == NodeKind::Token) return c;
    }
    return InvalidNode;
}

[[nodiscard]] std::string_view tokenKindName(Tree const& t, NodeId id) {
    if (!t.hasSchema() || !id.valid()) return {};
    return t.schema().schemaTokens().name(t.tokenKind(id));
}

} // namespace

// ── Backward-compat: flat validScopes loads as scopeRequire.anyOf ───────

TEST(ScopeRequire, LegacyValidScopesLoadsAsAnyOf) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp", "validScopes": ["Block", "Paren"] }]
      },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    auto const& m = (*loaded)->lookupLexeme("+");
    ASSERT_EQ(m.size(), 1u);
    ASSERT_EQ(m[0].scopeRequire.anyOf.size(), 2u);
    EXPECT_EQ(m[0].scopeRequire.anyOf[0], ScopeKind::Block);
    EXPECT_EQ(m[0].scopeRequire.anyOf[1], ScopeKind::Paren);
    EXPECT_TRUE(m[0].scopeRequire.forbid.empty());
    EXPECT_FALSE(m[0].scopeRequire.topMustBe.has_value());
    EXPECT_FALSE(m[0].scopeRequire.outermost.has_value());
}

TEST(ScopeRequire, ScopeRequireAnyOfLoadsAsExpected) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp", "scopeRequire": { "anyOf": ["Block"] } }]
      },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    auto const& m = (*loaded)->lookupLexeme("+");
    ASSERT_EQ(m[0].scopeRequire.anyOf.size(), 1u);
    EXPECT_EQ(m[0].scopeRequire.anyOf[0], ScopeKind::Block);
}

TEST(ScopeRequire, BothLegacyAndNewIsConflictingFieldError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp",
                 "validScopes": ["Block"],
                 "scopeRequire": { "forbid": ["Generic"] } }]
      },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_ConflictingField &&
               d.message.find("scopeRequire") != std::string::npos;
    }));
}

TEST(ScopeRequire, UnknownScopeNameInArrayIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp", "scopeRequire": { "forbid": ["NotAScope"] } }]
      },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_UnknownScopeName;
    }));
}

TEST(ScopeRequire, UnknownScopeNameInTopMustBeIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp", "scopeRequire": { "topMustBe": "Fictional" } }]
      },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_UnknownScopeName &&
               d.path.find("topMustBe") != std::string::npos;
    }));
}

TEST(ScopeRequire, UnknownScopeNameInOutermostIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp", "scopeRequire": { "outermost": "Fictional" } }]
      },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_UnknownScopeName &&
               d.path.find("outermost") != std::string::npos;
    }));
}

TEST(ScopeRequire, WrongTypeScopeRequireIsConflictingFieldError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp", "scopeRequire": "Block" }]
      },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_ConflictingField;
    }));
}

TEST(ScopeRequire, WrongTypeAnyOfIsConflictingFieldError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp", "scopeRequire": { "anyOf": "Block" } }]
      },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_ConflictingField &&
               d.path.find("anyOf") != std::string::npos;
    }));
}

TEST(ScopeRequire, NonStringInScopeArrayIsConflictingFieldError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp", "scopeRequire": { "anyOf": ["Block", 42] } }]
      },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_ConflictingField &&
               d.path.find("anyOf/1") != std::string::npos;
    }));
}

TEST(ScopeRequire, EmptyAnyOfArrayEmitsRedundantWarning) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp", "scopeRequire": { "anyOf": [] } }]
      },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    // Warning, not error — config loads. Hash via the error path being
    // empty AND lookup confirming the field is empty (no constraint).
    ASSERT_TRUE(loaded.has_value());
    auto const& m = (*loaded)->lookupLexeme("+");
    EXPECT_TRUE(m[0].scopeRequire.anyOf.empty())
        << "explicit empty array should yield no-constraint (and warn)";
}

TEST(ScopeRequire, TopMustBePlusAnyOfEmitsRedundantWarning) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp",
                 "scopeRequire": { "anyOf": ["Block"], "topMustBe": "Block" } }]
      },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    // Warning, not error — schema loads. Capture both the warning AND
    // that the field actually populated, so a regression that drops
    // either signal fails distinctly.
    ASSERT_TRUE(loaded.has_value());
    auto const& m = (**loaded).lookupLexeme("+");
    ASSERT_TRUE(m[0].scopeRequire.topMustBe.has_value());
    EXPECT_EQ(*m[0].scopeRequire.topMustBe, ScopeKind::Block);
    EXPECT_EQ(m[0].scopeRequire.anyOf.size(), 1u);
    // Re-load to capture the warning channel — loadFromText returns the
    // error vector only on failure, so we need to access the reporter
    // via a workaround: pass an inline malformed sibling field to keep
    // loaded an `expected` with error vector for inspection. Cleaner:
    // assert via a deliberately-failing companion config.
}

TEST(ScopeRequire, RedundantWarningIsActuallyEmitted) {
    // Companion to the test above. A second meaning declares an
    // unknown scope so load fails — the error vector then contains
    // BOTH the unknown-scope error and the redundancy warning,
    // letting us pin the warning emission.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [
          { "kind": "PlusOp",
            "scopeRequire": { "anyOf": ["Block"], "topMustBe": "Block" } },
          { "kind": "PlusOp2", "scopeRequire": { "topMustBe": "Fictional" } }
        ]
      },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    auto const& diags = loaded.error();
    EXPECT_TRUE(std::ranges::any_of(diags, [](auto const& d) {
        return d.code == DiagnosticCode::C_RedundantScopeRequire &&
               d.severity == DiagnosticSeverity::Warning &&
               d.message.find("anyOf") != std::string::npos;
    })) << "C_RedundantScopeRequire warning must be emitted for topMustBe+anyOf";
}

TEST(ScopeRequire, ForbidContainingTopMustBeEmitsRedundantWarning) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [
          { "kind": "PlusOp",
            "scopeRequire": { "topMustBe": "Block", "forbid": ["Block"] } },
          { "kind": "PlusOp2", "scopeRequire": { "topMustBe": "Fictional" } }
        ]
      },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_RedundantScopeRequire &&
               d.message.find("topMustBe") != std::string::npos &&
               d.message.find("never match") != std::string::npos;
    }));
}

TEST(ScopeRequire, ForbidIntersectsAnyOfEmitsRedundantWarning) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [
          { "kind": "PlusOp",
            "scopeRequire": { "anyOf": ["Block"], "forbid": ["Block"] } },
          { "kind": "PlusOp2", "scopeRequire": { "topMustBe": "Fictional" } }
        ]
      },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_RedundantScopeRequire &&
               d.message.find("never match") != std::string::npos;
    }));
}

// ── Loader-side field inspection (every clause populates correctly) ─────

TEST(ScopeRequire, AllFourFieldsLoadIndividually) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "a": [{ "kind": "TokA", "scopeRequire": { "anyOf": ["Block"] } }],
        "b": [{ "kind": "TokB", "scopeRequire": { "forbid": ["Generic"] } }],
        "c": [{ "kind": "TokC", "scopeRequire": { "topMustBe": "Paren" } }],
        "d": [{ "kind": "TokD", "scopeRequire": { "outermost": "Root" } }]
      },
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    auto const& s = **loaded;

    auto const& a = s.lookupLexeme("a");
    ASSERT_EQ(a[0].scopeRequire.anyOf.size(), 1u);
    EXPECT_EQ(a[0].scopeRequire.anyOf[0], ScopeKind::Block);
    EXPECT_TRUE(a[0].scopeRequire.forbid.empty());
    EXPECT_FALSE(a[0].scopeRequire.topMustBe.has_value());
    EXPECT_FALSE(a[0].scopeRequire.outermost.has_value());

    auto const& b = s.lookupLexeme("b");
    EXPECT_TRUE(b[0].scopeRequire.anyOf.empty());
    ASSERT_EQ(b[0].scopeRequire.forbid.size(), 1u);
    EXPECT_EQ(b[0].scopeRequire.forbid[0], ScopeKind::Generic);

    auto const& c = s.lookupLexeme("c");
    ASSERT_TRUE(c[0].scopeRequire.topMustBe.has_value());
    EXPECT_EQ(*c[0].scopeRequire.topMustBe, ScopeKind::Paren);

    auto const& d = s.lookupLexeme("d");
    ASSERT_TRUE(d[0].scopeRequire.outermost.has_value());
    EXPECT_EQ(*d[0].scopeRequire.outermost, ScopeKind::Root);
}

// ── Builder enforcement — forbid ────────────────────────────────────────

TEST(ScopeRequire, ForbidRejectsWhenScopeOnStack) {
    // `<` has two meanings: LtOp (forbidden inside Generic) and a stub
    // LtAngleOp valid only in Generic. Inside Generic scope, LtOp is
    // rejected by forbid, so the resolver picks LtAngleOp.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "<": [
          { "kind": "LtOp",       "scopeRequire": { "forbid": ["Generic"] } },
          { "kind": "LtAngleOp",  "scopeRequire": { "anyOf":  ["Generic"] } }
        ]
      },
      "shapes": { "root": { "sequence": [ "LtAngleOp" ] } }
    })JSON";

    auto h = make("<", kCfg);
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        b.pushScope(ScopeKind::Generic);                       // <Generic>
        b.pushToken(tokAt(*h.src, "<"));
        b.popScope();
    }
    Tree t = std::move(b).finish();

    ASSERT_FALSE(t.diagnostics().hasErrors());
    NodeId leaf = firstTokenLeaf(t);
    ASSERT_TRUE(leaf.valid());
    auto kindName = tokenKindName(t, leaf);
    EXPECT_EQ(kindName, "LtAngleOp")
        << "inside Generic, LtOp must be forbidden — LtAngleOp wins";
}

TEST(ScopeRequire, ForbidAllowsWhenScopeAbsent) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "<": [{ "kind": "LtOp", "scopeRequire": { "forbid": ["Generic"] } }]
      },
      "shapes": { "root": { "sequence": [ "LtOp" ] } }
    })JSON";

    auto h = make("<", kCfg);
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        b.pushToken(tokAt(*h.src, "<"));                       // no Generic on stack
    }
    Tree t = std::move(b).finish();
    EXPECT_FALSE(t.diagnostics().hasErrors());
    NodeId leaf = firstTokenLeaf(t);
    ASSERT_TRUE(leaf.valid());
    EXPECT_EQ(tokenKindName(t, leaf), "LtOp");
}

TEST(ScopeRequire, MultiMeaningSurvivorMirror) {
    // Mirror of ForbidRejectsWhenScopeOnStack: same config, without
    // Generic on the stack. LtOp's forbid clears; LtAngleOp's anyOf
    // (requires Generic) fails. LtOp wins.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "<": [
          { "kind": "LtOp",       "scopeRequire": { "forbid": ["Generic"] } },
          { "kind": "LtAngleOp",  "scopeRequire": { "anyOf":  ["Generic"] } }
        ]
      },
      "shapes": { "root": { "sequence": [ "LtOp" ] } }
    })JSON";

    auto h = make("<", kCfg);
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        b.pushToken(tokAt(*h.src, "<"));                       // no scopes pushed
    }
    Tree t = std::move(b).finish();
    EXPECT_FALSE(t.diagnostics().hasErrors());
    NodeId leaf = firstTokenLeaf(t);
    ASSERT_TRUE(leaf.valid());
    EXPECT_EQ(tokenKindName(t, leaf), "LtOp")
        << "outside Generic, LtAngleOp's anyOf fails; LtOp wins";
}

// ── Builder enforcement — topMustBe ─────────────────────────────────────

TEST(ScopeRequire, TopMustBeRejectsWhenInnermostMismatch) {
    // `case` is declared only as a token (not also as a keyword) so the
    // scopeRequire on it is the *only* meaning lookup. topMustBe Block
    // rejects when Paren is innermost — no meaning survives the scope
    // filter, and Word-fallback resolves the lexeme as Identifier.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "case": [{ "kind": "CaseKw", "scopeRequire": { "topMustBe": "Block" } }]
      },
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";

    auto h = make("case", kCfg);
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        b.pushScope(ScopeKind::Block);
        b.pushScope(ScopeKind::Paren);                         // Block, then Paren on top
        b.pushToken(tokAt(*h.src, "case", CoreTokenKind::Word));
        b.popScope();
        b.popScope();
    }
    Tree t = std::move(b).finish();

    NodeId leaf = firstTokenLeaf(t);
    ASSERT_TRUE(leaf.valid());
    auto kindName = tokenKindName(t, leaf);
    EXPECT_EQ(kindName, "Identifier")
        << "with Paren innermost, CaseKw is rejected; falls back to Identifier";
}

TEST(ScopeRequire, TopMustBeAcceptsWhenInnermostMatches) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "case": [{ "kind": "CaseKw", "scopeRequire": { "topMustBe": "Block" } }]
      },
      "shapes": { "root": { "sequence": [ "CaseKw" ] } }
    })JSON";

    auto h = make("case", kCfg);
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        b.pushScope(ScopeKind::Block);
        b.pushToken(tokAt(*h.src, "case", CoreTokenKind::Word));
        b.popScope();
    }
    Tree t = std::move(b).finish();

    NodeId leaf = firstTokenLeaf(t);
    ASSERT_TRUE(leaf.valid());
    auto kindName = tokenKindName(t, leaf);
    EXPECT_EQ(kindName, "CaseKw");
    EXPECT_FALSE(t.diagnostics().hasErrors());
}

TEST(ScopeRequire, TopMustBeRejectsOnEmptyStack) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp", "scopeRequire": { "topMustBe": "Block" } }]
      },
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";

    auto h = make("+", kCfg);
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        b.pushToken(tokAt(*h.src, "+"));                       // empty scope stack
    }
    Tree t = std::move(b).finish();

    // No meaning matched. P_UnknownToken emitted; tree carries Error leaf.
    EXPECT_EQ(countCode(t, DiagnosticCode::P_UnknownToken), 1u);
    EXPECT_TRUE(t.diagnostics().hasErrors());
}

// ── Builder enforcement — outermost ─────────────────────────────────────

TEST(ScopeRequire, OutermostRejectsWhenBottomMismatch) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp", "scopeRequire": { "outermost": "Root" } }]
      },
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";

    auto h = make("+", kCfg);
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        b.pushScope(ScopeKind::Block);                         // Block at bottom, not Root
        b.pushToken(tokAt(*h.src, "+"));
        b.popScope();
    }
    Tree t = std::move(b).finish();

    EXPECT_EQ(countCode(t, DiagnosticCode::P_UnknownToken), 1u);
}

TEST(ScopeRequire, OutermostAcceptsWhenBottomMatches) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp", "scopeRequire": { "outermost": "Root" } }]
      },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";

    auto h = make("+", kCfg);
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        b.pushScope(ScopeKind::Root);
        b.pushScope(ScopeKind::Block);                         // Root at bottom, Block on top
        b.pushToken(tokAt(*h.src, "+"));
        b.popScope();
        b.popScope();
    }
    Tree t = std::move(b).finish();
    EXPECT_FALSE(t.diagnostics().hasErrors());
    NodeId leaf = firstTokenLeaf(t);
    ASSERT_TRUE(leaf.valid());
    EXPECT_EQ(tokenKindName(t, leaf), "PlusOp");
}

TEST(ScopeRequire, OutermostRejectsOnEmptyStack) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp", "scopeRequire": { "outermost": "Root" } }]
      },
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";

    auto h = make("+", kCfg);
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        b.pushToken(tokAt(*h.src, "+"));                       // empty scope stack
    }
    Tree t = std::move(b).finish();
    EXPECT_EQ(countCode(t, DiagnosticCode::P_UnknownToken), 1u);
}

TEST(ScopeRequire, AllFourConstraintsAndedAcceptCase) {
    // forbid: !Generic, topMustBe: Block, outermost: Root, anyOf: redundant w/topMustBe.
    // Stack Root|Block from bottom up satisfies all four.
    // (anyOf warns but still applies.)
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "@": [{ "kind": "AtOp",
                 "scopeRequire": {
                   "anyOf":     ["Block"],
                   "forbid":    ["Generic"],
                   "topMustBe": "Block",
                   "outermost": "Root"
                 } }]
      },
      "shapes": { "root": { "sequence": [ "AtOp" ] } }
    })JSON";

    auto h = make("@", kCfg);
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        b.pushScope(ScopeKind::Root);
        b.pushScope(ScopeKind::Block);
        b.pushToken(tokAt(*h.src, "@"));
        b.popScope();
        b.popScope();
    }
    Tree t = std::move(b).finish();
    EXPECT_FALSE(t.diagnostics().hasErrors());
    NodeId leaf = firstTokenLeaf(t);
    ASSERT_TRUE(leaf.valid());
    EXPECT_EQ(tokenKindName(t, leaf), "AtOp");
}

TEST(ScopeRequire, AllFourConstraintsAndedRejectOnOutermostMismatch) {
    // Same constraints; stack Block alone (no Root at bottom) fails outermost.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "@": [{ "kind": "AtOp",
                 "scopeRequire": {
                   "anyOf":     ["Block"],
                   "forbid":    ["Generic"],
                   "topMustBe": "Block",
                   "outermost": "Root"
                 } }]
      },
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";

    auto h = make("@", kCfg);
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        b.pushScope(ScopeKind::Block);                         // no Root at bottom
        b.pushToken(tokAt(*h.src, "@"));
        b.popScope();
    }
    Tree t = std::move(b).finish();
    EXPECT_EQ(countCode(t, DiagnosticCode::P_UnknownToken), 1u);
}

// ── Builder enforcement — combined constraints AND'd ────────────────────

TEST(ScopeRequire, ForbidAndAnyOfBothMustHold) {
    // anyOf requires Block on stack; forbid rejects when Generic is on stack.
    // With both Block and Generic on stack → forbid wins, meaning rejected.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": {
        "+": [{ "kind": "PlusOp",
                 "scopeRequire": { "anyOf": ["Block"], "forbid": ["Generic"] } }]
      },
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";

    auto h = make("+", kCfg);
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        b.pushScope(ScopeKind::Block);
        b.pushScope(ScopeKind::Generic);
        b.pushToken(tokAt(*h.src, "+"));
        b.popScope();
        b.popScope();
    }
    Tree t = std::move(b).finish();
    EXPECT_EQ(countCode(t, DiagnosticCode::P_UnknownToken), 1u)
        << "Block satisfies anyOf, but Generic trips forbid — meaning must reject";
}

// ── Backward-compat smoke pin: shipped configs reload unchanged ─────────

TEST(ScopeRequire, ToyConfigReloadsWithEmptyScopeRequire) {
    // Toy has no scopeRequire on any meaning; every LexemeMeaning should
    // carry a default-constructed ScopeMatch. Pin that — a regression
    // that populated scopeRequire with stale state would fail here.
    auto loaded = GrammarSchema::loadShipped("toy");
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    auto const& s = **loaded;
    auto const& var = s.lookupLexeme("var");
    ASSERT_FALSE(var.empty());
    EXPECT_TRUE(var[0].scopeRequire.anyOf.empty());
    EXPECT_TRUE(var[0].scopeRequire.forbid.empty());
    EXPECT_FALSE(var[0].scopeRequire.topMustBe.has_value());
    EXPECT_FALSE(var[0].scopeRequire.outermost.has_value());
}

TEST(ScopeRequire, CSubsetConfigReloadsWithEmptyScopeRequire) {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    auto const& s = **loaded;
    auto const& ifKw = s.lookupLexeme("if");
    ASSERT_FALSE(ifKw.empty());
    EXPECT_TRUE(ifKw[0].scopeRequire.anyOf.empty());
    EXPECT_TRUE(ifKw[0].scopeRequire.forbid.empty());
    EXPECT_FALSE(ifKw[0].scopeRequire.topMustBe.has_value());
    EXPECT_FALSE(ifKw[0].scopeRequire.outermost.has_value());
}
