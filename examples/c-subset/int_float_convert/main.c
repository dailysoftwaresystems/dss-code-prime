/* D-CSUBSET-INT-FLOAT-CONVERSION: the C int<->float implicit ASSIGNMENT
 * conversion, BOTH directions, exercised in EVERY context — init, assign,
 * function-ARGUMENT (the sqlite blocker), and through a `volatile double` param.
 *
 * The sqlite frontier: kahanBabuskaNeumaierStep(pSum, iBig) passes an `i64`
 * (`iBig`) to a `volatile double` parameter — DSS had NO implicit int->float
 * conversion in isAssignable, so the call fired error[S0003] S_TypeMismatch.
 * (Even `double d = 5;` failed.) The two new gates intConvertsToFloat /
 * floatConvertsToInt admit both directions; coerce() materializes the MIR
 * SIToFP/UIToFP (int->float) and FPToSI (float->int), and the target.json
 * cvtsi2sd / SCVTF (int->double) + cvttsd2si / FCVTZS (double->int) encodings
 * execute the conversion.
 *
 * ALL cross-type transitions happen at an ASSIGNMENT-like site (init / assign /
 * call-arg / `return` / an explicit cast) — NO mixed-type binary arithmetic — so
 * this isolates the isAssignable conversion this cycle implements. All inputs come
 * through io() so the values stay RUNTIME across the optimized (release) arm: the
 * real conversion instructions run, not a const-folded answer.
 *
 * Each conversion's NUMERIC correctness is load-bearing:
 *   iBig   = io(1000000003)   i64, > 2^30 (a 32-bit-truncated convert would lose
 *                             value -> proves the i64-source REX.W / SCVTF-Xn path;
 *                             1000000003 < 2^53 so it is EXACT in f64)
 *   iSmall = io(7)            i32 (the 32-bit-source cvtsi2sd / SCVTF-Wn path)
 *
 *   di = iSmall               IMPLICIT int(i32)->double INIT   : 7.0
 *   da; da = iBig             IMPLICIT int(i64)->double ASSIGN : 1000000003.0
 *   roundTrip(iBig)           IMPLICIT i64->volatile double AT THE ARG (sqlite
 *                             shape); returns (long long)x -> 1000000003
 *   dv = io(5) ; dv += 0.5    a double local = 5.5
 *   int t = dv                IMPLICIT double->int, TRUNC toward zero : 5 (NOT 6)
 *
 * Assemble to 42 (every step same-typed or an assignment/cast conversion):
 *   big2  = roundTrip(iBig)            i64  : 1000000003   (i64->f64->i64 round trip)
 *   a = (int)(big2 - iBig) + 30        int  : 0 + 30 = 30  (i64 subtraction, then cast)
 *   b = (int)di                        int  : 7            (f64->i32 explicit)
 *   c = (int)da - 1000000003           int  : 0            (f64->i32 then i32 sub)
 *   d = t                              int  : 5            (the implicit f64->i32 trunc)
 *   return a + b + c + d               int  : 30+7+0+5 = 42
 *
 * RED-ON-DISABLE: flip "intConvertsToFloat" OR "floatConvertsToInt" to false in
 * c-subset.lang.json -> that direction's IMPLICIT assignment fails error[S0003]
 * S_TypeMismatch and the program does NOT compile. intConvertsToFloat guards the
 * `di` init, the `da` assign, and the roundTrip ARG (all int->double);
 * floatConvertsToInt guards `int t = dv` (double->int implicit). The explicit
 * `(int)`/`(long long)` casts are FC2-legal independently and are NOT the sites
 * under test. */

int io(int x) { return x; }   /* opaque: keeps inputs runtime across the optimized arm */

/* the sqlite shape: an integer argument flowing into a `volatile double` param. */
long long roundTrip(volatile double x) {
    return (long long)x;      /* f64 -> i64 (explicit) inside; the int->f64 is at the CALL */
}

int main(void) {
    long long iBig   = io(1000000003);   /* i64, > 2^30 */
    int       iSmall = io(7);

    double di = iSmall;        /* IMPLICIT int(i32) -> double, INIT  : 7.0 */
    double da;
    da = iBig;                 /* IMPLICIT int(i64) -> double, ASSIGN : 1000000003.0 */

    double dv = io(5);         /* IMPLICIT int(i32) -> double, INIT  : 5.0 */
    dv = dv + 0.5;             /* double + double : 5.5 (same-typed binary) */
    int t = dv;                /* IMPLICIT double -> int, TRUNC toward zero : 5 (not 6) */

    long long big2 = roundTrip(iBig);   /* IMPLICIT i64 -> volatile double at the ARG */

    int a = (int)(big2 - iBig) + 30;    /* (1000000003 - 1000000003) + 30 = 30 */
    int b = (int)di;                    /* 7 */
    int c = (int)da - 1000000003;       /* 1000000003 - 1000000003 = 0 */
    int d = t;                          /* 5 */

    return a + b + c + d;      /* 30 + 7 + 0 + 5 == 42 */
}
