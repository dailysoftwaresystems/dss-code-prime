/* c56 (D-CSUBSET-TERNARY-NULL-FUNCTION-POINTER): a conditional with one arm a
 * FUNCTION DESIGNATOR (a bare function name, type FnSig) and the other a literal-0,
 * assigned to a function-pointer lvalue. SQLite name-resolution:
 *     w.xSelectCallback = (pNC->ncFlags & NC_NoSelect) ? 0 : resolveSelectStep;
 * (sqlite3.c:111984; xSelectCallback is int(*)(Walker*,Select*)) fired S0003
 * S_TypeMismatch: combineTernary's c49 null-pointer-constant rule matched only
 * TypeKind::Ptr, never TypeKind::FnSig (an un-decayed function designator), so the
 * `0 : resolveSelectStep` conditional fell through to I32 and the assignment
 * isAssignable(Ptr<FnSig>, I32) failed. (The displayed line 20547 is the NC_NoSelect
 * #define — a macro-shifted source-span artifact; the real site is the assignment.)
 *
 * FIX (the c49 fn-pointer sibling, both tiers): a FnSig arm opposite a literal-0
 * decays to Ptr<FnSig> (C 6.3.2.1p4 function-to-pointer decay + 6.5.15p6) — the
 * designator coerces via a FnSig->Ptr Bitcast, the literal-0 via a null-ptr Cast.
 *
 * RED-ON-DISABLE: revert -> the `? 0 : add40` assignment (order 1) -> S0003.
 *
 * VALUE-CORRECT (the picked function is CALLED to prove the fn-ptr is the right,
 * non-NULL target — and BOTH arm orders are exercised, since the fix also cures a
 * latent silent-miscompile in the designator-FIRST order):
 *   order 1 (sqlite): w.cb = flag ? 0 : add40; flag==0 -> add40; add40(2) = 42.
 *   order 2 (swapped): w.cb = t ? sub1 : 0;     t==1   -> sub1;  sub1(43)  = 42. */
typedef int (*cb_t)(int);
struct W { cb_t cb; };

int add40(int x) { return x + 40; }
int sub1(int x)  { return x - 1; }

int main(void) {
    struct W w;
    int flag = 0;

    w.cb = flag ? 0 : add40;     /* order 1 (sqlite): (cond) ? 0 : designator */
    if (w.cb == 0) return 1;
    int a = w.cb(2);             /* add40(2) = 42 */

    int t = 1;
    w.cb = t ? sub1 : 0;         /* order 2 (swapped): (cond) ? designator : 0 */
    if (w.cb == 0) return 2;
    int b = w.cb(43);            /* sub1(43) = 42 */

    if (a != 42) return 3;
    return b;                    /* 42 */
}
