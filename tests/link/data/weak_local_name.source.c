/* SOURCE of the committed fixtures `tests/link/data/weak_local_name_<isa>_<format>_<compiler>.{o,obj}` (P69 fold 2,
 * lane p69/xa; D-LK-WEAK-NAME-REFERENCE-BOUND-TO-THE-BODY-NOT-THE-NAME, the review of fold 1, MAJOR 1).
 *
 * WHAT IT IS. A WEAK ALIAS OF A `static` BODY, twice: a datum and a function. Each body has two names, a
 * module-private one and a weak external one, and its unit refers to it through BOTH: `read_direct_l` and `direct_fl`
 * through the static name (a relocation that names the BYTES), `read_through_l` and `call_through_fl` through the
 * weak name (one that names the NAME, which another object may win).
 *
 * REBUILD (each prints nothing; no path of the builder is recorded in the object):
 *   gcc   -O0 -ffunction-sections -c weak_local_name.source.c -o weak_local_name_x86_64_elf_gcc.o
 *   clang -O0 -ffunction-sections -c weak_local_name.source.c -o weak_local_name_x86_64_elf_clang.o
 *   aarch64-linux-gnu-gcc -O0 -ffunction-sections -c weak_local_name.source.c -o weak_local_name_aarch64_elf_gcc.o
 *   clang --target=aarch64-linux-gnu -O0 -ffunction-sections -c weak_local_name.source.c -o weak_local_name_aarch64_elf_clang.o
 * (`-ffunction-sections` so that the call through the static name is a relocation and not a displacement the
 * assembler resolved.) The COFF objects, from the same source:
 *   gcc -O0 -ffunction-sections -c weak_local_name.source.c -o weak_local_name_x86_64_pe_mingw.o        (MinGW-w64)
 *   clang --target=x86_64-pc-windows-msvc -O0 -ffunction-sections -mno-incremental-linker-compatible
 *         -c weak_local_name.source.c -o weak_local_name_x86_64_pe_clang.obj
 * (`-mno-incremental-linker-compatible`: without it clang's COFF header records the time the object was built, and
 * no two builds are the same bytes.)
 *
 * PROVENANCE and what each reference linker makes of each object are recorded where the fixtures are read
 * (`tests/link/test_common_symbols.cpp`, the suite `WeakNameReferencesOnReferenceObjects`). An md5 written there
 * records WHICH BYTES were measured; no guard enforces it.
 */
static int impl_l = 7;
extern int shared_l __attribute__((weak, alias("impl_l")));
int read_direct_l(void) { return impl_l; }
int read_through_l(void) { return shared_l; }

static int impl_fl(void) { return 7; }
extern int shared_fl(void) __attribute__((weak, alias("impl_fl")));
int direct_fl(void) { int (*volatile p)(void) = impl_fl; return p(); }
int call_through_fl(void) { return shared_fl(); }
