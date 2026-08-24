/* D-ASM-SUB-NATIVE-OPERAND-UNUSABLE-IN-INLINE-ASM — a `char` or a `short` used
 * as a GNU extended-asm operand, end to end and BY EXECUTION.
 *
 * ★★★ WHY THIS EXAMPLE EXISTS. Every inline-asm example in this tree binds an
 * `int`, a `long` or a `long long`, and that was not taste — it was the only
 * thing that worked. A `char` operand substitutes at width 8, and no spelling
 * in the AT&T dialect's instruction table could operate there:
 *
 *     char v, r;  __asm__ ("movb %1, %0" : "=r"(r) : "r"(v));
 *     → error[A0008] unknown mnemonic 'movb' — it is not in this dialect's
 *       instruction table
 *
 *     char v, r;  __asm__ ("movl %1, %0" : "=r"(r) : "r"(v));
 *     → error[A0008] 'movl' declares operand width 32, but its register
 *       operands are 8 bits
 *
 * ✔MEASURED at the CLI on x86_64:pe64-x86_64-windows-exec and
 * x86_64:elf64-x86_64-linux-exec, debug and release; `short` gave the same pair
 * at width 16. ✔MEASURED on the reference: gcc 13.3.0 compiles AND RUNS every
 * shape below at -O0 and -O2, exit 42 — and REFUSES the `movl`-on-a-char
 * spelling exactly as DSS does (`Error: operand type mismatch for 'mov'`), so
 * the conformance runs BOTH ways.
 *
 * ★★★ THE SHAPE THIS EXAMPLE EXISTS TO CATCH IS A SILENT ONE, WHICH IS WHY
 * SHAPES 6 AND 7 LOOK ODD. AT&T `movb (%rax), %cl` is `8A /r` — it writes ONLY
 * `cl` and leaves the upper 56 bits of `rcx` ALONE. The target's width-8 `load`
 * row is `0F B6` (MOVZX), which ZEROES them; gas spells that one `movzbl`. Both
 * assemble, both run, and the difference is invisible unless something reads a
 * bit the narrow move did not write. Shapes 6 and 7 seed the destination with a
 * bit ABOVE the written field and require it to survive: a zero-extending
 * encoding returns 42 where the correct one returns 298 / 65578.
 *
 * ★★★ WHY THERE IS NO aarch64 ARM, AND IT IS A DECISION RATHER THAN AN
 * OMISSION. On aarch64 a `char` operand is still refused
 * (`error[A0008] 'mov' produced 1 LIR operand(s) at width 8, and no candidate
 * target opcode encodes that shape`) — that CPU declares no 8- or 16-bit GPR
 * ALU forms at all. Whether it should instead substitute the 64-bit `x` view,
 * as gcc and clang do there, is the subject of
 * `D-ASM-BARE-OPERAND-WIDTH-DIVERGES-FROM-REFERENCE`: an OPERATOR decision
 * whose two arms give OPPOSITE answers for
 * what a `char` operand should do — under one it works for free, under the
 * other it is correctly refused forever. Writing either answer here would be a
 * speculative build. So this example declares x86_64 targets only, and the
 * refusal on aarch64 stays loud and unpinned by this file.
 *
 * ⚠⚠ THIS PARAGRAPH WAS FALSE IN THE COMMIT THAT INTRODUCED IT, AND IT IS KEPT
 * AND CORRECTED RATHER THAN DELETED BECAUSE THE SHAPE IS THE REUSABLE PART. As
 * typed it said `movw $imm, %reg` "IS STILL REFUSED", and the reason it gave
 * was true up to the moment a sibling lane closed the row it cited IN THE SAME
 * COMMIT. The mechanism it describes is worth keeping: `movw $42, %cx` is
 * `66 C7 /0 iw`, a TWO-byte immediate, and the x86-variable encoder used to
 * declare no 2-byte immediate slot, so emitting the 4-byte `imm32` slot instead
 * would have corrupted the instruction stream with no diagnostic — which is why
 * the spelling was fail-loud rather than approximated. That slot now exists
 * (`EncodingSlotKind::Imm16Bytes`; the older `Imm16` belongs to the fixed32
 * shape and could not serve here), `mov` carries the width-16 variant that
 * wires it, and D-ASM-X86-NO-16BIT-IMMEDIATE-SLOT is CLOSED. ✔RE-MEASURED at
 * the CLI, x86_64:pe64-x86_64-windows-exec at debug AND release: `movw $42, %0`
 * into an `"=r"` short, and a hand-written `movw $42, %%cx`, both compile rc=0
 * and RUN. Shape 3 still spells the BYTE form because it is the shape that
 * tests the 1-byte slot; the 16-bit immediate's own byte-exact pins live in
 * `tests/asm/test_asm_x86_width_and_direction.cpp`.
 * ⇒ A CROSS-LANE CITATION OF AN OPEN ROW IS A CLAIM WITH AN EXPIRY DATE, AND
 * THE FOLD IS WHEN IT EXPIRES
 * (D-COMMENT-A-CLAIM-TRUE-WHEN-TYPED-AND-FALSE-WHEN-THE-COMMIT-LANDED).
 *
 * ⚠ THE `volatile` SEEDS ARE LOAD-BEARING: without them the release pipeline
 * folds the values to constants before lowering and silently changes which
 * mechanism each arm tests.
 */

