/* <stdlib.h>'s C23 names that NO platform's C library exports, so DSS ships their bodies — the libgcc
 * arrangement: the compiler's own runtime provides what the platform does not.
 *
 * ✔MEASURED 2026-09-30 (lane lm, P69), a hand declaration LINKED against each platform's C library:
 * glibc 2.39 (x86_64 and aarch64), the UCRT (MSVC 19.51 and mingw-w64 13.2.0) and libSystem (Apple
 * clang 21, arm64 and x86_64) export none of free_sized, free_aligned_sized or memalignment. The rows
 * in stdlib.json realize them from this file on every object format; its own source supersedes the
 * C-library import the rest of <stdlib.h> inherits (the R3 precedence, shippedLibs/README.md).
 *
 * One unit for every format: nothing here is format-specific, so a per-format file name would be a
 * second copy of the same body. */
#include <stdint.h>
#include <stdlib.h>

/* C23's memalignment: the maximum alignment the address satisfies — the largest power of two it is a
 * multiple of — and zero for a null pointer. `address & (0 - address)` isolates the lowest set bit,
 * and is zero for zero. */
size_t memalignment(const void *p) {
    uintptr_t const address = (uintptr_t)p;
    return (size_t)(address & ((uintptr_t)0 - address));
}

/* C23's free_sized and free_aligned_sized (7.24.3.4-5): equivalent to free(ptr) when `size` (and `alignment`) are the
 * ones the block was allocated with, undefined otherwise — so an implementation may ignore both, and NOTE 1 says so.
 * No platform exports either (✔MEASURED P69: glibc 2.39 both arches, the UCRT, libSystem both arches). */
void free_sized(void *ptr, size_t size) {
    (void)size;
    free(ptr);
}

void free_aligned_sized(void *ptr, size_t alignment, size_t size) {
    (void)alignment;
    (void)size;
    free(ptr);
}
