/* inline-asm P1 (D-LANG-GNU-EXTENDED-INLINE-ASM-UNSUPPORTED): `asm goto` — the
 * control-flow form — is parsed end to end and refused with exactly ONE
 * S_InlineAsmExtendedUnsupported (S0062).
 *
 * ★ THIS IS A DIFFERENT GATE ARM FROM `inline_asm_extended_unsupported.c`, not a
 * spelling of it. That file trips the gate on OPERANDS; this one carries no operands
 * at all — its section list is empty and only the `goto` qualifier + the label list
 * are present. Two of the extended gate's presence flags (`hasLabelList`,
 * `hasGotoQualifier`) are reachable ONLY through this shape, so without this file
 * they would be untested and `asm goto` could regress to a clean barrier with `lbl`
 * silently dropped — the asm would fall through where the author wrote a branch.
 * ★ IT IS ALSO THE CONTROL FOR THE FOURTH-SECTION RULE. The sibling
 * `inline_asm_label_section_without_goto.c` pins that a label section WITHOUT `goto`
 * is a constraint violation (S0063). Here the qualifier IS present, so S0063 must NOT
 * fire and the single S0062 is the whole golden — the pair pins both polarities of
 * that predicate.
 *
 * ★ The template is EMPTY so the golden pins the goto refusal alone rather than
 * racing S_InlineAsmNonEmptyTemplate (S0057), and `lbl` is a REAL, DEFINED label so
 * the diagnostic is about the asm and not about an undeclared name.
 *
 * RED-on-disable: drop `hasLabelList`/`hasGotoQualifier` from the extended gate ->
 * this compiles CLEAN and the golden goes empty (the harness refuses an empty
 * golden). */
int main(void) {
    __asm__ goto ("" : : : : lbl);
lbl:
    return 0;
}
