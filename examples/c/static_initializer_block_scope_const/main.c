/* P69 (lane `cs`) — D-C-A-BLOCK-SCOPE-CONST-OBJECTS-VALUE-IN-A-STATIC-INITIALIZER-IS-REFUSED:
 * a static local's initializer may read a block-scope CONST object's value, exactly as it may
 * read a file-scope one's — the object is const, not volatile, and its own initializer is a
 * constant. Exit 42 when every static below holds its value, otherwise a bitmask:
 *   1 a scalar, 2 an element, 4 a member, 8 a chain (`b = a + 2`), 16 a nested block and a
 *   shadowed name, 32 a whole-structure copy, 64 an enumeration-typed const, 128 a const
 *   local POINTER holding a file-scope object's address, 256 a volatile MEMBER of a const
 *   local (the object is what decides), 512 an array whose initializer names a const.
 * gcc 13.3.0 builds all but 8 and 512, clang 18.1.3 all but 256; DSS builds the union. */
static int g = 42;
const int k_file = 1;

int main(int argc, char **argv) {
    (void)argv;
    int bad = 0;
    const int k = 42;
    static int x1 = k;
    if (x1 != 42) bad |= 1;

    const int a[2] = { 1, 42 };
    static int x2 = a[1];
    if (x2 != 42) bad |= 2;

    const struct { int p; int q; } s = { 1, 42 };
    static int x3 = s.q;
    if (x3 != 42) bad |= 4;

    const int c1 = 40;
    const int c2 = c1 + 2;
    static int x4 = c2;
    if (x4 != 42) bad |= 8;

    {
        const int k_file = 42;   /* shadows the file-scope one */
        static int x5 = k_file;
        if (x5 != 42) bad |= 16;
    }

    const struct P { int u, v; } ps = { 40, 2 };
    static struct P x6 = ps;
    if (x6.u + x6.v != 42) bad |= 32;

    enum E { A = 40 };
    const enum E e = A;
    static int x7 = e + 2;
    if (x7 != 42) bad |= 64;

    int *const gp = &g;
    static int *x8 = gp;
    if (*x8 != 42) bad |= 128;

    const struct { volatile int w; } vs = { 42 };
    static int x9 = vs.w;
    if (x9 != 42) bad |= 256;

    const int n = 2;
    const int arr[2] = { 40, n };
    static int x10 = arr[0] + arr[1];
    if (x10 != 42) bad |= 512;

    return bad == 0 ? 42 + (argc - argc) : bad;
}
