// D-CSUBSET-VLA-NONLEAF-CALL-FRAME: a function that holds a VLA OBJECT and ALSO
// MAKES CALLS — the shape the C1b frame model refused outright
// (`L_VlaNonLeafFrameUnsupported`, "C1b supports a LEAF VLA only").
//
// ★ WHAT THIS EXAMPLE MEASURES, and why each arm is shaped the way it is.
//
// The frame model places the outgoing-args area at [SP+0 .. SP+outgoingArgAreaSize)
// where it TRAVELS WITH SP, and lifts every dynamic object above it. The failure it
// prevents is silent: the caller's own stack-argument stores landing INSIDE the live
// VLA. So every call below is deliberately WIDE enough to push arguments onto the
// stack on all four shipped calling conventions — 12 integer arguments overflows
// ms_x64's 4 argument GPRs (8 stacked, above a 32-byte shadow), sysv_amd64's 6 (6
// stacked) and aapcs64's 8 (4 stacked) — and the VLA is 32 ints = 128 bytes, wide
// enough to cover every one of those outgoing areas.
//
// ⚠ AND THE VLA IS READ BACK **AFTER** THE CALL, WHICH IS THE WHOLE POINT. An
// earlier draft passed the array's elements as the arguments and returned the sum;
// that program reads every element BEFORE the stores that would corrupt them, so it
// prints the right answer whether or not the frame model is there. A witness whose
// failure mode you cannot state is decoration.
//
// The `loop_scope` arm exercises the OTHER half: a block-scope VLA inside a loop
// emits a StackSave/StackRestore watermark pair, and under this frame model a
// watermark means `SP + outgoingArgAreaSize`, so the restore has to undo that bias.
// Get it wrong and the NEXT iteration's outgoing-argument stores walk up into the
// saved-register/spill area of the enclosing frame.
//
// `volatile` defeats const-folding so every bound is genuinely runtime; each
// `return k` is a strict in-program pin and only an all-pass path reaches 42.

int consume12(int a, int b, int c, int d, int e, int f,
              int g, int h, int i, int j, int k, int l) {
    return a + b + c + d + e + f + g + h + i + j + k + l;
}

// Writes through a pointer INTO the caller's VLA, so the object's address has to be
// right in both directions (the callee stores where the caller later reads).
void poke(int *p, int idx, int value) { p[idx] = value; }

// ── Arm 1: a VLA object, a wide call, then the object re-read. ───────────────
static int vla_survives_a_wide_call(int n) {
    int a[n];
    int i;
    for (i = 0; i < n; i = i + 1) a[i] = i * 3 + 1;

    // A wide call: stack arguments are written at [SP + shadow ..). Without the
    // frame model those stores land on a[0..] and the loop below fails.
    int r = consume12(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
    if (r != 78) return 1;

    for (i = 0; i < n; i = i + 1) {
        if (a[i] != i * 3 + 1) return 2;     // the silent-corruption catch
    }

    // A second wide call, then a callee that writes INTO the VLA.
    r = consume12(2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2);
    if (r != 24) return 3;
    poke(a, n - 1, 999);
    if (a[n - 1] != 999) return 4;
    if (a[0] != 1) return 5;
    for (i = 0; i < n - 1; i = i + 1) {
        if (a[i] != i * 3 + 1) return 6;
    }
    return 0;
}

// ── Arm 2: a block-scope VLA in a loop — StackSave / StackRestore. ───────────
static int loop_scope(int base) {
    int k;
    int total = 0;
    for (k = 0; k < 3; k = k + 1) {
        int m = base + k;
        int b[m];                    // re-allocated each iteration; scope exit restores
        int j;
        for (j = 0; j < m; j = j + 1) b[j] = j + k;
        int r = consume12(k, k, k, k, k, k, k, k, k, k, k, k);
        if (r != 12 * k) return 10 + k;
        for (j = 0; j < m; j = j + 1) {
            if (b[j] != j + k) return 20 + k;   // survived the call in the same scope
        }
        total = total + b[m - 1];
    }
    // b[m-1] == (m-1)+k == base+2k-1, over k=0,1,2: (base-1)+(base+1)+(base+3)
    if (total != 3 * base + 3) return 30;
    return 0;
}

// ── Arm 3: an OVER-ALIGNED element under an ODD stacked-argument count. ──────
//
// The bias makes the captured base `SP + outgoingArgAreaSize`, and with FIVE stacked
// arguments on ms_x64 that is 32 + 5*8 = 72 — congruent to 8 mod 16, not 0. So the
// element-alignment round-up must rest on NO premise about the base's residue. (It
// does not: the VLA lowering over-allocates a full `elemAlign` of headroom for
// exactly this reason, and this arm is the case that would catch a bias that
// silently broke that.)
struct Over { _Alignas(32) int v; int pad[7]; };

int sink9(int a, int b, int c, int d, int e, int f, int g, int h, int i) {
    return a + b + c + d + e + f + g + h + i;
}

static int overaligned_element(int n) {
    struct Over a[n];
    int i;
    for (i = 0; i < n; i = i + 1) { a[i].v = i + 1; a[i].pad[0] = 100 + i; }
    if (((unsigned long long)(void *)&a[0] & 31u) != 0) return 50;
    if (((unsigned long long)(void *)&a[n - 1] & 31u) != 0) return 51;
    if (sink9(1, 2, 3, 4, 5, 6, 7, 8, 9) != 45) return 52;
    for (i = 0; i < n; i = i + 1) {
        if (a[i].v != i + 1) return 53;
        if (a[i].pad[0] != 100 + i) return 54;
    }
    if (((unsigned long long)(void *)&a[0] & 31u) != 0) return 55;
    return 0;
}

int main(void) {
    volatile int vn = 32;
    int n = vn;
    int rc = vla_survives_a_wide_call(n);
    if (rc != 0) return rc;

    volatile int vo = 5;
    rc = overaligned_element(vo);
    if (rc != 0) return rc;

    volatile int vb = 5;
    rc = loop_scope(vb);
    if (rc != 0) return rc;

    // A VLA object AND a call in the SAME function as `main` itself — main is the
    // function the C1b gate refused most visibly.
    int c[n];
    int i;
    for (i = 0; i < n; i = i + 1) c[i] = n - i;
    if (consume12(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1) != 12) return 40;
    for (i = 0; i < n; i = i + 1) {
        if (c[i] != n - i) return 41;
    }
    return 42;
}
