/* AN `asm goto` INSIDE A `__try` WHOSE OUTPUT IS STORED ON ITS EDGES IS REFUSED BY NAME
 * (D-LIR-GUARDED-RANGE-DOES-NOT-COVER-BLOCKS-THE-LOWERING-CREATES).
 *
 * `*p` is a structure the template leaves in a register, so the compiler stores
 * it through `p` after the template — and an `asm goto` has more than one way
 * out, so that store is made on EACH edge, in a block of its own. A Windows x64
 * `__try` is one byte range in a scope table: the blocks the guarded body's own
 * statements became. The edge blocks are not among them. They are laid out at
 * the end of the function, so a fault raised by one of those stores is outside
 * the range and never reaches the handler.
 *
 * That is what this program did until P69: `p` is a no-access page, the store
 * faults, and the process ended with 0xC0000005 in debug and in release instead
 * of returning 42 — silently, with no diagnostic. The same statement written as
 * a plain `asm` (one way out, the store in the guarded block itself) returned
 * 42, as did a scalar output and a memory output of the `asm goto`.
 *
 * DSS now refuses the statement rather than compile a store its `__try` does
 * not guard. What compiles today: give the statement a local object as its
 * output and assign `*p = local;` after it — the assignment is a statement of
 * the guarded body, and its fault is caught.
 *
 * Windows x64 only: `__try` is SEH. No reference on these hosts has both halves
 * (MSVC has `__try` and no x64 inline asm, mingw gcc the reverse); the program
 * is the union, which is DSS's to compile.
 */
#include <windows.h>

#define NOINLINE __attribute__((noinline))

struct pair { int a; int b; };

NOINLINE int guarded(struct pair *p) {
    int rc = 0;
    __try {
        __asm__ goto ("movq $7, %0" : "=r"(*p) : : "cc" : other);
        rc += 1;
    other:
        rc += 2;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) {
        rc = 42;
    }
    return rc;
}

int main(void) {
    /* One no-access page: writing it raises EXCEPTION_ACCESS_VIOLATION. */
    struct pair *p = VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    if (p == 0) return 200;   /* the environment, not a result */
    return guarded(p);
}
