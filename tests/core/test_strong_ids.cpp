#include "core/types/strong_ids.hpp"

#include <gtest/gtest.h>

#include <type_traits>
#include <unordered_set>

using namespace dss;

TEST(StrongIds, DefaultConstructedIsInvalid) {
    EXPECT_FALSE(NodeId{}.valid());
    EXPECT_FALSE(RuleId{}.valid());
    EXPECT_FALSE(BufferId{}.valid());
    EXPECT_FALSE(TreeId{}.valid());
    EXPECT_FALSE(SchemaTokenId{}.valid());
    EXPECT_FALSE(DiagnosticIndex{}.valid());
}

TEST(StrongIds, NonZeroIsValid) {
    EXPECT_TRUE(NodeId{1}.valid());
    EXPECT_TRUE(NodeId{42}.valid());
    EXPECT_TRUE(RuleId{1}.valid());
}

TEST(StrongIds, EqualityAndOrdering) {
    EXPECT_EQ(NodeId{5}, NodeId{5});
    EXPECT_NE(NodeId{5}, NodeId{6});
    EXPECT_LT(NodeId{5}, NodeId{6});
    EXPECT_GT(NodeId{6}, NodeId{5});
}

TEST(StrongIds, DistinctTypes) {
    // Compile-time check: NodeId and RuleId are NOT the same type even though
    // both wrap uint32_t. This is the whole point of strong typing.
    static_assert(!std::is_same_v<NodeId, RuleId>);
    static_assert(!std::is_same_v<NodeId, BufferId>);
    static_assert(!std::is_same_v<RuleId, SchemaTokenId>);
}

TEST(StrongIds, NoImplicitConstructionFromUint32) {
    // The single-arg constructor is `explicit`. Implicit conversion must not compile.
    static_assert(!std::is_convertible_v<std::uint32_t, NodeId>);
    static_assert(!std::is_convertible_v<int, NodeId>);
    // But explicit construction works:
    [[maybe_unused]] NodeId id{42};
}

TEST(StrongIds, HashableInUnorderedSet) {
    std::unordered_set<NodeId> seen;
    seen.insert(NodeId{1});
    seen.insert(NodeId{2});
    seen.insert(NodeId{1});      // dup
    EXPECT_EQ(seen.size(), 2u);
    EXPECT_TRUE(seen.contains(NodeId{1}));
    EXPECT_FALSE(seen.contains(NodeId{3}));
}

TEST(StrongIds, InvalidSentinelsHaveExpectedValue) {
    EXPECT_EQ(InvalidNode.v, 0u);
    EXPECT_EQ(InvalidRule.v, 0u);
    EXPECT_FALSE(InvalidNode.valid());
}

TEST(StrongIds, IsTriviallyCopyable) {
    static_assert(std::is_trivially_copyable_v<NodeId>);
    static_assert(std::is_trivially_copyable_v<RuleId>);
    static_assert(sizeof(NodeId) == sizeof(std::uint32_t));
}
