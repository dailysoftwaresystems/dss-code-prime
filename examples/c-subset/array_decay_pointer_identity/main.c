/* c91 (D-CSUBSET-ARRAY-DECAY-POINTER-IDENTITY +
 * D-CSUBSET-ARRAY-DECAY-IN-COMPARISON + D-CSUBSET-ARRAY-DECAY-IN-CONDITION)
 * end-to-end RUNTIME witness: an ARRAY in a value position where a POINTER
 * is expected DECAYS to the address of its first element (C 6.3.2.1p3) —
 * comparison operands (both orders, ==/!=/</>), condition positions
 * (if / && / ternary-cond / `!`), and the POINTER-IDENTITY law
 * `arr == &arr[0] == &arr` (as addresses).
 *
 * Pre-c91 a MEMBER or GLOBAL array operand of a comparison/condition was
 * VALUE-LOADED — the compare read the array's first BYTES as a "pointer":
 *   - sqlite sqlite3ParserFinalize (sqlite3.c:182106)
 *         if( pParser->yystack != pParser->yystk0 ) YYFREE(pParser->yystack);
 *     compared yystack against yystk0's CONTENT -> always unequal -> freed
 *     the on-stack parser -> glibc `free(): invalid pointer` SIGABRT on
 *     EVERY SQL statement (the c91 wall). Check 1 is that EXACT shape.
 *   - a ZERO-filled global array compared EQUAL to a null pointer and its
 *     live address compared UNEQUAL to itself (checks 8/9 — the zero-luck
 *     shapes that silently green stack-luck tests).
 * (A plain LOCAL array was already correct — the c63 Ref-local decay — so
 * every array here that must FAIL pre-fix is a member, global, or deref.)
 *
 * Contents are NONZERO wherever content-vs-address confusion must flip the
 * exit (zero-luck defeat), and the core identity rows read their pointer
 * through a `volatile` slot so no pass can fold the compare away.
 *
 * RED-ON-DISABLE (audit-verified per layer):
 *   - HIR comparison decay arm reverted -> check 12 (`z == "x"`) fails
 *     LOUD (A_NoMatchingEncodingVariant: un-decayed string-literal
 *     operand);
 *   - HIR condition decay arm reverted  -> checks 13/14/16 fail LOUD
 *     (I_TerminatorTypeMismatch: raw Array condition);
 *   - HIR `!` decay arm reverted        -> check 15 (unit pin reds; e2e
 *     kept correct by the MIR backstop);
 *   - MIR value-read decay arms (the c63-twin backstop) reverted -> the
 *     member/global compare checks red end-to-end only with the HIR arms
 *     ALSO off (the two tiers overlap by design: the HIR funnel is the
 *     semantics, the MIR arm the mechanism guard) — the backstop's own
 *     red is the MIR unit pin (no-Array-typed-Load invariant) + the
 *     va_list-forward corpus example.
 * => 42 on success; each failing check exits with its own code. */

int gArr[4] = {11, 22, 33, 44};   /* NONZERO global (content != address) */
int gOther[4] = {11, 22, 33, 44}; /* same CONTENT, different address     */
int *gp = gArr;                   /* file-scope init decay (c68 path)    */
static int zeroArr[2];            /* ZERO content — the zero-luck shape  */

struct Inner { int pad; int arr[3]; };
struct Outer { int pad2; struct Inner in; };

/* the sqlite3ParserFinalize shape — EXACT (member array vs member ptr) */
struct yyParser { int *yystack; int yystk0[8]; };

static int freeCount = 0;
static void yyfree(int *p) { freeCount = freeCount + 1; (void)p; }

static int parserFinalizeShape(void) {
    struct yyParser sEngine;
    struct yyParser *pParser = &sEngine;
    sEngine.yystack = sEngine.yystk0;      /* stack never grew */
    sEngine.yystk0[0] = 0x7777;            /* NONZERO content  */
    if (pParser->yystack != pParser->yystk0) {   /* must compare EQUAL */
        yyfree(pParser->yystack);          /* the pre-c91 bogus free */
    }
    return freeCount;                      /* want 0 */
}

int paramIdentity(int a[4], int *expect) {  /* param-adjusted: a IS int* */
    return a == expect;                     /* want 1 */
}

int *returnDecay(void) { return gArr; }     /* return-position decay */

int main(void) {
    /* 1. the sqlite yystack shape: the bogus free must be SKIPPED */
    if (parserFinalizeShape() != 0)                        return 10;

    /* volatile-laundered global pointer: the compares below are RUNTIME */
    int * volatile vslot = gArr;
    int *p = vslot;

    /* 2./3. GLOBAL array vs pointer — BOTH orders */
    if (!(p == gArr))                                      return 11;
    if (!(gArr == p))                                      return 12;
    /* 4./5. != both orders (must be false) */
    if (p != gArr)                                         return 13;
    if (gArr != p)                                         return 14;
    /* 6. the POINTER-IDENTITY law: arr == &arr[0] == &arr (addresses) */
    if (!(gArr == &gArr[0]))                               return 15;
    if (!((void *)&gArr == (void *)gArr))                  return 16;
    /* 7. distinct arrays with IDENTICAL content compare UNEQUAL
          (pre-fix content-compare said EQUAL) */
    if (gArr == gOther)                                    return 17;
    /* 8./9. the zero-luck shapes: a zero-filled global's ADDRESS is
          neither null nor unequal-to-itself */
    int * volatile vz = zeroArr;
    if (!(vz == zeroArr))                                  return 18;
    if (zeroArr == 0)                                      return 19;
    /* 10. MEMBER + NESTED member + via-DEREF, both orders */
    struct Outer o;
    struct Outer * volatile vpo = &o;
    struct Outer *po = vpo;
    o.in.arr[0] = 5;                       /* nonzero content */
    int *q = o.in.arr;                     /* statement-assign decay (works pre-fix) */
    if (!(q == o.in.arr))                                  return 20;
    if (!(o.in.arr == q))                                  return 21;
    if (!(po->in.arr == q))                                return 22;
    if (q != po->in.arr)                                   return 23;
    /* 11. relational forms (< > with a decayed array operand) */
    if (!(gArr < gp + 1))                                  return 24;
    if (!(gp < gArr + 1))                                  return 25;
    if (gArr > gp)                                         return 26;
    /* 12. string-literal comparison operand (the c79-discovered sibling:
           fail-loud pre-fix). z is null so the compare is FALSE. */
    char * volatile vzp = (char *)0;
    char *z = vzp;
    if (z == "x")                                          return 27;
    /* 13./14. condition positions: if(arr) / arr&&x — always true */
    if (gArr) { } else                                     return 28;
    if (!(zeroArr && 1))                                   return 29;
    /* 15. `!arr` — always false */
    if (!gArr)                                             return 30;
    /* 16. ternary condition on an array */
    if ((o.in.arr ? 1 : 0) != 1)                           return 31;
    /* 17. param-adjusted array param identity */
    int loc[4];
    loc[0] = 9;
    if (!paramIdentity(loc, &loc[0]))                      return 32;
    /* 18. return-position decay identity */
    if (!(returnDecay() == gArr))                          return 33;
    /* 19. deref-of-pointer-to-array decays: *pa == &(*pa)[0] == gArr */
    int (* volatile vpa)[4] = &gArr;
    int (*pa)[4] = vpa;
    if (!(*pa == gArr))                                    return 34;

    return 42;
}
