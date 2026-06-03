// D-CSUBSET-MULTI-FN-WIN64-CC closure (step 13.5 cycle 2 post-fold,
// 2026-06-03): first runnable c-subset program with USER FUNCTIONS
// CALLING OTHER USER FUNCTIONS. Pre-fix, the trampoline injector
// resolved the user entry by falling back to `functions[0]` which
// silently picked the FIRST-declared function (here: `max`),
// returning rcx (= the trampoline's RCX-on-entry, uninitialized).
//
// Substrate closed: AssembledModule.userEntrySymbol field —
// populated by the compile pipeline from the source-language's
// entry-function-name config (c-subset declares "main" in
// `implicitReturnZeroForFunctionNames`). The trampoline's
// `resolveUserEntrySymbol` consults it before falling through to
// the format JSON's `entryPoint` string or the functions[0]
// default.
//
// Exercises (becomes future OPT1 13.6 diff-verification anchor for):
//   * Cross-function call (main → max twice) — passes argument in
//     argGprs[0]=rcx (Win64 ms_x64) on both call sites.
//   * Helper function with parameters + own control flow (if-else +
//     early return) — stresses arg-read inside a function that has
//     internal branching.
//   * Multiple calls to the same helper from main — each call
//     materializes its own arg-load + call sequence.
//
// max(3, 7) = 7; max(7, 5) = 7 — exit 7.

int max(int a, int b) {
    if (a > b) {
        return a;
    }
    return b;
}

int main() {
    int x;
    x = max(3, 7);
    x = max(x, 5);
    return x;
}
