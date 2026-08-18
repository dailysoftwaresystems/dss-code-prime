/* Inline-asm P5, the CROSS-CU half (D-MIR-MERGE-INLINE-ASM-CLONE-ARM-MISSING).
 *
 * ★★★ WHAT THIS EXAMPLE EXISTS TO COVER, AND WHY ITS SIBLING DID NOT.
 * `c_inline_asm_extended` is a ONE-source example, and a one-source build never
 * calls `mergeCuMirs` — `mir/merge/mir_merge.hpp` states the driver rule
 * outright: the byte-identical single-CU path is kept for N==1 and the merge
 * runs only for N>=2. So every inline-asm example in the corpus was compiled by
 * the path that has no cross-CU clone in it, and the cross-CU clone shipped with
 * NO `InlineAsm` arm at all. This example is the second source file.
 *
 * ✔MEASURED 2026-08-17, and this is the defect it pins: a 2-TU build carrying
 * any `__asm__` block ABORTED the compiler —
 *     dss::MirBuilder fatal: addInst: opcode 'inlineasm' has a dedicated builder
 * — at BOTH `--config=debug` and `--config=release`, with no diagnostic and no
 * artifact. It killed four of five sqlite legs the moment `--scanstatus` pulled
 * in `src/hwtime.h`, whose non-Windows arms are exactly the construct below.
 * The abort was the `addInst` refusal doing its job: the cross-CU cloner had
 * forwarded a descriptor-pool INDEX into a module whose pool is empty, and the
 * refusal turned what would have been a silently dropped clobber list into a
 * loud stop.
 *
 * ★★ THE COVERAGE LESSON, WHICH IS THE REAL REASON THIS FILE IS HERE. 874 green
 * tests saw none of it, because coverage is as wide as the SHAPES the corpus
 * names, never the test COUNT — and no example had ever put an `__asm__` and a
 * second translation unit in the same build. Adding an arm to the cloner without
 * adding this example would leave that hole exactly as wide as it was.
 *
 * STRUCTURE: the asm block lives HERE; `dssOp` and `dss_seed` live in
 * `helper.c`. The merge is therefore doing real work on this function — it
 * rewires the cross-CU calls to DIRECT intra-module calls in the same clone walk
 * that has to re-add the asm descriptor — and the values held across the asm
 * block are CALL-DERIVED, which is what makes the aarch64 clobber pin below bite.
 */

extern int dssOp(int k);

#if defined(__x86_64__)

/* sqlite `src/hwtime.h`'s x86_64 arm, verbatim in shape: two register-PINNED
 * outputs that must be materialised out of eax/edx. This is the construct that
 * aborted the 2-TU build. */
static unsigned long long dssHwtime(void) {
    unsigned int lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((unsigned long long)hi << 32) | lo;
}

int dssAsmProbe(void) {
    unsigned long long t1 = dssHwtime();
    /* Cross-CU calls BETWEEN the two counter reads, so the asm blocks and the
     * merged-away call boundary are interleaved in one function. */
    int spin = 0;
    for (int i = 0; i < 8; ++i) spin += dssOp(i);
    unsigned long long t2 = dssHwtime();
    /* MONOTONE-OR-EQUAL is the only value-independent property a counter has —
     * the exit code stays STRUCTURE-dependent, never value-dependent. */
    if (t2 < t1) return 0;
    return (spin == 52) ? 1 : 0;   /* 8*3 + (0+1+...+7) == 24 + 28 == 52 */
}

#elif defined(__aarch64__)

/* ★★★ THE NEGATIVE MISCOMPILE PIN, CARRIED ACROSS THE MERGE. Eight values
 * obtained from CROSS-CU calls are held live across a template that zeroes the
 * eight registers this allocator actually hands out first, and the block
 * DECLARES those registers in its clobber list. Every value must survive.
 *
 * This is the sibling example's pin re-aimed at the cloner that had no arm: it
 * is red not only if the descriptor is DROPPED (which aborts) but also if a
 * future cloner copies the descriptor FIELD BY FIELD and omits `clobbers` —
 * which would compile clean and miscompile silently. That is why the arm passes
 * the descriptor WHOLE by value in `mir_merge.cpp`, and why this pin is the
 * thing that would notice if someone stopped.
 *
 * ✔MEASURED 2026-08-17 ON THIS EXAMPLE — not inherited from the sibling. With
 * the clobber list deleted and everything else byte-identical, built for
 * `arm64:elf64-aarch64-linux-exec` and run under qemu-aarch64:
 *     debug    base exit 42   mutant exit 42   (does NOT discriminate)
 *     release  base exit 42   mutant exit  1   (DISCRIMINATES)
 * Both mutant arms COMPILE rc=0, so this is a silent wrong-answer pin, not a
 * compile-refusal one. Debug cannot see it because locals are memory-resident
 * before mem2reg — which is exactly why `expected.json`'s `release` arm is
 * mandatory here rather than decorative.
 *
 * ⚠ THE REGISTER LIST IS NOT ARBITRARY AND MUST NOT BE "TIDIED". Clobbering
 * x0..x2 is VACUOUS on this allocator: it assigns from the high end (x28
 * downward), so the low registers hold nothing live and the mutant stays green.
 * A clobber pin has to name the registers the allocator would otherwise USE. */
int dssAsmProbe(void) {
    int v0 = dssOp(0); int v1 = dssOp(1); int v2 = dssOp(2); int v3 = dssOp(3);
    int v4 = dssOp(4); int v5 = dssOp(5); int v6 = dssOp(6); int v7 = dssOp(7);
    __asm__ __volatile__ (
        "mov x21, xzr\n\tmov x22, xzr\n\tmov x23, xzr\n\tmov x24, xzr\n\t"
        "mov x25, xzr\n\tmov x26, xzr\n\tmov x27, xzr\n\tmov x28, xzr"
        : : : "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28");
    /* 8*3 + (0+1+...+7) == 24 + 28 == 52 */
    return (v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 == 52) ? 1 : 0;
}

#else
#error "c_inline_asm_crosscu_merge: no arm for this architecture — add one \
rather than letting the example pass without exercising anything"
#endif

int main(void) {
    return dssAsmProbe() ? 42 : 1;
}
