// Regression corpus for the MIR Block-lowering fresh-dead-block path via its
// SECOND entry: a both-arms-return `if` (the D-HIR-SEALED-JOIN shape) seals its
// join with `Unreachable`, and a `return 99` follows it. Neither a `Block`
// wrapper nor an `IfStmt` is an unconditional terminator, so the HIR dead-code
// rule lets the trailing sibling through to MIR — where it previously aborted
// (lowering into the sealed if-join block). It now lowers into a fresh dead
// block that the MIR unreachable-prune drops. (This incidentally fixes a
// pre-existing latent crash of `<both-arms-return-if> <sibling>`.) Observable
// exit is `f(1)` -> the then-arm `return 1`; the `return 99` is pruned.
int f(int x) {
    if (x) {
        return 1;
    } else {
        return 2;
    }
    return 99;
}

int main() {
    return f(1);
}
