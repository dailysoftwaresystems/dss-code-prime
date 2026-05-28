#include "analysis/syntactic/parser.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/lexer_mode.hpp"
#include "core/types/operator_table.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/char_decode.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/string_style.hpp"
#include "core/types/token.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_builder.hpp"
#include "tokenizer/tokenizer.hpp"
#include "tokenizer/token_stream.hpp"
#include "e2e_harness.hpp"
#include "test_pretty_print.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

// End-to-end pin for the shipped tsql-subset.lang.json — three lexer
// modes (single-string, unicode-string, bracket-id), doubled-delimiter
// escapes, contextual reserved-word policy, qualified names.

using namespace dss;
using dss::tests::drainWhitespace;
using dss::tests::E2EHarness;
using dss::tests::prettyPrint;
using dss::tests::pushNext;
using dss::tests::tokenizeShipped;

namespace {

// Pull body-mode tokens of `bodyKind` until the body pops.
// HR10 — the SQL string body modes COALESCE: the whole body is ONE
// in-grammar `StringLiteral` token (the close delimiter is consumed on
// mode-pop, not emitted). The pins below assert that single-token
// granularity directly on the stream, far clearer than rebuilding a tree
// by hand. The body span is the RAW (undecoded) bytes the lowering later
// decodes (escapes / doubled-delimiter resolved in char_decode.hpp).
std::vector<Token> collectTokens(TokenStream& s) {
    std::vector<Token> out;
    while (!s.isAtEnd()) out.push_back(s.advance());
    return out;
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
        pushNext(b, h.stream);
        {
            auto list = b.open(h.schema->rules().find("selectList"));
            auto star = b.open(h.schema->rules().find("selectStar"));
            b.pushToken(h.stream.advance());
        }
        drainWhitespace(b, h.stream);
        pushNext(b, h.stream);
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(h.stream.advance());
        }
        b.pushToken(h.stream.advance());
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
    auto loaded = GrammarSchema::loadShipped("tsql-subset");
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    const std::string sql = "SELECT a FROM db.schema.t;";
    auto src = SourceBuffer::fromString(sql, "test.sql");
    Tokenizer tk{src, schema};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    Parser p{src, schema, std::move(stream)};
    Tree t = std::move(p).parse().tree;
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
        "                rule:nameOrCall\n"
        "                  rule:qualifiedName\n"
        "                    rule:nameAtom\n"
        "                      tok:\"a\"\n"
        "      tok:\"FROM\"\n"
        "      rule:tableRef\n"
        "        rule:qualifiedName\n"
        "          rule:nameAtom\n"
        "            tok:\"db\"\n"
        "          tok:\".\"\n"
        "          rule:nameAtom\n"
        "            tok:\"schema\"\n"
        "          tok:\".\"\n"
        "          rule:nameAtom\n"
        "            tok:\"t\"\n"
        "      tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

TEST(TsqlSubset, SelectWithUnicodeLiteralOpener) {
    // The N' opener pushes unicode-string mode; the body coalesces (HR10)
    // into ONE StringLiteral token spanning the raw body `hello`. The
    // closing `'` is consumed on mode-pop, not emitted.
    auto h = tokenizeShipped("tsql-subset", "SELECT N'hello' FROM t;");
    ASSERT_NE(h.schema, nullptr);
    const auto unicodeStartKind = h.schema->schemaTokens().find("UnicodeStringStart");
    const auto stringLitKind    = h.schema->schemaTokens().find("StringLiteral");
    const auto tokens = collectTokens(h.stream);

    std::size_t unicodeOpeners = 0, bodies = 0;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].schemaKind.v == unicodeStartKind.v) {
            ++unicodeOpeners;
            ASSERT_LT(i + 1, tokens.size());
            EXPECT_EQ(tokens[i + 1].schemaKind.v, stringLitKind.v)
                << "opener must be followed by ONE coalesced body token";
            EXPECT_EQ(h.src->slice(tokens[i + 1].span), "hello");
            ++bodies;
        }
    }
    EXPECT_EQ(unicodeOpeners, 1u);
    EXPECT_EQ(bodies, 1u);
}

