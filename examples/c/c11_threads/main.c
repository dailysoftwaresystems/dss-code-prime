/* FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER + D-CSUBSET-C11-THREADS-MACHO) - the C11
 * <threads.h> witness across 3 vehicles. On elf (x86_64 + arm64) the mtx, cnd and tss
 * calls are DIRECT libc FFI (glibc>=2.34 exports the C11 thread API from libc.so.6); on
 * pe64 each is a COMPILER-SYNTHESIZED Win32 shim over kernel32 CRITICAL_SECTION,
 * CONDITION_VARIABLE and Fls primitives; on macho (arm64) each is a COMPILER-SYNTHESIZED
 * pthread shim over the libSystem pthread mutex, cond and key primitives - neither the
 * Windows CRT nor macOS libSystem exports the C11 thrd functions. Both synthesized
 * vehicles live in synth_threads_shim.cpp, selected by the format's `librarySynthesis`
 * block. A deterministic exit 42 with NO thread creation (thrd_create is Cycle 2) so the
 * witness is scheduler-free.
 *
 * The 40 + 2 arithmetic reads two MUTABLE globals, so the RELEASE pipeline cannot fold
 * them away - the release arm exercises the REAL synthesized-or-FFI path, not a constant.
 * RED-on-disable: delete shippedLibs/threads.json and the include of <threads.h> fires
 * F_ShippedHeaderNotFound and the compile fails. The macho arm RUNS natively on a
 * macOS-arm64 host (local ctest). */
#include <threads.h>

int g_base = 40;   /* mutable — defeats the optimizer's const-fold */
int g_two  = 2;

int main(void) {
    mtx_t m;
    cnd_t c;
    tss_t k;
    int   r = 0;
    int*  p;

    /* mutex round-trip: init, lock, read a runtime value under the lock, unlock, destroy */
    mtx_init(&m, mtx_plain);
    mtx_lock(&m);
    r = g_base;                 /* 40, inside the critical section */
    mtx_unlock(&m);
    mtx_destroy(&m);

    /* condition variable: init + destroy (Cycle 1 has no thrd_create to wait on) */
    cnd_init(&c);
    cnd_destroy(&c);

    /* thread-specific storage round-trip: create a key, store a pointer, read it back */
    tss_create(&k, 0);          /* NULL destructor */
    tss_set(k, &g_two);
    p = (int*)tss_get(k);
    r = r + *p;                 /* 40 + 2 = 42 */
    tss_delete(k);

    return r;                   /* 42 */
}
