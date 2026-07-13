// C23 _BitInt(N) host bignum value type (C4b, D-CSUBSET-BITINT-CONSTFOLD-LARGE).
// Strict exact-value vectors at N ∈ {4, 8, 40, 65, 128, 200, 256} against a
// hand/reference oracle — the wrap chokepoint, sign-aware ops, div/rem with a
// non-64-multiple width, v==0 width, and the limb-bytes round-trip.

#include "core/types/bit_int_value.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using dss::BitIntValue;

namespace {

BitIntValue mag(std::vector<std::uint64_t> limbs, bool isSigned) {
    return BitIntValue::fromLiteralMagnitude(limbs, isSigned);
}

}  // namespace

// ── Literal width derivation (M3: v==0 → N=1) ───────────────────────────────
TEST(BitIntValue, LiteralWidthDerivation) {
    EXPECT_EQ(mag({0}, /*signed=*/false).width(), 1u);        // M3
    EXPECT_EQ(mag({0}, /*signed=*/true).width(), 1u);         // M3
    EXPECT_EQ(mag({15}, /*signed=*/false).width(), 4u);       // 15uwb → _BitInt(4)
    EXPECT_EQ(mag({15}, /*signed=*/true).width(), 5u);        // 15wb → _BitInt(5) (sign bit)
    EXPECT_EQ(mag({1}, /*signed=*/false).width(), 1u);        // 1uwb → unsigned _BitInt(1)
    EXPECT_EQ(mag({255}, /*signed=*/false).width(), 8u);      // 255uwb → _BitInt(8)
    EXPECT_EQ(mag({255}, /*signed=*/true).width(), 9u);       // 255wb → _BitInt(9)
    // 2^99: bit length 100 → uwb width 100.
    BitIntValue v99 = BitIntValue::shiftLeft(BitIntValue::fromU64(1, 200, false), 99, 200, false);
    // rebuild as a literal magnitude from its limbs:
    EXPECT_EQ(mag(v99.limbs(), false).width(), 100u);
}

// ── The wrap chokepoint + signed/unsigned wrap ──────────────────────────────
TEST(BitIntValue, SignedNegativeWrap) {
    // (_BitInt(8))200 == -56 (200 mod 2^8, signed).
    EXPECT_EQ(BitIntValue::fromI64(200, 8, /*signed=*/true).asI64(), -56);
    // (_BitInt(4))15 stays 15 unsigned; 15 signed 4-bit == -1.
    EXPECT_EQ(BitIntValue::fromU64(15, 4, /*signed=*/false).low64(), 15u);
    EXPECT_EQ(BitIntValue::fromI64(15, 4, /*signed=*/true).asI64(), -1);
}

TEST(BitIntValue, SameWidthMultiplyOverflowWraps) {
    // (_BitInt(40))2000000 * (_BitInt(40))2000000 wraps mod 2^40 (signed) = -398046511104.
    BitIntValue const a = BitIntValue::fromI64(2000000, 40, true);
    BitIntValue const r = BitIntValue::mul(a, a, 40, true);
    EXPECT_EQ(r.asI64(), -398046511104LL);
    // Cross-check against the hand oracle: 4e12 mod 2^40 = 701465116672 → signed −398046511104.
    EXPECT_EQ(r.low64() & ((1ull << 40) - 1), 701465116672ull);
}

TEST(BitIntValue, NarrowUnsignedAddMulWrap) {
    // Mirrors c23_bitint_basic: (u4)9 + (u4)9 == 2 ; (u4)3 * (u4)6 == 2.
    EXPECT_EQ(BitIntValue::add(BitIntValue::fromU64(9, 4, false),
                               BitIntValue::fromU64(9, 4, false), 4, false).low64(), 2u);
    EXPECT_EQ(BitIntValue::mul(BitIntValue::fromU64(3, 4, false),
                               BitIntValue::fromU64(6, 4, false), 4, false).low64(), 2u);
}

