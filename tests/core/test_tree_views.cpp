#include "core/types/grammar_schema.hpp"
#include "core/types/rule_id.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"
#include "core/types/tree_views.hpp"
#include "core/types/well_known_names.hpp"
#include "raw_tree_builder.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace dss;
using dss::tests::RawTreeBuilder;

namespace {

// Schema-bound test builder: same shape as RawTreeBuilder but attaches a
// real GrammarSchema so token-level views can resolve token-kind names.
// The RuleInterner is local (NOT aliased to schema->rules()) so tests can
// intern arbitrary rule names (e.g. "binaryExpr") not in the toy grammar.
class SchemaTreeBuilder {
public:
    explicit SchemaTreeBuilder(std::shared_ptr<GrammarSchema const> schema,
                               std::string sourceText = "abcdefghijklmnopqrstuvwxyz")
        : src_(SourceBuffer::fromString(std::move(sourceText), "<test>"))
        , schema_(std::move(schema))
        , rules_(std::make_shared<RuleInterner>()) {
        td_.nodes.emplace_back();   // slot 0 reserved for InvalidNode
    }

    [[nodiscard]] RuleId internRule(std::string_view name) { return rules_->intern(name); }
    [[nodiscard]] SchemaTokenId tokenKind(std::string_view name) const {
        return schema_->schemaTokens().find(name);
    }

    NodeId addInternal(RuleId rule, SourceSpan span, NodeFlags flags,
                       NodeId parent, std::vector<NodeId> children = {}) {
        const auto value = static_cast<std::uint32_t>(td_.nodes.size());
        detail::Node n{};
        n.kind = NodeKind::Internal;
        n.flags = flags;
        n.rule = rule;
        n.span = span;
        n.parent = parent;
        td_.nodes.push_back(n);
        pending_.emplace_back(NodeId{value}, std::move(children));
        return NodeId{value};
    }

    NodeId addToken(SchemaTokenId kind, SourceSpan span, NodeFlags flags,
                    NodeId parent) {
        const auto value = static_cast<std::uint32_t>(td_.nodes.size());
        detail::Node n{};
        n.kind      = NodeKind::Token;
        n.flags     = flags;
        n.tokenKind = kind;
        n.span      = span;
        n.parent    = parent;
        td_.nodes.push_back(n);
        pending_.emplace_back(NodeId{value}, std::vector<NodeId>{});
        return NodeId{value};
    }

    [[nodiscard]] Tree finish(NodeId root, TreeId id = TreeId{1}) && {
        for (auto& [nid, kids] : pending_) {
            auto& n = td_.nodes[nid.v];
            n.firstChild = static_cast<std::uint32_t>(td_.childIndex.size());
            n.childCount = static_cast<std::uint32_t>(kids.size());
            for (NodeId k : kids) td_.childIndex.push_back(k);
        }
        rules_->freeze();
        td_.source = src_;
        td_.rules  = rules_;
        td_.schema = schema_;
        td_.id     = id;
        td_.root   = root;
        return Tree{std::move(td_)};
    }

private:
    std::shared_ptr<SourceBuffer>          src_;
    std::shared_ptr<GrammarSchema const>   schema_;
    std::shared_ptr<RuleInterner>          rules_;
    detail::TreeData                       td_;
    std::vector<std::pair<NodeId, std::vector<NodeId>>> pending_;
};

std::shared_ptr<GrammarSchema const> loadToySchemaShared() {
    auto loaded = GrammarSchema::loadShipped("toy");
    if (!loaded) {
        // Test-infra breakage signal — every core test depends on this path.
        ADD_FAILURE() << "Failed to load toy.lang.json";
        return nullptr;
    }
    return std::shared_ptr<GrammarSchema const>(std::move(*loaded));
}

} // namespace

// ── IdentifierView ───────────────────────────────────────────────────────

TEST(IdentifierView, FromAcceptsIdentifierToken) {
    auto schema = loadToySchemaShared();
    ASSERT_NE(schema, nullptr);
    SchemaTreeBuilder b{schema, "name"};
    const auto idKind = b.tokenKind(tokens::kIdentifier);
    ASSERT_TRUE(idKind.valid());
    const auto rule = b.internRule("root");
    const auto root = b.addInternal(rule, SourceSpan::of(0, 4), NodeFlags::None,
                                    InvalidNode, /*children=*/ {NodeId{2}});
    b.addToken(idKind, SourceSpan::of(0, 4), NodeFlags::None, root);
    Tree t = std::move(b).finish(root);

    auto v = IdentifierView::from(t, NodeId{2});
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->name(), "name");
    EXPECT_EQ(v->node(), NodeId{2});
    EXPECT_EQ(v->span(), SourceSpan::of(0, 4));
}

