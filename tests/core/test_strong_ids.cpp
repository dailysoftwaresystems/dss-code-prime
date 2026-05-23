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
    // NodeId carries an extra `treeTag` field (see strong_ids.hpp comment)
    // so cross-tree usage of NodeIds aborts loudly. The other strong ids
    // remain bare-uint32 sized.
    static_assert(sizeof(NodeId) == 2 * sizeof(std::uint32_t));
    static_assert(sizeof(RuleId) == sizeof(std::uint32_t));
}

TEST(StrongIds, NodeIdEqualityIgnoresTreeTag) {
    // The treeTag is provenance metadata, NOT identity. Two NodeIds with
    // the same `.v` but different tags MUST compare equal so existing
    // tests that mix literal NodeId{N} with tagged-from-tree NodeIds
    // continue to assert structurally. The cross-tree validator is the
    // enforcement point.
    EXPECT_EQ(NodeId{3}, NodeId(3, 0));
    EXPECT_EQ(NodeId{3}, NodeId(3, 42));
    EXPECT_EQ(NodeId(3, 7), NodeId(3, 42));
    EXPECT_NE(NodeId{3}, NodeId{4});

    // Hash matches equality — also tag-insensitive.
    auto const h1 = std::hash<NodeId>{}(NodeId{3});
    auto const h2 = std::hash<NodeId>{}(NodeId(3, 99));
    EXPECT_EQ(h1, h2);
}

TEST(StrongIds, NodeIdTwoArgCtorStoresTag) {
    NodeId id{7, 42};
    EXPECT_EQ(id.v, 7u);
    EXPECT_EQ(id.treeTag, 42u);

    NodeId untagged{7};
    EXPECT_EQ(untagged.v, 7u);
    EXPECT_EQ(untagged.treeTag, 0u);
}

// ── CompilationUnitId (CU1) ───────────────────────────────────────────────

TEST(StrongIds, CompilationUnitIdDefaultIsInvalid) {
    EXPECT_FALSE(CompilationUnitId{}.valid());
    EXPECT_FALSE(InvalidCompilationUnit.valid());
    EXPECT_EQ(InvalidCompilationUnit.v, 0u);
}

TEST(StrongIds, CompilationUnitIdDistinctType) {
    // Same DSS_STRONG_ID macro, distinct type. Mixing with TreeId/RuleId
    // must be a compile error, not a silent uint32_t conversion.
    static_assert(!std::is_same_v<CompilationUnitId, TreeId>);
    static_assert(!std::is_same_v<CompilationUnitId, RuleId>);
    static_assert(!std::is_convertible_v<std::uint32_t, CompilationUnitId>);
    static_assert(sizeof(CompilationUnitId) == sizeof(std::uint32_t));
}

TEST(StrongIds, CompilationUnitIdHashable) {
    std::unordered_set<CompilationUnitId> seen;
    seen.insert(CompilationUnitId{1});
    seen.insert(CompilationUnitId{2});
    seen.insert(CompilationUnitId{1});  // dup
    EXPECT_EQ(seen.size(), 2u);
    EXPECT_TRUE(seen.contains(CompilationUnitId{1}));
    EXPECT_FALSE(seen.contains(CompilationUnitId{3}));
}
