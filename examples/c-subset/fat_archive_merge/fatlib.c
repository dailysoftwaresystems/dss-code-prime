/* TF-C51 (D-FF1-STATICLIB-FAT-ARCHIVE) fat-library OWN-CU source.
 *
 * This CU is the fat library's own member. When the fat `-staticlib` build is
 * handed the INPUT `input.a` via `--resolve-library`, the driver MERGES that
 * archive's members INTO the output `fatlib.a` alongside this CU-derived
 * member. So `fatlib.a` ends up carrying BOTH `dss_fat_extra` (this CU) AND
 * the merged `dss_input_answer` (from input.a). Unreferenced here on purpose --
 * its presence proves the fat lib has its own member independent of the merge.
 *
 * D-EXAMPLES-DEPENDSON-NO-RELEASE-OPTIMIZER-ARM -- WHY THIS BODY IS NOT
 * `return 7;`. `expected.json` now declares a `release` arm; with the runner
 * building each prerequisite library under the arm's OWN configuration, this
 * CU is compiled by the shipped release pipeline too. Giving it the
 * SAME shape as `dss_input_answer` in input.c -- an inlinable helper, a
 * loop-invariant addend, and locals for Mem2Reg to promote -- means BOTH
 * halves of the fat archive are real optimizer input, not just the half
 * `main` happens to reference. The return value stays pinned at 7 (the fat
 * lib's own member is identified by value in the notes above); only the way it
 * is computed gives the optimizer something to do.
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
int dss_fat_bump(int acc, int addend) {
    return acc + addend;
}

int dss_fat_extra(void) {
    int acc    = 0;
    int base   = 2;
    int addend = 0;
    int n      = 7;
    while (n) {
        addend = base - 1;                    /* loop-invariant: 2 - 1 == 1 */
        acc    = dss_fat_bump(acc, addend);   /* inlinable under `release`  */
        n      = n - 1;
    }
    return acc;                               /* 7 iterations x 1 == 7 */
}
