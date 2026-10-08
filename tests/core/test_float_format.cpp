// C's floating characteristics from ONE owner per representation (P69, lane lm: M3 of
// D-C-FLOAT-H-AND-FLOAT-PREDEFINES-MISSING) — core/types/float_format.hpp.
//
// The table states four primary facts per floating TypeKind; every C characteristic is
// computed from them. This file pins the computation against three independent answers:
//   (1) the values the C standard and IEC 60559 give for each format, which are also the
//       references' `-dM` predefines (gcc 13.3.0 / clang 18.1.3 / Apple clang 21: e.g.
//       __FLT_DIG__ 6, __DBL_MAX_10_EXP__ 308, __LDBL_MANT_DIG__ 64 on x86_64 and 113 on
//       aarch64 Linux, __LDBL_MIN_EXP__ (-16381));
//   (2) the HOST's own std::numeric_limits<float> and <double> — a C++ implementation's
//       statement of binary32 and binary64, measured where the test runs;
//   (3) the hex spellings parsed back by the host's strtof/strtod, which must yield FLT_MAX,
//       FLT_MIN, FLT_EPSILON, FLT_TRUE_MIN and their double twins exactly.
// And it pins that the former owners now DERIVE: WideFloatValue's precision, widths and
// exponent constants, and number_decode's floatKindInfo.
//
// RED-ON-DISABLE: give the F80 row a precision of 63 and (1) reds on every LDBL-shaped
// characteristic and on WideFloatValue::significandBits(F80) — one edit, every consumer.

#include "core/types/float_format.hpp"
#include "core/types/number_decode.hpp"
#include "core/types/wide_float_value.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <string>

using namespace dss;

namespace {

struct Expected {
    TypeKind    kind;
    int         storage, mantDig, dig, decimalDig;
    int         minExp, min10Exp, maxExp, max10Exp;
    char const* maxHex;
    char const* minHex;
    char const* epsilonHex;
    char const* trueMinHex;
};

// C23 5.2.5.3.3 / IEC 60559 per format; each value is also the references' predefine.
constexpr Expected kExpected[] = {
    {TypeKind::F16, 16, 11, 3, 5, -13, -4, 16, 4,
     "0x1.ffcp+15", "0x1p-14", "0x1p-10", "0x1p-24"},
    {TypeKind::F32, 32, 24, 6, 9, -125, -37, 128, 38,
     "0x1.fffffep+127", "0x1p-126", "0x1p-23", "0x1p-149"},
    {TypeKind::F64, 64, 53, 15, 17, -1021, -307, 1024, 308,
     "0x1.fffffffffffffp+1023", "0x1p-1022", "0x1p-52", "0x1p-1074"},
    {TypeKind::F80, 80, 64, 18, 21, -16381, -4931, 16384, 4932,
     "0x1.fffffffffffffffep+16383", "0x1p-16382", "0x1p-63", "0x1p-16445"},
    {TypeKind::F128, 128, 113, 33, 36, -16381, -4931, 16384, 4932,
     "0x1.ffffffffffffffffffffffffffffp+16383", "0x1p-16382", "0x1p-112", "0x1p-16494"},
};

}  // namespace

// (1) Every characteristic of every row, computed, against the standard's values.
TEST(FloatFormat, EveryCharacteristicIsComputedFromThePrimaryFacts) {
    ASSERT_EQ(std::size(kExpected), kFloatFormats.size()) << "a row the expectations do not cover";
    for (Expected const& e : kExpected) {
        FloatFormat const* const f = floatFormatOf(e.kind);
        ASSERT_NE(f, nullptr) << static_cast<int>(e.kind);
        SCOPED_TRACE(e.mantDig);
        EXPECT_EQ(storageBits(*f), e.storage);
        EXPECT_EQ(f->precision, e.mantDig);
        EXPECT_EQ(decimalDigits(*f), e.dig);
        EXPECT_EQ(decimalDigitsRoundTrip(*f), e.decimalDig);
        EXPECT_EQ(minExponent(*f), e.minExp);
        EXPECT_EQ(min10Exponent(*f), e.min10Exp);
        EXPECT_EQ(maxExponent(*f), e.maxExp);
        EXPECT_EQ(max10Exponent(*f), e.max10Exp);
        EXPECT_EQ(maxValueHex(*f), e.maxHex);
        EXPECT_EQ(minNormalHex(*f), e.minHex);
        EXPECT_EQ(epsilonHex(*f), e.epsilonHex);
        EXPECT_EQ(trueMinHex(*f), e.trueMinHex);
    }
    EXPECT_EQ(floatFormatOf(TypeKind::I32), nullptr) << "an integer kind has no floating row";
}

