// Regression corpus for D-HIR-INFINITE-LOOP-NOT-TERMINATING — the dead-SIBLING
// case that exercises the MIR Block-lowering fresh-dead-block path. The
// provably-infinite `while(1)` is wrapped as `Block{ while, Unreachable }` (so
// `f` structurally terminates). The `return 99` AFTER the wrapper is dynamically
// unreachable, but the HIR dead-code rule permits it (a `Block` wrapper is not an
// unconditional terminator), so it reaches MIR — where the statement after the
// sealed wrapper lowers into a fresh zero-predecessor block that the mandatory
// MIR unreachable-prune (D-MIR-UNREACHABLE-PRUNE-NORMALIZE) drops. Observable
// exit is the loop's `return 5`; the `return 99` is pruned.
int f(int x) {
    while (1) {
        return 5;
    }
    return 99;
}

int main() {
    return f(0);
}
