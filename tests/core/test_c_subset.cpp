#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/operator_table.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/token.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_builder.hpp"
#include "core/types/tree_node.hpp"
#include "e2e_harness.hpp"
#include "test_pretty_print.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>

// End-to-end tests driving the shipped c-subset.lang.json through the
// live tokenize → resolve → build pipeline. These pin structural
// shapes, not parser semantics. Operator precedence is NOT modeled by
// the schema today — see `ExpressionWithMixedOpsIsLeftFolded` for the
// empirical record of that limitation. When precedence lands in the
// schema, that test's expected literal flips from a flat sequence to
// a nested grouping.

using namespace dss;
using dss::tests::drainWhitespace;
using dss::tests::E2EHarness;
using dss::tests::prettyPrint;
using dss::tests::pushNext;
using dss::tests::tokenizeShipped;

// Drives `int x = 5;` through the FC4 c1 specifier/declarator topLevel
// shape: topLevelHead carries the base type; the name + init live in the
// initDeclaratorList's initDeclarator (declarator → directDeclarator →
// Identifier); the `;` is the topLevelDeclTail's EndStatement arm
// (vs `{` = function definition — FIRST-disjoint).
TEST(CSubsetEndToEnd, TopLevelVarDeclWithIntInitializer) {
    auto h = tokenizeShipped("c-subset", "int x = 5;");
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto top  = b.open(h.schema->rules().find("topLevel"));
        auto tld  = b.open(h.schema->rules().find("topLevelDecl"));
        {
            auto head = b.open(h.schema->rules().find("topLevelHead"));
            auto ts   = b.open(h.schema->rules().find("typeSpecifierSeq"));
            b.pushToken(h.stream.advance());
        }
        {
            auto list  = b.open(h.schema->rules().find("initDeclaratorList"));
            auto idecl = b.open(h.schema->rules().find("initDeclarator"));
            {
                auto dtor   = b.open(h.schema->rules().find("declarator"));
                auto direct = b.open(h.schema->rules().find("directDeclarator"));
                drainWhitespace(b, h.stream);
                b.pushToken(h.stream.advance());
            }
            drainWhitespace(b, h.stream);
            pushNext(b, h.stream);
            {
                auto iv   = b.open(h.schema->rules().find("initValue"));
                auto expr = b.open(h.schema->rules().find("expression"));
                auto opr  = b.open(h.schema->rules().find("operand"));
                b.pushToken(h.stream.advance());
            }
        }
        {
            auto tail = b.open(h.schema->rules().find("topLevelDeclTail"));
            b.pushToken(h.stream.advance());
        }
    }
    Tree t = std::move(b).finish();

    EXPECT_TRUE(t.diagnostics().all().empty());
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_FALSE(hasError(t.flags(t.root())));
    const std::string_view expected =
        "rule:root\n"
        "  rule:topLevel\n"
        "    rule:topLevelDecl\n"
        "      rule:topLevelHead\n"
        "        rule:typeSpecifierSeq\n"
        "          tok:\"int\"\n"
        "      rule:initDeclaratorList\n"
        "        rule:initDeclarator\n"
        "          rule:declarator\n"
        "            rule:directDeclarator\n"
        "              tok:\"x\"\n"
        "          tok:\"=\"\n"
        "          rule:initValue\n"
        "            rule:expression\n"
        "              rule:operand\n"
        "                tok:\"5\"\n"
        "      rule:topLevelDeclTail\n"
        "        tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

