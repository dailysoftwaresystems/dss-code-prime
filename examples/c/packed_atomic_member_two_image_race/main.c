/* D-C-ATOMICS-RUNTIME-PE64-BUS-LOCKS-EVERY-ACCESS-TO-A-CACHE-LINE-STRADDLING-OBJECT
 * — the CROSS-IMAGE witness: two copies of the atomics runtime, one in this exe
 * and one in `dsslib.dll`, racing ONE object that crosses a cache line.
 *
 * ★★★ WHY THIS EXISTS. The pe64 atomics runtime is a static archive member, so
 * every DSS-built image links its own copy. Before P66 each copy served a
 * line-crossing object with a LOCKED instruction, which is atomic against every
 * other locked instruction in the process — and takes a bus lock that stalls
 * every core. P66 replaced it with a copy made under a SOFTWARE lock, and a
 * software lock is only atomic against code that takes the same lock. A lock
 * table private to each image would therefore have silently broken exactly this
 * program: the exe's store would take the exe's lock, the dll's load the dll's,
 * and the load could observe half of the store. The runtime takes the process
 * heap's lock, which `GetProcessHeap` makes ONE per process, so both copies take
 * the same arbiter. This witness is the execution evidence for that; the
 * deterministic evidence — both images' copies observed calling `HeapLock` with
 * the same heap, and neither executing a split-locked instruction — is
 * `tests/program/test_atomics_runtime_no_split_lock`.
 *
 * ★★ THE OBJECT CROSSES THE LINE ON PURPOSE, exactly as in
 * `examples/c/packed_atomic_member_concurrency`: the packed struct is placed 61
 * bytes past a 64-byte-aligned address, so the `_Atomic unsigned` occupies bytes
 * 62..65 and spans the boundary at 64. Inside one line a plain access is atomic
 * and the race would be green over a broken arbiter.
 *
 * ★ THE TEAR DETECTOR IS THE VALUE SET, NOT TIMING. Every stored value has four
 * equal bytes, so a value with unequal bytes can only come from a split read or
 * a split write. Each writer and each reader runs through a DIFFERENT image's
 * copy of the runtime than one of its opponents, in both directions.
 *
 * exit = 7 (the value crosses the image boundary both ways, single-threaded)
 *      + 35 (no torn value observed by either image's reader) = 42.
 */
#include <stdint.h>
#include <threads.h>

#define kRounds 40000

struct __attribute__((packed)) Packed {
    char              c;
    _Atomic unsigned  a;
};

extern void     dss_two_image_store(struct Packed *p, unsigned v);
extern unsigned dss_two_image_load(struct Packed *p);

static unsigned char   g_arena[256];
static struct Packed  *g_p;

static _Atomic unsigned g_stop = 0;
static _Atomic unsigned g_torn = 0;

static struct Packed *straddling(void) {
    uintptr_t const base = ((uintptr_t)(void *)g_arena + 63u) & ~(uintptr_t)63u;
    return (struct Packed *)(void *)(base + 61u);
}

static unsigned pattern(unsigned b) {
    return b | (b << 8) | (b << 16) | (b << 24);
}

static int isTorn(unsigned v) {
    unsigned const b0 = v & 0xFFu;
    return ((v >> 8) & 0xFFu) != b0 || ((v >> 16) & 0xFFu) != b0
           || ((v >> 24) & 0xFFu) != b0;
}

/* Stores through THIS image's copy of the runtime. */
static int exeWriter(void *arg) {
    unsigned const seed = (unsigned)(*(int *)arg);
    for (unsigned i = 0; i < kRounds; ++i) {
        g_p->a = pattern((seed + i) & 0xFFu);
    }
    return 0;
}

/* Stores through the DLL's copy. */
static int dllWriter(void *arg) {
    unsigned const seed = (unsigned)(*(int *)arg);
    for (unsigned i = 0; i < kRounds; ++i) {
        dss_two_image_store(g_p, pattern((seed + i) & 0xFFu));
    }
    return 0;
}

/* Loads through THIS image's copy. */
static int exeReader(void *arg) {
    (void)arg;
    while (g_stop == 0) {
        if (isTorn(g_p->a)) g_torn = 1;
    }
    return 0;
}

/* Loads through the DLL's copy. */
static int dllReader(void *arg) {
    (void)arg;
    while (g_stop == 0) {
        if (isTorn(dss_two_image_load(g_p))) g_torn = 1;
    }
    return 0;
}

int main(void) {
    int score = 0;

    g_p = straddling();

    /* ── arm 1: the value crosses the image boundary in both directions ──── */
    g_p->c = 0x5A;
    g_p->a = 0x11111111u;
    if (dss_two_image_load(g_p) != 0x11111111u) return 1;
    dss_two_image_store(g_p, 0x22222222u);
    if (g_p->a != 0x22222222u) return 2;
    if (g_p->c != 0x5A) return 3;
    score += 7;

    /* ── arm 2: the race, every reader facing a writer in the other image ── */
    {
        thrd_t ew, dw, er, dr;
        int    s1 = 0x11, s2 = 0x77;
        int    rc = 0;

        if (thrd_create(&ew, exeWriter, &s1) != thrd_success) return 4;
        if (thrd_create(&dw, dllWriter, &s2) != thrd_success) return 5;
        if (thrd_create(&er, exeReader, (void *)0) != thrd_success) return 6;
        if (thrd_create(&dr, dllReader, (void *)0) != thrd_success) return 7;

        if (thrd_join(ew, &rc) != thrd_success) return 8;
        if (thrd_join(dw, &rc) != thrd_success) return 9;
        g_stop = 1;
        if (thrd_join(er, &rc) != thrd_success) return 10;
        if (thrd_join(dr, &rc) != thrd_success) return 12;
    }

    if (g_torn != 0) return 11;   /* the two images' copies did not exclude each other */
    if (g_p->c != 0x5A) return 13;
    score += 35;

    return score;   /* 7 + 35 = 42 */
}
