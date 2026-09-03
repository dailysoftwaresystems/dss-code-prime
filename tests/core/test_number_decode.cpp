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

#include <cerrno>     // ERANGE — the D-C-DECODEFLOAT-TREATS-UNDERFLOW-AS-FATAL pins
#include <cmath>      // HUGE_VAL, std::isinf/std::signbit — the overflow pins
#include <cstdint>
#include <cstring>    // std::memcpy — bit patterns, never a value comparison
#include <limits>     // numeric_limits<double>::infinity() — the normalizer's target
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

// ─── D-C-DECODEFLOAT-TREATS-UNDERFLOW-AS-FATAL: the f64 underflow door ──────
//
// ★★★ THE ORACLE IS THE REFERENCE COMPILERS' EMITTED BYTES. Every pattern whose
// provenance below names the compilers was ✔MEASURED 2026-09-02 from the static
// initializer each reference ACTUALLY EMITTED for that literal, and all FOUR
// agree byte-for-byte: gcc 13.3.0 and clang 18.1.3 (WSL, `-c` then
// `objdump -s -j .rodata`, probed SEPARATELY), mingw-w64 gcc 13.2.0, and MSVC
// cl.exe 19.51.36252 (`/c /std:c17`, then `objdump -s -j .rdata`). None of the
// four refuses any of them; MSVC does not even warn at /W4.
//
// ⓘ The two SIGNED spellings are the exception and say so in their own
// provenance: `-1e-320` is not a C floating constant at all (the `-` is a unary
// operator), so no reference emits it as one initializer. They pin this
// DECODER's sign handling — `decodeFloat` is also called on descriptor text
// that may carry a sign — and their expected values are IEEE-754's, the
// unsigned pattern with the sign bit set.
//
// THE DEFECT. `decodeFloat` ended its verdict with `errno != ERANGE`, reading
// ONE errno as ONE verdict. C23 7.24.1.5 gives that errno to two OPPOSITE
// outcomes and separates them by the RETURNED VALUE (¶12 overflow returns
// ±HUGE_VAL, ¶13 underflow returns a magnitude no greater than the smallest
// normal), so every binary64 SUBNORMAL literal was refused as though it had
// overflowed: `static const double v = 1e-320;` failed with
// `error[H_UnsupportedLoweringForKind]: literal '1e-320' is out of range /
// undecodable` (✔MEASURED at x86_64:pe64-x86_64-windows-exec).
namespace {

[[nodiscard]] std::uint64_t bitsOf(double d) {
    std::uint64_t b = 0;
    static_assert(sizeof b == sizeof d);
    std::memcpy(&b, &d, sizeof b);
    return b;
}

// Decode one `double` literal and assert BOTH halves: that it was accepted, and
// that the accepted value carries the reference bit pattern. `ok` alone is the
// pin a fix in this direction can pass while losing the value.
void expectF64(char const* text, std::uint64_t wantBits, char const* provenance) {
    auto const s = cStyle();
    bool ok = false;
    double const d = decodeFloat(text, &s, ok);
    ASSERT_TRUE(ok) << text << " was refused; every reference accepts it ("
                    << provenance << ")";
    EXPECT_EQ(bitsOf(d), wantBits)
        << text << " decoded to 0x" << std::hex << bitsOf(d) << " — reference: "
        << provenance;
}

}  // namespace

// The row itself: a decimal that underflows to a representable binary64
// SUBNORMAL compiles, and carries the reference bit pattern.
TEST(NumberDecode, FloatBinary64SubnormalsDecodeInsteadOfFailingLoud) {
    // One ulp below the smallest normal — the value immediately across the old
    // accept/refuse line.
    expectF64("2.225073858507201e-308", 0x000FFFFFFFFFFFFFull,
              "gcc/clang/mingw-gcc/MSVC: ff ff ff ff ff ff 0f 00");
    // The literal the row was filed on.
    expectF64("1e-320", 0x00000000000007E8ull,
              "gcc/clang/mingw-gcc/MSVC: e8 07 00 00 00 00 00 00");
    // The smallest subnormal there is: one significand bit.
    expectF64("5e-324", 0x0000000000000001ull,
              "gcc/clang/mingw-gcc/MSVC: 01 00 00 00 00 00 00 00");
    // Just ABOVE the halfway point down to zero, so round-to-nearest carries it
    // UP to that same one bit rather than down to nothing.
    expectF64("3e-324", 0x0000000000000001ull,
              "gcc/clang/mingw-gcc/MSVC: 01 00 00 00 00 00 00 00");
    // Negative subnormals take the same door; only the sign bit moves.
    expectF64("-1e-320", 0x80000000000007E8ull, "glibc/UCRT strtod, sign bit set");
}

