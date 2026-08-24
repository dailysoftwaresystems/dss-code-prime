/* D-ASM-IMMEDIATE-CONSTRAINT-FORM-NOT-REALIZED — the `"i"` IMMEDIATE
 * constraint, end to end and BY EXECUTION.
 *
 * ★★★ WHY THIS EXAMPLE EXISTS. Both shipped targets have always DECLARED the
 * letter — `{ "letter": "i", "binds": "operandKind", "operandKind": "imm32" }`
 * in `arm64.target.json` and `x86_64.target.json` — and the pipeline refused it
 * by name. ✔MEASURED at the CLI, BOTH targets, at debug AND release:
 *
 *   int f(void){ int r=0; __asm__("addl %1, %0" : "+r"(r) : "i"(5)); return r; }
 *   → error[H0009] … operand 1 (constraint "i") binds the operand form 'imm32',
 *     which this pipeline does not yet realize
 *
 * while gcc 13.3.0 compiles the same construct on both — ✔RE-MEASURED for this
 * cycle: `__asm__("nop %1" : "=r"(r) : "i"(7))` emits `nop $7` on x86_64 and
 * `nop 7` on aarch64, and the runnable `add` shape emits `addl $5, %eax` /
 * `add x0, x0, 5`. One working reference makes the behaviour REQUIRED.
 *
 * ★★★ THE EXIT CODE IS A FUNCTION OF THE NUMBER REACHING THE INSTRUCTION, WHICH
 * IS THE ONE PROPERTY A COMPILE-ONLY PIN CANNOT SEE. An immediate operand is
 * the only form that binds NO register: the value is written INTO the
 * instruction. A regression that instead materialized the constant into a
 * register and handed the template that register still compiles rc=0, still
 * assembles, and still emits an `add` — it just adds the wrong thing (on
 * x86_64 the encoding would even be a legal `add reg, reg`). So this example
 * asserts a RESULT, and each shape carries its own arithmetic.
 *
 * ★★ FOUR SHAPES, AND THE LAST TWO ARE THE POINT RATHER THAN DECORATION.
 *   1. behind a CALL, positional `%N` — the value cannot be folded away.
 *   2. a CONSTANT EXPRESSION (`2 + 3`), not a literal: the constraint requires
 *      an INTEGER CONSTANT EXPRESSION, so an implementation that only accepted
 *      a bare literal token would be red here alone. ✔gcc 13.3.0 compiles
 *      `"i"(1+2)` to `nop $3` / `nop 3`, so folding is required, not optional.
 *   3. the SYMBOLIC `%[name]` spelling, which reaches the binding through a
 *      second row pointing at the same operand — an immediate that travelled
 *      only on the positional row would be red on shape 3 alone.
 *   4. `sizeof(int)` — a TYPE QUERY, which folds only if the constant-proof
 *      environment carries the layout engine. ✔gcc: `nop $4` / `nop 4`.
 *
 * ⚠ THE `volatile` SEED IS LOAD-BEARING, for its siblings' reason: without it
 * the release pipeline folds the accumulator to a constant before lowering,
 * which does not make the pins vacuous but does silently change which mechanism
 * each arm tests.
 *
 * ⚠ WHY THE aarch64 ARM USES `long` AND THE 3-OPERAND SPELLING. `add` is
 * suffix-less on this target and the operand REGISTERS are what state the
 * width; `%w0` is an operand MODIFIER no shipped target declares a width-view
 * vocabulary for, and the semantic tier refuses it fail-loud rather than
 * running at the wrong width (D-CSUBSET-INLINE-ASM-OPERANDS). Plain `%0` on
 * 64-bit operands is the shape that is expressible today — the same reason
 * `c_inline_asm_memory_operand` gives for its own `long`.
 */

volatile int dss_seed = 42;

#if defined(__x86_64__)

/* SHAPE 1 — the accumulator arrives as a PARAMETER, behind a call, so neither
 * the value nor the asm block can be folded into the caller. `%1` is the
 * IMMEDIATE: the engine writes the dialect's own immediate form around it,
 * which is `$5` here and a bare `5` on aarch64 — the vocabulary/grammar split,
 * in one line, exactly as `"m"` shows it for addresses. */
static int dssAsmAddImmediate(int a) {
    int r;
    r = a;
    __asm__ ("addl %1, %0" : "+r"(r) : "i"(5));
    return r;
}

#elif defined(__aarch64__)

static long dssAsmAddImmediate(long a) {
    long r;
    r = a;
    __asm__ ("add %0, %0, %1" : "+r"(r) : "i"(5));
    return r;
}

#else
#error "c_inline_asm_immediate_operand: no arm for this architecture — add one \
rather than letting the example pass without exercising an immediate constraint"
#endif

int main(void) {
#if defined(__x86_64__)
    int v;
    int r;

    v = dss_seed;                        /* 42 */
    r = dssAsmAddImmediate(v);
    if (r != 47) return 1;

    /* SHAPE 2 — a CONSTANT EXPRESSION rather than a literal. */
    r = v;
    __asm__ ("addl %1, %0" : "+r"(r) : "i"(2 + 3));
    if (r != 47) return 2;

    /* SHAPE 3 — the SYMBOLIC operand name. */
    r = v;
    __asm__ ("addl %[k], %[acc]" : [acc]"+r"(r) : [k]"i"(7));
    if (r != 49) return 3;

    /* SHAPE 4 — a TYPE QUERY, which folds only through the layout engine. */
    r = v;
    __asm__ ("addl %1, %0" : "+r"(r) : "i"(sizeof(int)));
    if (r != 46) return 4;
#else
    long v;
    long r;

    v = dss_seed;                        /* 42 */
    r = dssAsmAddImmediate(v);
    if (r != 47) return 1;

    r = v;
    __asm__ ("add %0, %0, %1" : "+r"(r) : "i"(2 + 3));
    if (r != 47) return 2;

    r = v;
    __asm__ ("add %[acc], %[acc], %[k]" : [acc]"+r"(r) : [k]"i"(7));
    if (r != 49) return 3;

    r = v;
    __asm__ ("add %0, %0, %1" : "+r"(r) : "i"(sizeof(int)));
    if (r != 46) return 4;
#endif

    return 42;
}
