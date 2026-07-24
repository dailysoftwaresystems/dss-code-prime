/* c-TF (D-CSUBSET-ARRAY-DECAY-IN-DEREF — the function-pointer-ARRAY facet): `*(tbl)`
 * where `tbl` is an ARRAY of function pointers decays the array to Ptr<Ptr<FnSig>>
 * (C 6.3.2.1p3), so `(*(tbl))(x)` == `tbl[0](x)` calls the FIRST function pointer.
 * TWO tiers cooperate (they must AGREE on the type — semantic_analyzer.cpp:9209):
 *   - cst_to_hir `combineUnaryOp` Deref decays the array → the deref types as the
 *     fn-ptr (the FnSig fold reads the DECAYED Ptr<Ptr<FnSig>>, sees operand[0] is a
 *     Ptr not a FnSig, and correctly does NOT collapse — it yields the fn-ptr);
 *   - the semantic-tier `subtreeType` Deref MIRRORS the decay so checkCall resolves
 *     the callee `(*(tbl))`.
 * `tbl[i](x)` (Index → indexResultType) already worked (fnptr_array_inferred); THIS
 * is the DIRECT-deref call form the two decays unblock together.
 *
 * RED-ON-DISABLE (each change independently → the file does not compile):
 *   - revert the cst_to_hir Deref array-decay → `*(tbl)` is a TYPELESS Deref → H0001;
 *   - revert the semantic-tier Deref array-decay → the callee `(*(tbl))` types
 *     InvalidType → error[S0004] S_NotCallable.
 *
 * VALUE-CORRECT + RUNTIME-DRIVEN: `tbl[0]` is assigned at RUNTIME (through the opaque
 * `pick()` index), so `(*(tbl))` cannot be constant-folded to a fixed callee; a wrong
 * deref level or target would not yield 42. Pure c-subset → all 4 targets; pe RUNS on
 * Windows, elf/arm64 RUN on linux/qemu. */
int add40(int x) { return x + 40; }
int sub3 (int x) { return x - 3; }

int pick(int x) { return x; }   /* opaque runtime pass-through — defeats const-fold */

int main(void) {
    int (*tbl[2])(int) = { sub3, sub3 };
    tbl[pick(0)] = add40;          /* RUNTIME: tbl[0] = add40 (pick(0) == 0) */
    /* the DIRECT array-deref call: `(*(tbl))(2)` == `tbl[0](2)` == add40(2) == 42. */
    return (*(tbl))(2);
}
