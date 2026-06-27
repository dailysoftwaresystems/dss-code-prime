/* c14d — shipped <fcntl.h> witness, AND the first runtime exercise of the c14c
 * variadic-externs extension. `#include <fcntl.h>` resolves to shippedLibs/
 * fcntl.json: the O_ and F_ constants, `struct flock`, and the VARIADIC open/fcntl
 * (dynamically linked from libc.so.6). `open` is called both 2-arg and 3-arg
 * (the variadic path); the opens hit nonexistent paths so they fail cleanly
 * (ENOENT) with no side effect; `fcntl(0, F_GETFD)` exercises fcntl on stdin.
 * The witness is the constant VALUES + struct flock field access → exit 42 on a
 * real Linux runtime. RED-ON-DISABLE: drop the surfaces → O_RDONLY/open/flock
 * undefined → compile fails; revert the c14c variadic support → the 3-arg open
 * fails to type-check. */
#include <fcntl.h>

int main(void) {
    struct flock fl;
    fl.l_type   = F_WRLCK;
    fl.l_whence = 0;
    fl.l_start  = 0;
    fl.l_len    = 0;
    fl.l_pid    = 0;
    open("/dss_nonexistent_a", O_RDONLY);        /* 2-arg variadic open */
    open("/dss_nonexistent_b", O_RDONLY, 420);   /* 3-arg variadic open (mode ignored, no O_CREAT) */
    fcntl(0, F_GETFD);                            /* fcntl (variadic) on stdin */
    return (O_RDONLY == 0 && O_CREAT == 64 && O_CLOEXEC == 524288
            && F_SETLK == 6 && F_WRLCK == 1 && fl.l_type == 1) ? 42 : 1;
}
