/* D-CSUBSET-ASM-OUTPUT-ON-A-PARAMETER-NOT-ADDRESS-TAKEN — an inline-asm OUTPUT
 * operand bound to a FUNCTION PARAMETER whose address is never taken.
 *
 * ★★★ WHY THIS EXAMPLE EXISTS, AND WHY NO SIBLING COULD REPLACE IT. Every other
 * inline-asm example in the corpus binds its outputs to BODY LOCALS, and a body
 * local is slot-backed from birth (its `VarDecl` allocates one). A PARAMETER is
 * not: unless something marks it address-taken, HIR→MIR keeps it as a pure-SSA
 * `Arg` value with no storage at all. An asm output is reached through
 * `lowerLvalueAddress` — it needs an address — so the two cases are lowered by
 * genuinely different paths and the corpus was exercising only the easy one.
 *
 *   ✔MEASURED 2026-08-17 through the real CLI, before the fix:
 *     static int f(int v){ __asm__("movl $42,%0" : "=r"(v)); return v; }
 *   was REFUSED at BOTH configs with
 *     error[H0009] symbol 86 has no storage slot (non-addressable param or
 *     unbound) — required by lvalue use
 *   while the SAME function with `&v` also passed to a helper compiled and
 *   exited 42. gcc 13.3.0 compiles and runs both. So this was valid C that DSS
 *   rejected, and the control named the cause exactly: the address-taken set,
 *   not the constraint letter (a plain `"=r"` and a `"+r"` failed identically).
 *
 * ★★ THE EXIT CODE PROVES THE WRITE-BACK HIT **THE PARAMETER**, NOT A COPY.
 * That is the whole point of shape 1 and it is not decoration. The obvious wrong
 * fix for the defect above is to copy the parameter into a fresh local and let
 * the asm write THAT — which compiles, runs, and is a silent miscompile: the
 * store-back would land in an object the source never named. `bumpParam` reads
 * `v` as an INPUT, writes `v` as an OUTPUT, and then `return v` reads it again.
 * If the output and the input are not the same object, the function returns the
 * incoming 22 instead of 42 and the example is RED.
 *
 * ★ TWO SHAPES, DELIBERATELY. Shape 1 has ONE output; shape 2 has TWO, both
 * parameters. The second is what exercises the OUTPUT LOOP — a fix that marked
 * only output 0 addressable passes shape 1 and fails shape 2, and nothing in
 * shape 1 can distinguish the two. (The sibling `c_inline_asm_operands` carries
 * two shapes for a different, allocator-luck reason; this one's second shape is
 * about operand COUNT.)
 *
 * ⚠ THE `volatile` SEED IS LOAD-BEARING, for the same reason it is in the
 * sibling: without it the release pipeline folds the arguments to constants
 * before lowering and both calls stop being parameter-shaped at all.
 *
 * ⚠ NO `"+r"` SHAPE HERE, AND THAT IS A MEASUREMENT. `"+r"(v)` on a parameter
 * reaches exactly the same verdict as `"+r"(v)` on an address-taken local —
 * ✔MEASURED 2026-08-17, both are refused by `D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE`
 * at the LIR tier, which is a SEPARATE open row. Using it here would make this
 * example red for a reason that has nothing to do with what it pins.
 */

volatile int dss_seed = 22;

#if defined(__x86_64__)

/* SHAPE 1 — one output, and it is the same object as the input.
 * `v` is a parameter; its address is never taken anywhere in this file. */
static int bumpParam(int v, int k) {
    /* `"=&r"` — earlyclobber. The template writes %0 and still needs %1 and %2
     * afterwards, so the output must not share a register with either input. */
    __asm__ ("movl %1, %0\n\taddl %2, %0"
             : "=&r"(v)
             : "r"(v), "r"(k)
             : "cc");
    return v;
}

/* SHAPE 2 — TWO outputs, both parameters. Exercises the output LOOP. */
static int twoParamOutputs(int a, int b) {
    __asm__ ("movl $40, %0\n\tmovl $2, %1" : "=r"(a), "=r"(b));
    return a + b;
}

#elif defined(__aarch64__)

/* ★ WHY THE OPERANDS ARE `long` AND CARRY NO WIDTH SUFFIX — the same reason the
 * sibling `c_inline_asm_operands` documents: the natural 32-bit spelling needs
 * the `%w` operand MODIFIER, no shipped target declares a width-view vocabulary,
 * and the semantic tier refuses `%w` fail-loud rather than running the operation
 * at the wrong width (D-CSUBSET-INLINE-ASM-OPERANDS). 64-bit operands with plain
 * `%0` are the shape that is expressible today. */
static long bumpParam(long v, long k) {
    __asm__ ("add %0, %1, %2" : "=&r"(v) : "r"(v), "r"(k));
    return v;
}

static long twoParamOutputs(long a, long b) {
    __asm__ ("mov %0, #40\n\tmov %1, #2" : "=r"(a), "=r"(b));
    return a + b;
}

#else
#error "asm_output_on_parameter: no arm for this architecture — add one rather \
than letting the example pass without binding an asm output to a parameter"
#endif

int main(void) {
    int s;
    s = dss_seed;              /* 22, opaque to the optimizer */

    /* 22 + 20 == 42, and only if the asm read and wrote THE PARAMETER. */
    if ((int)bumpParam(s, 20) != 42) return 1;

    /* 40 + 2 == 42, and only if BOTH parameter outputs were written back. */
    if ((int)twoParamOutputs(s, s) != 42) return 2;

    return 42;
}
