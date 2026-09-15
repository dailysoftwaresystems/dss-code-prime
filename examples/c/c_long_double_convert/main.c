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
 *   TO-UNSIGNED-64 (arms 24-27, ADDED IN P66) — the last conversion, and the
 *   only one in this file whose realization has TWO arms and a select between
 *   them. `(unsigned long long)ld` is exact below 2^63 through the direct
 *   truncation; at or above it the truncation writes the integer INDEFINITE and
 *   the answer comes from `(x - 2^63)` with the top bit put back. So the
 *   failure mode is a RANGE failure that only values at or above 2^63 can see,
 *   and it is silent: a lowering that shipped the direct arm alone returns a
 *   plausible number with no fault. ⚠ Testing AT 2^63 is not enough — the
 *   indefinite IS 2^63, so the broken lowering passes there; arm 25 therefore
 *   also asks for 1.5*2^63 and 2^64-2048, the second of which additionally
 *   catches a wrong bias. Both sides of each check are built independently:
 *   the long doubles by doubling in long-double arithmetic, the expected
 *   integers by shifting in integer arithmetic.
 *
 *   FROM-INTEGER (arms 28-32, ADDED IN P66 BY LD-7) — the LAST direction of
 *   this surface, and the only one whose four spellings reach four different
 *   realizations. On the x87 axis they are `fild m32` (signed 32), a register
 *   zero-extend then `fild m64` (unsigned 32), `fild m64` (signed 64) and the
 *   exact split `2*(u>>1) + (u&1)` (unsigned 64); on the ieee128 axis they are
 *   four different libgcc helpers. ⚠ THE FAILURE IS A SIGN, AND IT IS SILENT:
 *   both `fild` forms read their bytes SIGNED, so an `unsigned` at or above
 *   2^31 and an `unsigned long long` at or above 2^63 come back NEGATIVE from
 *   a lowering that skipped the widening or the split — a plausible number,
 *   no fault. The arms therefore ask for INT_MIN, for 2^31 and 2^32-1, for -1
 *   as a `long long`, and for 2^63, 1.5*2^63 and 2^64-2048 as an `unsigned
 *   long long`. They live INLINE in `main`, and the register-allocator limit
 *   that briefly forced them into a helper is recorded at the arms themselves.
 *
 *   SUB-NATIVE INTEGER RESULTS (arm 34, ADDED IN P66 BY LD-7) — `(short)ld`,
 *   `(unsigned char)ld` and the rest. The LAST refusal on this surface, and the
 *   one whose cause the anchor row got WRONG: it delegated them to the missing
 *   sub-native ALU forms, but ✔`(short)someDouble` and `(unsigned
 *   char)someFloat` compiled the whole time. What was missing was the three
 *   long-double dispatches asking `memAccessWidthFlags` — how many BYTES an
 *   object occupies — for a REGISTER-resident conversion result. See the banner
 *   on `check_long_double_to_subnative`.
 *
 *   TRUTHINESS (arm 33, ADDED IN P66 BY LD-7) — `!someLongDouble`. The promoter
 *   that turns a float comparison zero into an anonymous rodata global gated on
 *   F64/F32 only, so on a WIDE long-double axis the zero stayed a bare MIR
 *   `Const` and the whole function refused. ⚠ A CLOSED ROW CLAIMS OTHERWISE —
 *   see the banner on `check_long_double_truthiness`. The SAME gate's other
 *   door, the long-double `_Complex` zero, is witnessed by arm 9 of
 *   `examples/c/c99_complex_truth`; it is not here because a `_Complex` in this
 *   file would cost it its MSVC vote (cl.exe refuses `__builtin_complex`).
 *
 *   ARM 20 IS THE x87 STACK-BALANCE ARM, and no straight-line arm can replace
 *   it. Every sequence this cycle adds pushes onto the eight-deep x87 register
 *   stack and pops again; leak one entry and the stack overflows on the eighth
 *   iteration and every later result is an indefinite NaN. Arm 20 runs all six
 *   new sequences — negate, both narrowings, both widenings, both integer
 *   conversions — forty times round a loop and checks the value each time.
 *   Arms 27 and 32 are its siblings for the two P66 sequences, and 32 is the
 *   deepest: the unsigned-64 FROM-integer conversion pushes THREE times for
 *   one C operation.
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
long double g_nsmall = -0.5L;   /* arm 26: the one NEGATIVE case C defines
                                 * for an unsigned conversion — the integral
                                 * part is zero, which IS representable. */

