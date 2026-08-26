/* D-OPT-MEM2REG-CONDITIONAL-INIT-UNDEF — the FPR (float) sibling of cond_init_release.
 *
 * A conditionally-initialized DOUBLE now PROMOTES: its undef edge takes an anonymous
 * rodata zero (`GlobalAddr`+`Load` in the entry block) rather than an unlowerable float
 * `Const 0` — D-OPT-MEM2REG-FPR-CONDITIONAL-INIT-RODATA-ZERO, closed 2026-08-26.
 *
 * ⚠ THIS EXAMPLE'S JOB DID NOT CHANGE WHEN THAT LANDED, WHICH IS WHY IT IS STILL HERE:
 * it pins that the SHAPE COMPILES AND RUNS under release, and it went on passing across
 * the change (exit 1 either way) precisely because both a de-promoted and a promoted
 * lowering are correct. The PROMOTION itself is pinned by
 * `tests/opt/test_mem2reg.cpp::ConditionallyInitializedFloatAllocaPromotesViaRodataZero`
 * and by the sibling corpus example `examples/c/cond_init_float_promoted_release`.
 *
 * ★ The prose above used to describe de-promotion and stayed GREEN while saying something
 * false — an arm that cannot tell the two lowerings apart cannot notice which one shipped.
 * That is the whole reason the sibling example exists rather than this one being edited to
 * assert the new behaviour.
 *
 * ANTI-FOLD: `g_c` mutable global. g_c == 1 → then-branch → x = 1.0 → return 1. */
int g_c = 1;   /* mutable global: runtime-opaque */

double f(int c) {
    double x;
    if (c) {
        x = 1.0;
    }
    return x;
}

int main(void) { return (int)f(g_c); }   /* g_c=1 → x=1.0 → 1 */
