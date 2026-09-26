/* The `__SIZEOF_*__` family — every member predefined, and each equal to
 * `sizeof` of the type it names (D-C-SIZEOF-PREDEFINED-MACRO-FAMILY-MISSING).
 *
 * ★★★ WHY THIS EXAMPLE EXISTS. gcc and clang predefine these thirteen names on
 * every 64-bit triple; DSS defined one of them. An undefined name in `#if` is 0,
 * so a program sizing a buffer or choosing a representation on one took its
 * fallback arm under DSS, silently, and one that required it stopped at its
 * `#error`. ✔MEASURED 2026-09-21 at the P68 round-8 part-4 base: this file
 * stopped at its first `#error` (`__SIZEOF_SHORT__`) on every target.
 *
 * Each macro is the C language's `type-size` row: it names a TYPE, and DSS
 * computes its value for the build's (target, format) from the same layout
 * `sizeof` uses — so the compile-time checks below ask DSS's preprocessor and
 * DSS's `sizeof` one question twice. `wchar_t` is checked through BOTH of its
 * carriers, the `<stddef.h>` typedef and a wide character constant.
 * `__SIZEOF_WINT_T__` has no `wint_t` to compare with here (`<wchar.h>` is not
 * shipped); C makes `wint_t` hold every `wchar_t` value and `WEOF`, so it can
 * be no narrower. `__SIZEOF_FLOAT128__`/`__SIZEOF_FLOAT80__` are deliberately
 * NOT checked: DSS has no such type yet, and leaves them undefined.
 *
 * Exit codes:
 *   11  a pointer does not survive a copy of exactly `__SIZEOF_POINTER__` bytes
 *   12  an array of `wchar_t` does not stride by `__SIZEOF_WCHAR_T__`
 *   13  an all-ones `long` does not fill `__SIZEOF_LONG__` bytes
 *   14  an array of `__int128` does not stride by `__SIZEOF_INT128__`
 *   15  a `size_t` holding `~0` does not fill `__SIZEOF_SIZE_T__` bytes
 *   42  every check passed
 * The compile-time checks are the `#error`s and the `_Static_assert`s: a
 * missing macro, or one that disagrees with its type, stops the build on every
 * target, in every pipeline.
 *
 * ★ The `release` arm: every run-time value comes from a `volatile`, so the
 * optimized pipeline still has to move the bytes. */

#include <stddef.h>

#ifndef __SIZEOF_SHORT__
#error "__SIZEOF_SHORT__ is not predefined"
#endif
#ifndef __SIZEOF_INT__
#error "__SIZEOF_INT__ is not predefined"
#endif
#ifndef __SIZEOF_LONG__
#error "__SIZEOF_LONG__ is not predefined"
#endif
#ifndef __SIZEOF_LONG_LONG__
#error "__SIZEOF_LONG_LONG__ is not predefined"
#endif
#ifndef __SIZEOF_FLOAT__
#error "__SIZEOF_FLOAT__ is not predefined"
#endif
#ifndef __SIZEOF_DOUBLE__
#error "__SIZEOF_DOUBLE__ is not predefined"
#endif
#ifndef __SIZEOF_LONG_DOUBLE__
#error "__SIZEOF_LONG_DOUBLE__ is not predefined"
#endif
#ifndef __SIZEOF_POINTER__
#error "__SIZEOF_POINTER__ is not predefined"
#endif
#ifndef __SIZEOF_SIZE_T__
#error "__SIZEOF_SIZE_T__ is not predefined"
#endif
#ifndef __SIZEOF_PTRDIFF_T__
#error "__SIZEOF_PTRDIFF_T__ is not predefined"
#endif
#ifndef __SIZEOF_WCHAR_T__
#error "__SIZEOF_WCHAR_T__ is not predefined"
#endif
#ifndef __SIZEOF_WINT_T__
#error "__SIZEOF_WINT_T__ is not predefined"
#endif
#ifndef __SIZEOF_INT128__
#error "__SIZEOF_INT128__ is not predefined"
#endif

#define SIZE_IS(T, M) _Static_assert(sizeof(T) == (M), #M " must be sizeof(" #T ")")
SIZE_IS(short, __SIZEOF_SHORT__);
SIZE_IS(int, __SIZEOF_INT__);
SIZE_IS(long, __SIZEOF_LONG__);
SIZE_IS(long long, __SIZEOF_LONG_LONG__);
SIZE_IS(float, __SIZEOF_FLOAT__);
SIZE_IS(double, __SIZEOF_DOUBLE__);
SIZE_IS(long double, __SIZEOF_LONG_DOUBLE__);
SIZE_IS(void *, __SIZEOF_POINTER__);
SIZE_IS(size_t, __SIZEOF_SIZE_T__);
SIZE_IS(ptrdiff_t, __SIZEOF_PTRDIFF_T__);
SIZE_IS(wchar_t, __SIZEOF_WCHAR_T__);
SIZE_IS(L'a', __SIZEOF_WCHAR_T__);
SIZE_IS(__int128, __SIZEOF_INT128__);
_Static_assert(__SIZEOF_WINT_T__ >= __SIZEOF_WCHAR_T__,
               "wint_t holds every wchar_t value and WEOF");

/* The preprocessor's own arithmetic on the macros — the use a program makes of
 * them, and the one an undefined name silently answered with 0. */
#if __SIZEOF_POINTER__ * 8 != 64
#error "a 64-bit target's pointer is not 8 bytes by __SIZEOF_POINTER__"
#endif
#if __SIZEOF_LONG_LONG__ < __SIZEOF_LONG__ || __SIZEOF_LONG__ < __SIZEOF_INT__
#error "the integer ladder is out of order by its own macros"
#endif

static volatile long          all_ones_long = -1L;
static volatile size_t        all_ones_size = ~(size_t)0;
static volatile unsigned char pointee;

/* How many of the object's leading bytes are 0xFF. */
static int ff_bytes(unsigned char const *p, int n) {
    int i, k = 0;
    for (i = 0; i < n; ++i) k += (p[i] == 0xFF);
    return k;
}

int main(void) {
    unsigned char bytes[__SIZEOF_POINTER__];
    unsigned char volatile *src = &pointee;
    unsigned char volatile *back = 0;
    wchar_t wide[3];
    __int128 wides[3];
    long l = all_ones_long;
    size_t s = all_ones_size;
    int i;

    for (i = 0; i < __SIZEOF_POINTER__; ++i)
        bytes[i] = ((unsigned char const *)&src)[i];
    for (i = 0; i < __SIZEOF_POINTER__; ++i)
        ((unsigned char *)&back)[i] = bytes[i];
    if (back != src) return 11;

    if ((unsigned char *)&wide[2] - (unsigned char *)&wide[0]
        != 2 * __SIZEOF_WCHAR_T__) return 12;

    if (ff_bytes((unsigned char const *)&l, __SIZEOF_LONG__) != __SIZEOF_LONG__)
        return 13;

    if ((unsigned char *)&wides[2] - (unsigned char *)&wides[0]
        != 2 * __SIZEOF_INT128__) return 14;

    if (ff_bytes((unsigned char const *)&s, __SIZEOF_SIZE_T__) != __SIZEOF_SIZE_T__)
        return 15;

    return 42;
}
