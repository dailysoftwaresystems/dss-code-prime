// HR1: the HIR node vocabulary — open-core invariant, flag algebra, POD layout.

#include "hir/hir_node.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

using dss::HirFlags;
using dss::HirKind;
using dss::detail::HirNode;

// ── open-core invariant (compile-time) ──
static_assert(static_cast<std::uint32_t>(HirKind::Count_) < 256,
              "core HirKind must fit in [0, 256)");
static_assert(dss::kFirstHirExtensionKind == 256);
// Error is the POD default kind (a default-constructed node is a recovery
// sentinel, never silently a valid Module/Literal).
static_assert(HirKind::Error != HirKind::Module);

// ── POD layout (compile-time) ──
// Parent links live in a parallel array in `Hir` (not in the POD) so the
// scan-hot kind/type sweeps stay dense in cache; the 32-byte budget reflects
// that.
static_assert(std::is_trivially_copyable_v<HirNode>);
static_assert(sizeof(HirNode) <= 32, "HirNode layout budget is 32 bytes");

TEST(HirNode, DefaultIsErrorRecoverySentinel) {
    HirNode n;
    EXPECT_EQ(n.kind, HirKind::Error);
    EXPECT_EQ(n.flags, HirFlags::None);
    EXPECT_FALSE(n.typeId.valid());
    EXPECT_EQ(n.childStart, 0u);
    EXPECT_EQ(n.childCount, 0u);
    EXPECT_EQ(n.payload, 0u);
}

TEST(HirFlags, BitwiseAlgebra) {
    HirFlags f = HirFlags::HasError | HirFlags::ShaderUsable;
    EXPECT_TRUE(any(f));
    EXPECT_TRUE(has(f, HirFlags::HasError));
    EXPECT_TRUE(has(f, HirFlags::ShaderUsable));
    EXPECT_FALSE(has(f, HirFlags::Synthetic));
    EXPECT_FALSE(has(f, HirFlags::HostUsable));
    EXPECT_TRUE(dss::hasError(f));

    // None is the identity / empty set.
    EXPECT_FALSE(any(HirFlags::None));
    EXPECT_FALSE(dss::hasError(HirFlags::None));

    // |= accumulates; & masks; ~ complements.
    f |= HirFlags::Synthetic;
    EXPECT_TRUE(has(f, HirFlags::Synthetic));
    HirFlags only = f & HirFlags::HasError;
    EXPECT_EQ(only, HirFlags::HasError);
    EXPECT_FALSE(has(~HirFlags::HasError, HirFlags::HasError));
}

TEST(HirKind, ExtensionMarkerIsACoreMember) {
    // The Extension marker itself is a CORE kind (< 256); the concrete extension
    // kind it stands in for is a registry id >= 256, carried elsewhere.
    EXPECT_LT(static_cast<std::uint32_t>(HirKind::Extension), 256u);
}
