/* A scalar's initializer in more braces than C 6.7.9p11's one pair, and with more
 * elements than one (C 6.7.9p2): built with a warning, the scalar initialized by the
 * innermost FIRST value, every excess element dropped unevaluated. Exit 42, or the
 * number of the first check that fails (see expected.json). */
static int calls;
static int bump(void) { ++calls; return 1000; }

struct S { int a; int b; };

static int s1 = {{42}};
static int s2 = {42, 7};
static int s3 = {{42}, 9};

int main(void) {
    volatile int v1 = {{42}};
    volatile int v2 = {{{42}}};
    volatile int v3 = {42, bump()};
    volatile int v4 = {{42}, bump()};
    volatile int v5 = {{}};
    volatile int v6 = {{42, bump()}};
    volatile struct S st = {{40, bump()}, 2};
    volatile int va[2] = {{40}, {2}};
    volatile int vc = (int){{42}, 3};
    if (v1 != 42) return 1;
    if (v2 != 42) return 2;
    if (v3 != 42) return 3;
    if (v4 != 42) return 4;
    if (v5 != 0) return 5;
    if (v6 != 42) return 6;
    if (st.a + st.b != 42) return 7;
    if (va[0] + va[1] != 42) return 8;
    if (vc != 42) return 9;
    if (s1 != 42 || s2 != 42 || s3 != 42) return 10;
    if (calls != 0) return 11;
    return 42;
}
