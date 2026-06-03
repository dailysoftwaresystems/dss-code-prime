// D-CSUBSET-WHILE-LOOP-SUBSTRATE (step 13.5 cycle 1, 2026-06-03):
// the first DSS-emitted binary with intra-function control flow.
// Computes sum(1..10) = 55 via a `while` loop and returns it.
//
// Substrate exercised end-to-end:
//
//   * Per-target `condCodeEncoding` table — maps the 10 abstract
//     `TargetCondCode` values (eq/ne/slt/sle/sgt/sge/ult/ule/ugt/uge)
//     to x86's 4-bit nibbles (the low 4 bits of the setcc/jcc opcode
//     byte). Future ARM64 declares its own table mapping into bits
//     0..3 of the B.cc instruction word — same field-position
//     abstraction, different numeric values.
//
//   * `EncodingSlotKind::CondCodeNibble` (via `template.
//     condCodeFromPayload: true`) — the encoder reads the LIR
//     instruction's payload as a TargetCondCode and OR's the
//     schema's nibble into the LAST opcode byte. Used by both
//     `setcc` (0F 90+cc) and `jcc` (0F 80+cc).
//
//   * `EncodingSlotKind::BlockRel32` + per-function block-offset
//     table + per-function patch list — intra-function PC-relative
//     branch targets resolved at assemble time (NOT a linker
//     relocation). The asm.cpp per-function loop records each
//     block's start offset as it encodes, accumulates pending
//     patches when branches emit, then resolves them after the
//     function is fully assembled (`disp = target_offset -
//     (patch_offset + 4)`).
//
//   * `jcc`'s compound encoding `0F 8x rel32; E9 rel32` (11 bytes)
//     — wire[0] is the cond-branch to operand[0]=ifTrue;
//     wire[1] declares `prefixOpcodeBytes: [0xE9]` to bridge to
//     the trailing unconditional jmp to operand[1]=fallthrough.
//     A future optimizer pass elides the trailing jmp when
//     ifFalse IS the next-laid-out block (anchored D-OPT-JCC-
//     FALLTHROUGH).
//
//   * MIR→LIR `lowerCondBr` passes BOTH successors as BlockRef
//     operands (mirroring the existing `addBr` precedent which
//     records its target as operand[0] AND via the successor
//     pool), so the encoder can wire them via the standard
//     operand-index mechanism.

int main() {
    int i;
    int sum;
    i = 1;
    sum = 0;
    while (i <= 10) {
        sum = sum + i;
        i = i + 1;
    }
    return sum;
}
