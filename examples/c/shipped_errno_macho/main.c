/* c15b — shipped <errno.h> on macOS, the end-to-end proof of the c15a per-target
 * const/macro variant extension. On a macho target the `errno` macro selects
 * `(*__error())` (Darwin) not `(*__errno_location())` (glibc) — so this only
 * links + runs if the per-FORMAT MACRO variant selected correctly (libSystem has
 * __error, not __errno_location). And EAGAIN is the Darwin value 35 (Linux is
 * 11) — so the `!= 35` check is RED-ON-DISABLE for the per-format CONSTANT
 * variant selection (a broken selector would hand macho the elf value 11). exit
 * 42 on real macOS (the macos-latest CI leg runs Mach-O natively). */
#include <errno.h>

int main(void) {
    errno = EAGAIN;             /* errno macro -> (*__error()) on macho; EAGAIN=35 (Darwin) */
    if (errno != 35) return 1;  /* the DIVERGENT value — Linux is 11 */
    errno = EINVAL;
    if (errno != 22) return 2;  /* EINVAL agrees (22 on both) — flat constant */
    return 42;
}
