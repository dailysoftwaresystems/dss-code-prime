/* FC17.9(f) (D-CSUBSET-COMPLEX): runtime witness for C99 `_Complex` across the legs,
 * RUN on BOTH the baseline (debug) AND the `release` (shippedPipeline) arm. `_Complex`
 * is a memory-resident by-value aggregate {re@0, im@elemSize} reached BY ADDRESS
 * (mirroring wide-BitInt); componentwise arithmetic is element-float ops. EIGHT arms
 * — the seven double-complex arms worth 5 each + the float-complex arm worth 7 →
 * exit 35+7 = 42 iff ALL pass (a failing arm drops the exit below 42):
 *   +  ·  -  ·  *  (cross-term)  ·  /  (the division witness)  ·  mixed real+complex
 *   ·  a complex-RETURNING user function (IMPORTANT-6: the widened Call arm reaches
 *      the result slot by address)  ·  conj (the imag-negate builtin)
 *   ·  `float _Complex` MULTIPLY (audit F1) — the F32-ELEMENT materializer at
 *      runtime: component offsets {0, 4}, F32 componentwise cross-term ops, plus
 *      BOTH element-convert directions (the F64-complex builtin result NARROWS
 *      FPTrunc×2 into the F32 globals; the creal/cimag args WIDEN FPExt×2 back to
 *      the F64-monomorph accessors).
 *
 * ANTI-FOLD: g_a/g_b/gf_a/gf_b are MUTABLE GLOBALS (runtime-opaque — the c11_atomic/
 * c_long_double precedent), so the release pipeline proves REAL runtime complex
 * arithmetic, not a folded constant. `I` = `__builtin_complex(0.0, 1.0)` (<complex.h>
 * macro); creal/cimag/conj route to the __builtin_creal/cimag/conj intrinsics
 * (cycle-1 F64-monomorph).
 *
 * The pe64 (Win64), elf-x86_64 (SysV), and elf-aarch64 (AAPCS64, qemu) codegen must
 * agree — F32/F64 components have no per-format axis. exit = 7×5 + 7 = 42.
 */

#include <complex.h>

double _Complex g_a;   /* (3, 4) */
double _Complex g_b;   /* (1, 2) */
float _Complex gf_a;   /* (2, 3) — the F32-element arm's operands */
float _Complex gf_b;   /* (1, 2) */

/* A complex-RETURNING user function — by-value complex return + param reception. */
double _Complex csum(double _Complex a, double _Complex b) {
    return a + b;
}

int main(void) {
    g_a = 3.0 + 4.0 * I;   /* mixed real+complex construct → (3, 4) */
    g_b = __builtin_complex(1.0, 2.0);
    gf_a = __builtin_complex(2.0, 3.0);   /* F64-complex → F32-complex: FPTrunc×2 */
    gf_b = __builtin_complex(1.0, 2.0);

    double _Complex s = g_a + g_b;             /* (4, 6) */
    double _Complex d = g_a - g_b;             /* (2, 2) */
    double _Complex p = g_a * g_b;             /* (3-8, 6+4) = (-5, 10) */
    double _Complex q = g_a / g_b;             /* (11/5, -2/5) = (2.2, -0.4) */
    double _Complex m = 10.0 + g_a;            /* mixed = (13, 4) */
    double _Complex r = csum(g_a, g_b);        /* fn-return = (4, 6) */
    double _Complex c = conj(g_a);             /* (3, -4) */
    float _Complex pf = gf_a * gf_b;           /* F32 cross-term: (2-6, 4+3) = (-4, 7) */

    int acc = 0;
    if ((int)creal(s) == 4  && (int)cimag(s) == 6)             acc += 5;   /* +   */
    if ((int)creal(d) == 2  && (int)cimag(d) == 2)             acc += 5;   /* -   */
    if ((int)creal(p) == -5 && (int)cimag(p) == 10)            acc += 5;   /* *   */
    if ((int)creal(q) == 2  && (int)(cimag(q) * 10.0) == -4)   acc += 5;   /* /   */
    if ((int)creal(m) == 13 && (int)cimag(m) == 4)             acc += 5;   /* mix */
    if ((int)creal(r) == 4  && (int)cimag(r) == 6)             acc += 5;   /* fn  */
    if ((int)creal(c) == 3  && (int)cimag(c) == -4)            acc += 5;   /* conj*/
    if ((int)creal(pf) == -4 && (int)cimag(pf) == 7)           acc += 7;   /* fC  */
    return acc;
}
