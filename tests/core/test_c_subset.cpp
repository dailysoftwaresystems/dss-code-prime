#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/operator_table.hpp"
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

// Drives `int x = 5;` through the disambiguated topLevel shape:
// typeRef → Identifier → topLevelTail → varDeclTail. The shape graph
// resolves funcDecl-vs-varDecl on the post-Identifier token (ParenOpen vs
// AssignOp/EndStatement); both branches share a single typeRef + Identifier
// prefix so the loader's alt-FIRST ambiguity check stays happy.
TEST(CSubsetEndToEnd, TopLevelVarDeclWithIntInitializer) {
    auto h = loadShippedCSubset("int x = 5;");
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto top  = b.open(h.schema->rules().find("topLevel"));
        {
            auto ty = b.open(h.schema->rules().find("typeRef"));
            auto tb = b.open(h.schema->rules().find("typeBase"));
            b.pushToken(at(*h.src, "int", CoreTokenKind::Word));
        }
        b.pushToken(at(*h.src, "x", CoreTokenKind::Word));
        {
            auto tail   = b.open(h.schema->rules().find("topLevelTail"));
            auto vdTail = b.open(h.schema->rules().find("varDeclTail"));
            b.pushToken(at(*h.src, "="));
            {
                auto expr = b.open(h.schema->rules().find("expression"));
                auto opr  = b.open(h.schema->rules().find("operand"));
                b.pushToken(at(*h.src, "5", CoreTokenKind::Word));
            }
            b.pushToken(at(*h.src, ";"));
        }
    }
    Tree t = std::move(b).finish();

    EXPECT_TRUE(t.diagnostics().all().empty());
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_FALSE(hasError(t.flags(t.root())));

    const std::string_view expected =
        "rule:root\n"
        "  rule:topLevel\n"
        "    rule:typeRef\n"
        "      rule:typeBase\n"
        "        tok:\"int\"\n"
        "    tok:\"x\"\n"
        "    rule:topLevelTail\n"
        "      rule:varDeclTail\n"
        "        tok:\"=\"\n"
        "        rule:expression\n"
        "          rule:operand\n"
        "            tok:\"5\"\n"
        "        tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

