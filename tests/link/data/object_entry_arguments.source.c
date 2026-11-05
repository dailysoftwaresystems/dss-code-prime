/* P69 fold 3d, lane p69/xa: the FOREIGN units of tests/link/test_object_entry.cpp and of the entry-object cells of
 * tests/link/test_foreign_object_library_binding.cpp -- a program's ENTRY defined by an object a reference compiler
 * built, linked by DSS alone (the all-object route) or beside a DSS source (the mixed route).
 *
 * One unit per macro; every image is run as `prog 7 x y` (argc 4, argv[1] "7"):
 *   ENTRY_ARGS    main(argc, argv)        -> 47 under every reference (argc * 10 + the digit)
 *   ENTRY_VOID    main(void)              -> 5
 *   ENTRY_ENVP    main(argc, argv, envp)  -> 11 (argc is 4, the environment is not empty)
 *   ENTRY_HELPER  the `helper()` the three call (0); also the DSS source of the mixed route
 *   ENTRY_PUTS    main(void) calling the C library's `puts` and nothing of the program -> 42
 *   ENTRY_GETENV  a unit nothing calls, which itself calls the C library's `getenv`
 *
 * BUILT BY (`build_entry_fixtures.py` and `build_entry_fixtures_wsl.sh` of lane xa's work dir; each object twice,
 * byte-identical, recording no path of the host that built it):
 *   cl 19.44.35229 /nologo /c /O2 /MD /GS- /Brepro /experimental:deterministic
 *       /pathmap:<build dir>=X:\dss-fixture /D<macro>                               -> *_x86_64_pe_cl_MD_O2.obj
 *   mingw-w64 gcc 13.2.0 -O2 -c -D<macro>                                           -> *_x86_64_pe_gcc_O2.obj
 *   gcc 13.3.0 (Ubuntu 24.04) -O2 -c -D<macro>                                       -> *_x86_64_elf_gcc_O2.o
 *   clang 18.1.3 (Ubuntu 24.04) -O2 -c -D<macro>                                     -> *_x86_64_elf_clang_O2.o
 *   aarch64-linux-gnu-gcc 13.3.0 -O2 -c -D<macro>                                    -> *_arm64_elf_gcc_O2.o
 *   Apple clang 21.0.0 (macOS 26.6.2, arm64) -O2 -c -D<macro>                        -> *_arm64_macho_clang_O2.o
 * each from a copy of this file named `object_entry_arguments.c` in the build directory.
 */
#if defined(ENTRY_ARGS)
int helper(void);
int main(int argc, char **argv) { return helper() + argc * 10 + (argc > 1 ? argv[1][0] - '0' : 0); }
#elif defined(ENTRY_VOID)
int helper(void);
int main(void) { return helper() + 5; }
#elif defined(ENTRY_ENVP)
int helper(void);
int main(int argc, char **argv, char **envp) {
    int n = 0;
    if (envp) while (envp[n]) ++n;
    return helper() + (argc == 4 ? 1 : 2) + (n > 0 ? 10 : 20);
}
#elif defined(ENTRY_HELPER)
int helper(void) { return 0; }
#elif defined(ENTRY_PUTS)
#include <stdio.h>
int main(void) { return puts("entry object") >= 0 ? 42 : 7; }
#elif defined(ENTRY_GETENV)
#include <stdlib.h>
int unused_lookup(void) { return getenv("DSS_FIXTURE_NO_SUCH_VARIABLE") == 0 ? 0 : 1; }
#else
#error "name the unit: -DENTRY_ARGS, -DENTRY_VOID, -DENTRY_ENVP, -DENTRY_HELPER, -DENTRY_PUTS or -DENTRY_GETENV"
#endif
