/* D-C-ATOMIC-COMPOUND-ASSIGNMENT-AND-INCREMENT-ARE-A-LOAD-THEN-A-SEPARATE-STORE —
 * the THREADED witness for FLOATING `_Atomic` objects, and the value semantics
 * that make their read-modify-write a loop over the REPRESENTATION.
 *
 * ★★★ WHAT IS REQUIRED, AND WHY DSS HAD NONE OF IT. Every reference makes
 * `d += 1.0` on an `_Atomic double` work under contention — ✔MEASURED a
 * two-thread race over `d += 1.0`, `f += 1.0f`, `++dd` and a three-step update
 * exits clean on gcc 13.3.0 and clang 18.1.3 (-O0 and -O2) and on MSVC 19.51
 * (`/experimental:c11atomics`, /Od and /O2), three runs each. DSS lowered NO fenced
 * access of a floating `_Atomic` at all: every load and store was refused in
 * `mir_to_lir` as "a float _Atomic is a named deferral". So it is not enough for
 * the compound assignment to be indivisible — the plain reads and writes that
 * observe it had to exist too, and this example uses both.
 *
 * ★★ THE COMMIT COMPARES BITS, NOT VALUES, AND TWO ARMS PIN WHY. C23 7.17.7.4
 * defines compare-exchange over "the contents of the memory". A loop that
 * committed only when the observed VALUE equalled the expected one would never
 * commit over a NaN (NaN != NaN — the arm below would hang, not fail), and would
 * treat +0.0 and -0.0 as the same object state.
 *
 * ★ EVERY VERDICT IS AN EXACT VALUE: all sums are integers far below 2^53 (and
 * 2^24 for float), exactly representable, so no rounding can move them — only a
 * lost update can.
 *
 * ⓘ WHY EVERY LEG: x86-64 commits the 64 bits through `lock cmpxchg`, arm64 through
 * a real LL/SC loop over `ldaxr`/`stlxr`, and the value crosses register classes
 * (MOVQ/MOVD, FMOV) at both ends of each loop.
 *
 * exit = 42; each arm returns its own code on failure.
 */
#include <stdatomic.h>
#include <threads.h>

#define kRounds 100000

static _Atomic double g_add;
static _Atomic float  g_fadd;
static _Atomic double g_inc;
static _Atomic double g_multi;

static int add_worker(void *arg)   { (void)arg; for (int i = 0; i < kRounds; ++i) g_add += 1.0; return 0; }
static int fadd_worker(void *arg)  { (void)arg; for (int i = 0; i < kRounds; ++i) g_fadd += 1.0f; return 0; }
static int inc_worker(void *arg)   { (void)arg; for (int i = 0; i < kRounds; ++i) ++g_inc; return 0; }
static int multi_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < kRounds; ++i) { g_multi *= 1.0; g_multi += 0.5; g_multi += 0.5; }
    return 0;
}

static double g_zero = 0.0;     /* runtime-opaque, so no pass folds the NaN away */
static _Atomic double g_nan;
static _Atomic double g_negzero;
static _Atomic double g_explicit;

int main(void) {
    /* ── single-threaded value semantics ─────────────────────────────────── */
    _Atomic double local = 1.5;
    if ((local += 2.25) != 3.75 || local != 3.75)         return 1;
    if (local++ != 3.75 || local != 4.75)                 return 2;
    if (--local != 3.75)                                  return 3;
    if ((local *= 2.0) != 7.5)                            return 4;
    if ((local /= 4.0) != 1.875)                          return 5;

    _Atomic float flocal = 0.5f;
    if ((flocal -= 1.5f) != -1.0f || flocal != -1.0f)     return 6;
    if (flocal++ != -1.0f || flocal != 0.0f)              return 7;

    /* a NaN commits exactly once — a value-compare loop would spin here */
    g_nan = g_zero / g_zero;
    double const nanv = (g_nan += 1.0);
    if (nanv == nanv)                                     return 8;
    double const stored = g_nan;
    if (stored == stored)                                 return 9;

    /* the sign of zero is part of the object's state */
    g_negzero = -g_zero;
    g_negzero *= 1.0;
    if (g_negzero != 0.0 || 1.0 / g_negzero > 0.0)        return 10;

    /* the <stdatomic.h> generic functions take the same representation route —
     * ✔MEASURED gcc 13.3.0, clang 18.1.3 and MSVC 19.51 all run these on a double */
    atomic_store_explicit(&g_explicit, 41.5, memory_order_seq_cst);
    if (atomic_load_explicit(&g_explicit, memory_order_seq_cst) != 41.5) return 11;
    if (atomic_exchange(&g_explicit, 2.5) != 41.5)       return 16;
    double expected = 2.5;
    if (!atomic_compare_exchange_strong(&g_explicit, &expected, 4.0)) return 17;
    if (atomic_load(&g_explicit) != 4.0 || expected != 2.5) return 18;
    expected = 9.0;
    if (atomic_compare_exchange_strong(&g_explicit, &expected, 5.0)) return 19;
    if (expected != 4.0 || atomic_load(&g_explicit) != 4.0) return 20;
    /* a NaN compares by its bits: the exchange SUCCEEDS against the same NaN */
    atomic_store(&g_explicit, g_zero / g_zero);
    expected = atomic_load(&g_explicit);
    if (!atomic_compare_exchange_strong(&g_explicit, &expected, 7.0)) return 21;
    if (atomic_load(&g_explicit) != 7.0)                   return 22;
    /* fetch_add / fetch_sub on a double yield the OLD value — ✔MEASURED clang 18.1.3
     * runs both (gcc 13.3.0 and MSVC 19.51 refuse; one working reference suffices) */
    if (atomic_fetch_add(&g_explicit, 2.5) != 7.0)         return 23;
    if (atomic_fetch_sub(&g_explicit, 0.5) != 9.5)         return 24;
    if (atomic_load(&g_explicit) != 9.0)                   return 25;

    /* ── the race ────────────────────────────────────────────────────────── */
    int (*const workers[4])(void *) = { add_worker, fadd_worker, inc_worker, multi_worker };
    thrd_t threads[8];
    for (int i = 0; i < 8; ++i) {
        if (thrd_create(&threads[i], workers[i / 2], (void *)0) != thrd_success) return 90;
    }
    int rc = 0;
    for (int i = 0; i < 8; ++i) {
        if (thrd_join(threads[i], &rc) != thrd_success) return 91;
    }
    double const want = 2.0 * kRounds;
    if (g_add != want)                                    return 12;
    if (g_fadd != (float)want)                            return 13;
    if (g_inc != want)                                    return 14;
    if (g_multi != want)                                  return 15;
    return 42;
}
