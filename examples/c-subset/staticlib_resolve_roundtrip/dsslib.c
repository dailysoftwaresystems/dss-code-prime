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
 * ★ D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM -- WHY
 * `dss_lib_step` IS `static`, AND WHY DELETING THE KEYWORD WOULD DELETE THE
 * ONLY THING THIS MEMBER PROVES. The keyword IS the test. A file-local
 * function CALLED from an exported one, inside an ARCHIVE MEMBER, is the
 * shape that used to be unlinkable: the COFF and Mach-O archive-member
 * readers classified every non-external defined section symbol as a bodyless
 * interior block label rather than an atom, so the helper's bytes never
 * entered the image and the exec link red with `error[K_SymbolUndefined] ...
 * relocation in CU #N targets symbol #M which is not defined or imported in
 * that CompilationUnit`. Without `static` the helper is an ordinary external
 * symbol, which every reader has always promoted to an atom -- the member
 * would still build, still link and still exit 42 while asserting NOTHING
 * about the readers. It is also the ubiquitous shape: a `static` helper in an
 * archived TU is what any C author writes.
 * ✔MEASURED 2026-08-20 (shipped CLI, baseline config, this file with the
 * keyword): the `-staticlib` build and the `--resolve-library` exec link both
 * return rc=0 on pe64-x86_64, elf64-x86_64, elf64-aarch64, macho64-arm64 AND
 * macho64-x86_64, and the pe64 exec runs exit 42.
 *
 * ⚠⚠ THE WITNESS IS THE **BASELINE** ARM. THE `release` ARM DOES NOT EXERCISE
 * THIS DEFECT AND CANNOT. ✔MEASURED with `nm` over the two archives this file
 * produces: at the baseline configuration the member carries `dss_lib_answer`
 * (external) PLUS the file-local helper as a local `sym_<id>` -- DSS renames
 * internal-linkage functions, so `sym_<id>` is the only spelling a symbol
 * dump will show you -- and the intra-member relocation against it is exactly
 * what the reader must resolve. At `--config=release` that local symbol is
 * ABSENT FROM THE ARCHIVE ENTIRELY: Inlining removes the call and DCE removes
 * the function, so no file-local relocation survives and the arm would link
 * green against a reader that still had the bug. The `release` arm above is
 * still doing its own job -- it is the optimizer witness the
 * D-EXAMPLES-DEPENDSON-NO-RELEASE-OPTIMIZER-ARM note describes -- but it is
 * not this one, and a release-only test of this defect is vacuous. */
static int dss_lib_step(int acc, int addend) {
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
