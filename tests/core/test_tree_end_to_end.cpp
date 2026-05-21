#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/rule_id.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/token.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_builder.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/tree_node.hpp"
#include "core/types/tree_views.hpp"
#include "core/types/tree_visitor.hpp"
#include "core/types/well_known_names.hpp"
#include "e2e_harness.hpp"
#include "test_pretty_print.hpp"
#include "tokenizer/tokenizer.hpp"
#include "tokenizer/token_stream.hpp"
#include "toy_harness.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// End-to-end integration test for the core tree stack: shipped JSON config
// → SourceBuffer → GrammarSchema → Tokenizer → TokenStream → TreeBuilder
// → Tree → walk via visitor → pretty-printed AST → DiagnosticReporter →
// typed views. Happy-path tests assert the full pretty-printed AST
// against a string literal; broken-path tests assert specific
// diagnostic codes and that the recovered tree still walks.

using namespace dss;
using dss::tests::E2EHarness;
using dss::tests::prettyPrint;
using dss::tests::ToyHarness;

namespace {

[[nodiscard]] E2EHarness tokenizeShipped(std::string sourceText) {
    return dss::tests::tokenizeShipped("toy", std::move(sourceText));
}

std::size_t countCode(std::span<ParseDiagnostic const> diags, DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : diags) if (d.code == code) ++n;
    return n;
}

// Count descendants of `start` (excluding `start` itself) whose flags have
// HasError set. Used by broken-path tests to verify the parser actually
// inserted an Error leaf rather than just recording a diagnostic.
std::size_t countErrorDescendants(Tree const& t, NodeId start) {
    std::size_t n = 0;
    bool firstSeen = false;
    walkPreOrder(TreeCursor{t, start, CursorMode::Cst}, [&](TreeCursor const& c) {
        if (!firstSeen) { firstSeen = true; return; }   // skip start itself
        if (hasError(t.flags(c.current()))) ++n;
    });
    return n;
}

// Drive a "var <name> = <value>;" statement by consuming the next 8
// tokens from `ts` in order: var, ' ', name, ' ', =, ' ', value, ;.
// The token stream supplies the actual lexemes — the helper is
// parameterless because the source bytes drive what each advance()
// yields.
void driveVarDecl(TreeBuilder& b, TokenStream& ts, GrammarSchema const& schema) {
    auto stmt = b.open(schema.rules().find("statement"));
    auto vd   = b.open(schema.rules().find("varDecl"));
    b.pushToken(ts.advance());
    b.pushToken(ts.advance());
    b.pushToken(ts.advance());
    b.pushToken(ts.advance());
    b.pushToken(ts.advance());
    b.pushToken(ts.advance());
    {
        auto expr = b.open(schema.rules().find("expression"));
        b.pushToken(ts.advance());
    }
    b.pushToken(ts.advance());
}

void driveExprStmt(TreeBuilder& b, TokenStream& ts, GrammarSchema const& schema) {
    auto stmt = b.open(schema.rules().find("statement"));
    auto es   = b.open(schema.rules().find("exprStmt"));
    {
        auto expr = b.open(schema.rules().find("expression"));
        b.pushToken(ts.advance());
    }
    b.pushToken(ts.advance());
}

} // namespace

TEST(TreeEndToEnd, HappyPath_SingleVarDecl_PrintsExpectedTree) {
    auto h = tokenizeShipped("var x = y;");
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        driveVarDecl(b, h.stream, *h.schema);
    }
    Tree t = std::move(b).finish();

    EXPECT_TRUE(t.diagnostics().all().empty());
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_FALSE(hasError(t.flags(t.root())));

    const std::string_view expected =
        "rule:root\n"
        "  rule:statement\n"
        "    rule:varDecl\n"
        "      tok:\"var\"\n"
        "      tok:\"x\"\n"
        "      tok:\"=\"\n"
        "      rule:expression\n"
        "        tok:\"y\"\n"
        "      tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

TEST(TreeEndToEnd, HappyPath_SingleExprStmt_PrintsExpectedTree) {
    auto h = tokenizeShipped("y;");
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        driveExprStmt(b, h.stream, *h.schema);
    }
    Tree t = std::move(b).finish();

    EXPECT_TRUE(t.diagnostics().all().empty());
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const std::string_view expected =
        "rule:root\n"
        "  rule:statement\n"
        "    rule:exprStmt\n"
        "      rule:expression\n"
        "        tok:\"y\"\n"
        "      tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

