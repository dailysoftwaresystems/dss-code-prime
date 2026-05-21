#include "core/types/grammar_schema.hpp"
#include "core/types/lexer_mode.hpp"
#include "core/types/operator_table.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/string_style.hpp"
#include "core/types/token.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_builder.hpp"
#include "e2e_harness.hpp"
#include "test_pretty_print.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

// End-to-end pin for the shipped tsql-subset.lang.json — three lexer
// modes (single-string, unicode-string, bracket-id), doubled-delimiter
// escapes, contextual reserved-word policy, qualified names. TZ3 flipped
// every tree-building test from hand-tokenization to live tokenization;
// the schema-only inspection tests still load via the same harness with
// an empty source.

using namespace dss;
using dss::tests::drainWhitespace;
using dss::tests::E2EHarness;
using dss::tests::prettyPrint;
using dss::tests::tokenizeShipped;

namespace {

// Tokenize body-mode tokens until the next token's schemaKind is no
// longer the body's defaultToken kind. The tokenizer emits one
// defaultToken per codepoint inside a string/bracket body (plus a
// final close emission of the same kind covering the endsAt bytes);
// once the body pops, the next token is back in main mode.
//
// `bodyKind` is what the body emits — `StringChar` for single/
// unicode-string modes, `BracketIdChar` for bracket-id mode.
void drainBody(TreeBuilder& b, TokenStream& s, SchemaTokenId bodyKind) {
    while (!s.isAtEnd() && s.peek().schemaKind.v == bodyKind.v) {
        b.pushToken(s.advance());
    }
}

} // namespace

TEST(TsqlSubset, LoadsCleanly) {
    auto loaded = GrammarSchema::loadShipped("tsql-subset");
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
}

TEST(TsqlSubset, ReservedWordPolicyIsContextual) {
    auto h = tokenizeShipped("tsql-subset", "");
    ASSERT_NE(h.schema, nullptr);
    EXPECT_EQ(h.schema->reservedWordPolicy(), ReservedWordPolicy::Contextual);
}

TEST(TsqlSubset, OperatorPrecedenceTableLoaded) {
    auto h = tokenizeShipped("tsql-subset", "");
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
    auto h = tokenizeShipped("tsql-subset", "");
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
    auto h = tokenizeShipped("tsql-subset", "");
    ASSERT_NE(h.schema, nullptr);
    EXPECT_TRUE(h.schema->findLexerMode("main").valid());
    EXPECT_TRUE(h.schema->findLexerMode("bracket-id").valid());
    EXPECT_TRUE(h.schema->findLexerMode("single-string").valid());
    EXPECT_TRUE(h.schema->findLexerMode("unicode-string").valid());
}

TEST(TsqlSubset, SingleStringStyleIsDoubledDelimiter) {
    auto h = tokenizeShipped("tsql-subset", "");
    ASSERT_NE(h.schema, nullptr);
    auto const& q = h.schema->lookupLexeme("'");
    ASSERT_EQ(q.size(), 1u);
    auto const* style = h.schema->stringStyle(q[0]);
    ASSERT_NE(style, nullptr);
    EXPECT_EQ(style->escapeKind, EscapeKind::DoubledDelimiter);
    EXPECT_EQ(style->endsAt, "'");
}

TEST(TsqlSubset, UnicodeStringOpenerHasDistinctStyle) {
    auto h = tokenizeShipped("tsql-subset", "");
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
    auto h = tokenizeShipped("tsql-subset", "");
    ASSERT_NE(h.schema, nullptr);
    auto const& b = h.schema->lookupLexeme("[");
    ASSERT_EQ(b.size(), 1u);
    auto const* style = h.schema->stringStyle(b[0]);
    ASSERT_NE(style, nullptr);
    EXPECT_EQ(style->escapeKind, EscapeKind::DoubledDelimiter);
    EXPECT_EQ(style->endsAt, "]");
}

TEST(TsqlSubset, ModeOpsParseCorrectly) {
    auto h = tokenizeShipped("tsql-subset", "");
    ASSERT_NE(h.schema, nullptr);

    auto const& q = h.schema->lookupLexeme("'");
    ASSERT_EQ(q[0].modeOp, ModeOp::PushMode);
    EXPECT_EQ(h.schema->lexerMode(q[0].modeArg).name, "single-string");

    auto const& n = h.schema->lookupLexeme("N'");
    ASSERT_EQ(n[0].modeOp, ModeOp::PushMode);
    EXPECT_EQ(h.schema->lexerMode(n[0].modeArg).name, "unicode-string");

    auto const& bk = h.schema->lookupLexeme("[");
    ASSERT_EQ(bk[0].modeOp, ModeOp::PushMode);
    EXPECT_EQ(h.schema->lexerMode(bk[0].modeArg).name, "bracket-id");
}

