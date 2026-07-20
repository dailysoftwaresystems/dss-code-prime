/* D-PP-PRESCAN-ANGLE-MACRO-SPLICE-AUTHORITATIVE-LIVENESS -- the include-gating
 * pre-scan / shipped angle-macro-splice witness (the sqlite test_syscall.c
 * shape, reduced).
 *
 * `platform.h` is a QUOTE include that `#define`s DSS_USE_ERRNO 1. The C19-C21
 * pre-scan is BLIND to a quote-included header's `#define`s (the child
 * SynthBuilder's localMacros is discarded), so `#if DSS_USE_ERRNO` FOLDS TO 0
 * in the pre-scan (an undefined identifier -> 0, C 6.10.1p4) even though the
 * AUTHORITATIVE MacroExpander -- which sees platform.h's spliced text -- reads
 * it LIVE. Before the fix, the pre-scan therefore skipped splicing the shipped
 * `errno` object-macro for the ANGLE `#include <errno.h>` nested under the
 * guard, and `errno` reached the parser as an undeclared identifier ->
 * `error[S0001]: got errno` on VALID, authoritatively-live code.
 *
 * The fix splices the shipped `#define errno (*__errno_location())` regardless
 * of the pre-scan's (blind) verdict; the authoritative pass -- which elides
 * dead-branch `#define`s -- is the proper arbiter of its liveness. So here:
 *   errno = EINVAL (22); errno == 22  -> + 20
 *   errno = EACCES (13); errno == 13  -> + 22   => exit 42
 * reading + writing the real thread-local via the libc `(*__errno_location())`
 * accessor (a genuine libc.so.6 call), with value-divergent constants.
 *
 * RED-ON-DISABLE: revert the D-PP-PRESCAN-ANGLE-MACRO-SPLICE gate change and
 * the pre-scan skips the splice under `#if DSS_USE_ERRNO` -> `errno` is
 * undeclared -> S0001 -> no binary. (A `#else` is intentionally omitted: the
 * ONLY compiling program is the errno one, so a regression cannot silently
 * fall back to a passing exit code.) */
#include "platform.h"

#if DSS_USE_ERRNO
#include <errno.h>

int main(void) {
    errno = EINVAL;
    if (errno != 22) return 1;   /* EINVAL == 22 : macro lvalue/rvalue + libc accessor + constant */
    errno = EACCES;
    if (errno != 13) return 2;   /* EACCES == 13 */
    return 42;
}
#endif
