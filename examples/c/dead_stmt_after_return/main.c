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
// A regression that re-broke the predicate calls `f`'s end REACHED. Until P69
// that failed this example's COMPILE step (H0003, rc != 0). Under C it no longer
// does: C23 6.9.2p13 accepts a non-void function whose end is reached (only USING
// the value is undefined), so the compile succeeds, reports `f` with the warning
// H_NonVoidFunctionEndReachable, and the process still exits 42 — the `return 42`
// runs first. The manifest therefore FORBIDS that code (`forbidDiagnostics`): the
// arm is red on the diagnostic, in both runners. The exit code alone cannot see
// the regression; the dead `;` carries no runtime effect either way.
int f(void) {
    return 42;
    ;  // dead trailing null statement — warned unreachable, no runtime effect
}

int main(void) {
    return f();
}
