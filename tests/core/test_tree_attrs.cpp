#include "core/types/rule_id.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_attrs.hpp"
#include "core/types/tree_node.hpp"
#include "raw_tree_builder.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace dss;
using dss::tests::RawTreeBuilder;

namespace {

Tree buildFlatTree(int leafCount, std::string source = {}) {
    if (source.empty()) source.assign(static_cast<std::size_t>(leafCount), 'x');
    RawTreeBuilder rb{std::move(source)};
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

template <typename Attr>
std::map<std::uint32_t, std::string> collectStringMap(Attr const& attr) {
    std::map<std::uint32_t, std::string> out;
    for (auto const& [id, val] : attr) {
        out.emplace(id.v, val);
    }
    return out;
}

template <typename Attr>
std::map<std::uint32_t, int> collectIntMap(Attr const& attr) {
    std::map<std::uint32_t, int> out;
    for (auto const& [id, val] : attr) {
        out.emplace(id.v, val);
    }
    return out;
}

} // namespace

// ── identity / introspection ─────────────────────────────────────────────

TEST(NodeAttribute, ReportsBoundTreeId) {
    Tree t = buildFlatTree(3);
    NodeAttribute<std::string> attr{t};
    EXPECT_EQ(attr.tree(), t.id());
}

TEST(NodeAttribute, TwoAttributesOnDistinctTreesCaptureDistinctIds) {
    // RawTreeBuilder defaults TreeId to {1}; pass explicit distinct ids
    // to exercise the "captures, not aliases" contract.
    auto build = [](TreeId id) {
        RawTreeBuilder rb{"x"};
        const auto rule = rb.internRule("root");
        rb.addNode(NodeKind::Internal, rule, SourceSpan::of(0, 1),
                   NodeFlags::None, InvalidNode);
        return std::move(rb).finish(/*root=*/ NodeId{1}, id);
    };
    Tree a = build(TreeId{111});
    Tree b = build(TreeId{222});
    ASSERT_NE(a.id(), b.id());
    NodeAttribute<int> attrA{a};
    NodeAttribute<int> attrB{b};
    EXPECT_NE(attrA.tree(), attrB.tree());
    EXPECT_EQ(attrA.tree(), a.id());
    EXPECT_EQ(attrB.tree(), b.id());
}

TEST(NodeAttribute, NewlyConstructedIsEmptyAndSparse) {
    Tree t = buildFlatTree(3);
    NodeAttribute<std::string> attr{t};
    EXPECT_EQ(attr.size(), 0u);
    EXPECT_TRUE(attr.empty());
    EXPECT_FALSE(attr.isDense());
}

TEST(NodeAttribute, ConstGetReturnsConstReference) {
    Tree t = buildFlatTree(3);
    NodeAttribute<int> attr{t};
    attr.set(NodeId{2}, 7);
    NodeAttribute<int> const& cref = attr;
    static_assert(std::is_same_v<decltype(cref.get(NodeId{2})), int const&>);
    static_assert(std::is_same_v<decltype(cref.tryGet(NodeId{2})), int const*>);
    EXPECT_EQ(cref.get(NodeId{2}), 7);
}

// ── core sparse operations ───────────────────────────────────────────────

TEST(NodeAttribute, SetAndGetRoundtripSparse) {
    Tree t = buildFlatTree(3);
    NodeAttribute<std::string> attr{t};
    attr.set(NodeId{2}, "hello");
    EXPECT_TRUE(attr.has(NodeId{2}));
    EXPECT_EQ(attr.get(NodeId{2}), "hello");
    EXPECT_EQ(attr.size(), 1u);
    EXPECT_FALSE(attr.empty());
}

TEST(NodeAttribute, SetOverwritesExistingValue) {
    Tree t = buildFlatTree(3);
    NodeAttribute<std::string> attr{t};
    attr.set(NodeId{2}, "first");
    attr.set(NodeId{2}, "second");
    EXPECT_EQ(attr.get(NodeId{2}), "second");
    EXPECT_EQ(attr.size(), 1u);
}

TEST(NodeAttribute, HasReturnsFalseForUnsetNodes) {
    Tree t = buildFlatTree(3);
    NodeAttribute<std::string> attr{t};
    attr.set(NodeId{2}, "x");
    EXPECT_TRUE(attr.has(NodeId{2}));
    EXPECT_FALSE(attr.has(NodeId{3}));
}

TEST(NodeAttribute, TryGetReturnsNullptrForMissing) {
    Tree t = buildFlatTree(3);
    NodeAttribute<std::string> attr{t};
    EXPECT_EQ(attr.tryGet(NodeId{2}), nullptr);
    attr.set(NodeId{2}, "x");
    auto const* p = attr.tryGet(NodeId{2});
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, "x");
}

