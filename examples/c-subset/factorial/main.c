// D-CSUBSET-WHILE-LOOP-SUBSTRATE corpus expansion (step 13.5 cycle 2):
// factorial(5) = 120 via a multiplicative loop. Distinct from sum_loop
// because the loop body MULTIPLIES (`mul`) rather than adds (`add`),
// and the loop counter DECREMENTS. Becomes a future OPT1 differential-
// verification anchor for: loop-invariant code motion (the constant
// `1` initializer); reduction-strength (constant-product unrolling);
// const-fold on the trivial-cases (`factorial(0)` = 1, `factorial(1)` = 1).

int main() {
    int n;
    int result;
    n = 5;
    result = 1;
    while (n > 1) {
        result = result * n;
        n = n - 1;
    }
    return result;
}
