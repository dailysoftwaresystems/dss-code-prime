/* TF-C51 (D-FF1-STATICLIB-FAT-ARCHIVE) fat-archive MEMBER source.
 *
 * This CU is built into an INPUT static library (`input.a`) that the fat
 * library's build resolves via `--resolve-library`. The fat-archive merge
 * pulls THIS member (`dss_input_answer`) into the output library. `main.c`
 * links ONLY against the fat library -- so the sole way `main` can resolve
 * `dss_input_answer` is if the merge carried this member across. Returns 42.
 *
 * D-EXAMPLES-DEPENDSON-NO-RELEASE-OPTIMIZER-ARM -- WHY THIS BODY IS NOT
 * `return 42;`. This is the member `main` actually references, so it is the
 * one whose code reaches the linked exec, and `expected.json`'s `release` arm
 * compares that exec against its baseline with
 * `mustDifferFromBaseline: true`. The baseline pipeline is a bare `Identity`,
 * i.e. a no-op, so a single-`return` body emits the SAME bytes under `debug`
 * and under `release` and the arm would have nothing to assert.
 * `dss_input_answer` therefore carries real work for the shipped release
 * pipeline -- an inlinable helper, a loop-invariant addend, and locals for
 * Mem2Reg to promote out of their stack slots -- while still returning
 * exactly 42, because the example's `exitCode` contract must not move. Do NOT
 * simplify it back.
 *
 * ★ D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM -- WHY
 * `dss_input_add` IS `static`, AND WHAT IT PROVES HERE THAT IT CANNOT PROVE
 * ANYWHERE ELSE IN THE CORPUS. The keyword is the test: a file-local function
 * called from an exported one, inside an archive member, is the shape the
 * COFF and Mach-O archive-member readers used to demote to a bodyless
 * interior block label, dropping its bytes and reddening the link with
 * `error[K_SymbolUndefined] ... relocation in CU #N targets symbol #M`.
 * Without `static` the helper is an ordinary external, which every reader has
 * always promoted to an atom, so the member would still build, still link and
 * still exit 42 while asserting nothing.
 * ★★ THIS MEMBER IS THE ONE THAT CROSSES THE FAT MERGE, so its file-local
 * symbol has to survive TWO read-backs, not one: the fat `-staticlib` build
 * reads `input.a` to merge this member into `fatlib.a`, and the exec link then
 * reads `fatlib.a` to pull it. ✔MEASURED 2026-08-20 (shipped CLI, baseline
 * config) with `nm` on the intermediates: `input.a` carries the local as
 * `sym_84` and `fatlib.a` carries the merged copy as `sym_2` -- the merge
 * RENUMBERS it -- and the reconstructed body reaches the linked executable.
 * ✔The whole three-step chain returns rc=0 on pe64-x86_64, elf64-x86_64,
 * elf64-aarch64, macho64-arm64 AND macho64-x86_64, and the pe64 executable
 * runs exit 42 at both configurations.
 *
 * ⚠⚠ THE WITNESS IS THE **BASELINE** ARM. ✔MEASURED with `nm` on the archives
 * this file produces: at `--config=release` the local symbol is ABSENT
 * ENTIRELY -- Inlining removes the call, DCE removes the function, and no
 * file-local relocation survives for a reader to get wrong. A release-only
 * test of this defect is vacuous. (`examples/c-subset/staticlib_file_local_atoms`
 * is the example built to hold at BOTH configurations, by letting a file-local
 * const data slot hold the helper's address so DCE cannot delete it.) */
static int dss_input_add(int acc, int addend) {
    return acc + addend;
}

int dss_input_answer(void) {
    int acc    = 0;
    int base   = 3;
    int addend = 0;
    int n      = 7;
    while (n) {
        addend = base + base;                 /* loop-invariant: 3 + 3 == 6 */
        acc    = dss_input_add(acc, addend);  /* inlinable under `release`  */
        n      = n - 1;
    }
    return acc;                               /* 7 iterations x 6 == 42 */
}
