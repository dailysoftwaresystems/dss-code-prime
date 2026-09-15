/* P66 (C23 6.7.7.1): THE BARE-ELLIPSIS PARAMETER LIST — `int f(...)`, a
 * function that is VARIADIC WITH ZERO NAMED PARAMETERS.
 *
 * DSS REFUSED every declaration in this file at the pre-change HEAD —
 * `error[P_NoAlternativeMatched]: expected 'StringStart', 'EndStatement', …
 * or 'BlockOpen' — got '('` at the `(`, because `/shapes/paramList` required a
 * `param` before the marker could be reached — on programs gcc 13.3.0
 * (`-std=c2x`) and clang 18.1.3 (`-std=c23`) both compile and RUN.
 *
 * ★ C23 6.7.7.1 gives `parameter-type-list` three productions and the third is
 * the bare marker:
 *     parameter-type-list: parameter-list | parameter-list , ... | ...
 * It pairs with the ONE-ARGUMENT `va_start(ap)` the same edition introduced —
 * there is no `last` parameter to name — which DSS already accepts.
 *
 * ★★ THE REFERENCE ORACLE FOR THIS FILE IS gcc AND clang AND BOTH WERE RUN,
 * NOT ASSUMED. ✔MEASURED 2026-09-09: `gcc -std=c2x -Wall -Wextra` and
 * `clang -std=c23 -Wall -Wextra` compile this file silently, and both linked
 * binaries EXIT 42 — at -O0 and at -O2. MSVC 19.51.36257 `/std:clatest`
 * REFUSES the construct outright (`C2143: syntax error: missing ')' before
 * '...'`), which is why the oracle is the two that implement it; two working
 * accepting references make the behaviour REQUIRED.
 *
 * ★★★ WHAT EACH LINE COSTS IF "ACCEPTED" IS ALL IT IS. Acceptance is the
 * cheap half; a bare-variadic callee has NO named parameter to seed the
 * argument cursor from, so every ABI's va_list start offsets have to be
 * derived from the FnSig alone:
 *   • dss_bare_sum(3, 10, 11, 9)      — four integer varargs, none named   30
 *   • through a FUNCTION POINTER      — the same callee, indirect           3
 *   • dss_bare_mix(1, 1.5, 2)         — the INTEGER/FLOAT register-class
 *                                       split with no named argument at all,
 *                                       read back as 4.5 and doubled         9
 *   • dss_bare_zero()                 — a bare-variadic call with NO
 *                                       arguments at all                     7
 *   • dss_bare_zero(1, 2.0, "x")      — arguments passed and never read      7
 * A callee whose va_list started one slot late, or that took the AAPCS64/SysV
 * named-parameter offset when there is no named parameter, changes the exit
 * code rather than passing quietly.
 * argc-seeded so the release optimizer cannot fold the program away.
 * exit = 30 + 3 + 9 + 7 + 7 - 14 - (argc - 1) = 42 at argc == 1. */
#include <stdarg.h>

/* The DECLARATION spelling, defined below — one construct, both spellings. */
int dss_bare_sum(...);

/* THE DEFINITION: zero named parameters. The leading vararg carries the count,
 * which is the only way a bare-variadic callee can know how many follow. */
int dss_bare_sum(...) {
    va_list ap;
    va_start(ap);                 /* C23 one-argument form: no `last` exists  */
    int n     = va_arg(ap, int);
    int total = 0;
    while (n-- > 0) total += va_arg(ap, int);
    va_end(ap);
    return total;
}

/* The INTEGER/FLOAT register-class split, with no named argument to seed it.
 * On SysV x86_64 the gp and fp save-area cursors both start at their zero
 * positions here; on Win64 every incoming register spills to the home area;
 * on AAPCS64 the two lists start at the head of their own regions. */
static double dss_bare_mix(...) {
    va_list ap;
    va_start(ap);
    int    i = va_arg(ap, int);
    double d = va_arg(ap, double);
    int    j = va_arg(ap, int);
    va_end(ap);
    return (double)i + d + (double)j;
}

/* A bare-variadic callee that reads NOTHING: the prologue must still be
 * well-formed when no argument is consumed, and when none is passed. */
static int dss_bare_zero(...) { return 7; }

/* The TYPEDEF spelling of the same type, used as a function pointer. */
typedef int dss_bare_fn(...);

int main(int argc, char **argv) {
    (void)argv;

    dss_bare_fn *p = dss_bare_sum;

    int a = dss_bare_sum(3, 10, 11, 9);              /* 30                    */
    int b = p(2, 1, 2);                              /*  3, indirect          */
    int c = (int)(dss_bare_mix(1, 1.5, 2) * 2.0);    /*  9 = (1+1.5+2) * 2    */
    int d = dss_bare_zero();                         /*  7, no arguments      */
    int e = dss_bare_zero(1, 2.0, "x");              /*  7, unread arguments  */

    return a + b + c + d + e - 14 - (argc - 1);      /* 56 - 14 = 42          */
}
