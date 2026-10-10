/* SOURCE of the committed fixtures
 * `tests/link/data/tentative_common_<toolchain>.o` and `.obj`, kept beside
 * them as `tests/ffi/data/libtls.source.c` is kept beside its own.
 *
 * WHAT IT IS. A TENTATIVE DEFINITION -- `int dss_tentative;` -- which every
 * reference compiler writes as a COMMON under `-fcommon` (cl writes every C
 * tentative definition as one), and a function that reads it. Linked beside a
 * definition of the name that may be stated more than once, the reference
 * linkers SPLIT on which of the two the program reads: GNU ld's ELF and PE
 * linkers let the common outrank a WEAK definition (the program reads 0),
 * Apple's ld and both PE linkers on cl's select-any let the definition
 * replace the common (it reads 5).
 *
 * REBUILD, one object per reference toolchain, each in a directory holding a
 * copy of this file (the source is named RELATIVELY, so the object records no
 * directory):
 *
 *   gcc -O2 -fno-pie -fcommon -c tentative_common.source.c -o tentative_common_x86_64_elf.o
 *   aarch64-linux-gnu-gcc -O2 -fno-pie -fcommon -c tentative_common.source.c -o tentative_common_aarch64_elf.o
 *   gcc -O2 -fcommon -c tentative_common.source.c -o tentative_common_x86_64_pe_mingw.o
 *   cc -arch arm64 -O2 -fcommon -c tentative_common.source.c -o tentative_common_arm64_macho.o
 *   cc -arch x86_64 -O2 -fcommon -c tentative_common.source.c -o tentative_common_x86_64_macho.o
 *   cl /nologo /c /O2 /MD /GS- /experimental:deterministic /pathmap:%CD%=X:\dss-fixture /Fotentative_common_x86_64_pe_msvc.obj tentative_common.source.c
 *
 * cl's `/experimental:deterministic /pathmap:` pair is load-bearing: cl
 * records the absolute path of the object it writes inside the object, and
 * only that pair writes a neutral one (MEASURED 2026-10-08).
 *
 * PROVENANCE, MEASURED at the builds (2026-10-08):
 *
 *   x86_64_elf:      gcc 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1), GNU ld 2.42
 *   aarch64_elf:     aarch64-linux-gnu-gcc 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1), GNU ld 2.42
 *   x86_64_pe_mingw: mingw-w64 gcc 13.2.0 (x86_64-ucrt-posix-seh), GNU ld 2.42
 *   arm64_macho:     Apple clang 21.0.0 (clang-2100.1.1.101), ld-1267
 *   x86_64_macho:    Apple clang 21.0.0 (clang-2100.1.1.101), ld-1267
 *   x86_64_pe_msvc:  cl 19.44.35228 for x64 (MSVC 14.44.35207), llvm-ar 19.1.5
 *
 * Each object's md5, and what its own reference linker makes of the pair, are
 * in the manifests of the examples that link them (the `common_beside_...`
 * examples; each `expected.json` names this file). Do not read an md5 there
 * as a pin a guard enforces: it records WHICH BYTES were measured.
 */
int dss_tentative;
int dss_read_tentative(void) { return dss_tentative; }