// ── Carry propagation across limbs (the carry-2 add class) ──────────────────
TEST(BitIntValue, MultiLimbAddCarry) {
    // (2^128-1) + (2^128-1) == 2^128-2 (mod 2^128): limbs [0xFFF…FE, 0xFFF…FF].
    BitIntValue const all1 = BitIntValue::bitNot(BitIntValue::fromU64(0, 128, false), 128, false);
    BitIntValue const r = BitIntValue::add(all1, all1, 128, false);
    ASSERT_EQ(r.limbs().size(), 2u);
    EXPECT_EQ(r.limbs()[0], 0xFFFFFFFFFFFFFFFEull);
    EXPECT_EQ(r.limbs()[1], 0xFFFFFFFFFFFFFFFFull);
    // 1 + (2^64-1) == 2^64 : carry out of limb 0 into limb 1.
    BitIntValue const s = BitIntValue::add(BitIntValue::fromU64(1, 128, false),
                                           BitIntValue::fromU64(~0ull, 128, false), 128, false);
    EXPECT_EQ(s.limbs()[0], 0ull);
    EXPECT_EQ(s.limbs()[1], 1ull);
}

// ── Division / remainder at a non-64-multiple width (signed C99 fixup) ───────
TEST(BitIntValue, DivRemNonMultipleWidthSigned) {
    BitIntValue const a = BitIntValue::fromI64(-100, 40, true);
    BitIntValue const b = BitIntValue::fromI64(7, 40, true);
    auto const q = BitIntValue::divide(a, b, 40, true);
    auto const r = BitIntValue::remainder(a, b, 40, true);
    ASSERT_TRUE(q.has_value());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(q->asI64(), -14);   // trunc toward zero
    EXPECT_EQ(r->asI64(), -2);    // sign of the dividend
    // Unsigned 40-bit: 100 / 7 == 14 rem 2.
    auto const uq = BitIntValue::divide(BitIntValue::fromU64(100, 40, false),
                                        BitIntValue::fromU64(7, 40, false), 40, false);
    ASSERT_TRUE(uq.has_value());
    EXPECT_EQ(uq->low64(), 14u);
    // Division by zero → fail-loud sentinel (nullopt).
    EXPECT_FALSE(BitIntValue::divide(a, BitIntValue::fromI64(0, 40, true), 40, true).has_value());
    EXPECT_FALSE(BitIntValue::remainder(a, BitIntValue::fromI64(0, 40, true), 40, true).has_value());
}

// ── Shifts (cast-then-shift; arithmetic right shift when signed) ────────────
TEST(BitIntValue, ShiftsWideAndSigned) {
    // ((unsigned _BitInt(100))1) << 99 == 2^99 (M2): limbs [0, 2^35].
    BitIntValue const s = BitIntValue::shiftLeft(BitIntValue::fromU64(1, 100, false), 99, 100, false);
    ASSERT_EQ(s.limbs().size(), 2u);
    EXPECT_EQ(s.limbs()[0], 0ull);
    EXPECT_EQ(s.limbs()[1], (1ull << 35));   // 2^99 = 2^64 * 2^35
    // Arithmetic right shift of a negative narrow value: (-8 signed _BitInt(8)) >> 1 == -4.
    EXPECT_EQ(BitIntValue::shiftRight(BitIntValue::fromI64(-8, 8, true), 1, 8, true).asI64(), -4);
    // Logical right shift (unsigned): (u8)200 >> 2 == 50.
    EXPECT_EQ(BitIntValue::shiftRight(BitIntValue::fromU64(200, 8, false), 2, 8, false).low64(), 50u);
}

// ── Bitwise + unary ─────────────────────────────────────────────────────────
TEST(BitIntValue, BitwiseAndUnary) {
    EXPECT_EQ(BitIntValue::bitAnd(BitIntValue::fromU64(6, 40, false),
                                  BitIntValue::fromU64(3, 40, false), 40, false).low64(), 2u);
    EXPECT_EQ(BitIntValue::bitOr(BitIntValue::fromU64(4, 40, false),
                                 BitIntValue::fromU64(1, 40, false), 40, false).low64(), 5u);
    EXPECT_EQ(BitIntValue::bitXor(BitIntValue::fromU64(6, 40, false),
                                  BitIntValue::fromU64(5, 40, false), 40, false).low64(), 3u);
    // ~0 at 128 bits == 2^128-1.
    BitIntValue const n = BitIntValue::bitNot(BitIntValue::fromU64(0, 128, false), 128, false);
    EXPECT_EQ(n.limbs()[0], 0xFFFFFFFFFFFFFFFFull);
    EXPECT_EQ(n.limbs()[1], 0xFFFFFFFFFFFFFFFFull);
    // Unary neg: -(-56 as _BitInt(8)) == 56.
    EXPECT_EQ(BitIntValue::neg(BitIntValue::fromI64(-56, 8, true), 8, true).asI64(), 56);
}

