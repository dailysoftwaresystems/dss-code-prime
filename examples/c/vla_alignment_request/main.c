/* D-CSUBSET-ALIGNOF-VALUE-OVERALIGNED-OBJECT (P69, lane `cs`, fold f26e) -- an ALIGNMENT REQUEST ON A
 * VARIABLY MODIFIED TYPE is answered and DELIVERED: `_Alignof` of the type and of an object of it, the object's
 * PLACEMENT when it is allocated at run time behind run-time-sized neighbours, and the size, which the request
 * never pads.
 *
 * MEASURED 2026-11-19, probe vm2 (gcc 15.2.0 and clang 21.1.8, -std=gnu17 -Wall; MinGW-w64 gcc 13.2.0; Apple
 * clang 21): a VLA typedef asking 16 or 64 (after its declarator or leading its specifiers), an object of it, a
 * request at the start of a parenthesized declarator, a pointer's pointee and a LOWER request (1) are answered
 * alike by every reference, and every reference places each object where it asks. In a TYPE NAME
 * (`__alignof__(__attribute__((aligned(16))) int[n])`) gcc and MinGW answer 16 and clang and Apple clang 4,
 * saying "'aligned' attribute ignored when parsing type": clang does not implement the request there, so gcc's
 * documented meaning (6.4.1, `aligned` "specifies a minimum alignment for the entity it applies to") is DSS's.
 * MSVC has no variable-length array (C2057) and abstains.
 *
 * exit 42 = every answer is the references'; 1-12 name the first that was not. `n` comes from argc, so no length
 * is a constant the compiler could fold. */
#include <stdint.h>

static int vla_alignment(int n) {
    typedef int T16[n] __attribute__((aligned(16)));
    typedef __attribute__((aligned(16))) int L16[n];
    typedef int T64[n] __attribute__((aligned(64)));
    typedef int T1[n] __attribute__((aligned(1)));
    T64 a;
    int (__attribute__((aligned(64))) b)[n];
    T16 *p = 0;
    a[0] = 0;
    b[0] = 0;
    (void)p;
    if (_Alignof(T16) != 16) return 1;
    if (_Alignof(L16) != 16) return 2;
    if (__alignof__(a) != 64) return 3;
    if (__alignof__(b) != 64) return 4;
    if (__alignof__(*p) != 16) return 5;
    if (_Alignof(T1) != 1) return 6;
    if (sizeof(T16) != (unsigned long)n * sizeof(int)) return 7;
    if (__alignof__(__attribute__((aligned(16))) int[n]) != 16) return 8;
    if (__alignof__(__attribute__((aligned(16))) int[n][2]) != 16) return 9;
    if (_Alignof(int[n]) != _Alignof(int)) return 10;
    /* PLACEMENT, behind a run-time-sized neighbour of every length 1..24, so no base residue is assumed. */
    for (int k = 1; k <= 24; ++k) {
        char pad[k];
        T64 x;
        int (__attribute__((aligned(64))) y)[n];
        pad[0] = 1;
        x[0] = pad[0];
        y[n - 1] = x[0];
        if (((uintptr_t)x & 63u) != 0) return 11;
        if (((uintptr_t)y & 63u) != 0) return 12;
    }
    return 42 + a[0] + b[0];
}

int main(int argc, char **argv) {
    (void)argv;
    return vla_alignment(argc + 2);
}
