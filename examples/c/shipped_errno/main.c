/* c14 — shipped <errno.h> end-to-end witness (all three surfaces at once).
 *
 * `#include <errno.h>` resolves to shippedLibs/errno.json:
 *   - the `errno` object MACRO expands to `(*__errno_location())` (an lvalue —
 *     deref of the libc thread-local accessor),
 *   - `__errno_location` is the libc symbol it calls (dynamically linked from
 *     libc.so.6, the FULL-UNIX-build strategy, like `puts`),
 *   - `EINVAL`/`EACCES` are the constants surface (Linux values 22 / 13).
 *
 * `errno = EINVAL` writes 22 to the thread-local; reading `errno` back == 22
 * witnesses the macro (lvalue + rvalue), the constant VALUE, and the libc call.
 * exit 42 iff all three are correct on a real Linux runtime.
 *
 * RED-ON-DISABLE: remove the `macros`/`constants`/`symbols` surfaces from
 * errno.json and `errno`/`EINVAL`/`__errno_location` are undefined → the compile
 * fails (no binary). A wrong EINVAL value → the `!= 22` check → exit 1. */
#include <errno.h>

int main(void) {
    errno = EINVAL;
    if (errno != 22) return 1;   /* EINVAL == 22 + errno macro lvalue/rvalue + __errno_location */
    errno = EACCES;
    if (errno != 13) return 2;   /* EACCES == 13 */
    return 42;
}
