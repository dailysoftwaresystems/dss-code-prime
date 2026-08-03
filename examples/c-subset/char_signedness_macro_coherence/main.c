/* TF-C75 (D-TARGET-CHAR-SIGNEDNESS-PER-PLATFORM) — THE DRIFT GUARD.
 *
 * One fact — "is bare `char` unsigned on this (architecture x object-format)
 * pair?" — has TWO INDEPENDENT OBSERVABLE FACES, and the config declares each
 * of them separately:
 *
 *   1. the CODEGEN face: the resolved signedness drives the SExt-vs-ZExt choice
 *      for the char->int promotion, so it is visible in what the program
 *      COMPUTES.
 *   2. the PREPROCESSOR face: the `__CHAR_UNSIGNED__` predefine, so it is
 *      visible to `#ifdef` before a single instruction is emitted.
 *
 * These are declared SEPARATELY and can therefore DRIFT APART. The macro cannot
 * be DERIVED from the resolved signedness by the engine: deriving it would mean
 * naming `__CHAR_UNSIGNED__` in engine code, a forbidden identity branch (the
 * compiler compares config strings to config strings and never hardcodes a macro
 * name). So the seam is real and permanent, no agnostic engine-side coherence
 * check is possible, and the anti-drift device must be THIS TEST.
 *
 * ★ DELIBERATELY NOT NAMED HERE: the config keys that carry either face. Their
 * shape has already been reworked once and may be reworked again; this example
 * must keep guarding the SEAM across any such rework, so it asserts only the two
 * OBSERVABLES and never the spelling of the keys behind them.
 *
 * ★ HOW IT CANNOT BE MADE VACUOUSLY GREEN. The exit code encodes BOTH
 * observables independently — the macro in the tens digit, the emitted codegen
 * in the units digit — so all four combinations are DISTINCT values and the two
 * drift states are unreachable by any correct configuration:
 *
 *     exit  macro says   codegen does   verdict
 *      10   unsigned     zero-extend    COHERENT (correct on arm64 x elf only)
 *       1   signed       sign-extend    COHERENT (correct on every other leg)
 *       0   signed       zero-extend    DRIFT — macro row lost/mis-gated
 *      11   unsigned     sign-extend    DRIFT — macro row leaked onto a
 *                                       signed-`char` leg
 *
 * 0 and 11 are wrong on EVERY leg, and on any given leg exactly one of {1, 10}
 * is right, so no single-sided edit can restore green: dropping the macro
 * predefine turns the elf leg 10 -> 0, leaking it onto macho turns that leg
 * 1 -> 11, and making the config resolve arm64 x macho back to unsigned turns
 * it 1 -> 0. Each leg pins the macro AND the codegen, so a disagreement between
 * the two faces cannot be made green.
 *
 * ★ THE FIRST TWO OF THOSE WERE MEASURED, NOT ASSUMED. Perturbing the config to
 * ungate the macro predefine made the macho leg fail its `#error` leg loudly
 * (compiler rc=1); with that one `#error` neutered in a scratch copy of this
 * file, the same perturbed config produced EXIT 11 — the drift value predicted
 * above. Deleting the predefine outright made the arm64 x elf leg fail its
 * `#error` leg (compiler rc=1). Both perturbations were reverted.
 *
 * The `volatile` seed keeps the byte a RUNTIME value so no pass can fold
 * `(char)0x80 < 0` at compile time — the units digit is the REAL machine
 * char->int extension (sxtb/ldrsb vs uxtb/ldrb), not a const-fold, in the
 * baseline AND the `release` arm.
 *
 * ★ THE COMPILE-TIME LEGS BELOW ARE NOT REDUNDANT WITH THE EXIT CODE. Only the
 * arm64:macho64 leg actually EXECUTES on a darwin host; the runner compiles the
 * other three and then skips them because their `runOn` excludes host=darwin.
 * Without the `#error` legs, the arm64 x ELF half of this guard — the ONLY leg
 * where `__CHAR_UNSIGNED__` is defined, i.e. the entire point of the macro —
 * would be INERT on the only host that can run it. The `#error` legs are
 * checked at COMPILE time on every leg, so all four are verified on any host.
 *
 * ★ THIS EXAMPLE IS THE ONE SANCTIONED EXCEPTION to the standing constraint
 * recorded in examples/c-subset/arch_identity_predefines/main.c: do not build a
 * runtime ARCHITECTURE cross-check on bare-`char` signedness. That constraint
 * still holds and must not be spread into unrelated examples. This is not that.
 * It is a SELF-CONSISTENCY check between two declarations of ONE fact, not an
 * attempt to identify a machine — and self-consistency is the only way to catch
 * this particular drift. The reason the constraint holds is that `char`
 * signedness is not an architecture property at all: it is (architecture x
 * platform), so any example using it to identify a CPU asserts something untrue
 * and breaks on the next platform added for that CPU.
 *
 * MEASURED BY THIS AGENT 2026-07-28 (Darwin 25.5.0 / arm64, Apple clang 21.0.0 /
 * clang-2100.1.1.101 via /usr/bin/clang; DSS built from this tree), every exit
 * code captured DIRECTLY, never after a pipe:
 *   * `clang -dM -E -x c /dev/null` DEFINES __CHAR_UNSIGNED__ 1 for
 *     -target aarch64-linux-gnu, and does NOT define it for
 *     arm64-apple-darwin, x86_64-unknown-linux-gnu, x86_64-pc-windows-msvc or
 *     x86_64-apple-darwin.
 *   * DSS agrees on all four shipped legs (probed with #ifdef/#ifndef +
 *     #error, compiler exit code captured directly): DEFINED (== 1) on
 *     arm64:elf64-aarch64-linux-exec; NOT DEFINED on
 *     arm64:macho64-arm64-darwin-exec, x86_64:elf64-x86_64-linux-exec and
 *     x86_64:pe64-x86_64-windows-exec.
 *   * THIS file, clang-built and run natively -> exit 1 at -O0 and 1 at -O2.
 *   * THIS file, DSS-built for arm64:macho64-arm64-darwin-exec and run -> exit
 *     1 in the baseline arm and 1 under the `release` arm.
 */

