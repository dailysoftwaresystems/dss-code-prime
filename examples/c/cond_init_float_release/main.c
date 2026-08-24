/* D-OPT-MEM2REG-CONDITIONAL-INIT-UNDEF — the FPR (float) sibling of cond_init_release.
 *
 * A conditionally-initialized DOUBLE has no directly-lowerable zero `Const` (register
 * machines have no float-immediate form; DSS routes float constants through rodata),
 * so materializing an entry-block `Const 0` of float type would be unlowerable. Instead
 * Mem2Reg DE-PROMOTES a conditionally-initialized float alloca — leaves it as memory,
 * which is always correct (exactly what the debug pipeline does) — rather than emit an
 * unlowerable float Const. Promoting these via a rodata-zero is a deferred perf
 * refinement (D-OPT-MEM2REG-FPR-CONDITIONAL-INIT-RODATA-ZERO); correctness does not
 * depend on it.
 *
 * ANTI-FOLD: `g_c` mutable global. g_c == 1 → then-branch → x = 1.0 → return 1.
 * RED-on-disable: revert the FPR/aggregate de-promotion in src/opt/passes/mem2reg.cpp
 * → Mem2Reg tries a float `Const 0`, hits the non-zeroable assert, and the release arm
 * aborts. */
int g_c = 1;   /* mutable global: runtime-opaque */

double f(int c) {
    double x;
    if (c) {
        x = 1.0;
    }
    return x;
}

int main(void) { return (int)f(g_c); }   /* g_c=1 → x=1.0 → 1 */
