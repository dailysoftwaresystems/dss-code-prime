/* D-TARGET-ENCODING-WIDTH-GUARD (LD-5): the runtime witness for NEGATING a
 * `long double` and for CONVERTING one to and from every narrower scalar, on
 * every long-double axis.
 *
 * ✔MEASURED BEFORE THE CHANGE, through the shipped CLI at b1f31420, one tiny
 * program per operation compiled ALONE so no refusal could mask another: SEVEN
 * of the operations below REFUSED with `L_UnsupportedLoweringForOpcode` naming
 * this anchor, on x86_64:elf64-x86_64-linux-exec (F80, ordinal 14) and/or
 * arm64:elf64-aarch64-linux-exec (F128, ordinal 15):
 *
 *     -ld                      MIR FNeg              BOTH axes
 *     (unsigned)ld             MIR FPToUI (source)   BOTH axes
 *     (double)ld               MIR FPTrunc (source)  BOTH axes
 *     (float)ld                MIR FPTrunc (source)  BOTH axes
 *     (long double)someFloat   MIR FPExt (result)    BOTH axes
 *     (long double)someDouble  MIR FPExt (result)    x87 only
 *     (long long)ld            MIR FPToSI (source)   arm64 only (x87 said
 *                                                    `MIR opcode '<deferred>'`)
 *
 * The baseline was FAIL LOUD with no binary, never a wrong answer. The f64 axis
 * (pe64 x86_64, Apple arm64), where `long double` IS `double`, already ran every
 * arm here, which is what makes this file meaningful on all three.
 *
 * ★★ WHAT EACH ARM MEASURES, because a long-double program that compiles and
 * returns 42 proves NO FAULT and nothing whatever about whether the conversion
 * was CORRECT. None of these operations faults when it is wrong; each returns a
 * plausible number. The specific ways to get one:
 *
 *   NEGATE (arms 1-4) — a negate realized as `0 - x` instead of a sign flip
 *   agrees with the truth on every value this program can print, so the arms
 *   pin DIRECTION and FULL-WIDTH EXACTNESS instead: arm 3 negates a value that
 *   needs the type's LAST mantissa bit and adds it back, so a negate that
 *   round-tripped through anything narrower leaves a nonzero residue.
 *   ⓘ The one corner these arms cannot reach is the SIGN OF ZERO (`-(+0.0)`
 *   must be `-0.0`, which `0 - x` gets wrong): C compares the two zeros EQUAL,
 *   and distinguishing them needs either <math.h>'s signbit — DSS ships no
 *   headers — or a byte-level view of an object whose padding this axis leaves
 *   indeterminate. It is stated here rather than faked with an arm that would
 *   pass either way.
 *
 *   TO-INTEGER (arms 5-9) — the failure is a WRONG RANGE, silently. On the x87
 *   axis `(unsigned)ld` stores through the SIGNED 64-bit truncating form and
 *   narrows in a register; had it used the 32-bit form, every value at or above
 *   2^31 would store the 0x80000000 integer-indefinite. Arm 6 asks for
 *   3000000000, which is above 2^31 and below 2^32, and is the arm that
 *   discriminates the two. Arm 9 is the WIDTH arm for `(long long)`: it builds
 *   2^53 + 1, a value no `double` can hold, and asks for it back — and it
 *   asks SELF-CALIBRATINGLY, so it is equally meaningful on the f64 axis where
 *   the answer must be 2^53.
 *
 *   TO-FLOAT (arms 10-13) — the failure is a WRONG ROUNDING or a wrong-width
 *   store (`fstp m64` where `fstp m32` was meant differ in ONE opcode byte).
 *   Arms 11 and 13 place the value THREE QUARTERS of the way between two
 *   representable neighbours, so round-to-nearest and truncation give DIFFERENT
 *   answers; an arm at the halfway point would pass under both.
 *
 *   FROM-FLOAT (arms 14-17) — the failure is a wrong-width LOAD (`fld m64` on a
 *   4-byte slot reads four bytes of something else). These arms widen a value
 *   whose LOW mantissa bit is significant — 1 + ulp(1.0f) and 1 + ulp(1.0) —
 *   and then recover that exact bit by subtraction in long-double arithmetic,
 *   which is full-width by construction. A widen that dropped or invented low
 *   bits fails, and so does one that read the wrong four bytes.
 *
 *   ARM 20 IS THE x87 STACK-BALANCE ARM, and no straight-line arm can replace
 *   it. Every sequence this cycle adds pushes onto the eight-deep x87 register
 *   stack and pops again; leak one entry and the stack overflows on the eighth
 *   iteration and every later result is an indefinite NaN. Arm 20 runs all six
 *   new sequences — negate, both narrowings, both widenings, both integer
 *   conversions — forty times round a loop and checks the value each time.
 *
 * ★ EVERY WIDTH IS DISCOVERED AT RUNTIME, never assumed. `<float.h>` does not
 * exist in DSS, so the three ulps this file needs (of `float`, of `double`, and
 * of `long double`) are each found by halving until the sum stops moving, in
 * that type's own arithmetic. That is what lets one program be exact on an axis
 * with a 24-, 53-, 64- or 113-bit mantissa.
 *
 * ANTI-FOLD: every operand and every loop bound is read from a MUTABLE GLOBAL,
 * so the release pipeline cannot constant-fold a conversion away and leave the
 * lowering untested. The `release` arm therefore measures the same conversions
 * through Mem2Reg-promoted values (the long-double locals become real phis
 * across the loops' back edges) rather than through memory traffic.
 *
 * exit = 42; each arm returns its own number, so a failure names itself.
 */