double      d_one   = 1.0;
double      d_two   = 2.0;
float       f_one   = 1.0f;
float       f_two   = 2.0f;

/* ── arms 28-32: the INTEGER sources (see the block comment ON those arms,
 * which sit inline in `main` below) ─────────────────────────────────────── */
int                g_i32p  = 2147483647;         /* INT_MAX                */
int                g_i32n  = -2147483647 - 1;    /* INT_MIN                */
int                g_ineg3 = -3;
unsigned           g_u32h  = 2147483648u;        /* 2^31 — above the SIGNED
                                                  * 32-bit range, the arm
                                                  * that discriminates      */
unsigned           g_u32m  = 4294967295u;        /* 2^32 - 1               */
long long          g_l1    = 1;                  /* the doubling seed      */
unsigned long long g_ull1  = 1;                  /* the shifting seed      */

/* ── arm 34: the SUB-NATIVE integer results. Only IN-RANGE values (C 6.3.1.4
 * makes an out-of-range float→integer conversion undefined), and the
 * discriminating ones are ABOVE the signed sub-native range and inside the
 * unsigned one. */
long double g_sc100  = 100.5L;
long double g_scn100 = -100.5L;
long double g_uc200  = 200.5L;      /* > SCHAR_MAX, <= UCHAR_MAX */
long double g_s30000 = 30000.5L;    /* < SHRT_MAX  */
long double g_us60000 = 60000.5L;   /* > SHRT_MAX,  <= USHRT_MAX */

int         g_iters = 40;

/* ── ARM 33: `!someLongDouble` ───────────────────────────────────────────────
 * C 6.5.3.3p5 makes `!E` exactly `(0 == E)` for ANY scalar E, so `!ld` needs a
 * long-double ZERO to compare against — and a register machine has no
 * float-immediate form, so that zero has to be promoted to an anonymous rodata
 * global and LOADED. The promoter's width list was F64/F32 only, so on a WIDE
 * long-double axis the zero fell through to a bare MIR `Const` and dead-ended
 * at MIR→LIR with `MIR Const %N is a float (FPR-class) literal`, a diagnostic
 * naming D-TARGET-ENCODING-WIDTH-GUARD.
 *
 * ⚠⚠ AND A CLOSED ROW SAYS OTHERWISE. [[D-CSUBSET-LOGICAL-NOT-ON-A-FLOAT-MINTS-A-BARE-FLOAT-CONST]]
 * (P41) states in its trigger that "`!f`, `!d` and `!ld` compile" — ✔MEASURED
 * on `x86_64:pe64-x86_64-windows-exec`, THE ONE AXIS WHERE `long double` IS
 * `double`, so `!ld` there is `!d` under another spelling. ✔MEASURED at
 * de1e83ef that `!ld` and `!!ld` refuse rc 1 on BOTH `elf64-x86_64-linux-exec`
 * (F80) and `elf64-aarch64-linux-exec` (F128) while compiling on pe64. That is
 * why this arm lives on THIS file, whose expected.json runs all three axes.
 *
 * ⓘ THE SAME GATE HAS A SECOND DOOR AND ITS WITNESS IS ELSEWHERE, deliberately:
 * the real→complex construct's imaginary 0.0 comes from the SAME promoter, so
 * `long double _Complex z = 2.0L;` and `!z` refused for the same reason and
 * were fixed by the same two-word widening. That arm is arm 9 of
 * `examples/c/c99_complex_truth`, because putting a `_Complex` in THIS file
 * would cost it its MSVC vote — ✔MEASURED that cl.exe 19.51 refuses
 * `__builtin_complex` outright (`error C2065`), and MSVC's positive vote on the
 * f64 axis is what this file's reference oracle rests on.
 *
 * Returns 0, or the number of the arm that failed. */
static int check_long_double_truthiness(void) {
    if (!g_zero != 1)  return 33;   /* !0.0L is 1 — the arm the wall hid */
    if (!g_a    != 0)  return 33;   /* !1.5L is 0 */
    if (!g_negA != 0)  return 33;   /* a NEGATIVE nonzero is still truthy */
    if (!!g_zero != 0) return 33;
    if (!!g_a    != 1) return 33;
    /* Non-vacuity: `!` must not be answering some constant. The two operands
     * really do differ, in this type's own arithmetic. */
    if (!(g_a != g_zero)) return 33;
    return 0;
}

