/* D-EXAMPLES-RUNNER-MULTI-ARTIFACT (c171): the LIBRARY half of the
 * multi-artifact corpus round-trip. The runner builds THIS into a static
 * archive (a `.lib` on pe64 / a `.a` on elf64 via the c171-D `-staticlib`
 * format), then builds `main.c` resolving `dss_lib_answer` against it via
 * `--resolve-library` -> a self-contained exec -> exit 42. */
int dss_lib_answer(void) {
    return 42;
}
