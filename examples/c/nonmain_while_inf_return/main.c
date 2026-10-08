// Regression corpus for D-HIR-INFINITE-LOOP-NOT-TERMINATING (the anchor's
// exact repro). A NON-`main` non-void function `f` whose terminating tail is a
// provably-infinite `while (1)` whose only exit is its own `return`. Before the
// fix this was OVER-REJECTED at the HIR verifier (H0003: "non-void function may
// fall through without returning a value") — the verifier treats a loop body as
// non-terminating, and lowering never synthesized the documented `Unreachable`
// after the loop. Now `lowerWhile` wraps the provably-infinite loop as
// `Block{ while, Unreachable }`, so the body structurally terminates and the
// non-void check passes. Called from `main` so it actually runs; the loop's
// `return 5` is the observable exit (the synthetic Unreachable is never reached at runtime).
//
// WHAT GOES RED IF THE WRAP IS LOST: `f`'s end is then called REACHED. Until P69
// that refused the compile (the H0003 above). Under C it no longer does — C23
// 6.9.2p13 accepts a non-void function whose end is reached — so the compile
// succeeds with the warning H_NonVoidFunctionEndReachable and the program still
// exits 5. The manifest therefore FORBIDS that code (`forbidDiagnostics`): the arm
// is red on the diagnostic, in both runners, where the exit code could not see it.
int f(int x) {
    while (1) {
        return 5;
    }
}

int main() {
    return f(0);
}
