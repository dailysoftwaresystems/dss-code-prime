/* `__SIZEOF_LONG_DOUBLE__` — predefined, and equal to `sizeof(long double)`.
 * D-C-SIZEOF-LONG-DOUBLE-NOT-PREDEFINED.
 *
 * ★★★ WHY THIS EXAMPLE EXISTS. gcc and clang predefine `__SIZEOF_LONG_DOUBLE__`
 * and DSS did not. An undefined name in `#if` is 0, so a program selecting a
 * representation on it took its fallback arm under DSS on every target, and
 * one that required it stopped at its `#error`. ✔MEASURED 2026-09-19 at the
 * P68 round-7 base: this file stopped at the `#error` below on all four
 * targets. The size follows each object format's `long double` — binary128 in
 * 16 bytes on aarch64 Linux, x87 in 16 on x86_64 Linux and Darwin, binary64 in
 * 8 on Apple arm64 and on Windows (the MSVC ABI). ⓘ It was first stated as a
 * number in each format document; since P68 round 8 part 4 the C language's
 * one `type-size` row names the TYPE and each build derives the number from
 * the format's `longDoubleFormat`, as `sizeof` does
 * (D-C-SIZEOF-PREDEFINED-MACRO-FAMILY-MISSING).
 *
 * Exit codes:
 *   11  a `long double` does not survive a copy of exactly
 *       `__SIZEOF_LONG_DOUBLE__` bytes (the stated size is too small)
 *   12  an array of `long double` does not stride by the stated size
 *   13  the `#if` ladder chose an arm that disagrees with `sizeof`
 *   42  every check passed
 * The compile-time check is the `_Static_assert`: a stated size that is not
 * the type's size stops the build, on every target, in every pipeline.
 *
 * ★ The `release` arm: the value copied comes from a `volatile`, so the
 * optimized pipeline still has to move the bytes at run time. */

#ifndef __SIZEOF_LONG_DOUBLE__
#error "__SIZEOF_LONG_DOUBLE__ is not predefined"
#endif

_Static_assert(sizeof(long double) == __SIZEOF_LONG_DOUBLE__,
               "__SIZEOF_LONG_DOUBLE__ must be the size of long double");

/* The preprocessor's own arithmetic on the macro — the use a program makes of
 * it, and the one an undefined name silently answered with 0. */
#if __SIZEOF_LONG_DOUBLE__ == 16
#define LD_ARM_BYTES 16
#elif __SIZEOF_LONG_DOUBLE__ == 8
#define LD_ARM_BYTES 8
#else
#error "__SIZEOF_LONG_DOUBLE__ is neither 8 nor 16"
#endif

typedef union {
    long double   value;
    unsigned char bytes[__SIZEOF_LONG_DOUBLE__];
} ld_bytes;

static volatile long double seed = 2.75L;

int main(void) {
    ld_bytes from, to;
    long double row[3];
    int i;

    from.value = seed;
    for (i = 0; i < __SIZEOF_LONG_DOUBLE__; ++i) to.bytes[i] = from.bytes[i];
    if (to.value != seed) return 11;

    if ((unsigned char *)&row[2] - (unsigned char *)&row[0]
        != 2 * __SIZEOF_LONG_DOUBLE__) return 12;

    if (LD_ARM_BYTES != sizeof(long double)) return 13;

    return 42;
}
