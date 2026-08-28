// OPT8 (plan 22) NEGATIVE PIN for the LIR peephole's redundant-copy rule.
//
// THE HAZARD. After register allocation an enormous number of LIR
// instructions have their RESULT register equal to their only OPERAND
// register -- the allocator re-uses a dying input's register for the output,
// which is exactly what you want it to do. ✔MEASURED over the 585 dumping
// examples of `examples/c/**` (2026-08-25): 12021 such instructions exist at
// post-callconv, and only 5575 of them are the register class's declared
// COPY. The other 6446 -- `zext` (3928), `sext` (1283), `trunc` (404), `not`
// (219), `neg` (219), `shl`, `shr_l`, `movaps`, `fpcvt`, `clz`, `popcount`,
// `ctz`, `bswap`, `shr_a` -- read a register and write a DIFFERENT value
// back into it.
//
// A peephole shaped as "the result is its own only operand => delete" would
// delete all 6446. It would also still pass every structural check in the
// pipeline: the module verifies, the encoder is happy, the corpus links, and
// the program simply computes the wrong number. On x86-64 the temptation is
// sharper still, because `mov` (64-bit), `trunc` and `zext` are THREE
// DIFFERENT LIR opcodes that all encode to bytes a disassembler prints as
// `mov` -- so a byte-pattern or mnemonic-text peephole cannot tell them
// apart either. Only an IDENTITY TEST against the schema's declared
// `registerClassOps[].move` opcode separates them.
//
// THE PIN. Each check below owns one bit of the exit code, so a wrongly
// deleted instruction names itself instead of merely making the answer
// "not 15". Every one of them observes its operation at SIXTY-FOUR BITS,
// which is the part that is easy to get wrong: a 32-bit comparison of a
// wrongly-un-truncated value still sees the correct low half and would let
// the bug through.
//
//   bit 0 (1)  TRUNC+ZEXT : narrowing 0x0F0F0F0F0F0F0F0F to 32 bits and
//                           widening it back must give 0x0F0F0F0F. With the
//                           narrowing deleted the high half survives.
//   bit 1 (2)  SEXT       : (long long)(int)0x80000000 must be NEGATIVE.
//                           With the sign extension deleted it is the
//                           positive 0x0000000080000000.
//   bit 2 (4)  NOT        : ~g_bits must have low byte 0xF0, not 0x0F.
//   bit 3 (8)  NEG        : -(long long)g_bits must be negative and differ
//                           from its input.
//
// exit code = 1|2|4|8 = 15 when every self-op survived.
//
// THE OPACITY, and why it is `volatile`. The peephole runs post-regalloc, so
// the pin is only real if these operations reach LIR at all -- a literal the
// MIR ConstFold pass can evaluate never becomes an instruction. The inputs
// are therefore `volatile` globals: the optimizer must re-read them and
// cannot fold them, so the `release` arm exercises the SAME instructions the
// baseline arm does. (A `static` helper taking parameters is not enough:
// release inlines it and then folds the literals.)

volatile unsigned long long g_bits = 0x0F0F0F0F0F0F0F0FULL;
volatile unsigned int       g_high = 0x80000000U;

int main(void) {
    int code = 0;

    // TRUNC (+ the ZEXT that makes it observable at 64 bits). The narrowing
    // MUST drop the high half; a deleted `trunc pN, pN` leaves it in place,
    // and the 64-bit compare below sees it.
    unsigned long long roundTripped = (unsigned long long)(unsigned int)g_bits;
    if (roundTripped == 0x0F0F0F0FULL) code |= 1;

    // SEXT. The sign MUST be replicated into the upper half; a deleted
    // `sext pN, pN` leaves the value positive.
    long long widened = (long long)(int)g_high;
    if (widened < 0) code |= 2;

    // NOT. The complement differs from its input in every bit.
    unsigned long long inverted = ~g_bits;
    if ((inverted & 0xFFULL) == 0xF0ULL) code |= 4;

    // NEG. The negation of a positive value is negative and differs from it.
    long long negated = -(long long)g_bits;
    if (negated < 0 && negated != (long long)g_bits) code |= 8;

    return code;   // 15 when every self-op survived the peephole
}
