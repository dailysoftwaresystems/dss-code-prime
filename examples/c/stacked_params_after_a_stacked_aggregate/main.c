/* Parameters passed on the stack AFTER a stacked aggregate — and an aggregate
 * stacked after stacked scalars. D-MIR-STACKED-SCALAR-AFTER-A-STACKED-AGGREGATE-REFUSED.
 *
 * ★★★ WHY THIS EXAMPLE EXISTS. Once the argument registers run out, the ABI
 * lays every remaining parameter out in one stack area, in order: a by-value
 * struct that no longer fits its registers, an x87 `long double` (x86_64 SysV
 * always passes it in memory), or an AAPCS64 `_Complex long double` that no
 * longer fits v0..v7 takes its bytes there, and the scalars after it follow it.
 * DSS's callee received a stacked scalar through a cursor that could not see a
 * stacked aggregate, so it REFUSED every function whose stacked scalar followed
 * one — and every aggregate straddle after a stacked scalar. ✔MEASURED
 * 2026-09-19 at the P68 round-7 base: each function below was refused on the
 * callee side on x86_64 Linux, aarch64 Linux or both. gcc 13.3.0 and clang
 * 18.1.3 run this file to 42 at -O0 and -O2 (aarch64 under qemu); with the fix,
 * DSS callees linked against gcc and clang callers — and the reverse — agree
 * with them byte for byte (the interop matrix behind the row).
 *
 * Every argument is a distinct value, so a parameter read from its neighbour's
 * bytes fails its own function's check. Exit codes, one per function:
 *   11  x86_64: six integers, a struct stacked whole, then an integer
 *   12  eight doubles, a `long double`, then a double
 *   13  AAPCS64: seven integers, a struct stacked whole (the class is
 *       exhausted), then an integer
 *   14  a stacked integer, a stacked struct, a stacked integer
 *   15  AAPCS64: seven doubles, a `_Complex long double` stacked whole, a double
 *   16  eight doubles, a `long double`, a two-double struct, a double
 *   17  two `long double`s, then nine doubles
 *   18  a variadic function whose named parameters end past a stacked struct
 *   19  six integers, a `long double`, a struct, then nine floats
 *   42  every check passed
 * ⓘ Portable C: every target runs every function; the shapes discriminate on
 * the two whose registers run out as described. ★ The `release` arm: each
 * function is called through a `volatile` pointer, so the optimized pipeline
 * keeps every call — and its argument passing — real. */

#include <stdarg.h>

typedef long long i64;
typedef struct { i64 a, b; } S16;
typedef struct { double a, b; } D16;

static int f11(i64 a0, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5, S16 s, i64 a6) {
    return a0 == 1000 && a1 == 1007 && a2 == 1014 && a3 == 1021 && a4 == 1028
        && a5 == 1035 && s.a == 2006 && s.b == 3006 && a6 == 1049;
}

static int f12(double d0, double d1, double d2, double d3, double d4, double d5,
               double d6, double d7, long double x, double d8) {
    return d0 == 0.25 && d1 == 1.25 && d2 == 2.25 && d3 == 3.25 && d4 == 4.25
        && d5 == 5.25 && d6 == 6.25 && d7 == 7.25 && x == 8.75L && d8 == 9.25;
}

static int f13(i64 a0, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5, i64 a6, S16 s,
               i64 a7) {
    return a0 == 1000 && a1 == 1007 && a2 == 1014 && a3 == 1021 && a4 == 1028
        && a5 == 1035 && a6 == 1042 && s.a == 2007 && s.b == 3007 && a7 == 1056;
}

static int f14(i64 a0, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5, i64 a6, i64 a7,
               i64 a8, S16 s, i64 a9) {
    return a0 == 1000 && a1 == 1007 && a2 == 1014 && a3 == 1021 && a4 == 1028
        && a5 == 1035 && a6 == 1042 && a7 == 1049 && a8 == 1056 && s.a == 2009
        && s.b == 3009 && a9 == 1070;
}

static int f15(double d0, double d1, double d2, double d3, double d4, double d5,
               double d6, _Complex long double c, double d7) {
    long double const *const part = (long double const *)&c;
    return d0 == 0.25 && d1 == 1.25 && d2 == 2.25 && d3 == 3.25 && d4 == 4.25
        && d5 == 5.25 && d6 == 6.25 && part[0] == 7.5L && part[1] == 7.625L
        && d7 == 8.25;
}

static int f16(double d0, double d1, double d2, double d3, double d4, double d5,
               double d6, double d7, long double x, D16 h, double d8) {
    return d0 == 0.25 && d1 == 1.25 && d2 == 2.25 && d3 == 3.25 && d4 == 4.25
        && d5 == 5.25 && d6 == 6.25 && d7 == 7.25 && x == 8.75L && h.a == 9.125
        && h.b == 9.375 && d8 == 10.25;
}

