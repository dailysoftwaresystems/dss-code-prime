#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/token.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_builder.hpp"
#include "core/types/tree_node.hpp"

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
        SourceBuffer::fromString(std::move(source), "<contextual>"),
        loaded.has_value() ? *loaded : nullptr,
    };
}

[[nodiscard]] Token tokAt(SourceBuffer const& src, std::string_view text,
                          CoreTokenKind kind, std::size_t hint = std::string_view::npos) {
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

// Walk every leaf in the tree, collecting their schemaToken-kind names.
[[nodiscard]] std::vector<std::string> leafKindNames(Tree const& t) {
    std::vector<std::string> out;
    if (!t.hasSchema()) return out;
    auto const& tokens = t.schema().schemaTokens();
    for (NodeId id{1}; id.v < t.nodeCount(); id.v++) {
        if (t.kind(id) == NodeKind::Token) {
            out.emplace_back(tokens.name(t.tokenKind(id)));
        }
    }
    return out;
}

[[nodiscard]] std::size_t countCode(Tree const& t, DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : t.diagnostics().all()) if (d.code == code) ++n;
    return n;
}

} // namespace

// ── reservedWordPolicy parsing ───────────────────────────────────────────

TEST(ContextualKeywords, ReservedWordPolicyDefaultsToStrict) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ((*loaded)->reservedWordPolicy(), ReservedWordPolicy::Strict);
}

TEST(ContextualKeywords, ReservedWordPolicyContextualParsed) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "reservedWordPolicy": "contextual",
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ((*loaded)->reservedWordPolicy(), ReservedWordPolicy::Contextual);
}

TEST(ContextualKeywords, InvalidReservedWordPolicyIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "reservedWordPolicy": "loose",
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_MissingField;
    }));
}

TEST(ContextualKeywords, ContextualFlagOnTokenIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { "+": [{ "kind": "PlusOp", "contextual": true }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    auto const& diags = loaded.error();
    auto it = std::ranges::find_if(diags, [](auto const& d) {
        return d.code == DiagnosticCode::C_MissingField;
    });
    ASSERT_NE(it, diags.end());
    EXPECT_NE(it->message.find("keyword"), std::string::npos)
        << "diagnostic should explain that contextual is keyword-only; got: "
        << it->message;
}

TEST(ContextualKeywords, NonBooleanContextualOnKeywordIsLoadError) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "keywords": [ { "word": "await", "kind": "AwaitKw", "contextual": "yes" } ],
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_TRUE(std::ranges::any_of(loaded.error(), [](auto const& d) {
        return d.code == DiagnosticCode::C_MissingField;
    }));
}

TEST(ContextualKeywords, ContextualFlagPropagatesToLexemeMeaning) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "keywords": [
        { "word": "if",    "kind": "IfKw" },
        { "word": "await", "kind": "AwaitKw", "contextual": true }
      ],
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    auto const& schema = **loaded;
    auto ifKw = schema.lookupLexeme("if");
    ASSERT_EQ(ifKw.size(), 1u);
    EXPECT_FALSE(ifKw[0].contextual);
    auto awaitKw = schema.lookupLexeme("await");
    ASSERT_EQ(awaitKw.size(), 1u);
    EXPECT_TRUE(awaitKw[0].contextual);
}

TEST(ContextualKeywords, ContextualPolicyForcesEveryKeywordSoft) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "reservedWordPolicy": "contextual",
      "keywords": [
        { "word": "select", "kind": "SelectKw" },
        { "word": "from",   "kind": "FromKw"   }
      ],
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    auto const& schema = **loaded;
    EXPECT_TRUE(schema.lookupLexeme("select")[0].contextual);
    EXPECT_TRUE(schema.lookupLexeme("from")[0].contextual);
}

// ── Demotion in the builder ──────────────────────────────────────────────