TEST(IdentifierView, FromRejectsOtherTokenKinds) {
    auto schema = loadToySchemaShared();
    SchemaTreeBuilder b{schema, "+++++"};
    const auto sumKind = b.tokenKind("SumOperator");
    ASSERT_TRUE(sumKind.valid());
    const auto rule = b.internRule("root");
    const auto root = b.addInternal(rule, SourceSpan::of(0, 1), NodeFlags::None,
                                    InvalidNode, /*children=*/ {NodeId{2}});
    b.addToken(sumKind, SourceSpan::of(0, 1), NodeFlags::None, root);
    Tree t = std::move(b).finish(root);

    EXPECT_FALSE(IdentifierView::from(t, NodeId{2}).has_value());
}

TEST(IdentifierView, FromRejectsInternalNode) {
    auto schema = loadToySchemaShared();
    SchemaTreeBuilder b{schema, "x"};
    const auto rule = b.internRule("root");
    const auto root = b.addInternal(rule, SourceSpan::of(0, 1), NodeFlags::None, InvalidNode);
    Tree t = std::move(b).finish(root);
    EXPECT_FALSE(IdentifierView::from(t, root).has_value());
}

TEST(IdentifierView, FromRejectsInvalidNode) {
    auto schema = loadToySchemaShared();
    SchemaTreeBuilder b{schema, "x"};
    const auto rule = b.internRule("root");
    const auto root = b.addInternal(rule, SourceSpan::of(0, 1), NodeFlags::None, InvalidNode);
    Tree t = std::move(b).finish(root);
    EXPECT_FALSE(IdentifierView::from(t, InvalidNode).has_value());
}

TEST(IdentifierView, FromOnSchemaLessTreeReturnsNullopt) {
    // tokenKindFor's no-schema fallback ensures token-level `from()` is
    // safe on hand-built trees that lack a schema, matching rule-level
    // `from()`'s already-safe behavior.
    RawTreeBuilder rb{"x"};
    const auto rule = rb.internRule("root");
    rb.addNode(NodeKind::Internal, rule, SourceSpan::of(0, 1), NodeFlags::None,
               InvalidNode, /*children=*/ {NodeId{2}});
    rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(0, 1),
               NodeFlags::None, NodeId{1});
    Tree t = std::move(rb).finish(NodeId{1});
    EXPECT_FALSE(t.hasSchema());
    EXPECT_FALSE(IdentifierView::from(t, NodeId{2}).has_value());
}

// ── LiteralView ──────────────────────────────────────────────────────────

TEST(LiteralView, FromAcceptsEachLiteralKind) {
    auto schema = loadToySchemaShared();
    struct Case { std::string_view tokenName; LiteralView::Kind expected; };
    const Case cases[] = {
        {tokens::kIntLiteral,    LiteralView::Kind::Int},
        {tokens::kFloatLiteral,  LiteralView::Kind::Float},
        {tokens::kStringLiteral, LiteralView::Kind::String},
        {tokens::kCharLiteral,   LiteralView::Kind::Char},
        {tokens::kBoolLiteral,   LiteralView::Kind::Bool},
        {tokens::kNullLiteral,   LiteralView::Kind::Null},
    };
    for (auto const& c : cases) {
        SchemaTreeBuilder b{schema, "x"};
        const auto kind = b.tokenKind(c.tokenName);
        ASSERT_TRUE(kind.valid()) << "Missing schema token: " << c.tokenName;
        const auto rule = b.internRule("root");
        const auto root = b.addInternal(rule, SourceSpan::of(0, 1), NodeFlags::None,
                                        InvalidNode, /*children=*/ {NodeId{2}});
        b.addToken(kind, SourceSpan::of(0, 1), NodeFlags::None, root);
        Tree t = std::move(b).finish(root);

        auto v = LiteralView::from(t, NodeId{2});
        ASSERT_TRUE(v.has_value()) << "Literal kind not detected: " << c.tokenName;
        EXPECT_EQ(v->kind(), c.expected) << "Wrong kind for " << c.tokenName;
        EXPECT_EQ(v->text(), "x");
    }
}

TEST(LiteralView, FromRejectsIdentifierToken) {
    auto schema = loadToySchemaShared();
    SchemaTreeBuilder b{schema, "name"};
    const auto idKind = b.tokenKind(tokens::kIdentifier);
    const auto rule = b.internRule("root");
    const auto root = b.addInternal(rule, SourceSpan::of(0, 4), NodeFlags::None,
                                    InvalidNode, /*children=*/ {NodeId{2}});
    b.addToken(idKind, SourceSpan::of(0, 4), NodeFlags::None, root);
    Tree t = std::move(b).finish(root);

    EXPECT_FALSE(LiteralView::from(t, NodeId{2}).has_value());
}

