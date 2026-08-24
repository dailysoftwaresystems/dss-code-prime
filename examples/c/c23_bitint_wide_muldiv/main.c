/* C23 _BitInt(N>64) — MULTI-LIMB multiply + divide/remainder (D-CSUBSET-BITINT-C3-MULDIV),
 * C3. Builds on the C2 by-address wide substrate. Exercises the HARDEST wide arithmetic:
 * schoolbook `*` via UMulH, and binary long-division `/` `%` (signed + unsigned, bit-
 * precise wrap). Every operand is a RUNTIME value (seeded through `volatile`), so no
 * const-fold can pre-evaluate the wide arithmetic and mask a broken multi-limb impl.
 *
 * The exit code is 42 ONLY when the multi-limb machinery is correct. RED-ON-DISABLE:
 *  - an OR-carry multiply (dropping a column carry-out of 2) breaks the all-ones product
 *    (CRIT-A) → t1/t2 flip;
 *  - a low-limb-only or non-wrapping multiply breaks the cross-limb / wrap cases → t3/t4;
 *  - a signed-magnitude long-division that sign-extends the most-negative magnitude
 *    (the CRIT-B unsigned-type mandate) breaks the _BitInt(200) most-negative divide/mod
 *    (CRIT-C, N%64==8) → t5/t6;
 *  - a broken C99 sign fixup breaks the four-sign-combo divide/modulo → t7;
 *  - a single-limb divide breaks the multi-limb quotient+remainder → t8.
 * Each term is a boolean-gated contribution; any one wrong ⇒ the sum ≠ 42. */

