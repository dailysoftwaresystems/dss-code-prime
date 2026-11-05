/* A CALL WHOSE CALLEE THE UNIT DECLARES NOWHERE DECLARES IT IMPLICITLY (C90 6.3.2.2; P69 round 1, lane cs, fold f26c):
 * `extern int name();`, each argument passed after its default argument promotions (C 6.5.2.2p6) as a NON-variadic
 * call. other.c defines the three functions; this unit declares none of them. `h` reads a `double`, so the `float`
 * argument must reach it promoted; `k` reads an `int`, so the `short` must reach it promoted; on arm64 Darwin a
 * variadic call would pass them on the stack, where the definitions do not read. Every return before 42 names the
 * first fact that failed. */
int main(void) {
    short s = 40;
    if (g(40) + 2 != 42) return 1;      /* declared nowhere: the call declares `int g()`, warned */
    if (h(40.0f) + 2 != 42) return 2;   /* the float argument promoted to double */
    if (k(s) + 2 != 42) return 3;       /* the short argument promoted to int */
    if (g(2) + 40 != 42) return 4;      /* the same declaration: no second warning */
    return 42;
}
