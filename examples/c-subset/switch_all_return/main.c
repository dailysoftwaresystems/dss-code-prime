// D-MIR-UNREACHABLE-PRUNE-NORMALIZE corpus pin (the cf_switch shape): a
// `switch` in which EVERY arm `return`s. The frontend eagerly creates the
// switch-exit (SwitchJoin) block as the break/fall-through continuation;
// when no arm falls through to it, that block has ZERO predecessors and is
// unreachable-from-entry. Before the mandatory post-lowering prune the
// production MirVerifier rejected this (error[I_UnreachableBlock]). The
// prune drops the orphan exit centrally. x == 2 selects `case 2`, so the
// program returns 22.
int main() {
    int x = 2;
    switch (x) {
        case 1:
            return 11;
        case 2:
            return 22;
        default:
            return 99;
    }
}
