/* D-EXAMPLES-RUNNER-MULTI-ARTIFACT (c171): the CLIENT half. `dss_lib_answer`
 * is defined in `lib.c`, which the runner has already built into a static
 * library (the target's `dependsOn`); the driver's `--resolve-library` reads
 * that archive, pulls the member, and static-links it into this exec. No
 * runtime library is needed (self-contained). Returns 42. */
extern int dss_lib_answer(void);

int main(void) {
    return dss_lib_answer();
}
