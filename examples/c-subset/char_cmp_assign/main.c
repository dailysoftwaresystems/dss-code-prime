/* c48 (D-CSUBSET-BOOL-CHAR-WIDENING): a comparison/logical result — typed `Bool`
 * in the c-subset (C 6.5.9: `==` yields int; DSS types the relational/logical
 * result Bool at both tiers) — assigned to a plain `char` lhs.
 *
 * The frontier is sqlite3.c:25926
 *   p->nFloor = (p->D==31);          (nFloor is `char`)
 * which fired error[S0003] S_TypeMismatch. The `isAssignable` `boolWidensToArith`
 * arm (type_rules.hpp) admitted a Bool into the int/float RANK kinds but NOT
 * plain `char` (interned as `TypeKind::Char`, outside the ranks) — so
 * `char = (a==b)` (Bool result) was rejected while `char = (a-b)` (int result,
 * in rank) was ACCEPTED (via the separate charConvertsToArith arm). c48 adds
 * `Char` to the Bool arm (gated on charConvertsToArith). NO codegen change:
 * coerce() already materializes a Bool->Char Cast — a SAME-WIDTH Bitcast/mov (DSS
 * interns Bool as 8-bit, NOT i1), correct because a comparison/logical result is
 * a canonical 0/1 byte that a same-width copy preserves.
 *
 *   a = b = 41 (kept runtime via io() so the optimized arm runs the real path)
 *   c  = (a == b)   Bool 1 -> char 1   (the sqlite assign-stmt shape)
 *   c += (a <  b)   Bool 0 -> +0        : still 1   (relational, same Bool path)
 *   d  = (a && b)   Bool 1 -> char 1               (logical, same Bool path)
 *   e  =  a -  b    int  0 -> char 0               (the DIFFERENTIATOR: int->char
 *                                                    arithmetic always worked)
 *   return c + d + e + 40  ==  1 + 1 + 0 + 40  ==  42
 *
 * RED-ON-DISABLE: revert the `lk == TypeKind::Char` term in the boolWidensToArith
 * arm -> `c = (a==b)` fails S_TypeMismatch (S0003) -> does not compile. The
 * `e = a - b` line stays fine either way, proving the gap was the Bool-RESULT
 * path into `char`, NOT int->char narrowing. */

int io(int x) { return x; }   /* opaque: keeps a,b runtime across the optimized arm */

int main(void) {
    int a = io(41), b = io(41);
    char c, d, e;
    c  = (a == b);    /* comparison Bool -> char : 1  (sqlite p->nFloor=(p->D==31)) */
    c += (a <  b);    /* relational  Bool -> +0  : still 1 */
    d  = (a && b);    /* logical     Bool -> char : 1 */
    e  =  a -  b;     /* int arithmetic -> char (the differentiator) : 0 */
    return c + d + e + 40;   /* 1 + 1 + 0 + 40 == 42 */
}
