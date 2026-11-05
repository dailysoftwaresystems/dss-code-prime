/* A TAG USED BEFORE A BLOCK'S OWN TAG OF THAT NAME NAMES THE OUTER ONE (C 6.2.1p7, C 6.7.2.3p5-p9; P69 round 1,
 * lane cs, fold f26c). A tag's scope begins just after the tag appears in the specifier that declares it, and a
 * reference names the tag visible there: `struct S *p;` written before the block's own `struct S { ... };` points
 * at the ENCLOSING S. `struct S;` alone declares the block's own tag even when another is visible, and the block's
 * later definition completes it; a use before its own scope's definition with nothing outside is that tag. Every
 * return before 42 names the first fact that failed. */
struct S { int a; };

static struct S g_s = { 7 };

static int outer_before_inner(void) {
    struct S *p = &g_s;                   /* the file's S */
    int sz = (int)sizeof(struct S);       /* the file's S */
    int r = ((struct S *)(void *)&g_s)->a;
    struct S { long long b, c, d; };      /* the block's S from here */
    struct S q = { 1, 2, 3 };
    if ((int)sizeof *p != (int)sizeof(int)) return 1;
    if (sz != (int)sizeof(int)) return 2;
    if (p->a + r != 14) return 3;
    if (sizeof q != 3 * sizeof(long long) || q.c != 2) return 4;
    return 0;
}

static int tag_only_declaration(void) {
    struct S s = { 40 }, *o = &s;         /* the file's S */
    struct S;                             /* the block's own S (C 6.7.2.3p7) */
    struct S *p;                          /* the block's S */
    struct S { long long b; } x = { 2 };  /* completes the block's S */
    p = &x;
    if (sizeof *p != sizeof(long long)) return 5;
    return o->a + (int)p->b == 42 ? 0 : 6;
}

static int one_scope_one_tag(void) {
    struct T *p;                          /* no T outside: declares the block's T */
    struct T { int v; } x = { 42 };       /* completes it */
    p = &x;
    return p->v == 42 ? 0 : 7;
}

struct U *g_u;                            /* the file's U, written before its definition */

static int outer_written_by_a_reference(void) {
    struct U *p = g_u;                    /* the file's U: a reference declared it above */
    struct U { char c; };                 /* the block's U from here */
    if (p != g_u) return 8;
    return sizeof(struct U) == 1 ? 0 : 9;
}

struct U { int a, b; };

int main(void) {
    int r;
    if ((r = outer_before_inner()) != 0) return r;
    if ((r = tag_only_declaration()) != 0) return r;
    if ((r = one_scope_one_tag()) != 0) return r;
    if ((r = outer_written_by_a_reference()) != 0) return r;
    return 42;
}