// Drives `int main(void) { if (x) { return x; } }` through topLevel →
// topLevelTail → funcTail → block → statement → ifStmt → block →
// returnStmt. Exercises the actual config path real C source takes.
// A typo in any of `topLevelTail` / `funcTail` / `paramList` / `param` /
// `block` / `ifStmt` / `returnStmt` would fail this test.
TEST(CSubsetEndToEnd, FunctionWithIfReturnInsideBlock) {
    auto h = tokenizeShipped("c-subset", "int main(void) { if (x) { return x; } }");
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto top  = b.open(h.schema->rules().find("topLevel"));
        auto tld  = b.open(h.schema->rules().find("topLevelDecl"));
        {
            auto head = b.open(h.schema->rules().find("topLevelHead"));
            auto ts   = b.open(h.schema->rules().find("typeSpecifierSeq"));
            b.pushToken(h.stream.advance());
        }
        // FC4 c1: the name + parameter list live in the DECLARATOR — the
        // fn suffix is a direct-declarator child, the block is the tail.
        {
            auto list   = b.open(h.schema->rules().find("initDeclaratorList"));
            auto idecl  = b.open(h.schema->rules().find("initDeclarator"));
            auto dtor   = b.open(h.schema->rules().find("declarator"));
            auto direct = b.open(h.schema->rules().find("directDeclarator"));
            drainWhitespace(b, h.stream);
            b.pushToken(h.stream.advance());
            auto fs = b.open(h.schema->rules().find("fnSuffix"));
            b.pushToken(h.stream.advance());
            {
                auto pl   = b.open(h.schema->rules().find("paramList"));
                auto p    = b.open(h.schema->rules().find("param"));
                auto head = b.open(h.schema->rules().find("declHeadForParam"));
                auto ts   = b.open(h.schema->rules().find("typeSpecifierSeq"));
                b.pushToken(h.stream.advance());
            }
            pushNext(b, h.stream);
        }
        {
            auto tail = b.open(h.schema->rules().find("topLevelDeclTail"));
            {
                auto blk = b.open(h.schema->rules().find("block"));
                pushNext(b, h.stream);
                {
                    auto stmt = b.open(h.schema->rules().find("statement"));
                    auto ifs  = b.open(h.schema->rules().find("ifStmt"));
                    pushNext(b, h.stream);
                    b.pushToken(h.stream.advance());
                    {
                        auto expr = b.open(h.schema->rules().find("expression"));
                        auto opr  = b.open(h.schema->rules().find("operand"));
                        b.pushToken(h.stream.advance());
                    }
                    pushNext(b, h.stream);
                    {
                        auto innerStmt = b.open(h.schema->rules().find("statement"));
                        auto innerBlk  = b.open(h.schema->rules().find("block"));
                        pushNext(b, h.stream);
                        {
                            auto retStmt = b.open(h.schema->rules().find("statement"));
                            auto rs      = b.open(h.schema->rules().find("returnStmt"));
                            pushNext(b, h.stream);
                            {
                                auto retExpr = b.open(h.schema->rules().find("expression"));
                                auto retOpr  = b.open(h.schema->rules().find("operand"));
                                b.pushToken(h.stream.advance());
                            }
                            b.pushToken(h.stream.advance());
                        }
                        drainWhitespace(b, h.stream);
                        b.pushToken(h.stream.advance());
                    }
                }
                drainWhitespace(b, h.stream);
                b.pushToken(h.stream.advance());
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
        "    rule:topLevelDecl\n"
        "      rule:topLevelHead\n"
        "        rule:typeSpecifierSeq\n"
        "          tok:\"int\"\n"
        "      rule:initDeclaratorList\n"
        "        rule:initDeclarator\n"
        "          rule:declarator\n"
        "            rule:directDeclarator\n"
        "              tok:\"main\"\n"
        "              rule:fnSuffix\n"
        "                tok:\"(\"\n"
        "                rule:paramList\n"
        "                  rule:param\n"
        "                    rule:declHeadForParam\n"
        "                      rule:typeSpecifierSeq\n"
        "                        tok:\"void\"\n"
        "                tok:\")\"\n"
        "      rule:topLevelDeclTail\n"
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
    auto h = tokenizeShipped("c-subset", "");
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

// v2-gap-catalog row 1 used to live here as the flat-fold pin
// (`ExpressionWithMixedOpsIsLeftFolded`). PA2 closed the gap by
// migrating c-subset's `expression` rule to use the `expr` shape and
// wiring the Pratt walker into the parser. The precedence-correct
// shape is now pinned end-to-end in
// `tests/analysis/syntactic/test_parser_c_subset_pratt.cpp`, where it
// exercises the actual walker rather than a hand-rolled flat tree.
//
// The hand-driven test pattern no longer applies: with `binaryOp`
// removed from the schema and `expression` now expr-kind, the only
// way to produce a structurally-correct expression tree is through
// the parser — which is exactly what PA4's broader hand-driven→
// parser-driven flip is about.

// SH4b: end-to-end parse of a switch statement with a `case` arm + a
// `default` arm + a `break`. Closes v2-gap-catalog row 11. The design
// is shape-based positioning (`caseLabel` shape inside `switchBodyItem`),
// NOT `scopeRequire` — the catalog's original guess at scopeRequire-based
// adoption doesn't fit C's scope mechanics (Block is innermost inside a
// switch body, not Switch). See `.plans/03-substrate-hardening-plan - ok.md` SH4b
// for the design call.
TEST(CSubsetEndToEnd, SwitchStmtParsesAllArmKinds) {
    auto h = tokenizeShipped("c-subset",
                             "switch (x) { case 1: break; default: break; }");
    ASSERT_NE(h.schema, nullptr);

    TreeBuilder b{h.src, h.schema};
    {
        auto root = b.open(h.schema->rules().find("root"));
        auto stmt = b.open(h.schema->rules().find("statement"));
        auto sw   = b.open(h.schema->rules().find("switchStmt"));
        pushNext(b, h.stream);
        b.pushToken(h.stream.advance());
        {
            auto expr = b.open(h.schema->rules().find("expression"));
            auto opr  = b.open(h.schema->rules().find("operand"));
            b.pushToken(h.stream.advance());
        }
        pushNext(b, h.stream);
        pushNext(b, h.stream);
        // ── case arm
        {
            auto item = b.open(h.schema->rules().find("switchBodyItem"));
            auto lbl  = b.open(h.schema->rules().find("caseLabel"));
            pushNext(b, h.stream);
            {
                auto expr = b.open(h.schema->rules().find("expression"));
                auto opr  = b.open(h.schema->rules().find("operand"));
                b.pushToken(h.stream.advance());
            }
            b.pushToken(h.stream.advance());
        }
        drainWhitespace(b, h.stream);
        // ── break inside the case arm (parses as a sibling switchBodyItem
        //    statement, not a child of caseLabel — C's case fall-through
        //    structure is "label then statements", not "label containing
        //    statements").
        {
            auto item = b.open(h.schema->rules().find("switchBodyItem"));
            auto inner = b.open(h.schema->rules().find("statement"));
            auto br    = b.open(h.schema->rules().find("breakStmt"));
            b.pushToken(h.stream.advance());
            b.pushToken(h.stream.advance());
        }
        drainWhitespace(b, h.stream);
        // ── default arm
        {
            auto item = b.open(h.schema->rules().find("switchBodyItem"));
            auto lbl  = b.open(h.schema->rules().find("caseLabel"));
            b.pushToken(h.stream.advance());
            b.pushToken(h.stream.advance());
        }
        drainWhitespace(b, h.stream);
        {
            auto item = b.open(h.schema->rules().find("switchBodyItem"));
            auto inner = b.open(h.schema->rules().find("statement"));
            auto br    = b.open(h.schema->rules().find("breakStmt"));
            b.pushToken(h.stream.advance());
            b.pushToken(h.stream.advance());
        }
        drainWhitespace(b, h.stream);
        b.pushToken(h.stream.advance());
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
