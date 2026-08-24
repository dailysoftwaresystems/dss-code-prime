// Regression corpus for D-HIR-INFINITE-LOOP-NOT-TERMINATING, `do{...}while(1)`
// shape in a NON-`main` non-void function. The constant-truthy `while(1)`
// condition makes the loop provably-infinite (the body always re-enters). Pre-
// fix `f` was over-rejected H0003; now `lowerWhile` (do-while arm) wraps the
// loop as `Block{ do-while, Unreachable }`. Called from `main`; the loop body's
// `return 7` is the observable exit.
int f(int x) {
    do {
        return 7;
    } while (1);
}

int main() {
    return f(0);
}
