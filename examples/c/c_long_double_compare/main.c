/* D-TARGET-ENCODING-WIDTH-GUARD (LD-3): the runtime witness for COMPARING two
 * `long double` values, on every long-double axis.
 *
 * ✔MEASURED BEFORE THE CHANGE, through the shipped CLI at 01642ee3: this file's
 * very first comparison REFUSED with `L_UnsupportedLoweringForOpcode` — "MIR
 * FCmp operand: float TypeKind ordinal 14 is not lowerable ... only F64 and F32
 * have scalar float encodings this cycle" — on
 * x86_64:elf64-x86_64-linux-exec (F80), and the identical refusal at ordinal 15
 * (F128) on arm64:elf64-aarch64-linux-exec. The baseline was FAIL LOUD with no
 * binary, never a wrong answer. The f64 axis (pe64 x86_64, macOS arm64), where
 * `long double` IS `double`, already ran every arm here.
 *
 * ★★ WHAT EACH ARM MEASURES, because a long-double program that compiles and
 * returns 42 proves NO FAULT and nothing whatever about whether the comparison
 * was CORRECT. A compare lowering does not crash when it is wrong; it returns
 * the opposite answer. The two ways to get one are:
 *
 *   ARMS 1-13 — DIRECTION. On the x87 axis the compare is `fld [rhs]; fld
 *   [lhs]; fucomip; fstp st(0)`, and FUCOMIP compares ST(0) against ST(1) — so
 *   the LEFT operand must be pushed LAST, the REVERSE of the order the x87
 *   ARITHMETIC sequence uses. Swap those two pushes and every relational
 *   operator inverts, with nothing in the pipeline able to notice. Every
 *   relational operator is therefore exercised in BOTH directions over an
 *   ASYMMETRIC pair (arms 1-6), then at the equality boundary where `<=` and
 *   `<` must disagree (arms 7-9), then `==`/`!=` in both directions (10-13).
 *
 *   ARMS 14-16 — WIDTH, and they are the reason this file does not simply
 *   compare 1.5 with 2.5. Any comparison whose operands differ in their first
 *   few mantissa bits survives being narrowed to `double`, or to `float`, and
 *   so witnesses nothing about the type's real precision. These arms discover
 *   the ULP OF 1.0 at runtime using the compiler's own long-double compare, and
 *   then CROSS-CHECK that answer against long-double ARITHMETIC, which is
 *   full-width by construction (LD-1/LD-2 shipped it). If the comparison were
 *   done at any precision narrower than the arithmetic, the two disagree and
 *   arm 16 fires. Self-calibrating, so it is exact on all three axes: it finds
 *   2^-52 where `long double` is `double`, 2^-63 on x87, 2^-112 on binary128 —
 *   without needing <float.h>, which DSS does not ship.
 *
 *   ARM 14's LOOP IS ALSO THE x87 STACK-BALANCE ARM. Its condition is a
 *   long-double comparison evaluated once per mantissa bit — 64 times on the
 *   x87 axis. The x87 register stack is EIGHT deep and each compare pushes TWO
 *   operands; FUCOMIP pops one and the `fstp st(0)` cleanup pops the other. Leak
 *   either pop and the stack overflows on the fourth or eighth iteration and
 *   every later result is an indefinite NaN. A straight-line witness would
 *   never reach the leak.
 *
 * ANTI-FOLD: every operand and every branch condition is read from a MUTABLE
 * GLOBAL, so the release pipeline cannot constant-fold a comparison away and
 * leave the lowering untested. The `release` arm therefore measures the same
 * comparisons through Mem2Reg-promoted values (the long-double locals become
 * real phis across the loop's back edge) rather than through memory traffic.
 *
 * exit = 42; each arm returns its own number, so a failure names itself.
 */

/* The asymmetric pair. Mutable: see ANTI-FOLD above. */
long double g_a = 1.5L;
long double g_b = 2.5L;
/* A SECOND global holding the same value as g_a — an equality that the folder
 * cannot see and that is not the trivial `x == x`. */