TEST(NodeAttribute, EraseRemovesEntry) {
    Tree t = buildFlatTree(3);
    NodeAttribute<std::string> attr{t};
    attr.set(NodeId{2}, "x");
    EXPECT_TRUE(attr.erase(NodeId{2}));
    EXPECT_FALSE(attr.has(NodeId{2}));
    EXPECT_EQ(attr.size(), 0u);
}

TEST(NodeAttribute, EraseReturnsFalseForMissing) {
    Tree t = buildFlatTree(3);
    NodeAttribute<std::string> attr{t};
    EXPECT_FALSE(attr.erase(NodeId{2}));
    attr.set(NodeId{2}, "x");
    EXPECT_TRUE(attr.erase(NodeId{2}));
    EXPECT_FALSE(attr.erase(NodeId{2}));
}

TEST(NodeAttribute, ReserveDoesNotChangeSizeOrModeSparse) {
    Tree t = buildFlatTree(3);
    NodeAttribute<std::string> attr{t};
    attr.reserve(100);
    EXPECT_EQ(attr.size(), 0u);
    EXPECT_FALSE(attr.isDense());
}

TEST(NodeAttribute, ReserveIsSafeNoOpInDenseMode) {
    // Regression guard: an inverted `if (!isDense())` guard inside reserve()
    // would call std::get<SparseMap_>() in dense mode and throw bad_variant_access.
    Tree t = buildFlatTree(20);
    NodeAttribute<int> attr{t};
    for (std::uint32_t v = 1; v <= 11; ++v) attr.set(NodeId{v}, static_cast<int>(v));
    ASSERT_TRUE(attr.isDense());
    attr.reserve(500);
    EXPECT_TRUE(attr.isDense());
    EXPECT_EQ(attr.size(), 11u);
    EXPECT_EQ(attr.get(NodeId{5}), 5);
}

TEST(NodeAttribute, ClearOnAlreadySparseRemainsSparseAndEmpty) {
    Tree t = buildFlatTree(3);
    NodeAttribute<int> attr{t};
    attr.set(NodeId{1}, 1);
    attr.clear();
    EXPECT_FALSE(attr.isDense());
    EXPECT_EQ(attr.size(), 0u);
}

// ── promotion ────────────────────────────────────────────────────────────

TEST(NodeAttribute, SmallTreeStaysSparseEvenAtFullCoverage) {
    Tree t = buildFlatTree(3);
    NodeAttribute<int> attr{t};
    for (std::uint32_t v = 1; v < t.nodeCount(); ++v) {
        attr.set(NodeId{v}, static_cast<int>(v));
    }
    EXPECT_FALSE(attr.isDense());
    EXPECT_EQ(attr.size(), t.nodeCount() - 1);
}

TEST(NodeAttribute, PromotionFloorBoundary_nc16Promotes) {
    // buildFlatTree(14) → nodeCount = 16 (the kPromoteFloor value). The
    // floor check is `nc < kPromoteFloor`, so nc == 16 is on the promote
    // side. Threshold = size * 2 >= 16 → size >= 8.
    Tree t = buildFlatTree(14);
    ASSERT_EQ(t.nodeCount(), 16u);
    NodeAttribute<int> attr{t};
    for (std::uint32_t v = 1; v <= 7; ++v) attr.set(NodeId{v}, static_cast<int>(v));
    EXPECT_FALSE(attr.isDense());
    attr.set(NodeId{8}, 8);
    EXPECT_TRUE(attr.isDense());
}

TEST(NodeAttribute, PromotionFloorBoundary_nc15StaysSparse) {
    // buildFlatTree(13) → nodeCount = 15, one below the floor. Even at
    // 100% coverage promotion must not trigger.
    Tree t = buildFlatTree(13);
    ASSERT_EQ(t.nodeCount(), 15u);
    NodeAttribute<int> attr{t};
    for (std::uint32_t v = 1; v < t.nodeCount(); ++v) attr.set(NodeId{v}, static_cast<int>(v));
    EXPECT_FALSE(attr.isDense());
}