TEST(TreeEndToEnd, HappyPath_MultipleStatements_PrintsExpectedTree) {
    auto h = tokenizeShipped("var x = a; y; var w = b;");
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        driveVarDecl(b, h.stream, *h.schema);
        b.pushToken(h.stream.advance());
        driveExprStmt(b, h.stream, *h.schema);
        b.pushToken(h.stream.advance());
        driveVarDecl(b, h.stream, *h.schema);
    }
    Tree t = std::move(b).finish();

    EXPECT_TRUE(t.diagnostics().all().empty());

    // Whitespace between statements attaches as a child of root, not of the
    // preceding statement. Verify directly — pretty-print skips EmptySpace
    // so the string assertion alone wouldn't catch a re-attachment regression.
    std::size_t rootWsCount = 0;
    for (NodeId c : t.children(t.root())) {
        if (isEmptySpace(t.flags(c))) ++rootWsCount;
    }
    EXPECT_EQ(rootWsCount, 2u);

    const std::string_view expected =
        "rule:root\n"
        "  rule:statement\n"
        "    rule:varDecl\n"
        "      tok:\"var\"\n"
        "      tok:\"x\"\n"
        "      tok:\"=\"\n"
        "      rule:expression\n"
        "        tok:\"a\"\n"
        "      tok:\";\"\n"
        "  rule:statement\n"
        "    rule:exprStmt\n"
        "      rule:expression\n"
        "        tok:\"y\"\n"
        "      tok:\";\"\n"
        "  rule:statement\n"
        "    rule:varDecl\n"
        "      tok:\"var\"\n"
        "      tok:\"w\"\n"
        "      tok:\"=\"\n"
        "      rule:expression\n"
        "        tok:\"b\"\n"
        "      tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

TEST(TreeEndToEnd, HappyPath_ViewsResolveOnRealParse) {
    auto h = tokenizeShipped("var x = a; y; var w = b;");
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        driveVarDecl(b, h.stream, *h.schema);
        b.pushToken(h.stream.advance());
        driveExprStmt(b, h.stream, *h.schema);
        b.pushToken(h.stream.advance());
        driveVarDecl(b, h.stream, *h.schema);
    }
    Tree t = std::move(b).finish();
    ASSERT_FALSE(t.diagnostics().hasErrors());

    std::vector<std::string> seen;
    walkPreOrder(t.astCursor(), [&](TreeCursor const& c) {
        const auto id = c.current();
        if (t.kind(id) != NodeKind::Internal) return WalkAction::Continue;
        if (t.rules().name(t.rule(id)) != "statement") return WalkAction::Continue;

        // Statement's first VISIBLE child is the actual decl / expr-stmt
        // node. nthVisibleChild is robust to any future leading-EmptySpace
        // refactor; raw children().front() would silently break.
        const auto inner = detail::views::nthVisibleChild(t, id, 0);
        if (!inner.valid()) {
            ADD_FAILURE() << "statement has no visible child";
            return WalkAction::Stop;
        }

        if (auto vd = VarDeclView::from(t, inner)) {
            seen.push_back("varDecl:" + std::string{vd->name().name()});
        } else if (auto es = ExprStmtView::from(t, inner)) {
            const auto expr = es->expression();
            const auto first = detail::views::nthVisibleChild(t, expr, 0);
            if (!first.valid()) {
                ADD_FAILURE() << "exprStmt's expression has no visible child";
                return WalkAction::Stop;
            }
            seen.push_back("exprStmt:" + std::string{t.text(first)});
        } else {
            ADD_FAILURE() << "statement's first child was neither varDecl nor exprStmt";
        }
        return WalkAction::SkipChildren;
    });

    EXPECT_EQ(seen, (std::vector<std::string>{
        "varDecl:x", "exprStmt:y", "varDecl:w",
    }));
}

TEST(TreeEndToEnd, BrokenPath_UnknownTokenRecovered) {
    auto h = tokenizeShipped("var x = @;");
    ASSERT_NE(h.schema, nullptr);
    h.dismissLexerDiags();   // `@` triggers P_IllegalChar on purpose

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto vd   = b.open(h.schema->rules().find("varDecl"));
        // Push every token straight through: var, ' ', x, ' ', =, ' ',
        // @, ;, Eof. The tokenizer emits `@` as an Error token (with
        // a P_IllegalChar entry in `h.lexerDiags`); the builder still
        // emits P_UnknownToken when it can't resolve the lexeme.
        while (!h.stream.isAtEnd()) b.pushToken(h.stream.advance());
    }
    Tree t = std::move(b).finish();

    const auto diags = t.diagnostics().all();
    EXPECT_TRUE(t.diagnostics().hasErrors());
    EXPECT_TRUE(hasError(t.flags(t.root())));
    EXPECT_GE(countCode(diags, DiagnosticCode::P_UnknownToken), 1u);

    // The bad token must materialize as a flagged descendant — not just a
    // diagnostic entry. Catches a regression where the unknown token gets
    // silently dropped without an Error leaf.
    EXPECT_GE(countErrorDescendants(t, t.root()), 1u);

    // Error-leaf print format is implementation-defined; assert shape via
    // substring presence rather than full string equality.
    const auto printed = prettyPrint(t);
    EXPECT_NE(printed.find("rule:root"),    std::string::npos);
    EXPECT_NE(printed.find("rule:varDecl"), std::string::npos);
    EXPECT_NE(printed.find("tok:\"var\""),  std::string::npos);
    EXPECT_NE(printed.find("tok:\";\""),    std::string::npos);
}

