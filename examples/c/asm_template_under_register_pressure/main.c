/* One inline-asm statement is ONE instruction while registers are allocated
 * (P68 round 8 part 4): nothing the register allocator writes may land between
 * two lines of a template. Until then each template line was its own
 * allocation point, and under register pressure the allocator's spill and
 * reload code went BETWEEN them — 29 instructions and 15 memory accesses
 * inside a 7-line x86_64 template at release, measured 2026-09-21. An
 * `ldaxr`/`stlxr` pair with a store between its halves may fail on every try.
 *
 * Every shape below runs with more values live around the statement than the
 * target has registers, so the function spills:
 *   x86_64  — one statement naming 14 registers at once (13 inputs and
 *             an earlyclobber output), every input also read after it;
 *   aarch64 — the same with 29 registers, and an LL/SC increment
 *             (`ldaxr` / `add` / `stlxr` in ONE template, retried in C) with
 *             28 values live across it. The retry is BOUNDED, so a pair that
 *             never succeeds exits with code 23 instead of hanging.
 * Exit codes: 11/21 a sum statement read a wrong input, 22 the LL/SC result is
 * wrong, 23 the LL/SC pair never succeeded, 24 a value live across it changed.
 */
typedef long long i64;

#if defined(__x86_64__)
static i64 sum_under_pressure(i64 a0, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5, i64 a6, i64 a7, i64 a8, i64 a9, i64 a10, i64 a11, i64 a12) {
    i64 r;
    __asm__ volatile("xorq %0, %0\n\t"
                     "addq %1, %0\n\t"
                     "addq %2, %0\n\t"
                     "addq %3, %0\n\t"
                     "addq %4, %0\n\t"
                     "addq %5, %0\n\t"
                     "addq %6, %0\n\t"
                     "addq %7, %0\n\t"
                     "addq %8, %0\n\t"
                     "addq %9, %0\n\t"
                     "addq %10, %0\n\t"
                     "addq %11, %0\n\t"
                     "addq %12, %0\n\t"
                     "addq %13, %0"
                     : "=&r"(r)
                     : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7), "r"(a8), "r"(a9), "r"(a10), "r"(a11), "r"(a12));
    return r - (a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12);
}
static i64 (*volatile sum_fp)(i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64) = sum_under_pressure;

int main(void) {
    if (sum_fp(1, 4, 7, 10, 13, 16, 19, 22, 25, 28, 31, 34, 37) != 0) return 11;
    return 42;
}
#elif defined(__aarch64__)
static i64 sum_under_pressure(i64 a0, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5, i64 a6, i64 a7, i64 a8, i64 a9, i64 a10, i64 a11, i64 a12, i64 a13, i64 a14, i64 a15, i64 a16, i64 a17, i64 a18, i64 a19, i64 a20, i64 a21, i64 a22, i64 a23, i64 a24, i64 a25, i64 a26, i64 a27) {
    i64 r;
    __asm__ volatile("mov %0, #0\n\t"
                     "add %0, %0, %1\n\t"
                     "add %0, %0, %2\n\t"
                     "add %0, %0, %3\n\t"
                     "add %0, %0, %4\n\t"
                     "add %0, %0, %5\n\t"
                     "add %0, %0, %6\n\t"
                     "add %0, %0, %7\n\t"
                     "add %0, %0, %8\n\t"
                     "add %0, %0, %9\n\t"
                     "add %0, %0, %10\n\t"
                     "add %0, %0, %11\n\t"
                     "add %0, %0, %12\n\t"
                     "add %0, %0, %13\n\t"
                     "add %0, %0, %14\n\t"
                     "add %0, %0, %15\n\t"
                     "add %0, %0, %16\n\t"
                     "add %0, %0, %17\n\t"
                     "add %0, %0, %18\n\t"
                     "add %0, %0, %19\n\t"
                     "add %0, %0, %20\n\t"
                     "add %0, %0, %21\n\t"
                     "add %0, %0, %22\n\t"
                     "add %0, %0, %23\n\t"
                     "add %0, %0, %24\n\t"
                     "add %0, %0, %25\n\t"
                     "add %0, %0, %26\n\t"
                     "add %0, %0, %27\n\t"
                     "add %0, %0, %28"
                     : "=&r"(r)
                     : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7), "r"(a8), "r"(a9), "r"(a10), "r"(a11), "r"(a12), "r"(a13), "r"(a14), "r"(a15), "r"(a16), "r"(a17), "r"(a18), "r"(a19), "r"(a20), "r"(a21), "r"(a22), "r"(a23), "r"(a24), "r"(a25), "r"(a26), "r"(a27));
    return r - (a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13 + a14 + a15 + a16 + a17 + a18 + a19 + a20 + a21 + a22 + a23 + a24 + a25 + a26 + a27);
}
static i64 (*volatile sum_fp)(i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64) = sum_under_pressure;

static unsigned cell;

static i64 llsc_under_pressure(unsigned *p, i64 a0, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5, i64 a6, i64 a7, i64 a8, i64 a9, i64 a10, i64 a11, i64 a12, i64 a13, i64 a14, i64 a15, i64 a16, i64 a17, i64 a18, i64 a19, i64 a20, i64 a21, i64 a22, i64 a23, i64 a24, i64 a25, i64 a26, i64 a27) {
    unsigned v = 0, fail = 1;
    long tries = 0;
    do {
        __asm__ volatile("ldaxr %w0, [%2]\n\t"
                         "add %w0, %w0, #1\n\t"
                         "stlxr %w1, %w0, [%2]"
                         : "=&r"(v), "=&r"(fail)
                         : "r"(p)
                         : "memory");
    } while (fail != 0 && ++tries < 1000000);
    if (fail != 0) return -1;
    return (i64)v * 1000000 + (a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13 + a14 + a15 + a16 + a17 + a18 + a19 + a20 + a21 + a22 + a23 + a24 + a25 + a26 + a27);
}
static i64 (*volatile llsc_fp)(unsigned *, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64) = llsc_under_pressure;

int main(void) {
    if (sum_fp(1, 4, 7, 10, 13, 16, 19, 22, 25, 28, 31, 34, 37, 40, 43, 46, 49, 52, 55, 58, 61, 64, 67, 70, 73, 76, 79, 82) != 0) return 21;
    i64 const expect = 2 + 7 + 12 + 17 + 22 + 27 + 32 + 37 + 42 + 47 + 52 + 57 + 62 + 67 + 72 + 77 + 82 + 87 + 92 + 97 + 102 + 107 + 112 + 117 + 122 + 127 + 132 + 137;
    i64 const got = llsc_fp(&cell, 2, 7, 12, 17, 22, 27, 32, 37, 42, 47, 52, 57, 62, 67, 72, 77, 82, 87, 92, 97, 102, 107, 112, 117, 122, 127, 132, 137);
    if (got == -1) return 23;
    if (got / 1000000 != 1 || cell != 1) return 22;
    if (got % 1000000 != expect) return 24;
    return 42;
}
#else
#error "this example's templates are x86_64 and aarch64 assembly"
#endif
