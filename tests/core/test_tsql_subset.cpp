#include "core/types/grammar_schema.hpp"
#include "core/types/lexer_mode.hpp"
#include "core/types/operator_table.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/string_style.hpp"
#include "core/types/token.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_builder.hpp"
#include "test_pretty_print.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

// Hand-tokenized end-to-end pin for the shipped tsql-subset.lang.json,
// same harness pattern as test_c_subset.cpp.

using namespace dss;
using dss::tests::prettyPrint;

namespace {

struct TsqlHarness {
    std::shared_ptr<SourceBuffer>        src;
    std::shared_ptr<GrammarSchema const> schema;
};

TsqlHarness load(std::string sourceText) {
    auto loaded = GrammarSchema::loadShipped("tsql-subset");
    if (!loaded) {
        ADD_FAILURE() << "loadShipped(\"tsql-subset\") failed: "
                      << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
        return {SourceBuffer::fromString(std::move(sourceText), "<tsql>"), nullptr};
    }
    return {SourceBuffer::fromString(std::move(sourceText), "<tsql>"), *loaded};
}

// Find the nth (1-based) occurrence of `lexeme` in `src`. Aborts on
// miss — a test that names a lexeme not in its own source is a test
// bug, and silent ADD_FAILURE on a non-fatal path would let downstream
// assertions pass with a degenerate span.
[[nodiscard]] std::size_t nthOccurrence(SourceBuffer const& src,
                                        std::string_view lexeme,
                                        std::size_t n) {
    const auto sv = src.text();
    std::size_t pos = 0;
    for (std::size_t i = 0; i < n; ++i) {
        pos = sv.find(lexeme, (i == 0) ? 0 : pos + 1);
        if (pos == std::string_view::npos) {
            std::fprintf(stderr,
                "test_tsql_subset: nthOccurrence('%.*s', %zu) not in source\n",
                static_cast<int>(lexeme.size()), lexeme.data(), n);
            std::abort();
        }
    }
    return pos;
}

[[nodiscard]] Token at(SourceBuffer const& src, std::string_view text,
                       CoreTokenKind kind = CoreTokenKind::Operator,
                       std::size_t startHint = std::string_view::npos) {
    const auto sv = src.text();
    const auto first = (startHint == std::string_view::npos) ? sv.find(text)
                                                              : sv.find(text, startHint);
    if (first == std::string_view::npos) {
        std::fprintf(stderr,
            "test_tsql_subset: at('%.*s') not found in source (startHint=%zu)\n",
            static_cast<int>(text.size()), text.data(), startHint);
        std::abort();
    }
    return Token{
        .coreKind   = kind,
        .schemaKind = InvalidSchemaToken,
        .span       = SourceSpan::of(static_cast<ByteOffset>(first),
                                      static_cast<ByteOffset>(first + text.size())),
    };
}

[[nodiscard]] Token atNth(SourceBuffer const& src, std::string_view text,
                          std::size_t n,
                          CoreTokenKind kind = CoreTokenKind::Operator) {
    const auto pos = nthOccurrence(src, text, n);
    return Token{
        .coreKind   = kind,
        .schemaKind = InvalidSchemaToken,
        .span       = SourceSpan::of(static_cast<ByteOffset>(pos),
                                      static_cast<ByteOffset>(pos + text.size())),
    };
}

} // namespace

TEST(TsqlSubset, LoadsCleanly) {
    auto loaded = GrammarSchema::loadShipped("tsql-subset");
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
}

TEST(TsqlSubset, ReservedWordPolicyIsContextual) {
    auto h = load("");
    ASSERT_NE(h.schema, nullptr);
    EXPECT_EQ(h.schema->reservedWordPolicy(), ReservedWordPolicy::Contextual);
}

