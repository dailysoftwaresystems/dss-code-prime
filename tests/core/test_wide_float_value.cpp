// LD-3 (D-CSUBSET-LONG-DOUBLE-CONSTFOLD-PRECISION): the host wide-float soft-
// float kernel. Strict exact-BYTE vectors against the gcc-native oracle
// (scratchpad/ld3_oracle_values.md) for F80 (x87 80-bit) AND F128 (IEEE
// binary128) — the INEXACT div cases (20/22, 1/3) are the load-bearing round-
// to-nearest-even proofs (a binary64 fold mis-rounds them). The kernel gets
// NOTHING free from the host FPU here: guard/round/sticky, tie-to-even, and
// carry-out renormalization are all pinned. Plus fromDouble exactness, negate,
// inf/nan/zero, pack round-trip, and toInt64. The
// `__GNUC__ && __x86_64__ && !_MSC_VER`-guarded block adds a differential
// against gcc-native `long double`/`__float128` — inert on the MSVC gate (the
// hardcoded oracle-byte pins are the MSVC-side correctness check), live only on
// the WSL x86_64 leg the parent runs.

#include "core/types/wide_float_value.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

using dss::WideFloatValue;
using TypeKind = dss::TypeKind;

namespace {

// The 16-byte on-disk slot: pack() as {lo, hi} written lo-then-hi little-endian
// (exactly what asm.cpp's appendWideFloatBits emits). For F80 the low 10 bytes
// are significant + 6 zero pad; for F128 all 16 are significant.
std::array<std::uint8_t, 16> bytesOf(WideFloatValue const& v) {
    WideFloatValue::Packed const p = v.pack();
    std::array<std::uint8_t, 16> out{};
    for (int i = 0; i < 8; ++i) out[i]     = static_cast<std::uint8_t>((p.lo >> (8 * i)) & 0xFFu);
    for (int i = 0; i < 8; ++i) out[8 + i] = static_cast<std::uint8_t>((p.hi >> (8 * i)) & 0xFFu);
    return out;
}

std::string hex(std::array<std::uint8_t, 16> const& b) {
    static char const* d = "0123456789abcdef";
    std::string s;
    for (std::uint8_t x : b) { s += d[x >> 4]; s += d[x & 0xF]; s += ' '; }
    return s;
}

// Build the expected 16-byte slot from the oracle's significant bytes (F80: 10,
// zero-padded to 16; F128: 16).
std::array<std::uint8_t, 16> expect(std::initializer_list<std::uint8_t> sig) {
    std::array<std::uint8_t, 16> out{};
    std::size_t i = 0;
    for (std::uint8_t x : sig) out[i++] = x;
    return out;
}

WideFloatValue f80(double d)  { return WideFloatValue::fromDouble(d, TypeKind::F80); }
WideFloatValue f128(double d) { return WideFloatValue::fromDouble(d, TypeKind::F128); }

void expectBytes(WideFloatValue const& v, std::array<std::uint8_t, 16> const& want,
                 char const* label) {
    auto const got = bytesOf(v);
    EXPECT_EQ(got, want) << label << "\n  got:    " << hex(got)
                         << "\n  expect: " << hex(want);
}

}  // namespace

// ── F80 exact arithmetic (20+22, 22-20, 20*22) → oracle bytes ────────────────
TEST(WideFloatValue, F80ExactArithmeticMatchesOracle) {
    auto add = WideFloatValue::add(f80(20.0), f80(22.0));
    ASSERT_TRUE(add.has_value());
    expectBytes(*add, expect({0,0,0,0,0,0,0,0xa8,0x04,0x40}), "F80 20+22 = 42");

    auto sub = WideFloatValue::sub(f80(22.0), f80(20.0));
    ASSERT_TRUE(sub.has_value());
    expectBytes(*sub, expect({0,0,0,0,0,0,0,0x80,0x00,0x40}), "F80 22-20 = 2");

    auto mul = WideFloatValue::mul(f80(20.0), f80(22.0));
    ASSERT_TRUE(mul.has_value());
    expectBytes(*mul, expect({0,0,0,0,0,0,0,0xdc,0x07,0x40}), "F80 20*22 = 440");
}

