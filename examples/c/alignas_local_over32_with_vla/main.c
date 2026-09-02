// C11/C23 6.7.5 + 6.7.6.2 (D-CSUBSET-ALIGNAS-OVERALIGNED-STACK-LOCAL x D-CSUBSET-VLA):
// an over-aligned FIXED local in a function that ALSO declares a VLA. This is the one
// interaction the over-aligned-local mechanism has with the rest of the frame, and it
// is the case a reader would most reasonably doubt.
//
// A VLA function moves the stack pointer at RUN TIME (`sub sp, <runtime size>`), so
// the fixed frame can no longer be found off SP. Every fixed-frame reference in such
// a function is therefore emitted off the FRAME POINTER instead — a copy of the
// post-prologue SP that the runtime `sub` does not move. The over-aligned local's
// address rounding must ride THAT base, and the reasoning it depends on has to survive
// the substitution: the frame pointer is a copy of a stack pointer that is congruent
// to 0 modulo the stack alignment, so it carries exactly the same guarantee and the
// same `align - stackAlignment` bound on the rounding.
//
// The function is a LEAF by construction — no call. A VLA function that calls
// anything is separately refused today (`L_VlaNonLeafFrameUnsupported`,
// D-CSUBSET-VLA-NONLEAF-CALL-FRAME), so a sink call here would measure that refusal
// instead of this interaction. Address-taking is what keeps the locals in real frame
// slots; `mem2reg` cannot promote a local whose address is taken.
//
// Checks: the VLA's first and last elements survive, the over-aligned local's own
// bytes survive, its address is 32-aligned, and an ordinary local declared alongside
// is intact — so neither the runtime `sub sp` nor the address rounding walked into
// the other's bytes.
//
// Red-on-disable: zero `frameSlotAlignHeadroom` or drop the
// `emitAlignUpToPowerOfTwo` call in `emitLeaFrameSlot` and this returns 1, not 42.

int check(int n) {
    alignas(32) unsigned char fixed[8];
    int vla[n];
    int witness = 5;

    fixed[0] = 3;
    fixed[7] = 4;
    vla[0]     = 7;
    vla[n - 1] = 9;

    if (vla[0] != 7 || vla[n - 1] != 9)
        return 1;                       // the runtime sub-sp region moved under us
    if (fixed[0] != 3 || fixed[7] != 4)
        return 1;                       // the rounded slot overlapped something
    if (((unsigned long long)(&fixed[0]) & 31ull) != 0ull)
        return 1;                       // under-aligned off the frame pointer
    if (witness != 5)
        return 1;                       // a reservation overran its neighbour

    return 0;
}

int main(void) {
    if (check(6) != 0)
        return 1;
    return 42;
}
