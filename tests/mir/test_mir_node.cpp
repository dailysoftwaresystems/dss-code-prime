// ML1 — MIR storage PODs + strong ids + cross-arena guard wording.

#include "core/substrate/arena_container.hpp"
#include "core/types/strong_ids.hpp"
#include "mir/mir_node.hpp"

#include <gtest/gtest.h>

#include <type_traits>

using namespace dss;

// ── POD layout budgets (the scan-hot density discipline) ──
static_assert(sizeof(detail::MirInst) <= 32);
static_assert(sizeof(detail::MirBlock) <= 32);
static_assert(sizeof(detail::MirFunc) <= 32);
static_assert(std::is_trivially_copyable_v<detail::MirInst>);
static_assert(std::is_trivially_copyable_v<detail::MirBlock>);
static_assert(std::is_trivially_copyable_v<detail::MirFunc>);

// ── id distinctness + the fused-value alias ──
static_assert(std::is_same_v<MirValueId, MirInstId>,
              "fused model: a value IS its defining instruction");
static_assert(!std::is_same_v<MirInstId, MirBlockId>);
static_assert(!std::is_same_v<MirInstId, MirFuncId>);
static_assert(!std::is_same_v<MirBlockId, MirFuncId>);

TEST(MirNode, DefaultInstIsTheVisiblyBogusSentinel) {
    detail::MirInst n;
    EXPECT_EQ(n.opcode, MirOpcode::Invalid);
    EXPECT_FALSE(n.typeId.valid());
    EXPECT_EQ(n.operandCount, 0u);
}

TEST(MirNode, DefaultBlockMarkerIsLinear) {
    detail::MirBlock b;
    EXPECT_EQ(b.marker, StructCfMarker::Linear);
    EXPECT_EQ(b.instCount, 0u);
    EXPECT_EQ(b.succCount, 0u);
}

TEST(MirNode, InvalidSentinels) {
    EXPECT_FALSE(InvalidMirModule.valid());
    EXPECT_FALSE(InvalidMirInst.valid());
    EXPECT_FALSE(InvalidMirBlock.valid());
    EXPECT_FALSE(InvalidMirFunc.valid());
    EXPECT_EQ(InvalidMirInst.v, 0u);
}

TEST(MirNode, ArenaIdEqualityIsValueOnly) {
    // arenaTag is provenance, not identity — same slot from "different modules"
    // compares equal (the validators, not equality, are the enforcement point).
    EXPECT_EQ(MirInstId(3, 1), MirInstId(3, 2));
    EXPECT_NE(MirInstId(3, 1), MirInstId(4, 1));
}

TEST(MirNode, PhiIncomingIsTriviallyComparable) {
    MirPhiIncoming a{MirInstId(1), MirBlockId(2)};
    MirPhiIncoming b{MirInstId(1), MirBlockId(2)};
    MirPhiIncoming c{MirInstId(1), MirBlockId(3)};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

// ── the cross-arena guard fires with MIR-specific wording ──
// A MirInstId minted by module A used against module B's arena aborts, naming
// the element ("MirInstId") and both module tags (the ArenaNames specialization).
TEST(MirNodeDeathTest, CrossModuleInstAccessAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    substrate::ArenaBuilder<detail::MirInst, MirInstId, MirModuleId> a{MirModuleId{1}};
    substrate::ArenaBuilder<detail::MirInst, MirInstId, MirModuleId> b{MirModuleId{2}};
    MirInstId const fromA = a.addNode(detail::MirInst{});  // tagged with module 1
    auto frozenB = std::move(b).finish();
    EXPECT_DEATH({ (void)frozenB.at(fromA); }, "MirInstId");
}

TEST(MirNodeDeathTest, CrossModuleBlockAccessAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    substrate::ArenaBuilder<detail::MirBlock, MirBlockId, MirModuleId> a{MirModuleId{1}};
    substrate::ArenaBuilder<detail::MirBlock, MirBlockId, MirModuleId> b{MirModuleId{2}};
    MirBlockId const fromA = a.addNode(detail::MirBlock{});
    auto frozenB = std::move(b).finish();
    EXPECT_DEATH({ (void)frozenB.at(fromA); }, "MirBlockId");
}
