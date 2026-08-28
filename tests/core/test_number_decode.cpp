// FC1 cycle 2 (2026-06-10) — unit pins for the shared numeric-literal
// decoders (core/types/number_decode.hpp). decodeFloat was hoisted
// here from cst_to_hir.cpp where its old body stripped EVERY 'f'/'F'
// char (a hardcoded C-ism that value-corrupted hex-float mantissas);
// decodeInteger's prefix detection was de-hardcoded to read the
// schema's declared integerPrefixes (the old 0x/0b/0o/0 hardcode
// silently returned 0 for any non-C prefix like `$ff`). These pins
// use EXACT double equality — the values are all dyadic rationals, so
// strtod must produce them bit-exactly on every conforming toolchain
// (MSVC ≥ VS2015 / GCC / clang all parse C99 hex-floats).

#include "core/types/number_decode.hpp"
#include "core/types/number_style.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace dss;

namespace {

// The c-shaped style (mirrors c.lang.json's numberStyle).
[[nodiscard]] NumberStyle cStyle() {
    NumberStyle s;
    s.decimal = true;
    s.integerPrefixes.push_back({"0x", 16, "0-9a-fA-F", std::nullopt});
    s.integerPrefixes.push_back({"0X", 16, "0-9a-fA-F", std::nullopt});
    s.integerPrefixes.push_back({"0b", 2, "01", std::nullopt});
    s.integerPrefixes.push_back({"0B", 2, "01", std::nullopt});
    s.integerPrefixes.push_back({"0o", 8, "0-7", std::nullopt});
    s.integerPrefixes.push_back({"0O", 8, "0-7", std::nullopt});
    s.integerPrefixes.push_back({"0", 8, "0-7", std::nullopt});
    s.fractionPoint  = '.';
    s.digitSeparator = '\'';
    s.integerSuffixes = {"u", "U", "l", "L", "ll", "LL", "ul", "UL",
                         "lu", "LU", "ull", "ULL", "llu", "LLU"};
    s.floatSuffixes   = {"f", "F"};
    return s;
}

}  // namespace

// ─── decodeFloat: hex-float values, exact ──────────────────────────────────

TEST(NumberDecode, FloatHexBasicValuesExact) {
    auto const s = cStyle();
    bool ok = false;
    EXPECT_EQ(decodeFloat("0x1.8p3", &s, ok), 12.0);   // 1.5 * 2^3
    EXPECT_TRUE(ok);
    EXPECT_EQ(decodeFloat("0x.8p1", &s, ok), 1.0);     // 0.5 * 2^1
    EXPECT_TRUE(ok);
    EXPECT_EQ(decodeFloat("0x1.p3", &s, ok), 8.0);     // 1.0 * 2^3
    EXPECT_TRUE(ok);
    EXPECT_EQ(decodeFloat("0x1p-2", &s, ok), 0.25);    // 1.0 * 2^-2
    EXPECT_TRUE(ok);
    EXPECT_EQ(decodeFloat("0X1.8P3", &s, ok), 12.0);   // uppercase form
    EXPECT_TRUE(ok);
}

TEST(NumberDecode, FloatHexMantissaFDigitIsNotStripped) {
    // THE red-on-disable pin for the decodeFloat fix: the pre-FC1c2
    // body stripped every 'f'/'F' anywhere, so "0x1.fp3" decoded as
    // "0x1.p3" = 8.0 — a silent value corruption. 0x1.f = 1 + 15/16
    // = 1.9375; * 2^3 = 15.5 exactly.
    auto const s = cStyle();
    bool ok = false;
    EXPECT_EQ(decodeFloat("0x1.fp3", &s, ok), 15.5);
    EXPECT_TRUE(ok);
}