int main(void) {
    /* Runtime seeds — defeat const-fold (present and any future wide const-fold). */
    volatile int v1 = 1, v3 = 3, v5 = 5, v7 = 7, v17 = 17, v100 = 100;

    /* ── t1 (CRIT-A: the column-carry-2 discriminator). A multiply column can carry OUT
     * a value of 2 (c1+c2==2) when the accumulated dst limb, the partial-product low
     * word, AND the incoming carry are ALL near 2^64. The correct new carry is the
     * INTEGER Add(Add(hi,c1),c2); an Or(c1,c2) impl silently drops a 2^64. NOTE: an
     * all-ones square does NOT expose this (its lo = M*M mod 2^64 = 1 is tiny, so the
     * second carry never fires) — it takes crafted limbs. a=[1,M,0,0], b=[M,1,M,0]
     * (M=2^64-1): at column 2 the step (i=1,j=1) has dst=2^64-1, lo=2^64-1, cin=2^64-2
     * ⇒ c1==c2==1. a*b mod 2^256 = [2^64-1, 2, 2^64-4, 3]; an OR-carry impl reads limb 3
     * as 2, not 3. ──────────────────────────────────────────────────────────────────── */
    unsigned _BitInt(256) M   = ((unsigned _BitInt(256))v1 << 64) - (unsigned _BitInt(256))v1;
    unsigned _BitInt(256) ca  = (unsigned _BitInt(256))v1 | (M << 64);              /* [1,M,0,0] */
    unsigned _BitInt(256) cb  = M | ((unsigned _BitInt(256))v1 << 64) | (M << 128); /* [M,1,M,0] */
    unsigned _BitInt(256) cp  = ca * cb;
    int t1 = ((int)cp == -1 && (int)(cp >> 64) == 2 &&
              (int)(cp >> 128) == -4 && (int)(cp >> 192) == 3) ? 3 : 0;

    /* ── t2: all-ones multiply coverage + the middle-limb product. allones = 2^128-1;
     * (2^128-1)^2 mod 2^128 = 1. lo64 = 2^64-1; lo64^2 = 2^128 - 2^65 + 1 ⇒ low limb 1,
     * high limb 2^64-2 (a product value that LIVES in the high limb). ───────────────── */
    unsigned _BitInt(128) zero128  = (unsigned _BitInt(128))(v1 - v1);
    unsigned _BitInt(128) allones  = ~zero128;                 /* 2^128 - 1 */
    unsigned _BitInt(128) sq       = allones * allones;        /* mod 2^128 == 1 */
    unsigned _BitInt(128) lo64 = ((unsigned _BitInt(128))v1 << 64) - (unsigned _BitInt(128))v1;
    unsigned _BitInt(128) prod = lo64 * lo64;
    unsigned _BitInt(128) hiExpect = ((unsigned _BitInt(128))v1 << 64) - (unsigned _BitInt(128))(v1 + v1);
    int t2 = ((int)sq == 1 && (sq >> 64) == zero128 &&
              (int)prod == 1 && (prod >> 64) == hiExpect) ? 3 : 0;

    /* ── t3: multiply WRAP past N. 2^100 * 2^100 = 2^200 ; mod 2^128 == 0. ─────────── */
    unsigned _BitInt(128) h100 = (unsigned _BitInt(128))v1 << 100;
    int t3 = (h100 * h100 == zero128) ? 4 : 0;

    /* ── t4: multiply whose product lives in a HIGH limb. (2^64) * 3 = 3*2^64. ────── */
    unsigned _BitInt(128) two64 = (unsigned _BitInt(128))v1 << 64;
    unsigned _BitInt(128) cross = two64 * (unsigned _BitInt(128))v3;
    int t4 = ((int)(cross >> 64) == 3 && (int)cross == 0) ? 4 : 0;

    /* ── t5 (non-64-multiple N=200 VALUE coverage — NOT a red-on-disable discriminator:
     * the signed-uType bug is caught by t6, the carry bug by t1; here the corrupted
     * most-negative-dividend bits sit above bit 199, which the division never reads).
     * mn = -2^199 ; mn / 2 = -2^198 (trunc, arithmetic >> 197 = -2). mn % 3 = -2
     * (2^199 == 2 mod 3; the remainder takes the dividend's sign). Validates the
     * non-64-multiple magnitude path VALUE on the most-negative operand. ───────────── */
    _BitInt(200) mn   = (_BitInt(200))v1 << 199;               /* -2^199 (bit 199 = sign) */
    _BitInt(200) half = mn / (_BitInt(200))(v1 + v1);          /* -2^198 */
    int t5 = ((int)(half >> 197) == -2 &&
              (int)(mn % (_BitInt(200))v3) == -2) ? 5 : 0;

    /* ── t6 (CRIT-B: the unsigned-magnitude-type discriminator). A MOST-NEGATIVE signed
     * DIVISOR has magnitude 2^(N-1), which for N=128 (a 64-multiple) sets bit 127 = the
     * i64 SIGN bit of the top limb. If the long-division temps used the SIGNED type,
     * emitWideOrder's top-limb compare would go signed → the divisor reads NEGATIVE →
     * the `rem >= divisor` test inverts → wrong quotient. Under the mandated UNSIGNED
     * type it is a plain magnitude compare. mn128 = -2^127: mn128/mn128 = 1, rem 0; and
     * (-2^127+5)/mn128 = 0 (|dividend| < |divisor|). ──────────────────────────────── */
    _BitInt(128) mn128 = (_BitInt(128))v1 << 127;              /* -2^127 (INT128_MIN) */
    _BitInt(128) five  = (_BitInt(128))(v3 + v1 + v1);         /* 5 */
    int t6 = ((int)(mn128 / mn128) == 1 && (int)(mn128 % mn128) == 0 &&
              (int)((mn128 + five) / mn128) == 0) ? 3 : 0;

    /* ── t7: signed divide/modulo across all 4 sign combos of 17/5 (C99 trunc toward
     * zero: quotient toward 0, remainder takes the dividend's sign). ──────────────── */
    _BitInt(128) x = (_BitInt(128))v17, y = (_BitInt(128))v5;
    int t7 = ((int)(( x)/( y))== 3 && (int)(( x)%( y))== 2 &&
              (int)((-x)/( y))==-3 && (int)((-x)%( y))==-2 &&
              (int)(( x)/(-y))==-3 && (int)(( x)%(-y))== 2 &&
              (int)((-x)/(-y))== 3 && (int)((-x)%(-y))==-2) ? 4 : 0;

    /* ── t8: a MULTI-LIMB quotient AND a MULTI-LIMB remainder in one divide.
     * dividend = 7*2^128 + 9 (limb 2 + limb 0), divisor = 2^64.
     * quotient = 7*2^64 (spans limbs 1..2), remainder = 9. ────────────────────────── */
    unsigned _BitInt(200) dvd = ((unsigned _BitInt(200))v7 << 128) + (unsigned _BitInt(200))(v7 + v1 + v1);
    unsigned _BitInt(200) dvs = (unsigned _BitInt(200))v1 << 64;
    unsigned _BitInt(200) qq  = dvd / dvs;                     /* 7 * 2^64 */
    unsigned _BitInt(200) rr  = dvd % dvs;                     /* 9 */
    int t8 = ((int)(qq >> 64) == 7 && (int)qq == 0 && (int)rr == 9) ? 6 : 0;

    /* ── t9: unsigned _BitInt(128) basic divide + modulo. 100/7 = 14, 100%7 = 2. ──── */
    unsigned _BitInt(128) u100 = (unsigned _BitInt(128))v100, u7 = (unsigned _BitInt(128))v7;
    int t9 = ((int)(u100 / u7) == 14 && (int)(u100 % u7) == 2) ? 4 : 0;

    /* ── t10: signed _BitInt(128) divide + modulo with a negative dividend.
     * -100 / 7 = -14 ; -100 % 7 = -2. ────────────────────────────────────────────── */
    _BitInt(128) s100 = -(_BitInt(128))v100, s7 = (_BitInt(128))v7;
    int t10 = ((int)(s100 / s7) == -14 && (int)(s100 % s7) == -2) ? 6 : 0;

    /* 3+3+4+4+5+3+4+6+4+6 == 42, exactly. */
    return t1 + t2 + t3 + t4 + t5 + t6 + t7 + t8 + t9 + t10;
}
