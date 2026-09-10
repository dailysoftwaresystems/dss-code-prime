/* D-FFI-RESOLVE-LIBRARY-DOES-NOT-CHECK-THE-LIBRARY-ARCH -- the CLIENT half.
 * `dss_dyn_answer` is defined in `dsslib.c`, which the runner has already
 * built into a DYNAMIC library for this arm's target (the target's
 * `dependsOn`); the driver's `--resolve-library` reads that library's export
 * surface, validates it against the image this build emits -- object format
 * AND architecture -- and records a runtime import. Returns 42.
 *
 * KEEP THIS FILE THIN, for the same reason as its staticlib sibling: no
 * locals, no arithmetic, no inlinable callee, so the shipped release pipeline
 * has nothing HERE to transform and the whole baseline-vs-release difference
 * belongs to the PREREQUISITE LIBRARY. ✔MEASURED: the four executables are
 * byte-IDENTICAL between the two configurations while all four libraries
 * differ, which is exactly why `mustDifferFromBaseline` is declared per
 * dependency rather than on the arm. */
extern int dss_dyn_answer(void);

int main(void) {
    return dss_dyn_answer();
}
