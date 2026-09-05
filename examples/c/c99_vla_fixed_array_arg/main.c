// D-CSUBSET-VLA-FIXED-ARRAY-ARG-COMPAT: C 6.7.6.2p6 array-type COMPATIBILITY across a
// VLA bound and a CONSTANT one, in ALL THREE shapes it takes, RUNNING on every leg.
//
//   "For two array types to be compatible, both shall have compatible element types, and
//    if both size specifiers are present, and are integer constant expressions, then both
//    size specifiers shall have the same constant value."
//
// A VLA's size specifier is NOT an integer constant expression, so an `int[n]` row and an
// `int[2]` row ARE compatible — the mismatch is C's own UNDEFINED BEHAVIOUR only when the
// two bounds actually differ at RUNTIME, which is a callee obligation. DSS interned the
// VLA bound as a distinct `-2` sentinel and every identity compare therefore answered
// "incompatible", rejecting valid C with S_TypeMismatch. The three shapes, all three
// refused before this and all three exercised here:
//
//   (a) a FIXED `int b[2][2]` ARGUMENT decaying into an `int (*)[n]` PARAMETER  -> sum()
//   (b) `p = q` — ptr-to-FIXED-row assigned into a ptr-to-VLA-row
//   (c) `r = p` — the REVERSE, ptr-to-VLA-row assigned into a ptr-to-FIXED-row
//
// ✔MEASURED 2026-09-03: gcc 13.3.0 and clang 18.1.3 compile and RUN all three, at both
// `-std=c17` and `-std=c2x`, with `-Wall -Wextra -pedantic` silent on every one. MSVC
// ABSTAINS — it implements no C99 VLA at all and stops on the bound itself with
// `error C2057: expected constant expression` — so it casts no vote and the disjunction
// decides on the two references that WORK.
//
// ⚠ THE BOUNDARY THAT MAKES THIS A NARROWING AND NOT A HOLE, and it is the reason the
// compare walks the spine pairwise instead of just ignoring lengths: TWO DIFFERENT
// CONSTANT bounds stay incompatible. `int (*)[3]` from an `int[2][2]` is still refused
// (gcc and clang both DIAGNOSE it, `-Wincompatible-pointer-types`, where they say nothing
// at all about the three shapes above) — pinned in the semantic suite as
// `PtrToVlaFixedRowDifferentConstantsStillRejects`.
//
// main holds NO VLA OBJECT (b is a FIXED `int[2][2]` and p/q/r are pointers, which emit a
// fixed 8-byte stride slot rather than a dynamic-stack object), so main may CALL — this
// example is isolated from the orthogonal non-leaf-VLA-frame deferral
// (D-CSUBSET-VLA-NONLEAF-CALL-FRAME) exactly the way c99_vla_ptr_param is.
// `volatile` defeats constant folding so n is genuinely runtime. The correctness witness
// throughout is the OFF-DIAGONAL: cell [1][0] and cell [0][1] are DISTINCT bytes, so a
// wrong row stride would alias or transpose them. Each `return k` is a strict in-program
// pin; only all-pass reaches 42.

int sink(int v) { return v; }

// (a) the ptr-to-VLA PARAMETER that a FIXED 2-D array argument must decay into.
int sum(int n, int (*p)[n], int rows) {
    int t = 0;
    for (int i = 0; i < rows; i = i + 1)
        for (int j = 0; j < n; j = j + 1)
            t = t + p[i][j];
    return sink(t);                 // a CALL: sum is NOT leaf, and need not be
}

int main(void) {
    volatile int vn = 2;
    int n = vn;                     // runtime 2; volatile => no const fold

    int b[2][2];                    // a FIXED array — main holds NO VLA object
    b[0][0] = 1; b[0][1] = 2;
    b[1][0] = 3; b[1][1] = 4;

    // (a) the FIXED argument decays to `int (*)[n]`.
    if (sum(n, b, 2) != 10) return 1;

    // (b) ptr-to-FIXED-row -> ptr-to-VLA-row.
    int (*q)[2] = b;                // rows are `int[2]`, a constant bound
    int (*p)[n];                    // rows are `int[n]`, a runtime bound
    p = q;
    if (p[1][0] != 3) return 2;     // OFF-DIAGONAL: [1][0] must not alias [0][1]
    if (p[0][1] != 2) return 3;
    p[1][1] = sink(9);              // write through the VLA-row view
    if (b[1][1] != 9) return 4;     // ... and see it in the underlying object

    // (c) the REVERSE: ptr-to-VLA-row -> ptr-to-FIXED-row.
    int (*r)[2];
    r = p;
    if (r[0][0] != 1) return 5;
    if (r[1][0] != 3) return 6;     // OFF-DIAGONAL again, through the fixed view
    r[0][1] = 7;
    if (p[0][1] != 7) return 7;     // the two views alias the SAME storage

    // The three views must agree cell for cell — a stride error on any one of them
    // shows up here even if it happened to read back consistently above.
    if (b[0][0] + b[0][1] + b[1][0] + b[1][1] != 1 + 7 + 3 + 9) return 8;

    return 42;
}
