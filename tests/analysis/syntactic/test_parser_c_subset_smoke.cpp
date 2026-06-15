#include "analysis/syntactic/parser.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/tree_node.hpp"
#include "core/types/tree_visitor.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

using namespace dss;

namespace {

struct CSubsetHarness {
    std::shared_ptr<SourceBuffer>        src;
    std::shared_ptr<GrammarSchema const> schema;
    TokenStream                          stream;
};

[[nodiscard]] CSubsetHarness loadAndTokenize(std::string source) {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    EXPECT_TRUE(loaded.has_value());
    auto schema = *loaded;
    auto src    = SourceBuffer::fromString(std::move(source), "<csubset-smoke>");
    Tokenizer tk{src, schema};
    auto [stream, _] = std::move(tk).tokenize();
    return CSubsetHarness{
        .src    = std::move(src),
        .schema = std::move(schema),
        .stream = std::move(stream),
    };
}

[[nodiscard]] bool hasInternalNodeWithRule(Tree const& t,
                                           std::string_view ruleName) {
    if (!t.hasSchema()) return false;
    const auto ruleId = t.schema().rules().find(ruleName);
    if (!ruleId.valid()) return false;
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId id{i};
        if (t.kind(id) == NodeKind::Internal && t.rule(id).v == ruleId.v) return true;
    }
    return false;
}

