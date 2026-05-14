#include "core/types/tree_node.hpp"

#include <gtest/gtest.h>

#include <type_traits>

using namespace dss;

TEST(NodeFlags, BitwiseOr) {
    constexpr auto v = NodeFlags::EmptySpace | NodeFlags::HasError;
    EXPECT_TRUE(has(v, NodeFlags::EmptySpace));
    EXPECT_TRUE(has(v, NodeFlags::HasError));
    EXPECT_FALSE(has(v, NodeFlags::Missing));
}

TEST(NodeFlags, BitwiseAnd) {
    constexpr auto v = NodeFlags::EmptySpace | NodeFlags::HasError;
    EXPECT_EQ(v & NodeFlags::EmptySpace, NodeFlags::EmptySpace);
    EXPECT_EQ(v & NodeFlags::Missing, NodeFlags::None);
}

TEST(NodeFlags, OrEquals) {
    NodeFlags v = NodeFlags::None;
    v |= NodeFlags::EmptySpace;
    EXPECT_TRUE(isEmptySpace(v));
    v |= NodeFlags::HasError;
    EXPECT_TRUE(hasError(v));
    EXPECT_TRUE(isEmptySpace(v));
}

TEST(NodeFlags, AnyAndHas) {
    EXPECT_FALSE(any(NodeFlags::None));
    EXPECT_TRUE(any(NodeFlags::EmptySpace));
    EXPECT_TRUE(any(NodeFlags::EmptySpace | NodeFlags::HasError));
}

TEST(NodeFlags, IsEmptySpaceHelper) {
    // Single bit-test idiom that the cursor/visitor/IR-gen all use.
    EXPECT_TRUE(isEmptySpace(NodeFlags::EmptySpace));
    EXPECT_TRUE(isEmptySpace(NodeFlags::EmptySpace | NodeFlags::HasError));
    EXPECT_FALSE(isEmptySpace(NodeFlags::HasError));
    EXPECT_FALSE(isEmptySpace(NodeFlags::None));
}

TEST(NodeFlags, NegationViaTilde) {
    constexpr auto inv = ~NodeFlags::EmptySpace;
    EXPECT_FALSE(has(inv, NodeFlags::EmptySpace));
    EXPECT_TRUE(has(inv, NodeFlags::HasError));
}

TEST(NodeFlags, ConstexprUsable) {
    static_assert(has(NodeFlags::EmptySpace | NodeFlags::Missing, NodeFlags::EmptySpace));
    static_assert(!isEmptySpace(NodeFlags::Missing));
}

TEST(DetailNode, IsPodAndBoundedSize) {
    static_assert(std::is_trivially_copyable_v<detail::Node>);
    // 40 bytes still gives us >1 node per cacheline; see tree_node.hpp for the
    // rationale on why we didn't squeeze back to 32.
    static_assert(sizeof(detail::Node) <= 40);
}

TEST(DetailNode, DefaultsAreSane) {
    detail::Node n{};
    EXPECT_EQ(n.kind, NodeKind::Internal);
    EXPECT_EQ(n.flags, NodeFlags::None);
    EXPECT_FALSE(n.tokenKind.valid());
    EXPECT_FALSE(n.rule.valid());
    EXPECT_FALSE(n.parent.valid());
    EXPECT_EQ(n.childCount, 0u);
    EXPECT_FALSE(n.diagnostic.valid());
}