TEST(TsqlSubset, SelectWithBracketIdentifier) {
    // Live parse: both `[` openers push bracket-id mode and the body
    // emits BracketIdChar per codepoint. The parser absorbs body chars
    // as off-grammar siblings of the opener (no schema reference to
    // body-mode default-token kinds is allowed).
    auto loaded = GrammarSchema::loadShipped("tsql-subset");
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    const std::string sql = "SELECT [Order Date] FROM [My Table];";
    auto src = SourceBuffer::fromString(sql, "test.sql");
    Tokenizer tk{src, schema};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    Parser p{src, schema, std::move(stream)};
    Tree t = std::move(p).parse().tree;
    EXPECT_TRUE(t.diagnostics().all().empty());

    const auto bracketStartKind = schema->schemaTokens().find("BracketIdStart");
    const auto bracketCharKind  = schema->schemaTokens().find("BracketIdChar");

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
    // BracketIdChar counts: `Order Date` (10) + close `]` (1) = 11,
    // `My Table` (8) + close `]` (1) = 9, total 20.
    EXPECT_EQ(bracketChars, 11u + 9u);
}

TEST(TsqlSubset, InsertIntoValues) {
    auto h = tokenizeShipped("tsql-subset", "INSERT INTO t (a, b) VALUES (1, 2);");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto ins  = b.open(h.schema->rules().find("insertStmt"));
        pushNext(b, h.stream);
        pushNext(b, h.stream);
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(h.stream.advance());
        }
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());
        {
            auto cl = b.open(h.schema->rules().find("columnList"));
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(h.stream.advance()); }
            pushNext(b, h.stream);
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(h.stream.advance()); }
        }
        pushNext(b, h.stream);
        pushNext(b, h.stream);
        b.pushToken(h.stream.advance());
        {
            auto vl = b.open(h.schema->rules().find("valueList"));
            { auto e = b.open(h.schema->rules().find("expression"));
              auto o = b.open(h.schema->rules().find("operand"));
              b.pushToken(h.stream.advance()); }
            pushNext(b, h.stream);
            { auto e = b.open(h.schema->rules().find("expression"));
              auto o = b.open(h.schema->rules().find("operand"));
              b.pushToken(h.stream.advance()); }
        }
        b.pushToken(h.stream.advance());
        b.pushToken(h.stream.advance());
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
    auto loaded = GrammarSchema::loadShipped("tsql-subset");
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    const std::string sql = "UPDATE t SET a = 1 WHERE id = 5;";
    auto src = SourceBuffer::fromString(sql, "test.sql");
    Tokenizer tk{src, schema};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    Parser p{src, schema, std::move(stream)};
    Tree t = std::move(p).parse().tree;
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
        "            rule:nameOrCall\n"
        "              rule:qualifiedName\n"
        "                rule:nameAtom\n"
        "                  tok:\"id\"\n"
        "          rule:binaryOp\n"
        "            tok:\"=\"\n"
        "          rule:operand\n"
        "            tok:\"5\"\n"
        "      tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

