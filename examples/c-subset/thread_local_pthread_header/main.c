/* D-CSUBSET-THREAD-LOCAL (TLS C1): the REAL-INCLUDE form of the
 * per-thread-storage discriminator, composed with an actual mutex.
 *
 * The raw-extern twin (thread_local_pthread) pins the default
 * extern->libc binding mechanism; THIS example pins the shipped
 * <pthread.h> path (src/dss-config/shippedLibs/pthread.json): the real
 * include, the per-format opaque pthread_mutex_t typedef (elf:
 * arr<i64,6>), the per-format PTHREAD_MUTEX_INITIALIZER macro (elf:
 * {0}) as a file-scope static initializer, and the mutex lock/unlock
 * API guarding a shared accumulator while thread_local does the
 * per-thread part.
 *
 *   real TLS:      each worker's counter starts from the TEMPLATE (5)
 *                  -> both add 105 -> sum == 210, main's copy still 6.
 *   static alias:  the workers share main's object (6) -> sum becomes
 *                  106+206 = 312 (or a torn interleave), main ends 206.
 *
 * pthread_create's shipped 3rd parameter is ptr<void> (the neutral
 * descriptor keeps the opaque-buffer API pointer-typed), so the
 * function name is passed via an explicit (void *) cast — the bare
 * name does not implicitly convert fn -> void* in the c-subset.
 *
 * x86_64-linux only until TLS C2/C3/C4 land the other legs. Exit 42.
 */
#include <pthread.h>

thread_local int counter = 5;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int sum = 0;

static void *worker(void *arg) {
    counter = counter + 100;      /* this thread's copy: template 5 -> 105 */
    pthread_mutex_lock(&lock);
    sum = sum + counter;          /* mutex-guarded shared accumulator */
    pthread_mutex_unlock(&lock);
    return 0;
}

int main(void) {
    pthread_t t1;
    pthread_t t2;
    counter = counter + 1;        /* main's copy: 5 -> 6 */
    if (pthread_create(&t1, 0, (void *)worker, 0) != 0) return 1;
    if (pthread_create(&t2, 0, (void *)worker, 0) != 0) return 2;
    if (pthread_join(t1, 0) != 0) return 3;
    if (pthread_join(t2, 0) != 0) return 4;
    if (sum != 210) return 5;     /* 105 + 105 — each worker saw the TEMPLATE */
    if (counter != 6) return 6;   /* workers must not touch main's copy */
    return 42;
}
