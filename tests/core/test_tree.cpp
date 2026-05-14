#include "core/types/rule_id.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

using namespace dss;

namespace {

// Fabricates a small tree by hand without going through TreeBuilder (which
// arrives in CP3). Encodes a deliberately simple shape:
//
//   root (Internal, rule="program")
//   ├── (1) Token, span=[0,3)   "var"
//   ├── (2) Token, span=[3,4)   " "     (EmptySpace)
//   ├── (3) Token, span=[4,5)   "x"
//   └── (4) Token, span=[5,6)   ";"
//
// Returns a fully-formed Tree.
Tree makeSampleTree(std::shared_ptr<RuleInterner>& outRules,
                    std::shared_ptr<SourceBuffer>& outSrc) {
    outRules = std::make_shared<RuleInterner>();
    const RuleId programRule = outRules->intern("program");
    outRules->freeze();

    outSrc = SourceBuffer::fromString("var x;", "<test>");

    detail::TreeData td;
    td.source = outSrc;
    td.rules  = outRules;
    td.id     = TreeId{1};

    // Node 0 reserved (InvalidNode sentinel). Real nodes start at index 1.
    td.nodes.resize(5);

    // Node 1 = root
    td.nodes[1] = detail::Node{
        .kind       = NodeKind::Internal,
        .flags      = NodeFlags::None,
        .rule       = programRule,
        .span       = SourceSpan::of(0, 6),
        .parent     = InvalidNode,
        .firstChild = 0,                       // offset into childIndex_
        .childCount = 4,
    };

    // Children (nodes 2..5 by NodeId; using indices 2..5 in arena).
    td.nodes[2] = detail::Node{
        .kind      = NodeKind::Token,
        .tokenKind = SchemaTokenId{1},         // pretend: "VarKeyword"
        .span      = SourceSpan::of(0, 3),
        .parent    = NodeId{1},
    };
    td.nodes[3] = detail::Node{
        .kind      = NodeKind::Token,
        .flags     = NodeFlags::EmptySpace,
        .tokenKind = SchemaTokenId{2},         // pretend: "Whitespace"
        .span      = SourceSpan::of(3, 4),
        .parent    = NodeId{1},
    };
    // Reuse last slots — actually we declared 5 nodes (1 sentinel + 4 real). Let me fix:
    td.nodes[4] = detail::Node{
        .kind      = NodeKind::Token,
        .tokenKind = SchemaTokenId{3},         // pretend: "Identifier"
        .span      = SourceSpan::of(4, 5),
        .parent    = NodeId{1},
    };

    // Need 4 children: var, ws, ident, semicolon. Resize for one more.
    td.nodes.resize(6);
    td.nodes[5] = detail::Node{
        .kind      = NodeKind::Token,
        .tokenKind = SchemaTokenId{4},         // pretend: "EndCommand"
        .span      = SourceSpan::of(5, 6),
        .parent    = NodeId{1},
    };

    td.childIndex = { NodeId{2}, NodeId{3}, NodeId{4}, NodeId{5} };
    td.root = NodeId{1};

    return Tree{std::move(td)};
}

} // namespace

TEST(Tree, IdentityAccessors) {
    std::shared_ptr<RuleInterner> rules;
    std::shared_ptr<SourceBuffer> src;
    Tree t = makeSampleTree(rules, src);

    EXPECT_EQ(t.id(), TreeId{1});
    EXPECT_EQ(&t.source(), src.get());
    EXPECT_EQ(&t.rules(), rules.get());
    EXPECT_EQ(t.root(), NodeId{1});
    EXPECT_EQ(t.nodeCount(), 6u);    // 1 sentinel + 4 children + root
}

TEST(Tree, RootIsInternal) {
    std::shared_ptr<RuleInterner> rules;
    std::shared_ptr<SourceBuffer> src;
    Tree t = makeSampleTree(rules, src);

    EXPECT_EQ(t.kind(t.root()), NodeKind::Internal);
    EXPECT_EQ(t.flags(t.root()), NodeFlags::None);
    EXPECT_EQ(t.rule(t.root()), rules->intern("program"));  // returns existing id even after freeze
    EXPECT_EQ(t.text(t.root()), "var x;");
}