// ★ THE FLUSH-TO-ZERO BOUNDARY — where the OVER-correction and the
// UNDER-correction both land. A value below half the smallest subnormal
// underflows all the way to zero, and all four references still ACCEPT it (gcc,
// clang and mingw-gcc warn; MSVC is silent at /W4). So "refuse everything that
// underflows" and "refuse anything that reaches zero" are BOTH wrong.
//
// ⚠ THIS BLOCK ENDED "and the only refusal left is overflow", which was TRUE
// when it was written and went false the same day:
// D-C-FLOAT-LITERAL-OVERFLOW-REFUSED-INSTEAD-OF-YIELDING-INFINITY removed that
// refusal too. On the `double` door NO range verdict refuses any more — only a
// body the grammar cannot consume does.
TEST(NumberDecode, FloatUnderflowAllTheWayToZeroIsStillAccepted) {
    expectF64("2e-324", 0x0000000000000000ull,   // just BELOW the halfway point
              "gcc/clang/mingw-gcc/MSVC: eight zero bytes");
    expectF64("1e-330", 0x0000000000000000ull,
              "gcc/clang/mingw-gcc/MSVC: eight zero bytes");
    // The sign SURVIVES the flush: -1e-330 is negative zero, not positive zero.
    expectF64("-1e-330", 0x8000000000000000ull, "glibc/UCRT strtod: negative zero");
}

// ★★ D-C-FLOAT-LITERAL-OVERFLOW-REFUSED-INSTEAD-OF-YIELDING-INFINITY — the OTHER
// half of the same errno, and the test that used to stand here
// (`FloatOverflowStillFailsLoud`) pinned the behaviour this row reverses. It
// said so itself: "The split is a separate question this test does not settle —
// it pins only that TODAY's behaviour is the loud one." Today changed.
//
// THE SPLIT, ✔MEASURED 2026-09-02 with each reference invoked SEPARATELY:
// gcc 13.3.0 and clang 18.1.3 (WSL) and mingw-w64 gcc 13.2.0 all ACCEPT `1e400`
// — warning `-Woverflow` / `-Wliteral-range` — and all three emit the SAME
// `7ff0000000000000`; MSVC 19.51.36252 refuses (`error C2177: constant too
// big`). That is an accept-vs-refuse split, so the disjunction governs and one
// working reference makes the behaviour REQUIRED.
//
// ⓘ Refusing was never even required by ISO C: 5.2.5.3.3¶19 and Annex F.2.2¶1
// extend a type's representable RANGE to every real number once its infinities
// are representable, so on an IEC 60559 implementation the correctly-rounded
// value of `1e400` IS +∞ — the nearest representable value, exactly as
// 0x3FB999999999999A is 0.1's.
TEST(NumberDecode, FloatBinary64OverflowDecodesToInfinityInsteadOfFailingLoud) {
    expectF64("1e400",  0x7FF0000000000000ull,
              "gcc/clang/mingw-gcc: 00 00 00 00 00 00 f0 7f");
    expectF64("1e309",  0x7FF0000000000000ull,
              "gcc/clang/mingw-gcc: 00 00 00 00 00 00 f0 7f");
    // A hex float takes the same door — strtod parses both shapes, so one
    // predicate covers `0x1p+99999` and `1e400` alike.
    expectF64("0x1p+99999", 0x7FF0000000000000ull,
              "gcc/clang/mingw-gcc: 00 00 00 00 00 00 f0 7f");
    expectF64("0x1p20000",  0x7FF0000000000000ull, "same door, smaller exponent");
    // The SIGN survives. `-1e400` is not a C floating constant (the `-` is a
    // unary operator), so no reference emits it as one initializer; these pin
    // this DECODER's sign handling, which descriptor text also relies on. The
    // expected value is IEEE-754's: the unsigned pattern with the sign bit set,
    // and gcc/clang DO emit `fff0000000000000` for the negated expression.
    expectF64("-1e400",      0xFFF0000000000000ull, "gcc/clang: fff0000000000000");
    expectF64("-0x1p+99999", 0xFFF0000000000000ull, "gcc/clang: fff0000000000000");
}