TEST(NumberDecode, FloatTrailingSuffixStrippedOnlyAtEnd) {
    auto const s = cStyle();
    bool ok = false;
    EXPECT_EQ(decodeFloat("0x1.8p3f", &s, ok), 12.0);  // suffix gone
    EXPECT_TRUE(ok);
    EXPECT_EQ(decodeFloat("0x1.fp3F", &s, ok), 15.5);  // digit f kept, suffix F gone
    EXPECT_TRUE(ok);
    EXPECT_EQ(decodeFloat("1.5f", &s, ok), 1.5);       // decimal unchanged
    EXPECT_TRUE(ok);
    EXPECT_EQ(decodeFloat("12.5", &s, ok), 12.5);
    EXPECT_TRUE(ok);
}

TEST(NumberDecode, FloatSeparatorsStripped) {
    auto const s = cStyle();
    bool ok = false;
    // 0x18 = 24; * 2^4 = 384. Separator between mantissa digits.
    EXPECT_EQ(decodeFloat("0x1'8p4", &s, ok), 384.0);
    EXPECT_TRUE(ok);
    // Separator between exponent digits: 2^10 = 1024.
    EXPECT_EQ(decodeFloat("0x1p1'0", &s, ok), 1024.0);
    EXPECT_TRUE(ok);
}

TEST(NumberDecode, FloatDecimalEdgeFormsDecode) {
    auto const s = cStyle();
    bool ok = false;
    EXPECT_EQ(decodeFloat("1.", &s, ok), 1.0);
    EXPECT_TRUE(ok);
    EXPECT_EQ(decodeFloat(".5", &s, ok), 0.5);
    EXPECT_TRUE(ok);
    EXPECT_EQ(decodeFloat("1.e3", &s, ok), 1000.0);
    EXPECT_TRUE(ok);
}

TEST(NumberDecode, FloatPartialParseFailsLoud) {
    // Audit fold (FC1c2): a non-strtod-shaped float (a synthetic
    // config's `^` exponent) must NOT silently truncate to its
    // strtod-parsable prefix — full-consumption is required for
    // ok=true. (Pre-fold: "1.5^3" returned 1.5 with ok=true.)
    NumberStyle s;
    s.decimal       = true;
    s.fractionPoint = '.';
    bool ok = true;
    (void)decodeFloat("1.5^3", &s, ok);
    EXPECT_FALSE(ok)
        << "partial strtod consumption must report ok=false — the "
           "caller's diagnostic is the loud path for exotic configs.";
    // Empty body (a pathological all-suffix token) is also not ok.
    s.floatSuffixes = {"f"};
    ok = true;
    (void)decodeFloat("f", &s, ok);
    EXPECT_FALSE(ok);
}

// ─── decodeInteger: config-driven prefixes ─────────────────────────────────

TEST(NumberDecode, IntegerShippedShapesUnchanged) {
    // Behavior-identity pins for every shipped prefix shape — the
    // de-hardcode must not move a single value.
    auto const s = cStyle();
    EXPECT_EQ(decodeInteger("0x1F", &s), std::uint64_t{31});
    EXPECT_EQ(decodeInteger("017", &s), std::uint64_t{15});
    EXPECT_EQ(decodeInteger("0b101", &s), std::uint64_t{5});
    EXPECT_EQ(decodeInteger("0o17", &s), std::uint64_t{15});
    EXPECT_EQ(decodeInteger("0", &s), std::uint64_t{0});
    EXPECT_EQ(decodeInteger("123", &s), std::uint64_t{123});
    EXPECT_EQ(decodeInteger("123u", &s), std::uint64_t{123});
    EXPECT_EQ(decodeInteger("0xFull", &s), std::uint64_t{15});
    EXPECT_EQ(decodeInteger("1'000'000", &s), std::uint64_t{1000000});
}

TEST(NumberDecode, IntegerConfigPrefixRadixIsRead) {
    // RED-on-disable vs the old hardcode: a `$` hex prefix is not in
    // the 0x/0b/0o/0 set, so the pre-FC1c2 decoder parsed "$ff" as
    // decimal, stopped at '$'… and returned 0 — a silent wrong value.
    NumberStyle s;
    s.integerPrefixes.push_back({"$", 16, "0-9a-fA-F", std::nullopt});
    EXPECT_EQ(decodeInteger("$ff", &s), std::uint64_t{255});
}

