/* c58 (D-CSUBSET-INITIALIZER-CONST-TOKEN-LEAK): a SCALAR local declared with an
 * INITIALIZER that contains a `const` TOKEN (here a (const char*) cast in a call
 * argument) was wrongly marked a const OBJECT, so REASSIGNING it fired error[S0007]
 * S_ConstViolation. SQLite os name-collation (sqlite3.c:188075, nocaseCollatingFunc):
 *     int r = sqlite3StrNICmp((const char*)pKey1, (const char*)pKey2, ...);
 *     if( 0==r ){ r = nKey1-nKey2; }     // <-- S0007 on the reassignment of `r`
 * The clean sibling binCollFunc declares `int rc, n;` (NO initializer) and is fine.
 * ROOT: declaratorObjectIsConst's SCALAR fallback scanned the WHOLE varDecl node —
 * which spans the initializer — for the const marker, so the cast's `const` token
 * leaked onto `r`. (It was purely the TOKEN: even `int r = (int)sizeof(const char);`
 * triggered it.) FIX: a scalar object's const comes ONLY from its type-specifier
 * HEAD (kids[*decl.headChild], base type, no pointer stars) — never the initializer.
 *
 * RED-ON-DISABLE: revert the fix (scan declNode) -> `int r = firstByte((const char*)
 * buf)` marks r const -> the `r = 37 + r` reassignment -> S0007 (does not compile).
 *
 * NEGATIVE PIN (verified separately, NOT here since it must NOT compile): a genuine
 * `const int c = 5; c = 6;` MUST still error S0007 — its const lives in the HEAD, so
 * the narrowed scan still catches it.
 *
 * VALUE-CORRECT: r is REASSIGNED and the new value drives the exit (a wrong parse
 * that dropped the reassignment would return the init value 5, not 42). */
int firstByte(const char *s) { return (int)s[0]; }

int main(void) {
    char buf[2];
    buf[0] = 5;
    buf[1] = 0;

    int r = firstByte((const char *)buf);   /* init via a call whose arg has a (const char*) cast; r = 5 */
    if (r == 5) {
        r = 37 + r;                          /* the reassignment that fired S0007 before the fix; r = 42 */
    }
    return r;                                /* 42 */
}
