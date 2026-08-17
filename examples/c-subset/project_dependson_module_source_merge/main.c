/* AP6 (plan 06 §5.1 B.2 / B.6.2) — the CONSUMER half of the compiler's
 * `dependsOn` SourceMerge arm. `dss_scale_by_seven` is DEFINED NOWHERE IN THIS
 * PROJECT: it lives in `scalemod/scale.c`, a sibling project whose manifest
 * declares `"artifactProfile": "module"`. That profile's composition verb is
 * `SourceMerge`, so the resolver expands the module's own `sources[]` (against
 * the MODULE's directory, not this one — B.3) and joins them to this build's
 * compilation. Two sources reach the driver, so it takes the `compileUnits`
 * (`cc a.c b.c`) path and the linker binds this extern to the merged CU.
 *
 * NON-FOLDING, and the exit code is a FUNCTION OF THE DEPENDENCY, not a
 * constant: `argc` is a runtime value the OS supplies, so no pass can
 * precompute `argc + 5`, and the multiplier `7` exists ONLY inside the
 * dependency. Neither this file nor `scale.c` contains the literal 42. With
 * argc == 1 (the runners spawn with no arguments): (1 + 5) * 7 == 42.
 *
 * RED-ON-DISABLE: delete the `dependsOn` entry from `.dss-project.json` and
 * `scale.c` is never merged — nothing supplies `dss_scale_by_seven`, so no
 * arithmetic that yields 42 is left in the program. */
extern int dss_scale_by_seven(int v);

int main(int argc, char **argv) {
    (void)argv;
    return dss_scale_by_seven(argc + 5);   /* argc==1 -> 6 * 7 == 42 */
}