TEST(NumberDecode, IntegerHighRadixDigitsDecode) {
    // The digit map now covers a..z (10..35): the old map stopped at
    // 'f', silently mis-valuing any radix-17+ config. z9 in base 36
    // = 35*36 + 9 = 1269.
    NumberStyle s;
    s.integerPrefixes.push_back({"#", 36, "0-9a-zA-Z", std::nullopt});
    EXPECT_EQ(decodeInteger("#z9", &s), std::uint64_t{1269});
}

TEST(NumberDecode, IntegerHighRadixSuffixStrippedBeforeParse) {
    // At radix ≥ 31 the letter 'u' IS a digit (30) — the trailing
    // declared suffix must be stripped BEFORE the digit loop or it
    // would be consumed as a digit (silent wrong value).
    NumberStyle s;
    s.integerPrefixes.push_back({"#", 36, "0-9a-zA-Z", std::nullopt});
    s.integerSuffixes = {"u"};
    // #zu → strip 'u' → z = 35. (Unstripped: 35*36 + 30 = 1290.)
    EXPECT_EQ(decodeInteger("#zu", &s), std::uint64_t{35});
}

TEST(NumberDecode, IntegerNullStyleIsPlainDecimal) {
    EXPECT_EQ(decodeInteger("123", nullptr), std::uint64_t{123});
    // No style → no prefixes: "0x10" parses the leading 0 and stops.
    EXPECT_EQ(decodeInteger("0x10", nullptr), std::uint64_t{0});
}

TEST(NumberDecode, IntegerOverflowReturnsNullopt) {
    auto const s = cStyle();
    EXPECT_EQ(decodeInteger("0xFFFFFFFFFFFFFFFF", &s),
              std::uint64_t{0xFFFFFFFFFFFFFFFFull});
    EXPECT_EQ(decodeInteger("0x10000000000000000", &s), std::nullopt);
}

// ─── D-CSUBSET-BITINT-ZERO-LITERAL-EATEN-BY-THE-OCTAL-PREFIX ──────────────
//
// THE DEFECT, AT THE TIER IT LIVED IN. C's octal prefix is spelled `0`, so for
// the literal `0` the longest-prefix scan consumed the ONLY digit and left an
// empty body. `decodeInteger` has no "no digits" verdict and returned 0 (right
// answer, by luck); `decodeBigInteger` DOES have one and returned nullopt — and
// its caller reports nullopt as "no declared type can hold it", so a
// well-formed `_BitInt` ZERO was refused as TOO LARGE.
//
// ★ The two decoders are the same grammar read twice, and this is the shape the
// pins below fix in place: they now share ONE normalization, so the pair is
// asserted TOGETHER on every case. A future divergence cannot hide in one of
// them.
TEST(NumberDecode, IntegerZeroSurvivesADigitValuedPrefix) {
    auto const s = cStyle();
    // The bare decimal spelling — the one that was broken.
    EXPECT_EQ(decodeInteger("0", &s), std::uint64_t{0});
    ASSERT_TRUE(decodeBigInteger("0", &s).has_value())
        << "a digit-valued prefix that consumes the whole literal IS the "
           "literal's digits (C 6.4.4.1: octal-constant := 0 | …) — returning "
           "nullopt here made the caller report a ZERO as 'too large'";
    EXPECT_EQ(*decodeBigInteger("0", &s), (std::vector<std::uint64_t>{0}));
    // …and every ladder spelling that ALREADY worked must keep working.
    for (auto const* t : {"00", "0x0", "0X0", "0b0", "0B0", "0o0", "0O0"}) {
        SCOPED_TRACE(t);
        EXPECT_EQ(decodeInteger(t, &s), std::uint64_t{0});
        ASSERT_TRUE(decodeBigInteger(t, &s).has_value());
        EXPECT_EQ(*decodeBigInteger(t, &s), (std::vector<std::uint64_t>{0}));
    }
}