TEST(ContextualKeywords, AwaitDegradesWhenNotInExpectedSet) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "keywords": [
        { "word": "int",   "kind": "IntKw" },
        { "word": "await", "kind": "AwaitKw", "contextual": true }
      ],
      "shapes": { "root": { "sequence": [ "IntKw", "Identifier" ] } }
    })JSON";

    auto h = make("int await", kCfg);
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        b.pushToken(tokAt(*h.src, "int",   CoreTokenKind::Word));
        b.pushToken(tokAt(*h.src, "await", CoreTokenKind::Word));
    }
    Tree t = std::move(b).finish();

    auto names = leafKindNames(t);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "IntKw");
    EXPECT_EQ(names[1], "Identifier")   // demoted, not AwaitKw
        << "second leaf should have been demoted to Identifier";
    EXPECT_EQ(countCode(t, DiagnosticCode::P_ContextualKeywordResolution), 1u);
    EXPECT_FALSE(t.diagnostics().hasErrors());
}

TEST(ContextualKeywords, AwaitStaysKeywordWhenInExpectedSet) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "keywords": [
        { "word": "await", "kind": "AwaitKw", "contextual": true }
      ],
      "shapes": { "root": { "sequence": [ "AwaitKw" ] } }
    })JSON";

    auto h = make("await", kCfg);
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        b.pushToken(tokAt(*h.src, "await", CoreTokenKind::Word));
    }
    Tree t = std::move(b).finish();

    auto names = leafKindNames(t);
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "AwaitKw");
    EXPECT_EQ(countCode(t, DiagnosticCode::P_ContextualKeywordResolution), 0u);
}

TEST(ContextualKeywords, HardKeywordWinsRegardlessOfExpectedSet) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "keywords": [
        { "word": "int", "kind": "IntKw" },
        { "word": "if",  "kind": "IfKw" }
      ],
      "shapes": { "root": { "sequence": [ "IntKw", "Identifier" ] } }
    })JSON";

    auto h = make("int if", kCfg);
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        b.pushToken(tokAt(*h.src, "int", CoreTokenKind::Word));
        b.pushToken(tokAt(*h.src, "if",  CoreTokenKind::Word));
    }
    Tree t = std::move(b).finish();
    auto names = leafKindNames(t);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "IntKw");
    EXPECT_EQ(names[1], "IfKw")
        << "hard keyword does NOT degrade even when expectedSet excludes it";
    EXPECT_EQ(countCode(t, DiagnosticCode::P_ContextualKeywordResolution), 0u);
}

TEST(ContextualKeywords, ContextualPolicyDemotesEveryKeywordOutOfExpectedSet) {
    // `from` is in the keyword list but not in the root's expected set
    // at position 1 — `select` is. Under the contextual policy, every
    // keyword is soft, so `from` demotes; `select` stays (it IS expected).
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "reservedWordPolicy": "contextual",
      "keywords": [
        { "word": "select", "kind": "SelectKw" },
        { "word": "from",   "kind": "FromKw"   }
      ],
      "shapes": { "root": { "sequence": [ "SelectKw", "Identifier" ] } }
    })JSON";

    auto h = make("select from", kCfg);
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        b.pushToken(tokAt(*h.src, "select", CoreTokenKind::Word));
        b.pushToken(tokAt(*h.src, "from",   CoreTokenKind::Word));
    }
    Tree t = std::move(b).finish();
    auto names = leafKindNames(t);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "SelectKw");          // expected here
    EXPECT_EQ(names[1], "Identifier");        // demoted
    EXPECT_EQ(countCode(t, DiagnosticCode::P_ContextualKeywordResolution), 1u);
}

