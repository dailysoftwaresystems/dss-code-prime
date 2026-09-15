/* D-C-ATOMIC-COMPOUND-ASSIGNMENT-AND-INCREMENT-ARE-A-LOAD-THEN-A-SEPARATE-STORE —
 * the THREADED witness that `E1 op= E2`, `++` and `--` on an `_Atomic` object are
 * ONE indivisible read-modify-write, for every naturally-aligned object shape.
 *
 * ★★★ WHAT WAS BROKEN. DSS lowered `x += v` on an `_Atomic` object to an atomic
 * LOAD, the arithmetic, and a separate atomic STORE — two indivisible accesses
 * with nothing tying them together — so a second thread's update landing between
 * them was silently overwritten. ✔MEASURED at `caf053eb`: a two-thread race probe
 * exited 115 on pe64 and on elf64 (four of five forms lost updates) while MSVC's
 * build of the same source exited 100. C23 6.5.17.3p4: "If E1 has an atomic type,
 * compound assignment is a read-modify-write operation"; 6.5.3.5p2 says the same
 * of postfix `++`. `c11_atomic_rmw` could not see it: it is single-threaded, and
 * one thread cannot observe a lost update.
 *
 * ★★ EVERY VERDICT IS AN EXACT VALUE, NEVER A TIMING. Two threads each apply the
 * operation kRounds times; the final value is fixed by arithmetic alone, whatever
 * the interleaving, and ANY lost update moves it:
 *   +=   +1 per round              → exactly 2N
 *   -=   -1 per round, from 2N     → exactly 0
 *   *=   ×3 per round, from 1      → exactly 3^(2N) mod 2^32 (multiplication
 *                                     commutes, so order cannot change it)
 *   ++ / --                        → exactly 2N / 0
 *   a sub-word object              → exactly 2N mod 256
 *   a 64-bit object, += 3          → exactly 6N
 *   an aligned struct member       → exactly 2N
 *   an atomic POINTER, p++ / p += 1 → exactly 2N ELEMENTS past the base (the
 *                                     stride scales, so a byte-wise step reds too)
 *   the VALUES `++x` and `x++` yield → together exactly {1..2N} and {0..2N-1},
 *                                     each value once — a lost update yields a
 *                                     duplicate, and a re-read of the object
 *                                     instead of the committed value yields one
 *                                     too.
 * ⓘ The architecture matters: x86-64 commits through `lock cmpxchg`, arm64 through
 * a real LL/SC retry loop over `ldaxr`/`stlxr`. An x86-only witness would say
 * nothing about the exclusive-pair path, so this example runs on every leg.
 *
 * ⚠ FOLD-RESISTANT: every object is a mutable global crossing a real thread
 * boundary, and every access is `_Atomic`, so no pass may cache, sink or drop one.
 *
 * exit = 42. Every arm returns its OWN code on failure, so a red names the form.
 */
#include <stdint.h>
#include <threads.h>

#define kRounds 100000u

static _Atomic unsigned           g_add;
static _Atomic unsigned           g_sub = 2u * kRounds;
static _Atomic unsigned           g_mul = 1u;
static _Atomic unsigned           g_inc;
static _Atomic unsigned           g_dec = 2u * kRounds;
static _Atomic unsigned char      g_byte;
static _Atomic long long          g_wide;
static _Atomic unsigned           g_pre;
static _Atomic unsigned           g_post;

struct Holder { char tag; _Atomic unsigned member; };
static struct Holder g_holder;

static int  g_cells[2u * kRounds + 1u];
static int *_Atomic g_ptr = g_cells;

static unsigned g_pre_seen_a[kRounds];
static unsigned g_pre_seen_b[kRounds];
static unsigned g_post_seen_a[kRounds];
static unsigned g_post_seen_b[kRounds];
static unsigned char g_hits[2u * kRounds + 1u];