// ★ THE BOUNDARY, pinned from BOTH sides one ulp apart — the single check that
// separates "accepts overflow" from "stopped checking range at all". DBL_MAX is
// finite and must stay its own bits; the next representable decimal above it
// rounds to +∞. ✔MEASURED: gcc/clang/mingw-gcc emit `7fefffffffffffff` and
// `7ff0000000000000` respectively, and MSVC refuses the second while accepting
// the first — so this pair is also exactly where the reference split lies.
TEST(NumberDecode, FloatOverflowBoundaryIsWhereTheReferencesPutIt) {
    expectF64("1.7976931348623157e308", 0x7FEFFFFFFFFFFFFFull,
              "DBL_MAX — gcc/clang/mingw-gcc/MSVC: 7fefffffffffffff");
    expectF64("1.7976931348623159e308", 0x7FF0000000000000ull,
              "one ulp past DBL_MAX — gcc/clang/mingw-gcc: 7ff0000000000000");
}

// ⚠ WHAT MUST STILL FAIL LOUD, and why these three and not the overflow. Each is
// a case where there is NO correctly-rounded value to return — the decoder does
// not know the answer, rather than knowing an answer someone might find
// surprising. An overflow knows its answer.
TEST(NumberDecode, FloatLoudRefusalsSurviveTheOverflowChange) {
    auto const s = cStyle();
    for (char const* bad : {"1.5^3", "", "f", "0x", "1e"}) {
        bool ok = true;
        (void)decodeFloat(bad, &s, ok);
        EXPECT_FALSE(ok) << bad << " has no value to round to and must stay loud";
    }
}

// ★★ THE PREDICATE IS NOW A NORMALIZER, NOT A GATE, and this pins the reason it
// survived the change. C23 7.12¶6 makes `HUGE_VAL` a positive `double` constant
// and NOT necessarily an infinity — only F.10¶2 pins it to one, and only on an
// IEC 60559 implementation. Passing `strtod`'s return through unexamined would
// therefore hand back a large FINITE value on such a host as though the source
// had named it: the one silent wrong answer this direction can produce. So the
// decoded overflow must be an infinity by CONSTRUCTION, not by inheritance.
TEST(NumberDecode, FloatOverflowIsANormalizedInfinityNotWhateverStrtodReturned) {
    auto const s = cStyle();
    bool ok = false;
    double const d = decodeFloat("1e400", &s, ok);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(std::isinf(d)) << "must be a true infinity, not merely >= HUGE_VAL";
    EXPECT_EQ(bitsOf(d), bitsOf(std::numeric_limits<double>::infinity()));
    bool okNeg = false;
    double const n = decodeFloat("-1e400", &s, okNeg);
    ASSERT_TRUE(okNeg);
    EXPECT_TRUE(std::isinf(n));
    EXPECT_TRUE(std::signbit(n)) << "the sign must survive the normalization";
}

// ★★ THE PREDICATE ITSELF, pinned APART FROM `strtod` — because the errno is the
// weaker half of the pair and must not be the deciding one. C23 7.24.1.5¶13
// makes ERANGE-on-underflow IMPLEMENTATION-DEFINED, and that is not theoretical:
// ✔MEASURED 2026-09-02 on ONE Windows host, two strtod implementations disagreed
// about `1e-320` — mingw-w64 gcc 13.2.0's C `strtod` left errno 0 while the same
// toolchain's C++ `std::strtod` set ERANGE (glibc sets it too). A test that only
// fed `strtod` real text could therefore pass on this host for a reason that
// does not hold on the next one. These rows state the verdict for BOTH errno
// states directly, so the decoder is pinned to the VALUE and not to the libc.
TEST(NumberDecode, FloatOverflowVerdictReadsTheValueNotTheErrno) {
    double const subnormal = 1e-320;
    double const inf       = HUGE_VAL;
    ASSERT_NE(bitsOf(subnormal), 0u) << "the fixture's own subnormal was flushed";

    // ERANGE set: only the ±HUGE_VAL return is an overflow.
    EXPECT_TRUE(detail::strtodOverflowed(ERANGE, inf));
    EXPECT_TRUE(detail::strtodOverflowed(ERANGE, -inf));
    EXPECT_FALSE(detail::strtodOverflowed(ERANGE, subnormal));
    EXPECT_FALSE(detail::strtodOverflowed(ERANGE, -subnormal));
    EXPECT_FALSE(detail::strtodOverflowed(ERANGE, 0.0));
    EXPECT_FALSE(detail::strtodOverflowed(ERANGE, -0.0));

    // ERANGE NOT set: nothing is an overflow, whatever the value. This is the
    // host whose strtod reports underflow silently — the verdict must not move.
    EXPECT_FALSE(detail::strtodOverflowed(0, subnormal));
    EXPECT_FALSE(detail::strtodOverflowed(0, 0.0));
    EXPECT_FALSE(detail::strtodOverflowed(0, 1.0));
    // …and an infinity that arrived without ERANGE is `strtod("inf")`, not an
    // overflow. Unchanged from before the fix, deliberately.
    EXPECT_FALSE(detail::strtodOverflowed(0, inf));
}

