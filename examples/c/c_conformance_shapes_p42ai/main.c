/* P42 lane AI — runtime witness for four conformance divergences closed together,
 * on BOTH the baseline (debug) and the `release` (shippedPipeline) arm. Each arm
 * contributes a DISTINCT power of two, so the exit code names which one broke.
 *
 *   arm 1 (+1)   D-CSUBSET-BARE-COMPLEX-SPECIFIER-REFUSED
 *                `_Complex` with NO float specifier. Both references give it a
 *                DOUBLE element, so the arm reads a value only a double element
 *                can hold exactly: 0.1 + 0.2i round-tripped through the object
 *                and scaled by 1000 is 100 and 200 at F64 and misses at F32.
 *
 *   arm 2 (+2)   D-CSUBSET-COMPLEX-UNARY-PLUS-REFUSED
 *                `+z` is the identity on a complex (C 6.2.5p11+p18 make the
 *                complex types arithmetic, so C 6.5.3.3p1 admits them). BOTH
 *                components are checked: an implementation that read only the
 *                real part would still satisfy a real-part-only assertion.
 *
 *   arm 3 (+4)   D-CSUBSET-FUNCTION-DESIGNATOR-TO-INTEGER-CAST-REFUSED
 *                `(long long)fn` on a function DESIGNATOR, then the round trip
 *                back through a function pointer, then the CALL through it. The
 *                CALL is the part that matters: a cast that produced a plausible
 *                but wrong integer would pass a `!= 0` test and crash here.
 *                ⚠ `long long`, deliberately, NOT `long` — `long` is 32 bits on
 *                LLP64 (pe64) and 64 on LP64, so a `long` round trip TRUNCATES a
 *                code address on Windows and is a genuinely different program
 *                per target. An earlier draft of this arm used `long` and
 *                produced an ACCESS VIOLATION that looked exactly like a
 *                miscompile; it was the probe comparing two data models.
 *
 *   arm 4 (+8)   D-CSUBSET-SWITCH-ON-A-NON-INTEGER-DISCRIMINANT-ACCEPTED
 *                The POSITIVE control for the guard added with that row: an
 *                ordinary integer switch and an enum switch must both still
 *                dispatch. The refusal half is unit-pinned (a corpus example
 *                cannot contain source the compiler must reject), so this arm is
 *                what proves the guard did not redden every switch in the corpus.
 *
 * ANTI-FOLD: every complex and every integer input is a MUTABLE GLOBAL written at
 * runtime, so the release arm exercises real code rather than a constant fold.
 *
 * exit = 1 + 2 + 4 + 8 = 15 iff every arm passes.
 */

_Complex g_bare;            /* arm 1 — the BARE specifier, no `double` */
double _Complex g_z;        /* arm 2 */
int    g_sel;               /* arm 4 */

enum Color { Red, Green, Blue };
enum Color g_color;

typedef int (*Fp)(void);

static int seven(void) { return 7; }

int main(void) {
    int acc = 0;

    /* arm 1 — bare `_Complex` carries a DOUBLE element. */
    g_bare = __builtin_complex(0.1, 0.2);
    {
        int const re = (int)(__builtin_creal(g_bare) * 1000.0 + 0.5);
        int const im = (int)(__builtin_cimag(g_bare) * 1000.0 + 0.5);
        if (re == 100 && im == 200) acc += 1;
    }

    /* arm 2 — unary `+` is the identity, on BOTH components. */
    g_z = __builtin_complex(3.0, -4.0);
    {
        double _Complex const p = +g_z;
        if ((int)__builtin_creal(p) == 3 && (int)__builtin_cimag(p) == -4) acc += 2;
    }

    /* arm 3 — designator -> integer -> function pointer -> CALL. */
    {
        long long const asInt = (long long)seven;
        Fp const back = (Fp)asInt;
        if (asInt == (long long)&seven && back() == 7) acc += 4;
    }

    /* arm 4 — the integer and enum switches still dispatch. */
    g_sel   = 2;
    g_color = Green;
    {
        int a = 0;
        switch (g_sel) {
            case 1:  a = 10; break;
            case 2:  a = 20; break;
            default: a = 30; break;
        }
        switch (g_color) {
            case Red:   a += 1;  break;
            case Green: a += 2;  break;
            default:    a += 3;  break;
        }
        if (a == 22) acc += 8;
    }

    return acc;
}
