/* Cluster G — shipped <sys/types.h> POSIX typedefs + the FIRST SUBDIRECTORY
 * descriptor resolution. `#include <sys/types.h>` resolves to shippedLibs/sys/
 * types.json (resolveSystemDescriptor now PRESERVES the subdir), so the POSIX
 * `sys/*` headers map to distinct descriptors and never collide with a top-level
 * header of the same stem (the later <sys/time.h> -> sys/time.json stays distinct
 * from <time.h> -> time.json). off_t/ssize_t inject as i64, mode_t as u32.
 *
 * exit 42 is the witness that (a) <sys/types.h> resolved to the SUBDIR descriptor,
 * (b) its typedefs injected, (c) a program using them compiles + runs on every
 * ABI. RED-ON-DISABLE: without the subdir fix, <sys/types.h> -> types.json which
 * is not shipped -> the header fails to resolve -> compile fails (no binary). */
#include <sys/types.h>

int main(void) {
    off_t   sz   = 40;   /* i64 file offset */
    ssize_t n    = 2;    /* i64 signed size */
    mode_t  perm = 0;    /* u32 mode bits  */
    return (int)(sz + n) + (int)perm;   /* 42 */
}
