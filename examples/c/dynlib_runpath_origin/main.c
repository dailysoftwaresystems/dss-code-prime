/* D-LK-IMAGE-CANNOT-DECLARE-A-RUNPATH -- the CLIENT half.
 * `dss_runpath_answer` is defined in `dsslib.c`, which the runner has already
 * built into a DYNAMIC library beside this executable (the target's
 * `dependsOn`). This image records where to find it -- `runpaths:
 * ["${ORIGIN}"]`, the directory of the image that carries the path -- so it
 * RUNS with no LD_LIBRARY_PATH and returns 42.
 *
 * KEEP THIS FILE THIN: no locals, no arithmetic, no inlinable callee, so the
 * `release` arm has nothing HERE to transform and the whole baseline-vs-release
 * difference belongs to the prerequisite library, whose entries declare
 * `mustDifferFromBaseline`. */
extern int dss_runpath_answer(void);

int main(void) {
    return dss_runpath_answer();
}
