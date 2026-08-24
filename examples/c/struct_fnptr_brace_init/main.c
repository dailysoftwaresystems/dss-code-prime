/* c12 D-CSUBSET-STRUCT-FNPTR-BRACE-INIT runtime witness.
 *
 * Brace-initialization of a function-pointer struct member (the SQLite vtable
 * idiom `sqlite3_vfs x = { 3, ..., dbl, ... };`). A fn-ptr member already
 * resolved + member-assigned + called (cycle c10); only BRACE-INIT failed:
 * `struct Ops a = { dbl };` tripped `ConstructAggregate child[0] type N !=
 * field type M`. ROOT: a function-to-pointer DECAY gap — the field is
 * `Ptr<FnSig>` but the bare function name `dbl` stayed an undecayed `FnSig`
 * designator (the SAME FnSig the field's pointer wraps). FIX: CST->HIR's
 * `coerce` gained an `FnSig -> Ptr<FnSig>` decay arm (a synthetic Cast lowered
 * as a representation-free Bitcast over the function's GlobalAddr), gated on the
 * SAME same-signature predicate `isAssignable` uses — so a SIGNATURE MISMATCH
 * stays a loud verifier failure.
 *
 * Exercises brace-init of two distinct fn-ptr members + a second scalar field +
 * a CALL through each: `a.op(a.n)` = dbl(5) = 10, `b.op(b.n)` = inc(31) = 32 →
 * 10 + 32 = 42.
 *
 * RED-ON-DISABLE: remove the `FnSig -> Ptr<FnSig>` arm from `coerce` and the
 * brace-init fails LOUD again (H_VerifierFailure: ConstructAggregate child type
 * mismatch). The pe arm RUNS on the windows leg; elf/macho/arm64 on the
 * linux/darwin/qemu legs; the release arm witnesses the optimizer over the
 * indirect call. */
struct Ops { int (*op)(int); int n; };

int dbl(int x) { return x * 2; }
int inc(int x) { return x + 1; }

int main(void) {
    struct Ops a = { dbl, 5 };     /* brace-init: fn-ptr member + scalar */
    struct Ops b = { inc, 31 };
    return a.op(a.n) + b.op(b.n);  /* dbl(5) + inc(31) = 10 + 32 = 42 */
}
