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
 * is not shipped -> the header fails to resolve -> compile fails (no binary).
 *
 * time_t (i64) is ALSO exercised here: glibc <sys/types.h> re-provides <time.h>'s
 * time_t (via <bits/types/time_t.h>), and sqlite test_quota.c reaches it through
 * this header. This TU does NOT include <time.h>, so `time_t` is reachable ONLY
 * via sys/types.json -> a SECOND red-on-disable edge: drop time_t from that
 * descriptor and `time_t when` fails S0006 (undeclared type), no binary. */
#include <sys/types.h>

int main(void) {
    off_t   sz   = 30;   /* i64 file offset */
    ssize_t n    = 2;    /* i64 signed size */
    time_t  when = 10;   /* i64 calendar time (sys/types.h re-provides time_t) */
    mode_t  perm = 0;    /* u32 mode bits  */
    return (int)(sz + n) + (int)when + (int)perm;   /* 30+2+10+0 = 42 */
}