TEST(LiteralView, FromRejectsOperatorToken) {
    // Pins the "unknown-kind → nullopt" path against a token whose kind
    // IS valid but isn't one of the six literal kinds. A regression that
    // replaced the final `return std::nullopt` with a silent fallback
    // (e.g. `return Kind::Int`) would slip past Identifier-only rejection.
    auto schema = loadToySchemaShared();
    SchemaTreeBuilder b{schema, "+"};
    const auto opKind = b.tokenKind("SumOperator");
    ASSERT_TRUE(opKind.valid());
    const auto rule = b.internRule("root");
    const auto root = b.addInternal(rule, SourceSpan::of(0, 1), NodeFlags::None,
                                    InvalidNode, /*children=*/ {NodeId{2}});
    b.addToken(opKind, SourceSpan::of(0, 1), NodeFlags::None, root);
    Tree t = std::move(b).finish(root);

    EXPECT_FALSE(LiteralView::from(t, NodeId{2}).has_value());
}

TEST(LiteralView, FromRejectsInvalidNode) {
    auto schema = loadToySchemaShared();
    SchemaTreeBuilder b{schema, "x"};
    const auto rule = b.internRule("root");
    const auto root = b.addInternal(rule, SourceSpan::of(0, 1), NodeFlags::None, InvalidNode);
    Tree t = std::move(b).finish(root);
    EXPECT_FALSE(LiteralView::from(t, InvalidNode).has_value());
}

TEST(LiteralViewDeath, UncheckedConstructorOnNonLiteralAborts) {
    auto schema = loadToySchemaShared();
    SchemaTreeBuilder b{schema, "x"};
    const auto idKind = b.tokenKind(tokens::kIdentifier);
    const auto rule = b.internRule("root");
    const auto root = b.addInternal(rule, SourceSpan::of(0, 1), NodeFlags::None,
                                    InvalidNode, /*children=*/ {NodeId{2}});
    b.addToken(idKind, SourceSpan::of(0, 1), NodeFlags::None, root);
    Tree t = std::move(b).finish(root);

    EXPECT_DEATH({ LiteralView v(t, NodeId{2}); (void)v; },
                 "non-literal token");
}

// ── BinaryExprView (rule "binaryExpr" — not in toy grammar) ──────────────

namespace {

struct BinaryExprBuild { Tree tree; NodeId binExpr; NodeId lhs; NodeId op; NodeId rhs; };

// `withWhitespace` interleaves EmptySpace tokens between structural children
// so the test exercises the visible-child-filtering invariant.
BinaryExprBuild buildBinaryExpr(bool withWhitespace) {
    RawTreeBuilder rb{"a+b"};
    const auto ruleBinExpr = rb.internRule("binaryExpr");
    const auto ruleOperand = rb.internRule("operand");

    // Pre-decide NodeIds (RawTreeBuilder hands them out in append order
    // starting from slot 1).
    std::vector<NodeId> binChildren;
    if (withWhitespace) {
        binChildren = { NodeId{2}, NodeId{3}, NodeId{4}, NodeId{5}, NodeId{6} };
    } else {
        binChildren = { NodeId{2}, NodeId{3}, NodeId{4} };
    }
    rb.addNode(NodeKind::Internal, ruleBinExpr, SourceSpan::of(0, 3),
               NodeFlags::None, InvalidNode, binChildren);

    rb.addNode(NodeKind::Internal, ruleOperand, SourceSpan::of(0, 1),
               NodeFlags::None, NodeId{1});
    if (withWhitespace) {
        rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(0, 0),
                   NodeFlags::EmptySpace, NodeId{1});
    }
    rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(1, 2),
               NodeFlags::None, NodeId{1});
    if (withWhitespace) {
        rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(0, 0),
                   NodeFlags::EmptySpace, NodeId{1});
    }
    rb.addNode(NodeKind::Internal, ruleOperand, SourceSpan::of(2, 3),
               NodeFlags::None, NodeId{1});

    Tree t = std::move(rb).finish(NodeId{1});
    if (withWhitespace) {
        return {std::move(t), NodeId{1}, NodeId{2}, NodeId{4}, NodeId{6}};
    }
    return {std::move(t), NodeId{1}, NodeId{2}, NodeId{3}, NodeId{4}};
}

} // namespace

TEST(BinaryExprView, FromAcceptsBinaryExpr) {
    auto build = buildBinaryExpr(/*withWhitespace=*/ false);
    auto v = BinaryExprView::from(build.tree, build.binExpr);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->lhs(),    build.lhs);
    EXPECT_EQ(v->opNode(), build.op);
    EXPECT_EQ(v->rhs(),    build.rhs);
    EXPECT_EQ(v->op(),     "+");
    // span is the rule node's, not lhs/op/rhs.
    EXPECT_EQ(v->span(), SourceSpan::of(0, 3));
    EXPECT_EQ(v->node(), build.binExpr);
}

