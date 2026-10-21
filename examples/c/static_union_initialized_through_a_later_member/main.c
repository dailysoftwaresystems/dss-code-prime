/* P69 (lane `cs`) — D-C-A-STATIC-UNION-INITIALIZED-THROUGH-A-LATER-MEMBER-IS-NOT-ENCODED: a union
 * with static storage may be initialized through ANY member it designates (C 6.7.9p17), and its
 * static image is that member's bytes. Each object below is compared, BYTE BY BYTE through a
 * volatile pointer (so no fold of the initializer can stand in for the image), against the same
 * member stored at run time. Exit 42 when every image holds its initializer, otherwise 100 plus
 * the number of the first that does not:
 *   1 a float member after an int, 2 a member WIDER than the first, 3 a structure member,
 *   4 a const union (the float-bits idiom's object), 5 a union nested in a
 *   structure, 6 an array of unions each naming its own member, 7 a static local,
 *   8 a file-scope compound literal, 9 a bit-field member past the first, 10 a union beside a
 *   _Complex member (the static-data classifier's route), 11 the first member (the control).
 */
#include <string.h>

union IF { int i; float f; };
union CI { char c; int i; };
struct P { int p, q; };
union LS { long l; struct P s; };
union UF { unsigned u; float f; };
struct T { int tag; union { int i; double d; } v; };
union BF { unsigned a : 3; unsigned b : 5; };

static union IF u1 = { .f = 42.0f };
static union CI u2 = { .i = 0x01020304 };
static union LS u3 = { .s = { 40, 2 } };
static const union UF k = { .f = 1.0f };
static struct T t = { 1, { .d = 2.5 } };
static union IF a[2] = { { .f = 1.5f }, { .i = 7 } };
static union IF *const pc = &(union IF){ .f = 6.5f };
static union BF bf = { .b = 21 };
#ifndef __STDC_NO_COMPLEX__
static struct { double _Complex z; union IF v; } cx = { 1.0, { .f = 2.0f } };
#endif
static union IF u0 = { 42 };

/* The first n bytes at `img` (read through a volatile pointer) against those at `want`. */
static int same(void const volatile *img, void const *want, unsigned long n) {
    unsigned char const volatile *p = (unsigned char const volatile *)img;
    unsigned char const *q = (unsigned char const *)want;
    for (unsigned long j = 0; j < n; ++j)
        if (p[j] != q[j]) return 0;
    return 1;
}

int main(int argc, char **argv) {
    static union IF sl = { .f = 3.5f };
    union IF rif;
    union CI rci;
    union LS rls;
    union UF ruf;
    union BF rbf;
    double d = 2.5;
    int tag = 1;
    (void)argv;

    rif.f = 42.0f;
    if (!same(&u1, &rif, sizeof rif.f)) return 101;
    rci.i = 0x01020304;
    if (!same(&u2, &rci, sizeof rci.i)) return 102;
    rls.s.p = 40;
    rls.s.q = 2;
    if (!same(&u3, &rls, sizeof rls.s)) return 103;
    ruf.f = 1.0f;
    if (!same(&k, &ruf, sizeof ruf.f)) return 104;
    if (!same(&t.tag, &tag, sizeof tag) || !same(&t.v, &d, sizeof d)) return 105;
    rif.f = 1.5f;
    if (!same(&a[0], &rif, sizeof rif.f)) return 106;
    rif.i = 7;
    if (!same(&a[1], &rif, sizeof rif.i)) return 106;
    rif.f = 3.5f;
    if (!same(&sl, &rif, sizeof rif.f)) return 107;
    rif.f = 6.5f;
    if (!same(pc, &rif, sizeof rif.f)) return 108;
    memset(&rbf, 0, sizeof rbf);
    rbf.b = 21;
    if (!same(&bf, &rbf, sizeof rbf)) return 109;
#ifndef __STDC_NO_COMPLEX__
    rif.f = 2.0f;
    if (!same(&cx.v, &rif, sizeof rif.f)) return 110;
#endif
    rif.i = 42;
    if (!same(&u0, &rif, sizeof rif.i)) return 111;
    return 42 + (argc - argc);
}
