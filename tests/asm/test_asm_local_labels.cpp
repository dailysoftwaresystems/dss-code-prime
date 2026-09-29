// ★★★ THE ONE NUMERIC-LOCAL-LABEL RESOLVER (`asm/asm_local_labels.hpp`), pinned
// on its own — both callers (a `.s` and an inline-asm template) ask it the same
// question, so the answer is pinned once, here, and each caller's own pins show
// it is the one asked (P68 round 8,
// D-ASM-LABELS-INSIDE-A-TEMPLATE-AND-NUMERIC-LOCAL-LABELS-REFUSED).
//
// 📄DOCUMENTED, GNU as manual, "Local Symbol Names": `Nb` is the most recent
// previous definition of N, `Nf` the next one. ✔MEASURED 2026-09-23 with gas
// 2.42 and clang 18.1.3 on x86_64 and aarch64: a `.s` looping on `1b`, jumping
// `9f` over an EARLIER unrelated `9:` to the NEXT one, looping back on `9b` to
// the nearest one, and reaching `0:` through `0f` / `0b` beside the binary
// literal `0b10101` runs to 42 under all four.

#include "asm/asm_local_labels.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>

using namespace dss::asm_local_labels;

namespace {

constexpr Suffixes kGas{"b", "f"};

}  // namespace

TEST(AsmLocalLabels, AReferenceIsDigitsThenExactlyOneDeclaredSuffix) {
    auto const b1 = parseReference("1b", kGas);
    ASSERT_TRUE(b1.has_value());
    EXPECT_EQ(b1->number, 1u);
    EXPECT_EQ(b1->direction, Direction::Backward);
    auto const f9 = parseReference("9f", kGas);
    ASSERT_TRUE(f9.has_value());
    EXPECT_EQ(f9->number, 9u);
    EXPECT_EQ(f9->direction, Direction::Forward);
    // `0b` / `0f`: label 0, NOT a binary prefix with no digits.
    ASSERT_TRUE(parseReference("0b", kGas).has_value());
    EXPECT_EQ(parseReference("0b", kGas)->number, 0u);
    ASSERT_TRUE(parseReference("0f", kGas).has_value());
    EXPECT_EQ(parseReference("0f", kGas)->direction, Direction::Forward);
    EXPECT_EQ(parseReference("123b", kGas)->number, 123u);
}

TEST(AsmLocalLabels, ANumberIsNeverReadAsAReference) {
    // `0b101` is the binary literal 5 in the dialect's number grammar; it never
    // reaches here as a reference, and if it did it is not one: its last byte
    // is a digit.
    EXPECT_FALSE(parseReference("0b101", kGas).has_value());
    EXPECT_FALSE(parseReference("0x1f", kGas).has_value()) << "hex, not label 0x1";
    EXPECT_FALSE(parseReference("42", kGas).has_value());
    EXPECT_FALSE(parseReference("b", kGas).has_value()) << "a suffix alone";
    EXPECT_FALSE(parseReference("1bf", kGas).has_value());
    EXPECT_FALSE(parseReference("99999999999999999999b", kGas).has_value())
        << "a number that does not fit is refused, never wrapped";
}

TEST(AsmLocalLabels, NearestBeforeAndNearestAfterByPosition) {
    Table<int> t;
    // The measured probe's shape: `9:` at 10 (A), `9:` at 50 (C), `1:` at 5.
    ASSERT_TRUE(t.define(1, 5, 100));
    ASSERT_TRUE(t.define(9, 10, 200));
    ASSERT_TRUE(t.define(9, 50, 300));
    // `jmp 9f` written at 30 — BETWEEN A and C — goes forward to C, never back.
    EXPECT_EQ(t.resolve({9, Direction::Forward}, 30), std::optional<int>{300});
    // `jl 9b` written at 60 — after C — goes back to the NEAREST one, C, not A.
    EXPECT_EQ(t.resolve({9, Direction::Backward}, 60), std::optional<int>{300});
    // `9b` written at 20 sees only A.
    EXPECT_EQ(t.resolve({9, Direction::Backward}, 20), std::optional<int>{200});
    EXPECT_EQ(t.resolve({1, Direction::Backward}, 7), std::optional<int>{100});
}

TEST(AsmLocalLabels, NothingOnThatSideIsNothing) {
    Table<int> t;
    ASSERT_TRUE(t.define(1, 40, 7));
    EXPECT_FALSE(t.resolve({1, Direction::Backward}, 10).has_value())
        << "a `1b` before any `1:`";
    EXPECT_FALSE(t.resolve({1, Direction::Forward}, 50).has_value())
        << "a `1f` after the last `1:`";
    EXPECT_FALSE(t.resolve({2, Direction::Forward}, 0).has_value())
        << "a number never defined";
}

TEST(AsmLocalLabels, ADefinitionOutOfTextOrderIsRefused) {
    Table<int> t;
    ASSERT_TRUE(t.define(3, 20, 1));
    EXPECT_FALSE(t.define(3, 10, 2)) << "positions must only grow";
    EXPECT_FALSE(t.define(3, 20, 3)) << "two definitions at one position";
    EXPECT_TRUE(t.define(4, 10, 4)) << "each number keeps its own order";
}
