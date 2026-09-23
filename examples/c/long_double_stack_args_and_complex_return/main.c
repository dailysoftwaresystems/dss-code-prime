/* `long double` ARGUMENTS PASSED ON THE STACK, and a `_Complex long double`
 * RETURNED — the two AArch64 call-boundary shapes that were refused on both
 * sides of the call. D-LIR-AAPCS64-LONG-DOUBLE-ARG-PAST-V7-REFUSED,
 * D-LIR-AAPCS64-COMPLEX-LONG-DOUBLE-RETURN-PIECE-REFUSED.
 *
 * ★★★ WHY THIS EXAMPLE EXISTS. Under AAPCS64 a binary128 `long double` travels
 * in a Q register (v0..v7); once those are taken it goes on the STACK, 16-byte
 * aligned and 16 bytes wide — and every argument after it is placed after THAT.
 * A `_Complex long double` is a two-member binary128 HFA: RETURNED in q0 and q1.
 * ✔MEASURED 2026-09-19 at the P68 round-7 base, through the real CLI on
 * `arm64:elf64-aarch64-linux-exec`, debug AND release: every stacked
 * `long double` was refused — the caller "belongs on the STACK — a placement
 * this lowering does not realize", the callee "MIR opcode 'Arg' is not yet
 * lowered" — and the complex return was refused ("MIR opcode 'returnpiece' is
 * not yet lowered"). aarch64-linux-gnu-gcc 13.3.0 and clang 18.1.3 run every
 * shape here to 42 at -O0 and -O2 (qemu-aarch64).
 *
 * ★★ THE SHAPES DISCRIMINATE PLACEMENT, NOT MERELY ARRIVAL: each argument is a
 * distinct value, so an argument read from its neighbour's slot (a wrong
 * alignment, a wrong width, a 16-byte datum placed as 8) fails its own check.
 * Every check has its own exit code.
 *   11  nine `long double`s: v0..v7, then the ninth on the stack
 *   12  eight `double`s, then a `long double` on the stack
 *   13  eight `double`s, a stacked `long double`, then a stacked `double`
 *       AFTER it (the 16 bytes before it must be skipped)
 *   14  eight `double`s, a stacked `double`, then a stacked `long double`
 *       (the 16-byte alignment pad after an 8-byte slot)
 *   15  a `_Complex long double` returned (q0 and q1)
 *   16  the same value passed on through a second call
 *
 * ⓘ PORTABLE C, SO EVERY TARGET RUNS IT: on x86_64 Linux the `long double` is
 * the x87 80-bit value (always stack-passed there); on Windows and on Apple
 * arm64 `long double` IS `double`. The discriminating arm is aarch64 Linux; the
 * others are the same program meaning the same thing. Seeds are `volatile` so
 * the release pipeline cannot fold the arguments into the callee.
 */

typedef long double LD;
typedef _Complex long double CLD;
union ucld { CLD c; LD p[2]; };

volatile int dss_k = 1;

__attribute__((noinline)) static int nine(LD a0, LD a1, LD a2, LD a3, LD a4, LD a5,
                                          LD a6, LD a7, LD a8) {
    return a0 == 0.5L && a1 == 1.5L && a2 == 2.5L && a3 == 3.5L && a4 == 4.5L
        && a5 == 5.5L && a6 == 6.5L && a7 == 7.5L && a8 == 8.5L;
}

__attribute__((noinline)) static int ninth(double d0, double d1, double d2, double d3,
                                           double d4, double d5, double d6, double d7,
                                           LD l) {
    return d0 == 0.5 && d1 == 1.5 && d2 == 2.5 && d3 == 3.5 && d4 == 4.5
        && d5 == 5.5 && d6 == 6.5 && d7 == 7.5 && l == 8.25L;
}

__attribute__((noinline)) static int ldThenDouble(double d0, double d1, double d2,
                                                  double d3, double d4, double d5,
                                                  double d6, double d7, LD l, double d) {
    return d0 == 0.5 && d7 == 7.5 && l == 8.25L && d == 9.75;
}

__attribute__((noinline)) static int doubleThenLd(double d0, double d1, double d2,
                                                  double d3, double d4, double d5,
                                                  double d6, double d7, double d, LD l) {
    return d0 == 0.5 && d7 == 7.5 && d == 9.75 && l == 8.25L;
}

__attribute__((noinline)) static CLD makeComplex(int k) {
    union ucld u;
    u.p[0] = (LD)(40 * k);
    u.p[1] = (LD)(2 * k);
    return u.c;
}

__attribute__((noinline)) static long takeComplex(CLD v) {
    union ucld u;
    u.c = v;
    return (long)u.p[0] + (long)u.p[1];
}

int main(void) {
    LD const s = (LD)dss_k;   /* 1.0L, opaque to the optimizer */
    if (!nine(0.5L * s, 1.5L * s, 2.5L * s, 3.5L * s, 4.5L * s, 5.5L * s,
              6.5L * s, 7.5L * s, 8.5L * s)) return 11;
    if (!ninth(0.5 * dss_k, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.25L * s)) return 12;
    if (!ldThenDouble(0.5 * dss_k, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.25L * s, 9.75))
        return 13;
    if (!doubleThenLd(0.5 * dss_k, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 9.75, 8.25L * s))
        return 14;
    union ucld u;
    u.c = makeComplex(dss_k);
    if (u.p[0] != 40.0L || u.p[1] != 2.0L) return 15;
    if (takeComplex(makeComplex(dss_k)) != 42) return 16;
    return 42;
}
