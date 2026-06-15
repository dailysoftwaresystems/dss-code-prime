// NEGATIVE corpus for D-HIR-INFINITE-LOOP-NOT-TERMINATING (no false positives):
// a `while (1)` with a `break` REACHABLE in the loop's own frame (through an
// `if`) is NOT provably-infinite — control can leave the loop — so `lowerWhile`
// must NOT wrap it with a synthetic `Unreachable`. The HIR-tier pin
// `BreakableInfiniteLoopIsNotWrapped` asserts the no-wrap directly; this corpus
// proves the program still COMPILES (it was never the over-rejected shape — the
// trailing `return 7` already terminated it) AND RUNS correctly: the loop
// counts x down from 5, the `break` fires when x reaches 0, and the function
// returns 7.
int f(int x) {
    while (1) {
        if (x == 0) {
            break;
        }
        x = x - 1;
    }
    return 7;
}

int main() {
    return f(5);
}
