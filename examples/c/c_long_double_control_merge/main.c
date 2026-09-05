/* D-CSUBSET-LONG-DOUBLE-CONTROL-MERGE: a runtime witness for a `long double`
 * crossing a CONTROL-FLOW JOIN, in all three shapes the anchor names, on every
 * long-double axis DSS ships.
 *
 * WHY A JOIN NEEDED ITS OWN EXAMPLE. An F80/F128 SSA value is the GPR-held
 * ADDRESS of its memory home (LD-1/LD-2), never a register, so a phi cannot be a
 * register move — it is a memory-home merge: the phi owns a frame home and each
 * incoming edge copies the 80/128-bit datum into it. Until this landed the
 * lowering WALLED the phi fail-loud, so none of the programs below produced a
 * binary at all on the x87-80 and ieee128 axes.
 *
 * ★ WHAT EACH ARM MEASURES — a long double program that returns 42 proves no
 * fault; it does not prove the join carried the right bits, so each arm names the
 * property it pins:
 *
 *   arm 1 (exit 1)  TERNARY, arm TAKEN.     The join delivers the THEN incoming,
 *                                           exactly, including fraction bits.
 *   arm 2 (exit 2)  TERNARY, arm NOT taken. The join delivers the ELSE incoming —
 *                                           i.e. it SELECTS, it does not just
 *                                           always copy one side.
 *   arm 3 (exit 3)  LOOP-CARRIED.           A long double survives 40 trips round
 *                                           a back edge (`halve_n`), and the
 *                                           inverse loop restores it exactly.
 *   arm 4 (exit 4)  FULL WIDTH ACROSS THE   1 + 2^-40 crosses a ternary join and
 *                   JOIN.                   comes back with its 41st mantissa bit
 *                                           intact. A join that round-tripped the
 *                                           value through anything narrower than
 *                                           the type — or copied 8 bytes of a
 *                                           10-byte x87 datum — yields 0 here,
 *                                           not 4. THIS is the arm that would
 *                                           catch a "compiles and returns 42"
 *                                           merge that silently truncates.
 *   arm 5 (exit 5)  CONDITIONAL INIT,       the shape Mem2Reg promotes to a phi
 *                   value path.             at `--config=release`; in the debug
 *   arm 6 (exit 6)  CONDITIONAL INIT,       pipeline the same source is memory
 *                   default path.           traffic and no phi at all. The two
 *                                           tiers therefore measure DIFFERENT
 *                                           code, which is why the release arm is
 *                                           mandatory rather than decorative.
 *
 * ANTI-FOLD: every operand AND every branch condition is read from a MUTABLE
 * GLOBAL (the c11_atomic / c_long_double precedent), so the release pipeline
 * cannot fold a join away and leave the arms asserting about a constant. All
 * constants are exact binary fractions, so every axis agrees to the last bit:
 * 20.25 = 81/4, 22.75 = 91/4, and 2^-40 needs 41 mantissa bits — within the 53
 * of the f64 axis, so the SAME program is meaningful where `long double` IS
 * `double` (pe64 x86_64, Apple arm64).
 *
 * ⓘ NO LONG DOUBLE IS EVER COMPARED DIRECTLY, AND THAT IS NOW A CHOICE RATHER
 * THAN A WALL. ⚠ THIS PARAGRAPH USED TO SAY `FCmp` ON F80/F128 WAS STILL WALLED
 * BY THE OPERAND WIDTH GATE, AND THAT SENTENCE WENT FALSE ON 2026-09-03:
 * [[D-TARGET-ENCODING-WIDTH-GUARD]]'s compare arm shipped that day (cycle P58 —
 * an inline `fucomip` sequence on the x87-80 axis, a config'd `__lttf2`-family
 * softcall on ieee128), so a long-double comparison compiles and runs on both
 * axes today. The int-quarter extraction below is KEPT DELIBERATELY, and
 * re-affirmed 2026-09-04 after re-measuring: rewriting this example onto the
 * new compare would make a PHI witness depend on the COMPARE path, so one
 * defect in `emitFloatCompare` could turn a MERGE failure green. Keeping the
 * two subjects separate is what makes this an independent control. Every check
 * here therefore still extracts an exact integer with `(int)(v * 4)` —
 * quarters — and compares ints. ⓘ The comparison has its own witness
 * (`examples/c/c_long_double_compare`), and the CONVERSIONS have theirs
 * (`examples/c/c_long_double_convert`).
 *
 * exit = 42 on success; each arm returns its own number so a failure names itself.
 */