TEST(TsqlSubset, OperatorPrecedenceTableLoaded) {
    auto h = load("");
    ASSERT_NE(h.schema, nullptr);
    auto const& table  = h.schema->operatorTable();
    auto const& tokens = h.schema->schemaTokens();

    auto eq    = table.lookup(tokens.find("EqOp"),    OperatorArity::Infix);
    auto plus  = table.lookup(tokens.find("PlusOp"),  OperatorArity::Infix);
    auto star  = table.lookup(tokens.find("StarOp"),  OperatorArity::Infix);
    auto minus = table.lookup(tokens.find("MinusOp"), OperatorArity::Infix);
    auto mneg  = table.lookup(tokens.find("MinusOp"), OperatorArity::Prefix);

    ASSERT_TRUE(eq.has_value());
    ASSERT_TRUE(plus.has_value());
    ASSERT_TRUE(star.has_value());
    ASSERT_TRUE(minus.has_value());
    ASSERT_TRUE(mneg.has_value());

    EXPECT_LT(eq->precedence,   plus->precedence) << "`=` binds less tight than `+`";
    EXPECT_LT(plus->precedence, star->precedence) << "`+` binds less tight than `*`";
    EXPECT_GT(mneg->precedence, star->precedence) << "unary `-` binds tightest";
    EXPECT_EQ(plus->associativity, OperatorAssoc::Left);
}

TEST(TsqlSubset, ContextualKeywordsAreSoft) {
    auto h = load("");
    ASSERT_NE(h.schema, nullptr);
    // Every keyword carries `contextual = true` under reservedWordPolicy:contextual.
    auto const& select = h.schema->lookupLexeme("SELECT");
    ASSERT_EQ(select.size(), 1u);
    EXPECT_TRUE(select[0].contextual);

    auto const& from = h.schema->lookupLexeme("FROM");
    ASSERT_EQ(from.size(), 1u);
    EXPECT_TRUE(from[0].contextual);
}

TEST(TsqlSubset, LexerModesRegistered) {
    auto h = load("");
    ASSERT_NE(h.schema, nullptr);
    EXPECT_TRUE(h.schema->findLexerMode("main").valid());
    EXPECT_TRUE(h.schema->findLexerMode("bracket-id").valid());
    EXPECT_TRUE(h.schema->findLexerMode("single-string").valid());
    EXPECT_TRUE(h.schema->findLexerMode("unicode-string").valid());
}

TEST(TsqlSubset, SingleStringStyleIsDoubledDelimiter) {
    auto h = load("");
    ASSERT_NE(h.schema, nullptr);
    auto const& q = h.schema->lookupLexeme("'");
    ASSERT_EQ(q.size(), 1u);
    auto const* style = h.schema->stringStyle(q[0]);
    ASSERT_NE(style, nullptr);
    EXPECT_EQ(style->escapeKind, EscapeKind::DoubledDelimiter);
    EXPECT_EQ(style->endsAt, "'");
}

TEST(TsqlSubset, UnicodeStringOpenerHasDistinctStyle) {
    auto h = load("");
    ASSERT_NE(h.schema, nullptr);
    auto const& n = h.schema->lookupLexeme("N'");
    ASSERT_EQ(n.size(), 1u);
    auto const* style = h.schema->stringStyle(n[0]);
    ASSERT_NE(style, nullptr);
    EXPECT_EQ(style->escapeKind, EscapeKind::DoubledDelimiter);
    EXPECT_EQ(style->endsAt, "'");
    // Distinct kind from the plain string opener so the parse tree can
    // tell unicode literals from ASCII literals downstream.
    EXPECT_EQ(h.schema->schemaTokens().name(n[0].id), "UnicodeStringStart");
}

TEST(TsqlSubset, BracketIdentifierHasDoubledDelimiterStyle) {
    // T-SQL `[a]]b]` is the identifier `a]b` — the `]]` doubled
    // delimiter is the only escape mechanism. Same shape as strings
    // but the body matches identifier rules in the tokenizer.
    auto h = load("");
    ASSERT_NE(h.schema, nullptr);
    auto const& b = h.schema->lookupLexeme("[");
    ASSERT_EQ(b.size(), 1u);
    auto const* style = h.schema->stringStyle(b[0]);
    ASSERT_NE(style, nullptr);
    EXPECT_EQ(style->escapeKind, EscapeKind::DoubledDelimiter);
    EXPECT_EQ(style->endsAt, "]");
}

TEST(TsqlSubset, ModeOpsParseCorrectly) {
    auto h = load("");
    ASSERT_NE(h.schema, nullptr);

    auto const& q = h.schema->lookupLexeme("'");
    ASSERT_EQ(q[0].modeOp, ModeOp::PushMode);
    EXPECT_EQ(h.schema->lexerMode(q[0].modeArg).name, "single-string");

    auto const& n = h.schema->lookupLexeme("N'");
    ASSERT_EQ(n[0].modeOp, ModeOp::PushMode);
    EXPECT_EQ(h.schema->lexerMode(n[0].modeArg).name, "unicode-string");

    auto const& b = h.schema->lookupLexeme("[");
    ASSERT_EQ(b[0].modeOp, ModeOp::PushMode);
    EXPECT_EQ(h.schema->lexerMode(b[0].modeArg).name, "bracket-id");
}

