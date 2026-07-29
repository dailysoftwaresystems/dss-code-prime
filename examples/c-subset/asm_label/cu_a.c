/* TF-C88 (D-CSUBSET-ASM-LABEL-SYMBOL-RENAME) — CU A: the DEFINITION side of
 * the asm-label rename.
 *
 * `renamed_target` is defined under the ASSEMBLER name "dss_c88_impl". CU B
 * declares a DIFFERENT C identifier (`entry`) carrying the SAME label and calls
 * it. Nothing but the label connects them: the two C names never meet, so the
 * program links only if the label really is the on-binary symbol on BOTH rails —
 * the DEFINITION rail (compile_pipeline / program.cpp `nameOf`) and the IMPORT
 * rail (ffi ingest `FfiMetadata.mangledName`). Honoring it on one rail only
 * leaves the cross-CU merge key mismatched and the reference emitted as a
 * dynamic import; the linker then fails LOUD with an undefined symbol.
 *
 * A labelled GLOBAL rides along because data and functions take separate paths
 * through the object writers.
 *
 * exit 42 = 30 (via the renamed call) + 12 (the multi-declarator typedef arm).
 */

typedef unsigned int u8_t, acc_t, flag_t;

u8_t base __asm("dss_c88_base") = 10;

int renamed_target(int x) __asm("dss_c88_impl");
int renamed_target(int x) { return x + 20; }

/* The C name CU B calls. Declared here too so both CUs agree on the signature;
 * it is never DEFINED under this name — only the label resolves it. */
int entry(int x) __asm("dss_c88_impl");

int cu_b_contribution(void);

int main(void) {
    acc_t  acc  = (acc_t)entry((int)base);   /* 10 + 20 = 30 */
    flag_t flag = (flag_t)cu_b_contribution();
    return (int)(acc + flag);                /* 42 */
}
