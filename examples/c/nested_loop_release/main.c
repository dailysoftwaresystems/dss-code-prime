/* D-OPT-MEM2REG-DEAD-PHI-PRUNE (p18 SQLite-readiness Cluster G c3) — the RELEASE
 * witness for the SUBTLER half of the dead-phi crash: a NESTED LOOP (no body-
 * locals required).
 *
 * The inner counter `j` is re-initialized (`j = 0`) at the top of EACH OUTER
 * iteration, then walked by the inner loop. So `j` is LIVE across the INNER
 * back-edge (a mere upward-exposed / "semi-pruned" test keeps ALL its phis) yet
 * DEAD at the OUTER loop header — where minimal SSA still placed a phi (the
 * outer-body `j=0` store's iterated dominance frontier reaches the outer header
 * via the outer back-edge) whose entry-predecessor incoming was undefined → the
 * Mem2Reg rename walk ABORTED the whole release compile. Only true LIVENESS
 * (fully-pruned SSA) prunes that outer-header phi; semi-pruning cannot. Nested
 * loops are ubiquitous in real C (and SQLite), so this crashed a huge class.
 *
 * The BASELINE arm always worked (debug runs no Mem2Reg); the `release` arm is
 * the witness. ANTI-FOLD: `g_n` is a mutable global (runtime-opaque), so the
 * optimizer cannot fold `f(2)`. exit 4 == 2*2 (the inner body runs n*n times).
 * RED-on-disable: revert the live-in gate in src/opt/passes/mem2reg.cpp → the
 * release arm Mem2Reg-aborts. */
int g_n = 2;   /* mutable global: runtime-opaque */

int f(int n) {
    int s = 0;
    int i = 0;
    while (i < n) {
        int j = 0;               /* inner counter — dead at the OUTER header */
        while (j < n) {
            s = s + 1;
            j = j + 1;
        }
        i = i + 1;
    }
    return s;
}

int main(void) { return f(g_n); }   /* 2 * 2 = 4 */
