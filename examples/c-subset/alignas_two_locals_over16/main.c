// C11/C23 6.7.5 (D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN, #2 arm64 per-alloca fix):
// TWO over-aligned stack locals, each `alignas(16)`, that are NOT the first local
// (a `char pad` precedes them). BOTH must land on a 16-byte boundary at runtime.
//
// This is the arm64-specific gap the per-alloca frame fix closes. The frame layout
// aligns the local-area BASE to the max local alignment, but each alloca then sits
// at `base + Σ allocaSlotCount*slotWidth` — a FLAT progression with no per-alloca
// re-alignment. On arm64 EVERY register is 8 bytes, so the local slot width is 8
// while the stack alignment is 16. A `char pad` local reserves ONE 8-byte slot, so
// the NEXT local (`x`) lands at `base + 8` — only 8-aligned, NOT 16. A second
// `alignas(16)` local (`y`) after `x` compounds the skew. (x86_64 is SAFE: its
// slot width is 16, so every slot is already 16-aligned.)
//
// The fix aligns EACH alloca's offset up to its OWN effective alignment before
// placing it (identically at the size/precompute/materialize sites), so `x` and
// `y` each round up to a 16-byte boundary. The runtime check masks BOTH addresses;
// any nonzero low nibble on either is a loud failure.
//
// Red-on-disable: revert the per-alloca `alignUp(off, allocaAlign)` in
// computeFrameLayout + the callconv precompute/materialize, and on arm64 `x`/`y`
// land 8-aligned → `(&x | &y) & 15` is nonzero → exit 1, not 42. This manifests on
// the arm64/qemu leg specifically (x86_64 already passes). Holds under baseline AND
// the shipped release pipeline.

// A non-inlinable sink so the address-taken locals stay live across a real call
// (mem2reg cannot promote an address-taken local to a register — it needs a slot).
int sink(long long *p, long long *q, char *r);

int sink(long long *p, long long *q, char *r) {
    return (int)(*p + *q + (long long)(*r));
}

int check(void) {
    // `pad` occupies one 8-byte frame slot BEFORE the over-aligned locals, so the
    // running local offset is 8 (not 0) when `x` is placed — the exact condition
    // that leaves a non-first arm64 local 8-aligned without the per-alloca fix.
    char pad = 1;
    alignas(16) long long x = 2;
    alignas(16) long long y = 3;
    // Address-take all three + feed a live use so none is register-promoted and the
    // call is not dead-code-eliminated.
    int const s = sink(&x, &y, &pad);
    if (s != 6)                                      // 2 + 3 + 1
        return 1;
    unsigned long long const ax = (unsigned long long)(&x);
    unsigned long long const ay = (unsigned long long)(&y);
    if (((ax | ay) & 15ull) != 0ull)
        return 1;                                    // either misaligned → loud fail
    return 0;                                        // both 16-aligned
}

int main(void) {
    if (check() != 0)
        return 1;
    return 42;
}
