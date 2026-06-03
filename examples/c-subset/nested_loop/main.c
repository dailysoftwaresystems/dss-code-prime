// D-CSUBSET-WHILE-LOOP-SUBSTRATE corpus expansion (step 13.5 cycle 2,
// 2026-06-03): nested `while` loops — 3 outer × 4 inner = 12. Exercises:
//   * Inner loop's CFG (header / body / exit) nested INSIDE outer loop's
//     CFG — stresses block-relative branch patches across nested back-
//     edges in one function.
//   * Inner counter `j` re-initialized on each outer iteration —
//     stresses regalloc's cross-iteration live-range handling for the
//     re-defined loop counter.
//   * Counter `n` lives across both inner AND outer iterations —
//     stresses the regalloc's "live-across-loop" register pinning.
//
// Becomes a future OPT1 (13.6) differential-verification anchor for:
//   * Loop-invariant code motion (inner loop's `n + 1` could be
//     hoisted outside? Actually no — `n` is mutated per iter; that's
//     correct.)
//   * Induction-variable substitution (i and j both linear).
//   * Loop-unswitching / nested-loop-fusion would alter this binary's
//     control-flow shape; the runnable exit code (12) pins correctness
//     regardless of how OPT1 rewrites the loop nest.

int main() {
    int i;
    int j;
    int n;
    n = 0;
    i = 0;
    while (i < 3) {
        j = 0;
        while (j < 4) {
            n = n + 1;
            j = j + 1;
        }
        i = i + 1;
    }
    return n;
}
