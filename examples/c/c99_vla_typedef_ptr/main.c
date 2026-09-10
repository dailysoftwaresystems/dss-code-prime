// C99 6.7.6.2 + 6.7.7p2 (D-CSUBSET-VLA): a POINTER whose pointee comes from a typedef
// carrying a runtime bound — `typedef int R[n]; R *p;` (the alias is the POINTEE) and
// `typedef int (*P)[n]; P q;` (the alias IS the pointer). Both were EXCLUDED by the C4b
// typedef gate; both are compiled AND run by gcc 13.3.0 and clang 18.1.3.
//
// WHAT THE SHAPE ACTUALLY NEEDS: a pointer to a VLA row does not reserve storage — it
// reserves a fixed 8 bytes — but its SUBSCRIPT steps by the pointee's RUNTIME row size,
// so that one number has to come from somewhere. For `int (*p)[n]` it is computed at the
// pointer's declaration. Here it must instead be INHERITED from the size the typedef
// froze once at its own declaration (6.7.7p2). Re-deriving it from `n` at the pointer
// would be the freeze-once miscompile, and it is silent: the program still runs, just
// through the wrong rows.
//
// (A) FREEZE-ONCE THROUGH THE POINTER: `n` is mutated to 100 AFTER the typedef and
//     BEFORE the pointer. `p[i]` must still step by 3*4 == 12, which the cross-checks
//     against the array `a` detect — a stride of 400 reads far outside `a`.
// (B) OFF-DIAGONAL: `p[1][0]` and `p[0][2]` are DISTINCT cells, and both must alias the
//     same storage as `a`'s corresponding cells (write through one, read through the
//     other, in both directions).
// (C) THE POINTER-TYPEDEF ARM: `typedef int (*P)[n]; P q = a;` — the alias itself is the
//     pointer, so the frozen quantity is its pointee row stride rather than an object
//     size. Same subscript arithmetic, different declaration shape.
// (D) THE INIT FORM: both pointers are initialized at their declarations (`= a`), not
//     assigned afterwards — the spelling D-CSUBSET-VLA-PTR-INIT-FORM-TYPING once
//     deferred and which must keep working through the typedef composition too.
//
// main is a LEAF (no calls). ⚠ "The C1b VLA frame-model scope" was leaf-ONLY and that
// boundary is GONE since 2026-09-04 (P59): the non-leaf VLA frame model shipped and
// D-CSUBSET-VLA-NONLEAF-CALL-FRAME is CLOSED. Still a leaf, now by CHOICE, so this
// example's subject stays independent of that model. `volatile` defeats
// const-folding so the bound is genuinely runtime. Each `return k` is a strict
// in-program pin; only all-pass reaches 42.
//
// Red-on-disable: revert the semantic origin gate to `declTy == headTy` and (A)/(B)/(D)
// stop compiling; drop the Ptr arm from the MIR TypeDecl freeze and (C) stops compiling.

int main(void) {
    volatile int vn = 3;
    int n = vn;                  // n = 3 (runtime)
    typedef int R[n];            // R froze at n == 3
    typedef int (*P)[n];         // P's pointee row froze at n == 3
    n = 100;                     // must NOT restride anything declared from R or P

    R a[2];                      // == int a[2][3]

    R *p = a;                    // (D) INIT form; pointee row = R's FROZEN int[3]
    P q = a;                     // (C) the alias IS the pointer; same frozen row

    // (B) OFF-DIAGONAL written through the pointer, read through the array.
    p[1][0] = 77;
    p[0][2] = 88;
    if (a[1][0] != 77) return 1;
    if (a[0][2] != 88) return 2;
    // (A) a 400-byte stride would have put p[1][0] far outside `a` — these cells must
    // be untouched.
    a[0][0] = 5;
    a[1][2] = 6;
    if (p[1][0] != 77) return 3;
    if (p[0][2] != 88) return 4;
    if (p[0][0] != 5) return 5;
    if (p[1][2] != 6) return 6;

    // Written through the array, read through the pointer (the other direction).
    a[1][1] = 21;
    if (p[1][1] != 21) return 7;

    // (C) the pointer-typedef arm must agree cell for cell with both of the above.
    if (q[1][0] != 77) return 8;
    if (q[0][2] != 88) return 9;
    q[0][1] = 13;
    if (a[0][1] != 13) return 10;
    if (p[0][1] != 13) return 11;

    return 42;
}
