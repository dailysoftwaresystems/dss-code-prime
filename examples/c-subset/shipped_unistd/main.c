/* c14 — shipped <unistd.h> witness. `#include <unistd.h>` resolves to
 * shippedLibs/unistd.json: the POSIX I/O symbols (dynamically linked from
 * libc.so.6) + the SEEK_ and access-mode constants. `close` is referenced (so it
 * links) with fd -1, which fails cleanly (EBADF) — no crash; the witness is the
 * constant VALUES (SEEK_SET 0 / SEEK_END 2 / R_OK 4 / W_OK 2 / F_OK 0) → exit 42
 * on a real Linux runtime. RED-ON-DISABLE: drop the constants/symbols surface →
 * SEEK_SET/close undefined → compile fails. */
#include <unistd.h>

int main(void) {
    close(-1);   /* links close (dynamic libc.so.6); fd -1 → EBADF, no crash */
    return (SEEK_SET == 0 && SEEK_END == 2 && R_OK == 4 && W_OK == 2 && F_OK == 0) ? 42 : 1;
}
