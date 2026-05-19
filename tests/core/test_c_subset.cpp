#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/token.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_builder.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/tree_node.hpp"
#include "core/types/tree_visitor.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

// PR0 hand-tokenized end-to-end: drives the c-subset.lang.json config through
// TreeBuilder for a handful of representative C snippets. The tokenizer
// doesn't exist yet, so tests synthesize Token objects pointing into the
// SourceBuffer, exactly like test_tree_end_to_end.cpp does for toy.
//
// These tests are intentionally THIN — they prove the c-subset shape graph
// accepts the right structural shapes, not that any future parser would
// produce them correctly. The gap catalog (v2-gap-catalog.md) enumerates
// what these tests cannot yet verify (operator precedence, typedef, etc.).

using namespace dss;

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

// Synthesize a Token at the first occurrence of `text` in `src`. The
// uniqueness contract matches ToyHarness::tok — tests that need a repeated
// lexeme should use the offset overload.
Token at(SourceBuffer const& src, std::string_view text,
         CoreTokenKind kind = CoreTokenKind::Operator) {
    const auto sv = src.text();
    const auto first = sv.find(text);
    EXPECT_NE(first, std::string_view::npos) << "lexeme '" << text << "' not in source";
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

// Walk the tree in AST mode emitting "rule:<name>" / tok:"<text>" lines.
// Same shape as test_tree_end_to_end's prettyPrint — kept local so this
// file is self-contained and doesn't depend on toy-specific helpers.
std::string prettyPrint(Tree const& t) {
    std::string out;
    if (!t.root().valid()) return out;
    walkPreOrder(TreeCursor{t, t.root(), CursorMode::Ast},
                 [&](TreeCursor const& c) {
        const int d = c.depth();
        for (int i = 0; i < d; ++i) out += "  ";
        const auto id = c.current();
        if (t.kind(id) == NodeKind::Internal) {
            out += "rule:";
            out += t.rules().name(t.rule(id));
        } else {
            out += "tok:\"";
            out += t.text(id);
            out += '"';
        }
        out += '\n';
    });
    return out;
}

} // namespace

// `int x = 5;` — typeRef Identifier AssignOp IntLiteral EndStatement,
// inside topLevel → varDecl.
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

// `if (x) { return x; }` — exercises the block/return/Paren scope chain.
// We open `statement` directly under `root`: the c-subset schema's
// topLevel-shape enforcement is parser work (not yet implemented), so the
// builder accepts any rule we ask it to open. Two `x` occurrences need
// explicit offsets so the second resolves inside the return.
TEST(CSubsetEndToEnd, IfWithReturnInsideBlock) {
    auto h = loadShippedCSubset("if (x) { return x; }");
    ASSERT_NE(h.schema, nullptr);
    const auto src = h.src->text();
    const auto x1 = src.find("x");
    const auto x2 = src.find("x", x1 + 1);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto ifs  = b.open(h.schema->rules().find("ifStmt"));
        b.pushToken(at(*h.src, "if", CoreTokenKind::Word));
        b.pushToken(at(*h.src, "("));
        {
            auto expr = b.open(h.schema->rules().find("expression"));
            auto opr  = b.open(h.schema->rules().find("operand"));
            b.pushToken(at(*h.src, "x", x1, CoreTokenKind::Word));
        }
        b.pushToken(at(*h.src, ")"));
        {
            auto innerStmt = b.open(h.schema->rules().find("statement"));
            auto blk       = b.open(h.schema->rules().find("block"));
            b.pushToken(at(*h.src, "{"));
            {
                auto innerInner = b.open(h.schema->rules().find("statement"));
                auto rs         = b.open(h.schema->rules().find("returnStmt"));
                b.pushToken(at(*h.src, "return", CoreTokenKind::Word));
                {
                    auto retExpr = b.open(h.schema->rules().find("expression"));
                    auto retOpr  = b.open(h.schema->rules().find("operand"));
                    b.pushToken(at(*h.src, "x", x2, CoreTokenKind::Word));
                }
                b.pushToken(at(*h.src, ";"));
            }
            b.pushToken(at(*h.src, "}"));
        }
    }
    Tree t = std::move(b).finish();

    EXPECT_TRUE(t.diagnostics().all().empty()) << "first diag code: "
        << (t.diagnostics().all().empty()
            ? "<none>"
            : diagnosticCodeName(t.diagnostics().all()[0].code));
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const std::string_view expected =
        "rule:root\n"
        "  rule:statement\n"
        "    rule:ifStmt\n"
        "      tok:\"if\"\n"
        "      tok:\"(\"\n"
        "      rule:expression\n"
        "        rule:operand\n"
        "          tok:\"x\"\n"
        "      tok:\")\"\n"
        "      rule:statement\n"
        "        rule:block\n"
        "          tok:\"{\"\n"
        "          rule:statement\n"
        "            rule:returnStmt\n"
        "              tok:\"return\"\n"
        "              rule:expression\n"
        "                rule:operand\n"
        "                  tok:\"x\"\n"
        "              tok:\";\"\n"
        "          tok:\"}\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

// `a + b * c;` — exercises the gap-#1 wrong-precedence behavior. We assert
// the CURRENT (incorrect) left-fold shape; this test will be updated when
// PR1 lands operator precedence.
//
// The shape captured here is the empirical evidence of the gap — flipping
// the assertion after PR1 will be the visible signal that precedence works.
TEST(CSubsetEndToEnd, ExpressionWithMixedOpsIsCurrentlyLeftFolded) {
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

    // Flat left-fold: a + b * c is a flat sequence of operand/binaryOp/
    // operand/binaryOp/operand under one expression node. No grouping.
    // PR1 (precedence) is what introduces the (a + (b * c)) grouping.
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
