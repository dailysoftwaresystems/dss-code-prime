/* D-CSUBSET-C11-THREADS-MACHO-MTX-PLAIN-RECURSIVE (+ the pe half,
 * D-CSUBSET-C11-THREADS-MTX-PLAIN-RECURSIVE) — the C11 `mtx_recursive` witness.
 *
 * ★★ THE DEFECT THIS EXISTS TO CATCH IS A HANG, NOT A WRONG ANSWER. The macho
 * `mtx_init` recipe passed a NULL mutexattr, and on Darwin a NULL attr is
 * PTHREAD_MUTEX_DEFAULT == PTHREAD_MUTEX_NORMAL — NON-recursive. So
 * `mtx_init(&m, mtx_plain | mtx_recursive)` handed back a mutex whose owner could
 * not re-lock it, and the second `mtx_lock` below BLOCKED FOREVER. Nothing about
 * that is visible to a compile-only pin: the program compiles, links, loads, and
 * then stops. C11 7.26.4.2 / C23 7.28.4.2 make a recursive mutex re-lockable by
 * its owner, and both other implementations honour it (✔MEASURED: glibc 2.39 via
 * gcc 13.3.0 and clang 18.1.3 separately, and MSVC 19.51's own <threads.h>), so
 * DSS deadlocking there sat below (gcc ∪ clang ∪ MSVC) ∪ ISO C.
 *
 * ★★ RECURSION ALONE IS NOT ENOUGH TO ASSERT, AND THAT IS THE DESIGN HERE.
 * An implementation whose `mtx_lock` were a NO-OP would reach depth 3 happily. So
 * the witness pins the LOCK COUNT from the outside, through a second thread:
 *   (a) the worker takes the mutex three times — a non-recursive mutex hangs;
 *   (b) while it is held, main's `mtx_trylock` must report thrd_busy — proving
 *       the lock is real and not a no-op;
 *   (c) after TWO of the three unlocks, main's trylock must STILL report
 *       thrd_busy — proving the count is a count and not a flag, which is the
 *       one assertion an "always recursive" fake cannot satisfy either way;
 *   (d) after the third, trylock must SUCCEED — proving it is finally released.
 * (b) and (d) have opposite verdicts on the same call, so neither a constant
 * thrd_busy nor a constant thrd_success passes this file.
 *
 * ⚠ NOTHING HERE WAITS ON A CLOCK, AND NOTHING WAITS UNBOUNDED. Every wait is a
 * bounded poll (at most ~2 s of 2 ms `thrd_sleep` naps, well inside the examples
 * runner's own 5 s hang detector) whose failure is a DISTINCT EXIT CODE, so the
 * deadlock this witness is aimed at reports itself as exit 12 rather than as a
 * killed process. `thrd_sleep` is used because its interval is RELATIVE and
 * monotone-backed on all three platforms — this project's WSL leg has a measured
 * CLOCK_REALTIME that steps ±30 s every few seconds, so no verdict here may be a
 * property of the wall clock. The handshake is a `stage`/`go` pair under a plain
 * mutex, so no verdict is a property of the scheduler either.
 *
 * ⚠ THE `gate` MUTEX IS DELIBERATELY `mtx_plain` AND IS NEVER RE-LOCKED. It is
 * the control arm living inside the same file: if honouring `type` broke the
 * ordinary plain path, every handshake below would fail rather than the
 * recursive assertions.
 *
 * The 40 + 2 arithmetic reads two MUTABLE globals so the release arm cannot
 * const-fold the exit code, and a real cross-thread mutex handshake cannot be
 * folded at all. RED-on-disable: delete shippedLibs/threads.json and the
 * #include fires F_ShippedHeaderNotFound. */
#include <threads.h>
#include <time.h>

int g_base = 40;   /* mutable — defeats the optimizer's const-fold */
int g_two  = 2;

static mtx_t g_rec;    /* the mtx_recursive mutex under test */
static mtx_t g_gate;   /* mtx_plain — guards the handshake, and never re-locked */
static int   g_stage;  /* worker -> main progress */
static int   g_go;     /* main   -> worker permission */

