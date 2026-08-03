/* D-CSUBSET-FLOAT-FROM-DOUBLE-NARROWING (C 6.3.1.4 / 6.5.16.1): the implicit
 * double->float NARROWING assignment conversion, exercised in EVERY context — init,
 * assign, store-through-pointer, function-ARGUMENT, and `return`.
 *
 * The sqlite frontier: GEOPOLY `typedef float GeoCoord` is assigned `double` RHS
 * (geopolyParseNumber / geopolyRegularFunc) — DSS admitted float->double WIDENING but
 * rejected double->float NARROWING in every context (assign/call-arg/store -> S0003,
 * return -> S0008). The new `floatSameKindNarrows` gate admits a wider float -> a
 * narrower float lhs; coerce()'s arithmetic-core arm materializes the MIR FPTrunc (the
 * SAME makeCast path F32->F64 widening already uses), and the target.json cvtsd2ss
 * (x86) / FCVT (arm64) encodings execute the round.
 *
 * The narrowing's NUMERIC correctness is the runtime proof it actually happened:
 *   2^24+1 = 16777217 is EXACT in f64 but sits EXACTLY between the floats 16777216 and
 *   16777218 -> round-to-nearest-EVEN gives 16777216.0f, so the precision LOST is
 *   exactly 1. A missing/no-op narrowing would keep 16777217 and the diff would be 0.
 *   2^25+1 = 33554433 rounds to 33554432.0f (float ULP at 2^25 is 4) -> diff 1.
 *
 * All inputs flow through io_d()/io_f() so the values stay RUNTIME across the optimized
 * (release) arm: the real cvtsd2ss/FCVT runs, not only a const-folded answer (and if the
 * release arm DOES fold, its ConstFold double->float rounding must AGREE with the base
 * arm's runtime FPTrunc — the example reds if they disagree).
 *
 * RED-ON-DISABLE: flip "floatSameKindNarrows" to false in c-subset.lang.json -> every
 * `float <- double` site fails error[S0003] (the return site error[S0008]) and the
 * program does NOT compile. The explicit `(int)` casts are FC2-legal independently and
 * are NOT the sites under test. */

double io_d(double x) { return x; }   /* opaque: keeps a double runtime */
float  io_f(float  x) { return x; }   /* opaque: keeps a float runtime  */

/* RETURN-position narrowing: a double parameter, a float RETURN (implicit double->float,
 * the checkReturn isAssignable site — reverting the gate makes this error[S0008]). */
float narrow_ret(double x) { return x; }

/* CALL-ARG position: a float parameter receiving a double actual (narrows at the arg). */
int as_int(float x) { return (int)x; }

int main(void) {
    /* (1) INIT: implicit double -> float */
    double d1 = io_d(16777217.0);   /* 2^24 + 1 */
    float  f1 = d1;                 /* narrow -> 16777216.0f */
    if ((int)f1 != 16777216) return 1;
    if ((int)(d1 - f1) != 1) return 2;   /* 16777217 - 16777216 = 1 (0 if no narrowing) */

    /* (2) ASSIGN: implicit double -> float */
    double d2 = io_d(33554433.0);   /* 2^25 + 1 -> nearest float 33554432.0f */
    float  f2;
    f2 = d2;
    if ((int)f2 != 33554432) return 3;
    if ((int)(d2 - f2) != 1) return 4;

    /* (3) STORE through a float* : the narrowing happens at the store site */
    float  sbuf;
    float *pf = &sbuf;
    *pf = io_d(16777217.0);
    if ((int)sbuf != 16777216) return 5;

    /* (4) CALL-ARG: a double actual into a float parameter narrows at the arg */
    if (as_int(io_d(16777217.0)) != 16777216) return 6;

    /* (5) RETURN: a double -> float function return narrows (the S0008 path) */
    float f5 = narrow_ret(io_d(16777217.0));
    if ((int)f5 != 16777216) return 7;

    /* SCOPE GUARD (must STILL hold): F32 -> F64 WIDENING is unchanged by the gate. */
    float  fw = io_f(2.5f);
    double dw = fw;                 /* implicit float -> double widening : 2.5 */
    if ((int)(dw * 4.0) != 10) return 8;

    return 42;
}