// The side of the boundary that was ALREADY correct must not move. The smallest
// normal sets no ERANGE, so it always decoded — it is the control that says the
// fix touched only the arm it was aimed at.
TEST(NumberDecode, FloatSmallestNormalAndOrdinaryValuesAreUnmoved) {
    expectF64("2.2250738585072014e-308", 0x0010000000000000ull, "DBL_MIN");
    expectF64("0.1", 0x3FB999999999999Aull, "gcc/clang/MSVC: 9a 99 99 99 99 99 b9 3f");
    expectF64("1.0", 0x3FF0000000000000ull, "exact");
    expectF64("0.0", 0x0000000000000000ull, "exact");
    expectF64("-0.0", 0x8000000000000000ull, "negative zero survives");
    expectF64("1e-300", 0x01A56E1FC2F8F359ull, "an ordinary tiny NORMAL");
    expectF64("0x1.fp3", 0x402F000000000000ull, "hex-float door unchanged: 15.5");
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

// ★ A VALUE THE HOST `double` PATH CANNOT CARRY AT THIS PRECISION. `1e-320` is
// SUBNORMAL in binary64 — barely 7 significand bits left — and perfectly NORMAL
// in F80/F128, where it keeps all 64/113.
//
// ⚠ THIS TEST'S PREMISE CHANGED UNDER IT, and the change is the point. It used
// to assert `EXPECT_FALSE(hostOk)` — "the host double path must still refuse it
// (ERANGE)" — and read that refusal as the proof the wide decode was TARGET
// precision rather than a host facility.
// D-C-DECODEFLOAT-TREATS-UNDERFLOW-AS-FATAL made that refusal go away, because
// it was a defect: `1e-320` is an ordinary representable `double`. A refusal is
// a WEAK oracle in any case — it
// proves only that the host path did nothing. The claim is now stated the strong
// way instead: the host path ACCEPTS it, and the value it accepts is a DIFFERENT
// number from the one the wide path decodes, which no host facility could give.
TEST(NumberDecodeWide, SubnormalInBinary64IsNormalAtTargetPrecision) {
    auto const s = cStyle();
    bool hostOk = false;
    double const hostD = decodeFloat("1e-320L", &s, hostOk);
    ASSERT_TRUE(hostOk) << "the host double path now DECODES it — the binary64 "
                           "subnormal is a representable value, not an error";
    EXPECT_EQ(bitsOf(hostD), 0x00000000000007E8ull)
        << "and it is the binary64 subnormal all four references emit";
    for (auto kind : {TypeKind::F80, TypeKind::F128}) {
        bool wideOk = false;
        auto const wide = decodeFloatWide("1e-320L", &s, kind, wideOk);
        ASSERT_TRUE(wideOk);
        ASSERT_TRUE(wide.has_value());
        EXPECT_FALSE(*wide == WideFloatValue::fromDouble(hostD, kind))
            << "the target-precision decode must DIFFER from the widened host "
               "double — 1e-320 keeps ~7 significand bits in binary64 and all "
               "64/113 at target precision, so equality would mean the wide "
               "leaf is still host-rounded";
    }
    expectDecodes("1e-320L", TypeKind::F80,
                  {0xFD00B897478238D1ull, 0x3BD7ull},
                  "gcc 13.3.0 + clang 18.1.3: d1 38 82 47 97 b8 00 fd d7 3b");
    expectDecodes("1e-320L", TypeKind::F128,
                  {0x71A1124161312AAAull, 0x3BD7FA01712E8F04ull},
                  "aarch64 gcc 13.3.0");
}

// Zero and the loud contract. `ok` keeps `decodeFloat`'s meaning exactly: a body
// the grammar cannot FULLY consume, a kind the wide kernel does not realize, a
// literal carrying more significant digits than the exact kernel will decide, or
// an UNDERFLOW to subnormal all leave it false — never a silent zero, never a
// silently truncated value.
//
// ⚠ THIS COMMENT USED TO SAY "a value outside the target's NORMAL range", which
// covered both ends with one phrase and went false the moment the two ends
// stopped agreeing (D-C-FLOAT-LITERAL-OVERFLOW-REFUSED-INSTEAD-OF-YIELDING-INFINITY).
// An overflow HAS a correctly-rounded value — the signed infinity — and now
// returns it; the other three refusals are all cases where there is no value to
// return at all. That is the line, and it is not the NORMAL range.
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

    // ⚠ ONLY the UNDERFLOW end refuses now. The overflow end used to sit right
    // here and is D-C-FLOAT-LITERAL-OVERFLOW-REFUSED-INSTEAD-OF-YIELDING-INFINITY's
    // — see NumberDecodeWide.OverflowDecodesToInfinityOnBothWideKinds below. The
    // subnormal end stays loud and stays OPEN as
    // D-CSUBSET-LONG-DOUBLE-CONSTFOLD-SUBNORMAL-RESULT, which owns the wide
    // subnormal question for literals and fold RESULTS together.
    ok = true;
    EXPECT_FALSE(decodeFloatWide("1e-5000L", &s, TypeKind::F80, ok).has_value());
    EXPECT_FALSE(ok) << "a subnormal RESULT is the fail-loud verdict roundNormal "
                        "already carries for add/sub/mul/div";

    // A WORK bound is not a range verdict, and must stay a refusal: more
    // significant digits than the exact kernel will decide has no
    // correctly-rounded answer to return, unlike an overflow, which has one.
    // (Decade 1, so the ±5000 band lets it through to the digit bound.)
    ok = true;
    // ⚠ NO DIGIT SEPARATOR IN THAT COUNT, deliberately. ✔MEASURED 2026-09-02:
    // written `std::string(20'001, '3')` this line turns `wall_clock_in_tests_guard`
    // RED, and `std::string(20001, '3')` is green — same value, same line, same
    // everything else. That guard strips string and CHARACTER literals before
    // matching, and a C++14 digit separator beside a real char literal gives the
    // shared stripper two ways to pair the quotes. Reported, not worked around
    // elsewhere: the guard lives in `scripts/`, outside this lane's file set, and
    // it fails toward NOISY here rather than toward clean.
    std::string const tooManyDigits = "1." + std::string(20001, '3') + "L";
    EXPECT_FALSE(decodeFloatWide(tooManyDigits, &s, TypeKind::F80, ok).has_value());
    EXPECT_FALSE(ok) << "a literal the exact kernel will not decide must stay loud";
}

