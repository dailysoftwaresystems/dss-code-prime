/* c116b (D-WIN64-SEH-FUNCLETS): the RUNTIME witness that a MULTI-BLOCK `__try`
 * guarded body (a loop + conditionals) is caught. sqlite's SEH_TRY bodies are large
 * multi-block regions (loops, nested if/else, function calls); the scope-table
 * [BeginAddress, EndAddress) must cover the WHOLE guarded region as ONE contiguous PC
 * range. c116b lays the guarded body's blocks out CONTIGUOUSLY in .text (the region-
 * contiguity relayout — the optimizer's RPO order otherwise interleaves the join /
 * handler between body blocks) so [Begin,End) covers exactly the body and no non-
 * region block. Here the body runs a loop (summing 0..3), then faults on a no-access
 * read; the fault inside the multi-block body must dispatch to the __except handler.
 *
 *   42  the OS caught the AV raised INSIDE the multi-block guarded body (SUCCESS).
 *   10  VirtualAlloc failed (environment, not SEH).
 *   (a crash / no output)  the scope-table range did not cover the faulting block
 *       (the region was not laid out contiguously) → the AV escaped uncaught.
 *
 * pe64-ONLY: x64 SEH + windows.h are Windows. RED-on-disable: revert the c116b
 * region-contiguity relayout → a multi-block guarded body fails loud at compile
 * (D-WIN64-SEH-FUNCLETS), so this example does not build (never a silent skip).
 */
#include <windows.h>

int main(void) {
    void *p = VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    if (p == 0) {
        return 10;
    }

    int rc = 0;
    int sum = 0;
    __try {
        /* a MULTI-block guarded body: a loop, then a faulting read */
        for (int i = 0; i < 4; i = i + 1) {
            sum = sum + i;
        }
        rc = *(volatile int *)p;   /* → access violation, inside the multi-block body */
        rc = rc + sum;             /* unreached — the fault above dispatches out */
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) {
        rc = 42;
    }

    return rc;
}