static int f17(long double x0, long double x1, double d0, double d1, double d2,
               double d3, double d4, double d5, double d6, double d7, double d8) {
    return x0 == 0.75L && x1 == 1.75L && d0 == 2.25 && d1 == 3.25 && d2 == 4.25
        && d3 == 5.25 && d4 == 6.25 && d5 == 7.25 && d6 == 8.25 && d7 == 9.25
        && d8 == 10.25;
}

static int f18(i64 a0, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5, i64 a6, S16 s,
               i64 a7, ...) {
    va_list ap;
    int ok = a0 == 1000 && a1 == 1007 && a2 == 1014 && a3 == 1021 && a4 == 1028
          && a5 == 1035 && a6 == 1042 && s.a == 2007 && s.b == 3007 && a7 == 1056;
    va_start(ap, a7);
    ok = ok && va_arg(ap, i64) == 9000 && va_arg(ap, i64) == 9001
            && va_arg(ap, i64) == 9002;
    va_end(ap);
    return ok;
}

static int f19(i64 a0, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5, long double x,
               S16 s, float f0, float f1, float f2, float f3, float f4, float f5,
               float f6, float f7, float f8) {
    return a0 == 1000 && a1 == 1007 && a2 == 1014 && a3 == 1021 && a4 == 1028
        && a5 == 1035 && x == 6.75L && s.a == 2007 && s.b == 3007 && f0 == 8.5f
        && f1 == 9.5f && f2 == 10.5f && f3 == 11.5f && f4 == 12.5f
        && f5 == 13.5f && f6 == 14.5f && f7 == 15.5f && f8 == 16.5f;
}

static _Complex long double cld(long double re, long double im) {
    _Complex long double z;
    ((long double *)&z)[0] = re;
    ((long double *)&z)[1] = im;
    return z;
}

/* Every call goes through one of these, so no pipeline can inline it away. */
static int (*volatile p11)(i64, i64, i64, i64, i64, i64, S16, i64) = f11;
static int (*volatile p12)(double, double, double, double, double, double, double,
                           double, long double, double) = f12;
static int (*volatile p13)(i64, i64, i64, i64, i64, i64, i64, S16, i64) = f13;
static int (*volatile p14)(i64, i64, i64, i64, i64, i64, i64, i64, i64, S16,
                           i64) = f14;
static int (*volatile p15)(double, double, double, double, double, double, double,
                           _Complex long double, double) = f15;
static int (*volatile p16)(double, double, double, double, double, double, double,
                           double, long double, D16, double) = f16;
static int (*volatile p17)(long double, long double, double, double, double,
                           double, double, double, double, double, double) = f17;
static int (*volatile p18)(i64, i64, i64, i64, i64, i64, i64, S16, i64, ...) = f18;
static int (*volatile p19)(i64, i64, i64, i64, i64, i64, long double, S16, float,
                           float, float, float, float, float, float, float,
                           float) = f19;

int main(void) {
    S16 const s6 = {2006, 3006}, s7 = {2007, 3007}, s9 = {2009, 3009};
    D16 const h9 = {9.125, 9.375};

    if (!p11(1000, 1007, 1014, 1021, 1028, 1035, s6, 1049)) return 11;
    if (!p12(0.25, 1.25, 2.25, 3.25, 4.25, 5.25, 6.25, 7.25, 8.75L, 9.25)) return 12;
    if (!p13(1000, 1007, 1014, 1021, 1028, 1035, 1042, s7, 1056)) return 13;
    if (!p14(1000, 1007, 1014, 1021, 1028, 1035, 1042, 1049, 1056, s9, 1070))
        return 14;
    if (!p15(0.25, 1.25, 2.25, 3.25, 4.25, 5.25, 6.25, cld(7.5L, 7.625L), 8.25))
        return 15;
    if (!p16(0.25, 1.25, 2.25, 3.25, 4.25, 5.25, 6.25, 7.25, 8.75L, h9, 10.25))
        return 16;
    if (!p17(0.75L, 1.75L, 2.25, 3.25, 4.25, 5.25, 6.25, 7.25, 8.25, 9.25, 10.25))
        return 17;
    if (!p18(1000, 1007, 1014, 1021, 1028, 1035, 1042, s7, 1056, (i64)9000,
             (i64)9001, (i64)9002))
        return 18;
    if (!p19(1000, 1007, 1014, 1021, 1028, 1035, 6.75L, s7, 8.5f, 9.5f, 10.5f,
             11.5f, 12.5f, 13.5f, 14.5f, 15.5f, 16.5f))
        return 19;
    return 42;
}