TEST(NodeAttribute, PromotesToDenseAtFiftyPercentCoverage) {
    Tree t = buildFlatTree(20);
    ASSERT_EQ(t.nodeCount(), 22u);
    NodeAttribute<int> attr{t};
    for (std::uint32_t v = 1; v <= 10; ++v) attr.set(NodeId{v}, static_cast<int>(v));
    EXPECT_FALSE(attr.isDense());
    attr.set(NodeId{11}, 11);
    EXPECT_TRUE(attr.isDense());
    EXPECT_EQ(attr.size(), 11u);
}

TEST(NodeAttribute, PromotionThresholdAtOddNodeCount) {
    // buildFlatTree(15) → nodeCount = 17 (odd). Threshold = size * 2 >= 17
    // → size >= 9 (NOT size >= 8). Locks the inequality direction.
    Tree t = buildFlatTree(15);
    ASSERT_EQ(t.nodeCount(), 17u);
    NodeAttribute<int> attr{t};
    for (std::uint32_t v = 1; v <= 8; ++v) attr.set(NodeId{v}, static_cast<int>(v));
    EXPECT_FALSE(attr.isDense());
    attr.set(NodeId{9}, 9);
    EXPECT_TRUE(attr.isDense());
}

TEST(NodeAttribute, PromotionPreservesAllValues) {
    Tree t = buildFlatTree(20);
    NodeAttribute<std::string> attr{t};
    std::map<std::uint32_t, std::string> expected;
    for (std::uint32_t v = 1; v <= 11; ++v) {
        std::string val = "node-" + std::to_string(v);
        expected[v] = val;
        attr.set(NodeId{v}, std::move(val));
    }
    ASSERT_TRUE(attr.isDense());
    for (auto const& [v, val] : expected) {
        EXPECT_TRUE(attr.has(NodeId{v}));
        EXPECT_EQ(attr.get(NodeId{v}), val);
    }
    EXPECT_EQ(collectStringMap(attr), expected);
}

TEST(NodeAttribute, DenseModeOperationsAllWork) {
    Tree t = buildFlatTree(20);
    NodeAttribute<int> attr{t};
    for (std::uint32_t v = 1; v <= 11; ++v) attr.set(NodeId{v}, static_cast<int>(v));
    ASSERT_TRUE(attr.isDense());

    EXPECT_TRUE(attr.has(NodeId{5}));
    EXPECT_FALSE(attr.has(NodeId{15}));
    EXPECT_EQ(attr.get(NodeId{5}), 5);
    EXPECT_EQ(attr.tryGet(NodeId{15}), nullptr);
    ASSERT_NE(attr.tryGet(NodeId{5}), nullptr);
    EXPECT_EQ(*attr.tryGet(NodeId{5}), 5);

    EXPECT_TRUE(attr.erase(NodeId{5}));
    EXPECT_FALSE(attr.has(NodeId{5}));
    EXPECT_FALSE(attr.erase(NodeId{5}));
    EXPECT_EQ(attr.size(), 10u);
}

TEST(NodeAttribute, SetAddsNewEntryInDenseMode) {
    // Locks the `++denseCount_` increment on a previously-empty slot in
    // dense mode. Initial fill (1..11) creates dense entries; node 15 is
    // an empty slot until this set inserts.
    Tree t = buildFlatTree(20);
    NodeAttribute<int> attr{t};
    for (std::uint32_t v = 1; v <= 11; ++v) attr.set(NodeId{v}, static_cast<int>(v));
    ASSERT_TRUE(attr.isDense());
    ASSERT_EQ(attr.size(), 11u);

    attr.set(NodeId{15}, 99);
    EXPECT_TRUE(attr.isDense());
    EXPECT_EQ(attr.size(), 12u);
    EXPECT_EQ(attr.get(NodeId{15}), 99);

    // Overwriting must not bump size.
    attr.set(NodeId{15}, 100);
    EXPECT_EQ(attr.size(), 12u);
    EXPECT_EQ(attr.get(NodeId{15}), 100);
}

TEST(NodeAttribute, ClearResetsToSparseEvenFromDense) {
    Tree t = buildFlatTree(20);
    NodeAttribute<int> attr{t};
    for (std::uint32_t v = 1; v <= 11; ++v) attr.set(NodeId{v}, static_cast<int>(v));
    ASSERT_TRUE(attr.isDense());
    attr.clear();
    EXPECT_FALSE(attr.isDense());
    EXPECT_EQ(attr.size(), 0u);
    EXPECT_TRUE(attr.empty());

    attr.set(NodeId{1}, 42);
    EXPECT_EQ(attr.get(NodeId{1}), 42);
}

