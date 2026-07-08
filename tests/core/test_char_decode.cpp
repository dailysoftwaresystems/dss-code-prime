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

// ── universal character names \u / \U (C11/C23 6.4.3) ───────────────────────

TEST(CharDecode, UcnBmpEncodesCanonicalUtf8) {
    // é (é, U+00E9) → the two UTF-8 bytes C3 A9; A (A) → one ASCII byte;
    // € (€) → E2 82 AC. A UCN names a CODE POINT → canonical UTF-8, uniform
    // for narrow + wide.
    auto e = decodeStringLiteralBody("\\u00e9");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(*e, std::string({static_cast<char>(0xC3), static_cast<char>(0xA9)}));
    auto a = decodeStringLiteralBody("\\u0041");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(*a, "A");
    auto euro = decodeStringLiteralBody("\\u20AC");
    ASSERT_TRUE(euro.has_value());
    EXPECT_EQ(*euro, std::string({static_cast<char>(0xE2), static_cast<char>(0x82),
                                  static_cast<char>(0xAC)}));
}

TEST(CharDecode, UcnAstralEncodesFourUtf8Bytes) {
    // \U0001F600 (😀) → F0 9F 98 80 (the canonical 4-byte UTF-8 for U+1F600).
    auto r = decodeStringLiteralBody("\\U0001F600");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::string({static_cast<char>(0xF0), static_cast<char>(0x9F),
                               static_cast<char>(0x98), static_cast<char>(0x80)}));
}

TEST(CharDecode, UcnMaxScalarValueOk) {
    // \U0010FFFF is the largest Unicode scalar value → valid, 4 UTF-8 bytes
    // F4 8F BF BF. Boundary of the FF1 > U+10FFFF reject.
    auto r = decodeStringLiteralBody("\\U0010FFFF");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::string({static_cast<char>(0xF4), static_cast<char>(0x8F),
                               static_cast<char>(0xBF), static_cast<char>(0xBF)}));
}

TEST(CharDecode, UcnTooFewHexDigitsFails) {
    // \u needs EXACTLY 4 hex digits, \U EXACTLY 8. Fewer, or a non-hex digit
    // inside the run, fails loud.
    EXPECT_FALSE(decodeStringLiteralBody("\\u123").has_value());     // 3 hex
    EXPECT_FALSE(decodeStringLiteralBody("\\U0001").has_value());    // 4 hex for \U
    EXPECT_FALSE(decodeStringLiteralBody("\\u12zz").has_value());    // non-hex in run
    EXPECT_FALSE(decodeStringLiteralBody("\\u").has_value());        // no digits at all
}

TEST(CharDecode, UcnSurrogateHalfFailsLoud) {
    // FF1: U+D800..U+DFFF are UTF-16 surrogate halves, not scalar values →
    // rejected INSIDE the decoder (before any append), so the narrow path never
    // emits CESU-8. RED-ON-DISABLE for the surrogate guard.
    EXPECT_FALSE(decodeStringLiteralBody("\\uD800").has_value());
    EXPECT_FALSE(decodeStringLiteralBody("\\uDC00").has_value());
    EXPECT_FALSE(decodeStringLiteralBody("\\uDFFF").has_value());
}

TEST(CharDecode, UcnBeyondUnicodeRangeFailsLoud) {
    // FF1: > U+10FFFF is not a Unicode scalar value → fail loud.
    EXPECT_FALSE(decodeStringLiteralBody("\\U00110000").has_value());
    EXPECT_FALSE(decodeStringLiteralBody("\\UFFFFFFFF").has_value());
}

TEST(CharDecode, UcnFailureReportsInvalidUniversalName) {
    // FF2: an invalid/malformed UCN reports the SPECIFIC InvalidUniversalName
    // error (→ H_InvalidUniversalCharacterName), distinct from a generic
    // Malformed escape (→ the generic message).
    std::string out;
    EXPECT_EQ(decodeEscapedBytes("\\uD800", out).error,
              EscapeDecodeError::InvalidUniversalName);
    out.clear();
    EXPECT_EQ(decodeEscapedBytes("\\U00110000", out).error,
              EscapeDecodeError::InvalidUniversalName);
    out.clear();
    EXPECT_EQ(decodeEscapedBytes("\\u12", out).error,
              EscapeDecodeError::InvalidUniversalName);
    out.clear();
    EXPECT_EQ(decodeEscapedBytes("\\q", out).error, EscapeDecodeError::Malformed);
}

TEST(CharDecode, ByteEscapeFlagSetForHexAndOctalOnly) {
    // FF3: the decoder flags a consumed \x / octal byte escape (the wide path
    // rejects it fail-loud); a UCN, a named escape, and plain text do NOT.
    std::string out;
    EXPECT_TRUE(decodeEscapedBytes("\\x41", out).usedByteEscape);
    out.clear();
    EXPECT_TRUE(decodeEscapedBytes("\\101", out).usedByteEscape);
    out.clear();
    EXPECT_TRUE(decodeEscapedBytes("ok\\x41ok", out).usedByteEscape);  // flag persists across the loop
    out.clear();
    EXPECT_FALSE(decodeEscapedBytes("\\U0001F600", out).usedByteEscape);
    out.clear();
    EXPECT_FALSE(decodeEscapedBytes("\\n\\t", out).usedByteEscape);
    out.clear();
    EXPECT_FALSE(decodeEscapedBytes("plain", out).usedByteEscape);
}

TEST(CharDecode, CharUcnSingleByteOkMultiByteNotSingleChar) {
    // A narrow char UCN that fits ONE byte: 'A' → 'A'. A valid UCN that needs
    // >1 UTF-8 byte ('é' → C3 A9) is not a single narrow char → nullopt, but
    // the decode is OK (NOT an invalid UCN — the caller shows the multi-char
    // message, not H_InvalidUniversalCharacterName).
    auto a = decodeCharLiteralBody("\\u0041");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(*a, 65u);
    EscapeDecodeOutcome oc;
    EXPECT_FALSE(decodeCharLiteralBody("\\u00e9", &oc).has_value());
    EXPECT_TRUE(oc.ok()) << "a valid multi-byte UCN is OK, just not one narrow char";
    // An invalid UCN in a char body reports InvalidUniversalName.
    EscapeDecodeOutcome bad;
    EXPECT_FALSE(decodeCharLiteralBody("\\uD800", &bad).has_value());
    EXPECT_EQ(bad.error, EscapeDecodeError::InvalidUniversalName);
}
