// D-CSUBSET-MULTI-FN-WIN64-CC corpus expansion (step 13.5 cycle 2
// post-fold, 2026-06-03): first runnable c-subset RECURSIVE
// function. fact(5) = 5 * 4 * 3 * 2 * 1 = 120. Exercises:
//   * Self-recursive call — same name resolution as multi-function
//     case, but the callee == caller. Stresses regalloc's caller-
//     saved register handling across recursive depth.
//   * Argument propagation through 5 nested call frames (n=5, 4, 3,
//     2, 1) — each frame's arg-reg setup overwrites the previous
//     frame's caller-saved register set.
//   * Conditional early-return inside the recursive body (`if (n <=
//     1) return 1`) — exercises ICmp+CondBr fusion (Sle) AND the
//     control-flow merge BEFORE the recursive call's return-value
//     handling.
//   * Return-value chain (mul-by-n at each frame) — every frame's
//     `return n * fact(n-1)` materializes a `mul` AFTER the
//     recursive call returns; stresses the frame's outgoing-arg
//     area being correctly torn down by the epilogue before the
//     mul reads the new arg.
//
// Future OPT1 (13.6) differential-verification anchor for:
//   * Tail-call elimination (fact is NOT tail-recursive due to the
//     trailing `n *` — pinning the non-eligible case).
//   * Inlining heuristics (small leaf function — should NOT be
//     inlined since recursive; pins the inliner's no-recursive-
//     inline rule).
//   * Strength-reduction of integer multiplication (the chain
//     5*4*3*2*1 has constant-folding potential; the runnable exit
//     code 120 stays correct under any const-fold rewrite).

int fact(int n) {
    if (n <= 1) {
        return 1;
    }
    return n * fact(n - 1);
}

int main() {
    return fact(5);
}
