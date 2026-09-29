/* D-LK10-ENTRY-ARM64-WIDE-IMMEDIATE (the frame-offset arm): the RUN witness for
 * a frame access BEYOND every form AArch64 declares for it, through every frame
 * zone that can put one there.
 *
 * AArch64 reaches a frame slot two ways: the unscaled LDUR/STUR imm9 (+-256) and
 * the scaled LDR/STR imm12 (up to 4095 x the access size — 32760 bytes for an
 * 8-byte access). A slot past BOTH is reached by materializing its address in a
 * register (`lea A, [sp + off]`) and accessing through it at offset 0. Before
 * this, every function below was REFUSED at the default pipeline and at release
 * (A_ImmediateOperandOutOfRange on 'load', 'store' and 'fldur', then
 * A_FunctionEncodeAborted for all four), while aarch64-linux-gnu-gcc 13.3.0
 * compiles the same file and it RUNS exit 42 at -O0 and -O2 under qemu-aarch64.
 *
 *   stacked_gpr_far        the 9th and 10th integer params are read from the
 *                          caller's outgoing area, ABOVE a 40000-byte local. A
 *                          load into an integer register carries its own address.
 *   stacked_fpr_far        the 9th and 10th double params, same frame. A SIMD&FP
 *                          register cannot address memory, so the address goes in
 *                          the convention's frameAddressScratch (x30) — and this
 *                          function makes no call, so x30, its return address,
 *                          has to be SAVED for it.
 *   stacked_fpr_far_calls  the same in a function that also calls, whose x30 is
 *                          saved already.
 *   spill_far              one right-nested expression over 2305 volatile reads
 *                          keeps every term live until the innermost addition:
 *                          ~2280 spill slots of 16 bytes, so the spill STORES run
 *                          past 32760 — the store the defect was first measured on.
 *
 * ⚠ A COMPILE-ONLY PIN WOULD NOT DO. The far form computes an ADDRESS; a wrong
 * one assembles clean and reads someone else's slot. The exit code is 42 only if
 * every value survives the round trip through a slot past the reach.
 */

#define T1(i)   p[(i)] * ((i) % 7 + 3) + (
#define T4(i)   T1(i) T1((i) + 1) T1((i) + 2) T1((i) + 3)
#define T16(i)  T4(i) T4((i) + 4) T4((i) + 8) T4((i) + 12)
#define T64(i)  T16(i) T16((i) + 16) T16((i) + 32) T16((i) + 48)
#define T256(i) T64(i) T64((i) + 64) T64((i) + 128) T64((i) + 192)
#define C1   )
#define C4   C1 C1 C1 C1
#define C16  C4 C4 C4 C4
#define C64  C16 C16 C16 C16
#define C256 C64 C64 C64 C64

#define NTERMS 2305  /* 9 x 256 nested terms + the innermost one */

static volatile long src[NTERMS];

__attribute__((noinline))
long stacked_gpr_far(long a, long b, long c, long d, long e, long f, long g,
                     long h, long i, long j) {
    volatile char buf[40000];
    buf[0] = (char)a;
    buf[39999] = (char)b;
    return i * 3 + j + buf[0] + buf[39999] + c + d + e + f + g + h;
}

__attribute__((noinline))
double stacked_fpr_far(double a, double b, double c, double d, double e,
                       double f, double g, double h, double i, double j) {
    volatile char buf[40000];
    buf[0] = (char)a;
    buf[39999] = (char)b;
    return i * 4.0 + j + buf[0] + buf[39999] + c + d + e + f + g + h;
}

__attribute__((noinline))
long sink(long v) { return v + 1; }

__attribute__((noinline))
double stacked_fpr_far_calls(double a, double b, double c, double d, double e,
                             double f, double g, double h, double i, double j) {
    volatile char buf[40000];
    buf[0] = (char)sink((long)a);
    buf[39999] = (char)sink((long)b);
    return i * 5.0 + j + buf[0] + buf[39999] + c + d + e + f + g + h;
}

__attribute__((noinline))
long spill_far(volatile long *p) {
    return T256(0) T256(256) T256(512) T256(768) T256(1024) T256(1280)
           T256(1536) T256(1792) T256(2048)
           p[2304]
           C256 C256 C256 C256 C256 C256 C256 C256 C256;
}

int main(void) {
    long expect = 0;
    for (int k = 0; k < NTERMS; ++k) {
        src[k] = (long)(k % 5);
        expect += (long)(k % 5) * (k < NTERMS - 1 ? (k % 7 + 3) : 1);
    }
    int ok = 0;
    /* 7*3 + 8 + 1 + 2 + (3+4+5+6+7+8) = 65 */
    ok += stacked_gpr_far(1, 2, 3, 4, 5, 6, 7, 8, 7, 8) == 65;
    /* 7*4 + 8 + 1 + 2 + (3+4+5+6+7+8) = 72 */
    ok += stacked_fpr_far(1, 2, 3, 4, 5, 6, 7, 8, 7, 8) == 72.0;
    /* 7*5 + 8 + sink(1) + sink(2) + (3+4+5+6+7+8) = 35 + 8 + 2 + 3 + 33 = 81 */
    ok += stacked_fpr_far_calls(1, 2, 3, 4, 5, 6, 7, 8, 7, 8) == 81.0;
    ok += spill_far(src) == expect;
    return ok == 4 ? 42 : ok;
}
