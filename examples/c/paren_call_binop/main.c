/* c55 (D-CSUBSET-CALL-IN-PAREN-CAST-AMBIGUITY): a parenthesized function call
 * whose argument is an identifier, immediately followed by a binary operator —
 * the shape SQLite's ROUND8(x)=(((x)+7)&~7) macro produces around a call:
 *     journalFileSize = ROUND8(sqlite3JournalSize(pVfs));
 *  -> journalFileSize = (((sqlite3JournalSize(pVfs))+7)&~7);   (sqlite3.c:63552)
 * The parser's cast-vs-value triage (typeNameCommitApproved_, parser.cpp Rule 1)
 * committed whenever the parenthesized type-child had >1 leaf; `idn(p)`'s `(p)`
 * tail made it commit as a CAST to a type named `idn(p)` (base `idn` + abstract
 * fn-declarator `(p)`, p an unnamed param type), skipping the follower-operator
 * test -> resolveTypeNode failed on `idn(p)` -> error[S0006] S_UnknownType. Fix:
 * Rule 1 keys on the BASE (leftmost) leaf; an IDENTIFIER base (even with a `(p)`
 * / `*` declarator tail) stays type-vs-value ambiguous and falls through to the
 * sketch lookup + follower test, which sees the trailing `+`/`&`/`*` operator and
 * rolls back to the call reading. The c26 sibling of the `(c*c)`/`(b[0])` excl.
 *
 * RED-ON-DISABLE: revert Rule 1 -> `(idn(p))+...` mis-parses as a cast -> S0006.
 *
 * VALUE-CORRECT: idn(x)=x.
 *   r = (((idn(p)) + 7) & ~7), p=35  -> (35+7)&~7 = 40   (the EXACT ROUND8 shape)
 *   s = ((idn(q)) + 22),       q=20  -> 20+22     = 42   (a (call)+op shape)
 *   return s gated on r==40 (so a wrong parse / value breaks the exact result). */
int idn(int x) { return x; }

int main(void) {
    int p = 35;
    int q = 20;
    int r = (((idn(p)) + 7) & ~7);   /* ROUND8(idn(35)) = 40 */
    int s = ((idn(q)) + 22);         /* (idn(20)) + 22 = 42 */
    if (r != 40) return 1;
    return s;                        /* 42 */
}
