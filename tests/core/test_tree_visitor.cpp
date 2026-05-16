#include "core/types/rule_id.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/tree_node.hpp"
#include "core/types/tree_visitor.hpp"
#include "raw_tree_builder.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

using namespace dss;
using dss::tests::RawTreeBuilder;

// ── allocation counter ───────────────────────────────────────────────────
//
// Replaces global operator new/delete inside this test executable so the
// 10K-node walk can prove zero heap activity during the walk machinery.
// Counts allocations only; deletes pass through. Snapshot before the walk,
// compare after.

namespace alloc_counter {
std::atomic<std::size_t> g_count{0};

inline std::size_t snapshot() noexcept {
    return g_count.load(std::memory_order_relaxed);
}
} // namespace alloc_counter

void* operator new(std::size_t n) {
    alloc_counter::g_count.fetch_add(1, std::memory_order_relaxed);
    if (n == 0) n = 1;
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n) {
    alloc_counter::g_count.fetch_add(1, std::memory_order_relaxed);
    if (n == 0) n = 1;
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept              { std::free(p); }
void operator delete[](void* p) noexcept            { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept{ std::free(p); }

namespace {

// Canonical 6-node test tree:
//
//   root            (NodeId{1})
//   ├── a           (NodeId{2})
//   │   ├── a.1     (NodeId{3})
//   │   └── a.2     (NodeId{4})
//   └── b           (NodeId{5})
//       └── b.1     (NodeId{6})
//
// 6 nodes; lets every test reason about exact visit sequences.
Tree buildSimpleTree() {
    RawTreeBuilder rb{"abcdef"};
    const auto rule = rb.internRule("internal");

    rb.addNode(NodeKind::Internal, rule, SourceSpan::of(0, 6),
               NodeFlags::None, InvalidNode,
               /*children=*/ { NodeId{2}, NodeId{5} });
    rb.addNode(NodeKind::Internal, rule, SourceSpan::of(0, 2),
               NodeFlags::None, NodeId{1},
               /*children=*/ { NodeId{3}, NodeId{4} });
    rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(0, 1),
               NodeFlags::None, NodeId{2});
    rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(1, 2),
               NodeFlags::None, NodeId{2});
    rb.addNode(NodeKind::Internal, rule, SourceSpan::of(2, 6),
               NodeFlags::None, NodeId{1},
               /*children=*/ { NodeId{6} });
    rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(2, 6),
               NodeFlags::None, NodeId{5});

    return std::move(rb).finish(/*root=*/ NodeId{1});
}

template <typename Walker>
std::vector<NodeId> collect(Walker walker) {
    std::vector<NodeId> out;
    walker([&](TreeCursor const& c) { out.push_back(c.current()); });
    return out;
}

} // namespace

TEST(TreeVisitor, VoidVisitorCompilesAndVisits) {
    Tree t = buildSimpleTree();
    int count = 0;
    walkPreOrder(t, [&](TreeCursor const&) { ++count; });
    EXPECT_EQ(count, 6);
}

TEST(TreeVisitor, WalkActionVisitorCompilesAndVisits) {
    Tree t = buildSimpleTree();
    int count = 0;
    walkPreOrder(t, [&](TreeCursor const&) {
        ++count;
        return WalkAction::Continue;
    });
    EXPECT_EQ(count, 6);
}

TEST(TreeVisitor, MoveOnlyVisitorCompiles) {
    Tree t = buildSimpleTree();
    auto sink = std::make_unique<int>(0);
    auto visitor = [s = std::move(sink)](TreeCursor const&) mutable { ++*s; };
    walkPreOrder(t, std::move(visitor));
}

TEST(TreeVisitor, PreOrderVisitsRootBeforeChildren) {
    Tree t = buildSimpleTree();
    auto order = collect([&](auto&& v) { walkPreOrder(t, v); });
    const std::vector<NodeId> expected{
        NodeId{1}, NodeId{2}, NodeId{3}, NodeId{4}, NodeId{5}, NodeId{6},
    };
    EXPECT_EQ(order, expected);
}

