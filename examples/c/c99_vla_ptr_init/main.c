/* D-CSUBSET-VLA-PTR-INIT-FORM-TYPING (CLOSED in P34) -- the NATURAL initializer
 * form of a pointer to a VLA, `int (*p)[n] = b;`, compiles and STRIDES CORRECTLY.
 *
 * WHAT IT REPLACED. This form was a deferral for a year: it failed loud
 * S_TypeMismatch, and the sibling `examples/c/c99_vla_ptr` had to witness the
 * capability through the ASSIGNMENT form (`int (*p)[n]; p = b;`) instead. The
 * recorded root cause blamed the Pass-2 initializer stamp, and a fix aimed there
 * "proved INERT". The real blocker was on the DECLARATOR side and it closed as a
 * side effect of D-CSUBSET-VLA-INITIALIZER: with the flexible-array flag tested
 * ABOVE the VLA arm and an initializer present, `(*p)[n]` built
 * Ptr<incompleteArray<int>> rather than Ptr<vlaArray<int>>, so the
 * `Ptr<vlaArray> <- array(array)` decay compare could never match whatever the
 * initializer's stamp said -- and `storePtrToVlaStride`'s typeContainsVla gate,
 * which tests for the VLA sentinel, never fired either. One defect, both halves.
 *
 * THE OFF-DIAGONAL IS THE WHOLE TEST. b is int[2][n] with a RUNTIME n == 3, so
 * `p[1][0]` and `p[0][2]` are DISTINCT cells that a wrong row stride would alias
 * or transpose: a stride of one ELEMENT reads 11 where 20 belongs, a transposed
 * read gives 21. Only a runtime row stride of n*sizeof(int) gives 20. Verified
 * against gcc 13.3.0 and clang 19.1.1: all agree (MEASURED 2026-08-25).
 *
 * WHAT BREAKS IT: re-merging the flexible-array and init-inference signals (the
 * pointee becomes an incomplete array and the compile fails again, so there is no
 * binary at all); freezing the pointee stride from the wrong slot (the reads
 * alias and the exit code is no longer 42); the stride Load being const-folded
 * away by the release pipeline (n is volatile-seeded precisely so it cannot be).
 *
 * `volatile vn` defeats const-fold so n is genuinely a run-time value, and main
 * is a LEAF -- a VLA frame is leaf-only (L_VlaNonLeafFrameUnsupported), so this
 * file makes no calls at all. */
int main(void) {
    volatile int vn = 3;
    int n = vn;

    int b[2][n];
    b[0][0] = 10; b[0][1] = 11; b[0][2] = 12;
    b[1][0] = 20; b[1][1] = 21; b[1][2] = 22;

    int (*p)[n] = b;          /* THE INIT FORM -- the subject of this example */
    int (*q)[n];              /* the assignment form, as a matched control    */
    q = b;

    /* 1: the first row reads back unchanged. */
    if (p[0][0] != 10) return 1;
    /* 2: the far end of row 0 -- the cell a one-element stride would confuse
     *    with p[1][0]. */
    if (p[0][2] != 12) return 2;
    /* 3: THE OFF-DIAGONAL. 20, never 11 (wrong stride) and never 21 (transposed). */
    if (p[1][0] != 20) return 3;
    /* 4: the far corner, which also proves p aliases b rather than copying it. */
    if (p[1][2] != 22) return 4;
    /* 5: the init form and the assignment form must agree cell for cell. */
    if (q[1][0] != p[1][0]) return 5;
    /* 6: write THROUGH the init-form pointer and see it in b. */
    p[1][0] = 99;
    if (b[1][0] != 99) return 6;

    return 42;
}
