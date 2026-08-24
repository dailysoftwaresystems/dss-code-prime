/* D-ASM-X86-CMP-AGAINST-MEMORY-DIRECTION-IS-UNELECTABLE +
 * D-ASM-X86-NO-16BIT-IMMEDIATE-SLOT +
 * D-ASM-X86-WIDTH-EXTENDING-MOVES-UNSPELLABLE — three shapes GNU as assembles
 * and DSS refused, end to end and BY EXECUTION.
 *
 * ★★★ ALL THREE WERE FAIL-LOUD REFUSALS, NEVER MISCOMPILES, AND EACH WAS
 * MEASURED AT THE CLI BEFORE THE FIX on x86_64:pe64-x86_64-windows-exec and
 * x86_64:elf64-x86_64-linux-exec, debug and release:
 *
 *     cmpq %r14, 4096(%r15)   → error[A0008] 'cmpq' produced 4 LIR operand(s)
 *                               at width 64, and no candidate target opcode
 *                               encodes that shape
 *     movw $42, %cx           → error[A0008] 'movw' produced 1 LIR operand(s)
 *                               at width 16, and no candidate target opcode
 *                               encodes that shape
 *     movzbl %cl, %ecx        → error[A0008] unknown mnemonic 'movzbl' — it is
 *                               not in this dialect's instruction table
 *
 * ✔MEASURED on the reference: GNU as 2.42 assembles all three (and 80 further
 * spellings of the same families), and gcc 13.3.0 compiles AND RUNS the shapes
 * below at -O0 and -O2, exit 42.
 *
 * ★★★ WHY EVERY SHAPE ASSERTS A VALUE RATHER THAN COMPILING. Each subject's
 * WRONG answer also assembles and links:
 *   * `cmpq %reg, mem` is `39 /r` and `cmpq mem, %reg` is `3B /r` — the same
 *     four LIR operands, opposite operand orders. Shapes 1 and 2 use the SAME
 *     two numbers with opposite expected outcomes, so a lost direction is a
 *     wrong ANSWER and not a lucky one.
 *   * a 16-bit immediate wired to the 4-byte slot emits two extra bytes and
 *     corrupts everything after it. Shapes 3–6 use values above 0xFF so a
 *     1-byte slot could only truncate, and read the result back.
 *   * `movsbl` (32-bit destination, then bits 63:32 zeroed) and `movsbq`
 *     (64-bit destination) differ in VALUE for every negative byte. Shapes
 *     8–11 sign-extend a NEGATIVE value on purpose; a zero-extending encoding
 *     returns a large positive number instead.
 *
 * ★★★ WHY THERE IS NO aarch64 ARM, AND IT IS A DECISION RATHER THAN AN
 * OMISSION. Every shape below is written in x86 assembly TEXT — a memory
 * destination for a compare, a 2-byte immediate form, the movzx/movsx family.
 * None of the three exists on aarch64 (that CPU has no memory-destination ALU,
 * no immediate-into-memory form, and spells its widening as `sxtb`/`uxtb` with
 * a different operand shape), so an aarch64 arm would be a DIFFERENT program
 * held to the same exit code — which is what the sibling
 * `c_inline_asm_memory_arithmetic` does where a translation exists, and what
 * cannot honestly be done here. The example therefore declares x86_64 targets
 * only.
 *
 * ⚠ `long long` RATHER THAN `long` FOR EVERY 64-BIT SHAPE, AND THAT IS A
 * DATA-MODEL FACT RATHER THAN A STYLE ONE: one of this example's two targets is
 * LLP64, where `long` is 32 bits — a `long` operand there binds a 32-bit
 * register and `movslq %1, %0` is correctly refused (*"widens a 32-bit value
 * into a 64-bit destination, but its destination register is 32 bits"*).
 * ✔MEASURED: the first draft of this file used `long` and was refused on
 * pe64-x86_64-windows while compiling on elf64-x86_64-linux, which is exactly
 * the divergence the shared width check exists to make loud.
 *
 * ⚠ THE `volatile` SEEDS ARE LOAD-BEARING: without them the release pipeline
 * folds the values to constants before lowering and silently changes which
 * mechanism each arm tests.
 */

#if !defined(__x86_64__)
#error "c_inline_asm_width_and_direction: x86_64 only, by design — see the header"
#endif

volatile long long dss_five  = 5;
volatile long long dss_seven = 7;
volatile short dss_s42   = 42;
volatile char  dss_cneg  = -1;
volatile int   dss_ineg  = -1;
volatile short dss_sneg  = -1;

long long dss_lmem = 0;
short dss_smem = 0;
char  dss_cmem = 0;