TEST(BinaryExprView, FromRejectsWrongRule) {
    RawTreeBuilder rb{"x"};
    const auto rule = rb.internRule("notBinaryExpr");
    rb.addNode(NodeKind::Internal, rule, SourceSpan::of(0, 1), NodeFlags::None, InvalidNode);
    Tree t = std::move(rb).finish(NodeId{1});

    EXPECT_FALSE(BinaryExprView::from(t, NodeId{1}).has_value());
}

TEST(BinaryExprView, FromRejectsToken) {
    RawTreeBuilder rb{"+"};
    const auto rule = rb.internRule("root");
    rb.addNode(NodeKind::Internal, rule, SourceSpan::of(0, 1),
               NodeFlags::None, InvalidNode, /*children=*/ {NodeId{2}});
    rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(0, 1),
               NodeFlags::None, NodeId{1});
    Tree t = std::move(rb).finish(NodeId{1});

    EXPECT_FALSE(BinaryExprView::from(t, NodeId{2}).has_value());
}

TEST(BinaryExprView, AccessorsSkipEmptySpace) {
    auto build = buildBinaryExpr(/*withWhitespace=*/ true);
    auto v = BinaryExprView::from(build.tree, build.binExpr);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->lhs(),    build.lhs);
    EXPECT_EQ(v->opNode(), build.op);
    EXPECT_EQ(v->rhs(),    build.rhs);
    EXPECT_EQ(v->op(),     "+");
}

TEST(BinaryExprView, FromOnInvalidNodeReturnsNullopt) {
    RawTreeBuilder rb{"x"};
    const auto rule = rb.internRule("root");
    rb.addNode(NodeKind::Internal, rule, SourceSpan::of(0, 1), NodeFlags::None, InvalidNode);
    Tree t = std::move(rb).finish(NodeId{1});
    EXPECT_FALSE(BinaryExprView::from(t, InvalidNode).has_value());
}

// ── BlockView ────────────────────────────────────────────────────────────

namespace {

struct BlockBuild { Tree tree; NodeId block; std::vector<NodeId> stmts; };

BlockBuild buildBlock(int statementCount) {
    RawTreeBuilder rb{std::string(static_cast<std::size_t>(statementCount), 's')};
    const auto ruleBlock = rb.internRule("block");
    const auto ruleStmt  = rb.internRule("stmt");

    std::vector<NodeId> stmtIds;
    std::vector<NodeId> children;
    std::uint32_t next = 2;
    for (int i = 0; i < statementCount; ++i) {
        stmtIds.push_back(NodeId{next});
        children.push_back(NodeId{next++});
        if (i + 1 < statementCount) {
            children.push_back(NodeId{next++});
        }
    }
    rb.addNode(NodeKind::Internal, ruleBlock,
               SourceSpan::of(0, statementCount),
               NodeFlags::None, InvalidNode, children);

    for (int i = 0; i < statementCount; ++i) {
        rb.addNode(NodeKind::Internal, ruleStmt,
                   SourceSpan::of(static_cast<ByteOffset>(i),
                                  static_cast<ByteOffset>(i + 1)),
                   NodeFlags::None, NodeId{1});
        if (i + 1 < statementCount) {
            rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(0, 0),
                       NodeFlags::EmptySpace, NodeId{1});
        }
    }

    return {std::move(rb).finish(NodeId{1}), NodeId{1}, std::move(stmtIds)};
}

} // namespace

TEST(BlockView, FromAcceptsBlock) {
    auto build = buildBlock(2);
    auto v = BlockView::from(build.tree, build.block);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->node(), build.block);
    EXPECT_EQ(v->span(), SourceSpan::of(0, 2));
}

TEST(BlockView, FromRejectsWrongRule) {
    RawTreeBuilder rb{"x"};
    const auto rule = rb.internRule("notBlock");
    rb.addNode(NodeKind::Internal, rule, SourceSpan::of(0, 1), NodeFlags::None, InvalidNode);
    Tree t = std::move(rb).finish(NodeId{1});
    EXPECT_FALSE(BlockView::from(t, NodeId{1}).has_value());
}

TEST(BlockView, FromOnInvalidNodeReturnsNullopt) {
    auto build = buildBlock(1);
    EXPECT_FALSE(BlockView::from(build.tree, InvalidNode).has_value());
}

