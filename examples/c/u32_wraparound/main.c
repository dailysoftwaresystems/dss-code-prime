// FC3 c2 — THE defined-wraparound witness (D-CSUBSET-32BIT-ALU-FORMS).
//
// C 6.2.5: unsigned arithmetic is DEFINED modulo 2^32 for `unsigned
// int` — 0xFFFFFFFFu + 1u MUST be 0. Before c2 this program was
// impossible: U32 arithmetic was gated loudly (the shipped ALU forms
// computed 64-wide, which would produce 0x100000000 instead of 0).
// c2's 32-bit forms (x86 no-REX.W, auto-zero-extending / arm64
// W-forms) make the wrap correct BY CONSTRUCTION.
//
//   32-bit add:  big + 1u == 0          -> exit 42
//   64-wide add: big + 1u == 0x100000000 -> exit 7   (the miscompile
//                lever: force the width axis to 64 and this flips)
//
// **Encoding discipline**: every literal fits the narrowest shipped
// immediate slot (arm64 movz imm16 <= 65535; x86 imm32). 0xFFFFFFFF
// NEVER appears as a literal — it is BUILT at runtime:
//   b*b           = 256*256       = 65536
//   a*(b*b) + a   = 65535*65536 + 65535 = 0xFFFFFFFF
// (every intermediate fits unsigned int exactly; no wrap happens
// before the witness `+ 1u`).
//
// **Fold resistance**: the seeds reach wrapTag as FUNCTION ARGS, so
// the baseline (unoptimized) arm keeps live runtime 32-bit MUL/ADD.
// (No inlining+constfold differential arm HERE: folding the chain
// mints a U32 constant 0xFFFFFFFF, which materializes fine through
// x86's exact 32-bit mov-imm but hits arm64's pre-existing
// MOVZ-imm16 wide-immediate wall [D-LK10-ENTRY-ARM64-WIDE-IMMEDIATE]
// — and optimizedPipelines arms are manifest-level, applying to every
// target. ConstFold's U32 wrap-to-width correctness is pinned at the
// unit tier instead: tests/opt ConstFold U32 wraparound pin.)

unsigned int wrapTag(unsigned int a, unsigned int b) {
    unsigned int big;
    big = b * b;            // 65536
    big = a * big + a;      // 0xFFFFFFFF — built, never spelled
    unsigned int z;
    z = big + 1u;           // THE witness: DEFINED wrap to 0
    if (z == 0u) {
        return 42u;
    }
    return 7u;
}

int main() {
    if (wrapTag(65535u, 256u) == 42u) {
        return 42;
    }
    return 7;
}
