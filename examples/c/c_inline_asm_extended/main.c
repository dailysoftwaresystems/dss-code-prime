/* Inline-asm P5 — GNU EXTENDED `__asm__` with real operands, lowered through
 * the MIR→LIR expansion, on every architecture this corpus runs.
 *
 * ★★★ WHAT THIS EXAMPLE WITNESSES THAT NO EARLIER ONE DID. `c_inline_asm`
 * proves the EMPTY template survives every spelling; it cannot say anything
 * about code generation, because an empty template emits nothing and an
 * expander that emitted nothing would pass it identically. This example needs
 * the expansion to have actually happened:
 *
 *   * the x86_64 arm reads the hardware time-stamp counter through the exact
 *     construct sqlite's `src/hwtime.h` uses —
 *     `__asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi))` — so the two
 *     register-PINNED outputs must be materialised out of eax/edx and joined;
 *   * the aarch64 arm holds eight call-derived values live across a template
 *     that OVERWRITES the eight registers the allocator actually hands out,
 *     declared through the block's clobber list.
 *
 * ★★ THE EXIT CODE IS A FUNCTION OF THE BLOCK'S STRUCTURE, NEVER OF THE
 * COUNTER'S VALUE, and that distinction is the whole test design. A counter
 * read is non-deterministic by definition — asserting anything about WHAT it
 * returns would make this example flake. What IS deterministic is that a
 * counter is MONOTONE: two reads separated by work satisfy `t2 >= t1` on every
 * conforming machine, and nothing else about them is asserted.
 *
 * ⚠ THE `volatile` SEED IS LOAD-BEARING, NOT DECORATION. Without it the
 * optimizer folds `op(k)` to a constant, nothing is live across the asm block,
 * and the aarch64 arm's clobber assertion becomes vacuous — the same
 * "both arms look identical because the compiler scheduled around it" failure
 * that made the FIRST version of this project's reference-compiler clobber
 * probe worthless (plan 29 §2a).
 *
 * ★ WHY THE aarch64 ARM DOES NOT READ `cntvct_el0`. It would need
 * `mrs %0, cntvct_el0`, and ✔MEASURED 2026-08-15 NEITHER shipped dialect
 * declares the `%N` operand placeholder at all: `asm-x86_64-att.lang.json`
 * binds `%` to `RegisterSigil` and spells a register `[RegisterSigil,
 * Identifier]`, so `%0` lexes as sigil + IntLiteral and matches no operand
 * form; `asm-arm64-gas.lang.json` spells `%` as `TypeSigil` and is otherwise
 * sigil-less. Until a dialect declares the placeholder, an aarch64 output can
 * only be reached by a register-pinning constraint letter, which the AArch64
 * constraint vocabulary does not have. The clobber arm is what this
 * architecture CAN witness today, and it witnesses more than a counter read
 * would: it is red when the constraint is ignored.
 */

#if defined(__x86_64__)

/* sqlite `src/hwtime.h`'s x86_64 arm, verbatim in shape. */
static unsigned long long dssHwtime(void) {
    unsigned int lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((unsigned long long)hi << 32) | lo;
}

volatile int dss_seed = 3;

int dssAsmProbe(void) {
    unsigned long long t1 = dssHwtime();
    /* Work between the two reads, kept un-foldable by the volatile seed. */
    int spin = 0;
    for (int i = 0; i < 64; ++i) spin += dss_seed;
    unsigned long long t2 = dssHwtime();
    /* MONOTONE-OR-EQUAL is the only value-independent property a counter has. */
    if (t2 < t1) return 0;
    return (spin == 64 * 3) ? 1 : 0;
}

#elif defined(__aarch64__)

volatile int dss_seed = 3;

int dssOp(int k);

/* ★★★ THE NEGATIVE MISCOMPILE PIN. Eight values obtained from CALLS are held
 * live across a template that zeroes the eight registers this target's
 * allocator actually hands out first, and the block DECLARES those registers
 * in its clobber list. Every value must survive.
 *
 * ✔MEASURED 2026-08-15 that this is not vacuous: with the clobber list deleted
 * and everything else byte-identical, the `release` arm returns 0 instead of
 * 52 — every held value destroyed. Debug does NOT discriminate (locals are
 * memory-resident before mem2reg), which is exactly why the `release` arm in
 * `expected.json` is mandatory rather than decorative.
 *
 * ⚠ THE REGISTER LIST IS NOT ARBITRARY AND MUST NOT BE "TIDIED". Clobbering
 * x0..x2 — the obvious choice — is VACUOUS on this allocator: it assigns from
 * the high end (x28 downward), so the low registers hold nothing live and the
 * mutant stays green. A clobber pin has to name the registers the allocator
 * would otherwise USE, or it tests nothing.
 */
int dssAsmProbe(void) {
    int v0 = dssOp(0); int v1 = dssOp(1); int v2 = dssOp(2); int v3 = dssOp(3);
    int v4 = dssOp(4); int v5 = dssOp(5); int v6 = dssOp(6); int v7 = dssOp(7);
    __asm__ __volatile__ (
        "mov x21, xzr\n\tmov x22, xzr\n\tmov x23, xzr\n\tmov x24, xzr\n\t"
        "mov x25, xzr\n\tmov x26, xzr\n\tmov x27, xzr\n\tmov x28, xzr"
        : : : "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28");
    /* 8*3 + (0+1+..+7) == 24 + 28 == 52 */
    return (v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 == 52) ? 1 : 0;
}

int dssOp(int k) { return dss_seed + k; }

#else
#error "c_inline_asm_extended: no arm for this architecture — add one rather \
than letting the example pass without exercising anything"
#endif

int main(void) {
    return dssAsmProbe() ? 42 : 1;
}
