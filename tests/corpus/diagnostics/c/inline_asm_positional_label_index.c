/* inline-asm P20 (D-CSUBSET-INLINE-ASM-OPERANDS): the POSITIONAL `asm goto` label
 * reference `%l<N>` is ACCEPTED, and what this tier checks is its INDEX. There are
 * exactly two ways to get that index wrong and both are pinned here, both S006A.
 *
 * ★★★ THIS FILE REPLACES A REFUSAL, IT DOES NOT ADD ONE. Before P20 the semantic
 * tier refused the positional spelling OUTRIGHT (S0067) because no tier below it
 * could represent a label reference at all. Both reference compilers accept
 * `%l<N>`, and under the bar's "one working reference makes the behaviour REQUIRED"
 * rule that blanket refusal was a divergence. The grammar arm and the label-binding
 * lowering land in the same cycle as this check, so the only question left is
 * whether the index names a label.
 *
 * ★★ WHY THE FIXTURE NEEDS AN OPERAND AT ALL — and this is the half a careless
 * version of this file would omit. GNU 6.47.2.7 numbers the labels AFTER every
 * operand. With ZERO operands a labels-only numbering and the real one AGREE, so a
 * fixture without an operand cannot tell a correct base from a wrong one and would
 * be green under both. This statement declares ONE input operand, so the single
 * label is index 1: `%l0` names the OPERAND (the reference's "'%l' operand isn't a
 * label") and `%l9` names nothing at all (its "operand number out of range").
 *
 * ★ WHAT THE TWO MESSAGES SAY is pinned by CONTENT in the unit suite
 * (`InlineAsmRefusals.APositionalAsmGotoLabelReferenceIsAcceptedAndItsIndexIsChecked
 * AgainstTheLabelRange`), which also carries the POSITIVE controls this harness
 * cannot hold — a correctly-numbered `%l1`, the bracketed `%l[lbl]`, and a label
 * listed but never referenced all emit NOTHING, and a zero-diagnostic file is one
 * this harness refuses by construction. This file pins the CODE and the POSITION,
 * which the unit suite deliberately does not.
 *
 * ⚠ `lbl` is a REAL, DEFINED label, so neither diagnostic is about an undeclared
 * name, and `x` is read by both statements, so neither is about an unused local.
 *
 * RED-on-disable: delete the label-range check -> both statements compile clean, the
 * golden goes EMPTY and the harness refuses it outright. Restore the blanket
 * positional refusal -> the code changes from S006A to S0067 on BOTH lines. Widen
 * the bound to accept any index -> the second line disappears while the first
 * survives, which is the asymmetry that names which half broke. */
int main(void) {
    unsigned x = 1;
    __asm__ goto ("jmp %l0" : : "r"(x) : : lbl);
    __asm__ goto ("jmp %l9" : : "r"(x) : : lbl);
lbl:
    return 0;
}
