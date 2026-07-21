// D-CSUBSET-BLOCK-TERMINATION-LAST-REACHABLE (TF-C40) end-to-end witness: a
// non-void function whose body ends in a trailing null statement `;` AFTER the
// `return` — the `MACRO(...);` idiom (a macro whose body ends in `return`,
// invoked with a `;`) lowers `;` to an empty Block placed as the body's LAST
// child, AFTER the `return`. gcc/clang compile this cleanly; before the fix DSS
// spuriously rejected it (H0003 "non-void function may fall through") because
// `pathTerminates` inspected only the literal last child (the empty Block) rather
// than the last REACHABLE statement (the `return`). Now the block terminates at
// its last reachable statement, so `f` compiles and returns 42.
//
// A regression that re-broke the predicate would fail this example's COMPILE step
// (H0003, rc != 0); the dead `;` carries no runtime effect, so a correct compile
// runs the reachable `return 42` and the process exits 42.
int f(void) {
    return 42;
    ;  // dead trailing null statement — warned unreachable, no runtime effect
}

int main(void) {
    return f();
}
