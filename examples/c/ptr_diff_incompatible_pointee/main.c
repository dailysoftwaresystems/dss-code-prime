// D-CSUBSET-POINTER-DIFF-EDGE-CASES part (2) witness (positive / runtime).
//
// `char *a; int *b; a - b` is a C 6.5.6p3 constraint violation, and the row that
// asked for this wanted a LOUD refusal. The measurement refuted that.
//
// ✔MEASURED 2026-09-01, each reference probed SEPARATELY at -O0 AND -O2:
//   gcc 13.3.0 (-std=c2x)       REJECT "invalid operands to binary -"
//   clang 18.1.3 (-std=c23)     REJECT "'char *' and 'int *' are not pointers
//                                       to compatible types"
//   mingw-w64 gcc 13.2.0        REJECT, as gcc
//   MSVC 19.51.36252 (/std:c17) ACCEPT, warning C4133 "'-': incompatible types
//                                       - from 'int *' to 'char *'"
// `DSS = (gcc u clang u MSVC) u ISO C` settles accept-vs-refuse by the
// DISJUNCTION, so an Error here would refuse a program MSVC compiles. DSS warns
// (S_PointerDifferenceIncompatiblePointee) and accepts.
//
// ★★ AND C4133's OWN WORDING GAVE THE MEANING: "from 'int *' TO 'char *'" —
// MSVC converts the RIGHT operand to the LEFT's type and then does an ordinary
// same-type difference, so the stride is sizeof(*LEFT). ✔MEASURED from MSVC's
// own /FAs listing at /O2:
//   char*   - int*   -> `sub`            (stride 1)
//   int*    - char*  -> `sub; sar 2`     (stride 4)
//   double* - char*  -> `sub; sar 3`     (stride 8)
// Every term below is chosen so a WRONG stride gives a DIFFERENT exit code, and
// the three strides differ from each other — a single hardcoded stride cannot
// satisfy all three.
//
// The second half is what this row actually cost users: before P48 the SAME
// expression was accepted in a cast context and REFUSED (S_TypeMismatch) in an
// initializer or argument context, because the refusal was an incidental
// assignability failure on an un-typed `Ptr` rather than a decision about the
// subtraction. All three contexts now agree.
//
// RED-ON-DISABLE: revert the mismatched-pointee arm in the semantic
// `combineBinary` and the `ptrSub` widening in cst_to_hir -> `long n = a - b;`
// and `sink(a - b)` fail S_TypeMismatch and this example does not COMPILE.

static long sink(long n) { return n; }

int main(void) {
    int    y[8];
    double z[4];

    char   *ca = (char *)&y[2];        // 8 bytes past y[0]
    int    *ib = &y[0];
    int    *ia = &y[2];
    char   *cb = (char *)&y[0];
    double *da = &z[2];                // 16 bytes past z[0]
    char   *zb = (char *)&z[0];

    // ── the stride is the LEFT pointee's, three different sizes ─────────────
    long n1 = ca - ib;                 // stride 1 -> 8
    long n2 = ia - cb;                 // stride 4 -> 2
    long n3 = da - zb;                 // stride 8 -> 2
    long n4 = ia - ib;                 // control: same pointee, stride 4 -> 2
    if (n1 != 8) return 1;
    if (n2 != 2) return 2;
    if (n3 != 2) return 3;
    if (n4 != 2) return 4;

    // ── one expression, three contexts, one verdict ─────────────────────────
    int  asCast = (int)(ca - ib);      // cast context   (compiled before P48)
    long asInit = ca - ib;             // init context   (S_TypeMismatch before)
    long asArg  = sink(ca - ib);       // arg context    (S_TypeMismatch before)
    if (asCast != 8) return 5;
    if (asInit != 8) return 6;
    if (asArg  != 8) return 7;

    return 42;
}
