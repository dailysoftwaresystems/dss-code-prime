/* AN `asm goto` INSIDE A `__try` WHOSE OUTPUT IS STORED ON ITS EDGES IS GUARDED
 * (D-LIR-GUARDED-RANGE-DOES-NOT-COVER-BLOCKS-THE-LOWERING-CREATES).
 *
 * `*p` is a structure the template leaves in a register, so the compiler stores
 * it through `p` after the template — and an `asm goto` has more than one way
 * out, so that store is made on EACH edge, in a block of its own. Those edge
 * blocks are laid out at the end of the function, after every block the guarded
 * body's own statements became. A Windows x64 `__try` is a table of byte ranges,
 * and the table used to hold one range per `__try`: the body's own blocks. The
 * edge blocks were outside it, so a fault raised by one of those stores never
 * reached the handler.
 *
 * That is what `guarded` did until P69: `p` is a no-access page, the store
 * faults, and the process ended with 0xC0000005 in debug and in release instead
 * of returning 42 — silently. For a while DSS refused the statement by name.
 * Now the edge blocks are a run of the region and have a range of their own in
 * the table, with the region's filter and handler.
 *
 *   guarded(&cell)  the control: nothing faults, the store is really made on the
 *                   edge taken, and both `rc +=` run                    -> 3
 *   guarded(page)   the store faults on its edge; the handler runs      -> 42
 *   nested(&cell)   the same statement two regions deep, unfaulted      -> 103
 *   nested(page)    the inner filter DECLINES, so the fault goes on to the
 *                   outer handler: the edge blocks are a run of each
 *                   region around them                                  -> 42
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

NOINLINE int nested(struct pair *p) {
    int rc = 0;
    __try {
        __try {
            __asm__ goto ("movq $7, %0" : "=r"(*p) : : "cc" : other);
            rc += 1;
        other:
            rc += 2;
        } __except (0) {   /* EXCEPTION_CONTINUE_SEARCH: not this handler's */
            rc = 7;
        }
        rc += 100;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) {
        rc = 42;
    }
    return rc;
}

int main(void) {
    /* One no-access page: writing it raises EXCEPTION_ACCESS_VIOLATION. */
    struct pair *page = VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    if (page == 0) return 200;   /* the environment, not a result */
    struct pair cell = { 0, 0 };

    if (guarded(&cell) != 3) return 1;
    if (cell.a != 7 || cell.b != 0) return 2;
    if (guarded(page) != 42) return 3;

    cell.a = 0;
    if (nested(&cell) != 103) return 4;
    if (cell.a != 7) return 5;
    if (nested(page) != 42) return 6;
    return 42;
}
