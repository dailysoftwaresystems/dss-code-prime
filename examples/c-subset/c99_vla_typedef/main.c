// D-CSUBSET-VLA C4b: a VLA TYPEDEF (`typedef int R[n]; R a;`) RUNS on all 3 legs.
// C99 §6.7.7p2: the size expression `n` is evaluated ONCE, when the typedef
// declaration is reached, and FROZEN — every later `R a;` allocates with that frozen
// size (even if `n` changes between two uses of R). `volatile` seeds defeat constant
// folding so the dims are genuinely runtime. main is a LEAF (no calls) — the C1b VLA
// frame-model scope. Each `return k` (1..12) is a strict in-program pin; only all-pass
// reaches 42. Two witnesses carry the correctness weight: (A) FREEZE-ONCE — after
// mutating `n`, a SECOND `R b;` must still be int[3], NOT int[100] (proven by the
// runtime `sizeof a == sizeof b == 12`; if the freeze leaked, b would re-evaluate n=100
// and size 400); (B) MULTI-DIM OFF-DIAGONAL — `c[1][0]` vs `c[0][1]` are DISTINCT cells,
// so a wrong runtime row stride would alias/transpose them.
int main(void) {
    volatile int vn = 3;
    int n = vn;                 // runtime 3; volatile => no const fold

    // (A) FREEZE-ONCE. `R` freezes n=3 at the typedef; both `a` and `b` share that ONE
    // frozen size even though `n` is mutated to 100 between the two objects.
    typedef int R[n];
    R a;                        // int[3] (frozen)
    n = 100;                    // mutate the source variable AFTER the typedef
    R b;                        // MUST still be int[3] — the freeze does not re-evaluate n
    if (sizeof a != 12) return 1;          // runtime Load of a's frozen whole-object slot
    if (sizeof b != 12) return 2;          // THE freeze-once catch (leak => 400, not 12)
    a[0] = 10; a[1] = 11; a[2] = 12;       // index the frozen object a
    b[0] = 20; b[1] = 21; b[2] = 22;       // index the frozen object b (shares R's freeze)
    if (a[0] + a[1] + a[2] != 33) return 3;
    if (b[0] + b[1] + b[2] != 63) return 4;

    // (B) MULTI-DIM runtime-inner typedef `int[p][q]` (asymmetric 2x3). The OFF-DIAGONAL
    // write c[1][0] vs c[0][1] is the single witness that catches a level/order swap in
    // the copied-down runtime row stride.
    volatile int vp = 2, vq = 3;
    int p = vp, q = vq;
    typedef int R2[p][q];
    R2 c;
    c[1][0] = 77;
    c[0][1] = 88;
    if (c[1][0] != 77) return 5;           // off-diagonal integrity (transpose catch)
    if (c[0][1] != 88) return 6;
    if (sizeof c    != 24) return 7;        // whole = p*q*4 = 24 (runtime Load)
    if (sizeof c[0] != 12) return 8;        // row   = q*4   = 12 (copied-down row slot)

    // (C) FIXED-OUTER typedef `int[5][p]` (top is a fixed Array whose element is a VLA —
    // transitive typeContainsVla routing); d[i] steps by a RUNTIME row stride too.
    typedef int R3[5][p];
    R3 d;
    d[1][0] = 55;
    d[0][1] = 66;
    if (d[1][0] != 55) return 9;            // off-diagonal integrity
    if (d[0][1] != 66) return 10;
    if (sizeof d    != 40) return 11;       // whole = 5*p*4 = 40
    if (sizeof d[0] != 8)  return 12;       // row   = p*4   = 8

    // (D) MULTI-DECLARATOR one statement (`R4 e, f;` — two frozen objects share R4's ONE
    // freeze, each correlated a→R4 per-declarator) + a CONST-qualified VLA-typedef object
    // (`const R4 g;` — allocated + frozen-sized like any VLA object; a const VLA carries no
    // initializer, only reserved storage).
    volatile int vk = 3;
    int k = vk;
    typedef int R4[k];
    R4 e, f;
    const R4 g;
    e[0] = 13; e[1] = 14; e[2] = 15;
    f[0] = 1;  f[1] = 2;  f[2] = 3;
    if (sizeof e != 12) return 13;
    if (sizeof f != 12) return 14;
    if (sizeof g != 12) return 15;          // const VLA-typedef object is still frozen-sized
    if (e[0] + e[1] + e[2] != 42) return 16;
    if (f[0] + f[1] + f[2] != 6)  return 17;

    return 42;
}
