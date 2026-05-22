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
    EXPECT_TRUE(hasInternalNodeWithRule(t, "typeRef"))
        << "tree must include a typeRef frame (exercises optional-skip)";
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

TEST(ParserCSubsetSmoke, FunctionCallParsesAsPostfix) {
    auto h = loadAndTokenize("int main() { f(a, b); }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(hasInternalNodeWithRule(t, "postfixExpr"));
    EXPECT_TRUE(hasInternalNodeWithRule(t, "argList"));
}

TEST(ParserCSubsetSmoke, EmptyArgumentCallParsesAsPostfix) {
    auto h = loadAndTokenize("int main() { f(); }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "zero-arg calls must parse cleanly (argList nullable)";
    EXPECT_TRUE(hasInternalNodeWithRule(t, "postfixExpr"));
}

TEST(ParserCSubsetSmoke, ArrayIndexParsesAsPostfix) {
    auto h = loadAndTokenize("int main() { a[0]; }");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(hasInternalNodeWithRule(t, "postfixExpr"));
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
    EXPECT_TRUE(hasInternalNodeWithRule(t, "binaryExpr"))
        << "the index expression must engage operator climbing for `j*k` "
           "to bind tighter than `+`";
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
    EXPECT_TRUE(hasInternalNodeWithRule(t, "postfixExpr"));
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
    EXPECT_TRUE(hasInternalNodeWithRule(t, "unaryExpr"));
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
    // descends through `operand → ( expression ) → binaryExpr[a,+,b]`.
    const std::string_view expected =
        "rule:expression\n"
        "  rule:binaryExpr\n"
        "    rule:operand\n"
        "      tok:\"(\"\n"
        "      rule:expression\n"
        "        rule:binaryExpr\n"
        "          rule:operand\n"
        "            tok:\"a\"\n"
        "          tok:\"+\"\n"
        "          rule:operand\n"
        "            tok:\"b\"\n"
        "      tok:\")\"\n"
        "    tok:\"*\"\n"
        "    rule:operand\n"
        "      tok:\"c\"\n";
    EXPECT_EQ(prettyPrintSubtree(t, expr), expected);
}
