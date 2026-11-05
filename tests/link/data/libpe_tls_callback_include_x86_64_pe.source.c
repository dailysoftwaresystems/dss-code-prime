/* SOURCE of the committed fixture
 * `tests/link/data/libpe_tls_callback_include_x86_64_pe.a`, kept beside it as
 * `tests/ffi/data/libtls.source.c` is kept beside its own.
 *
 * WHAT IT IS. cl's TLS-CALLBACK idiom: a callback pointer in `.CRT$XLB`, kept
 * by `/INCLUDE:` beside `/INCLUDE:_tls_used`.
 *
 * REBUILD, in an x64 Visual Studio developer prompt (`vcvars64.bat`), in a
 * directory holding a copy of this file:
 *
 *   cl /nologo /c /O2 /MD /GS- /experimental:deterministic /pathmap:%CD%=X:\dss-fixture /Fodss_tls_member.obj libpe_tls_callback_include_x86_64_pe.source.c
 *   llvm-ar rcs --format=gnu libpe_tls_callback_include_x86_64_pe.a dss_tls_member.obj
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
#include <windows.h>
static volatile int dss_tls_seen = 0;
static void NTAPI dss_tls_callback(PVOID h, DWORD reason, PVOID r) {
    (void)h;
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) dss_tls_seen = 42;
}
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:dss_tls_callback_ptr")
#pragma section(".CRT$XLB", read)
__declspec(allocate(".CRT$XLB")) PIMAGE_TLS_CALLBACK dss_tls_callback_ptr = dss_tls_callback;
int dss_tls_member_seen(void) { return dss_tls_seen; }
