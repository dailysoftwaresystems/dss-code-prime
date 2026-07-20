/* TF-C27 — shipped <stdio.h> `fileno` witness (D-SHIPPED-SYMBOL-PER-TARGET-
 * AVAILABILITY, the unistd.json fdatasync/fallocate precedent). `#include <stdio.h>`
 * resolves to shippedLibs/stdio.json, which now ships `int fileno(FILE*)` gated
 * availableObjectFormats:[elf] (a real consumer landed: sqlite src/test_quota.c
 * calls fsync/ftruncate/fstat(fileno(p->f)) on its POSIX path).
 *
 * fileno is referenced (so it LINKS + LOADS — the D-FFI-DESCRIPTOR-EAGER-IMPORT
 * empirical proof: glibc exports it WEAK `W fileno`, present=resolvable at load on
 * both elf arches) and its runtime RETURN VALUES are the witness: POSIX guarantees
 * the three standard streams map to descriptors 0/1/2, so
 * fileno(stdin)/fileno(stdout)/fileno(stderr) == 0/1/2 → exit 42 on a real Linux
 * runtime. A broken binding (unresolved symbol → ld.so exit 127, or a wrong
 * FILE*→int signature → garbage fd) never yields 42 — value-divergent, and the
 * release arm runs the optimizer over the three opaque external calls.
 *
 * elf x86_64 + arm64 only (fileno gated [elf]; pe routes fileno→_fileno + macho
 * has no consumer yet — both fail loud until those legs land). RED-ON-DISABLE:
 * drop fileno from stdio.json → S0001 "undeclared identifier" → this stops
 * compiling. */
#include <stdio.h>

int main(void) {
    return (fileno(stdin) == 0 && fileno(stdout) == 1 && fileno(stderr) == 2) ? 42 : 1;
}