// ── Wide multiply into upper limbs (200 / 256-bit) ──────────────────────────
TEST(BitIntValue, WideMultiplyUpperLimbs) {
    // 2^150 at 200 bits: limb 2 == 2^22, rest 0.
    BitIntValue const p = BitIntValue::shiftLeft(BitIntValue::fromU64(1, 200, false), 150, 200, false);
    ASSERT_EQ(p.limbs().size(), 4u);
    EXPECT_EQ(p.limbs()[0], 0ull);
    EXPECT_EQ(p.limbs()[1], 0ull);
    EXPECT_EQ(p.limbs()[2], (1ull << 22));
    EXPECT_EQ(p.limbs()[3], 0ull);
    // (2^100) * (2^100) == 2^200 at 256 bits: limb 3 == 2^8.
    BitIntValue const e100 = BitIntValue::shiftLeft(BitIntValue::fromU64(1, 256, false), 100, 256, false);
    BitIntValue const e200 = BitIntValue::mul(e100, e100, 256, false);
    ASSERT_EQ(e200.limbs().size(), 4u);
    EXPECT_EQ(e200.limbs()[0], 0ull);
    EXPECT_EQ(e200.limbs()[1], 0ull);
    EXPECT_EQ(e200.limbs()[2], 0ull);
    EXPECT_EQ(e200.limbs()[3], (1ull << 8));
}

// ── Comparison over the common type (signed + unsigned) ─────────────────────
TEST(BitIntValue, CompareThreeWay) {
    // Signed: -1 < 0 < 1 at 65 bits.
    EXPECT_LT(BitIntValue::compare(BitIntValue::fromI64(-1, 65, true),
                                   BitIntValue::fromI64(0, 65, true), 65, true), 0);
    EXPECT_GT(BitIntValue::compare(BitIntValue::fromI64(1, 65, true),
                                   BitIntValue::fromI64(0, 65, true), 65, true), 0);
    EXPECT_EQ(BitIntValue::compare(BitIntValue::fromI64(5, 65, true),
                                   BitIntValue::fromI64(5, 65, true), 65, true), 0);
    // Two negatives: -2 < -1.
    EXPECT_LT(BitIntValue::compare(BitIntValue::fromI64(-2, 65, true),
                                   BitIntValue::fromI64(-1, 65, true), 65, true), 0);
    // Unsigned magnitude: 2^64 > 1 (needs the high limb).
    BitIntValue const big = BitIntValue::shiftLeft(BitIntValue::fromU64(1, 128, false), 64, 128, false);
    EXPECT_GT(BitIntValue::compare(big, BitIntValue::fromU64(1, 128, false), 128, false), 0);
}

// ── toLimbBytes → fromLimbBytes round-trip ──────────────────────────────────
TEST(BitIntValue, LimbBytesRoundTrip) {
    BitIntValue const v = BitIntValue::shiftLeft(BitIntValue::fromU64(0x0123456789ABCDEFull, 100, false),
                                                 20, 100, false);
    std::vector<std::uint8_t> const bytes = v.toLimbBytes();
    EXPECT_EQ(bytes.size(), static_cast<std::size_t>((100 + 63) / 64) * 8);   // ceil(100/64)*8 = 16
    BitIntValue const rt = BitIntValue::fromLimbBytes(bytes, v.width(), v.isSigned());
    EXPECT_EQ(rt, v);
    // A signed negative value round-trips (sign extension preserved).
    BitIntValue const nv = BitIntValue::fromI64(-1234567, 200, true);
    EXPECT_EQ(BitIntValue::fromLimbBytes(nv.toLimbBytes(), 200, true), nv);
}

// ── Conversion (C 6.3.1.3): a BitInt outranked by a wider width is value-preserving.
TEST(BitIntValue, ConvertWidening) {
    // -1 signed _BitInt(8) widened to _BitInt(40) stays -1 (sign extension).
    EXPECT_EQ(BitIntValue::fromI64(-1, 8, true).withType(40, true).asI64(), -1);
    // 255 unsigned _BitInt(8) widened to signed _BitInt(40) stays 255 (zero extension).
    EXPECT_EQ(BitIntValue::fromU64(255, 8, false).withType(40, true).asI64(), 255);
}
