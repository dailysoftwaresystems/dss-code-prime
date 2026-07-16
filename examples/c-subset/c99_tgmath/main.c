/* FC17.9(g) (D-CSUBSET-TGMATH): runtime witness for the C99/C23 <tgmath.h>
 * type-generic macros — pure config (shippedLibs/tgmath.json), zero compiler
 * change: each name is a function-like `_Generic` macro dispatching
 * float -> the f-variant (sqrtf/powf/...) and EVERYTHING ELSE (double, int via
 * implicit widening) -> the bare f64 function, over the shipped extern rows.
 *
 * DISPATCH IS OBSERVABLE, not assumed: `_Generic` selection is static and
 * `sizeof` does not evaluate its operand, so sizeof(sqrt(floatArg)) == 4 iff
 * the float arm (float RETURN) was selected and == 8 for the f64 default —
 * a wrong-arm regression flips the guard exits (1..6), not just precision.
 *
 * Exercised arms:
 *   - sqrt(float)  -> sqrtf        (the dss-state c99_tgmath_generic shape)
 *   - sqrt(double) -> sqrt         (default arm, self-reference blue paint)
 *   - sqrt(int)    -> sqrt         (int rides `default:` via int->f64 widening)
 *   - pow(float,float) -> powf ; pow(float,double) -> pow  (nested _Generic)
 *   - fabs(float) + ldexp(float,int): on pe64 msvcrt exports NO fabsf/ldexpf,
 *     so the macro's pe `variants` arm routes the float arm THROUGH the f64
 *     function — (float)fabs((double)(x)) — an EXACT round-trip (|x| and
 *     2^n*x of any F32 are exactly representable in F64); on elf/macho the
 *     real fabsf/ldexpf imports serve it (D-CSUBSET-MATH-FLOAT-VARIANTS-PE).
 *
 * ANTI-FOLD: operands flow through MUTABLE GLOBALS (runtime-opaque, the
 * c11_atomic precedent) so the release arm witnesses real dispatched libm
 * calls, not folded constants.
 *
 * RED-ON-DISABLE (descriptor): delete shippedLibs/tgmath.json -> the include
 * fails F_ShippedHeaderNotFound. RED-ON-DISABLE (cast discipline): flip a
 * default arm to `(double)(x)` and a `_Complex` argument compiles SILENTLY
 * (drops imag — the conformance miscompile); the bare arm keeps it S0003-loud
 * (pinned in test_semantic_analyzer_c_subset, D-CSUBSET-TGMATH-COMPLEX).
 *
 * exit = (int)sqrtf(1764.0f) = 42, every other arm contributing 0.
 */
#include <tgmath.h>

float  g_f;    /* 1764.0f -> sqrtf   */
double g_d;    /* 1764.0  -> sqrt    */
int    g_i;    /* 1764    -> default */
float  g_b;    /* 2.0f  (pow base)   */
float  g_e;    /* 5.0f  (pow float exponent)  */
double g_ed;   /* 5.0   (pow double exponent) */
float  g_n;    /* -7.5f (fabs/ldexp float)    */

int main(void) {
    g_f = 1764.0f;
    g_d = 1764.0;
    g_i = 1764;
    g_b = 2.0f;
    g_e = 5.0f;
    g_ed = 5.0;
    g_n = -7.5f;

    /* Compile-time dispatch witnesses, runtime-checked. */
    if (sizeof(sqrt(g_f))      != sizeof(float))  return 1; /* float -> sqrtf  */
    if (sizeof(sqrt(g_d))      != sizeof(double)) return 2; /* double -> sqrt  */
    if (sizeof(sqrt(g_i))      != sizeof(double)) return 3; /* int -> default  */
    if (sizeof(pow(g_b, g_e))  != sizeof(float))  return 4; /* f,f -> powf     */
    if (sizeof(pow(g_b, g_ed)) != sizeof(double)) return 5; /* f,d -> pow      */
    if (sizeof(fabs(g_n))      != sizeof(float))  return 6; /* float result on
                                                    every format (pe bridge) */

    int a = (int)sqrt(g_f);           /* sqrtf(1764.0f) = 42            */
    int b = (int)sqrt(g_d)  - 42;     /* sqrt(1764.0)   = 42 -> 0       */
    int c = (int)sqrt(g_i)  - 42;     /* int -> sqrt((double)1764) -> 0 */
    int d = (int)pow(g_b, g_e)  - 32; /* powf(2,5) = 32 -> 0            */
    int e = (int)pow(g_b, g_ed) - 32; /* mixed -> pow(2,5) = 32 -> 0    */
    int f = (int)fabs(g_n) - 7;       /* |-7.5f| = 7.5f -> 7 -> 0       */
    int g = (int)ldexp(g_b, 4) - 32;  /* 2*2^4 = 32 -> 0                */
    return a + b + c + d + e + f + g; /* 42                             */
}
