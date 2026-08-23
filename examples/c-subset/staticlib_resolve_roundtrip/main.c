/* D-EXAMPLES-RUNNER-MULTI-ARTIFACT (c171): the CLIENT half. `dss_lib_answer`
 * is defined in `dsslib.c`, which the runner has already built into a static
 * library (the target's `dependsOn`); the driver's `--resolve-library` reads
 * that archive, pulls the member, and static-links it into this exec. No
 * runtime library is needed (self-contained). Returns 42.
 *
 * D-EXAMPLES-DEPENDSON-NO-RELEASE-OPTIMIZER-ARM -- KEEP THIS FILE THIN. It has
 * no locals, no arithmetic and no inlinable callee, so the shipped release
 * pipeline has nothing HERE to transform. That is deliberate: it is what lets
 * `expected.json`'s `release` arm attribute the whole baseline-vs-release image
 * difference to the PREREQUISITE LIBRARY member rather than to the
 * executable's own code. Giving `main` work of its own would let that arm go
 * green while the static-library half of the build was never optimized at
 * all -- the exact masked-coverage trap the arm exists to close. */
extern int dss_lib_answer(void);

int main(void) {
    return dss_lib_answer();
}
