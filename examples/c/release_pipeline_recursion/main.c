// D15 cycle C corpus witness: the SHIPPED release pipeline must COMPILE
// AND RUN ordinary recursive control flow. `rec` is self-recursive (the
// inliner's SCC gate refuses rec->rec, so it survives out-of-line) and
// `main` calls it once. Under the shipped release.pipeline.json this is
// exactly the RC-A shape that used to fail `error[I_UnreachableBlock]`:
// ConstFold + SimplifyCfg fold the `if (k)` base-case CondBr inside the
// recursion, and SimplifyCfg must prune the dead arm in the SAME rebuild
// (post-fold reachability) so the per-pass verifier sees no orphan block.
//   rec(5) = rec(4)+1 = ... = rec(0)+5 = 37+5 = 42.
int rec(int k) {
    if (k) { return rec(k - 1) + 1; }
    return 37;
}

int main() { return rec(5); }
