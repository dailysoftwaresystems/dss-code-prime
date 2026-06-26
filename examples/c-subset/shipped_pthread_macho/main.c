/* c15c — shipped <pthread.h> on macOS: the first real consumer of the c15a
 * TYPEDEF-variant AND MACRO-variant mechanisms. pthread_mutex_t is 64 bytes on
 * macOS (an 8-byte long __sig + a 56-byte opaque buffer) vs 48 on Linux —
 * shipped as a per-format typedef variant (arr<i64,8> macho / arr<i64,6> elf).
 * The mutex lifecycle (init/lock/unlock/destroy) runs via libSystem (&m coerces
 * to the void* params; runtime init). PTHREAD_MUTEX_RECURSIVE = 2 on macOS (1 on
 * Linux). PTHREAD_MUTEX_INITIALIZER on macOS REQUIRES the magic __sig
 * _PTHREAD_MUTEX_SIG_init = 0x32AAABA7 = 850045863 in the first 8 bytes (Linux is
 * all-zero {0}) — `sm` is a static-initialized mutex and we read its __sig word
 * directly (no lock, safe) to witness the macro variant.
 *
 * RED-ON-DISABLE: a broken typedef-variant selector gives macho the elf size
 * (sizeof 48 != 64 — under-allocating the real 64B macOS mutex) → exit 1; a
 * broken constant selector gives RECURSIVE 1 != 2; a broken MACRO selector gives
 * the elf {0} initializer → __sig 0 != 850045863 → exit 1 (and an INVALID static
 * mutex on real macOS). */
#include <pthread.h>

static pthread_mutex_t sm = PTHREAD_MUTEX_INITIALIZER;

int main(void) {
    pthread_mutex_t m;
    pthread_mutex_init(&m, (void*)0);   /* &m (the 64B opaque buffer) coerces to void*; default attr */
    int a = pthread_mutex_lock(&m);
    int b = pthread_mutex_unlock(&m);
    pthread_mutex_destroy(&m);
    long sig = *(long*)&sm;              /* the Darwin __sig magic from the static initializer */
    return (a == 0 && b == 0
            && sizeof(pthread_mutex_t) == 64
            && PTHREAD_MUTEX_RECURSIVE == 2
            && sig == 850045863) ? 42 : 1;
}
