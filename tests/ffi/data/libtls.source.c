/* SOURCE of the committed fixtures tests/ffi/data/libtls-x86_64.so.1 and
 * tests/ffi/data/libtls-aarch64.so.1.
 *
 * Built by a REAL gcc, so the exported symbol's `STT_TLS` type is that
 * toolchain's own answer and never bytes this repository hand-assembled --
 * the same posture libdssver.source.c and README-bsd-archives.md take.
 * Rebuild with:
 *
 *   gcc -shared -fPIC -Os -o libtls-x86_64.so.1 libtls.source.c \
 *       -Wl,-soname,libtls.so -Wl,--build-id=none \
 *     && strip --strip-all libtls-x86_64.so.1
 *
 *   aarch64-linux-gnu-gcc -shared -fPIC -Os -o libtls-aarch64.so.1 \
 *       libtls.source.c -Wl,-soname,libtls.so -Wl,--build-id=none \
 *     && aarch64-linux-gnu-strip --strip-all libtls-aarch64.so.1
 *
 * PROVENANCE, ✔MEASURED at the build (2026-09-17, WSL Ubuntu 24.04):
 *   gcc 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1), aarch64-linux-gnu-gcc 13.3.0,
 *   GNU ld 2.42.
 *   libtls-x86_64.so.1    14320 bytes  md5 443498f7a6a7ac392812398fe0869754
 *   libtls-aarch64.so.1   67208 bytes  md5 2c94297c1088f68f0fa96f130324ee52
 * `readelf --dyn-syms -W` reports `TLS GLOBAL DEFAULT` for dss_lib_tls_counter
 * in BOTH images. ⚠ Do not read those md5s as a pin a guard enforces — they
 * record WHICH BYTES were measured, so a regenerated fixture that behaves
 * differently can be told apart from one that does not. The property the corpus
 * examples actually assert is the `containerWitness` `.tdata`, which a rebuild
 * without `__thread` would lose.
 *
 * WHAT THESE WITNESS, and it is one fact the FFI reader has always had and
 * the binder used to throw away: `dss_lib_tls_counter` is exported with
 * THREAD storage duration. It lands in `.tdata`, and its `.dynsym` row carries
 * `STT_TLS` -- which `ffi/binary_readers/elf_reader.cpp` classifies as
 * `SymbolKind::Tls`. A C reference that declares the same name as an ORDINARY
 * object disagrees with that definition about which machinery binds it, and
 * must be refused (C23 6.7.1p3; GNU ld refuses the identical program with
 * "TLS definition in <lib> section .tdata mismatches non-TLS reference").
 *
 * `dss_lib_plain_counter` is the NEGATIVE half and is why one image carries
 * both: a guard that refused every import from this library would pass the
 * positive arm while being useless. It is ordinary `STT_OBJECT` data in
 * `.data`, exported from the SAME image, and a plain `extern int` reference to
 * it must still bind and still run.
 *
 * ⚠ THE VALUES ARE NOT WHAT THE CORPUS EXAMPLE ASSERTS. The refusal happens at
 * BIND time, so nothing is ever spawned against the thread-local and its value
 * is never read. 7 and 11 are here so a future arm that DOES run against this
 * image has a definite answer to assert rather than inventing one.
 */

__thread int dss_lib_tls_counter = 7;

int dss_lib_plain_counter = 11;

int dss_lib_read_tls(void) { return dss_lib_tls_counter; }

int dss_lib_read_plain(void) { return dss_lib_plain_counter; }