static int add_worker(void *arg)    { (void)arg; for (unsigned i = 0; i < kRounds; ++i) g_add += 1u; return 0; }
static int sub_worker(void *arg)    { (void)arg; for (unsigned i = 0; i < kRounds; ++i) g_sub -= 1u; return 0; }
static int mul_worker(void *arg)    { (void)arg; for (unsigned i = 0; i < kRounds; ++i) g_mul *= 3u; return 0; }
static int inc_worker(void *arg)    { (void)arg; for (unsigned i = 0; i < kRounds; ++i) g_inc++; return 0; }
static int dec_worker(void *arg)    { (void)arg; for (unsigned i = 0; i < kRounds; ++i) --g_dec; return 0; }
static int byte_worker(void *arg)   { (void)arg; for (unsigned i = 0; i < kRounds; ++i) g_byte += 1; return 0; }
static int wide_worker(void *arg)   { (void)arg; for (unsigned i = 0; i < kRounds; ++i) g_wide += 3; return 0; }
static int member_worker(void *arg) { (void)arg; for (unsigned i = 0; i < kRounds; ++i) g_holder.member += 1u; return 0; }
static int ptr_inc_worker(void *arg) { (void)arg; for (unsigned i = 0; i < kRounds; ++i) g_ptr++; return 0; }
static int ptr_add_worker(void *arg) { (void)arg; for (unsigned i = 0; i < kRounds; ++i) g_ptr += 1; return 0; }

static int pre_worker(void *arg) {
    unsigned *const seen = (unsigned *)arg;
    for (unsigned i = 0; i < kRounds; ++i) seen[i] = ++g_pre;
    return 0;
}

static int post_worker(void *arg) {
    unsigned *const seen = (unsigned *)arg;
    for (unsigned i = 0; i < kRounds; ++i) seen[i] = g_post++;
    return 0;
}

typedef int (*Worker)(void *);

/* Every value in [lo, lo + 2N) must have been yielded exactly once across the two
 * threads' records. Runs after the joins, on one thread. */
static int each_value_once(unsigned const *a, unsigned const *b, unsigned lo) {
    for (unsigned i = 0; i < 2u * kRounds + 1u; ++i) g_hits[i] = 0;
    for (unsigned i = 0; i < kRounds; ++i) {
        unsigned const va = a[i] - lo, vb = b[i] - lo;
        if (va >= 2u * kRounds || vb >= 2u * kRounds) return 0;
        if (g_hits[va]++ != 0 || g_hits[vb]++ != 0) return 0;
    }
    return 1;
}

int main(void) {
    struct { Worker fn; void *arg; } const plan[] = {
        { add_worker, 0 },    { add_worker, 0 },
        { sub_worker, 0 },    { sub_worker, 0 },
        { mul_worker, 0 },    { mul_worker, 0 },
        { inc_worker, 0 },    { inc_worker, 0 },
        { dec_worker, 0 },    { dec_worker, 0 },
        { byte_worker, 0 },   { byte_worker, 0 },
        { wide_worker, 0 },   { wide_worker, 0 },
        { member_worker, 0 }, { member_worker, 0 },
        { ptr_inc_worker, 0 }, { ptr_add_worker, 0 },
        { pre_worker, g_pre_seen_a },   { pre_worker, g_pre_seen_b },
        { post_worker, g_post_seen_a }, { post_worker, g_post_seen_b },
    };
    enum { kThreads = sizeof plan / sizeof plan[0] };
    thrd_t threads[kThreads];
    for (int i = 0; i < kThreads; ++i) {
        if (thrd_create(&threads[i], plan[i].fn, plan[i].arg) != thrd_success) return 90;
    }
    int rc = 0;
    for (int i = 0; i < kThreads; ++i) {
        if (thrd_join(threads[i], &rc) != thrd_success) return 91;
    }

    unsigned mul_want = 1u;
    for (unsigned i = 0; i < 2u * kRounds; ++i) mul_want *= 3u;

    if (g_add != 2u * kRounds)                         return 1;
    if (g_sub != 0u)                                   return 2;
    if (g_mul != mul_want)                             return 3;
    if (g_inc != 2u * kRounds)                         return 4;
    if (g_dec != 0u)                                   return 5;
    if (g_byte != (unsigned char)(2u * kRounds))       return 6;
    if (g_wide != 6LL * (long long)kRounds)            return 7;
    if (g_holder.member != 2u * kRounds)               return 8;
    if (g_ptr - g_cells != (long)(2u * kRounds))       return 9;
    if (g_pre != 2u * kRounds)                         return 10;
    if (!each_value_once(g_pre_seen_a, g_pre_seen_b, 1u))   return 11;
    if (g_post != 2u * kRounds)                        return 12;
    if (!each_value_once(g_post_seen_a, g_post_seen_b, 0u)) return 13;
    return 42;
}
