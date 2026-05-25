// SP1: ArenaContainer + ArenaBuilder mechanics on non-Tree POD/id families,
// proving the arena substrate generalizes beyond detail::Node / NodeId / TreeId.

#include "core/substrate/arena_container.hpp"

#include "substrate/arena_test_types.hpp"

#include <gtest/gtest.h>

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
using ValueBuilder = ArenaBuilder<ValuePod, ValueId, ValueTag>;

} // namespace

TEST(ArenaContainer, BuilderEmitsTaggedIdsFromSlotOne) {
    ShapeBuilder b{ShapeTag{7}};
    EXPECT_EQ(b.size(), 1u);                 // slot 0 sentinel
    const ShapeId a = b.addNode(ShapePod{1, 10});
    const ShapeId c = b.addNode(ShapePod{2, 20});
    EXPECT_EQ(a.v, 1u);
    EXPECT_EQ(a.arenaTag, 7u);
    EXPECT_EQ(c.v, 2u);
    EXPECT_EQ(b.at(a).kind, 1);
    EXPECT_EQ(b.at(c).aux, 20u);
    EXPECT_EQ(b.size(), 3u);
}

TEST(ArenaContainer, FinishProducesFrozenContainer) {
    ShapeBuilder b{ShapeTag{42}};
    const ShapeId a = b.addNode(ShapePod{5, 0});
    ShapeArena arena = std::move(b).finish();

    EXPECT_EQ(arena.id().v, 42u);
    EXPECT_EQ(arena.nodeCount(), 2u);        // sentinel + one
    EXPECT_FALSE(arena.empty());
    EXPECT_EQ(arena.at(a).kind, 5);
    static_assert(std::is_same_v<ShapeArena::PodType, ShapePod>);
    static_assert(std::is_same_v<ShapeArena::IdType, ShapeId>);
    static_assert(std::is_same_v<ShapeArena::TagType, ShapeTag>);
}

TEST(ArenaContainer, TruncateToRollsBackTheArena) {
    ShapeBuilder b{ShapeTag{1}};
    b.addNode(ShapePod{1, 0});
    const std::size_t mark = b.size();       // 2
    b.addNode(ShapePod{2, 0});
    b.addNode(ShapePod{3, 0});
    EXPECT_EQ(b.size(), 4u);
    b.truncateTo(mark);
    EXPECT_EQ(b.size(), 2u);
}

TEST(ArenaContainer, DefaultIsEmpty) {
    ShapeArena arena;
    EXPECT_TRUE(arena.empty());
    EXPECT_EQ(arena.nodeCount(), 0u);
    EXPECT_FALSE(arena.id().valid());
}

TEST(ArenaContainer, SecondArenaFamilyAlsoWorks) {
    // A distinct pod/id/tag triple — the substrate isn't NodeId-specific.
    ValueBuilder b{ValueTag{3}};
    const ValueId a = b.addNode(ValuePod{2.5});
    auto arena = std::move(b).finish();
    EXPECT_EQ(arena.id().v, 3u);
    EXPECT_DOUBLE_EQ(arena.at(a).weight, 2.5);
}

TEST(ArenaContainer, MoveOnly) {
    static_assert(!std::is_copy_constructible_v<ShapeArena>);
    static_assert(std::is_move_constructible_v<ShapeArena>);
    static_assert(!std::is_copy_constructible_v<ShapeBuilder>);
    static_assert(std::is_move_constructible_v<ShapeBuilder>);
}

TEST(ArenaContainer, TruncateThenReAddResyncsIndexAndConstAt) {
    ShapeBuilder b{ShapeTag{5}};
    b.addNode(ShapePod{1, 0});
    const std::size_t mark = b.size();          // 2
    b.addNode(ShapePod{2, 0});
    b.truncateTo(mark);
    const ShapeId re = b.addNode(ShapePod{3, 0});   // must reuse slot `mark`
    EXPECT_EQ(re.v, static_cast<std::uint32_t>(mark));
    EXPECT_EQ(re.arenaTag, 5u);
    ShapeBuilder const& cb = b;
    EXPECT_EQ(cb.at(re).kind, 3);               // const at() overload
}

TEST(ArenaContainerDeathTest, OutOfRangeAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    ShapeBuilder b{ShapeTag{1}};
    b.addNode(ShapePod{1, 0});
    ShapeArena arena = std::move(b).finish();
    EXPECT_DEATH({ (void)arena.at(ShapeId{999, 1}); }, "ShapeId out of range");
    EXPECT_DEATH({ (void)arena.at(ShapeId{}); }, "ShapeId out of range");
}

TEST(ArenaContainerDeathTest, BuilderAtRejectsForeignArenaId) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    ShapeBuilder b{ShapeTag{111}};
    b.addNode(ShapePod{1, 0});
    EXPECT_DEATH({ (void)b.at(ShapeId{1, 222}); },
                 "ShapeId from ShapeTag=222 used on ShapeTag=111");
}

TEST(ArenaContainerDeathTest, TruncateBelowSentinelAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    ShapeBuilder b{ShapeTag{1}};
    b.addNode(ShapePod{1, 0});
    EXPECT_DEATH({ b.truncateTo(0); }, "truncateTo size out of range");
}
