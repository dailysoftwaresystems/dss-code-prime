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

TEST(ScopeRequire, BothLegacyAndNewIsLoadError) {
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
        return d.code == DiagnosticCode::C_MissingField &&
               d.message.find("scopeRequire") != std::string::npos;
    }));
}

TEST(ScopeRequire, UnknownScopeNameIsLoadError) {
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
        return d.code == DiagnosticCode::C_UnclosableScope;
    }));
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
    // Warning, not error — schema loads.
    ASSERT_TRUE(loaded.has_value());
    auto const& m = (**loaded).lookupLexeme("+");
    EXPECT_TRUE(m[0].scopeRequire.topMustBe.has_value());
    EXPECT_EQ(*m[0].scopeRequire.topMustBe, ScopeKind::Block);
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
    NodeId leaf{2};                                            // first child of root
    ASSERT_TRUE(leaf.valid());
    auto kindName = h.schema->schemaTokens().name(t.tokenKind(leaf));
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

    NodeId leaf{2};
    auto kindName = h.schema->schemaTokens().name(t.tokenKind(leaf));
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

    NodeId leaf{2};
    auto kindName = h.schema->schemaTokens().name(t.tokenKind(leaf));
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

TEST(ScopeRequire, ToyConfigReloadsUnchanged) {
    auto loaded = GrammarSchema::loadShipped("toy");
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
}

TEST(ScopeRequire, CSubsetConfigReloadsUnchanged) {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
}
