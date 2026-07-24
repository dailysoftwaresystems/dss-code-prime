/* SQLite testfixture arc (F001A blocker clear, 2026-07-18) — shipped <sys/uio.h>
 * witness + the SUBDIRECTORY descriptor resolution (sys/uio.json). `#include
 * <sys/uio.h>` resolves to shippedLibs/sys/uio.json, a TYPE-ONLY descriptor that
 * injects `struct iovec` alone (no readv/writev — test1.c, the real consumer,
 * references only the struct). This mirrors test1.c's actual usage of the header:
 * the `.iov_base`/`.iov_len` members + `sizeof(struct iovec)`.
 *
 * exit 42 witnesses (a) <sys/uio.h> resolved to the SUBDIR descriptor, (b) its
 * `struct iovec` injected with the members, and (c) the `{ void*, size_t }`
 * 16-byte LP64 layout — iov_len(26) + sizeof(struct iovec)(16) = 42. RED-ON-DISABLE:
 * delete sys/uio.json → <sys/uio.h> fails to resolve → compile fails; drop/rename
 * a field → `.iov_base`/`.iov_len` undefined → compile fails; a wrong layout (a
 * lost field → sizeof 8) → exit 34, not 42. On elf/macho `_WIN32` is undefined so
 * there is NO conflict with test1.c's own `#ifdef _WIN32` iovec (the pe portability
 * shim); here the descriptor is the sole provider. */
#include <sys/uio.h>

int main(void) {
    struct iovec v;
    v.iov_base = (void*)0;                                 /* void*  member */
    v.iov_len  = 26;                                       /* size_t member */
    return (int)v.iov_len + (int)sizeof(struct iovec);     /* 26 + 16 = 42 */
}
