/* D-CSUBSET-VARIADIC-DEFAULT-ARG-PROMOTION — the runtime witness that C's
 * default ARGUMENT promotions (C 6.5.2.2p6) are applied to a SCALAR variadic
 * tail: an integer narrower than `int` promotes to `int` (SIGN-extended for a
 * signed source, ZERO-extended for an unsigned one), and a `float` widens to
 * `double`.
 *
 * THE BUG this pins (a confirmed SILENT miscompile): `coerceCallArg` used to
 * pass a narrow-int variadic arg through UNCHANGED, so the value sat in a
 * 32-bit register with the default ZERO-fill — a signed `short -1` arrived as
 * 65535, a `signed char -1` as 255. This broke sqlite's `sqlite3_expert_new`
 * (nArg = -1 arrived as 65535 > SQLITE_MAX_FUNCTION_ARG → the expert API was
 * rejected). A `float` arg was pushed as 32 bits where `va_arg(ap, double)`
 * reads 64 → garbage.
 *
 * THE DISCRIMINATOR: `ss` (signed short -1) and `us` (unsigned short 65535)
 * have the SAME low-16 bits (0xFFFF) but MUST reach `va_arg(ap, int)` as
 * DIFFERENT 32-bit values — -1 via sign-extension vs 65535 via zero-extension.
 * Only a signedness-keyed promotion (the mapCast SExt/ZExt choice, driven by
 * the arg's OWN type) gets both right. `us` is the control: it must STAY 65535
 * (a wrong over-broad SExt would turn it into -1).
 *
 * The integer operands derive from the VOLATILE `one` and the float from a
 * VOLATILE local, so they are genuine RUNTIME values the optimizer cannot
 * const-fold the promotion away from. The two checkers bear `va_start`, so the
 * release pipeline's inlineLegalityGate refuses to inline them (see
 * varargs_inline_release) — the `release` arm therefore exercises the real
 * call-site promotion under the optimizer.
 *
 * Exit 42 iff every recovered value is correct.
 *
 * RED-ON-DISABLE: revert the scalar promotion in coerceCallArg
 * (src/hir/lowering/cst_to_hir.cpp) → `a` becomes 65535 (not -1) → check_ints
 * returns 1 → exit 1, not 42 (and the float arm would return 4). */
#include <stdarg.h>

volatile int one = 1;

static int check_ints(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int a = va_arg(ap, int);   /* signed short  -1     -> must be -1    */
    int b = va_arg(ap, int);   /* signed char   -1     -> must be -1    */
    int c = va_arg(ap, int);   /* unsigned short 0xFFFF -> must be 65535 */
    va_end(ap);
    if (a != -1)    return 1;
    if (b != -1)    return 2;
    if (c != 65535) return 3;   /* the unsigned control: ZExt, NOT SExt */
    return 0;
}

static int check_fp(int n, ...) {
    va_list ap;
    va_start(ap, n);
    double d = va_arg(ap, double);   /* float -1.5f -> promoted -> -1.5 */
    va_end(ap);
    if (d != -1.5) return 4;
    return 0;
}

int main(void) {
    short          ss = (short)(-one);           /* -1;    low16 = 0xFFFF */
    signed char    sc = (signed char)(-one);     /* -1;    low8  = 0xFF   */
    unsigned short us = (unsigned short)(-one);  /* 65535; low16 = 0xFFFF (same bits as ss) */
    /* A VOLATILE local seeds a runtime float WITHOUT an int->float cast — the
     * value stays unfoldable, and we sidestep the deferred int->F32 codegen gap
     * (D-CSUBSET-INT-TO-F32-CODEGEN) that `(float)someInt` would trip. */
    volatile float vf = -1.5f;
    float          f  = vf;                      /* -1.5f, runtime       */

    int e = check_ints(3, ss, sc, us);
    if (e != 0) return e;
    e = check_fp(1, f);
    if (e != 0) return e;
    return 42;
}
