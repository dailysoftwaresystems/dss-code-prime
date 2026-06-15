// D-HIR-DEAD-CODE-AFTER-RETURN-REJECTED closure: ISO C permits a statement
// after an unconditional terminator (C 6.8.x has no reachability constraint on
// statements). The HIR verifier WARNS (`H_UnreachableCode`) rather than reject;
// the dead
// statement flows to MIR where the generic Block-lowering fresh-dead-block +
// the mandatory MIR unreachable-prune drop it, so runtime is unaffected.
//
// `f` has a `return 7;` after `return 42;`: the `return 7` is unreachable and
// WARNED (not rejected). Runtime is the reachable `return 42`, so f(0) == 42 and
// the process exits 42. A regression that re-rejected dead code would fail the
// compile step (rc != 0) of this example; a regression that miscompiled by
// keeping the dead `return 7` would exit 7, not 42.
int f(int x) {
    return 42;
    return 7;  // unreachable: warned, pruned in MIR
}

int main() {
    return f(0);
}