// ★★ D-C-FLOAT-LITERAL-OVERFLOW-REFUSED-INSTEAD-OF-YIELDING-INFINITY, the wide
// half. `roundNormal` was ALREADY returning `infinity(k, sign)` above `kMaxExp`
// — `WideFloatValue`'s own header says "Overflow → infinity (IN scope)" — and
// the decoder threw it away with an `isInfinity()` test. ✔MEASURED 2026-09-02
// WITH THAT DISCARD STILL IN PLACE: `static const long double v = 1e3000L *
// 1e3000L;` compiled and emitted the x87-80 +∞ while the LITERAL `1e5000L` was
// refused `out of range / undecodable`. The fold reaches `roundNormal` directly;
// only the literal was routed through the discard. The fix is a deletion.
//
// ⚠ THE FIRST TRY AT THAT MEASUREMENT USED `1e2000L * 1e2000L` AND WENT RED OVER
// CORRECT BEHAVIOUR: it is 1e4000, and BOTH wide formats carry a 15-bit exponent
// and top out near 1.19e4932, so nothing overflowed. `1e4000L` is an ordinary
// FINITE wide value — pinned as such below — and only ≥ ~1.19e4932 overflows.
//
// ✔MEASURED 2026-09-02, gcc 13.3.0 and clang 18.1.3 probed SEPARATELY and
// agreeing byte-for-byte, reading the emitted static initializer: x86_64
// `long double` for F80 and `__float128` for F128.
TEST(NumberDecodeWide, OverflowDecodesToInfinityOnBothWideKinds) {
    expectDecodes("1e5000L", TypeKind::F80,
                  {0x8000000000000000ull, 0x7FFFull},
                  "gcc 13.3.0 + clang 18.1.3, x86_64 long double 1e5000L");
    expectDecodes("1e5000L", TypeKind::F128,
                  {0x0000000000000000ull, 0x7FFF000000000000ull},
                  "gcc 13.3.0 + clang 18.1.3, __float128 1e5000Q");
    // A hex float overflows through `roundExactInteger`'s binary arm, which
    // never touches a power of ten — a separate path to the same verdict.
    expectDecodes("0x1p+99999L", TypeKind::F80,
                  {0x8000000000000000ull, 0x7FFFull},
                  "gcc + clang, 0x1p+99999L");
    expectDecodes("0x1p+99999L", TypeKind::F128,
                  {0x0000000000000000ull, 0x7FFF000000000000ull},
                  "gcc + clang, 0x1p+99999Q");
}