// ★ F80 INEXACT div (20/22, 1/3) → the round-to-nearest-even proofs. 1/3 rounds
// UP (…aa → …ab); a binary64 fold would produce different bytes.
TEST(WideFloatValue, F80InexactDivRoundToNearestEvenMatchesOracle) {
    auto d1 = WideFloatValue::div(f80(20.0), f80(22.0));
    ASSERT_TRUE(d1.has_value());
    expectBytes(*d1, expect({0x2f,0xba,0xe8,0xa2,0x8b,0x2e,0xba,0xe8,0xfe,0x3f}),
                "F80 20/22 (INEXACT, round-to-nearest)");

    auto d2 = WideFloatValue::div(f80(1.0), f80(3.0));
    ASSERT_TRUE(d2.has_value());
    expectBytes(*d2, expect({0xab,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xfd,0x3f}),
                "F80 1/3 (INEXACT, rounds UP to ..ab)");
}

// ── F128 exact arithmetic → oracle bytes ─────────────────────────────────────
TEST(WideFloatValue, F128ExactArithmeticMatchesOracle) {
    auto add = WideFloatValue::add(f128(20.0), f128(22.0));
    ASSERT_TRUE(add.has_value());
    expectBytes(*add, expect({0,0,0,0,0,0,0,0,0,0,0,0,0,0x50,0x04,0x40}), "F128 20+22 = 42");

    auto sub = WideFloatValue::sub(f128(22.0), f128(20.0));
    ASSERT_TRUE(sub.has_value());
    expectBytes(*sub, expect({0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x00,0x40}), "F128 22-20 = 2");

    auto mul = WideFloatValue::mul(f128(20.0), f128(22.0));
    ASSERT_TRUE(mul.has_value());
    expectBytes(*mul, expect({0,0,0,0,0,0,0,0,0,0,0,0,0,0xb8,0x07,0x40}), "F128 20*22 = 440");
}

// ★ F128 INEXACT div (20/22, 1/3) → the 113-bit round-to-nearest-even proofs.
TEST(WideFloatValue, F128InexactDivRoundToNearestEvenMatchesOracle) {
    auto d1 = WideFloatValue::div(f128(20.0), f128(22.0));
    ASSERT_TRUE(d1.has_value());
    expectBytes(*d1, expect({0x17,0x5d,0x74,0xd1,0x45,0x17,0x5d,0x74,
                             0xd1,0x45,0x17,0x5d,0x74,0xd1,0xfe,0x3f}),
                "F128 20/22 (INEXACT)");

    auto d2 = WideFloatValue::div(f128(1.0), f128(3.0));
    ASSERT_TRUE(d2.has_value());
    expectBytes(*d2, expect({0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,
                             0x55,0x55,0x55,0x55,0x55,0x55,0xfd,0x3f}),
                "F128 1/3 (INEXACT, repeating .0101)");
}

// ── (int)(20+22) = 42 (toInt64 at the operand's own precision) ───────────────
TEST(WideFloatValue, ToInt64TruncatesTowardZero) {
    auto add80 = WideFloatValue::add(f80(20.0), f80(22.0));
    ASSERT_TRUE(add80.has_value());
    auto i80 = add80->toInt64();
    ASSERT_TRUE(i80.has_value());
    EXPECT_EQ(*i80, 42);

    auto add128 = WideFloatValue::add(f128(20.0), f128(22.0));
    ASSERT_TRUE(add128.has_value());
    auto i128 = add128->toInt64();
    ASSERT_TRUE(i128.has_value());
    EXPECT_EQ(*i128, 42);

    // Truncation toward zero: 20/22 = 0.909… → 0; -(20/22) → 0; 22/20 = 1.1 → 1.
    EXPECT_EQ(*WideFloatValue::div(f80(20.0), f80(22.0))->toInt64(), 0);
    EXPECT_EQ(*WideFloatValue::div(f80(22.0), f80(20.0))->toInt64(), 1);
    // Negative divide result truncates toward zero.
    auto neg = WideFloatValue::div(f80(-22.0), f80(20.0));
    ASSERT_TRUE(neg.has_value());
    EXPECT_EQ(*neg->toInt64(), -1);
    // NaN / Inf → nullopt.
    EXPECT_FALSE(WideFloatValue::nan(TypeKind::F80).toInt64().has_value());
    EXPECT_FALSE(WideFloatValue::infinity(TypeKind::F128, false).toInt64().has_value());
}

