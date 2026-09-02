// C11/C23 6.7.5 (D-CSUBSET-ALIGNAS-OVERALIGNED-STACK-LOCAL): a stack local whose
// alignment EXCEEDS the calling convention's stack alignment — `alignas(32)` where
// every shipped ABI guarantees 16. Until this row closed, DSS REFUSED the program
// (`L_OverAlignedStackLocal`); gcc 13.3, clang 18.1.3, mingw-w64 gcc 13.2 and MSVC
// 19.51 all accept it and all produce a genuinely 32-aligned local.
//
// WHY A STATIC OFFSET CANNOT DO IT. The post-prologue stack pointer is congruent to
// 0 modulo the cc's stack alignment and no finer — that congruence is the whole of
// what an ABI promises at a call boundary. So `sp + <compile-time constant>` is only
// ever 16-aligned, and no choice of frame offset makes a local 32-aligned. The frame
// instead reserves `align - stackAlignment` spare bytes above the slot, and the
// local's ADDRESS is rounded up at each materialization. The stack pointer, the
// prologue, the spill addressing and the unwind data are all untouched.
//
// THREE THINGS THIS PINS, each of which fails differently:
//
//   1. THE ADDRESS IS ALIGNED. `&buf[0] & 31` must be 0. Fails if the rounding is
//      dropped, or if it is applied to a base the frame did not reserve room above.
//
//   2. THE ROUNDING HAPPENS BEFORE A FOLDED DISPLACEMENT, NOT AFTER.
//      `&buf[3]` is a constant-offset Gep on the alloca. `alignUp(x) + 12` and
//      `alignUp(x + 12)` are DIFFERENT addresses whenever `x` is not already
//      32-aligned: the correct one is congruent to 12 (mod 32), and folding the
//      displacement into the rounding makes it congruent to 0. The check reads the
//      low bits, which no constant folder can answer at compile time — a frame
//      address is not a compile-time value.
//      ⚠ WHICH LOWERING PATH THIS TAKES IS MEASURED, NOT ASSUMED, because the
//      answer differs by config: ✔the `lea_frame_slot` DISPLACEMENT FOLD is reached
//      for this shape only under the RELEASE pipeline (the baseline lowers the Gep
//      through the general address path instead), on both x86_64 and arm64. So the
//      `release` arm below is what pins the fold ordering here, and the baseline
//      arm pins the general path — the two configs check different code, which is
//      the reason this example carries both rather than one. A shape whose fold is
//      reached in BOTH configs is `alignas_member_over32_local`'s `&s.field`.
//
//   3. THE HEADROOM DOES NOT OVERLAP THE NEXT SLOT. `tail` is declared AFTER the
//      over-aligned local, so it is placed above the spare bytes. It is written,
//      then read back after `buf` has been filled: an under-reserved slot lets the
//      rounded-up `buf` write over `tail` and the read-back changes.
//
// The frame shape is chosen to be the hostile one: `pad` sits ahead of the
// over-aligned local so its running offset is nonzero, and `sink9` takes 9 integer
// arguments, which overflow the stack ODDLY on every shipped ABI (SysV 9-6=3, Win64
// 9-4=5, AAPCS64 9-8=1) so the raw local-area base is congruent to 8 (mod 16).
//
// Red-on-disable: return `frameSlotAlignHeadroom` to 0 and drop the
// `emitAlignUpToPowerOfTwo` call in `emitLeaFrameSlot`, and check 1 fails — exit 1,
// not 42. Holds under the baseline AND the shipped release pipeline, on all four
// targets.

// A non-inlinable sink with 9 integer parameters — the odd stack-overflow count.
// Returns the sum so the call is not dead-code-eliminated.
int sink9(int a, int b, int c, int d, int e, int f, int g, int h, int i);

int sink9(int a, int b, int c, int d, int e, int f, int g, int h, int i) {
    return a + b + c + d + e + f + g + h + i;
}

int check(void) {
    // Ahead of the over-aligned local, so its running frame offset is nonzero.
    char pad = 7;
    alignas(32) int buf[4];
    // Behind it, so it sits above the alignment headroom.
    int tail = 0x5A5A;

    buf[0] = sink9(1, 2, 3, 4, 5, 6, 7, 8, 9);   // == 45
    buf[1] = pad;
    buf[2] = 0;
    buf[3] = 0;

    if (buf[0] != 45 || buf[1] != 7)
        return 1;

    // 1. the local itself is 32-aligned
    unsigned long long const base = (unsigned long long)(&buf[0]);
    if ((base & 31ull) != 0ull)
        return 1;

    // 2. a folded constant displacement is added AFTER the rounding
    unsigned long long const third = (unsigned long long)(&buf[3]);
    if ((third & 31ull) != 12ull)
        return 1;

    // 3. the headroom did not eat the next local
    if (tail != 0x5A5A)
        return 1;

    return 0;
}

int main(void) {
    if (check() != 0)
        return 1;
    return 42;
}
