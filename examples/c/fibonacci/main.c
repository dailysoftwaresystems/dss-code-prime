// D-CSUBSET-WHILE-LOOP-SUBSTRATE corpus expansion (step 13.5 cycle 2):
// fibonacci(10) = 55 via the canonical two-variable rotation.
// Distinct from sum_loop (single accumulator) and factorial (single
// register-pressure path) because each iteration must read TWO live
// values (a + b) AND simultaneously update BOTH — stressing the
// regalloc's cross-iteration phi resolution + the LIR's parallel-
// move resolver. Becomes a future OPT1 (13.6) differential-
// verification anchor for: loop-invariant analysis (none — both
// vars vary every iteration); strength-reduction (negative case);
// induction-variable substitution (the `i++` counter).

int main() {
    int a;
    int b;
    int t;
    int i;
    a = 0;
    b = 1;
    i = 0;
    while (i < 10) {
        t = a + b;
        a = b;
        b = t;
        i = i + 1;
    }
    return a;
}