// ── fromInt64 is EXACT — a >2^53 integer that binary64 CANNOT hold round-trips
// through F80/F128 (the reason int→long-double must not go via a host double). ──
TEST(WideFloatValue, FromInt64ExactBeyondBinary64) {
    // 2^53 + 1 = 9007199254740993 — NOT representable in binary64 (rounds to 2^53).
    std::int64_t const big = 9007199254740993LL;
    EXPECT_EQ(*WideFloatValue::fromInt64(big, TypeKind::F80).toInt64(),  big);
    EXPECT_EQ(*WideFloatValue::fromInt64(big, TypeKind::F128).toInt64(), big);
    // Round-numbers + sign + the INT64_MIN corner.
    // All elements MUST be std::int64_t: on Linux LP64 int64_t is `long` while
    // `0LL` is `long long`, so a mixed braced-init fails initializer_list<auto>
    // deduction on gcc (accepted on MSVC where int64_t IS long long).
    for (std::int64_t v : {std::int64_t{0}, std::int64_t{42}, std::int64_t{-42},
                           std::int64_t{1024},
                           std::numeric_limits<std::int64_t>::min(),
                           std::numeric_limits<std::int64_t>::max()}) {
        auto w80 = WideFloatValue::fromInt64(v, TypeKind::F80);
        // INT64_MAX = 2^63-1 fits F80's 64-bit significand exactly; toInt64 round-trips.
        EXPECT_EQ(*w80.toInt64(), v) << "F80 fromInt64/toInt64 round-trip of " << v;
        EXPECT_EQ(*WideFloatValue::fromInt64(v, TypeKind::F128).toInt64(), v);
    }
}

// ── fromDouble is EXACT (double 53-bit ⊆ 64/113-bit): a whole-number widen
// then toDouble round-trips; and widen→pack→(re-widen) stays bit-stable. ─────
TEST(WideFloatValue, FromDoubleExactAndToDoubleRoundTrip) {
    for (double d : {20.0, 22.0, 42.0, 0.5, 3.14159265358979, -7.25, 1024.0}) {
        EXPECT_EQ(f80(d).toDouble(), d)  << "F80 widen/narrow round-trip of " << d;
        EXPECT_EQ(f128(d).toDouble(), d) << "F128 widen/narrow round-trip of " << d;
    }
    // A folded result narrows to the binary64 value (42.0 is exact in all three).
    auto add = WideFloatValue::add(f80(20.0), f80(22.0));
    ASSERT_TRUE(add.has_value());
    EXPECT_EQ(add->toDouble(), 42.0);
}

