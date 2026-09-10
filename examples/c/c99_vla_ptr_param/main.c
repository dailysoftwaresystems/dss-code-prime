// D-CSUBSET-VLA C4a-param: a PARAMETER pointer-to-VLA `int (*p)[n]` (and the adjusted
// `int a[][n]`) RUNS on all 3 legs — the callee body's subscript p[i][j] scales by the
// RUNTIME pointee row size (n*sizeof(int)), frozen ONCE at the callee's prologue from the
// incoming `n` param (the C3/C4a vlaStrideSlot + scaleIndexToBytes substrate, populated
// for a ptr-to-VLA PARAM here). A ptr-to-VLA param emits only a FIXED 8-byte stride slot
// (NOT a dynamic-stack VLA object), which is why g and h could always CALL `sink` freely
// — they hold no dynamic-stack object at all.
//
// STORAGE NOTE — ★ LIFTED, and the lift is the point. This example used to hold a FIXED
// buffer `int buf[6]` CAST to a ptr-to-VLA row, because a caller that holds a VLA OBJECT
// *and* makes a call was a NON-leaf VLA function the C1b frame model refused
// (D-CSUBSET-VLA-NONLEAF-CALL-FRAME). The cast was a shape bent around a live refusal.
// That frame model is now built — the outgoing-args area travels with SP and the dynamic
// object is placed above it — so `main` holds a GENUINE 2 x n VLA OBJECT and passes it by
// ordinary array-to-pointer decay, which is what a reader of this example would write.
// n is genuinely runtime (`volatile` defeats const-fold).
//
// The KEY correctness witness is the OFF-DIAGONAL p[1][0] vs p[0][2] (DISTINCT cells: byte
// 1*n*4+0 vs 0+2*4) — a wrong row stride would alias/transpose them. Each `return k` is a
// strict in-program pin; only all-pass -> 42.

int sink(int v) { return v; }

// PARAMETER pointer-to-VLA `int (*p)[n]`.
int g(int n, int (*p)[n]) {
    p[1][0] = 77;                      // row 1, col 0 -> byte 1*n*4 + 0
    p[0][2] = 88;                      // row 0, col 2 -> byte 0     + 2*4  (DISTINCT)
    if (p[1][0] != 77) return 1;       // off-diagonal integrity (the runtime-stride catch)
    if (p[0][2] != 88) return 2;
    p[1][2] = sink(99);                // far corner written via a CALL (g is NOT leaf)
    if (p[1][2] != 99) return 3;
    return 42;
}

// The ADJUSTED form `int a[][n]`: the outer `[]` decays to the pointer, the inner `[n]`
// is the runtime pointee row — C-equivalent to `int (*a)[n]`.
int h(int n, int a[][n]) {
    a[1][0] = 55;
    a[0][1] = 66;
    if (a[1][0] != 55) return 4;       // off-diagonal (a[1][0] must NOT alias a[0][1])
    if (a[0][1] != 66) return 5;
    return sink(42);                   // a CALL (h is NOT leaf either)
}

int main(void) {
    volatile int vn = 3;               // n = 3 (runtime; volatile => no const fold)
    int n = vn;
    int buf[2][n];                     // a GENUINE 2 x n VLA OBJECT in a function that CALLS
    int i, j;
    for (i = 0; i < 2; i = i + 1)
        for (j = 0; j < n; j = j + 1) buf[i][j] = 0;

    int (*p)[n];                       // pointer to a VLA row (int[n]); assignment form
    p = buf;                           // ordinary array-to-pointer decay of a VLA object

    int r = g(n, p);                   // a REAL call; g writes off-diagonal through p
    if (r != 42) return r;
    // main-side aliasing: g wrote through p, which IS buf — verify the cells, which also
    // proves buf survived the call (the non-leaf VLA frame model's whole subject).
    if (buf[1][0] != 77) return 10;    // p[1][0]
    if (buf[0][2] != 88) return 11;    // p[0][2]
    if (buf[1][2] != 99) return 12;    // p[1][2]

    for (i = 0; i < 2; i = i + 1)
        for (j = 0; j < n; j = j + 1) buf[i][j] = 0;
    r = h(n, p);                       // pass the SAME ptr as `int a[][n]`
    if (r != 42) return r;
    if (buf[1][0] != 55) return 13;    // a[1][0]
    if (buf[0][1] != 66) return 14;    // a[0][1]

    return 42;
}
