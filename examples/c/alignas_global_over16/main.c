// C11/C23 6.7.5 (D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN): an OVER-ALIGNED global
// variable — `alignas(32) int g;` — must land on a 32-byte boundary in its data
// section. A data section aligns to any power of two (no stack-slot bound the way
// a local has), so 32 is honored directly: the frontend stores the explicit
// alignment on the symbol, HIR→MIR stamps it on MirGlobal.alignment, and the
// assembler raises the emitted data item's section alignment to max(natural, 32).
// This RUNS on every target (the address check is data-model-independent).
// Red-on-disable: revert the asm `raiseToExplicit` and the global lands at its
// natural 4-byte alignment → `&g & 31` is (almost always) nonzero → exit 0, not 42.

alignas(32) int g;                    // over-aligned global (natural align 4 → 32)

int main(void) {
    // The address must be 32-byte aligned. `(unsigned long long)&g` is the
    // integer value of the pointer; masking the low 5 bits must be 0.
    if (((unsigned long long)(&g) & 31ull) != 0ull)
        return 1;                     // misaligned → loud failure exit code
    g = 42;
    return g;                         // 42 on success
}
