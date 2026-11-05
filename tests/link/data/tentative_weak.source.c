/* SOURCE of the committed fixtures
 * `tests/link/data/tentative_weak_<toolchain>.o` and `.obj`, kept beside them
 * as `tests/ffi/data/libtls.source.c` is kept beside its own.
 *
 * WHAT IT IS. The GNU spelling of a definition that may be stated more than
 * once: a WEAK definition of the name `tentative_common.source.c` holds as a
 * common. (cl has no such attribute; its spelling is
 * `tentative_selectany.source.c`.)
 *
 * REBUILD, one object per reference toolchain, each in a directory holding a
 * copy of this file (the source is named RELATIVELY, so the object records no
 * directory):
 *
 *   gcc -O2 -fno-pie -c tentative_weak.source.c -o tentative_weak_x86_64_elf.o
 *   aarch64-linux-gnu-gcc -O2 -fno-pie -c tentative_weak.source.c -o tentative_weak_aarch64_elf.o
 *   gcc -O2 -c tentative_weak.source.c -o tentative_weak_x86_64_pe_mingw.o
 *   cc -arch arm64 -O2 -c tentative_weak.source.c -o tentative_weak_arm64_macho.o
 *   cc -arch x86_64 -O2 -c tentative_weak.source.c -o tentative_weak_x86_64_macho.o
 *
 * PROVENANCE, MEASURED at the builds (2026-10-08):
 *
 *   x86_64_elf:      gcc 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1), GNU ld 2.42
 *   aarch64_elf:     aarch64-linux-gnu-gcc 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1), GNU ld 2.42
 *   x86_64_pe_mingw: mingw-w64 gcc 13.2.0 (x86_64-ucrt-posix-seh), GNU ld 2.42
 *   arm64_macho:     Apple clang 21.0.0 (clang-2100.1.1.101), ld-1267
 *   x86_64_macho:    Apple clang 21.0.0 (clang-2100.1.1.101), ld-1267
 *
 * Each object's md5, and what its own reference linker makes of the pair, are
 * in the manifests of the examples that link them (the `common_beside_...`
 * examples; each `expected.json` names this file). Do not read an md5 there
 * as a pin a guard enforces: it records WHICH BYTES were measured.
 */
__attribute__((weak)) int dss_tentative = 5;
