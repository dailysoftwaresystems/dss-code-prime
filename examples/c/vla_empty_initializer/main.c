/* D-CSUBSET-VLA-INITIALIZER (C23 6.7.10p4: "An entity of variable length array
 * type shall not be initialized except by an empty initializer") -- the
 * REQUIRED-ACCEPT half, proven END-TO-END: the object is accepted, it is really
 * ZEROED at run time, and it is still ordinary writable storage afterwards.
 *
 * WHY A RUNNABLE EXAMPLE AND NOT ONLY A UNIT PIN. The semantic tier's job here is
 * one bit -- accept or refuse -- and a unit test can see that bit. It CANNOT see
 * the obligation that comes with the accept: C23 says the object IS initialized,
 * and gcc 13.3.0 and clang 19.1.1 both really write zeros (MEASURED 2026-08-25, a
 * 16-element VLA read back after the frame was dirtied sums to 0 under each). A
 * DSS that accepted the form and skipped the fill would pass every semantic test
 * and hand back a garbage read. Only spawning the binary catches that.
 *
 * ALL THREE CHUNK ARMS OF THE FILL ARE EXERCISED, DELIBERATELY, AND SO IS THE
 * TAIL. The lowering picks its store width from the OBJECT'S OWN ALIGNMENT,
 * because a blanket 8-byte store into a 1-aligned `char a[n]` slot would be an
 * unaligned access: `double` covers the 8-byte arm, `int` the 4-byte arm, `char`
 * the pure-byte arm.
 *
 * The TAIL loop needs its own case, and working out WHICH case is the point. For
 * an ordinary VLA the byte total is `count * elemStride` and `elemStride` is a
 * multiple of the element's alignment, so the total is ALWAYS a multiple of the
 * chunk and the tail runs zero times -- the three probes above cannot reach it.
 * What DOES reach it is an OVER-ALIGNED object, where the chunk comes from
 * `alignas` rather than from the element: `_Alignas(16) int a[3]` is 12 bytes with
 * an 8-byte chunk, so the wide loop writes bytes 0..7 and the TAIL must write
 * 8..11 -- exactly `a[2]`. Drop the tail loop and `a[2]` keeps the dirty pattern
 * while `a[0]`/`a[1]` are clean, which is the sneakiest shape this fill can fail
 * in and the reason it is pinned separately.
 *
 * WHAT BREAKS IT:
 *  - the MIR VarDecl arm's VLA interception removed => the empty
 *    ConstructAggregate falls into the ordinary array arm, iterates its ZERO
 *    children, emits nothing, and every probe reads the 0x5A5A5A5A the frame was
 *    deliberately filled with => not 42;
 *  - the tail loop dropped => `a[2]` of the over-aligned object stays dirty
 *    while a[0]/a[1] are clean => t4 flips (and NOTHING else does, which is
 *    exactly why it needs its own probe);
 *  - the chunk width taken as a constant 8 => an unaligned store on the char and
 *    int objects (and a fill that runs PAST the end of a small one);
 *  - the runtime byte count read from the wrong side-table slot => a partial fill;
 *  - the semantic tier going back to refusing `= {}` => no binary at all;
 *  - the fill running past the object => `guard` stops being 5 => t7 flips.
 *
 * `dirty()` runs FIRST and writes a recognisable non-zero pattern deep enough to
 * cover the frames the probes below allocate, so "already zero" cannot pass by
 * luck. Every count is volatile-seeded, so no const-fold / mem2reg / CSE in the
 * release arm can pre-evaluate a sum and mask a missing fill. */

static int dirty(void) {
    volatile int b[160];
    for (int i = 0; i < 160; i++) b[i] = 0x5A5A5A5A;
    return (int)b[0];
}

/* 4-aligned: 13 ints == 52 bytes == 13 four-byte chunks, no tail. */
static int sumIntVla(int n) {
    int a[n] = {};
    int s = 0;
    for (int i = 0; i < n; i++) s += a[i];
    return s;
}

/* 1-aligned: the pure-byte arm, 13 bytes. */
static int sumCharVla(int n) {
    char a[n] = {};
    int s = 0;
    for (int i = 0; i < n; i++) s += (int)a[i];
    return s;
}

/* 8-aligned: the widest arm, no tail. */
static int sumDoubleVla(int n) {
    double a[n] = {};
    double s = 0;
    for (int i = 0; i < n; i++) s += a[i];
    return (int)s;
}

/* OVER-ALIGNED: the chunk comes from `alignas`, not from the element, so 12 bytes
 * are filled by ONE 8-byte chunk plus a 4-byte TAIL. The only probe here that
 * reaches the tail loop at all. */
static int sumOverAlignedVla(int n) {
    _Alignas(16) int a[n] = {};
    int s = 0;
    for (int i = 0; i < n; i++) s += a[i];
    return s;
}

/* Multi-dimensional: the WHOLE object, not merely its first row. */
static int sumVla2(int n) {
    int a[n][3] = {};
    int s = 0;
    for (int i = 0; i < n; i++) s += a[i][0] + a[i][1] + a[i][2];
    return s;
}

/* Zeroed AND still ordinary storage: write through it and read it back. */
static int writeThrough(int n) {
    int a[n] = {};
    a[n - 1] = 6;
    a[0] = 7;
    int s = 0;
    for (int i = 0; i < n; i++) s += a[i];
    return s;   /* 7 + 6 + zeros == 13 */
}

int main(void) {
    (void)dirty();
    volatile int v13 = 13, v9 = 9, v5 = 5, v3 = 3;
    int guard = (int)v5;

    int t1 = (sumIntVla((int)v13)         == 0)  ? 6 : 0;
    int t2 = (sumCharVla((int)v13)        == 0)  ? 6 : 0;
    int t3 = (sumDoubleVla((int)v5)       == 0)  ? 6 : 0;
    int t4 = (sumOverAlignedVla((int)v3)  == 0)  ? 6 : 0;   /* the TAIL loop */
    int t5 = (sumVla2((int)v9)            == 0)  ? 6 : 0;
    int t6 = (writeThrough((int)v13)      == 13) ? 6 : 0;
    int t7 = (guard == 5) ? 6 : 0;

    /* 6*7 == 42, and only if every arm of the fill stayed inside its object. */
    return t1 + t2 + t3 + t4 + t5 + t6 + t7;
}