// ── toDouble narrowing a NORMAL F80/F128 into the F64-SUBNORMAL range: a SINGLE
// round-to-nearest-even at the subnormal grid (LSB 2^-1074), NEVER round-to-53-
// then-truncate (which double-rounds + drops the low tail). Each `expectBits` is
// gcc-native `(double)X` for the packed input. The ⋆ cases are RED under the pre-
// fix truncating path — it flushed 1.7·2^-1075 to +0 and broke the 1.5·2^-1074 tie
// DOWN to 1 (not to the even 2), and truncated 1/3·2^-1035 to …aaaa (not …aaab). ──
TEST(WideFloatValue, ToDoubleSubnormalRoundToNearestEven) {
    struct Case { TypeKind kind; std::uint64_t lo; std::uint64_t hi;
                  std::uint64_t expectBits; char const* label; };
    static constexpr Case cases[] = {
        // ⋆ deep-underflow: value ∈ (½·min-subnormal, min-subnormal) → rounds UP to
        //   the smallest subnormal 2^-1074; the old `shift>=53 return 0` flushed it.
        {TypeKind::F80,  0xd99999999999999aULL, 0x0000000000003bccULL,
         0x0000000000000001ULL, "F80 1.7*2^-1075 -> smallest subnormal (old flushed 0)"},
        // ⋆ exact tie at 1.5·2^-1074 → tie-to-EVEN rounds to 2, not down to 1.
        {TypeKind::F80,  0xc000000000000000ULL, 0x0000000000003bcdULL,
         0x0000000000000002ULL, "F80 1.5*2^-1074 tie -> even 2 (old truncated to 1)"},
        //   round-down / repeating pattern (both paths agree; correctness coverage).
        {TypeKind::F80,  0xaaaaaaaaaaaaaaabULL, 0x0000000000003bf7ULL,
         0x0000055555555555ULL, "F80 (1/3)*2^-1030"},
        {TypeKind::F128, 0x3333333333333333ULL, 0x3bccb33333333333ULL,
         0x0000000000000001ULL, "F128 1.7*2^-1075 -> smallest subnormal (old flushed 0)"},
        // ⋆ F128 sigLo_ tail MUST feed the sticky: old truncated to …aaaa.
        {TypeKind::F128, 0x5555555555555555ULL, 0x3bf2555555555555ULL,
         0x0000002aaaaaaaabULL, "F128 (1/3)*2^-1035 (old truncated ...aaaa)"},
        {TypeKind::F128, 0x95355fb8ac404e79ULL, 0x3bf05bf0a8b14576ULL,
         0x0000000adf85458aULL, "F128 e*2^-1040"},
    };
    for (auto const& c : cases) {
        double const got = WideFloatValue::fromPacked(c.lo, c.hi, c.kind).toDouble();
        std::uint64_t gotBits = 0;
        std::memcpy(&gotBits, &got, sizeof(double));
        EXPECT_EQ(gotBits, c.expectBits)
            << c.label << " — got double bits 0x" << std::hex << gotBits
            << " want 0x" << c.expectBits;
    }
}

// ── negate (exact sign flip) + signed zero ───────────────────────────────────
TEST(WideFloatValue, NegateAndSignedZero) {
    auto v  = f80(42.0);
    auto nv = v.negate();
    EXPECT_EQ(nv.toDouble(), -42.0);
    EXPECT_EQ(nv.negate(), v);   // double-negate is identity (bit-exact)

    // -20 + 22 = 2 ; 20 + -22 = -2 (opposite-sign magnitude subtract).
    auto s1 = WideFloatValue::add(f80(-20.0), f80(22.0));
    ASSERT_TRUE(s1.has_value());
    EXPECT_EQ(s1->toDouble(), 2.0);
    auto s2 = WideFloatValue::add(f80(20.0), f80(-22.0));
    ASSERT_TRUE(s2.has_value());
    EXPECT_EQ(s2->toDouble(), -2.0);

    // x - x = +0.
    auto z = WideFloatValue::sub(f80(42.0), f80(42.0));
    ASSERT_TRUE(z.has_value());
    EXPECT_TRUE(z->isZero());
}

// ── inf / nan production (div-by-zero → signed inf / NaN, IEEE) ───────────────
TEST(WideFloatValue, InfinityAndNaNProduction) {
    // x / 0 → signed infinity.
    auto pInf = WideFloatValue::div(f80(1.0), f80(0.0));
    ASSERT_TRUE(pInf.has_value());
    EXPECT_TRUE(pInf->isInfinity());
    auto nInf = WideFloatValue::div(f80(-1.0), f80(0.0));
    ASSERT_TRUE(nInf.has_value());
    EXPECT_TRUE(nInf->isInfinity());
    EXPECT_EQ(nInf->toDouble(), -std::numeric_limits<double>::infinity());
    // 0 / 0 → NaN ; inf - inf → NaN.
    EXPECT_TRUE(WideFloatValue::div(f128(0.0), f128(0.0))->isNaN());
    EXPECT_TRUE(WideFloatValue::sub(WideFloatValue::infinity(TypeKind::F80, false),
                                    WideFloatValue::infinity(TypeKind::F80, false))->isNaN());
    // inf + finite → inf.
    EXPECT_TRUE(WideFloatValue::add(WideFloatValue::infinity(TypeKind::F128, false),
                                    f128(1.0))->isInfinity());
}

