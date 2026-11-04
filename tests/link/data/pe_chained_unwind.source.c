/* P69 fold H5 (lane hm), for lane xa's fold 3e: a cl /O2 /GS C object that carries two forms DSS's COFF reader
 * refuses today, each written for ordinary C -- the compiler-support fixture's FIRST cut, its code kept byte for byte.
 * (pe_compiler_support_x86_64_pe_cl_MD_O2_GS.obj is the cut whose `sum` reads through a volatile view and whose
 * `__except` filter is a constant, so cl writes neither, and the /GS routines' pin reads the routines alone.)
 *
 * What the object holds (MEASURED, read-only COFF dumps of its bytes, md5 291a0733de0a264e226edc033c1c49a2, 7448 bytes -- the first cut's bytes: three builds under three different headers gave the same md5):
 *   CHAINED UNWIND, for D-LK-COFF-FOREIGN-UNWIND-DROP (reopened by the orchestrator, fold 3e): cl vectorizes `sum`'s
 *   plain loop (section 13 `.text$mn`, 237 bytes, the STATIC `sum`) and describes it with THREE RUNTIME_FUNCTIONs --
 *     sum+0   .. sum+159  -> $unwind$sum     (UNWIND_INFO version 1, flags 0, prolog 2, 1 code)
 *     sum+159 .. sum+212  -> $chain$0$sum    (version 1, flags 4 = UNW_FLAG_CHAININFO, prolog 5, 2 codes)
 *     sum+212 .. sum+237  -> $chain$1$sum    (version 1, flags 4 = UNW_FLAG_CHAININFO, prolog 0, no codes)
 *   each chain record ending with the RUNTIME_FUNCTION of `$unwind$sum`, each in its own .pdata/.xdata COMDAT,
 *   ASSOCIATIVE to section 13. DSS refuses the first record that begins inside a function: K_UnwindRuleUnrepresentable,
 *   "the RUNTIME_FUNCTION at offset 0 of '.pdata' begins at offset 159 of '.text$mn', where no function of this object
 *   begins" (MEASURED on the constant-filter cut of the same `sum`, macos-arm64-debug run 20261029-030849-d10e8742).
 *   A FILTER FUNCLET, for D-LK-COFF-COMDAT-UNSUPPORTED-SELECTION (fold 3d), which the reader meets FIRST: `cs_trygs`'s
 *   `__except` filter is an expression (GetExceptionCode() == 0xE0000002u ? ... : ...), and cl compiles it into
 *     section 11 `.text$mn`  COMDAT Selection 1 (NODUPLICATES), defines `cs_trygs` (EXTERNAL, DTYPE_FUNCTION)
 *     section 12 `.text$x`   32 bytes, no relocations, COMDAT Selection 5 (ASSOCIATIVE) to section 11; it defines ONE
 *                            symbol, `cs_trygs$filt$0`, class STATIC, DTYPE_FUNCTION -- the funclet
 *     section 32 `.xdata`    `$unwind$cs_trygs`, ASSOCIATIVE to 11: `__GSHandlerCheck_SEH` (ADDR32NB at +8), and the
 *                            scope table's filter field naming `cs_trygs$filt$0` (ADDR32NB at +24)
 *     sections 34/35         the funclet's own `.xdata`/`.pdata`, each ASSOCIATIVE to 11
 *   refused today: F_CorruptedBinary, "section '.text$x' has COMDAT Selection 5 (LARGEST / ASSOCIATIVE / unknown)"
 *   (MEASURED, macos-arm64-debug run 20261029-025047-5bc27529).
 * link.exe links it, and cl's own run of the same source with /DDSS_FIXTURE_REFERENCE_MAIN exits 42 (probe-reference-cc run 20261029-031937-7c816e30, windows-x86_64-release, cl 19.51.36260).
 *
 * Built through probe-reference-cc's keep mode (object mode, twice, the two reports' md5s equal), the probe's own
 * directory mapped away so the object records no host path:
 *   cl  /O2 /GS /MD /Brepro /experimental:deterministic /pathmap:{probe}=X:\dss-fixture  -> ..._cl_MD_O2_GS.obj
 * (cl 19.51.36260, windows-x86_64-release.) */
#include <excpt.h>
#include <malloc.h>

typedef void (*thrower)(void);

__declspec(noinline) static int sum(char const *p, int n) {
    int total = 0;
    for (int i = 0; i < n; ++i) total += p[i];
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
    } __except (GetExceptionCode() == 0xE0000002u ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
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
