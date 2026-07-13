// D-CSUBSET-VLA C3: MULTI-DIMENSIONAL variable-length arrays run with a RUNTIME inner
// stride. `int a[n][m]` steps a row index by the decl-frozen row byte size (m*sizeof
// (int)); every §B-included shape RUNS on all 3 legs. `volatile` seeds defeat constant
// folding so the dims are genuinely runtime. main is a LEAF (no calls) — the C1b VLA
// frame-model scope. Each `return k` (1..20) is a strict in-program pin; only all-pass
// reaches 42. The KEY correctness witness is the OFF-DIAGONAL write a[1][0] vs a[0][1]
// (distinct cells) — a level/order swap in the stride math would alias/transpose them.
int main(void) {
    volatile int vn = 3, vm = 5, vk = 2;   // runtime dims; volatile => no const fold
    int n = vn, m = vm, k = vk;

    // (1) a[n][m] ASYMMETRIC 3x5 + OFF-DIAGONAL: a[1][0] and a[0][1] are DISTINCT cells.
    int a[n][m];
    a[1][0] = 77;
    a[0][1] = 88;
    if (a[1][0] != 77) return 1;           // off-diagonal integrity (the CRITICAL-1 catch)
    if (a[0][1] != 88) return 2;
    if (sizeof a    != (unsigned long)n * m * sizeof(int)) return 3;   // whole = 60
    if (sizeof a[0] != (unsigned long)m * sizeof(int))     return 4;   // row   = 20

    // (2) 3-D a[n][m][k]: a runtime stride at TWO levels.
    int b[n][m][k];
    b[2][4][1] = 33;
    b[0][0][0] = 44;
    if (b[2][4][1] != 33) return 5;
    if (b[0][0][0] != 44) return 6;
    if (sizeof b       != (unsigned long)n * m * k * sizeof(int)) return 7;  // 120
    if (sizeof b[0]    != (unsigned long)m * k * sizeof(int))     return 8;  // 40
    if (sizeof b[0][0] != (unsigned long)k * sizeof(int))        return 9;   // 8

    // (3) FIXED-OUTER a[5][n]: top is a fixed Array whose element is a VLA (transitive
    // typeContainsVla routing); a[i] steps by a RUNTIME row stride too.
    int c[5][n];
    c[4][2] = 55;
    c[0][0] = 66;
    if (c[4][2] != 55) return 10;
    if (c[0][0] != 66) return 11;
    if (sizeof c    != (unsigned long)5 * n * sizeof(int)) return 12;  // 60
    if (sizeof c[0] != (unsigned long)n * sizeof(int))     return 13;  // 12

    // (4) TOP-VLA a[n][5] (fixed INNER): a[i] steps by a COMPILE-TIME stride (20).
    int d[n][5];
    d[2][4] = 99;
    d[0][0] = 10;
    if (d[2][4] != 99) return 14;
    if (d[0][0] != 10) return 15;
    if (sizeof d    != (unsigned long)n * 5 * sizeof(int)) return 16;  // 60
    if (sizeof d[0] != 5 * sizeof(int))                    return 17;  // 20 (compile-time)

    // (5) Evaluate-once (C 6.7.6.2p2): each dim size-expr runs EXACTLY once, in order.
    // (kk+=3)->3 then (kk+=5)->8, so e is e[3][8]; kk must be 8 after (double-eval => 16).
    int kk = 0;
    int e[(kk += 3)][(kk += 5)];
    e[2][0] = 12;
    if (e[2][0] != 12) return 18;
    if (kk != 8) return 19;                                            // evaluated once each
    if (sizeof e != (unsigned long)3 * 8 * sizeof(int)) return 20;     // 96

    return 42;
}
