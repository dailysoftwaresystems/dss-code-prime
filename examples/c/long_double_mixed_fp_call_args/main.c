/* D-LIR-AAPCS64-CALL-MIXING-LONG-DOUBLE-AND-DOUBLE-ARGS-REFUSED — a call that
 * passes a `long double` AND a `float`/`double` must place every argument in
 * its own register.
 *
 * Under AAPCS64 (aarch64 Linux) a `long double` is IEEE binary128 and rides a
 * whole Q register; it shares ONE floating-point argument sequence (the NSRN)
 * with `float` and `double`. DSS placed the binary128 arguments with one walk
 * and every other argument with another, and the two walks shared no cursor —
 * so a mix was REFUSED at the caller ("MIR opcode '<deferred>' is not yet
 * lowered"), debug and release. aarch64-linux-gnu-gcc 13.3.0 and clang 18.1.3
 * run this file to 42 at -O0 and -O2 under qemu-aarch64.
 *
 * Seven shapes: the long double first, the long double second, an interleaved
 * mix with a float and integer arguments between, a mix whose LAST double
 * overflows onto the stack once eight FP registers are taken (its placement
 * must count the long double), an indirect call through a function pointer,
 * a mix whose RESULT is a long double, and one long double beside THIRTY
 * doubles all live at the call (the allocator must not park one in the
 * register the long double is loaded into). The callee checks every argument
 * against the value the caller passed; each failing shape has its own exit
 * code. Every value is exact in binary32, binary64, x87 and binary128, so the
 * same file is meaningful where `long double` is a `double` (Apple arm64, the
 * Windows x64 ABI) and where it is x87 (x86_64 Linux) — there it runs as the
 * control the aarch64 arm is compared against.
 */

__attribute__((noinline)) static int second(long double a, double b) {
    return a == 40.0L && b == 2.0;
}

__attribute__((noinline)) static int first(double b, long double a) {
    return a == 40.0L && b == 2.0;
}

__attribute__((noinline)) static int mix(long double a, float f, int i, double d,
                                         long double b, long j, double e) {
    return a == 1.5L && f == 2.25f && i == 3 && d == 4.5 && b == -6.75L
        && j == 7 && e == 8.125;
}

__attribute__((noinline)) static int many(double a0, double a1, double a2,
                                          long double l, double a4, double a5,
                                          double a6, double a7, double s) {
    return a0 == 0.5 && a1 == 1.5 && a2 == 2.5 && l == 3.5L && a4 == 4.5
        && a5 == 5.5 && a6 == 6.5 && a7 == 7.5 && s == 8.5;
}

static int (*volatile indirect)(long double, double) = second;

__attribute__((noinline)) static long double both(double d, long double a) {
    return a + (long double)d;
}

/* The long double in v0 and THIRTY doubles after it, every one computed in the
 * caller — so all of them are live while the long double is loaded into v0,
 * and the allocator has to use the low FP registers for some of them. A
 * marshal that does not tell the allocator it writes v0 lets it park a double
 * there and then overwrites it (the double that arrives wrong is the ninth).
 * AArch64 only: on x86_64 an x87 `long double` rides the stack, and a double
 * overflowing onto the stack after it is a separate, recorded limitation
 * (D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS). */
#if defined(__aarch64__)
__attribute__((noinline)) static int wide(long double l,
        double d0, double d1, double d2, double d3, double d4, double d5,
        double d6, double d7, double d8, double d9, double d10, double d11,
        double d12, double d13, double d14, double d15, double d16, double d17,
        double d18, double d19, double d20, double d21, double d22, double d23,
        double d24, double d25, double d26, double d27, double d28, double d29) {
    double const got[30] = { d0, d1, d2, d3, d4, d5, d6, d7, d8, d9, d10, d11,
                             d12, d13, d14, d15, d16, d17, d18, d19, d20, d21,
                             d22, d23, d24, d25, d26, d27, d28, d29 };
    if (l != 7.25L) return 0;
    for (int i = 0; i < 30; ++i) {
        if (got[i] != (double)i + 0.5) return 0;
    }
    return 1;
}

__attribute__((noinline)) static int wide_caller(double k) {
    double v0 = 0.5 + k, v1 = 1.5 + k, v2 = 2.5 + k, v3 = 3.5 + k, v4 = 4.5 + k;
    double v5 = 5.5 + k, v6 = 6.5 + k, v7 = 7.5 + k, v8 = 8.5 + k, v9 = 9.5 + k;
    double v10 = 10.5 + k, v11 = 11.5 + k, v12 = 12.5 + k, v13 = 13.5 + k;
    double v14 = 14.5 + k, v15 = 15.5 + k, v16 = 16.5 + k, v17 = 17.5 + k;
    double v18 = 18.5 + k, v19 = 19.5 + k, v20 = 20.5 + k, v21 = 21.5 + k;
    double v22 = 22.5 + k, v23 = 23.5 + k, v24 = 24.5 + k, v25 = 25.5 + k;
    double v26 = 26.5 + k, v27 = 27.5 + k, v28 = 28.5 + k, v29 = 29.5 + k;
    return wide(7.25L, v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12,
                v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24,
                v25, v26, v27, v28, v29);
}

static double volatile zero = 0.0;
#endif

int main(void) {
    if (!second(40.0L, 2.0)) return 11;
    if (!first(2.0, 40.0L)) return 12;
    if (!mix(1.5L, 2.25f, 3, 4.5, -6.75L, 7, 8.125)) return 13;
    if (!many(0.5, 1.5, 2.5, 3.5L, 4.5, 5.5, 6.5, 7.5, 8.5)) return 14;
    if (!indirect(40.0L, 2.0)) return 15;
    if (both(2.0, 40.0L) != 42.0L) return 16;
#if defined(__aarch64__)
    if (!wide_caller(zero)) return 17;
#endif
    return 42;
}