/* ── ARM 34: `(short)ld`, `(unsigned char)ld` AND THE REST OF THE SUB-NATIVE
 * INTEGER RESULTS ───────────────────────────────────────────────────────────
 * The last refusal on this surface, and the one whose CAUSE the row got wrong.
 * ✔MEASURED at de1e83ef: `(char)ld`, `(signed char)ld` and `(short)ld` refused
 * with `MIR opcode '<deferred>'` and `(unsigned char)ld` / `(unsigned short)ld`
 * with `MIR FPToUI (source)`, on BOTH long-double axes, while compiling on the
 * f64 axis.
 *
 * ⚠⚠ THE ROW DELEGATED THIS TO [[D-CSUBSET-SUBNATIVE-ALU-FORMS]] AND THAT WAS
 * REFUTED BY MEASUREMENT: `(unsigned char)someDouble`, `(short)someDouble`,
 * `(signed char)someDouble`, `(unsigned char)someFloat` and `(unsigned
 * char)someInt` ALL compiled rc 0 the whole time on the same axis — so the
 * sub-native integer FORM was never what was missing. What was missing was one
 * WORD in three places: the three long-double conversion dispatches asked
 * `memAccessWidthFlags` for the RESULT's width, which answers how many BYTES an
 * object of that type occupies (8 for `unsigned char`, 16 for `short`) rather
 * than how wide the register holding the VALUE is. A sub-native integer lives
 * PROMOTED in a 32-bit register with its low bits significant (C 6.3.1.1), and
 * the narrowing realizes at the width-exact STORE — which is exactly what
 * `registerOpWidthFlags` says and what the softcall's own result CAPTURE was
 * already asking.
 *
 * ★ ONLY IN-RANGE VALUES ARE ASKED FOR, and that is not timidity: C 6.3.1.4
 * makes a float→integer conversion whose value does not fit UNDEFINED, so an
 * out-of-range arm would be measuring nothing that the references have to
 * agree on. The discriminating values are the ones ABOVE the SIGNED sub-native
 * range and inside the unsigned one — 200 for `unsigned char`, 60000 for
 * `unsigned short` — which a realization that narrowed to a SIGNED 8 or 16
 * bits gets wrong. Plain `char` is only asked for values that fit BOTH its
 * readings, since its signedness is per-platform (signed on x86_64-linux,
 * unsigned on aarch64-linux — ✔both measured).
 *
 * ✔ALL FOUR REFERENCES compile and RUN every arm here.
 * Returns 0, or the number of the arm that failed. */
