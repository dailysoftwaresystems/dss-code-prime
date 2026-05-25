// SP1: ArenaAttribute over a non-Tree arena — the same dual sparse↔dense
// side-table that backs NodeAttribute<T>, proving it generalizes.

#include "core/substrate/arena_attribute.hpp"
#include "core/substrate/arena_container.hpp"

#include "substrate/arena_test_types.hpp"

#include <gtest/gtest.h>

#include <string>
#include <type_traits>
#include <utility>

namespace {

using namespace dss::substrate;
using dss::substrate_test::ShapeId;
using dss::substrate_test::ShapePod;
using dss::substrate_test::ShapeTag;
using dss::substrate_test::ValueId;
using dss::substrate_test::ValuePod;
using dss::substrate_test::ValueTag;

using ShapeArena   = ArenaContainer<ShapePod, ShapeId, ShapeTag>;
using ShapeBuilder = ArenaBuilder<ShapePod, ShapeId, ShapeTag>;
using ValueArena   = ArenaContainer<ValuePod, ValueId, ValueTag>;
using ValueBuilder = ArenaBuilder<ValuePod, ValueId, ValueTag>;

// Build a ShapeArena with `count` real nodes (ids 1..count) tagged `tag`.
[[nodiscard]] ShapeArena buildArena(std::uint32_t count, std::uint32_t tag) {
    ShapeBuilder b{ShapeTag{tag}};
    for (std::uint32_t i = 0; i < count; ++i) b.addNode(ShapePod{static_cast<int>(i), 0});
    return std::move(b).finish();
}

} // namespace

TEST(ArenaAttribute, SetGetHasEraseSparse) {
    auto arena = buildArena(3, 1);
    ArenaAttribute<ShapeArena, std::string> attr{arena};
    EXPECT_TRUE(attr.empty());

    attr.set(ShapeId{1, 1}, "a");
    attr.set(ShapeId{2, 1}, "b");
    EXPECT_EQ(attr.size(), 2u);
    EXPECT_FALSE(attr.isDense());
    EXPECT_TRUE(attr.has(ShapeId{1, 1}));
    EXPECT_EQ(attr.get(ShapeId{2, 1}), "b");
    EXPECT_EQ(attr.tryGet(ShapeId{3, 1}), nullptr);
    EXPECT_TRUE(attr.erase(ShapeId{1, 1}));
    EXPECT_FALSE(attr.has(ShapeId{1, 1}));
    EXPECT_EQ(attr.size(), 1u);
    EXPECT_EQ(attr.tree().v, 1u);            // bound arena's tag
}

TEST(ArenaAttribute, PromotesToDenseAtCoverage) {
    // nodeCount = 21 (sentinel + 20). Floor is 16; promotion at >= 50%.
    auto arena = buildArena(20, 5);
    ASSERT_EQ(arena.nodeCount(), 21u);
    ArenaAttribute<ShapeArena, int> attr{arena};
    for (std::uint32_t i = 1; i <= 10; ++i) attr.set(ShapeId{i, 5}, static_cast<int>(i));
    EXPECT_FALSE(attr.isDense());            // 10/21 < 50%
    attr.set(ShapeId{11, 5}, 11);
    EXPECT_TRUE(attr.isDense());             // 11/21 >= 50%
    EXPECT_EQ(attr.size(), 11u);
    EXPECT_EQ(attr.get(ShapeId{7, 5}), 7);   // value survives promotion
}

TEST(ArenaAttribute, MoveOnlyAndMovedFromIsEmpty) {
    static_assert(!std::is_copy_constructible_v<ArenaAttribute<ShapeArena, int>>);
    static_assert(std::is_move_constructible_v<ArenaAttribute<ShapeArena, int>>);

    auto arena = buildArena(3, 1);
    ArenaAttribute<ShapeArena, int> attr{arena};
    attr.set(ShapeId{1, 1}, 10);
    attr.set(ShapeId{2, 1}, 20);
    ArenaAttribute<ShapeArena, int> moved{std::move(attr)};
    EXPECT_EQ(moved.size(), 2u);
    EXPECT_EQ(attr.size(), 0u);              // NOLINT(bugprone-use-after-move)
    EXPECT_TRUE(attr.empty());               // NOLINT(bugprone-use-after-move)
}

