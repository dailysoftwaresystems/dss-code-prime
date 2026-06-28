/* c41 (D-CSUBSET-POINTER-INT-ARITHMETIC) C 6.5.6p8: `p + n` / `n + p` / `p - n`
 * must step n * sizeof(*p) BYTES (the element stride), NOT 1 byte. This is the
 * most pervasive flagged run-time silent miscompile (sqlite indexes pointers
 * everywhere). Uses int* (stride 4) + a struct (stride >= 8); char* (stride 1)
 * is trivially unaffected (no Mul). Each failing sub-case returns its own code
 * so a regression narrows to the broken form. Exit 42 iff ALL correct.
 *
 * RED-ON-DISABLE: revert the combineBinaryOp c41 Gep intercept -> a raw Add/Sub
 * with 1-BYTE stride -> `p + 2` reads p+2 BYTES not p+2 elements -> wrong slot
 * -> return 1. Uses pointer VARS (not array names, dodging the deferred p-arr
 * gap) + int fields (dodging the sub-32-bit ALU gap). */
struct Pair { int x; int y; };

int main(void) {
    int a[6];
    a[0] = 10; a[1] = 20; a[2] = 30; a[3] = 40; a[4] = 50; a[5] = 60;
    int *p = a;

    int *q = p + 2;                 /* p + n */
    if (*q != 30) return 1;         /* a[2]=30; 1-byte stride -> garbage */

    int *r = 3 + p;                 /* n + p (commutative) */
    if (*r != 40) return 2;         /* a[3]=40 */

    int *s = (p + 5) - 3;           /* p - n */
    if (*s != 30) return 3;         /* a[2]=30 */

    int neg = -1;                   /* negative n: (p+3) + (-1) = p+2 */
    int *t = (p + 3) + neg;
    if (*t != 30) return 4;         /* a[2]=30 */

    struct Pair pairs[4];
    pairs[0].x = 1; pairs[1].x = 3; pairs[2].x = 5; pairs[3].x = 7;
    struct Pair *pp = pairs;

    struct Pair *pp2 = pp + 2;      /* struct stride: +2*sizeof(Pair), not +2 bytes */
    if (pp2->x != 5) return 5;      /* pairs[2].x=5 */

    struct Pair *pp1 = 1 + pp;      /* n + p, struct stride */
    if (pp1->x != 3) return 6;      /* pairs[1].x=3 */

    struct Pair *ppb = (pp + 3) - 1;/* p - n, struct stride */
    if (ppb->x != 5) return 7;      /* pairs[2].x=5 */

    return 42;
}