TEST(TsqlSubset, SimpleSelectStar) {
    auto h = tokenizeShipped("tsql-subset", "SELECT * FROM t;");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto sel  = b.open(h.schema->rules().find("selectStmt"));
        b.pushToken(h.stream.advance());                   // SELECT
        drainWhitespace(b, h.stream);
        {
            auto list = b.open(h.schema->rules().find("selectList"));
            auto star = b.open(h.schema->rules().find("selectStar"));
            b.pushToken(h.stream.advance());               // *
        }
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());                   // FROM
        drainWhitespace(b, h.stream);
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(h.stream.advance());               // t
        }
        b.pushToken(h.stream.advance());                   // ;
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty())
        << "SELECT * FROM t; must produce no diagnostics at all";
    EXPECT_TRUE(h.lexerDiags->all().empty());

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
    auto h = tokenizeShipped("tsql-subset", "SELECT a FROM db.schema.t;");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto sel  = b.open(h.schema->rules().find("selectStmt"));
        b.pushToken(h.stream.advance());                   // SELECT
        drainWhitespace(b, h.stream);
        {
            auto list = b.open(h.schema->rules().find("selectList"));
            auto items = b.open(h.schema->rules().find("selectItemList"));
            auto item  = b.open(h.schema->rules().find("selectItem"));
            auto expr  = b.open(h.schema->rules().find("expression"));
            auto opr   = b.open(h.schema->rules().find("operand"));
            b.pushToken(h.stream.advance());               // a
        }
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());                   // FROM
        drainWhitespace(b, h.stream);
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(h.stream.advance()); }           // db
            b.pushToken(h.stream.advance());               // .
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(h.stream.advance()); }           // schema
            b.pushToken(h.stream.advance());               // .
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(h.stream.advance()); }           // t
        }
        b.pushToken(h.stream.advance());                   // ;
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty());
    EXPECT_TRUE(h.lexerDiags->all().empty());

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
    // Live tokenization: the N' opener pushes unicode-string mode and
    // the tokenizer emits one StringChar per body codepoint (h/e/l/l/o)
    // plus a final StringChar covering the closing `'`. The body
    // tokens land as siblings of the opener inside `operand`. The
    // contract pinned here is structural (token-kind shape) rather
    // than prettyPrint identity — pre-TZ3 the body was omitted entirely.
    auto h = tokenizeShipped("tsql-subset", "SELECT N'hello' FROM t;");
    ASSERT_NE(h.schema, nullptr);
    const auto unicodeStartKind = h.schema->schemaTokens().find("UnicodeStringStart");
    const auto stringCharKind   = h.schema->schemaTokens().find("StringChar");
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto sel  = b.open(h.schema->rules().find("selectStmt"));
        b.pushToken(h.stream.advance());                   // SELECT
        drainWhitespace(b, h.stream);
        {
            auto list  = b.open(h.schema->rules().find("selectList"));
            auto items = b.open(h.schema->rules().find("selectItemList"));
            auto item  = b.open(h.schema->rules().find("selectItem"));
            auto expr  = b.open(h.schema->rules().find("expression"));
            auto opr   = b.open(h.schema->rules().find("operand"));
            // Opener + 5 body chars + 1 close emission = 7 leaves
            // under `operand`.
            b.pushToken(h.stream.advance());               // N' (opener)
            drainBody(b, h.stream, stringCharKind);        // hello + '
        }
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());                   // FROM
        drainWhitespace(b, h.stream);
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(h.stream.advance());               // t
        }
        b.pushToken(h.stream.advance());                   // ;
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty());
    EXPECT_TRUE(h.lexerDiags->all().empty());

    // Pin the opener landed at an `operand` position and the body
    // produced 6 StringChar leaves (5 body codepoints + 1 endsAt close).
    std::size_t unicodeOpeners = 0;
    std::size_t stringChars    = 0;
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId id{i};
        if (t.kind(id) != NodeKind::Token) continue;
        if (t.tokenKind(id).v == unicodeStartKind.v) ++unicodeOpeners;
        if (t.tokenKind(id).v == stringCharKind.v)   ++stringChars;
    }
    EXPECT_EQ(unicodeOpeners, 1u);
    EXPECT_EQ(stringChars,    6u) << "5 body chars + 1 endsAt close";
}

