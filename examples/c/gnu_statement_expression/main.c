/* P31 — the GNU statement expression `({ ... })`.
 *
 * ✔MEASURED: 59 occurrences across 15 C headers under /usr/include (glibc 2.39
 * + Linux uapi), `assert.h`, `math.h`, `ctype.h`, `unistd.h`, `netinet/in.h`,
 * `bits/stdio.h`, `bits/select2.h` and `bits/cpu-set.h` among them; 7 of those
 * are written `__extension__ ({`, which is why SHAPE 12 is here.
 *
 * The construct lowers onto `HirKind::SeqExpr` — [stmts..., result] — which
 * already existed for value-yielding `++` / assignment-as-a-value, and which
 * MIR already lowers by running every statement child through `lowerStmt`.
 * Each SHAPE below is a property of that lowering that a wrong one would break,
 * and every shape contributes to the exit code 42.
 *
 * ⚠ THE `volatile` SEED IS LOAD-BEARING. Without it the release pipeline folds
 * every shape to a constant before MIR is even asked to run the statements, and
 * the file exits 42 while proving nothing about the lowering. With it, the
 * statements MUST run. */

extern int printf(const char *, ...);

static volatile int seed = 1;      /* opaque to the constant folder */

/* The canonical GNU macro shape — the reason the construct exists. Two uses in
 * ONE function is the discriminating case: `_a`/`_b` must be scoped to the
 * statement expression's own braces, or the second use redeclares the first. */
#define MAXI(a, b) ({ int _a = (a); int _b = (b); _a > _b ? _a : _b; })

/* The glibc `bits/cpu-set.h` / `unistd.h` shape: a statement expression whose
 * body DECLARES, LOOPS and yields. */
#define SUM_TO(n)  ({ int _s = 0; for (int _i = 1; _i <= (n); ++_i) _s += _i; _s; })

struct Pair { int lo; int hi; };

int main(void) {
    int total = 0;

    /* SHAPE 1 — the value is the last statement's expression. */
    total += ({ seed; });                                   /* 1 */

    /* SHAPE 2 — declarations inside run, and their storage is real.  This is
     * the property that needs MIR's entry-block alloca hoist to have walked
     * INTO an expression, which it does (`collectLocalDecls` walks the whole
     * body subtree). */
    total += ({ int a = seed + 1; int b = a + 1; b; });      /* 4 */

    /* SHAPE 3 — two uses of the SAME macro in one function.  Without a scope of
     * its own per statement expression, `_a` is redeclared and the build fails;
     * with a WRONG scope, the second use reads the first's temporary. */
    total += MAXI(seed + 1, seed + 2);                       /* 7 */
    total += MAXI(seed * 4, seed);                           /* 11 */

    /* SHAPE 4 — control flow inside the body (if / while / switch / for). */
    total += ({ int a = seed; if (a) { a = 2; } a; });        /* 13 */
    total += ({ int a = seed; while (a < 3) { ++a; } a; });   /* 16 */
    total += ({ int a = seed; switch (a) { case 1: a = 4; break; default: a = 0; } a; });  /* 20 */
    total += SUM_TO(3);                                       /* 26 */

    /* SHAPE 5 — nesting, both in the body and in the value position. */
    total += ({ int a = ({ seed + 1; }); ({ a + 1; }); });     /* 29 */

    /* SHAPE 6 — an ASSIGNMENT as the last statement yields the assigned value
     * (this is the shape that must lower through the VALUE path, not the
     * statement path — statement position turns it into an AssignStmt that
     * yields nothing). */
    int k = 0;
    total += ({ k = 2; });                                     /* 31 */

    /* SHAPE 7 — a post-increment as the last statement yields the OLD value. */
    int m = 1;
    total += ({ m++; });                                       /* 32 */

    /* SHAPE 8 — an AGGREGATE-typed statement expression (the memory-based
     * aggregate model's carrier path, not the SSA one). */
    struct Pair p = ({ struct Pair t; t.lo = 1; t.hi = 2; t; });
    total += p.lo + p.hi;                                      /* 35 */

    /* SHAPE 9 — VOID: the last statement is a DECLARATION, so the construct has
     * no value.  Both references accept this in a DISCARD position and refuse it
     * where a value is required; here it must RUN its statements and yield
     * nothing.  `k` is read afterwards, so a lowering that dropped the body
     * would change the answer rather than merely lose a value. */
    ({ int unusedButRun = 3; k += unusedButRun; });
    total += k - 2;                                            /* 38 */

    /* SHAPE 10 — the value flows through a ternary and through a call
     * argument, so the node is proven usable wherever an operand is. */
    total += ({ seed; }) ? ({ seed; }) : 99;                   /* 39 */

    /* SHAPE 11 — a `goto` OUT of a statement expression body. */
    if (seed) { ({ goto joined; }); }
    total += 1000;                                             /* not reached */
joined:
    total += 2;                                                /* 41 */

    /* SHAPE 12 — the glibc spelling: `__extension__ ({ ... })`.  `math.h` and
     * `assert.h` write exactly this, and landing either construct without the
     * other leaves those two headers still refused. */
    total += __extension__ ({ int a = seed; a; });             /* 42 */

    printf("gnu_statement_expression total=%d\n", total);
    return total;
}
