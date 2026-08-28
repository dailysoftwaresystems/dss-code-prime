/* AP6 (plan 06 §5.1 B.2 / B.6.2) — the CONSUMER half of the compiler's
 * `dependsOn` ArtifactLink arm, the arm B.6.2 exists to stop shipping
 * unexercised. `dss_fold_twice` is defined in `foldlib/fold.c`, a sibling
 * project whose manifest declares `"artifactProfile": "staticlib"`. That
 * profile's composition verb is `ArtifactLink`, so the resolver does NOT merge
 * its sources: it BUILDS that project into its own archive — for the format it
 * DERIVES from this consumer's target (B.10) — files it under
 * `<output>/deps/foldlib/<format>/`, and appends it to this target's
 * resolveLibraries. The static-link pull then folds the member into a
 * self-contained exec.
 *
 * NON-FOLDING, and the exit code is a FUNCTION OF THE DEPENDENCY: `argc` is an
 * OS-supplied runtime value, so nothing here can be precomputed, and the
 * `v + v + 2` shape exists only inside the archive. Neither this file nor
 * `fold.c` contains the literal 42. With argc == 1: lift(1) == 5 twice gives
 * acc == 10, so the archive is called with 20 and returns 20 + 20 + 2 == 42.
 *
 * ★ THE LOOP AND THE `static` HELPER ARE WHAT MAKE THE `release` ARM BITE, and
 * they are here because of a MEASUREMENT rather than a habit. With a bare
 * `return dss_fold_twice(argc * 20);` the shipped `release` and `debug`
 * pipelines emitted a BYTE-IDENTICAL pe64 image (✔sha256
 * 18EF3364…BFAE08FA both ways, 2026-08-15): the one call is external, so
 * Inlining/Mem2Reg/SimplifyCfg had nothing to work on and the optimizer arm
 * asserted only that a no-op stayed a no-op. Consumer-side code the optimizer
 * really transforms — an inlinable helper, an accumulator Mem2Reg promotes, a
 * loop SimplifyCfg folds — makes the arm witness the optimizer ACROSS the
 * dependency-resolved call rather than beside it.
 *
 * RED-ON-DISABLE: delete the `dependsOn` entry from `.dss-project.json` and no
 * archive is built, nothing is appended to `--resolve-library`, and no
 * definition of `dss_fold_twice` reaches the link. */
extern int dss_fold_twice(int v);

static int lift(int v) {
    return v * 5;
}

int main(int argc, char **argv) {
    (void)argv;
    int acc = 0;
    for (int i = 0; i < 2; ++i) {
        acc = acc + lift(argc);        /* argc==1 -> 5 + 5 == 10 */
    }
    return dss_fold_twice(acc * 2);    /* 20 -> 20 + 20 + 2 == 42 */
}
