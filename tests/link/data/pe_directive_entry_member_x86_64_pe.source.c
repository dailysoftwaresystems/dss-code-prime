/* SOURCE of the committed fixtures
 * `tests/link/data/libpe_directive_entry_x86_64_pe.a` and
 * `tests/link/data/pe_directive_entry_member_x86_64_pe.obj`, kept beside them
 * as `tests/ffi/data/libtls.source.c` is kept beside its own.
 *
 * WHAT IT IS. The object names the image's entry, a function of its own: the
 * process starts there, with no C runtime startup, and exits with what it
 * returns. The ONE object is both fixtures: named to the link as it is, and
 * as the archive's one member (which the archive search pulls, and whose
 * entry then does not count).
 *
 * REBUILD, in an x64 Visual Studio developer prompt (`vcvars64.bat`), in a
 * directory holding a copy of this file:
 *
 *   cl /nologo /c /O2 /MD /GS- /experimental:deterministic /pathmap:%CD%=X:\dss-fixture /Fodss_entry_member.obj pe_directive_entry_member_x86_64_pe.source.c
 *   llvm-ar rcs --format=gnu libpe_directive_entry_x86_64_pe.a dss_entry_member.obj
 *   copy dss_entry_member.obj pe_directive_entry_member_x86_64_pe.obj
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
#pragma comment(linker, "/ENTRY:dss_member_entry")
int dss_member_entry(void) { return 42; }
