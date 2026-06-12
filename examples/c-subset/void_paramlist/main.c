// FC4 c1 stage 2b — C 6.7.6.3p10: a parameter list of exactly `(void)`
// declares ZERO parameters (config: `parameters.soleVoidMeansEmpty`).
//
// Discriminators:
//   * `int f(void)` must build FnSig([] -> I32) — a regression that kept
//     the void as a REAL parameter would make the zero-arg call `f()`
//     an S_ArgCountMismatch (compile failure IS the witness).
//   * `int main(void)` exercises the same normalization on the entry
//     function.
// A NAMED void param (`int g(void x)`) is S_InvalidVoidParam — pinned
// at the unit tier (NamedVoidParamFiresInvalidVoidParamPositioned).
int f(void) {
    return 42;
}

int main(void) {
    return f();
}
