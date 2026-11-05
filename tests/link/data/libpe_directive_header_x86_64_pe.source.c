/* SOURCE of the committed fixture
 * `tests/link/data/libpe_directive_header_x86_64_pe.a`, kept beside it as
 * `tests/ffi/data/libtls.source.c` is kept beside its own.
 *
 * WHAT IT IS. The member states the image's heap, version, subsystem (GUI,
 * version 6.02, started by the C runtime's console startup so that `main` is
 * the program), preferred base and section alignment.
 *
 * REBUILD, in an x64 Visual Studio developer prompt (`vcvars64.bat`), in a
 * directory holding a copy of this file:
 *
 *   cl /nologo /c /O2 /MD /GS- /experimental:deterministic /pathmap:%CD%=X:\dss-fixture /Fodss_header_member.obj libpe_directive_header_x86_64_pe.source.c
 *   llvm-ar rcs --format=gnu libpe_directive_header_x86_64_pe.a dss_header_member.obj
 *
 * `/experimental:deterministic /pathmap:` IS LOAD-BEARING. cl records the
 * absolute path of the object it writes INSIDE the object, and without the
 * pair that is the directory of whoever built it. MEASURED 2026-10-08 on cl
 * 19.44.35228: `/Brepro`, `/experimental:deterministic` alone and
 * `/d1trimfile:` all keep the real directory, and `/pathmap:` alone is
 * ignored (D9007). With the pair the object records its path as
 * `X:\dss-fixture\<its name>`, and the same bytes come out of any directory.
 *
 * PROVENANCE, MEASURED at the build (2026-10-08): cl 19.44.35228 for x64
 * (MSVC 14.44.35207), llvm-ar 19.1.5. The fixture's md5 and what link.exe and
 * lld-link make of it are in the manifest of the example that links it (its
 * `expected.json` names this file). Do not read an md5 there as a pin a guard
 * enforces: it records WHICH BYTES were measured.
 */
#pragma comment(linker, "/HEAP:0x200000,0x10000")
#pragma comment(linker, "/VERSION:3.5")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS,6.02")
#pragma comment(linker, "/ENTRY:mainCRTStartup")
#pragma comment(linker, "/BASE:0x200000000")
#pragma comment(linker, "/ALIGN:0x2000")
int dss_header_member(void) { return 0; }
