/* FC17.9(a) Cycle 3 (D-CSUBSET-C11-THREADS-TIMED) — the C11 `thrd_equal` witness.
 *
 * ★★ THE LOAD-BEARING CASE IS THE CROSS ONE: a thread asking whether ITS OWN
 * `thrd_current()` names the same thread as the `thrd_t` its creator holds. That
 * is the only comparison that can distinguish a correct implementation from a
 * plausible wrong one, and on pe it is precisely where a plausible wrong one
 * fails. pe's thrd_t IS a Win32 HANDLE, but `thrd_current()` answers kernel32's
 * PSEUDO-handle (HANDLE)-2, so comparing the two handle VALUES reports NOT-EQUAL
 * for the very thread doing the asking — a wrong answer with no fault and no
 * diagnostic. The shim compares GetThreadId of each handle instead, which resolves
 * the pseudo-handle to the caller's real thread id. Delete that indirection and
 * this program exits 43 instead of 42.
 *
 * ★ THERE IS NO RACE AND NO FLAG. `thrd_create` cannot hand the new thread its own
 * thrd_t (the handle exists only after the call returns), so the publication is
 * ordered by a MUTEX main holds from before the thread is created: the worker's
 * first act is to take that mutex, which cannot succeed until main has stored
 * `g_worker` and released it. No polled flag, no `volatile`, nothing an optimizer
 * could hoist.
 *
 * exit = 42 + worker_verdict, so a false NOT-EQUAL inside the thread shifts it to
 * 43 and both witnesses stay load-bearing.
 *
 * elf x86_64 + arm64 (qemu) resolve thrd_equal as DIRECT libc.so.6 FFI
 * (thrd_equal@@GLIBC_2.28, `nm -D` verified on both arches). pe64 SYNTHESIZES it
 * over kernel32 GetThreadId; macho DIRECT-PASSes it to libSystem's pthread_equal,
 * where thrd_current() is already a real pthread_self() id and no indirection is
 * needed. RED-on-disable: delete shippedLibs/threads.json and the #include fires
 * F_ShippedHeaderNotFound. */
#include <threads.h>

static mtx_t  g_gate;     /* held by main until g_worker has been published */
static thrd_t g_worker;   /* the creator's view of the worker's identity */

static int worker(void *unused) {
    int same;
    (void)unused;
    mtx_lock(&g_gate);    /* blocks until main has stored g_worker */
    same = thrd_equal(thrd_current(), g_worker);
    mtx_unlock(&g_gate);
    return same != 0 ? 0 : 1;   /* 0 = the identity round-tripped */
}

int main(void) {
    thrd_t self;
    thrd_t t;
    int    verdict = 99;

    /* Reflexivity, on the calling thread, before any of the rest matters. */
    self = thrd_current();
    if (thrd_equal(self, self) == 0) return 10;

    if (mtx_init(&g_gate, mtx_plain) != thrd_success) return 11;
    if (mtx_lock(&g_gate) != thrd_success) return 12;
    if (thrd_create(&t, worker, (void *)0) != thrd_success) return 13;
    g_worker = t;

    /* Two DIFFERENT threads must not compare equal. Checked while the worker is
     * still parked on the gate, so `t` is a live thread rather than a stale id. */
    if (thrd_equal(t, thrd_current()) != 0) return 14;

    if (mtx_unlock(&g_gate) != thrd_success) return 15;
    if (thrd_join(t, &verdict) != thrd_success) return 16;
    mtx_destroy(&g_gate);

    return 42 + verdict;   /* 42 iff the worker recognised its own thrd_t */
}
