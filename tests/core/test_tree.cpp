#include "core/types/rule_id.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"
#include "raw_tree_builder.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <utility>

using namespace dss;
using dss::tests::RawTreeBuilder;

namespace {

// Build a small tree by hand without going through TreeBuilder. Encodes
// a deliberately simple shape:
//
//   root (Internal, rule="program")
//   ├── (2) Token, span=[0,3)   "var"
//   ├── (3) Token, span=[3,4)   " "     (EmptySpace)
//   ├── (4) Token, span=[4,5)   "x"
//   └── (5) Token, span=[5,6)   ";"
//
// The two out-parameters (rules and src) let tests assert pointer
// identity against the Tree's accessors.
Tree makeSampleTree(std::shared_ptr<RuleInterner>& outRules,
                    std::shared_ptr<SourceBuffer>& outSrc) {
    RawTreeBuilder rb{"var x;", "<test>"};
    const RuleId programRule = rb.internRule("program");

    rb.addNode(NodeKind::Internal, programRule, SourceSpan::of(0, 6),
               NodeFlags::None, /*parent=*/ InvalidNode,
               /*children=*/ { NodeId{2}, NodeId{3}, NodeId{4}, NodeId{5} });

    rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(0, 3),
               NodeFlags::None, /*parent=*/ NodeId{1},
               /*children=*/ {}, /*tokenKind=*/ SchemaTokenId{1});

    rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(3, 4),
               NodeFlags::EmptySpace, /*parent=*/ NodeId{1},
               /*children=*/ {}, /*tokenKind=*/ SchemaTokenId{2});

    rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(4, 5),
               NodeFlags::None, /*parent=*/ NodeId{1},
               /*children=*/ {}, /*tokenKind=*/ SchemaTokenId{3});

    rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(5, 6),
               NodeFlags::None, /*parent=*/ NodeId{1},
               /*children=*/ {}, /*tokenKind=*/ SchemaTokenId{4});

    outRules = rb.rules();
    outSrc   = rb.source();
    return std::move(rb).finish(/*root=*/ NodeId{1});
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

// ── D-C-ANALYZER-READS-AN-ERROR-NODE-AS-A-RULE-ON-A-RECOVERED-TREE ──
//
// NodeKind has THREE arms. A tree the parser RECOVERED — the language server
// analyzes those — holds Error nodes beside Internal and Token ones, and a scan
// that tested ONE arm and then read the node through another arm's accessor
// read the half of the node's payload its kind does not own: a debug build's
// assert killed the server, a release build answered the stored field.
//
// Every node below stores a MISLEADING value in the field its kind does not
// own, so an answer taken from the field instead of from the kind is a visible
// wrong answer here, never a default that happens to compare equal.
namespace {

struct ThreeArmTree {
    Tree   tree;
    RuleId programRule;
    RuleId strayRule;
};

ThreeArmTree makeThreeArmTree() {
    RawTreeBuilder rb{"x @", "<test>"};
    RuleId const programRule = rb.internRule("program");
    RuleId const strayRule   = rb.internRule("stray");
    // (1) Internal: owns its rule; a stray token kind sits in the token field.
    rb.addNode(NodeKind::Internal, programRule, SourceSpan::of(0, 3),
               NodeFlags::HasError, /*parent=*/ InvalidNode,
               /*children=*/ { NodeId{2}, NodeId{3} }, /*tokenKind=*/ SchemaTokenId{9});
    // (2) Token `x`: owns its token kind; a stray rule sits in the rule field.
    rb.addNode(NodeKind::Token, strayRule, SourceSpan::of(0, 1),
               NodeFlags::None, /*parent=*/ NodeId{1},
               /*children=*/ {}, /*tokenKind=*/ SchemaTokenId{3});
    // (3) Error `@` — a parser recovery node: owns NEITHER field, both stray.
    rb.addNode(NodeKind::Error, strayRule, SourceSpan::of(2, 3),
               NodeFlags::HasError, /*parent=*/ NodeId{1},
               /*children=*/ {}, /*tokenKind=*/ SchemaTokenId{5});
    Tree t = std::move(rb).finish(/*root=*/ NodeId{1});
    return ThreeArmTree{std::move(t), programRule, strayRule};
}

} // namespace

TEST(TreeKindCheckedQueries, AnswerFromTheNodesKindForEveryArm) {
    ThreeArmTree const f = makeThreeArmTree();
    Tree const& t = f.tree;
    ASSERT_EQ(t.kind(NodeId{1}), NodeKind::Internal);
    ASSERT_EQ(t.kind(NodeId{2}), NodeKind::Token);
    ASSERT_EQ(t.kind(NodeId{3}), NodeKind::Error);

    std::optional<RuleId> const internalRule = t.ruleIfInternal(NodeId{1});
    ASSERT_TRUE(internalRule.has_value());
    EXPECT_EQ(*internalRule, f.programRule);
    EXPECT_FALSE(t.ruleIfInternal(NodeId{2}).has_value());   // not its stray rule
    EXPECT_FALSE(t.ruleIfInternal(NodeId{3}).has_value());   // a recovery node

    std::optional<SchemaTokenId> const tokenKind = t.tokenKindIfToken(NodeId{2});
    ASSERT_TRUE(tokenKind.has_value());
    EXPECT_EQ(*tokenKind, SchemaTokenId{3});
    EXPECT_FALSE(t.tokenKindIfToken(NodeId{1}).has_value());  // not its stray kind
    EXPECT_FALSE(t.tokenKindIfToken(NodeId{3}).has_value());  // a recovery node
}

// No NDEBUG guard, on purpose: the precondition is fatal in a RELEASE build too.
TEST(TreeDeathTest, TheDiscriminantCheckedAccessorsAreFatalOnAnotherKindInEveryBuild) {
    ThreeArmTree const f = makeThreeArmTree();
    Tree const& t = f.tree;
    // The owned arm answers.
    EXPECT_EQ(t.rule(NodeId{1}), f.programRule);
    EXPECT_EQ(t.tokenKind(NodeId{2}), SchemaTokenId{3});

    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH({ (void)t.rule(NodeId{2}); },      "Tree::rule on non-Internal node");
    EXPECT_DEATH({ (void)t.rule(NodeId{3}); },      "Tree::rule on non-Internal node");
    EXPECT_DEATH({ (void)t.tokenKind(NodeId{1}); }, "Tree::tokenKind on non-Token node");
    EXPECT_DEATH({ (void)t.tokenKind(NodeId{3}); }, "Tree::tokenKind on non-Token node");
}
