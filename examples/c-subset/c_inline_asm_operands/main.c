/* Inline-asm P5c — a GNU extended `__asm__` whose result depends on the value
 * of an INPUT operand.
 *
 * ★★★ WHY THIS EXAMPLE EXISTS, AND WHY ITS TWO SIBLINGS COULD NOT REPLACE IT.
 * `c_inline_asm` covers the EMPTY template. `c_inline_asm_extended` covers
 * register-PINNED OUTPUTS (`rdtsc` → `"=a"`/`"=d"`) on x86_64 and a pure
 * CLOBBER list on aarch64. Between them the corpus had ZERO end-to-end
 * coverage of an asm INPUT — and the input path was silently broken:
 *
 *   ✔MEASURED 2026-08-17, before the fix, on a compiler that passed 873/873:
 *   `__asm__("movl %1, %0" : "=r"(r) : "r"(a))` with a == 42 compiled rc=0
 *   and the program returned 0, on BOTH `pe64-x86_64` and `elf64-x86_64`, at
 *   BOTH debug and release. `mir_to_lir.cpp` materialised only the PINNED
 *   inputs into their bound registers and skipped the unpinned ones, so the
 *   template read a vreg nothing had ever written. The disassembly named the
 *   defect exactly: the input's load defined the register the OUTPUT had been
 *   allocated, and the template's `%1` read an untouched one —
 *   `mov %r15d,%r14d` where r15 held nothing.
 *
 * ⇒ the general lesson, which is the reason this file is worth its size: a
 * feature's corpus coverage is only as wide as the OPERAND SHAPES it names.
 * Three examples of inline asm proved nothing about inputs, because none of
 * the three HAD one. Count the shapes, not the examples.
 *
 * ★★ THE EXIT CODE IS A FUNCTION OF THE INPUT VALUES, WHICH IS THE WHOLE
 * POINT. The sibling deliberately asserts a structure-only property, because a
 * counter read has no value-independent one. Here the opposite is required: if
 * the inputs do not reach the template, the sum is not 42 and the example is
 * RED.
 *
 * ★★★ WHY THIS FILE CARRIES TWO SHAPES, AND WHY THAT IS A MEASUREMENT RATHER
 * THAN A STYLE CHOICE. Reading an UNDEFINED register is undefined at every
 * optimization level, so the obvious expectation is that any input-shaped pin
 * goes red on every arm. ✘ THAT EXPECTATION IS WRONG, and it was wrong here:
 * with the mutant in place (the `pinned` guard restored in `expandInlineAsm`)
 * and the whole example reduced to `dssAsmAdd` alone, ✔MEASURED
 *
 *   baseline → exit 42 (GREEN, the mutant survives)
 *   release  → exit  1 (RED)
 *
 * because at debug the operands are memory-resident and the allocator happened
 * to leave the right value in the register the template read. Adding the
 * single-input shape lowered DIRECTLY IN `main` flipped it: ✔MEASURED the same
 * mutant now fails the BASELINE arm outright (`baseline exit-code mismatch
 * (expected=42; OS=1)`).
 *
 * ⇒ ★★ A PIN THAT SURVIVES BECAUSE AN UNDEFINED REGISTER HAPPENED TO HOLD THE
 * RIGHT VALUE IS NOT A PIN, AND WHICH SHAPE GETS THAT LUCK CANNOT BE READ OFF
 * THE SOURCE. Carrying both — one behind a call, one lowered in place — is
 * what makes red-on-disable independent of the allocator's mood on any single
 * arm. ⛔ Do not "simplify" this file down to one shape; the two exit codes
 * above are the reason both exist.
 *
 * ⚠ THE `volatile` SEED IS LOAD-BEARING. Without it the release pipeline folds
 * `a` and `b` to constants before lowering. That does NOT make the pin vacuous
 * (a constant still has to be materialised into the bound register), but it
 * does change WHICH mechanism is under test, and an example whose subject
 * silently changes between arms is not a pin.
 */

volatile int dss_seed = 20;

#if defined(__x86_64__)

/* ★ `"=&r"` — EARLYCLOBBER IS REQUIRED HERE AND IS NOT DECORATION. The
 * template writes `%0` in its first instruction and reads `%2` in its second,
 * so an allocator that shared the output's register with an input would
 * destroy that input before it was read. Both reference compilers share
 * registers between a plain `"=r"` output and an input, and so does this
 * allocator — which is what makes the `&` a correctness requirement rather
 * than a hint. (`tests/lir/test_lir_earlyclobber.cpp` pins the LIR half.) */
static int dssAsmAdd(int a, int b) {
    int r;
    r = 0;
    __asm__ ("movl %1, %0\n\taddl %2, %0"
             : "=&r"(r)
             : "r"(a), "r"(b)
             : "cc");
    return r;
}

#elif defined(__aarch64__)

/* ★ WHY THE OPERANDS ARE `long` AND THE MNEMONIC CARRIES NO WIDTH SUFFIX.
 * The natural aarch64 spelling for 32-bit values is `add %w0, %w1, %w2`, and
 * `%w` is an operand MODIFIER — a narrower VIEW of the bound register. No
 * shipped target declares a width-view vocabulary, so the semantic tier
 * REFUSES `%w` with a diagnostic naming the construct rather than silently
 * running the operation at the wrong width (`D-CSUBSET-INLINE-ASM-OPERANDS`).
 * ✔MEASURED 2026-08-17: `%w0` is refused, plain `%0` on 64-bit operands
 * compiles and runs. Widening the operands is therefore the honest way to
 * witness inputs on this architecture today — not a workaround for a defect,
 * but the shape that is actually expressible. When the modifier vocabulary
 * lands, the `%w` arm belongs here beside this one. */
static int dssAsmAdd(int a, int b) {
    long x; long y; long r;
    x = a;
    y = b;
    r = 0;
    __asm__ ("add %0, %1, %2" : "=r"(r) : "r"(x), "r"(y));
    return (int)r;
}

#else
#error "c_inline_asm_operands: no arm for this architecture — add one rather \
than letting the example pass without exercising an asm input operand"
#endif

/* SHAPE 2 lives inside `main` — see the header block for the two mutant exit
 * codes that made a second shape necessary. It is inline rather than behind a
 * helper ON PURPOSE: routing it through a call is what let the mutant survive
 * the baseline arm. */
int main(void) {
    int a;
    int b;
    int r;
#if defined(__x86_64__)
    int m;
#else
    long mx;
    long m;
#endif
    a = dss_seed;       /* 20 */
    b = dss_seed + 2;   /* 22 */

    /* Shape 2, lowered HERE rather than behind a call. */
#if defined(__x86_64__)
    m = 0;
    __asm__ ("movl %1, %0" : "=r"(m) : "r"(a));
    if (m != 20) return 1;
#else
    mx = a;
    m  = 0;
    __asm__ ("mov %0, %1" : "=r"(m) : "r"(mx));
    if ((int)m != 20) return 1;
#endif

    r = dssAsmAdd(a, b);
    /* 20 + 22 == 42. Any input that failed to reach the template makes this
     * comparison false, and the example exits 1. */
    return (r == 42) ? 42 : 1;
}
