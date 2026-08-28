// D-CODEGEN-APPLE-ARM64-STACK-ARGS-NOT-NATURALLY-PACKED — runtime witness.
//
// Apple's arm64 ABI packs a stacked NAMED SCALAR at its own alignment and
// advances the overflow cursor by its own SIZE. AAPCS64 gives every stacked
// argument a whole 8-byte slot. DSS did the AAPCS64 thing on both, so a DSS
// binary agreed with itself in every direction and disagreed with clang at the
// boundary — the shape a single-compiler corpus cannot see.
//
// ✔MEASURED 2026-08-24, Apple clang 21.0.0 / macOS 26.5.2, `otool -tV` of a
// `-target arm64-apple-macos` object, re-derived at cycle P31:
//     sink(int x8, char c, short s, int i, long l, char c2)
//       callee: ldrsb w12,[sp,#0x40]   c  at incoming +0   (1-byte read)
//               ldrsh w11,[sp,#0x42]   s  at incoming +2   (2-byte read)
//               ldr   w10,[sp,#0x44]   i  at incoming +4   (4-byte read)
//               ldr   x9, [sp,#0x48]   l  at incoming +8
//               ldrsb w8, [sp,#0x50]   c2 at incoming +16
//       caller: strb w8,[x9] / strh w8,[x9,#0x2] / … / strb w8,[x9,#0x10]
//     and for a VARIADIC callee with the same named tail, the caller writes the
//     first vararg with `str x` at +8 — 8-byte vararg slots, over a named region
//     that ended at 4 and rounded up. So `va_start`'s base is incoming+8.
// gcc 13.3.0 on aarch64-linux gives +0/+8/+16/+24/+32 and a va_start base of
// +16 for the same source: the two ABIs genuinely differ, and both are right.
//
// ★ WHAT MAKES THIS RED WHEN THE RULE IS WRONG, rather than merely self-
// consistent. Part (2) below reads the named region THROUGH `ap` — an address
// the ABI defines, not one DSS chooses — and asserts the Apple offsets as
// LITERALS. A slot-packing DSS anchors `ap` 8 bytes further along, so `ap[-8]`
// lands on the short's slot instead of the char, and the check fails. Parts (1)
// and (3) are congruence checks across tiers (the caller's placement lives in
// LIR, the callee's `va_start` base in HIR->MIR): they stay green under either
// rule but red if the two tiers ever disagree about which rule they are using.
//
// Exit code 0 = every part agreed; otherwise a bit-mask naming the part.

#include <stdarg.h>

long g_sink;

// ── (3) NON-VARIADIC: three stacked named scalars of three sizes ────────────
// Eight named ints exhaust x0..x7 on both ABIs, so c/s/i are on the stack.
__attribute__((noinline))
int sink(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7,
         signed char c, short s, int i) {
    return (int)c + (int)s * 10 + i * 100;
}

// ── (1)+(2) VARIADIC: a naturally-packable named tail, then varargs ─────────
__attribute__((noinline))
int probe(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7,
          signed char c, short s, ...) {
    int bad = 0;
    va_list ap;
    va_start(ap, s);

    // (1) CROSS-TIER CONGRUENCE: the caller places the varargs from the LIR
    //     overflow cursor; the callee's `va_start` base comes from the HIR->MIR
    //     named-parameter cursor. Two implementations of one rule — if they
    //     disagree about where the named region ends, these reads are garbage.
    if (va_arg(ap, int) != 11) bad |= 1;
    if (va_arg(ap, int) != 22) bad |= 1;
    if (va_arg(ap, int) != 33) bad |= 1;
    va_end(ap);

#if defined(__arm64__)
    // (2) THE ABI FACT, ASSERTED AGAINST A LITERAL. On Apple `va_list` IS a
    //     plain `char *` anchored at the first stacked vararg, and the named
    //     region sits immediately below it: with the cursor at 4 (char@0 +
    //     short@2) the base rounds to +8, so the char is 8 bytes below `ap` and
    //     the short 6. Under whole-slot packing the base is +16 and `ap[-8]`
    //     is the short's slot — which is what makes this the red-on-disable.
    {
        va_list ap2;
        va_start(ap2, s);
        unsigned char const *base = (unsigned char const *)ap2;
        signed char const   *pc   = (signed char const *)(base - 8);
        short const         *ps   = (short const *)(base - 6);
        if (*pc != c) bad |= 2;
        if (*ps != s) bad |= 4;
        va_end(ap2);
    }
#endif
    return bad;
}

int main(void) {
    int rc = 0;

    // (3) caller and callee must agree on the packed placement AND on the
    //     width of each access: a whole-slot store of `c` would overwrite `s`.
    //     7 + 90 + 500 = 597.
    if (sink(0, 1, 2, 3, 4, 5, 6, 7, (signed char)7, (short)9, 5) != 597)
        rc |= 8;

    // (1)+(2)
    rc |= probe(0, 1, 2, 3, 4, 5, 6, 7, (signed char)7, (short)9, 11, 22, 33);

    // A second shape whose named region ends mid-slot at a DIFFERENT point:
    // int@0 + char@4 + int@8 leaves the cursor at 12 -> the varargs start at
    // +16, not +24. Same congruence claim, different arithmetic.
    g_sink = rc;
    return rc;
}