TEST(ArenaAttribute, ForwardIterationYieldsAllEntries) {
    auto arena = buildArena(3, 1);
    ArenaAttribute<ShapeArena, int> attr{arena};
    attr.set(ShapeId{1, 1}, 100);
    attr.set(ShapeId{2, 1}, 200);
    int sum = 0, n = 0;
    for (auto const& [id, value] : attr) { sum += value; ++n; (void)id; }
    EXPECT_EQ(n, 2);
    EXPECT_EQ(sum, 300);
}

TEST(ArenaAttributeDeathTest, SentinelAndBoundsAbort) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto arena = buildArena(3, 1);
    ArenaAttribute<ShapeArena, int> attr{arena};
    EXPECT_DEATH({ attr.set(ShapeId{}, 1); }, "invalid ShapeId");
    EXPECT_DEATH({ attr.set(ShapeId{999, 1}, 1); }, "out of bounds");
}

TEST(ArenaAttribute, TypeAliasesAndConstOverloads) {
    static_assert(std::is_same_v<ArenaAttribute<ShapeArena, int>::value_type, int>);
    static_assert(std::is_same_v<ArenaAttribute<ShapeArena, int>::IdType, ShapeId>);
    static_assert(std::is_same_v<ArenaAttribute<ShapeArena, int>::TagType, ShapeTag>);

    auto arena = buildArena(3, 1);
    ArenaAttribute<ShapeArena, int> attr{arena};
    attr.set(ShapeId{1, 1}, 5);
    auto const& cref = attr;
    static_assert(std::is_same_v<decltype(cref.get(ShapeId{1, 1})), int const&>);
    static_assert(std::is_same_v<decltype(cref.tryGet(ShapeId{1, 1})), int const*>);
    EXPECT_EQ(cref.get(ShapeId{1, 1}), 5);
    EXPECT_EQ(attr.tag().v, 1u);   // canonical accessor (tree() alias kept for callers)
}

TEST(ArenaAttribute, PromotionFloorAndBoundary) {
    // Below the floor (nodeCount 15 < 16): never promotes even at full coverage.
    {
        auto arena = buildArena(14, 1);   // nodeCount 15
        ASSERT_EQ(arena.nodeCount(), 15u);
        ArenaAttribute<ShapeArena, int> attr{arena};
        for (std::uint32_t i = 1; i <= 14; ++i) attr.set(ShapeId{i, 1}, 0);
        EXPECT_FALSE(attr.isDense());
    }
    // Floor edge (nodeCount 16): promotes at 8/16 == 50%.
    {
        auto arena = buildArena(15, 1);   // nodeCount 16
        ASSERT_EQ(arena.nodeCount(), 16u);
        ArenaAttribute<ShapeArena, int> attr{arena};
        for (std::uint32_t i = 1; i <= 7; ++i) attr.set(ShapeId{i, 1}, 0);
        EXPECT_FALSE(attr.isDense());     // 7/16 < 50%
        attr.set(ShapeId{8, 1}, 0);
        EXPECT_TRUE(attr.isDense());      // 8/16 == 50%
    }
    // Odd count (nodeCount 17): promotes at 9 not 8 — locks the `size*2 >= nc`
    // inequality direction.
    {
        auto arena = buildArena(16, 1);   // nodeCount 17
        ASSERT_EQ(arena.nodeCount(), 17u);
        ArenaAttribute<ShapeArena, int> attr{arena};
        for (std::uint32_t i = 1; i <= 8; ++i) attr.set(ShapeId{i, 1}, 0);
        EXPECT_FALSE(attr.isDense());     // 16 < 17
        attr.set(ShapeId{9, 1}, 0);
        EXPECT_TRUE(attr.isDense());      // 18 >= 17
    }
}

