/* c48 (D-CSUBSET-BOOL-CHAR-WIDENING): an ACTUAL `_Bool` value assigned to a plain
 * `char` lhs (C 6.3.1.1: `char` is an integer type; a `_Bool` 0/1 widens into it).
 * The frontier was sqlite3.c:25926 `p->nFloor = (p->D==31);` (nFloor is `char`),
 * which fired error[S0003] S_TypeMismatch.
 *
 * NOTE (2026-07-20): a comparison/logical RESULT now types as C's `int` at the
 * SEMANTIC tier (D-CSUBSET-SIZEOF-COMPARISON-INT-TYPE) — so `char c = (a==b)` now
 * routes via the int->char `charConvertsToArith` arm, NOT this Bool->char arm.
 * To keep witnessing the c48 `boolWidensToArith`+`Char` path, the operand must be
 * an ACTUAL `_Bool` value: `_Bool flag = ...; char c = flag;`. `isAssignable`'s
 * boolWidensToArith arm admits a Bool into the int/float RANK kinds but NOT plain
 * `char` (TypeKind::Char, outside the ranks) unless the `lk==Char` term (c48) is
 * present. coerce() materializes a Bool->Char Cast — a SAME-WIDTH mov (DSS interns
 * Bool as 8-bit, not i1), correct for a canonical 0/1 byte.
 *
 *   flag = (io(41)==41)  an actual _Bool : true   (a comparison IS Bool at the HIR
 *                                                   tier; its SEMANTIC type is int,
 *                                                   so obtaining `flag` also rides
 *                                                   scalar->bool — see the dedicated
 *                                                   scalar_to_bool_convert example)
 *   none = (io(41)< 41)  an actual _Bool : false
 *   c = flag             Bool -> char  (c48 boolWidensToArith+Char) : 1
 *   d = none             Bool -> char                                : 0
 *   e = io(7) - io(7)    int  -> char  (the DIFFERENTIATOR — int->char always OK) : 0
 *   return c + d + e + 41  ==  1 + 0 + 0 + 41  ==  42
 *
 * RED-ON-DISABLE: revert the `lk == TypeKind::Char` term in the boolWidensToArith
 * arm -> `char c = flag` (Bool->char) fails S_TypeMismatch (S0003) and the example
 * does not compile. The `e = io(7)-io(7)` line stays fine either way, proving the
 * gap is the Bool-value->char path, NOT int->char narrowing. */

int io(int x) { return x; }   /* opaque: keeps values runtime across the optimized arm */

int main(void) {
    _Bool flag = io(41) == 41;   /* an ACTUAL _Bool value : true */
    _Bool none = io(41) <  41;   /* an ACTUAL _Bool value : false */
    char c = flag;               /* Bool -> char via boolWidensToArith+Char (c48) : 1 */
    char d = none;               /* Bool -> char : 0 */
    char e = io(7) - io(7);      /* int arithmetic -> char (the differentiator) : 0 */
    return c + d + e + 41;       /* 1 + 0 + 0 + 41 == 42 */
}