TEST(TreeVisitor, PostOrderVisitsChildrenBeforeParent) {
    Tree t = buildSimpleTree();
    auto order = collect([&](auto&& v) { walkPostOrder(t, v); });
    const std::vector<NodeId> expected{
        NodeId{3}, NodeId{4}, NodeId{2}, NodeId{6}, NodeId{5}, NodeId{1},
    };
    EXPECT_EQ(order, expected);
}

TEST(TreeVisitor, SkipChildrenSkipsSubtreeInPreOrder) {
    Tree t = buildSimpleTree();
    std::vector<NodeId> visited;
    walkPreOrder(t, [&](TreeCursor const& c) {
        visited.push_back(c.current());
        return c.current() == NodeId{2}
            ? WalkAction::SkipChildren
            : WalkAction::Continue;
    });
    const std::vector<NodeId> expected{
        NodeId{1}, NodeId{2}, NodeId{5}, NodeId{6}
    };
    EXPECT_EQ(visited, expected);
}

TEST(TreeVisitor, SkipChildrenAtStartNodeTerminatesCleanly) {
    // Regression guard: returning SkipChildren at depth 0 must not fall
    // through to gotoNextSibling — which would escape the subtree if any
    // sibling existed. Start from the full-tree root (no sibling), then
    // from `a` (which HAS a sibling `b`) to prove the depth-0 guard fires
    // before the sibling probe.
    Tree t = buildSimpleTree();

    std::vector<NodeId> fromRoot;
    walkPreOrder(t, [&](TreeCursor const& c) {
        fromRoot.push_back(c.current());
        return WalkAction::SkipChildren;
    });
    EXPECT_EQ(fromRoot, (std::vector<NodeId>{NodeId{1}}));

    std::vector<NodeId> fromA;
    walkPreOrder(t, NodeId{2}, [&](TreeCursor const& c) {
        fromA.push_back(c.current());
        return WalkAction::SkipChildren;
    });
    EXPECT_EQ(fromA, (std::vector<NodeId>{NodeId{2}}));
}

TEST(TreeVisitor, StopHaltsEntireWalk) {
    Tree t = buildSimpleTree();
    std::vector<NodeId> visited;
    walkPreOrder(t, [&](TreeCursor const& c) {
        visited.push_back(c.current());
        return c.current() == NodeId{3}
            ? WalkAction::Stop
            : WalkAction::Continue;
    });
    const std::vector<NodeId> expected{NodeId{1}, NodeId{2}, NodeId{3}};
    EXPECT_EQ(visited, expected);
}

TEST(TreeVisitor, StopAtStartNodeReturnsImmediately) {
    Tree t = buildSimpleTree();
    std::vector<NodeId> visited;
    walkPreOrder(t, [&](TreeCursor const& c) {
        visited.push_back(c.current());
        return WalkAction::Stop;
    });
    EXPECT_EQ(visited, (std::vector<NodeId>{NodeId{1}}));
}

TEST(TreeVisitor, SkipChildrenIsTreatedAsContinueInPostOrder) {
    // Post-order has already visited children by the time it sees the
    // parent; SkipChildren is meaningless and silently becomes Continue.
    Tree t = buildSimpleTree();
    std::vector<NodeId> visited;
    walkPostOrder(t, [&](TreeCursor const& c) {
        visited.push_back(c.current());
        return WalkAction::SkipChildren;
    });
    const std::vector<NodeId> expected{
        NodeId{3}, NodeId{4}, NodeId{2}, NodeId{6}, NodeId{5}, NodeId{1},
    };
    EXPECT_EQ(visited, expected);
}

TEST(TreeVisitor, StopHaltsPostOrderWalk) {
    Tree t = buildSimpleTree();
    std::vector<NodeId> visited;
    walkPostOrder(t, [&](TreeCursor const& c) {
        visited.push_back(c.current());
        return c.current() == NodeId{4}
            ? WalkAction::Stop
            : WalkAction::Continue;
    });
    const std::vector<NodeId> expected{NodeId{3}, NodeId{4}};
    EXPECT_EQ(visited, expected);
}

