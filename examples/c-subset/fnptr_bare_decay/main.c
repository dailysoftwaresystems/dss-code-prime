// Bare function-to-pointer DECAY (C 6.3.2.1p4) end-to-end RUNTIME witness: a
// function NAME used (with NO `&`) where a function pointer is expected decays
// to the function's address. This is the most common function-pointer idiom
// (vtables / callbacks / dispatch tables) and the regression that 901fe89's
// assign-stmt assignability check reintroduced — `fp = add` was wrongly
// rejected S0003 while `fp = &add` passed. Fixed by the function-to-pointer
// decay arm in `isAssignable` (the shared assignability chokepoint), so ALL
// THREE bare-name positions below now type-check: assignment, initializer,
// and call-argument.
//
// RED-ON-DISABLE: revert the isAssignable fn-decay arm and every bare-name use
// here fails to compile (S0003) -> the example no longer BUILDS (a compile
// failure, not a wrong exit). The `&add` form would have hidden the bug; the
// bare form is the witness this corpus previously lacked.
//
// Fold-resistance: `apply` is SELF-RECURSIVE (its own SCC -> the inliner
// refuses it), and the function pointer arrives + is passed onward as a
// call-boundary Arg, so `f(a, b)` stays a genuine indirect call through a
// register in the baseline AND the full release pipeline. The local `fp` / `gp`
// in main additionally witness the bare assignment / initializer compiling.
//
// Exit arithmetic: fp(10,5)=add=15, gp(3,4)=mul=12, apply(add,1,10,5)=add(10,5)=15
// -> 15 + 12 + 15 = 42. A broken decay (wrong address / a fail-loud / a copy)
// flips the exit or fails to build. arm64 runs under qemu; macho on macos-latest.
int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }

int apply(int (*f)(int, int), int n, int a, int b) {
    if (n) { return apply(f, n - 1, a, b); }   /* self-recursive: never inlined */
    return f(a, b);                            /* genuine indirect call */
}

int main(void) {
    int (*gp)(int, int) = mul;       /* BARE initializer (no &) */
    int (*fp)(int, int);
    fp = add;                        /* BARE assignment — the exact regression */
    return fp(10, 5)                 /* 15 */
         + gp(3, 4)                  /* 12 */
         + apply(add, 1, 10, 5);     /* BARE callback arg -> add(10, 5) = 15 */
}
