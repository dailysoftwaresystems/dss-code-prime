/* STATIC AGGREGATES WHOSE LEAVES ARE 16 BYTES WIDE — `long double` members and
   elements (x87 extended on x86_64 Linux, binary128 on aarch64 Linux, `double`
   on Windows and Apple arm64) and `__int128` members, each past a narrower
   member so a leaf written at the wrong offset, short, or in the wrong format
   shows. Every object is read through a `volatile` pointer, so both pipelines
   read the emitted image rather than a folded copy of the initializer.
   (D-CSUBSET-LONG-DOUBLE-AGGREGATE-GLOBAL,
    D-CSUBSET-INT128-AGGREGATE-MEMBER-STATIC-INITIALIZER-REFUSED)

   Exit 42 = every value holds; 11-18 names the first one that does not:
     11  struct { char; long double; int }     — a member between narrower ones
     12  a struct nested in a struct           — the inner member's offset
     13  a long double array inside a struct   — the stride after a nested struct
     14  union { long double; int }            — the union's first member
     15  0.1L and -0.1L                        — not binary64 values: the image
                                                 carries the full-precision value
     16  struct { char; __int128; ... }        — a negative 128-bit member
     17  unsigned __int128 past 2^64           — both 64-bit halves
     18  __int128 pair[2]                      — an array of 128-bit elements */
struct S { char c; long double x; int tail; };
struct N { struct S inner; long double more[2]; };
union U { long double x; int i; };
struct W { char c; __int128 v; unsigned __int128 w; };

static struct S s = { 'a', 40.5L, 7 };
static long double arr[3] = { 40.0L, 1.5L, 0.5L };
static struct N n = { { 'b', 1.25L, 3 }, { 0.25L, 0.5L } };
static union U u = { 2.0L };
static long double const tenth[2] = { 0.1L, -0.1L };
static struct W wi = { 'x', -3, ((unsigned __int128)1 << 100) | 5 };
static __int128 pair[2] = { 40, 2 };

long double g_one = 1.0L, g_ten = 10.0L; /* read at run time */

int main(void) {
    struct S const *volatile ps = &s;
    long double const *volatile pa = arr;
    struct N const *volatile pn = &n;
    union U const *volatile pu = &u;
    long double const *volatile pt = tenth;
    struct W const *volatile pw = &wi;
    __int128 const *volatile pp = pair;

    if (ps->c != 'a' || ps->x != 40.5L || ps->tail != 7) return 11;
    if (pn->inner.c != 'b' || pn->inner.x != 1.25L || pn->inner.tail != 3) return 12;
    if (pn->more[0] != 0.25L || pn->more[1] != 0.5L) return 13;
    if (pu->x != 2.0L) return 14;
    long double const q = g_one / g_ten; /* the correctly rounded 0.1L */
    if (pt[0] != q || pt[1] != -q) return 15;
    if (pw->c != 'x' || pw->v != -3) return 16;
    if ((pw->w >> 100) != 1 || (unsigned)(pw->w & 0xff) != 5) return 17;
    if (pp[0] + pp[1] != 42) return 18;
    return (int)(pa[0] + pa[1] + pa[2]); /* 40 + 1.5 + 0.5 */
}
