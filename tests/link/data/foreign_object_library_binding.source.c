/* P69 fold 3c, lane p69/xa: the FOREIGN unit of tests/link/test_foreign_object_library_binding.cpp.
 *
 * It calls the C library through its producer's OWN <stdio.h>, so each object names `puts` the way that producer's
 * default runtime does, and a DSS link of it beside a DSS `main` must bind that name to the C library's image:
 *
 *   foreign_object_library_binding_x86_64_pe_cl_MD_O2.obj       cl 19.44.35229 /c /O2 /MD
 *       /experimental:deterministic /pathmap:<build dir>=X:\dss-fixture   -> `__imp_puts` (the UCRT headers'
 *                                                                           dllimport under /MD)
 *   foreign_object_library_binding_x86_64_pe_clangcl_MT_O2.obj  clang-cl 19.1.5 /c /O2 /MT /Brepro
 *                                                                        -> `puts`, an undefined symbol of TYPE 0
 *   foreign_object_library_binding_x86_64_pe_gcc_O2.obj         mingw-w64 gcc 13.2.0 -O2 -c
 *                                                                        -> `puts`, an undefined symbol of type 0x20
 *   libforeign_object_library_binding_x86_64_pe_cl_MD_O2.a      lib.exe 14.44 /Brepro /OUT:<name>.lib of the cl /MD
 *                                                               object, under its own name (past 15 characters, so
 *                                                               the archive carries a "//" longnames member: the
 *                                                               name and one NUL); committed under the `.a` name the
 *                                                               other PE archive fixtures here take (the bytes do not
 *                                                               record the archive's own name)
 *
 * Each object is built twice, byte-identical, and records no path of the host that built it.
 */
#include <stdio.h>

int say(void) { return puts("foreign unit") >= 0 ? 42 : 7; }
