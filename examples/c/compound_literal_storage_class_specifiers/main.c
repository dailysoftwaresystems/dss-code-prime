/* P69 (lane `cs`, D-C-A-STORAGE-CLASS-SPECIFIER-IN-A-COMPOUND-LITERAL-IS-A-PARSE-ERROR): C23
 * lets a compound literal name its own storage — `( storage-class-specifiers type-name )
 * braced-initializer` (C23 6.5.3.6) — judged as if it were the definition `SC typeof(T) ID =
 * { IL };` in its own scope. A `static` (or `static thread_local`) literal inside a function
 * is ONE object for the program, initialized once before start-up; a `constexpr` literal is
 * a compound literal CONSTANT (C23 6.6p6) an integer constant expression may use; a
 * `register` one is an ordinary automatic object whose address may not be taken. Exit 42 =
 * every check passed. */

struct S { int a, b; };

static int *counter(void) { return &(static int){ 40 }; }    /* the same object each call */
static int bump(void) { return ++(static int){ 40 }; }        /* 41, then 42 */
/* The static object's CURRENT value is what `+=` reads (C23 6.5.3.6p7: initialized once,
 * before program start-up). gcc 13.3.0 -std=c2x reads the INITIALIZER here instead and
 * returns 22 from the second call — while it persists the same object under `++` (bump)
 * and through `&` (counter). DSS keeps the one meaning the standard gives. */
static int accumulate(int n) { return (static int){ 0 } += n; }

int main(int argc, char **argv) {
    (void)argv;
    *counter() += argc;
    *counter() += argc;
    if (*counter() != 42) return 1;
    int total = 0;
    for (int i = 0; i < 3; i++) {                  /* one object: 10, 11, 12 */
        int *p = &(static int){ 10 };
        total += *p;
        *p += 1;
    }
    if (total != 33) return 2;
    if ((register int){ 41 } + argc != 42) return 3;
    if ((constexpr int){ 41 } + argc != 42) return 4;
    int *t = &(static thread_local int){ 40 };
    *t += argc + 1;
    if (*t != 42) return 5;
    struct S *sp = &(static struct S){ 1, 40 };
    sp->a += argc;
    if (sp->a + sp->b != 42) return 6;
    static int *keep = &(static int){ 42 };        /* a static literal's address is constant */
    if (*keep != 42) return 7;
    (void)bump();
    if (bump() != 42) return 8;
    int arr[(constexpr int){ 3 }];                 /* a compound literal constant as a bound */
    if (sizeof arr != 3 * sizeof(int)) return 9;
    switch (argc + 40) {
        case (constexpr int){ 41 }: break;
        default: return 10;
    }
    (void)accumulate(20);
    if (accumulate(22) != 42) return 11;
    return (static int){ 41 } + argc;
}
