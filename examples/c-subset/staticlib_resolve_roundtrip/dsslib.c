/* D-EXAMPLES-RUNNER-MULTI-ARTIFACT (c171): the LIBRARY half of the
 * multi-artifact corpus round-trip. The runner builds THIS into a static
 * archive (a `.lib` on pe64 / a `.a` on elf64 via the c171-D `-staticlib`
 * format), then builds `main.c` resolving `dss_lib_answer` against it via
 * `--resolve-library` -> a self-contained exec -> exit 42.
 *
 * D-EXAMPLES-DEPENDSON-NO-RELEASE-OPTIMIZER-ARM -- WHY THIS BODY IS NOT
 * `return 42;`, AND WHY SIMPLIFYING IT BACK WOULD SILENTLY DISARM A TEST.
 * `expected.json` now declares a `release` arm with
 * `mustDifferFromBaseline: true`, which reds unless the optimized executable
 * differs byte-wise from the baseline one. The baseline pipeline is a bare
 * `Identity`, i.e. a no-op, so a prerequisite library whose only function is a
 * single `return 42;` emits the SAME bytes under `debug` and under `release`,
 * the exec that pulls that member is identical either way, and the arm would
 * have nothing to assert. `dss_lib_answer` therefore carries real work for the
 * shipped release pipeline -- an inlinable helper (well under its
 * `inlineThreshold`), a loop-invariant addend, and locals for Mem2Reg to
 * promote out of their stack slots -- while still returning exactly 42,
 * because the example's `exitCode` contract must not move. `main.c` is kept
 * thin on purpose so that any baseline-vs-release difference in the linked
 * image is attributable to THIS member.
 *
 * !! D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM -- THE
 * !! HELPER BELOW IS DELIBERATELY *NOT* `static`. DO NOT PUT THE KEYWORD BACK
 * !! AS A TIDY-UP. Mechanism: a relocation to a FILE-LOCAL symbol inside an
 * !! archive member is not resolvable by the COFF and Mach-O readers today, so
 * !! with `static` this member links only on elf64 -- pe64 and macho64-arm64
 * !! red with `error[K_SymbolUndefined] ... relocation in CU #N targets symbol
 * !! #M ...` (MEASURED, baseline config) -- and it links under `release` only
 * !! because inlining deletes the call so the relocation never exists, which
 * !! is a config-dependent escape rather than a fix. External linkage costs
 * !! this example nothing: the helper is still inlined under `release`, and
 * !! the library image still differs between the two configs. The keyword is
 * !! restored when that row closes, and not before. */
int dss_lib_step(int acc, int addend) {
    return acc + addend;
}

int dss_lib_answer(void) {
    int acc    = 0;
    int base   = 5;
    int addend = 0;
    int k      = 6;
    while (k) {
        addend = base + 2;                  /* loop-invariant: 5 + 2 == 7 */
        acc    = dss_lib_step(acc, addend); /* inlinable under `release`   */
        k      = k - 1;
    }
    return acc;                             /* 6 iterations x 7 == 42 */
}
