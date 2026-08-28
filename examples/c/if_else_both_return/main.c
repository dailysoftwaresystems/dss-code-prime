// D-MIR-UNREACHABLE-PRUNE-NORMALIZE corpus pin: an `if`/`else` where
// BOTH arms `return`, so the frontend's eagerly-created if-join block
// has ZERO predecessors — it is unreachable-from-entry. Before the
// mandatory post-lowering prune the production MirVerifier rejected this
// shape (error[I_UnreachableBlock] "... not reachable from entry"), so
// `int main(){ if(c){return 1;} else {return 2;} }` failed to compile at
// all. The prune drops the orphan join centrally; the program now
// compiles + runs. c is non-zero, so the then-arm runs: exit code 7.
int main() {
    int c = 1;
    if (c) {
        return 7;
    } else {
        return 9;
    }
}
