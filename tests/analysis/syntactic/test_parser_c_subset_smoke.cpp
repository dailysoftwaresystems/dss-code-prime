#include "analysis/syntactic/parser.hpp"
#include "core/substrate/large_stack_call.hpp"
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

#include <cstddef>
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
// rebuilds the LHS chain through the iterative exprWorkStack driver at
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
// invariant lives per expression-descent on the exprWorkStack driver,
// so the inner expression frame's chain must bind to itself, not leak
// to the outer expression.
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
// The prefix's operand descent at `prefixPrec` (now an iterative
// exprWorkStack push, not host recursion) consumes the full chain.
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

// ── c25 REPRODUCTION + regression pin: the struct-body speculation budget ──
//
// A struct/union/enum BODY specifier is parsed INSIDE the speculative probe
// that disambiguates body-vs-ref (typedefHead / typeSpecifierForDecl /
// topLevelCompositeSpec / typeBaseAllowingStruct — lookahead 256 = 4096-token
// probe budget). The body-vs-ref choice is settled the instant a `{` appears,
// yet the probe keeps speculating through the WHOLE member list, so a struct
// whose body exceeds 4096 tokens trips exceededBudget() → the body-form alt
// fails → the parser mis-recovers to the matching `}` → P0009 at the orphan
// `};`. This is `struct sqlite3` (sqlite3.c:18907) in minimal form. It is NOT
// a member-COUNT limit (a 130-member control parses); it is a TOKEN-budget
// cliff that any large mixed struct (real SQLite) reaches at ~80 members.
TEST(ParserCSubsetSmoke, LargeStructBodyMustNotHitSpeculationBudget) {
    auto structOf = [](int n) {
        std::string s = "struct S {";
        for (int i = 0; i < n; ++i) s += "int a" + std::to_string(i) + ";";
        return s + "};";
    };
    // Control: 130 members (~390 body tokens) — far under the 4096 budget.
    {
        auto h = loadAndTokenize(structOf(130));
        Parser p{h.src, h.schema, std::move(h.stream)};
        auto const r = std::move(p).parse();
        EXPECT_FALSE(r.tree.diagnostics().hasErrors())
            << "130-member struct (control) must parse clean";
    }
    // Regression: 1500 members (~4500 body tokens) — exceeds the 4096 probe
    // budget. RED pre-c25 (P0009 at the `};`); GREEN once the large struct
    // body parse is no longer governed by the body-vs-ref speculation budget.
    {
        auto h = loadAndTokenize(structOf(1500));
        Parser p{h.src, h.schema, std::move(h.stream)};
        auto const r = std::move(p).parse();
        EXPECT_FALSE(r.tree.diagnostics().hasErrors())
            << "1500-member struct must parse clean — the body-vs-ref "
               "speculation must not budget-cap the member list (c25)";
    }
}

// c25: the SAME budget cliff for UNION and ENUM bodies — proves the
// unification (and thus the non-speculative direct descent) covers all
// three composites, not just struct. RED-on-disable for the union/enum
// arms: revert `unionSpec`/`enumSpec` back to the speculative
// `unionSpecifierBody | unionTypeRef` pair and the 1500-member body
// budget-caps → P0009 at the `};`.
TEST(ParserCSubsetSmoke, LargeUnionBodyMustNotHitSpeculationBudget) {
    std::string s = "union U {";
    for (int i = 0; i < 1500; ++i) s += "int a" + std::to_string(i) + ";";
    s += "};";
    auto h = loadAndTokenize(std::move(s));
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto const r = std::move(p).parse();
    EXPECT_FALSE(r.tree.diagnostics().hasErrors())
        << "1500-member union must parse clean (c25 unified unionSpec)";
}

TEST(ParserCSubsetSmoke, LargeEnumBodyMustNotHitSpeculationBudget) {
    std::string s = "enum E {";
    for (int i = 0; i < 1500; ++i) s += "A" + std::to_string(i) + ",";
    s += "};";
    auto h = loadAndTokenize(std::move(s));
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto const r = std::move(p).parse();
    EXPECT_FALSE(r.tree.diagnostics().hasErrors())
        << "1500-enumerator enum must parse clean (c25 unified enumSpec)";
}

