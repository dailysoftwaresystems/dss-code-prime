#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/token.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_builder.hpp"
#include "core/types/tree_node.hpp"
#include "test_pretty_print.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

// Hand-tokenized end-to-end tests driving the shipped c-subset.lang.json
// through TreeBuilder. The tokenizer doesn't exist yet, so each test
// synthesizes Token objects pointing into the SourceBuffer — the same
// pattern test_tree_end_to_end.cpp uses for toy.lang.json.
//
// These tests pin structural shapes, not parser semantics. Operator
// precedence is NOT modeled by the schema today — see
// `ExpressionWithMixedOpsIsLeftFolded` for the empirical record of that
// limitation. When precedence lands in the schema, that test's expected
// literal flips from a flat sequence to a nested grouping.

using namespace dss;
using dss::tests::prettyPrint;

namespace {

struct CSubsetHarness {
    std::shared_ptr<SourceBuffer>        src;
    std::shared_ptr<GrammarSchema const> schema;
};

CSubsetHarness loadShippedCSubset(std::string sourceText) {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    if (!loaded) {
        ADD_FAILURE() << "loadShipped(\"c-subset\") failed: "
                      << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message)
                      << " (cwd=" << std::filesystem::current_path().string() << ")";
        return {SourceBuffer::fromString(std::move(sourceText), "<c-subset>"), nullptr};
    }
    return {SourceBuffer::fromString(std::move(sourceText), "<c-subset>"), *loaded};
}

// Synthesize a Token at the first occurrence of `text` in `src`. Caller
// must ensure `text` is unique in the source — use the offset overload
// otherwise. On lookup failure, raises ADD_FAILURE and returns a Token
// with an empty (zero-width at offset 0) span so downstream pushToken
// won't produce span-overflow garbage that buries the original failure
// signal.
Token at(SourceBuffer const& src, std::string_view text,
         CoreTokenKind kind = CoreTokenKind::Operator) {
    const auto sv = src.text();
    const auto first = sv.find(text);
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

Token at(SourceBuffer const& src, std::string_view text, std::size_t startHint,
         CoreTokenKind kind = CoreTokenKind::Operator) {
    EXPECT_EQ(src.text().substr(startHint, text.size()), text);
    return Token{
        .coreKind   = kind,
        .schemaKind = InvalidSchemaToken,
        .span       = SourceSpan::of(static_cast<ByteOffset>(startHint),
                                      static_cast<ByteOffset>(startHint + text.size())),
    };
}

} // namespace

// Drives `int x = 5;` through the topLevel → varDecl chain. Pins the
// varDeclHead refactor (typeRef + Identifier + optional initializer) plus
// the typeBase nesting under typeRef that carries the future const slot.
TEST(CSubsetEndToEnd, TopLevelVarDeclWithIntInitializer) {
    auto h = loadShippedCSubset("int x = 5;");
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto top  = b.open(h.schema->rules().find("topLevel"));
        auto vd   = b.open(h.schema->rules().find("varDecl"));
        {
            auto head = b.open(h.schema->rules().find("varDeclHead"));
            {
                auto ty = b.open(h.schema->rules().find("typeRef"));
                {
                    auto tb = b.open(h.schema->rules().find("typeBase"));
                    b.pushToken(at(*h.src, "int", CoreTokenKind::Word));
                }
            }
            b.pushToken(at(*h.src, "x", CoreTokenKind::Word));
            b.pushToken(at(*h.src, "="));
            {
                auto expr = b.open(h.schema->rules().find("expression"));
                {
                    auto opr = b.open(h.schema->rules().find("operand"));
                    b.pushToken(at(*h.src, "5", CoreTokenKind::Word));
                }
            }
        }
        b.pushToken(at(*h.src, ";"));
    }
    Tree t = std::move(b).finish();

    EXPECT_TRUE(t.diagnostics().all().empty());
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_FALSE(hasError(t.flags(t.root())));

    const std::string_view expected =
        "rule:root\n"
        "  rule:topLevel\n"
        "    rule:varDecl\n"
        "      rule:varDeclHead\n"
        "        rule:typeRef\n"
        "          rule:typeBase\n"
        "            tok:\"int\"\n"
        "        tok:\"x\"\n"
        "        tok:\"=\"\n"
        "        rule:expression\n"
        "          rule:operand\n"
        "            tok:\"5\"\n"
        "      tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

