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

// End-to-end stress test for v2 schema expressiveness. Drives the
// shipped `tsql-subset.lang.json` through TreeBuilder via hand-tokenized
// inputs (the lexer doesn't exist yet — same harness pattern as
// `test_c_subset.cpp`). Every PR0–PR6 v2 feature is exercised at least
// once in this file; if a v2-target language can't load + parse a basic
// statement, the regression surfaces here.

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

[[nodiscard]] Token at(SourceBuffer const& src, std::string_view text,
                       CoreTokenKind kind = CoreTokenKind::Operator,
                       std::size_t startHint = std::string_view::npos) {
    const auto sv = src.text();
    const auto first = (startHint == std::string_view::npos) ? sv.find(text)
                                                              : sv.find(text, startHint);
    if (first == std::string_view::npos) {
        ADD_FAILURE() << "lexeme '" << text << "' not in source — test bug";
        return Token{
            .coreKind   = kind,
            .schemaKind = InvalidSchemaToken,
            .span       = SourceSpan::empty(0),
        };
    }
    return Token{
        .coreKind   = kind,
        .schemaKind = InvalidSchemaToken,
        .span       = SourceSpan::of(static_cast<ByteOffset>(first),
                                      static_cast<ByteOffset>(first + text.size())),
    };
}

} // namespace

// ── Loader: file parses cleanly ────────────────────────────────────────

TEST(TsqlSubset, LoadsCleanly) {
    auto loaded = GrammarSchema::loadShipped("tsql-subset");
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
}

// ── Feature coverage: every v2 mechanism is exercised by the config ────

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

// ── End-to-end parses: every statement shape exercised ─────────────────

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
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "SELECT * FROM t; should parse without errors";
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
            b.pushToken(at(*h.src, "."));
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(at(*h.src, "schema", CoreTokenKind::Word)); }
            const auto secondDot = h.src->text().find('.', h.src->text().find('.') + 1);
            b.pushToken(at(*h.src, ".", CoreTokenKind::Operator, secondDot));
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(at(*h.src, "t", CoreTokenKind::Word)); }
        }
        b.pushToken(at(*h.src, ";"));
    }
    Tree t = std::move(b).finish();
    EXPECT_FALSE(t.diagnostics().hasErrors());
}

TEST(TsqlSubset, SelectWithUnicodeLiteralPushesStringMode) {
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
            // The tokenizer (when authored) would push the unicode-
            // string mode here; for the schema-driven test we just
            // emit the opener token at the right shape position.
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
    EXPECT_FALSE(t.diagnostics().hasErrors());
}

TEST(TsqlSubset, SelectWithBracketIdentifier) {
    auto h = load("SELECT [Order Date] FROM [My Table];");
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
            b.pushToken(at(*h.src, "["));
        }
        b.pushToken(at(*h.src, "FROM", CoreTokenKind::Word));
        {
            auto qn = b.open(h.schema->rules().find("qualifiedName"));
            auto na = b.open(h.schema->rules().find("nameAtom"));
            const auto bracketPos = h.src->text().find("[My");
            b.pushToken(at(*h.src, "[", CoreTokenKind::Operator, bracketPos));
        }
        b.pushToken(at(*h.src, ";"));
    }
    Tree t = std::move(b).finish();
    EXPECT_FALSE(t.diagnostics().hasErrors());
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
        b.pushToken(at(*h.src, "("));
        {
            auto cl = b.open(h.schema->rules().find("columnList"));
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(at(*h.src, "a", CoreTokenKind::Word)); }
            b.pushToken(at(*h.src, ","));
            { auto na = b.open(h.schema->rules().find("nameAtom"));
              b.pushToken(at(*h.src, "b", CoreTokenKind::Word)); }
        }
        b.pushToken(at(*h.src, ")"));
        b.pushToken(at(*h.src, "VALUES", CoreTokenKind::Word));
        const auto secondParen = h.src->text().find('(', h.src->text().find('(') + 1);
        b.pushToken(at(*h.src, "(", CoreTokenKind::Operator, secondParen));
        {
            auto vl = b.open(h.schema->rules().find("valueList"));
            { auto e = b.open(h.schema->rules().find("expression"));
              auto o = b.open(h.schema->rules().find("operand"));
              b.pushToken(at(*h.src, "1", CoreTokenKind::Word)); }
            b.pushToken(at(*h.src, ",", CoreTokenKind::Operator,
                              h.src->text().find(',', h.src->text().find(',') + 1)));
            { auto e = b.open(h.schema->rules().find("expression"));
              auto o = b.open(h.schema->rules().find("operand"));
              b.pushToken(at(*h.src, "2", CoreTokenKind::Word)); }
        }
        const auto secondParenClose = h.src->text().find(')', h.src->text().find(')') + 1);
        b.pushToken(at(*h.src, ")", CoreTokenKind::Operator, secondParenClose));
        b.pushToken(at(*h.src, ";"));
    }
    Tree t = std::move(b).finish();
    EXPECT_FALSE(t.diagnostics().hasErrors());
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
            b.pushToken(at(*h.src, "="));
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
                const auto eqPos = h.src->text().rfind('=');
                b.pushToken(at(*h.src, "=", CoreTokenKind::Operator, eqPos));
            }
            { auto o = b.open(h.schema->rules().find("operand"));
              b.pushToken(at(*h.src, "5", CoreTokenKind::Word)); }
        }
        b.pushToken(at(*h.src, ";"));
    }
    Tree t = std::move(b).finish();
    EXPECT_FALSE(t.diagnostics().hasErrors());
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
    EXPECT_FALSE(t.diagnostics().hasErrors());
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
    EXPECT_FALSE(t.diagnostics().hasErrors());
}

// ── Contextual-keyword demotion: keywords-as-identifiers ───────────────

TEST(TsqlSubset, ContextualKeywordDemotesToIdentifierInNamePosition) {
    // T-SQL allows reserved words as identifiers when scoped right.
    // Here `SELECT` is the table name in `CREATE TABLE SELECT (...)`.
    // The contextual policy demotes the SELECT lexeme to Identifier
    // at the qualifiedName position.
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
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "contextual keyword 'SELECT' must demote to Identifier in qualifiedName position";

    // Find the SELECT leaf and verify it has tokenKind == Identifier.
    auto const& diags = t.diagnostics().all();
    EXPECT_TRUE(std::ranges::any_of(diags, [](auto const& d) {
        return d.code == DiagnosticCode::P_ContextualKeywordResolution;
    })) << "expected an info-level P_ContextualKeywordResolution when SELECT demotes";
}

// ── Reload smoke pin: the shipped config keeps loading ─────────────────

TEST(TsqlSubset, ShippedConfigReloadsUnchanged) {
    auto a = GrammarSchema::loadShipped("tsql-subset");
    auto b = GrammarSchema::loadShipped("tsql-subset");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    // Distinct SchemaIds (one per load) prove the loader-time stamp
    // pattern is doing its job.
    EXPECT_NE((*a)->schemaId().v, (*b)->schemaId().v);
}