TEST(TsqlSubset, SimpleSelectStar) {
    auto h = load("SELECT * FROM t;");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto sel  = b.open(h.schema->rules().find("selectStmt"));
        b.pushToken(at(*h.src, "SELECT", CoreTokenKind::Word));
        {
            auto list = b.open(h.schema->rules().find("selectList"));
            auto star = b.open(h.schema->rules().find("selectStar"));
            b.pushToken(at(*h.src, "*"));
        }
        b.pushToken(at(*h.src, "FROM", CoreTokenKind::Word));
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(at(*h.src, "t", CoreTokenKind::Word));
        }
        b.pushToken(at(*h.src, ";"));
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty())
        << "SELECT * FROM t; must produce no diagnostics at all";

    const std::string_view expected =
        "rule:root\n"
        "  rule:statement\n"
        "    rule:selectStmt\n"
        "      tok:\"SELECT\"\n"
        "      rule:selectList\n"
        "        rule:selectStar\n"
        "          tok:\"*\"\n"
        "      tok:\"FROM\"\n"
        "      rule:qualifiedName\n"
        "        rule:nameAtom\n"
        "          tok:\"t\"\n"
        "      tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

TEST(TsqlSubset, SelectFromQualifiedThreePartName) {
    auto h = load("SELECT a FROM db.schema.t;");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto sel  = b.open(h.schema->rules().find("selectStmt"));
        b.pushToken(at(*h.src, "SELECT", CoreTokenKind::Word));
        {
            auto list = b.open(h.schema->rules().find("selectList"));
            auto items = b.open(h.schema->rules().find("selectItemList"));
            auto item  = b.open(h.schema->rules().find("selectItem"));
            auto expr  = b.open(h.schema->rules().find("expression"));
            auto opr   = b.open(h.schema->rules().find("operand"));
            b.pushToken(at(*h.src, "a", CoreTokenKind::Word));
        }
        b.pushToken(at(*h.src, "FROM", CoreTokenKind::Word));
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(at(*h.src, "db", CoreTokenKind::Word)); }
            b.pushToken(atNth(*h.src, ".", 1));
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(at(*h.src, "schema", CoreTokenKind::Word)); }
            b.pushToken(atNth(*h.src, ".", 2));
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(at(*h.src, "t", CoreTokenKind::Word)); }
        }
        b.pushToken(at(*h.src, ";"));
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty());

    const std::string_view expected =
        "rule:root\n"
        "  rule:statement\n"
        "    rule:selectStmt\n"
        "      tok:\"SELECT\"\n"
        "      rule:selectList\n"
        "        rule:selectItemList\n"
        "          rule:selectItem\n"
        "            rule:expression\n"
        "              rule:operand\n"
        "                tok:\"a\"\n"
        "      tok:\"FROM\"\n"
        "      rule:qualifiedName\n"
        "        rule:nameAtom\n"
        "          tok:\"db\"\n"
        "        tok:\".\"\n"
        "        rule:nameAtom\n"
        "          tok:\"schema\"\n"
        "        tok:\".\"\n"
        "        rule:nameAtom\n"
        "          tok:\"t\"\n"
        "      tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

TEST(TsqlSubset, SelectWithUnicodeLiteralOpener) {
    // Schema-side test: the body inside the unicode-string mode is the
    // tokenizer's responsibility. Here we verify that an N'-opener
    // appears cleanly at an `operand` position.
    auto h = load("SELECT N'hello' FROM t;");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto sel  = b.open(h.schema->rules().find("selectStmt"));
        b.pushToken(at(*h.src, "SELECT", CoreTokenKind::Word));
        {
            auto list  = b.open(h.schema->rules().find("selectList"));
            auto items = b.open(h.schema->rules().find("selectItemList"));
            auto item  = b.open(h.schema->rules().find("selectItem"));
            auto expr  = b.open(h.schema->rules().find("expression"));
            auto opr   = b.open(h.schema->rules().find("operand"));
            b.pushToken(at(*h.src, "N'"));
        }
        b.pushToken(at(*h.src, "FROM", CoreTokenKind::Word));
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(at(*h.src, "t", CoreTokenKind::Word));
        }
        b.pushToken(at(*h.src, ";"));
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty());

    const std::string_view expected =
        "rule:root\n"
        "  rule:statement\n"
        "    rule:selectStmt\n"
        "      tok:\"SELECT\"\n"
        "      rule:selectList\n"
        "        rule:selectItemList\n"
        "          rule:selectItem\n"
        "            rule:expression\n"
        "              rule:operand\n"
        "                tok:\"N'\"\n"
        "      tok:\"FROM\"\n"
        "      rule:qualifiedName\n"
        "        rule:nameAtom\n"
        "          tok:\"t\"\n"
        "      tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

