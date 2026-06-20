/* D-FC12C-APPLE-ARM64-VARIADIC-CALLEE (struct varargs): STRUCT varargs under the Apple
 * arm64 (Mach-O) ABI. Apple passes ALL variadic args on the STACK (variadicArgsAlwaysStack),
 * so the callee `va_arg(struct)` is the LINEAR HomogeneousPointer slot walk (the SAME
 * code path as Win64) — no register-save-area, no dual cursor. This corpus is the
 * end-to-end runtime witness; it RUNS only on the macos-latest CI leg (no off-Mac
 * emulator for Mach-O — SkippedCrossHost elsewhere, which is expected, NOT a failure).
 *
 *   - sumTwoHfa() — FOLD 1(a) BLOCKER witness for the Step 2.0 size-aware bump. Reads
 *                   TWO {double;double} 16B-InRegisters HFA varargs and sums BOTH
 *                   structs' BOTH fields. Each 16B HFA occupies TWO contiguous 8-byte
 *                   stack slots, so the cursor MUST advance by roundUp(16,8)=16 after
 *                   the first va_arg — else the SECOND va_arg re-reads the first
 *                   struct's tail (its second double) and the sum is wrong. With the
 *                   correct 16-byte bump: {3,5}+{10,20} = 38; with the latent 8-byte
 *                   under-bump the second read sees {5,10} → 23 (≠ 38). This is the
 *                   ONLY shape where the 8-vs-16 bump changes an OBSERVED value.
 *   - sumSmall()  — {int;int} 8B InRegisters vararg: the slot IS the storage (one
 *                   8-byte slot, by value). One {10,20} → 30.
 *   - sumLarge()  — {long;long;long} 24B (>16B) ByReference vararg: the slot holds a
 *                   hidden POINTER to the caller's copy, dereferenced. One {1,2,3} → 6.
 *
 * Values flow through the non-inlined mkd()/mki()/mkl()/mkPairD()/mkPair()/mkBig()
 * factories so nothing folds. 38 + 30 + 6 = 74 -> exit 74. RED-ON-DISABLE: the Step 2.0
 * under-bump flips sumTwoHfa off 38; a dropped ByReference deref flips sumLarge.
 * Mach-O-arm64; macos-latest CI leg is the runtime witness.
 */
typedef struct { double a; double b; }        PairD;
typedef struct { int    a; int    b; }        Pair;
typedef struct { long   a; long   b; long c; } Big;

double mkd(double v) { return v; }
int    mki(int v)    { return v; }
long   mkl(long v)   { return v; }

PairD mkPairD(double a, double b)    { PairD s; s.a = a; s.b = b; return s; }
Pair  mkPair(int a, int b)          { Pair  s; s.a = a; s.b = b; return s; }
Big   mkBig(long a, long b, long c) { Big   s; s.a = a; s.b = b; s.c = c; return s; }

/* TWO 16B HFA varargs — the cursor must bump by 16 between them (FOLD 1a). */
double sumTwoHfa(int n, ...) {
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

/* {int;int} 8B InRegisters vararg — slot IS the storage. */
long sumSmall(int n, ...) {
    va_list ap;
    va_start(ap, n);
    long t = 0;
    for (int i = 0; i < n; i = i + 1) {
        Pair p = va_arg(ap, Pair);
        t = t + p.a + p.b;
    }
    va_end(ap);
    return t;
}

/* {long;long;long} 24B ByReference vararg — slot holds a hidden pointer. */
long sumLarge(int n, ...) {
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

int main(void) {
    long r = 0;
    r = r + (long)sumTwoHfa(2, mkPairD(mkd(3.0), mkd(5.0)),
                               mkPairD(mkd(10.0), mkd(20.0)));   /* 38 (FOLD 1a) */
    r = r + sumSmall(1, mkPair(mki(10), mki(20)));               /* 30 */
    r = r + sumLarge(1, mkBig(mkl(1), mkl(2), mkl(3)));          /*  6 */
    return (int)r;                                               /* 74 */
}