// Drives `int main(void) { if (x) { return x; } }` through topLevel →
// topLevelTail → funcTail → block → statement → ifStmt → block →
// returnStmt. Exercises the actual config path real C source takes.
// A typo in any of `topLevelTail` / `funcTail` / `paramList` / `param` /
// `block` / `ifStmt` / `returnStmt` would fail this test.
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
        {
            auto ty = b.open(h.schema->rules().find("typeRef"));
            auto tb = b.open(h.schema->rules().find("typeBase"));
            b.pushToken(at(*h.src, "int", CoreTokenKind::Word));
        }
        b.pushToken(at(*h.src, "main", CoreTokenKind::Word));
        {
            auto tail = b.open(h.schema->rules().find("topLevelTail"));
            auto fn   = b.open(h.schema->rules().find("funcTail"));
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
        "    rule:typeRef\n"
        "      rule:typeBase\n"
        "        tok:\"int\"\n"
        "    tok:\"main\"\n"
        "    rule:topLevelTail\n"
        "      rule:funcTail\n"
        "        tok:\"(\"\n"
        "        rule:paramList\n"
        "          rule:param\n"
        "            rule:typeRef\n"
        "              rule:typeBase\n"
        "                tok:\"void\"\n"
        "        tok:\")\"\n"
        "        rule:block\n"
        "          tok:\"{\"\n"
        "          rule:statement\n"
        "            rule:ifStmt\n"
        "              tok:\"if\"\n"
        "              tok:\"(\"\n"
        "              rule:expression\n"
        "                rule:operand\n"
        "                  tok:\"x\"\n"
        "              tok:\")\"\n"
        "              rule:statement\n"
        "                rule:block\n"
        "                  tok:\"{\"\n"
        "                  rule:statement\n"
        "                    rule:returnStmt\n"
        "                      tok:\"return\"\n"
        "                      rule:expression\n"
        "                        rule:operand\n"
        "                          tok:\"x\"\n"
        "                      tok:\";\"\n"
        "                  tok:\"}\"\n"
        "          tok:\"}\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

// Schema-side precedence is in place even though the builder doesn't yet
// consume it. `*` binds tighter than `+`, `=` is right-assoc at the
// bottom, and `-` carries distinct prefix and infix entries. The future
// parser will translate this table into nested tree shapes; today it's
// data sitting on the schema, queryable by the eventual Pratt walker.
TEST(CSubsetEndToEnd, OperatorTableMatchesCPrecedence) {
    auto h = loadShippedCSubset("");
    ASSERT_NE(h.schema, nullptr);
    auto const& table  = h.schema->operatorTable();
    auto const& tokens = h.schema->schemaTokens();

    auto plus  = table.lookup(tokens.find("PlusOp"),    OperatorArity::Infix);
    auto star  = table.lookup(tokens.find("StarOp"),    OperatorArity::Infix);
    auto assign= table.lookup(tokens.find("AssignOp"),  OperatorArity::Infix);
    auto bangP = table.lookup(tokens.find("BangOp"),    OperatorArity::Prefix);
    auto minusI= table.lookup(tokens.find("MinusOp"),   OperatorArity::Infix);
    auto minusP= table.lookup(tokens.find("MinusOp"),   OperatorArity::Prefix);

    ASSERT_TRUE(plus.has_value());
    ASSERT_TRUE(star.has_value());
    ASSERT_TRUE(assign.has_value());
    ASSERT_TRUE(bangP.has_value());
    ASSERT_TRUE(minusI.has_value());
    ASSERT_TRUE(minusP.has_value());

    EXPECT_LT(plus->precedence,   star->precedence)
        << "`+` must bind less tightly than `*`";
    EXPECT_LT(assign->precedence, plus->precedence)
        << "`=` must bind less tightly than `+`";
    EXPECT_EQ(assign->associativity, OperatorAssoc::Right);
    EXPECT_EQ(plus->associativity,   OperatorAssoc::Left);
    EXPECT_EQ(star->associativity,   OperatorAssoc::Left);

    // Same lexeme; arity-distinguished entries.
    EXPECT_NE(minusI->precedence, minusP->precedence);
    EXPECT_GT(minusP->precedence, minusI->precedence)
        << "unary `-` must bind tighter than binary `-`";
    EXPECT_EQ(minusP->associativity, OperatorAssoc::Right);
    EXPECT_GT(bangP->precedence, plus->precedence);
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

    // This test opens `statement` directly under `root`, bypassing the
    // c-subset schema's typical `topLevel → topLevelTail → funcTail → block
    // → statement` path. The schema cursor goes off-track on the first
    // statement open and emits a single P_SchemaCursorDesync (info) per
    // the one-shot policy. The tree shape itself is the structural pin.
    auto const& diags = t.diagnostics().all();
    EXPECT_EQ(diags.size(), 1u);
    if (!diags.empty()) {
        EXPECT_EQ(diags.front().code, DiagnosticCode::P_SchemaCursorDesync);
        EXPECT_EQ(diags.front().severity, DiagnosticSeverity::Info);
    }
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

// SH4b: end-to-end parse of a switch statement with a `case` arm + a
// `default` arm + a `break`. Closes v2-gap-catalog row 11. The design
// is shape-based positioning (`caseLabel` shape inside `switchBodyItem`),
// NOT `scopeRequire` — the catalog's original guess at scopeRequire-based
// adoption doesn't fit C's scope mechanics (Block is innermost inside a
// switch body, not Switch). See `.plans/substrate-hardening-plan.md` SH4b
// for the design call.
TEST(CSubsetEndToEnd, SwitchStmtParsesAllArmKinds) {
    auto h = loadShippedCSubset("switch (x) { case 1: break; default: break; }");
    ASSERT_NE(h.schema, nullptr);
    const auto src = h.src->text();
    const auto firstParen   = src.find("(");
    const auto firstParenC  = src.find(")");
    const auto bodyOpen     = src.find("{");
    const auto bodyClose    = src.find("}");
    const auto firstBreak   = src.find("break");
    const auto secondBreak  = src.find("break", firstBreak + 1);
    const auto firstSemi    = src.find(";");
    const auto secondSemi   = src.find(";", firstSemi + 1);
    const auto firstColon   = src.find(":");
    const auto secondColon  = src.find(":", firstColon + 1);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto sw   = b.open(h.schema->rules().find("switchStmt"));
        b.pushToken(at(*h.src, "switch", CoreTokenKind::Word));
        b.pushToken(at(*h.src, "(", firstParen));
        {
            auto expr = b.open(h.schema->rules().find("expression"));
            auto opr  = b.open(h.schema->rules().find("operand"));
            b.pushToken(at(*h.src, "x", CoreTokenKind::Word));
        }
        b.pushToken(at(*h.src, ")", firstParenC));
        b.pushToken(at(*h.src, "{", bodyOpen));
        // ── case arm
        {
            auto item = b.open(h.schema->rules().find("switchBodyItem"));
            auto lbl  = b.open(h.schema->rules().find("caseLabel"));
            b.pushToken(at(*h.src, "case", CoreTokenKind::Word));
            {
                auto expr = b.open(h.schema->rules().find("expression"));
                auto opr  = b.open(h.schema->rules().find("operand"));
                b.pushToken(at(*h.src, "1", CoreTokenKind::Word));
            }
            b.pushToken(at(*h.src, ":", firstColon));
        }
        // ── break inside the case arm (parses as a sibling switchBodyItem
        //    statement, not a child of caseLabel — C's case fall-through
        //    structure is "label then statements", not "label containing
        //    statements").
        {
            auto item = b.open(h.schema->rules().find("switchBodyItem"));
            auto inner = b.open(h.schema->rules().find("statement"));
            auto br    = b.open(h.schema->rules().find("breakStmt"));
            b.pushToken(at(*h.src, "break", firstBreak, CoreTokenKind::Word));
            b.pushToken(at(*h.src, ";", firstSemi));
        }
        // ── default arm
        {
            auto item = b.open(h.schema->rules().find("switchBodyItem"));
            auto lbl  = b.open(h.schema->rules().find("caseLabel"));
            b.pushToken(at(*h.src, "default", CoreTokenKind::Word));
            b.pushToken(at(*h.src, ":", secondColon));
        }
        {
            auto item = b.open(h.schema->rules().find("switchBodyItem"));
            auto inner = b.open(h.schema->rules().find("statement"));
            auto br    = b.open(h.schema->rules().find("breakStmt"));
            b.pushToken(at(*h.src, "break", secondBreak, CoreTokenKind::Word));
            b.pushToken(at(*h.src, ";", secondSemi));
        }
        b.pushToken(at(*h.src, "}", bodyClose));
    }
    Tree t = std::move(b).finish();

    // The test opens `statement` directly under `root` (bypassing the
    // topLevel → funcTail path), so the cursor desyncs once on the first
    // off-schema open — same one-shot info diagnostic as
    // ExpressionWithMixedOpsIsLeftFolded above. No errors.
    auto const& diags = t.diagnostics().all();
    EXPECT_EQ(diags.size(), 1u);
    if (!diags.empty()) {
        EXPECT_EQ(diags.front().code, DiagnosticCode::P_SchemaCursorDesync);
        EXPECT_EQ(diags.front().severity, DiagnosticSeverity::Info);
    }
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_FALSE(hasError(t.flags(t.root())));

    const std::string_view expected =
        "rule:root\n"
        "  rule:statement\n"
        "    rule:switchStmt\n"
        "      tok:\"switch\"\n"
        "      tok:\"(\"\n"
        "      rule:expression\n"
        "        rule:operand\n"
        "          tok:\"x\"\n"
        "      tok:\")\"\n"
        "      tok:\"{\"\n"
        "      rule:switchBodyItem\n"
        "        rule:caseLabel\n"
        "          tok:\"case\"\n"
        "          rule:expression\n"
        "            rule:operand\n"
        "              tok:\"1\"\n"
        "          tok:\":\"\n"
        "      rule:switchBodyItem\n"
        "        rule:statement\n"
        "          rule:breakStmt\n"
        "            tok:\"break\"\n"
        "            tok:\";\"\n"
        "      rule:switchBodyItem\n"
        "        rule:caseLabel\n"
        "          tok:\"default\"\n"
        "          tok:\":\"\n"
        "      rule:switchBodyItem\n"
        "        rule:statement\n"
        "          rule:breakStmt\n"
        "            tok:\"break\"\n"
        "            tok:\";\"\n"
        "      tok:\"}\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}
