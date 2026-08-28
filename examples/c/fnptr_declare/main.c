// FC4 c1 stage 2b — function-pointer DECLARATORS as runtime DATA
// (D-CSUBSET-FNPTR-INDIRECT-CALL stays the wall for calls THROUGH the
// pointer; this example proves the pointer VALUE itself flows).
//
// Fold-resistance (plan-lock MUST-FIX 8): the fn-ptr crosses a CALL
// BOUNDARY as an argument — `run(h, 40)`'s `f` parameter is a runtime
// ABI value (param-position declarator `int (*f)(int)` types it
// Ptr<FnSig>), so `f != 0` is a genuine runtime pointer compare against
// the null-pointer constant, not a const-foldable expression. The
// optimized arm (full release-like pass list) must preserve the
// GlobalAddr materialization + the compare.
//
// Exercised declarator forms:
//   * local `int (*fp)(int) = &helper;`  — fn-ptr object + &function
//     (GlobalAddr, C 6.5.3.2)
//   * file-scope `typedef int (*H)(int);` + local `H h = fp;` — the
//     typedef'd fn-ptr alias declares the SAME interned Ptr<FnSig>
//     (assignment compiles only when the two types agree exactly)
//   * param `int (*f)(int)` — fn-ptr through the call ABI
//
// NOTE: the typedef sits at FILE scope — a block-scope `typedef`
// STATEMENT does not parse (the statement alt has no typedefDecl arm;
// pinned residue, see the FC4 registry rows).
//
// Exit arithmetic: f arrives non-null -> bias + 2 = 42; a broken
// pointer flow (f == 0) -> 7. Delta 35 != 0 mod 256.
int helper(int v) { return v + 2; }

int run(int (*f)(int), int bias) {
    if (f != 0) {
        return bias + 2;
    }
    return 7;
}

typedef int (*H)(int);

int main() {
    int (*fp)(int) = &helper;
    H h = fp;
    return run(h, 40);
}