TEST(TreeEndToEnd, BrokenPath_UnclosedScopesAtEof) {
    auto h = tokenizeShipped("var x");
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};

    // Guards held in a heap vector and dropped after finish() — otherwise
    // their destructors would close the frames before finish() sees them.
    auto guards = std::make_unique<std::vector<TreeBuilder::OpenScope>>();
    guards->push_back(b.open(h.schema->rules().find("root")));
    guards->push_back(b.open(h.schema->rules().find("statement")));
    guards->push_back(b.open(h.schema->rules().find("varDecl")));
    while (!h.stream.isAtEnd()) b.pushToken(h.stream.advance());

    Tree t = std::move(b).finish();
    guards.reset();   // safe: closeFrame_ no-ops on a finished builder

    const auto diags = t.diagnostics().all();
    EXPECT_TRUE(t.diagnostics().hasErrors());
    EXPECT_EQ(countCode(diags, DiagnosticCode::P_PrematureEndOfInput), 3u);
    EXPECT_TRUE(hasError(t.flags(t.root())));
}

TEST(TreeEndToEnd, BrokenPath_TruncatedAfterKeyword) {
    auto h = tokenizeShipped("var");
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    auto guards = std::make_unique<std::vector<TreeBuilder::OpenScope>>();
    guards->push_back(b.open(h.schema->rules().find("root")));
    guards->push_back(b.open(h.schema->rules().find("statement")));
    guards->push_back(b.open(h.schema->rules().find("varDecl")));
    b.pushToken(h.stream.advance());

    Tree t = std::move(b).finish();
    guards.reset();

    const auto diags = t.diagnostics().all();
    EXPECT_TRUE(t.diagnostics().hasErrors());
    EXPECT_EQ(countCode(diags, DiagnosticCode::P_PrematureEndOfInput), 3u);

    const auto printed = prettyPrint(t);
    EXPECT_NE(printed.find("rule:varDecl"), std::string::npos);
    EXPECT_NE(printed.find("tok:\"var\""),  std::string::npos);
}

TEST(TreeEndToEnd, BrokenPath_PushErrorRecovered) {
    // pushError is the parser's explicit "wrong token" signal: inserts an
    // Error leaf, emits P_UnexpectedToken, propagates HasError to root.
    // Distinct from BrokenPath_UnknownTokenRecovered which exercises
    // schema-rejects-lexeme; this covers parser-rejects-token.
    auto h = tokenizeShipped("var x = ?;");
    ASSERT_NE(h.schema, nullptr);
    h.dismissLexerDiags();   // `?` triggers P_IllegalChar on purpose

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto vd   = b.open(h.schema->rules().find("varDecl"));
        // Consume the prefix `var x = ` (6 tokens including spaces),
        // then synthesize the parser-level error instead of pushing
        // the tokenized `?` (which would also produce an Error leaf —
        // but via the schema-resolves-empty path, not via pushError).
        for (int i = 0; i < 6; ++i) b.pushToken(h.stream.advance());
        b.pushError(SourceSpan::of(8, 9), std::nullopt,
                    h.schema->schemaTokens().find(tokens::kIdentifier),
                    "expected expression");
        // Skip past the `?` token the tokenizer produced.
        (void)h.stream.advance();
        b.pushToken(h.stream.advance());
    }
    Tree t = std::move(b).finish();

    const auto diags = t.diagnostics().all();
    EXPECT_TRUE(t.diagnostics().hasErrors());
    EXPECT_TRUE(hasError(t.flags(t.root())));
    EXPECT_GE(countCode(diags, DiagnosticCode::P_UnexpectedToken), 1u);
    EXPECT_GE(countErrorDescendants(t, t.root()), 1u);
}

TEST(TreeEndToEnd, BrokenPath_PopScopeUnderflow) {
    // Pushing a closesScope token (`}`) with no opener exercises the
    // scope-stack-underflow path inside pushToken → popScope, which
    // emits P_BuilderInvariant. Proves the schema's scope-effect
    // resolution is wired up at the E2E level (T5 doesn't yet emit
    // the dedicated P_UnmatchedClose code — that's reserved for a
    // future parser-level enhancement).
    auto h = tokenizeShipped("}");
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        b.pushToken(h.stream.advance());
    }
    Tree t = std::move(b).finish();

    const auto diags = t.diagnostics().all();
    EXPECT_GE(countCode(diags, DiagnosticCode::P_BuilderInvariant), 1u);
}
