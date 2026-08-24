/* D-FC12C-AAPCS64-HFA-STRUCT-VARARG: the first DSS binary that passes + reads STRUCT
 * varargs under the AAPCS64 (ARM64-ELF) dual-cursor ABI — every shape the Aapcs64Hfa
 * classifier produces, on BOTH the register arm AND the __stack overflow arm:
 *
 *   - sumHfa()         — {double;double} 16B HFA vararg → 2 FPR pieces gathered from
 *                        the VR save block (__vr_top/__vr_offs, 16B stride). One
 *                        {3,5} → 8. Witnesses the HFA REGISTER arm + H1 (sign-extend)
 *                        + H2 (reads __vr_offs, NOT __gr_offs).
 *   - sumGpr()         — {long;long} 16B non-HFA vararg → 2 GPR pieces gathered from
 *                        the GR save block (__gr_top/__gr_offs, 8B stride). One
 *                        {10,20} → 30. Witnesses the non-HFA REGISTER arm.
 *   - sumByRef()       — {long;long;long} 24B (>16B) ByReference vararg → a hidden
 *                        POINTER in ONE GR slot, dereferenced. One {1,2,3} → 6.
 *                        Witnesses the by-reference indirection (H5/H7).
 *   - sumAfterDrain()  — GR-EXHAUSTION: fixed `k`(x0) + 7 long scalars (x1..x7) drain
 *                        the 8-entry GR pool, so the trailing {long;long} struct is
 *                        forced WHOLE to __stack (the caller's ByValueStackArg carrier)
 *                        and the callee reads it from the GR OVERFLOW arm. 1..7 = 28,
 *                        struct 40+5 = 45 → 73. Witnesses caller H8 + the non-HFA
 *                        overflow read.
 *   - sumHfaOverflow() — FOLD 2 / H6: 8 scalar doubles (v0..v7) drain the 8-entry VR
 *                        pool, so the trailing {double;double} HFA struct lands on
 *                        __stack and the callee reads it from the HFA OVERFLOW arm
 *                        (the mis-bump code path the register arm never reaches). The
 *                        overflow bump is roundUp(16,8)=16 (the 8-byte NSAA quantum),
 *                        NOT fpSlotBytes=16-by-coincidence-of-stride: a wrong VR-stride
 *                        bump (16 per slot vs 16 total) or a missed register cutover
 *                        flips the result. 8×1 = 8, HFA 6+4 = 10 → 18.
 *
 * Values flow through the non-inlined mkd()/mkl()/mkPairD()/mkLL()/mkBig() factories so
 * nothing constant-folds across the call boundary; every far field (2nd FPR/GPR piece,
 * the @16 ByRef field, the overflow-resident struct) must survive the transfer.
 *   8 + 30 + 6 + 73 + 18 = 135 -> exit 135. RED-ON-DISABLE: a dropped HFA piece, a
 * wrong-class save block (H2), a missing sign-extend (H1), a dropped ByRef deref (H5),
 * a missing caller exhaustion check (H8), or a wrong overflow stride (H6) knocks the
 * exit off 135. arm64-ELF under qemu-aarch64 (the linux-arm64 CI leg / local qemu).
 */
typedef struct { double a; double b; }        PairD;
typedef struct { long   a; long   b; }        LL;
typedef struct { long   a; long   b; long c; } Big;

double mkd(double v) { return v; }
long   mkl(long v)   { return v; }

PairD mkPairD(double a, double b)         { PairD s; s.a = a; s.b = b; return s; }
LL    mkLL(long a, long b)               { LL    s; s.a = a; s.b = b; return s; }
Big   mkBig(long a, long b, long c)      { Big   s; s.a = a; s.b = b; s.c = c; return s; }

/* n {double;double} HFA varargs (register arm). */
double sumHfa(int n, ...) {
    va_list ap;
    va_start(ap, n);
    double t = 0.0;
    for (int i = 0; i < n; i = i + 1) {
        PairD p = va_arg(ap, PairD);
        t = t + p.a + p.b;
    }
    va_end(ap);
    return t;
}

/* n {long;long} non-HFA varargs (GR register arm). */
long sumGpr(int n, ...) {
    va_list ap;
    va_start(ap, n);
    long t = 0;
    for (int i = 0; i < n; i = i + 1) {
        LL p = va_arg(ap, LL);
        t = t + p.a + p.b;
    }
    va_end(ap);
    return t;
}

/* n {long;long;long} 24B ByReference varargs (hidden-pointer deref). */
long sumByRef(int n, ...) {
    va_list ap;
    va_start(ap, n);
    long t = 0;
    for (int i = 0; i < n; i = i + 1) {
        Big b = va_arg(ap, Big);
        t = t + b.a + b.b + b.c;
    }
    va_end(ap);
    return t;
}

/* k long scalars (drain the GR pool) then ONE {long;long} struct on the overflow. */
long sumAfterDrain(int k, ...) {
    va_list ap;
    va_start(ap, k);
    long t = 0;
    for (int i = 0; i < k; i = i + 1) {
        t = t + va_arg(ap, long);
    }
    LL p = va_arg(ap, LL);   /* GR pool drained → this struct is on __stack */
    va_end(ap);
    return t + p.a + p.b;
}

/* k double scalars (drain the VR pool) then ONE {double;double} HFA on the overflow. */
double sumHfaOverflow(int k, ...) {
    va_list ap;
    va_start(ap, k);
    double t = 0.0;
    for (int i = 0; i < k; i = i + 1) {
        t = t + va_arg(ap, double);
    }
    PairD p = va_arg(ap, PairD);   /* VR pool drained → this HFA is on __stack */
    va_end(ap);
    return t + p.a + p.b;
}

int main(void) {
    long r = 0;
    r = r + (long)sumHfa(1, mkPairD(mkd(3.0), mkd(5.0)));            /*  8 */
    r = r + sumGpr(1, mkLL(mkl(10), mkl(20)));                       /* 30 */
    r = r + sumByRef(1, mkBig(mkl(1), mkl(2), mkl(3)));              /*  6 */
    r = r + sumAfterDrain(7, mkl(1), mkl(2), mkl(3), mkl(4),
                              mkl(5), mkl(6), mkl(7),
                              mkLL(mkl(40), mkl(5)));                /* 73 */
    r = r + (long)sumHfaOverflow(8, mkd(1.0), mkd(1.0), mkd(1.0),
                                    mkd(1.0), mkd(1.0), mkd(1.0),
                                    mkd(1.0), mkd(1.0),
                                    mkPairD(mkd(6.0), mkd(4.0)));    /* 18 */
    return (int)r;                                                   /* 135 */
}
