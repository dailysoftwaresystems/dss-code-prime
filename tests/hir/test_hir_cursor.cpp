// HR1: the HIR walker — navigation, leaf/depth queries, [[nodiscard]] contract.

#include "hir/hir.hpp"
#include "hir/hir_cursor.hpp"
#include "hir/hir_node.hpp"

#include <gtest/gtest.h>

#include <array>
#include <utility>
#include <vector>

using dss::Hir;
using dss::HirBuilder;
using dss::HirCursor;
using dss::HirFlags;
using dss::HirKind;
using dss::HirNodeId;

namespace {

// Module → Block → [VarDecl, ReturnStmt → Literal]
struct Tree {
    Hir       hir;
    HirNodeId mod, blk, vd, ret, lit;
};

Tree build() {
    HirBuilder b{"toy"};
    HirNodeId const lit = b.addLeaf(HirKind::Literal);
    HirNodeId const ret = b.addParent(HirKind::ReturnStmt, std::array{lit});
    HirNodeId const vd  = b.addLeaf(HirKind::VarDecl);
    HirNodeId const blk = b.addParent(HirKind::Block, std::array{vd, ret});
    HirNodeId const mod = b.addParent(HirKind::Module, std::array{blk});
    Hir hir = std::move(b).finish(mod);
    return Tree{std::move(hir), mod, blk, vd, ret, lit};
}

} // namespace

TEST(HirCursor, StartsAtGivenNode) {
    Tree t = build();
    HirCursor c{t.hir, t.hir.root()};
    EXPECT_EQ(c.current(), t.mod);
    EXPECT_EQ(c.kind(), HirKind::Module);
    EXPECT_EQ(c.depth(), 0);
    EXPECT_FALSE(c.isAtLeaf());
    // hir() returns a reference to the bound module — identity, not just equality.
    EXPECT_EQ(&c.hir(), &t.hir);
}

TEST(HirCursor, DescendsAndReportsDepthAndLeaf) {
    Tree t = build();
    HirCursor c{t.hir, t.hir.root()};
    ASSERT_TRUE(c.gotoFirstChild());        // → Block
    EXPECT_EQ(c.current(), t.blk);
    EXPECT_EQ(c.depth(), 1);
    ASSERT_TRUE(c.gotoFirstChild());        // → VarDecl
    EXPECT_EQ(c.current(), t.vd);
    EXPECT_EQ(c.kind(), HirKind::VarDecl);
    EXPECT_EQ(c.depth(), 2);
    EXPECT_TRUE(c.isAtLeaf());
}

TEST(HirCursor, SiblingNavigationIsExact) {
    Tree t = build();
    HirCursor c{t.hir, t.blk};
    ASSERT_TRUE(c.gotoFirstChild());        // VarDecl
    EXPECT_EQ(c.current(), t.vd);
    EXPECT_FALSE(c.gotoPrevSibling());      // first child has no prev
    ASSERT_TRUE(c.gotoNextSibling());       // → ReturnStmt
    EXPECT_EQ(c.current(), t.ret);
    EXPECT_FALSE(c.gotoNextSibling());      // last child has no next
    ASSERT_TRUE(c.gotoPrevSibling());       // back to VarDecl
    EXPECT_EQ(c.current(), t.vd);
}

TEST(HirCursor, GotoLastChildAndAscend) {
    Tree t = build();
    HirCursor c{t.hir, t.blk};
    ASSERT_TRUE(c.gotoLastChild());         // ReturnStmt
    EXPECT_EQ(c.current(), t.ret);
    ASSERT_TRUE(c.gotoFirstChild());        // Literal
    EXPECT_EQ(c.current(), t.lit);
    EXPECT_TRUE(c.isAtLeaf());
    EXPECT_FALSE(c.gotoFirstChild());       // leaf has no children
    ASSERT_TRUE(c.gotoParent());            // ReturnStmt
    EXPECT_EQ(c.current(), t.ret);
    ASSERT_TRUE(c.gotoParent());            // Block
    EXPECT_EQ(c.current(), t.blk);
    ASSERT_TRUE(c.gotoParent());            // Module
    EXPECT_EQ(c.current(), t.mod);
    EXPECT_FALSE(c.gotoParent());           // root has no parent
}

TEST(HirCursor, MiddleChildPrevAndNextBothSucceed) {
    // A 3-child parent exercises the genuinely-middle case where both
    // gotoPrevSibling and gotoNextSibling succeed from the same position —
    // the one most likely to mask an off-by-one in sibling index handling.
    HirBuilder b{"toy"};
    HirNodeId const a   = b.addLeaf(HirKind::Literal);
    HirNodeId const m   = b.addLeaf(HirKind::Literal);
    HirNodeId const z   = b.addLeaf(HirKind::Literal);
    HirNodeId const blk = b.addParent(HirKind::Block, std::array{a, m, z});
    HirNodeId const mod = b.addParent(HirKind::Module, std::array{blk});
    Hir hir = std::move(b).finish(mod);

    HirCursor c{hir, m};
    ASSERT_TRUE(c.gotoPrevSibling());
    EXPECT_EQ(c.current(), a);
    EXPECT_FALSE(c.gotoPrevSibling());          // a is the first child
    ASSERT_TRUE(c.gotoNextSibling());
    EXPECT_EQ(c.current(), m);
    ASSERT_TRUE(c.gotoNextSibling());
    EXPECT_EQ(c.current(), z);
    EXPECT_FALSE(c.gotoNextSibling());          // z is the last child
}

TEST(HirCursor, AtInvalidPositionReturnsSentinelsAndRefusesMovement) {
    Tree t = build();
    HirCursor c{t.hir, HirNodeId{}};            // sentinel position
    EXPECT_EQ(c.kind(),   HirKind::Error);
    EXPECT_EQ(c.flags(),  HirFlags::None);
    EXPECT_FALSE(c.typeId().valid());
    EXPECT_TRUE(c.isAtLeaf());
    EXPECT_EQ(c.depth(), 0);                    // matches TreeCursor: 0 at invalid
    // Every movement primitive returns false; nothing aborts.
    EXPECT_FALSE(c.gotoFirstChild());
    EXPECT_FALSE(c.gotoLastChild());
    EXPECT_FALSE(c.gotoNextSibling());
    EXPECT_FALSE(c.gotoPrevSibling());
    EXPECT_FALSE(c.gotoParent());
}

TEST(HirCursor, FullPreOrderWalkVisitsEveryNodeInOrder) {
    Tree t = build();
    // Manual pre-order DFS using only the cursor's movement primitives.
    std::vector<HirNodeId> visited;
    HirCursor c{t.hir, t.hir.root()};
    visited.push_back(c.current());
    // Module → Block
    ASSERT_TRUE(c.gotoFirstChild()); visited.push_back(c.current());
    // Block → VarDecl
    ASSERT_TRUE(c.gotoFirstChild()); visited.push_back(c.current());
    // VarDecl → ReturnStmt
    ASSERT_TRUE(c.gotoNextSibling()); visited.push_back(c.current());
    // ReturnStmt → Literal
    ASSERT_TRUE(c.gotoFirstChild()); visited.push_back(c.current());

    EXPECT_EQ(visited, (std::vector<HirNodeId>{t.mod, t.blk, t.vd, t.ret, t.lit}));
}
