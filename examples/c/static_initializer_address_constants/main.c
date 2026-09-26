// P68 round 13 (lane `cs`, the static-initializer item) — the ADDRESS CONSTANTS C 6.6p7 and
// p9 let an object of static storage duration be initialized with: `&` of an lvalue that
// designates a static object or a function, reached through `[]`, `.`, `->`, `*` and pointer
// casts; an array or function designator decaying; a string literal; an integer constant
// cast to a pointer; and any of them plus or minus an integer constant expression — at file
// scope and in a block-scope `static`. Every one is laid down as static data (a relocated
// address), never computed at load time. `bad` counts the checks that fail.

struct S { int k; int m; };

static int g(void) { return 42; }

int a[3] = { 1, 42, 3 };
struct S s = { 1, 42 };
int y = 42, z = 1;

int *p1 = &a[1];                          // `&` of an element
int *p2 = a + 1;                          // an array designator decaying, plus a constant
int *p3 = 1 + a;                          // … in either order
int *p4 = &a[2] - 1;                      // minus a constant
int *p5 = (int *)&a + 1;                  // through a pointer cast
int *p6 = &(a + 1)[0];                    // `[]` of a constant pointer
int *p7 = &s.m;                           // `.`
int *p8 = &(&s)->m;                       // `->` of a constant pointer
int *p9 = &*&y;                           // `*` and `&` cancel
int *p10 = 1 ? &y : &z;                   // a constant condition selects one arm
int (*fp)(void) = g;                      // a function designator decaying
int (*fq)(void) = &g;
char const *str = "x*" + 1;               // a string literal plus a constant
int *null = (int *)0;                     // an integer constant cast to a pointer
int *table[2] = { &y, &a[2] };            // the same forms as aggregate elements
int grid[2][3] = { { 1, 2, 3 }, { 4, 42, 6 } };
struct A { int k; int arr[2]; } sa = { 1, { 7, 42 } };
int *row = grid[1];                       // an array lvalue that is not a name decays
int (*rowp)[3] = grid + 1;                // a pointer to a row, plus a constant
int *cell = *(grid + 1) + 1;              // `*` of it decays, plus a constant
int *member = sa.arr;                     // a member array decays
int *deref = &*a;                         // `&` and `*` cancel over an array designator
char *bytes = (char *)&a + sizeof(int);   // a character pointer strides by bytes
int nulls = (int *)0 == 0;                // two null pointers compare equal
unsigned long long wide = (unsigned long long)&a + 0;   // an address in a pointer-wide integer
int *back = (int *)((unsigned long long)&a + sizeof(int));   // ... and back to a pointer

static int check(void) {
    static int *local = &y;               // a block-scope static takes them too
    static int b = 40;
    static int *pb = &b;
    static int ss[3] = { 1, 42, 3 };
    static int *sp = &ss[2] - 1;          // a block-scope static's own element, minus a constant
    int l = 0;
    static int *pick = 1 ? &b : &l;       // the unselected arm is never evaluated
    int bad = 0;
    bad += *p1 != 42 || *p2 != 42 || *p3 != 42 || *p4 != 42 || *p5 != 42;
    bad += *p6 != 42 || *p7 != 42 || *p8 != 42 || *p9 != 42 || *p10 != 42;
    bad += fp() != 42 || fq() != 42 || str[0] != '*' || null != 0;
    bad += *table[0] != 42 || *table[1] != 3;
    bad += *local != 42 || *pb + 2 != 42;
    bad += row[1] != 42 || (*rowp)[1] != 42 || *cell != 42 || member[1] != 42;
    bad += *deref != 1 || *(int *)bytes != 42 || nulls != 1 || *back != 42;
    bad += *(int *)wide != 1 || *sp != 42 || pick != &b || l != 0;
    return bad;
}

int main(void) { return check() == 0 ? 42 : 1; }
