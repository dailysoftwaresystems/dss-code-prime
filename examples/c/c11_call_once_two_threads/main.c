/* call_once from TWO threads and main at once (P69): the initializer runs exactly once, and every caller returns only
 * after it has completed — C 7.28.2.1: "Completion of an effective call to the call_once function synchronizes with
 * all subsequent calls to the call_once function with the same value of flag", so each worker reads the counter after
 * its own call_once without a data race. On pe call_once is DSS's runtime unit over kernel32's InitOnceExecuteOnce
 * (runtime/platform/src/threads_once.c), on Mach-O libSystem's pthread_once under C's name, on ELF glibc's own.
 * A pair that ships <threads.h> must not define __STDC_NO_THREADS__ (C11 6.10.8.3 / C23 6.10.9.3), and every pair DSS
 * targets ships it — so the macro's absence is checked here, on all five. */
#include <stdio.h>
#include <threads.h>

#ifdef __STDC_NO_THREADS__
#error "__STDC_NO_THREADS__ is defined on a pair that ships <threads.h>"
#endif

static once_flag g_flag = ONCE_FLAG_INIT;
static int       g_runs = 0;

static void init(void) {
    ++g_runs;
}

static int worker(void *arg) {
    (void)arg;
    call_once(&g_flag, init);
    return g_runs;
}

int main(void) {
    thrd_t a;
    thrd_t b;
    int    ra = 0;
    int    rb = 0;
    if (thrd_create(&a, worker, NULL) != thrd_success) return 1;
    if (thrd_create(&b, worker, NULL) != thrd_success) return 1;
    call_once(&g_flag, init);
    int const mine = g_runs;
    thrd_join(a, &ra);
    thrd_join(b, &rb);
    printf("call_once ran its initializer %d time(s); every caller saw it: %d\n", g_runs,
           mine == 1 && ra == 1 && rb == 1);
    return g_runs == 1 && mine == 1 && ra == 1 && rb == 1 ? 42 : 1;
}
