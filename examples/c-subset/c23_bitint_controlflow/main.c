/* C23 _BitInt(N) in control flow (D-CSUBSET-BITINT), C1 — the shapes the first
 * `I_BitIntWidthInconsistent` verifier miscope crashed on / false-rejected:
 *
 *   (a) a _BitInt PHI. A `_BitInt(8)` ternary (a CFG merge, debug) and a
 *       `for (unsigned _BitInt(8) i; …)` induction variable (mem2reg, release) each
 *       materialize a `_BitInt(8)` Phi. The first verifier used a `!conversion`
 *       DENYLIST that admitted Phi, whose `instOperands` ABORTS → a hard compiler
 *       crash on this valid, common C23. (A Phi merges same-N arms — already width N.)
 *   (b) a mixed RETURN/ARG width CALL. `low4` returns `_BitInt(4)` from a
 *       `_BitInt(40)` argument, so the Call is `_BitInt(4)`-typed with a `_BitInt(40)`
 *       operand — the denylist FALSE-REJECTED it (a Call's result width is its return
 *       type, unrelated to its argument widths).
 *
 * The verifier now ALLOWLISTS only the arithmetic ops the wrap chokepoint produces,
 * so Phi/Call are exempt. Exit 42 on debug AND release. RED-ON-DISABLE: revert the
 * allowlist to the `!conversion` denylist and this crashes (the Phi) / fails the
 * build (the mixed-width Call) instead of returning 42. */

/* (b) a `_BitInt` function whose RETURN width (4) differs from its ARG width (40). */
unsigned _BitInt(4) low4(unsigned _BitInt(40) x) {
    return x;                          /* _BitInt(40) -> _BitInt(4): the low 4 bits */
}

int main(void) {
    /* (a1) ternary -> a _BitInt(8) Phi merging two same-N arms. `volatile` keeps the
     *      condition RUNTIME so the merge is not folded away (release). */
    volatile int sel = 1;
    unsigned _BitInt(8) hi = 17u, lo = 99u;
    unsigned _BitInt(8) t = sel ? hi : lo;         /* 17 */

    /* (a2) loop-induction var -> a _BitInt(8) Phi under mem2reg (release). */
    unsigned _BitInt(8) sum = 0u;
    for (unsigned _BitInt(8) i = 0u; i < 6u; i++) {
        sum = sum + i;                             /* 0+1+2+3+4+5 = 15 */
    }

    /* (b) the mixed return/arg-width call: low4(1034) = 1034 mod 16 = 10. */
    unsigned _BitInt(40) big = 1034u;
    unsigned _BitInt(4) r = low4(big);             /* 10 */

    return (int)t + (int)sum + (int)r;             /* 17 + 15 + 10 == 42 */
}
