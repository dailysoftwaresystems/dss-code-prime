/* P69 (lane `cs`) — D-CSUBSET-VLA-WIN64-STACK-PROBE: RUNTIME descents of the stack pointer far
 * past one page, through a variable-length array and through `__builtin_alloca`, with the FAR
 * end of each block — its lowest address, the bytes a bare `sub rsp, size` leaves below the
 * Windows guard page — written first and read back. Exit 42 when every group behaves,
 * otherwise a bitmask of the groups that did not:
 *   1 the boundary sizes of a page walk, by VLA and by alloca: a few bytes, exactly one page,
 *     one page and 16 bytes, exactly two pages — every byte written, then summed
 *   2 a VLA in a loop: four three-page descents, each freed when its iteration ends
 *   4 a recursion eight deep, every level holding most of a page of fixed frame and then
 *     descending three pages, its block still live across the call to the next level
 *   8 a VLA of 200000 bytes (49 pages) in a leaf, both ends written and read
 *   16 `__builtin_alloca(300000)` (74 pages) in a leaf, both ends written and read
 *   32 one function descending twice — a 150000-byte VLA, then a 250000-byte alloca below it —
 *      and calling out while both are live (a frame whose outgoing area travels with SP)
 * The groups run shallowest first, so each deep one reaches stack no earlier group touched.
 * Every size derives from argc, so no descent is a constant the optimizer could fold. MSVC
 * has no VLA: under it each VLA is an `_alloca` of the same size instead. */
#if defined(__GNUC__)
#define ALLOCA(n) __builtin_alloca(n)
#define NOINLINE __attribute__((noinline))
#define HAVE_VLA 1
#elif defined(_MSC_VER)
#include <malloc.h>
#define ALLOCA(n) _alloca(n)
#define NOINLINE __declspec(noinline)
#define HAVE_VLA 0
#else
#error "this example knows no runtime stack allocation for this compiler"
#endif

#if HAVE_VLA
#define DECLARE_BLOCK(name, n) volatile char name[n]
#else
#define DECLARE_BLOCK(name, n) volatile char *name = (volatile char *)ALLOCA(n)
#endif

/* A callee, so a caller holding a block is not a leaf. */
static NOINLINE long sum_bytes(volatile char *p, int n) {
    long s = 0;
    for (int i = 0; i < n; ++i) s += p[i];
    return s;
}

static NOINLINE int vla_bytes(int n, int fill) {
    DECLARE_BLOCK(a, n);
    for (int i = 0; i < n; ++i) a[i] = (char)fill;
    long s = 0;
    for (int i = 0; i < n; ++i) s += a[i];
    return s == (long)n * fill;
}

static NOINLINE int alloca_bytes(int n, int fill) {
    volatile char *p = (volatile char *)ALLOCA(n);
    for (int i = 0; i < n; ++i) p[i] = (char)fill;
    long s = 0;
    for (int i = 0; i < n; ++i) s += p[i];
    return s == (long)n * fill;
}

static int boundary_sizes(int one) {
    int const sizes[4] = {one * 3, one * 4096, one * 4096 + 16, one * 8192};
    for (int k = 0; k < 4; ++k) {
        if (!vla_bytes(sizes[k], 1 + k)) return 0;
        if (!alloca_bytes(sizes[k], 5 + k)) return 0;
    }
    return 1;
}

static NOINLINE int vla_in_a_loop(int n) {
    long acc = 0;
    for (int i = 0; i < 4; ++i) {
        DECLARE_BLOCK(a, n);
        a[0] = (char)(i + 1);
        a[n - 1] = (char)(i + 1);
        acc += a[0] + a[n - 1];
    }
    return acc == 2 * (1 + 2 + 3 + 4);
}

static NOINLINE int descend(int depth, int n) {
    volatile char pad[3000];                   /* most of a page, touched only at its top */
    pad[sizeof pad - 1] = 1;
    DECLARE_BLOCK(a, n);
    a[0] = (char)depth;
    a[n - 1] = (char)depth;
    int const deeper = depth == 0 ? 1 : descend(depth - 1, n);
    return deeper && a[0] == (char)depth && a[n - 1] == (char)depth
        && pad[sizeof pad - 1] == 1;
}

static NOINLINE int vla_leaf(int n) {
    DECLARE_BLOCK(a, n);
    a[0] = 1;
    a[n - 1] = 2;
    return a[0] + a[n - 1] == 3;
}

static NOINLINE int alloca_leaf(int n) {
    volatile char *p = (volatile char *)ALLOCA(n);
    p[0] = 1;
    p[n - 1] = 2;
    return p[0] + p[n - 1] == 3;
}

static NOINLINE int twice_calling_out(int n1, int n2) {
    DECLARE_BLOCK(a, n1);
    a[0] = 1;
    a[n1 - 1] = 1;
    volatile char *p = (volatile char *)ALLOCA(n2);
    p[0] = 2;
    p[n2 - 1] = 2;
    long const s = sum_bytes(a, 1) + sum_bytes(a + (n1 - 1), 1)
                 + sum_bytes(p, 1) + sum_bytes(p + (n2 - 1), 1);
    return s == 6;
}

int main(int argc, char **argv) {
    (void)argv;
    int const one = argc > 0 ? 1 : 0;
    int bad = 0;
    if (!boundary_sizes(one)) bad |= 1;
    if (!vla_in_a_loop(one * 3 * 4096)) bad |= 2;
    if (!descend(one * 7, one * 3 * 4096)) bad |= 4;
    if (!vla_leaf(one * 200000)) bad |= 8;
    if (!alloca_leaf(one * 300000)) bad |= 16;
    if (!twice_calling_out(one * 150000, one * 250000)) bad |= 32;
    return bad == 0 ? 42 : bad;
}
