#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/syntactic/parser.hpp"
#include "core/substrate/large_stack_call.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/tree_node.hpp"
#include "core/types/tree_visitor.hpp"
#include "core/types/type_lattice/type_interner.hpp"
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

struct CHarness {
    std::shared_ptr<SourceBuffer>        src;
    std::shared_ptr<GrammarSchema const> schema;
    TokenStream                          stream;
};

[[nodiscard]] CHarness loadAndTokenize(std::string source) {
    auto loaded = GrammarSchema::loadShipped("c");
    EXPECT_TRUE(loaded.has_value());
    auto schema = *loaded;
    auto src    = SourceBuffer::fromString(std::move(source), "<c-smoke>");
    Tokenizer tk{src, schema, DiagnosticBudget::libraryDefault()};
    auto [stream, _] = std::move(tk).tokenize();
    return CHarness{
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

// Smoke: parser drives c end-to-end on minimal input. Tree-shape
// pinning lives in PA4 corpus tests; this only confirms the
// AltChoice→RuleLeaf search fallback + optional/repeat nullable-skip
// paths work against the real grammar.
TEST(ParserCSmoke, IntVarDeclWithLiteralInitializer) {
    auto h = loadAndTokenize("int x = 5;");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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

// Closes v2-gap-catalog row 1: parser-driven c, mixed-precedence
// expression produces the precedence-correct (a + (b * c)) shape via
// the Pratt walker rather than the old flat-fold sequence.
TEST(ParserCSmoke, FunctionBodyExpressionIsPrecedenceCorrect) {
    auto h = loadAndTokenize("int main() { a + b * c; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, TightLhsMulThenAddNestsMulOnLeft) {
    auto h = loadAndTokenize("int main() { a * b + c; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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

// `a - b + c` — SAME-precedence chain (c declares `+`/`-` in one
// LEFT group) must nest LEFT: `(a - b) + c`. This is the exact shape
// whose right-recursive mis-nesting silently miscompiled `10 - 3 + 1`
// to 6 (instead of 8) before the wrap-in-place fix.
TEST(ParserCSmoke, SamePrecSubAddChainNestsLeftward) {
    auto h = loadAndTokenize("int main() { a - b + c; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, DivisionChainNestsLeftward) {
    auto h = loadAndTokenize("int main() { a / b / c; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, AssignmentChainNestsRightward) {
    auto h = loadAndTokenize("int main() { a = b = c; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, TernaryChainExactRightNestedShape) {
    auto h = loadAndTokenize("int main() { a ? b : c ? d : e; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, CallPlusCallMixesPostfixAndInfix) {
    auto h = loadAndTokenize("int main() { f(a) + g(b); }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, SpacedCallWrapsCalleeNotWhitespace) {
    auto h = loadAndTokenize("int main() { return f (40) + 2; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, CommentTriviaInChainKeepsLeafOrder) {
    const std::string source = "int main() { a /*x*/ + b; }";
    auto h = loadAndTokenize(source);
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, SpacedTernaryShapeMatchesCompactForm) {
    auto h = loadAndTokenize("int main() { a  ?  b  :  c ; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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

TEST(ParserCSmoke, TernaryParsesAsMixfix) {
    auto h = loadAndTokenize("int main() { a ? b : c; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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

TEST(ParserCSmoke, TernaryIsRightAssociative) {
    // `a ? b : c ? d : e` → a ? b : (c ? d : e): the else branch nests.
    auto h = loadAndTokenize("int main() { a ? b : c ? d : e; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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

TEST(ParserCSmoke, TernaryBindsLooserThanAssignmentRhs) {
    // `x = a ? b : c` → `x = (a ? b : c)`: ternary (prec 16) binds tighter than
    // the assignment RHS (prec 15), so the `=`'s RHS is the whole ternary.
    auto h = loadAndTokenize("int main() { x = a ? b : c; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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

TEST(ParserCSmoke, TernaryAsCallArgument) {
    // `f(a ? b : c)` — ternary nested as an operand (call arg) exercises the
    // climb re-entry inside the argList body.
    auto h = loadAndTokenize("int main() { f(a ? b : c); }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    EXPECT_FALSE(result.tree.diagnostics().hasErrors());
    EXPECT_NE(findFirstNodeWithRule(result.tree, "ternaryExpr"), NodeId{});
}

TEST(ParserCSmoke, TernaryMissingColonRecovers) {
    // `a ? b ;` — missing `:` separator. The walker emits P_MissingRequiredChild
    // + an Error leaf (HasError on root), and recovers without hanging.
    auto h = loadAndTokenize("int main() { a ? b ; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t = result.tree;
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_TRUE(t.diagnostics().hasErrors());
    bool sawMissing = false;
    for (auto const& d : t.diagnostics().all())
        if (d.code == DiagnosticCode::P_MissingRequiredChild) { sawMissing = true; break; }
    EXPECT_TRUE(sawMissing) << "missing ':' must emit P_MissingRequiredChild";
}

TEST(ParserCSmoke, FunctionCallParsesAsPostfix) {
    auto h = loadAndTokenize("int main() { f(a, b); }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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

TEST(ParserCSmoke, EmptyArgumentCallParsesAsPostfix) {
    auto h = loadAndTokenize("int main() { f(); }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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

TEST(ParserCSmoke, ArrayIndexParsesAsPostfix) {
    auto h = loadAndTokenize("int main() { a[0]; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, ArrayIndexBodyClimbsPrecedence) {
    auto h = loadAndTokenize("int main() { a[i + j * k]; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, MissingCloserEmitsRecoveryDiag) {
    auto h = loadAndTokenize("int main() { f(a; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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

TEST(ParserCSmoke, PostfixIncParsesAsPostfix) {
    auto h = loadAndTokenize("int main() { i++; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, PrefixDerefParsesAsUnary) {
    auto h = loadAndTokenize("int main() { *p; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, CompoundAssignmentParsesAsBinaryExpr) {
    auto h = loadAndTokenize("int main() { x += 1; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, ShlCompoundAssignmentRespectsLongestMatch) {
    auto h = loadAndTokenize("int main() { x <<= 1; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, CompoundAssignmentIsRightAssociative) {
    auto h = loadAndTokenize("int main() { x += y += z; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
// ★★ P53 SCOPE MOVE (D-C-EXTERN-MUST-LEAD-THE-DECLARATION-SPECIFIERS), and it
// applies to EVERY `externDecl`-shaped pin in this block. The source below used
// to be written at FILE scope. It is now written INSIDE A BLOCK, because
// `externDecl` no longer appears in `/shapes/topLevel` at all: C 6.7.1 makes the
// declaration specifiers an unordered SET, and two declaration branches on one
// lead token cannot both survive a long function body at any alt order
// (✔MEASURED, P53 lane `ex`), so the file-scope rules were MERGED and
// `topLevelDecl` owns `extern` there. `externDecl` survives UNCHANGED as the
// BLOCK-scope rule (D-CSUBSET-BLOCK-SCOPE-EXTERN), which is why every expected
// tree below is byte-identical to what it was — the rule did not change, its
// reachable POSITION did.
// ⓘ The FILE-scope shape is pinned by `ExternAtFileScopeParsesThroughTheMergedRule`
// below, and the two together are what say the merge moved the spelling without
// losing it.
TEST(ParserCSmoke, ExternFunctionPrototypeParses) {
    auto h = loadAndTokenize("int use(void){ extern int printf(char x); return 0; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(hasInternalNodeWithRule(t, "externDecl"));

    const NodeId ext = findFirstNodeWithRule(t, "externDecl");
    ASSERT_NE(ext, NodeId{});
    // c23 D-CSUBSET-EXTERN-MULTI-DECLARATOR: externDecl is now the extern twin of
    // topLevelDecl — `externSpecifiers` (the specifierPrefix HEAD WRAPPER, TLS C1) +
    // `typeRefAllowingStruct` (the star-free base-type head struct fields use) +
    // `initDeclaratorList` (N declarators, each owning its pointer/array/fn suffix) +
    // `;`. A function prototype is a declarator whose name carries a fnSuffix; in the
    // suffix-repeat position that suffix is `fnSuffixTail` (the guard-less post-base
    // twin of `fnSuffix`, semantically folded to FnSig identically). The legacy
    // single-declarator spine (`typeRef Identifier externTail(externFuncTail)`) was
    // retired. FC4 c1: `param` is declarator-shaped (declHeadForParam + declarator).
    constexpr std::string_view kExpected =
        "rule:externDecl\n"
        "  rule:externSpecifiers\n"
        "    tok:\"extern\"\n"
        "  rule:typeRefAllowingStruct\n"
        "    rule:typeBaseAllowingStruct\n"
        "      rule:typeSpecifierSeq\n"
        "        tok:\"int\"\n"
        "  rule:initDeclaratorList\n"
        "    rule:initDeclarator\n"
        "      rule:declarator\n"
        "        rule:directDeclarator\n"
        "          tok:\"printf\"\n"
        "          rule:fnSuffixTail\n"
        "            tok:\"(\"\n"
        "            rule:paramList\n"
        "              rule:param\n"
        "                rule:declHeadForParam\n"
        "                  rule:typeSpecifierSeq\n"
        "                    tok:\"char\"\n"
        "                rule:declarator\n"
        "                  rule:directDeclarator\n"
        "                    tok:\"x\"\n"
        "            tok:\")\"\n"
        // D-CSUBSET-EXTERN-FN-DEFINITION (§B 2026-07-21): the `;` now rides the
        // `externDeclTail` wrapper (the `{ block | EndStatement }` alt — the twin of
        // topLevelDeclTail), so a `block` tail can make externDecl a function
        // DEFINITION. A prototype takes the EndStatement arm.
        "  rule:externDeclTail\n"
        "    tok:\";\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, ext), kExpected);
}

// D-CSUBSET-EXTERN-FN-DEFINITION (§B 2026-07-21): `extern` on a FUNCTION
// DEFINITION — `extern int f(void){ return 0; }` (valid C 6.9.1, symmetric with
// `static int f(){…}`; the Tcl `EXTERN int Sqlite3_Init(…){…}` shape). The
// `externDeclTail`'s `block` arm makes it a definition; before this cycle
// externDecl was declaration-only and P0009'd at the `{`. RED-ON-DISABLE: revert
// externDeclTail's block arm -> this no longer parses (P0009).
TEST(ParserCSmoke, ExternFunctionDefinitionParses) {
    auto h = loadAndTokenize("extern int f(void){ return 0; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "`extern int f(void){ return 0; }` must parse as a function definition";
    // ★★ P53 (D-C-EXTERN-MUST-LEAD-THE-DECLARATION-SPECIFIERS): this is the ONE
    // `extern` form that CANNOT move to block scope with its siblings above — a
    // block-scope `extern int f(void){…}` is a NESTED FUNCTION, which is not C
    // and which `lowerStmtNode`'s ExternDecl arm rejects fail-loud. So it is the
    // form that pins the MERGED file-scope shape instead: `topLevelDecl` with
    // `extern` sitting in a `singleDeclSpecifier`, and the definition still
    // distinguished from the declaration by a `block` on the tail. The FACT
    // being pinned is unchanged (D-CSUBSET-EXTERN-FN-DEFINITION); only which
    // rule owns it moved.
    ASSERT_TRUE(hasInternalNodeWithRule(t, "topLevelDecl"));
    ASSERT_FALSE(hasInternalNodeWithRule(t, "externDecl"))
        << "`externDecl` must no longer be reachable at FILE scope — one owner "
           "per lead token is what keeps every specifier ordering off the "
           "128-token speculative probe";
    const NodeId spec = findFirstNodeWithRule(t, "singleDeclSpecifier");
    ASSERT_NE(spec, NodeId{})
        << "`extern` must ride the ordinary declaration-specifier run";
    bool specIsExtern = false;
    for (NodeId c : t.children(spec))
        if (t.kind(c) == NodeKind::Token && t.text(c) == "extern")
            specIsExtern = true;
    EXPECT_TRUE(specIsExtern);
    // The topLevelDeclTail's chosen arm is a `block` (a function-definition
    // body), NOT an EndStatement token — this is what distinguishes the
    // definition from the prototype/declaration forms.
    const NodeId tail = findFirstNodeWithRule(t, "topLevelDeclTail");
    ASSERT_NE(tail, NodeId{})
        << "topLevelDecl must carry a topLevelDeclTail wrapper";
    bool tailHasBlock = false;
    for (NodeId c : t.children(tail)) {
        if (t.kind(c) == NodeKind::Internal
            && t.rules().name(t.rule(c)) == "block")
            tailHasBlock = true;
    }
    EXPECT_TRUE(tailHasBlock)
        << "an extern function DEFINITION's tail is a `block` body, not `;`";
}

// ★★★ P53 (D-C-EXTERN-MUST-LEAD-THE-DECLARATION-SPECIFIERS) — THE FILE-SCOPE
// SHAPE, PINNED IN FULL, so the block-scope moves above cannot be read as
// coverage lost. `extern` is an ordinary `singleDeclSpecifier` inside
// `declSpecifiers`, and the declaration is an ordinary `topLevelDecl`: that is
// what makes C 6.7.1's specifier SET unordered, because there is no second
// declaration rule for the keyword to lead.
TEST(ParserCSmoke, ExternAtFileScopeParsesThroughTheMergedRule) {
    auto h = loadAndTokenize("extern int printf(int fmt);");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    const NodeId decl = findFirstNodeWithRule(t, "topLevelDecl");
    ASSERT_NE(decl, NodeId{});
    constexpr std::string_view kExpected =
        "rule:topLevelDecl\n"
        "  rule:declSpecifiers\n"
        "    rule:singleDeclSpecifier\n"
        "      tok:\"extern\"\n"
        "  rule:topLevelHead\n"
        "    rule:typeSpecifierSeq\n"
        "      tok:\"int\"\n"
        "  rule:declAttrRun\n"
        "  rule:initDeclaratorList\n"
        "    rule:initDeclarator\n"
        "      rule:declarator\n"
        "        rule:directDeclarator\n"
        "          tok:\"printf\"\n"
        "          rule:fnSuffixTail\n"
        "            tok:\"(\"\n"
        "            rule:paramList\n"
        "              rule:param\n"
        "                rule:declHeadForParam\n"
        "                  rule:typeSpecifierSeq\n"
        "                    tok:\"int\"\n"
        "                rule:declarator\n"
        "                  rule:directDeclarator\n"
        "                    tok:\"fmt\"\n"
        "            tok:\")\"\n"
        "  rule:topLevelDeclTail\n"
        "    tok:\";\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, decl), kExpected);
}

// The REVERSED orders, in the shape that matters: `extern` is not required to
// lead, so the specifier run may hold any of them in any order. A shape pin
// rather than a full tree, because the point is the RUN, not one spelling.
TEST(ParserCSmoke, ReversedExternSpecifierOrdersRideOneSpecifierRun) {
    for (char const* src : {"inline extern int p(int);",
                            "__inline extern int p(int);",
                            "_Noreturn extern void die(int);",
                            "_Thread_local extern int e;",
                            "thread_local extern int e;",
                            "_Noreturn inline extern void die(int);"}) {
        auto h = loadAndTokenize(src);
        Parser p{h.src, h.schema, std::move(h.stream),
                 DiagnosticBudget::libraryDefault()};
        auto result = std::move(p).parse();
        auto const& t = result.tree;
        EXPECT_FALSE(t.diagnostics().hasErrors()) << src;
        EXPECT_TRUE(hasInternalNodeWithRule(t, "topLevelDecl")) << src;
        const NodeId run = findFirstNodeWithRule(t, "declSpecifiers");
        ASSERT_NE(run, NodeId{}) << src;
        bool sawExtern = false;
        for (NodeId c : t.children(run)) {
            if (t.kind(c) != NodeKind::Internal) continue;
            for (NodeId g : t.children(c))
                if (t.kind(g) == NodeKind::Token && t.text(g) == "extern")
                    sawExtern = true;
        }
        EXPECT_TRUE(sawExtern)
            << src << " — `extern` must be one member of the specifier run, not "
                      "the head of a rule of its own";
    }
}

// D-CSUBSET-EXTERN-FN-DEFINITION regression: the DSS per-declaration import-
// library override (`extern void* g(int) "kernel32.dll";`) still parses — the
// `{optional stringLiteralExpr}` stays a DIRECT child BEFORE the externDeclTail
// (so externLibraryOverride's scan is unchanged), and the EndStatement tail rides
// the wrapper. RED-ON-DISABLE: nest the string inside externDeclTail -> the
// override lowering would no longer find it as a direct role child.
TEST(ParserCSmoke, ExternImportLibraryOverrideParses) {
    // P53 scope move — see `ExternFunctionPrototypeParses`. The FILE-scope form
    // of this same override is pinned end-to-end by
    // `HirLoweringC.MergedRuleKeepsExternFunctionImportAndLibraryOverride`,
    // which asserts the decoded library rather than the node shape.
    auto h = loadAndTokenize(
        "int use(void){ extern void* g(int) \"kernel32.dll\"; return 0; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "the import-library override form must still parse";
    ASSERT_TRUE(hasInternalNodeWithRule(t, "externDecl"));
    // The stringLiteralExpr is a DIRECT child of externDecl (not nested in the
    // tail) — the byte-preserved import-override position.
    const NodeId ext = findFirstNodeWithRule(t, "externDecl");
    ASSERT_NE(ext, NodeId{});
    bool directString = false;
    for (NodeId c : t.children(ext)) {
        if (t.kind(c) == NodeKind::Internal
            && t.rules().name(t.rule(c)) == "stringLiteralExpr")
            directString = true;
    }
    EXPECT_TRUE(directString)
        << "the library-override string must stay a DIRECT externDecl child";
}

// c23 D-CSUBSET-EXTERN-MULTI-DECLARATOR: a MULTI-declarator `extern` declaration
// (`extern int a, b;`) parses — ONE externDecl whose initDeclaratorList holds TWO
// initDeclarator children. RED-ON-DISABLE: reverting externDecl to the legacy
// single-declarator spine P0009's on the comma, so this test would not parse.
TEST(ParserCSmoke, ExternMultiDeclaratorParses) {
    // P53 scope move — see `ExternFunctionPrototypeParses`.
    auto h = loadAndTokenize("int use(void){ extern int a, b; return a; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "`extern int a, b;` must parse with no diagnostics";
    ASSERT_TRUE(hasInternalNodeWithRule(t, "externDecl"));
    const NodeId ext = findFirstNodeWithRule(t, "externDecl");
    ASSERT_NE(ext, NodeId{});
    // ⚠ The list must be found UNDER `ext`, not by a whole-tree search: since
    // the P53 scope move this source also carries the enclosing function's own
    // `topLevelDecl`, whose `use(void)` declarator list comes FIRST in document
    // order. A tree-wide `findFirstNodeWithRule` counts that one and reports 1.
    NodeId list{};
    for (NodeId c : t.children(ext)) {
        if (t.kind(c) == NodeKind::Internal
            && t.rules().name(t.rule(c)) == "initDeclaratorList")
            list = c;
    }
    ASSERT_NE(list, NodeId{})
        << "externDecl must carry an initDeclaratorList (the multi-declarator list)";
    std::size_t initDeclarators = 0;
    for (NodeId c : t.children(list)) {
        if (t.kind(c) == NodeKind::Internal
            && t.rules().name(t.rule(c)) == "initDeclarator")
            ++initDeclarators;
    }
    EXPECT_EQ(initDeclarators, 2u)
        << "`extern int a, b;` must bind TWO declarators (one per comma-separated name)";
}

// c23 D-CSUBSET-EXTERN-MULTI-DECLARATOR: per-declarator pointer/array suffix —
// `extern int *a, b;` — the star binds to `a` ONLY (a: int*, b: int), because the
// star lives inside `a`'s own declarator, not the shared head. RED-ON-DISABLE: a
// shared-head star (the retired `typeRef` spine) would apply to both.
TEST(ParserCSmoke, ExternMultiDeclaratorPerDeclaratorPointerParses) {
    auto h = loadAndTokenize("extern int *a, b;");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    const NodeId list = findFirstNodeWithRule(t, "initDeclaratorList");
    ASSERT_NE(list, NodeId{});
    // Exactly ONE pointerLayer in the whole declaration (a's star), not two.
    const auto ptrRule = t.schema().rules().find("pointerLayer");
    ASSERT_TRUE(ptrRule.valid());
    std::size_t pointerLayers = 0;
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId n{i};
        if (t.kind(n) == NodeKind::Internal && t.rule(n).v == ptrRule.v)
            ++pointerLayers;
    }
    EXPECT_EQ(pointerLayers, 1u)
        << "`extern int *a, b;` — the star binds ONLY to a (one pointerLayer), "
           "so b stays a plain int";
}

TEST(ParserCSmoke, ExternVariableDeclParses) {
    // P53 scope move — see `ExternFunctionPrototypeParses`.
    auto h = loadAndTokenize("int use(void){ extern int errno; return errno; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(hasInternalNodeWithRule(t, "externDecl"));
}

// ── Trailing type-qualifier on a type-specifier head (C 6.7.1: decl-specifiers
//    in ANY order) ─────────────────────────────────────────────────────────────
//
// D-CSUBSET-STRUCT-MULTI-DECLARATOR / D-CSUBSET-EXTERN-MULTI-DECLARATOR: the head
// `typeRefAllowingStruct` (shared by externDecl + struct/union fields) now takes a
// TRAILING `{repeat headQualifier}` (was `{optional ConstKeyword}`, const-only),
// symmetric with its leading slot and topLevelHead's trailing slot. So a base type
// FOLLOWED by a cv-qualifier — `LONG volatile` == `volatile LONG` (C 6.7.1) — parses
// on BOTH a typedef-name and a builtin base. The pre-fix const-only slot P0009'd at a
// trailing `volatile`. Motivating case: sqlite `src/test1.c`
// `extern LONG volatile sqlite3_os_type;` (LONG = windows.json) under #if
// SQLITE_OS_WIN. A parser-level test needs no `typedef` for LONG: externDecl is
// committed by `extern`, so `typeBaseAllowingStruct`'s Identifier arm reads LONG as
// the type (an UNKNOWN typedef is a later SEMANTIC S0006, not a parse error).

// PRIMARY (RED-ON-DISABLE): `extern LONG volatile d;` — a typedef-name base + a
// trailing `volatile`. The `volatile` is a `rule:headQualifier` AFTER the
// `typeBaseAllowingStruct` base (NOT a declarator: a keyword can't be a
// declarator/typedef-name, so `d` stays the sole declarator). RED-ON-DISABLE:
// revert the trailing slot to `{optional ConstKeyword}` -> P0009 "expected
// 'Identifier', 'ParenOpen', 'StarOp' or 'BracketOpen' -- got 'volatile'".
TEST(ParserCSmoke, ExternTypedefNameTrailingQualifierParses) {
    // P53 scope move — see `ExternFunctionPrototypeParses`.
    auto h = loadAndTokenize("int use(void){ extern LONG volatile d; return 0; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "`extern LONG volatile d;` (typedef-name + trailing qualifier) must parse";
    const NodeId ext = findFirstNodeWithRule(t, "externDecl");
    ASSERT_NE(ext, NodeId{});
    constexpr std::string_view kExpected =
        "rule:externDecl\n"
        "  rule:externSpecifiers\n"
        "    tok:\"extern\"\n"
        "  rule:typeRefAllowingStruct\n"
        "    rule:typeBaseAllowingStruct\n"
        "      tok:\"LONG\"\n"
        "    rule:headQualifier\n"
        "      tok:\"volatile\"\n"
        "  rule:initDeclaratorList\n"
        "    rule:initDeclarator\n"
        "      rule:declarator\n"
        "        rule:directDeclarator\n"
        "          tok:\"d\"\n"
        "  rule:externDeclTail\n"
        "    tok:\";\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, ext), kExpected);
}

// The SAME trailing-qualifier gap on a BUILTIN base under `extern` —
// `extern int volatile d;` ALSO P0009'd before this fix (both externDecl and struct
// fields consume the one const-only trailing slot). The trailing `volatile` rides a
// `headQualifier` AFTER the base `typeSpecifierSeq`. RED-ON-DISABLE: same revert.
TEST(ParserCSmoke, ExternBuiltinTrailingQualifierParses) {
    // P53 scope move — see `ExternFunctionPrototypeParses`.
    auto h = loadAndTokenize("int use(void){ extern int volatile d; return 0; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "`extern int volatile d;` (builtin base + trailing qualifier) must parse";
    const NodeId head = findFirstNodeWithRule(t, "typeRefAllowingStruct");
    ASSERT_NE(head, NodeId{});
    constexpr std::string_view kExpectedHead =
        "rule:typeRefAllowingStruct\n"
        "  rule:typeBaseAllowingStruct\n"
        "    rule:typeSpecifierSeq\n"
        "      tok:\"int\"\n"
        "  rule:headQualifier\n"
        "    tok:\"volatile\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, head), kExpectedHead);
}

// Multiple trailing qualifiers, any order — `extern LONG const volatile cv;` — the
// `{repeat}` admits a RUN. Exactly TWO trailing `headQualifier` children (const,
// volatile) after the base. RED-ON-DISABLE: the const-only `{optional}` slot admits
// at most the single leading `const`, so `volatile` P0009s.
TEST(ParserCSmoke, ExternTrailingQualifierRunParses) {
    // P53 scope move — see `ExternFunctionPrototypeParses`.
    auto h = loadAndTokenize(
        "int use(void){ extern LONG const volatile cv; return 0; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "`extern LONG const volatile cv;` (trailing qualifier run) must parse";
    const NodeId head = findFirstNodeWithRule(t, "typeRefAllowingStruct");
    ASSERT_NE(head, NodeId{});
    std::size_t headQualifiers = 0;
    for (NodeId c : t.children(head)) {
        if (t.kind(c) == NodeKind::Internal
            && t.rules().name(t.rule(c)) == "headQualifier")
            ++headQualifiers;
    }
    EXPECT_EQ(headQualifiers, 2u)
        << "the trailing `{repeat headQualifier}` must consume BOTH const and volatile";
}

// The OTHER consumer of `typeRefAllowingStruct` — a struct member head. Before the
// fix `struct S { int volatile x; };` P0009'd at `volatile` exactly like the extern
// case (proving the gap was in the SHARED head rule, not extern-specific). The
// leading form `struct S { volatile int x; };` already parsed. RED-ON-DISABLE: same
// revert -> P0009 in the field head.
TEST(ParserCSmoke, StructMemberTrailingQualifierParses) {
    auto h = loadAndTokenize("struct S { int volatile x; };");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "`struct S { int volatile x; };` (member trailing qualifier) must parse";
    const NodeId head = findFirstNodeWithRule(t, "typeRefAllowingStruct");
    ASSERT_NE(head, NodeId{});
    // The field head is BYTE-IDENTICAL to the extern builtin head — the trailing
    // `volatile` rides a headQualifier AFTER the base, proving the gap was in the
    // SHARED `typeRefAllowingStruct` rule (not extern-specific).
    constexpr std::string_view kExpectedHead =
        "rule:typeRefAllowingStruct\n"
        "  rule:typeBaseAllowingStruct\n"
        "    rule:typeSpecifierSeq\n"
        "      tok:\"int\"\n"
        "  rule:headQualifier\n"
        "    tok:\"volatile\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, head), kExpectedHead);
}

// Regression guards: the forms that ALREADY parsed before the fix must keep parsing
// — the LEADING qualifier on a typedef-name (`extern volatile LONG e;`), and the
// top-level trailing/leading forms that route through topLevelHead (already
// `{repeat headQualifier}`): `int volatile f;`, `volatile LONG g;`,
// `LONG volatile h = 0;`. All parser-level (an unknown typedef-name is a later
// SEMANTIC error, never a parse error).
TEST(ParserCSmoke, TypeHeadQualifierOrderRegressionForms) {
    for (std::string_view source : {
             std::string_view{"extern volatile LONG e;"},
             std::string_view{"int volatile f;"},
             std::string_view{"volatile LONG g;"},
             std::string_view{"LONG volatile h = 0;"},
         }) {
        auto h = loadAndTokenize(std::string{source});
        Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
        auto result = std::move(p).parse();
        auto const& t = result.tree;
        ASSERT_NE(t.root(), InvalidNode) << source;
        EXPECT_FALSE(t.diagnostics().hasErrors())
            << "must still parse (qualifier order is free per C 6.7.1): " << source;
    }
}

// Array declarator: expression-side `a[0]` is pinned by
// `ArrayIndexParsesAsPostfix`; this trio is the declarator-side
// complement (v2-gap-catalog row 12). The trio together exercises
// both `varDeclTail` (top-level) and `varDeclHead` (local) array-
// suffix attachment points, plus the empty-suffix path `int x[];`.
TEST(ParserCSmoke, TopLevelArrayDeclParses) {
    auto h = loadAndTokenize("int a[10];");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId tl = findFirstNodeWithRule(t, "topLevel");
    ASSERT_NE(tl, NodeId{});
    // FC4 c1: the specifier/declarator split — the array suffix now lives
    // INSIDE the directDeclarator (C 6.7.6), and the `;` is the
    // topLevelDeclTail's EndStatement arm (vs `{` = function definition).
    // ★ TF-C77: the EMPTY `declAttrRun` between the head and the declarator list
    // is present ON PURPOSE and this golden asserts it. `declAttrRun` is a named
    // rule over a lone `{repeat}`, so its node is emitted even when the source
    // carries no attribute — that is precisely what keeps the topLevelDecl row's
    // `declaratorList: 2` / `kindByChild [3,0]` indices CONSTANT instead of
    // shifting with the number of attributes written. If a future change makes
    // the run conditional, this line disappears and every declaration in the TU
    // is mis-indexed — so the empty node belongs in the golden, not out of it.
    constexpr std::string_view kExpected =
        "rule:topLevel\n"
        "  rule:topLevelDecl\n"
        "    rule:topLevelHead\n"
        "      rule:typeSpecifierSeq\n"
        "        tok:\"int\"\n"
        "    rule:declAttrRun\n"
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

TEST(ParserCSmoke, EmptyArrayDeclSuffixParses) {
    // `int x[];` — empty bracket pair. The size expression in
    // `arrayDeclSuffix` is `optional`, so the brackets can be empty.
    auto h = loadAndTokenize("int x[];");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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

TEST(ParserCSmoke, InnerArrayDeclParses) {
    auto h = loadAndTokenize("int main() { int buf[64]; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const NodeId head = findFirstNodeWithRule(t, "varDecl");
    ASSERT_NE(head, NodeId{});
    // FC4 c1: the local declaration statement is `varDecl` — keyword-led
    // head (kwDeclHead) + initDeclaratorList; the array suffix lives
    // INSIDE the directDeclarator (C 6.7.6).
    // ★ TF-C77: the EMPTY `declAttrRun` after the head is asserted DELIBERATELY —
    // the block-scope twin of the topLevelDecl golden above. The node is emitted
    // whether or not an attribute is written, which is what keeps this row's
    // `declaratorList: 2` constant; and it must appear in the `kwDeclHead` branch
    // as well as the specifier-led one, because ONE attribute must not mean two
    // different things depending on which branch of `varDecl` matched.
    constexpr std::string_view kExpected =
        "rule:varDecl\n"
        "  rule:kwDeclHead\n"
        "    rule:typeSpecifierForDecl\n"
        "      rule:typeSpecifierSeq\n"
        "        tok:\"int\"\n"
        "  rule:declAttrRun\n"
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

TEST(ParserCSmoke, ArrayDeclWithInitializerExpressionParses) {
    // Size expression can use earlier-declared constants. The pin
    // here captures the nested `binaryExpr` for `n * 2` inside the
    // arrayDeclSuffix — proving operator climb engages inside the
    // size expression (and that the binaryExpr is positioned
    // INSIDE arrayDeclSuffix, not somewhere else in the tree).
    auto h = loadAndTokenize("int main() { int buf[n * 2]; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, PostfixChainNestsLeftToRight) {
    auto h = loadAndTokenize("int main() { f(a)[i]; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, PostfixCallThenInfixBindsCorrectly) {
    auto h = loadAndTokenize("int main() { return f(a) + g(b); }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, ThreeDeepArrayIndexChainNests) {
    auto h = loadAndTokenize("int main() { a[i][j][k]; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, FunctionCallChainNests) {
    auto h = loadAndTokenize("int main() { f()(g); }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, ParenWrappedPostfixChainNests) {
    auto h = loadAndTokenize("int main() { (f(a)[i]); }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t = result.tree;
    EXPECT_FALSE(t.diagnostics().hasErrors());

    // The outer `expression` contains an `operand` whose body is the
    // named `parenExpr` rule (extracted from the prior anonymous
    // `(expression)` sequence when c's `operand` became
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
TEST(ParserCSmoke, BrokenPostfixChainEmitsDiagnostic) {
    auto h = loadAndTokenize("int main() { f(a)[i }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, PrefixOverPostfixChainNests) {
    auto h = loadAndTokenize("int main() { *p[i]++; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, ParenGroupingForcesOuterPrecedence) {
    auto h = loadAndTokenize("int main() { (a + b) * c; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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

// F2: c reintroduces CharLiteral via a `'`-opened body mode
// (mirrors the `"`-opened string mode). `'a'` parses cleanly: the
// `'` opener token, one `CharLiteral` body byte, the `'` closer
// (same token kind back-popping the body mode). Pin via a tree-
// presence assertion on the body-mode CharLiteral token kind.
TEST(ParserCSmoke, CharLiteralParsesAsOperand) {
    auto harness = loadAndTokenize("int main() { return 'a'; }");
    Parser parser{harness.src, harness.schema, std::move(harness.stream),
                  DiagnosticBudget::libraryDefault()};
    auto const result = std::move(parser).parse();
    auto const& tree = result.tree;
    EXPECT_FALSE(tree.diagnostics().hasErrors())
        << "c failed to parse CharLiteral cleanly";
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
// test_semantic_analyzer_c.cpp.

// Sketch-KNOWN Type: `MyP * p;` after `typedef int MyP;` COMMITS the
// declaration reading — an identVarDecl node exists, no binaryExpr
// multiplication wraps `MyP * p`.
TEST(ParserCSmoke, TypedefNameStarCommitsDeclarationStatement) {
    auto h = loadAndTokenize(
        "typedef int MyP;\n"
        "int main() { MyP * p; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto const result = std::move(p).parse();
    auto const& t = result.tree;
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(hasInternalNodeWithRule(t, "identVarDecl"))
        << "MyP * p; must parse as a DECLARATION statement";
}

// Sketch-KNOWN Value: `a * b;` (both locals) ROLLS BACK to the
// expression statement — NO identVarDecl; the `*` is the binary
// multiplication.
TEST(ParserCSmoke, ValueStarValueRollsBackToExpression) {
    auto h = loadAndTokenize("int main() { int a; int b; a * b; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, UnknownStarUnknownRollsBackToExpression) {
    auto h = loadAndTokenize("int main() { u * v; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto const result = std::move(p).parse();
    auto const& t = result.tree;
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_FALSE(hasInternalNodeWithRule(t, "identVarDecl"))
        << "unknown-led `u * v;` must stay an expression statement";
}

// C23 `auto x = 1;` type INFERENCE is NOT supported — `auto` is a
// storage-class specifier only, so the head consumed `x` as the type name
// and the missing declarator failed LOUD at the `=` (the
// D-CSUBSET-C23-AUTO-INFERENCE residue). FC17.5
// (D-CSUBSET-AUTO-TYPE-INFERENCE) — THE ANTICIPATED PIN FLIP: the C23
// 6.7.9 inference feature landed the HEAD-LESS `autoInferredVarDecl` rule
// (probed FIRST in declOrAttrStmt), so the form now PARSES into it. The
// flip asserts the new truth: a clean parse whose CST carries the
// inference rule (NOT varDecl — the statement must not have been absorbed
// by some other reading).
TEST(ParserCSmoke, AutoInferenceFormParsesIntoInferenceRule) {
    auto h = loadAndTokenize("int main() { auto x = 1; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto const result = std::move(p).parse();
    auto const& t = result.tree;
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "C23 `auto x = 1;` must parse (D-CSUBSET-AUTO-TYPE-INFERENCE)";
    EXPECT_TRUE(hasInternalNodeWithRule(t, "autoInferredVarDecl"))
        << "the statement must parse into the HEAD-LESS inference rule";
    EXPECT_FALSE(hasInternalNodeWithRule(t, "varDecl"))
        << "the C89-style varDecl reading must not have won (the head "
           "would have consumed `x` as a type name)";
}

// The C89 companion: `auto int x;` (auto as a plain storage-class with a
// real type head) still parses via varDecl — the inference rule fast-fails
// on `int` (not a declarator) and the probe rolls back. RED if the
// inference-first branch order ever swallows the C89 form.
TEST(ParserCSmoke, AutoWithTypeHeadStaysVarDecl) {
    auto h = loadAndTokenize("int main() { auto int x; }");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto const result = std::move(p).parse();
    auto const& t = result.tree;
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "C89 `auto int x;` must keep parsing";
    EXPECT_TRUE(hasInternalNodeWithRule(t, "varDecl"))
        << "the real-typed form must take the committed varDecl reading";
    EXPECT_FALSE(hasInternalNodeWithRule(t, "autoInferredVarDecl"))
        << "the inference rule must have rolled back on the `int` head";
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
TEST(ParserCSmoke, LongInitializerRidesTheStatementProbeBudget) {
    std::string src = "typedef int MyT;\nint main() { MyT x = f(1";
    for (int i = 0; i < 599; ++i) src += ", 1";
    src += "); return x; }";
    auto h = loadAndTokenize(std::move(src));
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
// `};`. This is `struct sqlite3` (sqlite3.c) in minimal form. It is NOT
// a member-COUNT limit (a 130-member control parses); it is a TOKEN-budget
// cliff that any large mixed struct (real SQLite) reaches at ~80 members.
TEST(ParserCSmoke, LargeStructBodyMustNotHitSpeculationBudget) {
    auto structOf = [](int n) {
        std::string s = "struct S {";
        for (int i = 0; i < n; ++i) s += "int a" + std::to_string(i) + ";";
        return s + "};";
    };
    // Control: 130 members (~390 body tokens) — far under the 4096 budget.
    {
        auto h = loadAndTokenize(structOf(130));
        Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
        auto const r = std::move(p).parse();
        EXPECT_FALSE(r.tree.diagnostics().hasErrors())
            << "130-member struct (control) must parse clean";
    }
    // Regression: 1500 members (~4500 body tokens) — exceeds the 4096 probe
    // budget. RED pre-c25 (P0009 at the `};`); GREEN once the large struct
    // body parse is no longer governed by the body-vs-ref speculation budget.
    {
        auto h = loadAndTokenize(structOf(1500));
        Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, LargeUnionBodyMustNotHitSpeculationBudget) {
    std::string s = "union U {";
    for (int i = 0; i < 1500; ++i) s += "int a" + std::to_string(i) + ";";
    s += "};";
    auto h = loadAndTokenize(std::move(s));
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto const r = std::move(p).parse();
    EXPECT_FALSE(r.tree.diagnostics().hasErrors())
        << "1500-member union must parse clean (c25 unified unionSpec)";
}

TEST(ParserCSmoke, LargeEnumBodyMustNotHitSpeculationBudget) {
    std::string s = "enum E {";
    for (int i = 0; i < 1500; ++i) s += "A" + std::to_string(i) + ",";
    s += "};";
    auto h = loadAndTokenize(std::move(s));
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, MixedLargeStructBodyParsesCleanPastOldBudget) {
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
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
TEST(ParserCSmoke, StructSpecBodyChildPresenceDiscriminatesDefineVsRef) {
    // DEFINITION head: a `structBody` child IS present.
    {
        auto h = loadAndTokenize("struct S { int x; } v;");
        Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
        Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
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
// raised": (A) the c `.lang.json` `parser.maxExpressionDepth` actually
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

// Parse `source` against the real shipped c schema with an explicit
// `maxExpressionDepth` cap, on the production 64 MiB deep-recursion worker
// stack (the parser's still-recursive paren arm needs it past a few hundred
// levels — exactly as `Program::compileFiles` runs the real parse). Returns
// the produced tree.
[[nodiscard]] Tree parseCWithCap(std::string source, std::size_t cap) {
    return dss::substrate::callOnLargeStack(
        dss::substrate::kDeepRecursionStackBytes, [&]() -> Tree {
            auto h = loadAndTokenize(std::move(source));
            ParserConfig cfg;
            cfg.maxExpressionDepth = cap;
            Parser p{h.src, h.schema, std::move(h.stream),
             DiagnosticBudget::libraryDefault(), std::move(cfg)};
            return std::move(std::move(p).parse().tree);
        });
}
} // namespace

// (A) The cap is CONFIG-DRIVEN: the c `.lang.json` declares
// `parser.maxExpressionDepth`, and it round-trips to the loaded schema. If the
// loader silently dropped the field (or the JSON omitted it), this reads
// `nullopt` and the CU would fall back to the hardcoded 256 — defeating the
// "100% config-driven" requirement. RED if the loader wiring regresses.
TEST(ParserCSmoke, ExpressionDepthCapIsConfigDriven) {
    auto loaded = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(loaded.has_value());
    auto cap = (*loaded)->maxExpressionDepth();
    ASSERT_TRUE(cap.has_value())
        << "c `parser.maxExpressionDepth` must reach the schema "
           "(config-driven, not the hardcoded ParserConfig default)";
    // The shipped value is HIGH (raised past the old 256) and BOUNDED. Pin the
    // exact value so a config edit that changes it must consciously update this
    // pin (and the corpus golden) rather than drift silently.
    EXPECT_EQ(*cap, 1024u)
        << "shipped c expression-depth cap (Debug-safe high bound)";
    EXPECT_GT(*cap, 256u) << "the lift must raise the cap above the old 256";
}

// (B) THE LIFT: a 300-deep paren nest EXCEEDS the OLD 256 cap, yet now parses
// CLEAN under the shipped cap (1024). Pre-lift this exact input tripped
// `P_ExpressionTooDeep` at the 256th paren; post-lift it is a legal parse.
TEST(ParserCSmoke, DeepParenNestParsesCleanUnderRaisedCap) {
    auto loaded = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(loaded.has_value());
    const std::size_t shippedCap = (*loaded)->maxExpressionDepth().value_or(256);
    ASSERT_GT(shippedCap, 300u);

    // Parse with the SHIPPED cap (mirrors the production CU path).
    Tree t = parseCWithCap(parenNest(300), shippedCap);
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
TEST(ParserCSmoke, ExpressionDepthCapStillFiresWhenReimposedAt256) {
    Tree t = parseCWithCap(parenNest(300), 256u);
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

// The visible TOKEN children of `node` as a `/`-joined list of schema-token
// KIND NAMES (e.g. `StringStart/StringLiteral/StringEnd`).
//
// D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN: added because a bare COUNT cannot tell
// `opener+body+closer` from `opener+body+body`, and the difference between those
// two is a silently corrupted string literal. Whenever a child count is pinned
// below, the ROLES are pinned alongside it.
[[nodiscard]] std::string visibleTokenChildKinds(Tree const& t, NodeId node) {
    std::string out;
    for (NodeId c : t.children(node)) {
        if (isEmptySpace(t.flags(c))) continue;
        if (t.kind(c) != NodeKind::Token) continue;
        if (!out.empty()) out += '/';
        out += t.schema().schemaTokens().name(t.tokenKind(c));
    }
    return out;
}

[[nodiscard]] Tree parseC(std::string source) {
    auto h = loadAndTokenize(std::move(source));
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    return std::move(p).parse().tree;
}

// The ROLE-bearing visible children of `node`, as `rule:NAME` / `tok:KIND`
// joined by '/'. The declaration rows in `c.lang.json` address a
// declaration's children BY INDEX, so this is the pin that makes an index
// shift visible: a bare "it parsed" check cannot tell `head: 0` pointing at
// the type head from `head: 0` pointing at a decoration.
[[nodiscard]] std::string visibleChildRoles(Tree const& t, NodeId node) {
    std::string out;
    for (NodeId c : t.children(node)) {
        if (isEmptySpace(t.flags(c))) continue;
        if (!out.empty()) out += '/';
        if (t.kind(c) == NodeKind::Internal) {
            out += "rule:";
            out += t.rules().name(t.rule(c));
        } else {
            out += "tok:";
            out += t.schema().schemaTokens().name(t.tokenKind(c));
        }
    }
    return out;
}

// The first error diagnostic's code + text, or "" when the tree is clean.
// Streamed into every parse assertion below so a failure names the real
// defect instead of just "expected false, got true".
[[nodiscard]] std::string firstErrorText(Tree const& t) {
    for (auto const& d : t.diagnostics().all()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        return std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    }
    return {};
}

// Analyze one in-memory c TU through the real CU + semantic pipeline.
// Mirrors `tests/analysis/semantic/semantic_test_fixture.hpp`'s
// `buildShippedUnit` + `analyze`, inlined because that fixture header is not
// on this target's include path. Needed by the anti-hijack pin below, which
// asserts a RESOLVED TYPE — a fact no parse-only check can reach.
[[nodiscard]] SemanticModel analyzeC(std::string source) {
    auto loaded = GrammarSchema::loadShipped("c");
    EXPECT_TRUE(loaded.has_value());
    UnitBuilder builder{*loaded, DiagnosticBudget::libraryDefault()};
    builder.addInMemory(std::move(source), "<c-smoke>");
    return analyze(std::make_shared<CompilationUnit>(std::move(builder).finish()),
                   DiagnosticBudget::libraryDefault());
}

// The Type-kind symbol named `name`, or nullptr.
[[nodiscard]] SymbolRecord const* typeAliasNamed(SemanticModel const& m,
                                                 std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i) {
        if (m.symbols()[i].name == name
            && m.symbols()[i].kind == DeclarationKind::Type) {
            return &m.symbols()[i];
        }
    }
    return nullptr;
}

} // namespace

// ── Adjacent-string-concat SHAPE (D-CSUBSET-ADJACENT-STRING-CONCAT) ──────────
//
// ★ RENAMED — D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN. These three tests were
// `SingleStringLiteralHasTwoChildren` / `TwoAdjacentStringsHaveFourChildren` /
// `ThreeAdjacentStringsHaveSixChildren`, i.e. their NAMES asserted 2/4/6. The
// closing `"` is now its own `StringEnd` token, so each adjacent piece
// contributes a TRIPLE and the counts are 3/6/9.
//
// They are not renamed to `…HasThreeChildren` — a literal count in a test name
// is exactly what went stale here, and would go stale again. The names now
// state the INVARIANT the grammar actually guarantees:
//
//     N adjacent string pieces  →  N token triples in ONE stringLiteralExpr
//
// and the per-piece width lives in `kTokensPerStringPiece` below, in ONE place,
// so a future shape change is a one-line edit with a compile-time-visible
// meaning rather than a hunt for magic numbers. A test named `HasTwoChildren`
// that asserts 3 is a landmine; so is one named `HasThreeChildren` if the shape
// moves again.
//
// The three children of each piece, in order:
//     [0] opener  — StringStart (or a wide/UTF variant: L" u" U" u8")
//     [1] body    — StringLiteral, the coalesced raw bytes BETWEEN delimiters
//     [2] closer  — StringEnd, the closing `"`     ← NEW; had no token before
//
// The closer MUST be a kind distinct from the body: `decodeAdjacentStringBodies`
// picks its segments by FILTERING children on the body kind, so a closer sharing
// that kind would decode `"abc"` as `abc"`. That is why these tests now pin the
// KIND SEQUENCE and not merely the count — a count of 3 is equally satisfied by
// `opener/body/body`, which is the corrupt shape.

// Each adjacent string piece contributes opener + body + closer.
// D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN took this from 2 to 3.
constexpr std::size_t kTokensPerStringPiece = 3;

// Regression: a LONE string literal produces EXACTLY one triple — the repeat
// fires zero times. RED if the grammar change perturbed the single-string shape.
TEST(ParserCSmoke, SingleStringLiteralHasOneTokenTriple) {
    Tree t = parseC("int main() { \"x\"; }");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    NodeId const sle = findFirstNodeWithRule(t, "stringLiteralExpr");
    ASSERT_TRUE(sle.valid()) << "a lone string must still form a stringLiteralExpr";
    EXPECT_EQ(visibleTokenChildCount(t, sle), 1 * kTokensPerStringPiece)
        << "lone string: opener + body + closer (repeat fires 0×)";
    EXPECT_EQ(visibleTokenChildKinds(t, sle), "StringStart/StringLiteral/StringEnd")
        << "the closer must be StringEnd, NOT a second StringLiteral — a "
           "body-kinded closer would make decodeAdjacentStringBodies read `x\"`";
}

// Two adjacent string literals concatenate into ONE stringLiteralExpr with TWO
// flat triples — no wrapper node. This is the shape `decodeAdjacentStringBodies`
// walks.
TEST(ParserCSmoke, TwoAdjacentStringsHaveTwoTokenTriples) {
    Tree t = parseC("int main() { \"a\" \"b\"; }");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    NodeId const sle = findFirstNodeWithRule(t, "stringLiteralExpr");
    ASSERT_TRUE(sle.valid());
    EXPECT_EQ(visibleTokenChildCount(t, sle), 2 * kTokensPerStringPiece)
        << "\"a\" \"b\" → 2 flat triples (the repeat fires once)";
    // ★ The closer had to be added to the grammar's REPEAT body as well as its
    // head. Omitting it there would not merely under-describe the tree — the
    // repeat would fail to match at the second piece's closer and the two
    // strings would stop concatenating. This kind sequence is what proves the
    // repeat consumed a FULL triple.
    EXPECT_EQ(visibleTokenChildKinds(t, sle),
              "StringStart/StringLiteral/StringEnd/StringStart/StringLiteral/StringEnd");
    // There must be exactly ONE stringLiteralExpr node — the second piece is
    // absorbed by the repeat, NOT a separate expression.
    std::size_t exprCount = 0;
    RuleId const rid = t.schema().rules().find("stringLiteralExpr");
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        NodeId const id{i};
        if (t.kind(id) == NodeKind::Internal && t.rule(id).v == rid.v) ++exprCount;
    }
    EXPECT_EQ(exprCount, 1u) << "adjacent strings form ONE expression, not two";
}

// Three adjacent string literals → THREE triples (the repeat fires twice).
TEST(ParserCSmoke, ThreeAdjacentStringsHaveThreeTokenTriples) {
    Tree t = parseC("int main() { \"a\" \"b\" \"c\"; }");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    NodeId const sle = findFirstNodeWithRule(t, "stringLiteralExpr");
    ASSERT_TRUE(sle.valid());
    EXPECT_EQ(visibleTokenChildCount(t, sle), 3 * kTokensPerStringPiece)
        << "\"a\" \"b\" \"c\" → 3 flat triples (repeat fires twice)";
    EXPECT_EQ(visibleTokenChildKinds(t, sle),
              "StringStart/StringLiteral/StringEnd/"
              "StringStart/StringLiteral/StringEnd/"
              "StringStart/StringLiteral/StringEnd");
}

// D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN, the CHAR form: `charLiteralExpr` went
// from 2 children to 3 for the same reason. Pinned here because the char form
// rides a DIFFERENT lexer mode and a DIFFERENT grammar rule from the string
// form — nothing above would catch a char-closer regression.
TEST(ParserCSmoke, CharLiteralHasOpenerBodyCloserChildren) {
    Tree t = parseC("int main() { 'c'; }");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    NodeId const cle = findFirstNodeWithRule(t, "charLiteralExpr");
    ASSERT_TRUE(cle.valid()) << "a char constant must form a charLiteralExpr";
    EXPECT_EQ(visibleTokenChildCount(t, cle), 3u)
        << "char constant: opener + body + closer";
    EXPECT_EQ(visibleTokenChildKinds(t, cle), "CharStart/CharLiteral/CharEnd")
        << "the char closer is CharEnd — a kind of its own, never the "
           "CharLiteral body kind reused";
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
TEST(ParserCSmoke, StatementVarDeclRidesTheDeclOrAttrStmtWrapper) {
    Tree t = parseC("int main() { int x; return x; }");
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
TEST(ParserCSmoke, FallthroughStatementParsesAsAttributeDeclaration) {
    Tree t = parseC(
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
TEST(ParserCSmoke, AttributedLocalDeclCommitsVarDeclBranch) {
    Tree t = parseC("int main() { [[maybe_unused]] int x = 5; return 0; }");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(hasInternalNodeWithRule(t, "varDecl"))
        << "the attributed declaration must commit the varDecl reading";
    EXPECT_FALSE(hasInternalNodeWithRule(t, "attributeDeclaration"))
        << "the attributeDeclaration branch must NOT win for a declaration";
}

// ── TF-C72 (D-CSUBSET-GNU-ATTRIBUTE): attribute decorations on a `typedef` ───
//
// A typedef may carry an attribute in three places, and NONE of them may be a
// child of the type-resolved head (`typedefHeadFull`) — see the anti-hijack pin
// at the bottom of this block for why that is a silent-miscompile hazard rather
// than a style preference. All three are therefore SIBLINGS at FIXED positions,
// which only works because `typedefDeclSpecifiers` is mandatory (it owns the
// `typedef` keyword, so it is always stripped) and each `typedefAttrRun` is a
// named rule over a lone `{repeat}` (so it emits its node even when EMPTY).
//
// `kCanonicalTypedefRoles` is that invariant written down: the shipped
// declarations row addresses this rule's children BY INDEX (`head: 0`,
// `declaratorList: 2`, POST-strip), so any change to this string is a change to
// what those indices mean. Every position test below re-pins it, because a
// decoration appearing in one slot must not shift the other slots.
//
// TF-C88 (D-CSUBSET-TYPEDEF-MULTI-DECLARATOR): slot 3 became
// `typedefDeclaratorList` — a comma-separated run of BARE declarators — and the
// row's key became `declaratorList: 2` in the SAME commit. The list rule replaces
// the declarator node 1:1 at the SAME position, which is why this is a one-token
// edit rather than an index move: `head: 0` and slot 2 are unchanged, and the
// three decoration-position tests below still assert exactly what they asserted
// before (a decoration in any slot shifts nothing).
constexpr std::string_view kCanonicalTypedefRoles =
    "rule:typedefDeclSpecifiers/rule:typedefHeadFull/rule:typedefAttrRun/"
    "rule:typedefDeclaratorList/rule:typedefAttrRun/tok:EndStatement";

// TRAILING (after the declarator) — the real SDK witness
// `bsm/audit.h`: `typedef u_int64_t au_asflgs_t __attribute__ ((aligned(8)));`
// The attribute lands in the SECOND `typedefAttrRun`; the first stays empty and
// `declarator` keeps role-index 2.
TEST(ParserCSmoke, TypedefTrailingAttributeParsesIntoTheSecondAttrRun) {
    Tree t = parseC(
        "typedef unsigned long long u_int64_t;\n"
        "typedef u_int64_t       au_asflgs_t __attribute__ ((aligned(8)));\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "bsm/audit.h must parse: " << firstErrorText(t);

    // The SECOND typedefDecl is the attributed one.
    NodeId attributed{};
    RuleId const typedefRule = t.schema().rules().find("typedefDecl");
    ASSERT_TRUE(typedefRule.valid());
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        NodeId const id{i};
        if (t.kind(id) == NodeKind::Internal && t.rule(id).v == typedefRule.v) {
            attributed = id;   // last wins
        }
    }
    ASSERT_TRUE(attributed.valid());
    EXPECT_EQ(visibleChildRoles(t, attributed), kCanonicalTypedefRoles)
        << "a trailing decoration must not shift head:0 / declarator:2";

    std::vector<NodeId> kids;
    for (NodeId c : t.children(attributed)) {
        if (!isEmptySpace(t.flags(c))) kids.push_back(c);
    }
    ASSERT_EQ(kids.size(), 6u);
    EXPECT_EQ(visibleChildRoles(t, kids[2]), "")
        << "the pre-declarator run must be EMPTY here (and still emit its node)";
    EXPECT_EQ(visibleChildRoles(t, kids[4]), "rule:attrSpec")
        << "the trailing __attribute__ must land in the post-declarator run";
}

// BETWEEN the head and the declarator — the real SDK witness
// `libkern/OSAtomicDeprecated.h`:
// `typedef int64_t __attribute__((__aligned__(8))) _OSAtomic_int64_t;`
// The attribute lands in the FIRST `typedefAttrRun` — a SIBLING of the head,
// never a child of it.
TEST(ParserCSmoke, TypedefMidAttributeParsesIntoTheFirstAttrRun) {
    Tree t = parseC(
        "typedef long int64_t;\n"
        "typedef int64_t __attribute__((__aligned__(8))) _OSAtomic_int64_t;\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "libkern/OSAtomicDeprecated.h must parse: " << firstErrorText(t);

    NodeId attributed{};
    RuleId const typedefRule = t.schema().rules().find("typedefDecl");
    ASSERT_TRUE(typedefRule.valid());
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        NodeId const id{i};
        if (t.kind(id) == NodeKind::Internal && t.rule(id).v == typedefRule.v) {
            attributed = id;
        }
    }
    ASSERT_TRUE(attributed.valid());
    EXPECT_EQ(visibleChildRoles(t, attributed), kCanonicalTypedefRoles)
        << "a mid-position decoration must not shift head:0 / declarator:2";

    std::vector<NodeId> kids;
    for (NodeId c : t.children(attributed)) {
        if (!isEmptySpace(t.flags(c))) kids.push_back(c);
    }
    ASSERT_EQ(kids.size(), 6u);
    EXPECT_EQ(visibleChildRoles(t, kids[2]), "rule:attrSpec")
        << "the decoration must land in the pre-declarator run";
    // ★ The structural half of the anti-hijack contract: the head subtree must
    // contain NO attribute node. The resolved-type half is the pin below.
    EXPECT_EQ(visibleChildRoles(t, kids[1]), "rule:typedefHead")
        << "the type-resolved head must hold ONLY the type — a decoration "
           "inside it is resolved as a candidate TYPE (silent miscompile)";
}

// AFTER the `typedef` keyword — the position that rides the stripped
// `typedefDeclSpecifiers` prefix (so the semantic specifier scans reach it).
TEST(ParserCSmoke, TypedefPostKeywordAttributeRidesTheSpecifierPrefix) {
    Tree t = parseC("typedef __attribute__((aligned(16))) long T;\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors()) << firstErrorText(t);

    NodeId const decl = findFirstNodeWithRule(t, "typedefDecl");
    ASSERT_TRUE(decl.valid());
    EXPECT_EQ(visibleChildRoles(t, decl), kCanonicalTypedefRoles)
        << "a post-keyword decoration must not shift head:0 / declarator:2";
    NodeId const prefix = findFirstNodeWithRule(t, "typedefDeclSpecifiers");
    ASSERT_TRUE(prefix.valid());
    EXPECT_EQ(visibleChildRoles(t, prefix), "tok:TypedefKeyword/rule:attrSpec")
        << "the keyword + decoration form the stripped specifier prefix";
}

// GAP 2 — `attrArgItem` accepts a `key = value` argument. Real SDK witness
// `sys/cdefs.h` (the `__swift_unavailable(_msg)` macro):
// `__attribute__((__availability__(swift, unavailable, message=_msg)))`.
// The assignment is an OPTIONAL TAIL on the item, so FIRST(attrArgItem) is
// unchanged and the comma list stays predictive.
TEST(ParserCSmoke, AttributeArgumentAcceptsKeyEqualsValue) {
    Tree t = parseC(
        "int f(void) __attribute__((__availability__(swift, unavailable, "
        "message=\"unavailable in Swift\")));\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "sys/cdefs.h must parse: " << firstErrorText(t);

    // Exactly one of the three arg items carries the `=` tail, and its shape is
    // atom/`=`/atom — pinned so a future edit cannot quietly turn the tail into
    // a separate alt (which would change FIRST(attrArgItem) and the pruning).
    RuleId const itemRule = t.schema().rules().find("attrArgItem");
    ASSERT_TRUE(itemRule.valid());
    int assigned = 0;
    int plain    = 0;
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        NodeId const id{i};
        if (t.kind(id) != NodeKind::Internal || t.rule(id).v != itemRule.v) continue;
        std::string const roles = visibleChildRoles(t, id);
        if (roles == "rule:attrArgAtom/tok:AssignOp/rule:attrArgAtom") {
            ++assigned;
        } else if (roles == "rule:attrArgAtom") {
            ++plain;
        } else {
            ADD_FAILURE() << "unexpected attrArgItem shape: " << roles;
        }
    }
    EXPECT_EQ(assigned, 1) << "`message=\"...\"` is the one assigned argument";
    EXPECT_EQ(plain, 2) << "`swift` and `unavailable` stay plain atoms";
}

// ★★ THE ANTI-HIJACK PIN — the most important test in this block.
//
// `resolveTypeNodeImpl` picks a declaration's head type FIRST-CHILD-THAT-
// RESOLVES-WINS, and its token arm resolves ANY identifier through the scope
// chain as a possible type alias. So an attribute IDENTIFIER placed inside
// `typedefHeadFull` is tried AS A TYPE before the real type specifier is
// reached: with `typedef int aligned;` in scope, `__attribute__((aligned(16)))`
// would make `T` resolve to **int** instead of **long**. That is a silent
// miscompile — the program keeps compiling, with the wrong type.
//
// This pin is what stops anyone re-introducing the head-decoration design. It
// asserts the RESOLVED TYPE, not that the input parses: a parse-only check
// stays green through exactly the defect it is meant to catch. `T`'s interned
// TypeId must equal a control `typedef long` in the same TU (interned types are
// pointer-equal), and must NOT equal the decoy `aligned` alias.
// ★ TF-C73 — THE ALIGNMENT VALUE IS `8`, NOT `16`, AND THAT IS DELIBERATE.
// This pin exists to prove an attribute does not HIJACK THE TYPE HEAD. It is
// not about alignment at all; the attribute is a vehicle, chosen because
// `aligned` can also be a plausible typedef name (hence the decoy).
//
// TF-C73 gave `aligned` a real sink (`attributeSemantics.effects` effect
// `align`), and with it the C 6.7.5-style validation that an explicit
// alignment must not be WEAKER than natural — plus, for a typedef, that the
// alias resolves to the same type as its aliasee and so cannot carry a
// STRONGER one either. Under LP64 `long` is 8-byte natural, so the original
// `aligned(16)` is now a diagnosable request: MEASURED through the real CLI,
// all three spellings fail `S002F __attribute__((aligned(16))) on a typedef
// cannot be honored: the alias resolves to the same type as its aliasee,
// whose alignment is 8`. (It does NOT fire in this harness today — the check
// needs a TARGET to know `long` is 8 wide, and the semantic-only pipeline has
// none — which is exactly why it must be fixed HERE rather than left to break
// later for a reason that has nothing to do with what this test guards.)
//
// `aligned(8)` == natural is a proven no-op: it exercises the identical
// grammar in all three positions while asserting nothing about alignment. The
// TYPE assertions below are untouched — weakening them would gut the pin.
TEST(ParserCSmoke, TypedefAttributeMustNotHijackTheHeadType) {
    SemanticModel const m = analyzeC(
        "typedef int aligned;\n"                             // the decoy alias
        "typedef long CONTROL;\n"                            // the ground truth
        "typedef __attribute__((aligned(8))) long POST_KW;\n"
        "typedef long __attribute__((aligned(8))) MID;\n"
        "typedef long TRAILING __attribute__((aligned(8)));\n");
    EXPECT_FALSE(m.hasErrors())
        << (m.diagnostics().all().empty() ? "" : m.diagnostics().all()[0].actual);

    SymbolRecord const* decoy   = typeAliasNamed(m, "aligned");
    SymbolRecord const* control = typeAliasNamed(m, "CONTROL");
    ASSERT_NE(decoy, nullptr);
    ASSERT_NE(control, nullptr);
    ASSERT_TRUE(decoy->type.valid());
    ASSERT_TRUE(control->type.valid());
    ASSERT_NE(decoy->type.v, control->type.v)
        << "the decoy and the control must be DIFFERENT types, or this pin "
           "cannot distinguish a hijack from a correct resolution";

    for (std::string_view name : {"POST_KW", "MID", "TRAILING"}) {
        SymbolRecord const* rec = typeAliasNamed(m, name);
        ASSERT_NE(rec, nullptr) << name << " must bind a type alias";
        ASSERT_TRUE(rec->type.valid()) << name << " must resolve";
        EXPECT_EQ(rec->type.v, control->type.v)
            << name << " must resolve to `long`, not to whatever the attribute "
                       "identifier happens to name";
        EXPECT_NE(rec->type.v, decoy->type.v)
            << name << " resolved to the `aligned` DECOY type — the attribute "
                       "hijacked the head (silent miscompile)";
        EXPECT_EQ(m.lattice().interner().kind(rec->type), TypeKind::I64)
            << name << " must be I64 (long under LP64), not I32 (int)";
    }
}

// ── TF-C73 (D-CSUBSET-GNU-ATTRIBUTE): two NEW attribute positions ────────────
//
// The SDK audit measured 204 `aligned` sites and split them typedef 99 (48.5%)
// / struct-union definition 64 (31.4%) / member 36 (17.6%). The typedef slots
// existed and were parse-and-ignore (TF-C72); this cycle gave them a sink. The
// MEMBER position did not parse at all, and the C23 `[[gnu::aligned(8)]]`
// spelling did not parse in ANY position. Both are closed below.
//
// Every C snippet in this block was verified with
//   clang -fsyntax-only -Wall -Wextra -std=c2x -isysroot $(xcrun --show-sdk-path)
// and is CLEAN — no warnings, no errors. That matters because a pin written
// against invalid C proves nothing about the grammar it claims to exercise.

// `structField`'s child layout with NO decoration — the control half of the
// index-preservation contract. The declaration row reads `head: 0` /
// `declaratorList: 1` POST-STRIP, so this string is what those indices mean.
constexpr std::string_view kUndecoratedMemberRoles =
    "rule:typeRefAllowingStruct/rule:structMemberDeclaratorList/tok:EndStatement";

// …and WITH one. `structMemberAttrList` is a TRAILING `{optional}`, so it
// appends at the end and the two role indices above are untouched. If it were
// an always-emitted node instead, it would land at index 1 whenever the
// (optional) declarator list is absent — the `int ;` declares-nothing form —
// and `declaratorList: 1` would silently address an attribute run.
constexpr std::string_view kDecoratedMemberRoles =
    "rule:typeRefAllowingStruct/rule:structMemberDeclaratorList/"
    "rule:structMemberAttrList/tok:EndStatement";

TEST(ParserCSmoke, UndecoratedStructMemberKeepsItsChildLayout) {
    Tree t = parseC("struct S { int x; };\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors()) << firstErrorText(t);
    NodeId const field = findFirstNodeWithRule(t, "structField");
    ASSERT_TRUE(field.valid());
    EXPECT_EQ(visibleChildRoles(t, field), kUndecoratedMemberRoles)
        << "an undecorated member must keep head:0 / declaratorList:1";
}

// RED-ON-DISABLE (measured): remove `{ \"optional\": \"structMemberAttrList\" }`
// from `structField` and this input reports, through the real CLI,
//   error[P0001]: expected 'EndStatement' — got '__attribute__'
//   error[P0001]: expected 'EndStatement' — got 'aligned'
TEST(ParserCSmoke, StructMemberAttributeParsesIntoTheMemberAttrList) {
    Tree t = parseC("struct S { int x __attribute__((aligned(8))); };\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "a member decoration must parse: " << firstErrorText(t);

    NodeId const field = findFirstNodeWithRule(t, "structField");
    ASSERT_TRUE(field.valid());
    EXPECT_EQ(visibleChildRoles(t, field), kDecoratedMemberRoles)
        << "the decoration must APPEND — head:0 / declaratorList:1 unmoved";

    // The run must be a DIRECT child of the declaration node, because that is
    // exactly what `declarationAttrSlotRules` matches. Nested one level deeper
    // (inside `structMemberDeclarator`, where GNU's per-declarator binding
    // would put it) the scan cannot see it and the alignment is silently
    // dropped — so this is a structural pin on the honoring path, not on shape
    // for its own sake.
    NodeId const run = findFirstNodeWithRule(t, "structMemberAttrList");
    ASSERT_TRUE(run.valid());
    EXPECT_EQ(t.parent(run).v, field.v)
        << "structMemberAttrList must be a DIRECT child of structField — a "
           "deeper nesting is invisible to declarationAttrSlotRules";
    EXPECT_EQ(visibleChildRoles(t, run), "rule:attrSpec")
        << "the GNU attribute is the run's sole entry";
}

TEST(ParserCSmoke, UnionMemberAttributeParsesIntoTheMemberAttrList) {
    Tree t = parseC(
        "union U { int x __attribute__((aligned(8))); char c; };\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "a union member decoration must parse: " << firstErrorText(t);
    NodeId const field = findFirstNodeWithRule(t, "unionField");
    ASSERT_TRUE(field.valid());
    EXPECT_EQ(visibleChildRoles(t, field), kDecoratedMemberRoles)
        << "unionField mirrors structField exactly";
}

// A bit-field member must be untouched: the run sits AFTER the declarator
// list, so `structMemberDeclarator`'s own [declarator?, bitfieldDeclSuffix?]
// layout — which the `bitfieldSuffix` role and the anonymous `int : 3;`
// reading both depend on — is not disturbed.
TEST(ParserCSmoke, MemberAttrListDoesNotDisturbBitfieldMembers) {
    Tree t = parseC("struct S { int a : 3, b : 5; int : 0; };\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors()) << firstErrorText(t);
    EXPECT_FALSE(hasInternalNodeWithRule(t, "structMemberAttrList"))
        << "no decoration is present, so the optional run must emit NOTHING";
    EXPECT_TRUE(hasInternalNodeWithRule(t, "bitfieldDeclSuffix"));
}

// NAMED RESIDUE, pinned so it stays LOUD rather than drifting into a silent
// mis-parse. `structMemberAttrList` admits the after-LIST position only, so a
// MID-LIST decoration is rejected. The input is valid C (clang accepts it
// clean) — this pins DSS's narrower admission, not a claim about the language.
TEST(ParserCSmoke, MidListMemberAttributeFailsLoud) {
    Tree t = parseC(
        "struct S { int x __attribute__((aligned(8))), y; };\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_TRUE(t.diagnostics().hasErrors())
        << "a MID-LIST member decoration is residue — it must fail loud, "
           "never parse into a shape that drops the attribute";
}

// ── TF-C94: the LEADING member attribute position (`structMemberDeclSpecifier`)
//
// The Tcl-9 gap. `struct TclStubs { TCL_NORETURN1 void (*tcl_Panic)(…); … }`
// (tclDecls.h, the `tcl.h` macro
// `#define TCL_NORETURN1 __attribute__ ((__noreturn__))`) put a GNU attribute
// BEFORE a member's declaration-specifiers, which GNU 6.34 permits on any
// declaration. RED-ON-DISABLE (measured through the real CLI at the pre-change
// HEAD): restore `"sequence": [ "alignasSpec", { "repeat": "alignasSpec" } ]` on
// `structMemberDeclSpecifiers` and this input reports
//   error[P0009]: expected 'Identifier', 'BlockClose', 'VoidKeyword', … — got
//                 '__attribute__'
// plus two cascading `— got '('`, all with `scope: Block`.
//
// The prefix is child 0 and is STRIPPED by decl_prefix_strip.hpp before
// positional counting, so the structField row's `head: 0` / `declaratorList: 1`
// are unmoved — the same contract `structMemberDeclSpecifiers` already had for
// `alignas`. That is what the role string pins: the attribute must ride the
// EXISTING prefix rather than becoming a new positional child.
constexpr std::string_view kLeadDecoratedMemberRoles =
    "rule:structMemberDeclSpecifiers/rule:typeRefAllowingStruct/"
    "rule:structMemberDeclaratorList/tok:EndStatement";

TEST(ParserCSmoke, StructMemberLeadingGnuAttributeRidesTheSpecifierPrefix) {
    Tree t = parseC(
        "struct S { __attribute__((__noreturn__)) void (*p)(int); };\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "the Tcl 9 TclStubs member shape must parse: " << firstErrorText(t);

    NodeId const field = findFirstNodeWithRule(t, "structField");
    ASSERT_TRUE(field.valid());
    EXPECT_EQ(visibleChildRoles(t, field), kLeadDecoratedMemberRoles)
        << "the leading decoration must ride the specifier PREFIX (child 0, "
           "stripped) — head/declaratorList must not shift";

    // ★ THE HONORING PIN, not a shape pin. `specifierPrefixChild` returns this
    // node, and `specifierPrefixNamesNoreturn` / `scanAttributeSemantics` /
    // `firstAlignasSpecInPrefix` all walk it. An attrSpec parked anywhere else
    // would parse and be invisible to every one of them.
    NodeId const prefix = findFirstNodeWithRule(t, "structMemberDeclSpecifiers");
    ASSERT_TRUE(prefix.valid());
    EXPECT_EQ(t.parent(prefix).v, field.v)
        << "structMemberDeclSpecifiers must be a DIRECT child of structField — "
           "that is what specifierPrefix matches";
    EXPECT_EQ(visibleChildRoles(t, prefix), "rule:structMemberDeclSpecifier")
        << "one specifier in the run";
    NodeId const one = findFirstNodeWithRule(t, "structMemberDeclSpecifier");
    ASSERT_TRUE(one.valid());
    EXPECT_EQ(t.parent(one).v, prefix.v);
    EXPECT_EQ(visibleChildRoles(t, one), "rule:attrSpec")
        << "the alt must have committed to the GNU attribute branch";
}

TEST(ParserCSmoke, UnionMemberLeadingGnuAttributeMirrorsStruct) {
    Tree t = parseC(
        "union U { __attribute__((__noreturn__)) void (*p)(int); int x; };\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "unionField shares the member prefix rule: " << firstErrorText(t);
    NodeId const field = findFirstNodeWithRule(t, "unionField");
    ASSERT_TRUE(field.valid());
    EXPECT_EQ(visibleChildRoles(t, field), kLeadDecoratedMemberRoles)
        << "unionField mirrors structField exactly — one attribute must not mean "
           "two things depending on which composite it is written in";
}

// REGRESSION: the rule that grew the alt still parses its ORIGINAL content.
// `alignas` now reaches the prefix through `structMemberDeclSpecifier` instead
// of directly, and the extra wrapper level must be transparent (every prefix
// consumer is a bounded descendant walk, never a fixed-child-index read).
TEST(ParserCSmoke, StructMemberLeadingAlignasStillRidesTheSamePrefix) {
    Tree t = parseC("struct S { alignas(16) int a; };\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors()) << firstErrorText(t);
    NodeId const prefix = findFirstNodeWithRule(t, "structMemberDeclSpecifiers");
    ASSERT_TRUE(prefix.valid());
    EXPECT_EQ(visibleChildRoles(t, prefix), "rule:structMemberDeclSpecifier");
    NodeId const one = findFirstNodeWithRule(t, "structMemberDeclSpecifier");
    ASSERT_TRUE(one.valid());
    EXPECT_EQ(visibleChildRoles(t, one), "rule:alignasSpec")
        << "the alignment branch of the alt must still be reachable";
}

// The two member slots are INDEPENDENT: a leading decoration and the shipped
// TF-C73 trailing one coexist on one member, each in its own node, and the
// declarator list still sits between them at role index 1 post-strip.
TEST(ParserCSmoke, MemberLeadingAndTrailingAttributeSlotsCoexist) {
    Tree t = parseC(
        "struct S { __attribute__((aligned(8))) int x __attribute__((aligned(16))); };\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors()) << firstErrorText(t);
    NodeId const field = findFirstNodeWithRule(t, "structField");
    ASSERT_TRUE(field.valid());
    EXPECT_EQ(visibleChildRoles(t, field),
              "rule:structMemberDeclSpecifiers/rule:typeRefAllowingStruct/"
              "rule:structMemberDeclaratorList/rule:structMemberAttrList/"
              "tok:EndStatement")
        << "leading prefix + trailing run must both be present and distinct";
}

// A member with NO leading decoration must emit NO prefix node — the
// `{ optional }` stays absent, so the post-strip indices of the undecorated
// form are byte-identical to what they were before this cycle.
TEST(ParserCSmoke, UndecoratedMemberEmitsNoLeadingSpecifierPrefix) {
    Tree t = parseC("struct S { int x; };\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors()) << firstErrorText(t);
    EXPECT_FALSE(hasInternalNodeWithRule(t, "structMemberDeclSpecifiers"))
        << "no leading specifier is present, so the optional must emit NOTHING";
}

// NAMED RESIDUE, pinned so it stays LOUD rather than drifting into a silent
// mis-parse. `structMemberDeclSpecifier` admits the GNU `attrSpec` only — the
// C23 `[[…]]` spelling is excluded to keep BracketOpen out of the prefix's
// FIRST set. The input is valid C23 (clang accepts AND honors it, measured) —
// this pins DSS's narrower admission, not a claim about the language.
TEST(ParserCSmoke, LeadingStdAttributeOnAMemberFailsLoud) {
    Tree t = parseC("struct S { [[deprecated]] int x; };\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_TRUE(t.diagnostics().hasErrors())
        << "a leading C23 member attribute is residue — it must fail loud, "
           "never parse into a shape that drops the attribute";
}

// ── TF-C73: `stdAttrItem`'s argument is now the shared `attrArgs` rule ───────
//
// RED-ON-DISABLE (measured): restore the inline
// `( { optional: stringLiteralExpr } )` argument and this input reports,
// through the real CLI,
//   error[P0009]: expected 'StringStart', 'ParenClose', 'WideStringStart',
//                 'Utf32StringStart', 'Utf16StringStart' or 'Utf8StringStart'
//                 — got '8'
// i.e. the C23 spelling of the SINGLE most common SDK attribute could not be
// written at all, in either direction, with zero coverage proving it.
TEST(ParserCSmoke, StdAttributeAcceptsANonStringArgument) {
    Tree t = parseC("typedef long ALT [[gnu::aligned(8)]];\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "[[gnu::aligned(8)]] must parse: " << firstErrorText(t);

    NodeId const item = findFirstNodeWithRule(t, "stdAttrItem");
    ASSERT_TRUE(item.valid());
    EXPECT_EQ(visibleChildRoles(t, item),
              "tok:Identifier/tok:ColonColonOp/tok:Identifier/rule:attrArgs")
        << "the namespaced name stays two DIRECT identifier children (the "
           "clause-name reading) and the argument is one attrArgs subtree";
}

TEST(ParserCSmoke, StdAttributeAcceptsALeadingNonStringArgument) {
    Tree t = parseC("[[gnu::aligned(8)]] int gv;\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "the C23 leading position must parse too: " << firstErrorText(t);
}

// The regression this reshape could plausibly cause, pinned explicitly: the
// `[[deprecated("m")]]` message moved from a DIRECT child of `stdAttrItem` to
// `attrArgs > attrArgList > attrArgItem > attrArgAtom > stringLiteralExpr`.
// The message search is depth-agnostic by construction, so the string must
// still be reachable — and it must still be exactly ONE stringLiteralExpr, not
// split or duplicated by the extra wrapper levels.
TEST(ParserCSmoke, StdAttributeStringArgumentSurvivesTheAttrArgsReshape) {
    Tree t = parseC("[[deprecated(\"use g\")]] int old_g;\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors()) << firstErrorText(t);

    NodeId const item = findFirstNodeWithRule(t, "stdAttrItem");
    ASSERT_TRUE(item.valid());
    EXPECT_EQ(visibleChildRoles(t, item), "tok:Identifier/rule:attrArgs")
        << "the un-namespaced name plus one attrArgs argument subtree";

    RuleId const strRule = t.schema().rules().find("stringLiteralExpr");
    ASSERT_TRUE(strRule.valid());
    int strings = 0;
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        NodeId const id{i};
        if (t.kind(id) == NodeKind::Internal && t.rule(id).v == strRule.v) {
            ++strings;
        }
    }
    EXPECT_EQ(strings, 1)
        << "exactly one string literal — the message, still one decodable unit";
}

// ── TF-C73: the file-scope GNU `aligned` now reaches its sink ────────────────
//
// RED-ON-DISABLE (measured): drop `\"aligned\"` from `topLevelDecl`'s
// `linkageSpecifierIgnoredNames` and this input reports, through the real CLI,
//   error[H000C]: 'aligned' is not a recognized linkage specifier
// The linkage tier runs BEFORE the semantic attribute scan, so without that
// name the `align` effect row is UNREACHABLE — the two edits are a matched
// pair and neither works alone.
TEST(ParserCSmoke, FileScopeGnuAlignedParsesCleanly) {
    Tree t = parseC("__attribute__((aligned(8))) int gv;\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors()) << firstErrorText(t);
    EXPECT_TRUE(hasInternalNodeWithRule(t, "attrSpec"));
}

// ── TF-C73: the AFTER-KEYWORD composite attribute slot ──────────────────────
//
// `struct __attribute__((aligned(16))) T { … };` — `mach-o/dyld_images.h`,
// and 64 of the SDK audit's 204 `aligned` sites (31.4%). Both shapes below are
// clang-clean under
//   clang -fsyntax-only -Wall -Wextra -isysroot $(xcrun --show-sdk-path)
//
// This slot landed one cycle AFTER the grammar for it was first written: the
// composite attribute scan then read a single surface, so a lead slot under a
// distinct rule name was invisible to it and a leading `packed` was silently
// dropped (sizeof 8 against clang's 5). The grammar was reverted unshipped and
// waited for the consumer. See the config's `$compositeAttrLeadLandedComment`.

// `structSpec`'s child layout is FIXED by `compositeAttrLead` being a named
// rule over a lone `{repeat}` — emitted whether or not a decoration is present.
// The declarations row reads `name: 2`, so this string is what that index means.
constexpr std::string_view kTaggedStructRoles =
    "tok:StructKeyword/rule:compositeAttrLead/tok:Identifier/rule:structBody";

// RED-ON-DISABLE (measured): remove `compositeAttrLead` from `structSpec` and
// this input no longer fails at the parse tier at all — it MIS-parses, and the
// real CLI reports `S0018 a function definition's declarator must be a function
// declarator`, a diagnostic naming a construct the source does not contain.
// (`structSpec` swallows the attribute with its TRAILING optional list, leaving
// `T { int x; }` to read as a declarator plus block tail.)
TEST(ParserCSmoke, AfterKeywordCompositeAttributeParsesIntoTheLeadSlot) {
    Tree t = parseC("struct __attribute__((aligned(16))) T { int x; };\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "mach-o/dyld_images.h must parse: " << firstErrorText(t);

    NodeId const spec = findFirstNodeWithRule(t, "structSpec");
    ASSERT_TRUE(spec.valid());
    EXPECT_EQ(visibleChildRoles(t, spec), kTaggedStructRoles)
        << "the decoration must not shift the tag off name:2";

    NodeId const lead = findFirstNodeWithRule(t, "compositeAttrLead");
    ASSERT_TRUE(lead.valid());
    EXPECT_EQ(t.parent(lead).v, spec.v)
        << "compositeAttrLead must be a DIRECT child of structSpec — that is "
           "what declarationAttrSlotRules matches, and what the composite "
           "attribute scan walks to reach the lead surface";
    EXPECT_EQ(visibleChildRoles(t, lead), "rule:compositeAttr");
}

// ★ The index half, and the reason `compositeAttrLead` is not an `{optional}`.
// An UNdecorated struct must produce the SAME child layout, or `name: 2` means
// one thing here and another there — and the failure is silent (the tag binds
// as anonymous). MEASURED with `name` set back to 1: `struct T v; … v.x` gives
// `S0028` on `v` then `S000D member access '.' requires a composite-typed
// operand` — a type error at the USE site, several steps from the cause.
TEST(ParserCSmoke, UndecoratedStructEmitsTheSameLeadSlotNode) {
    Tree t = parseC("struct T { int x; };\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors()) << firstErrorText(t);
    NodeId const spec = findFirstNodeWithRule(t, "structSpec");
    ASSERT_TRUE(spec.valid());
    EXPECT_EQ(visibleChildRoles(t, spec), kTaggedStructRoles)
        << "the lead slot must emit its node even when EMPTY — that is what "
           "makes name:2 constant";
    NodeId const lead = findFirstNodeWithRule(t, "compositeAttrLead");
    ASSERT_TRUE(lead.valid());
    EXPECT_EQ(visibleChildRoles(t, lead), "")
        << "…and it must be empty here";
}

// The ANONYMOUS forms keep their kind: child 2 is the body, a non-identifier
// node, so `anonymousNameAllowed` synthesizes the name exactly as it did when
// child 1 was the body.
TEST(ParserCSmoke, AnonymousStructWithLeadAttributeKeepsBodyAtNameIndex) {
    Tree t = parseC(
        "struct __attribute__((aligned(16))) { int x; } v;\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors()) << firstErrorText(t);
    NodeId const spec = findFirstNodeWithRule(t, "structSpec");
    ASSERT_TRUE(spec.valid());
    EXPECT_EQ(visibleChildRoles(t, spec),
              "tok:StructKeyword/rule:compositeAttrLead/rule:structBody")
        << "no tag: index 2 is the body, so the anonymous path is unchanged";
}

TEST(ParserCSmoke, AfterKeywordUnionAttributeParsesIntoTheLeadSlot) {
    Tree t = parseC("union __attribute__((aligned(16))) U { int x; };\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "unionSpec mirrors structSpec: " << firstErrorText(t);
    NodeId const spec = findFirstNodeWithRule(t, "unionSpec");
    ASSERT_TRUE(spec.valid());
    EXPECT_EQ(visibleChildRoles(t, spec),
              "tok:UnionKeyword/rule:compositeAttrLead/tok:Identifier/rule:unionBody");
}

// The TRAILING composite position, which parsed all along but died in the
// composite scan as an unrecognized type attribute (`S0031`) because `aligned`
// had no sink there. Pinned at the parse tier because the shape is the one the
// SDK actually writes for an anonymous-struct typedef.
TEST(ParserCSmoke, TrailingCompositeAlignedOnAnonymousStructTypedefParses) {
    Tree t = parseC(
        "typedef struct { int x; } __attribute__((aligned(16))) T2;\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors()) << firstErrorText(t);
    NodeId const spec = findFirstNodeWithRule(t, "structSpec");
    ASSERT_TRUE(spec.valid());
    EXPECT_EQ(visibleChildRoles(t, spec),
              "tok:StructKeyword/rule:compositeAttrLead/rule:structBody/"
              "rule:compositeAttrList")
        << "an EMPTY lead slot plus the trailing list — the two surfaces are "
           "distinct nodes, which is what lets the scan read both";
}

// The lead slot must not steal a REFERENCE's tag either (`struct S v;`), where
// the reference row resolves the tag by a lastIdentifier DFS over the whole
// spec node rather than by index.
TEST(ParserCSmoke, TagReferenceKeepsItsShapeWithTheLeadSlot) {
    Tree t = parseC("struct S; struct S *p;\n");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors()) << firstErrorText(t);
    NodeId const spec = findFirstNodeWithRule(t, "structSpec");
    ASSERT_TRUE(spec.valid());
    EXPECT_EQ(visibleChildRoles(t, spec),
              "tok:StructKeyword/rule:compositeAttrLead/tok:Identifier")
        << "a bare tag reference: lead slot present-and-empty, tag still at 2";
}

// ════════════════════════════════════════════════════════════════════════════
// TF-C77 — D-CSUBSET-ATTRIBUTE-LEADING-WITH-STORAGE-CLASS, the PARSE tier.
// One test per FORM. The APPLIED-FACT pins live in the HIR and semantic suites;
// these guard the shapes themselves and — just as importantly — the shapes that
// must STAY loud.
// ════════════════════════════════════════════════════════════════════════════

// MODE 1: the attribute rides INSIDE `externSpecifiers`, after the keyword.
TEST(ParserCSmoke, ExternHeadAttributeParses) {
    for (char const* src : {
             "extern __attribute__((__noreturn__)) void die(int);",
             "extern __attribute__((weak)) int wk;",
             "extern __attribute__((visibility(\"hidden\"))) int ev;",
             "extern __attribute__((__nothrow__, __leaf__)) int gg(void);",
             "extern _Thread_local __attribute__((weak)) int t1;",
             "extern __attribute__((weak)) _Thread_local int t2;",
             "extern __attribute__((weak)) int wfun(void) { return 1; }"}) {
        auto h = loadAndTokenize(src);
        Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
        auto result = std::move(p).parse();
        EXPECT_FALSE(result.tree.diagnostics().hasErrors()) << src;
    }
}

// MODE 2: the `declAttrRun` slot, every shape that can occupy it.
TEST(ParserCSmoke, MidPositionAttributeParses) {
    for (char const* src : {
             "int __attribute__((weak)) gv;",
             "int __attribute__((weak)) gv = 3;",
             "int __attribute__((weak)) a = 1, b = 2;",
             "int __attribute__((aligned(32))) av = 20;",
             "static int __attribute__((cold)) sf(int x){ return x + 1; }",
             "int __attribute__((weak)) wp(void);",
             "int main(void){ int __attribute__((aligned(16))) x = 1; return x; }",
             "int main(void){ struct S { int a; } __attribute__((packed)) s; "
             "return s.a; }"}) {
        auto h = loadAndTokenize(src);
        Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
        auto result = std::move(p).parse();
        EXPECT_FALSE(result.tree.diagnostics().hasErrors()) << src;
    }
}

// ★★ THE `stdAttr` EXCLUSION — AND IT IS THE MOST IMPORTANT TEST IN THIS BLOCK,
// because it is the one that can go green the WRONG way.
//
// Adding `stdAttr` to either new slot would be a one-word config edit and would
// make both of these compile. It must NOT: MEASURED with
// `/usr/bin/clang -std=c23 -fsyntax-only`, real clang REJECTS both —
//   `extern [[deprecated]] int dg;`
//       error: an attribute list cannot appear here
//   `int [[deprecated]] gv;`
//       error: 'deprecated' attribute cannot be applied to types
// (after a type-specifier a `[[…]]` appertains to the TYPE, not the declaration).
// DSS accepting C that no toolchain accepts is a defect in the permissive
// direction, and the permissive direction is the one nobody notices.
//
// RED-ON-DISABLE: add `stdAttr` beside `attrSpec` in `declAttrRun` or in
// `externSpecifiers`'s repeat alt → the corresponding line parses clean and this
// test fails. That is the demonstration; keep it.
// ⚠⚠ P53 CORRECTION, BY MEASUREMENT
// (D-C-EXTERN-MUST-LEAD-THE-DECLARATION-SPECIFIERS). The mode-1 line used to be
// written at FILE scope, where the exclusion belonged to `externSpecifiers`'s
// repeat. That rule no longer governs file scope, so the line has MOVED into a
// block — where `externDecl` still owns it and the exclusion is still real.
//
// ★ AND THE FILE-SCOPE ANSWER CHANGED, WHICH THIS COMMENT STATES RATHER THAN
// HIDES: `extern [[deprecated]] int dg;` at file scope now PARSES, because
// `stdAttr` has always been one of `singleDeclSpecifier`'s alts and `extern` has
// joined them. ⓘ THAT IS NOT A NEW CLASS — it makes a PRE-EXISTING permissiveness
// UNIFORM. ✔MEASURED 2026-09-02 through the shipped CLI at P53's base, before
// any of this row's edits: `static [[deprecated]] int sg = 1;` and
// `_Thread_local [[deprecated]] int tg;` ALREADY compiled rc 0, and gcc 13.3.0
// and clang 18.1.3 REFUSE all three ("an attribute list cannot appear here";
// C23 puts the `[[…]]` sequence BEFORE the declaration specifiers, not among
// them). So DSS was above the union on every storage-class specifier EXCEPT
// `extern`, and strict there only because `extern` happened to be a rule head —
// the same accident C 6.7.1p2 was being enforced by. The merge removes the
// accident; the divergence is recorded on this row's cells and belongs to
// [[D-CSUBSET-ATTRIBUTE-TYPE-POSITION]], which already owns the C23
// attribute-position question and already needs an engine capability to fix it.
// Do NOT "repair" it by deleting `stdAttr` from `singleDeclSpecifier`: that
// would refuse `[[deprecated]] int gv;`, which BOTH references accept.
TEST(ParserCSmoke, StdAttrStaysRejectedInBothNewSlots) {
    for (char const* src : {
             // mode 1 slot — `externSpecifiers`'s repeat, at the block scope
             // that is now the only place that rule is reachable.
             "int use(void){ extern [[deprecated]] int dg; return dg; }",
             "int [[deprecated]] gv;"}) {       // mode 2 slot
        auto h = loadAndTokenize(src);
        Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
        auto result = std::move(p).parse();
        EXPECT_TRUE(result.tree.diagnostics().hasErrors())
            << src << " — real clang rejects this; admitting `stdAttr` into the "
                      "new slots would make DSS accept C no toolchain does";
    }
}

// ★ THE ANTI-HIJACK PIN. `declAttrRun` is a SIBLING of the head, never a child.
// Nested inside the head, `resolveTypeNodeImpl`'s first-child-that-resolves-wins
// token arm would try the attribute identifier as a TYPE — and here `aligned` and
// `weak` are deliberately made real typedef names, so a hijack would bind the
// wrong type SILENTLY. `long` vs `int` makes the difference observable as a size.
// (The applied-type half is asserted in the semantic + HIR suites; this arm pins
// that the shape parses at all with the collision present, which is the parse
// tier's share of the guard.)
TEST(ParserCSmoke, MidPositionAttributeNameCollidingWithATypedefParses) {
    for (char const* src : {
             "typedef long weak;\nint __attribute__((weak)) gv = 3;",
             "typedef long aligned;\nint __attribute__((aligned(4))) av = 3;",
             "typedef long aligned;\nint main(void){ "
             "int __attribute__((aligned(4))) x = 1; return x; }"}) {
        auto h = loadAndTokenize(src);
        Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
        auto result = std::move(p).parse();
        EXPECT_FALSE(result.tree.diagnostics().hasErrors()) << src;
    }
}

// ★★★ THE ORDER WALL IS GONE, AND ITS REMOVAL IS THE POINT — P53
// (D-C-EXTERN-MUST-LEAD-THE-DECLARATION-SPECIFIERS), which is the escape (ii)
// that [[D-CSUBSET-ATTRIBUTE-BEFORE-EXTERN-KEYWORD]]'s own closing cell
// prescribes.
//
// This test used to assert the OPPOSITE — that `__attribute__((weak)) extern
// int g;` must stay a clean parse ERROR — on the grounds that admitting it
// would put AttributeKeyword into FIRST(externDecl) and collide with
// `topLevelDecl` at `/shapes/topLevel`. That reasoning was correct for a tree
// with TWO top-level declaration rules. There is now ONE: `externDecl` left
// `/shapes/topLevel`, `extern` is an ordinary `singleDeclSpecifier`, and an
// attribute before it is simply an earlier member of the same specifier run —
// no new FIRST, no collision, nothing to detect.
//
// ★ IT IS A CONFORMANCE GAIN, NOT A TOLERATED SIDE EFFECT. ✔MEASURED
// 2026-09-02, gcc 13.3.0 `-std=c2x` and clang 18.1.3 `-std=c23` probed
// SEPARATELY: BOTH ACCEPT `__attribute__((weak)) extern int g;` and
// `__attribute__((weak)) extern int f(int);` at rc 0, so the old refusal sat
// BELOW the reference union. The C23 spelling `[[deprecated]] extern int eg;`
// is accepted by both too, and now parses here as well.
// ⓘ `__attribute__((weak)) static int g;` is a DIFFERENT question and both
// references still refuse it ("weak declaration of 'g' must be public") — a
// semantic conflict on the attribute, not a position rule, and untouched here.
TEST(ParserCSmoke, AttributeBeforeExternKeywordNowParses) {
    for (char const* src : {"__attribute__((weak)) extern int g;",
                            "__attribute__((weak)) extern int f(int);",
                            "[[deprecated]] extern int eg;"}) {
        auto h = loadAndTokenize(src);
        Parser p{h.src, h.schema, std::move(h.stream),
                 DiagnosticBudget::libraryDefault()};
        auto result = std::move(p).parse();
        EXPECT_FALSE(result.tree.diagnostics().hasErrors())
            << src
            << " — gcc and clang both accept an attribute BEFORE the storage-"
               "class keyword; the old refusal was below the reference union and "
               "existed only because `extern` headed a declaration rule of its own";
    }
}

// ===========================================================================
// P51 [[D-CSUBSET-NORETURN-KEYWORD-PARAMETER-AND-TYPEDEF-POSITIONS]] — the
// `_Noreturn` KEYWORD in the two grammar positions that used to refuse it.
//
// ✔MEASURED 2026-09-01, each reference probed SEPARATELY at `-std=c11` AND
// `-std=c2x`: gcc 13.3.0 ACCEPTS both positions (rc=0, "parameter 'p' declared
// '_Noreturn'" / "typedef 'tfn' declared '_Noreturn'" — warn and IGNORE);
// clang 18.1.3 REJECTS both ("'_Noreturn' can only appear on functions");
// MSVC 19.51 REJECTS both (error C3829). ONE accepting reference makes the
// behaviour REQUIRED, so DSS must parse both. At the pre-change HEAD the
// parameter position was `error[P0009] expected 'EndStatement' or 'BlockOpen'
// — got '('` and the typedef head was `error[P0009] expected 'Identifier',
// 'VoidKeyword', … — got '_Noreturn'`.
//
// RED-ON-DISABLE: delete the `"NoreturnKeyword"` branch from
// `paramDeclSpecifier`'s alt -> the parameter test below fails; delete it from
// `typedefDeclSpecifiers`' post-keyword alt -> the typedef test below fails.
// ===========================================================================

// The PARAMETER position. The keyword must land in the stripped specifier
// PREFIX, never inside `declHeadForParam`: a token inside the type-resolved
// head is offered to `resolveTypeNodeImpl` as a candidate TYPE, which is the
// silent-miscompile shape `D-CSUBSET-TYPEDEF-HEAD-DECORATION-TYPE-HIJACK`
// exists to prevent and which the three attribute-position tests above pin.
TEST(ParserCSmoke, NoreturnKeywordInParameterSpecifiersParses) {
    for (char const* src : {
             "void g(_Noreturn void (*p)(void));\n",
             "void g(_Noreturn int p);\n",
             "void g(_Noreturn void (*p)(void)) { (void)p; }\n",
             "void g(_Noreturn int a, _Noreturn int b);\n",
             "void g(_Noreturn __attribute__((unused)) int p);\n"}) {
        Tree t = parseC(src);
        ASSERT_NE(t.root(), InvalidNode) << src;
        EXPECT_FALSE(t.diagnostics().hasErrors())
            << src << " must parse (gcc accepts it): " << firstErrorText(t);
    }

    Tree t = parseC("void g(_Noreturn int p);\n");
    // ⚠ FATAL, not EXPECT: everything below reads the tree's SHAPE, and
    // `visibleChildRoles`/`prettyPrintSubtree` call `Tree::tokenKind` on any
    // child that is not Internal — which ABORTS on the error nodes a failed
    // parse leaves behind. MEASURED this cycle: with the grammar branch
    // deleted, the EXPECT form ran on to the structural reads and the process
    // died `Tree::tokenKind on non-Token node` (0xc0000409) BEFORE gtest could
    // print the failing NAME, so the red-on-disable arm reported a crash and
    // zero names. A test whose disabled arm hides its own name is not a pin.
    ASSERT_FALSE(t.diagnostics().hasErrors()) << firstErrorText(t);
    NodeId const param = findFirstNodeWithRule(t, "param");
    ASSERT_TRUE(param.valid());
    EXPECT_EQ(visibleChildRoles(t, param),
              "rule:paramDeclSpecifiers/rule:declHeadForParam/rule:declarator")
        << "the keyword must ride the stripped specifier prefix, so the row's "
           "head: 0 / declarator: 1 are byte-identical decorated or not";
    NodeId const prefix = findFirstNodeWithRule(t, "paramDeclSpecifiers");
    ASSERT_TRUE(prefix.valid());
    EXPECT_EQ(visibleChildRoles(t, prefix), "rule:paramDeclSpecifier")
        << "one specifier, one singular-alt node — the run is a run of the "
           "named singular rule, at the SAME depth `compositeAttr` held";
    // The anti-hijack half: no `_Noreturn` token anywhere under the head.
    NodeId const head = findFirstNodeWithRule(t, "declHeadForParam");
    ASSERT_TRUE(head.valid());
    EXPECT_EQ(prettyPrintSubtree(t, head).find("_Noreturn"), std::string::npos)
        << "a specifier inside the type-resolved head is tried as a TYPE";
}

// The TYPEDEF HEAD. The post-keyword run gained the branch INLINE rather than
// by wrapping the run in a named rule, precisely so every already-parsing
// attribute keeps its exact depth — `TypedefPostKeywordAttributeRidesThe
// SpecifierPrefix` above pins that shape as `tok:TypedefKeyword/rule:attrSpec`
// and a wrapper turned it RED (MEASURED, this cycle).
TEST(ParserCSmoke, NoreturnKeywordInTypedefHeadParses) {
    for (char const* src : {
             "typedef _Noreturn void tfn(void);\n",
             "typedef _Noreturn _Noreturn void tfn(void);\n",
             "typedef _Noreturn void (*pfn)(void);\n",
             "typedef _Noreturn __attribute__((aligned(16))) long T;\n"}) {
        Tree t = parseC(src);
        ASSERT_NE(t.root(), InvalidNode) << src;
        EXPECT_FALSE(t.diagnostics().hasErrors())
            << src << " must parse (gcc accepts it): " << firstErrorText(t);
    }

    Tree t = parseC("typedef _Noreturn void tfn(void);\n");
    // FATAL for the same measured reason as the parameter test above: the
    // structural reads abort on an error tree, which would swallow this
    // test's own name on the red-on-disable arm.
    ASSERT_FALSE(t.diagnostics().hasErrors()) << firstErrorText(t);
    NodeId const decl = findFirstNodeWithRule(t, "typedefDecl");
    ASSERT_TRUE(decl.valid());
    EXPECT_EQ(visibleChildRoles(t, decl), kCanonicalTypedefRoles)
        << "the keyword must not shift head: 0 / declaratorList: 2";
    NodeId const prefix = findFirstNodeWithRule(t, "typedefDeclSpecifiers");
    ASSERT_TRUE(prefix.valid());
    EXPECT_EQ(visibleChildRoles(t, prefix),
              "tok:TypedefKeyword/tok:NoreturnKeyword")
        << "the keyword rides the stripped prefix as a DIRECT token child — "
           "the depth the inline alt preserves";
    NodeId const head = findFirstNodeWithRule(t, "typedefHeadFull");
    ASSERT_TRUE(head.valid());
    EXPECT_EQ(prettyPrintSubtree(t, head).find("_Noreturn"), std::string::npos)
        << "a specifier inside the type-resolved head is tried as a TYPE";
}

// ★ THE ANTI-WIDENING CENSUS. A grammar ALTERNATIVE is the one change class
// that can silently widen what parses ELSEWHERE, so the shapes adjacent to the
// two positions opened above are pinned as still-REFUSED — each one MEASURED
// against gcc 13.3.0 this cycle, and each refusal justified by that reading.
TEST(ParserCSmoke, NoreturnKeywordStaysRejectedInTheAdjacentPositions) {
    struct Shape { char const* src; char const* why; };
    for (Shape const& s : {
             // gcc ERRORS too ("expected specifier-qualifier-list before
             // '_Noreturn'") — `structMemberDeclSpecifier` was NOT widened.
             Shape{"struct S { _Noreturn int x; };\n",
                   "a struct member is a unanimous reject"},
             // gcc ERRORS too ("'_Noreturn' in empty declaration").
             Shape{"typedef _Noreturn void;\n",
                   "a typedef with no declarator is a unanimous reject"},
             // gcc ACCEPTS (warn-and-ignore) — NAMED LOUD RESIDUE, not an
             // oversight: admitting the LEADING run would put NoreturnKeyword
             // into FIRST(typedefDecl), where it collides with topLevelDecl's
             // `declSpecifiers` at /shapes/topLevel — the same
             // C_AmbiguousAlternatives wall `AttributeBeforeExternKeywordStays
             // Rejected` records. Its own anchor, not a variation of this one.
             Shape{"_Noreturn typedef void tfn(void);\n",
                   "the LEADING typedef position is deliberately not opened"}}) {
        Tree t = parseC(s.src);
        EXPECT_TRUE(t.diagnostics().hasErrors())
            << s.src << " must stay a LOUD refusal: " << s.why;
    }
}