/* ── EXHAUSTIVENESS ───────────────────────────────────────────────────────
 * Identifies which leg is being compiled. Fires if the target's identity
 * macros are not reaching the preprocessor at all, which would silently make
 * every gate below unreachable and this whole guard vacuous. */
#if !defined(__aarch64__) && !defined(__x86_64__)
#  error "char_signedness_macro_coherence: no architecture identity macro is predefined -- the target's predefinedMacros are not reaching the preprocessor, so the legs below cannot be selected"
#endif

#if defined(__aarch64__) && defined(__APPLE__)
/* arm64 x macho — Apple's platform ABI chose SIGNED `char`, so clang does NOT
 * define __CHAR_UNSIGNED__ here (MEASURED), and the config must resolve this
 * pair to signed. Fires if the macro predefine's object-format gate is dropped
 * or inverted and it leaks onto macho. */
#  ifdef __CHAR_UNSIGNED__
#    error "char_signedness_macro_coherence: __CHAR_UNSIGNED__ is defined on arm64 x macho -- bare `char` is SIGNED on arm64-apple-darwin, so the macro predefine's object-format gate has leaked; it must agree with the signedness the target config resolves for this pair"
#  endif

#elif defined(__aarch64__)
/* arm64 x ELF — the AArch64/GNU-Linux platform ABI chose UNSIGNED `char`, and
 * clang DOES define __CHAR_UNSIGNED__ as 1 there (MEASURED). This is the only
 * shipped leg on which the macro exists, and the only leg whose tens digit can
 * ever be 1. Fires if the predefine is missing, mis-gated, or valueless. */
#  ifndef __CHAR_UNSIGNED__
#    error "char_signedness_macro_coherence: __CHAR_UNSIGNED__ is NOT defined on arm64 x elf -- bare `char` IS unsigned on aarch64-linux-gnu, so the macro predefine is missing or its object-format gate is wrong"
#  endif
#  if __CHAR_UNSIGNED__ != 1
#    error "char_signedness_macro_coherence: __CHAR_UNSIGNED__ does not carry the value 1 on arm64 x elf -- the predefine's value is dropped or wrong"
#  endif

#else
/* x86_64, every format — SIGNED `char` on SysV, Darwin and Windows alike, so
 * clang defines __CHAR_UNSIGNED__ on none of them (MEASURED) and no x86_64
 * target declares it. Fires if one is ever added, or added ungated. */
#  ifdef __CHAR_UNSIGNED__
#    error "char_signedness_macro_coherence: __CHAR_UNSIGNED__ is defined on x86_64 -- bare `char` is SIGNED on every x86_64 platform DSS ships, so no x86_64 target may declare this predefine"
#  endif
#endif

int main(void) {
#ifdef __CHAR_UNSIGNED__
    int macroSaysUnsigned = 1;      /* the PREPROCESSOR declaration */
#else
    int macroSaysUnsigned = 0;
#endif
    volatile int seed = 0x80;       /* volatile so no pass can fold it */
    char c = (char)seed;            /* a bare `char` holding the byte 0x80 */

    /* tens digit = what the config SAYS; units digit = what codegen DOES. */
    return macroSaysUnsigned * 10 + (c < 0 ? 1 : 0);
}
