/* FC17.9(a) Cycle 3 (D-CSUBSET-C11-THREADS-TIMED) — the C11 <threads.h> TIMED-WAIT
 * witness. Three functions that were undeclared on every object format until this
 * cycle, and every one of them fails in a way NO COMPILE-ONLY PIN CAN SEE: a timed
 * wait that returns immediately, or that never returns, still compiles.
 *
 * ★★ EACH FUNCTION IS PINNED FROM BOTH SIDES, and that pairing is what makes the
 * witness load-bearing rather than merely green. A constant `return thrd_timedout`
 * passes any timeout test on its own; a constant `return thrd_success` passes any
 * acquire test on its own; neither passes both:
 *   · mtx_timedlock must REFUSE a mutex another thread holds once the deadline has
 *     passed, and must WAIT ON one that thread is about to release.
 *   · cnd_timedwait must REFUSE an expired deadline, and must WAKE when signalled.
 *
 * ★★ THE POISON IS LOAD-BEARING AND IS THE WHOLE REASON THIS EXAMPLE EXISTS.
 * `struct timespec` is 16 bytes with tv_nsec at offset 8 on BOTH worlds, but tv_nsec
 * is `long` — EIGHT bytes on LP64 (elf, macho) and FOUR on pe/LLP64. Neither a
 * sizeof nor an offsetof check can tell those apart, and a cleanly-initialized
 * struct cannot either: the trailing pad reads back as zero, so an 8-byte read of a
 * 4-byte field gives the right answer by luck. Filling the struct with 0xFF FIRST
 * removes the luck — a pe shim reading tv_nsec as 8 bytes folds 0xFFFFFFFF into the
 * high half, so the sleep computes a negative millisecond count and returns
 * instantly, and every deadline lands in the distant past so the ACQUIRE arms
 * report thrd_timedout instead of blocking.
 *
 * ★★ NO ARM DEPENDS ON A RACE. Every mutex is locked by main BEFORE the thread that
 * probes it exists, and `cnd_timedwait` releases its mutex atomically, so the
 * signalling thread cannot run ahead of the wait it is meant to wake.
 *
 * ⚠⚠ AND NO ARM WAITS OUT A DEADLINE, WHICH IS A DELIBERATE DESIGN AGAINST A
 * MEASURED HOST DEFECT. ✔MEASURED 2026-09-01 with a gcc-built control on this
 * project's WSL leg: CLOCK_REALTIME steps by +29 s and -27 s roughly every five
 * seconds while CLOCK_MONOTONIC stays exact (1100 ms for a 1100 ms nanosleep, eight
 * runs of eight). C11 states mtx_timedlock's and cnd_timedwait's deadlines in
 * TIME_UTC, so on such a host a wait for a deadline "two seconds out" can
 * legitimately last thirty — past the examples runner's own 5 s hang detector,
 * which must NOT be widened to accommodate it. So the timeout arms use a deadline a
 * MINUTE IN THE PAST (must refuse instantly, and a ±30 s step cannot drag it into
 * the future) and the acquire arms a deadline a minute AHEAD, ended not by the
 * clock but by another thread releasing the lock or signalling. Every verdict here
 * is a property of the implementation and none is a property of the clock.
 *
 * ⚠ Only `thrd_sleep` measures elapsed time, and it can: its duration is RELATIVE
 * and monotone-backed on all three platforms, so the reading is corrupted only when
 * the clock steps BACKWARDS during the sleep. The bound is one-sided (>= 1 s) so a
 * forward step still passes, and it retries twice more so a backward one does not
 * red. A no-op sleep reads 0 on every attempt and never passes.
 *
 * elf x86_64 + arm64 (qemu) resolve all three as DIRECT libc.so.6 FFI (glibc 2.39
 * exports thrd_sleep WEAK, mtx_timedlock and cnd_timedwait strong — `nm -D` verified
 * on both arches). pe64 SYNTHESIZES them over kernel32; macho over libSystem
 * pthread. RED-on-disable: delete shippedLibs/threads.json and the #include fires
 * F_ShippedHeaderNotFound. */
#include <threads.h>
#include <time.h>

static mtx_t g_target;  /* the contended mutex: main holds it, probes wait on it */
static mtx_t g_quiet;   /* guards the condition variable */
static cnd_t g_cv;

/* Fill an object with 0xFF so a field written at the WRONG WIDTH leaves evidence. */
static void poison(void *object, int bytes) {
    unsigned char *raw = (unsigned char *)object;
    int            i;
    for (i = 0; i < bytes; i++) raw[i] = 0xFF;
}

/* An absolute TIME_UTC deadline `offset` seconds from now (negative = already
 * expired), in a deliberately poisoned struct. */
static void deadline_in(struct timespec *when, int offset) {
    poison(when, (int)sizeof *when);
    when->tv_sec  = (long long)time((void *)0) + offset;
    when->tv_nsec = 0;
}

/* Runs while main holds `g_target` and is JOINED before main releases it, so the
 * lock is unavailable for this thread's whole life — by program order, not timing.
 * Its deadline is already a minute gone, so the answer must be immediate. */