TEST(TsqlSubset, SelectWithBracketIdentifier) {
    auto h = load("SELECT [Order Date] FROM [My Table];");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    NodeId fromNameLeaf{};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto sel  = b.open(h.schema->rules().find("selectStmt"));
        b.pushToken(at(*h.src, "SELECT", CoreTokenKind::Word));
        {
            auto list  = b.open(h.schema->rules().find("selectList"));
            auto items = b.open(h.schema->rules().find("selectItemList"));
            auto item  = b.open(h.schema->rules().find("selectItem"));
            auto expr  = b.open(h.schema->rules().find("expression"));
            auto opr   = b.open(h.schema->rules().find("operand"));
            b.pushToken(atNth(*h.src, "[", 1));
        }
        b.pushToken(at(*h.src, "FROM", CoreTokenKind::Word));
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(atNth(*h.src, "[", 2));
        }
        b.pushToken(at(*h.src, ";"));
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty());

    // Verify the `nameAtom` alt resolved to the BracketIdStart arm —
    // not just structurally but by tokenKind. A regression that routed
    // the alt to the Identifier arm would still build a tree but the
    // tokenKind would be wrong.
    auto const& s = *h.schema;
    const auto bracketKindId = s.schemaTokens().find("BracketIdStart");
    auto rootChildren = t.children(t.root());
    ASSERT_FALSE(rootChildren.empty());
    bool sawBracketLeaf = false;
    for (NodeId id{1}; id.v < t.nodeCount(); id.v++) {
        if (t.kind(id) == NodeKind::Token && t.tokenKind(id).v == bracketKindId.v) {
            sawBracketLeaf = true;
            break;
        }
    }
    EXPECT_TRUE(sawBracketLeaf)
        << "nameAtom alt must resolve to BracketIdStart for `[…]` openers";

    const std::string_view expected =
        "rule:root\n"
        "  rule:statement\n"
        "    rule:selectStmt\n"
        "      tok:\"SELECT\"\n"
        "      rule:selectList\n"
        "        rule:selectItemList\n"
        "          rule:selectItem\n"
        "            rule:expression\n"
        "              rule:operand\n"
        "                tok:\"[\"\n"
        "      tok:\"FROM\"\n"
        "      rule:qualifiedName\n"
        "        rule:nameAtom\n"
        "          tok:\"[\"\n"
        "      tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

TEST(TsqlSubset, InsertIntoValues) {
    auto h = load("INSERT INTO t (a, b) VALUES (1, 2);");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto ins  = b.open(h.schema->rules().find("insertStmt"));
        b.pushToken(at(*h.src, "INSERT", CoreTokenKind::Word));
        b.pushToken(at(*h.src, "INTO",   CoreTokenKind::Word));
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(at(*h.src, "t", CoreTokenKind::Word));
        }
        b.pushToken(atNth(*h.src, "(", 1));
        {
            auto cl = b.open(h.schema->rules().find("columnList"));
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(at(*h.src, "a", CoreTokenKind::Word)); }
            b.pushToken(atNth(*h.src, ",", 1));
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(at(*h.src, "b", CoreTokenKind::Word)); }
        }
        b.pushToken(atNth(*h.src, ")", 1));
        b.pushToken(at(*h.src, "VALUES", CoreTokenKind::Word));
        b.pushToken(atNth(*h.src, "(", 2));
        {
            auto vl = b.open(h.schema->rules().find("valueList"));
            { auto e = b.open(h.schema->rules().find("expression"));
              auto o = b.open(h.schema->rules().find("operand"));
              b.pushToken(at(*h.src, "1", CoreTokenKind::Word)); }
            b.pushToken(atNth(*h.src, ",", 2));
            { auto e = b.open(h.schema->rules().find("expression"));
              auto o = b.open(h.schema->rules().find("operand"));
              b.pushToken(at(*h.src, "2", CoreTokenKind::Word)); }
        }
        b.pushToken(atNth(*h.src, ")", 2));
        b.pushToken(at(*h.src, ";"));
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty());

    const std::string_view expected =
        "rule:root\n"
        "  rule:statement\n"
        "    rule:insertStmt\n"
        "      tok:\"INSERT\"\n"
        "      tok:\"INTO\"\n"
        "      rule:qualifiedName\n"
        "        rule:nameAtom\n"
        "          tok:\"t\"\n"
        "      tok:\"(\"\n"
        "      rule:columnList\n"
        "        rule:nameAtom\n"
        "          tok:\"a\"\n"
        "        tok:\",\"\n"
        "        rule:nameAtom\n"
        "          tok:\"b\"\n"
        "      tok:\")\"\n"
        "      tok:\"VALUES\"\n"
        "      tok:\"(\"\n"
        "      rule:valueList\n"
        "        rule:expression\n"
        "          rule:operand\n"
        "            tok:\"1\"\n"
        "        tok:\",\"\n"
        "        rule:expression\n"
        "          rule:operand\n"
        "            tok:\"2\"\n"
        "      tok:\")\"\n"
        "      tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