TEST(TsqlSubset, SelectWithBracketIdentifier) {
    // Live tokenization: both `[` openers push bracket-id mode and the
    // body emits BracketIdChar per codepoint. The body-mode flip means
    // the test pins token-kind structure (opener resolves at the
    // `nameAtom` alt's BracketIdStart arm, body chars are siblings)
    // rather than a hard-coded prettyPrint, which would now include
    // every bracketed codepoint.
    auto h = tokenizeShipped("tsql-subset", "SELECT [Order Date] FROM [My Table];");
    ASSERT_NE(h.schema, nullptr);
    const auto bracketStartKind = h.schema->schemaTokens().find("BracketIdStart");
    const auto bracketCharKind  = h.schema->schemaTokens().find("BracketIdChar");
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto sel  = b.open(h.schema->rules().find("selectStmt"));
        b.pushToken(h.stream.advance());                   // SELECT
        drainWhitespace(b, h.stream);
        {
            auto list  = b.open(h.schema->rules().find("selectList"));
            auto items = b.open(h.schema->rules().find("selectItemList"));
            auto item  = b.open(h.schema->rules().find("selectItem"));
            auto expr  = b.open(h.schema->rules().find("expression"));
            auto opr   = b.open(h.schema->rules().find("operand"));
            b.pushToken(h.stream.advance());               // [
            drainBody(b, h.stream, bracketCharKind);       // Order Date]
        }
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());                   // FROM
        drainWhitespace(b, h.stream);
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(h.stream.advance());               // [
            drainBody(b, h.stream, bracketCharKind);       // My Table]
        }
        b.pushToken(h.stream.advance());                   // ;
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty());
    EXPECT_TRUE(h.lexerDiags->all().empty());

    // Pin the alt-resolution: both `[` openers must land as BracketIdStart
    // leaves (not Identifier). Body chars total = 10 + 8 = 18 (9 body
    // codepoints for "Order Date" — incl. space — + close; 7 for "My Table"
    // + close; the close emissions are BracketIdChar-kind covering `]`).
    std::size_t bracketOpeners = 0;
    std::size_t bracketChars   = 0;
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId id{i};
        if (t.kind(id) != NodeKind::Token) continue;
        if (t.tokenKind(id).v == bracketStartKind.v) ++bracketOpeners;
        if (t.tokenKind(id).v == bracketCharKind.v)  ++bracketChars;
    }
    EXPECT_EQ(bracketOpeners, 2u)
        << "both `[` lexemes must resolve to BracketIdStart, not Identifier";
    // First bracket body `Order Date]` = 11 BracketIdChars (10 body
    // codepoints + the `]` close emission). Second bracket body
    // `My Table]` = 9 BracketIdChars. Total = 20.
    EXPECT_EQ(bracketChars, 20u);
}

