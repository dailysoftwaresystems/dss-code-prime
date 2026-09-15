/*
 * THE THREE REMAINING GNU `aligned` FORMS, EACH CONFERRED AND EACH OBSERVED.
 * Cycle P66, lane `ag`. exit 42 = every observation held.
 *
 * The three forms are INDEPENDENT defects that share a mechanism area, so each
 * owns its own exit code and a partial fix cannot read as a complete one:
 *
 *   (1) WEAKER THAN NATURAL   `aligned(2)` on an `int` alias LOWERS to 2.
 *       Codes 10..13. gcc 13.3.0, clang 18.1.3, mingw-w64 gcc 13.2.0 and
 *       aarch64-linux-gnu-gcc 13.3.0 all lower; DSS kept 4 SILENTLY, which is
 *       a different struct layout than every reference — measured here, before
 *       the fix, as an EXECUTED exit 10 against those references' 42.
 *
 *   (2) MULTI-DECLARATOR      `typedef int A __attribute__((aligned(8))), B;`
 *       confers on A ALONE. Codes 20..23. The `B` half is the load-bearing
 *       one: a fix that conferred on both would be a NEW above-the-union
 *       defect that no acceptance test could see.
 *
 *   (3) ZERO-ARGUMENT         `__attribute__((aligned))` is the target's
 *       maximum useful alignment. Codes 30..33. Measured 16 on x86_64 AND on
 *       aarch64 (asserted and EXECUTED under qemu on both), which is what the
 *       two shipped targets declare as `aggregateLayout.maxAlignment`.
 *
 * The `release` arm must differ from the baseline image, so the observations
 * are not a file of pure address checks (a lesson this example's parent row
 * paid for): the accumulator below is loaded THROUGH alias-typed objects and
 * folded by an inlinable helper, so the optimizer has real work to do.
 */

typedef int A2 __attribute__((aligned(2)));        /* (1) weaker than natural */
typedef int W8 __attribute__((aligned(8))), W4;    /* (2) A alone, not B      */
typedef int AM __attribute__((aligned));           /* (3) the target maximum  */

struct T { char c; A2 v; };   /* lowered  -> size 6,  align 2,  v at 2  */
struct U { char c; W8 v; };   /* raised   -> size 16, align 8,  v at 8  */
struct M { char c; AM v; };   /* max      -> size 32, align 16, v at 16 */
struct N { char c; W4 v; };   /* UNTOUCHED-> size 8,  align 4,  v at 4  */

static struct T gt = { 1, 2 };
static struct U gu = { 3, 4 };
static struct M gm = { 5, 6 };
static struct N gn = { 7, 8 };

static int fold(int acc, int x) { return acc * 3 + x; }

int main(void) {
    /* ── (1) a request WEAKER than natural must LOWER ───────────────────── */
    if (_Alignof(A2) != 2)                                   return 10;
    if (sizeof(A2) != 4)                                     return 11;  /* size untouched */
    if (sizeof(struct T) != 6 || _Alignof(struct T) != 2)    return 12;
    if ((char *)&gt.v - (char *)&gt != 2)                    return 13;

    /* ── (2) the FIRST declarator gets it; the SECOND must NOT ──────────── */
    if (_Alignof(W8) != 8)                                   return 20;
    if (_Alignof(W4) != 4)                                   return 21;  /* the whole point */
    if (sizeof(struct U) != 16 || (char *)&gu.v - (char *)&gu != 8) return 22;
    if (sizeof(struct N) != 8  || (char *)&gn.v - (char *)&gn != 4) return 23;

    /* ── (3) the BARE form is the target's maximum useful alignment ─────── */
    if (_Alignof(AM) != 16)                                  return 30;
    if (sizeof(AM) != 4)                                     return 31;
    if (sizeof(struct M) != 32 || _Alignof(struct M) != 16)  return 32;
    if ((char *)&gm.v - (char *)&gm != 16)                   return 33;

    /* ── the alias-typed objects must still hold their VALUES ───────────── */
    {
        A2 a = gt.v; W8 b = gu.v; AM c = gm.v; W4 d = gn.v;
        int acc = 0;
        for (int i = 0; i < 4; ++i) {
            acc = fold(acc, (int)a + (int)b * i);
            acc = fold(acc, (int)c - (int)d);
        }
        if (acc != 4504) return 40;
    }
    return 42;
}
