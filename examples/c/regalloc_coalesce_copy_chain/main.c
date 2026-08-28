/* plan 22 OPT8 — REGISTER COALESCING, runtime witness.
 *
 * Every function here is a shape the coalescer must get right, and the four
 * that must NOT be coalesced are the ones that make this a MISCOMPILE PIN
 * rather than a size test: if the coalescer merges two live ranges that
 * overlap, the second value overwrites the first and the exit code changes.
 * A size regression is invisible at runtime; a wrong merge is not.
 */

/* ── (1) NEGATIVE: a copy whose SOURCE IS STILL LIVE afterwards.
 * `b` is derived from `a` and then MUTATED while `a` is still needed. The two
 * ranges overlap, so a coalescer that looks only at "these are copy-related"
 * and not at interference gives them one register — and `a` becomes 21.
 * correct: 7*10 + 21 = 91.  wrongly-coalesced: 21*10 + 21 = 231.          */
static int source_still_live(int a) {
    int b = a;
    b = b * 3;
    return a * 10 + b;
}

/* ── (2) POSITIVE: a copy chain whose source dies at the copy. Every link is
 * coalescable, and the whole chain should collapse onto one register.      */
static int dead_after_copy(int x) {
    int t1 = x;
    int t2 = t1;
    int t3 = t2;
    return t3 + 1;
}

/* ── (3) POSITIVE: the TIED-OPERAND shape. `s += i` is a two-address add, so
 * the result is tied to `s`; coalescing the pair means `legalizeTwoAddress`
 * never mints the copy in the first place.                                 */
static int tied_accumulator(int n) {
    int s = 0;
    for (int i = 1; i <= n; ++i) s += i;
    return s;
}

/* ── (4) NEGATIVE, and the two-address variant of (1): the UNTIED operand of a
 * two-address op may never share the result's register even when their ranges
 * are disjoint, because the legalizer's `mov result, tied` runs BEFORE the op
 * and would destroy it. correct: (5+9)*2 = 28. clobbered: (5+5)*2 = 20.    */
static int untied_operand_survives(int p, int q) {
    int r = p + q;
    return r * 2;
}

/* ── (5) POSITIVE: a parameter used exactly once, passed straight back. With
 * pre-coloring the parameter is allocated INTO its incoming argument register
 * and the calling-convention materializer emits nothing at all.            */
static int passthrough(int v) { return v; }

/* ── (6) NEGATIVE across a CALL: `keep` is copy-related to `a`, but `a` is
 * still live at the call, so they interfere. `keep` must also survive the
 * call itself, which forbids a caller-saved register.
 * correct: 4 + 5 = 9.                                                      */
static int side_effect(int v) { return v + 1; }
static int keeps_value_across_call(int a) {
    int keep = a;
    int r = side_effect(a);
    return keep + r;
}

/* ── (7) POSITIVE in the FLOAT register class — the class move is `movaps` on
 * x86-64 and `fmov` on arm64, and the coalescer must resolve it through the
 * schema rather than assume the integer one.                               */
static double float_copy_chain(double d) {
    double e = d;
    double f = e;
    return f + 1.0;
}

int main(void) {
    int total = 0;
    total += source_still_live(7);          /* 91 */
    total += dead_after_copy(5);            /*  6 */
    total += tied_accumulator(4);           /* 10 */
    total += untied_operand_survives(5, 9); /* 28 */
    total += passthrough(3);                /*  3 */
    total += keeps_value_across_call(4);    /*  9 */
    total += (int)float_copy_chain(2.0);    /*  3 */
    return total;                           /* 150 */
}