// c25: a MIXED large struct closer to real sqlite3 — nested anonymous
// struct + nested anonymous union, a function-pointer member, a bit-field,
// an array member, and a multi-declarator member — REPEATED past the old
// 4096-token probe budget. Parses clean (no parse diagnostics). The mix
// exercises the RECURSIVE composite path (each nested body is itself a
// non-speculative direct descent) at scale, which the flat repro does not.
TEST(ParserCSubsetSmoke, MixedLargeStructBodyParsesCleanPastOldBudget) {
    std::string s = "struct Big {";
    // ~12 tokens per iteration; 500 iterations ≈ 6000 tokens, past 4096.
    for (int i = 0; i < 500; ++i) {
        std::string n = std::to_string(i);
        s += "struct { int sa" + n + "; } sx" + n + ";";   // nested anon struct
        s += "union { int ua" + n + "; long ub" + n + "; } ux" + n + ";"; // nested anon union
        s += "int (*fp" + n + ")(int);";                   // fn-pointer member
        s += "unsigned bf" + n + " : 3;";                  // bit-field
        s += "int arr" + n + "[4];";                       // array member
        s += "int ma" + n + ", mb" + n + ";";              // multi-declarator
    }
    s += "};";
    auto h = loadAndTokenize(std::move(s));
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto const r = std::move(p).parse();
    EXPECT_FALSE(r.tree.diagnostics().hasErrors())
        << "a large mixed struct (nested aggregates, fn-ptr, bitfield, "
           "array, multi-declarator) past the old budget must parse clean";
}

// c25 PARSER-SHAPE pin (the define-vs-reference structural discriminator):
// `struct S { … }` produces a `structSpec` node that HAS a `structBody`
// child; `struct S` (a bare reference, here as a pointer-param type) produces
// a `structSpec` node with NO `structBody` child. This is the exact shape the
// dual-mode binder keys on (`definesWhenChild: structBody`). RED-on-disable:
// if the grammar stopped factoring the body into `structBody`, the
// has-body/lacks-body assertions flip.
TEST(ParserCSubsetSmoke, StructSpecBodyChildPresenceDiscriminatesDefineVsRef) {
    // DEFINITION head: a `structBody` child IS present.
    {
        auto h = loadAndTokenize("struct S { int x; } v;");
        Parser p{h.src, h.schema, std::move(h.stream)};
        auto const r = std::move(p).parse();
        auto const& t = r.tree;
        ASSERT_FALSE(t.diagnostics().hasErrors());
        const NodeId spec = findFirstNodeWithRule(t, "structSpec");
        ASSERT_NE(spec, NodeId{}) << "a struct definition head is a structSpec";
        const auto bodyRule = t.schema().rules().find("structBody");
        ASSERT_TRUE(bodyRule.valid());
        bool hasBody = false;
        walkPreOrder(TreeCursor{t, spec, CursorMode::Ast}, [&](TreeCursor const& c) {
            const auto id = c.current();
            if (id.v != spec.v && t.kind(id) == NodeKind::Internal
                && t.rule(id).v == bodyRule.v) hasBody = true;
        });
        EXPECT_TRUE(hasBody)
            << "`struct S { … }` structSpec must HAVE a structBody child";
    }
    // REFERENCE head: NO `structBody` child (a bare `struct S` in a decl head).
    {
        auto h = loadAndTokenize("struct S v;");
        Parser p{h.src, h.schema, std::move(h.stream)};
        auto const r = std::move(p).parse();
        auto const& t = r.tree;
        ASSERT_FALSE(t.diagnostics().hasErrors());
        const NodeId spec = findFirstNodeWithRule(t, "structSpec");
        ASSERT_NE(spec, NodeId{}) << "a bare `struct S` head is a structSpec";
        const auto bodyRule = t.schema().rules().find("structBody");
        ASSERT_TRUE(bodyRule.valid());
        bool hasBody = false;
        walkPreOrder(TreeCursor{t, spec, CursorMode::Ast}, [&](TreeCursor const& c) {
            const auto id = c.current();
            if (id.v != spec.v && t.kind(id) == NodeKind::Internal
                && t.rule(id).v == bodyRule.v) hasBody = true;
        });
        EXPECT_FALSE(hasBody)
            << "`struct S` (reference) structSpec must have NO structBody child";
    }
}

