/* c14e — shipped <pthread.h> witness, the 6th and last POSIX header. The mutex
 * lifecycle: a pthread_mutex_t (an opaque 48-byte 8-aligned buffer, the max of
 * the per-arch glibc sizes) is init/lock/unlock/destroy'd via libc (dynamically
 * linked); &mutex coerces to the symbols' void* param (standard C). On a real
 * Linux runtime lock/unlock return 0 → exit 42. RED-ON-DISABLE: drop the
 * pthread.json surfaces → pthread_mutex_t/pthread_mutex_lock undefined → compile
 * fails. */
#include <pthread.h>

int main(void) {
    pthread_mutex_t m;
    pthread_mutex_init(&m, (void*)0);   /* &m (the opaque buffer) coerces to void*; default attr */
    int a = pthread_mutex_lock(&m);
    int b = pthread_mutex_unlock(&m);
    pthread_mutex_destroy(&m);
    return (a == 0 && b == 0 && PTHREAD_MUTEX_RECURSIVE == 1) ? 42 : 1;
}
