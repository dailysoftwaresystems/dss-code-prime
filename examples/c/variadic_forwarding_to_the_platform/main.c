/* A DSS-compiled VARIADIC DEFINITION on every pair: it reads `long double`, `double`, `long long` and pointer
 * arguments through `va_arg`, and it FORWARDS its own `va_list` to the platform C library's `vfprintf`, which
 * walks that list with the platform's own code. The C23 conversion layer DSS ships for the UCRT and libSystem
 * (D-C-C23-CONVERSIONS-MISSING-ON-THE-UCRT-AND-LIBSYSTEM) rests on all three: its entry points are variadic
 * definitions compiled by DSS, it fetches every argument class through `va_arg`, and its fast path hands the
 * caller's `va_list` to the platform unchanged. On x86_64 `long double` is x87's 80-bit format passed in MEMORY
 * (SysV class X87: always the overflow area, 16-aligned), on aarch64 Linux IEEE binary128 in a q register until
 * v7 and then on the stack, on pe and arm64 Mach-O binary64 — each a different `va_arg` path, and the calls below
 * reach every one: a long double after a stacked int (the 16-byte alignment pad), ten of them (past v7), and the
 * platform's own printf reading ten from a DSS caller.
 *
 * AAPCS64's own shape (P69 round 4): with x0..x7 full, the eighth int is STACKED in one 8-byte slot, and the long
 * doubles past q7 follow it on the stack 16-ALIGNED — the ninth at the next 16-byte boundary after the int (the
 * round-up pad). `after_full_registers` reads that shape back, weighting each long double by its position so a read
 * of the wrong slot cannot cancel out, and the platform's printf is handed the same shape from a DSS caller.
 *
 * The datum's own STEP is a different fact, and that shape cannot see it: two stacked long doubles in a row are both
 * 16-aligned, so a step of one 8-byte slot and the right step of 16 put the second in the same place (✔MEASURED: the
 * step mutant left that shape green). It shows only where a NARROWER argument follows a stacked wide one —
 * `narrow_after_wide` stacks the ninth long double and then an int, a double and one more long double, so the int
 * sits 16 bytes after the long double's start, where a one-slot step would have put it inside the long double — and
 * the platform's printf reads the same shape from a DSS caller. */
#include <stdarg.h>
#include <stdio.h>

static long double sum_long_doubles(int count, ...) {
    va_list ap;
    va_start(ap, count);
    long double total = 0;
    for (int i = 0; i < count; ++i) total += va_arg(ap, long double);
    va_end(ap);
    return total;
}

static long double after_stacked_ints(int count, ...) {
    va_list ap;
    va_start(ap, count);
    long long ints = 0;
    for (int i = 0; i < count; ++i) ints += va_arg(ap, int);
    long double const tail = va_arg(ap, long double);
    va_end(ap);
    return tail + (long double)ints;
}

static long double after_full_registers(int count, ...) {
    va_list ap;
    va_start(ap, count);
    long long ints = 0;
    for (int i = 0; i < 8; ++i) ints += va_arg(ap, int);   /* x1..x7, then the first stacked slot */
    long double weighted = 0;
    for (int i = 0; i < count; ++i) weighted += va_arg(ap, long double) * (long double)(i + 1);
    va_end(ap);
    return weighted + (long double)ints;
}

static long double narrow_after_wide(int count, ...) {
    va_list ap;
    va_start(ap, count);
    long long ints = 0;
    for (int i = 0; i < 8; ++i) ints += va_arg(ap, int);   /* x1..x7, then the first stacked slot */
    long double weighted = 0;
    for (int i = 0; i < count; ++i) weighted += va_arg(ap, long double) * (long double)(i + 1);
    int const after = va_arg(ap, int);                     /* directly after the last, stacked long double */
    double const then = va_arg(ap, double);
    long double const last = va_arg(ap, long double);
    va_end(ap);
    return weighted + (long double)ints + (long double)after * 1000 + (long double)then * 4000 + last * 80000;
}

static long long mixed(int count, ...) {
    va_list ap;
    va_start(ap, count);
    long long total = 0;
    for (int i = 0; i < count; ++i) {
        switch (i % 4) {
            case 0: total += va_arg(ap, int); break;
            case 1: total += (long long)va_arg(ap, double); break;
            case 2: total += va_arg(ap, long long); break;
            default: total += *va_arg(ap, int *); break;
        }
    }
    va_end(ap);
    return total;
}

static int forward(char const *format, ...) {
    va_list ap;
    va_start(ap, format);
    int const written = vfprintf(stdout, format, ap);
    va_end(ap);
    return written;
}

int main(void) {
    long double const sum = sum_long_doubles(3, 1.5L, 2.25L, 4.0L);
    long double const ten = sum_long_doubles(10, 0.5L, 1.0L, 1.5L, 2.0L, 2.5L, 3.0L, 3.5L, 4.0L, 4.5L, 5.0L);
    long double const padded = after_stacked_ints(6, 1, 2, 3, 4, 5, 6, 0.25L);
    long double const full = after_full_registers(10, 1, 2, 3, 4, 5, 6, 7, 8, 0.5L, 1.0L, 1.5L, 2.0L, 2.5L, 3.0L,
                                                  3.5L, 4.0L, 4.5L, 5.0L);
    long double const stepped = narrow_after_wide(9, 1, 2, 3, 4, 5, 6, 7, 8, 0.5L, 1.0L, 1.5L, 2.0L, 2.5L, 3.0L,
                                                  3.5L, 4.0L, 4.5L, 9, 0.25, 0.125L);
    int seven = 7;
    long long const total = mixed(8, 1, 2.0, 3LL, &seven, 5, 6.0, 7LL, &seven);
    int const written = forward("%d|%.2Lf|%s|%lld|%.1f\n", 42, sum, "text", total, 0.5);
    int const direct = printf("%.1Lf %.1Lf %.1Lf %.1Lf %.1Lf %.1Lf %.1Lf %.1Lf %.1Lf %.1Lf|%.2Lf|%.2Lf\n", 0.5L,
                              1.0L, 1.5L, 2.0L, 2.5L, 3.0L, 3.5L, 4.0L, 4.5L, 5.0L, ten, padded);
    int const shaped = printf("%d %d %d %d %d %d %d %d|%.1Lf %.1Lf %.1Lf %.1Lf %.1Lf %.1Lf %.1Lf %.1Lf %.1Lf "
                              "%.1Lf|%.2Lf\n", 1, 2, 3, 4, 5, 6, 7, 8, 0.5L, 1.0L, 1.5L, 2.0L, 2.5L, 3.0L, 3.5L,
                              4.0L, 4.5L, 5.0L, full);
    int const narrow = printf("%d %d %d %d %d %d %d %d|%.1Lf %.1Lf %.1Lf %.1Lf %.1Lf %.1Lf %.1Lf %.1Lf %.1Lf|%d|"
                              "%.2f|%.3Lf|%.1Lf\n", 1, 2, 3, 4, 5, 6, 7, 8, 0.5L, 1.0L, 1.5L, 2.0L, 2.5L, 3.0L, 3.5L,
                              4.0L, 4.5L, 9, 0.25, 0.125L, stepped);
    fflush(stdout);
    return (sum == 7.75L && ten == 27.5L && padded == 21.25L && full == 228.5L && stepped == 20178.5L && total == 38
            && written == 20 && direct == 52 && shaped == 63 && narrow == 73)
               ? 42 : 1;
}