long double g_a2 = 1.5L;

/* The width arms' fixed points. */
long double g_one  = 1.0L;
long double g_two  = 2.0L;
long double g_zero = 0.0L;

int main(void) {
    /* ── ARMS 1-6: every relational operator, both directions ───────────
     * g_a (1.5) < g_b (2.5). Each operator is asked BOTH ways round; an
     * inverted push order flips all six at once, so any one of these fires. */
    if (!(g_a <  g_b)) return 1;
    if (g_b  <  g_a)   return 2;
    if (!(g_b >  g_a)) return 3;
    if (g_a  >  g_b)   return 4;
    if (!(g_a <= g_b)) return 5;
    if (!(g_b >= g_a)) return 6;

    /* ── ARMS 7-9: the EQUALITY BOUNDARY ────────────────────────────────
     * `<=` and `>=` must be TRUE on equal operands where `<` must be FALSE.
     * This is where a lowering that used the wrong condition-code nibble —
     * `setb` for `<=`, say, or an unsigned integer nibble for a float compare
     * — diverges from one that merely got the direction right. */
    if (!(g_a <= g_a2)) return 7;
    if (!(g_a >= g_a2)) return 8;
    if (g_a  <  g_a2)   return 9;

    /* ── ARMS 10-13: equality and inequality ────────────────────────────
     * On x86 these two are the COMPOSED predicates (two setcc results folded
     * with `and`), on arm64 `==` is a single nibble and `!=` composed — so
     * these arms exercise a different tail from the six above on both axes. */
    if (!(g_a == g_a2)) return 10;
    if (g_a  == g_b)    return 11;
    if (!(g_a != g_b))  return 12;
    if (g_a  != g_a2)   return 13;

    /* ── ARM 14: the ULP OF 1.0, discovered by comparison ───────────────
     * Halve until 1+e no longer differs from 1. `last` is then the smallest
     * step the COMPARISON can still see at 1.0 — i.e. the ulp, whatever this
     * axis's mantissa width is. The loop condition is a long-double compare
     * evaluated once per mantissa bit; see the stack-balance note above. */
    {
        long double e    = g_one;
        long double last = g_one;
        long double u;
        long double h;
        long double sum;
        long double back;
        int cmpSaysEqual;
        int arithSaysEqual;

        while (g_one + e > g_one) {
            last = e;
            e = e / g_two;
        }
        u = last;

        /* The discovered step must genuinely perturb 1.0 … */
        if (!(g_one + u > g_one)) return 14;

        /* ── ARM 15: … and HALF of it must not. */
        h = u / g_two;
        if (!(g_one + h == g_one)) return 15;

        /* ── ARM 16: THE COMPARISON AND THE ARITHMETIC MUST AGREE ───────
         * Arms 14-15 are self-consistent under a UNIFORM narrowing: if the
         * comparison were done at `double` precision the loop would simply
         * stop early, at 2^-52, and both would still pass. This arm breaks
         * that circularity by asking the ARITHMETIC — which is full-width by
         * construction — the same question:
         *
         *   `sum - g_one` is exactly `h` at the type's true precision when
         *   `sum != g_one`, and exactly 0 when `sum` really did round back to
         *   1.0. So `back == g_zero` and `sum == g_one` must agree.
         *
         * Under a narrowed compare on the x87 axis they DISAGREE: the loop
         * stops at u = 2^-52, so h = 2^-53, and `1 + 2^-53` is exact in 80-bit
         * arithmetic — `back` is a nonzero 2^-53 — while the narrowed compare
         * truncates `sum` to 1.0 and calls it equal. ⓘ `back == g_zero`
         * survives that narrowing itself (2^-53 is a perfectly ordinary
         * `double`, nowhere near 0), which is what makes it usable as the
         * independent witness here. */
        sum            = g_one + h;
        back           = sum - g_one;
        cmpSaysEqual   = (sum  == g_one);
        arithSaysEqual = (back == g_zero);
        if (cmpSaysEqual != arithSaysEqual) return 16;
    }

    return 42;
}