TEST(ArenaAttribute, DenseModeOperationsAndTaggedIteration) {
    auto arena = buildArena(20, 7);       // nodeCount 21
    ArenaAttribute<ShapeArena, int> attr{arena};
    for (std::uint32_t i = 1; i <= 11; ++i) attr.set(ShapeId{i, 7}, static_cast<int>(i * 10));
    ASSERT_TRUE(attr.isDense());

    EXPECT_TRUE(attr.has(ShapeId{5, 7}));
    EXPECT_EQ(attr.get(ShapeId{5, 7}), 50);
    attr.set(ShapeId{5, 7}, 55);          // overwrite keeps size
    EXPECT_EQ(attr.get(ShapeId{5, 7}), 55);
    EXPECT_EQ(attr.size(), 11u);
    EXPECT_TRUE(attr.erase(ShapeId{5, 7}));
    EXPECT_EQ(attr.tryGet(ShapeId{5, 7}), nullptr);

    // The dense iterator synthesizes ids tagged with the arena's tag.
    int seen = 0;
    for (auto const& [id, value] : attr) {
        EXPECT_EQ(id.arenaTag, 7u);
        ++seen;
        (void)value;
    }
    EXPECT_EQ(seen, 10);                   // 11 set − 1 erased
}

TEST(ArenaAttribute, EraseDoesNotDemoteAndLookupsDoNotPromote) {
    auto dense = buildArena(20, 1);
    ArenaAttribute<ShapeArena, int> attr{dense};
    for (std::uint32_t i = 1; i <= 11; ++i) attr.set(ShapeId{i, 1}, 0);
    ASSERT_TRUE(attr.isDense());
    for (std::uint32_t i = 1; i <= 11; ++i) attr.erase(ShapeId{i, 1});
    EXPECT_TRUE(attr.isDense());           // dense is sticky
    EXPECT_EQ(attr.size(), 0u);

    auto sparse = buildArena(20, 1);
    ArenaAttribute<ShapeArena, int> attr2{sparse};
    for (std::uint32_t i = 1; i <= 11; ++i) {
        (void)attr2.has(ShapeId{i, 1});
        (void)attr2.tryGet(ShapeId{i, 1});
        attr2.erase(ShapeId{i, 1});
    }
    EXPECT_FALSE(attr2.isDense());         // only set() promotes
}

TEST(ArenaAttribute, ClearAndReserveInDense) {
    auto arena = buildArena(20, 1);
    ArenaAttribute<ShapeArena, int> attr{arena};
    for (std::uint32_t i = 1; i <= 11; ++i) attr.set(ShapeId{i, 1}, 0);
    ASSERT_TRUE(attr.isDense());
    attr.reserve(500);                     // no-op in dense mode (must not throw)
    EXPECT_TRUE(attr.isDense());
    attr.clear();                          // resets to sparse
    EXPECT_FALSE(attr.isDense());
    EXPECT_TRUE(attr.empty());
    attr.set(ShapeId{1, 1}, 9);            // usable again
    EXPECT_EQ(attr.get(ShapeId{1, 1}), 9);
}

TEST(ArenaAttribute, OverValueArenaFamily) {
    // A second id/pod family flowing through the side-table (distinct std::hash).
    ValueBuilder b{ValueTag{3}};
    for (int i = 0; i < 3; ++i) b.addNode(ValuePod{static_cast<double>(i)});
    auto arena = std::move(b).finish();
    ArenaAttribute<ValueArena, std::string> attr{arena};
    attr.set(ValueId{1, 3}, "x");
    attr.set(ValueId{2, 3}, "y");
    EXPECT_EQ(attr.get(ValueId{1, 3}), "x");
    EXPECT_EQ(attr.size(), 2u);
    EXPECT_EQ(attr.tag().v, 3u);
}

TEST(ArenaAttributeDeathTest, EmptyArenaAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    ShapeArena emptyArena;                 // default ctor: zero nodes
    ArenaAttribute<ShapeArena, int> attr{emptyArena};
    EXPECT_DEATH({ attr.set(ShapeId{1}, 1); }, "out of bounds");
    EXPECT_DEATH({ attr.set(ShapeId{}, 1); }, "invalid ShapeId");
}
