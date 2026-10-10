#pragma once

#include "core/types/type_lattice/core_type.hpp"   // TypeKind

#include <array>
#include <cstdint>
#include <string>

// ★★★ C'S FLOATING CHARACTERISTICS — ONE OWNER PER REPRESENTATION
// (P69, lane lm: M3 of D-C-FLOAT-H-AND-FLOAT-PREDEFINES-MISSING).
//
// Every floating TypeKind DSS models is one radix-2 representation, and C asks
// the same questions of each (C23 5.2.5.3.3, the <float.h> characteristics):
// how many significand digits (`MANT_DIG`), which exponent range (`MIN_EXP`,
// `MAX_EXP`), and what follows from those (`DIG`, `DECIMAL_DIG`, `MIN_10_EXP`,
// `MAX_10_EXP`, `MAX`, `MIN`, `EPSILON`, `TRUE_MIN`). The answers used to be
// scattered: `WideFloatValue` restated 64/113 and the 16383 bias,
// `number_decode`'s `floatKindInfo` the widths 16..128, `WideFloatValue`'s
// `kFormatBitWidths` the 80/128 again. This table is the ONE statement; those
// owners now DERIVE from it, and the derived C characteristics below are
// computed from its rows, never typed. A row is keyed by TypeKind — a fact of the
// REPRESENTATION, with no language, arch or format identity in it: which
// representation a C type has on a pair is the target's and format document's
// question (`long double` is F80 on x86_64 ELF/Mach-O, F128 on aarch64 ELF, F64
// on pe and arm64 Mach-O), answered elsewhere.
//
// A row states only PRIMARY facts — the significand's precision, the exponent
// field's width, whether the integer bit is stored, whether a host `double`
// carries every value — and everything else is a function of them, so no two
// numbers in this file can disagree:
//   storage width = 1 (sign) + exponent bits + stored significand bits;
//   bias          = 2^(exponent bits - 1) - 1;
//   C's e_max     = bias + 1, C's e_min = 2 - bias (C's model puts the
//                   significand in [1/2, 1), IEC 60559's in [1, 2): one apart).
// ✔ Pinned by tests/core/test_float_format.cpp against C's own formulas, the C
// standard's floors, the host's std::numeric_limits for float and double, and the
// references' `-dM` predefines (gcc 13.3.0, clang 18.1.3, Apple clang 21; MSVC's
// <float.h>) measured in P69 lane lm's plan.
namespace dss {

struct FloatFormat {
    TypeKind kind;
    int      precision;           // p: significand digits, the integer bit counted (C's MANT_DIG)
    int      exponentBits;        // width of the biased exponent field
    bool     explicitIntegerBit;  // the significand STORES its leading bit (x87 extended)
    bool     hostBacked;          // a host `double` carries every value of the format exactly
};

inline constexpr std::array<FloatFormat, 5> kFloatFormats{{
    //  kind             p    exponent  explicit int bit  host-backed
    {TypeKind::F16,      11,  5,        false,            true },   // IEC 60559 binary16
    {TypeKind::F32,      24,  8,        false,            true },   // IEC 60559 binary32
    {TypeKind::F64,      53,  11,       false,            true },   // IEC 60559 binary64
    {TypeKind::F80,      64,  15,       true,             false},   // x87 80-bit extended
    {TypeKind::F128,     113, 15,       false,            false},   // IEC 60559 binary128
}};

// The row of `k`, or null for a kind that is not floating.
[[nodiscard]] constexpr FloatFormat const* floatFormatOf(TypeKind k) noexcept {
    for (FloatFormat const& f : kFloatFormats) {
        if (f.kind == k) return &f;
    }
    return nullptr;
}

// ── Derived representation facts ───────────────────────────────────────────

// The object's width in bits — the format's own (80 for x87, before padding).
[[nodiscard]] constexpr int storageBits(FloatFormat const& f) noexcept {
    return 1 + f.exponentBits + f.precision - (f.explicitIntegerBit ? 0 : 1);
}

// The exponent field's bias.
[[nodiscard]] constexpr std::int32_t exponentBias(FloatFormat const& f) noexcept {
    return (std::int32_t{1} << (f.exponentBits - 1)) - 1;
}

// IEC 60559's exponent range (significand in [1, 2)): the largest and smallest
// unbiased exponent of a NORMAL number.
[[nodiscard]] constexpr std::int32_t ieeeMaxExponent(FloatFormat const& f) noexcept {
    return exponentBias(f);
}
[[nodiscard]] constexpr std::int32_t ieeeMinExponent(FloatFormat const& f) noexcept {
    return 1 - exponentBias(f);
}

// ── C23 5.2.5.3.3's characteristics, b = 2 ───────────────────────────────────

// C's model exponents (significand in [1/2, 1)): FLT_MAX_EXP, FLT_MIN_EXP.
[[nodiscard]] constexpr std::int32_t maxExponent(FloatFormat const& f) noexcept {
    return ieeeMaxExponent(f) + 1;
}
[[nodiscard]] constexpr std::int32_t minExponent(FloatFormat const& f) noexcept {
    return ieeeMinExponent(f) + 1;
}

namespace float_format_detail {
// log10(2) as an exact rational, 30102999566 / 10^11 (error < 4e-12). Every
// product below stays far inside 64 bits (|n| <= 16495), and the error it leaves
// (< 7e-8 over the widest exponent range) is far smaller than any result's
// distance from an integer — `tests/core/test_float_format.cpp` asserts that
// margin for every row, so a new row cannot sit on a boundary unnoticed.
inline constexpr std::int64_t kLog10Of2Numerator   = 30102999566;
inline constexpr std::int64_t kLog10Of2Denominator = 100000000000;

[[nodiscard]] constexpr std::int64_t floorDiv(std::int64_t a, std::int64_t b) noexcept {
    std::int64_t const q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}
[[nodiscard]] constexpr std::int64_t ceilDiv(std::int64_t a, std::int64_t b) noexcept {
    std::int64_t const q = a / b;
    return (a % b != 0 && a > 0) ? q + 1 : q;
}
// floor(n * log10 2) and ceil(n * log10 2).
[[nodiscard]] constexpr std::int64_t floorLog10Of2Times(std::int64_t n) noexcept {
    return floorDiv(n * kLog10Of2Numerator, kLog10Of2Denominator);
}
[[nodiscard]] constexpr std::int64_t ceilLog10Of2Times(std::int64_t n) noexcept {
    return ceilDiv(n * kLog10Of2Numerator, kLog10Of2Denominator);
}
}  // namespace float_format_detail

// DIG: floor((p - 1) log10 2) — decimal digits that survive a round trip
// through the format.
[[nodiscard]] constexpr int decimalDigits(FloatFormat const& f) noexcept {
    return static_cast<int>(float_format_detail::floorLog10Of2Times(f.precision - 1));
}

// The type's DECIMAL_DIG (FLT_DECIMAL_DIG ...): ceil(1 + p log10 2) — decimal
// digits that round-trip every value of the format.
[[nodiscard]] constexpr int decimalDigitsRoundTrip(FloatFormat const& f) noexcept {
    return 1 + static_cast<int>(float_format_detail::ceilLog10Of2Times(f.precision));
}

// MIN_10_EXP: ceil(log10(2^(e_min - 1))).
[[nodiscard]] constexpr int min10Exponent(FloatFormat const& f) noexcept {
    return static_cast<int>(float_format_detail::ceilLog10Of2Times(minExponent(f) - 1));
}

// MAX_10_EXP: floor(log10((1 - 2^-p) 2^e_max)). The (1 - 2^-p) factor moves the
// logarithm down by less than 2^-p / ln 10, which changes the floor only when
// e_max log10 2 lies that close ABOVE an integer; the test asserts the margin.
[[nodiscard]] constexpr int max10Exponent(FloatFormat const& f) noexcept {
    return static_cast<int>(float_format_detail::floorLog10Of2Times(maxExponent(f)));
}

// The exact values, as C hexadecimal floating constants (no suffix): the largest
// finite (MAX — and NORM_MAX, which equals it for every format here), the
// smallest normal (MIN), the gap above 1 (EPSILON), and the smallest positive
// subnormal (TRUE_MIN). Computed from p and the exponent range, never typed.
[[nodiscard]] inline std::string maxValueHex(FloatFormat const& f) {
    // 1.111...1 (p - 1 fraction bits) x 2^(e_max - 1), the fraction left-aligned
    // in hexadecimal digits.
    int const fractionBits = f.precision - 1;
    std::string digits;
    for (int done = 0; done < fractionBits; done += 4) {
        int const take = fractionBits - done >= 4 ? 4 : fractionBits - done;
        int const nibble = ((1 << take) - 1) << (4 - take);
        digits.push_back("0123456789abcdef"[nibble]);
    }
    return "0x1." + digits + "p+" + std::to_string(maxExponent(f) - 1);
}
[[nodiscard]] inline std::string minNormalHex(FloatFormat const& f) {
    return "0x1p" + std::to_string(minExponent(f) - 1);
}
[[nodiscard]] inline std::string epsilonHex(FloatFormat const& f) {
    return "0x1p" + std::to_string(1 - f.precision);
}
[[nodiscard]] inline std::string trueMinHex(FloatFormat const& f) {
    return "0x1p" + std::to_string(minExponent(f) - f.precision);
}

}  // namespace dss