// ★ THE PRE-FILTER MUST GIVE THE ANSWER THE EXACT PATH WOULD, and these two
// literals are chosen so that each takes a DIFFERENT route to it. `1e5000L` has
// decade 5001 and is decided by the ±5000 magnitude short-circuit without ever
// building the integer; `1e4940L` has decade 4941, so it is built exactly, all
// 16412 bits of it, and overflows inside `roundNormal`. They must agree — before
// this row they agreed on REFUSING, which is how one `return std::nullopt`
// serving both ends went unnoticed.
TEST(NumberDecodeWide, TheMagnitudeShortCircuitAgreesWithTheExactPath) {
    auto const s = cStyle();
    for (auto kind : {TypeKind::F80, TypeKind::F128}) {
        bool shortOk = false, exactOk = false;
        auto const viaShort = decodeFloatWide("1e5000L", &s, kind, shortOk);
        auto const viaExact = decodeFloatWide("1e4940L", &s, kind, exactOk);
        ASSERT_TRUE(shortOk);
        ASSERT_TRUE(exactOk) << "1e4940L is inside the band and must be BUILT";
        ASSERT_TRUE(viaShort.has_value());
        ASSERT_TRUE(viaExact.has_value());
        EXPECT_TRUE(viaShort->isInfinity());
        EXPECT_TRUE(viaExact->isInfinity());
        EXPECT_EQ(packOf(*viaShort).lo, packOf(*viaExact).lo);
        EXPECT_EQ(packOf(*viaShort).hi, packOf(*viaExact).hi);
    }
    // ⓘ `1e4940L` overflowing is itself ✔MEASURED, not assumed: gcc and clang
    // both emit +∞ for it at BOTH widths, because F80 and F128 share a 15-bit
    // exponent and so share a maximum near 1.19e4932.
    expectDecodes("1e4940L", TypeKind::F80,
                  {0x8000000000000000ull, 0x7FFFull}, "gcc + clang, 1e4940L");
    expectDecodes("1e4940L", TypeKind::F128,
                  {0x0000000000000000ull, 0x7FFF000000000000ull},
                  "gcc + clang, 1e4940Q");
}

// ★★ THE DIVISION ARM, which no shorter literal can reach. `roundExactQuotient`
// runs only when the text has MORE fractional digits than its exponent, so an
// overflowing literal that divides must carry its magnitude in its DIGITS: `1`
// followed by 4941 zeros and `.5`. The arm had its own `expLL > 100000` guard
// and its own `isInfinity()` discard, and a fix that touched only the sibling
// would have left it refusing — N transforms on one value are N defects.
// ✔MEASURED 2026-09-02: gcc 13.3.0 and clang 18.1.3, handed this exact 4944-
// character literal, both emit +∞ at both widths.
TEST(NumberDecodeWide, TheQuotientArmOverflowsToInfinityToo) {
    std::string const lit = "1" + std::string(4941, '0') + ".5L";
    expectDecodes(lit.c_str(), TypeKind::F80,
                  {0x8000000000000000ull, 0x7FFFull},
                  "gcc + clang, 1<4941 zeros>.5L");
    expectDecodes(lit.c_str(), TypeKind::F128,
                  {0x0000000000000000ull, 0x7FFF000000000000ull},
                  "gcc + clang, 1<4941 zeros>.5Q");
}

