/* TF-C51 (D-FF1-STATICLIB-FAT-ARCHIVE) fat-library OWN-CU source.
 *
 * This CU is the fat library's own member. When the fat `-staticlib` build is
 * handed the INPUT `input.a` via `--resolve-library`, the driver MERGES that
 * archive's members INTO the output `fatlib.a` alongside this CU-derived
 * member. So `fatlib.a` ends up carrying BOTH `dss_fat_extra` (this CU) AND
 * the merged `dss_input_answer` (from input.a). Unreferenced here on purpose --
 * its presence proves the fat lib has its own member independent of the merge. */
int dss_fat_extra(void) {
    return 7;
}
