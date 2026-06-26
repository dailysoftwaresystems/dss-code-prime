/* c14 — shipped <sys/mman.h> witness. `#include <sys/mman.h>` resolves to
 * shippedLibs/sys/mman.json: the mmap/munmap symbols (dynamically linked from
 * libc.so.6) + the PROT_ and MAP_SHARED constants. mmap is referenced (so it links)
 * with fd -1, which fails cleanly (EBADF → MAP_FAILED) — no crash; the witness is
 * the constant VALUES (PROT_READ 1 / PROT_WRITE 2 / MAP_SHARED 1) → exit 42 on a
 * real Linux runtime. RED-ON-DISABLE: drop the constants/symbols surface →
 * PROT_READ/mmap undefined → compile fails. */
#include <sys/mman.h>

int main(void) {
    void *p = mmap((void*)0, 4096, PROT_READ, MAP_SHARED, -1, 0);  /* links mmap; fd -1 → MAP_FAILED */
    munmap(p, 4096);                                               /* links munmap; no-op on failure */
    return (PROT_READ == 1 && PROT_WRITE == 2 && MAP_SHARED == 1) ? 42 : 1;
}
