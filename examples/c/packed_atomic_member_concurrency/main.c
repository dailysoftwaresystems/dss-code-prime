/* D-C-ATOMICS-RUNTIME-IS-OURS-ON-PE64 — the CONCURRENCY witness for the generic
 * atomics runtime an UNDER-ALIGNED `_Atomic` access lowers to.
 *
 * ★★★ WHY A SECOND EXAMPLE, AND WHY IT IS NOT `packed_atomic_member` WITH MORE
 * ITERATIONS. Its sibling proves the ROUND TRIP: the value written through an
 * under-aligned `_Atomic` member comes back. That is a true answer to a NARROWER
 * question — it holds for a completely NON-ATOMIC implementation, and the
 * pre-fix binary returned the same exit code. The property that actually matters
 * cannot be observed by one thread at all: whether every update was INDIVISIBLE.
 * A wrong atomics runtime does not crash; it produces a program that computes
 * the wrong thing under contention, occasionally. So this witness races.
 *
 * ★★ THE OBJECT IS PLACED TO STRADDLE A CACHE LINE, DELIBERATELY. On x86-64 a
 * plain misaligned 4-byte load is atomic in practice whenever the four bytes sit
 * inside one cache line — so a witness that did not force the split would be
 * green over an implementation that had lost atomicity entirely. The packed
 * struct is placed 61 bytes past a 64-byte-aligned address inside a byte arena,
 * so its `_Atomic unsigned` lands at byte 62 of a line and spans the boundary
 * into the next one. That is exactly the case where an unlocked access TEARS.
 * ✔MEASURED that the detector is not vacuous: with `x86_64.target.json`'s
 * `atomics.underAlignedNativeForm` mutated to `remainsAtomic` — the native
 * `movl`/`xchgl` pair, which is what gcc inlines — this program returns 11 (a
 * torn value observed) instead of 42, on this host, in under a second.
 *
 * ★ THE TEAR DETECTOR IS THE VALUE SET, NOT AN ASSERTION ABOUT TIMING. Every
 * value a writer stores has all four bytes EQUAL (0x00000000, 0x01010101, …).
 * Any value whose four bytes are not all equal can only have come from a read or
 * a write that was split, so a reader that finds one has caught a lost
 * atomicity. No barrier, sleep or scheduler assumption is involved: the check is
 * on the VALUE, and a correct implementation can never produce a mixed one no
 * matter how the threads interleave.
 *
 * ★ THE NEIGHBOUR WITNESS IS THE HALF A TEAR TEST CANNOT SEE. C11 §6.5.1p2 makes
 * `Packed::c` a distinct memory location from `Packed::a`, updatable
 * concurrently without a data race. An implementation that served the
 * under-aligned store by read-modify-writing the containing 8-byte block would
 * pass every tear check above and still be wrong here. A third thread hammers
 * `c` with a fixed byte throughout; if the final byte is ever anything else, the
 * atomics runtime wrote through a neighbour it does not own.
 *
 * ⚠ FOLD-RESISTANT BY CONSTRUCTION: the values cross real thread boundaries and
 * the object is `_Atomic`, so no pass may cache, sink or drop the accesses. The
 * `release` arm therefore exercises the same route as the baseline.
 *
 * exit = 7 (round trip) + 11 (no torn value ever observed) + 24 (the neighbour
 *        survived) = 42.  Each arm returns its OWN code on failure so a red
 *        names which property broke instead of collapsing to 1.
 */
#include <stdint.h>
#include <threads.h>

#define kRounds 40000

struct __attribute__((packed)) Packed {
    char              c;
    _Atomic unsigned  a;
};

/* ★ THE ADDRESS IS CHOSEN AT RUNTIME, NOT BY AN ALIGNMENT ATTRIBUTE, AND THAT
 * IS THE STRONGER SHAPE. `g_p` is placed 61 bytes past a 64-byte-aligned
 * address inside a plain byte arena, so the packed member's `_Atomic unsigned`
 * lands at byte 62 of a cache line and its four bytes span the boundary at 64.
 * Reaching it through a `struct Packed *` also keeps the lvalue's PROVABLE
 * alignment at 1 — the packed struct's own — so the access still routes through
 * the atomics runtime exactly as a direct member access does, while no pass can
 * fold the address away. */
static unsigned char   g_arena[256];
static struct Packed  *g_p;

static struct Packed *straddling(void) {
    uintptr_t const base = ((uintptr_t)(void *)g_arena + 63u) & ~(uintptr_t)63u;
    return (struct Packed *)(void *)(base + 61u);
}

static _Atomic unsigned g_stop      = 0;
static _Atomic unsigned g_torn      = 0;   /* a reader saw a mixed value */
static _Atomic unsigned g_neighbour = 0;   /* a reader saw `c` clobbered */

static int writer(void *arg) {
    unsigned const seed = (unsigned)(*(int *)arg);
    for (unsigned i = 0; i < kRounds; ++i) {
        unsigned const b = (seed + i) & 0xFFu;
        g_p->a = b | (b << 8) | (b << 16) | (b << 24);
    }
    return 0;
}

static int reader(void *arg) {
    (void)arg;
    while (g_stop == 0) {
        unsigned const v  = g_p->a;
        unsigned const b0 = v & 0xFFu;
        if (((v >> 8) & 0xFFu) != b0 || ((v >> 16) & 0xFFu) != b0
            || ((v >> 24) & 0xFFu) != b0) {
            g_torn = 1;
        }
    }
    return 0;
}

/* Hammers the byte that shares the packed struct with the atomic. It is a PLAIN
 * `char`, written non-atomically, exactly as C11 permits beside an atomic
 * neighbour. */
static int neighbour(void *arg) {
    (void)arg;
    while (g_stop == 0) {
        g_p->c = 0x5A;
        if (g_p->c != 0x5A) g_neighbour = 1;
    }
    return 0;
}

int main(void) {
    int score = 0;

    g_p = straddling();

    /* ── arm 1: the round trip, single-threaded ─────────────────────────── */
    g_p->c = 0x5A;
    g_p->a = 0x11111111u;
    if (g_p->a != 0x11111111u) return 1;
    if (g_p->c != 0x5A) return 2;
    score += 7;

    /* ── arms 2 and 3: the race ─────────────────────────────────────────── */
    {
        thrd_t w1, w2, r1, nb;
        int    s1 = 0x11, s2 = 0x77;
        int    rc = 0;

        if (thrd_create(&w1, writer, &s1) != thrd_success) return 3;
        if (thrd_create(&w2, writer, &s2) != thrd_success) return 4;
        if (thrd_create(&r1, reader, (void *)0) != thrd_success) return 5;
        if (thrd_create(&nb, neighbour, (void *)0) != thrd_success) return 6;

        if (thrd_join(w1, &rc) != thrd_success) return 7;
        if (thrd_join(w2, &rc) != thrd_success) return 8;
        g_stop = 1;
        if (thrd_join(r1, &rc) != thrd_success) return 9;
        if (thrd_join(nb, &rc) != thrd_success) return 10;
    }

    if (g_torn != 0) return 11;       /* an update was NOT indivisible */
    score += 11;

    if (g_neighbour != 0) return 12;  /* the runtime wrote a byte it does not own */
    if (g_p->c != 0x5A) return 13;
    score += 24;

    return score;   /* 7 + 11 + 24 = 42 */
}
