/* P69 (lane `cs`, D-C-A-FILE-SCOPE-COMPOUND-LITERAL-ADDRESS-IS-REFUSED-AS-A-RUNTIME-INITIALIZER
 * and D-C-A-COMPOUND-LITERAL-IS-ITS-INITIALIZERS-VALUE-NOT-AN-OBJECT): outside every function
 * body a compound literal has STATIC storage duration (C 6.5.2.5p5), so its address — or an
 * element's, or its array's decay — is an address constant (C 6.6p9) a static initializer
 * may hold. Each such object is its own, and writable unless its type is const. `*&E` is `E`
 * itself (C 6.5.3.2p3), so `*&(int){ 42 }` reads the literal's value and takes no address.
 * Exit 42 = every check passed. */

struct S { int a, b; };
struct P { int *q; };

static int *a = &(int){ 40 };
static int *b = (int[]){ 1, 42 } + 1;
static struct S *c = &(struct S){ 40, 2 };
static int *e = &((int[]){ 1, 42 })[1];
static struct P f = { &(int){ 41 } };
static int *arr[2] = { &(int){ 1 }, &(int){ 41 } };
static int h = *&(int){ 42 };
static struct P *nest = &(struct P){ &(int){ 42 } };
static const int *ro = &(const int){ 42 };

int main(int argc, char **argv) {
    (void)argv;
    static int k = *&(int){ 42 };   /* automatic literal: `*&` never takes its address */
    *a += argc + 1;                 /* 42: the object is writable */
    if (*a != 42 || *b != 42 || c->a + c->b != 42 || *e != 42) return 1;
    if (*f.q + *arr[0] != 42 || h != 42 || *nest->q != 42 || k != 42) return 2;
    if (arr[0] == arr[1] || (void *)a == (void *)b) return 3;   /* distinct objects */
    if (*ro != 42) return 4;
    return 42;
}
