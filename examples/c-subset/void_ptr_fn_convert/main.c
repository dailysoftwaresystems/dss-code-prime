// D-LANG-VOIDPTR-FN-CONVERT (C 6.3.2.3) end-to-end RUNTIME witness: a BARE
// function DESIGNATOR (a function name used with NO `&`) converted IMPLICITLY to
// a `void*` — the gcc/POSIX `dlsym` / Tcl `ClientData` idiom. sqlite's
// test_md5.c hits this 4x: `Tcl_CreateCommand(interp, "md5", MD5DigestToBase16,
// ...)` passes the bare function `MD5DigestToBase16` as Tcl_CreateCommand's
// `ClientData` argument (a `void*`), firing S0003 S_TypeMismatch without this
// conversion. Function-pointer <-> void* is UNDEFINED in ISO C (6.3.2.3 covers
// only object-pointer <-> void*) but POSIX-REQUIRED and representation-identical
// on every LP64/LLP64 target (same width, same bits) -> it can NEVER be a
// miscompile.
//
// Gated on `allowVoidPtrFnConvert` in c-subset.lang.json (c-subset opts in;
// default false = ISO-strict), the SINGLE authoritative gate for the whole
// fn<->void* class (Option B). RED-ON-DISABLE: flip that flag off (or revert the
// isAssignable FnSig->void* arm) and the BARE `register_and_call(add40, 2)`
// call-argument fails to compile (S0003) -> the example no longer BUILDS (a
// compile failure, not a wrong exit). The prior fnptr corpus stored only a TYPED
// fn-ptr (`&fn` / a fn-ptr variable) into a void*; the BARE designator (a function
// name, no `&`) into void* AND the IMPLICIT void*->fn-ptr retrieve are the
// coverage the suite lacked.
//
// VALUE-CORRECT: the function reaching the `void*` is retrieved, cast back to its
// fn-ptr type, and CALLED to drive the exit code — so a broken conversion (wrong
// address / a fail-loud / a copy) flips the exit or fails to build. Four legs
// exercise every admitted shape + direction: (1) a bare designator passed DIRECTLY
// as a `void*` CALL ARGUMENT — the exact test_md5 Tcl_CreateCommand shape; (2) a
// bare designator INITIALIZING a void*; (3) a TYPED fn-ptr (Ptr<FnSig>) stored into
// void* (the Option-B re-homing that routes Ptr<FnSig>->void* through the same
// single gate); (4) the IMPLICIT void*->fn-ptr RETRIEVE with NO cast (the reverse
// realize arm). Each round-trips and is called; all four yield 42. arm64 runs
// under qemu; macho on macos-latest.
typedef int (*unary_fn)(int);

int add40(int x)  { return x + 40; }
int times2(int x) { return x * 2; }

// Mirrors Tcl_CreateCommand's ClientData parameter: whatever reached the ERASED
// `void*` is cast back to its concrete fn-ptr type (void* -> Ptr<FnSig>, an
// explicit cast that already worked) and invoked as a genuine indirect call.
int register_and_call(void *clientData, int arg) {
    unary_fn f = (unary_fn) clientData;
    return f(arg);
}

int main(void) {
    // leg 1: BARE function designator passed DIRECTLY as a `void*` CALL ARGUMENT
    // — the exact `Tcl_CreateCommand(i, "md5", MD5DigestToBase16, ...)` blocker.
    int a = register_and_call(add40, 2);     /* add40(2) = 42 */
    if (a != 42) return 1;

    // leg 2: BARE function designator -> void* (INITIALIZER), then round-trip.
    void *slot = times2;
    int b = register_and_call(slot, 21);     /* times2(21) = 42 */
    if (b != 42) return 2;

    // leg 3: a TYPED fn-ptr (Ptr<FnSig>) -> void* (Option-B re-homing), assigned.
    unary_fn fp = add40;                     /* bare decay to fn-ptr */
    slot = fp;                               /* Ptr<FnSig> -> void* */
    int c = register_and_call(slot, 2);      /* add40(2) = 42 */
    if (c != 42) return 3;

    // leg 4: the IMPLICIT void* -> fn-ptr RETRIEVE (no cast) — direction 3's
    // implicit `coerce()` realize arm at runtime. Store a bare designator into a
    // `void*`, then assign that `void*` to a fn-ptr WITHOUT a cast (implicit,
    // gated on the same flag), and CALL it. Distinct from `register_and_call`'s
    // EXPLICIT `(unary_fn)` cast-back (the explicit-cast path, which always worked).
    void *vp = add40;                        /* bare designator -> void* */
    unary_fn g = vp;                         /* IMPLICIT void* -> Ptr<FnSig> */
    return g(2);                             /* add40(2) = 42 */
}