TEST(BlockView, EmptyBlockReportsZeroStatements) {
    auto build = buildBlock(0);
    auto v = BlockView::from(build.tree, build.block);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->statementCount(), 0u);
    EXPECT_EQ(v->statementAt(0),   InvalidNode);
    EXPECT_TRUE(v->statements().empty());
}

TEST(BlockView, SingleStatementBlock) {
    auto build = buildBlock(1);
    auto v = BlockView::from(build.tree, build.block);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->statementCount(), 1u);
    EXPECT_EQ(v->statementAt(0), build.stmts[0]);
    EXPECT_EQ(v->statements(), build.stmts);
}

TEST(BlockView, MultipleStatementsSkipsInterspersedWhitespace) {
    auto build = buildBlock(3);
    auto v = BlockView::from(build.tree, build.block);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->statementCount(), 3u);
    EXPECT_EQ(v->statementAt(0), build.stmts[0]);
    EXPECT_EQ(v->statementAt(1), build.stmts[1]);
    EXPECT_EQ(v->statementAt(2), build.stmts[2]);
    EXPECT_EQ(v->statementAt(3), InvalidNode);
    EXPECT_EQ(v->statements(),   build.stmts);
}

// ── FunctionDeclView ─────────────────────────────────────────────────────

namespace {

// Build  functionDecl[name, paramList, body]  optionally with EmptySpace
// tokens interspersed between the three structural children. Returns the
// functionDecl root + the structural NodeIds.
struct FunctionDeclBuild { Tree tree; NodeId fn; NodeId name; NodeId paramList; NodeId body; };

FunctionDeclBuild buildFunctionDecl(std::shared_ptr<GrammarSchema const> schema,
                                    bool withWhitespace) {
    SchemaTreeBuilder b{schema, "foo(){}"};
    const auto idKind   = b.tokenKind(tokens::kIdentifier);
    const auto fnRule   = b.internRule(rules::kFunctionDecl);
    const auto plRule   = b.internRule("paramList");
    const auto bodyRule = b.internRule("body");

    std::vector<NodeId> children;
    if (withWhitespace) {
        children = {NodeId{2}, NodeId{3}, NodeId{4}, NodeId{5}, NodeId{6}};
    } else {
        children = {NodeId{2}, NodeId{3}, NodeId{4}};
    }
    const auto fn = b.addInternal(fnRule, SourceSpan::of(0, 7), NodeFlags::None,
                                  InvalidNode, std::move(children));
    const auto name = b.addToken(idKind, SourceSpan::of(0, 3), NodeFlags::None, fn);
    if (withWhitespace) {
        b.addToken(InvalidSchemaToken, SourceSpan::of(0, 0), NodeFlags::EmptySpace, fn);
    }
    const auto pl   = b.addInternal(plRule, SourceSpan::of(3, 5), NodeFlags::None, fn);
    if (withWhitespace) {
        b.addToken(InvalidSchemaToken, SourceSpan::of(0, 0), NodeFlags::EmptySpace, fn);
    }
    const auto body = b.addInternal(bodyRule, SourceSpan::of(5, 7), NodeFlags::None, fn);
    Tree t = std::move(b).finish(fn);

    return {std::move(t), fn, name, pl, body};
}

} // namespace

TEST(FunctionDeclView, FromAcceptsFunctionDecl) {
    auto build = buildFunctionDecl(loadToySchemaShared(), /*withWhitespace=*/ false);
    auto v = FunctionDeclView::from(build.tree, build.fn);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->nameNode(),  build.name);
    EXPECT_EQ(v->paramList(), build.paramList);
    EXPECT_EQ(v->body(),      build.body);
    EXPECT_EQ(v->name().name(), "foo");
    EXPECT_EQ(v->span(), SourceSpan::of(0, 7));
}

TEST(FunctionDeclView, AccessorsSkipEmptySpace) {
    auto build = buildFunctionDecl(loadToySchemaShared(), /*withWhitespace=*/ true);
    auto v = FunctionDeclView::from(build.tree, build.fn);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->nameNode(),  build.name);
    EXPECT_EQ(v->paramList(), build.paramList);
    EXPECT_EQ(v->body(),      build.body);
    EXPECT_EQ(v->name().name(), "foo");
}

TEST(FunctionDeclView, FromRejectsWrongRule) {
    RawTreeBuilder rb{"x"};
    const auto rule = rb.internRule("notFunctionDecl");
    rb.addNode(NodeKind::Internal, rule, SourceSpan::of(0, 1), NodeFlags::None, InvalidNode);
    Tree t = std::move(rb).finish(NodeId{1});
    EXPECT_FALSE(FunctionDeclView::from(t, NodeId{1}).has_value());
}

