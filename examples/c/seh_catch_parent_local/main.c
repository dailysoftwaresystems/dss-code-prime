/* c116b (D-WIN64-SEH-FUNCLETS, H1): the RUNTIME witness that a SEH `__except` FILTER
 * can read a PARENT LOCAL — proving H1 (RecoverParentFrameSlot) recovers the parent
 * frame's live value in the funclet, exactly as sqlite's
 * `sehExceptionFilter(pWal, GetExceptionCode(), ...)` needs.
 *
 * The filter FUNCLET runs as a SEPARATE ms_x64 function at fault time. A parent local
 * is not visible to it as an SSA value — it must be recovered off the establisher
 * frame (arg1) via RecoverParentFrameSlot → `[establisher + parentFrameOffset]`. Here
 * the filter is `(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION) & (marker==42)`,
 * reading the parent local `marker`. Bitwise `&` (not `&&`) keeps it a single-block
 * filter (the shape c115 lowers). The __except body then sets rc = marker.
 *
 *   42  the OS caught the AV AND the funclet recovered `marker`==42 from the parent
 *       frame (SUCCESS — the filter matched and the handler read the same local).
 *   10  VirtualAlloc failed (environment, not SEH).
 *   (a crash / no output)  H1 is broken: the funclet read GARBAGE for `marker`, the
 *       filter (marker==42) was FALSE → CONTINUE_SEARCH → the AV escaped uncaught.
 *
 * Unforgeable: if the funclet did not recover the TRUE parent-frame value of `marker`
 * the filter would not match and the program crashes rather than returning 42 (a
 * negative control — `marker` set to any non-42 value crashes instead of catching).
 *
 * pe64-ONLY: x64 SEH + __C_specific_handler + windows.h are Windows. RED-on-disable:
 * revert the H1 RecoverParentFrameSlot lowering (or the SEH-filter param-escape) →
 * the filter's parent-local read fails loud at compile (D-WIN64-SEH-FUNCLETS), so
 * this example does not build (never a silent skip).
 */
#include <windows.h>

int main(void) {
    void *p = VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    if (p == 0) {
        return 10;
    }

    int marker = 42;   /* the parent local the filter must recover from the frame */
    int rc = 0;
    __try {
        rc = *(volatile int *)p;   /* → access violation */
    } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) & (marker == 42)) {
        rc = marker;   /* 42 iff the funclet recovered `marker` from the parent frame */
    }

    return rc;
}
