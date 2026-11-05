/* A STATEMENT EXPRESSION WHOSE LAST ITEM IS LABELED HAS THE LABELED EXPRESSION'S VALUE.
 *
 * GNU C's statement expression `({ ... })` has the value of its last item when that item
 * is an expression statement (GNU C 6.1). A label on that last item is stepped through:
 * `({ int r = 0; if (g) goto in; r = 1; in: r; })` is 1 when g is 0 and 0 otherwise, and a
 * `goto` of the body reaches the computation of the value. ✔MEASURED 2026-10-15: the
 * program below prints `1 0 5 3 2` on AppleClang 21 and on gcc 13.3 for arm64 GNU/Linux,
 * and gcc 13.3 and clang 18 on x86_64 GNU/Linux accept and run it — DSS refused the
 * construct ("this statement expression has no value"). Each check returns its own exit
 * code; 42 when every one holds.
 */

static int jumped(int g) { return ({ int r = 0; if (g) goto in; r = 1; in: r; }); }
static int plain(void) { return ({ x: 5; }); }
static int twice(int g) { return ({ int r = 2; if (g) goto b; r = 3; a: b: r; }); }

/* A label the body loops back to: the value is computed once the loop is left. */
static int counted(int n) {
    return ({ int i = 0; again: if (i < n) { i = i + 1; goto again; } done: i * 10; });
}

int main(void) {
    if (jumped(0) != 1) return 1;
    if (jumped(1) != 0) return 2;
    if (plain() != 5) return 3;
    if (twice(0) != 3) return 4;
    if (twice(1) != 2) return 5;
    if (counted(4) != 40) return 6;
    if (({ y: 7; }) + 1 != 8) return 7;
    return 42;
}