// ⚠ THE OVER-CORRECTION'S TRAP, from BOTH sides. The danger of this direction is
// accepting a value the source never named, so the largest FINITE value of each
// wide format is pinned beside the overflow — if the change had started
// saturating instead of rounding, or had begun calling finite values infinite,
// these move. ✔MEASURED: LDBL_MAX emits `ffffffffffffffff / 7ffe`, and `1e400L`
// — which gcc, clang and mingw-gcc accept with NO warning at all, because it is
// an ordinary x87-80 value — emits `da763fc8cb9ff9e6 / 452f`.
TEST(NumberDecodeWide, LargeFiniteWideValuesAreUnmovedByTheOverflowChange) {
    expectDecodes("1e400L", TypeKind::F80,
                  {0xDA763FC8CB9FF9E6ull, 0x452Full},
                  "gcc + clang, x86_64 long double 1e400L — finite, unwarned");
    expectDecodes("1e400L", TypeKind::F128,
                  {0xF3CB1CCF26FBC178ull, 0x452FB4EC7F91973Full},
                  "gcc + clang, __float128 1e400Q");
    expectDecodes("1.18973149535723176502e4932L", TypeKind::F80,
                  {0xFFFFFFFFFFFFFFFFull, 0x7FFEull},
                  "gcc + clang, LDBL_MAX — the last finite x87-80 value");
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

// ─────────────────────────────────────────────────────────────────────────────
// `decodeFloatLiteralAtKind` — THE ONE DISPATCH (P54 lane `fw`,
// D-C-FLOAT-LITERAL-OVERFLOW-REFUSED-INSTEAD-OF-YIELDING-INFINITY).
//
// The wide/narrow split used to be written out LONGHAND at each phase that
// decodes a float literal, and D-CSUBSET-LONG-DOUBLE-LITERAL-DECODE-PRECISION's
// own comment already named the hazard: "two leaves decode float literals, so
// fixing one would leave the other wrong differently". These pins are on the
// chokepoint that replaced both copies, and on the predicate the semantic
// tier's range warning keys on.
// ─────────────────────────────────────────────────────────────────────────────

// A literal's value at a HOST-BACKED kind must be the value NARROWED to that
// kind's width, not the host `double` the decoder produced. This is the property
// that makes `0.1f != 0.1` fold the way all four references fold it, and it is
// the one a caller most easily forgets when it writes the dispatch itself.
TEST(NumberDecodeAtKind, HostBackedKindsAreNarrowedToTheirOwnWidth) {
    auto const f32 = decodeFloatLiteralAtKind("0.1", nullptr, TypeKind::F32);
    ASSERT_TRUE(f32.ok);
    EXPECT_FALSE(f32.wide.has_value());
    EXPECT_EQ(f32.narrow, static_cast<double>(0.1f));
    EXPECT_NE(f32.narrow, 0.1);   // the whole point: narrowing CHANGED the value

    auto const f64 = decodeFloatLiteralAtKind("0.1", nullptr, TypeKind::F64);
    ASSERT_TRUE(f64.ok);
    EXPECT_FALSE(f64.wide.has_value());
    EXPECT_EQ(f64.narrow, 0.1);   // F64 is identity
}

// A kind `WideFloatValue` realizes takes the wide arm and lands in `wide`, never
// in `narrow` — the arms are exclusive and a caller reads whichever is set.
TEST(NumberDecodeAtKind, WideKindsTakeTheWideArm) {
    for (TypeKind const k : {TypeKind::F80, TypeKind::F128}) {
        auto const v = decodeFloatLiteralAtKind("0.1", nullptr, k);
        ASSERT_TRUE(v.ok);
        ASSERT_TRUE(v.wide.has_value());
        EXPECT_FALSE(v.wide->isInfinity());
    }
}

// ★★ `roundedToInfinity()` IS THE RANGE VERDICT the semantic tier's warning
// keys on, and it must agree with the value that actually ships. THREE doors
// reach an infinity and each is separate code: strtod's ERANGE, the F32
// NARROWING (where nothing overflowed in the decode at all), and the wide
// bignum kernel. The F32 row is the one a decode-side-only implementation
// misses.
TEST(NumberDecodeAtKind, RoundedToInfinityAgreesWithTheValueOnEveryDoor) {
    struct Row { char const* text; TypeKind kind; bool infinite; char const* why; };
    Row const rows[] = {
        {"1e400",   TypeKind::F64,  true,  "strtod ERANGE, the row's own literal"},
        {"1e309",   TypeKind::F64,  true,  "the first power of ten past DBL_MAX"},
        {"0x1p+99999", TypeKind::F64, true, "a hex float through the same door"},
        {"1.7976931348623157e308", TypeKind::F64, false, "DBL_MAX itself"},
        {"1.7976931348623159e308", TypeKind::F64, true,  "one ulp past DBL_MAX"},
        {"1e-320",  TypeKind::F64,  false, "a SUBNORMAL is an ordinary value"},
        {"1e-330",  TypeKind::F64,  false, "flush to zero is not overflow"},
        // The narrowing door: 1e40 is a perfectly ordinary `double`.
        {"1e40",    TypeKind::F32,  true,  "finite as a double, +inf as a float"},
        {"1e40",    TypeKind::F64,  false, "the SAME text, one kind wider"},
        {"3.40282347e38", TypeKind::F32, false, "FLT_MAX itself"},
        // The wide doors: the pre-filter, the exact path and the division arm.
        {"1e5000",  TypeKind::F80,  true,  "past the magnitude pre-filter"},
        {"1e4940",  TypeKind::F80,  true,  "inside the band, overflows in roundNormal"},
        {"1.5e4940", TypeKind::F80, true,  "the DIVISION arm"},
        {"1e4000",  TypeKind::F80,  false, "FINITE — the 15-bit exponent tops near 1.19e4932"},
        {"1e400",   TypeKind::F80,  false, "FINITE on a wide axis; it is NOT on F64"},
        {"1e5000",  TypeKind::F128, true,  "binary128 shares the 15-bit exponent"},
        {"1e400",   TypeKind::F128, false, "FINITE"},
    };
    for (Row const& r : rows) {
        auto const v = decodeFloatLiteralAtKind(r.text, nullptr, r.kind);
        ASSERT_TRUE(v.ok) << r.text << " @" << static_cast<int>(r.kind)
                          << " — " << r.why;
        EXPECT_EQ(v.roundedToInfinity(), r.infinite)
            << r.text << " @" << static_cast<int>(r.kind) << " — " << r.why;
        // The verdict must be READ OFF THE VALUE, never computed a second way.
        bool const valueIsInfinite = v.wide.has_value()
                                         ? v.wide->isInfinity()
                                         : std::isinf(v.narrow);
        EXPECT_EQ(v.roundedToInfinity(), valueIsInfinite) << r.text;
    }
}

// A body the grammar cannot fully consume stays a LOUD refusal on both arms, and
// `roundedToInfinity()` is FALSE for it — a failed decode has no verdict to give
// and must never be mistaken for an in-range one OR for an overflow.
TEST(NumberDecodeAtKind, ALoudRefusalCarriesNoRangeVerdict) {
    for (TypeKind const k : {TypeKind::F32, TypeKind::F64,
                             TypeKind::F80, TypeKind::F128}) {
        for (char const* bad : {"1.5^3", "", "1e400zzz"}) {
            auto const v = decodeFloatLiteralAtKind(bad, nullptr, k);
            EXPECT_FALSE(v.ok) << bad << " @" << static_cast<int>(k);
            EXPECT_FALSE(v.roundedToInfinity()) << bad;
        }
    }
}

// ⚠⚠ THE BOUNDARY OF THE CLAIM "an infinity means an overflow", MEASURED
// RATHER THAN ASSUMED — because the range warning is built on it.
//
// For every body a float LITERAL can have, the claim holds: a C pp-number must
// begin with a digit or a `.`, so the tokenizer never classifies `inf` as a
// float literal, and a word GLUED to digits (`1inf`) fails the whole-body
// consumption check. Those are the refusals below.
//
// ⚠ BUT `strtod` ITSELF ACCEPTS THE WORDS, and a DIRECT call with such a string
// therefore decodes — so the claim is a property of the CALLER having a literal,
// not of this function in isolation. Recorded, and pinned, because the sentence
// is otherwise the kind that quietly goes false: the ONE non-literal caller of
// the underlying `decodeFloat` (`shipped_lib_descriptor.cpp`) matches its own
// "inf"/"+inf"/"-inf" tokens first and then refuses ANY infinity or NaN the
// decoder returns, so it never depends on this and never emits a source
// diagnostic — it has no source span to emit one with.
//
// ⚠ AND THE TWO DOORS DIVERGE HERE, which is the only body for which they do.
// `decodeFloatWide`'s docblock says the ACCEPT/REFUSE line on a literal's SHAPE
// is shared "so no literal is parseable on one axis and unparseable on the
// other" — true of every literal, and NOT of these words: `parseFloatLiteralBody`
// is a digits-and-exponent grammar and rejects them, while `strtod` takes them.
// Pinned as-measured rather than papered over; no literal can reach it, and
// changing `decodeFloat`'s grammar contract to match is a behaviour change no
// measurement asks for (the descriptor reader's outcome is identical either way).
TEST(NumberDecodeAtKind, AnInfinityFromALITERALCanOnlyBeAnOverflow) {
    for (char const* glued : {"1inf", "1e400inf", "1nan", "1.5^3"}) {
        auto const v = decodeFloatLiteralAtKind(glued, nullptr, TypeKind::F64);
        EXPECT_FALSE(v.ok) << glued
            << " must not decode — if it did, roundedToInfinity() would report "
               "an overflow that never happened";
    }
    // The bare words: accepted by the host-backed door, refused by the wide one.
    for (char const* word : {"inf", "infinity", "nan"}) {
        auto const host = decodeFloatLiteralAtKind(word, nullptr, TypeKind::F64);
        EXPECT_TRUE(host.ok) << word << " — strtod consumes it whole";
        auto const wide = decodeFloatLiteralAtKind(word, nullptr, TypeKind::F80);
        EXPECT_FALSE(wide.ok)
            << word << " — the wide grammar has no word form; this asymmetry is "
               "unreachable from a float literal and is pinned, not relied on";
    }
}
