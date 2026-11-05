/* `&&` AND `||` INSIDE A `__try`: THE BLOCK THE COMPILER MAKES FOR THE OPERATOR IS GUARDED
 * (D-LIR-GUARDED-RANGE-DOES-NOT-COVER-BLOCKS-THE-LOWERING-CREATES).
 *
 * `a && b` is a branch on `a` that either evaluates `b` or goes straight to the
 * join with the answer already known. That second way is an edge of its own, and
 * the compiler gives it a small block of its own — laid out at the end of the
 * function, after every block the guarded body's statements became. A Windows x64
 * `__try` is a table of byte ranges; that block needs a range of its own in it,
 * with the region's filter and handler, or it is code written inside a `__try`
 * that no handler covers.
 *
 * This is the shape an ordinary C program puts there: no inline assembly, nothing
 * a reference lacks — cl compiles this file as it stands, and the exit code below
 * is cl's as well as DSS's.
 *
 * Each function writes `step` as it goes, so its answer says how far the guarded
 * body got and what the operator had stored in `t` by then:
 *   not caught:  step * 100 + (t + 1)
 *   caught:      1000 + step * 100 + (t + 1)
 *
 *   cond_and(0, &cell)      the short-circuit edge is taken; `t` is 2        -> 303
 *   cond_and(&five, &cell)  both operands are read; `t` is 1                 -> 302
 *   cond_and(&zero, &cell)  the right operand is false; `t` is 2             -> 303
 *   cond_and(page, &cell)   the fault is IN the operator (reading `*p`);
 *                           `t` is still -1 at step 1                        -> 1100
 *   cond_and(0, page)       the fault is AFTER the operator: the handler
 *                           sees the `t` the short-circuit edge stored       -> 1203
 *   value_or(0)             `!p` decides alone; `t` is 11                    -> 212
 *   value_or(&zero)         the right operand is read and is false; 10       -> 211
 *   value_or(page)          the fault is in the operator                     -> 1100
 *   nested(page)            the operator two regions deep: the inner filter
 *                           DECLINES, so the fault goes on to the outer
 *                           handler                                          -> 2100
 *   nested(0)               unfaulted, both regions left normally            -> 303
 *
 * Windows x64 only: `__try` is SEH.
 */
#include <windows.h>

#define NOINLINE __declspec(noinline)

NOINLINE int cond_and(int *p, int *after) {
    volatile int step = 0;
    int t = -1;
    __try {
        step = 1;
        if (p && *p) t = 1; else t = 2;
        step = 2;
        *after = 9;
        step = 3;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) {
        return 1000 + step * 100 + (t + 1);
    }
    return step * 100 + (t + 1);
}

NOINLINE int value_or(int *p) {
    volatile int step = 0;
    int t = -1;
    __try {
        step = 1;
        t = 10 + (!p || *p);
        step = 2;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) {
        return 1000 + step * 100 + (t + 1);
    }
    return step * 100 + (t + 1);
}

NOINLINE int nested(int *p) {
    volatile int step = 0;
    int t = -1;
    __try {
        __try {
            step = 1;
            if (p && *p) t = 1; else t = 2;
            step = 2;
        } __except (0) {   /* EXCEPTION_CONTINUE_SEARCH: not this handler's */
            t = 8000;      /* never runs: the filter declines */
        }
        step = 3;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) {
        return 2000 + step * 100 + (t + 1);
    }
    return step * 100 + (t + 1);
}

int main(void) {
    /* One no-access page: reading or writing it raises EXCEPTION_ACCESS_VIOLATION. */
    int *page = VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    if (page == 0) return 200;   /* the environment, not a result */
    int cell = 0;
    int five = 5;
    int zero = 0;

    if (cond_and(0, &cell) != 303) return 1;
    if (cell != 9) return 2;
    if (cond_and(&five, &cell) != 302) return 3;
    if (cond_and(&zero, &cell) != 303) return 4;
    if (cond_and(page, &cell) != 1100) return 5;
    if (cond_and(0, page) != 1203) return 6;

    if (value_or(0) != 212) return 7;
    if (value_or(&zero) != 211) return 8;
    if (value_or(page) != 1100) return 9;

    if (nested(page) != 2100) return 10;
    if (nested(0) != 303) return 11;
    return 42;
}