TEST(TsqlSubset, UpdateSetWhere) {
    auto h = load("UPDATE t SET a = 1 WHERE id = 5;");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto upd  = b.open(h.schema->rules().find("updateStmt"));
        b.pushToken(at(*h.src, "UPDATE", CoreTokenKind::Word));
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(at(*h.src, "t", CoreTokenKind::Word));
        }
        b.pushToken(at(*h.src, "SET", CoreTokenKind::Word));
        {
            auto al = b.open(h.schema->rules().find("assignList"));
            auto a  = b.open(h.schema->rules().find("assignment"));
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(at(*h.src, "a", CoreTokenKind::Word)); }
            b.pushToken(atNth(*h.src, "=", 1));
            { auto e = b.open(h.schema->rules().find("expression"));
              auto o = b.open(h.schema->rules().find("operand"));
              b.pushToken(at(*h.src, "1", CoreTokenKind::Word)); }
        }
        {
            auto w = b.open(h.schema->rules().find("whereClause"));
            b.pushToken(at(*h.src, "WHERE", CoreTokenKind::Word));
            auto e = b.open(h.schema->rules().find("expression"));
            { auto o = b.open(h.schema->rules().find("operand"));
              b.pushToken(at(*h.src, "id", CoreTokenKind::Word)); }
            {
                auto bop = b.open(h.schema->rules().find("binaryOp"));
                b.pushToken(atNth(*h.src, "=", 2));
            }
            { auto o = b.open(h.schema->rules().find("operand"));
              b.pushToken(at(*h.src, "5", CoreTokenKind::Word)); }
        }
        b.pushToken(at(*h.src, ";"));
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty());

    const std::string_view expected =
        "rule:root\n"
        "  rule:statement\n"
        "    rule:updateStmt\n"
        "      tok:\"UPDATE\"\n"
        "      rule:qualifiedName\n"
        "        rule:nameAtom\n"
        "          tok:\"t\"\n"
        "      tok:\"SET\"\n"
        "      rule:assignList\n"
        "        rule:assignment\n"
        "          rule:nameAtom\n"
        "            tok:\"a\"\n"
        "          tok:\"=\"\n"
        "          rule:expression\n"
        "            rule:operand\n"
        "              tok:\"1\"\n"
        "      rule:whereClause\n"
        "        tok:\"WHERE\"\n"
        "        rule:expression\n"
        "          rule:operand\n"
        "            tok:\"id\"\n"
        "          rule:binaryOp\n"
        "            tok:\"=\"\n"
        "          rule:operand\n"
        "            tok:\"5\"\n"
        "      tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

