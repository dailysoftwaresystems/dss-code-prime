/* TF-cycle (D-FFI-TCL-DESCRIPTOR / pthread completion): the runnable witness for
 * the pthread surface test4.c + test_thread.c need beyond mutex+create+join —
 * a REAL condition-variable handshake + a DETACHED thread + the tightened
 * pthread_create start-routine.
 *
 * Exercises (each red-on-disable — drop the symbol from pthread.json -> S0001,
 * or loosen pthread_create's 3rd param back to ptr<void> and the bare `worker`
 * designator stops matching):
 *   - pthread_create  : the 3rd param is now the PRECISE fn-ptr
 *                       ptr<fn(ptr<void>) -> ptr<void>>, so the bare `worker`
 *                       function designator (void*(void*)) decays straight to it
 *                       — the exact shape test4:160 passes (test_thread_main) and
 *                       sqlite3.c:35852 passes (xTask).
 *   - pthread_detach  : the created thread is DETACHED (never joined) — so its
 *                       completion can ONLY be observed through the condvar, which
 *                       is precisely why a cond handshake is the right witness.
 *   - pthread_cond_init / _wait / _signal / _destroy + the opaque pthread_cond_t
 *                       (48-byte 8-aligned buffer, same on elf+macho).
 *   - reuses pthread_mutex_init / _lock / _unlock / _destroy (already shipped).
 *
 * Handshake (deterministic, race-free — the textbook predicate-loop idiom):
 *   main locks m, creates + detaches worker, then cond_wait's (atomically
 *   releasing m). worker can only then lock m, add n (42) into the guarded
 *   accumulator, set ready, signal, unlock. main re-acquires m, sees ready,
 *   leaves the loop, unlocks, and destroys. The worker's acc write happens-before
 *   main's read (mutex release/acquire), so exit == acc == 42 EVERY run; a broken
 *   condvar (lost signal / no wait) would hang or exit 0, not 42.
 *
 * Value-divergent: the 42 is produced by the worker thread through the condvar,
 * not foldable at compile time (a real thread + real libc cond primitives).
 *
 * Gated [elf, macho] (pthread is unix; a pe build uses os_win). 3-leg note: the
 * worker must actually RUN + signal on WSL-elf + qemu-arm64, not just compile. */
#include <pthread.h>

static pthread_mutex_t m;
static pthread_cond_t  c;
static int ready = 0;
static int acc   = 0;

static void *worker(void *arg) {
    int *p = (int *)arg;
    pthread_mutex_lock(&m);
    acc = acc + *p;            /* guarded: main is parked in cond_wait here */
    ready = 1;
    pthread_cond_signal(&c);
    pthread_mutex_unlock(&m);
    return 0;
}

int main(void) {
    pthread_t t;
    int n = 42;
    if (pthread_mutex_init(&m, 0) != 0) return 1;
    if (pthread_cond_init(&c, 0) != 0) return 2;

    pthread_mutex_lock(&m);
    /* bare `worker` (void*(void*)) decays to the tightened fn-ptr 3rd param */
    if (pthread_create(&t, 0, worker, &n) != 0) return 3;
    if (pthread_detach(t) != 0) return 4;      /* detached -> observe via condvar */
    while (ready == 0)
        pthread_cond_wait(&c, &m);             /* releases m; worker signals */
    pthread_mutex_unlock(&m);

    pthread_cond_destroy(&c);
    pthread_mutex_destroy(&m);
    return acc;                                /* the worker added 42 -> exit 42 */
}
