/* The C type of every constant the shipped <windows.h> declares.
 *
 * A constant's TYPE is part of its meaning. Under both reference compilers
 * `MEM_COMMIT` is an `int`, `ERROR_FILE_NOT_FOUND` a `long` and
 * `EXCEPTION_ACCESS_VIOLATION` an `unsigned long`. The shipped header declared
 * all three — and 97 more — as `unsigned int`, so
 *
 *     -1 < MEM_COMMIT
 *
 * was FALSE (the comparison became an unsigned one) where both reference
 * compilers answer TRUE, and `_Generic`, a pointer to the constant's type and
 * the conversion of a negative operand all saw a type the references do not.
 *
 * Every assertion below is the reference compilers' own answer, ✔MEASURED
 * 2026-10-10 by a program that prints each name's type with `_Generic`: MSVC
 * cl 19.51 x64 and MinGW-w64 gcc 13.2.0 printed the IDENTICAL table for all
 * 117 names (63 `int`, 22 `long`, 31 `unsigned long`,
 * 1 `unsigned int`), and this same file compiles under both. A wrong
 * type in the shipped header is a failed assertion on EVERY host that
 * compiles this file, not only where the image runs.
 *
 * At run time the program makes the comparisons whose ANSWER depends on the
 * type — each with an operand the compiler cannot fold — and answers 42.
 *
 * pe64 only: the header is Windows'.
 */
#include <windows.h>

#define IS(name, type) _Static_assert(_Generic((name), type: 1, default: 0), #name " is " #type)

/* `int` */
IS(TRUE, int);
IS(FALSE, int);
IS(MAX_PATH, int);
IS(FILE_SHARE_READ, int);
IS(FILE_SHARE_WRITE, int);
IS(FILE_SHARE_DELETE, int);
IS(CREATE_NEW, int);
IS(CREATE_ALWAYS, int);
IS(OPEN_EXISTING, int);
IS(OPEN_ALWAYS, int);
IS(TRUNCATE_EXISTING, int);
IS(FILE_ATTRIBUTE_READONLY, int);
IS(FILE_ATTRIBUTE_HIDDEN, int);
IS(FILE_ATTRIBUTE_DIRECTORY, int);
IS(FILE_ATTRIBUTE_NORMAL, int);
IS(FILE_ATTRIBUTE_TEMPORARY, int);
IS(FILE_FLAG_DELETE_ON_CLOSE, int);
IS(FILE_FLAG_RANDOM_ACCESS, int);
IS(FILE_FLAG_OVERLAPPED, int);
IS(FILE_BEGIN, int);
IS(FILE_CURRENT, int);
IS(FILE_END, int);
IS(FILE_MAP_WRITE, int);
IS(FILE_MAP_READ, int);
IS(PAGE_READONLY, int);
IS(PAGE_READWRITE, int);
IS(PAGE_WRITECOPY, int);
IS(SECTION_MAP_WRITE, int);
IS(SECTION_MAP_READ, int);
IS(LMEM_FIXED, int);
IS(LMEM_ZEROINIT, int);
IS(HEAP_ZERO_MEMORY, int);
IS(HEAP_GENERATE_EXCEPTIONS, int);
IS(LOCKFILE_FAIL_IMMEDIATELY, int);
IS(LOCKFILE_EXCLUSIVE_LOCK, int);
IS(FORMAT_MESSAGE_ALLOCATE_BUFFER, int);
IS(FORMAT_MESSAGE_IGNORE_INSERTS, int);
IS(FORMAT_MESSAGE_FROM_SYSTEM, int);
IS(GetFileExInfoStandard, int);
IS(CP_ACP, int);
IS(CP_OEMCP, int);
IS(CP_UTF8, int);
IS(ENABLE_VIRTUAL_TERMINAL_PROCESSING, int);
IS(FILE_WRITE_ATTRIBUTES, int);
IS(FILE_FLAG_BACKUP_SEMANTICS, int);
IS(CTRL_C_EVENT, int);
IS(EXCEPTION_NONCONTINUABLE, int);
IS(MEM_COMMIT, int);
IS(MEM_RESERVE, int);
IS(MEM_RELEASE, int);
IS(PAGE_NOACCESS, int);
IS(EXCEPTION_EXECUTE_HANDLER, int);
IS(EXCEPTION_CONTINUE_SEARCH, int);
IS(EXCEPTION_CONTINUE_EXECUTION, int);
IS(EXCEPTION_MAXIMUM_PARAMETERS, int);
IS(EVENT_MODIFY_STATE, int);
IS(DRIVE_UNKNOWN, int);
IS(DRIVE_NO_ROOT_DIR, int);
IS(DRIVE_REMOVABLE, int);
IS(DRIVE_FIXED, int);
IS(DRIVE_REMOTE, int);
IS(DRIVE_CDROM, int);
IS(DRIVE_RAMDISK, int);

