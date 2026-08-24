/* D-LIR-OUTGOING-ARG-CURSOR-SPLIT-BETWEEN-TWO-PASSES-COLLIDES
 *
 * A CALLER-SIDE silent miscompile: a stacked SCALAR argument and a stacked
 * by-value AGGREGATE in the same call were placed by two different cursors —
 * `lowerWideCallArgs` placed the scalars and REMOVED them from the Call, and
 * `lir_callconv` then placed the aggregate from a cursor of its own, which
 * necessarily restarted at 0 because the scalars were no longer in the operand
 * list to advance it. The two wrote the SAME outgoing-argument bytes.
 *
 * The route in is a function POINTER: HIR->MIR refuses the CALLEE of this shape
 * (D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS names it as a residual),
 * so the callee here is spelled in SCALARS whose incoming byte layout is
 * identical, and the call is made through a pointer where the callee is never
 * seen. That is what the values below observe: what the CALLEE actually read.
 *
 * THE CONTROL IS BYTE-EXACT, NOT MERELY SIMILAR. The second call reaches the
 * SAME callee over the SAME eleven incoming positions with every argument a
 * SCALAR — so it exercises the identical stacked-scalar placement with no
 * aggregate in it. Its bit must stay CLEAN under any mutant of the aggregate
 * placement, which is what shows a red is the interleave and not the harness.
 */

struct S16 { long x; long y; };

/* Mutable globals, so nothing here folds away under the release pipeline. */
long g_in[11] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 100, 200};
long g_seen[11];

/* The callee. Eleven scalars: under SysV a1..a6 are register-passed and
 * a7..a11 occupy incoming +0,+8,+16,+24,+32; under AAPCS64/Apple a1..a8 are
 * register-passed and a9..a11 occupy +0,+8,+16. Either way the last two
 * positions are exactly where a 16-byte by-value aggregate lands. */
void sink(long a1, long a2, long a3, long a4, long a5, long a6,
          long a7, long a8, long a9, long a10, long a11) {
    g_seen[0] = a1;
    g_seen[1] = a2;
    g_seen[2] = a3;
    g_seen[3] = a4;
    g_seen[4] = a5;
    g_seen[5] = a6;
    g_seen[6] = a7;
    g_seen[7] = a8;
    g_seen[8] = a9;
    g_seen[9] = a10;
    g_seen[10] = a11;
}

typedef void (*AggFp)(long, long, long, long, long, long, long, long, long,
                      struct S16);
typedef void (*ScalarFp)(long, long, long, long, long, long, long, long, long,
                         long, long);

AggFp    g_agg = 0;
ScalarFp g_scalar = 0;

int main(void) {
    struct S16 s;
    int fail;
    int i;

    fail = 0;
    s.x = g_in[9];
    s.y = g_in[10];

    for (i = 0; i < 11; ++i) {
        g_seen[i] = -1;
    }
    g_agg = (AggFp)&sink;
    /* THE SUBJECT: stacked scalars, then a stacked by-value aggregate. */
    g_agg(g_in[0], g_in[1], g_in[2], g_in[3], g_in[4], g_in[5],
          g_in[6], g_in[7], g_in[8], s);

    /* bit 0 — the stacked scalar the aggregate's first eightbyte overwrote. */
    if (g_seen[8] != 9) {
        fail = fail + 1;
    }
    /* bit 1 — the aggregate's first eightbyte, one whole slot too low. */
    if (g_seen[9] != 100) {
        fail = fail + 2;
    }
    /* bit 2 — the aggregate's second eightbyte. */
    if (g_seen[10] != 200) {
        fail = fail + 4;
    }
    /* bit 3 — the stacked scalars BEFORE the collision point. */
    if (g_seen[6] != 7 || g_seen[7] != 8) {
        fail = fail + 8;
    }
    /* bit 4 — the register-passed arguments. */
    if (g_seen[0] != 1 || g_seen[5] != 6) {
        fail = fail + 16;
    }

    for (i = 0; i < 11; ++i) {
        g_seen[i] = -1;
    }
    g_scalar = (ScalarFp)&sink;
    /* THE CONTROL: the same eleven incoming positions, all scalars. */
    g_scalar(g_in[0], g_in[1], g_in[2], g_in[3], g_in[4], g_in[5],
             g_in[6], g_in[7], g_in[8], g_in[9], g_in[10]);

    /* bit 5 — the control. Clean under every mutant of aggregate placement. */
    if (g_seen[8] != 9 || g_seen[9] != 100 || g_seen[10] != 200) {
        fail = fail + 32;
    }

    return fail;
}
