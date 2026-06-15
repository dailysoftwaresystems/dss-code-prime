// D15 cycle C corpus witness: the SHIPPED release pipeline must COMPILE
// AND RUN ordinary loop control flow. Under the shipped
// release.pipeline.json this is exactly the RC-B shape that used to fail
// `MirFunctionRebuilder fatal: rewriteOperand ... no rewrite entry`:
// Inlining splices the loop-bearing `f` into `main`, and the cloned
// loop body's continuation consumed a clone-defined value before it was
// laid out (the 1-return degenerate-Phi elision) — the layout-inversion
// the C1 by-construction topological pre-creation fixes.
//   f(5): acc=37; while(k){acc++; k--;} runs 5 times -> acc=42.
int f(int k) {
    int acc = 37;
    while (k) {
        acc = acc + 1;
        k = k - 1;
    }
    return acc;
}

int main() { return f(5); }
