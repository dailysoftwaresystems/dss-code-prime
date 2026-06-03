// D-CSUBSET-WHILE-LOOP-SUBSTRATE corpus expansion (step 13.5 cycle 2,
// 2026-06-03): countdown sum 10+9+8+...+1 = 55. Distinct from
// `sum_loop` (count UP with `<=`) by using count-DOWN with `!=` cond.
// Exercises:
//   * The Ne TargetCondCode (eq-nibble 5) which the cmp_all_signed_
//     conds test pins but no other loop-bearing example uses for the
//     LOOP-EXIT comparison.
//   * Decrement-toward-zero pattern (the classic "loop while non-
//     zero") — a future OPT1 anchor for the loop-strength-reduction
//     pass that may transform this into the equivalent `while (n > 0)`
//     form.
//
// Future OPT1 (13.6) differential-verification: const-fold can prove
// at compile time that `sum(1..10) == 55` and replace the entire main
// body with `return 55`; the runnable exit code stays 55 regardless.
// Loop-invariant code motion has nothing to lift here (both `n` and
// `sum` mutate every iteration), pinning the negative case.

int main() {
    int n;
    int sum;
    n = 10;
    sum = 0;
    while (n != 0) {
        sum = sum + n;
        n = n - 1;
    }
    return sum;
}
