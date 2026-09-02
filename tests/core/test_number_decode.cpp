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
    // ✔MEASURED against `src/dss-config/sources/c.lang.json`'s own
    // `numberStyle.floatSuffixes`: the shipped list is f/F/l/L. The l/L pair is
    // load-bearing for the D-CSUBSET-LONG-DOUBLE-LITERAL-DECODE-PRECISION pins
    // below — without it `0.1L` would keep its suffix and no decoder would parse
    // it, so the pins would pass VACUOUSLY on the refusal path.
    s.floatSuffixes   = {"f", "F", "l", "L"};
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

// ─── D-CSUBSET-LONG-DOUBLE-LITERAL-DECODE-PRECISION: decodeFloatWide ────────
//
// ★★★ THE ORACLE IS THE REFERENCE COMPILER'S BIT PATTERN, NEVER DSS'S OWN.
// Comparing a DSS-decoded literal against a DSS-decoded literal would pass with
// both wrong together, which is how this defect survived from 2026-07-18 to
// 2026-09-02: `20.0L` and `0.5L` are EXACT in binary64 and the corpus held
// nothing else. Every `want` pair below is the `{lo, hi}` of the 16 bytes a
// reference compiler ACTUALLY EMITTED for that literal as a static initializer,
// ✔MEASURED 2026-09-02:
//   * F80  — gcc 13.3.0 AND clang 18.1.3, x86_64-linux `long double`, probed
//            SEPARATELY and agreeing byte-for-byte.
//   * F128 — gcc 13.3.0 via `__float128` on x86_64 AND aarch64-linux-gnu-gcc
//            13.3.0's own `long double`, agreeing byte-for-byte.
// `WideFloatValue::pack()` is byte-identical to what `appendWideFloatBits`
// writes (lo = the low 8 LE bytes of the 16-byte slot, hi = the high 8), so a
// `pack()` equality here IS a comparison of emitted bytes.
namespace {

struct WantBits { std::uint64_t lo, hi; };

[[nodiscard]] WantBits packOf(WideFloatValue const& v) {
    auto const p = v.pack();
    return {p.lo, p.hi};
}

void expectDecodes(char const* text, TypeKind kind, WantBits want,
                   char const* provenance) {
    auto const s = cStyle();
    bool ok = false;
    auto const v = decodeFloatWide(text, &s, kind, ok);
    ASSERT_TRUE(ok) << text << " (" << provenance << ") did not decode";
    ASSERT_TRUE(v.has_value());
    auto const got = packOf(*v);
    EXPECT_EQ(got.lo, want.lo) << text << " low half — reference: " << provenance;
    EXPECT_EQ(got.hi, want.hi) << text << " high half — reference: " << provenance;
}

}  // namespace

// THE ROW ITSELF. `0.1L` needs more than 53 mantissa bits, so the old
// strtod→host-`double` decode produced a value whose low 11 (F80) / 60 (F128)
// significand bits were ZERO. ✔MEASURED at the emitted bytes: DSS used to write
// `00 d0 cc cc cc cc cc cc fb 3f` where both references write
// `cd cc cc cc cc cc cc cc fb 3f`.
TEST(NumberDecodeWide, TenthDecodesAtTargetPrecisionNotHostDouble) {
    expectDecodes("0.1L", TypeKind::F80,
                  {0xCCCCCCCCCCCCCCCDull, 0x3FFBull},
                  "gcc 13.3.0 + clang 18.1.3, x86_64 long double");
    expectDecodes("0.1L", TypeKind::F128,
                  {0x999999999999999Aull, 0x3FFB999999999999ull},
                  "aarch64 gcc 13.3.0 long double / x86_64 gcc __float128");
}

