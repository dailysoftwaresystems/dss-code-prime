// CU3 contract tests for `UnitAttribute<T>` — the CompilationUnit-scoped
// NodeId → T side-table. Pins the routing-across-trees behavior, the
// membership-based cross-CU NodeId guard (the SH3-analog deferred from CU1
// §2.5 D3), the untagged-literal routing rule, and the duplicate-TreeId
// construction guard.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/compilation_unit/unit_attribute.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/tree_node.hpp"
#include "core/types/tree_visitor.hpp"

#include "core/raw_tree_builder.hpp"
#include "analysis/compilation_unit/toy_cu_fixture.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace dss;
using dss::cu_test::makeToyUnit;

// A tagged NodeId whose treeTag (4242) belongs to no tree in any test CU here
// (CUs are built from toy sources, whose trees get small monotonic ids) — i.e.
// a stand-in for a NodeId leaked from another CompilationUnit's tree. The
// fixed tag makes the cross-CU death-test regex exact and swap-proof.
constexpr std::uint32_t kForeignTag = 4242;
constexpr NodeId        kForeignNode{1, kForeignTag};

// Encodes (treeTag, NodeId.v) so forEach output can be compared exactly
// without depending on per-tree iteration order.
[[nodiscard]] std::uint64_t key(TreeId tree, NodeId node) {
    return (static_cast<std::uint64_t>(tree.v) << 32) | node.v;
}

} // namespace

// ── routing across trees ──────────────────────────────────────────────────

TEST(UnitAttribute, SetGetRoutesAcrossTrees) {
    auto cu = makeToyUnit({"var a = x;", "var b = y;"});
    ASSERT_EQ(cu.trees().size(), 2u);
    NodeId const r0 = cu.trees()[0].root();
    NodeId const r1 = cu.trees()[1].root();

    UnitAttribute<int> attr{cu};
    attr.set(r0, 10);
    attr.set(r1, 20);

    EXPECT_EQ(attr.size(), 2u);
    EXPECT_TRUE(attr.has(r0));
    EXPECT_TRUE(attr.has(r1));
    EXPECT_EQ(attr.get(r0), 10);
    EXPECT_EQ(attr.get(r1), 20);
    EXPECT_EQ(attr.unit(), cu.id());
}

TEST(UnitAttribute, EraseRoutesToCorrectTree) {
    auto cu = makeToyUnit({"var a = x;", "var b = y;"});
    NodeId const r0 = cu.trees()[0].root();
    NodeId const r1 = cu.trees()[1].root();

    UnitAttribute<int> attr{cu};
    attr.set(r0, 1);
    attr.set(r1, 2);

    EXPECT_TRUE(attr.erase(r0));
    EXPECT_FALSE(attr.erase(r0));   // already gone
    EXPECT_FALSE(attr.has(r0));
    EXPECT_TRUE(attr.has(r1));      // the other tree is untouched
    EXPECT_EQ(attr.get(r1), 2);
    EXPECT_EQ(attr.size(), 1u);
}

TEST(UnitAttribute, TryGetMissingReturnsNull) {
    auto cu = makeToyUnit({"var a = x;"});
    NodeId const root = cu.trees()[0].root();

    UnitAttribute<int> attr{cu};
    EXPECT_EQ(attr.tryGet(root), nullptr);
    attr.set(root, 7);
    ASSERT_NE(attr.tryGet(root), nullptr);
    EXPECT_EQ(*attr.tryGet(root), 7);
}

TEST(UnitAttribute, EmptyOnConstruction) {
    auto cu = makeToyUnit({"var a = x;", "var b = y;"});
    UnitAttribute<int> attr{cu};
    EXPECT_TRUE(attr.empty());
    EXPECT_EQ(attr.size(), 0u);
}

TEST(UnitAttribute, ForEachVisitsEveryEntryWithOwningTree) {
    auto cu = makeToyUnit({"var a = x;", "var b = y;"});
    TreeId const t0 = cu.trees()[0].id();
    TreeId const t1 = cu.trees()[1].id();
    NodeId const r0 = cu.trees()[0].root();
    NodeId const r1 = cu.trees()[1].root();

    UnitAttribute<int> attr{cu};
    attr.set(r0, 100);
    attr.set(r1, 200);

    std::map<std::uint64_t, int> seen;
    attr.forEach([&](TreeId tree, NodeId node, int const& value) {
        seen.emplace(key(tree, node), value);
    });

    std::map<std::uint64_t, int> const expected{
        {key(t0, r0), 100},
        {key(t1, r1), 200},
    };
    EXPECT_EQ(seen, expected);
}

