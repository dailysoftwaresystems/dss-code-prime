// HR1: the frozen Hir module + HirBuilder — bottom-up build, child-pool layout,
// parent back-patching, cross-module guard, and HirAttribute side-tables.

#include "hir/hir.hpp"
#include "hir/hir_node.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using dss::Hir;
using dss::HirAttribute;
using dss::HirBuilder;
using dss::HirFlags;
using dss::HirKind;
using dss::HirModuleId;
using dss::HirNodeId;

namespace {

// A canonical little module:  Module → Block → [VarDecl(leaf), ReturnStmt → Literal]
struct Sample {
    Hir       hir;
    HirNodeId mod, blk, vd, ret, lit;
};

Sample buildSample() {
    HirBuilder b{"toy"};
    HirNodeId const lit = b.addLeaf(HirKind::Literal, dss::InvalidType, /*payload=*/42);
    HirNodeId const ret = b.addParent(HirKind::ReturnStmt, std::array{lit});
    HirNodeId const vd  = b.addLeaf(HirKind::VarDecl);
    HirNodeId const blk = b.addParent(HirKind::Block, std::array{vd, ret});
    HirNodeId const mod = b.addParent(HirKind::Module, std::array{blk});
    Hir hir = std::move(b).finish(mod);
    return Sample{std::move(hir), mod, blk, vd, ret, lit};
}

} // namespace

// ── move-only (compile-time) ──
static_assert(!std::is_copy_constructible_v<Hir>);
static_assert(std::is_move_constructible_v<Hir>);
static_assert(!std::is_copy_constructible_v<HirBuilder>);
static_assert(std::is_move_constructible_v<HirBuilder>);

TEST(HirBuilder, EmitsTaggedIdsFromSlotOne) {
    HirBuilder b{"toy"};
    EXPECT_EQ(b.size(), 1u);                         // slot 0 sentinel
    HirNodeId const a = b.addLeaf(HirKind::Literal);
    EXPECT_EQ(a.v, 1u);
    EXPECT_EQ(a.arenaTag, b.id().v);                 // stamped with the module id
    EXPECT_EQ(b.size(), 2u);
}

TEST(HirBuilder, ModuleIdsAreMonotonicAndValid) {
    HirBuilder b1{"toy"};
    HirBuilder b2{"toy"};
    EXPECT_TRUE(b1.id().valid());
    EXPECT_TRUE(b2.id().valid());
    // Consecutive construction in the same test pins the strictly-monotonic
    // contract: each builder takes the very next counter value.
    EXPECT_EQ(b2.id().v, b1.id().v + 1);
}

TEST(Hir, DefaultConstructedIsEmptyAndRootless) {
    Hir h;
    EXPECT_TRUE(h.empty());
    EXPECT_EQ(h.nodeCount(), 0u);
    EXPECT_FALSE(h.root().valid());
    EXPECT_FALSE(h.id().valid());
    EXPECT_EQ(h.sourceLanguage(), "");
    EXPECT_TRUE(h.registry().extensions().empty());
}

TEST(Hir, MovedFromIsObservablyEmpty) {
    Sample s = buildSample();
    Hir moved = std::move(s.hir);
    EXPECT_EQ(moved.nodeCount(), 6u);
    EXPECT_EQ(moved.root(), s.mod);
    // The source's observable state is empty — not "valid but unspecified".
    EXPECT_TRUE(s.hir.empty());
    EXPECT_EQ(s.hir.nodeCount(), 0u);
    EXPECT_FALSE(s.hir.id().valid());
}

TEST(HirBuilder, IsMovableWithStatePreserved) {
    HirBuilder src{"x"};
    (void)src.addLeaf(HirKind::Literal);
    HirModuleId const srcId = src.id();
    EXPECT_EQ(src.size(), 2u);
    HirBuilder dst = std::move(src);
    EXPECT_EQ(dst.size(), 2u);
    EXPECT_EQ(dst.id(), srcId);
    // `HirBuilder` is single-use (`finish() &&` consumes it), so a moved-from
    // builder's state is intentionally unspecified — only safe to destroy.
}

TEST(Hir, FrozenModuleExposesRootAndMetadata) {
    Sample s = buildSample();
    EXPECT_EQ(s.hir.root(), s.mod);
    EXPECT_EQ(s.hir.sourceLanguage(), "toy");
    EXPECT_EQ(s.hir.nodeCount(), 6u);                // sentinel + 5 nodes
    EXPECT_FALSE(s.hir.empty());
    EXPECT_TRUE(s.hir.id().valid());
}

TEST(Hir, NodeAccessorsReturnBuiltValues) {
    Sample s = buildSample();
    EXPECT_EQ(s.hir.kind(s.mod), HirKind::Module);
    EXPECT_EQ(s.hir.kind(s.blk), HirKind::Block);
    EXPECT_EQ(s.hir.kind(s.vd),  HirKind::VarDecl);
    EXPECT_EQ(s.hir.kind(s.ret), HirKind::ReturnStmt);
    EXPECT_EQ(s.hir.kind(s.lit), HirKind::Literal);
    EXPECT_EQ(s.hir.payload(s.lit), 42u);
    EXPECT_EQ(s.hir.flags(s.lit), HirFlags::None);
    EXPECT_FALSE(s.hir.typeId(s.lit).valid());
}