#if !defined(__x86_64__)
#error "c_inline_asm_subnative_operand: x86_64 only, by design — see the header"
#endif

volatile char  dss_c42 = 42;
volatile short dss_s42 = 42;
volatile int   dss_wide = 2130706474;   /* 0x7F00002A: (char)==42, (short)==42 */

char  dss_cmem = 0;
short dss_smem = 0;

int main(void) {
    char  cv;
    char  cr;
    short sv;
    short sr;
    int   witness;

    /* 1 — the plain register-to-register byte move on `char` operands. */
    cv = dss_c42;
    cr = 0;
    __asm__ ("movb %1, %0" : "=r"(cr) : "r"(cv));
    if (cr != 42) return 1;

    /* 2 — the same at 16 bits. */
    sv = dss_s42;
    sr = 0;
    __asm__ ("movw %1, %0" : "=r"(sr) : "r"(sv));
    if (sr != 42) return 2;

    /* 3 — an IMMEDIATE into a byte register (`C6 /0 ib`, the one-byte slot). */
    cr = 0;
    __asm__ ("movb $42, %0" : "=r"(cr));
    if (cr != 42) return 3;

    /* 4 — MEMORY SOURCE. This is `load_subreg`, NOT `load`: `8A /r` leaves the
     *     destination's upper bits alone where `load`'s width-8 row (MOVZX)
     *     would zero them. */
    cv = dss_c42;
    cr = 0;
    __asm__ ("movb %1, %0" : "=r"(cr) : "m"(cv));
    if (cr != 42) return 4;

    sv = dss_s42;
    sr = 0;
    __asm__ ("movw %1, %0" : "=r"(sr) : "m"(sv));
    if (sr != 42) return 5;

    /* 5 — MEMORY DESTINATION on a global, so the address is a relocated symbol
     *     and the store is the byte/word-exact `88 /r` / `66 89 /r`. */
    cv = dss_c42;
    __asm__ ("movb %1, %0" : "=m"(dss_cmem) : "r"(cv));
    if (dss_cmem != 42) return 6;

    sv = dss_s42;
    __asm__ ("movw %1, %0" : "=m"(dss_smem) : "r"(sv));
    if (dss_smem != 42) return 7;

    /* 6 — THE DISCRIMINATOR AT 8 BITS. `%ecx` is seeded with 0x100 and only its
     *     low byte is written; the surviving 0x100 is what says the move was a
     *     byte move and not a zero-extending one. A `movzbl` here returns 42. */
    witness = 0;
    __asm__ ("movl $256, %%ecx\n\tmovl $42, %%eax\n\tmovb %%al, %%cl\n\t"
             "movl %%ecx, %0"
             : "=r"(witness) : : "rax", "rcx");
    if (witness != 298) return 8;

    /* 7 — THE DISCRIMINATOR AT 16 BITS, same construction one field up. */
    witness = 0;
    __asm__ ("movl $65536, %%ecx\n\tmovl $42, %%eax\n\tmovw %%ax, %%cx\n\t"
             "movl %%ecx, %0"
             : "=r"(witness) : : "rax", "rcx");
    if (witness != 65578) return 9;

    /* 8 — THE MATERIALISATION COPY CARRIES THE WHOLE REGISTER, AND THAT IS
     *     SAFE — measured rather than assumed. `MirToLir::emitAsmOperandMove`
     *     emits the copy that puts an operand's value into its bound register
     *     at the target's default width (64), regardless of the operand's own
     *     `widthBits`; the trigger for that could not fire while `char` and
     *     `short` operands were refused. Here the source register provably
     *     holds bits ABOVE the operand's width (0x7F00002A truncated to a
     *     `char`), the copy carries all 64, the template reads the low 8, and
     *     the output capture re-reads the low 8 and sign-extends — so the
     *     upper bits never reach a consumer un-recast. A regression that made
     *     the copy narrow-and-dirty, or that widened what the capture reads,
     *     shows up here as a value other than 42. */
    cv = (char)dss_wide;
    cr = 0;
    __asm__ ("movb %1, %0" : "=r"(cr) : "r"(cv));
    if (cr != 42) return 10;

    sv = (short)dss_wide;
    sr = 0;
    __asm__ ("movw %1, %0" : "=r"(sr) : "r"(sv));
    if (sr != 42) return 11;

    /* 9 — HAND-WRITTEN NARROW REGISTER SPELLINGS, which is what the 16-bit
     *     `registers[]` rows exist for: a bound `%N` never needs them (its
     *     width comes from the binding), but `%ax` written in the text does. */
    witness = 0;
    __asm__ ("movl $0, %%edx\n\tmovl $42, %%eax\n\tmovw %%ax, %%dx\n\t"
             "movb %%al, %%dl\n\tmovl %%edx, %0"
             : "=r"(witness) : : "rax", "rdx");
    if (witness != 42) return 12;

    return 42;
}