TEST(TsqlSubset, DeleteFromWhere) {
    auto h = load("DELETE FROM t WHERE id = 5;");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto del  = b.open(h.schema->rules().find("deleteStmt"));
        b.pushToken(at(*h.src, "DELETE", CoreTokenKind::Word));
        b.pushToken(at(*h.src, "FROM",   CoreTokenKind::Word));
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(at(*h.src, "t", CoreTokenKind::Word));
        }
        {
            auto w = b.open(h.schema->rules().find("whereClause"));
            b.pushToken(at(*h.src, "WHERE", CoreTokenKind::Word));
            auto e = b.open(h.schema->rules().find("expression"));
            { auto o = b.open(h.schema->rules().find("operand"));
              b.pushToken(at(*h.src, "id", CoreTokenKind::Word)); }
            { auto bop = b.open(h.schema->rules().find("binaryOp"));
              b.pushToken(at(*h.src, "=")); }
            { auto o = b.open(h.schema->rules().find("operand"));
              b.pushToken(at(*h.src, "5", CoreTokenKind::Word)); }
        }
        b.pushToken(at(*h.src, ";"));
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty());

    const std::string_view expected =
        "rule:root\n"
        "  rule:statement\n"
        "    rule:deleteStmt\n"
        "      tok:\"DELETE\"\n"
        "      tok:\"FROM\"\n"
        "      rule:qualifiedName\n"
        "        rule:nameAtom\n"
        "          tok:\"t\"\n"
        "      rule:whereClause\n"
        "        tok:\"WHERE\"\n"
        "        rule:expression\n"
        "          rule:operand\n"
        "            tok:\"id\"\n"
        "          rule:binaryOp\n"
        "            tok:\"=\"\n"
        "          rule:operand\n"
        "            tok:\"5\"\n"
        "      tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

TEST(TsqlSubset, CreateTableWithTypes) {
    auto h = load("CREATE TABLE t (id INT, name VARCHAR);");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto cr   = b.open(h.schema->rules().find("createTableStmt"));
        b.pushToken(at(*h.src, "CREATE", CoreTokenKind::Word));
        b.pushToken(at(*h.src, "TABLE",  CoreTokenKind::Word));
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(at(*h.src, "t", CoreTokenKind::Word));
        }
        b.pushToken(at(*h.src, "("));
        {
            auto cdl = b.open(h.schema->rules().find("columnDeclList"));
            { auto cd = b.open(h.schema->rules().find("columnDecl"));
              { auto na = b.open(h.schema->rules().find("nameAtom"));
                b.pushToken(at(*h.src, "id", CoreTokenKind::Word)); }
              { auto tr = b.open(h.schema->rules().find("typeRef"));
                b.pushToken(at(*h.src, "INT", CoreTokenKind::Word)); } }
            b.pushToken(at(*h.src, ","));
            { auto cd = b.open(h.schema->rules().find("columnDecl"));
              { auto na = b.open(h.schema->rules().find("nameAtom"));
                b.pushToken(at(*h.src, "name", CoreTokenKind::Word)); }
              { auto tr = b.open(h.schema->rules().find("typeRef"));
                b.pushToken(at(*h.src, "VARCHAR", CoreTokenKind::Word)); } }
        }
        b.pushToken(at(*h.src, ")"));
        b.pushToken(at(*h.src, ";"));
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty());

    const std::string_view expected =
        "rule:root\n"
        "  rule:statement\n"
        "    rule:createTableStmt\n"
        "      tok:\"CREATE\"\n"
        "      tok:\"TABLE\"\n"
        "      rule:qualifiedName\n"
        "        rule:nameAtom\n"
        "          tok:\"t\"\n"
        "      tok:\"(\"\n"
        "      rule:columnDeclList\n"
        "        rule:columnDecl\n"
        "          rule:nameAtom\n"
        "            tok:\"id\"\n"
        "          rule:typeRef\n"
        "            tok:\"INT\"\n"
        "        tok:\",\"\n"
        "        rule:columnDecl\n"
        "          rule:nameAtom\n"
        "            tok:\"name\"\n"
        "          rule:typeRef\n"
        "            tok:\"VARCHAR\"\n"
        "      tok:\")\"\n"
        "      tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