// ★ THE DIRECT REFUTATION, stated as an INEQUALITY rather than a pattern: the
// target-precision decode must NOT equal the host `double` decode widened. A
// pattern-only fixture would still pass if a future change re-routed the decode
// through binary64 and the expected pattern were regenerated from DSS itself;
// this one cannot.
TEST(NumberDecodeWide, TargetPrecisionDecodeDiffersFromTheWidenedHostDouble) {
    auto const s = cStyle();
    for (auto kind : {TypeKind::F80, TypeKind::F128}) {
        bool wideOk = false, hostOk = false;
        auto const wide  = decodeFloatWide("0.1L", &s, kind, wideOk);
        double const hostD = decodeFloat("0.1L", &s, hostOk);
        ASSERT_TRUE(wideOk);
        ASSERT_TRUE(hostOk);
        ASSERT_TRUE(wide.has_value());
        auto const widened = WideFloatValue::fromDouble(hostD, kind);
        EXPECT_FALSE(*wide == widened)
            << "0.1L decoded at target precision must differ from the binary64 "
               "value widened — equality means the leaf is still host-rounded";
        if (kind == TypeKind::F80) {
            // ...and the widened one is exactly the WRONG pattern that shipped.
            EXPECT_EQ(packOf(widened).lo, 0xCCCCCCCCCCCCD000ull)
                << "the pre-fix F80 significand, low 11 bits zeroed";
        }
    }
}

// ★★ ROUND-TO-NEAREST-EVEN AT THE BOUNDARY — where a decimal parser fails. Both
// decimals below are EXACT midpoints between two adjacent normals (generated
// with Python `Decimal`, prec=200), so neither is decidable by "enough digits";
// only the tie rule settles them. The first's lower neighbour is 1.0, whose
// significand is EVEN, so the tie rounds DOWN; the second's lower neighbour is
// odd, so the same tie rounds UP.
TEST(NumberDecodeWide, F80HalfwayCasesRoundToNearestEven) {
    // 1 + 2^-64 → 1.0 exactly (significand 0x8000000000000000, exp field 0x3FFF)
    expectDecodes("1.0000000000000000000542101086242752217003726400434970855712890625L",
                  TypeKind::F80, {0x8000000000000000ull, 0x3FFFull},
                  "gcc 13.3.0 + clang 18.1.3: 00 00 00 00 00 00 00 80 ff 3f");
    // 1 + 2^-63 + 2^-64 → rounds UP to 0x8000000000000002
    expectDecodes("1.0000000000000000001626303258728256651011179201304912567138671875L",
                  TypeKind::F80, {0x8000000000000002ull, 0x3FFFull},
                  "gcc 13.3.0 + clang 18.1.3: 02 00 00 00 00 00 00 80 ff 3f");
}

TEST(NumberDecodeWide, F128HalfwayCasesRoundToNearestEven) {
    // 1 + 2^-113 → 1.0 exactly (fraction 0)
    expectDecodes("1.00000000000000000000000000000000009629649721936179265279889712"
                  "924636592690508241076940976199693977832794189453125L",
                  TypeKind::F128, {0ull, 0x3FFF000000000000ull},
                  "gcc 13.3.0 __float128: 00 …00 ff 3f");
    // 1 + 2^-112 + 2^-113 → rounds UP to fraction 2
    expectDecodes("1.00000000000000000000000000000000028888949165808537795839669138"
                  "773909778071524723230822928599081933498382568359375L",
                  TypeKind::F128, {2ull, 0x3FFF000000000000ull},
                  "gcc 13.3.0 __float128: 02 00 …00 ff 3f");
}

// A 36-digit transcendental — the ordinary case a maths header ships, and where
// a decimal parser that is merely "close" is visibly wrong.
TEST(NumberDecodeWide, LongDecimalTranscendentalsAreBitExact) {
    expectDecodes("3.14159265358979323846264338327950288L", TypeKind::F80,
                  {0xC90FDAA22168C235ull, 0x4000ull},
                  "gcc 13.3.0 + clang 18.1.3: 35 c2 68 21 a2 da 0f c9 00 40");
    expectDecodes("3.14159265358979323846264338327950288L", TypeKind::F128,
                  {0x8469898CC51701B8ull, 0x4000921FB54442D1ull},
                  "aarch64 gcc 13.3.0: b8 01 17 c5 8c 89 69 84 d1 42 44 b5 1f 92 00 40");
    expectDecodes("2.71828182845904523536028747135266250L", TypeKind::F80,
                  {0xADF85458A2BB4A9Bull, 0x4000ull},
                  "gcc 13.3.0 + clang 18.1.3: 9b 4a bb a2 58 54 f8 ad 00 40");
}

