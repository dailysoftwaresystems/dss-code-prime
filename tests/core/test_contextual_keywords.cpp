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

// Contextual keywords + reservedWordPolicy resolution. The builder
// consults `expectedSet(cursor)` at pushToken time and demotes any
// `contextual: true` keyword whose schemaTokenId isn't in the expected
// set. `reservedWordPolicy: "contextual"` implicitly marks every keyword
// contextual; `strict` (the default) leaves them all hard.

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
    // `contextual` is keyword-only; on a token entry the loader rejects.
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
    // `await` is contextual. Grammar: root → sequence[IntKw, Identifier].
    // After consuming `int`, expectedSet is {Identifier}; `await`'s
    // AwaitKw isn't in that set, so the lexeme demotes to Identifier.
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
    // Same `await` keyword, but the grammar root accepts it as the
    // first token. expectedSet at root contains AwaitKw → no demotion.
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
    // `if` is NOT marked contextual. Even when expectedSet says
    // Identifier, a hard keyword stays as the keyword (matches v1
    // behaviour, no demotion path runs).
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
    // Mimics T-SQL: `reservedWordPolicy: "contextual"` makes every
    // keyword soft. `select` here is the FIRST token expected by root;
    // it stays as SelectKw. After consuming `select`, the next slot is
    // Identifier — so `from` (a contextual keyword now) demotes.
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
    // No open frame ⇒ cursor stays default-invalid. The build emits a
    // P_BuilderInvariant for pushToken-with-no-frame; the test focuses
    // on the cursor-fallback behaviour: a contextual keyword pushed
    // outside any rule must NOT be demoted (we don't know what's
    // expected, so we keep the keyword).
    //
    // The pushToken is dropped via the invariant guard, so this test
    // exercises the loader side of the contract: `await` retains its
    // contextual flag, and the schema reports it correctly.
    constexpr std::string_view kCfg = R"JSON({
      "dssSchemaVersion": 1,
      "language": { "name": "X", "version": "0.1.0" },
      "keywords": [ { "word": "await", "kind": "AwaitKw", "contextual": true } ],
      "shapes": { "root": { "sequence": [ "Identifier" ] } }
    })JSON";
    auto loaded = GrammarSchema::loadFromText(kCfg);
    ASSERT_TRUE(loaded.has_value());
    auto const& m = (*loaded)->lookupLexeme("await");
    ASSERT_EQ(m.size(), 1u);
    EXPECT_TRUE(m[0].contextual);
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
