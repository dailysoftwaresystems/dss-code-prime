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
int dss_input_add(int acc, int addend) {
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