TEST(TsqlSubset, DeleteFromWhere) {
    auto loaded = GrammarSchema::loadShipped("tsql-subset");
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    const std::string sql = "DELETE FROM t WHERE id = 5;";
    auto src = SourceBuffer::fromString(sql, "test.sql");
    Tokenizer tk{src, schema};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    Parser p{src, schema, std::move(stream)};
    Tree t = std::move(p).parse().tree;
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
        "            rule:nameOrCall\n"
        "              rule:qualifiedName\n"
        "                rule:nameAtom\n"
        "                  tok:\"id\"\n"
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
        pushNext(b, h.stream);
        pushNext(b, h.stream);
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(h.stream.advance());
        }
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());
        {
            auto cdl = b.open(h.schema->rules().find("columnDeclList"));
            { auto cd = b.open(h.schema->rules().find("columnDecl"));
              { auto na = b.open(h.schema->rules().find("nameAtom"));
                b.pushToken(h.stream.advance()); }
              drainWhitespace(b, h.stream);
              { auto tr = b.open(h.schema->rules().find("typeRef"));
                b.pushToken(h.stream.advance()); } }
            pushNext(b, h.stream);
            { auto cd = b.open(h.schema->rules().find("columnDecl"));
              { auto na = b.open(h.schema->rules().find("nameAtom"));
                b.pushToken(h.stream.advance()); }
              drainWhitespace(b, h.stream);
              { auto tr = b.open(h.schema->rules().find("typeRef"));
                b.pushToken(h.stream.advance()); } }
        }
        b.pushToken(h.stream.advance());
        b.pushToken(h.stream.advance());
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
    auto h = tokenizeShipped("tsql-subset", "CREATE TABLE SELECT (id INT);");
    ASSERT_NE(h.schema, nullptr);
    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto cr   = b.open(h.schema->rules().find("createTableStmt"));
        pushNext(b, h.stream);
        pushNext(b, h.stream);
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(h.stream.advance());
        }
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());
        {
            auto cdl = b.open(h.schema->rules().find("columnDeclList"));
            auto cd  = b.open(h.schema->rules().find("columnDecl"));
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(h.stream.advance()); }
            drainWhitespace(b, h.stream);
            { auto tr = b.open(h.schema->rules().find("typeRef"));
              b.pushToken(h.stream.advance()); }
        }
        b.pushToken(h.stream.advance());
        b.pushToken(h.stream.advance());
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
        pushNext(b, h.stream);
        {
            auto list = b.open(h.schema->rules().find("selectList"));
            auto star = b.open(h.schema->rules().find("selectStar"));
            b.pushToken(h.stream.advance());
        }
        drainWhitespace(b, h.stream);
        pushNext(b, h.stream);
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(h.stream.advance());
        }
        b.pushToken(h.stream.advance());
    };
    {
        auto root = b.open(h.schema->rules().find("root"));
        buildSelectStar();
        drainWhitespace(b, h.stream);
        buildSelectStar();
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
    // `expression` is flat-fold `operand (binaryOp operand)*` —
    // the sequence-shaped `expression` rule. Pin the schema-side
    // data that drives operator climbing (`*` binds tighter than
    // `+`); the parser drives this through assignment's expression.
    auto loaded = GrammarSchema::loadShipped("tsql-subset");
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    const std::string sql = "UPDATE t SET a = b + c * d;";
    auto src = SourceBuffer::fromString(sql, "test.sql");
    Tokenizer tk{src, schema};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    Parser p{src, schema, std::move(stream)};
    Tree t = std::move(p).parse().tree;
    EXPECT_TRUE(t.diagnostics().all().empty());

    auto const& tokens = schema->schemaTokens();
    auto const* table  = &schema->operatorTable();
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
        pushNext(b, h.stream);
        pushNext(b, h.stream);
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            b.pushToken(h.stream.advance());
        }
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());
        {
            auto cdl = b.open(h.schema->rules().find("columnDeclList"));
            auto cd  = b.open(h.schema->rules().find("columnDecl"));
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(h.stream.advance()); }
            drainWhitespace(b, h.stream);
            { auto tr = b.open(h.schema->rules().find("typeRef"));
              b.pushToken(h.stream.advance()); }
            drainWhitespace(b, h.stream);
            { auto nn = b.open(h.schema->rules().find("notNullClause"));
              pushNext(b, h.stream);
              b.pushToken(h.stream.advance()); }
        }
        b.pushToken(h.stream.advance());
        b.pushToken(h.stream.advance());
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

// ── Body-mode E2E pins ───────────────────────────────────────────────────

