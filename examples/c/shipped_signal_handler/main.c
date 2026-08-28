/* c81 (the sqlite shell.c CLI route): ship <signal.h> — shell.c registers
 * SIGINT/SIGTERM/SIGHUP handlers via `signal(SIG, handler)` (shell.c:38171+)
 * and raises SIGTRAP under SQLITE_DEBUG_BREAK (38165); the header was missing
 * -> F001A. This example is the END-TO-END proof of the whole surface:
 *   1. SIGINT must be PP-VISIBLE (shell.c guards each registration with
 *      `#ifdef SIGINT` — signal.json ships the SIG* numbers on the `macros`
 *      surface, NOT `constants`; a semantic-only constant would silently
 *      compile the handler out). The #ifdef below mirrors shell.c exactly:
 *      if SIGINT were not a macro, this program would return 1, not 42.
 *   2. `signal` takes/returns a FUNCTION POINTER (ptr<fn(i32) -> void>) —
 *      the bare-designator argument below is shell.c's exact shape.
 *   3. The handler must actually RUN on real libc: raise(SIGINT) delivers
 *      synchronously (POSIX: raise returns after the handler completes), the
 *      handler writes the volatile flag, main observes it -> exit 42.
 * RED-ON-DISABLE: delete signal.json -> F001A on the include; move the SIG*
 * macros to `constants` -> the #ifdef arm vanishes -> exit 1. */
#include <signal.h>

static volatile int g_hit = 0;

static void on_sigint(int sig) {
    if (sig == SIGINT) g_hit = 1;
}

int main(void) {
    int registered = 0;
#ifdef SIGINT
    signal(SIGINT, on_sigint);   /* shell.c:38172's exact registration shape */
    registered = 1;
#endif
    if (!registered) return 1;   /* SIGINT invisible to the preprocessor */
    if (raise(SIGINT) != 0) return 2;
    if (g_hit != 1) return 3;    /* the handler did not run */
    if (SIGHUP != 1 || SIGTERM != 15 || SIGTRAP != 5) return 4;  /* glibc/BSD values */
    return 42;
}