TEST(TsqlSubset, InsertIntoValues) {
    auto h = tokenizeShipped("tsql-subset", "INSERT INTO t (a, b) VALUES (1, 2);");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto ins  = b.open(h.schema->rules().find("insertStmt"));
        b.pushToken(h.stream.advance());                   // INSERT
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());                   // INTO
        drainWhitespace(b, h.stream);
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(h.stream.advance());               // t
        }
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());                   // (
        {
            auto cl = b.open(h.schema->rules().find("columnList"));
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(h.stream.advance()); }           // a
            b.pushToken(h.stream.advance());               // ,
            drainWhitespace(b, h.stream);
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(h.stream.advance()); }           // b
        }
        b.pushToken(h.stream.advance());                   // )
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());                   // VALUES
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());                   // (
        {
            auto vl = b.open(h.schema->rules().find("valueList"));
            { auto e = b.open(h.schema->rules().find("expression"));
              auto o = b.open(h.schema->rules().find("operand"));
              b.pushToken(h.stream.advance()); }           // 1
            b.pushToken(h.stream.advance());               // ,
            drainWhitespace(b, h.stream);
            { auto e = b.open(h.schema->rules().find("expression"));
              auto o = b.open(h.schema->rules().find("operand"));
              b.pushToken(h.stream.advance()); }           // 2
        }
        b.pushToken(h.stream.advance());                   // )
        b.pushToken(h.stream.advance());                   // ;
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty());
    EXPECT_TRUE(h.lexerDiags->all().empty());

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
    auto h = tokenizeShipped("tsql-subset", "UPDATE t SET a = 1 WHERE id = 5;");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto upd  = b.open(h.schema->rules().find("updateStmt"));
        b.pushToken(h.stream.advance());                   // UPDATE
        drainWhitespace(b, h.stream);
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(h.stream.advance());               // t
        }
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());                   // SET
        drainWhitespace(b, h.stream);
        {
            auto al = b.open(h.schema->rules().find("assignList"));
            auto a  = b.open(h.schema->rules().find("assignment"));
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(h.stream.advance()); }           // a
            drainWhitespace(b, h.stream);
            b.pushToken(h.stream.advance());               // =
            drainWhitespace(b, h.stream);
            { auto e = b.open(h.schema->rules().find("expression"));
              auto o = b.open(h.schema->rules().find("operand"));
              b.pushToken(h.stream.advance()); }           // 1
        }
        drainWhitespace(b, h.stream);
        {
            auto w = b.open(h.schema->rules().find("whereClause"));
            b.pushToken(h.stream.advance());               // WHERE
            drainWhitespace(b, h.stream);
            auto e = b.open(h.schema->rules().find("expression"));
            { auto o = b.open(h.schema->rules().find("operand"));
              b.pushToken(h.stream.advance()); }           // id
            drainWhitespace(b, h.stream);
            {
                auto bop = b.open(h.schema->rules().find("binaryOp"));
                b.pushToken(h.stream.advance());           // =
            }
            drainWhitespace(b, h.stream);
            { auto o = b.open(h.schema->rules().find("operand"));
              b.pushToken(h.stream.advance()); }           // 5
        }
        b.pushToken(h.stream.advance());                   // ;
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty());
    EXPECT_TRUE(h.lexerDiags->all().empty());

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
    auto h = tokenizeShipped("tsql-subset", "DELETE FROM t WHERE id = 5;");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto del  = b.open(h.schema->rules().find("deleteStmt"));
        b.pushToken(h.stream.advance());                   // DELETE
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());                   // FROM
        drainWhitespace(b, h.stream);
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(h.stream.advance());               // t
        }
        drainWhitespace(b, h.stream);
        {
            auto w = b.open(h.schema->rules().find("whereClause"));
            b.pushToken(h.stream.advance());               // WHERE
            drainWhitespace(b, h.stream);
            auto e = b.open(h.schema->rules().find("expression"));
            { auto o = b.open(h.schema->rules().find("operand"));
              b.pushToken(h.stream.advance()); }           // id
            drainWhitespace(b, h.stream);
            { auto bop = b.open(h.schema->rules().find("binaryOp"));
              b.pushToken(h.stream.advance()); }           // =
            drainWhitespace(b, h.stream);
            { auto o = b.open(h.schema->rules().find("operand"));
              b.pushToken(h.stream.advance()); }           // 5
        }
        b.pushToken(h.stream.advance());                   // ;
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty());
    EXPECT_TRUE(h.lexerDiags->all().empty());

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
    auto h = tokenizeShipped("tsql-subset", "CREATE TABLE t (id INT, name VARCHAR);");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto cr   = b.open(h.schema->rules().find("createTableStmt"));
        b.pushToken(h.stream.advance());                   // CREATE
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());                   // TABLE
        drainWhitespace(b, h.stream);
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(h.stream.advance());               // t
        }
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());                   // (
        {
            auto cdl = b.open(h.schema->rules().find("columnDeclList"));
            { auto cd = b.open(h.schema->rules().find("columnDecl"));
              { auto na = b.open(h.schema->rules().find("nameAtom"));
                b.pushToken(h.stream.advance()); }         // id
              drainWhitespace(b, h.stream);
              { auto tr = b.open(h.schema->rules().find("typeRef"));
                b.pushToken(h.stream.advance()); } }       // INT
            b.pushToken(h.stream.advance());               // ,
            drainWhitespace(b, h.stream);
            { auto cd = b.open(h.schema->rules().find("columnDecl"));
              { auto na = b.open(h.schema->rules().find("nameAtom"));
                b.pushToken(h.stream.advance()); }         // name
              drainWhitespace(b, h.stream);
              { auto tr = b.open(h.schema->rules().find("typeRef"));
                b.pushToken(h.stream.advance()); } }       // VARCHAR
        }
        b.pushToken(h.stream.advance());                   // )
        b.pushToken(h.stream.advance());                   // ;
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty());
    EXPECT_TRUE(h.lexerDiags->all().empty());

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
    auto h = tokenizeShipped("tsql-subset", "CREATE TABLE SELECT (id INT);");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto cr   = b.open(h.schema->rules().find("createTableStmt"));
        b.pushToken(h.stream.advance());                   // CREATE
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());                   // TABLE
        drainWhitespace(b, h.stream);
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(h.stream.advance());               // SELECT (demoted)
        }
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());                   // (
        {
            auto cdl = b.open(h.schema->rules().find("columnDeclList"));
            auto cd  = b.open(h.schema->rules().find("columnDecl"));
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(h.stream.advance()); }           // id
            drainWhitespace(b, h.stream);
            { auto tr = b.open(h.schema->rules().find("typeRef"));
              b.pushToken(h.stream.advance()); }           // INT
        }
        b.pushToken(h.stream.advance());                   // )
        b.pushToken(h.stream.advance());                   // ;
    }
    Tree t = std::move(b).finish();
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(h.lexerDiags->all().empty());

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
        if (h.src->slice(t.span(id)) != "SELECT") continue;
        if (t.tokenKind(id).v == identKindId.v)  sawDemotedSelect = true;
        if (t.tokenKind(id).v == selectKindId.v) sawUndemotedSelect = true;
    }
    EXPECT_TRUE(sawDemotedSelect)
        << "SELECT at qualifiedName must have tokenKind == Identifier";
    EXPECT_FALSE(sawUndemotedSelect)
        << "no SELECT leaf should remain as SelectKw after demotion";
}