// The rational log10(2) is safe: no characteristic sits within the approximation's error
// of an integer, for any row (else a floor or ceil could land one off).
TEST(FloatFormat, NoDecimalCharacteristicSitsOnAnIntegerBoundary) {
    using float_format_detail::kLog10Of2Denominator;
    using float_format_detail::kLog10Of2Numerator;
    // The approximation's error times the largest |n| used, in units of the denominator.
    constexpr std::int64_t kMargin = 1000000;   // 1e-5 of a unit, far above the < 7e-8 error
    auto distanceToInteger = [](std::int64_t n) {
        std::int64_t r = (n * kLog10Of2Numerator) % kLog10Of2Denominator;
        if (r < 0) r += kLog10Of2Denominator;
        return std::min(r, kLog10Of2Denominator - r);
    };
    for (FloatFormat const& f : kFloatFormats) {
        SCOPED_TRACE(f.precision);
        EXPECT_GT(distanceToInteger(f.precision - 1), kMargin);
        EXPECT_GT(distanceToInteger(f.precision), kMargin);
        EXPECT_GT(distanceToInteger(minExponent(f) - 1), kMargin);
        EXPECT_GT(distanceToInteger(maxExponent(f)), kMargin);
    }
}

// (2) The host's own statement of binary32 and binary64.
TEST(FloatFormat, Binary32AndBinary64AgreeWithTheHostsNumericLimits) {
    FloatFormat const& f32 = *floatFormatOf(TypeKind::F32);
    FloatFormat const& f64 = *floatFormatOf(TypeKind::F64);
    using F = std::numeric_limits<float>;
    using D = std::numeric_limits<double>;
    static_assert(F::is_iec559 && D::is_iec559, "the host's float/double are IEC 60559 binary32/64");
    EXPECT_EQ(f32.precision, F::digits);
    EXPECT_EQ(decimalDigits(f32), F::digits10);
    EXPECT_EQ(decimalDigitsRoundTrip(f32), F::max_digits10);
    EXPECT_EQ(minExponent(f32), F::min_exponent);
    EXPECT_EQ(maxExponent(f32), F::max_exponent);
    EXPECT_EQ(min10Exponent(f32), F::min_exponent10);
    EXPECT_EQ(max10Exponent(f32), F::max_exponent10);
    EXPECT_EQ(f64.precision, D::digits);
    EXPECT_EQ(decimalDigits(f64), D::digits10);
    EXPECT_EQ(decimalDigitsRoundTrip(f64), D::max_digits10);
    EXPECT_EQ(minExponent(f64), D::min_exponent);
    EXPECT_EQ(maxExponent(f64), D::max_exponent);
    EXPECT_EQ(min10Exponent(f64), D::min_exponent10);
    EXPECT_EQ(max10Exponent(f64), D::max_exponent10);
}

// (3) The spellings ARE the values.
TEST(FloatFormat, TheHexSpellingsParseToTheHostsExtremes) {
    FloatFormat const& f32 = *floatFormatOf(TypeKind::F32);
    FloatFormat const& f64 = *floatFormatOf(TypeKind::F64);
    EXPECT_EQ(std::strtof(maxValueHex(f32).c_str(), nullptr), FLT_MAX);
    EXPECT_EQ(std::strtof(minNormalHex(f32).c_str(), nullptr), FLT_MIN);
    EXPECT_EQ(std::strtof(epsilonHex(f32).c_str(), nullptr), FLT_EPSILON);
    EXPECT_EQ(std::strtof(trueMinHex(f32).c_str(), nullptr), std::numeric_limits<float>::denorm_min());
    EXPECT_EQ(std::strtod(maxValueHex(f64).c_str(), nullptr), DBL_MAX);
    EXPECT_EQ(std::strtod(minNormalHex(f64).c_str(), nullptr), DBL_MIN);
    EXPECT_EQ(std::strtod(epsilonHex(f64).c_str(), nullptr), DBL_EPSILON);
    EXPECT_EQ(std::strtod(trueMinHex(f64).c_str(), nullptr), std::numeric_limits<double>::denorm_min());
}

// The former owners derive from the table.
TEST(FloatFormat, TheFormerOwnersDeriveFromTheTable) {
    for (TypeKind const k : {TypeKind::F80, TypeKind::F128}) {
        FloatFormat const& f = *floatFormatOf(k);
        EXPECT_EQ(WideFloatValue::significandBits(k), f.precision);
        auto const width = WideFloatValue::formatBitWidth(k);
        ASSERT_TRUE(width.has_value());
        EXPECT_EQ(*width, static_cast<std::uint32_t>(storageBits(f)));
        EXPECT_EQ(WideFloatValue::kindOfFormatBitWidth(static_cast<std::uint64_t>(storageBits(f))), k);
    }
    for (FloatFormat const& f : kFloatFormats) {
        auto const info = detail::floatKindInfo(f.kind);
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->bits, storageBits(f));
        EXPECT_EQ(info->hostBacked, f.hostBacked);
    }
    EXPECT_FALSE(detail::floatKindInfo(TypeKind::I64).has_value());
}