// Walk the subtree rooted at `root` and assemble a `rule:NAME` /
// `tok:"TEXT"` representation. Used by the precedence-shape test to
// pin the EXACT nesting under the `expression` rule rather than just
// asserting "binaryExpr appears somewhere".
[[nodiscard]] std::string prettyPrintSubtree(Tree const& t, NodeId root) {
    std::string out;
    if (!root.valid()) return out;
    // `TreeCursor::depth()` is depth-from-tree-root; subtract the
    // starting depth so the output is subtree-relative (root at indent 0).
    int baseDepth = -1;
    walkPreOrder(TreeCursor{t, root, CursorMode::Ast},
                 [&](TreeCursor const& c) {
        if (baseDepth < 0) baseDepth = c.depth();
        const int d = c.depth() - baseDepth;
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

[[nodiscard]] NodeId findFirstNodeWithRule(Tree const& t,
                                           std::string_view ruleName) {
    if (!t.hasSchema()) return NodeId{};
    const auto ruleId = t.schema().rules().find(ruleName);
    if (!ruleId.valid()) return NodeId{};
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId id{i};
        if (t.kind(id) == NodeKind::Internal && t.rule(id).v == ruleId.v) {
            return id;
        }
    }
    return NodeId{};
}

} // namespace

// Smoke: parser drives c-subset end-to-end on minimal input. Tree-shape
// pinning lives in PA4 corpus tests; this only confirms the
// AltChoice→RuleLeaf search fallback + optional/repeat nullable-skip
// paths work against the real grammar.
TEST(ParserCSubsetSmoke, IntVarDeclWithLiteralInitializer) {
    auto h = loadAndTokenize("int x = 5;");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(hasInternalNodeWithRule(t, "topLevel"))
        << "tree must include a topLevel frame";
    // FC4 c1: the specifier/declarator split — the head carries the type,
    // the name + init live under the initDeclaratorList.
    EXPECT_TRUE(hasInternalNodeWithRule(t, "topLevelHead"))
        << "tree must include a topLevelHead frame (exercises optional-skip)";
    EXPECT_TRUE(hasInternalNodeWithRule(t, "initDeclarator"))
        << "tree must include an initDeclarator frame";
}

// Closes v2-gap-catalog row 1: parser-driven c-subset, mixed-precedence
// expression produces the precedence-correct (a + (b * c)) shape via
// the Pratt walker rather than the old flat-fold sequence.
TEST(ParserCSubsetSmoke, FunctionBodyExpressionIsPrecedenceCorrect) {
    auto h = loadAndTokenize("int main() { a + b * c; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{}) << "no expression node found in tree";

    const std::string_view expected =
        "rule:expression\n"
        "  rule:binaryExpr\n"
        "    rule:operand\n"
        "      tok:\"a\"\n"
        "    tok:\"+\"\n"
        "    rule:binaryExpr\n"
        "      rule:operand\n"
        "        tok:\"b\"\n"
        "      tok:\"*\"\n"
        "      rule:operand\n"
        "        tok:\"c\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), expected);
}

// `a * b + c` — the tighter `*` chain sits as the LEFT child of `+`.
// Complements the `a + b * c` pin above so BOTH mixed-precedence
// orientations are exact-shape pinned (unchanged by the wrap-in-place
// associativity fix).
TEST(ParserCSubsetSmoke, TightLhsMulThenAddNestsMulOnLeft) {
    auto h = loadAndTokenize("int main() { a * b + c; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});
    const std::string_view expected =
        "rule:expression\n"
        "  rule:binaryExpr\n"
        "    rule:binaryExpr\n"
        "      rule:operand\n"
        "        tok:\"a\"\n"
        "      tok:\"*\"\n"
        "      rule:operand\n"
        "        tok:\"b\"\n"
        "    tok:\"+\"\n"
        "    rule:operand\n"
        "      tok:\"c\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), expected);
}

// `a - b + c` — SAME-precedence chain (c-subset declares `+`/`-` in one
// LEFT group) must nest LEFT: `(a - b) + c`. This is the exact shape
// whose right-recursive mis-nesting silently miscompiled `10 - 3 + 1`
// to 6 (instead of 8) before the wrap-in-place fix.
TEST(ParserCSubsetSmoke, SamePrecSubAddChainNestsLeftward) {
    auto h = loadAndTokenize("int main() { a - b + c; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});
    const std::string_view expected =
        "rule:expression\n"
        "  rule:binaryExpr\n"
        "    rule:binaryExpr\n"
        "      rule:operand\n"
        "        tok:\"a\"\n"
        "      tok:\"-\"\n"
        "      rule:operand\n"
        "        tok:\"b\"\n"
        "    tok:\"+\"\n"
        "    rule:operand\n"
        "      tok:\"c\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), expected);
}

// `a / b / c` — division chain nests LEFT: `(a / b) / c`. (100/5/2 must
// be 10, not 100/(5/2) = 50.)
TEST(ParserCSubsetSmoke, DivisionChainNestsLeftward) {
    auto h = loadAndTokenize("int main() { a / b / c; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});
    const std::string_view expected =
        "rule:expression\n"
        "  rule:binaryExpr\n"
        "    rule:binaryExpr\n"
        "      rule:operand\n"
        "        tok:\"a\"\n"
        "      tok:\"/\"\n"
        "      rule:operand\n"
        "        tok:\"b\"\n"
        "    tok:\"/\"\n"
        "    rule:operand\n"
        "      tok:\"c\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), expected);
}

// `a = b = c` — assignment is declared RIGHT-assoc and must still nest
// RIGHT: `a = (b = c)`. Regression guard for the rhsMin selection
// (right ops keep recursing at their own precedence).
TEST(ParserCSubsetSmoke, AssignmentChainNestsRightward) {
    auto h = loadAndTokenize("int main() { a = b = c; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});
    const std::string_view expected =
        "rule:expression\n"
        "  rule:binaryExpr\n"
        "    rule:operand\n"
        "      tok:\"a\"\n"
        "    tok:\"=\"\n"
        "    rule:binaryExpr\n"
        "      rule:operand\n"
        "        tok:\"b\"\n"
        "      tok:\"=\"\n"
        "      rule:operand\n"
        "        tok:\"c\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), expected);
}

// `a ? b : c ? d : e` — exact right-chained ternary shape: the inner
// ternary is the OUTER's else child (`a ? b : (c ? d : e)`).
TEST(ParserCSubsetSmoke, TernaryChainExactRightNestedShape) {
    auto h = loadAndTokenize("int main() { a ? b : c ? d : e; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});
    const std::string_view expected =
        "rule:expression\n"
        "  rule:ternaryExpr\n"
        "    rule:operand\n"
        "      tok:\"a\"\n"
        "    tok:\"?\"\n"
        "    rule:operand\n"
        "      tok:\"b\"\n"
        "    tok:\":\"\n"
        "    rule:ternaryExpr\n"
        "      rule:operand\n"
        "        tok:\"c\"\n"
        "      tok:\"?\"\n"
        "      rule:operand\n"
        "        tok:\"d\"\n"
        "      tok:\":\"\n"
        "      rule:operand\n"
        "        tok:\"e\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), expected);
}

// `f(a) + g(b)` — postfix-infix mix: both calls wrap their callees,
// the `+` wraps the FIRST call as its left child. Exercises the
// postfix-then-infix climb hand-off (the old design rolled the postfix
// wrap back and replayed it inside the binary frame; wrap-in-place
// adopts it directly).
TEST(ParserCSubsetSmoke, CallPlusCallMixesPostfixAndInfix) {
    auto h = loadAndTokenize("int main() { f(a) + g(b); }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});
    const std::string_view expected =
        "rule:expression\n"
        "  rule:binaryExpr\n"
        "    rule:postfixExpr\n"
        "      rule:operand\n"
        "        tok:\"f\"\n"
        "      tok:\"(\"\n"
        "      rule:argList\n"
        "        rule:assignmentExpr\n"
        "          rule:operand\n"
        "            tok:\"a\"\n"
        "      tok:\")\"\n"
        "    tok:\"+\"\n"
        "    rule:postfixExpr\n"
        "      rule:operand\n"
        "        tok:\"g\"\n"
        "      tok:\"(\"\n"
        "      rule:argList\n"
        "        rule:assignmentExpr\n"
        "          rule:operand\n"
        "            tok:\"b\"\n"
        "      tok:\")\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), expected);
}

// ── trivia hold-then-place pins ─────────────────────────────────────────
//
// A SPACE between the callee and the call opener (`f (40)`) used to
// make the climb's trivia push land in the open frame BEFORE the wrap
// decision, so `wrapLastChildInFrame` wrapped the whitespace leaf
// instead of the callee — the call lowered to garbage HIR (H0001).
// The climb now HOLDS the trivia run, wraps the real subtree, then
// places the trivia inside the wrapper before the operator token.
TEST(ParserCSubsetSmoke, SpacedCallWrapsCalleeNotWhitespace) {
    auto h = loadAndTokenize("int main() { return f (40) + 2; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});
    // AST-mode print (trivia hidden): the callee `f` must be INSIDE the
    // postfixExpr, and the postfixExpr inside the binaryExpr.
    const std::string_view expected =
        "rule:expression\n"
        "  rule:binaryExpr\n"
        "    rule:postfixExpr\n"
        "      rule:operand\n"
        "        tok:\"f\"\n"
        "      tok:\"(\"\n"
        "      rule:argList\n"
        "        rule:assignmentExpr\n"
        "          rule:operand\n"
        "            tok:\"40\"\n"
        "      tok:\")\"\n"
        "    tok:\"+\"\n"
        "    rule:operand\n"
        "      tok:\"2\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), expected);
}

// Comment-as-trivia inside a chain: `a /*x*/ + b` must parse clean and
// the CST leaf stream must reproduce the source byte-for-byte (the
// held trivia run is re-placed without loss or reordering).
TEST(ParserCSubsetSmoke, CommentTriviaInChainKeepsLeafOrder) {
    const std::string source = "int main() { a /*x*/ + b; }";
    auto h = loadAndTokenize(source);
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(hasInternalNodeWithRule(t, "binaryExpr"));

    std::string rebuilt;
    walkPreOrder(TreeCursor{t, t.root(), CursorMode::Cst},
                 [&](TreeCursor const& c) {
        const auto id = c.current();
        if (t.kind(id) != NodeKind::Internal) rebuilt += t.text(id);
    });
    EXPECT_EQ(rebuilt, source);
}

// Spaced ternary: extra spaces around `?` and `:` must not perturb the
// mixfix shape (the ternary arm holds-then-places trivia like infix).
TEST(ParserCSubsetSmoke, SpacedTernaryShapeMatchesCompactForm) {
    auto h = loadAndTokenize("int main() { a  ?  b  :  c ; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});
    const std::string_view expected =
        "rule:expression\n"
        "  rule:ternaryExpr\n"
        "    rule:operand\n"
        "      tok:\"a\"\n"
        "    tok:\"?\"\n"
        "    rule:operand\n"
        "      tok:\"b\"\n"
        "    tok:\":\"\n"
        "    rule:operand\n"
        "      tok:\"c\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), expected);
}

TEST(ParserCSubsetSmoke, TernaryParsesAsMixfix) {
    auto h = loadAndTokenize("int main() { a ? b : c; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});
    const std::string_view expected =
        "rule:expression\n"
        "  rule:ternaryExpr\n"
        "    rule:operand\n"
        "      tok:\"a\"\n"
        "    tok:\"?\"\n"
        "    rule:operand\n"
        "      tok:\"b\"\n"
        "    tok:\":\"\n"
        "    rule:operand\n"
        "      tok:\"c\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), expected);
}

TEST(ParserCSubsetSmoke, TernaryIsRightAssociative) {
    // `a ? b : c ? d : e` → a ? b : (c ? d : e): the else branch nests.
    auto h = loadAndTokenize("int main() { a ? b : c ? d : e; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});
    // The OUTER ternary's else child (last visible) is itself a ternaryExpr.
    const std::string printed = prettyPrintSubtree(t, expr);
    // Two ternaryExpr levels, the inner nested under the outer's else.
    const auto first = printed.find("ternaryExpr");
    const auto second = printed.find("ternaryExpr", first + 1);
    ASSERT_NE(first, std::string::npos);
    ASSERT_NE(second, std::string::npos)
        << "right-assoc must nest a second ternaryExpr in the else branch:\n" << printed;
    // The inner ternary is more deeply indented (nested), confirming it's the else child.
    const auto innerIndent = printed.rfind("\n", second) ;
    EXPECT_GT(second - (innerIndent + 1), 4u) << "inner ternaryExpr should be indented (nested)";
}

TEST(ParserCSubsetSmoke, TernaryBindsLooserThanAssignmentRhs) {
    // `x = a ? b : c` → `x = (a ? b : c)`: ternary (prec 16) binds tighter than
    // the assignment RHS (prec 15), so the `=`'s RHS is the whole ternary.
    auto h = loadAndTokenize("int main() { x = a ? b : c; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    const NodeId assign = findFirstNodeWithRule(t, "binaryExpr");
    ASSERT_NE(assign, NodeId{});
    const std::string printed = prettyPrintSubtree(t, assign);
    // The binaryExpr (`=`) must contain a nested ternaryExpr (its RHS).
    EXPECT_NE(printed.find("tok:\"=\""), std::string::npos);
    EXPECT_NE(printed.find("ternaryExpr"), std::string::npos)
        << "the `=` RHS must be the ternary:\n" << printed;
}

TEST(ParserCSubsetSmoke, TernaryAsCallArgument) {
    // `f(a ? b : c)` — ternary nested as an operand (call arg) exercises the
    // climb re-entry inside the argList body.
    auto h = loadAndTokenize("int main() { f(a ? b : c); }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    EXPECT_FALSE(result.tree.diagnostics().hasErrors());
    EXPECT_NE(findFirstNodeWithRule(result.tree, "ternaryExpr"), NodeId{});
}

TEST(ParserCSubsetSmoke, TernaryMissingColonRecovers) {
    // `a ? b ;` — missing `:` separator. The walker emits P_MissingRequiredChild
    // + an Error leaf (HasError on root), and recovers without hanging.
    auto h = loadAndTokenize("int main() { a ? b ; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_TRUE(t.diagnostics().hasErrors());
    bool sawMissing = false;
    for (auto const& d : t.diagnostics().all())
        if (d.code == DiagnosticCode::P_MissingRequiredChild) { sawMissing = true; break; }
    EXPECT_TRUE(sawMissing) << "missing ':' must emit P_MissingRequiredChild";
}

TEST(ParserCSubsetSmoke, FunctionCallParsesAsPostfix) {
    auto h = loadAndTokenize("int main() { f(a, b); }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});
    constexpr std::string_view kExpected =
        "rule:expression\n"
        "  rule:postfixExpr\n"
        "    rule:operand\n"
        "      tok:\"f\"\n"
        "    tok:\"(\"\n"
        "    rule:argList\n"
        "      rule:assignmentExpr\n"
        "        rule:operand\n"
        "          tok:\"a\"\n"
        "      tok:\",\"\n"
        "      rule:assignmentExpr\n"
        "        rule:operand\n"
        "          tok:\"b\"\n"
        "    tok:\")\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), kExpected);
}

TEST(ParserCSubsetSmoke, EmptyArgumentCallParsesAsPostfix) {
    auto h = loadAndTokenize("int main() { f(); }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "zero-arg calls must parse cleanly (argList nullable)";

    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});
    constexpr std::string_view kExpected =
        "rule:expression\n"
        "  rule:postfixExpr\n"
        "    rule:operand\n"
        "      tok:\"f\"\n"
        "    tok:\"(\"\n"
        "    rule:argList\n"
        "    tok:\")\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), kExpected);
}

TEST(ParserCSubsetSmoke, ArrayIndexParsesAsPostfix) {
    auto h = loadAndTokenize("int main() { a[0]; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});
    constexpr std::string_view kExpected =
        "rule:expression\n"
        "  rule:postfixExpr\n"
        "    rule:operand\n"
        "      tok:\"a\"\n"
        "    tok:\"[\"\n"
        "    rule:expression\n"
        "      rule:operand\n"
        "        tok:\"0\"\n"
        "    tok:\"]\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), kExpected);
}

// The `[`-postfix declares `bodyRule: "expression"`. `expression` is
// itself an expr-rule, so the walker must route the body through
// `prattWalker->walkExpression` to engage operator climbing. Without
// that routing, `i + j * k` would parse as a flat sequence and there
// would be no `binaryExpr` for the nested `j * k`.
TEST(ParserCSubsetSmoke, ArrayIndexBodyClimbsPrecedence) {
    auto h = loadAndTokenize("int main() { a[i + j * k]; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});
    // The nested `binaryExpr[i, +, binaryExpr[j, *, k]]` shape is the
    // visible signal that operator climb ran inside the brackets —
    // `*` (prec 70) binds tighter than `+` (prec 65), so `j * k`
    // nests under the `+` RHS (precedence nesting, not associativity).
    constexpr std::string_view kExpected =
        "rule:expression\n"
        "  rule:postfixExpr\n"
        "    rule:operand\n"
        "      tok:\"a\"\n"
        "    tok:\"[\"\n"
        "    rule:expression\n"
        "      rule:binaryExpr\n"
        "        rule:operand\n"
        "          tok:\"i\"\n"
        "        tok:\"+\"\n"
        "        rule:binaryExpr\n"
        "          rule:operand\n"
        "            tok:\"j\"\n"
        "          tok:\"*\"\n"
        "          rule:operand\n"
        "            tok:\"k\"\n"
        "    tok:\"]\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), kExpected);
}

// `f(a;` — opener consumed, body parses, closer missing. Walker emits
// `P_MissingRequiredChild`, closes the wrap, and returns control to
// the parent dispatch so the `;` can still terminate the statement.
TEST(ParserCSubsetSmoke, MissingCloserEmitsRecoveryDiag) {
    auto h = loadAndTokenize("int main() { f(a; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_TRUE(t.diagnostics().hasErrors());
    std::size_t missing = 0;
    for (auto const& d : t.diagnostics().all()) {
        if (d.code == DiagnosticCode::P_MissingRequiredChild) ++missing;
    }
    EXPECT_GE(missing, 1u);
    EXPECT_TRUE(hasError(t.flags(t.root())));
}

TEST(ParserCSubsetSmoke, PostfixIncParsesAsPostfix) {
    auto h = loadAndTokenize("int main() { i++; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});
    constexpr std::string_view kExpected =
        "rule:expression\n"
        "  rule:postfixExpr\n"
        "    rule:operand\n"
        "      tok:\"i\"\n"
        "    tok:\"++\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), kExpected);
}

// Prefix `*` — dereference. Walker disambiguates from infix `*` by
// position (operator-table arity).
TEST(ParserCSubsetSmoke, PrefixDerefParsesAsUnary) {
    auto h = loadAndTokenize("int main() { *p; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});
    constexpr std::string_view kExpected =
        "rule:expression\n"
        "  rule:unaryExpr\n"
        "    tok:\"*\"\n"
        "    rule:operand\n"
        "      tok:\"p\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), kExpected);
}

// Compound assignment: `+=` and the other compound-assign operators
// are right-associative at precedence 15 alongside `=`. This test pins
// the common case (`+=`); `ShlCompoundAssignmentRespectsLongestMatch`
// below covers the longest-match boundary (`<<=` must NOT lex as
// `<< =`); `CompoundAssignmentIsRightAssociative` pins the right-assoc
// nested-binaryExpr shape that distinguishes right from left assoc.
TEST(ParserCSubsetSmoke, CompoundAssignmentParsesAsBinaryExpr) {
    auto h = loadAndTokenize("int main() { x += 1; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});
    constexpr std::string_view kExpected =
        "rule:expression\n"
        "  rule:binaryExpr\n"
        "    rule:operand\n"
        "      tok:\"x\"\n"
        "    tok:\"+=\"\n"
        "    rule:operand\n"
        "      tok:\"1\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), kExpected);
}

// `<<=` is the most longest-match-pressured compound-assign — the
// tokenizer must prefer it over `<<` followed by `=`. A regression in
// longest-match would silently lex `x <<= 1` as `x << = 1` (parses
// with an error). Pin the 3-char op as a single binaryExpr.
TEST(ParserCSubsetSmoke, ShlCompoundAssignmentRespectsLongestMatch) {
    auto h = loadAndTokenize("int main() { x <<= 1; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "longest-match must prefer <<= over << followed by =";

    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});
    constexpr std::string_view kExpected =
        "rule:expression\n"
        "  rule:binaryExpr\n"
        "    rule:operand\n"
        "      tok:\"x\"\n"
        "    tok:\"<<=\"\n"
        "    rule:operand\n"
        "      tok:\"1\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), kExpected);
}

// Right-associativity pin: `x += y += z` must parse as
// `x += (y += z)`, NOT `(x += y) += z`. The right-assoc declaration in
// the operator group is the only thing distinguishing the shape; a
// regression that flips the JSON to `"left"` would parse `x += 1`
// identically but produce a different nested shape here.
TEST(ParserCSubsetSmoke, CompoundAssignmentIsRightAssociative) {
    auto h = loadAndTokenize("int main() { x += y += z; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});
    constexpr std::string_view kExpected =
        "rule:expression\n"
        "  rule:binaryExpr\n"
        "    rule:operand\n"
        "      tok:\"x\"\n"
        "    tok:\"+=\"\n"
        "    rule:binaryExpr\n"
        "      rule:operand\n"
        "        tok:\"y\"\n"
        "      tok:\"+=\"\n"
        "      rule:operand\n"
        "        tok:\"z\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), kExpected);
}

// `extern` declares a function prototype or variable without a body.
// Two forms: function (paren-paramlist + `;`) and variable
// (optional-init + `;`). Function form is pinned via full
// `prettyPrintSubtree`; variable form is pinned via shape presence
// (the externTail's tail-only difference is what matters for that
// arm; the externDecl frame shape is already covered by the function
// form). Broken-path coverage in `test_parser_recovery.cpp`.
TEST(ParserCSubsetSmoke, ExternFunctionPrototypeParses) {
    auto h = loadAndTokenize("extern int printf(char x);");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(hasInternalNodeWithRule(t, "externDecl"));

    const NodeId ext = findFirstNodeWithRule(t, "externDecl");
    ASSERT_NE(ext, NodeId{});
    // FC4 c1: `param` is now declarator-shaped — `declHeadForParam` carries
    // the base type and the (optional) `declarator` carries the name. The
    // externDecl spine itself stays legacy (typeRef + Identifier).
    constexpr std::string_view kExpected =
        "rule:externDecl\n"
        "  tok:\"extern\"\n"
        "  rule:typeRef\n"
        "    rule:typeBase\n"
        "      rule:typeSpecifierSeq\n"
        "        tok:\"int\"\n"
        "  tok:\"printf\"\n"
        "  rule:externTail\n"
        "    rule:externFuncTail\n"
        "      tok:\"(\"\n"
        "      rule:paramList\n"
        "        rule:param\n"
        "          rule:declHeadForParam\n"
        "            rule:typeSpecifierSeq\n"
        "              tok:\"char\"\n"
        "          rule:declarator\n"
        "            rule:directDeclarator\n"
        "              tok:\"x\"\n"
        "      tok:\")\"\n"
        "      tok:\";\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, ext), kExpected);
}

TEST(ParserCSubsetSmoke, ExternVariableDeclParses) {
    auto h = loadAndTokenize("extern int errno;");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(hasInternalNodeWithRule(t, "externDecl"));
}

// Array declarator: expression-side `a[0]` is pinned by
// `ArrayIndexParsesAsPostfix`; this trio is the declarator-side
// complement (v2-gap-catalog row 12). The trio together exercises
// both `varDeclTail` (top-level) and `varDeclHead` (local) array-
// suffix attachment points, plus the empty-suffix path `int x[];`.
TEST(ParserCSubsetSmoke, TopLevelArrayDeclParses) {
    auto h = loadAndTokenize("int a[10];");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId tl = findFirstNodeWithRule(t, "topLevel");
    ASSERT_NE(tl, NodeId{});
    // FC4 c1: the specifier/declarator split — the array suffix now lives
    // INSIDE the directDeclarator (C 6.7.6), and the `;` is the
    // topLevelDeclTail's EndStatement arm (vs `{` = function definition).
    constexpr std::string_view kExpected =
        "rule:topLevel\n"
        "  rule:topLevelDecl\n"
        "    rule:topLevelHead\n"
        "      rule:typeSpecifierSeq\n"
        "        tok:\"int\"\n"
        "    rule:initDeclaratorList\n"
        "      rule:initDeclarator\n"
        "        rule:declarator\n"
        "          rule:directDeclarator\n"
        "            tok:\"a\"\n"
        "            rule:arrayDeclSuffix\n"
        "              tok:\"[\"\n"
        "              rule:expression\n"
        "                rule:operand\n"
        "                  tok:\"10\"\n"
        "              tok:\"]\"\n"
        "    rule:topLevelDeclTail\n"
        "      tok:\";\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, tl), kExpected);
}

TEST(ParserCSubsetSmoke, EmptyArrayDeclSuffixParses) {
    // `int x[];` — empty bracket pair. The size expression in
    // `arrayDeclSuffix` is `optional`, so the brackets can be empty.
    auto h = loadAndTokenize("int x[];");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId suffix = findFirstNodeWithRule(t, "arrayDeclSuffix");
    ASSERT_NE(suffix, NodeId{});
    constexpr std::string_view kExpected =
        "rule:arrayDeclSuffix\n"
        "  tok:\"[\"\n"
        "  tok:\"]\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, suffix), kExpected);
}

TEST(ParserCSubsetSmoke, InnerArrayDeclParses) {
    auto h = loadAndTokenize("int main() { int buf[64]; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId head = findFirstNodeWithRule(t, "varDecl");
    ASSERT_NE(head, NodeId{});
    // FC4 c1: the local declaration statement is `varDecl` — keyword-led
    // head (kwDeclHead) + initDeclaratorList; the array suffix lives
    // INSIDE the directDeclarator (C 6.7.6).
    constexpr std::string_view kExpected =
        "rule:varDecl\n"
        "  rule:kwDeclHead\n"
        "    rule:typeSpecifierForDecl\n"
        "      rule:typeSpecifierSeq\n"
        "        tok:\"int\"\n"
        "  rule:initDeclaratorList\n"
        "    rule:initDeclarator\n"
        "      rule:declarator\n"
        "        rule:directDeclarator\n"
        "          tok:\"buf\"\n"
        "          rule:arrayDeclSuffix\n"
        "            tok:\"[\"\n"
        "            rule:expression\n"
        "              rule:operand\n"
        "                tok:\"64\"\n"
        "            tok:\"]\"\n"
        "  tok:\";\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, head), kExpected);
}

TEST(ParserCSubsetSmoke, ArrayDeclWithInitializerExpressionParses) {
    // Size expression can use earlier-declared constants. The pin
    // here captures the nested `binaryExpr` for `n * 2` inside the
    // arrayDeclSuffix — proving operator climb engages inside the
    // size expression (and that the binaryExpr is positioned
    // INSIDE arrayDeclSuffix, not somewhere else in the tree).
    auto h = loadAndTokenize("int main() { int buf[n * 2]; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId suffix = findFirstNodeWithRule(t, "arrayDeclSuffix");
    ASSERT_NE(suffix, NodeId{});
    constexpr std::string_view kExpected =
        "rule:arrayDeclSuffix\n"
        "  tok:\"[\"\n"
        "  rule:expression\n"
        "    rule:binaryExpr\n"
        "      rule:operand\n"
        "        tok:\"n\"\n"
        "      tok:\"*\"\n"
        "      rule:operand\n"
        "        tok:\"2\"\n"
        "  tok:\"]\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, suffix), kExpected);
}

// Postfix chains nest left: `f(a)[i]` binds as `(f(a))[i]` — the `[i]`
// postfix wraps the result of `f(a)`. Every climb arm uses
// `wrapLastChildExprFrame`, so the previous wrap becomes the first
// child of the next wrap directly, with no intermediate `operand`
// layer.
TEST(ParserCSubsetSmoke, PostfixChainNestsLeftToRight) {
    auto h = loadAndTokenize("int main() { f(a)[i]; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});
    constexpr std::string_view kExpected =
        "rule:expression\n"
        "  rule:postfixExpr\n"
        "    rule:postfixExpr\n"
        "      rule:operand\n"
        "        tok:\"f\"\n"
        "      tok:\"(\"\n"
        "      rule:argList\n"
        "        rule:assignmentExpr\n"
        "          rule:operand\n"
        "            tok:\"a\"\n"
        "      tok:\")\"\n"
        "    tok:\"[\"\n"
        "    rule:expression\n"
        "      rule:operand\n"
        "        tok:\"i\"\n"
        "    tok:\"]\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), kExpected);
}

// Postfix followed by infix at the same level: `f(a) + g(b)`. The
// outer infix must roll back through the postfix wrap (postfix
// intentionally does NOT advance the snap) so the binaryExpr frame
// rebuilds the LHS chain via its recursive `parseExpressionAt` at
// `prec + 1`. Without that invariant the postfix wraps would land as
// siblings of the binaryExpr instead of children — fib's
// `return fib(n-1) + fib(n-2);` was the original reproducer.
TEST(ParserCSubsetSmoke, PostfixCallThenInfixBindsCorrectly) {
    auto h = loadAndTokenize("int main() { return f(a) + g(b); }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    // Pin the binaryExpr's full subtree shape. A regression where the
    // two postfix wraps end up as siblings of the binaryExpr (rather
    // than its LHS / RHS children) would produce a postfixExpr count
    // of 2 but with a wholly different tree — caught here.
    const NodeId bin = findFirstNodeWithRule(t, "binaryExpr");
    ASSERT_NE(bin, NodeId{});
    constexpr std::string_view kExpected =
        "rule:binaryExpr\n"
        "  rule:postfixExpr\n"
        "    rule:operand\n"
        "      tok:\"f\"\n"
        "    tok:\"(\"\n"
        "    rule:argList\n"
        "      rule:assignmentExpr\n"
        "        rule:operand\n"
        "          tok:\"a\"\n"
        "    tok:\")\"\n"
        "  tok:\"+\"\n"
        "  rule:postfixExpr\n"
        "    rule:operand\n"
        "      tok:\"g\"\n"
        "    tok:\"(\"\n"
        "    rule:argList\n"
        "      rule:assignmentExpr\n"
        "        rule:operand\n"
        "          tok:\"b\"\n"
        "    tok:\")\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, bin), kExpected);
}

// Three-deep chain: `a[i][j][k]` produces three nested postfixExpr
// frames. Full subtree pin — a count of 3 alone would pass a
// regression where the wraps land as siblings.
TEST(ParserCSubsetSmoke, ThreeDeepArrayIndexChainNests) {
    auto h = loadAndTokenize("int main() { a[i][j][k]; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});
    constexpr std::string_view kExpected =
        "rule:expression\n"
        "  rule:postfixExpr\n"
        "    rule:postfixExpr\n"
        "      rule:postfixExpr\n"
        "        rule:operand\n"
        "          tok:\"a\"\n"
        "        tok:\"[\"\n"
        "        rule:expression\n"
        "          rule:operand\n"
        "            tok:\"i\"\n"
        "        tok:\"]\"\n"
        "      tok:\"[\"\n"
        "      rule:expression\n"
        "        rule:operand\n"
        "          tok:\"j\"\n"
        "      tok:\"]\"\n"
        "    tok:\"[\"\n"
        "    rule:expression\n"
        "      rule:operand\n"
        "        tok:\"k\"\n"
        "    tok:\"]\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), kExpected);
}

// Function-call chain: `f()(g)` — two calls in a row, where the
// result of `f()` is called with `g`. Empty-args followed by
// arg-bearing call. Full subtree pin.
TEST(ParserCSubsetSmoke, FunctionCallChainNests) {
    auto h = loadAndTokenize("int main() { f()(g); }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});
    constexpr std::string_view kExpected =
        "rule:expression\n"
        "  rule:postfixExpr\n"
        "    rule:postfixExpr\n"
        "      rule:operand\n"
        "        tok:\"f\"\n"
        "      tok:\"(\"\n"
        "      rule:argList\n"
        "      tok:\")\"\n"
        "    tok:\"(\"\n"
        "    rule:argList\n"
        "      rule:assignmentExpr\n"
        "        rule:operand\n"
        "          tok:\"g\"\n"
        "    tok:\")\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), kExpected);
}

// Paren-wrapped chain: `(f(a)[i])` — the chain lives inside an
// `operand`'s `( expression )` branch which opens a fresh
// `expression` frame. The "snap stays valid across postfix iters"
// invariant lives per-`parseExpressionAt` call, so the inner
// expression frame's chain must bind to itself, not leak to the
// outer expression.
TEST(ParserCSubsetSmoke, ParenWrappedPostfixChainNests) {
    auto h = loadAndTokenize("int main() { (f(a)[i]); }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;
    EXPECT_FALSE(t.diagnostics().hasErrors());

    // The outer `expression` contains an `operand` whose body is the
    // named `parenExpr` rule (extracted from the prior anonymous
    // `(expression)` sequence when c-subset's `operand` became
    // `speculative: true` for D5.3 — speculative-alt rule-branches
    // must be named so `candidateBranches` enumerates them). The
    // inner `expression` (under `parenExpr`) contains the chained
    // postfixExpr structure.
    const NodeId outerExpr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(outerExpr, NodeId{});
    constexpr std::string_view kExpected =
        "rule:expression\n"
        "  rule:operand\n"
        "    rule:parenExpr\n"
        "      tok:\"(\"\n"
        "      rule:expression\n"
        "        rule:postfixExpr\n"
        "          rule:postfixExpr\n"
        "            rule:operand\n"
        "              tok:\"f\"\n"
        "            tok:\"(\"\n"
        "            rule:argList\n"
        "              rule:assignmentExpr\n"
        "                rule:operand\n"
        "                  tok:\"a\"\n"
        "            tok:\")\"\n"
        "          tok:\"[\"\n"
        "          rule:expression\n"
        "            rule:operand\n"
        "              tok:\"i\"\n"
        "          tok:\"]\"\n"
        "      tok:\")\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, outerExpr), kExpected);
}

// Broken chain: `f(a)[i` — the second postfix's closer `]` is
// missing AND the statement-terminating `;` doesn't appear. The
// walker must emit a diagnostic (`P_MissingRequiredChild`),
// propagate `HasError` to root, and not hang or stack-overflow.
TEST(ParserCSubsetSmoke, BrokenPostfixChainEmitsDiagnostic) {
    auto h = loadAndTokenize("int main() { f(a)[i }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_TRUE(t.diagnostics().hasErrors());
    EXPECT_TRUE(hasError(t.flags(t.root())));
    std::size_t missing = 0;
    for (auto const& d : t.diagnostics().all()) {
        if (d.code == DiagnosticCode::P_MissingRequiredChild) ++missing;
    }
    EXPECT_GE(missing, 1u);
}

// Mixed chain: `*p[i]++` — left-recursive postfix chain interacts
// with prefix `*` (lower precedence). C semantics: `*( (p[i])++ )`.
// The prefix's recursive `parseExpressionAt(prefixPrec)` consumes
// the full chain.
TEST(ParserCSubsetSmoke, PrefixOverPostfixChainNests) {
    auto h = loadAndTokenize("int main() { *p[i]++; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;
    EXPECT_FALSE(t.diagnostics().hasErrors());

    // Tree: expression → unaryExpr[*, postfixExpr[postfixExpr[p, [, i, ]], ++]]
    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});
    constexpr std::string_view kExpected =
        "rule:expression\n"
        "  rule:unaryExpr\n"
        "    tok:\"*\"\n"
        "    rule:postfixExpr\n"
        "      rule:postfixExpr\n"
        "        rule:operand\n"
        "          tok:\"p\"\n"
        "        tok:\"[\"\n"
        "        rule:expression\n"
        "          rule:operand\n"
        "            tok:\"i\"\n"
        "        tok:\"]\"\n"
        "      tok:\"++\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), kExpected);
}

// Parenthesized sub-expressions delegate through `operand`'s `( expression )`
// branch, which re-triggers the dispatch loop's expr-rule hook → recursive
// `walkExpression`. The inner walker manages its own snapshot/rollback
// stack independent of the outer.
TEST(ParserCSubsetSmoke, ParenGroupingForcesOuterPrecedence) {
    auto h = loadAndTokenize("int main() { (a + b) * c; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId expr = findFirstNodeWithRule(t, "expression");
    ASSERT_NE(expr, NodeId{});

    // Outer is `* c`; LHS of `*` is the parenthesized `(a + b)` which
    // descends through `operand → parenExpr → ( expression ) →
    // binaryExpr[a,+,b]`. (`parenExpr` is the named rule the operand
    // alt routes to under `speculative: true` — the engine's rule-
    // branch enumeration requires named rules, not inline sequences.)
    const std::string_view expected =
        "rule:expression\n"
        "  rule:binaryExpr\n"
        "    rule:operand\n"
        "      rule:parenExpr\n"
        "        tok:\"(\"\n"
        "        rule:expression\n"
        "          rule:binaryExpr\n"
        "            rule:operand\n"
        "              tok:\"a\"\n"
        "            tok:\"+\"\n"
        "            rule:operand\n"
        "              tok:\"b\"\n"
        "        tok:\")\"\n"
        "    tok:\"*\"\n"
        "    rule:operand\n"
        "      tok:\"c\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), expected);
}

// F2: c-subset reintroduces CharLiteral via a `'`-opened body mode
// (mirrors the `"`-opened string mode). `'a'` parses cleanly: the
// `'` opener token, one `CharLiteral` body byte, the `'` closer
// (same token kind back-popping the body mode). Pin via a tree-
// presence assertion on the body-mode CharLiteral token kind.
TEST(ParserCSubsetSmoke, CharLiteralParsesAsOperand) {
    auto harness = loadAndTokenize("int main() { return 'a'; }");
    Parser parser{harness.src, harness.schema, std::move(harness.stream)};
    auto const result = std::move(parser).parse();
    auto const& tree = result.tree;
    EXPECT_FALSE(tree.diagnostics().hasErrors())
        << "c-subset failed to parse CharLiteral cleanly";
    // The body-mode emits a CharLiteral schema-token inside the
    // `'a'` span; verify the token kind appears in the leaf stream.
    const auto charLitId = tree.schema().schemaTokens().find("CharLiteral");
    ASSERT_TRUE(charLitId.valid());
    bool sawCharLiteralToken = false;
    for (std::uint32_t i = 1; i < tree.nodeCount(); ++i) {
        const NodeId id{i};
        if (tree.kind(id) == NodeKind::Token
            && tree.tokenKind(id).v == charLitId.v) {
            sawCharLiteralToken = true;
            break;
        }
    }
    EXPECT_TRUE(sawCharLiteralToken)
        << "expected at least one CharLiteral body-token in the parse tree";
}

// ── FC4 c1 stage 2b: the decl-vs-expr TRIAGE matrix (parser tier) ───────
//
// The declOrExprStmt ambiguity site probes identVarDecl FIRST (declared
// order — the FC4 c0 fix) under the PreferType commit guard. These pins
// fix the STRUCTURAL outcome per triage row; the semantic-tier mirrors
// (symbol typing / undeclared positioning) live in
// test_semantic_analyzer_c_subset.cpp.

// Sketch-KNOWN Type: `MyP * p;` after `typedef int MyP;` COMMITS the
// declaration reading — an identVarDecl node exists, no binaryExpr
// multiplication wraps `MyP * p`.
TEST(ParserCSubsetSmoke, TypedefNameStarCommitsDeclarationStatement) {
    auto h = loadAndTokenize(
        "typedef int MyP;\n"
        "int main() { MyP * p; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto const result = std::move(p).parse();
    auto const& t = result.tree;
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(hasInternalNodeWithRule(t, "identVarDecl"))
        << "MyP * p; must parse as a DECLARATION statement";
}

// Sketch-KNOWN Value: `a * b;` (both locals) ROLLS BACK to the
// expression statement — NO identVarDecl; the `*` is the binary
// multiplication.
TEST(ParserCSubsetSmoke, ValueStarValueRollsBackToExpression) {
    auto h = loadAndTokenize("int main() { int a; int b; a * b; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto const result = std::move(p).parse();
    auto const& t = result.tree;
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_FALSE(hasInternalNodeWithRule(t, "identVarDecl"))
        << "a * b; with VALUE operands must stay an expression statement";
    EXPECT_TRUE(hasInternalNodeWithRule(t, "binaryExpr"))
        << "the multiplication must materialize as a binaryExpr";
}

// UNKNOWN lone identifier: `u * v;` with NO `u` anywhere — the
// follower-operator test sees `*` (continues a value reading) and ROLLS
// BACK; single-file compile keeps the expression reading (the
// cross-file oracle candidate is recorded for the CU reparse, which a
// single-file unit never seeds). Structure pin only — the TWO
// positioned S_UndeclaredIdentifier are the semantic-tier mirror.
TEST(ParserCSubsetSmoke, UnknownStarUnknownRollsBackToExpression) {
    auto h = loadAndTokenize("int main() { u * v; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto const result = std::move(p).parse();
    auto const& t = result.tree;
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_FALSE(hasInternalNodeWithRule(t, "identVarDecl"))
        << "unknown-led `u * v;` must stay an expression statement";
}

// C23 `auto x = 1;` type INFERENCE is NOT supported — `auto` is a
// storage-class specifier only, so the head consumes `x` as the type
// name and the missing declarator fails LOUD at the `=`:
// P_NoAlternativeMatched, positioned. Layout:
//   "int main() { auto x = 1; }"
//    0123456789012345678901234
// the `=` sits at offset 20. Pinned residue:
// D-CSUBSET-C23-AUTO-INFERENCE (registry).
TEST(ParserCSubsetSmoke, AutoInferenceFormIsALoudParseError) {
    auto h = loadAndTokenize("int main() { auto x = 1; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto const result = std::move(p).parse();
    auto const& t = result.tree;
    EXPECT_TRUE(t.diagnostics().hasErrors())
        << "C23 auto-inference must fail LOUD (never silently mistype)";
    bool sawPositionedMiss = false;
    for (auto const& d : t.diagnostics().all()) {
        if (d.code == DiagnosticCode::P_NoAlternativeMatched
            && d.span.start() == 20u) {
            sawPositionedMiss = true;
        }
    }
    EXPECT_TRUE(sawPositionedMiss)
        << "expected P_NoAlternativeMatched at the `=` (offset 20)";
}

// ── FC4 c1 stage 2b: the speculative-probe BUDGET guard ─────────────────
//
// An identifier-led declaration (typedef'd head -> rides the
// declOrExprStmt SPECULATIVE path, lookahead 256 = 4096-token probe
// budget) whose initializer is LONG: a 600-argument call is ~1205
// tokens. That EXCEEDS the operand alt's 1024-token budget (lookahead
// 64 — the OLD assumption a statement-probe regression would fall back
// to) — proving the OUTER statement probe's budget governs the whole
// `MyT x = <expr> ;` swallow — while staying inside 4096.
//
// Shape note: the initializer is WIDE (many flat call arguments), not a
// long `1 + 1 + ...` binary CHAIN — a left-assoc chain now builds
// ITERATIVELY (wrap-in-place; it no longer counts against
// ParserConfig::maxExpressionDepth), so a wide argument list is the
// shape that stresses the TOKEN budget specifically: each call argument
// parses as a fresh shallow expression, keeping depth ~constant while
// the token count grows.
TEST(ParserCSubsetSmoke, LongInitializerRidesTheStatementProbeBudget) {
    std::string src = "typedef int MyT;\nint main() { MyT x = f(1";
    for (int i = 0; i < 599; ++i) src += ", 1";
    src += "); return x; }";
    auto h = loadAndTokenize(std::move(src));
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto const result = std::move(p).parse();
    auto const& t = result.tree;
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "a 600-argument initializer (~1205 tokens, past the 1024-token "
           "operand budget) must parse clean under the 4096-token "
           "statement probe budget";
    EXPECT_TRUE(hasInternalNodeWithRule(t, "identVarDecl"))
        << "the long-initializer statement must still commit as a "
           "DECLARATION";
}