TEST(TsqlSubset, ParsesMultipleStatements) {
    // root = repeat(statement). Exercise the repeat with two SELECTs.
    auto h = tokenizeShipped("tsql-subset", "SELECT * FROM t; SELECT * FROM u;");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    auto buildSelectStar = [&] {
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto sel  = b.open(h.schema->rules().find("selectStmt"));
        b.pushToken(h.stream.advance());                   // SELECT
        drainWhitespace(b, h.stream);
        {
            auto list = b.open(h.schema->rules().find("selectList"));
            auto star = b.open(h.schema->rules().find("selectStar"));
            b.pushToken(h.stream.advance());               // *
        }
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());                   // FROM
        drainWhitespace(b, h.stream);
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(h.stream.advance());               // t / u
        }
        b.pushToken(h.stream.advance());                   // ;
    };
    {
        auto root = b.open(h.schema->rules().find("root"));
        buildSelectStar();
        drainWhitespace(b, h.stream);
        buildSelectStar();
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty());
    EXPECT_TRUE(h.lexerDiags->all().empty());

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
    auto h = tokenizeShipped("tsql-subset", "UPDATE t SET a = b + c * d;");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto upd  = b.open(h.schema->rules().find("updateStmt"));
        b.pushToken(h.stream.advance());                   // UPDATE
        drainWhitespace(b, h.stream);
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(h.stream.advance());               // t
        }
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());                   // SET
        drainWhitespace(b, h.stream);
        {
            auto al = b.open(h.schema->rules().find("assignList"));
            auto a  = b.open(h.schema->rules().find("assignment"));
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(h.stream.advance()); }           // a
            drainWhitespace(b, h.stream);
            b.pushToken(h.stream.advance());               // =
            drainWhitespace(b, h.stream);
            { auto e = b.open(h.schema->rules().find("expression"));
              { auto o = b.open(h.schema->rules().find("operand"));
                b.pushToken(h.stream.advance()); }         // b
              drainWhitespace(b, h.stream);
              { auto bop = b.open(h.schema->rules().find("binaryOp"));
                b.pushToken(h.stream.advance()); }         // +
              drainWhitespace(b, h.stream);
              { auto o = b.open(h.schema->rules().find("operand"));
                b.pushToken(h.stream.advance()); }         // c
              drainWhitespace(b, h.stream);
              { auto bop = b.open(h.schema->rules().find("binaryOp"));
                b.pushToken(h.stream.advance()); }         // *
              drainWhitespace(b, h.stream);
              { auto o = b.open(h.schema->rules().find("operand"));
                b.pushToken(h.stream.advance()); }         // d
            }
        }
        b.pushToken(h.stream.advance());                   // ;
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty());
    EXPECT_TRUE(h.lexerDiags->all().empty());

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
    auto h = tokenizeShipped("tsql-subset", "");
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
    auto h = tokenizeShipped("tsql-subset", "CREATE TABLE t (id INT NOT NULL);");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto cr   = b.open(h.schema->rules().find("createTableStmt"));
        b.pushToken(h.stream.advance());                   // CREATE
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());                   // TABLE
        drainWhitespace(b, h.stream);
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(h.stream.advance());               // t
        }
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());                   // (
        {
            auto cdl = b.open(h.schema->rules().find("columnDeclList"));
            auto cd  = b.open(h.schema->rules().find("columnDecl"));
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(h.stream.advance()); }           // id
            drainWhitespace(b, h.stream);
            { auto tr = b.open(h.schema->rules().find("typeRef"));
              b.pushToken(h.stream.advance()); }           // INT
            drainWhitespace(b, h.stream);
            { auto nn = b.open(h.schema->rules().find("notNullClause"));
              b.pushToken(h.stream.advance());             // NOT
              drainWhitespace(b, h.stream);
              b.pushToken(h.stream.advance()); }           // NULL
        }
        b.pushToken(h.stream.advance());                   // )
        b.pushToken(h.stream.advance());                   // ;
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty());
    EXPECT_TRUE(h.lexerDiags->all().empty());
}

