/* [[D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM]] -- THE
 * ARCHIVED MEMBER. Every declaration above `dss_file_local_answer` is
 * `static`, and the keyword is the whole subject of this example: an archive
 * member's FILE-LOCAL symbols must survive the round trip through the `.a` /
 * `.lib` and reach the linked image AS ATOMS. The COFF and Mach-O
 * archive-member readers used to classify every non-external defined section
 * symbol as a bodyless INTERIOR BLOCK LABEL instead, so a file-local body
 * never entered the image -- loudly when something referenced it
 * (`error[K_SymbolUndefined] ... relocation in CU #N targets symbol #M which
 * is not defined or imported in that CompilationUnit`) and SILENTLY when
 * nothing did. Deleting any `static` below turns the symbol into an ordinary
 * external, which every reader has always promoted to an atom, and the file
 * stops testing anything.
 *
 * The four file-local shapes here are deliberately different from each other,
 * because they take different routes through the reader:
 *
 *   1. `probe_shadow`  -- a file-local FUNCTION NOTHING REFERENCES: the SILENT
 *      arm. It is FIRST in the translation unit on purpose, so it is emitted
 *      at text-section offset 0, BELOW the first external atom. See the
 *      RED-ON-DISABLE note below for what it asserts and why that is not
 *      vacuous, since an unreachable function cannot move an exit code.
 *   2. `k_weights`     -- file-local read-only DATA, loaded by code. This is
 *      the COFF static-rodata half of the same root
 *      ([[D-LK-COFF-ARCHIVE-MEMBER-READER-LOSES-STATIC-RODATA-SYMBOLS]]): a
 *      `.text` relocation whose target is a class-STATIC symbol in `.rdata`.
 *   3. `fold_step`     -- a file-local FUNCTION CALLED BY NAME: the LOUD arm,
 *      an intra-member call relocation to a non-external text symbol.
 *   4. `k_fold_slot`   -- file-local CONST DATA HOLDING THE ADDRESS OF A
 *      FILE-LOCAL FUNCTION: an absolute-width relocation running the other
 *      way, from `.rdata` INTO `.text`, with a local symbol at BOTH ends. It
 *      also gives shape 3 a SECOND route into the exit code --
 *      `dss_file_local_answer` reaches `fold_step` by direct call inside the
 *      loop and once more through this slot -- and it is why the release arm
 *      still tests something (below).
 *
 * ★★ WHY THIS EXAMPLE EXERCISES THE DEFECT AT **BOTH** CONFIGURATIONS, WHICH
 * ITS TWO SIBLINGS (`staticlib_resolve_roundtrip`, `fat_archive_merge`)
 * CANNOT. In those, the single file-local helper is reached only by a direct
 * call, so at `--config=release` Inlining removes the call and DCE removes the
 * function -- ✔MEASURED with `nm`: their optimized archives contain NO local
 * symbol at all, and the arm would link green against a reader that still had
 * the bug. Here `k_fold_slot` holds `fold_step`'s ADDRESS, so the function
 * cannot be deleted and the `.rdata`-to-`.text` relocation against a local
 * symbol survives optimization. ✔MEASURED, this file's pe64-x86_64 archive,
 * `nm` + `objdump -r`: at BASELINE the member carries five symbols -- four
 * LOCAL (`probe_shadow` at text offset 0, the two `.rdata` items, `fold_step`)
 * and one external -- with three `.text` REL32s and one `.rdata` ADDR64, every
 * one of them targeting a local; at `--config=release` `probe_shadow` is gone
 * (DCE) and the direct-call REL32 is gone (Inlining), but the `.rdata` ADDR64
 * against the surviving local text symbol REMAINS, along with three `.text`
 * REL32s onto the two local data symbols. So the SILENT arm alone is
 * baseline-only; shapes 2, 3 and 4 are witnessed at BOTH configurations.
 *
 * ⚠ NO ADDRESS-TAKEN LABEL IN THIS FILE, AND THAT IS A MEASURED CONSTRAINT
 * RATHER THAN AN OVERSIGHT -- see the manifest's `$comment`, which records the
 * four-cell table and names [[D-LINK-OBJECT-READERS-DROP-INTERIOR-SYMBOL-OFFSET]].
 * A `&&label` here would red this example on three of its five legs for a
 * reason that is NOT the defect above.
 *
 * ⚠ DO NOT SIMPLIFY THE ARITHMETIC. The manifest's `release` arm and its
 * `dependsOn` entry BOTH declare `mustDifferFromBaseline: true`, and the
 * baseline pipeline is a bare `Identity`, i.e. a no-op -- so a member whose
 * body is a single `return 42;` emits the same bytes under both
 * configurations and both assertions would red with nothing to say. The loop,
 * the loop-invariant `bias`, the promotable locals and the inlinable
 * `fold_step` are what give Inlining / Mem2Reg / CSE / LICM / DCE something to
 * do. The value 42 is the example's `exitCode` contract and must not move. */

