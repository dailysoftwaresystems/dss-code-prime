/* `<stdint.h>`'s limit and width macros — the whole C23 7.22.2 / 7.22.3 family —
 * used the way programs use them: in expressions, in #if, and against the types
 * they describe (P68 round 9, D-FFI-STDINT-LIMIT-MACROS). Each group sets one
 * bit; the exit is 42 only when all eight hold, otherwise the bitmask of the
 * groups that did.
 *
 *   1  exact-width     INTN_MIN/INTN_MAX/UINTN_MAX: each typedef's range, in the
 *                      typedef's PROMOTED type (C 7.22.5 by 5.2.5.3)
 *   2  minimum-width   the same for INT_LEASTN_* / UINT_LEASTN_MAX
 *   3  fastest         the same for INT_FASTN_* / UINT_FASTN_MAX — whose types
 *                      are the platform's (long under glibc, short/int elsewhere)
 *   4  pointer, greatest  INTPTR_*, UINTPTR_MAX, INTMAX_*, UINTMAX_MAX
 *   5  other types     PTRDIFF_*, SIZE_MAX, WCHAR_*, WINT_*, and SIG_ATOMIC_*
 *                      exactly where <signal.h> is (C 7.22.3p2)
 *   6  #if             the limits in conditional inclusion agree with the types
 *   7  widths          every *_WIDTH is its type's width in bits, as an int (C23)
 *   8  version         __STDC_VERSION_STDINT_H__ is 202311L (C23 7.22p5)
 *
 * Every check is a RELATION — a limit against its typedef's own size and
 * signedness, a limit's type against the typedef's promoted type — so the file is
 * the same on every platform; the platforms' ABSOLUTE answers are the unit test's
 * (tests/analysis/preprocess/test_stdint_limit_macros.cpp).
 *
 * THE REFERENCES' DOCUMENTED DEVIATIONS stand behind their own guards (MEASURED
 * 2026-09-24; under DSS every check runs): MSVC 14.51 types the 8- and 16-bit
 * limits by its `i8`/`ui8`/`i16`/`ui16` suffixes (`INT8_MAX` is a `char`), and the
 * `#if` values of its `_MIN` macros disagree with their C values; mingw-w64 13.2.0
 * types WCHAR_MIN/MAX and WINT_MIN/MAX `unsigned int` where C 7.22.5 gives the
 * promoted type of `unsigned short`, `int`. Neither pe reference defines the C23
 * width macros, and no reference yet defines __STDC_VERSION_STDINT_H__.
 */
#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER) && !defined(__DSSCP__)
#define SUFFIXED_NARROW_LIMITS 1
#else
#define SUFFIXED_NARROW_LIMITS 0
#endif
#if defined(__MINGW32__) && !defined(__DSSCP__)
#define WIDE_CHAR_LIMITS_UNSIGNED 1
#else
#define WIDE_CHAR_LIMITS_UNSIGNED 0
#endif
#if defined(__DSSCP__) || defined(INT8_WIDTH)
#define HAVE_WIDTHS 1
#else
#define HAVE_WIDTHS 0
#endif
/* Whether <signal.h> — and so `sig_atomic_t` — exists here; left UNDEFINED where
 * the implementation cannot say (no __has_include), so neither arm is asserted. */
#if defined(__has_include)
#if __has_include(<signal.h>)
#include <signal.h>
#define HAVE_SIG_ATOMIC 1
#else
#define HAVE_SIG_ATOMIC 0
#endif
#endif

/* The integer-promotion rank of an expression's type, 0 for anything else. */
#define TCODE(e) _Generic((e), int: 1, unsigned int: 2, long: 3, unsigned long: 4, \
                               long long: 5, unsigned long long: 6, default: 0)
