/* TF-C88 (D-CSUBSET-ASM-LABEL-SYMBOL-RENAME) — CU B: the REFERENCE side. It calls the renamed function through a
 * C identifier that no translation unit defines; only the asm label binds it. */

int entry(int x) __asm("dss_c88_impl");

int cu_b_contribution(void) {
    /* entry(-8) == renamed_target(-8) == 12 */
    return entry(-8);
}
