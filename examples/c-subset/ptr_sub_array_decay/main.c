/* D-CSUBSET-POINTER-DIFF-ARRAY-DECAY (C 6.5.6p9 + 6.3.2.1p3): `pointer - arrayName`
 * (and `array - pointer`, `array - array`) is a pointer DIFFERENCE (ptrdiff_t = the
 * ELEMENT count) — the array operand decays to Ptr<elem> FIRST, then it is `p - q`.
 *
 * The sqlite FTS5 frontier: fts5TriTokenize does `zOut - aBuf` where `char aBuf[32];
 * char *zOut = aBuf;` and passes the difference DIRECTLY as an `int` token length —
 * NO explicit cast. DSS's semantic type-oracle required BOTH operands already Ptr and
 * never decayed an Array, so `zOut - aBuf` typed Ptr<char>; used in an integer context
 * it failed isAssignable -> error[S0003]. (The existing pointer_minus_array example
 * always CAST the result `(int)(t - arr)`, and the explicit pointer->int cast absorbed
 * the mistyped Ptr — which is exactly why that example never caught this gap.) The fix
 * decays the array operand in the semantic pointer-subtraction arm, mirroring the HIR
 * combineBinary c65 that already lowers it (PtrToInt + Sub + SDiv by sizeof(elem)).
 *
 * Every subtraction below is UNCAST — the difference flows implicitly into an `int`
 * lvalue, so the SEMANTIC decay (not an explicit cast) is what is under test. The
 * 4-byte `int arr[16]` makes the stride DIVISION load-bearing: ip - arr == 6 (the
 * element index), NOT 24 (the byte offset) — a missing decay/division would yield 24.
 * All offsets come through io() so the pointers stay RUNTIME across the optimized arm.
 *
 * RED-ON-DISABLE: revert the semantic pointer-subtraction array-decay -> `int a =
 * p - buf;` types Ptr<char> into an int lvalue -> error[S0003] and the program does NOT
 * compile (reverting the HIR c65 decay too reintroduces the older MIR abort). */

int io(int x) { return x; }   /* opaque: keeps the offsets (and pointers) runtime */

int main(void) {
    /* --- char array: the FTS5 `zOut - aBuf` shape (stride 1), UNCAST --- */
    char buf[32];
    char *p = buf + io(30);       /* &buf[30] (runtime, in-bounds) */
    char *q = buf + io(12);       /* &buf[12] */

    int a = p - buf;              /* ptr   - array  (implicit int, NO cast) :  30 */
    int b = q - buf;              /* ptr   - array                          :  12 */
    int c = p - q;                /* ptr   - ptr    (existing c40 control)  :  18 */
    int d = buf - buf;            /* array - array                          :   0 */
    int e = buf - p;              /* array - ptr                            : -30 */
    if (c != 18)  return 1;
    if (d != 0)   return 2;
    if (e != -30) return 3;

    /* --- int array (stride 4): the diff is the ELEMENT count, not bytes --- */
    int arr[16];
    int *ip = arr + io(6);
    int f = ip - arr;             /* ptr - array : element index 6 (NOT 24 bytes) */
    if (f != 6) return 4;

    return a + b;                 /* 30 + 12 = 42 */
}