// The hex-float door is the SAME decoder and must stay exact: a hex float is
// already binary, so every bit of it survives at any target precision.
TEST(NumberDecodeWide, HexFloatsDecodeExactlyAtTargetPrecision) {
    expectDecodes("0x1.fp3L", TypeKind::F80,
                  {0xF800000000000000ull, 0x4002ull}, "15.5L");
    expectDecodes("0x1.fp3L", TypeKind::F128,
                  {0ull, 0x4002F00000000000ull}, "15.5L");
    // Digit separators go through the SAME shared body normalization
    // `decodeFloat` uses, so the two decoders cannot disagree about the digits.
    expectDecodes("0x1'8p4L", TypeKind::F80,
                  {0xC000000000000000ull, 0x4007ull}, "384.0L = 0x1.8p8");
}

// ★ A VALUE THE HOST `double` PATH CANNOT EVEN CARRY. `1e-320` is SUBNORMAL in
// binary64 (strtod sets ERANGE, so `decodeFloat` refuses it loud) and perfectly
// NORMAL in F80/F128 — a literal DSS rejected on every axis and now decodes
// bit-exactly on the wide ones. That the host path still refuses it is itself
// the proof this is TARGET precision and not a host facility.
TEST(NumberDecodeWide, SubnormalInBinary64IsNormalAtTargetPrecision) {
    auto const s = cStyle();
    bool hostOk = true;
    (void)decodeFloat("1e-320L", &s, hostOk);
    EXPECT_FALSE(hostOk) << "the host double path must still refuse it (ERANGE)";
    expectDecodes("1e-320L", TypeKind::F80,
                  {0xFD00B897478238D1ull, 0x3BD7ull},
                  "gcc 13.3.0 + clang 18.1.3: d1 38 82 47 97 b8 00 fd d7 3b");
    expectDecodes("1e-320L", TypeKind::F128,
                  {0x71A1124161312AAAull, 0x3BD7FA01712E8F04ull},
                  "aarch64 gcc 13.3.0");
}

// Zero and the loud contract. `ok` keeps `decodeFloat`'s meaning exactly: a body
// the grammar cannot FULLY consume, a value outside the target's NORMAL range,
// or a kind the wide kernel does not realize all leave it false — never a silent
// zero, never a silently truncated value.
TEST(NumberDecodeWide, ZeroAndTheLoudRefusalContract) {
    auto const s = cStyle();
    bool ok = false;

    auto const z = decodeFloatWide("0.0L", &s, TypeKind::F80, ok);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(z.has_value());
    EXPECT_TRUE(z->isZero());

    // A kind the soft-float kernel does not realize — refused, never guessed.
    ok = true;
    EXPECT_FALSE(decodeFloatWide("0.1", &s, TypeKind::F64, ok).has_value());
    EXPECT_FALSE(ok);

    // A body the grammar cannot fully consume (the `1.5^3` case `decodeFloat`
    // already refuses — the two decoders must draw the line in the same place).
    for (char const* bad : {"1.5^3", "L", "1e", "0xL", ".L"}) {
        ok = true;
        EXPECT_FALSE(decodeFloatWide(bad, &s, TypeKind::F80, ok).has_value()) << bad;
        EXPECT_FALSE(ok) << bad;
    }

    // Out of the target's NORMAL range in both directions.
    ok = true;
    EXPECT_FALSE(decodeFloatWide("1e5000L", &s, TypeKind::F80, ok).has_value());
    EXPECT_FALSE(ok) << "overflow to infinity is refused, never baked silently";
    ok = true;
    EXPECT_FALSE(decodeFloatWide("1e-5000L", &s, TypeKind::F80, ok).has_value());
    EXPECT_FALSE(ok) << "a subnormal RESULT is the fail-loud verdict roundNormal "
                        "already carries for add/sub/mul/div";
}

// The register handed to the rounding chokepoint must be NORMALIZED, and a
// caller that gets that wrong fails loud rather than rounding a value it was not
// given. (The decoder always normalizes; this pins the guard itself.)
TEST(NumberDecodeWide, FromExactBinaryRefusesAnUnNormalizedRegister) {
    std::uint64_t const bad[4] = {0, 0, 0, 1};   // highest set bit is 192, not 255
    EXPECT_FALSE(WideFloatValue::fromExactBinary(
        TypeKind::F80, false, 0, bad, false).has_value());
    std::uint64_t const good[4] = {0, 0, 0, std::uint64_t{1} << 63};
    auto const one = WideFloatValue::fromExactBinary(
        TypeKind::F80, false, 0, good, false);
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(one->pack().lo, 0x8000000000000000ull);
    EXPECT_EQ(one->pack().hi, 0x3FFFull);
}
