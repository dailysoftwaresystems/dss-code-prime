/* <limits.h> realized per (target, format) pair from the lattice
 * (P68 round 9, D-C-LIMITS-H-DEFINES-NINE-OF-THE-STANDARD-MACROS).
 *
 * `LONG_MAX`, `LLONG_MAX`, `CHAR_MIN`, `ULONG_MAX` and the C23 width macros used
 * in EXPRESSIONS and in `#if`. Their values differ per pair — `long` is 32 bits on
 * Windows and 64 on Linux and macOS, plain `char` is unsigned on aarch64 Linux
 * only — so every check compares a macro with what the TARGET's own types say at
 * run time (`sizeof(long)`, `(char)-1 < 0`), read through `volatile`s the
 * optimizer cannot fold. Each group sets one bit; the exit is 42 only when all six
 * hold, and otherwise the bitmask of the groups that DID hold.
 *
 *   1  the `#if` value of LONG_MAX agrees with long's size
 *   2  LONG_MAX / LONG_MIN / ULONG_MAX are long's range, in expressions
 *   4  LLONG_MIN / LLONG_MAX / ULLONG_MAX
 *   8  CHAR_MIN / CHAR_MAX follow plain char's signedness, in #if and at run time
 *  16  the promoted TYPES (C 5.2.5.3.2), by _Generic
 *  32  the widths, the unsigned #if arm, BOOL_MAX, BITINT_MAXWIDTH, INT64_MAX
 */
#include <limits.h>
#include <stdint.h>

#if LONG_MAX > 2147483647
#define PP_LONG_BITS 64
#else
#define PP_LONG_BITS 32
#endif

#if CHAR_MIN < 0
#define PP_CHAR_SIGNED 1
#else
#define PP_CHAR_SIGNED 0
#endif

/* ULONG_MAX is unsigned in #if too: -1 converts to uintmax_t and is the larger. */
#if -1 < ULONG_MAX
#define PP_ULONG_UNSIGNED 0
#else
#define PP_ULONG_UNSIGNED 1
#endif

#if LONG_WIDTH == PP_LONG_BITS && ULONG_WIDTH == LONG_WIDTH && LLONG_WIDTH == 64
#define PP_WIDTHS 1
#else
#define PP_WIDTHS 0
#endif

/* A compiler with `_BitInt` (it predefines `__BITINT_MAXWIDTH__`) must give
 * <limits.h> a BITINT_MAXWIDTH of at least ULLONG_WIDTH (C23 5.2.5.3.2). gcc 13
 * has no `_BitInt`, so there the question does not arise. */
#if !defined(__BITINT_MAXWIDTH__) \
    || (defined(BITINT_MAXWIDTH) && BITINT_MAXWIDTH >= ULLONG_WIDTH \
        && BITINT_MAXWIDTH == __BITINT_MAXWIDTH__)
#define PP_BITINT 1
#else
#define PP_BITINT 0
#endif

/* BOOL_MAX: C23 5.2.5.3.2p2 gives every unsigned type with a _WIDTH macro a _MAX
 * (and bool is one, 6.2.5p8), so gcc 13, clang 18 and mingw define it as 1 — but
 * Apple clang 21 does not define it at all. Checked where it is defined. */
#if !defined(BOOL_MAX) || BOOL_MAX == 1
#define PP_BOOL_MAX 1
#else
#define PP_BOOL_MAX 0
#endif

static volatile long vAllOnesLong = -1;
static volatile char vMinusOneChar = (char)-1;
static volatile int  vCharBit = CHAR_BIT;

int main(void) {
    int bits = 0;

    if (PP_LONG_BITS == (int)sizeof(long) * vCharBit) bits |= 1;

    unsigned long const ones = (unsigned long)vAllOnesLong;
    if (ones == ULONG_MAX && (long)(ones >> 1) == LONG_MAX
        && -LONG_MAX - 1 == LONG_MIN) {
        bits |= 2;
    }

    if (LLONG_MAX == 9223372036854775807LL && LLONG_MIN == -LLONG_MAX - 1
        && ULLONG_MAX == 18446744073709551615ULL) {
        bits |= 4;
    }

    int const charSigned = vMinusOneChar < 0;
    if (PP_CHAR_SIGNED == charSigned
        && CHAR_MIN == (charSigned ? SCHAR_MIN : 0)
        && CHAR_MAX == (charSigned ? SCHAR_MAX : UCHAR_MAX)) {
        bits |= 8;
    }

    if (_Generic(LONG_MAX, long: 1, default: 0)
        && _Generic(ULONG_MAX, unsigned long: 1, default: 0)
        && _Generic(LLONG_MAX, long long: 1, default: 0)
        && _Generic(CHAR_MIN, int: 1, default: 0)
        && _Generic(USHRT_MAX, int: 1, default: 0)
        && _Generic(UINT_MAX, unsigned int: 1, default: 0)) {
        bits |= 16;
    }

    if (PP_WIDTHS && PP_ULONG_UNSIGNED && PP_BITINT && PP_BOOL_MAX
        && CHAR_WIDTH == CHAR_BIT && SHRT_WIDTH == 16 && INT_WIDTH == 32
        && _Generic(INT64_MAX, int64_t: 1, default: 0) && MB_LEN_MAX >= 1) {
        bits |= 32;
    }

    return bits == 63 ? 42 : bits;
}