static int read_stage(void) {
    int v;
    mtx_lock(&g_gate);
    v = g_stage;
    mtx_unlock(&g_gate);
    return v;
}
static void set_stage(int v) {
    mtx_lock(&g_gate);
    g_stage = v;
    mtx_unlock(&g_gate);
}
static int read_go(void) {
    int v;
    mtx_lock(&g_gate);
    v = g_go;
    mtx_unlock(&g_gate);
    return v;
}
static void set_go(int v) {
    mtx_lock(&g_gate);
    g_go = v;
    mtx_unlock(&g_gate);
}

/* A 2 ms nap. Relative and monotone-backed everywhere, so it is immune to the
 * measured WSL CLOCK_REALTIME stepping. */
static void nap(void) {
    struct timespec d;
    d.tv_sec  = 0;
    d.tv_nsec = 2000000;
    thrd_sleep(&d, (void *)0);
}

/* Bounded polls: 1000 * 2 ms = ~2 s ceiling, then give up. Returns 0 on timeout,
 * which every caller turns into its own distinct exit code — a deadlock must
 * REPORT, never hang. */
static int wait_stage(int want) {
    int i;
    for (i = 0; i < 1000; i++) {
        if (read_stage() >= want) return 1;
        nap();
    }
    return 0;
}
static int wait_go(int want) {
    int i;
    for (i = 0; i < 1000; i++) {
        if (read_go() >= want) return 1;
        nap();
    }
    return 0;
}

static int worker(void *unused) {
    (void)unused;
    mtx_lock(&g_rec);            /* depth 1 */
    mtx_lock(&g_rec);            /* depth 2 — A NON-RECURSIVE MUTEX BLOCKS HERE */
    mtx_lock(&g_rec);            /* depth 3 */
    set_stage(1);

    if (!wait_go(1)) return 1;
    mtx_unlock(&g_rec);          /* depth 2 */
    mtx_unlock(&g_rec);          /* depth 1 — STILL HELD */
    set_stage(2);

    if (!wait_go(2)) return 2;
    mtx_unlock(&g_rec);          /* depth 0 — released */
    set_stage(3);
    return 0;
}

int main(void) {
    thrd_t t;
    int    joined = -1;

    if (mtx_init(&g_gate, mtx_plain) != thrd_success) return 10;
    /* C11 spells a recursive mutex as the OR of a base type with mtx_recursive.
     * `mtx_plain | mtx_recursive` is the value the standard names; a shim that
     * compared the type for equality against mtx_recursive alone would still pass
     * here, which is why the unit pin asserts the MASK. */
    if (mtx_init(&g_rec, mtx_plain | mtx_recursive) != thrd_success) return 11;

    if (thrd_create(&t, worker, (void *)0) != thrd_success) return 19;

    /* (a) RECURSION. The worker re-locked its own mutex twice more. A
     * non-recursive mutex never reaches stage 1 and this is exit 12. */
    if (!wait_stage(1)) return 12;

    /* (b) THE LOCK IS REAL. Another thread must not be able to take it. */
    if (mtx_trylock(&g_rec) != thrd_busy) return 13;

    set_go(1);
    if (!wait_stage(2)) return 14;

    /* (c) THE COUNT IS A COUNT. Two unlocks of a triple-held mutex must NOT
     * release it. This is the assertion a lock-count-of-one fake fails. */
    if (mtx_trylock(&g_rec) != thrd_busy) return 15;

    set_go(2);
    if (!wait_stage(3)) return 16;

    /* (d) AND THE LAST UNLOCK DOES RELEASE IT — the opposite verdict from the
     * same call, so a constant return value cannot satisfy both. */
    if (mtx_trylock(&g_rec) != thrd_success) return 17;
    mtx_unlock(&g_rec);

    if (thrd_join(t, &joined) != thrd_success) return 18;
    if (joined != 0) return 20;          /* the worker hit one of its own timeouts */

    mtx_destroy(&g_rec);
    mtx_destroy(&g_gate);
    return g_base + g_two;               /* 42 */
}
