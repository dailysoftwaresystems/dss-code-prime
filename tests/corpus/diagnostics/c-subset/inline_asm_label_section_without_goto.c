/* inline-asm P1 (D-LANG-GNU-EXTENDED-INLINE-ASM-UNSUPPORTED): a label section — the
 * FOURTH `:` group — without the `goto` qualifier is a CONSTRAINT VIOLATION, pinned
 * as S_InlineAsmLabelSectionRequiresGoto (S0063).
 *
 * ★ WHY THIS IS ITS OWN CODE AND NOT S0062. "Extended asm is not yet supported" is a
 * statement about DSS and expires when P5 lands. This form is ill-formed in gcc,
 * clang and MSVC alike (✔MEASURED: `expected ')' before '::'`) and stays ill-formed
 * AFTER P5 — so reporting it as "not yet supported" would be a lie that only gets
 * louder with time. The golden pins WHICH code fires, which is the whole point.
 *
 * ★ THE PREDICATE IS THE TAIL'S PRESENCE, NOT THE LABEL LIST'S — and `("" ::::)` is
 * exactly the shape that separates the two: its fourth section carries NOTHING, so a
 * gate keyed off a non-empty label list would let it through. It survives only
 * because the grammar mints a named node for a FUSED section boundary; with inline
 * alt arms this form would be structurally indistinguishable from the valid
 * `("" : : : )` and would have compiled CLEAN.
 *
 * ★ The span is the label SECTION node (the fused `::` that opens the fourth group),
 * not the whole statement — a regression that re-stamps it on the statement flips
 * this golden's line:col. The template is EMPTY so S_InlineAsmNonEmptyTemplate
 * (S0057) cannot race the code under test.
 *
 * RED-on-disable: remove check (1) -> the extended gate below it still fires, so the
 * golden does NOT go empty; it changes S0063 to S0062, which is exactly the
 * substitution this file exists to catch. */
int main(void) {
    __asm__ ("" ::::);
    return 0;
}
