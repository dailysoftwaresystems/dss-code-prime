// SP2: TypeRegistry extension minting + per-CU isolation, and the
// schema-driven two-language isolation acceptance.

#include "core/types/grammar_schema.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_lattice.hpp"
#include "core/types/type_lattice/type_registry.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace dss;

TEST(TypeRegistry, RegisterMintsMonotonicKindsFrom256) {
    TypeRegistry reg{"TSQL"};
    const TypeKindId a = reg.registerExtension("TSQL::Varchar", {});
    const TypeKindId b = reg.registerExtension("TSQL::RowType", {});
    EXPECT_EQ(a.v, kFirstExtensionKind);          // 256 — core kinds occupy [0,256)
    EXPECT_EQ(b.v, kFirstExtensionKind + 1);
}

TEST(TypeRegistry, FindAndIdempotentRegistration) {
    TypeRegistry reg{"TSQL"};
    const TypeKindId a = reg.registerExtension("TSQL::Varchar", {});
    EXPECT_EQ(reg.registerExtension("TSQL::Varchar", {}).v, a.v);  // idempotent by name
    auto const found = reg.findExtension("TSQL::Varchar");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->v, a.v);
    EXPECT_FALSE(reg.findExtension("TSQL::Nope").has_value());
    EXPECT_EQ(reg.extensions().size(), 1u);
}

TEST(TypeRegistry, DescriptorRoundTrip) {
    TypeRegistry reg{"TSQL"};
    std::vector<TypeParam> const params{{"N", TypeParamKind::Integer}};
    const TypeKindId k = reg.registerExtension("TSQL::Varchar", params);
    auto const& d = reg.descriptor(k);
    EXPECT_EQ(d.name, "TSQL::Varchar");
    EXPECT_EQ(d.kindId.v, k.v);
    EXPECT_EQ(d.sourceLanguage, "TSQL");
    ASSERT_EQ(d.parameters.size(), 1u);
    EXPECT_EQ(d.parameters[0].name, "N");
    EXPECT_EQ(d.parameters[0].kind, TypeParamKind::Integer);
}

TEST(TypeRegistry, ExtensionsIsolatedAcrossRegistries) {
    TypeRegistry tsql{"TSQL"};
    TypeRegistry csharp{"CSharp"};
    tsql.registerExtension("TSQL::Varchar", {});
    EXPECT_TRUE(tsql.findExtension("TSQL::Varchar").has_value());
    EXPECT_FALSE(csharp.findExtension("TSQL::Varchar").has_value());
}

TEST(TypeRegistryDeathTest, DescriptorOnCoreKindAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    TypeRegistry reg;
    EXPECT_DEATH({ (void)reg.descriptor(TypeKindId{5}); },
                 "TypeRegistry::descriptor: kindId is a core kind, not an extension");
}

TEST(TypeRegistryDeathTest, DescriptorOnUnknownExtensionAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    TypeRegistry reg;
    EXPECT_DEATH({ (void)reg.descriptor(TypeKindId{999}); },
                 "TypeRegistry::descriptor: unknown extension kindId");
}

TEST(TypeRegistryDeathTest, ConflictingReDeclarationAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    TypeRegistry reg{"TSQL"};
    reg.registerExtension("TSQL::Varchar", {{"N", TypeParamKind::Integer}});
    // Same name, different parameter list → conflict, not silent first-wins.
    EXPECT_DEATH({ (void)reg.registerExtension("TSQL::Varchar", {{"M", TypeParamKind::Type}}); },
                 "re-declared with a different parameter list");
}

// Acceptance (08.5 §5): two languages in two CUs show extension isolation,
// driven from the schemas' typeExtensions[].
TEST(TypeLattice, SchemaDrivenTwoLanguageIsolation) {
    auto sqlSchema = GrammarSchema::loadFromText(R"JSON({
        "dssSchemaVersion": 3,
        "language": { "name": "TsqlSubset", "version": "0.1.0" },
        "typeExtensions": [
            { "name": "TSQL::Varchar", "parameters": [ { "name": "N", "kind": "Integer" } ] }
        ]
    })JSON");
    ASSERT_TRUE(sqlSchema.has_value());
    auto toySchema = GrammarSchema::loadFromText(
        R"JSON({ "dssSchemaVersion": 3, "language": { "name": "Toy", "version": "0.1.0" } })JSON");
    ASSERT_TRUE(toySchema.has_value());

    TypeLattice sqlLattice{CompilationUnitId{1}, std::string((*sqlSchema)->name())};
    registerSchemaTypeExtensions(sqlLattice.registry(), **sqlSchema);
    TypeLattice toyLattice{CompilationUnitId{2}, std::string((*toySchema)->name())};
    registerSchemaTypeExtensions(toyLattice.registry(), **toySchema);

    EXPECT_TRUE(sqlLattice.registry().findExtension("TSQL::Varchar").has_value());
    EXPECT_FALSE(toyLattice.registry().findExtension("TSQL::Varchar").has_value());  // isolation
    EXPECT_EQ(sqlLattice.registry().extensions().size(), 1u);
    EXPECT_EQ(toyLattice.registry().extensions().size(), 0u);

    auto const k = *sqlLattice.registry().findExtension("TSQL::Varchar");
    auto const& d = sqlLattice.registry().descriptor(k);
    EXPECT_EQ(d.sourceLanguage, "TsqlSubset");
    ASSERT_EQ(d.parameters.size(), 1u);
    EXPECT_EQ(d.parameters[0].kind, TypeParamKind::Integer);
}
