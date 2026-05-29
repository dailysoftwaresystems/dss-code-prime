// ML1 — MirLiteralPool add/at round-trip + out-of-range guard.

#include "mir/mir_literal_pool.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <variant>

using namespace dss;

TEST(MirLiteralPool, AddReturnsSequentialIndicesAndRoundTrips) {
    MirLiteralPool pool;
    EXPECT_TRUE(pool.empty());

    MirLiteralValue a;
    a.value = std::int64_t{42};
    a.core  = TypeKind::I32;
    MirLiteralValue b;
    b.value = std::string{"hi"};
    b.core  = TypeKind::Char;

    std::uint32_t const ia = pool.add(a);
    std::uint32_t const ib = pool.add(std::move(b));
    EXPECT_EQ(ia, 0u);
    EXPECT_EQ(ib, 1u);
    EXPECT_EQ(pool.size(), 2u);

    ASSERT_TRUE(std::holds_alternative<std::int64_t>(pool.at(ia).value));
    EXPECT_EQ(std::get<std::int64_t>(pool.at(ia).value), 42);
    EXPECT_EQ(pool.at(ia).core, TypeKind::I32);
    ASSERT_TRUE(std::holds_alternative<std::string>(pool.at(ib).value));
    EXPECT_EQ(std::get<std::string>(pool.at(ib).value), "hi");
}

TEST(MirLiteralPoolDeathTest, AtOutOfRangeAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    MirLiteralPool pool;
    (void)pool.add(MirLiteralValue{});
    EXPECT_DEATH({ (void)pool.at(5); }, "out of range");
}
