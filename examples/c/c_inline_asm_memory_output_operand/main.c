/* D-ASM-MEMORY-CONSTRAINT-OUTPUT-FORM-NOT-REALIZED — the `"=m"` / `"+m"` memory
 * constraint in the OUTPUT section, end to end and BY EXECUTION.
 *
 * ★★★ WHY THIS EXAMPLE EXISTS. Its sibling `c_inline_asm_memory_operand` pinned
 * the INPUT direction. The OUTPUT direction was refused BY NAME, at every cell:
 *
 *   __asm__("movl %1, %0" : "=m"(*p) : "r"(v));
 *   → error[H0009] … operand 0 (constraint "=m") binds the memory form in the
 *     OUTPUT section — the template writes that memory itself, so there is no
 *     register to capture and no result piece to anchor
 *
 * ✔MEASURED at the CLI on a clean HEAD build, all six runnable cells (pe64
 * x86_64, elf64 x86_64, elf64 aarch64 x debug/release): identical H0009. And
 * ✔MEASURED on gcc 13.3.0, `-O0` AND `-O2`, x86_64-linux-gnu AND
 * aarch64-linux-gnu: `"=m"` and `"+m"` both COMPILE and RUN (exit 42, natively
 * and under qemu-aarch64). One working reference makes the behaviour REQUIRED.
 *
 * ★★★ THE REFUSAL'S REASONING WAS RIGHT AND ITS CONCLUSION WAS NOT. It is true
 * that a memory-form output has no register to capture and no result piece to
 * anchor — but something still has to supply the ADDRESS and the WIDTH, and the
 * INPUT direction already answers both. `"m"` and `"=m"` hand the template the
 * identical thing: a register holding the object's ADDRESS. The DIRECTION lives
 * entirely inside the template's own instruction. So the operand is carried as
 * an address (it produces no result piece, so it is not in the descriptor's
 * result-piece list at all) and nothing new was minted anywhere.
 *
 * ★★★ THE EXIT CODE IS A FUNCTION OF THE OPERAND BEING AN **ADDRESS**, WHICH IS
 * THE ONE PROPERTY A COMPILE-ONLY PIN CANNOT SEE. A regression that lowered the
 * operand's VALUE into the bound register instead of its ADDRESS still compiles
 * rc=0, still emits a memory form, and still assembles — it just writes through
 * a wild pointer. That is a crash or a corrupted neighbour, never a diagnostic,
 * which is why every shape below asserts a RESULT rather than the absence of an
 * error.
 *
 * ★★ THE SHAPES, AND EACH IS A DIFFERENT PATH TO THE SAME BINDING. Each returns
 * its own exit code, so a failure names which path broke instead of collapsing
 * to a single 1.
 *   1  the address arrives as a PARAMETER (a register value, behind a CALL, so
 *      it cannot be folded to a frame offset);
 *   2  the address is a FRAME OFFSET on a local, lowered directly in `main` —
 *      the shape the optimizer can see through, which is why the `release` arm
 *      is mandatory rather than decorative. ★ This statement also has NO result
 *      piece at all: its only output is memory-form, and getting that wrong is
 *      a `MirBuilder::addInlineAsm` process ABORT ("a result type was supplied
 *      but the descriptor declares no outputs"), not a wrong answer;
 *   3  the SYMBOLIC `%[name]` spelling, a second row pointing at the same
 *      register — a form travelling only on the positional row would be red
 *      here alone;
 *   4  a GLOBAL, whose address is a SYMBOL rather than a frame offset;
 *   5  `"+m"` — the read-modify-write spelling, which rides the very same
 *      predicate as `"=m"` and is realized by the very same carriage: for a
 *      memory operand the location the template READS and the one it WRITES are
 *      the same memory named by ONE address register, so `+` asks for nothing
 *      `=` does not already give. ★ It is ALSO the mixed shape: output 0 is
 *      memory-form and the only result piece belongs to output 1;
 *   6  MIXED **WIDTHS** — `"=m"` on a 32-bit object followed by `"=r"` on a
 *      64-bit one, in ONE statement. The result piece's type must come from the
 *      REGISTER output, not from the memory operand that precedes it in the
 *      source's output section; taking the wrong one runs the register operand
 *      at 32 bits and the `movq`/`mov x` spelling stops agreeing with it;
 *   7  `"=&m"` — earlyclobber on the memory form. gcc 13.3.0 compiles it on
 *      both targets (✔MEASURED), and the promise it makes is already true here
 *      by construction: the bound register holds the ADDRESS, is materialised
 *      before the template and is read by it, so its live range covers the
 *      template and overlaps every other operand's — two overlapping ranges
 *      cannot receive one physical register. ✔MEASURED in the emitted code:
 *      `lea 0xb0(%rsp),%r13` / `mov %r15d,0x0(%r13)`, address and input in
 *      different registers;
 *   8  TWO memory-form outputs in ONE statement, written from two different
 *      inputs — so a carriage that shared one address register, or crossed the
 *      two, produces the wrong pair rather than the right one twice.
 *
 * ⚠ THE `volatile` SEEDS ARE LOAD-BEARING, for the sibling's reason: without
 * them the release pipeline folds the values to constants before lowering,
 * which does not make the pin vacuous but does silently change which mechanism
 * each arm tests.
 *
 * ⚠ WHY `long long` FOR THE READ-MODIFY-WRITE PAIR AND FOR THE WIDE OPERAND.
 * `long` is 64-bit under LP64 and 32-bit under LLP64, so one template spelling
 * cannot serve both x86_64 formats; `long long` is 64-bit on all four shipped
 * formats and `int` is 32-bit on all four. ✔MEASURED, not assumed: the `long`
 * spelling compiled on elf64 and failed on pe64 with
 * `error[A0008] 'movq' declares operand width 64, but its register operands are
 * 32 bits`.
 *
 * ⚠ THE x86_64 READ-MODIFY-WRITE ARM WRITES `addq $5, %0` DIRECTLY AS OF
 * 2026-08-23, AND THE PARAGRAPH THAT USED TO SIT HERE EXPLAINED A WORKAROUND
 * THAT NO LONGER EXISTS. It said memory-destination arithmetic was refused
 * (`error[A0008] 'addl' produced 4 LIR operand(s) at width 32, and no candidate
 * target opcode encodes that shape`) so the arm had to load, add in a register
 * and store back. That refusal was a MISSING ROW, not a missing capability:
 * `x86_64.target.json` now declares `add_mem`/`sub_mem`/`and_mem`/`or_mem`/
 * `xor_mem`/`not_mem`/`neg_mem` plus the memory-SOURCE variants on their
 * producers, and the AT&T dialect's arithmetic spellings name both directions
 * (D-TARGET-X86-64-DECLARES-NO-MEMORY-DESTINATION-ARITHMETIC).
 * ⚠ THE aarch64 ARM STILL LOADS, ADDS AND STORES, AND THAT IS NOT A DSS GAP:
 * AArch64 has no memory-destination arithmetic in the ISA — gcc emits the same
 * three instructions there for the same `"+m"` operand (✔MEASURED, -O0 and -O2).
 * ⚠⚠ THIS PARAGRAPH SAID `cmpq`/`cmpl` AGAINST MEMORY REMAIN REFUSED, AND IT
 * WAS FALSE IN ITS OWN COMMIT — corrected 2026-08-24 by the independent
 * step-10 audit. It was written while
 * [[D-ASM-X86-CMP-AGAINST-MEMORY-DIRECTION-IS-UNELECTABLE]] was still open, and
 * the SAME commit closed it: the memory-DIRECTION election axis landed, `cmp`
 * carries both direction variants, and ✔MEASURED at the CLI on
 * `x86_64:pe64-x86_64-windows-exec` BOTH direction spellings build rc=0 and both
 * artifacts RUN rc=42.
 * ★ Worth recording rather than deleting, because no guard can catch this shape:
 * a comment true when the sentence was typed and false when the commit landed
 * still READS as evidence, and this one pointed at a closed row as if it were a
 * live blocker. The original reason it named — that `cmp` produces no value, so
 * nothing separated the memory-destination operand list from the memory-source
 * one — is exactly what that row's closure fixed.
 *
 * ⚠ NO `%w0`-STYLE OPERAND MODIFIER ANYWHERE. A modifier is a narrower VIEW of
 * a bound register and no shipped target declares a width-view vocabulary, so
 * the semantic tier refuses it fail-loud rather than running the access at the
 * wrong width. ✔MEASURED for THIS constraint rather than inherited from the
 * sibling example: `__asm__("str %w1, %0" : "=m"(m) : "r"(s))` on
 * arm64:elf64-aarch64-linux-exec is `error[S0067] … the inline-asm template
 * uses the operand modifier `%w` … no shipped target declares its width-view
 * vocabulary` (D-CSUBSET-INLINE-ASM-OPERANDS). Every operand below is used at
 * its own natural width.
 *
 * ★★★ AND "ITS OWN NATURAL WIDTH" IS EXACTLY THE HALF-FACT — IT IS DSS'S RULE,
 * NOT THE REFERENCES'. The paragraph above records the MODIFIER being
 * unsupported. It does not record that bare `%N` MEANS SOMETHING ELSE here than
 * it means to gcc and clang on this target, and this example is the only one in
 * the tree where that bites: its aarch64 arms bind `int` operands, so 8 of its
 * operand references land on the divergence while the `long long` pair in shape
 * 5 does not. ✔MEASURED, from emitted code on both sides:
 *
 *   DSS   — `%N` substitutes at the OPERAND'S OWN TYPE width, carried by
 *           `MirToLir::widthFlagsForType` → `asmWidthBitsForType` and stamped by
 *           `AsmInstructionLowering::effectiveWidth`. ✔This file's own aarch64
 *           debug artifact carries 8 narrow `str w…, [x…]` forms — one per
 *           `int` operand, each correct for its object — beside the single
 *           wide `str x27, [x28]` belonging to the `long long` shape 5.
 *   gcc 13.3.0 / clang 19, aarch64 — bare `%N` on an `"r"` operand renders the
 *           64-bit `x` name for EVERY integer type. The identical source text
 *           emits `str x1, [x0]` — an 8-BYTE store into that same 4-byte `int`.
 *           ✔The whole file still exits 42 under gcc at -O0 and -O2, which is
 *           stack-layout luck absorbing the overrun, not agreement: the
 *           instruction differs. clang flags all 8 sites under
 *           `-Wasm-operand-widths` with the fix-it *use constraint modifier "w"*.
 *
 * ⇒ THE DIVERGENCE RUNS BOTH WAYS AND IS SILENT IN BOTH. gcc-authored source is
 * not portable here (`%w1` is `S0067`); DSS-authored source is a memory-safety
 * hazard under gcc (the shape above). ✔And DSS's rule can produce the WRONG
 * ANSWER where the reference's is right: `long long m; int v;
 * __asm__("str %1, %0" : "=m"(m) : "r"(v))` stores 4 bytes under DSS and leaves
 * the high half stale (exit 1), and 8 bytes under gcc (exit 42) — same text, no
 * diagnostic anywhere. ⚠ Do NOT "fix" that by widening an operand in this file:
 * every shape here is correct under the rule DSS ships today, and which rule
 * DSS should ship is `D-ASM-BARE-OPERAND-WIDTH-DIVERGES-FROM-REFERENCE`, an
 * operator decision whose (A) arm respells this example.
 */

