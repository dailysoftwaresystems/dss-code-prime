// SP2: core type-lattice value/POD invariants.

#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

using namespace dss;

TEST(CoreType, KindEnumOccupiesCoreRange) {
    static_assert(static_cast<std::uint32_t>(TypeKind::Count_) < 256,
                  "core kinds must fit [0,256)");
    static_assert(kFirstExtensionKind == 256);
    static_assert(static_cast<std::uint16_t>(TypeKind::Bool) == 0);   // first member anchor
    EXPECT_LT(static_cast<std::uint32_t>(TypeKind::Extension), kFirstExtensionKind);
}

TEST(CoreType, TypeRecordIsTriviallyCopyablePod) {
    static_assert(std::is_trivially_copyable_v<TypeRecord>);
    static_assert(std::is_standard_layout_v<TypeRecord>);
    TypeRecord r;
    EXPECT_EQ(r.kind, TypeKind::Void);
    EXPECT_FALSE(r.extensionKind.valid());
    EXPECT_FALSE(r.name.valid());
    EXPECT_EQ(r.operandCount, 0u);
    EXPECT_EQ(r.scalarCount, 0u);
}

TEST(CoreType, CallConvAndTypeParam) {
    EXPECT_EQ(static_cast<int>(CallConv::CcSysV), 0);
    TypeParam p{"N", TypeParamKind::Integer};
    EXPECT_EQ(p.name, "N");
    EXPECT_EQ(p.kind, TypeParamKind::Integer);
}
