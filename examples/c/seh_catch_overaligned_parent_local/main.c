/* c116b x C11/C23 6.7.5: a SEH `__except` FILTER reading OVER-ALIGNED parent
 * locals — D-WIN64-SEH-FUNCLETS H1 crossed with
 * D-CSUBSET-ALIGNAS-OVERALIGNED-STACK-LOCAL.
 *
 * WHY THIS COMBINATION IS ITS OWN CASE. The filter runs as a SEPARATE ms_x64
 * function at fault time and reads a parent local off the ESTABLISHER frame (the
 * parent's post-prologue SP) at the parent's own slot offset. For an ordinary local
 * that raw slot address IS the local. For an OVER-ALIGNED one it is not: the parent
 * rounds it up at run time, because no compile-time offset can carry an alignment
 * finer than the stack's own. So the funclet has to apply the IDENTICAL rounding to
 * the identical base, or the two functions address one local two different ways.
 * Until D-CSUBSET-ALIGNAS-OVERALIGNED-STACK-LOCAL closed, this program did not
 * compile at all (the whole construct was refused), so there is no history behind
 * the funclet's raw read being adequate — it is new ground and this pins it.
 *
 * ★★ WHY THERE ARE TWO OVER-ALIGNED LOCALS WITH A PLAIN ONE BETWEEN THEM, AND WHY
 * ONE WOULD HAVE BEEN A HALF-BLIND PIN. The funclet's error, if the rounding is
 * dropped, is `alignUp(base, 32) - base` — which is ZERO whenever `base` happens to
 * already be 32-aligned. `base` is the runtime stack pointer plus a frame offset, so
 * with a SINGLE local a broken build reads the right address about half the time and
 * the pin would pass at random. Two over-aligned locals separated by an ordinary one
 * sit 48 bytes apart in the frame (16-byte slot, 16 bytes of alignment headroom,
 * then the 16-byte slot of `gap`), and 48 is 16 modulo 32 — so whatever the stack
 * pointer's residue, EXACTLY ONE of the two needs rounding. A build that drops it
 * always misreads one of them.
 *
 *   42  the OS caught the AV AND the funclet recovered BOTH over-aligned parent
 *       locals at the addresses the parent uses (SUCCESS).
 *   10  VirtualAlloc failed (environment, not SEH).
 *   (a crash / no output)  the funclet read one of them at the UNROUNDED address,
 *       the filter was FALSE, CONTINUE_SEARCH, and the AV escaped uncaught.
 *
 * Unforgeable in the same way `seh_catch_parent_local` is: a wrong recovered value
 * does not fail an assertion, it fails to catch, and the process dies.
 *
 * pe64-ONLY: x64 SEH + __C_specific_handler + windows.h are Windows.
 */
#include <windows.h>

int main(void) {
    void *p = VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    if (p == 0) {
        return 10;
    }

    /* Two over-aligned parent locals with an ordinary one between them: their frame
     * offsets differ by 16 modulo 32, so exactly one of the two is misread by a
     * funclet that skips the rounding, whatever the stack pointer's residue. */
    alignas(32) int first  = 42;
    char            gap    = 7;
    alignas(32) int second = 24;
    int rc = 0;

    __try {
        rc = *(volatile int *)p;   /* → access violation */
    } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION)
                & (first == 42) & (second == 24) & (gap == 7)) {
        rc = first;   /* 42 iff the funclet recovered both over-aligned locals */
    }

    return rc;
}
