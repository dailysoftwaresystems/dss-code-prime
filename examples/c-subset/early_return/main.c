// D-CSUBSET-WHILE-LOOP-SUBSTRATE corpus expansion (step 13.5 cycle 2,
// 2026-06-03): early `return` inside a `while` loop. Cumulative sum
// 0+1+2+3+4+5+6 = 21 is the first iteration where the running sum
// exceeds 20; the loop exits via `return n` instead of the natural
// loop-exit condition (`i < 100`). Exercises:
//   * A function with TWO terminating return statements — one inside
//     the loop body's `if`, one after the loop. Stresses the c-subset
//     HIR's return-completeness check + the LIR's CFG laying-out the
//     two return blocks correctly.
//   * Forward branch (jcc) to a successor block that the loop body
//     ALSO reaches via fallthrough — the BlockRel32 patch list
//     handles both forward and backward branches in one function.
//
// Future OPT1 (13.6) differential-verification anchor for:
//   * Dead-code elimination (the trailing `return n` is reachable
//     ONLY when the loop runs to completion AND n stays ≤ 20 — for
//     this specific arithmetic the second return is dead, but OPT1
//     can't prove that without value-tracking; pinning the exit code
//     here ensures any DCE-induced behavioral change surfaces).
//   * Loop-exit-condition strengthening (a value-tracker could
//     replace `i < 100` with `i < 7` for this specific sum; the
//     exit code stays 21 either way).

int main() {
    int i;
    int n;
    i = 0;
    n = 0;
    while (i < 100) {
        n = n + i;
        if (n > 20) {
            return n;
        }
        i = i + 1;
    }
    return n;
}
