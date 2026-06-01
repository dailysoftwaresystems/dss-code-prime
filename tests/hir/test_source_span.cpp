#include "hir/attributes/source_span.hpp"

#include <gtest/gtest.h>

// Direct unit pins for `HirSourceLoc` predicates (D-FF2 type-design Q2
// fold + test-analyzer gap closure). The struct's predicates are
// load-bearing across LSP / FFI / lowering surfaces; these tests pin
// the contract per the header comment so a future regression that
// breaks `is_present == buffer.valid()` or that misroutes the
// caret-vs-text distinction fails loud.

using namespace dss;

TEST(HirSourceLoc, DefaultConstructedIsAbsent) {
    HirSourceLoc const loc{};
    EXPECT_FALSE(loc.isPresent());
    EXPECT_TRUE(loc.isAbsent());
    EXPECT_FALSE(loc.spansText());
}

TEST(HirSourceLoc, AbsentFactoryMatchesDefaultConstruction) {
    auto const loc = HirSourceLoc::absent();
    EXPECT_TRUE(loc.isAbsent());
    EXPECT_FALSE(loc.isPresent());
    EXPECT_FALSE(loc.spansText());
    // Equivalence with the literal default-construction (the documented
    // absent sentinel): both code paths must produce the same value.
    HirSourceLoc const def{};
    EXPECT_EQ(loc.buffer, def.buffer);
    EXPECT_EQ(loc.span.start(), def.span.start());
    EXPECT_EQ(loc.span.end(), def.span.end());
}

TEST(HirSourceLoc, PresentWithCoveringSpanSpansText) {
    BufferId const b{1};
    HirSourceLoc const loc{b, SourceSpan::of(0, 10)};
    EXPECT_TRUE(loc.isPresent());
    EXPECT_FALSE(loc.isAbsent());
    EXPECT_TRUE(loc.spansText());
}

TEST(HirSourceLoc, PresentWithZeroLengthSpanIsCaretNotText) {
    // The contract per source_span.hpp:32-39 explicitly endorses a
    // zero-length span paired with a valid buffer as "caret-pointer at
    // a token boundary" — present, just not covering text. Pins the
    // asymmetry between `isPresent()` (yes — locus is bindable) and
    // `spansText()` (no — no covered text). Consumers that need a
    // renderable underline use `spansText()`; consumers that need
    // "do I have a locus" use `isPresent()`.
    BufferId const b{1};
    HirSourceLoc const loc{b, SourceSpan::empty(5)};
    EXPECT_TRUE(loc.isPresent())
        << "caret pointer (zero-length span at a valid offset) is "
           "still bindable to a buffer";
    EXPECT_FALSE(loc.spansText())
        << "caret pointer covers no text — distinguishes from a "
           "renderable-underline locus";
}

TEST(HirSourceLoc, NonEmptySpanWithInvalidBufferStillAbsent) {
    // `buffer.valid()` is the single discriminator. A span without a
    // known buffer can't index into anything regardless of its size,
    // so the locus is semantically absent.
    HirSourceLoc const loc{InvalidBuffer, SourceSpan::of(0, 10)};
    EXPECT_TRUE(loc.isAbsent());
    EXPECT_FALSE(loc.isPresent());
    EXPECT_FALSE(loc.spansText());
}

// constexpr exercises — the predicates are documented constexpr so
// they can fire in `static_assert` chains / constant-expression
// contexts (closed-table consistency checks, future). Pin both arms.
static_assert(HirSourceLoc::absent().isAbsent());
static_assert(!HirSourceLoc::absent().isPresent());
static_assert(!HirSourceLoc::absent().spansText());
