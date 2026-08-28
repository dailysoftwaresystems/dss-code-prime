/* D-OPT-MEM2REG-FPR-CONDITIONAL-INIT-RODATA-ZERO — the FPR conditional-init local
 * that now PROMOTES instead of being left in memory.
 *
 * `double x; if (c) x = p; return x + q;` has an edge into the join on which `x` has
 * no reaching store. Mem2Reg materializes that missing incoming as undef-as-zero —
 * and a float has no directly-lowerable zero `Const` (register machines have no
 * float-immediate form), so the zero is minted the way DSS mints EVERY float
 * constant: an anonymous read-only global holding the zero bit pattern, reached by
 * `GlobalAddr` + `Load` in the ENTRY block. Previously such an alloca was
 * DE-PROMOTED (left as memory) — correct, but a per-access memory round trip under
 * release for a value that belongs in a register.
 *
 * WHAT THE RUNNABLE ARM IS ACTUALLY WORTH, stated rather than implied. The `release`
 * arm proves the promoted shape BUILDS, LINKS and RUNS on every target: the
 * entry-block rodata `Load` is emitted BEFORE the `Arg` receives (so it must not
 * disturb the incoming argument registers — `p` and `q` arrive in FPRs), the join
 * `Phi` is a double-typed SSA merge, and the result feeds a real `FAdd`. It does NOT
 * observe the zero's VALUE, and cannot: `g_c` is 1 so the undef edge is never taken
 * at runtime, exactly as its sibling `cond_init_float_release` arranges — executing
 * that edge would read an uninitialized object under the baseline (debug) pipeline,
 * and the runner diff-asserts baseline against optimized. The zero's value, binding,
 * section and literal encoding are pinned at the MIR tier instead, by
 * `Mem2Reg.ConditionallyInitializedFloatAllocaPromotesViaRodataZero` in
 * tests/opt/test_mem2reg.cpp.
 *
 * ANTI-FOLD: every input is a MUTABLE global, so the branch cannot be folded away
 * and the arithmetic cannot be constant-folded — the conditional-init shape has to
 * survive to Mem2Reg to be promoted.
 *
 * WHY `float`/`double` AND NOT `long double`, stated because the boundary is not
 * obvious: F80/F128 stay DE-PROMOTED. Their zero CONSTANT would materialize fine
 * through this same rodata path — but the PHI would not. ✔MEASURED 2026-08-26:
 * with them admitted, `long double x; if(c) x=p; return x+q;` at `--config=release`
 * REFUSES with `L_UnsupportedLoweringForOpcode` ("long double (F80/F128)
 * control-flow merge (phi) is not yet lowered … D-CSUBSET-LONG-DOUBLE-CONTROL-MERGE")
 * on both x86_64:elf64-x86_64-linux-exec (long double = F80) and
 * arm64:elf64-aarch64-linux-exec (long double = F128), while the same program builds
 * clean at `--config=debug`. A perf refinement may not turn a program that compiles
 * today into a refusal. F16 is out for a different reason: no encodings at any width.
 *
 * RED-on-disable: put F32/F64 back on the `ZeroForm::None` arm of `zeroFormFor` in
 * src/opt/passes/mem2reg.cpp (restoring the de-promotion) → the alloca/store/load
 * survive under release. The program still exits 42 (de-promotion is correct, just
 * slower), so the RED for this change is the unit-test pair, not this exit code;
 * what this example red-detects is a promoted-float shape that fails to BUILD or RUN
 * on any of the four targets.
 *
 * 10.0 + 32.0 = 42.0 -> 42. */

int    g_c = 1;      /* mutable: runtime-opaque condition */
double g_a = 10.0;   /* mutable: the conditionally-stored value */
double g_b = 32.0;   /* mutable: the addend */

double pick(int c, double p, double q) {
    double x;        /* conditionally initialized: the undef edge is the whole point */
    if (c) {
        x = p;
    }
    return x + q;    /* the join Phi's value, in a register once promoted */
}

int main(void) {
    return (int)pick(g_c, g_a, g_b);   /* 10.0 + 32.0 = 42.0 -> 42 */
}