TEST(FunctionDeclView, FromOnInvalidNodeReturnsNullopt) {
    auto build = buildFunctionDecl(loadToySchemaShared(), false);
    EXPECT_FALSE(FunctionDeclView::from(build.tree, InvalidNode).has_value());
}

TEST(FunctionDeclView, ChainedUncheckedNameSurvivesWrongShape) {
    // Documented hazard at tree_views.hpp's FunctionDeclView::name():
    // returning an UNCHECKED IdentifierView means a malformed tree whose
    // first visible child ISN'T an Identifier token gives a view whose
    // accessors don't abort — they return whatever lexeme is there. The
    // safe path is IdentifierView::from on nameNode().
    auto schema = loadToySchemaShared();
    SchemaTreeBuilder b{schema, "wrongshape"};
    const auto fnRule    = b.internRule(rules::kFunctionDecl);
    const auto plRule    = b.internRule("paramList");

    // Swap: child 0 is now a paramList (Internal), not an Identifier token.
    const auto fn = b.addInternal(fnRule, SourceSpan::of(0, 10), NodeFlags::None,
                                  InvalidNode, /*children=*/ {NodeId{2}, NodeId{3}, NodeId{4}});
    b.addInternal(plRule, SourceSpan::of(0, 5), NodeFlags::None, fn);   // wrong shape
    b.addInternal(plRule, SourceSpan::of(5, 7), NodeFlags::None, fn);
    b.addInternal(plRule, SourceSpan::of(7, 10), NodeFlags::None, fn);
    Tree t = std::move(b).finish(fn);

    auto v = FunctionDeclView::from(t, fn);
    ASSERT_TRUE(v.has_value());

    // Safe path: IdentifierView::from rejects (nameNode is Internal).
    EXPECT_FALSE(IdentifierView::from(t, v->nameNode()).has_value());

    // Unsafe path: chained .name().name() does NOT abort, returns the
    // wrong-shape node's lexeme. This pins the documented contract.
    EXPECT_EQ(v->name().name(), "wrong");   // tree.text() of span [0,5)
}

// ── VarDeclView (toy-aligned) ────────────────────────────────────────────