// THE OTHER DIRECTION, and it is what keeps the fix from being "return 0 for
// anything empty": a prefix carrying a LETTER that is not a digit in its own
// radix stays a pure marker, so an empty remainder is still MALFORMED. These
// are the spellings the tokenizer already refuses (`0xwb` → P0001); the decoder
// must not become the tier that silently accepts them if it ever sees one.
TEST(NumberDecode, IntegerLetterBearingPrefixWithNoBodyStaysMalformed) {
    auto const s = cStyle();
    for (auto const* t : {"0x", "0X", "0b", "0B", "0o", "0O"}) {
        SCOPED_TRACE(t);
        EXPECT_EQ(decodeBigInteger(t, &s), std::nullopt)
            << "'x'/'b'/'o' are not valid digits in radix 16/2/8, so the prefix "
               "is a marker and the body is genuinely empty";
    }
}

// AND THE VALUE-BEARING CASE, which is the pin that would catch a fix that
// merely seeded a flag: `010` must stay EIGHT. Re-prepending the prefix digits
// is value-neutral only because C's octal marker is a ZERO; a fix that dropped
// or double-counted them shows up here.
TEST(NumberDecode, IntegerDigitValuedPrefixDoesNotMoveANonZeroValue) {
    auto const s = cStyle();
    EXPECT_EQ(decodeInteger("010", &s), std::uint64_t{8});
    ASSERT_TRUE(decodeBigInteger("010", &s).has_value());
    EXPECT_EQ(*decodeBigInteger("010", &s), (std::vector<std::uint64_t>{8}));
    EXPECT_EQ(decodeInteger("0777", &s), std::uint64_t{511});
    EXPECT_EQ(*decodeBigInteger("0777", &s), (std::vector<std::uint64_t>{511}));
    // The RADIX CLASS is unmoved too — `0` is an octal constant per C 6.4.4.1,
    // which is what gives it the extra unsigned ladder candidates.
    EXPECT_TRUE(integerLiteralIsPrefixed("0", &s));
    EXPECT_TRUE(integerLiteralIsPrefixed("010", &s));
    EXPECT_FALSE(integerLiteralIsPrefixed("10", &s));
}

// THE AGNOSTIC ARM. The rule is derived from the DECLARED radix, never from the
// spelling `0`: a language whose octal marker is `Q` (not a digit in base 8)
// keeps a strictly-marker prefix, while one whose marker is `7` (a valid base-8
// digit) gets the same standalone reading C's `0` gets — and its VALUE is the
// prefix's own digits, not zero. Without this the fix would read as a C-ism.
TEST(NumberDecode, IntegerDigitValuedPrefixRuleIsDerivedFromTheDeclaredRadix) {
    NumberStyle marker;                     // letter marker: never digits
    marker.integerPrefixes.push_back({"Q", 8, "0-7", std::nullopt});
    EXPECT_EQ(decodeBigInteger("Q", &marker), std::nullopt);
    EXPECT_EQ(*decodeBigInteger("Q7", &marker), (std::vector<std::uint64_t>{7}));

    NumberStyle digitish;                   // digit marker: stands alone
    digitish.integerPrefixes.push_back({"7", 8, "0-7", std::nullopt});
    ASSERT_TRUE(decodeBigInteger("7", &digitish).has_value());
    EXPECT_EQ(*decodeBigInteger("7", &digitish), (std::vector<std::uint64_t>{7}))
        << "the standalone prefix's VALUE is its own digits read in its own "
           "radix — 0 would be a C-ism smuggled in as a default";
    EXPECT_EQ(decodeInteger("7", &digitish), std::uint64_t{7});
    // With a body it is a marker again, exactly as `010` is.
    EXPECT_EQ(decodeInteger("71", &digitish), std::uint64_t{1});
}