TEST(NodeAttribute, ClearedAttributeCanPromoteAgain) {
    Tree t = buildFlatTree(20);
    NodeAttribute<int> attr{t};
    for (std::uint32_t v = 1; v <= 11; ++v) attr.set(NodeId{v}, static_cast<int>(v));
    attr.clear();
    EXPECT_FALSE(attr.isDense());
    for (std::uint32_t v = 1; v <= 11; ++v) attr.set(NodeId{v}, static_cast<int>(v));
    EXPECT_TRUE(attr.isDense());
}

TEST(NodeAttribute, EraseBelowThresholdDoesNotDemote) {
    // Once dense, the table sticks. Erasing back below 50% coverage must
    // NOT demote back to sparse — a future auto-shrink feature would
    // change observable behavior; this test makes that surprise loud.
    Tree t = buildFlatTree(20);
    NodeAttribute<int> attr{t};
    for (std::uint32_t v = 1; v <= 11; ++v) attr.set(NodeId{v}, static_cast<int>(v));
    ASSERT_TRUE(attr.isDense());
    for (std::uint32_t v = 4; v <= 11; ++v) (void)attr.erase(NodeId{v});
    EXPECT_TRUE(attr.isDense());
    EXPECT_EQ(attr.size(), 3u);
}

TEST(NodeAttribute, PromotionOnlyHappensOnSet) {
    // Lookups must not be able to flip storage mode.
    Tree t = buildFlatTree(20);
    NodeAttribute<int> attr{t};
    for (std::uint32_t v = 1; v <= 5; ++v) attr.set(NodeId{v}, static_cast<int>(v));
    ASSERT_FALSE(attr.isDense());

    for (std::uint32_t v = 1; v <= 25; ++v) {
        (void)attr.has(NodeId{v % 20 + 1});
        (void)attr.tryGet(NodeId{v % 20 + 1});
    }
    (void)attr.erase(NodeId{1});
    EXPECT_FALSE(attr.isDense());
}

// ── mutation through get / tryGet ────────────────────────────────────────

TEST(NodeAttribute, MutationViaNonConstGetPersists) {
    Tree t = buildFlatTree(3);
    NodeAttribute<std::string> attr{t};
    attr.set(NodeId{2}, "first");
    attr.get(NodeId{2}).append("-mut");
    EXPECT_EQ(attr.get(NodeId{2}), "first-mut");
}

TEST(NodeAttribute, MutationViaTryGetPersists) {
    Tree t = buildFlatTree(3);
    NodeAttribute<std::string> attr{t};
    attr.set(NodeId{2}, "first");
    auto* p = attr.tryGet(NodeId{2});
    ASSERT_NE(p, nullptr);
    p->append("-mut");
    EXPECT_EQ(attr.get(NodeId{2}), "first-mut");
}

TEST(NodeAttribute, MutationPersistsAcrossPromotion) {
    Tree t = buildFlatTree(20);
    NodeAttribute<std::string> attr{t};
    attr.set(NodeId{2}, "stable");
    attr.get(NodeId{2}).append("-mut");
    for (std::uint32_t v = 3; v <= 12; ++v) attr.set(NodeId{v}, "x");
    ASSERT_TRUE(attr.isDense());
    EXPECT_EQ(attr.get(NodeId{2}), "stable-mut");

    attr.get(NodeId{2}).append("-more");
    EXPECT_EQ(attr.get(NodeId{2}), "stable-mut-more");
}

// ── iteration ────────────────────────────────────────────────────────────

TEST(NodeAttribute, IterateEmptyYieldsNothing) {
    Tree t = buildFlatTree(3);
    NodeAttribute<int> attr{t};
    EXPECT_EQ(attr.begin(), attr.end());

    int visited = 0;
    for (auto const& kv : attr) { (void)kv; ++visited; }
    EXPECT_EQ(visited, 0);
}

TEST(NodeAttribute, IterateSparseYieldsAllEntries) {
    Tree t = buildFlatTree(3);
    NodeAttribute<std::string> attr{t};
    attr.set(NodeId{1}, "a");
    attr.set(NodeId{3}, "c");
    ASSERT_FALSE(attr.isDense());

    const std::map<std::uint32_t, std::string> expected{{1, "a"}, {3, "c"}};
    EXPECT_EQ(collectStringMap(attr), expected);
}