TEST(ContextualKeywords, InvalidCursorFallbackKeepsKeyword) {
    // Drive the builder into a state where the cursor is off-track when
    // a contextual keyword arrives. The cursor goes invalid after an
    // off-grammar token, after which contextual resolution must stay
    // strict — the keyword keeps its kind.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "keywords": [
        { "word": "int",   "kind": "IntKw" },
        { "word": "await", "kind": "AwaitKw", "contextual": true }
      ],
      "shapes": { "root": { "sequence": [ "IntKw" ] } }
    })JSON";

    auto h = make("int int await", kCfg);
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        b.pushToken(tokAt(*h.src, "int", CoreTokenKind::Word));            // consumes IntKw slot
        b.pushToken(tokAt(*h.src, "int", CoreTokenKind::Word, 4));         // off-track: drives cursor invalid
        b.pushToken(tokAt(*h.src, "await", CoreTokenKind::Word));          // contextual; cursor invalid → keep
    }
    Tree t = std::move(b).finish();

    auto names = leafKindNames(t);
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "IntKw");
    EXPECT_EQ(names[1], "IntKw");
    EXPECT_EQ(names[2], "AwaitKw")
        << "with cursor invalid, contextual keyword must NOT demote";
    EXPECT_EQ(countCode(t, DiagnosticCode::P_ContextualKeywordResolution), 0u);
    // The off-track pushToken trips exactly one desync diagnostic per
    // the one-shot policy; the later token doesn't re-emit.
    EXPECT_EQ(countCode(t, DiagnosticCode::P_SchemaCursorDesync), 1u);
}

// ── C2: loader rejection of contextual on built-in Identifier kind ──────

TEST(ContextualKeywords, ContextualOnBuiltinIdentifierKindIsLoadError) {
    // Identifier IS the demotion target. Marking a keyword's kind as
    // Identifier AND contextual would mean the demoted form equals the
    // original form — a silent no-op. The loader rejects this loudly.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "keywords": [ { "word": "self", "kind": "Identifier", "contextual": true } ],
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_FALSE(loaded.has_value());
    auto const& diags = loaded.error();
    auto it = std::ranges::find_if(diags, [](auto const& d) {
        return d.code == DiagnosticCode::C_MissingField;
    });
    ASSERT_NE(it, diags.end());
    EXPECT_NE(it->message.find("Identifier"), std::string::npos)
        << "diagnostic should mention Identifier: " << it->message;
}

// ── I3: cursor advances by Identifier after demotion ────────────────────

TEST(ContextualKeywords, DemotedKeywordAdvancesCursorAsIdentifier) {
    // After `int await`, expectedSet is {Identifier} → `await` demotes
    // to Identifier. The cursor must then advance past the Identifier
    // slot (not the AwaitKw slot) so a following token at the next slot
    // resolves cleanly. With three tokens (`int await x`), the third
    // token can only attach if the cursor advanced correctly through
    // the demoted Identifier.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "keywords": [
        { "word": "int",   "kind": "IntKw" },
        { "word": "await", "kind": "AwaitKw", "contextual": true }
      ],
      "shapes": { "root": { "sequence": [ "IntKw", "Identifier", "Identifier" ] } }
    })JSON";

    auto h = make("int await x", kCfg);
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        b.pushToken(tokAt(*h.src, "int",   CoreTokenKind::Word));
        b.pushToken(tokAt(*h.src, "await", CoreTokenKind::Word));
        b.pushToken(tokAt(*h.src, "x",     CoreTokenKind::Word));
    }
    Tree t = std::move(b).finish();

    auto names = leafKindNames(t);
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "IntKw");
    EXPECT_EQ(names[1], "Identifier")
        << "await must demote to Identifier";
    EXPECT_EQ(names[2], "Identifier")
        << "third token must reach the second Identifier slot — proves cursor advanced";
    EXPECT_EQ(countCode(t, DiagnosticCode::P_ContextualKeywordResolution), 1u);
    EXPECT_EQ(countCode(t, DiagnosticCode::P_SchemaCursorDesync), 0u)
        << "demotion path must not trip the desync diagnostic";
    EXPECT_FALSE(t.diagnostics().hasErrors());
}

