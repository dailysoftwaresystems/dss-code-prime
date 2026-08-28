/* D-CSUBSET-VLA-INITIALIZER (C23 6.7.10p4: "An entity of variable length array
 * type shall not be initialized except by an empty initializer") — the REFUSAL
 * half, through the shipped CLI.
 *
 * gcc 13.3.0, clang 19.1.1 and clang 18.1.3 all refuse this ("variable-sized
 * object may not be initialized except with an empty initializer") in both
 * -std=gnu17 and -std=c2x — MEASURED 2026-08-25.
 *
 * WHAT IT REPLACED IS THE REASON THIS IS AN ERROR EXAMPLE RATHER THAN A NOTE.
 * DSS did not merely mis-diagnose this: it ACCEPTED it, at a COMPILE-TIME
 * `sizeof` of 12. The declarator resolver tested the flexible-array /
 * init-inference flag ABOVE its VLA arm, so a present-but-non-constant bound was
 * read as an ABSENT one and the object was re-sized from the brace list — the
 * `argc` the programmer wrote was discarded with no diagnostic at all. A silent
 * wrong size is exactly the class this project refuses, and only a REJECTED
 * compile proves it is gone. Red-on-disable: restore the merged flag and this
 * compiles clean, so the runner finds no S_VlaInitializerNotEmpty and goes red. */
int main(int argc, char **argv) {
    int a[argc] = {1, 2, 3};
    return a[0];
}
