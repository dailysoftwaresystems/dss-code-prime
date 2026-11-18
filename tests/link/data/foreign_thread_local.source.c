/* P69 fold 3f, lane p69/xa (ledger 716): the FOREIGN unit of tests/link/test_foreign_thread_local_objects.cpp -- an
 * object a reference compiler built that reaches its own thread-local through the access model the compiler writes
 * for position-independent code, linked by DSS beside a DSS `main` that bumps the counter twice and returns it (42:
 * the counter starts at 40 in the thread that runs it).
 *
 * HOW EACH OBJECT WAS BUILT (2026-11-17, lane xa, through the harness's probe-reference-cc; each in three runs,
 * byte-identical, recording no host path; the reference's own program -- this unit beside that `main`, linked by its
 * own toolchain -- RUN: every one exits 42), from this text less this comment:
 *   linux-x86_64-debug (Ubuntu 24.04):
 *     gcc 13.3.0 -O2 -fPIC -ffile-prefix-map=<probe dir>=. -c   -> foreign_thread_local_x86_64_elf_gcc_fpic_O2.o
 *     clang 18.1.3, the same                                     -> foreign_thread_local_x86_64_elf_clang_fpic_O2.o
 *       (both: R_X86_64_TLSGD on the counter and a call to `__tls_get_addr`, the general-dynamic model)
 *   linux-arm64-debug (Ubuntu 24.04):
 *     gcc 13.3.0, the same                                       -> foreign_thread_local_arm64_elf_gcc_fpic_O2.o
 *       (R_AARCH64_TLSDESC_ADR_PAGE21, _LD64_LO12, _ADD_LO12 and _CALL: the TLS descriptor)
 *   macos-arm64-debug (macOS 26):
 *     Apple clang 21.0.0 -O2 -ffile-prefix-map=<probe dir>=. -c  -> foreign_thread_local_arm64_macho_clang_O2.o
 *       (ARM64_RELOC_TLVP_LOAD_PAGE21 / _PAGEOFF12 on the counter's descriptor, whose thunk is `__tlv_bootstrap`)
 * Sizes and md5s (no guard enforces one: they record WHICH BYTES were measured):
 *   x86_64_elf_gcc_fpic     1576  md5 9f4028f60735ef0c6460ca1097fc5127
 *   x86_64_elf_clang_fpic   1248  md5 81c33879527a07e1e516fb630551c230
 *   arm64_elf_gcc_fpic      1656  md5 4cee1163fdec8156d77a6fda89438271
 *   arm64_macho_clang        960  md5 a428851afce930bcdba1d585f2bfba16
 */
__thread int dss_tls_counter = 40;
int dss_tls_bump(void) { return ++dss_tls_counter; }