/* 1. THE SILENT ARM. Nothing in this program refers to `probe_shadow` by any
 *    means -- no call, no address-of, no table. It is here to be PRESENT in
 *    the member, which is the whole point: before the fix its bytes were
 *    dropped on read-back and the link still returned rc=0.
 *
 *    ⚠ AN UNREFERENCED FUNCTION CANNOT BE OBSERVED IN AN EXIT CODE. That is
 *    not a weakness of the example, it is the definition of the arm -- code
 *    nothing reaches has no runtime behaviour to change, which is exactly why
 *    the loss went unnoticed for as long as it did. What the corpus asserts
 *    for this shape is therefore the LINK VERDICT, which both runners already
 *    check strictly: the `-staticlib` build and the `--resolve-library` exec
 *    link must both succeed with ZERO diagnostics, and the binary must then
 *    run to 42. That assertion is NOT vacuous, because a reader that drops
 *    this body no longer does so quietly: `everyDefinedSymbolIsCoveredByAnAtom`
 *    refuses a partial reconstruction with `F_ObjectReaderSymbolBodyDropped`,
 *    and `probe_shadow` sits below the first external atom, so a demotion
 *    leaves its offset covered by nothing and the refusal fires. Silent loss
 *    became a loud refusal; this example turns the loud refusal back into a
 *    green link. */
static int probe_shadow(int seed) {
    int acc = 0;
    int k   = seed;
    while (k) {
        acc = acc + seed;
        k   = k - 1;
    }
    return acc;
}

/* 2. FILE-LOCAL READ-ONLY DATA. Read by `fold_step` below, so the member
 *    carries a `.text` relocation against a class-STATIC `.rdata` symbol --
 *    the shape [[D-LK-COFF-ARCHIVE-MEMBER-READER-LOSES-STATIC-RODATA-SYMBOLS]]
 *    named. The four weights sum to the example's exit code. */
static const int k_weights[4] = { 6, 9, 12, 15 };

/* 3. THE LOUD ARM: a file-local function CALLED BY NAME from the exported
 *    one. Small enough to sit under the release pipeline's `inlineThreshold`,
 *    so Inlining is genuinely witnessed on an ARCHIVED translation unit. */
static int fold_step(int acc, int idx) {
    return acc + k_weights[idx];
}

/* 4. FILE-LOCAL CONST DATA POINTING AT A FILE-LOCAL FUNCTION. A local symbol
 *    at both ends of one absolute-width relocation, running from the data
 *    section INTO the text section -- the mirror of shape 2's direction. It is
 *    also the reason `fold_step` outlives DCE at `--config=release`: its
 *    address escapes into an initialized object, so the optimizer may inline
 *    the direct call but may not delete the body. */
static int (*const k_fold_slot)(int, int) = fold_step;

/* THE ONLY EXTERNAL SYMBOL IN THIS MEMBER. `main.c` resolves this name and
 * nothing else, so everything the linked image gains beyond it arrived by way
 * of a FILE-LOCAL symbol being reconstructed correctly. */
int dss_file_local_answer(void) {
    int acc  = 0;
    int base = 2;
    int bias = 0;
    int i    = 0;
    while (i < 3) {
        bias = base - 2;                    /* loop-invariant: 2 - 2 == 0  */
        acc  = fold_step(acc + bias, i);    /* direct call to the local    */
        i    = i + 1;                       /* 6, then 15, then 27         */
    }
    return k_fold_slot(acc, 3);             /* indirect: 27 + 15 == 42     */
}
