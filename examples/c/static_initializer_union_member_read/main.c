/* P69 (lane `cs`) — D-C-A-CONST-UNION-MEMBER-READ-IN-A-STATIC-INITIALIZER-IS-REFUSED: a
 * static initializer may read the ACTIVE member of a const union — the member its
 * initializer made current — and nothing else. Exit 42 when every read below holds its
 * value, otherwise a bitmask of the reads that did not:
 *   1 the first member, 2 a designated member of ANOTHER type than the first,
 *   4 a union inside a structure, 8 an element of an array member,
 *   16 a designated member of the SAME type as the first, 32 a block-scope const union,
 *   64 a structure member of a union.
 * (Bit 2's union OBJECT is emitted too: its static image is the member it names —
 * D-C-A-STATIC-UNION-INITIALIZED-THROUGH-A-LATER-MEMBER-IS-NOT-ENCODED, closed in P69.)
 * gcc 13.3.0 and clang 18.1.3 build every one (both modes); MSVC 19.51 none (C2099). */
static const union { int i; float f; } u1 = { 42 };
static const union { int i; float f; } u2 = { .f = 42.0f };
static const struct { int a; union { int i; float f; } u; } u3 = { 1, { 42 } };
static const union { int a[2]; long l; } u4 = { { 1, 42 } };
static const union { int a; int b; } u5 = { .b = 42 };
static const union { struct { int p, q; } s; long l; } u7 = { { 40, 2 } };

int r1 = u1.i;
int r2 = (int)u2.f;
int r3 = u3.u.i;
int r4 = u4.a[1];
int r5 = u5.b;
int r7 = u7.s.p + u7.s.q;

int main(int argc, char **argv) {
    (void)argv;
    int bad = 0;
    const union { int i; float f; } u6 = { 42 };
    static int r6 = u6.i;
    if (r1 != 42) bad |= 1;
    if (r2 != 42 || u2.f != 42.0f) bad |= 2;
    if (r3 != 42) bad |= 4;
    if (r4 != 42) bad |= 8;
    if (r5 != 42) bad |= 16;
    if (r6 != 42) bad |= 32;
    if (r7 != 42) bad |= 64;
    return bad == 0 ? 42 + (argc - argc) : bad;
}
