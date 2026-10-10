/* SOURCE of the committed fixture
 * `tests/link/data/libpe_directive_exports_x86_64_pe.a`, kept beside it as
 * `tests/ffi/data/libtls.source.c` is kept beside its own.
 *
 * WHAT IT IS. The archive's SECOND member: a function nothing references but
 * the first member's `/EXPORT:dss_pulled_fn`.
 *
 * REBUILD, in an x64 Visual Studio developer prompt (`vcvars64.bat`), in a
 * directory holding a copy of this file and the object
 * `dss_export_member.obj` (from
 * `libpe_directive_exports_x86_64_pe.source.c`):
 *
 *   cl /nologo /c /O2 /MD /GS- /experimental:deterministic /pathmap:%CD%=X:\dss-fixture /Fodss_export_pulled.obj libpe_directive_exports_x86_64_pe.pulled.source.c
 *   llvm-ar rcs --format=gnu libpe_directive_exports_x86_64_pe.a dss_export_member.obj dss_export_pulled.obj
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
int dss_pulled_fn(void) { return 9; }
