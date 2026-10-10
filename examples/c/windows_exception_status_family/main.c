/* The EXCEPTION_* status names of <windows.h>, by value.
 *
 * `GetExceptionCode()` answers one of these, and a `__try` filter compares it
 * against them by NAME. The shipped header declared two of the family
 * (EXCEPTION_ACCESS_VIOLATION and EXCEPTION_IN_PAGE_ERROR); a filter naming any
 * other — an integer division by zero is the everyday one — did not compile.
 *
 * Every value below is the SDK header's own, ✔MEASURED 2026-10-08 with the
 * reference compiler (cl 19.51 x64, a program printing each name the header
 * defines): this same file compiles there, so each assertion is a comparison of
 * the shipped header against the vendor's. A wrong value in the shipped header
 * is a failed assertion on EVERY host that compiles this file, not only where
 * the image runs.
 *
 * Not declared, on purpose: EXCEPTION_POSSIBLE_DEADLOCK — the SDK header defines
 * it as STATUS_POSSIBLE_DEADLOCK, which <windows.h> alone does not declare, so
 * the reference compiler itself refuses a program that names it.
 *
 * At run time the program catches an integer division by zero by its name and
 * answers 42; an escaped fault is a crash, never another exit code.
 *
 * pe64 only: the header and the handler routine are Windows.
 */
#include <windows.h>

_Static_assert(EXCEPTION_ACCESS_VIOLATION == 0xC0000005u, "EXCEPTION_ACCESS_VIOLATION");
_Static_assert(EXCEPTION_IN_PAGE_ERROR == 0xC0000006u, "EXCEPTION_IN_PAGE_ERROR");
_Static_assert(EXCEPTION_INVALID_HANDLE == 0xC0000008u, "EXCEPTION_INVALID_HANDLE");
_Static_assert(EXCEPTION_ILLEGAL_INSTRUCTION == 0xC000001Du, "EXCEPTION_ILLEGAL_INSTRUCTION");
_Static_assert(EXCEPTION_NONCONTINUABLE_EXCEPTION == 0xC0000025u, "EXCEPTION_NONCONTINUABLE_EXCEPTION");
_Static_assert(EXCEPTION_INVALID_DISPOSITION == 0xC0000026u, "EXCEPTION_INVALID_DISPOSITION");
_Static_assert(EXCEPTION_ARRAY_BOUNDS_EXCEEDED == 0xC000008Cu, "EXCEPTION_ARRAY_BOUNDS_EXCEEDED");
_Static_assert(EXCEPTION_FLT_DENORMAL_OPERAND == 0xC000008Du, "EXCEPTION_FLT_DENORMAL_OPERAND");
_Static_assert(EXCEPTION_FLT_DIVIDE_BY_ZERO == 0xC000008Eu, "EXCEPTION_FLT_DIVIDE_BY_ZERO");
_Static_assert(EXCEPTION_FLT_INEXACT_RESULT == 0xC000008Fu, "EXCEPTION_FLT_INEXACT_RESULT");
_Static_assert(EXCEPTION_FLT_INVALID_OPERATION == 0xC0000090u, "EXCEPTION_FLT_INVALID_OPERATION");
_Static_assert(EXCEPTION_FLT_OVERFLOW == 0xC0000091u, "EXCEPTION_FLT_OVERFLOW");
_Static_assert(EXCEPTION_FLT_STACK_CHECK == 0xC0000092u, "EXCEPTION_FLT_STACK_CHECK");
_Static_assert(EXCEPTION_FLT_UNDERFLOW == 0xC0000093u, "EXCEPTION_FLT_UNDERFLOW");
_Static_assert(EXCEPTION_INT_DIVIDE_BY_ZERO == 0xC0000094u, "EXCEPTION_INT_DIVIDE_BY_ZERO");
_Static_assert(EXCEPTION_INT_OVERFLOW == 0xC0000095u, "EXCEPTION_INT_OVERFLOW");
_Static_assert(EXCEPTION_PRIV_INSTRUCTION == 0xC0000096u, "EXCEPTION_PRIV_INSTRUCTION");
_Static_assert(EXCEPTION_STACK_OVERFLOW == 0xC00000FDu, "EXCEPTION_STACK_OVERFLOW");
_Static_assert(EXCEPTION_GUARD_PAGE == 0x80000001u, "EXCEPTION_GUARD_PAGE");
_Static_assert(EXCEPTION_DATATYPE_MISALIGNMENT == 0x80000002u, "EXCEPTION_DATATYPE_MISALIGNMENT");
_Static_assert(EXCEPTION_BREAKPOINT == 0x80000003u, "EXCEPTION_BREAKPOINT");
_Static_assert(EXCEPTION_SINGLE_STEP == 0x80000004u, "EXCEPTION_SINGLE_STEP");

_Static_assert(EXCEPTION_NONCONTINUABLE == 1, "EXCEPTION_NONCONTINUABLE");
_Static_assert(EXCEPTION_EXECUTE_HANDLER == 1, "EXCEPTION_EXECUTE_HANDLER");
_Static_assert(EXCEPTION_CONTINUE_SEARCH == 0, "EXCEPTION_CONTINUE_SEARCH");
_Static_assert(EXCEPTION_CONTINUE_EXECUTION == -1, "EXCEPTION_CONTINUE_EXECUTION");
_Static_assert(EXCEPTION_MAXIMUM_PARAMETERS == 15, "EXCEPTION_MAXIMUM_PARAMETERS");

static int divide(int by) {
    int rc = 0;
    __try {
        rc = 100 / by;
    } __except (GetExceptionCode() == EXCEPTION_INT_DIVIDE_BY_ZERO) {
        rc = 42;
    }
    return rc;
}

int main(void) {
    volatile int zero = 0;
    return divide(zero);
}
