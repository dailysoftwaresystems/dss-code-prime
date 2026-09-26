/* A 128-BIT INTEGER TO `float`, BIT FOR BIT (D-CSUBSET-INT-TO-F32-CODEGEN, P68 round 12).
 *
 * `(float)` from `__int128`, `unsigned __int128`, `_BitInt(128)` and `unsigned _BitInt(128)`. Each
 * value is assembled from two 64-bit halves held in `volatile`s and the float's BIT PATTERN is
 * compared with the correctly rounded value computed in exact arithmetic by the lane's generator
 * (round to nearest, ties to even, ONE rounding; the unsigned maximum rounds past FLT_MAX to +inf).
 * Checks 3, 4, 9, 11, 15, 16, 21, 23 are the values a conversion THROUGH `double` rounds twice and lands on a different
 * float — the shape DSS must not take. Check k returns k. */
#include <stdint.h>
#include <string.h>

static uint32_t bits_of(float f) { uint32_t u; memcpy(&u, &f, sizeof u); return u; }

int main(void) {
    { volatile uint64_t hi = 0x8000000000000000ULL, lo = 0x0000000000000000ULL; __int128 x = (__int128)(((unsigned __int128)hi << 64) | lo);
      if (bits_of((float)x) != 0xff000000U) return 1; }
    { volatile uint64_t hi = 0x7fffffffffffffffULL, lo = 0xffffffffffffffffULL; __int128 x = (__int128)(((unsigned __int128)hi << 64) | lo);
      if (bits_of((float)x) != 0x7f000000U) return 2; }
    { volatile uint64_t hi = 0x0000001000001000ULL, lo = 0x0000000000000001ULL; __int128 x = (__int128)(((unsigned __int128)hi << 64) | lo);
      if (bits_of((float)x) != 0x71800001U) return 3; }
    { volatile uint64_t hi = 0xffffffefffffefffULL, lo = 0xffffffffffffffffULL; __int128 x = (__int128)(((unsigned __int128)hi << 64) | lo);
      if (bits_of((float)x) != 0xf1800001U) return 4; }
    { volatile uint64_t hi = 0x0000000000000001ULL, lo = 0x0000000000000001ULL; __int128 x = (__int128)(((unsigned __int128)hi << 64) | lo);
      if (bits_of((float)x) != 0x5f800000U) return 5; }
    { volatile uint64_t hi = 0xffffffffffffffffULL, lo = 0xffffffffffffffffULL; __int128 x = (__int128)(((unsigned __int128)hi << 64) | lo);
      if (bits_of((float)x) != 0xbf800000U) return 6; }
    { volatile uint64_t hi = 0xffffffffffffffffULL, lo = 0xffffffffffffffffULL; unsigned __int128 x = ((unsigned __int128)hi << 64) | lo;
      if (bits_of((float)x) != 0x7f800000U) return 7; }
    { volatile uint64_t hi = 0x8000000000000000ULL, lo = 0x0000000000000000ULL; unsigned __int128 x = ((unsigned __int128)hi << 64) | lo;
      if (bits_of((float)x) != 0x7f000000U) return 8; }
    { volatile uint64_t hi = 0x0000001000001000ULL, lo = 0x0000000000000001ULL; unsigned __int128 x = ((unsigned __int128)hi << 64) | lo;
      if (bits_of((float)x) != 0x71800001U) return 9; }
    { volatile uint64_t hi = 0x0000000000000000ULL, lo = 0xffffffffffffffffULL; unsigned __int128 x = ((unsigned __int128)hi << 64) | lo;
      if (bits_of((float)x) != 0x5f800000U) return 10; }
    { volatile uint64_t hi = 0x0000000000000001ULL, lo = 0x0000010000000001ULL; unsigned __int128 x = ((unsigned __int128)hi << 64) | lo;
      if (bits_of((float)x) != 0x5f800001U) return 11; }
    { volatile uint64_t hi = 0x0000000000000000ULL, lo = 0x0000000000000005ULL; unsigned __int128 x = ((unsigned __int128)hi << 64) | lo;
      if (bits_of((float)x) != 0x40a00000U) return 12; }
    { volatile uint64_t hi = 0x8000000000000000ULL, lo = 0x0000000000000000ULL; _BitInt(128) x = (_BitInt(128))(((unsigned _BitInt(128))hi << 64) | lo);
      if (bits_of((float)x) != 0xff000000U) return 13; }
    { volatile uint64_t hi = 0x7fffffffffffffffULL, lo = 0xffffffffffffffffULL; _BitInt(128) x = (_BitInt(128))(((unsigned _BitInt(128))hi << 64) | lo);
      if (bits_of((float)x) != 0x7f000000U) return 14; }
    { volatile uint64_t hi = 0x0000001000001000ULL, lo = 0x0000000000000001ULL; _BitInt(128) x = (_BitInt(128))(((unsigned _BitInt(128))hi << 64) | lo);
      if (bits_of((float)x) != 0x71800001U) return 15; }
    { volatile uint64_t hi = 0xffffffefffffefffULL, lo = 0xffffffffffffffffULL; _BitInt(128) x = (_BitInt(128))(((unsigned _BitInt(128))hi << 64) | lo);
      if (bits_of((float)x) != 0xf1800001U) return 16; }
    { volatile uint64_t hi = 0x0000000000000001ULL, lo = 0x0000000000000001ULL; _BitInt(128) x = (_BitInt(128))(((unsigned _BitInt(128))hi << 64) | lo);
      if (bits_of((float)x) != 0x5f800000U) return 17; }
    { volatile uint64_t hi = 0xffffffffffffffffULL, lo = 0xffffffffffffffffULL; _BitInt(128) x = (_BitInt(128))(((unsigned _BitInt(128))hi << 64) | lo);
      if (bits_of((float)x) != 0xbf800000U) return 18; }
    { volatile uint64_t hi = 0xffffffffffffffffULL, lo = 0xffffffffffffffffULL; unsigned _BitInt(128) x = ((unsigned _BitInt(128))hi << 64) | lo;
      if (bits_of((float)x) != 0x7f800000U) return 19; }
    { volatile uint64_t hi = 0x8000000000000000ULL, lo = 0x0000000000000000ULL; unsigned _BitInt(128) x = ((unsigned _BitInt(128))hi << 64) | lo;
      if (bits_of((float)x) != 0x7f000000U) return 20; }
    { volatile uint64_t hi = 0x0000001000001000ULL, lo = 0x0000000000000001ULL; unsigned _BitInt(128) x = ((unsigned _BitInt(128))hi << 64) | lo;
      if (bits_of((float)x) != 0x71800001U) return 21; }
    { volatile uint64_t hi = 0x0000000000000000ULL, lo = 0xffffffffffffffffULL; unsigned _BitInt(128) x = ((unsigned _BitInt(128))hi << 64) | lo;
      if (bits_of((float)x) != 0x5f800000U) return 22; }
    { volatile uint64_t hi = 0x0000000000000001ULL, lo = 0x0000010000000001ULL; unsigned _BitInt(128) x = ((unsigned _BitInt(128))hi << 64) | lo;
      if (bits_of((float)x) != 0x5f800001U) return 23; }
    { volatile uint64_t hi = 0x0000000000000000ULL, lo = 0x0000000000000005ULL; unsigned _BitInt(128) x = ((unsigned _BitInt(128))hi << 64) | lo;
      if (bits_of((float)x) != 0x40a00000U) return 24; }
    return 42;
}
