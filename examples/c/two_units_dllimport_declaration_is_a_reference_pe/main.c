/* A `__declspec(dllimport)` DECLARATION WITH NO `extern` NAMES ANOTHER UNIT'S OBJECT, AT EITHER SCOPE, AND AN
 * ATTRIBUTE MAY LEAD A BLOCK-SCOPE `extern` (P69 round 1, lane cs, fold f26b). b.c defines every object read here.
 * Every return before 42 names the first fact that failed. */

__declspec(dllimport) int dss_imported_value;   /* file scope, no initializer: a reference, never a tentative */

static int read_through_a_block_import(void) {
    __declspec(dllimport) int dss_imported_value;   /* block scope, no `extern`: a reference, never a local */
    return dss_imported_value;
}

static int read_through_attributed_externs(void) {
    __attribute__((unused)) extern int dss_gnu_led;
    [[maybe_unused]] extern int dss_standard_led;
    __declspec(dllimport) extern int dss_declspec_led;
    return dss_gnu_led + dss_standard_led + dss_declspec_led;
}

int main(void) {
    if (dss_imported_value != 40) return 1;               /* the file-scope import reads b.c's object */
    if (read_through_a_block_import() != 40) return 2;    /* so does the block one */
    dss_imported_value = 41;                                /* one object: a write through one name ... */
    if (read_through_a_block_import() != 41) return 3;    /* ... is read through the other */
    if (read_through_attributed_externs() != 6) return 4; /* 1 + 2 + 3 */
    return dss_imported_value + 1;
}