/* M has the type a T object has after the integer promotions. */
#define PROMO(M, T) (TCODE(M) != 0 && TCODE(M) == TCODE(+(T)0))
/* A type's range from its size alone (two's complement, no padding bits). */
#define SMAX(T) ((long long)((~0ULL) >> (65 - 8 * sizeof(T))))
#define UMAX(T) ((unsigned long long)((~0ULL) >> (64 - 8 * sizeof(T))))
/* A signed typedef's two limits, and an unsigned one's maximum; `typed` gates the
 * type checks (a reference's documented deviation turns them off, never DSS). */
#define SLIM(T, MN, MX, typed) \
    ((MX) == SMAX(T) && (MN) == -(MX) - 1 && (!(typed) || (PROMO(MX, T) && PROMO(MN, T))))
#define ULIM(T, MX, typed) ((MX) == UMAX(T) && (!(typed) || PROMO(MX, T)))

/* ── 6: the limits in #if ──────────────────────────────────────────────────── */
#if INT8_MAX == 127 && UINT8_MAX == 255 && INT16_MAX == 32767 && UINT16_MAX == 65535 \
    && INT32_MAX == 2147483647 && UINT32_MAX == 4294967295 \
    && INT64_MAX == 9223372036854775807 && UINT64_MAX == 18446744073709551615u
#define PP_EXACT 1
#else
#define PP_EXACT 0
#endif
#if SUFFIXED_NARROW_LIMITS
#define PP_MINS 1
#elif INT8_MIN == -128 && INT16_MIN == -32768 && INT32_MIN == -2147483647 - 1 \
    && INT64_MIN == -9223372036854775807 - 1 && INT_LEAST8_MIN == -128 \
    && INTMAX_MIN == -INTMAX_MAX - 1 && PTRDIFF_MIN == -PTRDIFF_MAX - 1
#define PP_MINS 1
#else
#define PP_MINS 0
#endif
/* An unsigned-typed limit is unsigned in #if too (C 6.10.2): `M - M - 1` wraps to
 * the maximum there, where an int-typed limit's is -1. */
#define PP_IS_UNSIGNED(M) ((M) - (M) - 1 > 0)
#if PP_IS_UNSIGNED(UINT32_MAX) && PP_IS_UNSIGNED(UINT64_MAX) && PP_IS_UNSIGNED(SIZE_MAX) \
    && PP_IS_UNSIGNED(UINTMAX_MAX)
#define PP_UNSIGNED_LIMITS 1
#else
#define PP_UNSIGNED_LIMITS 0
#endif
#if SUFFIXED_NARROW_LIMITS
#define PP_INT_LIMITS 1
#elif !PP_IS_UNSIGNED(UINT8_MAX) && !PP_IS_UNSIGNED(UINT16_MAX) \
    && !PP_IS_UNSIGNED(UINT_LEAST16_MAX)
#define PP_INT_LIMITS 1
#else
#define PP_INT_LIMITS 0
#endif
/* Platform-dependent, but each must agree with its type at run time (group 6). */
#if SIZE_MAX > 0xffffffffu
#define PP_SIZE_64 1
#else
#define PP_SIZE_64 0
#endif
#if INT_FAST16_MAX > 32767
#define PP_FAST16_WIDE 1
#else
#define PP_FAST16_WIDE 0
#endif
#if WCHAR_MIN < 0
#define PP_WCHAR_SIGNED 1
#else
#define PP_WCHAR_SIGNED 0
#endif
#if INTPTR_MAX >= 9223372036854775807 && UINTMAX_MAX >= 18446744073709551615u
#define PP_WIDE_PTR_MAX 1
#else
#define PP_WIDE_PTR_MAX 0
#endif

static volatile int vZero = 0;

