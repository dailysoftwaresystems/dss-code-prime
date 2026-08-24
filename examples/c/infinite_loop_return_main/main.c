// Regression corpus for the infinite-loop control-flow path. `main`'s body is a
// single provably-infinite `while(1)` whose only exit is a `return`. As of
// D-HIR-INFINITE-LOOP-NOT-TERMINATING, lowering wraps the loop as
// `Block{ while, Unreachable }`, so `main` structurally terminates — the
// implicit-return-0 path is NOT taken (no `return 0` is appended). The synthetic
// `Unreachable` is never reached at runtime; the observable exit is
// the loop's `return 5`. (The D-HIR-LOOP-BODY-ONLY-RETURN-DOUBLE-ATTACH
// double-attach fix is now exercised by the straight-line non-terminating-main
// case — see tests/hir `NonTerminatingStraightLineMainNestsImplicitReturnZero`.)
int main() {
    while (1) {
        return 5;
    }
}
