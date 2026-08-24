/* D-CSUBSET-THREAD-LOCAL (TLS C3): the TRUE per-thread-storage runtime
 * discriminator on Windows — the pe64 analog of the ELF thread_local_pthread
 * witness (Windows has no pthread; kernel32 CreateThread is the native
 * thread-creation primitive).
 *
 * Single-thread runtime CANNOT distinguish real per-thread TLS from a
 * process-shared static alias — both pass "init visible + mutation sticks".
 * TWO threads can: main mutates its copy 5 -> 6, then two CreateThread
 * workers each do `counter += 100` and report what they observed.
 *
 *   real TLS:      each worker starts from the TEMPLATE (5) -> both see 105,
 *                  and main's copy is still 6.
 *   static alias:  the workers share ONE object seeded 6 -> they observe
 *                  {106, 206} in some order, and main's copy ends at 206.
 *
 * The OS calls the worker with the ms_x64 (Win64) convention (arg in rcx) —
 * the SAME convention DSS emits every pe64 function with, so the thread proc
 * receives its parameter correctly. CreateThread / WaitForSingleObject /
 * CloseHandle bind to kernel32.dll via <windows.h> (shippedLibs/windows.json).
 *
 * pe64-ONLY (windows.json availableObjectFormats:[pe]); the local Windows
 * ctest runs it natively. Exit 42. RED-on-disable: route isThreadLocal
 * globals through ordinary .data (the C1 asm section-select) and both workers
 * share one object -> v1/v2 become 106/206 and this returns 5/6, never 42.
 */
#include <windows.h>

thread_local int counter = 5;      /* .tls TEMPLATE (tdata) — init 5 */
static thread_local int z;         /* .tls TBSS (embedded-zero) — init 0 */

static unsigned long worker(void *arg) {
    int *out = (int *)arg;
    /* Each spawned thread must get its OWN embedded-zero tbss copy: z==0
     * here proves the per-thread block's zero region was initialized on
     * THIS thread (not just main). A process-shared alias makes the SECOND
     * worker see z != 0 (the first worker's +7). */
    if (z != 0) { *out = -1; return 0; }
    z = z + 7;                 /* this thread's tbss copy: 0 -> 7 */
    counter = counter + 100;   /* this thread's tdata copy: template 5 -> 105 */
    *out = counter + z;        /* 105 + 7 = 112 — folds BOTH copies in */
    return 0;
}

int main(void) {
    int v1 = 0;
    int v2 = 0;
    counter = 6;               /* main's tdata copy: 5 -> 6 (main leaves z at 0) */
    HANDLE t1 = CreateThread(0, 0, (void *)worker, &v1, 0, 0);
    HANDLE t2 = CreateThread(0, 0, (void *)worker, &v2, 0, 0);
    if (t1 == 0) return 1;
    if (t2 == 0) return 2;
    WaitForSingleObject(t1, INFINITE);
    WaitForSingleObject(t2, INFINITE);
    CloseHandle(t1);
    CloseHandle(t2);
    if (v1 != 112) return 5;   /* a shared alias gives 106/113/-1 etc. here */
    if (v2 != 112) return 6;
    if (counter != 6) return 7; /* workers must not touch main's tdata copy */
    if (z != 0) return 8;       /* workers must not touch main's tbss copy */
    return 42;
}