TEST(Hir, ChildrenSpanIsExactAndOrdered) {
    Sample s = buildSample();
    // Module has one child (the block).
    {
        auto kids = s.hir.children(s.mod);
        std::vector<HirNodeId> got(kids.begin(), kids.end());
        EXPECT_EQ(got, (std::vector<HirNodeId>{s.blk}));
    }
    // Block has exactly [vd, ret] in build order.
    {
        auto kids = s.hir.children(s.blk);
        std::vector<HirNodeId> got(kids.begin(), kids.end());
        EXPECT_EQ(got, (std::vector<HirNodeId>{s.vd, s.ret}));
    }
    // ReturnStmt wraps the literal; VarDecl + Literal are leaves.
    {
        auto kids = s.hir.children(s.ret);
        std::vector<HirNodeId> got(kids.begin(), kids.end());
        EXPECT_EQ(got, (std::vector<HirNodeId>{s.lit}));
    }
    EXPECT_TRUE(s.hir.children(s.vd).empty());
    EXPECT_TRUE(s.hir.children(s.lit).empty());
}

TEST(Hir, ParentLinksAreBackPatched) {
    Sample s = buildSample();
    EXPECT_FALSE(s.hir.parent(s.mod).valid());       // root has no parent
    EXPECT_EQ(s.hir.parent(s.blk), s.mod);
    EXPECT_EQ(s.hir.parent(s.vd),  s.blk);
    EXPECT_EQ(s.hir.parent(s.ret), s.blk);
    EXPECT_EQ(s.hir.parent(s.lit), s.ret);
}

TEST(HirAttributeTest, BindsAndStoresPerNode) {
    Sample s = buildSample();
    HirAttribute<int> notes{s.hir};
    EXPECT_TRUE(notes.empty());
    notes.set(s.lit, 7);
    notes.set(s.vd, 9);
    EXPECT_EQ(notes.size(), 2u);
    EXPECT_TRUE(notes.has(s.lit));
    EXPECT_EQ(notes.get(s.lit), 7);
    EXPECT_EQ(*notes.tryGet(s.vd), 9);
    EXPECT_EQ(notes.tryGet(s.mod), nullptr);         // unset → null
}

TEST(HirDeathTest, CrossModuleNodeIdAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // Two modules with pinned, distinct ids; a node from A queried against B
    // must abort with both module tags (the SH3 / SP1 cross-arena guard — the
    // HR1 acceptance criterion).
    auto build = [](std::uint32_t tag) {
        HirBuilder b{HirModuleId{tag}, "x"};
        HirNodeId const n = b.addLeaf(HirKind::Literal);
        return std::pair<Hir, HirNodeId>{std::move(b).finish(n), n};
    };
    auto [hirA, nA] = build(7);
    auto [hirB, nB] = build(8);
    (void)nB;
    EXPECT_DEATH({ (void)hirB.kind(nA); },
                 "HirNodeId from HirModuleId=7 used on HirModuleId=8");
}

TEST(HirDeathTest, DoubleAttachAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    HirBuilder b{"x"};
    HirNodeId const c = b.addLeaf(HirKind::Literal);
    (void)b.addParent(HirKind::Block, std::array{c});
    EXPECT_DEATH({ (void)b.addParent(HirKind::Block, std::array{c}); },
                 "already has a parent");
}

TEST(HirDeathTest, FinishWithOutOfRangeRootAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    HirBuilder b{"x"};
    (void)b.addLeaf(HirKind::Literal);
    EXPECT_DEATH({ (void)std::move(b).finish(HirNodeId{999}); },
                 "HirNodeId out of range");
}

TEST(HirDeathTest, FinishWithNonRootParentedNodeAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    HirBuilder b{"x"};
    HirNodeId const child = b.addLeaf(HirKind::Literal);
    HirNodeId const mod   = b.addParent(HirKind::Module, std::array{child});
    (void)mod;
    // `child` is parented under `mod`; finishing on it would freeze a sub-tree
    // and silently elide `mod`. The builder must catch this.
    EXPECT_DEATH({ (void)std::move(b).finish(child); },
                 "is not a root \\(parent HirNodeId=");
}

TEST(HirDeathTest, AccessWithInvalidSentinelAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Sample s = buildSample();
    // The sentinel HirNodeId{} (v==0) is rejected by the substrate's
    // validateElement — proves the Hir::at access string is wired through.
    EXPECT_DEATH({ (void)s.hir.kind(HirNodeId{}); }, "HirNodeId out of range");
}

TEST(HirAttributeDeathTest, SentinelAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Sample s = buildSample();
    HirAttribute<int> notes{s.hir};
    // Pins that the ArenaNames<HirNodeId, HirModuleId> specialization is the
    // one driving the attribute-side fatals (message names "HirAttribute" + the
    // HIR element/tag, not generic placeholders).
    EXPECT_DEATH({ (void)notes.has(HirNodeId{}); },
                 "dss::HirAttribute fatal: invalid HirNodeId");
}

TEST(HirAttributeDeathTest, CrossModuleAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto build = [](std::uint32_t tag) {
        HirBuilder b{HirModuleId{tag}, "x"};
        HirNodeId const n = b.addLeaf(HirKind::Literal);
        return std::pair<Hir, HirNodeId>{std::move(b).finish(n), n};
    };
    auto [hirA, nA] = build(11);
    auto [hirB, nB] = build(12);
    (void)nB;
    HirAttribute<int> notesB{hirB};
    EXPECT_DEATH({ (void)notesB.has(nA); },
                 "dss::HirAttribute fatal: HirAttribute bound to HirModuleId=12 got "
                 "HirNodeId from HirModuleId=11");
}