TEST(NodeAttribute, IterateDenseYieldsAllEntries) {
    Tree t = buildFlatTree(20);
    NodeAttribute<std::string> attr{t};
    std::map<std::uint32_t, std::string> expected;
    for (std::uint32_t v = 1; v <= 11; ++v) {
        std::string val = "n" + std::to_string(v);
        expected[v] = val;
        attr.set(NodeId{v}, std::move(val));
    }
    ASSERT_TRUE(attr.isDense());
    EXPECT_EQ(collectStringMap(attr), expected);
}

TEST(NodeAttribute, IterateDenseSkipsInternalGaps) {
    // Critical for the dense iterator's nullopt-skip loop. Construct a
    // "live, gap, live, gap, live" pattern by promoting then erasing
    // alternating entries.
    Tree t = buildFlatTree(20);
    NodeAttribute<int> attr{t};
    for (std::uint32_t v = 1; v <= 11; ++v) attr.set(NodeId{v}, static_cast<int>(v));
    ASSERT_TRUE(attr.isDense());

    // Leave 1, 3, 5, 7, 9, 11 set; erase the even ids in [2..11].
    for (std::uint32_t v = 2; v <= 10; v += 2) {
        ASSERT_TRUE(attr.erase(NodeId{v}));
    }
    ASSERT_TRUE(attr.isDense());

    const std::map<std::uint32_t, int> expected{
        {1, 1}, {3, 3}, {5, 5}, {7, 7}, {9, 9}, {11, 11},
    };
    EXPECT_EQ(collectIntMap(attr), expected);
}

TEST(NodeAttribute, IterateSparseAfterEraseSkipsRemoved) {
    Tree t = buildFlatTree(5);
    NodeAttribute<int> attr{t};
    attr.set(NodeId{1}, 10);
    attr.set(NodeId{3}, 30);
    attr.set(NodeId{5}, 50);
    ASSERT_TRUE(attr.erase(NodeId{3}));
    ASSERT_FALSE(attr.isDense());

    const std::map<std::uint32_t, int> expected{{1, 10}, {5, 50}};
    EXPECT_EQ(collectIntMap(attr), expected);
}

TEST(NodeAttribute, ConstIterationCompiles) {
    Tree t = buildFlatTree(3);
    NodeAttribute<std::string> attr{t};
    attr.set(NodeId{2}, "x");

    NodeAttribute<std::string> const& cref = attr;
    int visited = 0;
    for (auto const& kv : cref) { (void)kv; ++visited; }
    EXPECT_EQ(visited, 1);
}

TEST(NodeAttribute, NonConstIterationAllowsMutationSparse) {
    Tree t = buildFlatTree(3);
    NodeAttribute<std::string> attr{t};
    attr.set(NodeId{1}, "a");
    attr.set(NodeId{2}, "b");
    attr.set(NodeId{3}, "c");
    ASSERT_FALSE(attr.isDense());

    // Yielded pair copies the reference — mutation goes through to storage.
    for (auto kv : attr) {
        kv.second.append("!");
    }
    EXPECT_EQ(attr.get(NodeId{1}), "a!");
    EXPECT_EQ(attr.get(NodeId{2}), "b!");
    EXPECT_EQ(attr.get(NodeId{3}), "c!");
}

TEST(NodeAttribute, NonConstIterationAllowsMutationDense) {
    Tree t = buildFlatTree(20);
    NodeAttribute<std::string> attr{t};
    for (std::uint32_t v = 1; v <= 11; ++v) attr.set(NodeId{v}, "x");
    ASSERT_TRUE(attr.isDense());

    for (auto kv : attr) {
        kv.second.append("!");
    }
    for (std::uint32_t v = 1; v <= 11; ++v) {
        EXPECT_EQ(attr.get(NodeId{v}), "x!");
    }
}

// ── T variations ─────────────────────────────────────────────────────────

TEST(NodeAttribute, MoveOnlyTSupported) {
    Tree t = buildFlatTree(3);
    NodeAttribute<std::unique_ptr<int>> attr{t};
    attr.set(NodeId{2}, std::make_unique<int>(42));
    ASSERT_TRUE(attr.has(NodeId{2}));
    EXPECT_EQ(*attr.get(NodeId{2}), 42);

    *attr.get(NodeId{2}) = 99;
    EXPECT_EQ(*attr.get(NodeId{2}), 99);

    // Move-only T: exercises std::move in the promotion loop.
    Tree t2 = buildFlatTree(20);
    NodeAttribute<std::unique_ptr<int>> bigAttr{t2};
    for (std::uint32_t v = 1; v <= 11; ++v) {
        bigAttr.set(NodeId{v}, std::make_unique<int>(static_cast<int>(v)));
    }
    ASSERT_TRUE(bigAttr.isDense());
    EXPECT_EQ(*bigAttr.get(NodeId{5}), 5);
}

