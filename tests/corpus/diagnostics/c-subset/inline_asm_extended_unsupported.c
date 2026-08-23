/* inline-asm P5 (D-CSUBSET-INLINE-ASM-OPERANDS): a `%N` placeholder naming an operand
 * that does not exist is refused with exactly ONE S_InlineAsmPlaceholderOutOfRange
 * (S006A), and the bound it is checked against is the OUTPUTS-THEN-INPUTS
 * CONCATENATION (GCC 6.47.2.3).
 *
 * ★★★ THIS FILE CHANGED MEANING IN P5, AND THE REASON IS THE WHOLE POINT OF THE ARC.
 * It used to pin S_InlineAsmExtendedUnsupported (S0062) on
 * `__asm__ __volatile__ ("" : "=a"(lo), "=d"(hi))` — the blanket refusal of every
 * operand-carrying statement, which was correct only while DSS could not CARRY an
 * operand. P5 carries them: that exact statement is sqlite's `hwtime.h` rdtsc shape
 * and it now compiles, so the old golden could not survive and the blanket code has no
 * remaining emit site for this shape.
 *
 * ⛔ ACCEPT-AND-IGNORE IS STILL THE FORBIDDEN OUTCOME — this file just guards it from
 * the other side. Before, the proof that the operands were not dropped was that the
 * statement was REFUSED. Now the proof is that the operands are COUNTED: an index the
 * operand list cannot satisfy is caught. A lowering that dropped the operand list
 * would have nothing to count and this refusal would vanish.
 *
 * ★ WHY `%2` WITH ONE OUTPUT AND ONE INPUT, AND NOT `%9` WITH NONE. The index space is
 * the JOINED list, so `%0` is the output and `%1` is the input; `%2` is the first index
 * that is out of range. A bound written against either SECTION alone — the natural
 * wrong implementation — would accept `%2` here (two sections, one entry each) while
 * rejecting the perfectly legal `%1`. Picking the first out-of-range index of a
 * two-section statement is what makes this file able to see that error; `%9` would pass
 * under both the right rule and the wrong one.
 * ★ `lo` and `x` ARE DECLARED, so no S_UndeclaredIdentifier joins the golden and the
 * one-code shape is preserved.
 * ★ THE TEMPLATE IS OTHERWISE INERT. It carries no instruction, so this pins the
 * placeholder check alone rather than racing a template-parse refusal (S0069).
 *
 * RED-on-disable: remove the placeholder bound check -> this compiles clean and the
 * golden goes EMPTY, which the harness itself refuses (a zero-diagnostic corpus file is
 * an ADD_FAILURE, not a silent pass). Widen the bound to either section's own count ->
 * `%2` is accepted and the golden goes empty the same way. */
int main(void) {
    unsigned lo;
    unsigned x = 1;
    __asm__ ("%2" : "=a"(lo) : "r"(x));
    return 0;
}
