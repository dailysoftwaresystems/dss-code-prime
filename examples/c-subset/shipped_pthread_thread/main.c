/* c51 (D-FFI-PTHREAD-CREATE-JOIN): ship pthread_create + pthread_join from
 * <pthread.h> — the two symbols sqlite's threaded sorter needs (sqlite3.c:35646
 * `rc = pthread_create(&p->tid, 0, xTask, pIn)` + the matching pthread_join).
 * The rest of the pthread surface (mutex API + pthread_self/equal + the opaque
 * pthread_mutex_t/_t typedefs) was already shipped in c14/c15; only these two
 * were missing -> S0001 undeclared.
 *   pthread_create: fn(ptr<void>,ptr<void>,ptr<void>,ptr<void>) -> i32  (all four
 *     args are pointer-position: pthread_t*, attr*, start_routine, arg).
 *   pthread_join:   fn(u64, ptr<void>) -> i32   (the pthread_t thread-id BY VALUE
 *     = u64 — the established pthread_self/equal ABI; void** result out-param).
 *
 * The start routine is passed as a function-POINTER variable (`fn`), exactly as
 * sqlite passes `xTask` — so it converts via the Ptr->void* arm (a bare function
 * designator -> void* is a SEPARATE unshipped gap, NOT exercised here).
 *
 * Gated [elf,macho] (pthread is unix; a pe build uses os_win). RED-ON-DISABLE:
 * remove either symbol from pthread.json -> S0001. */
#include <pthread.h>

static int g = 0;

void *worker(void *arg) {
    g = 42;            /* side effect proves the thread actually RAN */
    return arg;
}

int main(void) {
    pthread_t t;
    void *r = 0;
    void *(*fn)(void *) = worker;   /* a fn-POINTER (sqlite's xTask shape) */
    if (pthread_create(&t, 0, fn, 0) != 0) return 1;
    if (pthread_join(t, &r) != 0) return 2;
    return g;                       /* the worker ran on the real libc thread -> 42 */
}
