/* D-CSUBSET-COMPLEX-STATIC-STORAGE-INITIALIZER-HAS-NO-CONSTANT-IMAGE — the RUNTIME
 * witness that a `_Complex` object with STATIC storage duration carries a real
 * constant IMAGE, and that the image is the value the emitted code would have
 * computed. RUN on both the baseline (debug) and the `release` arm.
 *
 * ✔MEASURED at 301e2a63 every declaration below refused with
 *   error[K_NoMatchingObjectFormat] … has a runtime initializer (__module_init__-driven)
 * — a complex initializer had no constant-image path at all, so it fell to a
 * load-time store-chain the producer does not emit. gcc 13.3.0 (`-std=c2x`) and
 * clang 18.1.3 (`-std=c23`), probed SEPARATELY, compile and run all of them.
 *
 * ★★ EVERY ARM IS BUILT SO THAT WRITING ONLY ONE COMPONENT CHANGES THE EXIT CODE.
 * That is the whole design: a half-emitted image — real right, imaginary zero — is a
 * SILENT MISCOMPILE and not a refusal, because nothing recomputes the value at load.
 * So `g_bc` has two DISTINCT non-zero components (swapping them shows), `g_imag` has
 * a ZERO real and a non-zero imaginary (a producer that writes only `re` yields
 * (0,0) and fails), and `g_real` has a zero IMAGINARY that must be zero (a producer
 * that writes `re` twice yields (7,7) and fails).
 *
 * ★★★ ARMS 5, 6 AND 8 COMPARE THE STATIC IMAGE AGAINST THE SAME EXPRESSION EVALUATED
 * AT RUNTIME. That is the property the const-fold actually claims — the folded bytes
 * are the bytes the emitted FAdd/FMul/FDiv chain would have stored — and it is the
 * one a hand-written expected constant cannot check, because a fold and a hand-typed
 * literal can agree with each other while both disagree with the machine. Arm 5 is
 * the MIXED-PRECISION arm and is the sharpest of the three -- see its declaration
 * below for the three-way measurement that shows why a reference's bytes are not
 * even a well-defined target here.
 *
 * ARM 7 reads the object's BYTES through `unsigned char *` — the C 6.2.6.1 object
 * representation — and compares them against a complex built at runtime. It is the
 * only arm that cannot be satisfied by value-level constant propagation, so it is
 * what pins the IMAGE rather than the literal the image was made from.
 *
 * ANTI-FOLD: every runtime-side value is derived from `volatile` seeds, so the
 * release arm compares a genuinely computed value against the static image instead of
 * folding both sides to one constant.
 *
 * exit = 1+2+4+8+16+32+64+128 = 255 iff every arm passes; each failing arm subtracts
 * its own distinct power of two, so the exit code NAMES which arm broke.
 */

#include <complex.h>

/* ── the static images under test ─────────────────────────────────────────── */
double _Complex g_bc   = __builtin_complex(3.0, 4.0);  /* both components, distinct */
double _Complex g_real = 7.0;                          /* real -> complex: (7, 0)   */
double _Complex g_int  = 5;                            /* int  -> complex: (5, 0)   */
double _Complex g_imag = I;                            /* (0, 1): ZERO real         */
double _Complex g_neg  = -__builtin_complex(3.0, 4.0); /* (-3, -4)                  */
double _Complex g_conj = conj(__builtin_complex(3.0, 4.0)); /* (3, -4); gcc folds it */
double _Complex g_sum  = 3.0 + 4.0 * I;                /* complex arithmetic fold   */
double _Complex g_div  = (1.0 + 2.0 * I) / (3.0 + 4.0 * I);
float  _Complex g_f32  = (0.1f + 0.2f * I) * (0.3f + 0.7f * I);
                       /* ^ the MIXED-PRECISION arm. `I` is complex<double>, so the
                        *   product is computed at F64 and narrowed to F32 exactly
                        *   ONCE; rounding every intermediate to F32 instead moves the
                        *   imaginary component by 1 ULP.
                        *   ★★ THE STANDARD LEAVES THIS LATITUDE AND THE REFERENCES USE
                        *   IT DIFFERENTLY -- one of them INCONSISTENTLY. MEASURED
                        *   2026-08-27, the imaginary component as raw f32 bits:
                        *       gcc 13.3.0   static 3e051eb9   runtime 3e051eb8
                        *       clang 18.1.3 static 3e051eb8   runtime 3e051eb8
                        *       DSS          static 3e051eb9   runtime 3e051eb9
                        *   gcc's own compile-time fold DISAGREES WITH ITS OWN RUNTIME.
                        *   So "match a reference's bytes" is not even a well-defined
                        *   target here -- exactly the shape the `_BitInt` padding
                        *   decision hit. The invariant that IS well-defined, and the
                        *   only one that can be a correctness constraint, is that a
                        *   compiler's static image equals what ITS OWN emitted code
                        *   computes. That is what this arm asserts, which is why it
                        *   compares against the RUNTIME value and never against a
                        *   hand-typed constant.
                        *   ⚠ A consequence worth knowing before you debug it: running
                        *   THIS FILE under gcc exits 239 (255 - 16), because gcc fails
                        *   its own arm 5. That is gcc's inconsistency, not a defect in
                        *   the example. Every other arm passes under gcc. */
