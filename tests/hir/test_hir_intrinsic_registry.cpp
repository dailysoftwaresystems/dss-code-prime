// HR6: the intrinsic registry — the resolution table IntrinsicCall payloads key
// off. Unlike the kind/op registries it has no [0,256) core range: ids run from 1.

#include "hir/hir_intrinsic_registry.hpp"

#include <gtest/gtest.h>

using dss::HirIntrinsicId;
using dss::HirIntrinsicRegistry;
using dss::InvalidHirIntrinsic;

TEST(HirIntrinsicRegistry, MintsMonotonicIdsFromOne) {
    HirIntrinsicRegistry reg;
    auto a = reg.registerIntrinsic("toy::sqrt", "toy");
    auto b = reg.registerIntrinsic("toy::memcpy", "toy");
    EXPECT_EQ(a.v, 1u);
    EXPECT_EQ(b.v, 2u);
    EXPECT_EQ(reg.intrinsics().size(), 2u);
}

TEST(HirIntrinsicRegistry, ContainsTracksMembership) {
    HirIntrinsicRegistry reg;
    auto a = reg.registerIntrinsic("toy::sqrt", "toy");
    EXPECT_TRUE(reg.contains(a));
    EXPECT_FALSE(reg.contains(InvalidHirIntrinsic));   // id 0 is never a member
    EXPECT_FALSE(reg.contains(HirIntrinsicId{2}));      // never minted
    EXPECT_FALSE(reg.contains(HirIntrinsicId{999}));
}

TEST(HirIntrinsicRegistry, FindHitAndMiss) {
    HirIntrinsicRegistry reg;
    auto s = reg.registerIntrinsic("toy::sqrt", "toy");
    auto found = reg.findIntrinsic("toy::sqrt");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->v, s.v);
    EXPECT_FALSE(reg.findIntrinsic("toy::nope").has_value());
}

TEST(HirIntrinsicRegistry, FindResolvesEveryRegistrationNotJustTheFirst) {
    HirIntrinsicRegistry reg;
    auto a = reg.registerIntrinsic("toy::sqrt",   "toy");
    auto b = reg.registerIntrinsic("toy::memcpy", "toy");
    auto c = reg.registerIntrinsic("sql::coalesce", "sql");
    EXPECT_EQ(reg.findIntrinsic("toy::sqrt")->v,     a.v);
    EXPECT_EQ(reg.findIntrinsic("toy::memcpy")->v,   b.v);
    EXPECT_EQ(reg.findIntrinsic("sql::coalesce")->v, c.v);
    EXPECT_EQ(a.v, 1u);
    EXPECT_EQ(b.v, 2u);
    EXPECT_EQ(c.v, 3u);
}

TEST(HirIntrinsicRegistry, IdenticalReRegistrationIsIdempotent) {
    HirIntrinsicRegistry reg;
    auto first  = reg.registerIntrinsic("toy::sqrt", "toy");
    auto second = reg.registerIntrinsic("toy::sqrt", "toy");
    EXPECT_EQ(first.v, second.v);
    EXPECT_EQ(reg.intrinsics().size(), 1u);   // no duplicate descriptor
}

TEST(HirIntrinsicRegistry, DescriptorRoundTrips) {
    HirIntrinsicRegistry reg;
    auto id = reg.registerIntrinsic("toy::sqrt", "toy");
    auto const& d = reg.descriptor(id);
    EXPECT_EQ(d.name(), "toy::sqrt");
    EXPECT_EQ(d.id().v, id.v);
    EXPECT_EQ(d.sourceLanguage(), "toy");
}

TEST(HirIntrinsicRegistryDeathTest, CrossDomainNameCollisionAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    HirIntrinsicRegistry reg;
    reg.registerIntrinsic("shared", "LangA");
    EXPECT_DEATH({ reg.registerIntrinsic("shared", "LangB"); },
                 "re-registered under language 'LangB' but was first registered under 'LangA'");
}

TEST(HirIntrinsicRegistryDeathTest, DescriptorForUnmintedIdAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    HirIntrinsicRegistry reg;
    reg.registerIntrinsic("toy::sqrt", "toy");   // mints 1 only
    EXPECT_DEATH({ (void)reg.descriptor(InvalidHirIntrinsic); }, "never minted");
    EXPECT_DEATH({ (void)reg.descriptor(HirIntrinsicId{999}); }, "never minted");
}
