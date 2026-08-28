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
 * ★ D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM -- WHY
 * `dss_fat_bump` IS `static`, AND WHY IT MATTERS THAT IT IS THE HALF NOBODY
 * REFERENCES. The keyword is the test: a file-local function called from an
 * exported one, inside an archive member, is the shape the COFF and Mach-O
 * archive-member readers used to demote to a bodyless interior block label,
 * dropping its bytes. Without `static` the helper is an ordinary external,
 * which every reader has always promoted to an atom, and this file stops
 * asserting anything.
 * ★★ WHAT THIS COPY ADDS OVER `input.c`'s. `dss_fat_extra` is the fat
 * library's OWN member and `main` never calls it, so this file's file-local
 * symbol has to survive being WRITTEN INTO the merged archive without ever
 * being pulled out of it -- the fat `-staticlib` build must emit a member
 * carrying a local function beside the merged one, and the exec link must
 * then read `fatlib.a`'s index and correctly leave this member alone.
 * ✔MEASURED 2026-08-20 (shipped CLI, baseline config, `nm` on `fatlib.a`): the
 * merged archive carries BOTH members, this one's local as `sym_84` and the
 * merged `input.o`'s as `sym_2`, and the linked executable contains the
 * merged copy only. The whole three-step chain returns rc=0 on pe64-x86_64,
 * elf64-x86_64, elf64-aarch64, macho64-arm64 AND macho64-x86_64.
 *
 * ⚠⚠ THE WITNESS IS THE **BASELINE** ARM. ✔MEASURED with `nm`: at
 * `--config=release` the local symbol is ABSENT ENTIRELY -- Inlining removes
 * the call and DCE removes the function -- so no file-local relocation
 * survives for a reader to get wrong, and a release-only test of this defect
 * is vacuous. (`examples/c/staticlib_file_local_atoms` is the example
 * built to hold at BOTH configurations.) */
static int dss_fat_bump(int acc, int addend) {
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
