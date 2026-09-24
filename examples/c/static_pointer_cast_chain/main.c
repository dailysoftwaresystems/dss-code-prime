/* A null pointer, or an integer address, that reaches a pointer object with STATIC
 * storage through a CHAIN of pointer casts is an address constant (C 6.6p9: pointer
 * casts may be used to create one; 6.3.2.3p4: a null pointer converted to another
 * pointer type is a null pointer), so it is static data — never a load-time store
 * (P68 round 9, D-C-STATIC-INITIALIZER-POINTER-CAST-CHAIN-REFUSED).
 *
 * `char *p = NULL;` is such a chain with the platforms' own `NULL`, `((void *)0)`:
 * the explicit `(void *)` plus the implicit `void *` -> `char *` conversion. Every
 * object below is read back through a volatile lvalue, so the check reads the bytes
 * the image holds at every optimization level. Each group sets one bit; the exit is
 * 42 only when all seven hold, otherwise the bitmask of the groups that did.
 *
 *   1  a scalar null pointer through a cast chain: `char *`, an explicit chain, a
 *      function pointer, a `char *const` (and the one-cast `void *` beside them)
 *   2  `NULL` into a `char *`: external, file-scope static and block-scope static
 *   4  a struct member and a mixed pointer array, `(void *)0` beside a string
 *   8  a pointer array of `NULL` and a string
 *  16  a getopt-style option table, `NULL` in its name and flag members
 *  32  an integer address through a chain, as a scalar and as a member (and the
 *      one-cast scalar beside them)
 *  64  an element address off a null base reached through a chain — sqlite's
 *      `SQLITE_INT_TO_PTR(X)`, `(void *)&((char *)0)[X]`, with `NULL` or
 *      `(void *)0` as the base: the address X, as a scalar and as a member
 */
#include <stddef.h>

typedef int (*fn_t)(void);
struct S { char *p; int n; };
struct opt { const char *name; int has_arg; int *flag; int val; };

void *g01 = (void *)0;
char *g02 = (void *)0;
char *g03 = (char *)(void *)0;
fn_t g04 = (void *)0;
char *const g07 = (void *)0;

char *n10 = NULL;
static char *s10 = NULL;

struct S g05 = { (void *)0, 3 };
char *g06[3] = { (void *)0, "x", (void *)0 };

char *n14[3] = { NULL, "x", NULL };

static struct opt g13[] = { { "help", 0, NULL, 'h' }, { NULL, 0, NULL, 0 } };

char *g08 = (char *)(void *)0x10;
char *g09 = (char *)0x10;
struct S g12 = { (char *)(void *)0x10, 3 };

struct F { const char *name; void *user; };
void *b1 = (void *)&((char *)(void *)0)[5];
void *b2 = (void *)&((char *)NULL)[7];
struct F b3[2] = { { "a", (void *)&((char *)NULL)[3] }, { "b", (void *)&((char *)(void *)0)[9] } };

static void *rdv(void *const volatile *p) { return *p; }
static char *rdc(char *const volatile *p) { return *p; }
static fn_t rdf(fn_t const volatile *p) { return *p; }
static const char *rdk(const char *const volatile *p) { return *p; }
static int *rdi(int *const volatile *p) { return *p; }
static int rdn(int const volatile *p) { return *p; }

static char *blockStatic(void) {
    static char *p = NULL;
    return rdc(&p);
}

int main(void) {
    int bits = 0;

    if (rdv(&g01) == NULL && rdc(&g02) == NULL && rdc(&g03) == NULL && rdf(&g04) == NULL
        && rdc(&g07) == NULL) {
        bits |= 1;
    }
    if (rdc(&n10) == NULL && rdc(&s10) == NULL && blockStatic() == NULL) bits |= 2;
    if (rdc(&g05.p) == NULL && rdn(&g05.n) == 3 && rdc(&g06[0]) == NULL
        && rdc(&g06[1])[0] == 'x' && rdc(&g06[2]) == NULL) {
        bits |= 4;
    }
    if (rdc(&n14[0]) == NULL && rdc(&n14[1])[0] == 'x' && rdc(&n14[2]) == NULL) bits |= 8;
    if (rdk(&g13[0].name)[0] == 'h' && rdi(&g13[0].flag) == NULL && rdn(&g13[0].val) == 'h'
        && rdk(&g13[1].name) == NULL && rdi(&g13[1].flag) == NULL && rdn(&g13[1].val) == 0) {
        bits |= 16;
    }
    if ((size_t)rdc(&g08) == 0x10 && (size_t)rdc(&g09) == 0x10 && (size_t)rdc(&g12.p) == 0x10
        && rdn(&g12.n) == 3) {
        bits |= 32;
    }
    if ((size_t)rdv(&b1) == 5 && (size_t)rdv(&b2) == 7 && (size_t)rdv(&b3[0].user) == 3
        && (size_t)rdv(&b3[1].user) == 9 && rdk(&b3[1].name)[0] == 'b') {
        bits |= 64;
    }

    return bits == 127 ? 42 : bits;
}
