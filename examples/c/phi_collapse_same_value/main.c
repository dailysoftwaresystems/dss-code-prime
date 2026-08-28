// D-OPT-COPYPROP-PHI-COLLAPSE-CORPUS: the canonical SSA Phi-collapse
// witness. Both arms of the conditional store the SAME value to `y`,
// so Mem2Reg produces `y_join = Phi(x_load, then; x_load, else)`
// whose two incomings resolve to the same SSA value. CopyProp's
// Phi-collapse algorithm rewrites every use of the Phi to that
// value; the dead Phi is then swept by the subsequent DCE pass.
//
// Runtime exit: 42. A buggy substitution that points uses elsewhere
// (e.g., to a stale `cond` value) would change the exit code. The
// IR-shape invariant (phisCollapsed == 1) is pinned by
// `DiamondSameValueCollapsesPhi` in `test_copy_prop.cpp`.

int main() {
    int x;
    int y;
    int cond;
    x = 42;
    cond = 1;
    if (cond > 0) {
        y = x;
    } else {
        y = x;
    }
    return y;
}