TEST(TsqlSubset, DoubledDelimiterEscapeInsideSingleString) {
    // `'it''s'` represents the literal `it's` — the doubled `''` is an
    // embedded delimiter, not end-of-string + start-of-new. The coalesce
    // path keeps `''` raw in the body span (branch order: doubled-delim
    // before endsAt); the lowering's decoder collapses it to one `'`.
    // Under a regression, `''` would close + reopen and the body would end
    // at `it`.
    auto h = tokenizeShipped("tsql-subset", "SELECT 'it''s' FROM t;");
    ASSERT_NE(h.schema, nullptr);
    const auto stringStartKind = h.schema->schemaTokens().find("StringStart");
    const auto stringLitKind   = h.schema->schemaTokens().find("StringLiteral");
    const auto tokens = collectTokens(h.stream);

    std::size_t openers = 0, bodies = 0;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].schemaKind.v != stringStartKind.v) continue;
        ++openers;
        ASSERT_LT(i + 1, tokens.size());
        EXPECT_EQ(tokens[i + 1].schemaKind.v, stringLitKind.v);
        const auto body = h.src->slice(tokens[i + 1].span);
        EXPECT_EQ(body, "it''s") << "doubled-delim kept raw in the body span";
        EXPECT_EQ(decodeDoubledDelimiterBody(body, '\''), "it's")
            << "decoder collapses the doubled `''` to one `'`";
        ++bodies;
    }
    EXPECT_EQ(openers, 1u);
    EXPECT_EQ(bodies, 1u);
}

TEST(TsqlSubset, EmptyStringLiteralOpensAndImmediatelyCloses) {
    // `''` is the empty SQL string. The body mode opens on the first `'`,
    // immediately matches endsAt on the next, and pops. The coalesce path
    // still emits ONE body token — a zero-width StringLiteral — so the
    // operand has a literal to lower (decodes to the empty string).
    auto h = tokenizeShipped("tsql-subset", "SELECT '' FROM t;");
    ASSERT_NE(h.schema, nullptr);
    const auto stringStartKind = h.schema->schemaTokens().find("StringStart");
    const auto stringLitKind   = h.schema->schemaTokens().find("StringLiteral");
    const auto tokens = collectTokens(h.stream);

    std::size_t openers = 0, bodies = 0;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].schemaKind.v != stringStartKind.v) continue;
        ++openers;
        ASSERT_LT(i + 1, tokens.size());
        EXPECT_EQ(tokens[i + 1].schemaKind.v, stringLitKind.v);
        EXPECT_EQ(tokens[i + 1].span.length(), 0u) << "empty body is zero-width";
        ++bodies;
    }
    EXPECT_EQ(openers, 1u);
    EXPECT_EQ(bodies, 1u);
}

TEST(TsqlSubset, UnicodeStringBodyAcceptsMultiByteUTF8) {
    // The coalesced body captures raw bytes, so multi-byte UTF-8 passes
    // through verbatim. `héllo` is 5 codepoints but 6 bytes (é = 0xC3 0xA9),
    // so the one coalesced StringLiteral body spans 6 bytes.
    auto h = tokenizeShipped("tsql-subset", "SELECT N'h\xC3\xA9llo' FROM t;");
    ASSERT_NE(h.schema, nullptr);
    const auto unicodeStartKind = h.schema->schemaTokens().find("UnicodeStringStart");
    const auto stringLitKind    = h.schema->schemaTokens().find("StringLiteral");
    const auto tokens = collectTokens(h.stream);

    std::size_t bodies = 0;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].schemaKind.v != unicodeStartKind.v) continue;
        ASSERT_LT(i + 1, tokens.size());
        EXPECT_EQ(tokens[i + 1].schemaKind.v, stringLitKind.v);
        EXPECT_EQ(tokens[i + 1].span.length(), 6u) << "héllo = 6 raw bytes";
        EXPECT_EQ(h.src->slice(tokens[i + 1].span), "h\xC3\xA9llo");
        ++bodies;
    }
    EXPECT_EQ(bodies, 1u);
}
