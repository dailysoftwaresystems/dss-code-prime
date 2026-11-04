/* P69 fold H5 (lane hm): the routines a cl object names from its toolchain's own static runtime, which
 * DSS's PE `compilerSupport` runtime defines (tests/link/test_pe_compiler_support_runtime.cpp). Each function below
 * makes the compiler name one family:
 *   cs_big              a frame past a page            -> __chkstk (and, its array guarded, the cookie pair)
 *   cs_guarded          a character array escaping     -> __security_cookie, __security_check_cookie, __GSHandlerCheck
 *   cs_ranged           an index bounded by the loop, stored once past it -> __report_rangecheckfailure (cl /O2)
 *   cs_overrun          stores through a pointer past a 16-byte array     -> the cookie check fails
 *   cs_float            floating point                 -> _fltused
 *   cs_guarded_through  a /GS frame an exception passes through          -> __GSHandlerCheck checks its cookie
 *   cs_corrupt_through  the same, its cookie overwritten before the exception passes: the handler's check fails
 *   cs_trygs            a /GS frame with __try          -> __GSHandlerCheck_SEH, then __C_specific_handler's scope
 *                       (its filter a CONSTANT, which cl compiles into no filter funclet: a filter that is an
 *                       expression is pe_chained_unwind.source.c's object, for the ASSOCIATIVE code COMDAT cl
 *                       puts the funclet in)
 *   cs_alloca_through   a /GS frame with a frame register (alloca) an exception passes through
 * Nothing calls the C library: every loop is written out, so the object names the compiler's routines alone.
 *
 * Built through probe-reference-cc's keep mode (object mode, each twice, the two reports' md5s equal), the probe's own
 * directory mapped away so the object records no host path:
 *   cl  /O2 /GS /MD /Brepro /experimental:deterministic /pathmap:{probe}=X:\dss-fixture  -> ..._cl_MD_O2_GS.obj
 *   cl  /Od /GS  (the rest alike)                                                        -> ..._cl_MD_Od_GS.obj
 *   cl  /O2 /GS-                                                                         -> ..._cl_MD_O2_GSoff.obj
 *   cl  /Od /GS-                                                                         -> ..._cl_MD_Od_GSoff.obj
 * (The four cells of {/Od, /O2} x {/GS, /GS-}: a /GS- object names `__chkstk`, `_fltused` and `__C_specific_handler`,
 * and none of the /GS family.)
 * (`/pathmap:` is ignored without `/experimental:deterministic`: cl's warning D9007.) The same source in run mode
 * with /DDSS_FIXTURE_REFERENCE_MAIN (and /DDSS_REF_MODE=<a mode's character code> for a mode other than 'o') is
 * cl's own measurement of what the pins expect. No leg has a clang-cl, so cl is the measured producer. */
#include <excpt.h>
#include <malloc.h>

typedef void (*thrower)(void);

/* Read one character at a time through a volatile view: cl /O2 vectorizes the plain loop and writes CHAINED unwind
 * for the vector part (UNW_FLAG_CHAININFO records inside `sum`), a reading this fixture does not claim -- the first
 * cut's `sum` did, and pe_chained_unwind.source.c keeps it. MEASURED: this cut's object holds no such record (a
 * read-only dump of its .pdata: every RUNTIME_FUNCTION begins a function, none flags UNW_FLAG_CHAININFO). */
__declspec(noinline) static int sum(char const *p, int n) {
    char const volatile *v = p;
    int total = 0;
    for (int i = 0; i < n; ++i) total += v[i];
    return total;
}

__declspec(noinline) int cs_big(int seed) {
    volatile char buf[70000];
    for (int i = 0; i < 70000; i += 4096) buf[i] = (char)seed;
    buf[69999] = (char)seed;
    return buf[0] + buf[69999];
}

__declspec(noinline) int cs_guarded(char const *s) {
    char buf[64];
    int  n = 0;
    for (; s[n] != 0 && n < 32; ++n) buf[n] = s[n];
    return sum(buf, n);
}

__declspec(noinline) int cs_ranged(char const *s) {
    char buf[64];
    int  n = 0;
    while (s[n] != 0 && n < 64) buf[n] = s[n], ++n;
    buf[n] = 0;
    return sum(buf, n);
}

__declspec(noinline) int cs_overrun(char const *s, int n) {
    char           buf[16];
    volatile char *p = buf;
    for (int i = 0; i < n; ++i) p[i] = s[i];
    return sum(buf, 16);
}

__declspec(noinline) int cs_float(int x) {
    volatile double d = x;
    return (int)(d * 1.5);
}

__declspec(noinline) int cs_guarded_through(char const *s, thrower f) {
    char buf[64];
    int  n = 0;
    for (; s[n] != 0 && n < 32; ++n) buf[n] = s[n];
    f();
    return sum(buf, n);
}

__declspec(noinline) int cs_corrupt_through(char const *s, int n, thrower f) {
    char           buf[16];
    volatile char *p = buf;
    for (int i = 0; i < n; ++i) p[i] = s[i];
    f();
    return sum(buf, 16);
}

__declspec(noinline) int cs_trygs(char const *s, thrower f) {
    char buf[64];
    int  n = 0;
    for (; s[n] != 0 && n < 32; ++n) buf[n] = s[n];
    __try {
        f();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return sum(buf, n) + 100;
    }
    return sum(buf, n);
}

__declspec(noinline) int cs_alloca_through(int size, char const *s, thrower f) {
    char *dyn = (char *)_alloca(size);
    char  buf[64];
    int   n = 0;
    for (; s[n] != 0 && n < 32; ++n) buf[n] = s[n], dyn[n % size] = s[n];
    f();
    return sum(buf, n) + dyn[0];
}

#ifdef DSS_FIXTURE_REFERENCE_MAIN
/* The reference's own measurement: the same functions, run by the reference toolchain's own link. The DSS pin's main
 * is this one, compiled by DSS. */
#include <windows.h>
#ifndef DSS_REF_MODE
#define DSS_REF_MODE 'o'
#endif
static void raise_through(void) { RaiseException(0xE0000002u, 0, 0, 0); }
static void quiet(void) {}
int main(int argc, char **argv) {
    char m = argc > 1 ? argv[1][0] : (char)DSS_REF_MODE;
    if (m == 'v') return cs_overrun("0123456789abcdef0123456789abcdef0123456789", 40) == 0 ? 1 : 2;
    if (m == 'r') return cs_ranged("0123456789012345678901234567890123456789012345678901234567890123") == 0 ? 3 : 4;
    if (m == 'x') {
        __try {
            cs_corrupt_through("0123456789abcdef0123456789abcdef0123456789", 40, raise_through);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 8;
        }
        return 9;
    }
    int ok = cs_big(3) == 6 && cs_guarded("abc") == 'a' + 'b' + 'c' && cs_ranged("ab") == 'a' + 'b'
          && cs_float(4) == 6 && cs_trygs("ab", raise_through) == 'a' + 'b' + 100 && cs_trygs("ab", quiet) == 'a' + 'b';
    int through = 0;
    __try {
        cs_guarded_through("ab", raise_through);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        through += 1;
    }
    __try {
        cs_alloca_through(100, "ab", raise_through);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        through += 1;
    }
    return ok && through == 2 ? 42 : 5;
}
#endif
