/* The GNU integer-type predefined macros — `__*_TYPE__`, `__*_MAX__`/`__*_MIN__`,
 * `__*_WIDTH__`, `__*_C(c)` and `__*_C_SUFFIX__` — used the way headers and
 * programs use them: as types in declarations, as values in expressions and in
 * #if, and as literal builders. Each group sets one bit; the exit is 42 only when
 * all six hold, otherwise the bitmask of the groups that did.
 *
 *   1  each `__X_TYPE__` IS the type of the typedef it is documented to underlie
 *   2  each `__X_MAX__`/`__X_MIN__` is its type's range, in its PROMOTED type
 *   3  the limits in #if agree with the types at run time
 *   4  each `__X_WIDTH__` is its type's width in bits
 *   5  `__X_C(v)` / `__X_C_SUFFIX__` build a constant of the promoted type
 *   6  the values survive a round trip through volatile objects of the types
 *
 * THE REFERENCES DEFINE DIFFERENT SUBSETS, so the names only some of them define
 * stand behind #ifdef: clang has no `__SIG_ATOMIC_TYPE__` (on any triple) and, on
 * Linux, no `__CHAR8_TYPE__` and no function-like `__INT64_C`; gcc and mingw have
 * no `__*_C_SUFFIX__`, `__POINTER_WIDTH__`, `__LLONG_WIDTH__`. ONE documented
 * DISAGREEMENT stands behind its own guard: clang 18 on Linux defines
 * `__INT_FAST16_TYPE__`/`__INT_FAST32_TYPE__` as `short`/`int` while glibc's own
 * <stdint.h> says `long` for both typedefs (gcc agrees with glibc); DSS follows
 * the shipped typedef (P68 round 9), so the check runs under DSS always.
 * MSVC defines none of these macros (it is not a GNU compiler); DSS's pe pair
 * presents mingw's identity, whose answers are the pe row here.
 */
#include <stddef.h>
#include <stdint.h>

#define IS(T, E) _Generic((E), T: 1, default: 0)
#define CAT_(a, b) a##b
#define CAT(a, b) CAT_(a, b)

/* The value of a signed/unsigned type's maximum, from its size alone (two's
 * complement, no padding bits — true of every target here). */
#define SMAX(T) ((long long)((~0ULL) >> (65 - 8 * sizeof(T))))
#define UMAX(T) ((unsigned long long)((~0ULL) >> (64 - 8 * sizeof(T))))

/* clang on Linux: the documented fast16/fast32 disagreement with glibc. */
#if defined(__clang__) && defined(__linux__) && !defined(__DSSCP__)
#define FAST_MACROS_FOLLOW_TYPEDEFS 0
#else
#define FAST_MACROS_FOLLOW_TYPEDEFS 1
#endif

/* ── 3: the limits in #if ──────────────────────────────────────────────────── */
#if __INT64_MAX__ == 0x7fffffffffffffff && __UINT64_MAX__ == 0xffffffffffffffffU
#define PP_64_OK 1
#else
#define PP_64_OK 0
#endif
#if __LONG_MAX__ > 0x7fffffff
#define PP_LONG_IS_64 1
#else
#define PP_LONG_IS_64 0
#endif
#if __SIZE_MAX__ > 0xffffffffU
#define PP_SIZE_IS_64 1
#else
#define PP_SIZE_IS_64 0
#endif
#if __INT_FAST32_MAX__ > 0x7fffffff
#define PP_FAST32_IS_64 1
#else
#define PP_FAST32_IS_64 0
#endif
#if -1 < __UINT32_MAX__
#define PP_U32_SIGNED 1   /* would be wrong: __UINT32_MAX__ is unsigned int */
#else
#define PP_U32_SIGNED 0
#endif

static volatile int vOne = 1;

