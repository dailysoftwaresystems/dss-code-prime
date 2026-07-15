/* FC17.9(a) Cycle 2 (D-CSUBSET-C11-THREADS-TRAMPOLINES) - the C11 <threads.h>
 * thread-CREATION + call_once witness. A REAL second thread whose return value
 * round-trips through thrd_join, plus a one-shot initializer that must fire
 * EXACTLY once across two call_once invocations.
 *
 * On elf x86_64 + arm64 (qemu) thrd_create/thrd_join/call_once are DIRECT
 * libc.so.6 FFI (glibc>=2.28 exports them; verified `nm -D`). On pe64 each is a
 * COMPILER-SYNTHESIZED shim (synth_threads_shim.cpp): thrd_create DIRECT-PASSes
 * `worker` to kernel32 CreateThread (the C11 int(*)(void*) start routine has the
 * same x64 ABI as Win32's DWORD(*)(void*)); thrd_join is a MULTI-block shim
 * (WaitForSingleObject; if(res) GetExitCodeThread; CloseHandle); call_once rides
 * InitOnceExecuteOnce via a module-scoped __dss_once_tramp adapter that invokes
 * the C11 void(*)(void).
 *
 * FOLD-RESISTANT: a real CreateThread / pthread_create cannot be const-folded, so
 * the release arm exercises the true spawn+join path. The exit code is
 *   worker(&2) + (g_once_count - 1) == 42 + (1 - 1) == 42
 * so a call_once that fired 0x or 2x (g_once_count 0 or 2) SHIFTS the exit code
 * to 41/43 - both witnesses are load-bearing.
 *
 * On macho (arm64) each is a pthread shim: thrd_create DIRECT-PASSes worker to
 * pthread_create, thrd_join is a pthread_join into a stack void-star slot then a
 * truncate, and call_once is a DIRECT pthread_once. The macho arm RUNS natively
 * on a macOS-arm64 host (a real pthread spawn). RED-on-disable: delete
 * shippedLibs/threads.json and the #include fires F_ShippedHeaderNotFound and the
 * compile fails. */
#include <threads.h>

static int       g_once_count = 0;
static once_flag g_flag       = ONCE_FLAG_INIT;

static void bump_once(void) { g_once_count = g_once_count + 1; }

static int worker(void *a) { return *(int *)a + 40; }   /* 2 + 40 = 42 */

int main(void) {
    thrd_t t;
    int    arg = 2;
    int    r   = 0;

    /* call_once: bump_once runs EXACTLY once across the two calls -> g_once_count == 1 */
    call_once(&g_flag, bump_once);
    call_once(&g_flag, bump_once);

    /* a REAL second thread: worker returns arg+40 = 42, round-tripped via thrd_join */
    thrd_create(&t, worker, &arg);
    thrd_join(t, &r);

    return r + (g_once_count - 1);   /* 42 + (1 - 1) = 42 */
}
