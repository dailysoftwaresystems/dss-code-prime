/* <stdlib.h>'s aligned_alloc on the UCRT, which exports none (✔MEASURED P69: mingw-w64 13.2.0 and MSVC 19.51 neither
 * declare nor link it; glibc and libSystem export it, and ELF and Mach-O bind theirs).
 *
 * C requires `free` to release what aligned_alloc returns (C23 7.24.3.3), and the UCRT's `free` releases only what
 * its `malloc` family returned — its `_aligned_malloc` memory needs `_aligned_free`, and an over-allocated block
 * handed back at an interior address is no pointer `free` accepts. So this returns `malloc(size)` for every alignment
 * the UCRT's malloc already guarantees — a power of two up to 16, the alignment MSVC documents for malloc on 64-bit
 * platforms (and measured: every block of a 1..4096-byte sweep came back 16-aligned) — and a null pointer for any
 * other, which C23 7.24.3.1 prescribes for an alignment "not supported by the implementation". Every pointer it
 * returns stays valid for `free`, free_sized and free_aligned_sized.
 *
 * pe's only. */
#include <errno.h>
#include <stdlib.h>

void *aligned_alloc(size_t alignment, size_t size) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 || alignment > 16) {
        errno = EINVAL;
        return NULL;
    }
    return malloc(size);
}
