/* P69 (lane `cs`, D-C-A-COMPOUND-LITERAL-IS-ITS-INITIALIZERS-VALUE-NOT-AN-OBJECT): a compound
 * literal is an unnamed OBJECT initialized by its brace list (C 6.5.2.5p4) — never that
 * list's value. Each check below is one way DSS used to get it wrong: the literal aliased the
 * variable it was initialized from, a character-array literal WAS the read-only string, two
 * literals were one object. Every operand comes from `argc`, so nothing folds away. Exit 42
 * = every check passed; any other value names the first one that failed. */

struct S { int a, b; };

static int g = 1;
static int *file_scope = &(int){ 30 };             /* static storage: one writable object */
static struct S *file_pair = &(struct S){ 1, 2 };

/* Each activation evaluates the literal into its OWN object (C 6.5.2.5p5: automatic
 * storage associated with the enclosing block). */
static int depth(int n, int *prev) {
    int *p = &(int){ n };
    if (prev != 0 && (p == prev || *prev != n + 1)) return 100;
    if (n == 0) return 0;
    int r = depth(n - 1, p);
    return *p == n ? r : 101;
}

static int sum(struct S s) { return s.a + s.b; }
static struct S make(int x) { return (struct S){ x, 40 }; }

int main(int argc, char **argv) {
    (void)argv;
    int x = argc;                                  /* 1 */
    int *p = &(int){ x };                          /* a COPY of x's value, not x */
    *p = 42;
    if (x != 1) return 1;
    int *q = &(int){ g };
    *q = 7;
    if (g != 1) return 2;
    char *s = (char[]){ "ab" };                    /* writable: its own array */
    s[0] = 'z';
    if (s[0] != 'z' || s[1] != 'b') return 3;
    int *d1 = &(int){ x };
    int *d2 = &(int){ x };                         /* two literals, two objects */
    if (d1 == d2) return 4;
    (int){ 0 } = 5;                                /* an lvalue (C 6.5.2.5p4) */
    int total = 0;
    for (int i = 0; i < 3; i++) {                  /* initialized at each evaluation */
        int *t = &(int){ 10 * x };
        total += *t;
        *t = 99;
    }
    if (total != 30) return 5;
    if (depth(4 + x, 0) != 0) return 6;
    struct S local = (struct S){ x + 1, 40 };
    if (local.a + local.b != 42) return 7;
    struct S pick = x > 0 ? (struct S){ x, 41 } : (struct S){ 0, 0 };
    if (pick.a + pick.b != 42) return 8;
    if (sum((struct S){ x, 1 }) + make(x).a != 3) return 9;
    *file_scope += 11 + x;                         /* 30 + 12 */
    file_pair->b += 40 - x;                        /* 1 + 41 */
    if (*file_scope != 42 || file_pair->a + file_pair->b != 42) return 10;
    int e = 0;
    (int){ e++ } += x;                             /* its initializer runs once */
    if (e != 1) return 11;
    return ++(int){ 40 + x };                      /* 42 */
}
