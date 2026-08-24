// D-OPT1-CSE-CORPUS (step 13.5 cycle 2 post-fold, 2026-06-03): the
// expression `(a + b)` is computed TWICE in straight-line code with
// no intervening mutation of `a` or `b`. A correct CSE (common
// subexpression elimination) pass replaces the 2nd computation
// with a load of the 1st result:
//
//     int t = a + b;
//     x = t * 2;
//     y = t * 3;
//
// (5+3)*2 + (5+3)*3 = 16 + 24 = 40.
//
// This corpus row pins that ANY OPT1 CSE rewrite preserves the
// runtime exit code 40. A CSE bug that incorrectly identified two
// DIFFERENT expressions as identical (false-positive CSE) would
// shift the exit code; a CSE bug that PROPAGATED the wrong source
// value (false-positive copy-prop) likewise shifts it.
//
// Also exercises:
//   * Two distinct expressions sharing a common sub-tree (`a + b`)
//     — stresses the value-numbering equivalence relation.
//   * Multiplication by 2 vs 3 on the shared value — pins that
//     CSE doesn't accidentally merge non-identical TOP-level
//     expressions just because they share a sub-tree.
//   * The final `x + y` sum re-uses both results, pinning that
//     downstream consumers see the same value as the un-CSE'd
//     program.

int main() {
    int a;
    int b;
    int x;
    int y;
    a = 5;
    b = 3;
    x = (a + b) * 2;
    y = (a + b) * 3;
    return x + y;
}