static int probe_expired(void *unused) {
    struct timespec deadline;
    (void)unused;
    deadline_in(&deadline, -60);
    if (mtx_timedlock(&g_target, &deadline) != thrd_timedout) return 1;
    return 0;
}

/* Also runs while main holds `g_target`, but main releases it a fraction of a
 * second later, so a correct implementation BLOCKS and then succeeds. One that
 * gives up instead of waiting returns thrd_timedout; one that never returns hangs
 * the runner's timeout. The deadline is a minute out, so no observed clock step can
 * reach it and the wait is ended by the unlock, never by the clock. */
static int probe_acquire(void *unused) {
    struct timespec deadline;
    (void)unused;
    deadline_in(&deadline, 60);
    if (mtx_timedlock(&g_target, &deadline) != thrd_success) return 1;
    if (mtx_unlock(&g_target) != thrd_success) return 2;
    return 0;
}

/* Cannot take `g_quiet` until main's cnd_timedwait has atomically released it, so
 * the signal cannot be delivered before the wait begins. */
static int signaller(void *unused) {
    (void)unused;
    if (mtx_lock(&g_quiet) != thrd_success) return 1;
    if (cnd_signal(&g_cv) != thrd_success) return 2;
    if (mtx_unlock(&g_quiet) != thrd_success) return 3;
    return 0;
}

int main(void) {
    struct timespec duration;
    struct timespec deadline;
    thrd_t          helper;
    long long       t0;
    long long       t1;
    int             attempt;
    int             measured = 0;
    int             r;

    /* ── 1. thrd_sleep must BLOCK for at least the requested interval ── */
    for (attempt = 0; attempt < 3 && !measured; attempt++) {
        poison(&duration, (int)sizeof duration);
        duration.tv_sec  = 1;
        duration.tv_nsec = 100000000;    /* 1.1 s — spans a whole second boundary */
        t0 = (long long)time((void *)0);
        if (thrd_sleep(&duration, (void *)0) != 0) return 10;
        t1 = (long long)time((void *)0);
        if (t1 - t0 >= 1) measured = 1;  /* a backward clock step retries, not reds */
    }
    if (!measured) return 11;            /* never blocked, three attempts running */

    /* ── 2. mtx_timedlock, both directions, on a mutex main owns throughout ── */
    if (mtx_init(&g_target, mtx_plain) != thrd_success) return 20;
    if (mtx_lock(&g_target) != thrd_success) return 21;

    /* 2a. REFUSES an expired deadline: the prober lives and dies inside main's
     * ownership, and its deadline is already gone, so it must not wait at all. */
    if (thrd_create(&helper, probe_expired, (void *)0) != thrd_success) return 22;
    r = 99;
    if (thrd_join(helper, &r) != thrd_success) return 23;
    if (r != 0) return 24;               /* granted a lock main holds, or errored */

    /* 2b. WAITS AND SUCCEEDS: same held mutex, released a fraction of a second in.
     * This is the arm that exercises the deadline LOOP rather than its exit. */
    if (thrd_create(&helper, probe_acquire, (void *)0) != thrd_success) return 25;
    poison(&duration, (int)sizeof duration);
    duration.tv_sec  = 0;
    duration.tv_nsec = 300000000;        /* 0.3 s of contention */
    if (thrd_sleep(&duration, (void *)0) != 0) return 26;
    if (mtx_unlock(&g_target) != thrd_success) return 27;
    r = 99;
    if (thrd_join(helper, &r) != thrd_success) return 28;
    if (r != 0) return 30 + r;           /* 31 = gave up early · 32 = unlock failed */
    mtx_destroy(&g_target);

    /* ── 3. cnd_timedwait, both directions ── */
    if (mtx_init(&g_quiet, mtx_plain) != thrd_success) return 40;
    if (cnd_init(&g_cv) != thrd_success) return 41;

    /* 3a. REFUSES an expired deadline with nothing signalling. A spurious wake
     * re-arms the wait rather than failing it — C11 and POSIX both permit one. */
    if (mtx_lock(&g_quiet) != thrd_success) return 42;
    deadline_in(&deadline, -60);
    do {
        r = cnd_timedwait(&g_cv, &g_quiet, &deadline);
    } while (r == thrd_success);
    if (r != thrd_timedout) return 43;
    if (mtx_unlock(&g_quiet) != thrd_success) return 44;

    /* 3b. WAKES AND SUCCEEDS: the signaller cannot take g_quiet until the wait has
     * atomically released it, so the signal cannot arrive before the wait exists. */
    if (mtx_lock(&g_quiet) != thrd_success) return 45;
    if (thrd_create(&helper, signaller, (void *)0) != thrd_success) return 46;
    deadline_in(&deadline, 60);
    if (cnd_timedwait(&g_cv, &g_quiet, &deadline) != thrd_success) return 47;
    if (mtx_unlock(&g_quiet) != thrd_success) return 48;
    r = 99;
    if (thrd_join(helper, &r) != thrd_success) return 49;
    if (r != 0) return 50 + r;
    cnd_destroy(&g_cv);
    mtx_destroy(&g_quiet);

    return 42;
}
