/* D-OPT-MEM2REG-CONDITIONAL-INIT-UNDEF (post-sqlite C23+perf arc) — the RELEASE
 * witness for a benign CONDITIONALLY-INITIALIZED live local.
 *
 * `x` is stored on only ONE arm of the `if`, then read at the merge where it is
 * live-in. The release pipeline builds minimal SSA via Mem2Reg, which places a phi
 * at the merge whose ELSE-edge incoming has NO reaching store — DSS previously
 * ABORTED the whole release compile there (`Mem2Reg fatal: phi ... has no reaching
 * value from predecessor`). That is VALID C — gcc compiles it and SQLite is full of
 * it — so aborting was the bug; the missing incoming is now materialized as
 * undef-as-zero (an entry-block `Const 0`, mirroring gcc/LLVM's `undef`). The BASELINE
 * (debug) arm always worked — debug runs no Mem2Reg — so the `release` arm is the
 * witness.
 *
 * ANTI-FOLD: `g_c` is a mutable global (runtime-opaque) so the optimizer cannot fold
 * `f(1)` and elide the conditional. `g_c == 1`, so the then-branch runs → x = 1 →
 * return 1 + 1 = 2. (The undef ELSE edge is never dynamically taken — a correct
 * program cannot observe the zero, since reading an uninitialized value is UB; the
 * PINNED property is that the release compile no longer ABORTS and runs the
 * conditional-init correctly.) RED-on-disable: revert the undef-as-zero
 * materialization in src/opt/passes/mem2reg.cpp → the release arm Mem2Reg-aborts. */
int g_c = 1;   /* mutable global: runtime-opaque */

int f(int c) {
    int x;
    if (c) {
        x = 1;
    }
    return x + c;
}

int main(void) { return f(g_c); }   /* g_c=1 → x=1 → 1 + 1 = 2 */
