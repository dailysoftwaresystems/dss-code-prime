// D-OPT1-LICM-CORPUS (step 13.5 cycle 2 post-fold, 2026-06-03): the
// product `a * b` inside the loop body is LOOP-INVARIANT — neither
// `a` nor `b` is mutated inside the loop. A correct LICM (loop-
// invariant code motion) pass hoists the multiply OUT of the loop:
//
//     int t = a * b;
//     while (i < 5) { acc = acc + t; i = i + 1; }
//
// This corpus row pins that ANY OPT1 LICM rewrite must preserve the
// runtime exit code 60 (= 5 * 3 * 4). A LICM bug that incorrectly
// hoisted a NON-invariant expression — say, `acc + a*b` instead of
// just `a*b` — would shift the exit code (e.g. the constant initial
// `acc=0` would dominate). Exit-code mismatch makes the bug
// bisectable.
//
// Also exercises:
//   * Two scalar locals (a, b) live across 5 loop iterations —
//     stresses regalloc's "live-across-loop" pinning.
//   * Multiplication INSIDE a loop body — pins mul's frame-aware
//     emission across loop back-edges.
//   * acc increment + i increment in same body — sequential updates
//     with phi merges at the header.

int main() {
    int a;
    int b;
    int i;
    int acc;
    a = 3;
    b = 4;
    acc = 0;
    i = 0;
    while (i < 5) {
        acc = acc + a * b;
        i = i + 1;
    }
    return acc;
}