int main(void) {
    int bits = 0;
    int const narrow = !SUFFIXED_NARROW_LIMITS;   /* 8/16-bit type checks */

    /* ── 1: exact width ──────────────────────────────────────────────────── */
    if (SLIM(int8_t, INT8_MIN, INT8_MAX, narrow) && ULIM(uint8_t, UINT8_MAX, narrow)
        && SLIM(int16_t, INT16_MIN, INT16_MAX, narrow) && ULIM(uint16_t, UINT16_MAX, narrow)
        && SLIM(int32_t, INT32_MIN, INT32_MAX, 1) && ULIM(uint32_t, UINT32_MAX, 1)
        && SLIM(int64_t, INT64_MIN, INT64_MAX, 1) && ULIM(uint64_t, UINT64_MAX, 1))
        bits |= 1;

    /* ── 2: minimum width ────────────────────────────────────────────────── */
    if (SLIM(int_least8_t, INT_LEAST8_MIN, INT_LEAST8_MAX, narrow)
        && ULIM(uint_least8_t, UINT_LEAST8_MAX, narrow)
        && SLIM(int_least16_t, INT_LEAST16_MIN, INT_LEAST16_MAX, narrow)
        && ULIM(uint_least16_t, UINT_LEAST16_MAX, narrow)
        && SLIM(int_least32_t, INT_LEAST32_MIN, INT_LEAST32_MAX, 1)
        && ULIM(uint_least32_t, UINT_LEAST32_MAX, 1)
        && SLIM(int_least64_t, INT_LEAST64_MIN, INT_LEAST64_MAX, 1)
        && ULIM(uint_least64_t, UINT_LEAST64_MAX, 1))
        bits |= 2;

    /* ── 3: fastest ──────────────────────────────────────────────────────── */
    if (SLIM(int_fast8_t, INT_FAST8_MIN, INT_FAST8_MAX, narrow)
        && ULIM(uint_fast8_t, UINT_FAST8_MAX, narrow)
        && SLIM(int_fast16_t, INT_FAST16_MIN, INT_FAST16_MAX, 1)
        && ULIM(uint_fast16_t, UINT_FAST16_MAX, 1)
        && SLIM(int_fast32_t, INT_FAST32_MIN, INT_FAST32_MAX, 1)
        && ULIM(uint_fast32_t, UINT_FAST32_MAX, 1)
        && SLIM(int_fast64_t, INT_FAST64_MIN, INT_FAST64_MAX, 1)
        && ULIM(uint_fast64_t, UINT_FAST64_MAX, 1))
        bits |= 4;

    /* ── 4: pointer, greatest ────────────────────────────────────────────── */
    if (SLIM(intptr_t, INTPTR_MIN, INTPTR_MAX, 1) && ULIM(uintptr_t, UINTPTR_MAX, 1)
        && SLIM(intmax_t, INTMAX_MIN, INTMAX_MAX, 1) && ULIM(uintmax_t, UINTMAX_MAX, 1)
        && INTMAX_MAX >= INT64_MAX && UINTMAX_MAX >= UINT64_MAX)
        bits |= 8;

    /* ── 5: the types of other headers ───────────────────────────────────── */
    {
        int ok = SLIM(ptrdiff_t, PTRDIFF_MIN, PTRDIFF_MAX, 1) && ULIM(size_t, SIZE_MAX, 1);
        /* wchar_t: signed or not is the platform's; its _MIN is 0 when unsigned. */
        if ((wchar_t)-1 < 0) {
            ok = ok && SLIM(wchar_t, WCHAR_MIN, WCHAR_MAX, !WIDE_CHAR_LIMITS_UNSIGNED);
        } else {
            ok = ok && WCHAR_MIN == 0 && ULIM(wchar_t, WCHAR_MAX, !WIDE_CHAR_LIMITS_UNSIGNED)
                 && (WIDE_CHAR_LIMITS_UNSIGNED || PROMO(WCHAR_MIN, wchar_t));
        }
        /* wint_t: no shipped header declares it everywhere, so its limits are
         * checked against each other: a 16- or 32-bit type's range, its _MIN 0
         * exactly when it is unsigned. */
        if (WINT_MIN == 0) {
            ok = ok && (WINT_MAX == 65535 || WINT_MAX == 4294967295u);
        } else {
            ok = ok && WINT_MIN == -WINT_MAX - 1 && WINT_MAX == 2147483647;
        }
#if defined(HAVE_SIG_ATOMIC) && HAVE_SIG_ATOMIC
        ok = ok && SLIM(sig_atomic_t, SIG_ATOMIC_MIN, SIG_ATOMIC_MAX, 1);
#elif defined(HAVE_SIG_ATOMIC) \
    && (defined(SIG_ATOMIC_MIN) || defined(SIG_ATOMIC_MAX) || defined(SIG_ATOMIC_WIDTH))
        ok = 0;   /* C 7.22.3p2: no sig_atomic_t, no SIG_ATOMIC_* */
#endif
        if (ok) bits |= 16;
    }

    /* ── 6: #if agrees with the types ────────────────────────────────────── */
    if (PP_EXACT && PP_MINS && PP_UNSIGNED_LIMITS && PP_INT_LIMITS
        && PP_SIZE_64 == (sizeof(size_t) == 8)
        && PP_FAST16_WIDE == (sizeof(int_fast16_t) > 2)
        && PP_WCHAR_SIGNED == ((wchar_t)-1 < 0)
        && PP_WIDE_PTR_MAX == (sizeof(intptr_t) == 8))
        bits |= 32;

    /* ── 7: widths (C23 7.22.2, 7.22.3) ──────────────────────────────────── */
#if HAVE_WIDTHS
#define WIDTH_OF(W, T) ((W) == 8 * (int)sizeof(T) && TCODE(W) == 1)
    if (WIDTH_OF(INT8_WIDTH, int8_t) && WIDTH_OF(UINT8_WIDTH, uint8_t)
        && WIDTH_OF(INT16_WIDTH, int16_t) && WIDTH_OF(UINT16_WIDTH, uint16_t)
        && WIDTH_OF(INT32_WIDTH, int32_t) && WIDTH_OF(UINT32_WIDTH, uint32_t)
        && WIDTH_OF(INT64_WIDTH, int64_t) && WIDTH_OF(UINT64_WIDTH, uint64_t)
        && WIDTH_OF(INT_LEAST8_WIDTH, int_least8_t) && WIDTH_OF(UINT_LEAST64_WIDTH, uint_least64_t)
        && WIDTH_OF(INT_FAST16_WIDTH, int_fast16_t) && WIDTH_OF(UINT_FAST32_WIDTH, uint_fast32_t)
        && WIDTH_OF(INTPTR_WIDTH, intptr_t) && WIDTH_OF(UINTPTR_WIDTH, uintptr_t)
        && WIDTH_OF(INTMAX_WIDTH, intmax_t) && WIDTH_OF(UINTMAX_WIDTH, uintmax_t)
        && WIDTH_OF(PTRDIFF_WIDTH, ptrdiff_t) && WIDTH_OF(SIZE_WIDTH, size_t)
        && WIDTH_OF(WCHAR_WIDTH, wchar_t)
        && (WINT_WIDTH == 16 || WINT_WIDTH == 32) && TCODE(WINT_WIDTH) == 1
#if defined(HAVE_SIG_ATOMIC) && HAVE_SIG_ATOMIC
        && WIDTH_OF(SIG_ATOMIC_WIDTH, sig_atomic_t)
#endif
        )
        bits |= 64;
#else
    bits |= 64;   /* a reference without C23's width macros: nothing to check */
#endif

    /* ── 8: the header's version (C23 7.22p5) ────────────────────────────── */
#if defined(__DSSCP__) || defined(__STDC_VERSION_STDINT_H__)
    if (__STDC_VERSION_STDINT_H__ == 202311L && TCODE(__STDC_VERSION_STDINT_H__) == 3)
        bits |= 128;
#else
    bits |= 128;   /* no reference defines it yet */
#endif

    return (bits == 255 ? 42 : bits) + vZero;
}
