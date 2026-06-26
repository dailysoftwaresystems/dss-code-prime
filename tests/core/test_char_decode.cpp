// Direct unit tests for the shared char/string literal body decoder
// (core/types/char_decode.hpp). The decoder's escape state machine — especially
// the one-or-two-digit `\xHH` lookahead and the fail-loud rejection of unknown
// / malformed escapes — is otherwise exercised only through end-to-end lowering,
// which hits just the trivial paths. These tests pin the intricate branches.

#include "core/types/char_decode.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>

using namespace dss;

// ── decodeStringLiteralBody ─────────────────────────────────────────────────

TEST(CharDecode, PlainStringIsVerbatim) {
    auto r = decodeStringLiteralBody("hello");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "hello");
}

TEST(CharDecode, SimpleEscapes) {
    auto r = decodeStringLiteralBody("a\\nb\\tc\\r\\0d");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::string("a\nb\tc\r\0d", 8));
}

TEST(CharDecode, EmbeddedEscapedQuotes) {
    auto r = decodeStringLiteralBody("a\\\"b\\'c");   // body: a\"b\'c
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "a\"b'c");
}

TEST(CharDecode, BackslashAndBellEtc) {
    auto r = decodeStringLiteralBody("\\\\\\a\\b\\f\\v");  // \\ \a \b \f \v
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::string("\\\a\b\f\v"));
}

TEST(CharDecode, HexEscapeTwoDigits) {
    auto r = decodeStringLiteralBody("\\x41\\x42");   // \x41 \x42 → "AB"
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "AB");
}

TEST(CharDecode, HexEscapeOneDigitStopsAtNonHex) {
    // `\x7` followed by a non-hex byte `g` — the second-digit lookahead must
    // stop, consuming only one hex digit.
    auto r = decodeStringLiteralBody("\\x7g");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::string("\x07g"));
}

TEST(CharDecode, EmptyStringDecodesToEmpty) {
    auto r = decodeStringLiteralBody("");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->empty());
}

TEST(CharDecode, UnknownEscapeFails) {
    EXPECT_FALSE(decodeStringLiteralBody("\\q").has_value());
}

TEST(CharDecode, TrailingLoneBackslashFails) {
    EXPECT_FALSE(decodeStringLiteralBody("abc\\").has_value());
}

TEST(CharDecode, HexEscapeWithNoDigitFails) {
    EXPECT_FALSE(decodeStringLiteralBody("\\xg").has_value());   // \x not followed by a hex digit
    EXPECT_FALSE(decodeStringLiteralBody("\\x").has_value());    // \x at end
}

// ── octal escapes \ooo (C 6.4.4.4) ──────────────────────────────────────────

TEST(CharDecode, OctalEscapeOneToThreeDigits) {
    // `\101` = 'A' (65), `\0` = NUL, `\7` = BEL (7), `\377` = 255 (max in range).
    auto r = decodeStringLiteralBody("\\101\\0\\7\\377");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::string("A\0\a\xFF", 4));
}

TEST(CharDecode, OctalEscapeConsumesAtMostThreeDigitsNoMisSplit) {
    // `\012` is ONE byte (octal 12 = 10 = '\n'), NOT `\0` + "12". `\1234` is
    // `\123` (octal 123 = 83 = 'S') + the literal '4' — the loop takes at most
    // three octal digits total.
    auto r = decodeStringLiteralBody("\\012\\1234");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::string("\nS4", 3));
}

TEST(CharDecode, OctalEscapeOutOfRangeFailsLoud) {
    // C 6.4.4.4p9: an octal escape past the unsigned-char range (\400..\777,
    // i.e. > 255) is a constraint violation — fail loud, NEVER silently masked.
    // RED-ON-DISABLE for the `if (v > 0xFF) return false;` guard: restore the
    // old `v & 0xFF` and `\400` decodes to 0 → has_value() → this fails.
    EXPECT_FALSE(decodeStringLiteralBody("\\400").has_value());   // octal 256
    EXPECT_FALSE(decodeStringLiteralBody("\\777").has_value());   // octal 511
    auto ok = decodeStringLiteralBody("\\377");                   // largest in range
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(*ok, std::string("\xFF", 1));
}

// ── decodeCharLiteralBody ───────────────────────────────────────────────────

TEST(CharDecode, CharPlainByte) {
    auto r = decodeCharLiteralBody("a");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, static_cast<std::uint32_t>('a'));
}

TEST(CharDecode, CharEscape) {
    auto r = decodeCharLiteralBody("\\n");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 10u);
}

TEST(CharDecode, CharHexEscape) {
    auto r = decodeCharLiteralBody("\\x41");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 65u);
}

TEST(CharDecode, EmptyCharFails) {
    EXPECT_FALSE(decodeCharLiteralBody("").has_value());
}

TEST(CharDecode, MultiCharFails) {
    EXPECT_FALSE(decodeCharLiteralBody("ab").has_value());       // two plain bytes
    EXPECT_FALSE(decodeCharLiteralBody("a\\n").has_value());     // byte + escape
}

TEST(CharDecode, CharUnknownEscapeFails) {
    EXPECT_FALSE(decodeCharLiteralBody("\\q").has_value());
}

TEST(CharDecode, CharOctalEscape) {
    // `'\101'` is the int 65 ('A'); `'\301'` is 193 (the SQLite EBCDIC-guard
    // value, `#if 'A' == '\301'`). An out-of-range `'\400'` fails loud.
    auto a = decodeCharLiteralBody("\\101");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(*a, 65u);
    auto b = decodeCharLiteralBody("\\301");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(*b, 193u);
    EXPECT_FALSE(decodeCharLiteralBody("\\400").has_value());
}
