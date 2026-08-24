/* D-ASM-MEMORY-CONSTRAINT-REFUSED-DESPITE-BEING-DECLARED — the `"m"` memory
 * constraint, end to end and BY EXECUTION.
 *
 * ★★★ WHY THIS EXAMPLE EXISTS. Both shipped targets have always DECLARED the
 * letter — `{ "letter": "m", "binds": "operandKind", "operandKind": "membase" }`
 * in `arm64.target.json` and `x86_64.target.json` — and the pipeline refused it
 * anyway, with a diagnostic asserting the letter "was never bound to a
 * processor". ✔MEASURED at the CLI on a clean HEAD worktree, BOTH targets, at
 * debug AND release:
 *
 *   int f(int *p){ int r; __asm__("nop %1" : "=r"(r) : "m"(*p)); return r; }
 *   → error[H0009] … operand 1 (constraint "m") has no resolved register class
 *
 * while gcc 13.3.0 compiles the same source on both — `nop (%rdi)` on x86_64,
 * `nop [x0]` on aarch64. One working reference makes the behaviour REQUIRED,
 * so the pipeline was made to honour what the config already claimed.
 *
 * ★★★ THE EXIT CODE IS A FUNCTION OF THE OPERAND BEING AN **ADDRESS**, WHICH IS
 * THE ONE PROPERTY A COMPILE-ONLY PIN CANNOT SEE. `"r"(*p)` hands the template
 * the VALUE at `p`; `"m"(*p)` hands it the OBJECT, and the machine names an
 * object by its address. A regression that lowered the operand's VALUE into the
 * bound register instead of its ADDRESS still compiles rc=0, still emits a
 * memory form, and still assembles — it just dereferences an `int` as a
 * pointer. That is a wild read, not a diagnostic, which is exactly why this
 * example asserts a RESULT rather than the absence of an error.
 *
 * ★★ TWO SHAPES, AND THAT IS A MEASUREMENT RATHER THAN A STYLE CHOICE — the
 * lesson `c_inline_asm_operands` records in full. One shape is lowered behind a
 * CALL (the operand's address is a parameter, so it cannot be folded to a frame
 * offset) and one is lowered DIRECTLY IN `main` (the address IS a frame offset,
 * the shape the optimizer can see through). The two exercise different
 * addressing paths, and a pin that survives on one arm can die on the other.
 *
 * ⚠ THE `volatile` SEED IS LOAD-BEARING, for its sibling's reason: without it
 * the release pipeline folds the value to a constant before lowering, which
 * does not make the pin vacuous but does silently change which mechanism each
 * arm tests.
 *
 * ⚠ WHY THE aarch64 ARM USES `long`. `ldr %w0, %1` is the natural 32-bit
 * spelling and `%w` is an operand MODIFIER — a narrower VIEW of the bound
 * register — which no shipped target declares a width-view vocabulary for. The
 * semantic tier refuses it fail-loud rather than running the load at the wrong
 * width (`D-CSUBSET-INLINE-ASM-OPERANDS`), so plain `%0` on 64-bit operands is
 * the shape that is expressible today. ✔MEASURED, not assumed: `%w0` is
 * refused, `%0` compiles and runs. When the modifier vocabulary lands, a `%w`
 * arm belongs beside this one.
 *
 * ★★★ AND THAT IS ONLY HALF THE FACT — THE MISSING HALF IS WHY `long` IS THE
 * ONE WIDTH AT WHICH DSS AND THE REFERENCES AGREE ON THIS TARGET. The paragraph
 * above records the MODIFIER being unsupported. It does not record that bare
 * `%N` was given a DIFFERENT MEANING here than gcc and clang give it, which is
 * what makes 64-bit operands not merely "expressible" but the only width whose
 * template text means the same instruction under both. ✔MEASURED — the rule on
 * each side, from emitted code, never from documentation:
 *
 *   DSS   — `%N` substitutes at the OPERAND'S OWN TYPE width, carried from the
 *           C type by `MirToLir::widthFlagsForType` → `asmWidthBitsForType` →
 *           `AsmOperandBinding::widthBits` and stamped on the instruction by
 *           `AsmInstructionLowering::effectiveWidth`. `int` ⇒ a 32-bit form,
 *           `long`/`long long`/pointer ⇒ 64-bit. ⚠ WHAT `char`/`short` DO IS
 *           PER-TARGET, and the qualifier is the load-bearing half: the width
 *           the operand asks for is target-independent, but whether the target
 *           DECLARES an opcode at that width is not. ON aarch64 they are
 *           refused — `error[A0008] … at width 8` / `… width 16` — because that
 *           CPU declares no 8/16-bit GPR ALU forms at all, which is the case
 *           this bullet is contrasting. ON x86_64 they WORK as of this commit
 *           (D-ASM-SUB-NATIVE-OPERAND-UNUSABLE-IN-INLINE-ASM, closed for that
 *           target only); ✔RE-MEASURED at the CLI on
 *           x86_64:pe64-x86_64-windows-exec, a `char` and a `short` operand
 *           each compile rc=0 and RUN, and the sibling example
 *           `c_inline_asm_subnative_operand` is their runtime witness. An
 *           earlier draft of this bullet stated the refusal with NO target
 *           qualifier while the gcc/clang bullet below carried one, which made
 *           a per-target fact read as a universal
 *           (D-COMMENT-A-CLAIM-TRUE-WHEN-TYPED-AND-FALSE-WHEN-THE-COMMIT-LANDED).
 *   gcc 13.3.0 and clang 19, aarch64 — bare `%N` on an `"r"` operand ALWAYS
 *           renders the 64-bit `x` name, for EVERY integer type including
 *           `char`; `%w` is how a 32-bit form is asked for. clang says so
 *           itself: `-Wasm-operand-widths`, *"value size does not match
 *           register size"*, fix-it *use constraint modifier "w"*.
 *
 * ⇒ on a 32-bit operand the SAME template text is a different instruction on
 * each side, silently, with no diagnostic at either end. ✔MEASURED on this
 * example's own sibling shape: `str %1, %0` with `"=m"` on an `int` and `"r"`
 * on an `int` is `str w28, [x29]` under DSS and `str x1, [x0]` under gcc — a
 * 4-byte store against an 8-byte one. `long` here sidesteps that entirely.
 * Cross-target, this is aarch64-shaped: on x86_64 both references substitute at
 * the operand's type width too (`%dil`/`%di`/`%edi`/`%rdi`), so DSS and the
 * references already agree there, and the AT&T mnemonic suffix cross-check
 * catches a disagreement anyway ([[D-ASM-DIALECT-WIDTH-SOURCE-IS-DIALECT-DEPENDENT]]).
 * The fork is `D-ASM-BARE-OPERAND-WIDTH-DIVERGES-FROM-REFERENCE` — an operator
 * decision, because adopting the reference rule respells this example.
 */