// ── I4: cascade-close still emits one-shot desync ───────────────────────

TEST(ContextualKeywords, CascadeCloseTripsOneShotDesync) {
    // OpenScope closed out of LIFO order forces a cascade-close. The
    // cascade walks each frame's leaveRule; on a frame whose parent
    // cursor isn't a RuleLeaf (e.g. when the test bypasses the schema's
    // expected nesting), leaveRule returns invalid and the desync
    // diagnostic fires exactly once.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "keywords": [ { "word": "do", "kind": "DoKw" } ],
      "shapes": {
        "root":  { "sequence": [ "outer" ] },
        "outer": { "sequence": [ "DoKw" ] }
      }
    })JSON";

    auto h = make("do", kCfg);
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root  = b.open(h.schema->rules().find("root"));
        // Open `root` again rather than `outer` — drives the cursor
        // off-track because root's slot is RuleLeaf(outer), not
        // RuleLeaf(root). On close, leaveRule fails and emits desync.
        auto inner = b.open(h.schema->rules().find("root"));
        b.pushToken(tokAt(*h.src, "do", CoreTokenKind::Word));
    }
    Tree t = std::move(b).finish();

    EXPECT_EQ(countCode(t, DiagnosticCode::P_SchemaCursorDesync), 1u)
        << "off-schema nesting must emit exactly one desync diagnostic";
}

// ── I5: P_ContextualKeywordResolution diagnostic content ────────────────

TEST(ContextualKeywords, ContextualResolutionDiagnosticContentIsPinned) {
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "keywords": [
        { "word": "int",   "kind": "IntKw" },
        { "word": "await", "kind": "AwaitKw", "contextual": true }
      ],
      "shapes": { "root": { "sequence": [ "IntKw", "Identifier" ] } }
    })JSON";

    auto h = make("int await", kCfg);
    ASSERT_NE(h.schema, nullptr);
    const auto awaitOffset = h.src->text().find("await");
    ASSERT_NE(awaitOffset, std::string_view::npos);

    const RuleId rootRule = h.schema->rules().find("root");
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(rootRule);
        b.pushToken(tokAt(*h.src, "int",   CoreTokenKind::Word));
        b.pushToken(tokAt(*h.src, "await", CoreTokenKind::Word));
    }
    Tree t = std::move(b).finish();

    auto const& diags = t.diagnostics().all();
    auto it = std::ranges::find_if(diags, [](auto const& d) {
        return d.code == DiagnosticCode::P_ContextualKeywordResolution;
    });
    ASSERT_NE(it, diags.end());
    EXPECT_EQ(it->severity, DiagnosticSeverity::Info);
    EXPECT_EQ(it->span.start(), awaitOffset);
    EXPECT_EQ(it->span.end(),   awaitOffset + 5);
    ASSERT_TRUE(it->ruleContext.has_value());
    EXPECT_EQ(it->ruleContext->v, rootRule.v);
    EXPECT_EQ(it->actual, "'await' as Identifier (demoted from AwaitKw)");
}

// ── I6: nested-rule demotion ────────────────────────────────────────────

TEST(ContextualKeywords, ContextualDemotionAtNestedRuleLevel) {
    // The cursor is the *current* rule's position. A contextual keyword
    // pushed inside a nested rule consults that rule's expectedSet, not
    // the outer's. Here the demotion fires deep in `inner`.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "keywords": [
        { "word": "int",   "kind": "IntKw" },
        { "word": "await", "kind": "AwaitKw", "contextual": true }
      ],
      "shapes": {
        "root":  { "sequence": [ "outer" ] },
        "outer": { "sequence": [ "IntKw", "inner" ] },
        "inner": { "sequence": [ "Identifier" ] }
      }
    })JSON";

    auto h = make("int await", kCfg);
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root  = b.open(h.schema->rules().find("root"));
        auto outer = b.open(h.schema->rules().find("outer"));
        b.pushToken(tokAt(*h.src, "int", CoreTokenKind::Word));
        {
            auto inner = b.open(h.schema->rules().find("inner"));
            b.pushToken(tokAt(*h.src, "await", CoreTokenKind::Word));
        }
    }
    Tree t = std::move(b).finish();

    auto names = leafKindNames(t);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "IntKw");
    EXPECT_EQ(names[1], "Identifier")
        << "contextual keyword inside `inner` must demote based on inner's expected set";
    EXPECT_EQ(countCode(t, DiagnosticCode::P_ContextualKeywordResolution), 1u);
    EXPECT_EQ(countCode(t, DiagnosticCode::P_SchemaCursorDesync), 0u);
    EXPECT_FALSE(t.diagnostics().hasErrors());
}

