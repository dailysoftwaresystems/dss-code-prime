/* A NaN's PAYLOAD survives every constant spelling of it, in every floating format.
 * `__builtin_nan("1")` / `__builtin_nanf` / `__builtin_nanl` place the payload in the low fraction
 * bits with the quiet bit set; a NaN converted between formats keeps the fraction's TOP bits; and
 * folded arithmetic with a NaN operand returns that NaN (the first one), its sign and payload kept.
 * The long double is whatever this target makes it -- x87 80-bit, binary128 or binary64 -- read
 * off the bytes of 1.0L (no <float.h>: DSS ships none yet), and each is checked against its own
 * bytes.
 *
 * MSVC has no `__builtin_nan*` (no `__GNUC__`): it returns 42 untested. The exit code is 42, or a
 * bitmask: 1 double/float, 2 long double constants, 4 a static long double, 8 conversions,
 * 16 folded arithmetic, 32 a long double read at run time, 64 an unknown long double format.
 */
#include <string.h>

#if defined(__GNUC__)
static long double s1 = __builtin_nanl("1");
static long double s2 = -__builtin_nanl("0x10");
/* Folded NaN arithmetic, where `long double` is WIDER than `double` -- the soft-float folding
 * this example is about. AppleClang 21 at -std=c2x refuses NaN arithmetic in a static
 * initializer ("cannot compile this static initializer yet", its own limitation; its long
 * double is `double`). */
#  if __SIZEOF_LONG_DOUBLE__ != 8
static long double sum = __builtin_nanl("2") + 1.0L;
static long double first = __builtin_nanl("4") + __builtin_nanl("5");
#  endif
static double narrowed = (double)__builtin_nanl("0x800");
static long double widened = (long double)__builtin_nan("0x800");

/* x87: the 64-bit significand (integer bit set), then sign and exponent -- 10 bytes. */
static unsigned char const x87_one[16]   = {1, 0, 0, 0, 0, 0, 0, 0xc0, 0xff, 0x7f};
static unsigned char const x87_neg10[16] = {0x10, 0, 0, 0, 0, 0, 0, 0xc0, 0xff, 0xff};
static unsigned char const x87_two[16]   = {2, 0, 0, 0, 0, 0, 0, 0xc0, 0xff, 0x7f};
static unsigned char const x87_four[16]  = {4, 0, 0, 0, 0, 0, 0, 0xc0, 0xff, 0x7f};
static unsigned char const x87_wide[16]  = {0, 0, 0x40, 0, 0, 0, 0, 0xc0, 0xff, 0x7f};
/* binary128: the 112-bit fraction (quiet bit at 111), then sign and exponent -- 16 bytes. */
static unsigned char const q_one[16]   = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0xff, 0x7f};
static unsigned char const q_neg10[16] = {0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0xff, 0xff};
static unsigned char const q_two[16]   = {2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0xff, 0x7f};
static unsigned char const q_four[16]  = {4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0xff, 0x7f};
static unsigned char const q_wide[16]  = {0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0, 0, 0, 0, 0x80, 0xff, 0x7f};
/* binary64: `long double` is `double` -- 8 bytes. */
static unsigned char const d_one[16]   = {1, 0, 0, 0, 0, 0, 0xf8, 0x7f};
static unsigned char const d_neg10[16] = {0x10, 0, 0, 0, 0, 0, 0xf8, 0xff};
static unsigned char const d_wide[16]  = {0, 8, 0, 0, 0, 0, 0xf8, 0x7f};

/* Does `v`'s storage hold exactly `want` (its first n bytes, little-endian)? */
static int same(long double v, unsigned char const *want, unsigned n) {
    unsigned char b[sizeof(long double)];
    memcpy(b, &v, sizeof v);
    return memcmp(b, want, n) == 0;
}

/* The format of THIS target's long double: 64 (x87), 113 (binary128), 53 (binary64), or 0. */
static int long_double_format(void) {
    long double one = 1.0L;
    unsigned char b[sizeof(long double)];
    memcpy(b, &one, sizeof one);
    if (sizeof one == 8) return 53;
    if (b[7] == 0x80 && b[8] == 0xff && b[9] == 0x3f) return 64;
    if (sizeof one == 16 && b[14] == 0xff && b[15] == 0x3f) return 113;
    return 0;
}
#endif

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    int bad = 0;
#if defined(__GNUC__)
    unsigned long long d;
    unsigned f;
    double const dn = __builtin_nan("1");
    float const fn = __builtin_nanf("0x10");
    memcpy(&d, &dn, 8);
    memcpy(&f, &fn, 4);
    if (d != 0x7ff8000000000001ull || f != 0x7fc00010u) bad |= 1;

    int const fmt = long_double_format();
    unsigned char const *one = d_one, *neg10 = d_neg10, *wide = d_wide;
    unsigned n = 8;
    unsigned long long narrowWant = 0x7ff8000000000800ull;
    if (fmt == 64) {
        one = x87_one; neg10 = x87_neg10; wide = x87_wide; n = 10;
        narrowWant = 0x7ff8000000000001ull;
    } else if (fmt == 113) {
        one = q_one; neg10 = q_neg10; wide = q_wide; n = 16;
        narrowWant = 0x7ff8000000000000ull;
    } else if (fmt != 53) {
        bad |= 64;
    }
    long double const l1 = __builtin_nanl("1");
    if (!same(l1, one, n) || !same(-__builtin_nanl("0x10"), neg10, n)) bad |= 2;
    if (!same(s1, one, n) || !same(s2, neg10, n)) bad |= 4;
    unsigned long long nb;
    memcpy(&nb, &narrowed, 8);
    if (nb != narrowWant || !same(widened, wide, n)) bad |= 8;
#  if __SIZEOF_LONG_DOUBLE__ != 8
    if (!same(sum, fmt == 64 ? x87_two : q_two, n) || !same(first, fmt == 64 ? x87_four : q_four, n))
        bad |= 16;
#  endif
    volatile long double rt = __builtin_nanl("1");
    long double const r = rt;
    if (!same(r, one, n)) bad |= 32;
#endif
    return bad ? bad : 42;
}
