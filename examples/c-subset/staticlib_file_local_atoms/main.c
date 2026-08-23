/* [[D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM]] -- the
 * CONSUMER half. `dss_file_local_answer` is the ONLY name this translation
 * unit knows about; everything that makes the answer 42 is file-local to
 * `filelocal.c` and reaches this image only by surviving the archive round
 * trip. The manifest builds `filelocal.c` into a static library first, then
 * builds this file resolving that archive via `--resolve-library`, so the
 * member is PULLED and READ BACK -- which is the only path on which the
 * defect this example exists for can appear.
 *
 * ⚠ KEEP THIS FILE THIN -- no locals, no arithmetic, no inlinable callee --
 * for the reason its two sibling examples state: it leaves the executable
 * nothing of its own for the release pipeline to transform, so the
 * baseline-vs-release difference the arm asserts is attributable to the
 * ARCHIVED MEMBER. (The manifest's per-`dependsOn` `mustDifferFromBaseline`
 * asserts that directly, on the library's own image; the arm-level one on the
 * executable is the weaker statement and this file is what keeps it honest.)
 *
 * ⚠⚠ AND KEEP IT FREE OF ADDRESS-TAKEN LABELS SPECIFICALLY. ✔MEASURED 2026-08-20
 * with the shipped CLI, this exact consumer with a `&&label` / `goto *` added:
 * every one of the five legs reds at the exec link with `error[K_SymbolUndefined]
 * ... relocation in symbol #N references undefined symbol #M (not declared by
 * any AssembledFunction, ExternImport, nor AssembledData item)`. The three
 * controls that isolate it are recorded in the manifest's `$comment`; it is a
 * DIFFERENT open defect from the one this example witnesses, and putting a
 * computed goto here would red the corpus for a reason the example is not
 * about. */
extern int dss_file_local_answer(void);

int main(void) {
    return dss_file_local_answer();
}