// Drives `int main(void) { if (x) { return x; } }` through topLevel →
// funcDecl → block → statement → ifStmt → block → returnStmt. Exercises
// the actual config path real C source would take, not a synthetic
// `statement`-under-`root` shortcut: a typo in any of `funcDecl` /
// `paramList` / `param` / `block` / `ifStmt` / `returnStmt` in the JSON
// would fail this test.
TEST(CSubsetEndToEnd, FunctionWithIfReturnInsideBlock) {
    auto h = loadShippedCSubset("int main(void) { if (x) { return x; } }");
    ASSERT_NE(h.schema, nullptr);
    const auto src = h.src->text();
    const auto x1 = src.find("x");
    const auto x2 = src.find("x", x1 + 1);
    const auto firstParen   = src.find("(");
    const auto firstParenC  = src.find(")");
    const auto secondParen  = src.find("(", firstParen + 1);
    const auto secondParenC = src.find(")", firstParenC + 1);
    const auto outerBlockO  = src.find("{");
    const auto innerBlockO  = src.find("{", outerBlockO + 1);
    const auto innerBlockC  = src.find("}");
    const auto outerBlockC  = src.find("}", innerBlockC + 1);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto top  = b.open(h.schema->rules().find("topLevel"));
        auto fn   = b.open(h.schema->rules().find("funcDecl"));
        {
            auto ty = b.open(h.schema->rules().find("typeRef"));
            auto tb = b.open(h.schema->rules().find("typeBase"));
            b.pushToken(at(*h.src, "int", CoreTokenKind::Word));
        }
        b.pushToken(at(*h.src, "main", CoreTokenKind::Word));
        b.pushToken(at(*h.src, "(", firstParen));
        {
            auto pl = b.open(h.schema->rules().find("paramList"));
            auto p  = b.open(h.schema->rules().find("param"));
            auto ty = b.open(h.schema->rules().find("typeRef"));
            auto tb = b.open(h.schema->rules().find("typeBase"));
            b.pushToken(at(*h.src, "void", CoreTokenKind::Word));
        }
        b.pushToken(at(*h.src, ")", firstParenC));
        {
            auto blk = b.open(h.schema->rules().find("block"));
            b.pushToken(at(*h.src, "{", outerBlockO));
            {
                auto stmt = b.open(h.schema->rules().find("statement"));
                auto ifs  = b.open(h.schema->rules().find("ifStmt"));
                b.pushToken(at(*h.src, "if", CoreTokenKind::Word));
                b.pushToken(at(*h.src, "(", secondParen));
                {
                    auto expr = b.open(h.schema->rules().find("expression"));
                    auto opr  = b.open(h.schema->rules().find("operand"));
                    b.pushToken(at(*h.src, "x", x1, CoreTokenKind::Word));
                }
                b.pushToken(at(*h.src, ")", secondParenC));
                {
                    auto innerStmt = b.open(h.schema->rules().find("statement"));
                    auto innerBlk  = b.open(h.schema->rules().find("block"));
                    b.pushToken(at(*h.src, "{", innerBlockO));
                    {
                        auto retStmt = b.open(h.schema->rules().find("statement"));
                        auto rs      = b.open(h.schema->rules().find("returnStmt"));
                        b.pushToken(at(*h.src, "return", CoreTokenKind::Word));
                        {
                            auto retExpr = b.open(h.schema->rules().find("expression"));
                            auto retOpr  = b.open(h.schema->rules().find("operand"));
                            b.pushToken(at(*h.src, "x", x2, CoreTokenKind::Word));
                        }
                        b.pushToken(at(*h.src, ";"));
                    }
                    b.pushToken(at(*h.src, "}", innerBlockC));
                }
            }
            b.pushToken(at(*h.src, "}", outerBlockC));
        }
    }
    Tree t = std::move(b).finish();

    EXPECT_TRUE(t.diagnostics().all().empty()) << "first diag code: "
        << (t.diagnostics().all().empty()
            ? "<none>"
            : diagnosticCodeName(t.diagnostics().all()[0].code));
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_FALSE(hasError(t.flags(t.root())));

    const std::string_view expected =
        "rule:root\n"
        "  rule:topLevel\n"
        "    rule:funcDecl\n"
        "      rule:typeRef\n"
        "        rule:typeBase\n"
        "          tok:\"int\"\n"
        "      tok:\"main\"\n"
        "      tok:\"(\"\n"
        "      rule:paramList\n"
        "        rule:param\n"
        "          rule:typeRef\n"
        "            rule:typeBase\n"
        "              tok:\"void\"\n"
        "      tok:\")\"\n"
        "      rule:block\n"
        "        tok:\"{\"\n"
        "        rule:statement\n"
        "          rule:ifStmt\n"
        "            tok:\"if\"\n"
        "            tok:\"(\"\n"
        "            rule:expression\n"
        "              rule:operand\n"
        "                tok:\"x\"\n"
        "            tok:\")\"\n"
        "            rule:statement\n"
        "              rule:block\n"
        "                tok:\"{\"\n"
        "                rule:statement\n"
        "                  rule:returnStmt\n"
        "                    tok:\"return\"\n"
        "                    rule:expression\n"
        "                      rule:operand\n"
        "                        tok:\"x\"\n"
        "                    tok:\";\"\n"
        "                tok:\"}\"\n"
        "        tok:\"}\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

