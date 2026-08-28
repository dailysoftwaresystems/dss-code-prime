/* FC17.9(e) + D-CSUBSET-LONG-DOUBLE-X87-ARITH (LD-1) + -IEEE128-ARITH (LD-2) +
 * -AGGREGATE-ABI (LD-4): runtime witness for `long double` across ALL THREE
 * long-double axes AND across USER CALL BOUNDARIES (scalar + by-value struct).
 *   - f64-axis formats (pe64 x86_64 + Apple arm64): the axis collapses
 *     `long double` to binary64 (it IS `double` per the MSVC x64 / Apple arm64
 *     ABIs) and the full double CC serves both the scalar call and the aggregate.
 *   - x87-80 axis (elf64 x86_64 linux): `long double` is the 80-bit x87 extended
 *     type — the literals widen double->80-bit extended in rodata, `a + b` runs
 *     on the REAL x87 FPU (fld/fld/faddp/fstp, LD-1), the scalar `add` passes its
 *     args on the x87 STACK and returns in st0 (LD-4), and the by-value `struct S`
 *     goes BY REFERENCE (the SysV x87 MEMORY class, LD-4).
 *   - ieee128 axis (elf64 aarch64 linux, qemu): `long double` is IEEE binary128 —
 *     `a + b` lowers to the libgcc softfloat helper __addtf3 (LD-2); the scalar
 *     `add` passes/returns its args in v-registers, and the by-value `struct S`
 *     is a 1-element binary128 HFA (v0) — both the AAPCS64 v-register boundary
 *     (LD-4).
 * Exercised end-to-end: the `long double` declaration multiset, 20.0L/22.0L
 * l-suffixed FLOAT literals, assignment, usual-arithmetic-conversion arithmetic,
 * sizeof, float->int conversion, a SCALAR long-double call boundary (`add`), and
 * a BY-VALUE struct-with-long-double-leaf call boundary (`add_s`, passed AND
 * returned by value).
 *
 * ANTI-FOLD: the operands flow through MUTABLE GLOBALS (runtime-opaque, the
 * c11_atomic precedent), so the sums survive the release pipeline as real
 * runtime arithmetic rather than a folded constant.
 *
 * exit = (int)(20.0L + 22.0L) = 42. The `sizeof g_a >= sizeof(double)` guard
 * (a long double is NEVER narrower than a double — equal on the f64 axis, 16 vs 8
 * on the x87-80 / ieee128 axes) short-circuits to 1 on any layout drift; the
 * scalar and struct boundaries must agree ((int)sr.x == (int)sum) or exit is 2.
 */

long double g_a;
long double g_b;

/* A scalar `long double` ACROSS a call boundary (LD-4). */
long double add(long double a, long double b) {
    return a + b;
}

/* A by-value struct with a `long double` leaf, PASSED and RETURNED by value
 * (LD-4 aggregate ABI). */
struct S { long double x; };
struct S add_s(struct S a, struct S b) {
    struct S r;
    r.x = a.x + b.x;
    return r;
}

int main(void) {
    g_a = 20.0L;
    g_b = 22.0L;
    if (sizeof g_a < sizeof(double)) {
        return 1;
    }
    long double sum = add(g_a, g_b);            /* scalar call boundary */
    struct S sa;
    struct S sb;
    sa.x = g_a;
    sb.x = g_b;
    struct S sr = add_s(sa, sb);                /* by-value struct boundary */
    if ((int)sr.x != (int)sum) {
        return 2;
    }
    return (int)sum;                            /* = 42 */
}
