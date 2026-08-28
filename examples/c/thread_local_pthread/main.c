/* D-CSUBSET-THREAD-LOCAL (TLS C1): the TRUE per-thread-storage runtime
 * discriminator (the design-audit's recommended witness).
 *
 * Single-thread runtime CANNOT distinguish real TLS from a process-shared
 * static alias — both pass "init visible + mutation sticks". TWO threads
 * can: main mutates its copy 5 -> 6, then two pthread workers each do
 * `counter += 100` and report what they observed.
 *
 *   real TLS:      each worker starts from the TEMPLATE (5) -> both see
 *                  105, and main's copy is still 6.
 *   static alias:  the workers share ONE object seeded 6 -> they observe
 *                  {106, 206} in some order (or a torn interleaving),
 *                  and main's copy ends at 206.
 *
 * pthread_create/pthread_join live IN libc.so.6 since glibc 2.34, so the
 * raw `extern` declarations below bind through the language's default
 * extern-library config (externLibraryByFormat -> libc.so.6) — the same
 * mechanism every FFI example uses; zero pthread-specific config.
 *
 * x86_64-linux only until TLS C2/C3/C4 land the other legs. Exit 42.
 */
typedef unsigned long pthread_t;

thread_local int counter = 5;

extern int pthread_create(pthread_t *t, void *attr,
                          void *(*fn)(void *), void *arg);
extern int pthread_join(pthread_t th, void **ret);

static void *worker(void *arg) {
    int *out = (int *)arg;
    counter = counter + 100;   /* this thread's copy: template 5 -> 105 */
    *out = counter;
    return 0;
}

int main(void) {
    pthread_t t1;
    pthread_t t2;
    int v1 = 0;
    int v2 = 0;
    counter = 6;               /* main's copy: 5 -> 6 */
    if (pthread_create(&t1, 0, worker, &v1) != 0) return 1;
    if (pthread_create(&t2, 0, worker, &v2) != 0) return 2;
    if (pthread_join(t1, 0) != 0) return 3;
    if (pthread_join(t2, 0) != 0) return 4;
    if (v1 != 105) return 5;   /* a shared alias gives 106/206 here */
    if (v2 != 105) return 6;
    if (counter != 6) return 7; /* workers must not touch main's copy */
    return 42;
}
