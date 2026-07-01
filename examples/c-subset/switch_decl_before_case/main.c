/* c69 (D-MIR-ENTRY-BLOCK-ALLOCA-HOIST): a local DECLARATION with an INITIALIZER
 * appears BEFORE the first `case` label of a `switch`. Per C11 6.8 the name is in
 * scope across the whole switch, but control jumps directly to a case label, so
 * the initializer (and its side effects) is never executed — yet the local's
 * STORAGE must still exist for the reachable case bodies that use it.
 *
 * The c60 switch-flatten places the pre-first-case statements in a predecessor-
 * less block. Before c69, the local's storage slot (Alloca) was emitted into that
 * dead block; the mandatory unreachable-prune (D-MIR-UNREACHABLE-PRUNE-NORMALIZE)
 * dropped the block while the reachable case bodies still Load/Store the slot →
 * the MIR optimization rebuilder aborted with `rewriteOperand: old MirInstId v=N
 * has no rewrite entry` (D-OPT2-REWRITE-MAP-COMPLETENESS), a SIGABRT compiler
 * crash. c69 hoists every local's Alloca to the function entry block (which
 * dominates all uses), leaving the dead pre-case block value-free; the init Store
 * stays at the (dead) decl site, so the jumped-over initializer still never runs.
 *
 * Verifies BOTH: (1) it compiles+runs (no crash) and the case bodies see their
 * own assigned value; (2) the jumped-over initializer's side effect did NOT run.
 * RED-ON-DISABLE: revert c69 → the compiler SIGABRTs (exit 134) on this file.
 * gcc/clang compile it and exit 42.
 */

static int g_sideEffect = 0;

static int bump(void) { g_sideEffect = 111; return 999; }

int classify(int n) {
    switch (n) {
        int local = bump();          /* decl+init BEFORE the first case: jumped over */
        case 1:
            local = 40;              /* reachable assignment (well-defined) */
            return local + g_sideEffect;   /* 40 + 0 */
        case 2:
            local = 7;
            return local + g_sideEffect;   /* 7 + 0 */
        default:
            return g_sideEffect;           /* 0 */
    }
}

int main(void) {
    int a = classify(1);   /* 40 */
    int b = classify(2);   /* 7  */
    int c = classify(9);   /* 0  (default) */
    if (g_sideEffect != 0) return 1;   /* the jumped-over `= bump()` must NOT have run */
    return a - b + c + 9;              /* 40 - 7 + 0 + 9 = 42 */
}
