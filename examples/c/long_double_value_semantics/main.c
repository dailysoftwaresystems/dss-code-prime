/* A `long double` READ is a value: it keeps what the object held when it was
 * read, whatever is stored there afterwards — and a `volatile long double` can
 * be read and written at all.
 * D-LIR-LONG-DOUBLE-LOAD-READ-THE-OBJECT-AFTER-IT-WAS-OVERWRITTEN,
 * D-LIR-VOLATILE-LONG-DOUBLE-ACCESS-MOVED-ITS-ADDRESS-INTO-A-FLOAT-REGISTER.
 *
 * ★★★ WHY THIS EXAMPLE EXISTS. Where `long double` is wider than a register
 * (x87 on x86_64 Linux, binary128 on aarch64 Linux) DSS keeps each value in a
 * 16-byte memory home. A load used to define its value AS THE ADDRESS OF THE
 * OBJECT READ, so every shape below that reads an object, then overwrites it,
 * then uses the value it read, got the NEW contents. ✔MEASURED 2026-09-19 at the
 * P68 round-7 base: this FILE is refused on both Linux targets, debug and
 * release — its `volatile` accesses moved a home address into a floating-point
 * register (`xmm14`, `v30`) and used it as a base — and the five read-then-
 * overwrite shapes (11–15), probed WITHOUT `volatile` so the base could compile
 * them, compiled and returned the OVERWRITTEN value in the release pipeline:
 * all five on x86_64 Linux, all but the swap on aarch64 Linux (debug happened to
 * copy at a local's own store). gcc 13.3.0 and clang 18.1.3 run this file to 42
 * at -O0 and -O2 on both processors (aarch64 under qemu).
 *
 * Exit codes — every value is distinct, so a read of the wrong object fails:
 *   11  a swap through a temporary
 *   12  `x = g; g = …; use(x)`
 *   13  a comma operand's value outliving a later store
 *   14  `old = g++` — the post-increment's value is the OLD one
 *   15  an argument read before a later argument's call writes the object
 *   16  a `volatile long double` read
 *   17  a read through a `volatile long double *`
 *   18  a write through a `volatile long double *`
 *   42  every check passed
 *
 * ⓘ Portable C: where `long double` IS `double` (Windows, Apple arm64) every
 * check holds just the same. ★ The `release` arm: every starting value comes
 * from a `volatile`, so the optimized pipeline cannot fold the reads away. */

static volatile long double seed_a = 2.75L;
static volatile long double seed_b = -6.5L;

static long double g;

static int swap_check(long double a, long double b) {
    long double t = a;
    a = b;
    b = t;
    return a == -6.5L && b == 2.75L;
}

static int use(long double x) { return x == 2.75L; }

static int overwrite(void) { g = 1.0L; return 7; }

static int two(long double a, int b) { return a == 2.75L && b == 7; }

int main(void) {
    long double x, old, v;
    volatile long double *vp = &g;

    if (!swap_check(seed_a, seed_b)) return 11;

    g = seed_a;
    x = g;
    g = seed_b;
    if (!use(x)) return 12;

    g = seed_a;
    x = (g + 0.0L, g);
    g = seed_b;
    if (x != 2.75L) return 13;

    g = seed_a;
    old = g++;
    if (old != 2.75L || g != 3.75L) return 14;

    g = seed_a;
    x = g;
    if (!two(x, overwrite())) return 15;

    v = seed_b;
    if (v != -6.5L) return 16;

    g = seed_a;
    v = *vp;
    if (v != 2.75L) return 17;

    *vp = seed_b;
    if (g != -6.5L) return 18;

    return 42;
}