static int check_long_double_to_subnative(void) {
    /* signed char — truncation toward zero, both signs, in range. */
    if ((signed char)g_frac  !=  7)   return 34;
    if ((signed char)g_nfrac != -7)   return 34;
    if ((signed char)g_sc100 != 100)  return 34;
    if ((signed char)g_scn100 != -100) return 34;
    if ((signed char)g_zero  !=  0)   return 34;
    /* unsigned char — 200 is ABOVE SCHAR_MAX and inside UCHAR_MAX: the value a
     * signed 8-bit realization answers with -56. */
    if ((unsigned char)g_frac !=   7u) return 34;
    if ((unsigned char)g_sc100 != 100u) return 34;
    if ((unsigned char)g_uc200 != 200u) return 34;
    /* short / unsigned short — 60000 is above SHRT_MAX and inside USHRT_MAX. */
    if ((short)g_s30000 != 30000)  return 34;
    if ((short)g_frac   != 7)      return 34;
    if ((short)g_nfrac  != -7)     return 34;
    if ((short)g_scn100 != -100)   return 34;
    if ((unsigned short)g_s30000 != 30000u) return 34;
    if ((unsigned short)g_us60000 != 60000u) return 34;
    /* plain `char`, whose signedness is per-platform: only values that fit
     * BOTH readings, so the arm reads the same on every axis. */
    if ((char)g_frac  != 7)   return 34;
    if ((char)g_sc100 != 100) return 34;
    /* Non-vacuity, in long-double arithmetic so nothing comes from the
     * conversion under test. */
    if (!(g_uc200 > g_sc100))     return 34;
    if (!(g_us60000 > g_s30000))  return 34;
    if (!(g_nfrac < g_zero))      return 34;
    /* WIDTH: a value needing more than 8 bits must survive a 16-bit
     * conversion, so a lowering that narrowed too early misses. Built by
     * DOUBLING, and the expectation by SHIFTING. */
    {
        long double p = g_one;
        long        n = 1;
        int         k;
        for (k = 0; k < 14; ++k) { p = p * g_two; n = n << 1; }  /* 16384 */
        if (!(p > g_one))                       return 34;
        if ((short)p != (short)n)               return 34;
        if ((unsigned short)p != (unsigned short)n) return 34;
    }
    return 0;
}

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

    /* ── ARMS 24-27: (unsigned long long)ld — THE HARD RANGE ──────────────
     * The last long-double conversion, added in P66. Every other arm above
     * measures a conversion whose realization is ONE sequence; this one has
     * TWO arms inside it and a select between them, and only values AT OR
     * ABOVE 2^63 take the second. A lowering that shipped the direct
     * truncation alone answers arms 24 and 26 correctly and arm 25 with the
     * integer indefinite — no fault, no diagnostic, a plausible number.
     *
     * ⚠ 2^63 ITSELF IS NOT ENOUGH AND ARM 25 SAYS SO BY USING THREE VALUES:
     * the broken lowering returns 0x8000000000000000 there, which IS the right
     * answer for 2^63, so a witness that stopped at the boundary would pass.
     * 1.5*2^63 and 2^64-2048 are the two that discriminate — and 2^64-2048
     * additionally catches a WRONG BIAS, since it is the value furthest from
     * 2^63 that every axis can still hold exactly.
     *
     * The four values are built by DOUBLING in long-double arithmetic and the
     * expected answers by SHIFTING in integer arithmetic, so neither side is
     * derived from the conversion under test. All four are exact in binary64,
     * x87-80 and binary128 alike (in [2^63, 2^64) a binary64's ulp is 2048),
     * so this reads the same on all three axes with nothing calibrated. */
    {
        long double p = g_one;
        int         k;
        for (k = 0; k < 62; ++k) p = p * g_two;   /* p = 2^62 */
        {
            long double x62 = p;                  /* 2^62            */
            long double x63 = p * g_two;          /* 2^63            */
            long double x63h = x63 + x62;         /* 1.5 * 2^63      */
            long double t    = g_one;
            long double xtop;
            for (k = 0; k < 11; ++k) t = t * g_two;   /* t = 2048    */
            xtop = x63 + (x63 - t);               /* 2^64 - 2048     */

            /* Non-vacuity: the doubling must really have reached the range
             * this arm is about, or every check below is about small values. */
            if (!(x63 > x62))  return 24;
            if (!(x63h > x63)) return 24;
            if (!(xtop > x63h)) return 24;

            /* ARM 24 — the CONTROL, below 2^63, where the direct truncation
             * is the whole answer. */
            if ((unsigned long long)x62 != (1ULL << 62)) return 24;

            /* ARM 25 — THE HARD RANGE. */
            if ((unsigned long long)x63  != (1ULL << 63))               return 25;
            if ((unsigned long long)x63h != ((1ULL << 63) + (1ULL << 62))) return 25;
            if ((unsigned long long)xtop != (~0ULL - 2047ULL))          return 25;
        }
    }

    /* ── ARM 26: (unsigned long long) truncates TOWARD ZERO ───────────────
     * Including the one NEGATIVE case C actually defines: the integral part of
     * -0.5 is zero, which IS representable, so the answer must be 0 and not a
     * wrapped 2^64-1. ✔MEASURED that all of WSL gcc 13.3.0, WSL clang 18.1.3
     * (both -O0 and -O2) and aarch64-linux-gnu-gcc 13.3.0 under qemu answer 7
     * and 0 here. */
    if ((unsigned long long)g_frac != 7ULL)  return 26;
    if ((unsigned long long)g_nsmall != 0ULL) return 26;
    if ((unsigned long long)g_zero != 0ULL)  return 26;

    /* ── ARM 27: THE x87 STACK BALANCE FOR THE NEW SEQUENCE ───────────────
     * A separate loop from arm 20 because this conversion is the only one in
     * the file that pushes the x87 stack TWICE for one C operation — once for
     * the direct truncation and once more for the biased one, whose bias is a
     * SECOND push on top of the source. The stack is eight deep, so leaking one
     * of those four pops overflows it inside three iterations and every later
     * conversion returns the indefinite. Forty iterations, checked each time.
     * `g_big` is 3000000000, comfortably inside the range where the direct arm
     * answers, so a failure here is a leak and not a range error — the range is
     * arm 25's job. */
    {
        unsigned long long usum2 = 0;
        int                i;
        for (i = 0; i < g_iters; ++i) {
            unsigned long long one_ = (unsigned long long)g_big;
            if (one_ != 3000000000ULL) return 27;
            usum2 += one_;
        }
        if (usum2 != 40ULL * 3000000000ULL) return 27;
    }