// ── compare (the 6 relational folds) — ordering + NaN unordered + ±0 equal ───
TEST(WideFloatValue, CompareOrdering) {
    using O = WideFloatValue::Ordering;
    EXPECT_EQ(WideFloatValue::compare(f80(20.0), f80(22.0)), O::Less);
    EXPECT_EQ(WideFloatValue::compare(f80(22.0), f80(20.0)), O::Greater);
    EXPECT_EQ(WideFloatValue::compare(f80(42.0), f80(42.0)), O::Equal);
    EXPECT_EQ(WideFloatValue::compare(f80(-1.0), f80(1.0)),  O::Less);
    EXPECT_EQ(WideFloatValue::compare(f80(-2.0), f80(-1.0)), O::Less);
    EXPECT_EQ(WideFloatValue::compare(WideFloatValue::zero(TypeKind::F80, true),
                                      WideFloatValue::zero(TypeKind::F80, false)), O::Equal);  // -0 == +0
    EXPECT_EQ(WideFloatValue::compare(WideFloatValue::nan(TypeKind::F80), f80(1.0)),
              O::Unordered);
    EXPECT_EQ(WideFloatValue::compare(f128(1.0), f128(2.0)), O::Less);   // F128 too
}

// ── subnormal double INPUT widens to a normal wide value (not flushed) ───────
TEST(WideFloatValue, SubnormalDoubleInputWidensToNormal) {
    double const sub = std::numeric_limits<double>::denorm_min();  // 2^-1074
    auto w = f80(sub);
    EXPECT_FALSE(w.isZero());
    EXPECT_EQ(w.toDouble(), sub);   // exact round-trip (F80's wider exponent absorbs it)
    auto w128 = f128(sub);
    EXPECT_FALSE(w128.isZero());
    EXPECT_EQ(w128.toDouble(), sub);
}

// ── cross-kind arithmetic is a defensive nullopt (never a silent mis-fold) ───
TEST(WideFloatValue, CrossKindArithmeticRefuses) {
    EXPECT_FALSE(WideFloatValue::add(f80(1.0), f128(1.0)).has_value());
    EXPECT_FALSE(WideFloatValue::mul(f128(1.0), f80(1.0)).has_value());
    // An unsupported kind (F64) never enters the engine.
    EXPECT_FALSE(WideFloatValue::isSupportedKind(TypeKind::F64));
    EXPECT_TRUE(WideFloatValue::isSupportedKind(TypeKind::F80));
    EXPECT_TRUE(WideFloatValue::isSupportedKind(TypeKind::F128));
}

// ── pack bit-exact equality (the pool round-trip identity) ───────────────────
TEST(WideFloatValue, OperatorEqualsIsBitExact) {
    EXPECT_EQ(f80(42.0), f80(42.0));
    EXPECT_FALSE(f80(42.0) == f80(43.0));
    EXPECT_FALSE(f80(42.0) == f128(42.0));   // different kind
    auto a = WideFloatValue::add(f128(20.0), f128(22.0));
    auto b = WideFloatValue::add(f128(20.0), f128(22.0));
    ASSERT_TRUE(a && b);
    EXPECT_EQ(*a, *b);
}

// ── pack ∘ fromPacked round-trip (the .dsshir/.dssir text reader inverse) ────
TEST(WideFloatValue, PackFromPackedRoundTrip) {
    auto roundtrip = [](WideFloatValue const& v) {
        auto const p = v.pack();
        return WideFloatValue::fromPacked(p.lo, p.hi, v.kind());
    };
    for (TypeKind const k : {TypeKind::F80, TypeKind::F128}) {
        for (double d : {20.0, 22.0, 42.0, -7.25, 0.5, 1024.0, 0.0, -0.0}) {
            auto v = WideFloatValue::fromDouble(d, k);
            EXPECT_EQ(roundtrip(v), v) << "round-trip of " << d << " kind " << static_cast<int>(k);
        }
        // Folded inexact values (20/22, 1/3) round-trip bit-exactly.
        auto d1 = *WideFloatValue::div(WideFloatValue::fromDouble(20.0, k),
                                       WideFloatValue::fromDouble(22.0, k));
        EXPECT_EQ(roundtrip(d1), d1) << "round-trip of 20/22 kind " << static_cast<int>(k);
        auto d2 = *WideFloatValue::div(WideFloatValue::fromDouble(1.0, k),
                                       WideFloatValue::fromDouble(3.0, k));
        EXPECT_EQ(roundtrip(d2), d2) << "round-trip of 1/3 kind " << static_cast<int>(k);
        // inf / nan / signed zero round-trip.
        EXPECT_EQ(roundtrip(WideFloatValue::infinity(k, true)), WideFloatValue::infinity(k, true));
        EXPECT_TRUE(roundtrip(WideFloatValue::nan(k)).isNaN());
    }
}