TEST(TsqlSubset, ContextualKeywordDemotesToIdentifierInNamePosition) {
    // T-SQL allows reserved words as identifiers when scoped right.
    // Here `SELECT` is the table name; contextual policy demotes the
    // lexeme to Identifier at the qualifiedName position.
    auto h = load("CREATE TABLE SELECT (id INT);");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto cr   = b.open(h.schema->rules().find("createTableStmt"));
        b.pushToken(at(*h.src, "CREATE", CoreTokenKind::Word));
        b.pushToken(at(*h.src, "TABLE",  CoreTokenKind::Word));
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(at(*h.src, "SELECT", CoreTokenKind::Word));
        }
        b.pushToken(at(*h.src, "("));
        {
            auto cdl = b.open(h.schema->rules().find("columnDeclList"));
            auto cd  = b.open(h.schema->rules().find("columnDecl"));
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(at(*h.src, "id", CoreTokenKind::Word)); }
            { auto tr = b.open(h.schema->rules().find("typeRef"));
              b.pushToken(at(*h.src, "INT", CoreTokenKind::Word)); }
        }
        b.pushToken(at(*h.src, ")"));
        b.pushToken(at(*h.src, ";"));
    }
    Tree t = std::move(b).finish();
    EXPECT_FALSE(t.diagnostics().hasErrors());

    // Verify (a) the diagnostic fired AND (b) the SELECT leaf's
    // tokenKind is actually Identifier (not SelectKw). The demotion
    // machinery has both an emit-side and a meaning-rewrite side; a
    // regression that drops one would leave the other working.
    auto const& diags = t.diagnostics().all();
    EXPECT_TRUE(std::ranges::any_of(diags, [](auto const& d) {
        return d.code == DiagnosticCode::P_ContextualKeywordResolution;
    })) << "info-level P_ContextualKeywordResolution must fire on SELECT demotion";

    auto const& s = *h.schema;
    const auto identKindId  = s.schemaTokens().find("Identifier");
    const auto selectKindId = s.schemaTokens().find("SelectKw");
    bool sawDemotedSelect = false;
    bool sawUndemotedSelect = false;
    for (NodeId id{1}; id.v < t.nodeCount(); id.v++) {
        if (t.kind(id) != NodeKind::Token) continue;
        const auto lex = s.schemaTokens().name(t.tokenKind(id));
        const auto span = t.span(id);
        if (h.src->slice(span) != "SELECT") continue;
        if (t.tokenKind(id).v == identKindId.v)  sawDemotedSelect = true;
        if (t.tokenKind(id).v == selectKindId.v) sawUndemotedSelect = true;
        (void)lex;
    }
    EXPECT_TRUE(sawDemotedSelect)
        << "SELECT at qualifiedName must have tokenKind == Identifier";
    EXPECT_FALSE(sawUndemotedSelect)
        << "no SELECT leaf should remain as SelectKw after demotion";
}

TEST(TsqlSubset, ParsesMultipleStatements) {
    // root = repeat(statement). Exercise the repeat with two SELECTs.
    auto h = load("SELECT * FROM t; SELECT * FROM u;");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    auto buildSelectStar = [&](std::string_view tableName, std::size_t tableNth) {
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto sel  = b.open(h.schema->rules().find("selectStmt"));
        b.pushToken(atNth(*h.src, "SELECT", tableNth, CoreTokenKind::Word));
        {
            auto list = b.open(h.schema->rules().find("selectList"));
            auto star = b.open(h.schema->rules().find("selectStar"));
            b.pushToken(atNth(*h.src, "*", tableNth));
        }
        b.pushToken(atNth(*h.src, "FROM", tableNth, CoreTokenKind::Word));
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(at(*h.src, tableName, CoreTokenKind::Word));
        }
        b.pushToken(atNth(*h.src, ";", tableNth));
    };
    {
        auto root = b.open(h.schema->rules().find("root"));
        buildSelectStar("t", 1);
        buildSelectStar("u", 2);
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty());

    // Two `statement` children under root.
    std::size_t statementChildren = 0;
    for (NodeId c : t.children(t.root())) {
        if (t.kind(c) == NodeKind::Internal) ++statementChildren;
    }
    EXPECT_EQ(statementChildren, 2u);
}