/* ── ARMS 28-32: INTEGER → long double — THE LAST DIRECTION ───────────────
 * Everything in `main` converts a `long double` to something else or widens a
 * narrower FLOAT into one. These four spellings go the other way, and each
 * has its own way of being silently wrong:
 *
 *   ARM 28  int → ld.  On the x87 axis this is `fild m32`, which reads its
 *           four bytes SIGNED — so the arm asks for INT_MIN, which an
 *           unsigned-form lowering answers with +2^31.
 *   ARM 29  unsigned → ld.  THE RANGE ARM, and the mirror of arm 8: an
 *           `unsigned` at or above 2^31 read through the SIGNED 32-bit
 *           form arrives NEGATIVE. 2^31 and 2^32-1 are both above it.
 *   ARM 30  long long → ld.  WIDTH (self-calibrating at 2^53, exactly as
 *           arm 10 does in the opposite direction) and SIGN — a lowering
 *           that took the unsigned path answers -1 with 2^64-1.
 *   ARM 31  unsigned long long → ld.  THE HARD ONE. `fild m64` reads its
 *           quadword SIGNED too, so there is no single instruction; the
 *           realization is `2*(u>>1) + (u&1)`, exact because x87-80 carries
 *           a 64-bit significand. ⚠ EVERY VALUE THIS ARM CARES ABOUT IS AT
 *           OR ABOVE 2^63 — below it a plain signed read is already right,
 *           so a witness that stopped there would pass on a broken
 *           lowering. Unlike arm 25 (whose broken answer AT 2^63 happens to
 *           be correct, because the x87 indefinite IS 2^63) the boundary
 *           itself discriminates here: a signed read of 2^63 is -2^63.
 *   ARM 32  the x87 STACK BALANCE for all four, because the unsigned-64
 *           sequence pushes THREE times for one C operation — the deepest
 *           in the file.
 *
 * Every integer here is built by SHIFTING or by repeated doubling in
 * INTEGER arithmetic, and every expected long double by DOUBLING in
 * LONG-DOUBLE arithmetic, so neither side of any check comes from the
 * conversion under test. Where a value needs more of the significand than
 * an axis has, the expectation is derived from that type's OWN comparison
 * (arm 30's 2^53+1, arm 31's 2^64-1) so one program reads correctly on a
 * 53-, 64- and 113-bit mantissa alike.
 *
 * ⚠⚠ THESE FIVE ARMS ONCE LIVED IN A FUNCTION OF THEIR OWN, AND THE REASON
 * THEY ARE BACK INSIDE `main` IS THE POINT OF THIS PARAGRAPH. ✔MEASURED
 * 2026-09-09 (P66, lane `ld`) with the arms written INSIDE `main`, the two
 * versions compiled the same way and nothing else changed:
 * `x86_64:pe64-x86_64-windows-exec --config=release` REFUSED with
 * `error[L_VirtualRegInPostRegalloc] … exhausted the per-class scratch pool`
 * while the SAME source at `--config=debug` built and ran to 42 and the pre-P66
 * file built and ran at BOTH configs. Moving the arms out was recorded as a
 * WORKAROUND IN A TEST FIXTURE, never a design choice, and
 * [[D-AS-REGALLOC-SCRATCH-POOL-EXHAUSTED-BY-A-LARGE-FUNCTION-IN-RELEASE]] was
 * opened to close it in the compiler.
 *
 * ★★ IT WAS NOT A SIZE LIMIT, WHICH IS WHY THE FIX IS NOT A BIGGER NUMBER.
 * ✔MEASURED (P66, lane `ra`) through the shipped CLI on the same target and
 * config: probes spilling 107 and 119 vregs compiled CLEAN while this file at
 * 50 refused, so "a function large enough to spill N" was never the property.
 * The allocator reserves, per register class, as many spill-reload scratch
 * registers as the function's peak SINGLE-INSTRUCTION reload demand — and it
 * could draw them only from the class's NON-ARGUMENT CALLER-SAVED registers.
 * ms_x64 declares exactly TWO of those in the FPR class (xmm4, xmm5) against a
 * demand of THREE, so the reservation silently came up one short, and the
 * shortfall stayed invisible until a function's pressure happened to assign
 * every other FPR. ✔The refusal named class `fpr` at opcode `fadd`, pool 2,
 * reserved 2 of 3. The reservation now draws its shortfall from the class's
 * callee-saved registers — which the rewriter's scratch pool already harvested
 * when they went unassigned, and which the prologue already saves because the
 * callee-save scan reads the POST-REWRITE instruction stream. ✔MEASURED in the
 * emitted pe64 release binary: `main`'s prologue saves xmm15 and the reloads
 * then stage through it.
 *
 * ⓘ SO THE ARMS SIT WHERE A C PROGRAMMER WOULD HAVE PUT THEM ANYWAY, and this
 * file goes back to being about long-double conversion only. What it also
 * happens to be is a standing CLI-level witness at a real threshold — but the
 * property that cannot pass vacuously is pinned in the unit suite instead, by
 * `LirRegAlloc.ReloadScratchReservationMeetsItsDemandOnEveryShippedConvention`,
 * which asserts reserved >= demanded for every shipped convention and mentions
 * no function size at all.
 *
 * Each arm returns its own number, or execution falls through to the next. */