TEST(Tree, ChildrenReturnsAllInOrder) {
    std::shared_ptr<RuleInterner> rules;
    std::shared_ptr<SourceBuffer> src;
    Tree t = makeSampleTree(rules, src);

    auto kids = t.children(t.root());
    ASSERT_EQ(kids.size(), 4u);
    EXPECT_EQ(kids[0], NodeId{2});
    EXPECT_EQ(kids[1], NodeId{3});
    EXPECT_EQ(kids[2], NodeId{4});
    EXPECT_EQ(kids[3], NodeId{5});
}

TEST(Tree, ChildrenReturnsEmptyForLeaves) {
    std::shared_ptr<RuleInterner> rules;
    std::shared_ptr<SourceBuffer> src;
    Tree t = makeSampleTree(rules, src);

    auto kids = t.children(NodeId{2});
    EXPECT_TRUE(kids.empty());
}

TEST(Tree, ParentLinks) {
    std::shared_ptr<RuleInterner> rules;
    std::shared_ptr<SourceBuffer> src;
    Tree t = makeSampleTree(rules, src);

    EXPECT_EQ(t.parent(NodeId{1}), InvalidNode);   // root
    EXPECT_EQ(t.parent(NodeId{2}), NodeId{1});
    EXPECT_EQ(t.parent(NodeId{5}), NodeId{1});
}

TEST(Tree, TextSlicesSource) {
    std::shared_ptr<RuleInterner> rules;
    std::shared_ptr<SourceBuffer> src;
    Tree t = makeSampleTree(rules, src);

    EXPECT_EQ(t.text(NodeId{2}), "var");
    EXPECT_EQ(t.text(NodeId{3}), " ");
    EXPECT_EQ(t.text(NodeId{4}), "x");
    EXPECT_EQ(t.text(NodeId{5}), ";");
}

TEST(Tree, EmptySpaceFlagDetection) {
    std::shared_ptr<RuleInterner> rules;
    std::shared_ptr<SourceBuffer> src;
    Tree t = makeSampleTree(rules, src);

    // The whitespace token carries the bit; the keyword doesn't.
    EXPECT_TRUE(isEmptySpace(t.flags(NodeId{3})));
    EXPECT_FALSE(isEmptySpace(t.flags(NodeId{2})));
}

TEST(Tree, TokenKindOnLeafs) {
    std::shared_ptr<RuleInterner> rules;
    std::shared_ptr<SourceBuffer> src;
    Tree t = makeSampleTree(rules, src);

    EXPECT_EQ(t.kind(NodeId{2}), NodeKind::Token);
    EXPECT_EQ(t.tokenKind(NodeId{2}), SchemaTokenId{1});
    EXPECT_EQ(t.tokenKind(NodeId{5}), SchemaTokenId{4});
}

TEST(Tree, DiagnosticIsNulloptWhenAbsent) {
    std::shared_ptr<RuleInterner> rules;
    std::shared_ptr<SourceBuffer> src;
    Tree t = makeSampleTree(rules, src);

    EXPECT_FALSE(t.diagnostic(NodeId{1}).has_value());
    EXPECT_FALSE(t.diagnostic(NodeId{2}).has_value());
}

// Death-test: out-of-range NodeId aborts (release-mode fatal, not UB).
TEST(TreeDeathTest, InvalidNodeIdAborts) {
    std::shared_ptr<RuleInterner> rules;
    std::shared_ptr<SourceBuffer> src;
    Tree t = makeSampleTree(rules, src);

    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH({ (void)t.kind(NodeId{9999}); }, "Tree::node_: NodeId out of range");
    EXPECT_DEATH({ (void)t.kind(InvalidNode);  }, "Tree::node_: NodeId out of range");
}
