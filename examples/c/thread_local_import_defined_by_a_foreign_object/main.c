/* P69 (lane `cs`, D-LIR-THREAD-LOCAL-IMPORT-STAMPED-READ-THROUGH-A-SLOT): a DSS program that
 * only DECLARES two thread-locals, whose definitions come from a REFERENCE compiler's object.
 *
 * The prebuilt archive's one member is gcc 13.3.0's `-O2 -c` of
 *     _Thread_local int foreign_tls_shared = 7;
 *     _Thread_local long foreign_tls_tally;
 * (`.tdata` and `.tbss`), archived by GNU ar `rcsD`; the aarch64 one is the cross compiler's.
 *
 * MIR->LIR registered every DATA import as read through an address slot, a thread-local one
 * included, so the import row left stamped `readThroughSlot`, the link minted an address slot
 * for the definition and retargeted the thread-pointer access INTO it, and the ELF writer's
 * thread-local data-item backstop refused the image (K_RelocationKindMismatch) on both ELF
 * execs -- at HEAD 71648598 as in this round's tree -- where GNU ld and ld.lld link the same
 * program and it runs. A thread-local's access is the format's TLS sequence; it reads no slot.
 *
 * WHAT IT PROVES, PER THREAD: main reads the template's 7 and the zero-fill's 0, then writes
 * its own copies; a second thread reads ITS OWN copies (7 and 0, not main's 30 and 11), bumps
 * them, and returns 8; main's copies are untouched afterwards. A process-shared binding fails
 * the thread's first read (30 -> 130) and main's re-read; a template the link did not carry
 * fails main's first read (1); a zero-fill that is not zero fails 2 or 99.
 * Exit 42 = 30 + 8 + 4 when every check passed.
 */
#include <threads.h>

extern _Thread_local int foreign_tls_shared;
extern _Thread_local long foreign_tls_tally;

static int worker(void *arg) {
    (void)arg;
    if (foreign_tls_shared != 7) return 100 + foreign_tls_shared;   /* this thread's OWN copy */
    if (foreign_tls_tally != 0) return 99;                           /* .tbss: zero per thread */
    foreign_tls_shared = foreign_tls_shared + 1;                     /* 8 */
    foreign_tls_tally = foreign_tls_tally + 5;
    return foreign_tls_shared;
}

int main(void) {
    if (foreign_tls_shared != 7) return 1;    /* the definition's template, before any write */
    if (foreign_tls_tally != 0) return 2;
    foreign_tls_shared = 30;
    foreign_tls_tally = 11;
    thrd_t t;
    int r = 0;
    if (thrd_create(&t, worker, 0) != thrd_success) return 3;
    if (thrd_join(t, &r) != thrd_success) return 4;
    if (r != 8) return 5;
    if (foreign_tls_shared != 30 || foreign_tls_tally != 11) return 6;   /* the thread's writes stayed its own */
    return foreign_tls_shared + r + 4;   /* 30 + 8 + 4 = 42 */
}