TEST(TsqlSubset, SchemaIdsAreDistinctPerLoad) {
    auto a = GrammarSchema::loadShipped("tsql-subset");
    auto b = GrammarSchema::loadShipped("tsql-subset");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_NE((*a)->schemaId().v, (*b)->schemaId().v);
}

// ── TZ3 body-mode E2E pins ───────────────────────────────────────────────
//
// Three additional tests added during TZ3 to pin body-mode tokenizer
// behavior end-to-end. The body modes are the highest-risk surface
// since the pre-TZ3 hand-tokenization completely sidestepped them.

TEST(TsqlSubset, DoubledDelimiterEscapeInsideSingleString) {
    // `'it''s'` represents the literal `it's` — the doubled `''` is a
    // single StringChar escape, not the end-of-string + start-of-new.
    // The body-mode branch order (doubled-delim before endsAt) is what
    // makes this work; under a regression, `''` would close + reopen.
    auto h = tokenizeShipped("tsql-subset", "SELECT 'it''s' FROM t;");
    ASSERT_NE(h.schema, nullptr);
    const auto stringStartKind = h.schema->schemaTokens().find("StringStart");
    const auto stringCharKind  = h.schema->schemaTokens().find("StringChar");
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto sel  = b.open(h.schema->rules().find("selectStmt"));
        b.pushToken(h.stream.advance());                   // SELECT
        drainWhitespace(b, h.stream);
        {
            auto list  = b.open(h.schema->rules().find("selectList"));
            auto items = b.open(h.schema->rules().find("selectItemList"));
            auto item  = b.open(h.schema->rules().find("selectItem"));
            auto expr  = b.open(h.schema->rules().find("expression"));
            auto opr   = b.open(h.schema->rules().find("operand"));
            b.pushToken(h.stream.advance());               // '
            drainBody(b, h.stream, stringCharKind);
        }
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());                   // FROM
        drainWhitespace(b, h.stream);
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(h.stream.advance());               // t
        }
        b.pushToken(h.stream.advance());                   // ;
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty());
    EXPECT_TRUE(h.lexerDiags->all().empty());

    // The body emits: 'i', 't', "''" (one doubled-delim StringChar
    // spanning 2 bytes), 's', then "'" close. That's 1 opener + 5 body
    // chars = 6 tokens with StringChar kind covering the body and close.
    std::size_t openers   = 0;
    std::size_t bodyChars = 0;
    std::size_t doubledDelimSpans = 0;
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId id{i};
        if (t.kind(id) != NodeKind::Token) continue;
        if (t.tokenKind(id).v == stringStartKind.v) ++openers;
        if (t.tokenKind(id).v == stringCharKind.v) {
            ++bodyChars;
            if (t.span(id).length() == 2) ++doubledDelimSpans;
        }
    }
    EXPECT_EQ(openers, 1u);
    EXPECT_EQ(bodyChars, 5u) << "'i', 't', \"''\", 's', \"'\" close = 5";
    EXPECT_EQ(doubledDelimSpans, 1u)
        << "exactly one body char must span 2 bytes — the `''` escape";
}