// Pins the current shape of `a + b * c;` — a flat left-fold under one
// expression node rather than the (a + (b * c)) grouping C requires.
// The schema as shipped today has no concept of operator precedence;
// `expression` is `operand (binaryOp operand)*` with no grouping rule.
// When precedence lands, this test's expected literal flips from a flat
// sequence to a nested binaryExpr grouping — that flip is the visible
// signal the limitation has been removed.
TEST(CSubsetEndToEnd, ExpressionWithMixedOpsIsLeftFolded) {
    auto h = loadShippedCSubset("a + b * c;");
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto es   = b.open(h.schema->rules().find("exprStmt"));
        {
            auto expr = b.open(h.schema->rules().find("expression"));
            {
                auto opr = b.open(h.schema->rules().find("operand"));
                b.pushToken(at(*h.src, "a", CoreTokenKind::Word));
            }
            {
                auto bop = b.open(h.schema->rules().find("binaryOp"));
                b.pushToken(at(*h.src, "+"));
            }
            {
                auto opr = b.open(h.schema->rules().find("operand"));
                b.pushToken(at(*h.src, "b", CoreTokenKind::Word));
            }
            {
                auto bop = b.open(h.schema->rules().find("binaryOp"));
                b.pushToken(at(*h.src, "*"));
            }
            {
                auto opr = b.open(h.schema->rules().find("operand"));
                b.pushToken(at(*h.src, "c", CoreTokenKind::Word));
            }
        }
        b.pushToken(at(*h.src, ";"));
    }
    Tree t = std::move(b).finish();

    EXPECT_TRUE(t.diagnostics().all().empty());
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_FALSE(hasError(t.flags(t.root())));

    const std::string_view expected =
        "rule:root\n"
        "  rule:statement\n"
        "    rule:exprStmt\n"
        "      rule:expression\n"
        "        rule:operand\n"
        "          tok:\"a\"\n"
        "        rule:binaryOp\n"
        "          tok:\"+\"\n"
        "        rule:operand\n"
        "          tok:\"b\"\n"
        "        rule:binaryOp\n"
        "          tok:\"*\"\n"
        "        rule:operand\n"
        "          tok:\"c\"\n"
        "      tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}
