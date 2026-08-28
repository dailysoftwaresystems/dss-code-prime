/* inline-asm P1 (D-LANG-GNU-EXTENDED-INLINE-ASM-UNSUPPORTED): a REPEATED inline-asm
 * qualifier is rejected — S_InlineAsmDuplicateQualifier (S0064). ✔MEASURED rejected
 * by gcc, clang and MSVC alike, so accepting it would be a direction-B defect (DSS
 * admitting what no reference compiler admits).
 *
 * ★ THE TWO SPELLINGS ARE THE TEST, not decoration. `volatile` and `__volatile__` are
 * DIFFERENT TEXT for the SAME token kind — DSS's keyword table aliases the dunder
 * spellings onto one kind. A duplicate detector written over SPELLINGS would see two
 * distinct strings and pass this file silently; only a detector written over TOKEN
 * KIND catches it. Pinning `volatile volatile` instead would have proved nothing
 * about which of the two was implemented, so the kind-not-spelling case is the one
 * that belongs in the corpus.
 * ★ THE MESSAGE ECHOES THE SOURCE TEXT of the offending token, so the golden's span
 * lands on the SECOND occurrence (`__volatile__`, col 22) rather than the first or
 * the statement — a detector that reported the wrong occurrence still "fires" but
 * points the author at the wrong token, and that flips this golden.
 *
 * ★ The qualifier run is otherwise INERT here: no sections, `goto` absent, template
 * empty. So checks (1)-(3) all pass and S0064 arrives ALONE — this file pins that the
 * duplicate check is ORTHOGONAL to the section gates (it is a property of the
 * qualifier run) rather than a side effect of some other refusal.
 *
 * RED-on-disable: drop the qualifier scan, or narrow it from token KIND back to
 * spelling -> zero diagnostics, and the harness refuses an empty golden outright. */
int main(void) {
    __asm__ volatile __volatile__ ("");
    return 0;
}