TEST(UnitAttribute, ForEachOnEmptyVisitsNothing) {
    auto cu = makeToyUnit({"var a = x;", "var b = y;"});
    UnitAttribute<int> attr{cu};
    int count = 0;
    attr.forEach([&](TreeId, NodeId, int const&) { ++count; });
    EXPECT_EQ(count, 0);
}

TEST(UnitAttribute, ForEachOverDenseStorageYieldsTaggedEntries) {
    // Set every node in a tree large enough to promote the per-tree
    // NodeAttribute to dense storage, then verify forEach still yields each
    // entry with the correct owning TreeId (the dense iterator synthesizes
    // NodeIds and must tag them).
    auto cu = makeToyUnit({"var a = b; var c = d; var e = f; var g = h; var i = j;"});
    Tree const& tree = cu.trees()[0];
    ASSERT_GE(tree.nodeCount(), std::size_t{16});   // promotion floor

    std::vector<NodeId> nodes;
    walkPreOrder(tree, [&](TreeCursor const& cursor) { nodes.push_back(cursor.current()); });

    UnitAttribute<int> attr{cu};
    std::map<std::uint64_t, int> expected;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        attr.set(nodes[index], static_cast<int>(index));
        expected.emplace(key(tree.id(), nodes[index]), static_cast<int>(index));
    }
    ASSERT_EQ(attr.size(), nodes.size());

    std::map<std::uint64_t, int> seen;
    attr.forEach([&](TreeId tree_, NodeId node, int const& value) {
        seen.emplace(key(tree_, node), value);
    });
    EXPECT_EQ(seen, expected);
}

// ── untagged-literal routing ───────────────────────────────────────────────

TEST(UnitAttribute, UntaggedLiteralRoutesInSingleTreeUnit) {
    // A single-tree CU can route an untagged literal NodeId{v} to the sole
    // tree — preserving the same test ergonomics NodeAttribute offers. The
    // untagged set and the tagged read address the same arena slot (NodeId
    // equality is by .v).
    auto cu = makeToyUnit({"var a = x;"});
    NodeId const tagged = cu.trees()[0].root();
    ASSERT_NE(tagged.treeTag, 0u);
    NodeId const untagged{tagged.v};
    ASSERT_EQ(untagged.treeTag, 0u);

    UnitAttribute<int> attr{cu};
    attr.set(untagged, 42);
    EXPECT_EQ(attr.get(tagged), 42);
    EXPECT_EQ(attr.size(), 1u);
}

// ── type traits / const overloads ──────────────────────────────────────────

TEST(UnitAttribute, MoveOnly) {
    static_assert(!std::is_copy_constructible_v<UnitAttribute<int>>);
    static_assert(!std::is_copy_assignable_v<UnitAttribute<int>>);
    static_assert(std::is_move_constructible_v<UnitAttribute<int>>);
    static_assert(std::is_move_assignable_v<UnitAttribute<int>>);
    static_assert(std::is_same_v<UnitAttribute<int>::value_type, int>);
}

TEST(UnitAttribute, ConstAccessorsReturnConstReferences) {
    auto cu = makeToyUnit({"var a = x;"});
    NodeId const root = cu.trees()[0].root();
    UnitAttribute<int> attr{cu};
    attr.set(root, 5);

    UnitAttribute<int> const& cref = attr;
    static_assert(std::is_same_v<decltype(cref.get(root)), int const&>);
    static_assert(std::is_same_v<decltype(cref.tryGet(root)), int const*>);
    EXPECT_EQ(cref.get(root), 5);
    ASSERT_NE(cref.tryGet(root), nullptr);
    EXPECT_EQ(*cref.tryGet(root), 5);
}

TEST(UnitAttribute, MovedFromIsEmpty) {
    auto cu = makeToyUnit({"var a = x;", "var b = y;"});
    UnitAttribute<int> attr{cu};
    attr.set(cu.trees()[0].root(), 1);
    attr.set(cu.trees()[1].root(), 2);
    ASSERT_EQ(attr.size(), 2u);

    UnitAttribute<int> moved{std::move(attr)};
    EXPECT_EQ(moved.size(), 2u);
    EXPECT_EQ(attr.size(), 0u);     // NOLINT(bugprone-use-after-move)
    EXPECT_TRUE(attr.empty());      // NOLINT(bugprone-use-after-move)
    int count = 0;
    attr.forEach([&](TreeId, NodeId, int const&) { ++count; });  // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(count, 0);
}