// ── move semantics on the attribute itself ───────────────────────────────

TEST(NodeAttribute, MoveConstructTransfersState) {
    Tree t = buildFlatTree(3);
    NodeAttribute<std::string> src{t};
    src.set(NodeId{2}, "x");

    NodeAttribute<std::string> moved{std::move(src)};
    EXPECT_EQ(moved.tree(), t.id());
    EXPECT_TRUE(moved.has(NodeId{2}));
    EXPECT_EQ(moved.get(NodeId{2}), "x");
    EXPECT_EQ(moved.size(), 1u);

    // Moved-from object is valid but unspecified; size() must not crash
    // and the standard contract for moved-from unordered_map/vector is
    // "valid, empty-ish state".
    EXPECT_EQ(src.size(), 0u);
}

TEST(NodeAttribute, MoveAssignTransfersState) {
    Tree t = buildFlatTree(20);
    NodeAttribute<std::string> src{t};
    for (std::uint32_t v = 1; v <= 11; ++v) src.set(NodeId{v}, "x");
    ASSERT_TRUE(src.isDense());

    NodeAttribute<std::string> sink{t};
    sink = std::move(src);
    EXPECT_TRUE(sink.isDense());
    EXPECT_EQ(sink.size(), 11u);
    EXPECT_EQ(sink.get(NodeId{5}), "x");
    EXPECT_EQ(src.size(), 0u);
}

// ── bounds / fatal paths (death tests) ───────────────────────────────────

TEST(NodeAttributeDeath, SetWithInvalidSentinelAborts) {
    Tree t = buildFlatTree(3);
    NodeAttribute<int> attr{t};
    EXPECT_DEATH({ attr.set(InvalidNode, 1); }, "invalid NodeId");
}

TEST(NodeAttributeDeath, SetWithOutOfRangeNodeIdAborts) {
    Tree t = buildFlatTree(3);
    NodeAttribute<int> attr{t};
    const auto past = NodeId{static_cast<std::uint32_t>(t.nodeCount())};
    EXPECT_DEATH({ attr.set(past, 1); }, "out of bounds");
}

TEST(NodeAttributeDeath, GetWithOutOfRangeNodeIdAborts) {
    Tree t = buildFlatTree(3);
    NodeAttribute<int> attr{t};
    const auto past = NodeId{static_cast<std::uint32_t>(t.nodeCount() + 5)};
    EXPECT_DEATH({ (void)attr.get(past); }, "out of bounds");
}

TEST(NodeAttributeDeath, HasWithInvalidSentinelAborts) {
    Tree t = buildFlatTree(3);
    NodeAttribute<int> attr{t};
    EXPECT_DEATH({ (void)attr.has(InvalidNode); }, "invalid NodeId");
}

TEST(NodeAttributeDeath, TryGetWithOutOfRangeNodeIdAborts) {
    Tree t = buildFlatTree(3);
    NodeAttribute<int> attr{t};
    const auto past = NodeId{static_cast<std::uint32_t>(t.nodeCount())};
    EXPECT_DEATH({ (void)attr.tryGet(past); }, "out of bounds");
}

TEST(NodeAttributeDeath, EraseWithInvalidSentinelAborts) {
    Tree t = buildFlatTree(3);
    NodeAttribute<int> attr{t};
    EXPECT_DEATH({ (void)attr.erase(InvalidNode); }, "invalid NodeId");
}

TEST(NodeAttributeDeath, GetOnAbsentNodeIdAbortsSparse) {
    Tree t = buildFlatTree(3);
    NodeAttribute<int> attr{t};
    EXPECT_DEATH({ (void)attr.get(NodeId{2}); }, "get: no value");
}

TEST(NodeAttributeDeath, GetOnAbsentNodeIdAbortsDense) {
    Tree t = buildFlatTree(20);
    NodeAttribute<int> attr{t};
    for (std::uint32_t v = 1; v <= 11; ++v) attr.set(NodeId{v}, static_cast<int>(v));
    ASSERT_TRUE(attr.isDense());
    EXPECT_DEATH({ (void)attr.get(NodeId{15}); }, "get: no value");
}

// ── empty-tree (nodeCount == 0) fatals on every API entry ────────────────

namespace {
Tree makeEmptyTree() {
    detail::TreeData td;
    td.arena = substrate::ArenaContainer<detail::Node, NodeId, TreeId>{std::vector<detail::Node>{}, TreeId{4242}};
    return Tree{std::move(td)};
}
} // namespace

