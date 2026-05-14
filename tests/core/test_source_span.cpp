#include "core/types/source_span.hpp"

#include <gtest/gtest.h>

using namespace dss;

TEST(SourceSpan, FactoryEnforcesOrdering) {
    auto s = SourceSpan::of(5, 10);
    EXPECT_EQ(s.start(), 5u);
    EXPECT_EQ(s.end(), 10u);
    EXPECT_EQ(s.length(), 5u);
    EXPECT_FALSE(s.isEmpty());
}

TEST(SourceSpan, EmptyFactory) {
    auto s = SourceSpan::empty(7);
    EXPECT_EQ(s.start(), 7u);
    EXPECT_EQ(s.end(), 7u);
    EXPECT_EQ(s.length(), 0u);
    EXPECT_TRUE(s.isEmpty());
}

TEST(SourceSpan, Contains) {
    auto s = SourceSpan::of(5, 10);
    EXPECT_TRUE(s.contains(5));
    EXPECT_TRUE(s.contains(9));
    EXPECT_FALSE(s.contains(10));   // half-open
    EXPECT_FALSE(s.contains(4));
}

TEST(SourceSpan, ContainsSpan) {
    auto outer = SourceSpan::of(5, 20);
    EXPECT_TRUE(outer.containsSpan(SourceSpan::of(7, 15)));
    EXPECT_TRUE(outer.containsSpan(SourceSpan::of(5, 20)));   // identical
    EXPECT_FALSE(outer.containsSpan(SourceSpan::of(0, 7)));
    EXPECT_FALSE(outer.containsSpan(SourceSpan::of(15, 25)));

    // Empty spans at boundary points are contained.
    EXPECT_TRUE(outer.containsSpan(SourceSpan::empty(10)));
    EXPECT_TRUE(outer.containsSpan(SourceSpan::empty(5)));
    EXPECT_TRUE(outer.containsSpan(SourceSpan::empty(20)));
    EXPECT_FALSE(outer.containsSpan(SourceSpan::empty(21)));
}

TEST(SourceSpan, Overlaps) {
    auto a = SourceSpan::of(5, 10);
    EXPECT_TRUE(a.overlaps(SourceSpan::of(7, 15)));
    EXPECT_TRUE(a.overlaps(SourceSpan::of(0, 7)));
    EXPECT_FALSE(a.overlaps(SourceSpan::of(10, 15)));     // touching boundary, half-open
    EXPECT_FALSE(a.overlaps(SourceSpan::of(0, 5)));       // touching boundary
    EXPECT_FALSE(a.overlaps(SourceSpan::empty(7)));       // empty never overlaps
}

TEST(SourceSpan, JoinIgnoresEmptyOperands) {
    auto a = SourceSpan::of(5, 10);
    auto b = SourceSpan::of(20, 25);
    auto e = SourceSpan::empty(50);

    EXPECT_EQ(SourceSpan::join(a, b), SourceSpan::of(5, 25));
    EXPECT_EQ(SourceSpan::join(b, a), SourceSpan::of(5, 25));
    // The crucial case: empty operands are *ignored*, not "merged" — synthetic
    // Missing-node parents rely on this so their span isn't dragged down to 0.
    EXPECT_EQ(SourceSpan::join(a, e), a);
    EXPECT_EQ(SourceSpan::join(e, a), a);
    EXPECT_TRUE(SourceSpan::join(e, e).isEmpty());
}

TEST(SourceSpan, Intersect) {
    auto a = SourceSpan::of(5, 15);
    auto b = SourceSpan::of(10, 20);
    EXPECT_EQ(SourceSpan::intersect(a, b), SourceSpan::of(10, 15));

    auto c = SourceSpan::of(25, 30);
    EXPECT_TRUE(SourceSpan::intersect(a, c).isEmpty());
}

TEST(SourceSpan, ComparisonOrderBy) {
    auto a = SourceSpan::of(5, 10);
    auto b = SourceSpan::of(5, 11);
    auto c = SourceSpan::of(6, 7);
    EXPECT_LT(a, b);   // same start, shorter
    EXPECT_LT(a, c);   // earlier start
    EXPECT_EQ(a, SourceSpan::of(5, 10));
}

TEST(SourceSpan, Size) {
    static_assert(sizeof(SourceSpan) == 8);
}