struct Holder { double _Complex z; int tag; };
struct Holder g_hold = { __builtin_complex(9.0, -2.0), 5 };
double _Complex g_arr[2] = { __builtin_complex(1.0, 2.0),
                             __builtin_complex(-3.0, -4.0) };

/* ── anti-fold seeds: written before use, read as volatile ────────────────── */
volatile double vd = 1.0;
volatile float  vf = 1.0f;

static int sameComplex(double _Complex a, double _Complex b) {
    return creal(a) == creal(b) && cimag(a) == cimag(b);
}

/* Byte-for-byte object-representation compare (C 6.2.6.1) of two complex objects. */
static int sameBytes(double _Complex *a, double _Complex *b) {
    unsigned char *pa = (unsigned char *)a;
    unsigned char *pb = (unsigned char *)b;
    unsigned long i;
    for (i = 0; i < sizeof(double _Complex); ++i)
        if (pa[i] != pb[i]) return 0;
    return 1;
}

int main(void) {
    int acc = 0;
    double d1 = vd;          /* == 1.0, but the optimizer cannot know it */
    float  f1 = vf;          /* == 1.0f */

    /* 1 — both components present, distinct, and in the right slots. */
    if (creal(g_bc) == 3.0 * d1 && cimag(g_bc) == 4.0 * d1) acc += 1;

    /* 2 — the real->complex promotion: the imaginary half MUST be zero, and the
     *     int->complex sibling must agree. A producer writing `re` twice fails. */
    if (creal(g_real) == 7.0 * d1 && cimag(g_real) == 0.0
        && creal(g_int) == 5.0 * d1 && cimag(g_int) == 0.0) acc += 2;

    /* 3 — ZERO real, non-zero imaginary. The mirror of arm 2: a producer that only
     *     ever writes the real component yields (0,0) here. */
    if (creal(g_imag) == 0.0 && cimag(g_imag) == 1.0 * d1) acc += 4;

    /* 4 — signs survive into the image, on BOTH components independently. */
    if (creal(g_neg) == -3.0 * d1 && cimag(g_neg) == -4.0 * d1
        && creal(g_conj) == 3.0 * d1 && cimag(g_conj) == -4.0 * d1) acc += 8;

    /* 5 — THE MIXED-PRECISION ARM, and the sharpest one here: the folded image must
     *     equal the SAME expression computed at RUNTIME. `I` is complex<double>, so
     *     the product below is evaluated at F64 and narrowed to F32 exactly once, at
     *     the assignment. A const-fold that rounded every intermediate to the
     *     DECLARED element type instead differs in the imaginary component by one
     *     ULP (MEASURED: b9 1e 05 3e vs b8 1e 05 3e) — an initializer that disagrees
     *     with the code reading it, which nothing recomputes at load. Every F64 arm
     *     above stays green under that defect; only this one sees it. */
    {
        float _Complex runtime = (0.1f * f1 + (0.2f * f1) * I)
                               * (0.3f * f1 + (0.7f * f1) * I);
        if (creal(g_f32) == creal(runtime) && cimag(g_f32) == cimag(runtime))
            acc += 16;
    }

    /* 6 — complex arithmetic: the folded sum equals the runtime sum. */
    {
        double _Complex runtime = 3.0 * d1 + (4.0 * d1) * I;
        if (sameComplex(g_sum, runtime)) acc += 32;
    }

    /* 7 — THE IMAGE ITSELF, read as bytes. A runtime-built (3,4) must have the same
     *     object representation as the static one; this arm survives any amount of
     *     value-level constant propagation. */
    {
        double _Complex runtime = __builtin_complex(3.0 * d1, 4.0 * d1);
        if (sameBytes(&g_bc, &runtime)) acc += 64;
    }

    /* 8 — the aggregate leaves (struct member, array elements) and the DIVISION
     *     fold, each against a runtime computation. The array's two elements have
     *     four distinct components, so a stride or ordering error shows. */
    {
        double _Complex rdiv = (1.0 * d1 + (2.0 * d1) * I)
                             / (3.0 * d1 + (4.0 * d1) * I);
        int ok = sameComplex(g_div, rdiv);
        if (creal(g_hold.z) != 9.0 * d1 || cimag(g_hold.z) != -2.0 * d1) ok = 0;
        if (g_hold.tag != 5) ok = 0;
        if (creal(g_arr[0]) != 1.0 * d1 || cimag(g_arr[0]) != 2.0 * d1) ok = 0;
        if (creal(g_arr[1]) != -3.0 * d1 || cimag(g_arr[1]) != -4.0 * d1) ok = 0;
        if (ok) acc += 128;
    }

    return acc;
}