volatile int dss_seed = 42;

#if defined(__x86_64__)

/* SHAPE 1 — the operand's address arrives as a PARAMETER, so it is a register
 * value rather than a frame offset the optimizer can fold into the form. */
static int dssAsmLoadThroughPointer(int *p) {
    int r;
    r = 0;
    /* `%1` is the MEMORY at the bound register, not the register: the engine
     * writes the dialect's own memory form around it, which is `(%reg)` here
     * and `[reg]` on aarch64 — the vocabulary/grammar split, in one line. */
    __asm__ ("movl %1, %0" : "=r"(r) : "m"(*p));
    return r;
}

#elif defined(__aarch64__)

static long dssAsmLoadThroughPointer(long *p) {
    long r;
    r = 0;
    __asm__ ("ldr %0, %1" : "=r"(r) : "m"(*p));
    return r;
}

#else
#error "c_inline_asm_memory_operand: no arm for this architecture — add one \
rather than letting the example pass without exercising a memory constraint"
#endif

int main(void) {
#if defined(__x86_64__)
    int v;
    int viaPointer;
    int viaLocal;

    v = dss_seed;                              /* 42 */
    viaPointer = dssAsmLoadThroughPointer(&v);
    if (viaPointer != 42) return 1;

    /* SHAPE 2 — lowered HERE, on a local whose address is a frame offset. */
    viaLocal = 0;
    __asm__ ("movl %1, %0" : "=r"(viaLocal) : "m"(v));
    if (viaLocal != 42) return 2;

    /* SHAPE 3 — the SYMBOLIC operand name, which reaches the same binding
     * through the `%[name]` spelling rather than the positional one. The two
     * spellings are separate rows pointing at one register, so a memory form
     * that only travelled on the positional row would be red here alone. */
    viaLocal = 0;
    __asm__ ("movl %[src], %[dst]" : [dst]"=r"(viaLocal) : [src]"m"(v));
    if (viaLocal != 42) return 3;
#else
    long v;
    long viaPointer;
    long viaLocal;

    v = dss_seed;                              /* 42 */
    viaPointer = dssAsmLoadThroughPointer(&v);
    if (viaPointer != 42) return 1;

    viaLocal = 0;
    __asm__ ("ldr %0, %1" : "=r"(viaLocal) : "m"(v));
    if (viaLocal != 42) return 2;

    viaLocal = 0;
    __asm__ ("ldr %[dst], %[src]" : [dst]"=r"(viaLocal) : [src]"m"(v));
    if (viaLocal != 42) return 3;
#endif

    return 42;
}