// ── Smoke pin: existing configs reload unchanged ─────────────────────────

TEST(ContextualKeywords, ToyConfigReloadsUnchanged) {
    auto loaded = GrammarSchema::loadShipped("toy");
    if (!loaded.has_value()) {
        FAIL() << "loadShipped(\"toy\") failed: "
               << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    }
    auto const& schema = **loaded;
    EXPECT_EQ(schema.reservedWordPolicy(), ReservedWordPolicy::Strict);
    EXPECT_FALSE(schema.lookupLexeme("var")[0].contextual);
    EXPECT_FALSE(schema.lookupLexeme("if")[0].contextual);
}

TEST(ContextualKeywords, CSubsetIntDeclEmitsNoContextualResolution) {
    // Smoke pin: under c-subset's default Strict reservedWordPolicy, a
    // clean `int x;` parse must not emit any P_ContextualKeywordResolution
    // — there are no contextual keywords to demote.
    auto loaded = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    auto src = SourceBuffer::fromString("int x;", "<csubset-smoke>");

    TreeBuilder b{src, schema};
    {
        auto root = b.open(schema->rules().find("root"));
        auto top  = b.open(schema->rules().find("topLevel"));
        auto tld  = b.open(schema->rules().find("topLevelDecl"));
        // FC4 c1: the specifier/declarator topLevelDecl shape — head
        // carries the type; the name lives in the declarator; `;` is the
        // tail's EndStatement arm.
        {
            auto head = b.open(schema->rules().find("topLevelHead"));
            auto ts   = b.open(schema->rules().find("typeSpecifierSeq"));
            b.pushToken(tokAt(*src, "int", CoreTokenKind::Word));
        }
        {
            auto list   = b.open(schema->rules().find("initDeclaratorList"));
            auto idecl  = b.open(schema->rules().find("initDeclarator"));
            auto dtor   = b.open(schema->rules().find("declarator"));
            auto direct = b.open(schema->rules().find("directDeclarator"));
            b.pushToken(tokAt(*src, "x", CoreTokenKind::Word));
        }
        {
            auto tail = b.open(schema->rules().find("topLevelDeclTail"));
            b.pushToken(tokAt(*src, ";", CoreTokenKind::Operator));
        }
    }
    Tree t = std::move(b).finish();
    EXPECT_EQ(countCode(t, DiagnosticCode::P_ContextualKeywordResolution), 0u);
    EXPECT_EQ(countCode(t, DiagnosticCode::P_SchemaCursorDesync), 0u);
    EXPECT_FALSE(t.diagnostics().hasErrors());
}

TEST(ContextualKeywords, CSubsetConfigReloadsUnchanged) {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    if (!loaded.has_value()) {
        FAIL() << "loadShipped(\"c-subset\") failed: "
               << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    }
    auto const& schema = **loaded;
    EXPECT_EQ(schema.reservedWordPolicy(), ReservedWordPolicy::Strict);
    EXPECT_FALSE(schema.lookupLexeme("if")[0].contextual);
    EXPECT_FALSE(schema.lookupLexeme("return")[0].contextual);
}
