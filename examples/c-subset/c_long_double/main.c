/* FC17.9(e) + D-CSUBSET-LONG-DOUBLE-X87-ARITH (LD-1): runtime witness for
 * `long double` across BOTH long-double axes.
 *   - f64-axis formats (pe64 x86_64 + Apple arm64): the axis collapses
 *     `long double` to binary64 (it IS `double` per the MSVC x64 / Apple arm64
 *     ABIs) and the full double machinery serves it.
 *   - x87-80 axis (elf64 x86_64 linux): `long double` is the 80-bit x87
 *     extended type and the arithmetic runs on the REAL x87 FPU — the literals
 *     widen double->80-bit extended in rodata, `g_a + g_b` lowers to
 *     fld/fld/faddp/fstp, and `(int)sum` to fld/fisttp/mov (LD-1). The ieee128
 *     formats (linux-arm64) remain absent (D-CSUBSET-LONG-DOUBLE-IEEE128-ARITH).
 * Exercised end-to-end: the `long double` declaration multiset, 20.0L/22.0L
 * l-suffixed FLOAT literals, assignment, usual-arithmetic-conversion arithmetic,
 * sizeof, and float->int conversion.
 *
 * ANTI-FOLD: the operands flow through MUTABLE GLOBALS (runtime-opaque, the
 * c11_atomic precedent), so the sum survives the release pipeline as real
 * runtime arithmetic rather than a folded constant.
 *
 * exit = (int)(20.0L + 22.0L) = 42. The `sizeof g_a >= sizeof(double)` guard
 * (a long double is NEVER narrower than a double — holds on the f64 axis where
 * they are equal AND on the x87-80 axis where it is 16 vs 8) short-circuits to
 * 1 on any layout drift, on either axis.
 */

long double g_a;
long double g_b;

int main(void) {
    g_a = 20.0L;
    g_b = 22.0L;
    long double sum = g_a + g_b;
    if (sizeof g_a < sizeof(double)) {
        return 1;
    }
    return (int)sum;
}