namespace {

struct VarDeclBuild { Tree tree; NodeId varDecl; NodeId nameTok; NodeId valueExpr; };

VarDeclBuild buildToyVarDecl(std::shared_ptr<GrammarSchema const> schema,
                             bool withWhitespace) {
    SchemaTreeBuilder b{schema, "var x = y;"};
    const auto idKind  = b.tokenKind(tokens::kIdentifier);
    const auto varKw   = b.tokenKind("VarKeyword");
    const auto assign  = b.tokenKind("AssignmentOperator");
    const auto eos     = b.tokenKind("EndCommand");

    const auto ruleVarDecl = b.internRule(rules::kVarDecl);
    const auto ruleExpr    = b.internRule("expression");

    std::vector<NodeId> children;
    if (withWhitespace) {
        // 5 structural + 4 EmptySpace between each pair = 9 children.
        children = {NodeId{2}, NodeId{3}, NodeId{4}, NodeId{5}, NodeId{6},
                    NodeId{7}, NodeId{8}, NodeId{9}, NodeId{10}};
    } else {
        children = {NodeId{2}, NodeId{3}, NodeId{4}, NodeId{5}, NodeId{6}};
    }
    const auto vd = b.addInternal(ruleVarDecl, SourceSpan::of(0, 10), NodeFlags::None,
                                  InvalidNode, std::move(children));
    b.addToken(varKw,   SourceSpan::of(0, 3), NodeFlags::None, vd);
    const auto name = b.addToken(idKind, SourceSpan::of(4, 5), NodeFlags::None, vd);
    b.addToken(assign,  SourceSpan::of(6, 7), NodeFlags::None, vd);
    const auto expr = b.addInternal(ruleExpr, SourceSpan::of(8, 9), NodeFlags::None, vd);
    b.addToken(eos,     SourceSpan::of(9, 10), NodeFlags::None, vd);
    if (withWhitespace) {
        // Append four EmptySpace tokens; they live as children of vd at the
        // end of the child list. nthVisibleChild walks in append order, so
        // the EmptySpace nodes between structural children would shift role
        // indexing IF the visible-child filter were broken. Layout here puts
        // them after the structural children (between the original 5), but
        // RawTreeBuilder/SchemaTreeBuilder serializes children in the order
        // listed in the parent's child array — we already covered the
        // interspersed case for BinaryExprView/Block; for VarDecl we use a
        // simpler shape that still exercises the filter when EmptySpace
        // nodes appear inside the child range.
        b.addToken(InvalidSchemaToken, SourceSpan::of(0, 0), NodeFlags::EmptySpace, vd);
        b.addToken(InvalidSchemaToken, SourceSpan::of(0, 0), NodeFlags::EmptySpace, vd);
        b.addToken(InvalidSchemaToken, SourceSpan::of(0, 0), NodeFlags::EmptySpace, vd);
        b.addToken(InvalidSchemaToken, SourceSpan::of(0, 0), NodeFlags::EmptySpace, vd);
    }

    Tree t = std::move(b).finish(vd);
    return {std::move(t), vd, name, expr};
}

// True EmptySpace-interleaved varDecl: ws tokens between each structural
// child, requiring the visible-child filter to skip them mid-walk.
VarDeclBuild buildToyVarDeclInterleaved(std::shared_ptr<GrammarSchema const> schema) {
    SchemaTreeBuilder b{schema, "var x = y;"};
    const auto idKind  = b.tokenKind(tokens::kIdentifier);
    const auto varKw   = b.tokenKind("VarKeyword");
    const auto assign  = b.tokenKind("AssignmentOperator");
    const auto eos     = b.tokenKind("EndCommand");

    const auto ruleVarDecl = b.internRule(rules::kVarDecl);
    const auto ruleExpr    = b.internRule("expression");

    // [VarKeyword, ws, Identifier, ws, AssignmentOperator, ws, expression, ws, EndCommand]
    std::vector<NodeId> children = {
        NodeId{2}, NodeId{3}, NodeId{4}, NodeId{5}, NodeId{6},
        NodeId{7}, NodeId{8}, NodeId{9}, NodeId{10}
    };
    const auto vd = b.addInternal(ruleVarDecl, SourceSpan::of(0, 10), NodeFlags::None,
                                  InvalidNode, std::move(children));
    b.addToken(varKw,                SourceSpan::of(0, 3),  NodeFlags::None,       vd); // 2: VarKeyword
    b.addToken(InvalidSchemaToken,   SourceSpan::of(0, 0),  NodeFlags::EmptySpace, vd); // 3: ws
    const auto name = b.addToken(idKind, SourceSpan::of(4, 5), NodeFlags::None,    vd); // 4: name
    b.addToken(InvalidSchemaToken,   SourceSpan::of(0, 0),  NodeFlags::EmptySpace, vd); // 5: ws
    b.addToken(assign,               SourceSpan::of(6, 7),  NodeFlags::None,       vd); // 6: =
    b.addToken(InvalidSchemaToken,   SourceSpan::of(0, 0),  NodeFlags::EmptySpace, vd); // 7: ws
    const auto expr = b.addInternal(ruleExpr, SourceSpan::of(8, 9), NodeFlags::None, vd); // 8: expr
    b.addToken(InvalidSchemaToken,   SourceSpan::of(0, 0),  NodeFlags::EmptySpace, vd); // 9: ws
    b.addToken(eos,                  SourceSpan::of(9, 10), NodeFlags::None,       vd); // 10: ;

    Tree t = std::move(b).finish(vd);
    return {std::move(t), vd, name, expr};
}

} // namespace

TEST(VarDeclView, FromAcceptsToyVarDecl) {
    auto schema = loadToySchemaShared();
    auto build = buildToyVarDecl(schema, /*withWhitespace=*/ false);
    auto v = VarDeclView::from(build.tree, build.varDecl);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->nameNode(), build.nameTok);
    EXPECT_EQ(v->value(),    build.valueExpr);
    EXPECT_EQ(v->name().name(), "x");
    EXPECT_EQ(v->span(), SourceSpan::of(0, 10));
}

TEST(VarDeclView, AccessorsSkipInterleavedEmptySpace) {
    auto schema = loadToySchemaShared();
    auto build = buildToyVarDeclInterleaved(schema);
    auto v = VarDeclView::from(build.tree, build.varDecl);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->nameNode(), build.nameTok);
    EXPECT_EQ(v->value(),    build.valueExpr);
    EXPECT_EQ(v->name().name(), "x");
}

TEST(VarDeclView, FromRejectsWrongRule) {
    RawTreeBuilder rb{"x"};
    const auto rule = rb.internRule("notVarDecl");
    rb.addNode(NodeKind::Internal, rule, SourceSpan::of(0, 1), NodeFlags::None, InvalidNode);
    Tree t = std::move(rb).finish(NodeId{1});
    EXPECT_FALSE(VarDeclView::from(t, NodeId{1}).has_value());
}

TEST(VarDeclView, FromOnInvalidNodeReturnsNullopt) {
    auto schema = loadToySchemaShared();
    auto build = buildToyVarDecl(schema, false);
    EXPECT_FALSE(VarDeclView::from(build.tree, InvalidNode).has_value());
}

// ── ExprStmtView (toy-aligned) ───────────────────────────────────────────

