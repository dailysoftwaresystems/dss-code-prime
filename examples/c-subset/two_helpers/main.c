// D-CSUBSET-MULTI-FN-WIN64-CC corpus expansion (step 13.5 cycle 2
// post-fold, 2026-06-03): two DISTINCT user helpers called in
// sequence from main. doubled(5)=10; square(10)=100. Exercises:
//   * 3-function module (doubled / square / main). The entry-fn
//     name resolution picks `main` correctly via the new
//     `AssembledModule.userEntrySymbol` plumbing (pre-fix would
//     have silently picked `doubled` as functions[0]).
//   * Two helpers with the SAME parameter shape but DIFFERENT
//     return-value computation — pins that arg-reg setup at the
//     call site doesn't bleed state between adjacent calls.
//   * The 2nd call's input (`a`) is the 1st call's output —
//     stresses the result-vreg → next-call's arg-reg move
//     ordering. Pre-fix this WOULD have worked because the
//     entry was correctly `main`; this example PINS that the
//     fix didn't break the happy-path flow.
//
// Future OPT1 (13.6) differential-verification anchor for:
//   * Inlining of small leaf functions — `doubled` (1 add) and
//     `square` (1 mul) are obvious inlining candidates; the
//     runnable exit code 100 stays correct whether they're
//     inlined or not.
//   * Constant-propagation through a call chain — if OPT1 inlines
//     both helpers, the entire main body folds to `return 100`.

int doubled(int x) {
    return x + x;
}

int square(int x) {
    return x * x;
}

int main() {
    int a;
    int b;
    a = doubled(5);
    b = square(a);
    return b;
}
