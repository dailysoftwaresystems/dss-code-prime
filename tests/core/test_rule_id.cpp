#include "core/types/rule_id.hpp"

#include <gtest/gtest.h>

using namespace dss;

TEST(RuleInterner, ReservesSlotZero) {
    RuleInterner i;
    EXPECT_EQ(i.size(), 1u);          // sentinel slot
    EXPECT_EQ(i.name(InvalidRule), "");
}

TEST(RuleInterner, InternReturnsStableId) {
    RuleInterner i;
    auto a1 = i.intern("functionDecl");
    auto a2 = i.intern("functionDecl");
    EXPECT_EQ(a1, a2);
    EXPECT_TRUE(a1.valid());
}

TEST(RuleInterner, DistinctNamesDistinctIds) {
    RuleInterner i;
    auto a = i.intern("foo");
    auto b = i.intern("bar");
    auto c = i.intern("baz");
    EXPECT_NE(a, b);
    EXPECT_NE(b, c);
    EXPECT_NE(a, c);
}

TEST(RuleInterner, NameRoundTrips) {
    RuleInterner i;
    auto a = i.intern("ifStmt");
    auto b = i.intern("whileStmt");
    EXPECT_EQ(i.name(a), "ifStmt");
    EXPECT_EQ(i.name(b), "whileStmt");
}

TEST(RuleInterner, ContainsTrue) {
    RuleInterner i;
    (void)i.intern("x");
    EXPECT_TRUE(i.contains("x"));
    EXPECT_FALSE(i.contains("y"));
}

TEST(RuleInterner, FreezeRejectsNewEntries) {
    RuleInterner i;
    auto existing = i.intern("foo");
    i.freeze();
    EXPECT_TRUE(i.isFrozen());

    // Existing names still resolve.
    EXPECT_EQ(i.intern("foo"), existing);

    // New name post-freeze: in debug builds this asserts, in release it
    // returns InvalidRule. Test the release-mode contract.
#ifdef NDEBUG
    EXPECT_FALSE(i.intern("new-name-after-freeze").valid());
#endif
}

TEST(RuleInterner, FreezeIsIdempotent) {
    RuleInterner i;
    i.freeze();
    i.freeze();
    EXPECT_TRUE(i.isFrozen());
}

TEST(RuleInterner, IteratesOverNames) {
    RuleInterner i;
    (void)i.intern("a");
    (void)i.intern("b");
    (void)i.intern("c");
    auto names = std::vector<std::string>{i.begin(), i.end()};
    EXPECT_EQ(names.size(), 4u);    // including sentinel
    EXPECT_EQ(names[0], "");        // sentinel
    EXPECT_EQ(names[1], "a");
    EXPECT_EQ(names[2], "b");
    EXPECT_EQ(names[3], "c");
}
