/* FC7 C3 (D-FC7-STRUCT-BY-VALUE-ARG-RETURN — AAPCS64 / Apple arm64): struct
 * by-value ARG + RETURN under AAPCS64, across every class the strategy realizes:
 *   - mkPairD() — {double;double} 16B HFA → TWO FPR pieces, returned in d0:d1.
 *   - mkTriF()  — {float;float;float} 12B HFA → THREE FPR pieces returned in
 *                 s0:s1:s2 (a FLOAT HFA: width-4 pieces — exercises the F32
 *                 return-piece accept, plan-lock MF-2; a single d-load would
 *                 mis-pack two floats).
 *   - mkTri()   — {int;int;int} 12B non-HFA → TWO GPR pieces, returned in x0:x1
 *                 (NO per-eightbyte SSE — a non-HFA AAPCS64 aggregate is all-GPR).
 *   - mkBig()   — {long;long;long} 24B → >16B → x8 SRET (the caller allocates the
 *                 result storage and passes its address in the dedicated x8
 *                 indirect-result register; the callee returns VOID). mkBig ALSO
 *                 takes a real int arg `tag` (plan-lock N-2): `tag` MUST land in x0
 *                 and the sret pointer in x8 — if the sret pointer were mis-routed
 *                 to x0 (the arg-shift Option A prevents) `tag` would read garbage
 *                 and the exit would drift off 42.
 *   - sumPairD()— takes the {double;double} HFA BY VALUE → d0:d1 arg pieces.
 *   - sumBig()  — takes the 24B struct BY VALUE → >16B → BY REFERENCE (caller copies
 *                 to a temp + passes a pointer; the callee owns the copy).
 * Values flow through the non-inlined mkd()/mkf()/mki()/mkl() so nothing folds;
 * every far field (2nd FPR/GPR piece, the @16 sret field) must survive the transfer.
 *   sumPairD=10+5=15 · mkTriF=3+4+0=7 · mkTri=2+1+4=7 · sumBig=(2+1)+8+2=13 → 42.
 * The aggregate types use `typedef` because a top-level `struct Tag` specifier as a
 * function RETURN type is a pre-FC4 grammar residue (D-CSUBSET-STRUCT-BODY-VARDECL-
 * POSITION); the typedef-name return type is the idiomatic reachable form and
 * exercises the identical ABI codegen. AAPCS64 runtime closes on arm64-ELF under
 * qemu (SysV/Win64 take the hidden-arg sret path; Apple Mach-O is the same struct
 * ABI but runs only on the macos CI leg). RED-ON-DISABLE: a dropped HFA piece, a
 * missed x8 sret copy, the float HFA loaded as a double, or the sret pointer
 * shifting `tag` out of x0 knocks the exit off 42. */
typedef struct { double a; double b; }        PairD;
typedef struct { float  x; float  y; float z; } TriF;
typedef struct { int    p; int    q; int   r; } Tri;
typedef struct { long   a; long   b; long  c; } Big;

double mkd(double v) { return v; }
float  mkf(float v)  { return v; }
int    mki(int v)    { return v; }
long   mkl(long v)   { return v; }

PairD mkPairD(double a, double b)            { PairD s; s.a = a; s.b = b; return s; }
TriF  mkTriF(float x, float y, float z)      { TriF  s; s.x = x; s.y = y; s.z = z; return s; }
Tri   mkTri(int p, int q, int r)             { Tri   s; s.p = p; s.q = q; s.r = r; return s; }
Big   mkBig(int tag, long a, long b, long c) { Big   s; s.a = a + tag; s.b = b; s.c = c; return s; }

double sumPairD(PairD s) { return s.a + s.b; }              /* HFA arg in d0:d1 */
int    sumBig(Big s)     { return (int)(s.a + s.b + s.c); } /* >16B arg by reference */

int main(void) {
    PairD pd = mkPairD(mkd(10.0), mkd(5.0));        /* 16B HFA → d0:d1 */
    TriF  tf = mkTriF(mkf(3.0f), mkf(4.0f), mkf(0.0f)); /* 12B float HFA → s0:s1:s2 (MF-2) */
    Tri   tr = mkTri(mki(2), mki(1), mki(4));        /* 12B → x0:x1 (2 GPR) */
    Big   bg = mkBig(mki(1), mkl(2), mkl(8), mkl(2)); /* 24B → x8 sret; tag@x0 (N-2) */
    return (int)sumPairD(pd)             /* HFA arg: 10+5    = 15 */
         + (int)(tf.x + tf.y + tf.z)     /* float HFA: 3+4+0 =  7 */
         + (tr.p + tr.q + tr.r)          /* 2-GPR:    2+1+4  =  7 */
         + sumBig(bg);                   /* by-ref arg: (2+1)+8+2 = 13 */
}
