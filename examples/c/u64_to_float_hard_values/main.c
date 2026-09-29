/* THE UNSIGNED 64-BIT TO `float` CONVERSION AT ITS HARD VALUES.
 *
 * A C cast from a `volatile unsigned long long`, the float's BIT PATTERN compared with the
 * correctly rounded value (round to nearest, ties to even, ONE rounding), computed in exact
 * arithmetic by the lane's conversion-matrix generator. AArch64 converts an unsigned 64-bit
 * integer directly (UCVTF); baseline x86_64 has only the SIGNED conversion, so a value with the
 * top bit set is halved first, and a halving that drops bit 0 instead of folding it into the
 * kept bits rounds checks 12, 16, 18 the wrong way. Checks 7, 12, 13, 16, 18, 20
 * are the values a detour through `double` rounds TWICE, landing on a different float.
 * Check k returns k. */
#include <string.h>

static unsigned bits_of(float f) { unsigned u; memcpy(&u, &f, sizeof u); return u; }

int main(void) {
    /* zero */
    { volatile unsigned long long x = 0ULL; if (bits_of((float)x) != 0x00000000U) return 1; }
    /* one */
    { volatile unsigned long long x = 1ULL; if (bits_of((float)x) != 0x3f800000U) return 2; }
    /* 2^24+1: the first integer float cannot hold, a tie, to even (down) */
    { volatile unsigned long long x = 16777217ULL; if (bits_of((float)x) != 0x4b800000U) return 3; }
    /* 2^24+3: a tie, to even (up) */
    { volatile unsigned long long x = 16777219ULL; if (bits_of((float)x) != 0x4b800002U) return 4; }
    /* 2^53+1: rounds to 2^53 */
    { volatile unsigned long long x = 9007199254740993ULL; if (bits_of((float)x) != 0x5a000000U) return 5; }
    /* 2^62+2^38: a tie below the top bit, to even (down) */
    { volatile unsigned long long x = 4611686293305294848ULL; if (bits_of((float)x) != 0x5e800000U) return 6; }
    /* 2^62+2^38+1: just past that tie (up); through double it rounds twice (down) */
    { volatile unsigned long long x = 4611686293305294849ULL; if (bits_of((float)x) != 0x5e800001U) return 7; }
    /* 2^63-1: the largest value with the top bit clear, rounds up to 2^63 */
    { volatile unsigned long long x = 9223372036854775807ULL; if (bits_of((float)x) != 0x5f000000U) return 8; }
    /* 2^63: the first value with the top bit set, exact */
    { volatile unsigned long long x = 9223372036854775808ULL; if (bits_of((float)x) != 0x5f000000U) return 9; }
    /* 2^63+1: the halving drops bit 0, far below a tie */
    { volatile unsigned long long x = 9223372036854775809ULL; if (bits_of((float)x) != 0x5f000000U) return 10; }
    /* 2^63+2^39: a tie above the top bit, to even (down) */
    { volatile unsigned long long x = 9223372586610589696ULL; if (bits_of((float)x) != 0x5f000000U) return 11; }
    /* 2^63+2^39+1: just past that tie (up) ONLY IF the halving keeps the dropped bit; through double it rounds twice (down) */
    { volatile unsigned long long x = 9223372586610589697ULL; if (bits_of((float)x) != 0x5f000001U) return 12; }
    /* 2^63+3*2^39-1: just below the next tie (down) */
    { volatile unsigned long long x = 9223373686122217471ULL; if (bits_of((float)x) != 0x5f000001U) return 13; }
    /* 2^63+3*2^39: a tie, to even (up) */
    { volatile unsigned long long x = 9223373686122217472ULL; if (bits_of((float)x) != 0x5f000002U) return 14; }
    /* 2^63+5*2^39: a tie, to even (down) */
    { volatile unsigned long long x = 9223374785633845248ULL; if (bits_of((float)x) != 0x5f000002U) return 15; }
    /* 2^63+5*2^39+1: just past that tie (up) ONLY IF the halving keeps the dropped bit */
    { volatile unsigned long long x = 9223374785633845249ULL; if (bits_of((float)x) != 0x5f000003U) return 16; }
    /* 2^64-3*2^39: a tie near the top, to even (down) */
    { volatile unsigned long long x = 18446742424442109952ULL; if (bits_of((float)x) != 0x5f7ffffeU) return 17; }
    /* 2^64-3*2^39+1: just past that tie (up) ONLY IF the halving keeps the dropped bit */
    { volatile unsigned long long x = 18446742424442109953ULL; if (bits_of((float)x) != 0x5f7fffffU) return 18; }
    /* 2^64-2^40: the largest float below 2^64, exact */
    { volatile unsigned long long x = 18446742974197923840ULL; if (bits_of((float)x) != 0x5f7fffffU) return 19; }
    /* 2^64-2^39-1: just below the last tie (down) */
    { volatile unsigned long long x = 18446743523953737727ULL; if (bits_of((float)x) != 0x5f7fffffU) return 20; }
    /* 2^64-2^39: the last tie, to even — up to 2^64 */
    { volatile unsigned long long x = 18446743523953737728ULL; if (bits_of((float)x) != 0x5f800000U) return 21; }
    /* 2^64-1: rounds up to 2^64 */
    { volatile unsigned long long x = 18446744073709551615ULL; if (bits_of((float)x) != 0x5f800000U) return 22; }
    return 42;
}