/* ── mutable operands: see ANTI-FOLD above ───────────────────────────────── */
long double g_a     = 1.5L;
long double g_negA  = -1.5L;
long double g_zero  = 0.0L;
long double g_one   = 1.0L;
long double g_two   = 2.0L;
long double g_three = 3.0L;
long double g_four  = 4.0L;
long double g_big   = 3000000000.0L;   /* above 2^31, below 2^32 */
long double g_frac  = 7.9L;
long double g_nfrac = -7.9L;

double      d_one   = 1.0;
double      d_two   = 2.0;
float       f_one   = 1.0f;
float       f_two   = 2.0f;

int         g_iters = 40;

int main(void) {
    /* ── the three ulps, each discovered in its OWN type's arithmetic ─────
     * Halve until 1 + e stops differing from 1; `last` is then the smallest
     * step that type can still see at 1.0. No <float.h> needed, and exact on
     * every axis. */
    long double u;      /* ulp(1.0L) — 2^-52, 2^-63 or 2^-112 by axis */
    double      du;     /* ulp(1.0)  — always 2^-52                   */
    float       fu;     /* ulp(1.0f) — always 2^-23                   */
    {
        long double e = g_one, last = g_one;
        double      de = d_one, dlast = d_one;
        float       fe = f_one, flast = f_one;
        while (g_one + e > g_one) { last = e;  e  = e  / g_two; }
        while (d_one + de > d_one) { dlast = de; de = de / d_two; }
        while (f_one + fe > f_one) { flast = fe; fe = fe / f_two; }
        u = last; du = dlast; fu = flast;
    }
    /* The discovery must have found something usable — a zero here would make
     * every width arm below vacuously true. */
    if (!(u > g_zero))  return 1;
    if (!(du > 0.0))    return 2;
    if (!(fu > 0.0f))   return 3;

    /* ── ARM 4: NEGATE, direction ────────────────────────────────────────
     * `-g_a` must be the OTHER value, and negating twice must return. A
     * lowering that emitted a no-op, or an absolute value, fails here. */
    if (!(-g_a == g_negA))  return 4;
    if (!(-g_negA == g_a))  return 4;
    if (!(-(-g_a) == g_a))  return 4;

    /* ── ARM 5: NEGATE, agreement with subtraction on a NON-zero value ───
     * `-x` and `0 - x` must give the same answer everywhere except at zero. */
    if (!(-g_a == g_zero - g_a)) return 5;

    /* ── ARM 6: NEGATE, FULL WIDTH ───────────────────────────────────────
     * Negate a value whose LAST mantissa bit is significant and add it back:
     * a negate that round-tripped through a narrower format leaves a residue,
     * and `w + (-w)` is then not exactly zero. The second half of the arm is
     * what stops it passing vacuously — `(-w) + g_one` must NOT be zero, so
     * the ulp really is carried. */
    {
        long double w = g_one + u;
        if (!(w + (-w) == g_zero))  return 6;
        if ((-w) + g_one == g_zero) return 6;
    }

    /* ── ARM 7: (unsigned) of a fractional value truncates TOWARD ZERO ────
     * Not floor, not round: C 6.3.1.4. */
    if ((unsigned)g_frac != 7u) return 7;

    /* ── ARM 8: (unsigned) ABOVE 2^31 — the range arm ────────────────────
     * 3000000000 does not fit a SIGNED 32-bit integer. A lowering that stored
     * through the x87 32-bit truncating form would write the 0x80000000
     * integer-indefinite here and read it back with no fault at all. */
    if ((unsigned)g_big != 3000000000u) return 8;

    /* ── ARM 9: (long long) truncates toward zero on the NEGATIVE side ────
     * -7.9 must give -7, not -8: the direction a floor-based lowering gets
     * wrong only for negatives. */
    if ((long long)g_nfrac != -7LL) return 9;
    if ((long long)g_frac  !=  7LL) return 9;

    /* ── ARM 10: (long long) WIDTH, self-calibrating ─────────────────────
     * Build 2^53 by doubling, then ask for 2^53 + 1. On an axis whose
     * `long double` is WIDER than double the sum is representable and the
     * answer is 2^53+1; where `long double` IS double it rounds back and the
     * answer is 2^53. The expected value is derived from the type's OWN
     * comparison, so the arm is exact on all four mantissa widths — and it
     * still discriminates, because a conversion that truncated to 32 bits, or
     * that went through a float, misses BOTH answers. */
    {
        long double p = g_one;
        long long   n = 1;
        int         k;
        for (k = 0; k < 53; ++k) { p = p * g_two; n = n * 2; }
        {
            long double q = p + g_one;
            long long   expect = (q > p) ? (n + 1) : n;
            if ((long long)q != expect) return 10;
            if ((long long)p != n)      return 10;
        }
    }

    /* ── ARM 11: (double) of an exact small value ────────────────────────*/
    if (!((double)g_a == 1.5))   return 11;
    if (!((float)g_a  == 1.5f))  return 11;

    /* ── ARM 12: (double) ROUNDS TO NEAREST, not toward zero ─────────────
     * Place the value three quarters of the way from 1.0 to 1.0+ulp(double):
     * round-to-nearest gives the upper neighbour, truncation gives the lower,
     * so the two answers DIFFER — which a halfway-point arm would not show.
     * `(3*du)/4` is exact in every long-double format here, so the input is
     * exact and only the narrowing is under test. */
    {
        long double x = g_one + ((long double)du * g_three) / g_four;
        if (!((double)x == 1.0 + du)) return 12;
    }

    /* ── ARM 13: (float) ROUNDS TO NEAREST too ───────────────────────────
     * The same construction one format narrower. This is also the arm that
     * separates the two narrowing STORES: `fstp m32` and `fstp m64` differ in
     * one opcode byte and an m64 store followed by a 4-byte read would land
     * nowhere near 1.0f + ulp(1.0f). */
    {
        long double x = g_one + ((long double)fu * g_three) / g_four;
        if (!((float)x == 1.0f + fu)) return 13;
    }

    /* ── ARM 14: (double)ld LOSES exactly what it should, cross-checked ───
     * `1 + ulp(long double)` narrows to 1.0 precisely when `long double` is
     * strictly wider than `double`, and that predicate is derived here from the
     * two ULPS THIS PROGRAM DISCOVERED rather than assumed — so the arm reads
     * the same on a 64-bit, a 113-bit and a 53-bit mantissa, and fires if the
     * narrowing kept bits it could not hold or dropped bits it could.
     * It exercises the widen as well (`(long double)` of the result), which is
     * why arms 15-17 pin the widen on its own too. */
    {
        int wider  = (u < (long double)du);
        int folded = ((long double)(double)(g_one + u) == g_one);
        if (folded != wider) return 14;
    }

    /* ── ARM 15: (long double) of a float, LOW BIT INTACT ────────────────
     * Widen 1 + ulp(1.0f) and recover that ulp by subtracting in long-double
     * arithmetic, which is full width by construction. A widen that read the
     * wrong four bytes, or that went through a narrower format, misses. */
    {
        float       fx = 1.0f + fu;
        long double wx = (long double)fx;
        if (!(wx > g_one))                    return 15;
        if (!(wx - g_one == (long double)fu)) return 15;
    }

    /* ── ARM 16: (long double) of a double, LOW BIT INTACT ───────────────
     * The same for 1 + ulp(1.0). ⚠ THIS IS THE ARM THAT SEPARATES THE TWO
     * WIDENING LOADS: `fld m32` and `fld m64` differ in one opcode byte, and
     * an m32 load of an 8-byte slot reads the low four bytes as a float —
     * a wildly wrong value, but not a fault. */
    {
        double      dx = 1.0 + du;
        long double wx = (long double)dx;
        if (!(wx > g_one))                    return 16;
        if (!(wx - g_one == (long double)du)) return 16;
    }

    /* ── ARM 17: the widen is EXACT for a plain value, both sources ───────*/
    if (!((long double)1.5f == g_a)) return 17;
    if (!((long double)1.5  == g_a)) return 17;

    /* ── ARM 18: float → long double → float round trips ─────────────────
     * Every float is exactly representable in every long-double format, so the
     * round trip must be the identity — including for a value with a full
     * float mantissa. */
    {
        float fx = 1.0f + fu;
        if (!((float)(long double)fx == fx))  return 18;
    }

    /* ── ARM 19: double → long double → double round trips ───────────────*/
    {
        double dx = 1.0 + du;
        if (!((double)(long double)dx == dx)) return 19;
    }

    /* ── ARM 20: THE x87 STACK-BALANCE LOOP ──────────────────────────────
     * Forty iterations, each running EVERY sequence this cycle added: negate
     * (twice), narrow to double, narrow to float, widen from double, widen
     * from float, convert to unsigned, convert to long long. The x87 register
     * stack is eight deep and each of those pushes at least one entry; leak a
     * single pop and the stack overflows by the eighth iteration, after which
     * every result is an indefinite NaN and the == checks below stop holding.
     * A straight-line witness never reaches the leak.
     *
     * `acc` alternates sign and returns to +1.5 after an even number of
     * negations, and 1.5 is exact in float, so nothing drifts: the loop's own
     * arithmetic contributes no rounding and any failure is the lowering's. */
    {
        long double acc = g_a;
        unsigned    usum = 0u;
        long long   lsum = 0;
        int         i;
        for (i = 0; i < g_iters; ++i) {
            long double neg = -acc;
            double      dn  = (double)acc;
            float       fn  = (float)acc;
            long double w1  = (long double)dn;
            long double w2  = (long double)fn;
            long double pos = (neg > g_zero) ? neg : acc;
            if (!(w1 == acc)) return 20;
            if (!(w2 == acc)) return 20;
            usum += (unsigned)pos;      /* 1.5 truncates to 1 every time */
            lsum += (long long)pos;
            acc = neg;
        }
        if (!(acc == g_a))  return 21;  /* 40 negations = even */
        if (usum != 40u)    return 22;
        if (lsum != 40)     return 23;
    }

    return 42;
}
