// Pins for `utf8ByteOffsetToUtf16Column` — the LSP server's only
// Unicode-arithmetic touch point. Tests cover the four UTF-8
// sequence lengths plus malformed-input fallback.

#include "lsp/utf16_column.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>

using dss::lsp::utf8ByteOffsetToUtf16Column;

// Each ASCII byte is one UTF-8 byte and one UTF-16 code unit, so
// the UTF-8 byte column equals the UTF-16 column.
TEST(Utf16Column, AsciiByteOffsetEqualsUtf16Column) {
    constexpr std::string_view line = "hello world";
    EXPECT_EQ(utf8ByteOffsetToUtf16Column(line, 0), 0u);
    EXPECT_EQ(utf8ByteOffsetToUtf16Column(line, 5), 5u);
    EXPECT_EQ(utf8ByteOffsetToUtf16Column(line, 11), 11u);
}

// U+00E9 (`é`) is 2 UTF-8 bytes, 1 UTF-16 unit. After consuming
// those 2 bytes, the UTF-16 column is 1.
TEST(Utf16Column, TwoByteUtf8YieldsOneUtf16Unit) {
    constexpr std::string_view line = "\xC3\xA9 cafe"; // "é cafe" (1 + 6 = 7 chars; 8 bytes)
    EXPECT_EQ(utf8ByteOffsetToUtf16Column(line, 0), 0u);
    EXPECT_EQ(utf8ByteOffsetToUtf16Column(line, 2), 1u); // past `é`
    EXPECT_EQ(utf8ByteOffsetToUtf16Column(line, 3), 2u); // past `é `
}

// U+4E2D (`中`) is 3 UTF-8 bytes, 1 UTF-16 unit.
TEST(Utf16Column, ThreeByteUtf8YieldsOneUtf16Unit) {
    constexpr std::string_view line = "\xE4\xB8\xADx"; // "中x"
    EXPECT_EQ(utf8ByteOffsetToUtf16Column(line, 0), 0u);
    EXPECT_EQ(utf8ByteOffsetToUtf16Column(line, 3), 1u); // past `中`
    EXPECT_EQ(utf8ByteOffsetToUtf16Column(line, 4), 2u); // past `中x`
}

// U+1F600 (😀) is 4 UTF-8 bytes, encodes as a UTF-16 surrogate
// pair = 2 code units. The critical edge case for LSP.
TEST(Utf16Column, FourByteUtf8YieldsTwoUtf16Units) {
    constexpr std::string_view line = "\xF0\x9F\x98\x80!"; // "😀!"
    EXPECT_EQ(utf8ByteOffsetToUtf16Column(line, 0), 0u);
    EXPECT_EQ(utf8ByteOffsetToUtf16Column(line, 4), 2u); // past 😀 → surrogate pair
    EXPECT_EQ(utf8ByteOffsetToUtf16Column(line, 5), 3u); // past 😀!
}

// Mixed line — exercises that the accumulator is correctly
// per-codepoint, not per-byte.
TEST(Utf16Column, MixedLineAccumulatesCorrectly) {
    // "a" + "é" + "中" + "😀" + "b"
    // UTF-8 bytes:  1   2     3     4     1   = 11 bytes
    // UTF-16 units: 1 + 1   + 1   + 2   + 1   = 6 units
    constexpr std::string_view line = "a\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80""b";
    EXPECT_EQ(utf8ByteOffsetToUtf16Column(line, 11), 6u);
}

// Past-end byte offset clamps to the line length. Callers should
// never pass past-end, but `SourceSpan::end()` is one-past so
// clamping is the documented graceful behavior.
TEST(Utf16Column, PastEndOffsetIsClampedToLineEnd) {
    constexpr std::string_view line = "abc";
    EXPECT_EQ(utf8ByteOffsetToUtf16Column(line, 100), 3u);
}

// Empty line is a zero-width range.
TEST(Utf16Column, EmptyLineReturnsZero) {
    EXPECT_EQ(utf8ByteOffsetToUtf16Column("", 0), 0u);
    EXPECT_EQ(utf8ByteOffsetToUtf16Column("", 5), 0u);
}

// Continuation byte at the start (malformed) — documented best-
// effort: advance one byte, count one BMP unit, keep walking.
TEST(Utf16Column, ContinuationByteAtStartRecoversGracefully) {
    constexpr std::string_view line = "\x80""abc";
    EXPECT_EQ(utf8ByteOffsetToUtf16Column(line, 4), 4u);
}