long double g_a;      /* 20.25 = 81/4 */
long double g_b;      /* 22.75 = 91/4 */
long double g_one;    /* 1.0  */
long double g_half;   /* 0.5  */
long double g_two;    /* 2.0  */
long double g_four;   /* 4.0  */
int         g_take;   /* the join condition — opaque to the optimizer */
int         g_n;      /* the loop trip count — likewise */

/* SHAPE 1 — the ternary join. `hir_to_mir` emits a real MIR `Phi` for this at
 * BOTH tiers, so this arm exercises the merge even in the debug pipeline. */
long double pick(int c, long double x, long double y) {
    return c ? x : y;
}

/* SHAPE 2 — a loop-carried long double: `v` crosses the loop back edge on every
 * trip. In release Mem2Reg promotes it and the back edge becomes a phi. */
long double halve_n(long double v, int n) {
    int i;
    for (i = 0; i < n; ++i) v = v * g_half;
    return v;
}

long double double_n(long double v, int n) {
    int i;
    for (i = 0; i < n; ++i) v = v * g_two;
    return v;
}

/* SHAPE 3 — a conditionally-initialised local, the anchor's own spelling.
 * `x` is READ only on the path that WROTE it, so nothing indeterminate is ever
 * used; the promoter's path-insensitive liveness still sees a read-before-write
 * and gives the join an undef incoming, which is the rodata +0.0 zero. */
long double cond_add(int c, long double p, long double q) {
    long double x;
    if (c) x = p;
    if (c) return x + q;
    return q;
}

/* The one measuring instrument: a long double in QUARTERS, as an exact int. */
int quarters(long double v) {
    return (int)(v * g_four);
}

int main(void) {
    long double t;
    long double u;
    long double w;

    g_one  = 1.0L;
    g_half = 0.5L;
    g_two  = 2.0L;
    g_four = 4.0L;
    g_a    = 20.25L;
    g_b    = 22.75L;
    g_n    = 40;

    /* 1 — the ternary join delivers the THEN incoming, exactly. */
    g_take = 1;
    if (quarters(pick(g_take, g_a, g_b)) != 81) {
        return 1;
    }

    /* 2 — and the ELSE incoming when the condition is false: it SELECTS. */
    g_take = 0;
    if (quarters(pick(g_take, g_a, g_b)) != 91) {
        return 2;
    }

    /* 3 — loop-carried: 40 halvings then 40 doublings is the identity, and both
     *     loops carry the long double across a back edge. */
    t = halve_n(g_one, g_n);            /* 2^-40 */
    if (quarters(double_n(t, g_n)) != 4) {
        return 3;
    }

    /* 4 — FULL WIDTH across the join. 1 + 2^-40 needs 41 mantissa bits; it goes
     *     through the ternary and must come back with the 41st bit intact, so
     *     (u - 1) * 2^40 is exactly 1. A join that lost the low bits gives 0. */
    g_take = 1;
    u = pick(g_take, g_one + t, g_one);
    w = double_n(u - g_one, g_n);
    if (quarters(w) != 4) {
        return 4;
    }

    /* 5 — conditional init, the value path (x was written, then joined). */
    g_take = 1;
    if (quarters(cond_add(g_take, g_a, g_b)) != 81 + 91) {
        return 5;
    }

    /* 6 — conditional init, the default path. */
    g_take = 0;
    if (quarters(cond_add(g_take, g_a, g_b)) != 91) {
        return 6;
    }

    return 42;
}