// ── gcc-native differential (WSL x86_64 leg ONLY — inert on MSVC) ────────────
// Compute the SAME op in gcc-native `long double` (F80/x87) and `__float128`
// (F128/libgcc), memcpy the significant bytes, and assert the soft-float bit
// pattern matches. Host-independent since gcc-native there IS the reference.
#if defined(__GNUC__) && defined(__x86_64__) && !defined(_MSC_VER)
namespace {

std::array<std::uint8_t, 16> nativeF80Bytes(long double v) {
    std::array<std::uint8_t, 16> out{};
    std::memcpy(out.data(), &v, sizeof(long double));   // 10 significant + pad
    return out;
}
std::array<std::uint8_t, 16> nativeF128Bytes(__float128 v) {
    std::array<std::uint8_t, 16> out{};
    std::memcpy(out.data(), &v, sizeof(__float128));
    return out;
}

// Compare only the SIGNIFICANT bytes (F80: 0..9; F128: 0..15).
void expectMatchNative(WideFloatValue const& soft,
                       std::array<std::uint8_t, 16> const& native,
                       std::size_t significant, char const* label) {
    auto const got = bytesOf(soft);
    for (std::size_t i = 0; i < significant; ++i)
        EXPECT_EQ(got[i], native[i]) << label << " byte " << i;
}

}  // namespace

TEST(WideFloatValue, DifferentialAgainstGccNativeF80) {
    long double const a = 20.0L, b = 22.0L;
    expectMatchNative(*WideFloatValue::add(f80(20.0), f80(22.0)), nativeF80Bytes(a + b), 10, "F80 add");
    expectMatchNative(*WideFloatValue::sub(f80(22.0), f80(20.0)), nativeF80Bytes(b - a), 10, "F80 sub");
    expectMatchNative(*WideFloatValue::mul(f80(20.0), f80(22.0)), nativeF80Bytes(a * b), 10, "F80 mul");
    expectMatchNative(*WideFloatValue::div(f80(20.0), f80(22.0)), nativeF80Bytes(a / b), 10, "F80 div 20/22");
    expectMatchNative(*WideFloatValue::div(f80(1.0),  f80(3.0)),  nativeF80Bytes(1.0L / 3.0L), 10, "F80 div 1/3");
}

TEST(WideFloatValue, DifferentialAgainstGccNativeF128) {
    // No `Q` suffix — gcc rejects it in -std=c++23 without -fext-numeric-literals;
    // int->__float128 (20, 22 are exactly representable) is portable.
    __float128 const a = 20, b = 22;
    expectMatchNative(*WideFloatValue::add(f128(20.0), f128(22.0)), nativeF128Bytes(a + b), 16, "F128 add");
    expectMatchNative(*WideFloatValue::sub(f128(22.0), f128(20.0)), nativeF128Bytes(b - a), 16, "F128 sub");
    expectMatchNative(*WideFloatValue::mul(f128(20.0), f128(22.0)), nativeF128Bytes(a * b), 16, "F128 mul");
    expectMatchNative(*WideFloatValue::div(f128(20.0), f128(22.0)), nativeF128Bytes(a / b), 16, "F128 div 20/22");
    expectMatchNative(*WideFloatValue::div(f128(1.0),  f128(3.0)),  nativeF128Bytes((__float128)1 / (__float128)3), 16, "F128 div 1/3");
}
#endif  // gcc-native differential
