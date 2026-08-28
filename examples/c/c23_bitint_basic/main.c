/* C23 _BitInt(N) — bit-precise integers (D-CSUBSET-BITINT), C1: N<=64.
 *
 * Exercises the full operator surface (+ - * / % & | ^ << >>), int<->_BitInt
 * conversions, sizeof / _Alignof, and — critically — the mod-2^N WRAP: two 4-bit
 * additions/multiplications OVERFLOW and MUST wrap. The exit code is 42 ONLY when
 * the masking is applied; a non-masking implementation keeps the un-wrapped 18s
 * and returns 74 (RED-ON-DISABLE: the wrap is load-bearing on the exit).
 *
 * sizeof / _Alignof are const-folded (a _BitInt's SIZE is a size_t, not a bit-
 * precise value), so the _Static_asserts hold; they match gcc-14 / clang-19's
 * x86-64 psABI (sizeof(_BitInt(4))==1, _BitInt(17)==4, _BitInt(40)==8, align too).
 * The _BitInt ARITHMETIC itself never const-folds (const-eval is bit-precise-free
 * this cycle), so every operator below runs at runtime in the -config=debug arm. */

_Static_assert(sizeof(_BitInt(4))            == 1, "sizeof(_BitInt(4)) == 1");
_Static_assert(sizeof(_BitInt(17))           == 4, "sizeof(_BitInt(17)) == 4");
_Static_assert(sizeof(unsigned _BitInt(40))  == 8, "sizeof(unsigned _BitInt(40)) == 8");
_Static_assert(sizeof(_BitInt(64))           == 8, "sizeof(_BitInt(64)) == 8");
_Static_assert(_Alignof(_BitInt(17))         == 4, "_Alignof(_BitInt(17)) == 4");

int main(void) {
    /* '+' : a 4-bit overflow — 9 + 9 = 18, which MUST wrap mod 2^4 to 2.
     * (A non-masking build keeps 18 → the exit changes: this is the wrap pin.) */
    unsigned _BitInt(4) x = 9u, y = 9u;
    unsigned _BitInt(4) sum4 = x + y;                 /* 18 -> 2 */

    /* '*' : another 4-bit wrap — 3 * 6 = 18 -> 2. */
    unsigned _BitInt(4) p = 3u, r = 6u;
    unsigned _BitInt(4) mul4 = p * r;                 /* 18 -> 2 */

    /* '-', '/', '%' on a 17-bit signed _BitInt (operands fit, no wrap). */
    _BitInt(17) a = 10, b = 5;
    _BitInt(17) sub17 = a - b;                        /* 5 */
    _BitInt(17) c = 20, d = 4;
    _BitInt(17) div17 = c / d;                        /* 5 */
    _BitInt(17) e = 23, f = 9;
    _BitInt(17) mod17 = e % f;                        /* 5 */

    /* '&', '|', '^', '>>' on an unsigned 40-bit _BitInt. */
    unsigned _BitInt(40) g = 6u, h = 3u;
    unsigned _BitInt(40) and40 = g & h;               /* 2 */
    unsigned _BitInt(40) i = 4u, j = 1u;
    unsigned _BitInt(40) or40 = i | j;                /* 5 */
    unsigned _BitInt(40) k = 6u, l = 5u;
    unsigned _BitInt(40) xor40 = k ^ l;               /* 3 */
    unsigned _BitInt(40) m = 40u;
    unsigned _BitInt(40) shr40 = m >> 3;              /* 5 */

    /* '<<' on a 6-bit _BitInt. */
    unsigned _BitInt(6) n = 1u;
    unsigned _BitInt(6) shl6 = n << 3;                /* 8 */

    /* Each _BitInt result converts back to int; the sum is exactly 42. */
    int total = (int)sum4 + (int)mul4                 /* 2 + 2          */
              + (int)sub17 + (int)div17 + (int)mod17  /* 5 + 5 + 5      */
              + (int)and40 + (int)or40 + (int)xor40   /* 2 + 5 + 3      */
              + (int)shl6  + (int)shr40;              /* 8 + 5          */
    return total;                                     /* == 42          */
}
