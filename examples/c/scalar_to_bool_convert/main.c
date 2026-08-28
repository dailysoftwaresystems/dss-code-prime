/* D-CSUBSET-NULLPTR-BOOL-CONVERSION (C 6.3.1.2): the scalar -> `_Bool` implicit
 * assignment conversion. A scalar (arithmetic / pointer / nullptr) assigned to a
 * `_Bool` yields 0 if it compares equal to 0, else 1 — NOT a low-bit-truncating
 * Cast (`_Bool b = 2` is TRUE, not false). The semantic tier admits it via
 * isAssignable's `scalarConvertsToBool` arm; coerce() REALIZES the `!= 0`
 * truthiness test (the SAME shape `if(x)` lowers), so the assignment and condition
 * paths cannot drift. This closes the gap the D-CSUBSET-SIZEOF-COMPARISON-INT-TYPE
 * fix unmasked: once a comparison types `int`, `_Bool b = (a<b)` flows int->bool.
 *
 * RED-ON-DISABLE: revert the isAssignable `scalarConvertsToBool` arm -> every
 * `_Bool b = <scalar>` below fails S0003 and the example does not compile. The
 * `_Bool btwo = two` row is the KEY value witness: a low-bit Trunc would make it
 * false (0); the `!= 0` truthiness makes it true (1). io() keeps values runtime so
 * the optimized (release) arm exercises the real truthiness materialization. */

int io(int x) { return x; }   /* opaque: keeps values runtime across the optimized arm */

enum E { EZ, EO };            /* enum -> bool bridges enum->underlying-int then != 0 */

/* ── THE THREE SOURCE CATEGORIES THE ARM'S FIRST ROSTER LEFT OUT ─────────────
 * "Scalar" is ARITHMETIC ∪ POINTER (C 6.2.5p21), and three C source categories
 * land in that set without ever appearing in a hand-written rank list:
 *   * `_BitInt(N)`          — C23 6.2.5: a bit-precise INTEGER, hence arithmetic.
 *   * an ARRAY              — C 6.3.2.1p3 converts it to `&a[0]` FIRST.
 *   * a FUNCTION DESIGNATOR — C 6.3.2.1p4 converts it to `&fn` FIRST.
 * So this is ONE conversion composed with an ALREADY-admitted one, not a new
 * rule about `_Bool`. ✔MEASURED with each reference probed SEPARATELY: clang
 * 18.1.3 -std=c23 compiles and RUNS all three; gcc 13.3.0 -std=c2x runs the array
 * and the designator and lacks `_BitInt` entirely (a missing FEATURE, not a
 * rejection). The bar's test is the DISJUNCTION, so all three are required.
 *
 * ⚠ EVERY ROW BELOW IS CHOSEN SO A WRONG REALIZATION GIVES A DIFFERENT ANSWER —
 * accepting the conversion and materializing it wrongly is worse than refusing:
 *   * `bbi`  — a `_BitInt` holding 2: its LOW BIT IS 0, so a low-bit-truncating
 *              Cast says FALSE where the `!= 0` truthiness says TRUE.
 *   * `bbw`  — an 80-bit `_BitInt` holding `1 << 70`: LIMB 0 IS ENTIRELY ZERO,
 *              so a low-LIMB-only compare says FALSE.
 *   * `barr` — an array whose CONTENTS are all zero: a VALUE-load of the first
 *              element says FALSE where the decayed ADDRESS says TRUE (the exact
 *              shape of the c91 array-in-condition bug).
 *   * `bfn`  — a function designator, which has a truth value only once decayed.
 * Each category is driven through all FOUR assignment-family positions — INIT,
 * ASSIGN, CALL-ARG and RETURN — because those are four callers of ONE
 * `isAssignable`/`coerce` pair, so a fix at one caller would be a fix at none.
 * ⓘ The CONDITION forms (`if (a)`, `if (fn)`, `if (w)`) already worked before
 * this: `coerceCondition` is the one truthiness chokepoint and already knew every
 * scalar kind. Only the assignment family's guard onto that chokepoint had a
 * second, hand-copied roster — which is what drifted. */
static int   sfn(int v)            { return v; }        /* a fn DESIGNATOR    */
static _Bool takeBool(_Bool b)     { return b; }        /* CALL-ARG position  */
static _Bool retArr(void)          { static int z[3]; return z; }   /* RETURN */
static _Bool retFn(void)           { return sfn; }                  /* RETURN */
static _Bool retBi(_BitInt(37) v)  { return v; }                    /* RETURN */