TEST(TreeVisitor, StopAtStartNodeInPostOrder) {
    // Post-order visits the start node LAST. Returning Stop there must not
    // fall off the end of the walk; this guards the `if (a == Stop) return`
    // check at the visit point against a regression that gates it on depth.
    Tree t = buildSimpleTree();
    std::vector<NodeId> visited;
    walkPostOrder(t, [&](TreeCursor const& c) {
        visited.push_back(c.current());
        return c.current() == NodeId{1}
            ? WalkAction::Stop
            : WalkAction::Continue;
    });
    const std::vector<NodeId> expected{
        NodeId{3}, NodeId{4}, NodeId{2}, NodeId{6}, NodeId{5}, NodeId{1},
    };
    EXPECT_EQ(visited, expected);
}

TEST(TreeVisitor, PreOrderBoundedToInternalSubtree) {
    Tree t = buildSimpleTree();
    auto order = collect([&](auto&& v) {
        walkPreOrder(t, /*start=*/ NodeId{2}, v);
    });
    const std::vector<NodeId> expected{NodeId{2}, NodeId{3}, NodeId{4}};
    EXPECT_EQ(order, expected);
}

TEST(TreeVisitor, PreOrderBoundedToSecondSubtree) {
    // Symmetric to the NodeId{2} test: starting from `b` (NodeId{5}) must
    // stop after `b.1`. Catches a regression that exits via gotoNextSibling
    // (which would naturally return false at NodeId{5}, masking the bug).
    Tree t = buildSimpleTree();
    auto order = collect([&](auto&& v) {
        walkPreOrder(t, /*start=*/ NodeId{5}, v);
    });
    EXPECT_EQ(order, (std::vector<NodeId>{NodeId{5}, NodeId{6}}));
}

TEST(TreeVisitor, PostOrderBoundedToInternalSubtree) {
    Tree t = buildSimpleTree();
    auto order = collect([&](auto&& v) {
        walkPostOrder(t, /*start=*/ NodeId{2}, v);
    });
    const std::vector<NodeId> expected{NodeId{3}, NodeId{4}, NodeId{2}};
    EXPECT_EQ(order, expected);
}

TEST(TreeVisitor, PostOrderBoundedToSecondSubtree) {
    Tree t = buildSimpleTree();
    auto order = collect([&](auto&& v) {
        walkPostOrder(t, /*start=*/ NodeId{5}, v);
    });
    EXPECT_EQ(order, (std::vector<NodeId>{NodeId{6}, NodeId{5}}));
}

TEST(TreeVisitor, WalkSingleLeafJustVisitsThatNode) {
    Tree t = buildSimpleTree();
    auto pre  = collect([&](auto&& v) { walkPreOrder(t,  NodeId{3}, v); });
    auto post = collect([&](auto&& v) { walkPostOrder(t, NodeId{3}, v); });
    EXPECT_EQ(pre,  (std::vector<NodeId>{NodeId{3}}));
    EXPECT_EQ(post, (std::vector<NodeId>{NodeId{3}}));
}

namespace {

// Tree with an EmptySpace leaf in the middle. AST cursor skips NodeId{3};
// CST cursor visits it.
Tree buildTreeWithEmptySpace() {
    RawTreeBuilder rb{"abc"};
    const auto rule = rb.internRule("internal");
    rb.addNode(NodeKind::Internal, rule, SourceSpan::of(0, 3), NodeFlags::None,
               InvalidNode, /*children=*/ { NodeId{2}, NodeId{3}, NodeId{4} });
    rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(0, 1),
               NodeFlags::None, NodeId{1});
    rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(1, 2),
               NodeFlags::EmptySpace, NodeId{1});
    rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(2, 3),
               NodeFlags::None, NodeId{1});
    return std::move(rb).finish(/*root=*/ NodeId{1});
}

} // namespace

TEST(TreeVisitor, AstCursorPropagatesAcrossPreOrderWalk) {
    Tree t = buildTreeWithEmptySpace();
    auto cstVisits = collect([&](auto&& v) { walkPreOrder(t.cursor(),    v); });
    auto astVisits = collect([&](auto&& v) { walkPreOrder(t.astCursor(), v); });
    EXPECT_EQ(cstVisits, (std::vector<NodeId>{NodeId{1}, NodeId{2}, NodeId{3}, NodeId{4}}));
    EXPECT_EQ(astVisits, (std::vector<NodeId>{NodeId{1}, NodeId{2}, NodeId{4}}));
}