/* ── ARM 28: int → long double, full SIGNED 32-bit range ──────────────*/
{
    long double p31 = g_one;
    int         k;
    for (k = 0; k < 31; ++k) p31 = p31 * g_two;      /* 2^31 */
    if (!(p31 > g_one))                        return 28;  /* non-vacuity */
    if (!((long double)g_i32n == -p31))        return 28;  /* INT_MIN */
    if (!((long double)g_i32p == p31 - g_one)) return 28;  /* INT_MAX */
    if (!((long double)g_ineg3 == -(g_one + g_one + g_one))) return 28;
}

/* ── ARM 29: unsigned → long double — THE RANGE ARM ───────────────────*/
{
    long double p31 = g_one;
    int         k;
    for (k = 0; k < 31; ++k) p31 = p31 * g_two;      /* 2^31 */
    {
        long double p32 = p31 * g_two;               /* 2^32 */
        if (!(p32 > p31))                        return 29;  /* non-vacuity */
        /* 2^31 does not fit a SIGNED 32-bit read: that answers -2^31. */
        if (!((long double)g_u32h == p31))       return 29;
        if ((long double)g_u32h < g_zero)        return 29;
        if (!((long double)g_u32m == p32 - g_one)) return 29;
    }
}

/* ── ARM 30: long long → long double, WIDTH and SIGN ──────────────────*/
{
    long double p = g_one;
    long long   n = g_l1;
    int         k;
    for (k = 0; k < 53; ++k) { p = p * g_two; n = n * 2; }   /* 2^53 */
    if (!(p > g_one))              return 30;   /* non-vacuity */
    if (!((long double)n == p))    return 30;
    if (!((long double)(-n) == -p)) return 30;  /* the SIGN arm */
    /* -1 must be -1 and not 2^64-1: the whole difference between the
     * signed and unsigned 64-bit forms, and it does not fault. */
    if (!((long double)(-g_l1) == -g_one)) return 30;
    {
        /* 2^53 + 1 — representable exactly where `long double` is strictly
         * wider than `double`, and rounding back to 2^53 where it is not.
         * The expectation comes from the TYPE's own arithmetic, so the arm
         * is exact on every axis and still misses under any conversion
         * that truncated to 32 bits or went through a float. */
        long double want = (p + g_one > p) ? (p + g_one) : p;
        if (!((long double)(n + 1) == want)) return 30;
    }
}

