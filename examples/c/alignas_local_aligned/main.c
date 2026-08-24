// C11/C23 6.7.5 (D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN): an OVER-ALIGNED stack local
// — `alignas(16) ...` in a NON-LEAF function that passes an ODD number of
// stack-overflow arguments — must land on a 16-byte boundary at RUNTIME. This is
// the exact case the frame fix (B2) closes:
//
//   The local slots sit at `sp + localAreaOffset() + i*slotWidth`, where
//   `localAreaOffset() = outgoingArgArea + savedRegArea + spillArea`. The saved +
//   spill areas are slot-width (16) multiples, but the outgoing-arg area is
//   `shadowSpace + outgoingArgSlots*8`. When `outgoingArgSlots` is ODD the base is
//   only 8-aligned (≡ 8 mod 16), so a 16-aligned local is NOT honored — a latent
//   silent miscompile. The fix pads the local base up to 16 when a local needs it.
//
// `sink9` takes 9 int args. The stack-overflow arg count is ODD on every shipped
// ABI: SysV 9-6=3, Win64 9-4=5, AAPCS64 9-8=1 — so `outgoingArgSlots` is odd on
// all four targets, forcing the ≡8-mod-16 base the fix corrects. The `alignas(16)`
// local's runtime address is masked; a nonzero low nibble is a loud failure.
//
// Red-on-disable: revert the computeFrameLayout `localAreaAlignPad` (B2) and the
// local base stays 8-aligned → `&buf & 15` is 8 → the check fails → exit 1, not 42.
// This holds under both the baseline and the shipped release pipeline.

// A non-inlinable sink with 9 integer parameters. Returns their sum so the call
// is not dead-code-eliminated (its result feeds the guarded return).
int sink9(int a, int b, int c, int d, int e, int f, int g, int h, int i);

int sink9(int a, int b, int c, int d, int e, int f, int g, int h, int i) {
    return a + b + c + d + e + f + g + h + i;
}

int check(void) {
    // A 16-natural type would also exercise this; `alignas(16)` on a 4-aligned
    // array proves the EXPLICIT override reaches the frame layout.
    alignas(16) int buf[4];
    // Force `buf` to be address-taken + live across the call so it gets a real
    // frame slot (not promoted to a register), and the call forces the non-leaf
    // odd-overflow frame shape above.
    buf[0] = sink9(1, 2, 3, 4, 5, 6, 7, 8, 9);   // == 45 (odd stack overflow)
    if (((unsigned long long)(&buf[0]) & 15ull) != 0ull)
        return 1;                                // misaligned → loud failure
    return 0;                                    // aligned
}

int main(void) {
    if (check() != 0)
        return 1;
    return 42;
}
