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

TEST(CharDecode, OctalEscapeOutOfRangeFailsLoudForAByteElement) {
    // ⏳ RESTATED, NOT RETIRED (P55). The CLAIM is unchanged and still true —
    // C 6.4.4.4p9, an octal escape past the unsigned-char range is a constraint
    // violation and must fail loud, never be silently masked. What moved is WHO
    // enforces it. It used to be `if (v > 0xFF)` inside the decoder, and that was
    // wrong in the other direction: the decoder does not know the element, so it
    // also refused `u"\777"`, which all four references assemble into one 0x1FF
    // char16_t unit. The bound belongs to the ELEMENT, so the check is now
    // `firstEscapeValueTooWide` and the decoder merely records the value.
    //
    // RED-ON-DISABLE, REMOVE DIRECTION: delete the `firstEscapeValueTooWide` call
    // from `decodeCharLiteralBody` (or the narrow-run call in `lowerStringLiteral`)
    // and `\400` comes back as a silent 0x00 — the first two arms go green-to-red.
    std::string out;
    EscapeDecodeOutcome oc256 = decodeEscapedBytes("\\400", out);
    ASSERT_TRUE(oc256.ok()) << "the decoder records the value; it does not judge it";
    ASSERT_EQ(oc256.escapeUnits.size(), 1u);
    EXPECT_EQ(oc256.escapeUnits[0].value, 0400u);
    EXPECT_TRUE(firstEscapeValueTooWide(oc256, 8).has_value())
        << "octal 256 does not fit a byte element — the narrow path must refuse";

    out.clear();
    EscapeDecodeOutcome oc511 = decodeEscapedBytes("\\777", out);
    ASSERT_TRUE(oc511.ok());
    EXPECT_TRUE(firstEscapeValueTooWide(oc511, 8).has_value());
    // ★ AND THE HALF THE OLD GUARD GOT WRONG: 0x1FF is an ordinary char16_t unit.
    EXPECT_FALSE(firstEscapeValueTooWide(oc511, 16).has_value())
        << "u\"\\777\" is one 0x1FF unit on gcc, clang, mingw-w64 and MSVC alike";

    // The narrow ENTRY POINTS still refuse both, which is what a caller sees.
    EXPECT_FALSE(decodeCharLiteralBody("\\400").has_value());
    EXPECT_FALSE(decodeCharLiteralBody("\\777").has_value());

    auto ok = decodeStringLiteralBody("\\377");                   // largest in a byte
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

// ── D-CSUBSET-NARROW-HEX-ESCAPE-TRUNCATED-TO-TWO-DIGITS ──────────────────────
//
// Every expectation below is a MEASURED reference answer, not a reading of the
// standard: each literal was compiled AND RUN by gcc 13.3.0, clang 18.1.3,
// mingw-w64 gcc 13.2.0 and MSVC 19.51 on 2026-09-02 with its emitted units read
// back (scratchpad/p55/we/reference-matrix.json).

TEST(CharDecode, HexEscapeConsumesEveryFollowingHexDigit) {
    // ★ THE DEFECT, PINNED FROM THE DIRECTION IT FAILED. The old decoder took at
    // most TWO hex digits, so `\x041` became 0x04 followed by the CHARACTER '1' —
    // three bytes where all four references produce one. C 6.4.4.4p7 gives `\x` an
    // unbounded digit run.
    std::string out;
    EscapeDecodeOutcome oc = decodeEscapedBytes("\\x041", out);
    EXPECT_TRUE(oc.ok());
    EXPECT_EQ(out, std::string(1, static_cast<char>(0x41)))
        << "\\x041 is ONE byte 0x41 — gcc/clang/mingw/MSVC unanimous";
    ASSERT_EQ(oc.escapeUnits.size(), 1u);
    EXPECT_EQ(oc.escapeUnits[0].value, 0x41u);
    EXPECT_TRUE(oc.escapeUnits[0].hex);

    // Leading zeros are free — they must not count toward the 64-bit ceiling.
    out.clear();
    oc = decodeEscapedBytes("\\x0000000000041", out);
    EXPECT_TRUE(oc.ok());
    EXPECT_EQ(out, std::string(1, static_cast<char>(0x41)))
        << "13 digits of which one is significant is still 0x41";

    // ★ THE SHARPEST CASE: `b` IS a hex digit, so `a\xFFb` is 'a' then the ONE
    // escape 0xFFB. DSS used to emit 61 FF 62 — matching no reference at all.
    out.clear();
    oc = decodeEscapedBytes("a\\xFFb", out);
    EXPECT_TRUE(oc.ok());
    ASSERT_EQ(oc.escapeUnits.size(), 1u);
    EXPECT_EQ(oc.escapeUnits[0].value, 0xFFBu)
        << "the trailing 'b' is a hex DIGIT, not a letter";
    EXPECT_EQ(oc.escapeUnits[0].byteOffset, 1u) << "offset is past the leading 'a'";

    // A non-hex neighbour DOES terminate the escape: `a\xFFz` is three bytes.
    out.clear();
    oc = decodeEscapedBytes("a\\xFFz", out);
    EXPECT_TRUE(oc.ok());
    EXPECT_EQ(out, std::string({'a', static_cast<char>(0xFF), 'z'}));

    // A `\x` with NO digit at all stays malformed — never a bare 'x'.
    out.clear();
    EXPECT_EQ(decodeEscapedBytes("\\xz", out).error, EscapeDecodeError::Malformed);
}

TEST(CharDecode, OctalStopsAtThreeDigitsAndIsNotCappedAt255) {
    // The two escape kinds terminate DIFFERENTLY and that asymmetry is the
    // standard's: octal takes at most three digits, so `a\101b` keeps its 'b'.
    std::string out;
    EscapeDecodeOutcome oc = decodeEscapedBytes("a\\101b", out);
    EXPECT_TRUE(oc.ok());
    EXPECT_EQ(out, "aAb");

    // ★ THE RANGE CHECK LEFT THE DECODER. `\777` is 0x1FF: a constraint violation
    // for a narrow char, and an ordinary unit for char16_t — ✔MEASURED, all four
    // references emit one 0x1FF unit for `u"\777"`. Rejecting it HERE (as the old
    // decoder did) made the wide form unreachable, which is why `u"\777"` was
    // refused as a malformed escape.
    out.clear();
    oc = decodeEscapedBytes("\\777", out);
    EXPECT_TRUE(oc.ok()) << "the decoder does not know the element width";
    ASSERT_EQ(oc.escapeUnits.size(), 1u);
    EXPECT_EQ(oc.escapeUnits[0].value, 0x1FFu);
    EXPECT_FALSE(oc.escapeUnits[0].hex);
    // ...and the width-aware check is what refuses it for a byte element.
    EXPECT_TRUE(firstEscapeValueTooWide(oc, 8).has_value());
    EXPECT_FALSE(firstEscapeValueTooWide(oc, 16).has_value());
}

TEST(CharDecode, EscapeValueTooWideIsReportedWithItsValue) {
    // The check returns the offending UNIT, not a bool, because the diagnostic has
    // to name the width AND the value — both anchors this closes were cases where
    // the compiler knew the number and did not say it.
    std::string out;
    EscapeDecodeOutcome oc = decodeEscapedBytes("\\x1FFFF", out);
    ASSERT_TRUE(oc.ok());
    auto bad = firstEscapeValueTooWide(oc, 16);
    ASSERT_TRUE(bad.has_value());
    EXPECT_EQ(bad->value, 0x1FFFFu);
    EXPECT_TRUE(bad->hex);
    EXPECT_FALSE(firstEscapeValueTooWide(oc, 32).has_value())
        << "0x1FFFF fits a 32-bit unit — U\"\\x1FFFF\" is valid on all four references";

    // A value wider than ANY code unit stops in the decoder itself.
    out.clear();
    EXPECT_EQ(decodeEscapedBytes("\\x123456789ABCDEF01", out).error,
              EscapeDecodeError::EscapeValueTooLarge);

    // 0xFFFFFFFF exactly fills a 32-bit unit and is PAST U+10FFFF — the proof that
    // a byte escape is a raw code UNIT and not a code POINT.
    out.clear();
    oc = decodeEscapedBytes("\\xFFFFFFFF", out);
    ASSERT_TRUE(oc.ok());
    EXPECT_FALSE(firstEscapeValueTooWide(oc, 32).has_value());
}

TEST(CharDecode, NarrowCharLiteralRefusesAnOutOfRangeEscape) {
    // `decodeCharLiteralBody` is the NARROW entry point, so it owns the 8-bit
    // check. ⚠ The check must run BEFORE the single-byte test: `'\x101'` decodes
    // to ONE placeholder byte, so a size-only test would wave it through as 0x01 —
    // which is exactly what gcc/mingw do with a warning and what clang/MSVC refuse.
    EscapeDecodeOutcome oc;
    EXPECT_FALSE(decodeCharLiteralBody("\\x101", &oc).has_value());
    EXPECT_TRUE(oc.ok()) << "the escape itself is well formed; its VALUE is out of range";
    ASSERT_TRUE(firstEscapeValueTooWide(oc, 8).has_value());
    EXPECT_EQ(firstEscapeValueTooWide(oc, 8)->value, 0x101u);

    EXPECT_FALSE(decodeCharLiteralBody("\\777").has_value());
    // The in-range forms are untouched.
    auto ok = decodeCharLiteralBody("\\x41");
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(*ok, 0x41u);
    auto oct = decodeCharLiteralBody("\\101");
    ASSERT_TRUE(oct.has_value());
    EXPECT_EQ(*oct, 0x41u);
}

TEST(CharDecode, ByteEscapeFlagSetForHexAndOctalOnly) {
    // The decoder flags a consumed \x / octal byte escape; a UCN, a named escape,
    // and plain text do NOT. `escapeUnits` carries the same population.
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
    out.clear();
    EXPECT_TRUE(decodeEscapedBytes("\\U0001F600", out).escapeUnits.empty())
        << "a UCN names a code POINT and must never appear as an escape unit";
    out.clear();
    EXPECT_EQ(decodeEscapedBytes("\\x41\\102", out).escapeUnits.size(), 2u);
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