// ── death tests: the cross-CU membership guard (SH3-analog, §2.5 D3) ────────
//
// Each NodeId-keyed entry point routes independently through route_, so the
// foreign-id abort is pinned on every one of them. The regex pins the exact
// (fixed) source TreeId, so a swapped crossUnitFatal(cuId, treeTag) argument
// order would print the CU id there instead and fail the match.

TEST(UnitAttributeDeathTest, CrossUnitNodeIdAbortsOnSet) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto cu = makeToyUnit({"var a = x;"});
    UnitAttribute<int> attr{cu};
    EXPECT_DEATH({ attr.set(kForeignNode, 1); }, "got NodeId from TreeId=4242");
}

TEST(UnitAttributeDeathTest, CrossUnitNodeIdAbortsOnGet) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto cu = makeToyUnit({"var a = x;"});
    UnitAttribute<int> attr{cu};
    EXPECT_DEATH({ (void)attr.get(kForeignNode); }, "got NodeId from TreeId=4242");
}

TEST(UnitAttributeDeathTest, CrossUnitNodeIdAbortsOnHas) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto cu = makeToyUnit({"var a = x;"});
    UnitAttribute<int> attr{cu};
    EXPECT_DEATH({ (void)attr.has(kForeignNode); }, "got NodeId from TreeId=4242");
}

TEST(UnitAttributeDeathTest, CrossUnitNodeIdAbortsOnTryGet) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto cu = makeToyUnit({"var a = x;"});
    UnitAttribute<int> attr{cu};
    EXPECT_DEATH({ (void)attr.tryGet(kForeignNode); }, "got NodeId from TreeId=4242");
}

TEST(UnitAttributeDeathTest, CrossUnitNodeIdAbortsOnErase) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto cu = makeToyUnit({"var a = x;"});
    UnitAttribute<int> attr{cu};
    EXPECT_DEATH({ (void)attr.erase(kForeignNode); }, "got NodeId from TreeId=4242");
}

TEST(UnitAttributeDeathTest, UntaggedLiteralInMultiTreeUnitAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto cu = makeToyUnit({"var a = x;", "var b = y;"});
    UnitAttribute<int> attr{cu};
    EXPECT_DEATH({ attr.set(NodeId{1}, 1); }, "untagged NodeId.*ambiguous");
}

TEST(UnitAttributeDeathTest, InvalidNodeDelegatesToPerTreeGuard) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // An untagged InvalidNode routes to the sole tree (single-tree CU), where
    // the per-tree NodeAttribute's sentinel guard fires.
    auto cu = makeToyUnit({"var a = x;"});
    UnitAttribute<int> attr{cu};
    EXPECT_DEATH({ attr.set(InvalidNode, 1); }, "invalid NodeId");
}

// ── death tests: empty (zero-tree) CompilationUnit ──────────────────────────

TEST(UnitAttributeDeathTest, EmptyUnitTaggedIdAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto cu = makeToyUnit({});      // zero trees — valid (CU1), but unroutable
    UnitAttribute<int> attr{cu};
    EXPECT_DEATH({ attr.set(kForeignNode, 1); }, "got NodeId from TreeId=4242");
}

TEST(UnitAttributeDeathTest, EmptyUnitUntaggedIdAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto cu = makeToyUnit({});
    UnitAttribute<int> attr{cu};
    EXPECT_DEATH({ attr.set(NodeId{1}, 1); }, "untagged NodeId.*ambiguous");
}

// ── death test: duplicate-TreeId construction guard ─────────────────────────

TEST(UnitAttributeDeathTest, DuplicateTreeIdInUnitAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // Two trees sharing a TreeId can't arise from the real pipeline (TreeIds
    // are globally unique), so fabricate them via RawTreeBuilder to prove the
    // construction-time guard fires rather than silently overwriting a route.
    auto makeTaggedTree = [] {
        dss::tests::RawTreeBuilder builder{"x", "<dup>", TreeId{777}};
        NodeId const root = builder.addNode(NodeKind::Internal, builder.internRule("root"),
                                            SourceSpan::of(0, 1), NodeFlags::None, InvalidNode, {});
        return std::move(builder).finish(root);
    };

    UnitBuilder unitBuilder{cu_test::loadToySchema()};
    unitBuilder.addTree(makeTaggedTree());
    unitBuilder.addTree(makeTaggedTree());   // both carry TreeId{777}
    auto cu = std::move(unitBuilder).finish();

    EXPECT_DEATH({ UnitAttribute<int> attr{cu}; }, "duplicate TreeId");
}
