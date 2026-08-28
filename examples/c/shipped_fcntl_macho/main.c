/* c15c — shipped <fcntl.h> on macOS: the O_ and F_ constant divergences + the
 * REORDERED 24-byte struct flock + the variadic open/fcntl (linked from
 * libSystem). The macOS values DIFFER from Linux (verified against xnu): O_CREAT
 * 512 (not 64), O_CLOEXEC 16777216, O_NOFOLLOW 256, F_SETLK 8, and the lock
 * TYPES are reordered — F_RDLCK 1 / F_WRLCK 3 (Linux 0/1; F_UNLCK=2 agrees). The
 * macOS flock puts the off_t fields first → sizeof 24 (Linux 32). open hits a
 * nonexistent path (ENOENT, no side effect); fcntl(0,F_GETFD) on stdin.
 *
 * RED-ON-DISABLE: a broken variant selector hands macho the elf values (O_CREAT
 * 64 != 512) or the elf flock layout (sizeof 32 != 24) → exit 1. */
#include <fcntl.h>

int main(void) {
    struct flock fl;
    fl.l_type   = F_WRLCK;                        /* macho F_WRLCK = 3 */
    fl.l_whence = 0;
    fl.l_start  = 0;
    fl.l_len    = 0;
    fl.l_pid    = 0;
    open("/dss_nonexistent_a", O_RDONLY);         /* 2-arg variadic open */
    open("/dss_nonexistent_b", O_RDWR, 420);      /* 3-arg variadic open (libSystem) */
    fcntl(0, F_GETFD);                            /* fcntl (variadic) on stdin */
    return (sizeof(struct flock) == 24
            && O_CREAT == 512 && O_CLOEXEC == 16777216 && O_NOFOLLOW == 256
            && F_SETLK == 8 && F_RDLCK == 1 && F_WRLCK == 3 && F_UNLCK == 2
            && fl.l_type == 3) ? 42 : 1;
}
