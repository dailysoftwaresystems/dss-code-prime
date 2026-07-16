/* FC17.9(e) (D-CSUBSET-LONG-DOUBLE): runtime witness for `long double` on the
 * f64-axis formats (pe64 x86_64 + Apple arm64), where the axis collapses it to
 * binary64 (`long double` IS `double` per the MSVC x64 / Apple arm64 ABIs) and
 * the FULL double machinery serves it end-to-end: declarations, l/L-suffixed
 * literals, assignment, usual-arithmetic-conversion arithmetic, sizeof, and
 * float->int conversion. The x87-80/ieee128 formats (elf/macho x86_64,
 * linux-arm64) are deliberately ABSENT: long double VALUE arithmetic walls
 * loud there (L_UnsupportedLoweringForOpcode) until the per-format arithmetic
 * arcs land — the fail-loud pins live in the unit suites.
 *
 * ANTI-FOLD: the operands flow through MUTABLE GLOBALS (runtime-opaque, the
 * c11_atomic precedent), so the sum survives the release pipeline as real
 * runtime double arithmetic rather than a folded constant.
 *
 * exit = (int)(20.0L + 22.0L) = 42, with a sizeof(long double)==sizeof(double)
 * guard (the f64-axis identity) short-circuiting to 1 on any layout drift.
 */

long double g_a;
long double g_b;

int main(void) {
    g_a = 20.0L;
    g_b = 22.0L;
    long double sum = g_a + g_b;
    if (sizeof g_a != sizeof(double)) {
        return 1;
    }
    return (int)sum;
}
