/* D-C-ATOMIC-COMPOUND-ASSIGNMENT-AND-INCREMENT-ARE-A-LOAD-THEN-A-SEPARATE-STORE
 * — the RACE witness for DSS's pe64 generic compare-exchange entry: compare-
 * exchange MIXED with plain store and load on ONE object that crosses a cache
 * line.
 *
 * ★★★ WHAT IT PROVES. The body serves every access to a line-crossing object —
 * load, store AND compare-exchange — under one arbiter, the process lock. If
 * the compare-exchange took a different arbiter from the store or the load, or
 * none, a torn value would be observed or an update would be lost. Both are
 * checked on VALUES, never on timing.
 *
 * ★★ THE ENCODING MAKES A TEAR VISIBLE ACROSS THE LINE. The object is a 4-byte
 * `_Atomic unsigned` at byte 62 of a 64-byte line: its low two bytes sit in one
 * line and its high two in the next. Every value written is
 * `k | (~k << 16)` for a 16-bit k, so the half in each line must be the
 * complement of the other; any mix of two different writes fails that check.
 *
 * ★ THE ARMS.
 *   1  single-threaded: one compare-exchange succeeds and the load reads it.
 *   2  EXACT COUNT: two threads each complete 30 000 increments with the
 *      load/compute/compare-exchange/retry loop — the loop compound assignment
 *      lowers to — while a reader loads and a neighbour thread hammers the
 *      byte below the object. The final value must be exactly 60 000; one
 *      lost update is a red, and so is any torn value seen by the reader or
 *      written back through `expected` by a failing compare-exchange.
 *   3  compare-exchange RACING A PLAIN STORE: one thread stores 30 000 values,
 *      another compare-exchanges from whatever it observed, a reader loads.
 *      No count is possible here, so the check is that nothing torn is ever
 *      seen, stored or written back, and that the neighbour byte survived.
 *
 * ⚠ The accesses are the generic runtime entries: `g_p->a` through a pointer
 * to a packed struct is a store or a load of the runtime, and the compare-
 * exchange is called by name, as the compound-assignment lowering will.
 *
 * exit = 7 (arm 1) + 17 (arm 2) + 18 (arm 3) = 42; each arm returns its own
 * code on failure: 1-2 arm 1, 11 torn in arm 2, 12 a lost update, 13 torn in
 * arm 3, 14 a torn final value, 15 the neighbour clobbered.
 */
#include <stddef.h>
#include <stdint.h>
#include <threads.h>

#define kIncrementsPerThread 30000u
#define kStoreRounds 30000u

extern _Bool __atomic_compare_exchange(size_t size, void *mem, void *expected,
                                       void *desired, int success, int failure);

struct __attribute__((packed)) Packed {
    char              c;
    _Atomic unsigned  a;
};

static unsigned char   g_arena[256];
static struct Packed  *g_p;
static void           *g_mem;

static _Atomic unsigned g_stop      = 0;
static _Atomic unsigned g_invalid   = 0;
static _Atomic unsigned g_neighbour = 0;

static unsigned encode(unsigned k) {
    unsigned const low = k & 0xFFFFu;
    return low | ((~low & 0xFFFFu) << 16);
}
static int isWhole(unsigned v) { return ((v >> 16) & 0xFFFFu) == (~v & 0xFFFFu); }
static unsigned decode(unsigned v) { return v & 0xFFFFu; }

static int incrementer(void *arg) {
    (void)arg;
    for (unsigned i = 0; i < kIncrementsPerThread; ++i) {
        unsigned expected = g_p->a;
        for (;;) {
            if (!isWhole(expected)) g_invalid = 1;
            unsigned desired = encode(decode(expected) + 1u);
            if (__atomic_compare_exchange(4, g_mem, &expected, &desired, 5, 5)) break;
        }
    }
    return 0;
}

static int reader(void *arg) {
    (void)arg;
    while (g_stop == 0) {
        if (!isWhole(g_p->a)) g_invalid = 1;
    }
    return 0;
}

static int neighbour(void *arg) {
    (void)arg;
    while (g_stop == 0) {
        g_p->c = 0x5A;
        if (g_p->c != 0x5A) g_neighbour = 1;
    }
    return 0;
}

static int storer(void *arg) {
    (void)arg;
    for (unsigned i = 0; i < kStoreRounds; ++i) {
        g_p->a = encode(0x8000u + (i & 0x3FFFu));
    }
    return 0;
}

static int swapper(void *arg) {
    (void)arg;
    while (g_stop == 0) {
        unsigned expected = g_p->a;
        if (!isWhole(expected)) g_invalid = 1;
        unsigned desired = encode(decode(expected) ^ 0x0101u);
        if (!__atomic_compare_exchange(4, g_mem, &expected, &desired, 5, 5)) {
            if (!isWhole(expected)) g_invalid = 1;
        }
    }
    return 0;
}

int main(void) {
    int score = 0;
    uintptr_t const base = ((uintptr_t)(void *)g_arena + 63u) & ~(uintptr_t)63u;
    g_p   = (struct Packed *)(void *)(base + 61u);
    g_mem = (void *)(base + 62u);

    /* ── arm 1 ────────────────────────────────────────────────────────────── */
    g_p->c = 0x5A;
    g_p->a = encode(0u);
    {
        unsigned expected = encode(0u);
        unsigned desired = encode(1u);
        if (!__atomic_compare_exchange(4, g_mem, &expected, &desired, 5, 5)) return 1;
        if (g_p->a != encode(1u)) return 2;
    }
    g_p->a = encode(0u);
    score += 7;

    /* ── arm 2: the exact count ───────────────────────────────────────────── */
    {
        thrd_t first, second, load, nb;
        int    rc = 0;
        g_stop = 0;
        if (thrd_create(&load, reader, (void *)0) != thrd_success) return 20;
        if (thrd_create(&nb, neighbour, (void *)0) != thrd_success) return 21;
        if (thrd_create(&first, incrementer, (void *)0) != thrd_success) return 22;
        if (thrd_create(&second, incrementer, (void *)0) != thrd_success) return 23;
        if (thrd_join(first, &rc) != thrd_success) return 24;
        if (thrd_join(second, &rc) != thrd_success) return 25;
        g_stop = 1;
        if (thrd_join(load, &rc) != thrd_success) return 26;
        if (thrd_join(nb, &rc) != thrd_success) return 27;
    }
    if (g_invalid != 0) return 11;
    if (decode(g_p->a) != 2u * kIncrementsPerThread) return 12;
    score += 17;

    /* ── arm 3: compare-exchange racing a plain store ─────────────────────── */
    {
        thrd_t store, swap, load, nb;
        int    rc = 0;
        g_stop = 0;
        if (thrd_create(&load, reader, (void *)0) != thrd_success) return 30;
        if (thrd_create(&nb, neighbour, (void *)0) != thrd_success) return 31;
        if (thrd_create(&swap, swapper, (void *)0) != thrd_success) return 32;
        if (thrd_create(&store, storer, (void *)0) != thrd_success) return 33;
        if (thrd_join(store, &rc) != thrd_success) return 34;
        g_stop = 1;
        if (thrd_join(swap, &rc) != thrd_success) return 35;
        if (thrd_join(load, &rc) != thrd_success) return 36;
        if (thrd_join(nb, &rc) != thrd_success) return 37;
    }
    if (g_invalid != 0) return 13;
    if (!isWhole(g_p->a)) return 14;
    if (g_neighbour != 0 || g_p->c != 0x5A) return 15;
    score += 18;

    return score;   /* 7 + 17 + 18 = 42 */
}
