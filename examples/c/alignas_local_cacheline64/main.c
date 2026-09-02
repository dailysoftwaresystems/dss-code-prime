// C11/C23 6.7.5 (D-CSUBSET-ALIGNAS-OVERALIGNED-STACK-LOCAL): the cache-line case —
// `alignas(64)`, four times the stack alignment every shipped ABI guarantees. This
// is the shape a real consumer reaches for (a cache-line-aligned scratch buffer, or
// the alignment an AVX-512 / SVE load demands), and it is the reason the fix cannot
// stop at "one step above the stack alignment": the headroom is
// `align - stackAlignment` bytes, so it GROWS with the requested alignment, and a
// fix that hard-coded a single step would under-reserve here while passing at 32.
//
// TWO over-aligned locals, so the reservation is exercised twice in one frame and
// the second is placed above the first's headroom. Both addresses are masked; a
// nonzero low six bits on either is a loud failure. A third, ordinary local is read
// back afterwards to prove neither reservation overran its neighbour.
//
// Red-on-disable: zero `frameSlotAlignHeadroom` (or drop `emitAlignUpToPowerOfTwo`
// from `emitLeaFrameSlot`) and this returns 1 instead of 42. Holds under the
// baseline AND the shipped release pipeline, on all four targets.

// Non-inlinable sink: keeps both buffers address-taken and live across a real call,
// so neither is register-promoted and the frame is a non-leaf one.
int sink(unsigned char *p, unsigned char *q, int *r);

int sink(unsigned char *p, unsigned char *q, int *r) {
    return (int)p[0] + (int)q[0] + *r;
}

int check(void) {
    alignas(64) unsigned char a[8];
    alignas(64) unsigned char b[8];
    int witness = 100;

    a[0] = 1;
    b[0] = 2;

    if (sink(a, b, &witness) != 103)
        return 1;

    unsigned long long const pa = (unsigned long long)(&a[0]);
    unsigned long long const pb = (unsigned long long)(&b[0]);
    if (((pa | pb) & 63ull) != 0ull)
        return 1;                       // either under-aligned → loud failure
    if (pa == pb)
        return 1;                       // two locals must not share one address
    if (witness != 100)
        return 1;                       // a reservation overran its neighbour

    return 0;
}

int main(void) {
    if (check() != 0)
        return 1;
    return 42;
}
