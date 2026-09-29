/* D-FFI-LIBRARY-TLS-EXPORT-BINDS-AS-PLAIN-DATA — the end-to-end witness.
 *
 * The library named by this example's `prebuiltLibraries` exports BOTH names
 * from ONE image: `dss_lib_tls_counter` with THREAD storage duration (ELF
 * STT_TLS, in .tdata) and `dss_lib_plain_counter` as ordinary data. Both are
 * declared here as plain `extern int`, and both are USED, so both reach the
 * binder.
 *
 * ★★★ THE SECOND DECLARATION IS THE NEGATIVE CONTROL AND IT IS WHY IT IS IN
 * THIS FILE RATHER THAN A SIBLING ONE. The expect-error arm asserts EXACT SET
 * EQUALITY over the produced diagnostics, so declaring ONE refusal while the
 * program makes TWO library imports pins both halves at once: the thread-local
 * import is refused, and the ordinary one is NOT. A guard that fired on every
 * import of this library would produce two diagnostics and red here, which a
 * single-import source could never detect.
 *
 * WHAT THE REFUSAL PREVENTS, ✔MEASURED before the rule landed and recorded on
 * the commit that added it: this exact program linked with rc 0 under
 * --warnings-as-errors, emitted R_X86_64_GLOB_DAT / R_AARCH64_GLOB_DAT against
 * a thread-local symbol, minted `OBJECT GLOBAL DEFAULT UND` for it in its own
 * .dynsym — and then read the SHARED LIBRARY'S OWN ELF HEADER where its datum
 * should have been. GNU ld refuses the identical program.
 *
 * ⓘ The sum is never computed: the compile is refused before anything is
 * emitted, and the arm spawns nothing. It is written as a sum so that BOTH
 * imports are live and neither can be dropped as unused.
 */

extern int dss_lib_tls_counter;
extern int dss_lib_plain_counter;

int main(void) {
    return dss_lib_tls_counter + dss_lib_plain_counter;
}