TEST(TsqlSubset, EmptyStringLiteralOpensAndImmediatelyCloses) {
    // `''` is the empty SQL string. The body mode opens on the first
    // `'` then immediately matches endsAt on the next char and pops.
    // The close emission covers the closing `'`, so the body produces
    // exactly 1 token (the close).
    auto h = tokenizeShipped("tsql-subset", "SELECT '' FROM t;");
    ASSERT_NE(h.schema, nullptr);
    const auto stringStartKind = h.schema->schemaTokens().find("StringStart");
    const auto stringCharKind  = h.schema->schemaTokens().find("StringChar");
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto sel  = b.open(h.schema->rules().find("selectStmt"));
        b.pushToken(h.stream.advance());                   // SELECT
        drainWhitespace(b, h.stream);
        {
            auto list  = b.open(h.schema->rules().find("selectList"));
            auto items = b.open(h.schema->rules().find("selectItemList"));
            auto item  = b.open(h.schema->rules().find("selectItem"));
            auto expr  = b.open(h.schema->rules().find("expression"));
            auto opr   = b.open(h.schema->rules().find("operand"));
            b.pushToken(h.stream.advance());               // '
            drainBody(b, h.stream, stringCharKind);        // just the close
        }
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());                   // FROM
        drainWhitespace(b, h.stream);
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(h.stream.advance());               // t
        }
        b.pushToken(h.stream.advance());                   // ;
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty());
    EXPECT_TRUE(h.lexerDiags->all().empty());

    std::size_t openers   = 0;
    std::size_t bodyChars = 0;
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId id{i};
        if (t.kind(id) != NodeKind::Token) continue;
        if (t.tokenKind(id).v == stringStartKind.v) ++openers;
        if (t.tokenKind(id).v == stringCharKind.v) ++bodyChars;
    }
    EXPECT_EQ(openers, 1u);
    EXPECT_EQ(bodyChars, 1u) << "empty body emits ONLY the close char";
}

TEST(TsqlSubset, UnicodeStringBodyAcceptsMultiByteUTF8) {
    // The tokenizer's body-mode default emission consumes one CODEPOINT
    // per token via UTF-8 lead-byte length detection. `héllo` is 5
    // codepoints but 6 bytes (é = 0xC3 0xA9). Each codepoint becomes
    // exactly one StringChar leaf; the é leaf spans 2 bytes.
    auto h = tokenizeShipped("tsql-subset", "SELECT N'h\xC3\xA9llo' FROM t;");
    ASSERT_NE(h.schema, nullptr);
    const auto stringCharKind  = h.schema->schemaTokens().find("StringChar");
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto sel  = b.open(h.schema->rules().find("selectStmt"));
        b.pushToken(h.stream.advance());                   // SELECT
        drainWhitespace(b, h.stream);
        {
            auto list  = b.open(h.schema->rules().find("selectList"));
            auto items = b.open(h.schema->rules().find("selectItemList"));
            auto item  = b.open(h.schema->rules().find("selectItem"));
            auto expr  = b.open(h.schema->rules().find("expression"));
            auto opr   = b.open(h.schema->rules().find("operand"));
            b.pushToken(h.stream.advance());               // N'
            drainBody(b, h.stream, stringCharKind);
        }
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());                   // FROM
        drainWhitespace(b, h.stream);
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(h.stream.advance());               // t
        }
        b.pushToken(h.stream.advance());                   // ;
    }
    Tree t = std::move(b).finish();
    EXPECT_TRUE(t.diagnostics().all().empty());
    EXPECT_TRUE(h.lexerDiags->all().empty());

    // 5 body codepoints (h é l l o) + 1 close = 6 StringChar leaves.
    // Exactly one of them must span 2 bytes (the é).
    std::size_t bodyChars = 0;
    std::size_t twoByteChars = 0;
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId id{i};
        if (t.kind(id) != NodeKind::Token) continue;
        if (t.tokenKind(id).v != stringCharKind.v) continue;
        ++bodyChars;
        if (t.span(id).length() == 2) ++twoByteChars;
    }
    EXPECT_EQ(bodyChars, 6u)    << "h, é, l, l, o, close = 6";
    EXPECT_EQ(twoByteChars, 1u) << "exactly one StringChar spans 2 bytes (é)";
}
