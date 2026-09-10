// C99 6.7.6.2 + 6.7.7p2 (D-CSUBSET-VLA): an ARRAY whose ELEMENT comes from a typedef
// that is (or contains) a variable-length array — `typedef int R[n]; R a[m];` and its
// fixed-alias sibling `typedef int R[5]; R a[n];`. Both are shapes the VLA arc's C4b
// typedef gate EXCLUDED (its `declTy == headTy` test admitted only the suffix-less
// `R a;`), and both are compiled AND run by gcc 13.3.0 and clang 18.1.3.
//
// WHAT MAKES THE SHAPE HARD, AND WHAT THIS EXAMPLE CATCHES: the object's type has MORE
// array levels than its declarator has bounds. `R a[m]` writes ONE suffix but names a
// two-level type, and the missing level's size is not the object's to compute — C99
// 6.7.7p2 evaluated `n` ONCE, at the typedef, and froze it. So the size is a
// COMPOSITION: the object's own `[m]` multiplied by the size the alias froze. Get the
// seam wrong in either direction and the array is the wrong size or the rows are the
// wrong stride, which the off-diagonal writes below detect.
//
// (A) FREEZE-ONCE ACROSS THE COMPOSITION: `n` is mutated to 100 AFTER the typedef and
//     BEFORE the object. `sizeof a` must stay 2*3*4 == 24. A composition that re-lowered
//     `n` at the object would read 100 and reserve 800 — and, worse, stride the rows by
//     400, so this is the load-bearing one.
// (B) OFF-DIAGONAL: `a[1][0]` and `a[0][2]` are DISTINCT cells. A row stride taken from
//     the wrong level aliases or transposes them.
// (C) THE FIXED-ALIAS SIBLING: `typedef int R5[5]; R5 c[k];` == `int c[k][5]`. Here the
//     alias froze nothing (its size is a compile-time constant), so the seam is seeded
//     by a constant instead of a copied slot — a different arm of the same code.
// (D) `sizeof a[0]` pins the copied-down PER-LEVEL row slot, not just the total.
//
// main is a LEAF (no calls). It USED TO SAY this was the C1b VLA frame-model scope and
// that a VLA function which calls anything is separately refused
// (L_VlaNonLeafFrameUnsupported). ⚠ THAT REFUSAL IS GONE since 2026-09-04 (P59): the non-leaf VLA frame model shipped and `D-CSUBSET-VLA-NONLEAF-CALL-FRAME` is CLOSED. `main` here is STILL a leaf, but now by CHOICE rather than by force -- a leaf keeps this example's own subject independent of the frame model, which has its own witness in `examples/c/c99_vla_nonleaf_call_frame`. `volatile` defeats
// const-folding so the bounds are genuinely runtime. Each `return k` is a strict
// in-program pin; only all-pass reaches 42.
//
// Red-on-disable: revert the semantic origin gate to `declTy == headTy` and (A)/(B)/(D)
// stop compiling at all; remove the fixed-sub-spine seam in `computeVlaByteSize` and (C)
// stops compiling.

int main(void) {
    volatile int vn = 3;
    volatile int vm = 2;
    int n = vn;                  // n = 3 (runtime)
    typedef int R[n];            // R froze at n == 3
    n = 100;                     // must NOT resize anything declared from R
    int m = vm;                  // m = 2

    R a[m];                      // == int a[2][3], composed: own [m] over R's frozen [3]

    // (A) FREEZE-ONCE: 2 * 3 * 4 == 24, never 2 * 100 * 4.
    if (sizeof a != 24u) return 1;
    // (D) the copied-down per-level row slot: sizeof a[0] == 3 * 4.
    if (sizeof a[0] != 12u) return 2;

    // (B) OFF-DIAGONAL through the composed rows.
    a[1][0] = 77;
    a[0][2] = 88;
    if (a[1][0] != 77) return 3;
    if (a[0][2] != 88) return 4;
    // Neighbouring cells must be independent of those two writes.
    a[0][0] = 5;
    a[1][2] = 6;
    if (a[1][0] != 77) return 5;
    if (a[0][2] != 88) return 6;
    if (a[0][0] != 5) return 7;
    if (a[1][2] != 6) return 8;

    // (C) THE FIXED-ALIAS SIBLING: the alias contributes a compile-time constant.
    volatile int vk = 2;
    int k = vk;
    typedef int R5[5];
    R5 c[k];                     // == int c[2][5]
    if (sizeof c != 40u) return 9;
    if (sizeof c[0] != 20u) return 10;
    c[1][0] = 33;
    c[0][4] = 44;
    if (c[1][0] != 33) return 11;
    if (c[0][4] != 44) return 12;

    return 42;
}
