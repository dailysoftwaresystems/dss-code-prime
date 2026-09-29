// P68 round 9 (lane `cs`): the CURRENT OBJECT of a brace list (C 6.7.9p17-p20).
// An element initializes the next subobject in order, or the one its designator
// names — and initialization continues after that subobject, INSIDE its aggregate.
// A value that meets an aggregate subobject without braces initializes that
// aggregate's members in turn (brace elision) unless it initializes the whole
// subobject (a string for a character array, a structure value). A union takes one
// member. Every line lands its elements where C says; `bad` counts the ones that
// did not, so the exit code names how many placements went wrong.
struct P { int x, y; };
struct I { int a, b; };
struct O { struct I i; int c; };
struct B { int a; unsigned : 3; int b; };
union U { struct { int a, b; } s; int i; };
struct S { union { int i; char c; } u; int k; };
struct V { char s[4]; int x; };
struct T { int x; union { int a; float f; }; int y; };

static int g[][2] = { 1, 2, 3, 4 };                  // file scope: two rows
static struct P gps[] = { 5, 6, 7, 8 };              // file-scope structure elements

int main(void) {
    int n = 40;
    int bad = 0;

    int a[2][2] = { 1, 2, 3, n };                     // rows elided
    bad += a[0][0] != 1 || a[0][1] != 2 || a[1][0] != 3 || a[1][1] != 40;

    int b[][2] = { 1, 2, 3 };                         // unknown size: TWO rows, not three
    bad += sizeof(b) != 4 * sizeof(int) || b[1][0] != 3 || b[1][1] != 0;

    struct P ps[] = { 1, 2, 3, n };                   // structure elements elided
    bad += sizeof(ps) != 2 * sizeof(struct P) || ps[1].x != 3 || ps[1].y != 40;

    int d[][2] = { [1][0] = 5, 6 };                   // a deep designator continues INSIDE its row
    bad += sizeof(d) != 4 * sizeof(int) || d[1][0] != 5 || d[1][1] != 6 || d[0][0] != 0;

    struct O o = { .i.a = 7, 8 };                     // ... inside the designated member
    bad += o.i.a != 7 || o.i.b != 8 || o.c != 0;

    struct O o2 = { .i.b = 9, 10 };                   // ... and leaves it when it is complete
    bad += o2.i.a != 0 || o2.i.b != 9 || o2.c != 10;

    union U u = { 11, 12 };                           // a union's first member, elided
    bad += u.s.a != 11 || u.s.b != 12;

    struct S s = { .u.i = 13, 14 };                   // through a union, then the next member
    bad += s.u.i != 13 || s.k != 14;

    struct T t = { .a = 15, 16 };                     // a member of an anonymous union
    bad += t.x != 0 || t.a != 15 || t.y != 16;

    struct B bs[] = { 17, 18, 19, 20 };               // an unnamed bit-field is skipped
    bad += sizeof(bs) != 2 * sizeof(struct B) || bs[0].b != 18 || bs[1].a != 19 || bs[1].b != 20;

    struct V v[] = { "ab", 21, "cd", 22 };            // a string initializes its array whole
    bad += sizeof(v) != 2 * sizeof(struct V) || v[0].s[1] != 'b' || v[0].s[2] != 0
         || v[1].s[0] != 'c' || v[0].x != 21 || v[1].x != 22;

    struct P q = { 23, 24 };
    struct P qs[] = { q, 25, 26 };                    // a structure VALUE is not elided
    bad += sizeof(qs) != 2 * sizeof(struct P) || qs[0].y != 24 || qs[1].x != 25 || qs[1].y != 26;

    int e[3][2] = { 1, 2, { 3, 4 }, 5 };              // a braced row after an elided one
    bad += e[1][0] != 3 || e[1][1] != 4 || e[2][0] != 5 || e[2][1] != 0;

    int (*cl)[2] = (int[][2]){ 1, 2, 3, n };          // a compound literal
    bad += cl[1][0] != 3 || cl[1][1] != 40 || sizeof((int[][2]){ 1, 2, 3 }) != 4 * sizeof(int);

    bad += sizeof(g) != 4 * sizeof(int) || g[1][1] != 4;
    bad += sizeof(gps) != 2 * sizeof(struct P) || gps[1].x != 7 || gps[1].y != 8;

    return bad == 0 ? 42 : bad;
}
