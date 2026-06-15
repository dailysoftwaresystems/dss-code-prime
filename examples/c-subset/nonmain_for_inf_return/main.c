// Regression corpus for D-HIR-INFINITE-LOOP-NOT-TERMINATING, `for(;;)` shape in
// a NON-`main` non-void function. A clause-less `for` has an ABSENT condition,
// which is provably-infinite by construction. Pre-fix `f` was over-rejected
// H0003; now `lowerFor` wraps the loop as `Block{ for, Unreachable }` so the
// body structurally terminates. Called from `main`; the loop's `return 9` is the
// observable exit.
int f(int x) {
    for (;;) {
        return 9;
    }
}

int main() {
    return f(0);
}
