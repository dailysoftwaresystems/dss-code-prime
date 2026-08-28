/* D-CSUBSET-DARWIN-BSD-SYMBOL-CLUSTER — the rest of the Darwin/BSD cluster
 * sqlite os_unix.c's proxy/AFP-locking region consumes, in one runnable
 * witness: random/srandomdev (<stdlib.h> — afpLock os_unix.c:3197 picks the
 * shared-lock byte via `lk = random()`; :6169 seeds via `srandomdev()`),
 * futimes (<sys/time.h> — proxyTakeConch os_unix.c:7883
 * `futimes(conchFile->h, NULL)`), and the uuid_t typedef (<unistd.h> —
 * gethostuuid/PROXY_HOSTIDLEN, os_unix.c:7600/:7607). All Darwin-only, all
 * inside `#if defined(__APPLE__) && SQLITE_ENABLE_LOCKING_STYLE`.
 *
 * DETERMINISM WITHOUT CONSTRAINING THE VALUES:
 *   - `r - r` is 0 for ANY runtime value, so the exit code needs NO optimizer
 *     cooperation — while random() still EXECUTES (an extern libSystem call
 *     is never DCE'd): the seeded value genuinely crosses the ABI and is
 *     discarded arithmetically, not elided.
 *   - futimes(-1, 0) is a safe EBADF no-op: fd -1 names nothing, so no file
 *     is touched, while the call still crosses the ABI for real (the
 *     ioctl(-1, ...) pattern from shipped_ioctl_iowr_macho).
 *   - sizeof(id) == 16 is the DETERMINISTIC witness that uuid_t decoded as
 *     the 16-byte unsigned-char array (`typedef unsigned char
 *     __darwin_uuid_t[16]`) — the exact identity sqlite asserts at
 *     os_unix.c:7607 (`PROXY_HOSTIDLEN == sizeof(uuid_t)`).
 *
 * Exit arithmetic: (int)sizeof(id) + (int)(r - r) + 26 = 16 + 0 + 26 = 42.
 *
 * RED-ON-DISABLE: delete any of the three symbol rows (stdlib.json random /
 * srandomdev, sys/time.json futimes) → S0001 at its call site; delete the
 * uuid_t macho variants arm (unistd.json) → S0006 unknown type at the
 * declaration. The descriptors SHADOW the real SDK headers totally, so the
 * rows are the ONLY possible source of these names. */
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

int main(void) {
    uuid_t id;               /* 16-byte u8 array — the S0006 half of the pin */
    long r;

    srandomdev();            /* seed from the system entropy source          */
    r = random();            /* any value; the CALL itself is the witness    */
    (void)futimes(-1, 0);    /* EBADF no-op — crosses the ABI, touches nothing */

    return (int)sizeof(id) + (int)(r - r) + 26;   /* 16 + 0 + 26 = 42 */
}