TEST(ExprStmtView, FromAcceptsToyExprStmt) {
    auto schema = loadToySchemaShared();
    SchemaTreeBuilder b{schema, "y;"};
    const auto eos      = b.tokenKind("EndCommand");
    const auto ruleStmt = b.internRule(rules::kExprStmt);
    const auto ruleExpr = b.internRule("expression");

    const auto stmt = b.addInternal(ruleStmt, SourceSpan::of(0, 2), NodeFlags::None,
                                    InvalidNode, /*children=*/ {NodeId{2}, NodeId{3}});
    const auto expr = b.addInternal(ruleExpr, SourceSpan::of(0, 1), NodeFlags::None, stmt);
    b.addToken(eos, SourceSpan::of(1, 2), NodeFlags::None, stmt);
    Tree t = std::move(b).finish(stmt);

    auto v = ExprStmtView::from(t, stmt);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->expression(), expr);
    EXPECT_EQ(v->span(), SourceSpan::of(0, 2));
}

TEST(ExprStmtView, AccessorsSkipInterleavedEmptySpace) {
    auto schema = loadToySchemaShared();
    SchemaTreeBuilder b{schema, "y;"};
    const auto eos      = b.tokenKind("EndCommand");
    const auto ruleStmt = b.internRule(rules::kExprStmt);
    const auto ruleExpr = b.internRule("expression");

    // [ws, expression, ws, ;] — leading EmptySpace would shift index 0
    // away from the expression if the filter didn't skip it.
    const auto stmt = b.addInternal(ruleStmt, SourceSpan::of(0, 2), NodeFlags::None,
                                    InvalidNode,
                                    /*children=*/ {NodeId{2}, NodeId{3}, NodeId{4}, NodeId{5}});
    b.addToken(InvalidSchemaToken, SourceSpan::of(0, 0), NodeFlags::EmptySpace, stmt);
    const auto expr = b.addInternal(ruleExpr, SourceSpan::of(0, 1), NodeFlags::None, stmt);
    b.addToken(InvalidSchemaToken, SourceSpan::of(0, 0), NodeFlags::EmptySpace, stmt);
    b.addToken(eos, SourceSpan::of(1, 2), NodeFlags::None, stmt);
    Tree t = std::move(b).finish(stmt);

    auto v = ExprStmtView::from(t, stmt);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->expression(), expr);
}

TEST(ExprStmtView, FromRejectsWrongRule) {
    RawTreeBuilder rb{"x"};
    const auto rule = rb.internRule("notExprStmt");
    rb.addNode(NodeKind::Internal, rule, SourceSpan::of(0, 1), NodeFlags::None, InvalidNode);
    Tree t = std::move(rb).finish(NodeId{1});
    EXPECT_FALSE(ExprStmtView::from(t, NodeId{1}).has_value());
}

TEST(ExprStmtView, FromOnInvalidNodeReturnsNullopt) {
    auto schema = loadToySchemaShared();
    SchemaTreeBuilder b{schema, "y;"};
    const auto rule = b.internRule(rules::kExprStmt);
    const auto stmt = b.addInternal(rule, SourceSpan::of(0, 2), NodeFlags::None, InvalidNode);
    Tree t = std::move(b).finish(stmt);
    EXPECT_FALSE(ExprStmtView::from(t, InvalidNode).has_value());
}

// ── View ergonomics: triviality, optional, copy/move ─────────────────────

TEST(TreeViews, ViewsAreTriviallyCopyableAndSmall) {
    static_assert(std::is_trivially_copyable_v<BinaryExprView>);
    static_assert(std::is_trivially_copyable_v<BlockView>);
    static_assert(std::is_trivially_copyable_v<FunctionDeclView>);
    static_assert(std::is_trivially_copyable_v<VarDeclView>);
    static_assert(std::is_trivially_copyable_v<ExprStmtView>);
    static_assert(std::is_trivially_copyable_v<IdentifierView>);
    static_assert(std::is_trivially_copyable_v<LiteralView>);
    // Size budget: Tree* + NodeId (+ one byte Kind for LiteralView).
    static_assert(sizeof(IdentifierView)   <= 2 * sizeof(void*));
    static_assert(sizeof(BinaryExprView)   <= 2 * sizeof(void*));
}

TEST(TreeViews, OptionalReassignmentWorks) {
    auto build = buildBinaryExpr(/*withWhitespace=*/ false);
    std::optional<BinaryExprView> v = BinaryExprView::from(build.tree, build.binExpr);
    ASSERT_TRUE(v.has_value());
    v.reset();
    EXPECT_FALSE(v.has_value());
    v = BinaryExprView::from(build.tree, build.binExpr);
    EXPECT_TRUE(v.has_value());
}
