// D-CSUBSET-MULTI-FN-WIN64-CC regression pin (step 13.5 cycle 2
// post-fold, 2026-06-03): helper function DECLARED FIRST in source
// order, then main. Pre-fix, the trampoline injector's
// `resolveUserEntrySymbol` silently fell back to functions[0] = the
// FIRST-declared function = helper, returning whatever rcx was on
// entry (typically 0). This example PINS the fix: with the new
// `AssembledModule.userEntrySymbol` populated from the language's
// `implicitReturnZeroForFunctionNames` config, the trampoline
// correctly routes to `main` regardless of declaration order.
//
// Without this corpus row, a future regression in the user-entry-
// symbol plumbing would silently re-introduce the bug class (the
// happy-path examples max_of_3 / multi_function ALSO declare helper
// before main, but their exit codes happen to equal a path that
// helper might reach — pinning sub(50, 8)=42 means a regression
// that called sub with rcx=0 would yield -8 (exit 248 on Win32)
// vs the expected 42).

int sub(int a, int b) {
    return a - b;
}

int main() {
    return sub(50, 8);
}