// ── plan-24 Stage 7: the config-driven expression-depth cap LIFT ────────────
//
// Three pins for the single change "`maxExpressionDepth` is config-driven and
// raised": (A) the c-subset `.lang.json` `parser.maxExpressionDepth` actually
// reaches `GrammarSchema`; (B) the LIFT — a paren nest DEEPER than the old 256
// cap now parses CLEAN under the shipped cap; (C) the RED-on-disable BACKSTOP —
// the SAME nest, parsed with the cap RE-IMPOSED at 256, still FAILS LOUD with a
// positioned `P_ExpressionTooDeep` (the fail-loud ceiling is intact, never
// removed — BC-1). (B)+(C) on the SAME input are the load-bearing pair: only
// the raised cap distinguishes a clean parse from the depth diagnostic.

namespace {
[[nodiscard]] std::size_t countCode(Tree const& t, DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : t.diagnostics().all()) {
        if (d.code == code) ++n;
    }
    return n;
}

// `int main(void){ return ((( ... 0 ... )));}` with `depth` expression parens.
// A paren nest is the heaviest expression-DEEPENING path (each `(` counts one
// `maxExpressionDepth` unit) AND the one the cap is sized around (the residual
// recursive paren arm), so it is the right shape to exercise the cap. It folds
// flat (each paren is a transparent wrapper over the inner `0`), so nothing
// downstream cares — this pin is purely about the parser's depth gate.
[[nodiscard]] std::string parenNest(int depth) {
    std::string s = "int main(void){ return ";
    s.reserve(static_cast<std::size_t>(depth) * 2 + 40);
    for (int i = 0; i < depth; ++i) s += '(';
    s += '0';
    for (int i = 0; i < depth; ++i) s += ')';
    s += "; }";
    return s;
}

// Parse `source` against the real shipped c-subset schema with an explicit
// `maxExpressionDepth` cap, on the production 64 MiB deep-recursion worker
// stack (the parser's still-recursive paren arm needs it past a few hundred
// levels — exactly as `Program::compileFiles` runs the real parse). Returns
// the produced tree.
[[nodiscard]] Tree parseCSubsetWithCap(std::string source, std::size_t cap) {
    return dss::substrate::callOnLargeStack(
        dss::substrate::kDeepRecursionStackBytes, [&]() -> Tree {
            auto h = loadAndTokenize(std::move(source));
            ParserConfig cfg;
            cfg.maxExpressionDepth = cap;
            Parser p{h.src, h.schema, std::move(h.stream), std::move(cfg)};
            return std::move(std::move(p).parse().tree);
        });
}
} // namespace

// (A) The cap is CONFIG-DRIVEN: the c-subset `.lang.json` declares
// `parser.maxExpressionDepth`, and it round-trips to the loaded schema. If the
// loader silently dropped the field (or the JSON omitted it), this reads
// `nullopt` and the CU would fall back to the hardcoded 256 — defeating the
// "100% config-driven" requirement. RED if the loader wiring regresses.
TEST(ParserCSubsetSmoke, ExpressionDepthCapIsConfigDriven) {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(loaded.has_value());
    auto cap = (*loaded)->maxExpressionDepth();
    ASSERT_TRUE(cap.has_value())
        << "c-subset `parser.maxExpressionDepth` must reach the schema "
           "(config-driven, not the hardcoded ParserConfig default)";
    // The shipped value is HIGH (raised past the old 256) and BOUNDED. Pin the
    // exact value so a config edit that changes it must consciously update this
    // pin (and the corpus golden) rather than drift silently.
    EXPECT_EQ(*cap, 1024u)
        << "shipped c-subset expression-depth cap (Debug-safe high bound)";
    EXPECT_GT(*cap, 256u) << "the lift must raise the cap above the old 256";
}

// (B) THE LIFT: a 300-deep paren nest EXCEEDS the OLD 256 cap, yet now parses
// CLEAN under the shipped cap (1024). Pre-lift this exact input tripped
// `P_ExpressionTooDeep` at the 256th paren; post-lift it is a legal parse.
TEST(ParserCSubsetSmoke, DeepParenNestParsesCleanUnderRaisedCap) {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(loaded.has_value());
    const std::size_t shippedCap = (*loaded)->maxExpressionDepth().value_or(256);
    ASSERT_GT(shippedCap, 300u);

    // Parse with the SHIPPED cap (mirrors the production CU path).
    Tree t = parseCSubsetWithCap(parenNest(300), shippedCap);
    EXPECT_EQ(countCode(t, DiagnosticCode::P_ExpressionTooDeep), 0u)
        << "a 300-deep nest (> old 256) must NOT trip the cap at the raised "
           "value — this is the lift the stage delivers";
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "the 300-deep paren nest is legal C and must parse clean";
}