/* ── ARM 31: unsigned long long → long double — THE HARD RANGE ────────*/
{
    long double        p63 = g_one;
    unsigned long long n63 = g_ull1;
    int                k;
    for (k = 0; k < 63; ++k) { p63 = p63 * g_two; n63 = n63 << 1; }
    {
        long double        h62  = p63 / g_two;          /* 2^62         */
        long double        x    = p63 + h62;            /* 1.5 * 2^63   */
        long double        t    = g_one;
        long double        top;
        unsigned long long ntop = ~(unsigned long long)0 - 2047ULL;
        for (k = 0; k < 11; ++k) t = t * g_two;         /* 2048         */
        top = p63 + (p63 - t);                          /* 2^64 - 2048  */

        /* Non-vacuity, both sides: the doubling really reached the range
         * this arm is about, and the shifting really reached 2^63. */
        if (!(p63 > h62))              return 31;
        if (!(x > p63))                return 31;
        if (!(top > x))                return 31;
        if (!((n63 >> 1) * 2ULL == n63)) return 31;
        if (!(n63 > (n63 >> 1)))       return 31;

        /* The below-2^63 CONTROL, where a plain signed read is already
         * the right answer. */
        if (!((long double)(n63 >> 1) == h62))         return 31;

        /* THE BOUNDARY AND ABOVE. All three are exact in binary64, x87-80
         * and binary128 alike — in [2^63, 2^64) a binary64's ulp is 2048 —
         * so this reads the same on every axis with nothing calibrated. */
        if (!((long double)n63 == p63))                return 31;
        if (!((long double)(n63 + (n63 >> 1)) == x))   return 31;
        if (!((long double)ntop == top))               return 31;

        /* THE ODD VALUES, self-calibrated. 2^63+1 and 2^64-1 need a
         * 64-bit significand, which the f64 axis has not got; where the
         * type CAN hold them they are what catches a low-bit mask other
         * than 1, and a split that dropped the low bit entirely. */
        if (p63 + g_one > p63) {
            if (!((long double)(n63 + 1ULL) == p63 + g_one))   return 31;
            if (!((long double)(~(unsigned long long)0)
                  == p63 + (p63 - g_one)))                     return 31;
        }
    }
}

/* ── ARM 32: THE x87 STACK BALANCE FOR THE FOUR NEW SEQUENCES ─────────
 * The unsigned-64 conversion pushes the eight-deep x87 stack THREE times
 * for one C operation — the halved value twice and the low bit once — so
 * it is the deepest sequence in this file and the one most able to leak a
 * pop. Leak one and the stack overflows inside three iterations, after
 * which every result is an indefinite NaN.
 *
 * ★ ITS SOURCE ALTERNATES BETWEEN TWO VALUES so the conversion cannot be
 * hoisted out of the loop and the arm measured once: both are exact in
 * every long-double format here (1.5*2^63 has two significant bits and
 * 0.75*2^63 = 3*2^61 has two as well), so the check needs no calibration.
 * The other three run against globals, as arms 20 and 27 do. */
{
    long double        p31 = g_one, p63 = g_one, x, xhalf;
    unsigned long long ubig = g_ull1;
    long double        acc = g_zero, count = g_zero;
    int                i, k;
    for (k = 0; k < 31; ++k) p31 = p31 * g_two;
    for (k = 0; k < 63; ++k) { p63 = p63 * g_two; ubig = ubig << 1; }
    ubig  = ubig + (ubig >> 1);           /* 1.5 * 2^63, by SHIFTING     */
    x     = p63 + (p63 / g_two);          /* 1.5 * 2^63, by DOUBLING     */
    xhalf = x / g_two;                    /* 0.75 * 2^63                 */
    if (!(x > p63))     return 32;        /* non-vacuity                 */
    if (!(x > xhalf))   return 32;
    for (i = 0; i < g_iters; ++i) {
        unsigned long long src = ((i & 1) != 0) ? ubig : (ubig >> 1);
        long double        wnt = ((i & 1) != 0) ? x    : xhalf;
        long double a = (long double)g_ineg3;   /* fild m32            */
        long double b = (long double)g_u32h;    /* zext + fild m64     */
        long double c = (long double)g_l1;      /* fild m64            */
        long double d = (long double)src;       /* the exact split     */
        if (!(a + (g_one + g_one + g_one) == g_zero)) return 32;
        if (!(b == p31))   return 32;
        if (!(c == g_one)) return 32;
        if (!(d == wnt))   return 32;
        acc   = acc + (a + c);            /* -3 + 1 = -2 per iteration  */
        count = count + g_one;
    }
    if (!(count > g_zero))                return 32;   /* the loop ran  */
    if (!(acc + (count + count) == g_zero)) return 32;
}

    /* ARM 33, likewise. */
    { int rc = check_long_double_truthiness(); if (rc != 0) return rc; }

    /* ARM 34, likewise. */
    { int rc = check_long_double_to_subnative(); if (rc != 0) return rc; }

    return 42;
}