/* `long` */
IS(GENERIC_WRITE, long);
IS(WAIT_TIMEOUT, long);
IS(ERROR_FILE_NOT_FOUND, long);
IS(ERROR_ACCESS_DENIED, long);
IS(ERROR_NOT_ENOUGH_MEMORY, long);
IS(ERROR_NO_MORE_FILES, long);
IS(ERROR_HANDLE_DISK_FULL, long);
IS(ERROR_NOT_SUPPORTED, long);
IS(ERROR_LOCK_VIOLATION, long);
IS(ERROR_SHARING_VIOLATION, long);
IS(ERROR_RETRY, long);
IS(ERROR_PATH_NOT_FOUND, long);
IS(ERROR_INVALID_HANDLE, long);
IS(ERROR_HANDLE_EOF, long);
IS(ERROR_DEV_NOT_EXIST, long);
IS(ERROR_NETNAME_DELETED, long);
IS(ERROR_DISK_FULL, long);
IS(ERROR_SEM_TIMEOUT, long);
IS(ERROR_NOT_LOCKED, long);
IS(ERROR_USER_MAPPED_FILE, long);
IS(ERROR_NETWORK_UNREACHABLE, long);
IS(NO_ERROR, long);

/* `unsigned long` */
IS(INVALID_FILE_ATTRIBUTES, unsigned long);
IS(INVALID_SET_FILE_POINTER, unsigned long);
IS(GENERIC_READ, unsigned long);
IS(WAIT_OBJECT_0, unsigned long);
IS(WAIT_FAILED, unsigned long);
IS(WAIT_IO_COMPLETION, unsigned long);
IS(STD_INPUT_HANDLE, unsigned long);
IS(STD_OUTPUT_HANDLE, unsigned long);
IS(STD_ERROR_HANDLE, unsigned long);
IS(EXCEPTION_IN_PAGE_ERROR, unsigned long);
IS(EXCEPTION_ACCESS_VIOLATION, unsigned long);
IS(EXCEPTION_DATATYPE_MISALIGNMENT, unsigned long);
IS(EXCEPTION_BREAKPOINT, unsigned long);
IS(EXCEPTION_SINGLE_STEP, unsigned long);
IS(EXCEPTION_ARRAY_BOUNDS_EXCEEDED, unsigned long);
IS(EXCEPTION_FLT_DENORMAL_OPERAND, unsigned long);
IS(EXCEPTION_FLT_DIVIDE_BY_ZERO, unsigned long);
IS(EXCEPTION_FLT_INEXACT_RESULT, unsigned long);
IS(EXCEPTION_FLT_INVALID_OPERATION, unsigned long);
IS(EXCEPTION_FLT_OVERFLOW, unsigned long);
IS(EXCEPTION_FLT_STACK_CHECK, unsigned long);
IS(EXCEPTION_FLT_UNDERFLOW, unsigned long);
IS(EXCEPTION_INT_DIVIDE_BY_ZERO, unsigned long);
IS(EXCEPTION_INT_OVERFLOW, unsigned long);
IS(EXCEPTION_PRIV_INSTRUCTION, unsigned long);
IS(EXCEPTION_ILLEGAL_INSTRUCTION, unsigned long);
IS(EXCEPTION_NONCONTINUABLE_EXCEPTION, unsigned long);
IS(EXCEPTION_STACK_OVERFLOW, unsigned long);
IS(EXCEPTION_INVALID_DISPOSITION, unsigned long);
IS(EXCEPTION_GUARD_PAGE, unsigned long);
IS(EXCEPTION_INVALID_HANDLE, unsigned long);

/* `unsigned int` */
IS(INFINITE, unsigned int);

/* The values the comparisons below read at RUN time: nothing here is a
 * constant expression, so the comparison is made by the code the compiler
 * emits for the two operand types. */
static volatile int  minus_one_int  = -1;
static volatile long minus_one_long = -1;

int main(void) {
    /* int < int: a signed comparison. Typed `unsigned int`, MEM_COMMIT turned
     * the left operand into 4294967295 and the answer into false. */
    if (!(minus_one_int < MEM_COMMIT)) return 1;
    /* long < long: signed as well, and false for the same reason before. */
    if (!(minus_one_long < ERROR_FILE_NOT_FOUND)) return 2;
    /* int < unsigned long: an UNSIGNED comparison in both references — the
     * left operand is 4294967295, which is not below 0xC0000005. */
    if (minus_one_int < EXCEPTION_ACCESS_VIOLATION) return 3;
    /* A difference of two `long` constants can be negative. */
    if (!(ERROR_FILE_NOT_FOUND - ERROR_ACCESS_DENIED + minus_one_long < 0)) return 4;
    return 42;
}
