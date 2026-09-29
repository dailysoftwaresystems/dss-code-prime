// P68 round 9 (lane `cs`) — D-C-INCOMPATIBLE-POINTER-CONVERSION-REFUSED-WHERE-EVERY-REFERENCE-WARNS
// (and D-CSUBSET-INCOMPATIBLE-ELEMENT-ARRAY-TERNARY, the `?:` arm).
//
// A pointer mixed with a pointer to an INCOMPATIBLE type, or with an integer that is
// not a null pointer constant, at every site of the pointer-compatibility
// constraint: initialization (a brace element included), assignment, argument,
// return, `==`, and the arms of `?:`. C makes each a constraint violation — a
// diagnostic is owed, nothing more — and gcc 13.3.0, mingw-w64 13.2.0 and MSVC 19.51
// build every one with a warning and run it to the value below (clang 18.1.3 builds
// the pointer pairs and refuses the integer/pointer and function-signature mixes by
// default). DSS warns at each and builds; every pointer keeps its value, and a `?:`
// of incompatible pointer arms is a `void *` (gcc's and clang's meaning).
//
// ★ EVERY SHAPE HAS THE SAME CLASS ON EVERY TARGET, so one warning list holds for all
// four: the same-representation pair is chosen PER DATA MODEL (`long`/`long long`
// collide only on LP64, `int`/`long` only on LLP64 — `__LP64__` is declared by the
// object FORMAT), and every other pair differs in more than a name on every model.
//
// ★ THE VALUES TRAVEL THROUGH THE CONVERTED POINTERS: each shape reads (or writes)
// through the pointer it produced, so a conversion that pointed somewhere else, or
// an integer round trip that lost bits, changes the exit code.
//
// exit 42 = 20 (init + assignment) + 0 (argument) + 0 (return) + 5 (`unsigned *` read
// as `int *`) + 3 (`unsigned char[]` as `char *`) + 2 (a write through the
// other-identity integer pointer) + 0 (integer to pointer) + 4 (the pointer/integer
// round trip) + 2 (brace element + null member) + 4 (`?:` of `int *` and `float *`,
// read as `int *`) + 2 (`==` of distinct pointer types) + 0 (a function pointer of
// another signature, cast back before the call).

#include <stdint.h>

#ifdef __LP64__
typedef long      wide_a;
typedef long long wide_b;
#else
typedef long wide_a;
typedef int  wide_b;
#endif

struct A { int x; };
struct B { int x; };
struct H { struct A *p; int *q; };

static int take(struct A *pa) { return pa->x; }
static struct A *conv(struct B *pb) { return pb; }
static int twice(int v) { return 2 * v; }

int main(void) {
    struct B b = { 10 };
    struct A *pa = &b;
    struct A *pb2;
    pb2 = &b;
    int total = pa->x + pb2->x;
    total += take(&b) - 10;
    total += conv(&b)->x - 10;
    unsigned u = 5;
    int *pu = &u;
    total += *pu;
    unsigned char bytes[2] = { 3, 0 };
    char *pc = bytes;
    total += *pc;
    wide_b wb = 0;
    wide_a *pw = &wb;
    *pw = 2;
    total += (int)wb;
    intptr_t v = (intptr_t)&b;
    struct B *pv = v;
    total += pv->x - 10;
    intptr_t back = pv;
    total += back == v ? 4 : 0;
    struct H h = { &b, 0 };
    total += h.p->x - 10 + (h.q == 0 ? 2 : 0);
    int c = 1;
    int i = 4;
    float f = 1.0f;
    int *pi = c ? &i : &f;
    total += *pi;
    total += &b == pa ? 2 : 0;
    int (*fp)(void) = twice;
    total += ((int (*)(int))fp)(1) - 2;
    return total;
}
