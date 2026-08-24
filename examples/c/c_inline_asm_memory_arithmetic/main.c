/* D-TARGET-X86-64-DECLARES-NO-MEMORY-DESTINATION-ARITHMETIC — arithmetic whose
 * OPERAND IS MEMORY, in both directions, end to end and BY EXECUTION.
 *
 * ★★★ WHY THIS EXAMPLE EXISTS. Its sibling `c_inline_asm_memory_output_operand`
 * pinned that a `"m"`-form operand can be an OUTPUT. What it could not spell was
 * the instruction gcc actually emits for a `"+m"` operand:
 *
 *     __asm__ ("addq $5, %0" : "+m"(m));
 *     → error[A0008] 'addl' produced 4 LIR operand(s) at width 32, and no
 *       candidate target opcode encodes that shape
 *
 * ✔MEASURED at the CLI before the fix, on x86_64:pe64-x86_64-windows-exec and
 * x86_64:elf64-x86_64-linux-exec, at debug AND release. ✔And MEASURED that it
 * was NOT a property of `"+m"`: the identical refusal came through the
 * already-shipped `"m"` INPUT form (`__asm__("addl $5, %0" : : "m"(x)
 * : "memory")`), which is the control that made it a target-table gap.
 *
 * ★★★ THE REFERENCE DISJUNCTION, MEASURED RATHER THAN ASSUMED (bar §A.3b). GNU
 * as 2.42 was asked, one spelling at a time, which arithmetic mnemonics take a
 * memory operand on x86_64, and gcc 13.3.0 was asked to COMPILE AND RUN every
 * shape below at -O0 and -O2. Results: `add`, `sub`, `and`, `or`, `xor` and
 * `cmp` accept a memory DESTINATION with an immediate or a register source;
 * `not` and `neg` accept one as their single operand; every one of those plus
 * `imul` and `lea` accepts a memory SOURCE; and `imul` has NO memory-
 * destination form at all (its two-operand shape writes a register), which is
 * why no `mul_mem` exists to test. One working reference makes the rest
 * REQUIRED.
 *
 * ⚠⚠ THIS PARAGRAPH WAS FALSE IN THE COMMIT THAT INTRODUCED IT, AND IT IS KEPT
 * AND CORRECTED RATHER THAN DELETED BECAUSE THE SHAPE IS THE REUSABLE PART. As
 * typed it said `cmp` against memory "landed only halfway" and that BOTH
 * register directions "stay refused fail-loud". The DIAGNOSIS was right and is
 * still worth reading: `cmp` produces no value, so its dialect row lists no
 * producer, so the engine offers the SAME candidate set to the memory-
 * destination and the memory-source operand lists — and for the REGISTER form
 * those two lists are byte-identical while meaning `mem - reg` and `reg - mem`,
 * so declaring either direction ALONE would silently encode the other for the
 * opposite spelling. What was wrong was the TENSE. A sibling lane closed
 * D-ASM-X86-CMP-AGAINST-MEMORY-DIRECTION-IS-UNELECTABLE in this same commit by
 * putting the axis on the INSTRUCTION — `kLirInstFlagMemoryIsDestination` plus
 * the `guard.memoryDestination` variant key — which is the one place BOTH the
 * text lowering and the encoder can read it, so the two lists stop being
 * indistinguishable. ✔RE-MEASURED at the CLI, x86_64:pe64-x86_64-windows-exec,
 * debug AND release: `cmpq %reg, mem` and `cmpq mem, %reg` both compile rc=0
 * and both RUN, and a probe written so the two directions give OPPOSITE answers
 * on the same two numbers returns the RIGHT one for each. The IMMEDIATE form
 * never had the twin — a memory-source list always begins with the destination
 * REGISTER, so it can never be `[imm32, …]` — and it is what shape 11 runs; the
 * REGISTER directions are witnessed by execution in the sibling example
 * `c_inline_asm_width_and_direction`, whose shapes 1 and 2 are written so a
 * swapped direction is a wrong answer rather than a lucky one.
 * ⇒ A CROSS-LANE CITATION OF AN OPEN ROW IS A CLAIM WITH AN EXPIRY DATE, AND
 * THE FOLD IS WHEN IT EXPIRES
 * (D-COMMENT-A-CLAIM-TRUE-WHEN-TYPED-AND-FALSE-WHEN-THE-COMMIT-LANDED).
 *
 * ★★★ WHY EVERY SHAPE ASSERTS A VALUE AND NOT AN ABSENCE OF ERRORS. A memory-
 * destination encoding differs from its memory-SOURCE twin only in one opcode
 * byte: `subq %rax, (%r15)` is `29 /r` and computes `mem := mem - rax`, while
 * `subq (%r15), %rax` is `2B /r` and computes `rax := rax - mem`. Both assemble,
 * both run, and swapping them is a wrong ANSWER with no diagnostic anywhere.
 * The non-commutative shapes below (2, 7, 8) are what tell the two apart, and
 * only execution can read that difference.
 *
 * ★★ THE aarch64 ARMS ARE NOT A TRANSLATION OF THE x86_64 ONES — THEY ARE WHAT
 * THAT ISA ACTUALLY DOES. AArch64 has no memory-destination arithmetic at all;
 * gcc emits load / operate / store there for the identical `"+m"` operand
 * (✔MEASURED, -O0 and -O2). So each arm below spells its own machine's answer,
 * and both are held to the same result.
 *
 * ★★ SHAPE 11 IS A COMPARE, AND IT NEEDS A BRANCH TO BE OBSERVABLE. A compare
 * writes only flags, so the only way a corpus example can read one is to act on
 * it — which is what `__asm__ goto` is for. Shape 11 spells the IMMEDIATE form;
 * the REGISTER forms of `cmp` against memory also work as of this commit (see
 * the corrected paragraph above) and are witnessed in
 * `c_inline_asm_width_and_direction` rather than duplicated here. The arm is
 * written so a dropped compare falls through to `return 11` rather than passing.
 *
 * ⚠ EVERY aarch64 ARM USES REGISTER-FORM OPERANDS, INCLUDING FOR THE CONSTANTS.
 * ✔MEASURED in `arm64.target.json`: `add`/`sub` declare an immediate variant
 * only at width 64, and `and`/`orr`/`eor` declare none, so `and %1, %1, #12` on
 * an `int` would be refused. The constants therefore travel as `"r"` inputs.
 *
 * ⚠ THE `volatile` SEEDS ARE LOAD-BEARING, for the sibling examples' reason:
 * without them the release pipeline folds the values to constants before
 * lowering, which does not make the pins vacuous but does silently change which
 * mechanism each arm tests.
 *
 * ⚠ NO `%w0`-STYLE OPERAND MODIFIER ANYWHERE, and no operand whose width the
 * two references would substitute differently — the `int` arms below are
 * x86_64-and-aarch64 safe because each arm names its own registers implicitly
 * through operands of ONE type per statement
 * (D-ASM-BARE-OPERAND-WIDTH-DIVERGES-FROM-REFERENCE is about a MIXED-width
 * statement, and there is none here).
 */

