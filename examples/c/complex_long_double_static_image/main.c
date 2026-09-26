/* `_Complex long double` objects with STATIC storage — a whole object, the
   product and quotient of two constants, an integer past 2^53, a struct member
   and an array element. Each image is checked against the same value computed
   at RUN TIME from mutable globals, so a component folded at the wrong
   precision or placed at the wrong offset fails its own check.
   (D-CSUBSET-COMPLEX-LONG-DOUBLE-STATIC-INITIALIZER-REFUSED)

   Exit 42 = every image holds; 11-16 names the first that does not:
     11  = 40.0L                    — real→complex: (40, 0)
     12  = -(complex)2.5L           — the sign flip on both parts
     13  = (complex)0.1L * 3.0L     — 0.1L has no binary64 value: the product
                                      must be folded at long double precision
     14  = 9007199254740993LL       — 2^53 + 1, exact in long double on Linux
     15  struct { int; _Complex long double; } — the member's offset
     16  an array element (6.0L / 2.0L) and its neighbour */
typedef _Complex long double cld;

static cld z1 = 40.0L;
static cld z2 = -(cld)2.5L;
static cld z3 = (cld)0.1L * (cld)3.0L;
static cld z4 = 9007199254740993LL;
struct C { int tag; cld v; };
static struct C c = { 5, 1.5L };
static cld arr[2] = { 1.0L, (cld)6.0L / (cld)2.0L };

/* read at run time: nothing below can be folded */
long double g_tenth = 0.1L, g_three = 3.0L;
long long g_big = 9007199254740993LL;

static long double re(cld const *p) { return ((long double const *)p)[0]; }
static long double im(cld const *p) { return ((long double const *)p)[1]; }

int main(void) {
    cld const *volatile p1 = &z1;
    cld const *volatile p2 = &z2;
    cld const *volatile p3 = &z3;
    cld const *volatile p4 = &z4;
    struct C const *volatile pc = &c;
    cld const *volatile pa = arr;

    if (re(p1) != 40.0L || im(p1) != 0.0L) return 11;
    if (re(p2) != -2.5L || im(p2) != 0.0L) return 12;
    cld const t = (cld)g_tenth * (cld)g_three;
    if (re(p3) != re(&t) || im(p3) != im(&t)) return 13;
    if (re(p4) != (long double)g_big || im(p4) != 0.0L) return 14;
    if (pc->tag != 5 || re(&pc->v) != 1.5L || im(&pc->v) != 0.0L) return 15;
    if (re(&pa[0]) != 1.0L || im(&pa[0]) != 0.0L) return 16;
    if (re(&pa[1]) != 3.0L || im(&pa[1]) != 0.0L) return 16;
    return 42;
}