// (C) RED-on-disable BACKSTOP: the SAME 300-deep nest, parsed with the cap
// RE-IMPOSED at the old 256, STILL fails loud with a positioned
// `P_ExpressionTooDeep` + recovery (no crash, no silent truncation). This is
// the proof the fail-loud ceiling was NOT removed by the lift — it fires at
// WHATEVER cap is configured. Pairs with (B): identical input, only the cap
// differs, so the diagnostic's presence/absence is attributable solely to the
// cap value.
TEST(ParserCSubsetSmoke, ExpressionDepthCapStillFiresWhenReimposedAt256) {
    Tree t = parseCSubsetWithCap(parenNest(300), 256u);
    // Exactly one positioned too-deep diagnostic — the deepest push trips the
    // guard once at the 257th paren; the parse RECOVERS (returns a tree, no
    // overflow) and flags HasError.
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_GE(countCode(t, DiagnosticCode::P_ExpressionTooDeep), 1u)
        << "with the cap re-imposed at 256, a 300-deep nest MUST still fail "
           "loud — the backstop is intact at whatever value is configured";
    EXPECT_TRUE(t.diagnostics().hasErrors());
    // The diagnostic is POSITIONED at a real `(` (line 1), at or before the
    // innermost paren — never an unpositioned/zero span.
    for (auto const& d : t.diagnostics().all()) {
        if (d.code != DiagnosticCode::P_ExpressionTooDeep) continue;
        const auto lc = t.source().lineCol(d.span.start());
        EXPECT_EQ(lc.line, 1u);
        EXPECT_EQ(t.source().slice(d.span), "(");
    }
}

// ── C 5.1.1.2 phase 6: adjacent string-literal concatenation grammar ────────
// (D-CSUBSET-ADJACENT-STRING-CONCAT). The `stringLiteralExpr` rule is now
// `StringStart StringLiteral (StringStart StringLiteral)*` — flat children, no
// wrapper node. These pin the FLAT child shape the decode chokepoint relies on.

namespace {

// Count visible (non-EmptySpace) TOKEN children of `node`.
[[nodiscard]] std::size_t visibleTokenChildCount(Tree const& t, NodeId node) {
    std::size_t n = 0;
    for (NodeId c : t.children(node)) {
        if (isEmptySpace(t.flags(c))) continue;
        if (t.kind(c) == NodeKind::Token) ++n;
    }
    return n;
}

[[nodiscard]] Tree parseCSubset(std::string source) {
    auto h = loadAndTokenize(std::move(source));
    Parser p{h.src, h.schema, std::move(h.stream)};
    return std::move(p).parse().tree;
}

} // namespace

// Regression: a LONE string literal still produces EXACTLY two token children
// (StringStart + StringLiteral) — the repeat fires zero times, byte-identical
// to the pre-c20 single-pair rule. RED if the grammar change perturbed the
// single-string shape.
TEST(ParserCSubsetSmoke, SingleStringLiteralHasTwoChildren) {
    Tree t = parseCSubset("int main() { \"x\"; }");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    NodeId const sle = findFirstNodeWithRule(t, "stringLiteralExpr");
    ASSERT_TRUE(sle.valid()) << "a lone string must still form a stringLiteralExpr";
    EXPECT_EQ(visibleTokenChildCount(t, sle), 2u)
        << "lone string: StringStart + StringLiteral (repeat fires 0×)";
}

// Two adjacent string literals concatenate into ONE stringLiteralExpr with
// FOUR token children (StringStart StringLiteral StringStart StringLiteral) —
// flat, no wrapper. This is the shape `decodeAdjacentStringBodies` walks.
TEST(ParserCSubsetSmoke, TwoAdjacentStringsHaveFourChildren) {
    Tree t = parseCSubset("int main() { \"a\" \"b\"; }");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    NodeId const sle = findFirstNodeWithRule(t, "stringLiteralExpr");
    ASSERT_TRUE(sle.valid());
    EXPECT_EQ(visibleTokenChildCount(t, sle), 4u)
        << "\"a\" \"b\" → 4 flat token children (the repeat fires once)";
    // There must be exactly ONE stringLiteralExpr node — the second pair is
    // absorbed by the repeat, NOT a separate expression.
    std::size_t exprCount = 0;
    RuleId const rid = t.schema().rules().find("stringLiteralExpr");
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        NodeId const id{i};
        if (t.kind(id) == NodeKind::Internal && t.rule(id).v == rid.v) ++exprCount;
    }
    EXPECT_EQ(exprCount, 1u) << "adjacent strings form ONE expression, not two";
}