volatile int       dss_seed  = 42;
volatile int       dss_seed2 = 7;
volatile long long dss_wide  = 4294967297; /* 2^32 + 1 — needs all 64 bits */

int dss_global_out = 0;
int dss_mem_a      = 0;
int dss_mem_b      = 0;

#if defined(__x86_64__)

/* SHAPE 1 — the operand's address arrives as a PARAMETER, so it is a register
 * value rather than a frame offset the optimizer can fold into the form. */
static void dssAsmStoreThroughPointer(int *p, int v) {
    /* `%0` is the MEMORY at the bound register, not the register: the engine
     * writes the dialect's own memory form around it, which is `(%reg)` here
     * and `[reg]` on aarch64 — the vocabulary/grammar split, in one line. */
    __asm__ ("movl %1, %0" : "=m"(*p) : "r"(v));
}

#elif defined(__aarch64__)

static void dssAsmStoreThroughPointer(int *p, int v) {
    __asm__ ("str %1, %0" : "=m"(*p) : "r"(v));
}

#else
#error "c_inline_asm_memory_output_operand: no arm for this architecture — add \
one rather than letting the example pass without exercising a memory output"
#endif

int main(void) {
    int       viaPointer;
    int       viaLocal;
    int       viaSymbolic;
    long long rmw;
    long long scratch;
    int       narrow;
    long long wide;
    int       early;

    viaPointer  = 0;
    viaLocal    = 0;
    viaSymbolic = 0;
    narrow      = 0;
    wide        = 0;
    early       = 0;

    /* 1 — behind a CALL. */
    dssAsmStoreThroughPointer(&viaPointer, dss_seed);
    if (viaPointer != 42) return 1;

    /* 2 — a FRAME OFFSET, and a statement with NO result piece at all. */
#if defined(__x86_64__)
    __asm__ ("movl %1, %0" : "=m"(viaLocal) : "r"(dss_seed));
#else
    __asm__ ("str %1, %0" : "=m"(viaLocal) : "r"(dss_seed));
#endif
    if (viaLocal != 42) return 2;

    /* 3 — the SYMBOLIC spelling on the memory-form output. */
#if defined(__x86_64__)
    __asm__ ("movl %[src], %[dst]" : [dst]"=m"(viaSymbolic) : [src]"r"(dss_seed));
#else
    __asm__ ("str %[src], %[dst]" : [dst]"=m"(viaSymbolic) : [src]"r"(dss_seed));
#endif
    if (viaSymbolic != 42) return 3;

    /* 4 — a GLOBAL: the address is a symbol, not a frame offset. */
#if defined(__x86_64__)
    __asm__ ("movl %1, %0" : "=m"(dss_global_out) : "r"(dss_seed));
#else
    __asm__ ("str %1, %0" : "=m"(dss_global_out) : "r"(dss_seed));
#endif
    if (dss_global_out != 42) return 4;

    /* 5 — `"+m"`: the template READS and WRITES the same memory. The x86_64
     *     arm writes it IN PLACE (`addq $5, %0`), which is the spelling gcc
     *     emits for a `"+m"` operand; the aarch64 arm loads, adds and stores
     *     because that CPU has no memory-destination arithmetic at all. Both
     *     then read the memory form back through `%0` into `%1`, so the
     *     statement still carries a memory-form output with NO result piece
     *     beside a register output that owns the only one. */
    rmw     = dss_seed2;                     /* 7 */
    scratch = 0;
#if defined(__x86_64__)
    __asm__ ("addq $5, %0\n\tmovq %0, %1"
             : "+m"(rmw), "=&r"(scratch));
#else
    __asm__ ("ldr %1, %0\n\tadd %1, %1, #5\n\tstr %1, %0"
             : "+m"(rmw), "=&r"(scratch));
#endif
    if (rmw != 12) return 5;
    if (scratch != 12) return 11;

    /* 6 — MIXED WIDTHS: a 32-bit memory output ahead of a 64-bit register
     *     output, in one statement. */
#if defined(__x86_64__)
    __asm__ ("movl %2, %0\n\tmovq %3, %1"
             : "=m"(narrow), "=r"(wide) : "r"(dss_seed), "r"(dss_wide));
#else
    __asm__ ("str %2, %0\n\tmov %1, %3"
             : "=m"(narrow), "=r"(wide) : "r"(dss_seed), "r"(dss_wide));
#endif
    if (narrow != 42) return 6;
    if (wide != 4294967297) return 7;

    /* 7 — `"=&m"`: earlyclobber on the memory form. */
#if defined(__x86_64__)
    __asm__ ("movl %1, %0" : "=&m"(early) : "r"(dss_seed));
#else
    __asm__ ("str %1, %0" : "=&m"(early) : "r"(dss_seed));
#endif
    if (early != 42) return 8;

    /* 8 — TWO memory-form outputs in one statement, two different values. */
#if defined(__x86_64__)
    __asm__ ("movl %2, %0\n\tmovl %3, %1"
             : "=m"(dss_mem_a), "=m"(dss_mem_b)
             : "r"(dss_seed), "r"(dss_seed2));
#else
    __asm__ ("str %2, %0\n\tstr %3, %1"
             : "=m"(dss_mem_a), "=m"(dss_mem_b)
             : "r"(dss_seed), "r"(dss_seed2));
#endif
    if (dss_mem_a != 42) return 9;
    if (dss_mem_b != 7)  return 10;

    return 42;
}
