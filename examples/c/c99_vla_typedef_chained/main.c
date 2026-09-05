// D-CSUBSET-VLA-TYPEDEF-CHAINED (C 6.7.7p3): a CHAINED VLA typedef — `typedef int R[n];
// typedef R S;` — where S's VLA-ness comes entirely from the head alias and S carries no
// `[n]` of its own to freeze. Objects of S must allocate and index with R's ALREADY-FROZEN
// size, and the chain must resolve at ANY depth (`typedef S T;` here goes three deep).
//
// C 6.7.7p3 evaluates a variably-modified typedef's size expression ONCE, when the typedef
// declaration is reached. R froze n = 3. S and T are aliases of R's already-frozen type and
// have nothing of their own to evaluate — so `n` moving to 100 between R and the objects
// must be invisible to every one of them. That is witness (A) below, and it is the check
// that would catch a "fix" that merely re-evaluated the bound at S's own declaration: such
// a fix reads 100 and sizes 400 bytes instead of 12.
//
// HOW IT RESOLVES, and why nothing below the semantic tier changed: `vlaTypedefOrigin` now
// names the typedef that OWNS a captured bound rather than merely the alias name a
// declaration spells, and the stamp is TRANSITIVE — S→R at S's own declaration, T→R at T's,
// and every object →R. So `T c;` reaches R's frozen slots through the SAME C4b copy-down a
// direct `R a;` has always used. An arbitrarily long chain costs one hop per link and no
// walk at all.
//
// ✔MEASURED 2026-09-03: gcc 13.3.0 and clang 18.1.3 both compile and RUN the chained form
// AND the three-deep form, at `-std=c17` and `-std=c2x` (`R=12 S=12` with n mutated to 99
// in between, exit 0). MSVC ABSTAINS — it implements no C99 VLA at all (`error C2057:
// expected constant expression` on the typedef itself) — so it casts no vote.
//
// ⚠ STILL DEFERRED, deliberately and loudly: an alias that carries its OWN suffix over a
// VLA head (`typedef R S[2];`). That one reaches `captureVlaSize` on its own `[2]` and the
// HIR arity check refuses it by name ("2 array level(s) ... 1 bound(s) captured") — a
// DISTINCT shape with an intact fail-loud, not a silent wrong size, and admitting it here
// would have traded that refusal for a guess.
//
// ★ LIFTED in P59: main used to be kept a LEAF (no calls) because a VLA-object holder that
// CALLS was refused (D-CSUBSET-VLA-NONLEAF-CALL-FRAME). It now CALLS `sink` with every
// chained-alias object still live and re-reads all of them afterwards.
// `volatile` defeats constant folding so the bound is genuinely runtime. Two witnesses
// carry the weight: (A) FREEZE-ONCE through the chain, and (B) the multi-dim OFF-DIAGONAL,
// where c[1][0] and c[0][1] are DISTINCT cells so a wrong runtime row stride would alias or
// transpose them. Each `return k` is a strict in-program pin; only all-pass reaches 42.

// A wide call target, so main is a NON-LEAF function that also holds VLA objects.
int sink(int a, int b, int c, int d, int e, int f,
         int g, int h, int i, int j) { return a+b+c+d+e+f+g+h+i+j; }

int main(void) {
    volatile int vn = 3;
    int n = vn;                    // runtime 3; volatile => no const fold

    // (A) FREEZE-ONCE THROUGH THE CHAIN. R freezes n = 3; S and T inherit that ONE frozen
    // size. `n` is then mutated to 100, which must reach none of them.
    typedef int R[n];
    typedef R S;                   // the CHAINED alias — this row's subject
    typedef S T;                   // ... and a THIRD link, to prove depth resolves
    n = 100;                       // mutate the source variable AFTER the typedefs

    R a;
    S b;
    T c;
    if (sizeof a != 12) return 1;  // the ORIGINAL alias (the C4b control)
    if (sizeof b != 12) return 2;  // THE chained catch: a leak re-evaluates n -> 400
    if (sizeof c != 12) return 3;  // ... and at depth three
    if (sizeof b != sizeof a) return 4;
    if (sizeof c != sizeof a) return 5;

    a[0] = 10; a[1] = 11; a[2] = 12;
    b[0] = 20; b[1] = 21; b[2] = 22;
    c[0] = 30; c[1] = 31; c[2] = 32;
    if (a[0] + a[1] + a[2] != 33) return 6;
    if (b[0] + b[1] + b[2] != 63) return 7;   // indexing b uses R's frozen stride
    if (c[0] + c[1] + c[2] != 93) return 8;
    // The three objects are DISTINCT storage — a chain that collapsed two aliases onto one
    // slot would show up as a write to c landing in b.
    if (b[0] != 20) return 9;
    if (a[2] != 12) return 10;

    // (B) MULTI-DIM OFF-DIAGONAL through a chained alias. `n` is now 100, so a re-evaluated
    // bound would also blow the frame here rather than merely mis-size it.
    volatile int vp = 2, vq = 3;
    int p = vp, q = vq;
    typedef int M[p][q];           // 2 x 3, frozen at the typedef
    typedef M N;                   // the chained alias of a MULTI-DIM VLA typedef
    N d;
    if (sizeof d != 2 * 3 * (int)sizeof(int)) return 11;
    d[1][0] = 55;
    d[0][1] = 66;                  // DISTINCT cell from [1][0] — the stride witness
    if (d[1][0] != 55) return 12;
    if (d[0][1] != 66) return 13;
    d[1][2] = 77;                  // the far corner
    if (d[1][2] != 77) return 14;
    if (d[1][0] != 55) return 15;  // ... and the far-corner write did not clobber it

    // ★ THE LIFT: a wide CALL with every chained-alias object still live, then all of
    // them re-read. Ten arguments so the call genuinely writes stack arguments into the
    // outgoing-args area that travels with SP under the non-leaf VLA frame model.
    if (sink(1, 2, 3, 4, 5, 6, 7, 8, 9, 10) != 55) return 16;
    if (a[0] + a[1] + a[2] != 33) return 17;
    if (b[0] + b[1] + b[2] != 63) return 18;
    if (c[0] + c[1] + c[2] != 93) return 19;
    if (d[1][0] != 55 || d[0][1] != 66 || d[1][2] != 77) return 20;

    return 42;
}