volatile long long dss_seed7  = 7;
volatile long long dss_seed5  = 5;
volatile int       dss_i7     = 7;
volatile int       dss_i12    = 12;
volatile int       dss_i3     = 3;
volatile int       dss_i5     = 5;

long long dss_gmem = 0;

int main(void) {
    long long m;
    long long r;
    long long scratch;
    int       n;
    int       nscratch;

    scratch  = 0;
    nscratch = 0;

    /* 1 — IMMEDIATE into MEMORY, width 64. `addq $5, %0` is the whole point. */
    m = dss_seed7;                                       /* 7 */
#if defined(__x86_64__)
    __asm__ ("addq $5, %0" : "+m"(m));
#elif defined(__aarch64__)
    __asm__ ("ldr %1, %0\n\tadd %1, %1, %2\n\tstr %1, %0"
             : "+m"(m), "=&r"(scratch) : "r"(dss_seed5));
#else
#error "c_inline_asm_memory_arithmetic: no arm for this architecture — add one \
rather than letting the example pass without exercising memory arithmetic"
#endif
    if (m != 12) return 1;

    /* 2 — REGISTER into MEMORY, width 64, and NON-COMMUTATIVE: the memory is
     *     the left-hand side, so 7 - 5 == 2 and a swapped encoding gives -2. */
    m = dss_seed7;                                       /* 7 */
#if defined(__x86_64__)
    __asm__ ("subq %1, %0" : "+m"(m) : "r"(dss_seed5));
#else
    __asm__ ("ldr %1, %0\n\tsub %1, %1, %2\n\tstr %1, %0"
             : "+m"(m), "=&r"(scratch) : "r"(dss_seed5));
#endif
    if (m != 2) return 2;

    /* 3 — IMMEDIATE into MEMORY at WIDTH 32 (`andl`), which is a different
     *     encoding row: the same opcode byte WITHOUT the REX.W prefix. */
    n = dss_i7;                                          /* 7 */
#if defined(__x86_64__)
    __asm__ ("andl $12, %0" : "+m"(n));
#else
    __asm__ ("ldr %1, %0\n\tand %1, %1, %2\n\tstr %1, %0"
             : "+m"(n), "=&r"(nscratch) : "r"(dss_i12));
#endif
    if (n != 4) return 3;

    /* 4 — OR at width 32. */
#if defined(__x86_64__)
    __asm__ ("orl $3, %0" : "+m"(n));
#else
    __asm__ ("ldr %1, %0\n\torr %1, %1, %2\n\tstr %1, %0"
             : "+m"(n), "=&r"(nscratch) : "r"(dss_i3));
#endif
    if (n != 7) return 4;

    /* 5 — XOR at width 32, register source. */
#if defined(__x86_64__)
    __asm__ ("xorl %1, %0" : "+m"(n) : "r"(dss_i5));
#else
    __asm__ ("ldr %1, %0\n\teor %1, %1, %2\n\tstr %1, %0"
             : "+m"(n), "=&r"(nscratch) : "r"(dss_i5));
#endif
    if (n != 2) return 5;

    /* 6 — UNARY against memory: the operand is BOTH the only operand and the
     *     destination, so the lowering builds an address tail with no leading
     *     source at all. */
    m = dss_seed7;                                       /* 7 */
#if defined(__x86_64__)
    __asm__ ("notq %0" : "+m"(m));
#else
    __asm__ ("ldr %1, %0\n\tmvn %1, %1\n\tstr %1, %0"
             : "+m"(m), "=&r"(scratch));
#endif
    if (m != -8) return 6;

    /* 7 — UNARY negate at width 32, also non-commutative in the sense that
     *     matters: a dropped operation leaves 2 and a wrong one leaves 0. */
    n = dss_i5;                                          /* 5 */
#if defined(__x86_64__)
    __asm__ ("negl %0" : "+m"(n));
#else
    __asm__ ("ldr %1, %0\n\tneg %1, %1\n\tstr %1, %0"
             : "+m"(n), "=&r"(nscratch));
#endif
    if (n != -5) return 7;

    /* 8 — THE OTHER DIRECTION: memory as the SOURCE of an arithmetic
     *     instruction whose destination is a register. Non-commutative on
     *     purpose — 5 - 7 == -2 here, where shape 2's memory-destination
     *     subtraction gave +2 from the same two numbers. */
    m = dss_seed7;                                       /* 7 */
    r = dss_seed5;                                       /* 5 */
#if defined(__x86_64__)
    __asm__ ("subq %1, %0" : "+r"(r) : "m"(m));
#else
    __asm__ ("ldr %1, %2\n\tsub %0, %0, %1"
             : "+r"(r), "=&r"(scratch) : "m"(m));
#endif
    if (r != -2) return 8;

    /* 9 — memory SOURCE for a multiply, the one mnemonic that has no
     *     memory-destination form on x86_64. */
    r = dss_seed5;                                       /* 5 */
#if defined(__x86_64__)
    __asm__ ("imulq %1, %0" : "+r"(r) : "m"(m));
#else
    __asm__ ("ldr %1, %2\n\tmul %0, %0, %1"
             : "+r"(r), "=&r"(scratch) : "m"(m));
#endif
    if (r != 35) return 9;

    /* 10 — a GLOBAL: the memory operand's address is a SYMBOL rather than a
     *      frame offset, so the address register is materialised by a
     *      relocated `lea`/`adrp` instead of an sp-relative one. */
    dss_gmem = dss_seed7;                                /* 7 */
#if defined(__x86_64__)
    __asm__ ("addq $5, %0" : "+m"(dss_gmem));
#else
    __asm__ ("ldr %1, %0\n\tadd %1, %1, %2\n\tstr %1, %0"
             : "+m"(dss_gmem), "=&r"(scratch) : "r"(dss_seed5));
#endif
    if (dss_gmem != 12) return 10;

    /* 11 — A COMPARE WHOSE OPERAND IS MEMORY, witnessed by a BRANCH, because
     *      a compare writes only flags and nothing else can read them here.
     *      ⚠ THIS COMMENT ALSO SHIPPED FALSE AND IS CORRECTED IN PLACE: it
     *      said `cmpq %r14, mem` and `cmpq mem, %r14` "stay refused fail-loud"
     *      while citing a row the SAME commit closed. ✔RE-MEASURED — both
     *      compile rc=0 and RUN. What the sentence got right is WHY they were
     *      hard: `cmp` produces no value, so the memory-DESTINATION and
     *      memory-SOURCE operand lists for its REGISTER form are identical
     *      while meaning opposite operand orders. The fix was to stop asking
     *      the operand list and put the direction on the instruction
     *      (`kLirInstFlagMemoryIsDestination` + `guard.memoryDestination`) —
     *      D-ASM-X86-CMP-AGAINST-MEMORY-DIRECTION-IS-UNELECTABLE, now CLOSED.
     *      This arm keeps the IMMEDIATE form, which never had the twin (a
     *      memory-source list always begins with the destination REGISTER and
     *      so can never be `[imm32, …]`); the register directions are
     *      witnessed in `c_inline_asm_width_and_direction`.
     *      ⚠ THE aarch64 ARM IS NOT THE SAME INSTRUCTION AND DOES NOT PRETEND
     *      TO BE: that ISA compares registers only, so the arm loads first —
     *      which is exactly what gcc emits there. */
    m = dss_seed7;                                       /* 7 */
#if defined(__x86_64__)
    __asm__ goto ("cmpq $7, %0\n\tje %l[memeq]" : : "m"(m) : "cc" : memeq);
#else
    __asm__ goto ("ldr %0, %1\n\tcmp %0, %2\n\tb.eq %l[memeq]"
                  : "=&r"(scratch) : "m"(m), "r"(dss_seed7) : "cc" : memeq);
#endif
    return 11;
memeq:

    return 42;
}
