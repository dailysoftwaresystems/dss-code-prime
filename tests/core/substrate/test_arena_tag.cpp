// SP1: the cross-arena guard — an id minted by one arena instance must abort
// when used against a different instance (the generalization of SH3's
// cross-tree arenaTag discipline). Untagged literals still pass (test
// ergonomics). Distinct arena *types* can't be confused at all — that's a
// compile-time type mismatch, a strictly stronger guarantee.

#include "core/substrate/arena_attribute.hpp"
#include "core/substrate/arena_container.hpp"

#include "substrate/arena_test_types.hpp"

#include <gtest/gtest.h>

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

[[nodiscard]] ShapeArena oneNodeArena(std::uint32_t tag) {
    ShapeBuilder b{ShapeTag{tag}};
    b.addNode(ShapePod{1, 0});
    return std::move(b).finish();
}

} // namespace

TEST(ArenaTag, UntaggedLiteralPasses) {
    auto arena = oneNodeArena(111);
    ArenaAttribute<ShapeArena, int> attr{arena};
    attr.set(ShapeId{1}, 42);                       // arenaTag == 0 — must pass
    EXPECT_EQ(attr.get(ShapeId{1, 111}), 42);       // same slot, tagged equivalent
    EXPECT_EQ(arena.at(ShapeId{1}).kind, 1);        // container accepts untagged too
}

TEST(ArenaTagDeathTest, ContainerRejectsForeignArenaId) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto arenaA = oneNodeArena(111);
    // An id tagged with arena B (222) used against arena A (111).
    EXPECT_DEATH({ (void)arenaA.at(ShapeId{1, 222}); },
                 "ShapeId from ShapeTag=222 used on ShapeTag=111");
}

TEST(ArenaTagDeathTest, AttributeRejectsForeignArenaId) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto arenaA = oneNodeArena(111);
    ArenaAttribute<ShapeArena, int> attr{arenaA};
    EXPECT_DEATH({ attr.set(ShapeId{1, 222}, 1); },
                 "ShapeAttr bound to ShapeTag=111 got ShapeId from ShapeTag=222");
}

TEST(ArenaTagDeathTest, SecondArenaFamilyAlsoGuardsAndUsesItsOwnNames) {
    // The guard + ArenaNames wording are per-arena: a Value-family attribute must
    // reject a foreign Value id and surface the <ValueId, ValueTag> diagnostic
    // names, not the Shape family's. Pins the second ArenaNames specialization
    // end-to-end (the Shape death tests can't reach it).
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    ValueBuilder b{ValueTag{111}};
    b.addNode(ValuePod{1.0});
    auto arenaA = std::move(b).finish();
    ArenaAttribute<ValueArena, int> attr{arenaA};
    EXPECT_DEATH({ attr.set(ValueId{1, 222}, 1); },
                 "ValueAttr bound to ValueTag=111 got ValueId from ValueTag=222");
}

TEST(ArenaTagDeathTest, DenseIteratorIdRejectedByForeignAttribute) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // Promote arena A (tag 111) to dense; the iterator synthesizes ids from
    // bare indices, so it must stamp tag 111. Feeding such an id to arena B's
    // (tag 222) attribute must abort — the regression guard for the dense
    // iterator's id-tagging.
    ShapeBuilder ba{ShapeTag{111}};
    for (int i = 0; i < 20; ++i) ba.addNode(ShapePod{i, 0});
    auto a = std::move(ba).finish();
    ArenaAttribute<ShapeArena, int> attrA{a};
    for (std::uint32_t i = 1; i <= 11; ++i) attrA.set(ShapeId{i, 111}, 0);
    ASSERT_TRUE(attrA.isDense());
    const ShapeId yielded = (*attrA.begin()).first;
    ASSERT_EQ(yielded.arenaTag, 111u);

    auto b = oneNodeArena(222);
    ArenaAttribute<ShapeArena, int> attrB{b};
    EXPECT_DEATH({ attrB.set(yielded, 1); },
                 "ShapeAttr bound to ShapeTag=222 got ShapeId from ShapeTag=111");
}
