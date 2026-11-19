/* P69 fold H5 (lane hm): mingw-w64 gcc's stack protector -- gcc's analogue of cl's /GS -- names two routines from the
 * toolchain's own runtime, which DSS's PE `compilerSupport` runtime defines (tests/link/test_pe_compiler_support_runtime.cpp):
 *   gs_guarded   a character array, -fstack-protector-strong   -> __stack_chk_guard (the canary's value, read through
 *                                                                 gcc's `.refptr.__stack_chk_guard`) and
 *                                                                 __stack_chk_fail (called when the canary moved)
 * Nothing calls the C library: the loop is written out, so the object names the compiler's routines alone. gcc takes
 * the guard from libmingwex.a and the reporter from libssp-0.dll (MEASURED, -Xlinker --trace-symbol), an image a plain
 * Windows does not have.
 *
 * Built through probe-reference-cc's keep mode (object mode, each twice, the two reports' md5s equal), the probe's own
 * directory mapped away so the object records no host path:
 *   gcc -O0 -fstack-protector-strong -ffile-prefix-map={probe}=.   -> ..._gcc_O0_ssp.o
 *   gcc -O2 -fstack-protector-strong -ffile-prefix-map={probe}=.   -> ..._gcc_O2_ssp.o
 * (MinGW-W64 gcc 13.2.0, x86_64-ucrt-posix-seh.) The same source in run mode with -DDSS_FIXTURE_REFERENCE_MAIN (and
 * -DDSS_REF_MODE=<a mode's character code> for a mode other than 'o') is gcc's own measurement of what the pins
 * expect. */
__attribute__((noinline)) int gs_guarded(char const *s, int n) {
    char           buf[16];
    char volatile *p = buf;
    for (int i = 0; i < n; ++i) p[i] = s[i];
    int t = 0;
    for (int i = 0; i < 16; ++i) t += buf[i];
    return t;
}

#ifdef DSS_FIXTURE_REFERENCE_MAIN
/* The reference's own measurement: the same function, run by the reference toolchain's own link. The DSS pin's main
 * is this one, compiled by DSS, with one more mode, the guard's seeding. Modes:
 *   o  in bounds: the sum of "0123456789abcdef", 1122 -> 42
 *   s  32 bytes into the 16-byte array: at -O0 the array is 32 bytes below the frame pointer and the canary 8, so the
 *      write ends with the canary (24 bytes reach only padding and go undetected; 40 reach the saved frame too, and
 *      gcc's -O0 program then faults before its check) -> the canary moved
 *   g  the guard's value, printed: mingw-w64's library seeds it per run */
#include <stdint.h>
#include <stdio.h>
#ifndef DSS_REF_MODE
#define DSS_REF_MODE 'o'
#endif
extern uintptr_t __stack_chk_guard;
int main(int argc, char **argv) {
    char m = argc > 1 ? argv[1][0] : (char)DSS_REF_MODE;
    if (m == 's') return gs_guarded("0123456789abcdef0123456789abcdef", 32) == 0 ? 1 : 2;
    if (m == 'g') {
        printf("guard=%llx\n", (unsigned long long)__stack_chk_guard);
        return 42;
    }
    return gs_guarded("0123456789abcdef", 16) == 1122 ? 42 : 5;
}
#endif
