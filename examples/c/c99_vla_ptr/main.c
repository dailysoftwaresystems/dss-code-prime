// D-CSUBSET-VLA C4a-local: a LOCAL pointer-to-VLA `int (*p)[n]` RUNS — the subscript p[i]
// steps by the RUNTIME pointee row size (n*sizeof(int)), frozen ONCE at p's decl (the
// C3 vlaStrideSlot/scaleIndexToBytes substrate, now populated for pointers). The witness
// is the ASSIGNMENT form `int (*p)[n]; p = b;` (the natural `= b` init form is deferred,
// D-CSUBSET-VLA-PTR-INIT-FORM-TYPING). `volatile` defeats const-fold so n is genuinely
// runtime; main is a LEAF (the C1b VLA frame-model scope). The KEY correctness witness is
// the OFF-DIAGONAL write p[1][0] vs p[0][2] (DISTINCT cells) — a wrong row stride would
// alias/transpose them. Each `return k` is a strict in-program pin; only all-pass -> 42.
int main(void) {
    volatile int vn = 4;
    int n = vn;              // n = 4 (runtime; volatile => no const fold)
    int b[3][n];             // a real 3x4 VLA object (rows are int[n])

    int (*p)[n];             // pointer to a VLA row (int[n])
    p = b;                   // ASSIGNMENT form (the C4a-local witness; b decays to &b[0])

    // (1) OFF-DIAGONAL via p: p[1][0] and p[0][2] are DISTINCT cells.
    p[1][0] = 77;
    p[0][2] = 88;
    if (p[1][0] != 77) return 1;   // off-diagonal integrity (the runtime-stride catch)
    if (p[0][2] != 88) return 2;

    // (2) p and b ALIAS the same storage (p's row stride == b's runtime row stride).
    if (b[1][0] != 77) return 3;
    if (b[0][2] != 88) return 4;

    // (3) cross-write at the far corner: through b, read via p; through p, read via b.
    b[2][3] = 99;
    if (p[2][3] != 99) return 5;
    p[2][0] = 55;
    if (b[2][0] != 55) return 6;

    return 42;
}
