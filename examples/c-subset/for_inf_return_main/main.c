// Regression corpus, `for(;;)` infinite-loop variant. A clause-less `for` is the
// second provably-infinite loop shape. As of D-HIR-INFINITE-LOOP-NOT-TERMINATING
// the loop is wrapped as `Block{ for, Unreachable }`, so `main` structurally
// terminates (no implicit `return 0` is appended); the synthetic `Unreachable`
// is dropped in MIR. The observable exit is the loop's `return 9`.
int main() {
    for (;;) {
        return 9;
    }
}