TEST(NodeAttributeDeath, EmptyTreeSetAborts) {
    Tree empty = makeEmptyTree();
    NodeAttribute<int> attr{empty};
    // Any NodeId is out of bounds; the sentinel check fires first for NodeId{}.
    EXPECT_DEATH({ attr.set(NodeId{1}, 1); }, "out of bounds");
    EXPECT_DEATH({ attr.set(InvalidNode, 1); }, "invalid NodeId");
}

TEST(NodeAttributeDeath, EmptyTreeHasAborts) {
    Tree empty = makeEmptyTree();
    NodeAttribute<int> attr{empty};
    EXPECT_DEATH({ (void)attr.has(NodeId{1}); }, "out of bounds");
}

TEST(NodeAttributeDeath, EmptyTreeGetAborts) {
    Tree empty = makeEmptyTree();
    NodeAttribute<int> attr{empty};
    EXPECT_DEATH({ (void)attr.get(NodeId{1}); }, "out of bounds");
}

TEST(NodeAttributeDeath, EmptyTreeTryGetAborts) {
    Tree empty = makeEmptyTree();
    NodeAttribute<int> attr{empty};
    EXPECT_DEATH({ (void)attr.tryGet(NodeId{1}); }, "out of bounds");
}

TEST(NodeAttributeDeath, EmptyTreeEraseAborts) {
    Tree empty = makeEmptyTree();
    NodeAttribute<int> attr{empty};
    EXPECT_DEATH({ (void)attr.erase(NodeId{1}); }, "out of bounds");
}

TEST(NodeAttribute, EmptyTreeIterationYieldsNothing) {
    Tree empty = makeEmptyTree();
    NodeAttribute<int> attr{empty};
    EXPECT_EQ(attr.size(), 0u);
    EXPECT_TRUE(attr.empty());
    EXPECT_EQ(attr.begin(), attr.end());
}

// ── cross-tree NodeId guard (SH3) ────────────────────────────────────────
//
// `NodeAttribute<T>` is bound to one Tree at construction. Passing a NodeId
// that was minted by a different Tree must abort with both TreeIds in the
// message so the death-test regex can pin both numbers. Untagged literals
// (`NodeId{N}`) bypass the cross-tree check — those are the existing test-
// ergonomic path; the bounds check still catches out-of-range untagged ids.

