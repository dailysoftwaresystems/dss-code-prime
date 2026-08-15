/* inline-asm P5 (D-CSUBSET-INLINE-ASM-OPERANDS / D-CSUBSET-INLINE-ASM-GOTO): the
 * `asm goto` form — qualifier + fourth (label) section — parses end to end, is NO
 * LONGER refused for being `goto`, and its template is held to the SAME placeholder
 * bound as every other extended statement: exactly ONE
 * S_InlineAsmPlaceholderOutOfRange (S006A).
 *
 * ★★★ WHY THIS FILE NO LONGER PINS S0062. `asm goto` LANDED in P5 (plan 29 §4.5 —
 * the operator ruled to build it rather than refuse it, because gcc 11+ and clang both
 * accept the form and under [[feedback_reference_compilers_are_the_spec]] refusing it
 * is a real divergence). The blanket S_InlineAsmExtendedUnsupported it used to pin has
 * no emit site for this shape any more, so the old golden could not survive.
 *
 * ★ IT IS STILL A DIFFERENT GATE ARM FROM `inline_asm_extended_unsupported.c`, and
 * that is why it is still here rather than deleted. Two grammar facts are reachable
 * ONLY through this shape — `hasLabelList` and `hasGotoQualifier` — and this file is
 * what keeps them exercised. If the label section or the `goto` qualifier regressed to
 * a parse error, this file would report a P_* cascade instead of the single S006A
 * below; if `asm goto` regressed to a blanket refusal, the code would change back to
 * S0062. Either way the golden moves and says which.
 * ★ IT IS ALSO STILL THE CONTROL FOR THE FOURTH-SECTION RULE. The sibling
 * `inline_asm_label_section_without_goto.c` pins that a label section WITHOUT `goto`
 * is a constraint violation (S0063). Here the qualifier IS present, so S0063 must NOT
 * fire — the pair pins both polarities of that predicate, and the pair is the reason
 * neither file may be folded into the other.
 *
 * ★ `%0` WITH ZERO OPERANDS is the minimal out-of-range index. The statement opens all
 * three operand sections and fills none, so the joined operand count is 0 and EVERY
 * index is out of range — including the first. `lbl` is a REAL, DEFINED label, so the
 * diagnostic is about the template and not about an undeclared name.
 *
 * RED-on-disable: drop `hasLabelList`/`hasGotoQualifier` from the grammar -> a P_*
 * parse cascade replaces the single S006A. Remove the placeholder bound check -> this
 * compiles clean and the golden goes EMPTY, which the harness refuses outright. */
int main(void) {
    __asm__ goto ("%0" : : : : lbl);
lbl:
    return 0;
}
