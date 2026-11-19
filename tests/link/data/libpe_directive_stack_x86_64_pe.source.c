/* SOURCE of the committed fixture
 * `tests/link/data/libpe_directive_stack_x86_64_pe.a`, kept beside it as
 * `tests/ffi/data/libtls.source.c` is kept beside its own.
 *
 * WHAT IT IS. The member states the program's stack, as `#pragma
 * comment(linker, ...)` writes it into `.drectve`.
 *
 * REBUILD, in an x64 Visual Studio developer prompt (`vcvars64.bat`), in a
 * directory holding a copy of this file:
 *
 *   cl /nologo /c /O2 /MD /GS- /experimental:deterministic /pathmap:%CD%=X:\dss-fixture /Fodss_stack_member.obj libpe_directive_stack_x86_64_pe.source.c
 *   llvm-ar rcs --format=gnu libpe_directive_stack_x86_64_pe.a dss_stack_member.obj
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
#pragma comment(linker, "/STACK:0x10000000,0x20000")
int dss_stack_member_frames(void) { return 4000; }