namespace {
Tree buildFlatTreeWithId(int leafCount, TreeId id) {
    std::string source(static_cast<std::size_t>(leafCount), 'x');
    RawTreeBuilder rb{std::move(source), "<test>", id};
    const auto rule = rb.internRule("root");
    std::vector<NodeId> kids;
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

TEST(NodeAttributeDeath, SetWithForeignTreeNodeIdAborts) {
    Tree a = buildFlatTreeWithId(3, TreeId{111});
    Tree b = buildFlatTreeWithId(3, TreeId{222});
    NodeAttribute<int> attrA{a};
    NodeId idFromB = b.root();
    ASSERT_NE(idFromB.treeTag, 0u);
    EXPECT_DEATH({ attrA.set(idFromB, 1); },
                 "NodeAttribute bound to TreeId=111 got NodeId from TreeId=222");
}

TEST(NodeAttributeDeath, GetWithForeignTreeNodeIdAborts) {
    Tree a = buildFlatTreeWithId(3, TreeId{111});
    Tree b = buildFlatTreeWithId(3, TreeId{222});
    NodeAttribute<int> attrA{a};
    attrA.set(NodeId{1}, 7);
    EXPECT_DEATH({ (void)attrA.get(b.root()); },
                 "NodeAttribute bound to TreeId=111 got NodeId from TreeId=222");
}

TEST(NodeAttributeDeath, HasWithForeignTreeNodeIdAborts) {
    Tree a = buildFlatTreeWithId(3, TreeId{111});
    Tree b = buildFlatTreeWithId(3, TreeId{222});
    NodeAttribute<int> attrA{a};
    EXPECT_DEATH({ (void)attrA.has(b.root()); },
                 "NodeAttribute bound to TreeId=111 got NodeId from TreeId=222");
}

TEST(NodeAttributeDeath, TryGetWithForeignTreeNodeIdAborts) {
    Tree a = buildFlatTreeWithId(3, TreeId{111});
    Tree b = buildFlatTreeWithId(3, TreeId{222});
    NodeAttribute<int> attrA{a};
    EXPECT_DEATH({ (void)attrA.tryGet(b.root()); },
                 "NodeAttribute bound to TreeId=111 got NodeId from TreeId=222");
}

TEST(NodeAttributeDeath, EraseWithForeignTreeNodeIdAborts) {
    Tree a = buildFlatTreeWithId(3, TreeId{111});
    Tree b = buildFlatTreeWithId(3, TreeId{222});
    NodeAttribute<int> attrA{a};
    attrA.set(NodeId{1}, 7);
    EXPECT_DEATH({ (void)attrA.erase(b.root()); },
                 "NodeAttribute bound to TreeId=111 got NodeId from TreeId=222");
}

TEST(NodeAttribute, UntaggedLiteralPassesValidator) {
    // Hand-fabricated NodeId literals (treeTag == 0) MUST pass the cross-
    // tree validator — existing tests rely on `NodeId{N}` shorthand. The
    // bounds check is still the catch for genuinely-bad ids.
    Tree a = buildFlatTreeWithId(3, TreeId{111});
    NodeAttribute<int> attrA{a};
    NodeId untagged{1};
    ASSERT_EQ(untagged.treeTag, 0u);
    attrA.set(untagged, 42);
    EXPECT_EQ(attrA.get(untagged), 42);
    EXPECT_EQ(attrA.get(NodeId(1, 111)), 42);  // same .v, tagged equivalent
}

TEST(NodeAttribute, MoveTransfersBoundTreeId) {
    Tree a = buildFlatTreeWithId(3, TreeId{111});
    Tree b = buildFlatTreeWithId(3, TreeId{222});
    NodeAttribute<int> attrA{a};
    attrA.set(NodeId{1}, 7);
    NodeAttribute<int> moved = std::move(attrA);
    EXPECT_EQ(moved.tree(), a.id());
    EXPECT_EQ(moved.get(NodeId{1}), 7);
}

TEST(NodeAttributeDeath, MovedAttributeRejectsForeignNodeId) {
    Tree a = buildFlatTreeWithId(3, TreeId{111});
    Tree b = buildFlatTreeWithId(3, TreeId{222});
    NodeAttribute<int> attrA{a};
    NodeAttribute<int> moved = std::move(attrA);
    EXPECT_DEATH({ moved.set(b.root(), 1); },
                 "NodeAttribute bound to TreeId=111 got NodeId from TreeId=222");
}

TEST(NodeAttributeDeath, IteratorYieldsTaggedIdsAfterPromotion) {
    // Verify dense-mode iteration produces tagged NodeIds (bug class: post-
    // promotion iteration synthesizes a NodeId from just the arena index;
    // without explicit tagging, those would be untagged and cross-tree-
    // pass silently when forwarded to another attribute).
    Tree a = buildFlatTreeWithId(20, TreeId{111});
    Tree b = buildFlatTreeWithId(20, TreeId{222});
    NodeAttribute<int> attrA{a};
    for (std::uint32_t v = 1; v <= 11; ++v) attrA.set(NodeId{v}, static_cast<int>(v));
    ASSERT_TRUE(attrA.isDense());

    NodeAttribute<int> attrB{b};
    auto it = attrA.begin();
    NodeId yielded = (*it).first;
    EXPECT_EQ(yielded.treeTag, 111u);
    EXPECT_DEATH({ attrB.set(yielded, 1); },
                 "NodeAttribute bound to TreeId=222 got NodeId from TreeId=111");
}

// ── cross-tree Tree::children / accessors (SH3) ──────────────────────────

TEST(TreeDeath, ChildrenWithForeignNodeIdAborts) {
    Tree a = buildFlatTreeWithId(3, TreeId{111});
    Tree b = buildFlatTreeWithId(3, TreeId{222});
    EXPECT_DEATH({ (void)a.children(b.root()); },
                 "NodeId from TreeId=222 used on TreeId=111");
}

TEST(TreeDeath, KindWithForeignNodeIdAborts) {
    Tree a = buildFlatTreeWithId(3, TreeId{111});
    Tree b = buildFlatTreeWithId(3, TreeId{222});
    EXPECT_DEATH({ (void)a.kind(b.root()); },
                 "NodeId from TreeId=222 used on TreeId=111");
}

TEST(Tree, ChildrenOnSameTreeReturnsTaggedIds) {
    Tree a = buildFlatTreeWithId(3, TreeId{111});
    auto kids = a.children(a.root());
    ASSERT_EQ(kids.size(), 3u);
    for (NodeId k : kids) {
        EXPECT_EQ(k.treeTag, 111u);
    }
}