int main(void) {
    long long m;
    long long r;
    short sr;
    short sv;
    char  cr;
    char  cv;
    int   ir;
    int   iv;
    long long lr;

    /* ── SUBJECT 1: `cmp` against MEMORY, both directions ─────────────────
     * 1 — memory is the DESTINATION: `cmpq %reg, mem` computes `mem - reg`.
     *     5 - 7 < 0, so `jl` is taken. A swapped direction computes 7 - 5 > 0
     *     and falls through to the failure exit. */
    m = dss_five;
    r = dss_seven;
    __asm__ goto ("cmpq %[reg], %[mem]\n\tjl %l[less]"
                  : : [mem] "m"(m), [reg] "r"(r) : "cc" : less);
    return 1;
less:

    /* 2 — memory is the SOURCE: `cmpq mem, %reg` computes `reg - mem`. The
     *     SAME two numbers, the opposite outcome — which is what makes this
     *     pair a direction test rather than two comparisons. */
    m = dss_five;
    r = dss_seven;
    __asm__ goto ("cmpq %[mem], %[reg]\n\tjl %l[wrong]"
                  : : [mem] "m"(m), [reg] "r"(r) : "cc" : wrong);
    goto ok2;
wrong:
    return 2;
ok2:

    /* ── SUBJECT 2: the 2-byte immediate slot ────────────────────────────
     * 3 — `movw $imm, %reg`, the row the anchor was named for. 300 does not
     *     fit one byte, so a 1-byte slot could only truncate it to 44. */
    sr = 0;
    __asm__ ("movw $300, %0" : "=r"(sr));
    if (sr != 300) return 3;

    /* 4 — the 16-bit ALU immediate family the same slot was blocking. */
    sv = dss_s42;
    sr = sv;
    __asm__ ("addw $300, %0" : "+r"(sr));
    if (sr != 342) return 4;

    /*     ★ 0x0100 is chosen so the SET bit lives in the SECOND immediate
     *     byte: a 1-byte slot would drop it entirely and leave 42, and the
     *     answer would look like a plausible no-op rather than a truncation. */
    sr = sv;
    __asm__ ("orw $0x0100, %0" : "+r"(sr));
    if (sr != 298) return 5;

    /* 5 — the 8-bit ALU immediate family, which rides the existing 1-byte
     *     slot and was withheld only so the family would not ship half-done. */
    cv = 40;
    cr = cv;
    __asm__ ("addb $2, %0" : "+r"(cr));
    if (cr != 42) return 6;

    /* 6 — an IMMEDIATE straight into MEMORY at 16 bits (`mov_mem`): `store`
     *     takes a register source, so this had no opcode at all. */
    dss_smem = 0;
    __asm__ ("movw $300, %0" : "=m"(dss_smem));
    if (dss_smem != 300) return 7;

    dss_cmem = 0;
    __asm__ ("movb $42, %0" : "=m"(dss_cmem));
    if (dss_cmem != 42) return 8;

    /* 7 — the narrow memory-DESTINATION arithmetic, at both widths. */
    dss_smem = 42;
    __asm__ ("addw $300, %0" : "+m"(dss_smem));
    if (dss_smem != 342) return 9;

    dss_lmem = 40;
    __asm__ ("addq $2, %0" : "+m"(dss_lmem));
    if (dss_lmem != 42) return 10;

    /* 8 — the three-operand IMUL with an immediate, refused at every width
     *     before this cycle (D-ASM-X86-IMUL-IMMEDIATE-FORM-UNDECLARED). */
    ir = 0;
    iv = 7;
    __asm__ ("imull $6, %1, %0" : "=r"(ir) : "r"(iv));
    if (ir != 42) return 11;

    /* ── SUBJECT 3: the width-extending moves ────────────────────────────
     * 9 — SIGN extension from a byte into a 32-bit destination. The seed is
     *     NEGATIVE on purpose: a zero-extending encoding yields 255. */
    cv = dss_cneg;
    ir = 0;
    __asm__ ("movsbl %1, %0" : "=r"(ir) : "r"(cv));
    if (ir != -1) return 12;

    /* 10 — ZERO extension from the SAME byte. The pair is what separates the
     *      two opcodes: one answer is -1, the other 255, and the input is
     *      identical. */
    cv = dss_cneg;
    ir = 0;
    __asm__ ("movzbl %1, %0" : "=r"(ir) : "r"(cv));
    if (ir != 255) return 13;

    /* 11 — sign extension from a HALFWORD, and its zero-extending twin. */
    sv = dss_sneg;
    ir = 0;
    __asm__ ("movswl %1, %0" : "=r"(ir) : "r"(sv));
    if (ir != -1) return 14;

    sv = dss_sneg;
    ir = 0;
    __asm__ ("movzwl %1, %0" : "=r"(ir) : "r"(sv));
    if (ir != 65535) return 15;

    /* 12 — the 64-bit DESTINATION family. `movslq` is the one gcc emits for
     *      every int-to-long widening; `movsbq`/`movswq` complete it. */
    iv = dss_ineg;
    lr = 0;
    __asm__ ("movslq %1, %0" : "=r"(lr) : "r"(iv));
    if (lr != -1LL) return 16;

    cv = dss_cneg;
    lr = 0;
    __asm__ ("movsbq %1, %0" : "=r"(lr) : "r"(cv));
    if (lr != -1LL) return 17;

    cv = dss_cneg;
    lr = 0;
    __asm__ ("movzbq %1, %0" : "=r"(lr) : "r"(cv));
    if (lr != 255LL) return 18;

    /* 13 — MEMORY SOURCE for an extending move, which is the shape gcc emits
     *      constantly (`movslq (%rax), %rcx`). */
    iv = dss_ineg;
    lr = 0;
    __asm__ ("movslq %1, %0" : "=r"(lr) : "m"(iv));
    if (lr != -1LL) return 19;

    cv = dss_cneg;
    ir = 0;
    __asm__ ("movzbl %1, %0" : "=r"(ir) : "m"(cv));
    if (ir != 255) return 20;

    return 42;
}