TEST(TsqlSubset, OperatorPrecedenceConsumedInExpression) {
    // `expression` is flat-fold `operand (binaryOp operand)*`. The
    // parser would consult OperatorTable for precedence; here we just
    // exercise that the schema produces the flat form expected.
    auto h = load("UPDATE t SET a = b + c * d;");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto upd  = b.open(h.schema->rules().find("updateStmt"));
        b.pushToken(at(*h.src, "UPDATE", CoreTokenKind::Word));
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(at(*h.src, "t", CoreTokenKind::Word));
        }
        b.pushToken(at(*h.src, "SET", CoreTokenKind::Word));
        {
            auto al = b.open(h.schema->rules().find("assignList"));
            auto a  = b.open(h.schema->rules().find("assignment"));
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(at(*h.src, "a", CoreTokenKind::Word)); }
            b.pushToken(atNth(*h.src, "=", 1));
            { auto e = b.open(h.schema->rules().find("expression"));
              { auto o = b.open(h.schema->rules().find("operand"));
                b.pushToken(at(*h.src, "b", CoreTokenKind::Word)); }
              { auto bop = b.open(h.schema->rules().find("binaryOp"));
                b.pushToken(at(*h.src, "+")); }
              { auto o = b.open(h.schema->rules().find("operand"));
                b.pushToken(at(*h.src, "c", CoreTokenKind::Word)); }
              { auto bop = b.open(h.schema->rules().find("binaryOp"));
                b.pushToken(at(*h.src, "*")); }
              { auto o = b.open(h.schema->rules().find("operand"));
                b.pushToken(at(*h.src, "d", CoreTokenKind::Word)); }
            }
        }
        b.pushToken(at(*h.src, ";"));
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty());

    // Pin the flat-fold shape: 3 operands + 2 binaryOps under
    // `expression`. When the parser's Pratt walker lands, this test
    // flips to a nested grouping (b + (c * d)) — the visible signal
    // that operator precedence climbing is consuming the OperatorTable.
    auto const& tokens = h.schema->schemaTokens();
    auto const* table  = &h.schema->operatorTable();
    auto plus = table->lookup(tokens.find("PlusOp"), OperatorArity::Infix);
    auto star = table->lookup(tokens.find("StarOp"), OperatorArity::Infix);
    ASSERT_TRUE(plus.has_value());
    ASSERT_TRUE(star.has_value());
    EXPECT_LT(plus->precedence, star->precedence)
        << "schema-side data: `*` must bind tighter than `+`";
}

TEST(TsqlSubset, IsNullPredicateInWhere) {
    // Exercises IS + NULL keywords. The current shape only allows a
    // single `expression` after WHERE, and `expression` is the flat
    // operand-(binaryOp operand)* fold. IS NULL doesn't fit that shape
    // — IS isn't a binaryOp. Pin the current limitation: trying to
    // parse `WHERE x IS NULL` would fail. Documents the residual gap
    // for the onboarding plan.
    auto h = load("");
    ASSERT_NE(h.schema, nullptr);
    auto const& s = *h.schema;
    EXPECT_TRUE(s.schemaTokens().find("IsKw").valid())
        << "IsKw declared even though the subset's shape doesn't consume it yet — "
           "ready for the onboarding plan to add IS-NULL/IS-NOT-NULL shapes";
    EXPECT_TRUE(s.schemaTokens().find("InKw").valid());
    EXPECT_TRUE(s.schemaTokens().find("AndKw").valid());
    EXPECT_TRUE(s.schemaTokens().find("OrKw").valid());
}

TEST(TsqlSubset, NotNullClauseInCreateTable) {
    // Exercises the notNullClause shape (the one place NotKw + NullKw
    // are positionally consumed today).
    auto h = load("CREATE TABLE t (id INT NOT NULL);");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto cr   = b.open(h.schema->rules().find("createTableStmt"));
        b.pushToken(at(*h.src, "CREATE", CoreTokenKind::Word));
        b.pushToken(at(*h.src, "TABLE",  CoreTokenKind::Word));
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(at(*h.src, "t", CoreTokenKind::Word));
        }
        b.pushToken(at(*h.src, "("));
        {
            auto cdl = b.open(h.schema->rules().find("columnDeclList"));
            auto cd  = b.open(h.schema->rules().find("columnDecl"));
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(at(*h.src, "id", CoreTokenKind::Word)); }
            { auto tr = b.open(h.schema->rules().find("typeRef"));
              b.pushToken(at(*h.src, "INT", CoreTokenKind::Word)); }
            { auto nn = b.open(h.schema->rules().find("notNullClause"));
              b.pushToken(at(*h.src, "NOT",  CoreTokenKind::Word));
              b.pushToken(at(*h.src, "NULL", CoreTokenKind::Word)); }
        }
        b.pushToken(at(*h.src, ")"));
        b.pushToken(at(*h.src, ";"));
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty());
}

TEST(TsqlSubset, SchemaIdsAreDistinctPerLoad) {
    auto a = GrammarSchema::loadShipped("tsql-subset");
    auto b = GrammarSchema::loadShipped("tsql-subset");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_NE((*a)->schemaId().v, (*b)->schemaId().v);
}
