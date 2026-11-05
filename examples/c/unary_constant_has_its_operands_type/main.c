/* D-CE-UNSIGNED-UNARY-RESULT-NOT-REDUCED-TO-ITS-TYPE,
 * D-CE-CAST-OF-AN-UNSIGNED-CONSTANT-ABOVE-INT64-DOES-NOT-FOLD,
 * D-PP-IF-TRUTH-RESULT-IS-AN-INT-NOT-INTMAX — a constant expression has the
 * value its TYPE gives it.
 *
 * A unary `-` or `~` is performed in the promoted type of its operand and its
 * result has that type (C 6.5.3.3); an unsigned type's computation is reduced
 * modulo 2^N (6.2.5p9). So `~0U` is 0xFFFFFFFF, and stays 0xFFFFFFFF when
 * something widens it. The constant evaluator computed it on 64 bits and
 * reduced nothing: every widening below read 2^64-1.
 *
 * The exit code names the first check that failed:
 *    1..9   a widened `~` / `-` of an `unsigned int` in a static initializer
 *   10..14  the same in an assertion's own positions: an enumerator, an array
 *           bound (the assertions themselves stop the compile)
 *   15..19  an unsigned constant above INT64_MAX as an operand and under a
 *           cast — a hash seed, the identity cast, a truth test
 *   20..24  unary `+` in a constant expression
 *   25..30  A CONDITION that is a constant, inside a function: folded to the
 *           wrong truth value, the code after the `if` used to be deleted
 *           (the program died of an illegal instruction)
 *   31..33  `#if`: a unary operator there is performed at the WIDEST type
 *           (`~0U` is 2^64-1 in a `#if`), and so is a shift whose left operand
 *           is a truth result
 *   34..41  the shipped headers' own constants above INT64_MAX, each under a
 *           cast — `(unsigned long long)(ULLONG_MAX)` in a static initializer
 *           was refused as not constant
 *   42      every one right
 */
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#if ~0U != 0xFFFFFFFFFFFFFFFF
#error in a #if the complement of 0U is 2^64-1
#endif
#if ((1 == 1) << 40) != 0x10000000000
#error in a #if a comparison's result is an operand of the widest type
#endif
#if ((!0) << 40) != 0x10000000000 || ((1 && 1) << 31) < 0
#error in a #if a truth result is an operand of the widest type
#endif

_Static_assert((unsigned long long)(~0U) == 0xFFFFFFFFull, "the complement of 0U, widened");
_Static_assert((unsigned long long)(~0U) != 0xFFFFFFFFFFFFFFFFull, "the same, written in the negative");
_Static_assert((long long)(-1U) == 4294967295ll, "minus 1U, widened");
_Static_assert(~(unsigned short)0 == -1, "below int the operand promotes to a signed int");
_Static_assert(~0x8000000000000000ull == 0x7FFFFFFFFFFFFFFFull, "an operand above INT64_MAX");
_Static_assert(+5U == 5U && -+-+7 == 7, "unary plus");

/* 1..9 */
static volatile unsigned long long w1 = ~0U;
static volatile unsigned long long w2 = (unsigned long long)(~0U);
static volatile long long w3 = -1U;
static volatile unsigned long long w4 = (unsigned long long)(-(unsigned)5);
static volatile unsigned long long w5 = ~0U + 0ULL;
static volatile double w6 = (double)(~0U);
static volatile long double w7 = (long double)(~0U);
static volatile long long w8 = ~(unsigned short)0;
/* The low-32-bit mask, as it is ordinarily spelt. */
#define LOW32 ((unsigned long long)~0U)
static volatile unsigned long long w9 = 0x123456789ABCDEF0ull & LOW32;

/* 10..14 */
enum { NIBBLE = (int)((unsigned long long)(~0U) >> 28), MINUS = (int)(-1U >> 31) };
static char sized[((unsigned long long)(~0U) >> 30) + 1];
static char masked[(0xF00000003ull & LOW32) + 1];

/* 15..19 */
#define FNV_OFFSET 0xCBF29CE484222325ull
static volatile unsigned long long s1 = (unsigned long long)FNV_OFFSET;
static volatile unsigned int s2 = (unsigned int)FNV_OFFSET;
static volatile unsigned long long s3 = ~FNV_OFFSET;
static volatile int s4 = !FNV_OFFSET;
static volatile int s5 = FNV_OFFSET ? 7 : 9;
static volatile long double s6 = (long double)18446744073709551615ULL;