int main(void) {
    int bits = 0;

    /* ── 1: `__X_TYPE__` IS the typedef ─────────────────────────────────── */
    {
        int ok = IS(int8_t, (__INT8_TYPE__)0) && IS(int16_t, (__INT16_TYPE__)0)
              && IS(int32_t, (__INT32_TYPE__)0) && IS(int64_t, (__INT64_TYPE__)0)
              && IS(uint8_t, (__UINT8_TYPE__)0) && IS(uint16_t, (__UINT16_TYPE__)0)
              && IS(uint32_t, (__UINT32_TYPE__)0) && IS(uint64_t, (__UINT64_TYPE__)0)
              && IS(int_least8_t, (__INT_LEAST8_TYPE__)0)
              && IS(int_least64_t, (__INT_LEAST64_TYPE__)0)
              && IS(uint_least16_t, (__UINT_LEAST16_TYPE__)0)
              && IS(int_fast8_t, (__INT_FAST8_TYPE__)0)
              && IS(int_fast64_t, (__INT_FAST64_TYPE__)0)
              && IS(uint_fast64_t, (__UINT_FAST64_TYPE__)0)
              && IS(intptr_t, (__INTPTR_TYPE__)0) && IS(uintptr_t, (__UINTPTR_TYPE__)0)
              && IS(intmax_t, (__INTMAX_TYPE__)0) && IS(uintmax_t, (__UINTMAX_TYPE__)0)
              && IS(ptrdiff_t, (__PTRDIFF_TYPE__)0) && IS(size_t, (__SIZE_TYPE__)0)
              && IS(wchar_t, (__WCHAR_TYPE__)0)
              && IS(uint_least16_t, (__CHAR16_TYPE__)0)
              && IS(uint_least32_t, (__CHAR32_TYPE__)0);
#if FAST_MACROS_FOLLOW_TYPEDEFS
        ok = ok && IS(int_fast16_t, (__INT_FAST16_TYPE__)0)
                && IS(int_fast32_t, (__INT_FAST32_TYPE__)0)
                && IS(uint_fast16_t, (__UINT_FAST16_TYPE__)0)
                && IS(uint_fast32_t, (__UINT_FAST32_TYPE__)0);
#endif
#ifdef __CHAR8_TYPE__
        ok = ok && IS(unsigned char, (__CHAR8_TYPE__)0);
#endif
#ifdef __SIG_ATOMIC_TYPE__
        ok = ok && sizeof(__SIG_ATOMIC_TYPE__) == sizeof(int)
                && IS(int, (__SIG_ATOMIC_TYPE__)0);
#endif
        __SIZE_TYPE__ const n = sizeof(__PTRDIFF_TYPE__);   /* usable in declarations */
        if (ok && n == sizeof(ptrdiff_t)) bits |= 1;
    }

    /* ── 2: the range, in the promoted type ─────────────────────────────── */
    {
        int ok = __INT8_MAX__ == SMAX(int8_t) && __INT16_MAX__ == SMAX(int16_t)
              && __INT32_MAX__ == SMAX(int32_t) && __INT64_MAX__ == SMAX(int64_t)
              && __UINT8_MAX__ == UMAX(uint8_t) && __UINT16_MAX__ == UMAX(uint16_t)
              && __UINT32_MAX__ == UMAX(uint32_t) && __UINT64_MAX__ == UMAX(uint64_t)
              && __INT_LEAST32_MAX__ == SMAX(int_least32_t)
              && __UINT_LEAST64_MAX__ == UMAX(uint_least64_t)
              && __INT_FAST8_MAX__ == SMAX(int_fast8_t)
              && __INTPTR_MAX__ == SMAX(intptr_t) && __UINTPTR_MAX__ == UMAX(uintptr_t)
              && __INTMAX_MAX__ == SMAX(intmax_t) && __UINTMAX_MAX__ == UMAX(uintmax_t)
              && __PTRDIFF_MAX__ == SMAX(ptrdiff_t) && __SIZE_MAX__ == UMAX(size_t)
              && __SCHAR_MAX__ == 127 && __SHRT_MAX__ == 32767
              && __INT_MAX__ == SMAX(int) && __LONG_MAX__ == SMAX(long)
              && __LONG_LONG_MAX__ == SMAX(long long)
              /* the promoted types: narrower than int → int; the rest keep theirs */
              && IS(int, __INT8_MAX__) && IS(int, __UINT8_MAX__) && IS(int, __UINT16_MAX__)
              && IS(unsigned int, __UINT32_MAX__) && IS(__UINT64_TYPE__, __UINT64_MAX__)
              && IS(__SIZE_TYPE__, __SIZE_MAX__) && IS(long, __LONG_MAX__)
              && IS(__INTMAX_TYPE__, __INTMAX_MAX__);
#if FAST_MACROS_FOLLOW_TYPEDEFS
        ok = ok && __INT_FAST16_MAX__ == SMAX(int_fast16_t)
                && __UINT_FAST32_MAX__ == UMAX(uint_fast32_t);
#endif
#ifdef __WCHAR_MIN__
        ok = ok && (__WCHAR_MIN__ == 0) == ((wchar_t)-1 > 0);
#endif
        if (ok) bits |= 2;
    }

    /* ── 3: #if against the types ───────────────────────────────────────── */
    if (PP_64_OK && PP_LONG_IS_64 == (sizeof(long) == 8) && PP_SIZE_IS_64 == (sizeof(size_t) == 8)
        && !PP_U32_SIGNED
#if FAST_MACROS_FOLLOW_TYPEDEFS
        && PP_FAST32_IS_64 == (sizeof(int_fast32_t) == 8)
#endif
    ) {
        bits |= 4;
    }

    /* ── 4: the widths ──────────────────────────────────────────────────── */
    {
        int ok = __INT_WIDTH__ == 8 * (int)sizeof(int)
              && __LONG_WIDTH__ == 8 * (int)sizeof(long)
              && __SIZE_WIDTH__ == 8 * (int)sizeof(size_t)
              && __PTRDIFF_WIDTH__ == 8 * (int)sizeof(ptrdiff_t)
              && __WCHAR_WIDTH__ == 8 * (int)sizeof(wchar_t)
              && __INTMAX_WIDTH__ == 8 * (int)sizeof(intmax_t)
              && __INT_LEAST16_WIDTH__ == 8 * (int)sizeof(int_least16_t)
              && __INT_FAST64_WIDTH__ == 8 * (int)sizeof(int_fast64_t)
              && __SHRT_WIDTH__ == 16 && IS(int, __SIZE_WIDTH__);
#if FAST_MACROS_FOLLOW_TYPEDEFS
        ok = ok && __INT_FAST16_WIDTH__ == 8 * (int)sizeof(int_fast16_t);
#endif
#ifdef __POINTER_WIDTH__
        ok = ok && __POINTER_WIDTH__ == 8 * (int)sizeof(void *);
#endif
#ifdef __LLONG_WIDTH__
        ok = ok && __LLONG_WIDTH__ == 64;
#endif
#ifdef __LONG_LONG_WIDTH__
        ok = ok && __LONG_LONG_WIDTH__ == 64;
#endif
        if (ok) bits |= 8;
    }

    /* ── 5: the literal builders ──────────────────────────────────────────
     * Each form is keyed on `__INT8_*`, never on `__INT64_C`: glibc's own
     * <stdint.h> defines `__INT64_C`/`__UINT64_C` for its internal use, so under
     * clang on Linux (which predefines only the `_SUFFIX__` form) `__INT64_C` is
     * defined after the #include while `__INT8_C` is not. */
    {
        int ok = 1;
#ifdef __INT8_C   /* gcc, mingw, Apple clang, DSS: the ten function-like forms */
        ok = ok && IS(int_least64_t, __INT64_C(1)) && __INT64_C(1) == 1
                && IS(int, __INT8_C(1)) && IS(int, __UINT16_C(1))
                && IS(unsigned int, __UINT32_C(1)) && IS(uint_least64_t, __UINT64_C(1))
                && IS(intmax_t, __INTMAX_C(1)) && IS(uintmax_t, __UINTMAX_C(1));
#endif
#ifdef __INT8_C_SUFFIX__   /* clang, Apple clang, DSS: the ten suffix forms */
        ok = ok && IS(int_least64_t, CAT(1, __INT64_C_SUFFIX__))
                && IS(int, CAT(1, __INT8_C_SUFFIX__))
                && IS(unsigned int, CAT(1, __UINT32_C_SUFFIX__))
                && IS(uintmax_t, CAT(1, __UINTMAX_C_SUFFIX__));
#endif
#if !defined(__INT8_C) && !defined(__INT8_C_SUFFIX__)
        ok = 0;   /* every reference defines one of the two forms */
#endif
        if (ok) bits |= 16;
    }

    /* ── 6: through volatile objects ────────────────────────────────────── */
    {
        volatile __INT64_TYPE__ a = __INT64_MAX__;
        volatile __UINT32_TYPE__ b = __UINT32_MAX__;
        volatile __SIZE_TYPE__ c = __SIZE_MAX__;
        volatile __INTMAX_TYPE__ d = __INTMAX_MAX__;
        /* `a - 1 + 1` never overflows; the wrap past the maximum is asked in the
         * UNSIGNED type, where it is defined. */
        if (a - vOne + vOne == SMAX(int64_t) && b == UMAX(uint32_t)
            && c == UMAX(size_t) && d == SMAX(intmax_t)
            && (__UINT64_TYPE__)a + (__UINT64_TYPE__)vOne
                   == (__UINT64_TYPE__)SMAX(int64_t) + 1u) {
            bits |= 32;
        }
    }

    return bits == 63 ? 42 : bits;
}
