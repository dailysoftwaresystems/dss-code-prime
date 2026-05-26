// HR1: the extension-kind registry — the open-core counterpart to TypeRegistry.

#include "hir/hir_kind_registry.hpp"
#include "hir/hir_node.hpp"

#include <gtest/gtest.h>

using dss::HirKindRegistry;
using dss::kFirstHirExtensionKind;

TEST(HirKindRegistry, MintsMonotonicIdsFromTwoFiftySix) {
    HirKindRegistry reg;
    auto a = reg.registerExtension("SQL::Query", "SQL");
    auto b = reg.registerExtension("SQL::DmlInsert", "SQL");
    EXPECT_EQ(a.v, kFirstHirExtensionKind);        // 256
    EXPECT_EQ(b.v, kFirstHirExtensionKind + 1);    // 257
    EXPECT_EQ(reg.extensions().size(), 2u);
}

TEST(HirKindRegistry, FindHitAndMiss) {
    HirKindRegistry reg;
    auto q = reg.registerExtension("SQL::Query", "SQL");
    auto found = reg.findExtension("SQL::Query");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->v, q.v);
    EXPECT_FALSE(reg.findExtension("SQL::Nope").has_value());
}

TEST(HirKindRegistry, FindResolvesEveryRegistrationNotJustTheFirst) {
    // Guards against a regression where the byName_ map only retained the
    // first inserted key — the FindHitAndMiss test above only registers one
    // name, so it would pass even with that bug.
    HirKindRegistry reg;
    auto q  = reg.registerExtension("SQL::Query",      "SQL");
    auto d  = reg.registerExtension("SQL::DmlInsert",  "SQL");
    auto b  = reg.registerExtension("Shader::Barrier", "Shader");
    EXPECT_EQ(reg.findExtension("SQL::Query")->v,      q.v);
    EXPECT_EQ(reg.findExtension("SQL::DmlInsert")->v,  d.v);
    EXPECT_EQ(reg.findExtension("Shader::Barrier")->v, b.v);
    // Three distinct, monotonic ids — no aliasing.
    EXPECT_EQ(q.v, kFirstHirExtensionKind);
    EXPECT_EQ(d.v, kFirstHirExtensionKind + 1);
    EXPECT_EQ(b.v, kFirstHirExtensionKind + 2);
}

TEST(HirKindRegistry, IdenticalReRegistrationIsIdempotent) {
    HirKindRegistry reg;
    auto first  = reg.registerExtension("Shader::Barrier", "Shader");
    auto second = reg.registerExtension("Shader::Barrier", "Shader");
    EXPECT_EQ(first.v, second.v);
    EXPECT_EQ(reg.extensions().size(), 1u);   // no duplicate descriptor
}

TEST(HirKindRegistry, DescriptorRoundTrips) {
    HirKindRegistry reg;
    auto id = reg.registerExtension("SQL::Cte", "SQL");
    auto const& d = reg.descriptor(id);
    EXPECT_EQ(d.name(), "SQL::Cte");
    EXPECT_EQ(d.kindId().v, id.v);
    EXPECT_EQ(d.sourceLanguage(), "SQL");
}

TEST(HirKindRegistryDeathTest, CrossDomainNameCollisionAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    HirKindRegistry reg;
    reg.registerExtension("X::Thing", "LangA");
    EXPECT_DEATH({ reg.registerExtension("X::Thing", "LangB"); },
                 "re-registered under language 'LangB' but was first registered under 'LangA'");
}

TEST(HirKindRegistryDeathTest, DescriptorForUnmintedIdAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    HirKindRegistry reg;
    reg.registerExtension("SQL::Query", "SQL");   // mints 256 only
    // A core-range id and a never-minted extension id both abort.
    EXPECT_DEATH({ (void)reg.descriptor(dss::HirKindId{10}); }, "registry never minted");
    EXPECT_DEATH({ (void)reg.descriptor(dss::HirKindId{999}); }, "registry never minted");
}