/* 20..24 */
enum { PLUS = +3 };
static char plus_sized[+4];
static volatile long long p1 = +(unsigned char)255;
static volatile unsigned long long p2 = +18446744073709551615ULL;

/* 34..41: each of the seven is the maximum of a 64-bit unsigned type on the four
 * targets this example runs on, so every one is 2^64-1. */
static volatile unsigned long long h1 = (unsigned long long)(ULLONG_MAX);
static volatile unsigned long long h2 = (unsigned long long)(SIZE_MAX);
static volatile unsigned long long h3 = (unsigned long long)(UINT64_MAX);
static volatile unsigned long long h4 = (unsigned long long)(UINT_LEAST64_MAX);
static volatile unsigned long long h5 = (unsigned long long)(UINT_FAST64_MAX);
static volatile unsigned long long h6 = (unsigned long long)(UINTPTR_MAX);
static volatile unsigned long long h7 = (unsigned long long)(UINTMAX_MAX);
static volatile unsigned int h8 = (unsigned int)(UINT64_MAX);
static volatile long long h9 = (long long)(ULLONG_MAX);

/* 25..30: each must return its argument. */
static int c1(int x) { if ((unsigned long long)~0u > 0xFFFFFFFFull) return 0; return x; }
static int c2(int x) { if ((long long)-1u < 0) return 0; return x; }
static int c3(int x) { while ((unsigned long long)~0u != 0xFFFFFFFFull) { return 0; } return x; }
static int c4(int x) { if (~0u + 0ull > 0xFFFFFFFFull) return 0; return x; }
static int c5(int x) { if ((double)~0u > 4294967295.0) return 0; return x; }
static int c6(int x) { if (~0u > 0xFFFFFFFFu) return 0; return x; }

static volatile int source = 7;

int main(void) {
    int x = source;
    if (w1 != 0xFFFFFFFFull) return 1;
    if (w2 != 0xFFFFFFFFull) return 2;
    if (w3 != 4294967295ll) return 3;
    if (w4 != 0xFFFFFFFBull) return 4;
    if (w5 != 0xFFFFFFFFull) return 5;
    if (w6 != 4294967295.0) return 6;
    if (w7 != 4294967295.0L) return 7;
    if (w8 != -1) return 8;
    if (w9 != 0x9ABCDEF0ull) return 9;
    if (NIBBLE != 15) return 10;
    if (MINUS != 1) return 11;
    if (sizeof sized != 4) return 12;
    if (sizeof masked != 4) return 13;
    {
        static volatile unsigned long long in_a_block = (unsigned long long)(~0U);
        if (in_a_block != 0xFFFFFFFFull) return 14;
    }
    if (s1 != 0xCBF29CE484222325ull) return 15;
    if (s2 != 0x84222325u) return 16;
    if (s3 != 0x340D631B7BDDDCDAull) return 17;
    if (s4 != 0 || s5 != 7) return 18;
    if (s6 != 18446744073709551615.0L) return 19;
    if (PLUS != 3) return 20;
    if (sizeof plus_sized != 4) return 21;
    if (p1 != 255) return 22;
    if (p2 != 18446744073709551615ULL) return 23;
    if (+x != 7) return 24;
    if (c1(x) != 7) return 25;
    if (c2(x) != 7) return 26;
    if (c3(x) != 7) return 27;
    if (c4(x) != 7) return 28;
    if (c5(x) != 7) return 29;
    if (c6(x) != 7) return 30;
    if (h1 != 0xFFFFFFFFFFFFFFFFull) return 34;
    if (h2 != 0xFFFFFFFFFFFFFFFFull) return 35;
    if (h3 != 0xFFFFFFFFFFFFFFFFull) return 36;
    if (h4 != 0xFFFFFFFFFFFFFFFFull) return 37;
    if (h5 != 0xFFFFFFFFFFFFFFFFull) return 38;
    if (h6 != 0xFFFFFFFFFFFFFFFFull) return 39;
    if (h7 != 0xFFFFFFFFFFFFFFFFull) return 40;
    if (h8 != 0xFFFFFFFFu || h9 != -1) return 41;
#if (-1U >> 63) != 1
    return 31;
#endif
#if (~(1 == 1) << 40) != -0x20000000000
    return 32;
#endif
#if !(1 << 40)
    return 33;
#endif
    return 42;
}
