/* c116 (D-WIN64-SEH-FUNCLETS): the RUNTIME witness that DSS's x64 SEH funclet +
 * __C_specific_handler scope table actually CATCHES a hardware fault on Windows.
 *
 * This exercises the WHOLE chain unforgeably: the parent UNWIND_INFO's
 * UNW_FLAG_EHANDLER + trailing __C_specific_handler thunk RVA + SCOPE_TABLE, the
 * filter FUNCLET (a separate ms_x64 function reading the exception code from the
 * dispatch context via rcx=EXCEPTION_POINTERS*), and the OS's post-filter jump
 * into the __except body (JumpTarget). If ANY of those is wrong the program
 * crashes (0xC0000005 escapes) instead of returning 42 — a native, unforgeable
 * proof. There is no silent pass: the fault MUST be dispatched to the handler.
 *
 * main commits a PAGE_NOACCESS page, reads it inside a __try (→ an access
 * violation), and the __except filter is `GetExceptionCode() ==
 * EXCEPTION_ACCESS_VIOLATION` (a single-block filter over ONLY the exception
 * code — no parent local; that is c116b). On EXECUTE_HANDLER the handler sets
 * rc = 42.
 *   10  VirtualAlloc failed (couldn't set up the fault) — environment, not SEH.
 *   42  the OS caught the AV and dispatched to the __except body (SUCCESS).
 *   (a crash / no output) the scope table / funclet / dispatch is WRONG.
 *
 * pe64-ONLY: x64 SEH + __C_specific_handler + windows.h are Windows
 * (windows.json is availableObjectFormats:["pe"]). RED-on-disable: revert the
 * SEH funclet/scope-table lowering → the __try/__except fails loud at compile
 * (D-WIN64-SEH-FUNCLETS), so this example does not build (never a silent skip).
 */
#include <windows.h>

int main(void) {
    /* A single no-access page: reading it raises EXCEPTION_ACCESS_VIOLATION. */
    void *p = VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    if (p == 0) {
        return 10;   /* environment failure — not an SEH result */
    }

    int rc = 0;
    __try {
        rc = *(volatile int *)p;                              /* → access violation */
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) {
        rc = 42;     /* reached iff the OS caught the AV + dispatched here */
    }

    return rc;       /* 42 on success */
}
