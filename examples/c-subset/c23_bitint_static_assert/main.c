/* C23 _BitInt wrap-aware CONST-FOLD in constant expressions (D-CSUBSET-BITINT-
 * CONSTFOLD-LARGE, C4b). Every `_Static_assert` below folds through the shared
 * host bignum at the TRUE C23 usual-arithmetic-conversion RESULT width — the
 * FILE-SCOPE asserts are the corpus pins, RED-ON-DISABLE (a wrong width or a
 * missing wrap turns one false and the translation unit fails to compile):
 *
 *   • `15wb + 1 == 16`         — `int` OUTRANKS `_BitInt(5)` (rank 5 < 32), so the
 *                                result is a plain `int`, NO wrap → 16 (not a
 *                                wrap-to-5-bits 0). The #1 correctness trap.
 *   • `(_BitInt(4))15 + 1 == 0`— the CAST wraps 15 into 4 signed bits → -1; -1 + 1
 *                                is `int` arithmetic → 0.
 *   • `(_BitInt(8))200 == -56` — 200 in 8 signed bits is -56.
 *   • `(u_BitInt(100))1 << 99` — cast THEN shift (M2): 1 << 99 == 2^99, exactly the
 *                                arbitrary-magnitude `...688uwb` literal.
 *   • bit-precise `/`          — 1000000 / 7 == 142857 at _BitInt(40).
 *
 * main proves a const-expr `_BitInt` array DIMENSION folds: `(unsigned _BitInt(8))130
 * - 100 == 30` (again `int`-outranked, no wrap), so `arr` has 30 ints; `30 + 12wb`
 * (a narrow wb literal) == 42.
 */

_Static_assert(15wb + 1 == 16,
               "int outranks _BitInt(5): result is int, no wrap -> 16");
_Static_assert((_BitInt(4))15 + 1 == 0,
               "narrow cast wrap: (_BitInt(4))15 == -1, so -1 + 1 == 0");
_Static_assert((_BitInt(8))200 == -56,
               "signed wrap: 200 in 8 signed bits == -56");
_Static_assert(((unsigned _BitInt(100))1uwb) << 99 == 633825300114114700748351602688uwb,
               "cast THEN shift (M2): 1 << 99 == 2^99");
_Static_assert((_BitInt(40))1000000 / (_BitInt(40))7 == 142857,
               "bit-precise division: 1000000 / 7 == 142857");
/* Same-width MULTIPLY overflow wraps mod 2^40 (unsigned uwb RHS, both bit-precise). */
_Static_assert((unsigned _BitInt(40))2000000 * (unsigned _BitInt(40))2000000
               == 701465116672uwb,
               "unsigned wrap: 4e12 mod 2^40 == 701465116672");
/* The SIGNED wrap == -398046511104: the RHS is a `long`/`long long` literal, so the
 * const-eval must type it 64-bit (Fork-2c) — treating it as `int` would mis-wrap it. */
_Static_assert((_BitInt(40))2000000 * (_BitInt(40))2000000 == -398046511104,
               "signed wrap: 4e12 mod 2^40 (signed) == -398046511104");

int main(void) {
    int arr[(int)((unsigned _BitInt(8))130 - 100)];   /* dim folds to 30 (int-outranked) */
    int n = (int)(sizeof(arr) / sizeof(arr[0]));       /* 30 */
    return n + (int)(12wb);                            /* 30 + 12 == 42 */
}