// Three adjacent string literals → SIX token children (the repeat fires twice).
TEST(ParserCSubsetSmoke, ThreeAdjacentStringsHaveSixChildren) {
    Tree t = parseCSubset("int main() { \"a\" \"b\" \"c\"; }");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    NodeId const sle = findFirstNodeWithRule(t, "stringLiteralExpr");
    ASSERT_TRUE(sle.valid());
    EXPECT_EQ(visibleTokenChildCount(t, sle), 6u)
        << "\"a\" \"b\" \"c\" → 6 flat token children (repeat fires twice)";
}

// ── FC17 (D-CSUBSET-ATTRIBUTE-STATEMENT, C23 6.8.1): the attribute-declaration
//    statement + the declOrAttrStmt wrapper shape ─────────────────────────────

// ★ The F2 wrapper-shape pin: a statement-position declaration now parses as
// `statement > declOrAttrStmt > varDecl` — the named alt rule MATERIALIZES a
// CST node (the declOrExprStmt precedent). Both consumers are transparent BY
// MECHANISM (HIR's unmapped-statement soleMeaningfulChild PassThrough peels
// it; semantic passes are rule-keyed full-tree walks), so the SHAPE is the
// contract this pin owns: if the wrapper is ever removed (or doubled), the
// corpus .tree golden AND this pin flip together — deliberately.
TEST(ParserCSubsetSmoke, StatementVarDeclRidesTheDeclOrAttrStmtWrapper) {
    Tree t = parseCSubset("int main() { int x; return x; }");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    NodeId const wrapper = findFirstNodeWithRule(t, "declOrAttrStmt");
    ASSERT_TRUE(wrapper.valid())
        << "a local declaration statement must ride the declOrAttrStmt wrapper";
    // The wrapper's sole child is the committed varDecl branch.
    NodeId const stmt = t.parent(wrapper);
    ASSERT_TRUE(stmt.valid());
    EXPECT_EQ(t.rules().name(t.rule(stmt)), std::string_view{"statement"})
        << "the wrapper's parent is the statement alt node";
    NodeId const decl = findFirstNodeWithRule(t, "varDecl");
    ASSERT_TRUE(decl.valid());
    EXPECT_EQ(t.parent(decl).v, wrapper.v)
        << "statement > declOrAttrStmt > varDecl — the varDecl is the "
           "wrapper's direct child";
}

// `[[fallthrough]];` parses as `statement > declOrAttrStmt >
// attributeDeclaration` (the varDecl probe rolls back on the immediate `;`).
TEST(ParserCSubsetSmoke, FallthroughStatementParsesAsAttributeDeclaration) {
    Tree t = parseCSubset(
        "int main() { int x = 1; switch (x) { case 1: x = 2; [[fallthrough]]; "
        "case 2: x = 3; break; } return x; }");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "[[fallthrough]]; must parse as a statement";
    NodeId const attrStmt = findFirstNodeWithRule(t, "attributeDeclaration");
    ASSERT_TRUE(attrStmt.valid());
    NodeId const wrapper = t.parent(attrStmt);
    ASSERT_TRUE(wrapper.valid());
    EXPECT_EQ(t.rules().name(t.rule(wrapper)), std::string_view{"declOrAttrStmt"})
        << "the bare attribute statement rides the same wrapper alt";
}

// An ATTRIBUTED declaration statement (`[[maybe_unused]] int x = 5;`) still
// commits the varDecl branch — the attribute rides varDecl's
// localDeclSpecifiers prefix, NOT the attributeDeclaration reading (declared
// probe order: varDecl first).
TEST(ParserCSubsetSmoke, AttributedLocalDeclCommitsVarDeclBranch) {
    Tree t = parseCSubset("int main() { [[maybe_unused]] int x = 5; return 0; }");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(hasInternalNodeWithRule(t, "varDecl"))
        << "the attributed declaration must commit the varDecl reading";
    EXPECT_FALSE(hasInternalNodeWithRule(t, "attributeDeclaration"))
        << "the attributeDeclaration branch must NOT win for a declaration";
}
