/* TF-C51 (D-FF1-STATICLIB-FAT-ARCHIVE) end-to-end RUNTIME witness.
 *
 * `main` calls `dss_input_answer`, which is defined in input.c -> built into
 * `input.a`. The runner builds the chain bottom-up:
 *   1. input.a   (from input.c:  defines dss_input_answer == 42)
 *   2. fatlib.a  (from fatlib.c, RESOLVING input.a)  -> the FAT MERGE:
 *                fatlib.a now contains BOTH dss_fat_extra AND the merged
 *                dss_input_answer.
 *   3. main      (from main.c,   RESOLVING ONLY fatlib.a)
 *
 * main's `--resolve-library` names ONLY fatlib.a -- input.a is NOT on it. So
 * `dss_input_answer` reaches main SOLELY via the merged copy the fat archive
 * carried across. The exec returns 42 IFF the merge worked.
 *
 * RED-ON-DISABLE (both gates verified in WSL, x86_64 native + arm64 qemu):
 *   (1) BUILD gate -- remove the merge and the `-staticlib` + `--resolve-library`
 *       path fail-louds again (D_StaticLibFatArchiveUnsupported) -> fatlib.a's
 *       dependsOn build returns rc != 0 -> the arm poisons -> red.
 *   (2) RUN gate -- even if the merge silently no-op'd, fatlib.a would lack
 *       dss_input_answer; main STILL links (DSS does not yet fail loud on an
 *       undefined exec symbol -- the open D-LINK-EXEC-UNDEFINED-SYMBOL-FAIL-LOUD
 *       gap), but at RUNTIME the loader reports "undefined symbol:
 *       dss_input_answer" -> exit 127 != 42 -> the runner's exit-code assert
 *       fires -> red.
 * gcc/clang agree (build the same 3-step chain -> exit 42). */
extern int dss_input_answer(void);

int main(void) {
    return dss_input_answer();
}
