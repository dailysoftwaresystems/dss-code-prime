/* D-OPT-MEM2REG-LOOP-BODY-LOCAL-DEAD-PHI (p18 SQLite-readiness Cluster G c3) —
 * the RELEASE-pipeline witness that a local declared AND used entirely inside a
 * loop body promotes correctly.
 *
 * `iv` is stored (from `i * 2`) then read (into `total`) within the SAME loop-
 * body iteration → it is BLOCK-LOCAL (not upward-exposed). Minimal (un-pruned)
 * SSA placed a DEAD phi for it at the loop HEADER (the body-Store's iterated
 * dominance frontier reaches the header via the back-edge), and that phi's
 * entry-predecessor incoming was genuinely undefined (`iv` was never stored
 * before the loop) → Mem2Reg's empty-stack guard ABORTED the whole release
 * compile. This crashed the release pipeline on ANY `while (...) { int x = ...; }`
 * — an enormous, ordinary class of C. Semi-pruned SSA places no phi for a non-
 * upward-exposed alloca, so the compile succeeds and the loop runs.
 *
 * The BASELINE arm always worked (the debug pipeline runs no Mem2Reg) — which is
 * exactly why FC-era examples shipping baseline-only kept this hidden. The
 * `{"shippedPipeline":"release"}` arm is the witness.
 *
 * ANTI-FOLD: `g_n` is a mutable global (runtime-opaque), so the optimizer cannot
 * fold `f(5)` to a literal — the loop + its body-local run at runtime in the
 * release arm. exit 20 == 0 + 2 + 4 + 6 + 8. RED-on-disable: revert the upward-
 * exposed gate in src/opt/passes/mem2reg.cpp → the release arm Mem2Reg-aborts. */
int g_n = 5;   /* mutable global: runtime-opaque */

int f(int n) {
    int total = 0;
    int i = 0;
    while (i < n) {
        int iv = i * 2;          /* loop-body-local: Stored then read in the body */
        total = total + iv;
        i = i + 1;
    }
    return total;
}

int main(void) { return f(g_n); }   /* 0+2+4+6+8 = 20 */