int main(void) {
    int  five = io(5);
    int  zero = io(0);
    int  two  = io(2);
    int *p    = &five;         /* a non-null pointer */
    int *np   = 0;             /* a null pointer (null-pointer constant) */
    double half = io(1) / 2.0; /* 0.5 at runtime (opaque)  */
    double dz   = io(0) / 2.0; /* 0.0 at runtime           */
    enum E eo   = io(1);       /* runtime enum EO (int->enum assign) */
    enum E ez   = io(0);       /* runtime enum EZ          */

    _Bool bcmp = (io(3) < io(4));   /* comparison (int at semantic) -> bool : true  */
    _Bool bnz  = five;              /* nonzero int -> bool : true  */
    _Bool bz   = zero;              /* zero int    -> bool : false */
    _Bool btwo = two;               /* 2 -> bool : TRUE (the low-bit-Trunc catch)  */
    _Bool bptr = p;                 /* non-null ptr -> bool : true  */
    _Bool bnp  = np;                /* null ptr     -> bool : false */
    _Bool bnul = nullptr;           /* nullptr      -> bool : false */
    _Bool bf   = half;              /* 0.5 float -> bool : true  (FCmp != 0.0, NOT an int compare) */
    _Bool bfz  = dz;                /* 0.0 float -> bool : false */
    _Bool ben  = eo;                /* enum EO   -> bool : true  (bridges enum->int) */
    _Bool bez  = ez;                /* enum EZ   -> bool : false */

    if (bcmp != 1) return 1;
    if (bnz  != 1) return 2;
    if (bz   != 0) return 3;
    if (btwo != 1) return 4;        /* 2 -> true, NOT low-bit false */
    if (bptr != 1) return 5;
    if (bnp  != 0) return 6;
    if (bnul != 0) return 7;
    if (bf   != 1) return 10;       /* 0.5 float -> true via FCmp, NOT an int compare */
    if (bfz  != 0) return 11;
    if (ben  != 1) return 12;       /* enum bridges to int then != 0 */
    if (bez  != 0) return 13;
    /* round-trip the stored _Bool values through arithmetic (each is a 0/1 byte) */
    if (bcmp + bnz + btwo + bptr + bf + ben != 6) return 8;
    if (bz + bnp + bnul + bfz + bez != 0) return 9;

    /* ── _BitInt / array / function designator -> _Bool ────────────────────── */
    _BitInt(37) bi  = io(2);        /* 2: NON-zero with a ZERO low bit */
    _BitInt(37) biz = io(0);        /* 0                               */
    unsigned _BitInt(80) wide = (unsigned _BitInt(80))io(1);
    wide = wide << 70;              /* bit 70 -> LIMB 1; limb 0 is ALL ZERO */
    int zarr[4] = {0, 0, 0, 0};     /* contents all zero; its ADDRESS is not */

    _Bool bbi  = bi;                /* init,   _BitInt 2        -> true  */
    _Bool bbiz = biz;               /* init,   _BitInt 0        -> false */
    _Bool bbw  = wide;              /* init,   _BitInt(80) 2^70 -> true  */
    _Bool barr = zarr;              /* init,   array decay      -> true  */
    _Bool bfn  = sfn;               /* init,   fn designator    -> true  */
    _Bool bas1 = 0, bas2 = 0, bas3 = 1;
    bas1 = zarr;                    /* assign, array decay      -> true  */
    bas2 = bi;                      /* assign, _BitInt 2        -> true  */
    bas3 = biz;                     /* assign, _BitInt 0        -> false */

    if (bbi  != 1) return 14;       /* 2 -> TRUE: a low-BIT Trunc would say 0  */
    if (bbiz != 0) return 15;
    if (bbw  != 1) return 16;       /* 2^70 -> TRUE: a low-LIMB test says 0    */
    if (barr != 1) return 17;       /* the ZERO-FILLED array's ADDRESS -> TRUE */
    if (bfn  != 1) return 18;
    if (bas1 != 1) return 19;
    if (bas2 != 1) return 20;
    if (bas3 != 0) return 21;
    if (takeBool(zarr) != 1) return 22;   /* call-arg, array          */
    if (takeBool(sfn)  != 1) return 23;   /* call-arg, fn designator  */
    if (takeBool(bi)   != 1) return 24;   /* call-arg, _BitInt 2      */
    if (takeBool(biz)  != 0) return 25;   /* call-arg, _BitInt 0      */
    if (retArr()       != 1) return 26;   /* return,   array          */
    if (retFn()        != 1) return 27;   /* return,   fn designator  */
    if (retBi(bi)      != 1) return 28;   /* return,   _BitInt 2      */
    if (retBi(biz)     != 0) return 29;   /* return,   _BitInt 0      */
    /* round-trip the new stored bytes through arithmetic, both polarities */
    if (bbi + bbw + barr + bfn + bas1 + bas2 != 6) return 30;
    if (bbiz + bas3 != 0) return 31;
    return 42;
}
