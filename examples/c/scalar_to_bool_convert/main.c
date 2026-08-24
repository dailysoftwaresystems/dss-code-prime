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
    return 42;
}