TEST(TreeVisitor, AstCursorPropagatesAcrossPostOrderWalk) {
    // Verifies the leftmost-leaf descent and sibling-then-descend loops both
    // honor the cursor's AST mode — a regression in either would visit the
    // EmptySpace child.
    Tree t = buildTreeWithEmptySpace();
    auto cstVisits = collect([&](auto&& v) { walkPostOrder(t.cursor(),    v); });
    auto astVisits = collect([&](auto&& v) { walkPostOrder(t.astCursor(), v); });
    EXPECT_EQ(cstVisits, (std::vector<NodeId>{NodeId{2}, NodeId{3}, NodeId{4}, NodeId{1}}));
    EXPECT_EQ(astVisits, (std::vector<NodeId>{NodeId{2}, NodeId{4}, NodeId{1}}));
}

TEST(TreeVisitor, EmptyTreeWalkIsNoOp) {
    detail::TreeData td;
    td.id = TreeId{42};
    Tree t{std::move(td)};

    int visits = 0;
    walkPreOrder(t,  [&](TreeCursor const&) { ++visits; });
    walkPostOrder(t, [&](TreeCursor const&) { ++visits; });
    EXPECT_EQ(visits, 0);
}

TEST(TreeVisitor, InvalidStartNodeIsNoOp) {
    Tree t = buildSimpleTree();
    int visits = 0;
    walkPreOrder(t,  InvalidNode, [&](TreeCursor const&) { ++visits; });
    walkPostOrder(t, InvalidNode, [&](TreeCursor const&) { ++visits; });
    EXPECT_EQ(visits, 0);
}

namespace {

// Wide (not deep) so the cursor's O(depth) parent walk stays cheap.
Tree buildWideTree(int leafCount) {
    RawTreeBuilder rb{std::string(static_cast<std::size_t>(leafCount), 'x')};
    const auto rule = rb.internRule("root");

    std::vector<NodeId> kids;
    kids.reserve(static_cast<std::size_t>(leafCount));
    for (int i = 0; i < leafCount; ++i) {
        kids.push_back(NodeId{static_cast<std::uint32_t>(i + 2)});
    }
    rb.addNode(NodeKind::Internal, rule, SourceSpan::of(0, leafCount),
               NodeFlags::None, InvalidNode, std::move(kids));
    for (int i = 0; i < leafCount; ++i) {
        rb.addNode(NodeKind::Token, InvalidRule,
                   SourceSpan::of(static_cast<ByteOffset>(i),
                                  static_cast<ByteOffset>(i + 1)),
                   NodeFlags::None, NodeId{1});
    }
    return std::move(rb).finish(/*root=*/ NodeId{1});
}

} // namespace

TEST(TreeVisitor, TenThousandNodeWalkVisitsEveryNode) {
    constexpr int kLeafCount = 9999;
    Tree t = buildWideTree(kLeafCount);

    int pre = 0, post = 0;
    walkPreOrder(t,  [&](TreeCursor const&) { ++pre; });
    walkPostOrder(t, [&](TreeCursor const&) { ++post; });
    EXPECT_EQ(pre,  kLeafCount + 1);
    EXPECT_EQ(post, kLeafCount + 1);
}

TEST(TreeVisitor, TenThousandNodeWalkAllocatesNothing) {
    // Plan acceptance criterion: "benchmarks confirm zero allocations during
    // a 10K-node walk." Build everything BEFORE the snapshot, then prove the
    // walk machinery does not touch the heap. Visitor is `++int` only.
    constexpr int kLeafCount = 9999;
    Tree t = buildWideTree(kLeafCount);
    int preCount = 0, postCount = 0;

    const auto preBefore = alloc_counter::snapshot();
    walkPreOrder(t, [&](TreeCursor const&) { ++preCount; });
    const auto preAfter = alloc_counter::snapshot();

    const auto postBefore = alloc_counter::snapshot();
    walkPostOrder(t, [&](TreeCursor const&) { ++postCount; });
    const auto postAfter = alloc_counter::snapshot();

    EXPECT_EQ(preCount,  kLeafCount + 1);
    EXPECT_EQ(postCount, kLeafCount + 1);
    EXPECT_EQ(preAfter  - preBefore,  std::size_t{0});
    EXPECT_EQ(postAfter - postBefore, std::size_t{0});
}
