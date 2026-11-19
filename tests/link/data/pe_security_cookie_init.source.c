/* P69 fold 3d (lane xa): the PE startup seeds the /GS security cookie before the entry.
 *
 * Every reference C runtime calls `__security_init_cookie` before `main`, so a program never runs with the cookie
 * the runtime's object ships (MSVC x64: 0x00002B992DDFA232), which an attacker knows and which protects nothing.
 * `fill` holds a character array whose address escapes, so cl /GS guards its frame with the cookie (it references
 * `__security_cookie`, `__security_check_cookie` and the frame's handler `__GSHandlerCheck`); every index is a
 * constant in range, so no range check (`__report_rangecheckfailure`) is emitted and the object names the cookie
 * family alone. `main` returns 42 when the cookie it reads is NOT the shipped constant and the guarded function ran,
 * 7 when the cookie was never seeded.
 *
 * Built through probe-reference-cc's keep mode (object mode, each twice, the two reports' md5s equal), the probe's
 * own directory mapped away so the object records no host path:
 *   cl        /O2 /GS /MD /Brepro /experimental:deterministic /pathmap:{probe}=X:\dss-fixture -> ..._cl_MD_O2_GS.obj
 *   cl        /Od /GS (the rest alike)                                                     -> ..._cl_MD_Od_GS.obj
 *   clang-cl  /O2 /GS /MD /Brepro                                                          -> ..._clangcl_MD_O2_GS.obj
 *   clang-cl  /Od /GS /MD /Brepro                                                          -> ..._clangcl_MD_Od_GS.obj
 * (cl 19.51, windows-x86_64-release; a clang-cl object exists only where that leg's PATH has one.) The same
 * source in probe-reference-cc's run mode (cl /O2 /GS /MD: cl compiles, link.exe links, the program runs) is the
 * reference's own reading: exit 42. tests/link/test_pe_security_cookie_init.cpp links each object ALONE with
 * DSS, link.exe and lld-link, and each program must exit 42. */
#include <stdint.h>

extern uintptr_t __security_cookie;

__declspec(noinline) static int sum(char const *p) {
    int total = 0;
    for (int i = 0; p[i] != 0; ++i) total += p[i];
    return total;
}

__declspec(noinline) int fill(char const *s) {
    char buf[64];
    for (int i = 0; i < 4; ++i) buf[i] = s[i];
    return sum(buf);
}

int main(void) {
    int const ran = fill("dss") == 'd' + 's' + 's';
    return (ran && __security_cookie != (uintptr_t)0x00002B992DDFA232ull) ? 42 : 7;
}
