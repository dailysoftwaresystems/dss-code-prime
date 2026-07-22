/* TF-C51 (D-FF1-STATICLIB-FAT-ARCHIVE) fat-archive MEMBER source.
 *
 * This CU is built into an INPUT static library (`input.a`) that the fat
 * library's build resolves via `--resolve-library`. The fat-archive merge
 * pulls THIS member (`dss_input_answer`) into the output library. `main.c`
 * links ONLY against the fat library -- so the sole way `main` can resolve
 * `dss_input_answer` is if the merge carried this member across. Returns 42. */
int dss_input_answer(void) {
    return 42;
}
